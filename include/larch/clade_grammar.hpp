#pragma once

#include <larch/compute.hpp>
#include <larch/compact_genome.hpp>
#include <larch/phylo_dag.hpp>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace larch {

// WRIC / collapsed-clade grammar sidecar invariants
// --------------------------------------------------
// * build_clade_offsets(d) should be called before grammar construction for
//   fast get_clades(d, node_idx) access.  The Phase-1 builder below repairs
//   offsets by rebuilding them at entry, but code paths that construct/modify
//   DAGs should still treat offsets as stale until rebuilt.
// * Do not use larch::no_idx (std::size_t(-1)) as a sentinel for grammar IDs.
//   Grammar IDs are intentionally narrower than DAG node/edge indices; use the
//   dedicated no_clade and no_production values below.
// * Leaf identity is sample_id, not the compact-genome string, unless the
//   caller has intentionally coerced sample IDs.
// * Empty sample IDs and conflicting duplicate sample IDs are hard errors by
//   default.
// * Grammar clade identity is the sorted set of descendant taxa.
// * Productions are keyed only by parent clade and child clades, not by
//   compact-genome labels.
// * Leaf clades have no productions; their chart entries are initialized
//   directly from observed states.
// * UA/root passthrough nodes must be suppressed in the collapsed grammar to
//   avoid self-productions on the full-taxon clade.  Root-edge scoring is
//   handled as an optional chart boundary condition instead.
// * For binary phylogenetic trees, internal productions should normally have
//   arity 2; only UA/root self-passthroughs are suppressed.  Non-UA unary and
//   other non-binary structure is a hard error by default.  Audit callers may
//   explicitly opt into representing arity >2 polytomies for diagnostics; later
//   chart phases may still reject them until their recurrences are generalized.

using taxon_id = std::uint32_t;
using clade_id = std::uint32_t;
using production_id = std::uint32_t;

inline constexpr clade_id no_clade = std::numeric_limits<clade_id>::max();
inline constexpr production_id no_production =
    std::numeric_limits<production_id>::max();

struct taxon_registry {
  std::vector<std::string> id_to_sample_id;
  std::unordered_map<std::string, taxon_id> sample_id_to_id;
};

struct clade_key {
  std::vector<taxon_id> taxa;  // sorted, unique

  bool operator==(clade_key const&) const = default;
};

struct production_child_witness {
  clade_id child = no_clade;
  // All history-DAG edges in this parent node's clade group that collapse to
  // this child clade. In a valid Phase-1 grammar this vector is non-empty and
  // every edge in the group has the same collapsed child clade.
  std::vector<std::size_t> edge_alternatives;
};

struct production_witness {
  std::size_t parent_node = std::numeric_limits<std::size_t>::max();
  // Aligned with grammar_production::children, preserving which DAG edge
  // alternatives belonged to which child clade group.
  std::vector<production_child_witness> children;
};

struct grammar_production {
  clade_id parent = no_clade;
  // Binary by default. Leaf clades have no productions, and UA/root
  // self-passthroughs are suppressed rather than represented as X -> X.
  // Non-UA unary productions are invalid. Polytomies are represented only when
  // clade_grammar_options::allow_polytomies is true and counted in the audit.
  std::vector<clade_id> children;
  std::vector<production_witness> witnesses;
  std::uint64_t multiplicity = 0;  // history-DAG witnesses before dedup
};

struct clade_grammar {
  taxon_registry taxa;
  std::vector<clade_key> clades;
  std::vector<grammar_production> productions;
  std::vector<std::vector<production_id>> productions_by_parent;
  std::vector<std::vector<production_id>> productions_by_child;
  std::vector<clade_id> node_to_clade;  // indexed by DAG node, no_clade absent
  clade_id root_clade = no_clade;
};

struct clade_grammar_audit {
  std::size_t reachable_node_count = 0;
  std::size_t reachable_edge_count = 0;
  std::size_t taxon_count = 0;
  std::size_t duplicate_sample_id_occurrences = 0;
  std::size_t collapsed_clade_count = 0;
  std::size_t grammar_production_count = 0;

