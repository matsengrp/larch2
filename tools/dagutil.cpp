#include <larch/load_proto_dag.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/sample_method.hpp>
#include <larch/fasta.hpp>
#include <larch/vcf.hpp>
#include <larch/merge.hpp>
#include <larch/compute.hpp>
#include <larch/model_variant.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/weight_accumulator.hpp>
#include <larch/rf_distance.hpp>
#include <larch/thread_pool.hpp>
#include <larch/version.hpp>
#include <larch/pmr_arena.hpp>
#include <larch/io_util.hpp>
#include <larch/newick.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>
#include <larch/chart_trim.hpp>
#include <larch/chart_bnb_trim_apply.hpp>
#include <larch/chart_spr_search.hpp>
#include <larch/plateau.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// Read reference sequence from file (FASTA or raw text)
// ---------------------------------------------------------------------------

static nuc_base strict_fasta_nuc_from_char(char c, std::string_view label,
                                           mutation_position pos) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base{nuc_base::A};
    case 'C':
    case 'c':
      return nuc_base{nuc_base::C};
    case 'G':
    case 'g':
      return nuc_base{nuc_base::G};
    case 'T':
    case 't':
      return nuc_base{nuc_base::T};
    default:
      throw std::runtime_error(std::string{"FASTA/Newick input: non-ACGT "} +
                               std::string{label} + " nucleotide '" + c +
                               "' at position " + std::to_string(pos));
  }
}

static void validate_fasta_acgt_sequence(std::string_view sequence,
                                         std::string_view label) {
  for (std::size_t i = 0; i < sequence.size(); ++i)
    (void)strict_fasta_nuc_from_char(sequence[i], label, i + 1);
}

static std::string read_refseq(std::string_view path) {
  auto bytes = read_file(path);
  std::string_view content{bytes.data(), bytes.size()};
  if (!content.empty() && content[0] == '>') {
    auto entries = read_fasta(path);
    if (!entries.empty()) return std::move(entries[0].sequence);
  }
  std::string ref;
  for (char c : content) {
    if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
      ref += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return ref;
}

// ---------------------------------------------------------------------------
// FASTA+Newick DAG builder (same logic as larch2.cpp)
// ---------------------------------------------------------------------------

static phylo_dag build_from_fasta_newick(std::string_view fasta_path,
                                         std::string_view newick_path,
                                         std::string const& reference) {
  validate_fasta_acgt_sequence(reference, "reference");

  auto entries = read_fasta(fasta_path);
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto& e : entries) {
    validate_fasta_acgt_sequence(e.sequence,
                                 std::string{"sample '"} + e.name + "'");
    fasta_map[e.name] = std::move(e.sequence);
  }

  auto nw_bytes = read_file(newick_path);
  std::string newick_str{nw_bytes.data(), nw_bytes.size()};

  phylo_dag d;

  std::unordered_map<std::size_t, std::size_t> num_children;
  std::map<std::size_t, std::string> seq_ids;
  struct nw_edge_t {
    std::size_t parent, child, clade;
  };
  std::vector<nw_edge_t> nw_edges;

  parse_newick(
      newick_str,
      [&](std::size_t id, std::string_view label, std::optional<double>) {
        seq_ids[id] = std::string{label};
      },
      [&](std::size_t parent, std::size_t child) {
        nw_edges.push_back({parent, child, num_children[parent]++});
      });

  std::unordered_map<std::size_t, bool> has_children;
  std::unordered_map<std::size_t, bool> is_child_map;
  for (auto& e : nw_edges) {
    has_children[e.parent] = true;
    is_child_map[e.child] = true;
  }

  std::size_t newick_root = 0;
  for (auto& [id, _] : seq_ids) {
    if (!is_child_map.contains(id)) {
      newick_root = id;
      break;
    }
  }

  std::unordered_map<std::size_t, std::size_t> nw_to_dag;
  for (auto& [nw_id, label] : seq_ids) {
    bool is_leaf_node = !has_children.contains(nw_id);
    if (is_leaf_node) {
      auto leaf = d.append_node<node_kind::leaf>();
      if (!label.empty()) leaf.sample_id() = label;
      nw_to_dag[nw_id] = leaf.index();
    } else {
      auto inner = d.append_node<node_kind::inner>();
      nw_to_dag[nw_id] = inner.index();
    }
  }

  for (auto& ne : nw_edges) {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = ne.clade;
    auto pi = nw_to_dag[ne.parent];
    auto ci = nw_to_dag[ne.child];
    std::visit([&](auto n) { edge.set_parent(n); }, d.get_node(pi));
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(ci));
  }

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = reference;
  d.set_root(ua);
  {
    auto edge = ua.append_child<edge_kind::clade>();
    auto root_dag_idx = nw_to_dag[newick_root];
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(root_dag_idx));
    edge.clade_index() = 0;
  }

  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            auto it = fasta_map.find(node.sample_id());
            if (it == fasta_map.end()) return;
            auto& seq = it->second;
            std::map<mutation_position, nuc_base> muts;
            for (std::size_t i = 0; i < seq.size() && i < reference.size();
                 ++i) {
              auto ref_base = strict_fasta_nuc_from_char(reference[i],
                                                         "reference", i + 1);
              auto seq_base = strict_fasta_nuc_from_char(
                  seq[i], std::string{"sample '"} + node.sample_id() + "'",
                  i + 1);
              if (!(ref_base == seq_base)) muts[i + 1] = seq_base;
            }
            node.cg() = compact_genome{std::move(muts)};
          }
        },
        nv);
  }

  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);
  return d;
}

// ---------------------------------------------------------------------------
// WRIC/chart diagnostic helpers
// ---------------------------------------------------------------------------

static char chart_state_label(std::uint8_t state) {
  switch (state) {
    case nuc_base::A:
      return 'A';
    case nuc_base::C:
      return 'C';
    case nuc_base::G:
      return 'G';
    case nuc_base::T:
      return 'T';
    default:
      return '?';
  }
}

static void print_chart_cost(std::ostream& out, chart_cost cost) {
  if (cost >= chart_inf)
    out << "INF";
  else
    out << cost;
}

static void print_chart_row(
    std::ostream& out,
    std::array<chart_cost, nuc_state_count> const& row) {
  out << "[";
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    if (state != 0) out << ", ";
    out << chart_state_label(state) << ":";
    print_chart_cost(out, row[state]);
  }
  out << "]";
}

static double elapsed_ms(std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

static bool within_limit(std::size_t printed, std::size_t limit) {
  return limit == 0 || printed < limit;
}

static std::size_t count_kept_clade_states(chart_trim_mask const& mask) {
  std::size_t count = 0;
  for (auto const& row : mask.keep_clade_state)
    for (bool keep : row)
      if (keep) ++count;
  return count;
}

static std::size_t count_true(std::vector<bool> const& values) {
  return static_cast<std::size_t>(
      std::count(values.begin(), values.end(), true));
}

static void print_frontier_size_stats(
    std::ostream& out, std::vector<std::size_t> const& frontier_sizes) {
  std::size_t sum = 0;
  std::size_t max_size = 0;
  std::map<std::size_t, std::size_t> hist;
  for (auto size : frontier_sizes) {
    sum += size;
    max_size = std::max(max_size, size);
    ++hist[size];
  }
  out << "  frontier_size_sum: " << sum << "\n";
  out << "  frontier_size_max: " << max_size << "\n";
  out << "  frontier_size_histogram:\n";
  if (hist.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [size, count] : hist)
      out << "    " << size << ": " << count << "\n";
  }
}

static std::vector<std::size_t> direct_parent_witness_nodes(
    grammar_production const& prod) {
  std::vector<std::size_t> nodes;
  nodes.reserve(prod.witnesses.size());
  for (auto const& witness : prod.witnesses) {
    if (witness.parent_node != std::numeric_limits<std::size_t>::max())
      nodes.push_back(witness.parent_node);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

static void print_size_list(std::ostream& out,
                            std::vector<std::size_t> const& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  out << "]";
}

static char const* wric_polytomy_mode_name(polytomy_mode mode) {
  switch (mode) {
    case polytomy_mode::reject:
      return "reject";
    case polytomy_mode::audit_kary:
      return "audit-kary";
    case polytomy_mode::expand_soft_exact_or_fail:
      return "expand-exact";
    case polytomy_mode::expand_soft_bounded:
      return "expand-bounded";
  }
  return "unknown";
}

struct wric_polytomy_report_totals {
  std::size_t selected_seed_shapes = 0;
  std::uint64_t refined_grammar_tree_count = 0;
  bool refined_grammar_tree_count_saturated = false;
  std::uint64_t sum_event_represented_refinement_counts = 0;
  bool sum_event_represented_refinement_counts_saturated = false;
  std::size_t truncated_events = 0;
};

static std::uint64_t dagutil_saturated_add(std::uint64_t lhs,
                                           std::uint64_t rhs,
                                           bool& saturated) {
  auto max = std::numeric_limits<std::uint64_t>::max();
  if (max - lhs < rhs) {
    saturated = true;
    return max;
  }
  return lhs + rhs;
}

static std::uint64_t dagutil_saturated_mul(std::uint64_t lhs,
                                           std::uint64_t rhs,
                                           bool& saturated) {
  auto max = std::numeric_limits<std::uint64_t>::max();
  if (lhs != 0 && rhs > max / lhs) {
    saturated = true;
    return max;
  }
  return lhs * rhs;
}

static constexpr std::uint64_t k_wric_exact_warning_clade_threshold = 4096;
static constexpr std::uint64_t k_wric_exact_warning_production_threshold =
    100000;
static constexpr std::uint64_t k_wric_exact_warning_refinement_threshold =
    1000;
static constexpr std::size_t k_wric_benchmark_shape_cap_limit = 100000;

struct wric_exact_event_size_estimate {
  std::size_t arity = 0;
  std::uint64_t synthetic_clade_upper_bound = 0;
  std::uint64_t binary_production_upper_bound = 0;
  std::uint64_t rooted_binary_refinement_count = 0;
  bool synthetic_clade_upper_bound_saturated = false;
  bool binary_production_upper_bound_saturated = false;
  bool rooted_binary_refinement_count_saturated = false;

  [[nodiscard]] bool size_estimates_saturated() const {
    return synthetic_clade_upper_bound_saturated ||
           binary_production_upper_bound_saturated;
  }
};

static std::uint64_t wric_saturated_pow_u64(std::uint64_t base,
                                            std::size_t exp,
                                            bool& saturated) {
  std::uint64_t result = 1;
  for (std::size_t i = 0; i < exp; ++i)
    result = dagutil_saturated_mul(result, base, saturated);
  return result;
}

static std::uint64_t wric_rooted_binary_refinement_count(std::size_t arity,
                                                         bool& saturated) {
  if (arity <= 2) return 1;
  std::uint64_t result = 1;
  for (std::size_t value = 3; value <= 2 * arity - 3; value += 2)
    result = dagutil_saturated_mul(result, static_cast<std::uint64_t>(value),
                                   saturated);
  return result;
}

static wric_exact_event_size_estimate estimate_wric_exact_event_size(
    std::size_t arity) {
  wric_exact_event_size_estimate estimate;
  estimate.arity = arity;

  if (arity >= 3) {
    bool clade_saturated = false;
    auto pow2 = wric_saturated_pow_u64(2, arity, clade_saturated);
    if (clade_saturated || pow2 < arity + 2) {
      estimate.synthetic_clade_upper_bound_saturated = true;
      estimate.synthetic_clade_upper_bound =
          std::numeric_limits<std::uint64_t>::max();
    } else {
      estimate.synthetic_clade_upper_bound =
          pow2 - static_cast<std::uint64_t>(arity) - 2;
    }
  }

  if (arity >= 2) {
    bool production_saturated = false;
    auto pow3 = wric_saturated_pow_u64(3, arity, production_saturated);
    auto pow2_next =
        wric_saturated_pow_u64(2, arity + 1, production_saturated);
    if (production_saturated || pow3 < pow2_next - 1) {
      estimate.binary_production_upper_bound_saturated = true;
      estimate.binary_production_upper_bound =
          std::numeric_limits<std::uint64_t>::max();
    } else {
      estimate.binary_production_upper_bound =
          (pow3 - pow2_next + 1) / 2;
    }
  }

  bool refinement_saturated = false;
  estimate.rooted_binary_refinement_count =
      wric_rooted_binary_refinement_count(arity, refinement_saturated);
  estimate.rooted_binary_refinement_count_saturated = refinement_saturated;
  return estimate;
}

static bool wric_u64_exceeds_size_cap(std::uint64_t value, bool saturated,
                                      std::size_t cap) {
  if (saturated) return true;
  if (value >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return true;
  }
  return static_cast<std::size_t>(value) > cap;
}

static std::size_t recommended_wric_exact_arity_cap() {
  polytomy_refinement_options defaults;
  std::size_t recommendation = 2;
  for (std::size_t arity = 3; arity <= 63; ++arity) {
    auto estimate = estimate_wric_exact_event_size(arity);
    if (wric_u64_exceeds_size_cap(
            estimate.synthetic_clade_upper_bound,
            estimate.synthetic_clade_upper_bound_saturated,
            defaults.max_new_clades_per_polytomy) ||
        wric_u64_exceeds_size_cap(
            estimate.binary_production_upper_bound,
            estimate.binary_production_upper_bound_saturated,
            defaults.max_new_productions_per_polytomy) ||
        estimate.rooted_binary_refinement_count_saturated ||
        estimate.rooted_binary_refinement_count >
            k_wric_exact_warning_refinement_threshold) {
      break;
    }
    recommendation = arity;
  }
  return recommendation;
}

static std::uint64_t grammar_tree_count_for_clade(
    clade_grammar const& grammar, clade_id clade,
    std::vector<std::uint64_t>& memo, std::vector<bool>& memoized,
    std::vector<bool>& visiting, bool& saturated) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }
  if (memoized[clade]) return memo[clade];
  if (visiting[clade]) {
    throw std::runtime_error("WRIC grammar tree-count DP found a cycle");
  }
  visiting[clade] = true;

  if (clade >= grammar.productions_by_parent.size()) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }

  std::uint64_t total = 0;
  auto const& parent_productions = grammar.productions_by_parent[clade];
  if (parent_productions.empty()) {
    total = 1;
  } else {
    for (auto pid : parent_productions) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        saturated = true;
        total = std::numeric_limits<std::uint64_t>::max();
        break;
      }
      std::uint64_t prod_count = 1;
      for (auto child : grammar.productions[pid].children) {
        auto child_count = grammar_tree_count_for_clade(
            grammar, child, memo, memoized, visiting, saturated);
        prod_count = dagutil_saturated_mul(prod_count, child_count,
                                           saturated);
      }
      total = dagutil_saturated_add(total, prod_count, saturated);
    }
  }

  visiting[clade] = false;
  memo[clade] = total;
  memoized[clade] = true;
  return total;
}

static std::uint64_t grammar_tree_count(clade_grammar const& grammar,
                                        bool& saturated) {
  if (grammar.clades.empty() || grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    return 0;
  }
  std::vector<std::uint64_t> memo(grammar.clades.size(), 0);
  std::vector<bool> memoized(grammar.clades.size(), false);
  std::vector<bool> visiting(grammar.clades.size(), false);
  return grammar_tree_count_for_clade(grammar, grammar.root_clade, memo,
                                      memoized, visiting, saturated);
}

static std::uint64_t soft_refinement_tree_count_for_clade(
    clade_grammar const& grammar, clade_id clade,
    std::vector<std::uint64_t>& memo, std::vector<bool>& memoized,
    std::vector<bool>& visiting, bool& saturated) {
  if (clade == no_clade || clade >= grammar.clades.size()) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }
  if (memoized[clade]) return memo[clade];
  if (visiting[clade]) {
    throw std::runtime_error(
        "WRIC soft-refinement tree-count DP found a cycle");
  }
  visiting[clade] = true;

  std::uint64_t total = 0;
  auto const& parent_productions = grammar.productions_by_parent[clade];
  if (parent_productions.empty()) {
    total = 1;
  } else {
    for (auto pid : parent_productions) {
      if (pid == no_production || pid >= grammar.productions.size()) {
        saturated = true;
        total = std::numeric_limits<std::uint64_t>::max();
        break;
      }
      auto const& prod = grammar.productions[pid];
      auto arity = prod.children.size();
      auto prod_weight = arity >= 3 ? wric_rooted_binary_refinement_count(
                                          arity, saturated)
                                    : std::uint64_t{1};
      std::uint64_t prod_count = prod_weight;
      for (auto child : prod.children) {
        auto child_count = soft_refinement_tree_count_for_clade(
            grammar, child, memo, memoized, visiting, saturated);
        prod_count = dagutil_saturated_mul(prod_count, child_count,
                                           saturated);
      }
      total = dagutil_saturated_add(total, prod_count, saturated);
    }
  }

  visiting[clade] = false;
  memo[clade] = total;
  memoized[clade] = true;
  return total;
}

static std::uint64_t soft_refinement_tree_count_estimate(
    clade_grammar const& grammar, bool& saturated) {
  if (grammar.clades.empty() || grammar.root_clade == no_clade ||
      grammar.root_clade >= grammar.clades.size()) {
    return 0;
  }
  std::vector<std::uint64_t> memo(grammar.clades.size(), 0);
  std::vector<bool> memoized(grammar.clades.size(), false);
  std::vector<bool> visiting(grammar.clades.size(), false);
  return soft_refinement_tree_count_for_clade(
      grammar, grammar.root_clade, memo, memoized, visiting, saturated);
}

static wric_polytomy_report_totals summarize_wric_polytomy_report(
    polytomy_refinement_result const& refinement) {
  auto const& audit = refinement.audit;
  wric_polytomy_report_totals totals;
  totals.refined_grammar_tree_count =
      grammar_tree_count(refinement.grammar,
                         totals.refined_grammar_tree_count_saturated);
  for (auto const& event : audit.events) {
    totals.selected_seed_shapes += event.selected_seed_shape_count;
    if (event.truncated_by_shape_cap || event.truncated_by_production_cap)
      ++totals.truncated_events;
    if (event.refinement_count_saturated) {
      totals.sum_event_represented_refinement_counts_saturated = true;
      totals.sum_event_represented_refinement_counts =
          std::numeric_limits<std::uint64_t>::max();
      continue;
    }
    totals.sum_event_represented_refinement_counts = dagutil_saturated_add(
        totals.sum_event_represented_refinement_counts,
        event.represented_refinement_count,
        totals.sum_event_represented_refinement_counts_saturated);
  }
  return totals;
}

static std::map<std::size_t, std::size_t> source_kary_arity_histogram(
    polytomy_refinement_result const& refinement) {
  std::map<std::size_t, std::size_t> histogram;
  for (auto const& event : refinement.audit.events) {
    if (event.arity >= 3) ++histogram[event.arity];
  }
  if (!histogram.empty()) return histogram;

  // Fallback for diagnostic grammars not represented by event records.
  auto const& grammar = refinement.grammar;
  for (auto pid : kary_productions(grammar))
    ++histogram[grammar.productions[pid].children.size()];
  return histogram;
}

struct wric_exact_preflight_summary {
  std::size_t event_count = 0;
  std::size_t max_arity = 0;
  std::uint64_t total_synthetic_clade_upper_bound = 0;
  std::uint64_t total_binary_production_upper_bound = 0;
  std::uint64_t full_soft_refinement_count_product = 1;
  bool total_synthetic_clade_upper_bound_saturated = false;
  bool total_binary_production_upper_bound_saturated = false;
  bool full_soft_refinement_count_product_saturated = false;
  std::size_t events_exceeding_exact_arity_cap = 0;
  std::size_t events_exceeding_clade_cap = 0;
  std::size_t events_exceeding_production_cap = 0;
  std::size_t events_exceeding_warning_threshold = 0;
  bool exceeds_total_clade_cap = false;
  bool exceeds_total_production_cap = false;

  [[nodiscard]] bool fits_configured_exact_caps() const {
    return events_exceeding_exact_arity_cap == 0 &&
           events_exceeding_clade_cap == 0 &&
           events_exceeding_production_cap == 0 &&
           !exceeds_total_clade_cap && !exceeds_total_production_cap &&
           !total_synthetic_clade_upper_bound_saturated &&
           !total_binary_production_upper_bound_saturated;
  }
};

static wric_exact_preflight_summary summarize_wric_exact_preflight(
    polytomy_refinement_result const& refinement,
    polytomy_refinement_options const& options) {
  wric_exact_preflight_summary summary;
  summary.event_count = refinement.audit.events.size();
  for (auto const& event : refinement.audit.events) {
    if (event.arity < 3) continue;
    summary.max_arity = std::max(summary.max_arity, event.arity);
    auto estimate = estimate_wric_exact_event_size(event.arity);
    summary.total_synthetic_clade_upper_bound_saturated |=
        estimate.synthetic_clade_upper_bound_saturated;
    summary.total_synthetic_clade_upper_bound = dagutil_saturated_add(
        summary.total_synthetic_clade_upper_bound,
        estimate.synthetic_clade_upper_bound,
        summary.total_synthetic_clade_upper_bound_saturated);
    summary.total_binary_production_upper_bound_saturated |=
        estimate.binary_production_upper_bound_saturated;
    summary.total_binary_production_upper_bound = dagutil_saturated_add(
        summary.total_binary_production_upper_bound,
        estimate.binary_production_upper_bound,
        summary.total_binary_production_upper_bound_saturated);
    summary.full_soft_refinement_count_product_saturated |=
        estimate.rooted_binary_refinement_count_saturated;
    summary.full_soft_refinement_count_product = dagutil_saturated_mul(
        summary.full_soft_refinement_count_product,
        estimate.rooted_binary_refinement_count,
        summary.full_soft_refinement_count_product_saturated);

    if (event.arity > options.max_exact_arity || event.arity > 63)
      ++summary.events_exceeding_exact_arity_cap;
    if (wric_u64_exceeds_size_cap(
            estimate.synthetic_clade_upper_bound,
            estimate.synthetic_clade_upper_bound_saturated,
            options.max_new_clades_per_polytomy)) {
      ++summary.events_exceeding_clade_cap;
    }
    if (wric_u64_exceeds_size_cap(
            estimate.binary_production_upper_bound,
            estimate.binary_production_upper_bound_saturated,
            options.max_new_productions_per_polytomy)) {
      ++summary.events_exceeding_production_cap;
    }
    if (estimate.size_estimates_saturated() ||
        estimate.rooted_binary_refinement_count_saturated ||
        estimate.synthetic_clade_upper_bound >
            k_wric_exact_warning_clade_threshold ||
        estimate.binary_production_upper_bound >
            k_wric_exact_warning_production_threshold ||
        estimate.rooted_binary_refinement_count >
            k_wric_exact_warning_refinement_threshold) {
      ++summary.events_exceeding_warning_threshold;
    }
  }

  summary.exceeds_total_clade_cap = wric_u64_exceeds_size_cap(
      summary.total_synthetic_clade_upper_bound,
      summary.total_synthetic_clade_upper_bound_saturated,
      options.max_total_new_clades);
  summary.exceeds_total_production_cap = wric_u64_exceeds_size_cap(
      summary.total_binary_production_upper_bound,
      summary.total_binary_production_upper_bound_saturated,
      options.max_total_new_productions);
  return summary;
}

