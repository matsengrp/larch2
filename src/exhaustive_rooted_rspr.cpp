#include <larch/exhaustive_rooted_rspr.hpp>

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
      throw std::invalid_argument("rSPR path is outside the tree");
    }
    current = &current->children[index];
  }
  return *current;
}

rooted_tree& at(rooted_tree& tree, path const& location) {
  auto* current = &tree;
  for (auto index : location) {
    if (current->is_leaf() || index >= current->children.size()) {
      throw std::invalid_argument("rSPR path is outside the tree");
    }
    current = &current->children[index];
  }
  return *current;
}

void collect_nonroot_paths(rooted_tree const& tree, path& prefix,
                           std::vector<path>& result) {
  if (tree.is_leaf()) return;
  for (std::size_t i = 0; i < tree.children.size(); ++i) {
    prefix.push_back(i);
    result.push_back(prefix);
    collect_nonroot_paths(tree.children[i], prefix, result);
    prefix.pop_back();
  }
}

void collect_internal_paths(rooted_tree const& tree, path& prefix,
                            std::vector<path>& result) {
  if (tree.is_leaf()) return;
  result.push_back(prefix);
  for (std::size_t i = 0; i < tree.children.size(); ++i) {
    prefix.push_back(i);
    collect_internal_paths(tree.children[i], prefix, result);
    prefix.pop_back();
  }
}

void canonicalize_in_place(rooted_tree& tree) {
  if (tree.is_leaf()) return;
  for (auto& child : tree.children) canonicalize_in_place(child);
  std::ranges::sort(tree.children, {}, canonical_tree_bytes);
}

struct detach_result {
  rooted_tree remainder;
  rooted_tree moved;
  std::string parent_support;
  source_parent_fate fate{};
};

detach_result detach(rooted_tree const& source, path const& location) {
  if (location.empty()) {
    throw std::invalid_argument("rSPR cannot detach the biological root");
  }
  auto parent_path = location;
  auto const child_index = parent_path.back();
  parent_path.pop_back();
  auto const& source_parent = at(source, parent_path);
  if (source_parent.is_leaf() || child_index >= source_parent.children.size()) {
    throw std::invalid_argument("rSPR source path has no parent edge");
  }

  detach_result result{source, source_parent.children[child_index],
                       taxon_support_bytes(source_parent),
                       source_parent.children.size() == 2
                           ? (parent_path.empty()
                                  ? source_parent_fate::root_suppressed
                                  : source_parent_fate::suppressed)
                           : source_parent_fate::retained};

  auto& parent = at(result.remainder, parent_path);
  parent.children.erase(parent.children.begin() +
                        static_cast<std::ptrdiff_t>(child_index));
  if (parent.children.size() == 1) {
    auto survivor = std::move(parent.children.front());
    if (parent_path.empty()) {
      result.remainder = std::move(survivor);
    } else {
      auto grandparent_path = parent_path;
      auto const parent_index = grandparent_path.back();
      grandparent_path.pop_back();
      auto& grandparent = at(result.remainder, grandparent_path);
      grandparent.children[parent_index] = std::move(survivor);
    }
  }
  canonicalize_in_place(result.remainder);
  validate(result.remainder);
  return result;
}

rooted_tree attach_edge(rooted_tree const& remainder, path const& destination,
                        rooted_tree const& moved) {
  if (destination.empty()) return internal({remainder, moved});
  auto result = remainder;
  auto parent_path = destination;
  auto const child_index = parent_path.back();
  parent_path.pop_back();
  auto& parent = at(result, parent_path);
  if (child_index >= parent.children.size()) {
    throw std::invalid_argument("rSPR destination edge is outside the remainder");
  }
  auto previous = parent.children[child_index];
  parent.children[child_index] = internal({std::move(previous), moved});
  canonicalize_in_place(result);
  validate(result);
  return result;
}

rooted_tree attach_vertex(rooted_tree const& remainder, path const& destination,
                          rooted_tree const& moved) {
  auto result = remainder;
  auto& target = at(result, destination);
  if (target.is_leaf()) {
    throw std::invalid_argument("rSPR vertex attachment target is a leaf");
  }
  target.children.push_back(moved);
  canonicalize_in_place(result);
  validate(result);
  return result;
}

std::string digest(std::string_view domain, std::string_view bytes) {
  larch::sha256 value;
  value.update(domain);
  value.update(bytes);
  return value.hex_digest();
}

}  // namespace

std::string_view name(source_parent_fate value) noexcept {
  switch (value) {
    case source_parent_fate::retained:
      return "retained";
    case source_parent_fate::suppressed:
      return "suppressed";
    case source_parent_fate::root_suppressed:
      return "root_suppressed";
  }
  return "invalid";
}