  // Indexed by clade_id. Counts non-UA history DAG nodes in each collapsed
  // clade; the UA node is excluded so root-clade multiplicity reflects
  // compact-genome-labelled history nodes only.
  std::vector<std::size_t> collapsed_clade_node_count;
  std::map<std::size_t, std::size_t> nodes_per_clade_histogram;
  std::map<std::uint64_t, std::size_t> production_multiplicity_histogram;

  std::size_t max_nodes_per_collapsed_clade = 0;
  double mean_nodes_per_collapsed_clade = 0.0;
  double median_nodes_per_collapsed_clade = 0.0;

  std::size_t invalid_clade_index_group_count = 0;
  std::size_t non_binary_production_count = 0;

  clade_id root_clade = no_clade;
  std::size_t root_clade_size = 0;
  std::size_t root_production_count = 0;

  std::size_t strict_reference_acgt_pass = 0;
  std::size_t strict_reference_acgt_fail = 0;
  std::size_t strict_leaf_compact_genome_acgt_pass = 0;
  std::size_t strict_leaf_compact_genome_acgt_fail = 0;

  std::uint64_t grammar_tree_count_estimate = 0;
  bool grammar_tree_count_estimate_saturated = false;
};

struct clade_grammar_options {
  bool coalesce_duplicate_sample_ids_with_identical_cg = true;
  bool canonicalize_binary_children = true;
  bool allow_polytomies = false;
};

struct clade_grammar_build_result {
  clade_grammar grammar;
  clade_grammar_audit audit;
};

namespace detail {

inline bool is_acgt_char(char c) {
  switch (c) {
    case 'A':
    case 'a':
    case 'C':
    case 'c':
    case 'G':
    case 'g':
    case 'T':
    case 't':
      return true;
    default:
      return false;
  }
}

inline bool is_valid_nuc_base(nuc_base base) { return base.raw() <= nuc_base::T; }

inline bool is_ua_node(phylo_dag::node_variant_type const& nv) {
  return std::visit(
      [](auto node) {
        return std::is_same_v<std::remove_cvref_t<decltype(node)>,
                              node_view<phylo_dag, node_kind::ua>>;
      },
      nv);
}

inline bool is_leaf_node(phylo_dag::node_variant_type const& nv) {
  return std::visit(
      [](auto node) {
        return std::is_same_v<std::remove_cvref_t<decltype(node)>,
                              node_view<phylo_dag, node_kind::leaf>>;
      },
      nv);
}

inline bool node_has_children(phylo_dag& dag, std::size_t node_idx) {
  auto nv = dag.get_node(node_idx);
  return std::visit(
      [](auto node) {
        for (auto child_edge : node.get_children()) {
          (void)child_edge;
          return true;
        }
        return false;
      },
      nv);
}

inline std::vector<std::size_t> child_edge_indices(phylo_dag& dag,
                                                   std::size_t node_idx) {
  std::vector<std::size_t> edges;
  auto nv = dag.get_node(node_idx);
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children()) {
          std::visit([&](auto edge) { edges.push_back(edge.index()); }, ev);
        }
      },
      nv);
  return edges;
}

inline bool has_ua_parent(phylo_dag& dag, std::size_t node_idx) {
  auto nv = dag.get_node(node_idx);
  return std::visit(
      [&](auto node) {
        for (auto ev : node.get_parents()) {
          bool parent_is_ua = false;
          std::visit(
              [&](auto edge) {
                auto pv = edge.get_parent();
                parent_is_ua = is_ua_node(pv);
              },
              ev);
          if (parent_is_ua) return true;
        }
        return false;
      },
      nv);
}

struct reachable_dag_info {
  std::vector<std::size_t> nodes;
  std::vector<std::size_t> edges;
  std::vector<bool> node_reachable;
  std::vector<bool> edge_reachable;
};