static void print_saturated_u64(std::ostream& out, std::uint64_t value,
                                bool saturated) {
  out << value;
  if (saturated) out << " (saturated)";
}

static void print_wric_exact_expansion_preflight(
    std::ostream& out, polytomy_refinement_result const& refinement,
    polytomy_refinement_options const& options) {
  auto summary = summarize_wric_exact_preflight(refinement, options);
  out << "  exact_expansion_preflight:\n";
  out << "    default_exact_arity_cap_recommendation: "
      << recommended_wric_exact_arity_cap() << "\n";
  out << "    default_exact_arity_cap_recommendation_basis: "
      << "largest arity within default per-event size caps and rooted "
         "refinement warning threshold\n";
  out << "    configured_max_exact_arity: " << options.max_exact_arity
      << "\n";
  out << "    configured_max_new_clades_per_polytomy: "
      << options.max_new_clades_per_polytomy << "\n";
  out << "    configured_max_new_productions_per_polytomy: "
      << options.max_new_productions_per_polytomy << "\n";
  out << "    warning_clade_threshold: "
      << k_wric_exact_warning_clade_threshold << "\n";
  out << "    warning_production_threshold: "
      << k_wric_exact_warning_production_threshold << "\n";
  out << "    warning_rooted_refinement_count_threshold: "
      << k_wric_exact_warning_refinement_threshold << "\n";
  out << "    source_kary_events: " << summary.event_count << "\n";
  out << "    max_observed_kary_arity: " << summary.max_arity << "\n";
  out << "    total_exact_synthetic_clade_upper_bound: ";
  print_saturated_u64(out, summary.total_synthetic_clade_upper_bound,
                      summary.total_synthetic_clade_upper_bound_saturated);
  out << "\n";
  out << "    total_exact_binary_production_upper_bound: ";
  print_saturated_u64(out, summary.total_binary_production_upper_bound,
                      summary.total_binary_production_upper_bound_saturated);
  out << "\n";
  out << "    product_event_full_soft_refinement_count: ";
  print_saturated_u64(out, summary.full_soft_refinement_count_product,
                      summary.full_soft_refinement_count_product_saturated);
  out << "\n";
  out << "    events_exceeding_configured_exact_arity_cap: "
      << summary.events_exceeding_exact_arity_cap << "\n";
  out << "    events_whose_theoretical_clade_upper_bound_exceeds_configured_cap: "
      << summary.events_exceeding_clade_cap << "\n";
  out << "    events_whose_theoretical_production_upper_bound_exceeds_configured_cap: "
      << summary.events_exceeding_production_cap << "\n";
  out << "    events_exceeding_warning_threshold: "
      << summary.events_exceeding_warning_threshold << "\n";
  out << "    theoretical_total_clade_upper_bound_exceeds_total_cap: "
      << (summary.exceeds_total_clade_cap ? "true" : "false") << "\n";
  out << "    theoretical_total_production_upper_bound_exceeds_total_cap: "
      << (summary.exceeds_total_production_cap ? "true" : "false")
      << "\n";
  out << "    exact_expansion_theoretical_upper_bound_fits_configured_caps: "
      << (summary.fits_configured_exact_caps() ? "true" : "false")
      << "\n";
  out << "    exact_expansion_preflight_may_exceed_configured_caps: "
      << (summary.fits_configured_exact_caps() ? "false" : "true")
      << "\n";
  out << "    warnings:\n";
  bool printed_warning = false;
  auto print_warning = [&](std::string const& message) {
    printed_warning = true;
    out << "      - " << message << "\n";
  };
  if (summary.events_exceeding_exact_arity_cap != 0) {
    print_warning("one or more polytomies exceed the configured exact arity "
                  "cap; exact expansion will fail unless the cap is raised");
  }
  if (summary.events_exceeding_clade_cap != 0 ||
      summary.exceeds_total_clade_cap) {
    print_warning("theoretical upper-bound exact synthetic clades may exceed "
                  "configured clade caps; run exact expansion/benchmark to "
                  "report reuse-aware success or failure");
  }
  if (summary.events_exceeding_production_cap != 0 ||
      summary.exceeds_total_production_cap) {
    print_warning("theoretical upper-bound exact binary productions may "
                  "exceed configured production caps; run exact "
                  "expansion/benchmark to report reuse-aware success or "
                  "failure");
  }
  if (summary.events_exceeding_warning_threshold != 0) {
    print_warning("theoretical exact expansion estimates exceed benchmark "
                  "warning thresholds; audit/benchmark before scoring");
  }
  if (!printed_warning) out << "      <empty>\n";

  out << "    event_estimates:\n";
  if (refinement.audit.events.empty()) {
    out << "      <empty>\n";
    return;
  }
  for (auto const& event : refinement.audit.events) {
    auto estimate = estimate_wric_exact_event_size(event.arity);
    out << "      - source_production: " << event.source_production << "\n";
    out << "        arity: " << event.arity << "\n";
    out << "        exact_synthetic_clade_upper_bound: ";
    print_saturated_u64(out, estimate.synthetic_clade_upper_bound,
                        estimate.synthetic_clade_upper_bound_saturated);
    out << "\n";
    out << "        exact_binary_production_upper_bound: ";
    print_saturated_u64(out, estimate.binary_production_upper_bound,
                        estimate.binary_production_upper_bound_saturated);
    out << "\n";
    out << "        rooted_binary_refinement_count: ";
    print_saturated_u64(out, estimate.rooted_binary_refinement_count,
                        estimate.rooted_binary_refinement_count_saturated);
    out << "\n";
    out << "        exceeds_configured_exact_arity_cap: "
        << (event.arity > options.max_exact_arity || event.arity > 63
                ? "true"
                : "false")
        << "\n";
    out << "        theoretical_clade_upper_bound_exceeds_configured_cap: "
        << (wric_u64_exceeds_size_cap(
                estimate.synthetic_clade_upper_bound,
                estimate.synthetic_clade_upper_bound_saturated,
                options.max_new_clades_per_polytomy)
                ? "true"
                : "false")
        << "\n";
    out << "        theoretical_production_upper_bound_exceeds_configured_cap: "
        << (wric_u64_exceeds_size_cap(
                estimate.binary_production_upper_bound,
                estimate.binary_production_upper_bound_saturated,
                options.max_new_productions_per_polytomy)
                ? "true"
                : "false")
        << "\n";
  }
}

static void print_wric_polytomy_summary_fields(
    std::ostream& out, polytomy_refinement_result const& refinement,
    polytomy_mode mode) {
  auto const& audit = refinement.audit;
  auto totals = summarize_wric_polytomy_report(refinement);
  out << "  polytomy_mode: " << wric_polytomy_mode_name(mode) << "\n";
  out << "  polytomy_refinement_label: "
      << polytomy_refinement_status_label(audit) << "\n";
  out << "  source_kary_productions: "
      << audit.source_kary_production_count << "\n";
  out << "  contains_kary_productions: "
      << (audit.contains_kary_productions ? "true" : "false") << "\n";
  out << "  binary_chart_compatible: "
      << (audit.binary_chart_compatible ? "true" : "false") << "\n";
  out << "  synthetic_clades: " << audit.synthetic_clade_count << "\n";
  out << "  synthetic_productions: " << audit.synthetic_production_count
      << "\n";
  out << "  selected_seed_shapes: " << totals.selected_seed_shapes << "\n";
  out << "  represented_refinement_count: "
      << totals.refined_grammar_tree_count
      << (totals.refined_grammar_tree_count_saturated ? " (saturated)" : "")
      << "\n";
  out << "  sum_event_represented_refinement_counts: "
      << totals.sum_event_represented_refinement_counts
      << (totals.sum_event_represented_refinement_counts_saturated
              ? " (saturated)"
              : "")
      << "\n";
  out << "  exact_for_soft_polytomies: "
      << (audit.exact_for_soft_polytomies ? "true" : "false") << "\n";
  out << "  truncated_events: " << totals.truncated_events << "\n";
  out << "  any_truncated: " << (audit.any_truncated ? "true" : "false")
      << "\n";
}

static std::vector<std::size_t> source_parent_nodes_for_polytomy_event(
    polytomy_refinement_result const& refinement,
    polytomy_event_audit const& event) {
  std::vector<std::size_t> nodes;
  if (event.source_production == no_production) return nodes;
  for (auto const& info : refinement.production_info) {
    if (std::find(info.source_productions.begin(),
                  info.source_productions.end(),
                  event.source_production) == info.source_productions.end()) {
      continue;
    }
    nodes.insert(nodes.end(), info.source_parent_nodes.begin(),
                 info.source_parent_nodes.end());
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

static void print_wric_polytomy_event_details(
    std::ostream& out, polytomy_refinement_result const& refinement) {
  auto const& audit = refinement.audit;
  out << "  polytomy_refinement_events:\n";
  if (audit.events.empty()) {
    out << "    <empty>\n";
    return;
  }
  for (auto const& event : audit.events) {
    auto parent_nodes = source_parent_nodes_for_polytomy_event(refinement,
                                                               event);
    out << "    - source_production: " << event.source_production << "\n";
    out << "      source_parent_clade: " << event.source_parent << "\n";
    out << "      refined_parent_clade: " << event.parent << "\n";
    out << "      arity: " << event.arity << "\n";
    out << "      source_multiplicity: " << event.source_multiplicity << "\n";
    out << "      source_parent_nodes: ";
    print_size_list(out, parent_nodes);
    out << "\n";
    out << "      new_clades_added: " << event.new_clades_added << "\n";
    out << "      new_productions_added: " << event.new_productions_added
        << "\n";
    out << "      selected_seed_shapes: " << event.selected_seed_shape_count
        << "\n";
    out << "      represented_refinement_count: "
        << event.represented_refinement_count
        << (event.refinement_count_saturated ? " (saturated)" : "")
        << "\n";
    out << "      expanded: " << (event.expanded ? "true" : "false") << "\n";
    out << "      exact: " << (event.exact ? "true" : "false") << "\n";
    out << "      truncated_by_shape_cap: "
        << (event.truncated_by_shape_cap ? "true" : "false") << "\n";
    out << "      truncated_by_production_cap: "
        << (event.truncated_by_production_cap ? "true" : "false") << "\n";
    out << "      refused_by_exact_cap: "
        << (event.refused_by_exact_cap ? "true" : "false") << "\n";
  }
}

static void print_wric_polytomy_source_kary_details(
    std::ostream& out, polytomy_refinement_result const& refinement) {
  auto histogram = source_kary_arity_histogram(refinement);
  out << "  kary_productions: "
      << refinement.audit.source_kary_production_count << "\n";
  out << "  source_kary_production_arity_histogram:\n";
  if (histogram.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [arity, count] : histogram)
      out << "    " << arity << ": " << count << "\n";
  }

  out << "  source_kary_production_details:\n";
  auto const& grammar = refinement.grammar;
  if (!refinement.audit.events.empty()) {
    for (auto const& event : refinement.audit.events) {
      auto parent_nodes = source_parent_nodes_for_polytomy_event(refinement,
                                                                 event);
      out << "    - source_production: " << event.source_production << "\n";
      out << "      source_parent_clade: " << event.source_parent << "\n";
      out << "      refined_parent_clade: " << event.parent << "\n";
      out << "      arity: " << event.arity << "\n";
      out << "      witness_multiplicity: " << event.source_multiplicity
          << "\n";
      out << "      source_parent_nodes: ";
      print_size_list(out, parent_nodes);
      out << "\n";
    }
    return;
  }

  auto kary = kary_productions(grammar);
  if (kary.empty()) {
    out << "    <empty>\n";
    return;
  }
  for (auto pid : kary) {
    auto const& prod = grammar.productions[pid];
    auto parent_nodes =
        pid < refinement.production_info.size()
            ? refinement.production_info[pid].source_parent_nodes
            : direct_parent_witness_nodes(prod);
    out << "    - source_production: " << pid << "\n";
    out << "      parent_clade: " << prod.parent << "\n";
    out << "      arity: " << prod.children.size() << "\n";
    out << "      witness_multiplicity: " << prod.multiplicity << "\n";
    out << "      source_parent_nodes: ";
    print_size_list(out, parent_nodes);
    out << "\n";
  }
}

static void print_wric_polytomy_audit_fields(
    std::ostream& out, polytomy_refinement_result const& refinement,
    polytomy_mode mode) {
  auto const& audit = refinement.audit;
  print_wric_polytomy_summary_fields(out, refinement, mode);
  print_wric_polytomy_source_kary_details(out, refinement);
  print_wric_exact_expansion_preflight(out, refinement, refinement.options);
  out << "  downstream_binary_charting_allowed: "
      << (audit.binary_chart_compatible && !audit.contains_kary_productions
              ? "true"
              : "false")
      << "\n";
  out << "  kary_grammar_diagnostic_only: "
      << (audit.contains_kary_productions ? "true" : "false") << "\n";
  print_wric_polytomy_event_details(out, refinement);
}

static void print_wric_polytomy_score_fields(
    std::ostream& out, polytomy_refinement_result const& refinement,
    polytomy_mode mode, bool detailed_report) {
  auto const& audit = refinement.audit;
  print_wric_polytomy_summary_fields(out, refinement, mode);
  out << "  exact_for_full_soft_polytomy_space: "
      << (audit.exact_for_soft_polytomies ? "true" : "false") << "\n";
  if (!audit.exact_for_soft_polytomies) {
    out << "  score_scope: BOUNDED_REFINED_GRAMMAR\n";
  } else {
    out << "  score_scope: FULL_SOFT_POLYTOMY_SPACE\n";
  }
  if (detailed_report) print_wric_polytomy_event_details(out, refinement);
}

static char const* exact_or_bounded_score_kind(
    polytomy_refinement_audit const& audit) {
  return audit.exact_for_soft_polytomies ? "EXACT"
                                         : "BOUNDED_REFINED_GRAMMAR";
}

static void print_key_chart_clade_entries(
    std::ostream& out, clade_grammar const& grammar,
    single_site_chart const& chart, std::size_t entry_limit) {
  std::vector<clade_id> clades(grammar.clades.size());
  std::iota(clades.begin(), clades.end(), clade_id{0});
  std::stable_sort(clades.begin(), clades.end(), [&](clade_id lhs,
                                                     clade_id rhs) {
    if (lhs == grammar.root_clade) return true;
    if (rhs == grammar.root_clade) return false;
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize > rsize;
    return lhs < rhs;
  });

  out << "  clade_entries:\n";
  std::size_t printed = 0;
  std::size_t eligible = 0;
  for (auto cid : clades) {
    auto size = grammar.clades[cid].taxa.size();
    if (cid != grammar.root_clade && size == 1) continue;
    ++eligible;
    if (!within_limit(printed, entry_limit)) continue;
    out << "    - clade: " << cid << "\n";
    out << "      size: " << size << "\n";
    out << "      parent_productions: "
        << grammar.productions_by_parent[cid].size() << "\n";
    out << "      inside: ";
    print_chart_row(out, chart.inside[cid]);
    out << "\n";
    ++printed;
  }
  out << "  clade_entries_printed: " << printed << "\n";
  out << "  clade_entries_total_internal_or_root: " << eligible << "\n";
  if (entry_limit != 0 && printed < eligible)
    out << "  clade_entries_truncated: true\n";
}

static void print_limited_per_pattern_roots(
    std::ostream& out, site_pattern_set const& patterns,
    composite_chart_score const& score, std::size_t entry_limit) {
  out << "  per_pattern_root_min:\n";
  std::size_t printed = 0;
  for (std::size_t i = 0; i < patterns.patterns.size(); ++i) {
    if (!within_limit(printed, entry_limit)) continue;
    out << "    - pattern: " << i << "\n";
    out << "      weight: " << patterns.patterns[i].weight << "\n";
    out << "      positions: " << patterns.patterns[i].positions.size()
        << "\n";
    out << "      root_min: ";
    print_chart_cost(out, score.per_pattern_root_min[i]);
    out << "\n";
    ++printed;
  }
  out << "  per_pattern_root_min_printed: " << printed << "\n";
  if (entry_limit != 0 && printed < patterns.patterns.size())
    out << "  per_pattern_root_min_truncated: true\n";
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage() {
  std::cerr <<
      R"(dagutil -- phylogenetic DAG merge / prune / inspect utility

Input (repeatable, at least one required):
  --dag-pb <path>         Input DAG in protobuf DAG format (.pb or .pb.gz)
  --tree-pb <path>        Input tree in parsimony protobuf format (requires --refseq)
  --fasta <path>          Input leaf sequences (FASTA, requires --newick and --refseq)
  --newick <path>         Newick tree file (paired with --fasta)
  --refseq <path>         Reference sequence file

  --vcf <path>            VCF file (required unless --force-no-vcf)
  --force-no-vcf          Skip VCF requirement

Output:
  -o, --output <path>     Output DAG in protobuf DAG format (optional); with
                          chart-B&B annotated trim, writes annotation JSON

Pruning:
  -t, --trim              Trim to best parsimony score
  --rf <path>             Trim to minimize RF distance to this DAG file
  -s, --sample            Sample a single tree from the DAG
  --sample-method <M>     random (default), parsimony, ml/thrifty, edge-weight
  --sample-uniformly      Weight sampling proportional to subtree tree-counts
  --model-dir <path>      Model directory for ml/thrifty sampling, --edge-ml,
                          or --edge-thrifty
  --model-name <name>     Model name for ml/thrifty sampling, --edge-ml,
                          or --edge-thrifty
  --score-ua-edge-ml      ML ignores UA->root by default; opt in to score it
  --ignore-ua-edge-ml     Explicitly keep the default ML UA-edge-ignore policy
  --seed <N>              Random seed for sampling

Notes:
  --sample-method applies only to --sample.  --edge-parsimony/--edge-ml write
  output edge_weight penalties and cannot combine with --trim or --sample.

Analysis:
  --dag-info              Print all DAG statistics (tree count, parsimony, RF)
  --wric-audit            Build collapsed clade grammar and print multiplicity
                          diagnostics (allows polytomies for audit only)
  --wric-benchmark        Run the Phase-9 WRIC grammar/chart/pattern benchmark
                          (polytomy-aware alias of --wric-polytomy-benchmark)
  --wric-polytomy-mode <M>
                          reject (default), audit-kary, expand-exact,
                          or expand-bounded for WRIC chart diagnostics;
                          chart-SPR search also uses this mode and requires a
                          binary chart-compatible grammar (reject fails fast on
                          unresolved high-arity productions)
  --wric-polytomy-max-exact-arity <N>
                          Exact expansion arity cap (default 6)
  --wric-polytomy-max-shapes <N>
                          Bounded seed-shape cap per polytomy (default 16)
  --wric-polytomy-max-productions <N>
                          Per-polytomy production cap for expansion
  --wric-polytomy-max-clades <N>
                          Per-polytomy synthetic-clade cap for expansion
  --wric-polytomy-benchmark
                          Benchmark audit/exact/bounded refinement and chart
                          timings for the loaded DAG(s)
  --wric-polytomy-benchmark-shape-caps <CSV>
                          Candidate bounded seed-shape caps (default 1,4,16;
                          positive integers, each <= 100000)
  --wric-polytomy-benchmark-site <POS>
                          Site used for single-site benchmark (default 1)
  --wric-polytomy-benchmark-bnb
                          Also run exact multi-site B&B frontier benchmark
  --wric-polytomy-benchmark-max-trace-choices <N>
                          Trace-choice cap for benchmark chart-with-trace
                          (default 1000000; 0 means no cap)
  --wric-polytomy-report  Print polytomy refinement status/audit details;
                          chart reports include per-event details
  --chart-site <POS>      Build a one-site chart and print root optimum plus
                          key clade entries (POS is 1-based)
  --chart-pattern-info    Print exact/invariant/normalized site-pattern counts
                          using the collapsed clade grammar taxa
  --chart-trim-site <POS> Print single-site optimal production/clade-state
                          counts (POS is 1-based)
  --chart-composite-score Print summed per-pattern chart score labelled as a
                          LOWER_BOUND diagnostic
  --chart-bnb-trim        Run exact multi-site B&B trimming and print frontier
                          statistics (intended for small/medium DAGs)
  --chart-fluidity-site <POS>
                          Print a single-site chart trace/fluidity report
                          (alias: --plateau-site)
  --chart-score-ua-edge   Include the UA/reference edge in chart diagnostics
  --chart-entry-limit <N> Limit printed chart/pattern entries (default 20;
                          positive integer)
  --chart-bnb-max-frontier <N>
                          Fail if any B&B clade frontier exceeds N entries
                          (omit for no cap; if supplied, must be positive)
  --chart-bnb-no-bound-pruning
                          Disable B&B lower-bound pruning for diagnostics
  --chart-bnb-dominance <M>
                          B&B dominance mode: off (default), score-only,
                          strict-mask-safe, two-pass-exact-mask, or
                          provenance-preserving. score-only requires
                          --chart-bnb-score-only and returns no exact mask;
                          strict-mask-safe is exact for masks/topology
                          witnesses; provenance-preserving is reserved for a
                          future compact provenance mode.
  --chart-bnb-score-only  Report the B&B result as score-only/non-exact-mask
                          instead of requiring an exact keep-production mask
  --chart-bnb-apply-trim  Apply exact B&B trim results and optionally write a DAG
  --chart-bnb-trim-application <M>
                          production-mask (default), annotated-optimal-trim
                          (coupled frontier annotation; no protobuf DAG), or
                          optimal-topology-materialize
  --chart-bnb-max-exact-topologies <N>
                          Cap optimal topology materialization; explicit 0
                          means unlimited (unset uses a conservative cap). For
                          annotated-optimal-trim this caps optional validation
                          enumeration only.
  --chart-bnb-output-dag <path>
                          Output path for --chart-bnb-apply-trim (DAG protobuf
                          for DAG modes; annotation JSON for annotated mode;
                          or use -o)
  --chart-bnb-report-json <path>
                          Write JSON report for B&B trim application; annotated
                          mode embeds the coupled frontier annotation
  --chart-spr-helper-benchmark
                          Benchmark legacy chart-SPR helper costs and counters
                          (diagnostic/oracle path; not production search)
  --chart-spr-helper-benchmark-max-candidates <N>
                          Candidate cap for --chart-spr-helper-benchmark
                          (default 16; 0 means unlimited)
  --chart-spr-candidates  Enumerate grammar-native SPR candidates with the
                          streaming Phase-1 API and print generation counters
  --chart-spr-score-local Score streamed grammar-native candidates with the
                          cached-chart local overlay-delta scorer diagnostic
  --chart-spr-search      Run the grammar-native chart-SPR search loop
                          (conservative accepted-move materialization/rebuild
                          by default). Acceptance objective and candidate-
                          selection exact-verification policy are separate and
                          reported separately.
  --chart-spr-local-accept-updates
                          Use optional Phase-9 accepted-state local cache
                          updates instead of rebuilding sidecar state after
                          each accepted move; accepted overlays are still
                          densely materialized, and final compaction emits one
                          selected tree with a rebuild-equivalence check
  --chart-spr-max-candidates <N>
                          Candidate cap for chart-SPR diagnostics
                          (default 0, unlimited; post-dedup)
  --chart-spr-max-iterations <N>
                          Maximum chart-SPR accept/reject iterations
                          (default 1)
  --chart-spr-top-k-exact <N>
                          Number of lower-bound-ranked candidates exact-verified
                          in lower-bound-top-k mode (default 16; 0 verifies none)
  --chart-spr-acceptance <M>
                          Objective used to accept a move: exact/exact-
                          multisite (default grammar-exact B&B), fixed-
                          topology/fixed-topology-exact (requires/records a
                          complete topology certificate or selector), or lower-
                          bound/lower-bound-heuristic (opt-in composite lower
                          bound, not an exact parsimony proof)
  --chart-spr-candidate-selection <M>
                          Which locally ranked candidates get exact
                          verification: exhaustive-exact, lower-bound-top-k
                          (default), lower-bound-first-improvement, or
                          randomized. Non-exhaustive exact modes can leave
                          unverified improvements in the candidate stream.
  --chart-spr-candidate-source <M>
                          grammar (default), sampled-tree, or hybrid
  --chart-spr-local-score-workers <N>
                          Worker count for parallel local candidate scoring
                          (default 1; 0 chooses hardware concurrency)
  --chart-spr-candidate-batch-size <N>
                          Bounded candidate batch size for local scoring
                          (default 0, choose automatically when needed)
  --chart-spr-memory-budget <BYTES>
                          Advisory resident chart-cache memory budget; if the
                          active cache estimate exceeds it, use pattern batches
  --chart-spr-max-cached-patterns <N>
                          Maximum active patterns resident in the chart cache
                          before switching to pattern-batch scoring
  --chart-spr-pattern-batch-size <N>
                          Explicit active-pattern batch size for local scoring
  --chart-spr-randomize-order
                          Seeded lazy traversal randomization for chart-SPR
                          candidate enumeration
  --chart-spr-reservoir-sample
                          Use max-candidates as a bounded reservoir sample
                          size instead of an early stream cutoff
  --chart-spr-include-immediate-reversals
                          Do not suppress the taxon-key reverse of the
                          immediately previous accepted chart-SPR move
  --chart-spr-sampled-tree-count <N>
                          Number of representative grammar trees to project in
                          sampled-tree/hybrid source modes (default 1)
  --chart-spr-sampled-tree-radius <N>
                          Tree-SPR radius for sampled-tree projected moves
                          (default 0 => tree depth * 2)
  --chart-spr-topology-selector <M>
                          Deterministic selector used by fixed-topology mode
                          when no explicit certificate provider is wired
                          (default first-reachable-overlay-topology)
  --chart-spr-max-upward-path-expansions <N>
                          Hard budget for lazy upward-path expansions
                          (default 0, unlimited)
  --chart-spr-max-path-pairs <N>
                          Hard budget for source-path x target-path pairs
                          (default 0, unlimited)
  --chart-spr-min-moved-clade-size <N>
                          Minimum moved-clade taxon count (default 1)
  --chart-spr-max-moved-clade-size <N>
                          Maximum moved-clade taxon count (default 0,
                          unlimited)
  --chart-spr-min-target-clade-size <N>
                          Minimum target-clade taxon count (default 1)
  --chart-spr-max-target-clade-size <N>
                          Maximum target-clade taxon count (default 0,
                          unlimited)
  --parsimony             Print parsimony score distribution
  --sum-rf-distance       Print sum RF distance distribution
  --edge-parsimony        Compute per-edge parsimony penalties (store in output;
                          cannot combine with --trim/--sample)
  --edge-ml, --edge-thrifty
                          Compute per-edge ML penalties (store in output;
                          cannot combine with --trim/--sample)

Debugging:
  --validate              Validate DAG invariants

Other:
  --version               Print version and exit
  -h, --help              Print this help
)";
}

struct args {
  std::vector<std::string> dag_pbs;
  std::vector<std::string> tree_pbs;
  std::vector<std::string> fastas;
  std::vector<std::string> newicks;
  std::string refseq;
  std::string vcf;
  bool force_no_vcf = false;
  std::string output;
  bool trim = false;
  std::string rf;
  bool sample = false;
  std::string sample_method_text = "random";
  sample_method sampling_method = sample_method::random;
  bool sample_method_explicit = false;
  bool sample_uniformly = false;
  std::string model_dir;
  std::string model_name;
  bool ignore_ua_edge_ml = true;
  std::optional<std::uint32_t> seed;
  bool dag_info = false;
  bool print_parsimony = false;
  bool print_rf_distance = false;
  bool edge_parsimony = false;
  bool edge_ml = false;
  bool validate = false;
  bool wric_audit = false;
  bool wric_benchmark = false;
  polytomy_refinement_options wric_polytomy_opts;
  bool wric_polytomy_mode_explicit = false;
  bool wric_polytomy_report = false;
  bool wric_polytomy_benchmark = false;
  std::vector<std::size_t> wric_polytomy_benchmark_shape_caps{1, 4, 16};
  std::optional<mutation_position> wric_polytomy_benchmark_site;
  bool wric_polytomy_benchmark_bnb = false;
  std::size_t wric_polytomy_benchmark_max_trace_choices = 1000000;
  std::optional<mutation_position> chart_site;
  bool chart_pattern_info = false;
  std::optional<mutation_position> chart_trim_site;
  bool chart_composite_score = false;
  bool chart_bnb_trim = false;
  std::optional<mutation_position> chart_fluidity_site;
  bool chart_score_ua_edge = false;
  std::size_t chart_entry_limit = 20;
  std::size_t chart_bnb_max_frontier = 0;
  bool chart_bnb_no_bound_pruning = false;
  multisite_dominance_mode chart_bnb_dominance =
      multisite_dominance_mode::off;
  bool chart_bnb_score_only = false;
  bool chart_bnb_apply_trim = false;
  chart_bnb_trim_application_mode chart_bnb_application_mode =
      chart_bnb_trim_application_mode::production_mask_superset;
  std::optional<std::size_t> chart_bnb_max_exact_topologies;
  std::string chart_bnb_output_dag;
  std::string chart_bnb_report_json;
  bool chart_spr_helper_benchmark = false;
  std::size_t chart_spr_helper_benchmark_max_candidates = 16;
  bool chart_spr_candidates = false;
  bool chart_spr_score_local = false;
  bool chart_spr_search = false;
  std::size_t chart_spr_max_iterations = 1;
  std::size_t chart_spr_top_k_exact = 16;
  chart_spr_acceptance_mode chart_spr_acceptance =
      chart_spr_acceptance_mode::exact_multisite;
  chart_spr_candidate_selection_mode chart_spr_candidate_selection =
      chart_spr_candidate_selection_mode::lower_bound_top_k;
  std::string chart_spr_topology_selector =
      "first_reachable_overlay_topology";
  grammar_spr_enumeration_options chart_spr_enumeration;
  chart_cache_options chart_spr_cache;
  std::size_t chart_spr_local_score_workers = 1;
  bool chart_spr_local_accept_updates = false;
};

static bool has_any_model_arg(args const& a) {
  return !a.model_dir.empty() || !a.model_name.empty();
}

static bool has_complete_model_args(args const& a) {
  return !a.model_dir.empty() && !a.model_name.empty();
}

static bool ml_model_requested(args const& a) {
  return a.edge_ml ||
         (a.sample && !a.trim && is_ml_sample_method(a.sampling_method));
}

static std::optional<polytomy_mode> parse_wric_polytomy_mode(
    std::string_view text) {
  if (text == "reject") return polytomy_mode::reject;
  if (text == "audit-kary" || text == "audit_kary")
    return polytomy_mode::audit_kary;
  if (text == "expand-exact" || text == "expand_exact" ||
      text == "expand_soft_exact_or_fail")
    return polytomy_mode::expand_soft_exact_or_fail;
  if (text == "expand-bounded" || text == "expand_bounded" ||
      text == "expand_soft_bounded")
    return polytomy_mode::expand_soft_bounded;
  return std::nullopt;
}

static std::optional<chart_spr_acceptance_mode>
parse_chart_spr_acceptance_mode(std::string_view text) {
  if (text == "exact" || text == "exact-multisite" ||
      text == "exact_multisite") {
    return chart_spr_acceptance_mode::exact_multisite;
  }
  if (text == "fixed-topology" || text == "fixed_topology" ||
      text == "fixed-topology-exact" || text == "fixed_topology_exact") {
    return chart_spr_acceptance_mode::fixed_topology_exact;
  }
  if (text == "lower-bound" || text == "lower_bound" ||
      text == "lower-bound-heuristic" ||
      text == "lower_bound_heuristic") {
    return chart_spr_acceptance_mode::lower_bound_heuristic;
  }
  return std::nullopt;
}

static std::optional<chart_spr_candidate_selection_mode>
parse_chart_spr_candidate_selection_mode(std::string_view text) {
  if (text == "exhaustive-exact" || text == "exhaustive_exact" ||
      text == "exhaustive") {
    return chart_spr_candidate_selection_mode::exhaustive_exact;
  }
  if (text == "lower-bound-top-k" || text == "lower_bound_top_k" ||
      text == "top-k" || text == "top_k") {
    return chart_spr_candidate_selection_mode::lower_bound_top_k;
  }
  if (text == "lower-bound-first-improvement" ||
      text == "lower_bound_first_improvement" ||
      text == "first-improvement" || text == "first_improvement") {
    return chart_spr_candidate_selection_mode::lower_bound_first_improvement;
  }
  if (text == "randomized" || text == "sampled" ||
      text == "sampled-or-randomized" || text == "sampled_or_randomized") {
    return chart_spr_candidate_selection_mode::sampled_or_randomized;
  }
  return std::nullopt;
}

static std::optional<multisite_dominance_mode>
parse_multisite_dominance_mode(std::string_view text) {
  if (text == "off") return multisite_dominance_mode::off;
  if (text == "score-only" || text == "score_only") {
    return multisite_dominance_mode::score_only;
  }
  if (text == "strict-mask-safe" || text == "strict_mask_safe") {
    return multisite_dominance_mode::strict_mask_safe;
  }
  if (text == "two-pass-exact-mask" ||
      text == "two_pass_exact_mask") {
    return multisite_dominance_mode::two_pass_exact_mask;
  }
  if (text == "provenance-preserving" ||
      text == "provenance_preserving") {
    return multisite_dominance_mode::provenance_preserving;
  }
  return std::nullopt;
}

static std::optional<chart_bnb_trim_application_mode>
parse_chart_bnb_trim_application_mode(std::string_view text) {
  if (text == "production-mask" || text == "production_mask" ||
      text == "production-mask-superset" ||
      text == "production_mask_superset") {
    return chart_bnb_trim_application_mode::production_mask_superset;
  }
  if (text == "annotated-optimal-trim" ||
      text == "annotated_optimal_trim" ||
      text == "coupled-frontier-exact" ||
      text == "coupled_frontier_exact" ||
      text == "coupled-frontier" || text == "coupled_frontier") {
    return chart_bnb_trim_application_mode::annotated_optimal_trim;
  }
  if (text == "optimal-topology-materialize" ||
      text == "optimal_topology_materialize" ||
      text == "topology-materialize" || text == "topology_materialize") {
    return chart_bnb_trim_application_mode::optimal_topology_materialize;
  }
  return std::nullopt;
}

static std::optional<chart_spr_candidate_source>
parse_chart_spr_candidate_source(std::string_view text) {
  if (text == "grammar") return chart_spr_candidate_source::grammar;
  if (text == "sampled-tree" || text == "sampled_tree" ||
      text == "sampled") {
    return chart_spr_candidate_source::sampled_tree;
  }
  if (text == "hybrid") return chart_spr_candidate_source::hybrid;
  return std::nullopt;
}

static std::size_t parse_size_token_strict(std::string_view token,
                                           std::string_view arg_name) {
  if (token.empty()) {
    throw std::runtime_error(std::string{arg_name} +
                             " contains an empty value");
  }
  for (char c : token) {
    if (c < '0' || c > '9') {
      throw std::runtime_error(std::string{arg_name} + " value '" +
                               std::string{token} +
                               "' must be an unsigned decimal integer");
    }
  }

  std::string owned{token};
  std::size_t pos = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(owned, &pos, 10);
  } catch (std::exception const&) {
    throw std::runtime_error(std::string{arg_name} + " value '" + owned +
                             "' is out of range");
  }
  if (pos != owned.size()) {
    throw std::runtime_error(std::string{arg_name} + " value '" + owned +
                             "' has trailing characters");
  }
  if (parsed > static_cast<unsigned long long>(
                   std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error(std::string{arg_name} + " value '" + owned +
                             "' exceeds size_t range");
  }
  return static_cast<std::size_t>(parsed);
}

static std::size_t parse_positive_size_token_strict(
    std::string_view token, std::string_view arg_name) {
  auto value = parse_size_token_strict(token, arg_name);
  if (value == 0) {
    throw std::runtime_error(std::string{arg_name} +
                             " value must be positive");
  }
  return value;
}

static std::vector<std::size_t> parse_size_csv(std::string_view text,
                                               std::string_view arg_name) {
  std::vector<std::size_t> values;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    auto comma = text.find(',', begin);
    auto end = comma == std::string_view::npos ? text.size() : comma;
    auto token = text.substr(begin, end - begin);
    if (token.empty()) {
      throw std::runtime_error(std::string{arg_name} +
                               " contains an empty CSV entry");
    }
    auto value = parse_size_token_strict(token, arg_name);
    if (value == 0) {
      throw std::runtime_error(std::string{arg_name} +
                               " values must be positive");
    }
    if (value > k_wric_benchmark_shape_cap_limit) {
      throw std::runtime_error(std::string{arg_name} + " value '" +
                               std::string{token} +
                               "' exceeds benchmark safety limit " +
                               std::to_string(
                                   k_wric_benchmark_shape_cap_limit));
    }
    values.push_back(value);
    if (comma == std::string_view::npos) break;
    begin = comma + 1;
  }
  if (values.empty()) {
    throw std::runtime_error(std::string{arg_name} +
                             " must contain at least one value");
  }
  return values;
}