std::string_view name(attachment_kind value) noexcept {
  switch (value) {
    case attachment_kind::edge:
      return "edge";
    case attachment_kind::root_stem:
      return "root_stem";
    case attachment_kind::internal_vertex:
      return "internal_vertex";
  }
  return "invalid";
}

std::string witness_semantic_bytes(rspr_witness const& witness) {
  auto field = [](std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value);
  };
  return "rooted-common-prune-reduction-rspr-witness-v1\n" +
         field(witness.source) + field(witness.destination) +
         field(witness.moved_support) + field(witness.source_parent_support) +
         field(name(witness.source_fate)) + field(name(witness.attachment)) +
         field(witness.destination_support);
}

std::string witness_sha256(rspr_witness const& witness) {
  return digest("topology-landscape.rspr-witness.v1\n",
                witness_semantic_bytes(witness));
}

std::vector<rspr_witness> enumerate_rooted_rspr(rooted_tree const& source,
                                                rspr_policy policy) {
  validate(source);
  if (policy != rspr_policy::binary_edge_subdivision &&
      policy != rspr_policy::hard_symmetric_edge_or_vertex) {
    throw std::invalid_argument("unknown rooted rSPR policy");
  }
  if (policy == rspr_policy::binary_edge_subdivision && !is_binary(source)) {
    throw std::invalid_argument("binary rSPR policy requires a binary source");
  }
  auto const source_bytes = canonical_tree_bytes(source);
  std::vector<path> cuts;
  path prefix;
  collect_nonroot_paths(source, prefix, cuts);
  std::set<rspr_witness> unique;

  for (auto const& cut : cuts) {
    auto detached = detach(source, cut);
    auto const moved_support = taxon_support_bytes(detached.moved);

    std::vector<path> edges;
    edges.push_back({});  // virtual root stem
    prefix.clear();
    collect_nonroot_paths(detached.remainder, prefix, edges);
    for (auto const& destination : edges) {
      auto attached = attach_edge(detached.remainder, destination, detached.moved);
      auto destination_bytes = canonical_tree_bytes(attached);
      if (destination_bytes == source_bytes) continue;
      unique.insert(rspr_witness{
          source_bytes,
          std::move(destination_bytes),
          moved_support,
          detached.parent_support,
          detached.fate,
          destination.empty() ? attachment_kind::root_stem
                              : attachment_kind::edge,
          destination.empty()
              ? std::string("root-stem")
              : taxon_support_bytes(at(detached.remainder, destination)),
      });
    }

    if (policy == rspr_policy::hard_symmetric_edge_or_vertex) {
      std::vector<path> vertices;
      prefix.clear();
      collect_internal_paths(detached.remainder, prefix, vertices);
      for (auto const& destination : vertices) {
        auto attached =
            attach_vertex(detached.remainder, destination, detached.moved);
        auto destination_bytes = canonical_tree_bytes(attached);
        if (destination_bytes == source_bytes) continue;
        unique.insert(rspr_witness{
            source_bytes,
            std::move(destination_bytes),
            moved_support,
            detached.parent_support,
            detached.fate,
            attachment_kind::internal_vertex,
            taxon_support_bytes(at(detached.remainder, destination)),
        });
      }
    }
  }
  return {unique.begin(), unique.end()};
}

std::vector<rspr_edge> build_rooted_rspr_graph(
    std::span<rooted_tree const> universe, rspr_policy policy) {
  std::set<std::string> vertices;
  for (auto const& tree : universe) {
    if (!vertices.insert(canonical_tree_bytes(tree)).second) {
      throw std::invalid_argument("rSPR universe has duplicate topology bytes");
    }
  }

  std::map<std::pair<std::string, std::string>, std::vector<rspr_witness>> edges;
  for (auto const& tree : universe) {
    for (auto& witness : enumerate_rooted_rspr(tree, policy)) {
      if (!vertices.contains(witness.destination)) {
        throw std::invalid_argument(
            "rSPR generated an endpoint outside the declared universe");
      }
      auto low = std::min(witness.source, witness.destination);
      auto high = std::max(witness.source, witness.destination);
      edges[{std::move(low), std::move(high)}].push_back(std::move(witness));
    }
  }

  std::vector<rspr_edge> result;
  result.reserve(edges.size());
  for (auto& [endpoints, witnesses] : edges) {
    std::ranges::sort(witnesses);
    result.push_back(
        {std::move(endpoints.first), std::move(endpoints.second),
         std::move(witnesses)});
  }
  return result;
}

bool has_reverse_witnesses(std::span<rspr_edge const> edges) {
  for (auto const& edge : edges) {
    bool low_to_high = false;
    bool high_to_low = false;
    for (auto const& witness : edge.directed_witnesses) {
      low_to_high |= witness.source == edge.low && witness.destination == edge.high;
      high_to_low |= witness.source == edge.high && witness.destination == edge.low;
    }
    if (!low_to_high || !high_to_low) return false;
  }
  return true;
}

}  // namespace larch::topology