inline reachable_dag_info collect_reachable(phylo_dag& dag) {
  reachable_dag_info info;
  info.node_reachable.assign(dag.node_high_mark(), false);
  info.edge_reachable.assign(dag.edge_high_mark(), false);

  auto root_idx = get_root_idx(dag);
  if (root_idx >= info.node_reachable.size())
    throw std::runtime_error("clade grammar: DAG root index outside node range");

  std::vector<std::size_t> stack{root_idx};
  while (!stack.empty()) {
    auto node_idx = stack.back();
    stack.pop_back();
    if (node_idx >= info.node_reachable.size())
      throw std::runtime_error("clade grammar: child node index outside range");
    if (info.node_reachable[node_idx]) continue;
    info.node_reachable[node_idx] = true;
    info.nodes.push_back(node_idx);

    auto edges = child_edge_indices(dag, node_idx);
    // Stable traversal: pushing in reverse preserves child order when popping.
    for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
      auto edge_idx = *it;
      if (edge_idx >= info.edge_reachable.size())
        throw std::runtime_error("clade grammar: child edge index outside range");
      if (!info.edge_reachable[edge_idx]) {
        info.edge_reachable[edge_idx] = true;
        info.edges.push_back(edge_idx);
      }
      stack.push_back(get_child_idx(dag, edge_idx));
    }
  }

  std::sort(info.nodes.begin(), info.nodes.end());
  std::sort(info.edges.begin(), info.edges.end());
  return info;
}

struct leaf_observation {
  std::string sample_id;
  compact_genome cg;
};

inline leaf_observation get_leaf_observation(phylo_dag& dag,
                                             std::size_t node_idx) {
  auto nv = dag.get_node(node_idx);
  return std::visit(
      [&](auto node) -> leaf_observation {
        if constexpr (requires {
                        node.sample_id();
                        node.cg();
                      }) {
          return leaf_observation{std::string{node.sample_id()}, node.cg()};
        } else {
          throw std::runtime_error(
              "clade grammar: expected leaf node with sample_id at node " +
              std::to_string(node_idx));
        }
      },
      nv);
}

inline void update_strict_nucleotide_counts(phylo_dag& dag,
                                            reachable_dag_info const& reachable,
                                            clade_grammar_audit& audit) {
  auto const& reference = get_reference_sequence(dag);
  std::vector<bool> reference_is_acgt(reference.size(), false);
  for (std::size_t i = 0; i < reference.size(); ++i) {
    reference_is_acgt[i] = is_acgt_char(reference[i]);
    if (reference_is_acgt[i])
      ++audit.strict_reference_acgt_pass;
    else
      ++audit.strict_reference_acgt_fail;
  }

  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (!is_leaf_node(nv)) continue;

    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            auto it = node.cg().begin();
            auto end = node.cg().end();
            for (mutation_position pos = 1; pos <= reference.size(); ++pos) {
              while (it != end && it->first < pos) {
                ++audit.strict_leaf_compact_genome_acgt_fail;
                ++it;
              }
              if (it != end && it->first == pos) {
                if (is_valid_nuc_base(it->second))
                  ++audit.strict_leaf_compact_genome_acgt_pass;
                else
                  ++audit.strict_leaf_compact_genome_acgt_fail;
                ++it;
              } else if (reference_is_acgt[pos - 1]) {
                ++audit.strict_leaf_compact_genome_acgt_pass;
              } else {
                ++audit.strict_leaf_compact_genome_acgt_fail;
              }
            }
            while (it != end) {
              ++audit.strict_leaf_compact_genome_acgt_fail;
              ++it;
            }
          }
        },
        nv);
  }
}

inline taxon_registry build_taxon_registry(
    phylo_dag& dag, reachable_dag_info const& reachable,
    clade_grammar_options const& options, clade_grammar_audit& audit) {
  std::map<std::string, compact_genome> sample_to_cg;

  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (!is_leaf_node(nv)) continue;
    if (node_has_children(dag, node_idx))
      throw std::runtime_error("clade grammar: leaf node " +
                               std::to_string(node_idx) + " has children");

    auto obs = get_leaf_observation(dag, node_idx);
    if (obs.sample_id.empty())
      throw std::runtime_error("clade grammar: empty sample_id at leaf node " +
                               std::to_string(node_idx));

    auto [it, inserted] = sample_to_cg.emplace(obs.sample_id, obs.cg);
    if (!inserted) {
      ++audit.duplicate_sample_id_occurrences;
      if (!(it->second == obs.cg)) {
        throw std::runtime_error(
            "clade grammar: duplicate sample_id '" + obs.sample_id +
            "' has conflicting compact genomes");
      }
      if (!options.coalesce_duplicate_sample_ids_with_identical_cg) {
        throw std::runtime_error(
            "clade grammar: duplicate sample_id '" + obs.sample_id +
            "' encountered and duplicate coalescing is disabled");
      }
    }
  }

  if (sample_to_cg.empty())
    throw std::runtime_error("clade grammar: no reachable leaves with taxa");
  if (sample_to_cg.size() >= static_cast<std::size_t>(no_clade))
    throw std::runtime_error("clade grammar: too many taxa for uint32 IDs");

  taxon_registry taxa;
  taxa.id_to_sample_id.reserve(sample_to_cg.size());
  taxa.sample_id_to_id.reserve(sample_to_cg.size());
  for (auto const& [sample_id, _] : sample_to_cg) {
    auto id = static_cast<taxon_id>(taxa.id_to_sample_id.size());
    taxa.id_to_sample_id.push_back(sample_id);
    taxa.sample_id_to_id.emplace(sample_id, id);
  }
  return taxa;
}