static args parse_args(int argc, char** argv) {
  args a;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto next = [&]() -> std::string_view {
      if (++i >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(1);
      }
      return argv[i];
    };
    if (arg == "--dag-pb")
      a.dag_pbs.emplace_back(next());
    else if (arg == "--tree-pb")
      a.tree_pbs.emplace_back(next());
    else if (arg == "--fasta")
      a.fastas.emplace_back(next());
    else if (arg == "--newick")
      a.newicks.emplace_back(next());
    else if (arg == "--refseq")
      a.refseq = next();
    else if (arg == "--vcf")
      a.vcf = next();
    else if (arg == "--force-no-vcf")
      a.force_no_vcf = true;
    else if (arg == "-o" || arg == "--output")
      a.output = next();
    else if (arg == "-t" || arg == "--trim")
      a.trim = true;
    else if (arg == "--rf")
      a.rf = next();
    else if (arg == "-s" || arg == "--sample")
      a.sample = true;
    else if (arg == "--sample-method") {
      a.sample_method_text = next();
      a.sample_method_explicit = true;
    } else if (arg == "--sample-uniformly")
      a.sample_uniformly = true;
    else if (arg == "--model-dir")
      a.model_dir = next();
    else if (arg == "--model-name")
      a.model_name = next();
    else if (arg == "--ignore-ua-edge-ml")
      a.ignore_ua_edge_ml = true;
    else if (arg == "--score-ua-edge-ml")
      a.ignore_ua_edge_ml = false;
    else if (arg == "--seed")
      a.seed = static_cast<uint32_t>(std::stoull(std::string{next()}));
    else if (arg == "--dag-info") {
      a.dag_info = true;
      a.print_parsimony = true;
      a.print_rf_distance = true;
    } else if (arg == "--parsimony") {
      a.dag_info = true;
      a.print_parsimony = true;
    } else if (arg == "--sum-rf-distance") {
      a.dag_info = true;
      a.print_rf_distance = true;
    } else if (arg == "--wric-audit") {
      a.wric_audit = true;
    } else if (arg == "--wric-benchmark") {
      a.wric_benchmark = true;
    } else if (arg == "--wric-polytomy-mode") {
      auto value = next();
      auto mode = parse_wric_polytomy_mode(value);
      if (!mode) {
        std::cerr << "error: unknown --wric-polytomy-mode '" << value
                  << "'\n";
        std::exit(1);
      }
      a.wric_polytomy_opts.mode = *mode;
      a.wric_polytomy_mode_explicit = true;
    } else if (arg == "--wric-polytomy-max-exact-arity") {
      a.wric_polytomy_opts.max_exact_arity = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
    } else if (arg == "--wric-polytomy-max-shapes") {
      a.wric_polytomy_opts.max_shapes_per_polytomy = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
    } else if (arg == "--wric-polytomy-max-productions") {
      auto value = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
      a.wric_polytomy_opts.max_new_productions_per_polytomy = value;
      a.wric_polytomy_opts.max_bounded_productions_per_polytomy = value;
    } else if (arg == "--wric-polytomy-max-clades") {
      a.wric_polytomy_opts.max_new_clades_per_polytomy =
          static_cast<std::size_t>(std::stoull(std::string{next()}));
    } else if (arg == "--wric-polytomy-benchmark") {
      a.wric_polytomy_benchmark = true;
    } else if (arg == "--wric-polytomy-benchmark-shape-caps") {
      a.wric_polytomy_benchmark_shape_caps =
          parse_size_csv(next(), "--wric-polytomy-benchmark-shape-caps");
    } else if (arg == "--wric-polytomy-benchmark-site") {
      auto pos = static_cast<mutation_position>(parse_size_token_strict(
          next(), "--wric-polytomy-benchmark-site"));
      if (pos == 0) {
        throw std::runtime_error("benchmark site positions are 1-based");
      }
      a.wric_polytomy_benchmark_site = pos;
    } else if (arg == "--wric-polytomy-benchmark-bnb") {
      a.wric_polytomy_benchmark_bnb = true;
    } else if (arg == "--wric-polytomy-benchmark-max-trace-choices") {
      a.wric_polytomy_benchmark_max_trace_choices =
          parse_size_token_strict(
              next(), "--wric-polytomy-benchmark-max-trace-choices");
    } else if (arg == "--wric-polytomy-report") {
      a.wric_polytomy_report = true;
    } else if (arg == "--chart-site") {
      auto pos = static_cast<mutation_position>(
          parse_size_token_strict(next(), "--chart-site"));
      if (pos == 0) {
        throw std::runtime_error("chart site positions are 1-based");
      }
      a.chart_site = pos;
    } else if (arg == "--chart-pattern-info") {
      a.chart_pattern_info = true;
    } else if (arg == "--chart-trim-site") {
      auto pos = static_cast<mutation_position>(
          parse_size_token_strict(next(), "--chart-trim-site"));
      if (pos == 0) {
        throw std::runtime_error("chart trim site positions are 1-based");
      }
      a.chart_trim_site = pos;
    } else if (arg == "--chart-composite-score") {
      a.chart_composite_score = true;
    } else if (arg == "--chart-bnb-trim") {
      a.chart_bnb_trim = true;
    } else if (arg == "--chart-fluidity-site" || arg == "--plateau-site") {
      auto pos = static_cast<mutation_position>(
          parse_size_token_strict(next(), std::string{arg}));
      if (pos == 0) {
        throw std::runtime_error("chart fluidity site positions are 1-based");
      }
      a.chart_fluidity_site = pos;
    } else if (arg == "--chart-score-ua-edge") {
      a.chart_score_ua_edge = true;
    } else if (arg == "--chart-entry-limit") {
      a.chart_entry_limit = parse_positive_size_token_strict(
          next(), "--chart-entry-limit");
    } else if (arg == "--chart-bnb-max-frontier") {
      a.chart_bnb_max_frontier = parse_positive_size_token_strict(
          next(), "--chart-bnb-max-frontier");
    } else if (arg == "--chart-bnb-no-bound-pruning") {
      a.chart_bnb_no_bound_pruning = true;
    } else if (arg == "--chart-bnb-dominance") {
      auto value = next();
      auto mode = parse_multisite_dominance_mode(value);
      if (!mode) {
        std::cerr << "error: unknown --chart-bnb-dominance '" << value
                  << "'\n";
        std::exit(1);
      }
      a.chart_bnb_dominance = *mode;
    } else if (arg == "--chart-bnb-score-only") {
      a.chart_bnb_score_only = true;
    } else if (arg == "--chart-bnb-apply-trim") {
      a.chart_bnb_apply_trim = true;
    } else if (arg == "--chart-bnb-trim-application") {
      auto value = next();
      auto mode = parse_chart_bnb_trim_application_mode(value);
      if (!mode) {
        std::cerr << "error: unknown --chart-bnb-trim-application '" << value
                  << "'\n";
        std::exit(1);
      }
      a.chart_bnb_application_mode = *mode;
    } else if (arg == "--chart-bnb-max-exact-topologies") {
      a.chart_bnb_max_exact_topologies = parse_size_token_strict(
          next(), "--chart-bnb-max-exact-topologies");
    } else if (arg == "--chart-bnb-output-dag") {
      a.chart_bnb_output_dag = next();
    } else if (arg == "--chart-bnb-report-json") {
      a.chart_bnb_report_json = next();
    } else if (arg == "--chart-spr-helper-benchmark") {
      a.chart_spr_helper_benchmark = true;
    } else if (arg == "--chart-spr-helper-benchmark-max-candidates") {
      a.chart_spr_helper_benchmark_max_candidates = parse_size_token_strict(
          next(), "--chart-spr-helper-benchmark-max-candidates");
    } else if (arg == "--chart-spr-candidates") {
      a.chart_spr_candidates = true;
    } else if (arg == "--chart-spr-score-local") {
      a.chart_spr_score_local = true;
    } else if (arg == "--chart-spr-search") {
      a.chart_spr_search = true;
    } else if (arg == "--chart-spr-local-accept-updates" ||
               arg == "--chart-spr-no-rebuild-after-accept") {
      a.chart_spr_local_accept_updates = true;
    } else if (arg == "--chart-spr-max-candidates") {
      a.chart_spr_enumeration.max_candidates = parse_size_token_strict(
          next(), "--chart-spr-max-candidates");
    } else if (arg == "--chart-spr-max-iterations") {
      a.chart_spr_max_iterations = parse_positive_size_token_strict(
          next(), "--chart-spr-max-iterations");
    } else if (arg == "--chart-spr-top-k-exact") {
      a.chart_spr_top_k_exact = parse_size_token_strict(
          next(), "--chart-spr-top-k-exact");
    } else if (arg == "--chart-spr-acceptance") {
      auto value = next();
      auto mode = parse_chart_spr_acceptance_mode(value);
      if (!mode) {
        std::cerr << "error: unknown --chart-spr-acceptance '" << value
                  << "'\n";
        std::exit(1);
      }
      a.chart_spr_acceptance = *mode;
    } else if (arg == "--chart-spr-candidate-selection") {
      auto value = next();
      auto mode = parse_chart_spr_candidate_selection_mode(value);
      if (!mode) {
        std::cerr << "error: unknown --chart-spr-candidate-selection '"
                  << value << "'\n";
        std::exit(1);
      }
      a.chart_spr_candidate_selection = *mode;
    } else if (arg == "--chart-spr-candidate-source") {
      auto value = next();
      auto source = parse_chart_spr_candidate_source(value);
      if (!source) {
        std::cerr << "error: unknown --chart-spr-candidate-source '"
                  << value << "'\n";
        std::exit(1);
      }
      a.chart_spr_enumeration.source = *source;
    } else if (arg == "--chart-spr-local-score-workers") {
      a.chart_spr_local_score_workers = parse_size_token_strict(
          next(), "--chart-spr-local-score-workers");
    } else if (arg == "--chart-spr-candidate-batch-size") {
      a.chart_spr_cache.candidate_batch_size = parse_size_token_strict(
          next(), "--chart-spr-candidate-batch-size");
    } else if (arg == "--chart-spr-memory-budget") {
      a.chart_spr_cache.memory_budget_bytes = parse_size_token_strict(
          next(), "--chart-spr-memory-budget");
    } else if (arg == "--chart-spr-max-cached-patterns") {
      a.chart_spr_cache.max_cached_patterns = parse_size_token_strict(
          next(), "--chart-spr-max-cached-patterns");
    } else if (arg == "--chart-spr-pattern-batch-size") {
      a.chart_spr_cache.pattern_batch_size = parse_size_token_strict(
          next(), "--chart-spr-pattern-batch-size");
    } else if (arg == "--chart-spr-randomize-order") {
      a.chart_spr_enumeration.randomize_order = true;
    } else if (arg == "--chart-spr-reservoir-sample") {
      a.chart_spr_enumeration.reservoir_sample = true;
    } else if (arg == "--chart-spr-include-immediate-reversals") {
      a.chart_spr_enumeration.include_immediate_reversal_candidates = true;
    } else if (arg == "--chart-spr-sampled-tree-count") {
      a.chart_spr_enumeration.sampled_tree_count =
          parse_size_token_strict(next(), "--chart-spr-sampled-tree-count");
    } else if (arg == "--chart-spr-sampled-tree-radius") {
      a.chart_spr_enumeration.sampled_tree_spr_radius =
          parse_size_token_strict(next(), "--chart-spr-sampled-tree-radius");
    } else if (arg == "--chart-spr-topology-selector") {
      a.chart_spr_topology_selector = std::string{next()};
      if (!chart_spr_builtin_fixed_topology_selector_name(
              a.chart_spr_topology_selector)) {
        std::cerr << "error: unknown --chart-spr-topology-selector '"
                  << a.chart_spr_topology_selector << "'\n";
        std::exit(1);
      }
    } else if (arg == "--chart-spr-max-upward-path-expansions") {
      a.chart_spr_enumeration.max_upward_path_expansions =
          parse_size_token_strict(next(),
                                  "--chart-spr-max-upward-path-expansions");
    } else if (arg == "--chart-spr-max-path-pairs") {
      a.chart_spr_enumeration.max_path_pairs_considered =
          parse_size_token_strict(next(), "--chart-spr-max-path-pairs");
    } else if (arg == "--chart-spr-min-moved-clade-size") {
      a.chart_spr_enumeration.min_moved_clade_size =
          parse_positive_size_token_strict(
              next(), "--chart-spr-min-moved-clade-size");
    } else if (arg == "--chart-spr-max-moved-clade-size") {
      a.chart_spr_enumeration.max_moved_clade_size = parse_size_token_strict(
          next(), "--chart-spr-max-moved-clade-size");
    } else if (arg == "--chart-spr-min-target-clade-size") {
      a.chart_spr_enumeration.min_target_clade_size =
          parse_positive_size_token_strict(
              next(), "--chart-spr-min-target-clade-size");
    } else if (arg == "--chart-spr-max-target-clade-size") {
      a.chart_spr_enumeration.max_target_clade_size = parse_size_token_strict(
          next(), "--chart-spr-max-target-clade-size");
    } else if (arg == "--edge-parsimony")
      a.edge_parsimony = true;
    else if (arg == "--edge-ml" || arg == "--edge-thrifty")
      a.edge_ml = true;
    else if (arg == "--validate")
      a.validate = true;
    else if (arg == "--version") {
      std::cerr << "dagutil " << larch::version << " (" << larch::git_commit
                << ")\n";
      std::exit(0);
    } else if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      usage();
      std::exit(1);
    }
  }

  auto total_inputs = a.dag_pbs.size() + a.tree_pbs.size() + a.fastas.size();
  if (total_inputs == 0) {
    std::cerr << "error: at least one input required (--dag-pb, --tree-pb, or "
                 "--fasta)\n";
    usage();
    std::exit(1);
  }
  if (!a.tree_pbs.empty() && a.refseq.empty()) {
    std::cerr << "error: --refseq is required with --tree-pb\n";
    std::exit(1);
  }
  if (!a.fastas.empty()) {
    if (a.fastas.size() != a.newicks.size()) {
      std::cerr << "error: each --fasta must be paired with a --newick\n";
      std::exit(1);
    }
    if (a.refseq.empty()) {
      std::cerr << "error: --refseq is required with --fasta\n";
      std::exit(1);
    }
  }
  if (a.vcf.empty() && !a.force_no_vcf) {
    std::cerr << "error: --vcf is required (use --force-no-vcf to skip)\n";
    std::exit(1);
  }
  auto parsed_method = parse_sample_method(a.sample_method_text);
  if (!parsed_method) {
    std::cerr << "error: unknown sample method '" << a.sample_method_text
              << "'\n";
    std::exit(1);
  }
  a.sampling_method = *parsed_method;

  if (has_any_model_arg(a) && !has_complete_model_args(a)) {
    std::cerr << "error: --model-dir and --model-name must be provided "
                 "together\n";
    std::exit(1);
  }
  if (a.sample_method_explicit && !a.sample) {
    std::cerr << "error: --sample-method requires --sample\n";
    std::exit(1);
  }
  if (a.sample_method_explicit && a.sample && a.output.empty()) {
    std::cerr << "error: --sample-method requires -o/--output because "
                 "sampling writes an output tree\n";
    std::exit(1);
  }
  if (a.trim && a.sample && a.sample_method_explicit) {
    if (!a.rf.empty()) {
      std::cerr << "error: --sample-method is not used with --trim --rf "
                   "--sample; omit --sample-method because --rf selects the "
                   "criterion\n";
      std::exit(1);
    }
    if (a.sampling_method != sample_method::parsimony) {
      std::cerr << "error: --sample-method applies to --sample without "
                   "--trim; omit --trim for "
                << a.sample_method_text << " sampling\n";
      std::exit(1);
    }
  }
  if (a.sample && !a.trim && is_ml_sample_method(a.sampling_method) &&
      !has_complete_model_args(a)) {
    std::cerr << "error: --model-dir and --model-name required with "
                 "--sample-method "
              << a.sample_method_text << "\n";
    std::exit(1);
  }
  if (a.edge_parsimony && a.edge_ml) {
    std::cerr << "error: choose only one of --edge-parsimony or --edge-ml\n";
    std::exit(1);
  }
  if ((a.edge_parsimony || a.edge_ml) && (a.trim || a.sample)) {
    std::cerr << "error: --edge-parsimony/--edge-ml cannot be combined with "
                 "--trim or --sample; write edge penalties to an output DAG "
                 "first, then run sampling/trimming in a second command\n";
    std::exit(1);
  }
  if (a.edge_ml && !has_complete_model_args(a)) {
    std::cerr << "error: --model-dir and --model-name required with "
                 "--edge-ml/--edge-thrifty\n";
    std::exit(1);
  }
  if (a.chart_spr_enumeration.max_moved_clade_size != 0 &&
      a.chart_spr_enumeration.min_moved_clade_size >
          a.chart_spr_enumeration.max_moved_clade_size) {
    std::cerr << "error: --chart-spr-min-moved-clade-size exceeds "
                 "--chart-spr-max-moved-clade-size\n";
    std::exit(1);
  }
  if (a.chart_spr_enumeration.max_target_clade_size != 0 &&
      a.chart_spr_enumeration.min_target_clade_size >
          a.chart_spr_enumeration.max_target_clade_size) {
    std::cerr << "error: --chart-spr-min-target-clade-size exceeds "
                 "--chart-spr-max-target-clade-size\n";
    std::exit(1);
  }
  if (a.chart_bnb_apply_trim) {
    a.chart_bnb_trim = true;
    if (a.trim || a.sample || a.edge_parsimony || a.edge_ml) {
      std::cerr << "error: --chart-bnb-apply-trim cannot be combined with "
                   "--trim, --sample, --edge-parsimony, or --edge-ml\n";
      std::exit(1);
    }
    if (a.chart_bnb_score_only) {
      std::cerr << "error: --chart-bnb-apply-trim requires an exact keep "
                   "mask; remove --chart-bnb-score-only\n";
      std::exit(1);
    }
    if (!a.chart_bnb_output_dag.empty() && !a.output.empty() &&
        a.chart_bnb_output_dag != a.output) {
      std::cerr << "error: use only one B&B output path (-o/--output or "
                   "--chart-bnb-output-dag)\n";
      std::exit(1);
    }
  }
  if (!a.chart_bnb_output_dag.empty() && !a.chart_bnb_apply_trim) {
    std::cerr << "error: --chart-bnb-output-dag requires "
                 "--chart-bnb-apply-trim\n";
    std::exit(1);
  }
  if (!a.chart_bnb_report_json.empty() && !a.chart_bnb_apply_trim) {
    std::cerr << "error: --chart-bnb-report-json requires "
                 "--chart-bnb-apply-trim\n";
    std::exit(1);
  }
  if (a.seed) a.chart_spr_enumeration.seed = *a.seed;

  return a;
}

