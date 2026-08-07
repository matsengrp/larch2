#include <larch/rooted_rspr_pair_index.hpp>

#include <larch/sha256.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace larch::topology {
namespace {

using path = std::vector<std::size_t>;

rooted_tree const& at(rooted_tree const& tree, path const& location) {
  auto const* current = &tree;
  for (auto index : location) {
    if (current->is_leaf() || index >= current->children.size()) {
      throw std::invalid_argument("rSPR cut path is outside the tree");
    }
    current = &current->children[index];
  }
  return *current;
}

rooted_tree& at(rooted_tree& tree, path const& location) {
  auto* current = &tree;
  for (auto index : location) {
    if (current->is_leaf() || index >= current->children.size()) {
      throw std::invalid_argument("rSPR cut path is outside the tree");
    }
    current = &current->children[index];
  }
  return *current;
}

void collect_nonroot_paths(rooted_tree const& tree, path& prefix,
                           std::vector<path>& result) {
  if (tree.is_leaf()) return;
  for (std::size_t child = 0; child < tree.children.size(); ++child) {
    prefix.push_back(child);
    result.push_back(prefix);
    collect_nonroot_paths(tree.children[child], prefix, result);
    prefix.pop_back();
  }
}

path nonroot_path_at(rooted_tree const& tree, std::size_t ordinal) {
  std::vector<path> paths;
  path prefix;
  collect_nonroot_paths(tree, prefix, paths);
  if (ordinal >= paths.size()) {
    throw std::logic_error("rSPR cut ordinal is outside the canonical tree");
  }
  return std::move(paths[ordinal]);
}

std::string path_bytes(path const& location) {
  std::string result;
  for (auto index : location) {
    if (!result.empty()) result.push_back('/');
    result += std::to_string(index);
  }
  return result;
}

void canonicalize_in_place(rooted_tree& tree) {
  if (tree.is_leaf()) return;
  for (auto& child : tree.children) canonicalize_in_place(child);
  std::ranges::sort(tree.children, {}, canonical_tree_bytes);
}

struct exact_detachment {
  std::string moved;
  std::string reduced_remainder;
  source_parent_fate source_fate{};
};

exact_detachment detach_signature(rooted_tree const& source,
                                  path const& location) {
  if (location.empty()) {
    throw std::invalid_argument("rSPR cannot detach the biological root");
  }
  auto parent_path = location;
  auto const child_index = parent_path.back();
  parent_path.pop_back();
  auto const& source_parent = at(source, parent_path);
  if (source_parent.is_leaf() || child_index >= source_parent.children.size()) {
    throw std::invalid_argument("rSPR cut path has no parent edge");
  }

  auto moved = canonical_tree_bytes(source_parent.children[child_index]);
  auto fate = source_parent.children.size() == 2
                  ? (parent_path.empty()
                         ? source_parent_fate::root_suppressed
                         : source_parent_fate::suppressed)
                  : source_parent_fate::retained;

  auto remainder = source;
  auto& parent = at(remainder, parent_path);
  parent.children.erase(parent.children.begin() +
                        static_cast<std::ptrdiff_t>(child_index));
  if (parent.children.size() == 1) {
    auto survivor = std::move(parent.children.front());
    if (parent_path.empty()) {
      remainder = std::move(survivor);
    } else {
      auto grandparent_path = parent_path;
      auto const parent_index = grandparent_path.back();
      grandparent_path.pop_back();
      auto& grandparent = at(remainder, grandparent_path);
      grandparent.children[parent_index] = std::move(survivor);
    }
  }
  canonicalize_in_place(remainder);
  validate(remainder);
  return {std::move(moved), canonical_tree_bytes(remainder), fate};
}

std::string sha256_bytes(std::string_view bytes) {
  larch::sha256 digest;
  digest.update(bytes);
  return digest.hex_digest();
}

struct indexed_vertex {
  rooted_tree tree;
  std::string bytes;
  std::string topology_hash;
};

struct cut_record {
  std::size_t vertex = 0;
  std::size_t cut_ordinal = 0;
};

struct resolved_cut_record {
  std::string moved;
  std::string reduced_remainder;
  std::string topology_hash;
  std::size_t cut_ordinal = 0;
};

}  // namespace

rooted_rspr_signature_fingerprint rooted_rspr_sha256_candidate_fingerprint(
    std::string_view moved, std::string_view reduced_remainder) {
  return {sha256_bytes(moved), sha256_bytes(reduced_remainder)};
}

std::vector<rooted_rspr_cut_signature>
enumerate_rooted_rspr_cut_signatures(rooted_tree const& source) {
  auto canonical = parse_canonical_tree(canonical_tree_bytes(source));
  std::vector<path> paths;
  path prefix;
  collect_nonroot_paths(canonical, prefix, paths);

  std::vector<rooted_rspr_cut_signature> result;
  result.reserve(paths.size());
  for (auto const& location : paths) {
    auto signature = detach_signature(canonical, location);
    result.push_back({path_bytes(location), std::move(signature.moved),
                      std::move(signature.reduced_remainder),
                      signature.source_fate});
  }
  return result;
}