inline std::vector<taxon_id> set_union_sorted_unique(
    std::vector<taxon_id> result, std::vector<taxon_id> const& extra) {
  result.insert(result.end(), extra.begin(), extra.end());
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

struct node_taxa_computer {
  phylo_dag& dag;
  taxon_registry const& taxa;
  reachable_dag_info const& reachable;
  std::vector<std::vector<taxon_id>>& node_taxa;
  std::vector<std::uint8_t> state;  // 0 unvisited, 1 visiting, 2 done

  explicit node_taxa_computer(phylo_dag& dag_, taxon_registry const& taxa_,
                              reachable_dag_info const& reachable_,
                              std::vector<std::vector<taxon_id>>& node_taxa_)
      : dag(dag_),
        taxa(taxa_),
        reachable(reachable_),
        node_taxa(node_taxa_),
        state(dag_.node_high_mark(), 0) {}

  std::vector<taxon_id> const& compute(std::size_t node_idx) {
    if (node_idx >= state.size() || !reachable.node_reachable[node_idx])
      throw std::runtime_error(
          "clade grammar: attempted to compute unreachable node " +
          std::to_string(node_idx));
    if (state[node_idx] == 2) return node_taxa[node_idx];
    if (state[node_idx] == 1)
      throw std::runtime_error("clade grammar: cycle detected at node " +
                               std::to_string(node_idx));
    state[node_idx] = 1;

    auto nv = dag.get_node(node_idx);
    if (is_leaf_node(nv)) {
      auto obs = get_leaf_observation(dag, node_idx);
      auto it = taxa.sample_id_to_id.find(obs.sample_id);
      if (it == taxa.sample_id_to_id.end())
        throw std::runtime_error("clade grammar: unknown sample_id '" +
                                 obs.sample_id + "' at leaf node " +
                                 std::to_string(node_idx));
      node_taxa[node_idx] = {it->second};
    } else {
      auto child_edges = child_edge_indices(dag, node_idx);
      if (child_edges.empty()) {
        throw std::runtime_error(
            "clade grammar: non-leaf node " + std::to_string(node_idx) +
            " has no children");
      }
      std::vector<taxon_id> taxa_union;
      for (auto edge_idx : child_edges) {
        auto child_idx = get_child_idx(dag, edge_idx);
        taxa_union = set_union_sorted_unique(taxa_union, compute(child_idx));
      }
      if (taxa_union.empty())
        throw std::runtime_error("clade grammar: node " +
                                 std::to_string(node_idx) +
                                 " has empty descendant taxon set");
      node_taxa[node_idx] = std::move(taxa_union);
    }

    state[node_idx] = 2;
    return node_taxa[node_idx];
  }
};

inline bool clade_key_less(clade_key const& a, clade_key const& b) {
  if (a.taxa.size() != b.taxa.size()) return a.taxa.size() < b.taxa.size();
  return a.taxa < b.taxa;
}

struct production_map_key {
  clade_id parent = no_clade;
  std::vector<clade_id> children;

  auto operator<=>(production_map_key const&) const = default;
};

inline std::string clade_id_set_to_string(std::set<clade_id> const& ids) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (auto id : ids) {
    if (!first) out << ",";
    first = false;
    out << id;
  }
  out << "}";
  return out.str();
}

inline std::string taxon_id_vector_to_string(std::vector<taxon_id> const& ids) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (auto id : ids) {
    if (!first) out << ",";
    first = false;
    out << id;
  }
  out << "}";
  return out.str();
}