static multisite_trim_options make_chart_bnb_trim_options(args const& a) {
  multisite_trim_options trim_opts;
  trim_opts.use_bound_pruning = !a.chart_bnb_no_bound_pruning;
  trim_opts.dominance_mode = a.chart_bnb_dominance;
  trim_opts.require_exact_keep_mask = !a.chart_bnb_score_only;
  trim_opts.max_frontier_entries_per_clade = a.chart_bnb_max_frontier;
  return trim_opts;
}

static chart_bnb_trim_apply_options make_chart_bnb_trim_apply_options(
    args const& a) {
  chart_bnb_trim_apply_options opts;
  opts.mode = a.chart_bnb_application_mode;
  opts.max_exact_topologies_to_materialize =
      a.chart_bnb_max_exact_topologies;
  return opts;
}

static void print_chart_bnb_apply_result(
    std::ostream& out, chart_bnb_trim_apply_result const& apply,
    std::string const& indent) {
  out << indent << "trim_application_mode: "
      << chart_bnb_trim_application_mode_name(apply.mode) << "\n";
  out << indent << "output_artifact_kind: " << apply.output_artifact_kind
      << "\n";
  out << indent << "output_dag_available: "
      << (apply.output_dag_available ? "true" : "false") << "\n";
  out << indent << "topology_exact: "
      << (apply.topology_exact ? "true" : "false") << "\n";
  out << indent << "grammar_topology_exact: "
      << (apply.grammar_topology_exact ? "true" : "false") << "\n";
  out << indent << "source_history_topology_exact: "
      << (apply.source_history_topology_exact ? "true" : "false") << "\n";
  out << indent << "coupled_frontier_exact: "
      << (apply.coupled_frontier_exact ? "true" : "false") << "\n";
  out << indent << "annotated_optimal_trim: "
      << (apply.annotated_optimal_trim ? "true" : "false") << "\n";
  out << indent << "identity_preserving_tree_set: "
      << (apply.identity_preserving_tree_set ? "true" : "false") << "\n";
  out << indent << "production_mask_superset: "
      << (apply.production_mask_superset ? "true" : "false") << "\n";
  out << indent << "coupled_frontier_entries: "
      << apply.coupled_frontier_entries << "\n";
  out << indent << "coupled_provenance_choices: "
      << apply.coupled_provenance_choices << "\n";
  out << indent << "coupled_root_frontier_entries: "
      << apply.coupled_root_frontier_entries << "\n";
  out << indent << "refinement_exactness: " << apply.refinement_exactness
      << "\n";
  out << indent << "bnb_optimum: " << apply.bnb_optimum << "\n";
  out << indent << "validated_output_parsimony_min: "
      << apply.validated_output_parsimony_min << "\n";
  out << indent << "validated_output_parsimony_min_exact: "
      << (apply.validated_output_parsimony_min_exact ? "true" : "false")
      << "\n";
  out << indent << "validation_oracle: " << apply.validation_oracle << "\n";
  out << indent << "validation_strength: " << apply.validation_strength
      << "\n";
  out << indent << "output_contains_only_optimal_topologies: "
      << apply.output_contains_only_optimal_topologies << "\n";
  out << indent << "source_edges_removed: " << apply.source_edges_removed
      << "\n";
  out << indent << "source_nodes_removed: " << apply.source_nodes_removed
      << "\n";
  out << indent << "materialized_topologies: "
      << apply.materialized_topologies << "\n";
  out << indent << "topology_cap_truncated: "
      << (apply.topology_cap_truncated ? "true" : "false") << "\n";
  out << indent << "validation_topology_cap_truncated: "
      << (apply.validation_topology_cap_truncated ? "true" : "false")
      << "\n";
  out << indent << "kept_productions_requested: "
      << apply.kept_productions_requested << "\n";
  out << indent << "kept_productions_rebuilt: "
      << apply.kept_productions_rebuilt << "\n";
  out << indent << "masked_productions_reappeared: "
      << apply.masked_productions_reappeared << "\n";
  out << indent << "validation_status: " << apply.validation_status << "\n";
}

static std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

static void write_json_bool_vector(std::ostream& out,
                                   std::vector<bool> const& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << (values[i] ? "true" : "false");
  }
  out << "]";
}

static void write_json_size_vector(std::ostream& out,
                                   std::vector<std::size_t> const& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  out << "]";
}

static void write_json_chart_cost_vector(
    std::ostream& out, std::vector<chart_cost> const& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  out << "]";
}

static void write_coupled_frontier_annotation_json(
    std::ostream& out,
    multisite_coupled_frontier_trim_result const& annotation,
    std::string const& indent) {
  auto child = indent + "  ";
  auto grandchild = child + "  ";
  auto great_grandchild = grandchild + "  ";
  auto entry_indent = great_grandchild + "  ";
  auto entry_field = entry_indent + "  ";
  auto choice_indent = entry_field + "  ";

  out << "{\n";
  out << child << "\"schema\": "
      << "\"larch.multisite_coupled_frontier_trim_result\",\n";
  out << child << "\"schema_version\": 1,\n";
  out << child << "\"cost_vector_layout\": "
      << "\"cost[active_pattern * 4 + state]; state_order=A,C,G,T\",\n";
  out << child << "\"clade_count\": "
      << annotation.entries_by_clade.size() << ",\n";
  out << child << "\"production_count\": "
      << annotation.keep_production.size() << ",\n";
  out << child << "\"optimum\": " << annotation.optimum << ",\n";
  out << child << "\"composite_lower_bound\": "
      << annotation.composite_lower_bound << ",\n";
  out << child << "\"initial_upper_bound\": "
      << annotation.initial_upper_bound << ",\n";
  out << child << "\"dominance_mode\": \""
      << multisite_dominance_mode_name(annotation.dominance_mode) << "\",\n";
  out << child << "\"equality_deduplicated\": "
      << annotation.equality_deduplicated << ",\n";
  out << child << "\"dominance_candidates_considered\": "
      << annotation.dominance_candidates_considered << ",\n";
  out << child << "\"dominance_pruned\": "
      << annotation.dominance_pruned << ",\n";
  out << child << "\"bound_pruned\": " << annotation.bound_pruned
      << ",\n";
  out << child << "\"optimal_root_frontier_entry_count\": "
      << annotation.optimal_root_frontier_entry_count << ",\n";
  out << child << "\"coupled_frontier_entry_count\": "
      << annotation.coupled_frontier_entry_count << ",\n";
  out << child << "\"coupled_provenance_choice_count\": "
      << annotation.coupled_provenance_choice_count << ",\n";
  out << child << "\"coupled_frontier_exact\": "
      << (annotation.coupled_frontier_exact ? "true" : "false") << ",\n";
  out << child << "\"annotated_optimal_trim\": "
      << (annotation.annotated_optimal_trim ? "true" : "false") << ",\n";
  out << child << "\"active_pattern_count\": "
      << annotation.active_pattern_count << ",\n";
  out << child << "\"invariant_constant_offset\": "
      << annotation.invariant_constant_offset << ",\n";
  out << child << "\"keep_production\": ";
  write_json_bool_vector(out, annotation.keep_production);
  out << ",\n";
  out << child << "\"frontier_sizes_by_clade\": ";
  write_json_size_vector(out, annotation.frontier_sizes_by_clade);
  out << ",\n";
  out << child << "\"coupled_frontier_entries_by_clade\": ";
  write_json_size_vector(out, annotation.coupled_frontier_entries_by_clade);
  out << ",\n";
  out << child << "\"entries_by_clade\": [\n";

  for (std::size_t clade = 0; clade < annotation.entries_by_clade.size();
       ++clade) {
    out << grandchild << "{\n";
    out << great_grandchild << "\"clade\": " << clade << ",\n";
    out << great_grandchild << "\"entries\": [\n";
    auto const& entries = annotation.entries_by_clade[clade];
    for (std::size_t entry_index = 0; entry_index < entries.size();
         ++entry_index) {
      auto const& entry = entries[entry_index];
      out << entry_indent << "{\n";
      out << entry_field << "\"entry\": " << entry_index << ",\n";
      out << entry_field << "\"cost\": ";
      write_json_chart_cost_vector(out, entry.f.cost);
      out << ",\n";
      out << entry_field << "\"topology_hash\": "
          << entry.f.topology_hash << ",\n";
      out << entry_field << "\"choices\": [\n";
      for (std::size_t choice_index = 0;
           choice_index < entry.choices.size(); ++choice_index) {
        auto const& choice = entry.choices[choice_index];
        out << choice_indent << "{\"production\": " << choice.production
            << ", \"left_child\": " << choice.left_child
            << ", \"right_child\": " << choice.right_child
            << ", \"left_entry\": " << choice.left_entry
            << ", \"right_entry\": " << choice.right_entry << "}";
        out << (choice_index + 1 == entry.choices.size() ? "\n" : ",\n");
      }
      out << entry_field << "]\n";
      out << entry_indent << "}";
      out << (entry_index + 1 == entries.size() ? "\n" : ",\n");
    }
    out << great_grandchild << "]\n";
    out << grandchild << "}";
    out << (clade + 1 == annotation.entries_by_clade.size() ? "\n"
                                                              : ",\n");
  }

  out << child << "]\n";
  out << indent << "}";
}

static void write_chart_bnb_apply_json(
    std::string const& path, multisite_trim_result const& trim,
    chart_bnb_trim_apply_result const& apply) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("failed to open JSON report: " + path);
  out << "{\n";
  out << "  \"trim_application_mode\": \""
      << chart_bnb_trim_application_mode_name(apply.mode) << "\",\n";
  out << "  \"output_artifact_kind\": \""
      << json_escape(apply.output_artifact_kind) << "\",\n";
  out << "  \"output_dag_available\": "
      << (apply.output_dag_available ? "true" : "false") << ",\n";
  out << "  \"dominance_mode\": \""
      << multisite_dominance_mode_name(trim.dominance_mode) << "\",\n";
  out << "  \"keep_mask_kind\": \""
      << multisite_keep_mask_kind_name(trim.keep_mask_kind) << "\",\n";
  out << "  \"keep_production_exact\": "
      << (trim.keep_production_exact ? "true" : "false") << ",\n";
  out << "  \"topology_exact\": "
      << (apply.topology_exact ? "true" : "false") << ",\n";
  out << "  \"grammar_topology_exact\": "
      << (apply.grammar_topology_exact ? "true" : "false") << ",\n";
  out << "  \"source_history_topology_exact\": "
      << (apply.source_history_topology_exact ? "true" : "false")
      << ",\n";
  out << "  \"coupled_frontier_exact\": "
      << (apply.coupled_frontier_exact ? "true" : "false") << ",\n";
  out << "  \"annotated_optimal_trim\": "
      << (apply.annotated_optimal_trim ? "true" : "false") << ",\n";
  out << "  \"identity_preserving_tree_set\": "
      << (apply.identity_preserving_tree_set ? "true" : "false")
      << ",\n";
  out << "  \"production_mask_superset\": "
      << (apply.production_mask_superset ? "true" : "false") << ",\n";
  out << "  \"coupled_frontier_entries\": "
      << apply.coupled_frontier_entries << ",\n";
  out << "  \"coupled_provenance_choices\": "
      << apply.coupled_provenance_choices << ",\n";
  out << "  \"coupled_root_frontier_entries\": "
      << apply.coupled_root_frontier_entries << ",\n";
  out << "  \"refinement_exactness\": \""
      << json_escape(apply.refinement_exactness) << "\",\n";
  out << "  \"bnb_optimum\": " << apply.bnb_optimum << ",\n";
  out << "  \"validated_output_parsimony_min\": "
      << apply.validated_output_parsimony_min << ",\n";
  out << "  \"validated_output_parsimony_min_exact\": "
      << (apply.validated_output_parsimony_min_exact ? "true" : "false")
      << ",\n";
  out << "  \"validation_oracle\": \""
      << json_escape(apply.validation_oracle) << "\",\n";
  out << "  \"validation_strength\": \""
      << json_escape(apply.validation_strength) << "\",\n";
  out << "  \"output_contains_only_optimal_topologies\": \""
      << json_escape(apply.output_contains_only_optimal_topologies)
      << "\",\n";
  out << "  \"source_edges_removed\": " << apply.source_edges_removed
      << ",\n";
  out << "  \"source_nodes_removed\": " << apply.source_nodes_removed
      << ",\n";
  out << "  \"materialized_topologies\": "
      << apply.materialized_topologies << ",\n";
  out << "  \"topology_cap_truncated\": "
      << (apply.topology_cap_truncated ? "true" : "false") << ",\n";
  out << "  \"validation_topology_cap_truncated\": "
      << (apply.validation_topology_cap_truncated ? "true" : "false")
      << ",\n";
  out << "  \"kept_productions_requested\": "
      << apply.kept_productions_requested << ",\n";
  out << "  \"kept_productions_rebuilt\": "
      << apply.kept_productions_rebuilt << ",\n";
  out << "  \"masked_productions_reappeared\": "
      << apply.masked_productions_reappeared << ",\n";
  out << "  \"validation_status\": \""
      << json_escape(apply.validation_status) << "\",\n";
  out << "  \"coupled_frontier_annotation\": ";
  if (apply.annotated_optimal_trim || apply.coupled_frontier_exact) {
    write_coupled_frontier_annotation_json(
        out, apply.coupled_frontier_annotation, "  ");
    out << "\n";
  } else {
    out << "null\n";
  }
  out << "}\n";
}

static std::size_t saturating_size_add(std::size_t lhs, std::size_t rhs) {
  auto max = std::numeric_limits<std::size_t>::max();
  if (max - lhs < rhs) return max;
  return lhs + rhs;
}

static std::size_t saturating_size_mul(std::size_t lhs, std::size_t rhs) {
  auto max = std::numeric_limits<std::size_t>::max();
  if (lhs != 0 && rhs > max / lhs) return max;
  return lhs * rhs;
}