rooted_rspr_pair_index_result build_rooted_rspr_pair_index(
    std::span<rooted_tree const> universe,
    rooted_rspr_pair_index_options options) {
  if (options.candidate_fingerprint == nullptr) {
    throw std::invalid_argument(
        "rSPR pair index requires a candidate fingerprint function");
  }

  rooted_rspr_pair_index_result result;
  result.statistics.tree_count = universe.size();
  if (universe.empty()) return result;

  std::vector<indexed_vertex> vertices;
  vertices.reserve(universe.size());
  auto const expected_taxa = taxa(universe.front());
  std::map<std::string, std::string, std::less<>> bytes_by_hash;
  for (auto const& input : universe) {
    validate(input);
    if (taxa(input) != expected_taxa) {
      throw std::invalid_argument(
          "rSPR pair-index universe contains different taxon sets");
    }
    auto bytes = canonical_tree_bytes(input);
    auto canonical = parse_canonical_tree(bytes);
    auto topology_hash = topology_sha256(canonical);
    auto [found, inserted] =
        bytes_by_hash.emplace(topology_hash, bytes);
    if (!inserted) {
      if (found->second == bytes) {
        throw std::invalid_argument(
            "rSPR pair-index universe has duplicate topology bytes");
      }
      throw std::runtime_error(
          "rSPR pair-index topology SHA-256 collision detected");
    }
    vertices.push_back(
        {std::move(canonical), std::move(bytes), std::move(topology_hash)});
  }
  std::ranges::sort(vertices, {}, &indexed_vertex::bytes);

  std::map<rooted_rspr_signature_fingerprint, std::vector<cut_record>> buckets;
  for (std::size_t vertex_index = 0; vertex_index < vertices.size();
       ++vertex_index) {
    std::vector<path> paths;
    path prefix;
    collect_nonroot_paths(vertices[vertex_index].tree, prefix, paths);
    result.statistics.cut_record_count += paths.size();
    for (std::size_t cut_ordinal = 0; cut_ordinal < paths.size();
         ++cut_ordinal) {
      auto signature =
          detach_signature(vertices[vertex_index].tree, paths[cut_ordinal]);
      auto fingerprint = options.candidate_fingerprint(
          signature.moved, signature.reduced_remainder);
      buckets[std::move(fingerprint)].push_back(
          {vertex_index, cut_ordinal});
    }
  }
  result.statistics.candidate_bucket_count = buckets.size();

  std::set<rooted_rspr_endpoint_pair> endpoint_pairs;
  for (auto const& [fingerprint, records] : buckets) {
    (void)fingerprint;
    if (records.size() == 1) {
      ++result.statistics.exact_signature_class_count;
      result.statistics.largest_exact_signature_class_size =
          std::max(result.statistics.largest_exact_signature_class_size,
                   std::size_t{1});
      continue;
    }

    std::vector<resolved_cut_record> resolved;
    resolved.reserve(records.size());
    for (auto const& record : records) {
      auto const& vertex = vertices[record.vertex];
      auto signature = detach_signature(
          vertex.tree, nonroot_path_at(vertex.tree, record.cut_ordinal));
      resolved.push_back({std::move(signature.moved),
                          std::move(signature.reduced_remainder),
                          vertex.topology_hash, record.cut_ordinal});
    }
    std::ranges::sort(resolved, {}, [](resolved_cut_record const& value) {
      return std::tie(value.moved, value.reduced_remainder,
                      value.topology_hash, value.cut_ordinal);
    });

    std::size_t exact_group_count = 0;
    for (std::size_t begin = 0; begin < resolved.size();) {
      auto end = begin + 1;
      while (end < resolved.size() &&
             resolved[end].moved == resolved[begin].moved &&
             resolved[end].reduced_remainder ==
                 resolved[begin].reduced_remainder) {
        ++end;
      }
      ++exact_group_count;
      ++result.statistics.exact_signature_class_count;
      result.statistics.largest_exact_signature_class_size =
          std::max(result.statistics.largest_exact_signature_class_size,
                   end - begin);

      std::vector<std::string> endpoints;
      endpoints.reserve(end - begin);
      for (auto index = begin; index < end; ++index) {
        if (endpoints.empty() ||
            endpoints.back() != resolved[index].topology_hash) {
          endpoints.push_back(resolved[index].topology_hash);
        }
      }
      for (std::size_t low = 0; low < endpoints.size(); ++low) {
        for (std::size_t high = low + 1; high < endpoints.size(); ++high) {
          endpoint_pairs.insert({endpoints[low], endpoints[high]});
        }
      }
      begin = end;
    }
    if (exact_group_count > 1) {
      ++result.statistics.fingerprint_collision_bucket_count;
    }
  }

  result.endpoint_pairs.assign(endpoint_pairs.begin(), endpoint_pairs.end());
  return result;
}

}  // namespace larch::topology