inline void validate_child_clade_partition(clade_grammar const& grammar,
                                           clade_id parent,
                                           std::vector<clade_id> const& children,
                                           std::size_t node_idx) {
  if (parent == no_clade || parent >= grammar.clades.size())
    throw std::runtime_error("clade grammar: partition check parent out of range");

  std::vector<taxon_id> covered;
  for (auto child : children) {
    if (child == no_clade || child >= grammar.clades.size())
      throw std::runtime_error("clade grammar: partition check child out of range");

    auto const& child_taxa = grammar.clades[child].taxa;
    std::vector<taxon_id> overlap;
    std::set_intersection(covered.begin(), covered.end(), child_taxa.begin(),
                          child_taxa.end(), std::back_inserter(overlap));
    if (!overlap.empty()) {
      throw std::runtime_error(
          "clade grammar: node " + std::to_string(node_idx) +
          " production children are not pairwise disjoint; overlapping taxa " +
          taxon_id_vector_to_string(overlap));
    }

    std::vector<taxon_id> next;
    std::set_union(covered.begin(), covered.end(), child_taxa.begin(),
                   child_taxa.end(), std::back_inserter(next));
    covered = std::move(next);
  }

  auto const& parent_taxa = grammar.clades[parent].taxa;
  if (covered != parent_taxa) {
    throw std::runtime_error(
        "clade grammar: node " + std::to_string(node_idx) +
        " production children do not union to parent clade; covered " +
        taxon_id_vector_to_string(covered) + " parent " +
        taxon_id_vector_to_string(parent_taxa));
  }
}

inline bool is_root_passthrough_node(phylo_dag& dag, std::size_t node_idx) {
  if (is_ua_node(dag.get_node(node_idx))) return true;
  return has_ua_parent(dag, node_idx);
}

inline std::uint64_t saturated_add(std::uint64_t a, std::uint64_t b,
                                   bool& saturated) {
  auto max = std::numeric_limits<std::uint64_t>::max();
  if (max - a < b) {
    saturated = true;
    return max;
  }
  return a + b;
}

inline std::uint64_t saturated_mul(std::uint64_t a, std::uint64_t b,
                                   bool& saturated) {
  auto max = std::numeric_limits<std::uint64_t>::max();
  if (a != 0 && b > max / a) {
    saturated = true;
    return max;
  }
  return a * b;
}

inline void compute_tree_count_estimate(clade_grammar const& grammar,
                                        clade_grammar_audit& audit) {
  if (grammar.clades.empty() || grammar.root_clade == no_clade) return;

  std::vector<std::uint64_t> counts(grammar.clades.size(), 0);
  bool saturated = false;
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto const& prods = grammar.productions_by_parent[cid];
    if (prods.empty()) {
      counts[cid] = 1;
      continue;
    }

    std::uint64_t total = 0;
    for (auto pid : prods) {
      std::uint64_t prod_count = 1;
      for (auto child : grammar.productions[pid].children) {
        if (child >= counts.size()) {
          saturated = true;
          prod_count = std::numeric_limits<std::uint64_t>::max();
          break;
        }
        prod_count = saturated_mul(prod_count, counts[child], saturated);
      }
      total = saturated_add(total, prod_count, saturated);
    }
    counts[cid] = total;
  }

  audit.grammar_tree_count_estimate = counts[grammar.root_clade];
  audit.grammar_tree_count_estimate_saturated = saturated;
}