static std::size_t estimate_chart_memory_without_trace_bytes(
    clade_grammar const& grammar) {
  return saturating_size_mul(grammar.clades.size(),
                             sizeof(std::array<chart_cost, nuc_state_count>));
}

static std::size_t estimate_chart_memory_with_trace_bytes(
    clade_grammar const& grammar, single_site_chart const& chart) {
  auto total = estimate_chart_memory_without_trace_bytes(grammar);
  auto vector_count = saturating_size_mul(grammar.clades.size(),
                                          nuc_state_count);
  total = saturating_size_add(
      total,
      saturating_size_mul(vector_count, sizeof(std::vector<chart_choice>)));
  total = saturating_size_add(
      total, saturating_size_mul(chart.trace_choice_count,
                                 sizeof(chart_choice)));
  return total;
}

static bool clade_info_has_synthetic_polytomy_origin(
    refined_clade_info const& info) {
  return info.origin == refined_clade_origin::synthetic_polytomy_intermediate ||
         info.origin == refined_clade_origin::observed_and_synthetic;
}

struct frontier_size_summary {
  std::size_t clades = 0;
  std::size_t sum = 0;
  std::size_t max = 0;
  std::map<std::size_t, std::size_t> histogram;
};

static frontier_size_summary summarize_frontier_sizes(
    std::vector<std::size_t> const& frontier_sizes,
    std::vector<bool> const* include_clade = nullptr) {
  frontier_size_summary summary;
  for (std::size_t cid = 0; cid < frontier_sizes.size(); ++cid) {
    if (include_clade != nullptr &&
        (cid >= include_clade->size() || !(*include_clade)[cid])) {
      continue;
    }
    auto size = frontier_sizes[cid];
    ++summary.clades;
    summary.sum = saturating_size_add(summary.sum, size);
    summary.max = std::max(summary.max, size);
    ++summary.histogram[size];
  }
  return summary;
}

static void print_named_frontier_summary(std::ostream& out,
                                         std::string_view name,
                                         frontier_size_summary const& summary,
                                         std::string const& indent) {
  out << indent << name << ":\n";
  auto child = indent + "  ";
  out << child << "clades: " << summary.clades << "\n";
  out << child << "frontier_size_sum: " << summary.sum << "\n";
  out << child << "frontier_size_max: " << summary.max << "\n";
  out << child << "frontier_size_histogram:\n";
  if (summary.histogram.empty()) {
    out << child << "  <empty>\n";
  } else {
    for (auto const& [size, count] : summary.histogram)
      out << child << "  " << size << ": " << count << "\n";
  }
}

static void print_refinement_benchmark_summary(
    std::ostream& out, polytomy_refinement_result const& refinement,
    std::string const& indent) {
  auto const& audit = refinement.audit;
  auto totals = summarize_wric_polytomy_report(refinement);
  out << indent << "status: " << polytomy_refinement_status_label(audit)
      << "\n";
  out << indent << "source_kary_productions: "
      << audit.source_kary_production_count << "\n";
  out << indent << "refined_clades: " << audit.refined_clade_count << "\n";
  out << indent << "refined_productions: "
      << audit.refined_production_count << "\n";
  out << indent << "synthetic_clades: " << audit.synthetic_clade_count
      << "\n";
  out << indent << "synthetic_productions: "
      << audit.synthetic_production_count << "\n";
  out << indent << "selected_seed_shapes: " << totals.selected_seed_shapes
      << "\n";
  out << indent << "represented_refinement_count: ";
  print_saturated_u64(out, totals.refined_grammar_tree_count,
                      totals.refined_grammar_tree_count_saturated);
  out << "\n";
  out << indent << "exact_for_soft_polytomies: "
      << (audit.exact_for_soft_polytomies ? "true" : "false") << "\n";
  out << indent << "truncated_events: " << totals.truncated_events << "\n";
  auto event_count = audit.events.size();
  double fraction = event_count == 0
                        ? 0.0
                        : static_cast<double>(totals.truncated_events) /
                              static_cast<double>(event_count);
  out << indent << "bounded_truncation_frequency: " << std::fixed
      << std::setprecision(3) << fraction << "\n";
}

static void print_binary_refinement_performance_benchmark(
    std::ostream& out, phylo_dag& dag,
    polytomy_refinement_result const& refinement, mutation_position site,
    args const& a, std::string const& indent) {
  out << indent << "binary_chart_benchmark:\n";
  auto child = indent + "  ";
  if (!polytomy_refinement_allows_binary_charting(refinement.audit)) {
    out << child << "skipped: true\n";
    out << child
        << "skipped_reason: grammar is not binary-chart-compatible\n";
    return;
  }

  auto const& grammar = refinement.grammar;
  chart_options chart_opts;
  chart_opts.score_ua_edge = a.chart_score_ua_edge;

  auto state_start = std::chrono::steady_clock::now();
  auto states = extract_leaf_site_states(dag, grammar, site);
  auto state_ms = elapsed_ms(state_start, std::chrono::steady_clock::now());

  auto chart_start = std::chrono::steady_clock::now();
  auto chart = build_single_site_chart(grammar, states, chart_opts);
  auto chart_ms = elapsed_ms(chart_start, std::chrono::steady_clock::now());

  auto reference_state = extract_reference_site_state(dag, site);
  auto optimum = root_min(chart, grammar.root_clade, chart_opts,
                          reference_state);

  bool trace_success = true;
  std::string trace_error;
  single_site_chart trace_chart;
  double trace_ms = 0.0;
  try {
    auto trace_opts = chart_opts;
    trace_opts.keep_trace = true;
    trace_opts.max_trace_choices =
        a.wric_polytomy_benchmark_max_trace_choices;
    auto trace_start = std::chrono::steady_clock::now();
    trace_chart = build_single_site_chart(grammar, states, trace_opts);
    trace_ms = elapsed_ms(trace_start, std::chrono::steady_clock::now());
  } catch (std::exception const& e) {
    trace_success = false;
    trace_error = e.what();
  }

  site_pattern_options pattern_opts;
  pattern_opts.build_normalized_binary_patterns = true;
  auto pattern_start = std::chrono::steady_clock::now();
  auto patterns = build_site_patterns(dag, grammar, pattern_opts);
  auto pattern_ms = elapsed_ms(pattern_start, std::chrono::steady_clock::now());

  auto composite_start = std::chrono::steady_clock::now();
  auto composite = build_composite_chart_score(grammar, patterns, chart_opts);
  auto composite_ms = elapsed_ms(composite_start,
                                 std::chrono::steady_clock::now());

  out << child << "site: " << site << "\n";
  out << child << "score_ua_edge: "
      << (a.chart_score_ua_edge ? "true" : "false") << "\n";
  out << child << "reference_state: " << chart_state_label(reference_state)
      << "\n";
  out << child << "root_min: ";
  print_chart_cost(out, optimum);
  out << "\n";
  out << child << "leaf_state_extract_ms: " << std::fixed
      << std::setprecision(3) << state_ms << "\n";
  out << child << "chart_without_trace_ms: " << std::fixed
      << std::setprecision(3) << chart_ms << "\n";
  out << child << "chart_estimated_memory_without_trace_bytes: "
      << estimate_chart_memory_without_trace_bytes(grammar) << "\n";
  out << child << "chart_with_trace_success: "
      << (trace_success ? "true" : "false") << "\n";
  out << child << "chart_trace_choice_cap: "
      << a.wric_polytomy_benchmark_max_trace_choices << "\n";
  if (trace_success) {
    out << child << "chart_with_trace_ms: " << std::fixed
        << std::setprecision(3) << trace_ms << "\n";
    out << child << "chart_trace_choice_count: "
        << trace_chart.trace_choice_count << "\n";
    out << child << "chart_estimated_memory_with_trace_bytes: "
        << estimate_chart_memory_with_trace_bytes(grammar, trace_chart)
        << "\n";
  } else {
    out << child << "chart_with_trace_error: " << trace_error << "\n";
  }
  auto pattern_build_ms_per_site =
      patterns.total_site_count == 0
          ? 0.0
          : pattern_ms / static_cast<double>(patterns.total_site_count);
  auto composite_ms_per_pattern =
      patterns.patterns.empty()
          ? 0.0
          : composite_ms / static_cast<double>(patterns.patterns.size());
  out << child << "pattern_build_ms: " << std::fixed
      << std::setprecision(3) << pattern_ms << "\n";
  out << child << "pattern_build_ms_per_site: " << std::fixed
      << std::setprecision(6) << pattern_build_ms_per_site << "\n";
  out << child << "exact_patterns: " << patterns.patterns.size() << "\n";
  out << child << "total_sites: " << patterns.total_site_count << "\n";
  out << child << "taxa: " << patterns.taxon_count << "\n";
  out << child << "invariant_sites: " << patterns.invariant_site_count
      << "\n";
  out << child << "variable_sites: " << patterns.variable_site_count
      << "\n";
  out << child << "binary_variable_sites: "
      << patterns.binary_variable_site_count << "\n";
  out << child << "nonbinary_variable_sites: "
      << patterns.nonbinary_variable_site_count << "\n";
  out << child << "skipped_invariant_sites: "
      << patterns.skipped_invariant_site_count << "\n";
  out << child << "normalized_binary_patterns: "
      << patterns.normalized_binary_patterns.size() << "\n";
  out << child << "invariant_constant_score_excluding_ua: "
      << patterns.invariant_constant_score_excluding_ua << "\n";
  out << child << "invariant_constant_score_with_reference_edge: "
      << patterns.invariant_constant_score_with_reference_edge << "\n";
  out << child << "skipped_invariant_constant_score_with_reference_edge: "
      << patterns.skipped_invariant_constant_score_with_reference_edge << "\n";
  out << child << "composite_chart_ms: " << std::fixed
      << std::setprecision(3) << composite_ms << "\n";
  out << child << "composite_chart_ms_per_pattern: " << std::fixed
      << std::setprecision(6) << composite_ms_per_pattern << "\n";
  out << child << "composite_lower_bound: "
      << composite.weighted_lower_bound << "\n";

  out << indent << "bnb_frontier_benchmark:\n";
  if (!a.wric_polytomy_benchmark_bnb) {
    out << child << "skipped: true\n";
    out << child
        << "skipped_reason: enable --wric-polytomy-benchmark-bnb\n";
    return;
  }

  auto trim_opts = make_chart_bnb_trim_options(a);
  try {
    auto bnb_start = std::chrono::steady_clock::now();
    auto trim = build_multisite_trim(grammar, patterns, chart_opts, trim_opts);
    auto bnb_ms = elapsed_ms(bnb_start, std::chrono::steady_clock::now());
    out << child << "success: true\n";
    out << child << "bnb_trim_ms: " << std::fixed << std::setprecision(3)
        << bnb_ms << "\n";
    out << child << "optimum: " << trim.optimum << "\n";
    out << child << "active_patterns: " << trim.active_pattern_count << "\n";
    out << child << "equality_deduplicated: "
        << trim.equality_deduplicated << "\n";
    out << child << "dominance_mode: "
        << multisite_dominance_mode_name(trim.dominance_mode) << "\n";
    out << child << "keep_mask_kind: "
        << multisite_keep_mask_kind_name(trim.keep_mask_kind) << "\n";
    out << child << "keep_production_exact: "
        << (trim.keep_production_exact ? "true" : "false") << "\n";
    out << child << "dominance_candidates_considered: "
        << trim.dominance_candidates_considered << "\n";
    out << child << "dominance_pruned_score_pass: "
        << trim.dominance_pruned_score_pass << "\n";
    out << child << "dominance_pruned_mask_pass: "
        << trim.dominance_pruned_mask_pass << "\n";
    out << child << "dominance_pruned: " << trim.dominance_pruned << "\n";
    out << child << "exact_mask_recovery_passes: "
        << trim.exact_mask_recovery_passes << "\n";
    out << child << "bound_pruned: " << trim.bound_pruned << "\n";
    out << child << "bound_pruning: "
        << (trim_opts.use_bound_pruning ? "true" : "false") << "\n";
    out << child << "upper_bound_override: "
        << (trim_opts.upper_bound_override
                ? std::to_string(*trim_opts.upper_bound_override)
                : std::string{"none"})
        << "\n";
    out << child << "upper_bound_override_kind: PRUNING_ONLY\n";
    out << child << "max_frontier_entries_per_clade: "
        << trim_opts.max_frontier_entries_per_clade << "\n";
    print_named_frontier_summary(
        out, "frontier_all_clades",
        summarize_frontier_sizes(trim.frontier_sizes_by_clade), child);

    std::vector<bool> synthetic_clades(refinement.clade_info.size(), false);
    for (std::size_t cid = 0; cid < refinement.clade_info.size(); ++cid) {
      synthetic_clades[cid] = clade_info_has_synthetic_polytomy_origin(
          refinement.clade_info[cid]);
    }
    print_named_frontier_summary(
        out, "frontier_synthetic_clades",
        summarize_frontier_sizes(trim.frontier_sizes_by_clade,
                                 &synthetic_clades),
        child);
  } catch (std::exception const& e) {
    out << child << "success: false\n";
    out << child << "error: " << e.what() << "\n";
  }
}

static mutation_position benchmark_site(args const& a, phylo_dag& dag) {
  if (a.wric_polytomy_benchmark_site) return *a.wric_polytomy_benchmark_site;
  auto const& reference = get_reference_sequence(dag);
  if (reference.empty()) {
    throw std::runtime_error(
        "WRIC polytomy benchmark requires a non-empty reference sequence");
  }
  return 1;
}

static void run_wric_polytomy_benchmark(std::ostream& out, phylo_dag& dag,
                                        args const& a) {
  auto site = benchmark_site(a, dag);

  clade_grammar_options grammar_opts;
  auto audit_opts = a.wric_polytomy_opts;
  audit_opts.mode = polytomy_mode::audit_kary;

  auto audit_start = std::chrono::steady_clock::now();
  auto audit_refinement = build_polytomy_refined_clade_grammar(
      dag, grammar_opts, audit_opts);
  auto audit_ms = elapsed_ms(audit_start, std::chrono::steady_clock::now());

  auto exact_preflight = summarize_wric_exact_preflight(
      audit_refinement, a.wric_polytomy_opts);
  bool soft_count_saturated = false;
  auto soft_count = soft_refinement_tree_count_estimate(
      audit_refinement.grammar, soft_count_saturated);

  out << "wric_polytomy_benchmark:\n";
  out << "  benchmark_scope: WRIC_PHASE9_POLYTOMY_AWARE\n";
  out << "  benchmark_site: " << site << "\n";
  out << "  audit_build_ms: " << std::fixed << std::setprecision(3)
      << audit_ms << "\n";
  out << "  source_clades: " << audit_refinement.audit.source_clade_count
      << "\n";
  out << "  source_productions: "
      << audit_refinement.audit.source_production_count << "\n";
  out << "  source_history_nodes_per_collapsed_clade_max: "
      << audit_refinement.source_grammar_audit.max_nodes_per_collapsed_clade
      << "\n";
  out << "  source_history_nodes_per_collapsed_clade_mean: " << std::fixed
      << std::setprecision(3)
      << audit_refinement.source_grammar_audit.mean_nodes_per_collapsed_clade
      << "\n";
  out << "  source_history_nodes_per_collapsed_clade_median: " << std::fixed
      << std::setprecision(3)
      << audit_refinement.source_grammar_audit.median_nodes_per_collapsed_clade
      << "\n";
  out << "  source_strict_reference_acgt_fail: "
      << audit_refinement.source_grammar_audit.strict_reference_acgt_fail
      << "\n";
  out << "  source_strict_leaf_compact_genome_acgt_fail: "
      << audit_refinement.source_grammar_audit
             .strict_leaf_compact_genome_acgt_fail
      << "\n";
  out << "  source_kary_productions: "
      << audit_refinement.audit.source_kary_production_count << "\n";
  out << "  source_kary_arity_histogram:\n";
  auto histogram = source_kary_arity_histogram(audit_refinement);
  if (histogram.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [arity, count] : histogram)
      out << "    " << arity << ": " << count << "\n";
  }
  out << "  source_grammar_tree_count_estimate: ";
  print_saturated_u64(
      out, audit_refinement.source_grammar_audit.grammar_tree_count_estimate,
      audit_refinement.source_grammar_audit
          .grammar_tree_count_estimate_saturated);
  out << "\n";
  out << "  full_soft_refinement_count_estimate: ";
  print_saturated_u64(out, soft_count, soft_count_saturated);
  out << "\n";
  print_wric_exact_expansion_preflight(out, audit_refinement,
                                       a.wric_polytomy_opts);

  out << "  source_chart_benchmark:\n";
  if (polytomy_refinement_allows_binary_charting(audit_refinement.audit)) {
    print_binary_refinement_performance_benchmark(out, dag, audit_refinement,
                                                  site, a, "    ");
  } else {
    out << "    skipped: true\n";
    out << "    skipped_reason: source grammar contains k-ary productions\n";
  }

  out << "  exact_expansion_benchmark:\n";
  out << "    preflight_theoretical_upper_bound_fits_configured_caps: "
      << (exact_preflight.fits_configured_exact_caps() ? "true" : "false")
      << "\n";
  if (!exact_preflight.fits_configured_exact_caps()) {
    out << "    preflight_note: theoretical upper bounds may exceed "
           "configured caps; attempting reuse-aware exact expansion\n";
  }
  auto exact_opts = a.wric_polytomy_opts;
  exact_opts.mode = polytomy_mode::expand_soft_exact_or_fail;
  auto exact_start = std::chrono::steady_clock::now();
  try {
    auto exact_refinement = build_polytomy_refined_clade_grammar(
        dag, grammar_opts, exact_opts);
    auto exact_ms = elapsed_ms(exact_start, std::chrono::steady_clock::now());
    out << "    attempted: true\n";
    out << "    success: true\n";
    out << "    exact_expansion_ms: " << std::fixed
        << std::setprecision(3) << exact_ms << "\n";
    print_refinement_benchmark_summary(out, exact_refinement, "    ");
    print_binary_refinement_performance_benchmark(out, dag, exact_refinement,
                                                  site, a, "    ");
  } catch (std::exception const& e) {
    auto exact_ms = elapsed_ms(exact_start, std::chrono::steady_clock::now());
    out << "    attempted: true\n";
    out << "    success: false\n";
    out << "    exact_expansion_ms: " << std::fixed
        << std::setprecision(3) << exact_ms << "\n";
    out << "    error: " << e.what() << "\n";
  }

  out << "  bounded_expansion_benchmarks:\n";
  for (auto cap : a.wric_polytomy_benchmark_shape_caps) {
    out << "    - max_shapes_per_polytomy: " << cap << "\n";
    auto bounded_opts = a.wric_polytomy_opts;
    bounded_opts.mode = polytomy_mode::expand_soft_bounded;
    bounded_opts.max_shapes_per_polytomy = cap;
    auto bounded_start = std::chrono::steady_clock::now();
    try {
      auto bounded_refinement = build_polytomy_refined_clade_grammar(
          dag, grammar_opts, bounded_opts);
      auto bounded_ms = elapsed_ms(bounded_start,
                                   std::chrono::steady_clock::now());
      out << "      success: true\n";
      out << "      bounded_expansion_ms: " << std::fixed
          << std::setprecision(3) << bounded_ms << "\n";
      print_refinement_benchmark_summary(out, bounded_refinement, "      ");
      print_binary_refinement_performance_benchmark(
          out, dag, bounded_refinement, site, a, "      ");
    } catch (std::exception const& e) {
      auto bounded_ms = elapsed_ms(bounded_start,
                                   std::chrono::steady_clock::now());
      out << "      success: false\n";
      out << "      bounded_expansion_ms: " << std::fixed
          << std::setprecision(3) << bounded_ms << "\n";
      out << "      error: " << e.what() << "\n";
    }
  }
}

static void print_chart_spr_search_counter_fields(
    std::ostream& out, chart_spr_search_counters const& counters,
    std::string const& indent) {
  out << indent << "grammar_rebuilds: " << counters.grammar_rebuilds << "\n";
  out << indent << "pattern_rebuilds: " << counters.pattern_rebuilds << "\n";
  out << indent << "base_chart_cache_rebuilds: "
      << counters.base_chart_cache_rebuilds << "\n";
  out << indent << "full_overlay_materializations: "
      << counters.full_overlay_materializations << "\n";
  out << indent << "overlay_materializations_for_oracle: "
      << counters.overlay_materializations_for_oracle << "\n";
  out << indent << "overlay_materializations_for_local_scoring_bridge: "
      << counters.overlay_materializations_for_local_scoring_bridge << "\n";
  out << indent << "overlay_materializations_for_exact_verification: "
      << counters.overlay_materializations_for_exact_verification << "\n";
  out << indent << "overlay_materializations_for_accept_materialization: "
      << counters.overlay_materializations_for_accept_materialization << "\n";
  out << indent << "sidecar_rebuilds_after_accept: "
      << counters.sidecar_rebuilds_after_accept << "\n";
  out << indent << "full_composite_rebuilds: "
      << counters.full_composite_rebuilds << "\n";
  out << indent << "local_candidate_scores: "
      << counters.local_candidate_scores << "\n";
  out << indent << "local_rows_recomputed: "
      << counters.local_rows_recomputed << "\n";
  out << indent << "local_score_parallel_batches: "
      << counters.local_score_parallel_batches << "\n";
  out << indent << "local_score_worker_tasks: "
      << counters.local_score_worker_tasks << "\n";
  out << indent << "candidate_batches_scored: "
      << counters.candidate_batches_scored << "\n";
  out << indent << "pattern_batch_cache_builds: "
      << counters.pattern_batch_cache_builds << "\n";
  out << indent << "exact_verifications: " << counters.exact_verifications
      << "\n";
  out << indent << "accepted_moves: " << counters.accepted_moves << "\n";
  out << indent << "candidate_accepts_attempted: "
      << counters.candidate_accepts_attempted << "\n";
  out << indent << "rejected_moves: " << counters.rejected_moves << "\n";
  out << indent << "post_materialization_rejections: "
      << counters.post_materialization_rejections << "\n";
  out << indent << "skipped_invariant_sites: "
      << counters.skipped_invariant_sites << "\n";
  out << indent << "candidate_source_productions_considered: "
      << counters.candidate_source_productions_considered << "\n";
  out << indent << "upward_path_iterator_steps: "
      << counters.upward_path_iterator_steps << "\n";
  out << indent << "upward_paths_completed: "
      << counters.upward_paths_completed << "\n";
  out << indent << "path_pairs_considered: "
      << counters.path_pairs_considered << "\n";
  out << indent << "candidates_constructed: "
      << counters.candidates_constructed << "\n";
  out << indent << "candidates_pruned_before_construction: "
      << counters.candidates_pruned_before_construction << "\n";
  out << indent << "candidates_pruned_after_construction: "
      << counters.candidates_pruned_after_construction << "\n";
  out << indent << "candidates_generated_after_dedup: "
      << counters.candidates_generated_after_dedup << "\n";
  out << indent << "candidates_pruned_root_or_trivial: "
      << counters.candidates_pruned_root_or_trivial << "\n";
  out << indent << "candidates_pruned_moved_size: "
      << counters.candidates_pruned_moved_size << "\n";
  out << indent << "candidates_pruned_target_size: "
      << counters.candidates_pruned_target_size << "\n";
  out << indent << "candidates_pruned_overlap: "
      << counters.candidates_pruned_overlap << "\n";
  out << indent << "candidates_pruned_affected_estimate: "
      << counters.candidates_pruned_affected_estimate << "\n";
  out << indent << "candidates_pruned_immediate_reversal: "
      << counters.candidates_pruned_immediate_reversal << "\n";
  out << indent << "candidates_pruned_duplicate: "
      << counters.candidates_pruned_duplicate << "\n";
  out << indent << "candidates_pruned_invalid: "
      << counters.candidates_pruned_invalid << "\n";
  out << indent << "candidate_cap_cutoffs: "
      << counters.candidate_cap_cutoffs << "\n";
  out << indent << "path_budget_cutoffs: "
      << counters.path_budget_cutoffs << "\n";
  out << indent << "overlay_reachability_validations: "
      << counters.overlay_reachability_validations << "\n";
  out << indent << "reachable_clades_traversed: "
      << counters.reachable_clades_traversed << "\n";
  out << indent << "reachable_productions_traversed: "
      << counters.reachable_productions_traversed << "\n";
  out << indent << "reachable_temp_clades_traversed: "
      << counters.reachable_temp_clades_traversed << "\n";
  out << indent << "reachable_temp_productions_traversed: "
      << counters.reachable_temp_productions_traversed << "\n";
  out << indent << "reachability_full_grammar_like_passes: "
      << counters.reachability_full_grammar_like_passes << "\n";
}

static void run_chart_spr_candidate_diagnostic(
    std::ostream& out, phylo_dag& dag,
    polytomy_refinement_result& refinement, args const& a) {
  auto const& grammar = refinement.grammar;
  auto opts = a.chart_spr_enumeration;
  if (opts.source != chart_spr_candidate_source::grammar) {
    opts.sampled_tree_source_dag = &dag;
  }

  std::vector<std::string> signatures;
  std::size_t candidates_emitted = 0;
  auto start = std::chrono::steady_clock::now();
  auto stats = for_each_grammar_spr_candidate(
      grammar, opts, [&](grammar_spr_candidate const& candidate) {
        ++candidates_emitted;
        if (within_limit(signatures.size(), a.chart_entry_limit)) {
          signatures.push_back(
              chart_spr_candidate_sample_signature(grammar, candidate));
        }
        return true;
      });
  auto generation_ms = elapsed_ms(start, std::chrono::steady_clock::now());

  out << "chart_spr_candidates:\n";
  out << "  api: streaming\n";
  out << "  polytomy_mode: "
      << wric_polytomy_mode_name(a.wric_polytomy_opts.mode) << "\n";
  out << "  binary_chart_compatibility: required_checked\n";
  out << "  source: " << chart_spr_candidate_source_name(opts.source)
      << "\n";
  out << "  candidate_signature_identity: "
      << "stable_sample_taxa_and_production_keys\n";
  out << "  randomize_order: "
      << (opts.randomize_order ? "true" : "false") << "\n";
  out << "  reservoir_sample: "
      << (opts.reservoir_sample ? "true" : "false") << "\n";
  out << "  sampled_tree_count: " << opts.sampled_tree_count << "\n";
  out << "  sampled_tree_spr_radius: " << opts.sampled_tree_spr_radius
      << "\n";
  out << "  sampled_tree_score_threshold: "
      << opts.sampled_tree_score_threshold << "\n";
  out << "  generation_ms: " << std::fixed << std::setprecision(3)
      << generation_ms << "\n";
  out << "  candidate_count: " << candidates_emitted << "\n";
  out << "  candidates_emitted: " << candidates_emitted << "\n";
  if (opts.reservoir_sample && opts.max_candidates != 0) {
    out << "  reservoir_size: " << candidates_emitted << "\n";
  }
  out << "  candidate_cap: " << opts.max_candidates << "\n";
  out << "  candidate_cap_semantics: "
      << (opts.max_candidates_is_post_dedup ? "post_dedup" : "pre_dedup")
      << "\n";
  out << "  max_upward_path_expansions: "
      << opts.max_upward_path_expansions << "\n";
  out << "  max_path_pairs_considered: "
      << opts.max_path_pairs_considered << "\n";
  out << "  min_moved_clade_size: " << opts.min_moved_clade_size << "\n";
  out << "  max_moved_clade_size: " << opts.max_moved_clade_size << "\n";
  out << "  min_target_clade_size: " << opts.min_target_clade_size << "\n";
  out << "  max_target_clade_size: " << opts.max_target_clade_size << "\n";
  out << "  candidate_generation:\n";
  out << "    stop_reason: "
      << chart_spr_candidate_stop_reason_name(stats.stop_reason) << "\n";
  out << "    upward_path_iterator_steps: "
      << stats.upward_path_iterator_steps << "\n";
  out << "    upward_paths_completed: " << stats.upward_paths_completed
      << "\n";
  out << "    path_pairs_considered: " << stats.path_pairs_considered
      << "\n";
  out << "    candidates_constructed: " << stats.candidates_constructed
      << "\n";
  out << "    candidates_pruned_before_construction: "
      << stats.candidates_pruned_before_construction << "\n";
  out << "    candidates_pruned_after_construction: "
      << stats.candidates_pruned_after_construction << "\n";
  out << "    candidates_generated_after_dedup: "
      << stats.candidates_generated_after_dedup << "\n";
  out << "    pruned_root_or_trivial: "
      << stats.candidates_pruned_root_or_trivial << "\n";
  out << "    pruned_moved_size: " << stats.candidates_pruned_moved_size
      << "\n";
  out << "    pruned_target_size: " << stats.candidates_pruned_target_size
      << "\n";
  out << "    pruned_overlap: " << stats.candidates_pruned_overlap
      << "\n";
  out << "    pruned_affected_estimate: "
      << stats.candidates_pruned_affected_estimate << "\n";
  out << "    pruned_immediate_reversal: "
      << stats.candidates_pruned_immediate_reversal << "\n";
  out << "    pruned_duplicate: " << stats.candidates_pruned_duplicate
      << "\n";
  out << "    pruned_invalid: " << stats.candidates_pruned_invalid << "\n";
  out << "  candidate_signatures:\n";
  if (signatures.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& signature : signatures) {
      out << "    - " << signature << "\n";
    }
  }
  if (a.chart_entry_limit != 0 && signatures.size() < candidates_emitted) {
    out << "  candidate_signatures_truncated: true\n";
  }
}

static void add_candidate_generation_stats_to_counters(
    chart_spr_candidate_generation_stats const& stats,
    chart_spr_search_counters& counters) {
  counters.upward_path_iterator_steps += stats.upward_path_iterator_steps;
  counters.upward_paths_completed += stats.upward_paths_completed;
  counters.path_pairs_considered += stats.path_pairs_considered;
  counters.candidates_constructed += stats.candidates_constructed;
  counters.candidates_pruned_before_construction +=
      stats.candidates_pruned_before_construction;
  counters.candidates_pruned_after_construction +=
      stats.candidates_pruned_after_construction;
  counters.candidates_generated_after_dedup +=
      stats.candidates_generated_after_dedup;
  counters.candidates_pruned_root_or_trivial +=
      stats.candidates_pruned_root_or_trivial;
  counters.candidates_pruned_moved_size += stats.candidates_pruned_moved_size;
  counters.candidates_pruned_target_size += stats.candidates_pruned_target_size;
  counters.candidates_pruned_overlap += stats.candidates_pruned_overlap;
  counters.candidates_pruned_affected_estimate +=
      stats.candidates_pruned_affected_estimate;
  counters.candidates_pruned_immediate_reversal +=
      stats.candidates_pruned_immediate_reversal;
  counters.candidates_pruned_duplicate += stats.candidates_pruned_duplicate;
  counters.candidates_pruned_invalid += stats.candidates_pruned_invalid;
  if (stats.stop_reason == chart_spr_candidate_stop_reason::candidate_cap)
    ++counters.candidate_cap_cutoffs;
  if (stats.stop_reason == chart_spr_candidate_stop_reason::path_budget)
    ++counters.path_budget_cutoffs;
}

static void print_chart_spr_generation_stats(
    std::ostream& out, chart_spr_candidate_generation_stats const& stats,
    std::string const& indent) {
  out << indent << "stop_reason: "
      << chart_spr_candidate_stop_reason_name(stats.stop_reason) << "\n";
  out << indent << "upward_path_iterator_steps: "
      << stats.upward_path_iterator_steps << "\n";
  out << indent << "upward_paths_completed: "
      << stats.upward_paths_completed << "\n";
  out << indent << "path_pairs_considered: "
      << stats.path_pairs_considered << "\n";
  out << indent << "candidates_constructed: "
      << stats.candidates_constructed << "\n";
  out << indent << "candidates_pruned_before_construction: "
      << stats.candidates_pruned_before_construction << "\n";
  out << indent << "candidates_pruned_after_construction: "
      << stats.candidates_pruned_after_construction << "\n";
  out << indent << "candidates_generated_after_dedup: "
      << stats.candidates_generated_after_dedup << "\n";
  out << indent << "pruned_root_or_trivial: "
      << stats.candidates_pruned_root_or_trivial << "\n";
  out << indent << "pruned_moved_size: "
      << stats.candidates_pruned_moved_size << "\n";
  out << indent << "pruned_target_size: "
      << stats.candidates_pruned_target_size << "\n";
  out << indent << "pruned_overlap: "
      << stats.candidates_pruned_overlap << "\n";
  out << indent << "pruned_affected_estimate: "
      << stats.candidates_pruned_affected_estimate << "\n";
  out << indent << "pruned_immediate_reversal: "
      << stats.candidates_pruned_immediate_reversal << "\n";
  out << indent << "pruned_duplicate: "
      << stats.candidates_pruned_duplicate << "\n";
  out << indent << "pruned_invalid: "
      << stats.candidates_pruned_invalid << "\n";
}

static chart_spr_search_options make_chart_spr_search_options(args const& a);

static void run_chart_spr_local_scoring_diagnostic(
    std::ostream& out, phylo_dag& dag,
    polytomy_refinement_result& refinement, args const& a,
    std::string_view report_name) {
  auto search_options = make_chart_spr_search_options(a);
  search_options.acceptance_mode =
      chart_spr_acceptance_mode::lower_bound_heuristic;

  auto cache_start = std::chrono::steady_clock::now();
  auto state = build_chart_spr_search_state(dag, refinement.grammar,
                                            search_options);
  auto cache_ms = elapsed_ms(cache_start, std::chrono::steady_clock::now());

  auto enumeration = a.chart_spr_enumeration;
  if (enumeration.source != chart_spr_candidate_source::grammar) {
    enumeration.sampled_tree_source_dag = state.dag;
  }
  std::vector<grammar_spr_candidate> candidates;
  candidates.reserve(enumeration.max_candidates == 0
                         ? 0
                         : enumeration.max_candidates);
  auto generation_start = std::chrono::steady_clock::now();
  auto generation = for_each_grammar_spr_candidate(
      state.grammar, enumeration,
      [&](grammar_spr_candidate const& candidate) {
        candidates.push_back(candidate);
        return true;
      });
  auto generation_ms = elapsed_ms(generation_start,
                                  std::chrono::steady_clock::now());
  add_candidate_generation_stats_to_counters(generation, state.counters);

  std::optional<chart_spr_candidate_score> best;
  std::vector<std::size_t> affected_counts;
  std::size_t failures = 0;
  std::string last_error;
  double local_ms_total = 0.0;
  auto scoring_start = std::chrono::steady_clock::now();
  auto scored_candidates = score_candidates_locally(
      state, candidates, {}, a.chart_spr_local_score_workers);
  auto local_scoring_wall_ms = elapsed_ms(scoring_start,
                                          std::chrono::steady_clock::now());
  for (auto& scored : scored_candidates) {
    local_ms_total += scored.local_score_ms;
    if (!scored.valid) {
      ++failures;
      last_error = scored.invalid_reason;
      continue;
    }
    affected_counts.push_back(scored.affected_clade_count);
    if (!best || scored.lower_bound.value.delta < best->lower_bound.value.delta) {
      best = std::move(scored);
    }
  }

  auto affected = summarize_affected_clade_counts(std::move(affected_counts));
  auto scored_count = state.counters.local_candidate_scores;
  if (local_ms_total == 0.0 && scored_count != 0) {
    local_ms_total = local_scoring_wall_ms;
  }
  auto local_ms_per_candidate = scored_count == 0
                                    ? 0.0
                                    : local_ms_total /
                                          static_cast<double>(scored_count);

  out << report_name << ":\n";
  out << "  api: search_state_cached_active_pattern_charts\n";
  out << "  polytomy_mode: "
      << wric_polytomy_mode_name(a.wric_polytomy_opts.mode) << "\n";
  out << "  binary_chart_compatibility: required_checked\n";
  out << "  score_kind: composite_lower_bound\n";
  out << "  score_convention: active_cache_plus_single_invariant_offset\n";
  out << "  phase1_materialized_overlay_local_recompute: false\n";
  out << "  phase3_lightweight_overlay_delta: true\n";
  if (report_name == "chart_spr_search") {
    out << "  search_mode: phase3_rank_only_no_dag_mutation\n";
    out << "  actual_accept_reject_search: false\n";
    out << "  requested_max_iterations: " << a.chart_spr_max_iterations
        << "\n";
    out << "  implemented_rank_only_iterations: 1\n";
    out << "  max_iterations_note: ignored by the Phase-3 rank-only "
           "diagnostic; Phase-5 implements multi-iteration mutation\n";
  }
  out << "  score_ua_edge: "
      << (a.chart_score_ua_edge ? "true" : "false") << "\n";
  out << "  root_row_scoring_api: chart_spr_weighted_root_score_from_row\n";
  out << "  active_patterns: "
      << state.active_patterns.patterns.patterns.size() << "\n";
  out << "  skipped_invariant_sites: "
      << state.skipped_invariant_site_count << "\n";
  out << "  invariant_constant_offset: "
      << state.invariant_constant_offset << "\n";
  out << "  cache_strategy: "
      << chart_spr_cache_strategy_name(state.cache_strategy) << "\n";
  out << "  chart_cache_estimated_full_bytes: "
      << state.estimated_full_pattern_cache_bytes << "\n";
  out << "  chart_cache_resident_bytes: "
      << state.resident_pattern_cache_bytes << "\n";
  out << "  effective_pattern_batch_size: "
      << state.effective_pattern_batch_size << "\n";
  out << "  candidate_batch_size: "
      << search_options.cache.candidate_batch_size << "\n";
  out << "  local_score_workers: "
      << a.chart_spr_local_score_workers << "\n";
  out << "  cache_build_ms: " << std::fixed << std::setprecision(3)
      << cache_ms << "\n";
  out << "  candidate_source: "
      << chart_spr_candidate_source_name(enumeration.source) << "\n";
  out << "  candidate_signature_identity: "
      << "stable_sample_taxa_and_production_keys\n";
  out << "  randomize_order: "
      << (enumeration.randomize_order ? "true" : "false") << "\n";
  out << "  reservoir_sample: "
      << (enumeration.reservoir_sample ? "true" : "false") << "\n";
  out << "  sampled_tree_count: " << enumeration.sampled_tree_count << "\n";
  out << "  sampled_tree_spr_radius: "
      << enumeration.sampled_tree_spr_radius << "\n";
  out << "  candidate_generation_ms: " << std::fixed << std::setprecision(3)
      << generation_ms << "\n";
  out << "  local_scoring_wall_ms: " << std::fixed << std::setprecision(3)
      << local_scoring_wall_ms << "\n";
  out << "  local_scoring_ms_total: " << std::fixed << std::setprecision(3)
      << local_ms_total << "\n";
  out << "  local_scoring_ms_per_candidate: " << std::fixed
      << std::setprecision(6) << local_ms_per_candidate << "\n";
  if (local_scoring_wall_ms > 0.0) {
    auto seconds = local_scoring_wall_ms / 1000.0;
    out << "  local_candidates_per_second: " << std::fixed
        << std::setprecision(3)
        << (static_cast<double>(state.counters.local_candidate_scores) /
            seconds)
        << "\n";
    out << "  local_rows_recomputed_per_second: " << std::fixed
        << std::setprecision(3)
        << (static_cast<double>(state.counters.local_rows_recomputed) /
            seconds)
        << "\n";
  }
  out << "  candidates_generated: "
      << generation.candidates_generated_after_dedup << "\n";
  out << "  candidates_emitted: " << candidates.size() << "\n";
  if (enumeration.reservoir_sample && enumeration.max_candidates != 0) {
    out << "  reservoir_size: " << candidates.size() << "\n";
  }
  out << "  candidates_scored: " << scored_count << "\n";
  out << "  candidate_score_failures: " << failures << "\n";
  if (!last_error.empty()) out << "  last_score_error: " << last_error << "\n";
  out << "  current_lower_bound_active_only: "
      << state.composite_lower_bound_without_invariants << "\n";
  out << "  current_lower_bound: "
      << state.composite_lower_bound_with_invariants << "\n";
  if (best) {
    out << "  best_delta: " << best->lower_bound.value.delta << "\n";
    out << "  best_new_score: " << best->lower_bound.value.new_score << "\n";
    out << "  best_improves: "
        << (best->lower_bound.value.improves() ? "true" : "false") << "\n";
    out << "  best_affected_clades: " << best->affected_clade_count << "\n";
    out << "  best_candidate_signature: "
        << chart_spr_candidate_sample_signature(state.grammar,
                                                best->candidate)
        << "\n";
    if (report_name == "chart_spr_search") {
      out << "  accepted_move_present: "
          << (best->lower_bound.value.improves() ? "true" : "false")
          << "\n";
      out << "  output_dag_mutated: false\n";
    }
  } else if (report_name == "chart_spr_search") {
    out << "  accepted_move_present: false\n";
    out << "  output_dag_mutated: false\n";
  }
  out << "  affected_clade_count_distribution:\n";
  out << "    mean: " << std::fixed << std::setprecision(3)
      << affected.mean << "\n";
  out << "    p50: " << affected.p50 << "\n";
  out << "    p95: " << affected.p95 << "\n";
  out << "    max: " << affected.max << "\n";
  out << "  candidate_generation:\n";
  print_chart_spr_generation_stats(out, generation, "    ");
  out << "  counters:\n";
  print_chart_spr_search_counter_fields(out, state.counters, "    ");
}

static std::string chart_spr_topology_selection_label(
    chart_spr_topology_selection const& selection) {
  std::string label = chart_spr_topology_selection_kind_name(selection.kind);
  if (!selection.selector_name.empty()) {
    label += ":";
    label += selection.selector_name;
  }
  if (selection.certificate) label += ":certificate";
  return label;
}

static chart_spr_search_options make_chart_spr_search_options(
    args const& a) {
  chart_spr_search_options options;
  options.max_iterations = a.chart_spr_max_iterations;
  options.max_candidates_per_iteration = 0;
  options.top_k_exact_verify = a.chart_spr_top_k_exact;
  options.acceptance_mode = a.chart_spr_acceptance;
  options.candidate_selection = a.chart_spr_candidate_selection;
  options.fixed_topology_selector_name = a.chart_spr_topology_selector;
  options.enumeration = a.chart_spr_enumeration;
  options.cache = a.chart_spr_cache;
  options.local_score_worker_count = a.chart_spr_local_score_workers;
  options.rebuild_after_accept = !a.chart_spr_local_accept_updates;
  options.seed = a.seed.value_or(options.seed);
  options.chart.score_ua_edge = a.chart_score_ua_edge;
  options.exact_trim = make_chart_bnb_trim_options(a);
  return options;
}