inline void finalize_audit(clade_grammar const& grammar,
                           reachable_dag_info const& reachable,
                           clade_grammar_audit& audit, phylo_dag& dag) {
  audit.reachable_node_count = reachable.nodes.size();
  audit.reachable_edge_count = reachable.edges.size();
  audit.taxon_count = grammar.taxa.id_to_sample_id.size();
  audit.collapsed_clade_count = grammar.clades.size();
  audit.grammar_production_count = grammar.productions.size();
  audit.root_clade = grammar.root_clade;
  if (grammar.root_clade != no_clade) {
    audit.root_clade_size = grammar.clades[grammar.root_clade].taxa.size();
    audit.root_production_count =
        grammar.productions_by_parent[grammar.root_clade].size();
  }

  audit.collapsed_clade_node_count.assign(grammar.clades.size(), 0);
  for (auto node_idx : reachable.nodes) {
    if (is_ua_node(dag.get_node(node_idx))) continue;
    auto cid = node_idx < grammar.node_to_clade.size()
                   ? grammar.node_to_clade[node_idx]
                   : no_clade;
    if (cid != no_clade) ++audit.collapsed_clade_node_count[cid];
  }

  std::vector<std::size_t> counts = audit.collapsed_clade_node_count;
  if (!counts.empty()) {
    for (auto count : counts) {
      ++audit.nodes_per_clade_histogram[count];
      audit.max_nodes_per_collapsed_clade =
          std::max(audit.max_nodes_per_collapsed_clade, count);
    }
    auto sum = std::accumulate(counts.begin(), counts.end(), std::size_t{0});
    audit.mean_nodes_per_collapsed_clade =
        static_cast<double>(sum) / static_cast<double>(counts.size());
    std::sort(counts.begin(), counts.end());
    if (counts.size() % 2 == 1) {
      audit.median_nodes_per_collapsed_clade =
          static_cast<double>(counts[counts.size() / 2]);
    } else {
      auto hi = counts.size() / 2;
      audit.median_nodes_per_collapsed_clade =
          (static_cast<double>(counts[hi - 1]) + static_cast<double>(counts[hi])) /
          2.0;
    }
  }

  for (auto const& prod : grammar.productions)
    ++audit.production_multiplicity_histogram[prod.multiplicity];

  compute_tree_count_estimate(grammar, audit);
}

}  // namespace detail

inline clade_grammar_build_result build_clade_grammar_with_audit(
    phylo_dag& dag, clade_grammar_options options = {}) {
  // Repair stale/missing clade offsets so get_clades has stable semantics.
  build_clade_offsets(dag);

  clade_grammar_build_result result;
  auto& grammar = result.grammar;
  auto& audit = result.audit;

  auto reachable = detail::collect_reachable(dag);
  detail::update_strict_nucleotide_counts(dag, reachable, audit);
  grammar.taxa = detail::build_taxon_registry(dag, reachable, options, audit);

  std::vector<std::vector<taxon_id>> node_taxa(dag.node_high_mark());
  detail::node_taxa_computer taxa_computer{dag, grammar.taxa, reachable,
                                           node_taxa};
  auto root_idx = get_root_idx(dag);
  (void)taxa_computer.compute(root_idx);
  for (auto node_idx : reachable.nodes) (void)taxa_computer.compute(node_idx);

  std::vector<clade_key> unique_clades;
  unique_clades.reserve(reachable.nodes.size());
  for (auto node_idx : reachable.nodes) {
    if (node_taxa[node_idx].empty()) continue;
    unique_clades.push_back(clade_key{node_taxa[node_idx]});
  }
  std::sort(unique_clades.begin(), unique_clades.end(), detail::clade_key_less);
  unique_clades.erase(
      std::unique(unique_clades.begin(), unique_clades.end(),
                  [](clade_key const& a, clade_key const& b) {
                    return a.taxa == b.taxa;
                  }),
      unique_clades.end());
  if (unique_clades.size() >= static_cast<std::size_t>(no_clade))
    throw std::runtime_error("clade grammar: too many collapsed clades");

  grammar.clades = std::move(unique_clades);
  std::map<std::vector<taxon_id>, clade_id> key_to_clade;
  for (std::size_t i = 0; i < grammar.clades.size(); ++i) {
    key_to_clade.emplace(grammar.clades[i].taxa, static_cast<clade_id>(i));
  }

  grammar.node_to_clade.assign(dag.node_high_mark(), no_clade);
  for (auto node_idx : reachable.nodes) {
    auto it = key_to_clade.find(node_taxa[node_idx]);
    if (it == key_to_clade.end())
      throw std::runtime_error("clade grammar: internal clade interning error");
    grammar.node_to_clade[node_idx] = it->second;
  }
  grammar.root_clade = grammar.node_to_clade[root_idx];

  std::map<detail::production_map_key, grammar_production> production_map;

  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (detail::is_ua_node(nv) || detail::is_leaf_node(nv)) continue;

    auto parent_clade = grammar.node_to_clade[node_idx];
    if (parent_clade == no_clade)
      throw std::runtime_error("clade grammar: parent node has no clade");

    // Singleton clades are chart leaves even if multiple identical sample_id
    // leaves were coalesced underneath an internal history node.
    if (grammar.clades[parent_clade].taxa.size() == 1) continue;

    auto clade_groups = get_clades(dag, node_idx);
    if (clade_groups.empty()) {
      throw std::runtime_error("clade grammar: internal node " +
                               std::to_string(node_idx) +
                               " has no clade-index groups");
    }

    std::vector<clade_id> children;
    std::vector<production_child_witness> witness_children;
    children.reserve(clade_groups.size());
    witness_children.reserve(clade_groups.size());

    for (std::size_t group_idx = 0; group_idx < clade_groups.size();
         ++group_idx) {
      auto const& edge_group = clade_groups[group_idx];
      if (edge_group.empty()) {
        ++audit.invalid_clade_index_group_count;
        throw std::runtime_error(
            "clade grammar: node " + std::to_string(node_idx) +
            " has empty clade-index group " + std::to_string(group_idx));
      }

      std::set<clade_id> child_clades;
      for (auto edge_idx : edge_group) {
        auto child_idx = get_child_idx(dag, edge_idx);
        auto child_clade = child_idx < grammar.node_to_clade.size()
                               ? grammar.node_to_clade[child_idx]
                               : no_clade;
        if (child_clade != no_clade) child_clades.insert(child_clade);
      }
      if (child_clades.size() != 1) {
        ++audit.invalid_clade_index_group_count;
        throw std::runtime_error(
            "clade grammar: node " + std::to_string(node_idx) +
            " clade-index group " + std::to_string(group_idx) +
            " maps to " + std::to_string(child_clades.size()) +
            " collapsed child clades " +
            detail::clade_id_set_to_string(child_clades));
      }

      auto child_clade = *child_clades.begin();
      children.push_back(child_clade);
      auto& child_witness = witness_children.emplace_back();
      child_witness.child = child_clade;
      child_witness.edge_alternatives = edge_group;
      std::sort(child_witness.edge_alternatives.begin(),
                child_witness.edge_alternatives.end());
    }

    if (children.size() == 1 && children.front() == parent_clade &&
        detail::is_root_passthrough_node(dag, node_idx)) {
      continue;
    }

    if (children.size() == 1) {
      ++audit.non_binary_production_count;
      throw std::runtime_error(
          "clade grammar: node " + std::to_string(node_idx) +
          " would create non-UA unary production");
    }
    if (children.size() != 2) {
      ++audit.non_binary_production_count;
      if (!options.allow_polytomies) {
        throw std::runtime_error(
            "clade grammar: node " + std::to_string(node_idx) +
            " would create non-binary production of arity " +
            std::to_string(children.size()));
      }
    }

    for (auto child : children) {
      if (child == parent_clade) {
        ++audit.non_binary_production_count;
        throw std::runtime_error(
            "clade grammar: node " + std::to_string(node_idx) +
            " would create a non-root self-production");
      }
    }

    detail::validate_child_clade_partition(grammar, parent_clade, children,
                                           node_idx);

    if (options.canonicalize_binary_children && children.size() > 1) {
      std::vector<std::size_t> permutation(children.size());
      std::iota(permutation.begin(), permutation.end(), std::size_t{0});
      std::stable_sort(permutation.begin(), permutation.end(),
                       [&](std::size_t lhs, std::size_t rhs) {
                         return children[lhs] < children[rhs];
                       });
      std::vector<clade_id> sorted_children;
      std::vector<production_child_witness> sorted_witness_children;
      sorted_children.reserve(children.size());
      sorted_witness_children.reserve(witness_children.size());
      for (auto old_idx : permutation) {
        sorted_children.push_back(children[old_idx]);
        sorted_witness_children.push_back(std::move(witness_children[old_idx]));
      }
      children = std::move(sorted_children);
      witness_children = std::move(sorted_witness_children);
    }

    production_witness witness;
    witness.parent_node = node_idx;
    witness.children = std::move(witness_children);

    detail::production_map_key key{parent_clade, children};
    auto [it, inserted] = production_map.emplace(key, grammar_production{});
    auto& prod = it->second;
    if (inserted) {
      prod.parent = parent_clade;
      prod.children = std::move(children);
    }
    prod.witnesses.push_back(std::move(witness));
    ++prod.multiplicity;
  }

  if (production_map.size() >= static_cast<std::size_t>(no_production))
    throw std::runtime_error("clade grammar: too many productions");

  grammar.productions.reserve(production_map.size());
  for (auto& [_, prod] : production_map) {
    std::sort(prod.witnesses.begin(), prod.witnesses.end(),
              [](production_witness const& a, production_witness const& b) {
                return a.parent_node < b.parent_node;
              });
    grammar.productions.push_back(std::move(prod));
  }

  grammar.productions_by_parent.assign(grammar.clades.size(), {});
  grammar.productions_by_child.assign(grammar.clades.size(), {});
  for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
    auto pid = static_cast<production_id>(i);
    auto const& prod = grammar.productions[i];
    grammar.productions_by_parent[prod.parent].push_back(pid);

    std::vector<clade_id> unique_children = prod.children;
    std::sort(unique_children.begin(), unique_children.end());
    unique_children.erase(std::unique(unique_children.begin(), unique_children.end()),
                          unique_children.end());
    for (auto child : unique_children) {
      if (child == no_clade || child >= grammar.clades.size())
        throw std::runtime_error("clade grammar: production child clade out of range");
      grammar.productions_by_child[child].push_back(pid);
    }
  }

  detail::finalize_audit(grammar, reachable, audit, dag);
  return result;
}