static void run_chart_spr_search_diagnostic(
    std::ostream& out, phylo_dag& dag,
    polytomy_refinement_result& refinement, args const& a) {
  auto options = make_chart_spr_search_options(a);
  auto search = run_chart_spr_search(std::move(dag), refinement.grammar,
                                     options);
  dag = std::move(search.dag);
  build_clade_offsets(dag);

  out << "chart_spr_search:\n";
  out << "  api: search_state_cached_active_pattern_charts\n";
  out << "  polytomy_mode: "
      << wric_polytomy_mode_name(a.wric_polytomy_opts.mode) << "\n";
  out << "  binary_chart_compatibility: required_checked\n";
  out << "  search_mode: "
      << (options.rebuild_after_accept
              ? "phase5_accept_reject_materialize_rebuild"
              : "phase9_accept_reject_local_cache_update")
      << "\n";
  out << "  accepted_state_update_mode: "
      << (options.rebuild_after_accept ? "materialize_rebuild"
                                       : "local_cache_update")
      << "\n";
  out << "  accepted_state_materialization: "
      << (options.rebuild_after_accept ? "materialize_dag_and_rebuild"
                                       : "dense_overlay_grammar_per_accept")
      << "\n";
  out << "  final_compaction_mode: "
      << (options.rebuild_after_accept
              ? "per_accept_materialized_dag"
              : "selected_tree_rebuild_equivalence_checked")
      << "\n";
  out << "  preserves_full_accepted_overlay_dag: "
      << (options.rebuild_after_accept ? "not_applicable" : "false")
      << "\n";
  out << "  actual_dag_mutation: "
      << (search.summary.accepted_moves > 0 ? "true" : "false") << "\n";
  out << "  acceptance: "
      << chart_spr_acceptance_mode_name(options.acceptance_mode) << "\n";
  out << "  candidate_selection: "
      << chart_spr_candidate_selection_mode_name(options.candidate_selection)
      << "\n";
  out << "  candidate_source: "
      << chart_spr_candidate_source_name(options.enumeration.source) << "\n";
  out << "  candidate_signature_identity: "
      << "stable_sample_taxa_and_production_keys\n";
  out << "  randomize_order: "
      << (options.enumeration.randomize_order ? "true" : "false")
      << "\n";
  out << "  reservoir_sample: "
      << (options.enumeration.reservoir_sample ? "true" : "false")
      << "\n";
  out << "  sampled_tree_count: " << options.enumeration.sampled_tree_count
      << "\n";
  out << "  sampled_tree_spr_radius: "
      << options.enumeration.sampled_tree_spr_radius << "\n";
  out << "  top_k_exact_verify: " << options.top_k_exact_verify << "\n";
  out << "  topology_selection: ";
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::fixed_topology_exact) {
    if (options.enumeration.source ==
            chart_spr_candidate_source::sampled_tree ||
        options.enumeration.source == chart_spr_candidate_source::hybrid) {
      out << "source_tree_move_certificate_or_deterministic_selector:"
          << options.fixed_topology_selector_name << "\n";
    } else {
      out << "deterministic_selector:"
          << options.fixed_topology_selector_name << "\n";
    }
  } else {
    out << "none\n";
  }
  out << "  objective: ";
  if (options.acceptance_mode ==
      chart_spr_acceptance_mode::lower_bound_heuristic) {
    out << "composite_lower_bound_heuristic\n";
  } else if (options.acceptance_mode ==
             chart_spr_acceptance_mode::fixed_topology_exact) {
    out << "fixed_topology_exact\n";
  } else {
    out << "grammar_exact\n";
  }
  out << "  score_convention: active_cache_plus_single_invariant_offset\n";
  out << "  score_ua_edge: "
      << (a.chart_score_ua_edge ? "true" : "false") << "\n";
  out << "  root_row_scoring_api: chart_spr_weighted_root_score_from_row\n";
  out << "  local_score_workers: "
      << search.summary.local_score_worker_count << "\n";
  out << "  cache_strategy: ";
  if (search.summary.effective_pattern_batch_size != 0 &&
      search.summary.effective_pattern_batch_size <
          search.summary.active_pattern_count) {
    out << "pattern_batches\n";
  } else {
    out << "all_active_patterns\n";
  }
  out << "  active_patterns: "
      << search.summary.active_pattern_count << "\n";
  out << "  initial_grammar_clades: "
      << search.summary.initial_grammar_clade_count << "\n";
  out << "  initial_grammar_productions: "
      << search.summary.initial_grammar_production_count << "\n";
  out << "  final_grammar_clades: "
      << search.summary.final_grammar_clade_count << "\n";
  out << "  final_grammar_productions: "
      << search.summary.final_grammar_production_count << "\n";
  out << "  chart_cache_estimated_full_bytes: "
      << search.summary.chart_cache_estimated_full_bytes << "\n";
  out << "  chart_cache_resident_bytes: "
      << search.summary.chart_cache_resident_bytes << "\n";
  out << "  effective_pattern_batch_size: "
      << search.summary.effective_pattern_batch_size << "\n";
  out << "  effective_candidate_batch_size: "
      << search.summary.effective_candidate_batch_size << "\n";
  out << "  requested_max_iterations: " << a.chart_spr_max_iterations
      << "\n";
  out << "  iterations: " << search.summary.iterations << "\n";
  out << "  accepted_moves: " << search.summary.accepted_moves << "\n";
  out << "  output_dag_mutated: "
      << (search.summary.accepted_moves > 0 ? "true" : "false") << "\n";
  out << "  candidate_accepts_attempted: "
      << search.summary.candidate_accepts_attempted << "\n";
  out << "  post_materialization_rejections: "
      << search.summary.post_materialization_rejections << "\n";
  out << "  initial_score: " << search.summary.initial_score << "\n";
  out << "  final_score: " << search.summary.final_score << "\n";
  out << "  candidates_generated: "
      << search.summary.candidates_generated << "\n";
  out << "  candidates_scored: "
      << search.summary.candidates_locally_scored << "\n";
  out << "  exact_verifications: "
      << search.summary.exact_verifications << "\n";
  out << "  initial_search_state_rebuilds: "
      << search.summary.initial_search_state_rebuilds << "\n";
  out << "  sidecar_rebuilds_after_accept: "
      << search.summary.sidecar_rebuilds_after_accept << "\n";
  out << "  full_search_state_rebuilds: "
      << search.summary.full_search_state_rebuilds << "\n";
  out << "  final_compaction_rebuilds: "
      << search.summary.final_compaction_rebuilds << "\n";
  out << "  overlay_materializations_for_exact_verification: "
      << search.summary.overlay_materializations_for_exact_verification
      << "\n";
  out << "  overlay_materializations_for_accept_materialization: "
      << search.summary.overlay_materializations_for_accept_materialization
      << "\n";
  out << "  cache_build_ms: " << std::fixed << std::setprecision(3)
      << search.summary.cache_build_ms << "\n";
  out << "  local_scoring_ms: " << std::fixed << std::setprecision(3)
      << search.summary.local_scoring_ms << "\n";
  out << "  local_candidates_per_second: " << std::fixed
      << std::setprecision(3)
      << search.summary.local_candidates_per_second << "\n";
  out << "  local_rows_recomputed: "
      << search.summary.local_rows_recomputed << "\n";
  out << "  local_rows_recomputed_per_second: " << std::fixed
      << std::setprecision(3)
      << search.summary.local_rows_recomputed_per_second << "\n";
  out << "  candidate_batches_scored: "
      << search.summary.candidate_batches_scored << "\n";
  out << "  pattern_batch_cache_builds: "
      << search.summary.pattern_batch_cache_builds << "\n";
  out << "  exact_verification_ms: " << std::fixed << std::setprecision(3)
      << search.summary.exact_verification_ms << "\n";
  out << "  accepted_rebuild_ms: " << std::fixed << std::setprecision(3)
      << search.summary.accepted_rebuild_ms << "\n";
  out << "  final_compaction_ms: " << std::fixed << std::setprecision(3)
      << search.summary.final_compaction_ms << "\n";
  out << "  post_materialization_check_ms: " << std::fixed
      << std::setprecision(3)
      << search.summary.post_materialization_check_ms << "\n";
  out << "  total_ms: " << std::fixed << std::setprecision(3)
      << search.summary.total_ms << "\n";
  out << "  final_dag:\n";
  out << "    leaves: " << leaf_count(dag) << "\n";
  out << "    nodes: " << node_count(dag) << "\n";
  out << "    edges: " << edge_count(dag) << "\n";
  out << "  affected_clade_count_distribution:\n";
  out << "    mean: " << std::fixed << std::setprecision(3)
      << search.summary.affected_distribution.mean << "\n";
  out << "    p50: " << search.summary.affected_distribution.p50 << "\n";
  out << "    p95: " << search.summary.affected_distribution.p95 << "\n";
  out << "    max: " << search.summary.affected_distribution.max << "\n";
  out << "  iteration_reports:\n";
  if (search.iterations.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& iteration : search.iterations) {
      out << "    - iteration: " << iteration.iteration << "\n";
      out << "      candidates_generated: "
          << iteration.candidates_generated << "\n";
      out << "      candidates_scored: " << iteration.candidates_scored
          << "\n";
      out << "      candidate_score_failures: "
          << iteration.candidate_score_failures << "\n";
      out << "      local_improving_candidates: "
          << iteration.local_improving_candidates << "\n";
      out << "      locally_ranked_candidates_retained: "
          << iteration.locally_ranked_candidates_retained << "\n";
      out << "      candidates_exact_verified: "
          << iteration.candidates_exact_verified << "\n";
      out << "      local_scoring_ms: " << std::fixed << std::setprecision(3)
          << iteration.local_scoring_ms << "\n";
      out << "      exact_verification_ms: " << std::fixed
          << std::setprecision(3) << iteration.exact_verification_ms
          << "\n";
      out << "      unverified_candidates_may_contain_improvements: "
          << (iteration.unverified_candidates_may_contain_improvements
                  ? "true"
                  : "false")
          << "\n";
      out << "      state_score_before: "
          << iteration.state_score_before << "\n";
      out << "      state_score_after: "
          << iteration.state_score_after << "\n";
      out << "      accepted_move_present: "
          << (iteration.accepted ? "true" : "false") << "\n";
      out << "      accepted_move_committed: "
          << (iteration.accepted_move_committed ? "true" : "false")
          << "\n";
      out << "      post_materialization_rejected: "
          << (iteration.post_materialization_rejected ? "true" : "false")
          << "\n";
      if (!iteration.no_accept_reason.empty()) {
        out << "      no_accept_reason: "
            << iteration.no_accept_reason << "\n";
      }
      if (!iteration.post_materialization_rejection_reason.empty()) {
        out << "      post_materialization_rejection_reason: "
            << iteration.post_materialization_rejection_reason << "\n";
      }
      if (iteration.accepted) {
        auto const& accepted = *iteration.accepted;
        out << "      accepted_lower_bound_delta: "
            << accepted.lower_bound.value.delta << "\n";
        out << "      accepted_lower_bound_new_score: "
            << accepted.lower_bound.value.new_score << "\n";
        if (accepted.exact) {
          out << "      accepted_exact_kind: "
              << chart_spr_score_kind_name(accepted.exact->kind) << "\n";
          out << "      accepted_exact_delta: "
              << accepted.exact->value.delta << "\n";
          out << "      accepted_exact_new_score: "
              << accepted.exact->value.new_score << "\n";
        }
        out << "      accepted_affected_clades: "
            << accepted.affected_clade_count << "\n";
        out << "      accepted_topology_selection: "
            << chart_spr_topology_selection_label(
                   accepted.topology_selection)
            << "\n";
        out << "      reused_patterns_after_accept: "
            << (iteration.reused_patterns_after_accept ? "true" : "false")
            << "\n";
        out << "      post_materialization_rebuilt_score: "
            << iteration.post_materialization_rebuilt_score << "\n";
        if (iteration.iteration == 0) {
          out << "      accepted_candidate_signature: "
              << chart_spr_candidate_sample_signature(
                     refinement.grammar, accepted.candidate)
              << "\n";
        }
      }
      out << "      affected_clade_count_distribution:\n";
      out << "        mean: " << std::fixed << std::setprecision(3)
          << iteration.affected_distribution.mean << "\n";
      out << "        p50: " << iteration.affected_distribution.p50
          << "\n";
      out << "        p95: " << iteration.affected_distribution.p95
          << "\n";
      out << "        max: " << iteration.affected_distribution.max
          << "\n";
      out << "      candidate_generation:\n";
      print_chart_spr_generation_stats(out, iteration.candidate_generation,
                                       "        ");
    }
  }
  out << "  counters:\n";
  print_chart_spr_search_counter_fields(out, search.counters, "    ");
}