inline clade_grammar build_clade_grammar(phylo_dag& dag,
                                         clade_grammar_options options = {}) {
  return build_clade_grammar_with_audit(dag, options).grammar;
}

inline std::ostream& print_clade_grammar_audit(
    std::ostream& out, clade_grammar_audit const& audit) {
  out << "wric_audit:\n";
  out << "  reachable_nodes: " << audit.reachable_node_count << "\n";
  out << "  reachable_edges: " << audit.reachable_edge_count << "\n";
  out << "  taxa: " << audit.taxon_count << "\n";
  out << "  duplicate_sample_id_occurrences: "
      << audit.duplicate_sample_id_occurrences << "\n";
  out << "  collapsed_clades: " << audit.collapsed_clade_count << "\n";
  out << "  grammar_productions: " << audit.grammar_production_count << "\n";
  out << "  root_clade: ";
  if (audit.root_clade == no_clade)
    out << "none\n";
  else
    out << audit.root_clade << "\n";
  out << "  root_clade_size: " << audit.root_clade_size << "\n";
  out << "  root_productions: " << audit.root_production_count << "\n";
  out << "  nodes_per_collapsed_clade_max: "
      << audit.max_nodes_per_collapsed_clade << "\n";
  out << "  nodes_per_collapsed_clade_mean: " << std::fixed
      << std::setprecision(3) << audit.mean_nodes_per_collapsed_clade << "\n";
  out << "  nodes_per_collapsed_clade_median: " << std::fixed
      << std::setprecision(3) << audit.median_nodes_per_collapsed_clade << "\n";
  out << "  invalid_clade_index_groups: "
      << audit.invalid_clade_index_group_count << "\n";
  out << "  non_binary_productions: " << audit.non_binary_production_count
      << "\n";
  out << "  strict_reference_acgt_pass: "
      << audit.strict_reference_acgt_pass << "\n";
  out << "  strict_reference_acgt_fail: "
      << audit.strict_reference_acgt_fail << "\n";
  out << "  strict_leaf_compact_genome_acgt_pass: "
      << audit.strict_leaf_compact_genome_acgt_pass << "\n";
  out << "  strict_leaf_compact_genome_acgt_fail: "
      << audit.strict_leaf_compact_genome_acgt_fail << "\n";
  out << "  grammar_tree_count_estimate: "
      << audit.grammar_tree_count_estimate
      << (audit.grammar_tree_count_estimate_saturated ? " (saturated)" : "")
      << "\n";

  out << "  nodes_per_clade_histogram:\n";
  if (audit.nodes_per_clade_histogram.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [nodes, count] : audit.nodes_per_clade_histogram)
      out << "    " << nodes << ": " << count << "\n";
  }

  out << "  production_multiplicity_histogram:\n";
  if (audit.production_multiplicity_histogram.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [multiplicity, count] :
         audit.production_multiplicity_histogram)
      out << "    " << multiplicity << ": " << count << "\n";
  }
  return out;
}

}  // namespace larch