static void run_chart_spr_helper_benchmark(
    std::ostream& out, polytomy_refinement_result& refinement,
    double grammar_build_ms, site_pattern_set& patterns,
    double pattern_build_ms, args const& a) {
  auto const& grammar = refinement.grammar;
  chart_options chart_opts;
  chart_opts.score_ua_edge = a.chart_score_ua_edge;

  chart_spr_search_counters counters;
  counters.grammar_rebuilds = 1;
  counters.pattern_rebuilds = 1;
  counters.skipped_invariant_sites = patterns.skipped_invariant_site_count;

  auto composite_start = std::chrono::steady_clock::now();
  auto composite = build_composite_chart_score(grammar, patterns, chart_opts);
  auto composite_ms = elapsed_ms(composite_start,
                                 std::chrono::steady_clock::now());

  grammar_spr_enumeration_options enum_opts;
  enum_opts.max_candidates = a.chart_spr_helper_benchmark_max_candidates;
  auto generation_start = std::chrono::steady_clock::now();
  auto enumeration = enumerate_grammar_spr_candidates_eager_diagnostic(
      grammar, enum_opts, &counters);
  auto generation_ms = elapsed_ms(generation_start,
                                  std::chrono::steady_clock::now());

  double helper_ms_total = 0.0;
  std::size_t helper_improving_candidates = 0;
  std::size_t helper_failures = 0;
  std::string helper_last_error;
  for (auto const& candidate : enumeration.candidates) {
    auto start = std::chrono::steady_clock::now();
    try {
      auto score = score_multisite_spr_candidate_lower_bound_oracle(
          grammar, patterns, candidate, chart_opts, &counters);
      if (score.improves()) ++helper_improving_candidates;
    } catch (std::exception const& e) {
      ++helper_failures;
      helper_last_error = e.what();
    }
    helper_ms_total += elapsed_ms(start, std::chrono::steady_clock::now());
  }

  bool local_oracle_available = !patterns.patterns.empty();
  double local_oracle_base_chart_ms = 0.0;
  double local_oracle_ms_total = 0.0;
  std::size_t local_oracle_failures = 0;
  std::string local_oracle_last_error;
  std::vector<std::size_t> affected_counts;
  single_site_chart base_chart;
  leaf_site_states first_pattern_states;
  if (local_oracle_available) {
    first_pattern_states.state_by_taxon = patterns.patterns.front().state_by_taxon;
    auto start = std::chrono::steady_clock::now();
    base_chart = build_single_site_chart(grammar, first_pattern_states,
                                         chart_opts);
    local_oracle_base_chart_ms = elapsed_ms(start,
                                            std::chrono::steady_clock::now());
  }

  if (local_oracle_available) {
    for (auto const& candidate : enumeration.candidates) {
      auto start = std::chrono::steady_clock::now();
      try {
        auto overlay = overlay_from_candidate(grammar, candidate);
        auto local = score_rejected_candidate_with_local_recompute_oracle(
            overlay, base_chart, first_pattern_states, chart_opts, &counters);
        affected_counts.push_back(local.affected_clade_count);
      } catch (std::exception const& e) {
        ++local_oracle_failures;
        local_oracle_last_error = e.what();
      }
      local_oracle_ms_total += elapsed_ms(start,
                                          std::chrono::steady_clock::now());
    }
  }

  auto affected = summarize_affected_clade_counts(std::move(affected_counts));
  auto candidate_count = enumeration.candidates.size();
  auto helper_ms_per_candidate = candidate_count == 0
                                     ? 0.0
                                     : helper_ms_total /
                                           static_cast<double>(candidate_count);
  auto local_oracle_ms_per_candidate = candidate_count == 0
                                           ? 0.0
                                           : local_oracle_ms_total /
                                                 static_cast<double>(candidate_count);

  out << "chart_spr_helper_benchmark:\n";
  out << "  benchmark_scope: CHART_SPR_PHASE0_HELPER_BASELINE\n";
  out << "  diagnostic_oracle_path: true\n";
  out << "  score_ua_edge: "
      << (a.chart_score_ua_edge ? "true" : "false") << "\n";
  out << "  grammar_build_ms: " << std::fixed << std::setprecision(3)
      << grammar_build_ms << "\n";
  out << "  pattern_extraction_ms: " << std::fixed << std::setprecision(3)
      << pattern_build_ms << "\n";
  out << "  composite_chart_ms: " << std::fixed << std::setprecision(3)
      << composite_ms << "\n";
  out << "  baseline_composite_lower_bound: "
      << composite.weighted_lower_bound << "\n";
  out << "  baseline_full_composite_rebuilds: 1\n";
  out << "  candidate_generation_ms: " << std::fixed << std::setprecision(3)
      << generation_ms << "\n";
  out << "  candidate_count: " << candidate_count << "\n";
  out << "  candidate_cap: " << enum_opts.max_candidates << "\n";
  out << "  current_helper_ms_total: " << std::fixed << std::setprecision(3)
      << helper_ms_total << "\n";
  out << "  current_helper_ms_per_candidate: " << std::fixed
      << std::setprecision(6) << helper_ms_per_candidate << "\n";
  out << "  current_helper_failures: " << helper_failures << "\n";
  if (!helper_last_error.empty())
    out << "  current_helper_last_error: " << helper_last_error << "\n";
  out << "  current_helper_improving_candidates: "
      << helper_improving_candidates << "\n";
  out << "  local_recompute_oracle_available: "
      << (local_oracle_available ? "true" : "false") << "\n";
  out << "  local_recompute_oracle_base_chart_ms: " << std::fixed
      << std::setprecision(3) << local_oracle_base_chart_ms << "\n";
  out << "  local_recompute_oracle_ms_total: " << std::fixed
      << std::setprecision(3) << local_oracle_ms_total << "\n";
  out << "  local_recompute_oracle_ms_per_candidate: " << std::fixed
      << std::setprecision(6) << local_oracle_ms_per_candidate << "\n";
  out << "  local_recompute_oracle_failures: "
      << local_oracle_failures << "\n";
  if (!local_oracle_last_error.empty())
    out << "  local_recompute_oracle_last_error: "
        << local_oracle_last_error << "\n";
  out << "  affected_clade_count_distribution:\n";
  out << "    mean: " << std::fixed << std::setprecision(3)
      << affected.mean << "\n";
  out << "    p50: " << affected.p50 << "\n";
  out << "    p95: " << affected.p95 << "\n";
  out << "    max: " << affected.max << "\n";
  out << "  candidate_generation:\n";
  out << "    stop_reason: "
      << chart_spr_candidate_stop_reason_name(enumeration.stats.stop_reason)
      << "\n";
  out << "    upward_path_iterator_steps: "
      << enumeration.stats.upward_path_iterator_steps << "\n";
  out << "    upward_paths_completed: "
      << enumeration.stats.upward_paths_completed << "\n";
  out << "    path_pairs_considered: "
      << enumeration.stats.path_pairs_considered << "\n";
  out << "    candidates_constructed: "
      << enumeration.stats.candidates_constructed << "\n";
  out << "    candidates_pruned_before_construction: "
      << enumeration.stats.candidates_pruned_before_construction << "\n";
  out << "    candidates_pruned_after_construction: "
      << enumeration.stats.candidates_pruned_after_construction << "\n";
  out << "  counters:\n";
  print_chart_spr_search_counter_fields(out, counters, "    ");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) try {
  auto a = parse_args(argc, argv);

  // ---- Load all inputs (in parallel) ----
  std::string refseq;
  if (!a.refseq.empty()) refseq = read_refseq(a.refseq);

  auto total_inputs = a.dag_pbs.size() + a.tree_pbs.size() + a.fastas.size();
  std::vector<phylo_dag> dags(total_inputs);
  {
    auto& pool = thread_pool::get_default();
    std::vector<std::size_t> indices(total_inputs);
    std::iota(indices.begin(), indices.end(), std::size_t{0});

    std::cerr << "Loading " << total_inputs << " input(s)...\n";
    parallel_for_each(pool, indices, [&](std::size_t idx) {
      auto dag_pb_count = a.dag_pbs.size();
      auto tree_pb_count = a.tree_pbs.size();
      if (idx < dag_pb_count) {
        dags[idx] = load_proto_dag(a.dag_pbs[idx]);
      } else if (idx < dag_pb_count + tree_pb_count) {
        auto ti = idx - dag_pb_count;
        dags[idx] = load_parsimony_tree(a.tree_pbs[ti], refseq);
      } else {
        auto fi = idx - dag_pb_count - tree_pb_count;
        dags[idx] =
            build_from_fasta_newick(a.fastas[fi], a.newicks[fi], refseq);
      }
    });
    std::cerr << "Loading done.\n";
  }

  // ---- Apply VCF (in parallel) ----
  if (!a.vcf.empty()) {
    auto& pool = thread_pool::get_default();
    std::vector<std::size_t> indices(dags.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    parallel_for_each(pool, indices, [&](std::size_t idx) {
      auto const& ref = get_reference_sequence(dags[idx]);
      auto vcf = read_vcf(a.vcf, ref);
      apply_vcf_to_dag(dags[idx], vcf);
    });
  }

  // ---- Merge ----
  auto const& ref = get_reference_sequence(dags.front());
  merge m{ref};
  for (auto& dag : dags) m.add_dag(dag);

  auto& result = m.get_result();
  auto root_idx = get_root_idx(result);

  std::cout << "leaves: " << leaf_count(result) << "\n";
  std::cout << "nodes: " << node_count(result) << "\n";
  std::cout << "edges: " << edge_count(result) << "\n";

  if (a.validate)
    validate_dag(result, "after merge", thread_pool::get_default());

  std::unique_ptr<ml_model> ml_model_storage;
  if (ml_model_requested(a)) {
    assert(has_complete_model_args(a) &&
           "parse_args must require model args before ML loading");
    std::cerr << "Loading ML model " << a.model_name << "...\n";
    ml_model_storage =
        std::make_unique<ml_model>(load_ml_model(a.model_dir, a.model_name));
    std::cerr << "  ML model loaded\n";
  }

  // ---- Analysis ----
  std::optional<polytomy_refinement_result> chart_refinement_cache;
  double chart_grammar_build_ms = 0.0;
  auto get_chart_refinement = [&]() -> polytomy_refinement_result& {
    if (!chart_refinement_cache) {
      auto start = std::chrono::steady_clock::now();
      clade_grammar_options grammar_opts;
      chart_refinement_cache = build_polytomy_refined_clade_grammar(
          result, grammar_opts, a.wric_polytomy_opts);
      chart_grammar_build_ms =
          elapsed_ms(start, std::chrono::steady_clock::now());
      require_polytomy_refinement_binary_charting(
          chart_refinement_cache->audit,
          "WRIC chart diagnostics (use --wric-polytomy-mode expand-exact "
          "or expand-bounded for soft polytomies)");
    }
    return *chart_refinement_cache;
  };
  auto get_chart_grammar = [&]() -> clade_grammar& {
    return get_chart_refinement().grammar;
  };

  std::optional<site_pattern_set> exact_patterns_cache;
  double exact_pattern_build_ms = 0.0;
  auto get_exact_patterns = [&]() -> site_pattern_set& {
    if (!exact_patterns_cache) {
      auto& grammar = get_chart_grammar();
      site_pattern_options pattern_opts;
      auto start = std::chrono::steady_clock::now();
      exact_patterns_cache = build_site_patterns(result, grammar, pattern_opts);
      exact_pattern_build_ms = elapsed_ms(start, std::chrono::steady_clock::now());
    }
    return *exact_patterns_cache;
  };

  if (a.wric_audit || a.wric_polytomy_report) {
    clade_grammar_options grammar_opts;
    auto refinement_opts = a.wric_polytomy_opts;
    if (!a.wric_polytomy_mode_explicit)
      refinement_opts.mode = polytomy_mode::audit_kary;
    auto start = std::chrono::steady_clock::now();
    auto refined = build_polytomy_refined_clade_grammar(
        result, grammar_opts, refinement_opts);
    auto build_ms = elapsed_ms(start, std::chrono::steady_clock::now());
    print_clade_grammar_audit(std::cout, refined.source_grammar_audit);
    print_wric_polytomy_audit_fields(std::cout, refined,
                                     refinement_opts.mode);
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << build_ms << "\n";
  }

  if (a.wric_benchmark || a.wric_polytomy_benchmark) {
    run_wric_polytomy_benchmark(std::cout, result, a);
  }

  if (a.chart_spr_helper_benchmark) {
    auto& refinement = get_chart_refinement();
    auto& patterns = get_exact_patterns();
    run_chart_spr_helper_benchmark(std::cout, refinement,
                                   chart_grammar_build_ms, patterns,
                                   exact_pattern_build_ms, a);
  }

  if (a.chart_spr_candidates) {
    auto& refinement = get_chart_refinement();
    run_chart_spr_candidate_diagnostic(std::cout, result, refinement, a);
  }

  if (a.chart_spr_score_local) {
    auto& refinement = get_chart_refinement();
    run_chart_spr_local_scoring_diagnostic(
        std::cout, result, refinement, a, "chart_spr_score_local");
  }

  if (a.chart_spr_search) {
    auto& refinement = get_chart_refinement();
    run_chart_spr_search_diagnostic(
        std::cout, result, refinement, a);
    root_idx = get_root_idx(result);
    chart_refinement_cache.reset();
    exact_patterns_cache.reset();
    chart_grammar_build_ms = 0.0;
    exact_pattern_build_ms = 0.0;
  }

  if (a.chart_site) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;

    auto state_start = std::chrono::steady_clock::now();
    auto states = extract_leaf_site_states(result, grammar, *a.chart_site);
    auto state_ms = elapsed_ms(state_start, std::chrono::steady_clock::now());

    auto chart_start = std::chrono::steady_clock::now();
    auto chart = build_single_site_chart(grammar, states, chart_opts);
    auto chart_ms = elapsed_ms(chart_start, std::chrono::steady_clock::now());

    auto reference_state = extract_reference_site_state(result, *a.chart_site);
    auto optimum = root_min(chart, grammar.root_clade, chart_opts,
                            reference_state);

    std::cout << "chart_site:\n";
    std::cout << "  pos: " << *a.chart_site << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    std::cout << "  reference_state: " << chart_state_label(reference_state)
              << "\n";
    std::cout << "  taxa: " << grammar.taxa.id_to_sample_id.size() << "\n";
    std::cout << "  clades: " << grammar.clades.size() << "\n";
    std::cout << "  productions: " << grammar.productions.size() << "\n";
    std::cout << "  root_clade: " << grammar.root_clade << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement,
                                      a.wric_polytomy_opts.mode,
                                      a.wric_polytomy_report);
    std::cout << "  root_min: ";
    print_chart_cost(std::cout, optimum);
    std::cout << "\n";
    std::cout << "  root_inside: ";
    print_chart_row(std::cout, chart.inside[grammar.root_clade]);
    std::cout << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  leaf_state_extract_ms: " << std::fixed
              << std::setprecision(3) << state_ms << "\n";
    std::cout << "  chart_build_ms: " << std::fixed << std::setprecision(3)
              << chart_ms << "\n";
    print_key_chart_clade_entries(std::cout, grammar, chart,
                                  a.chart_entry_limit);
  }

  if (a.chart_pattern_info) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    site_pattern_options pattern_opts;
    pattern_opts.build_normalized_binary_patterns = true;
    auto pattern_start = std::chrono::steady_clock::now();
    auto patterns = build_site_patterns(result, grammar, pattern_opts);
    auto pattern_ms = elapsed_ms(pattern_start, std::chrono::steady_clock::now());
    print_site_pattern_summary(std::cout, patterns);
    print_wric_polytomy_score_fields(std::cout, refinement,
                                      a.wric_polytomy_opts.mode,
                                      a.wric_polytomy_report);
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  pattern_build_ms: " << std::fixed
              << std::setprecision(3) << pattern_ms << "\n";
  }

  if (a.chart_trim_site) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;

    auto state_start = std::chrono::steady_clock::now();
    auto states = extract_leaf_site_states(result, grammar, *a.chart_trim_site);
    auto state_ms = elapsed_ms(state_start, std::chrono::steady_clock::now());

    auto chart_start = std::chrono::steady_clock::now();
    auto chart = build_single_site_chart(grammar, states, chart_opts);
    auto chart_ms = elapsed_ms(chart_start, std::chrono::steady_clock::now());

    auto outside_start = std::chrono::steady_clock::now();
    auto outside = build_single_site_outside_chart(
        grammar, chart, chart_opts, result, *a.chart_trim_site);
    auto outside_ms = elapsed_ms(outside_start, std::chrono::steady_clock::now());

    chart_trim_options trim_opts;
    trim_opts.store_optimal_choices = true;
    auto trim_start = std::chrono::steady_clock::now();
    auto mask = build_single_site_trim_mask(grammar, chart, outside, trim_opts);
    auto trim_ms = elapsed_ms(trim_start, std::chrono::steady_clock::now());

    std::cout << "chart_trim_site:\n";
    std::cout << "  pos: " << *a.chart_trim_site << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement,
                                      a.wric_polytomy_opts.mode,
                                      a.wric_polytomy_report);
    std::cout << "  global_min: ";
    print_chart_cost(std::cout, mask.global_min);
    std::cout << "\n";
    std::cout << "  kept_productions: " << count_true(mask.keep_production)
              << "\n";
    std::cout << "  total_productions: " << grammar.productions.size()
              << "\n";
    std::cout << "  kept_clade_states: "
              << count_kept_clade_states(mask) << "\n";
    std::cout << "  total_clade_states: "
              << grammar.clades.size() * nuc_state_count << "\n";
    std::cout << "  kept_production_choice_count: "
              << mask.kept_production_choice_count << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  leaf_state_extract_ms: " << std::fixed
              << std::setprecision(3) << state_ms << "\n";
    std::cout << "  chart_build_ms: " << std::fixed << std::setprecision(3)
              << chart_ms << "\n";
    std::cout << "  outside_build_ms: " << std::fixed << std::setprecision(3)
              << outside_ms << "\n";
    std::cout << "  trim_mask_build_ms: " << std::fixed
              << std::setprecision(3) << trim_ms << "\n";
    std::cout << "  kept_productions_by_parent_clade:\n";
    std::size_t printed = 0;
    std::size_t eligible = 0;
    for (clade_id cid = 0; cid < grammar.productions_by_parent.size(); ++cid) {
      std::size_t kept = 0;
      for (auto pid : grammar.productions_by_parent[cid])
        if (mask.keep_production[pid]) ++kept;
      if (kept == 0) continue;
      ++eligible;
      if (!within_limit(printed, a.chart_entry_limit)) continue;
      std::cout << "    " << cid << ": " << kept << "/"
                << grammar.productions_by_parent[cid].size() << "\n";
      ++printed;
    }
    std::cout << "  kept_parent_clades_printed: " << printed << "\n";
    if (a.chart_entry_limit != 0 && printed < eligible)
      std::cout << "  kept_parent_clades_truncated: true\n";
  }

  if (a.chart_composite_score) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    auto& patterns = get_exact_patterns();
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;

    auto composite_start = std::chrono::steady_clock::now();
    auto composite = build_composite_chart_score(grammar, patterns, chart_opts);
    auto composite_ms = elapsed_ms(composite_start,
                                   std::chrono::steady_clock::now());

    std::cout << "chart_composite_score:\n";
    std::cout << "  score_kind: LOWER_BOUND\n";
    print_wric_polytomy_score_fields(std::cout, refinement,
                                      a.wric_polytomy_opts.mode,
                                      a.wric_polytomy_report);
    std::cout << "  weighted_lower_bound: "
              << composite.weighted_lower_bound << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    std::cout << "  exact_patterns: " << patterns.patterns.size() << "\n";
    std::cout << "  total_sites: " << patterns.total_site_count << "\n";
    std::cout << "  invariant_sites: " << patterns.invariant_site_count
              << "\n";
    std::cout << "  skipped_invariant_sites: "
              << patterns.skipped_invariant_site_count << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  pattern_build_ms: " << std::fixed
              << std::setprecision(3) << exact_pattern_build_ms << "\n";
    std::cout << "  composite_chart_ms: " << std::fixed
              << std::setprecision(3) << composite_ms << "\n";
    print_limited_per_pattern_roots(std::cout, patterns, composite,
                                    a.chart_entry_limit);
  }

  if (a.chart_bnb_trim) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    auto& patterns = get_exact_patterns();
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;
    auto trim_opts = make_chart_bnb_trim_options(a);

    auto bnb_start = std::chrono::steady_clock::now();
    auto trim = build_multisite_trim(grammar, patterns, chart_opts, trim_opts);
    auto bnb_ms = elapsed_ms(bnb_start, std::chrono::steady_clock::now());

    std::cout << "chart_bnb_trim:\n";
    std::cout << "  score_kind: "
              << exact_or_bounded_score_kind(refinement.audit) << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement,
                                      a.wric_polytomy_opts.mode,
                                      a.wric_polytomy_report);
    std::cout << "  optimum: " << trim.optimum << "\n";
    std::cout << "  composite_lower_bound_kind: LOWER_BOUND\n";
    std::cout << "  composite_lower_bound: "
              << trim.composite_lower_bound << "\n";
    std::cout << "  initial_upper_bound: " << trim.initial_upper_bound
              << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    std::cout << "  active_patterns: " << trim.active_pattern_count << "\n";
    std::cout << "  exact_patterns: " << patterns.patterns.size() << "\n";
    std::cout << "  invariant_constant_offset: "
              << trim.invariant_constant_offset << "\n";
    std::cout << "  kept_productions: " << count_true(trim.keep_production)
              << "\n";
    std::cout << "  kept_productions_count_exact: "
              << (trim.keep_production_exact ? "true" : "false") << "\n";
    std::cout << "  total_productions: " << grammar.productions.size()
              << "\n";
    std::cout << "  equality_deduplicated: "
              << trim.equality_deduplicated << "\n";
    std::cout << "  dominance_mode: "
              << multisite_dominance_mode_name(trim.dominance_mode) << "\n";
    std::cout << "  keep_mask_kind: "
              << multisite_keep_mask_kind_name(trim.keep_mask_kind) << "\n";
    std::cout << "  keep_production_exact: "
              << (trim.keep_production_exact ? "true" : "false") << "\n";
    std::cout << "  dominance_candidates_considered: "
              << trim.dominance_candidates_considered << "\n";
    std::cout << "  dominance_pruned_score_pass: "
              << trim.dominance_pruned_score_pass << "\n";
    std::cout << "  dominance_pruned_mask_pass: "
              << trim.dominance_pruned_mask_pass << "\n";
    std::cout << "  dominance_pruned: " << trim.dominance_pruned << "\n";
    std::cout << "  exact_mask_recovery_passes: "
              << trim.exact_mask_recovery_passes << "\n";
    std::cout << "  bound_pruned: " << trim.bound_pruned << "\n";
    std::cout << "  bound_pruning: "
              << (trim_opts.use_bound_pruning ? "true" : "false") << "\n";
    std::cout << "  upper_bound_override: "
              << (trim_opts.upper_bound_override
                      ? std::to_string(*trim_opts.upper_bound_override)
                      : std::string{"none"})
              << "\n";
    std::cout << "  upper_bound_override_kind: PRUNING_ONLY\n";
    std::cout << "  max_frontier_entries_per_clade: "
              << trim_opts.max_frontier_entries_per_clade << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  pattern_build_ms: " << std::fixed
              << std::setprecision(3) << exact_pattern_build_ms << "\n";
    std::cout << "  bnb_trim_ms: " << std::fixed << std::setprecision(3)
              << bnb_ms << "\n";
    print_frontier_size_stats(std::cout, trim.frontier_sizes_by_clade);

    if (a.chart_bnb_apply_trim) {
      auto apply_opts = make_chart_bnb_trim_apply_options(a);
      auto apply_start = std::chrono::steady_clock::now();
      auto apply = apply_chart_bnb_trim(result, refinement, patterns,
                                        chart_opts, trim, apply_opts);
      auto apply_ms = elapsed_ms(apply_start,
                                 std::chrono::steady_clock::now());
      std::cout << "  apply_trim_ms: " << std::fixed << std::setprecision(3)
                << apply_ms << "\n";
      print_chart_bnb_apply_result(std::cout, apply, "  ");

      auto output_path = !a.chart_bnb_output_dag.empty()
                             ? a.chart_bnb_output_dag
                             : a.output;
      if (!output_path.empty()) {
        if (apply.output_dag_available) {
          save_proto_dag(apply.dag, output_path);
          std::cout << "  output_dag: " << output_path << "\n";
        } else if (apply.mode ==
                   chart_bnb_trim_application_mode::annotated_optimal_trim) {
          write_chart_bnb_apply_json(output_path, trim, apply);
          std::cout << "  output_artifact: " << output_path << "\n";
        } else {
          throw std::runtime_error(
              "chart B&B trim application '" +
              std::string{chart_bnb_trim_application_mode_name(apply.mode)} +
              "' produces a " + apply.output_artifact_kind +
              ", not an ordinary protobuf DAG and has no CLI serializer; "
              "omit -o/--chart-bnb-output-dag or choose "
              "production-mask/optimal-topology-materialize");
        }
      }
      if (!a.chart_bnb_report_json.empty()) {
        write_chart_bnb_apply_json(a.chart_bnb_report_json, trim, apply);
        std::cout << "  report_json: " << a.chart_bnb_report_json << "\n";
      }
    }
  }

  if (a.chart_fluidity_site) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    auto states =
        extract_leaf_site_states(result, grammar, *a.chart_fluidity_site);
    chart_options chart_opts;
    chart_opts.keep_trace = true;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;
    auto chart = build_single_site_chart(grammar, states, chart_opts);
    auto outside = build_single_site_outside_chart(
        grammar, chart, chart_opts, result, *a.chart_fluidity_site);
    auto report = build_single_site_fluidity_report(grammar, chart, outside);
    std::cout << "chart_fluidity_site: " << *a.chart_fluidity_site << "\n";
    std::cout << "chart_fluidity_score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement,
                                      a.wric_polytomy_opts.mode,
                                      a.wric_polytomy_report);
    print_fluidity_report(std::cout, grammar, report);
  }

  if (a.dag_info) {
    tree_count_ops tc_ops;
    subtree_weight<tree_count_ops> tc_sw(result, a.seed);
    auto tree_count = tc_sw.compute_weight_below(root_idx, tc_ops);
    std::cout << "tree_count: " << tree_count << "\n";

    if (a.print_parsimony) {
      parsimony_score_ops pops;
      weight_accumulator<parsimony_score_ops> wa_pops(pops);
      subtree_weight<weight_accumulator<parsimony_score_ops>> psw(result,
                                                                  a.seed);
      auto all_parsimony = psw.compute_weight_below(root_idx, wa_pops);
      auto const& pweights = all_parsimony.get_weights();

      std::cout << "parsimony_all: " << pweights.size() << "\n"
                << all_parsimony << "\n";
      if (!pweights.empty()) {
        auto min_it = pweights.begin();
        auto max_it = std::prev(pweights.end());
        std::cout << "parsimony_min: score:" << min_it->first
                  << ", count:" << min_it->second << "\n";
        std::cout << "parsimony_max: score:" << max_it->first
                  << ", count:" << max_it->second << "\n";
      }
    }

    if (a.print_rf_distance) {
      sum_rf_distance_ops rf_ops{m, m};
      sum_rf_distance srf(rf_ops);
      weight_accumulator<sum_rf_distance> wa_rf(srf);
      subtree_weight<weight_accumulator<sum_rf_distance>> rf_sw(result, a.seed);
      auto all_rf = rf_sw.compute_weight_below(root_idx, wa_rf);
      auto const& rf_weights = all_rf.get_weights();

      auto shift_sum = srf.get_ops().get_shift_sum();

      // Shift scores and rebuild map
      using rf_wc = weight_counter<sum_rf_distance>;
      rf_wc::map_type shifted;
      for (auto const& [score, count] : rf_weights)
        shifted[score + shift_sum] += count;
      rf_wc shifted_wc(std::move(shifted));

      std::cout << "sum_rf_dist_all: " << shifted_wc.get_weights().size()
                << "\n"
                << shifted_wc << "\n";
      if (!shifted_wc.get_weights().empty()) {
        auto min_it = shifted_wc.get_weights().begin();
        auto max_it = std::prev(shifted_wc.get_weights().end());
        std::cout << "sum_rf_dist_min: score:" << min_it->first
                  << ", count:" << min_it->second << "\n";
        std::cout << "sum_rf_dist_max: score:" << max_it->first
                  << ", count:" << max_it->second << "\n";
      }
    }
  }

  // ---- Per-edge global penalties ----
  std::vector<float> edge_penalties;
  if (a.edge_parsimony) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
    auto global_min = sw.compute_weight_below(root_idx, pops);
    auto scores = sw.compute_edge_min_global_scores(pops);

    // Convert to penalties (float), preserving edge-index addressing for
    // save_proto_dag() while reporting counts over live edges only.
    edge_penalties.assign(scores.size(), 0.0f);
    std::size_t zero_penalty = 0;
    std::size_t nonzero_penalty = 0;
    std::size_t max_penalty = 0;
    for (auto ev : result.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            auto i = edge.index();
            assert(scores[i] >= global_min);
            auto penalty = scores[i] - global_min;
            edge_penalties[i] = static_cast<float>(penalty);
            if (penalty == 0)
              ++zero_penalty;
            else
              ++nonzero_penalty;
            if (penalty > max_penalty) max_penalty = penalty;
          },
          ev);
    }

    std::cerr << "edge_parsimony: global_min=" << global_min
              << " edges=" << edge_count(result)
              << " zero_penalty=" << zero_penalty
              << " nonzero_penalty=" << nonzero_penalty
              << " max_penalty=" << max_penalty << "\n";
  }
  if (a.edge_ml) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();
    assert(ml_model_storage != nullptr);

    auto const& ml_ref = get_reference_sequence(result);
    ml_model_likelihood_score_ops ml_ops{.model = *ml_model_storage,
                                         .reference = ml_ref,
                                         .ignore_ua_edge =
                                             a.ignore_ua_edge_ml};
    subtree_weight<ml_model_likelihood_score_ops> sw(result, a.seed, mr);
    auto global_min = sw.compute_weight_below(root_idx, ml_ops);
    if (!std::isfinite(global_min)) {
      std::cerr << "error: non-finite edge-ML global minimum\n";
      std::exit(1);
    }
    auto scores = sw.compute_edge_min_global_scores(ml_ops);

    // Convert to penalties (float).  Treat tiny negative/positive deviations
    // around zero as floating-point noise so globally optimal edges round to 0.
    double eps = 1e-8 * std::max(1.0, std::abs(global_min));
    edge_penalties.assign(scores.size(), 0.0f);
    std::size_t zero_penalty = 0;
    std::size_t nonzero_penalty = 0;
    double max_penalty = 0.0;
    for (auto ev : result.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            auto i = edge.index();
            if (!std::isfinite(scores[i])) {
              std::cerr << "error: non-finite edge-ML score at edge " << i
                        << "\n";
              std::exit(1);
            }
            double penalty = scores[i] - global_min;
            if (penalty < 0.0 && penalty >= -eps) penalty = 0.0;
            if (penalty < 0.0) {
              std::cerr << "error: edge-ML score below global minimum at edge "
                        << i << " (score=" << scores[i]
                        << ", global_min=" << global_min << ")\n";
              std::exit(1);
            }
            if (penalty <= eps) penalty = 0.0;
            edge_penalties[i] = static_cast<float>(penalty);
            if (penalty == 0.0)
              ++zero_penalty;
            else
              ++nonzero_penalty;
            if (penalty > max_penalty) max_penalty = penalty;
          },
          ev);
    }

    std::cerr << "edge_ml: global_min=" << std::fixed << std::setprecision(6)
              << global_min << " edges=" << edge_count(result)
              << " zero_penalty=" << zero_penalty
              << " nonzero_penalty=" << nonzero_penalty
              << " max_penalty=" << max_penalty
              << (a.ignore_ua_edge_ml ? " (UA edge ignored)"
                                      : " (UA edge scored)")
              << "\n";
  }

  // ---- Output ----
  if (!a.output.empty() && !a.chart_bnb_apply_trim) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();

    if ((a.edge_parsimony || a.edge_ml) && !a.trim && !a.sample) {
      save_proto_dag(result, a.output, edge_penalties);
    } else if (a.trim) {
      if (a.rf.empty()) {
        // Trim to best parsimony
        parsimony_score_ops pops;
        subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
        sw.compute_weight_below(root_idx, pops);
        if (a.sample) {
          std::cerr << "Sampling a tree from min-parsimony options...\n";
          auto tree = a.sample_uniformly ? sw.min_weight_uniform_sample_tree(pops)
                                         : sw.min_weight_sample_tree(pops);
          save_proto_dag(tree, a.output);
        } else {
          std::cerr << "Trimming DAG to min parsimony...\n";
          auto trimmed = sw.trim_to_min_weight(pops);
          save_proto_dag(trimmed, a.output);
        }
      } else {
        // Trim to minimize RF distance to provided DAG
        auto rf_dag = load_proto_dag(a.rf);
        merge rf_m{get_reference_sequence(rf_dag)};
        rf_m.add_dag(rf_dag);

        sum_rf_distance_ops rf_ops{rf_m, m};
        sum_rf_distance srf(rf_ops);
        subtree_weight<sum_rf_distance> sw(result, a.seed, mr);
        sw.compute_weight_below(root_idx, srf);
        if (a.sample) {
          std::cerr << "Sampling a tree from min-RF options...\n";
          auto tree = a.sample_uniformly ? sw.min_weight_uniform_sample_tree(srf)
                                         : sw.min_weight_sample_tree(srf);
          save_proto_dag(tree, a.output);
        } else {
          std::cerr << "Trimming DAG to min RF distance...\n";
          auto trimmed = sw.trim_to_min_weight(srf);
          save_proto_dag(trimmed, a.output);
        }
      }
    } else if (a.sample) {
      switch (a.sampling_method) {
        case sample_method::random: {
          std::cerr << "Sampling a random tree from the DAG...\n";
          parsimony_score_ops pops;
          subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
          sw.compute_weight_below(root_idx, pops);
          auto tree = a.sample_uniformly ? sw.uniform_sample_tree(pops)
                                         : sw.sample_tree(pops);
          if (a.validate)
            validate_dag(tree, "sampled random tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::parsimony: {
          std::cerr << "Sampling a tree from min-parsimony options...\n";
          parsimony_score_ops pops;
          subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
          auto min_score = sw.compute_weight_below(root_idx, pops);
          auto tree = a.sample_uniformly
                          ? sw.min_weight_uniform_sample_tree(pops)
                          : sw.min_weight_sample_tree(pops);
          std::cerr << "sampled_tree: parsimony_min=" << min_score << "\n";
          if (a.validate)
            validate_dag(tree, "sampled min-parsimony tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::edge_weight: {
          std::cerr << "Sampling a tree from min-edge-weight options...\n";
          edge_weight_score_ops ew_ops;
          subtree_weight<edge_weight_score_ops> sw(result, a.seed, mr);
          auto min_score = sw.compute_weight_below(root_idx, ew_ops);
          auto tree = a.sample_uniformly
                          ? sw.min_weight_uniform_sample_tree(ew_ops)
                          : sw.min_weight_sample_tree(ew_ops);
          std::cerr << "sampled_tree: edge_weight_min=" << std::fixed
                    << std::setprecision(6) << min_score << "\n";
          if (a.validate)
            validate_dag(tree, "sampled min-edge-weight tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::ml: {
          assert(ml_model_storage != nullptr);
          std::cerr << "Sampling a tree from min-ML options...\n";
          auto const& ml_ref = get_reference_sequence(result);
          ml_model_likelihood_score_ops ml_ops{.model = *ml_model_storage,
                                               .reference = ml_ref,
                                               .ignore_ua_edge =
                                                   a.ignore_ua_edge_ml};
          subtree_weight<ml_model_likelihood_score_ops> sw(result, a.seed, mr);
          auto ml_min = sw.compute_weight_below(root_idx, ml_ops);
          if (!std::isfinite(ml_min)) {
            std::cerr << "error: non-finite ML sample score\n";
            std::exit(1);
          }
          auto tree = a.sample_uniformly
                          ? sw.min_weight_uniform_sample_tree(ml_ops)
                          : sw.min_weight_sample_tree(ml_ops);
          std::cerr << "sampled_tree: ML_NLL=" << std::fixed
                    << std::setprecision(6) << ml_min
                    << (a.ignore_ua_edge_ml ? " (UA edge ignored)"
                                            : " (UA edge scored)")
                    << "\n";
          if (a.validate)
            validate_dag(tree, "sampled min-ML tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::rf_minsum:
        case sample_method::rf_maxsum:
          std::cerr << "error: --sample-method "
                    << format_sample_method(a.sampling_method)
                    << " is not supported by dagutil sampling\n";
          std::exit(1);
      }
    } else {
      save_proto_dag(result, a.output);
    }
  }

  return EXIT_SUCCESS;
} catch (std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return EXIT_FAILURE;
}
