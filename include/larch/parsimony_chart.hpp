#pragma once

#include <larch/chart_execution_plan.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/compute.hpp>
#include <larch/compact_genome.hpp>
#include <larch/phylo_dag.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch {

using chart_cost = std::uint32_t;
inline constexpr chart_cost chart_inf =
    std::numeric_limits<chart_cost>::max() / 4;
inline constexpr std::size_t nuc_state_count = 4;

struct chart_choice {
  production_id production = no_production;
  // The trace/choice layer is binary-gated. The entry is aligned with
  // grammar_production::children for a binary chosen production.
  std::array<std::uint8_t, 2> child_states{0, 0};
  chart_cost cost = chart_inf;
};

struct single_site_chart {
  std::vector<std::array<chart_cost, nuc_state_count>> inside;

  // Empty when build_single_site_chart(..., keep_trace=false). When present,
  // optimal_choices[clade][state] stores every binary production / child-state
  // pair that ties for inside[clade][state].
  std::vector<std::array<std::vector<chart_choice>, nuc_state_count>>
      optimal_choices;

  // Number of stored chart_choice records. Zero on the no-trace fast path.
  std::size_t trace_choice_count = 0;

  // Number of non-binary production rows evaluated by this chart build.
  std::size_t multifurcation_productions_scored = 0;

  [[nodiscard]] bool has_trace() const { return !optimal_choices.empty(); }

  [[nodiscard]] chart_cost root_min_excluding_ua(clade_id root) const {
    if (root == no_clade || root >= inside.size())
      throw std::runtime_error("single-site chart: root clade out of range");

    chart_cost best = chart_inf;
    for (auto cost : inside[root]) best = std::min(best, cost);
    return best;
  }

  [[nodiscard]] chart_cost root_min_with_reference_edge(
      clade_id root, std::uint8_t reference_state) const;
};

struct leaf_site_states {
  // taxon_id -> 0..3 (A,C,G,T)
  std::vector<std::uint8_t> state_by_taxon;
};

// Immutable, non-owning leaf-state input for allocation-sensitive chart
// kernels.  The owning leaf_site_states API remains the standalone/public
// default; callers use this view only while the backing pattern/state vector is
// alive and immutable.
struct leaf_site_states_view {
  std::span<std::uint8_t const> state_by_taxon;
};

inline leaf_site_states_view view_leaf_site_states(
    leaf_site_states const& states) noexcept {
  return {std::span<std::uint8_t const>{states.state_by_taxon}};
}

inline leaf_site_states_view view_leaf_site_states(
    std::vector<std::uint8_t> const& states) noexcept {
  return {std::span<std::uint8_t const>{states}};
}

leaf_site_states_view view_leaf_site_states(leaf_site_states&&) = delete;
leaf_site_states_view view_leaf_site_states(std::vector<std::uint8_t>&&) =
    delete;
leaf_site_states_view view_leaf_site_states(leaf_site_states const&&) = delete;
leaf_site_states_view view_leaf_site_states(
    std::vector<std::uint8_t> const&&) = delete;

struct chart_options {
  bool keep_trace = false;
  // Interpreted by callers when selecting the root convention; chart
  // construction itself always computes the UA-free inside recurrence.
  bool score_ua_edge = false;
  // 0 means unlimited. If non-zero and keep_trace=true, construction throws
  // before exceeding this many stored chart_choice records.
  std::size_t max_trace_choices = 0;
};

enum class arity_gate_consumer : std::size_t {
  single_site_chart_trace,
  single_site_trim_mask,
  multisite_trim,
  single_site_fluidity_report,
  multisite_plateau_report,
  chart_spr_exact_multisite,
  count,
};

inline constexpr std::size_t arity_gate_consumer_count =
    static_cast<std::size_t>(arity_gate_consumer::count);

struct arity_gate_throw_snapshot {
  std::size_t total = 0;
  std::array<std::size_t, arity_gate_consumer_count> by_consumer{};

  [[nodiscard]] std::size_t count(arity_gate_consumer consumer) const {
    return by_consumer[static_cast<std::size_t>(consumer)];
  }
};

inline std::size_t clade_grammar_max_production_arity(
    clade_grammar const& grammar) {
  std::size_t max_arity = 0;
  for (auto const& prod : grammar.productions) {
    max_arity = std::max(max_arity, prod.children.size());
  }
  return max_arity;
}

inline std::optional<production_id> first_multifurcating_production(
    clade_grammar const& grammar) {
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (grammar.productions[pid].children.size() > 2) {
      return static_cast<production_id>(pid);
    }
  }
  return std::nullopt;
}

namespace parsimony_chart_detail {

// Optional thread-local instrumentation for proving that trusted candidate
// pattern recurrences perform no structural validation or ordering work.  The
// observer is installed only around that hot region; ordinary chart callers
// pay one predictable null branch at validation/sort boundaries, never in the
// arithmetic recurrence.
struct structural_work_observer {
  std::size_t* full_grammar_validations = nullptr;
  std::size_t* production_partition_validations = nullptr;
  std::size_t* clade_order_sorts = nullptr;
};

inline thread_local structural_work_observer* active_structural_work_observer =
    nullptr;

class structural_work_observer_scope {
 public:
  explicit structural_work_observer_scope(structural_work_observer* observer)
      : previous_(active_structural_work_observer) {
    active_structural_work_observer = observer;
  }
  structural_work_observer_scope(structural_work_observer_scope const&) =
      delete;
  structural_work_observer_scope& operator=(
      structural_work_observer_scope const&) = delete;
  ~structural_work_observer_scope() {
    active_structural_work_observer = previous_;
  }

 private:
  structural_work_observer* previous_ = nullptr;
};

inline void record_full_grammar_validation() {
  if (active_structural_work_observer != nullptr &&
      active_structural_work_observer->full_grammar_validations != nullptr) {
    ++*active_structural_work_observer->full_grammar_validations;
  }
}

inline void record_production_partition_validation() {
  if (active_structural_work_observer != nullptr &&
      active_structural_work_observer->production_partition_validations !=
          nullptr) {
    ++*active_structural_work_observer->production_partition_validations;
  }
}

inline void record_clade_order_sort() {
  if (active_structural_work_observer != nullptr &&
      active_structural_work_observer->clade_order_sorts != nullptr) {
    ++*active_structural_work_observer->clade_order_sorts;
  }
}

inline std::array<std::atomic<std::size_t>, arity_gate_consumer_count>
    arity_gate_throw_counts{};

inline std::string_view arity_gate_consumer_name(
    arity_gate_consumer consumer) {
  switch (consumer) {
    case arity_gate_consumer::single_site_chart_trace:
      return "single_site_chart_trace";
    case arity_gate_consumer::single_site_trim_mask:
      return "single_site_trim_mask";
    case arity_gate_consumer::multisite_trim:
      return "multisite_trim";
    case arity_gate_consumer::single_site_fluidity_report:
      return "single_site_fluidity_report";
    case arity_gate_consumer::multisite_plateau_report:
      return "multisite_plateau_report";
    case arity_gate_consumer::chart_spr_exact_multisite:
      return "chart_spr_exact_multisite";
    case arity_gate_consumer::count:
      break;
  }
  return "unknown";
}

inline std::string_view arity_gate_consumer_reason(
    arity_gate_consumer consumer) {
  switch (consumer) {
    case arity_gate_consumer::single_site_chart_trace:
      return "binary trace layer";
    case arity_gate_consumer::single_site_trim_mask:
      return "binary choice-layer trim mask";
    case arity_gate_consumer::multisite_trim:
      return "binary B&B frontier";
    case arity_gate_consumer::single_site_fluidity_report:
      return "binary choice-layer fluidity report";
    case arity_gate_consumer::multisite_plateau_report:
      return "binary choice-layer plateau report";
    case arity_gate_consumer::chart_spr_exact_multisite:
      return "exact_multisite verifier";
    case arity_gate_consumer::count:
      break;
  }
  return "unknown";
}

inline void record_arity_gate_throw(arity_gate_consumer consumer) {
  auto index = static_cast<std::size_t>(consumer);
  if (index >= arity_gate_throw_counts.size()) return;
  arity_gate_throw_counts[index].fetch_add(1, std::memory_order_relaxed);
}

inline void reset_arity_gate_throw_counters_for_tests() {
  for (auto& count : arity_gate_throw_counts) {
    count.store(0, std::memory_order_relaxed);
  }
}

inline arity_gate_throw_snapshot arity_gate_throws_snapshot() {
  arity_gate_throw_snapshot snapshot;
  for (std::size_t i = 0; i < arity_gate_throw_counts.size(); ++i) {
    snapshot.by_consumer[i] =
        arity_gate_throw_counts[i].load(std::memory_order_relaxed);
    snapshot.total += snapshot.by_consumer[i];
  }
  return snapshot;
}

inline std::uint8_t strict_decode_acgt_state(char c) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base::A;
    case 'C':
    case 'c':
      return nuc_base::C;
    case 'G':
    case 'g':
      return nuc_base::G;
    case 'T':
    case 't':
      return nuc_base::T;
    default:
      throw std::runtime_error(std::string{"single-site chart: non-ACGT "
                                           "reference nucleotide '"} +
                               c + "'");
  }
}

inline void require_no_multifurcating_productions_for_consumer(
    clade_grammar const& grammar, arity_gate_consumer consumer,
    std::string_view context, std::string_view layer,
    std::string_view resolution) {
  auto first = first_multifurcating_production(grammar);
  if (!first) return;
  record_arity_gate_throw(consumer);
  auto const& prod = grammar.productions[*first];
  throw std::runtime_error(
      std::string{context} +
      ": WI6 arity gate: the chart supports multifurcations; this consumer's " +
      std::string{layer} + " does not (production " +
      std::to_string(*first) + " has arity " +
      std::to_string(prod.children.size()) + ", grammar max arity " +
      std::to_string(clade_grammar_max_production_arity(grammar)) + "); " +
      std::string{resolution});
}

inline std::uint8_t strict_decode_acgt_state(nuc_base base) {
  if (base.raw() > nuc_base::T)
    throw std::runtime_error("single-site chart: invalid nuc_base raw value " +
                             std::to_string(base.raw()));
  return base.raw();
}

inline void validate_state(std::uint8_t state, std::string_view label) {
  if (state >= nuc_state_count)
    throw std::runtime_error("single-site chart: invalid nucleotide state " +
                             std::to_string(state) + " for " +
                             std::string{label});
}

inline chart_cost transition_cost(std::uint8_t parent_state,
                                  std::uint8_t child_state) {
  validate_state(parent_state, "parent");
  validate_state(child_state, "child");
  return parent_state == child_state ? chart_cost{0} : chart_cost{1};
}

inline chart_cost saturated_add(chart_cost lhs, chart_cost rhs) {
  if (lhs >= chart_inf || rhs >= chart_inf) return chart_inf;
  if (lhs > chart_inf - rhs) return chart_inf;
  return lhs + rhs;
}

inline std::uint8_t strict_reference_state_at(std::string_view reference,
                                              mutation_position pos) {
  if (pos == 0 || pos > reference.size()) {
    throw std::runtime_error(
        "single-site chart: mutation position " + std::to_string(pos) +
        " outside reference length " + std::to_string(reference.size()));
  }

  std::uint8_t result = nuc_base::A;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    auto state = strict_decode_acgt_state(reference[i]);
    if (i + 1 == pos) result = state;
  }
  return result;
}

inline std::uint8_t strict_compact_genome_state_at(compact_genome const& cg,
                                                   std::string_view reference,
                                                   mutation_position pos,
                                                   std::uint8_t reference_state,
                                                   std::string_view sample_id) {
  auto observed = reference_state;
  for (auto const& [mut_pos, base] : cg) {
    if (mut_pos == 0 || mut_pos > reference.size()) {
      throw std::runtime_error(
          "single-site chart: compact-genome mutation position " +
          std::to_string(mut_pos) + " for sample '" + std::string{sample_id} +
          "' outside reference length " + std::to_string(reference.size()));
    }
    auto state = strict_decode_acgt_state(base);
    if (mut_pos == pos) observed = state;
  }
  return observed;
}

inline std::array<chart_cost, nuc_state_count> make_inf_row() {
  std::array<chart_cost, nuc_state_count> row{};
  row.fill(chart_inf);
  return row;
}

inline void validate_binary_production_partition(clade_grammar const& grammar,
                                                 grammar_production const& prod,
                                                 production_id pid) {
  if (prod.children.size() != 2) {
    throw std::runtime_error(
        "single-site chart: production " + std::to_string(pid) +
        " has arity " + std::to_string(prod.children.size()) +
        "; binary partition validation requires arity 2");
  }
  larch::detail::validate_production_partition(
      grammar, prod.parent, prod.children,
      "single-site chart: production " + std::to_string(pid));
}

inline void validate_production_inside_row_inputs(
    clade_grammar const& grammar, grammar_production const& prod,
    production_id pid, std::string_view context) {
  record_production_partition_validation();
  auto prefix = std::string{context} + ": production " + std::to_string(pid);
  if (prod.children.size() < 2) {
    throw std::runtime_error(prefix + " has arity " +
                             std::to_string(prod.children.size()) +
                             "; expected at least 2 children");
  }
  larch::detail::validate_production_partition(grammar, prod.parent,
                                               prod.children, prefix);

  auto const parent_size = grammar.clades[prod.parent].taxa.size();
  for (auto child : prod.children) {
    if (grammar.clades[child].taxa.size() >= parent_size) {
      throw std::runtime_error(prefix +
                               " child is not smaller than parent");
    }
  }
}

template <class Production, class RowProvider>
inline chart_cost combine_production_inside_row(
    Production const& prod, std::uint8_t parent_state,
    RowProvider&& row_provider) {
  validate_state(parent_state, "parent");
  chart_cost total = 0;
  for (auto child : prod.children) {
    auto const& row = row_provider(child);
    chart_cost best_child = chart_inf;
    for (std::uint8_t child_state = 0; child_state < nuc_state_count;
         ++child_state) {
      best_child = std::min(
          best_child,
          saturated_add(row[child_state],
                        transition_cost(parent_state, child_state)));
    }
    total = saturated_add(total, best_child);
  }
  return total;
}

// Specialized all-parent-state recurrence for the fixed four-state, unit-cost
// Fitch transition model.  For parent state p and one child row r,
//
//   min_s(r[s] + [s != p]) = min(r[p], min_s(r[s]) + 1).
//
// Keep the generic transition recurrence above for consumers whose transition
// model is not known to be unit Fitch.  This specialization deliberately
// obtains each child row once, then accumulates that child's contribution into
// all four parent states before moving to the next child.  The child loop order
// and saturated_add calls therefore match the generic recurrence, including
// normalization of row values at or above chart_inf.
template <class Production, class RowProvider>
inline std::array<chart_cost, nuc_state_count>
combine_production_inside_rows_unit_fitch(Production const& prod,
                                          RowProvider&& row_provider) {
  static_assert(nuc_state_count == 4);

  std::array<chart_cost, nuc_state_count> totals{};
  for (auto child : prod.children) {
    auto const& row = row_provider(child);
    auto const row_min =
        std::min(std::min(row[0], row[1]), std::min(row[2], row[3]));
    auto const best_mismatch = saturated_add(row_min, chart_cost{1});

    for (std::size_t parent_state = 0; parent_state < nuc_state_count;
         ++parent_state) {
      auto const best_child = std::min(
          saturated_add(row[parent_state], chart_cost{0}), best_mismatch);
      totals[parent_state] = saturated_add(totals[parent_state], best_child);
    }
  }
  return totals;
}

inline void validate_chart_grammar(clade_grammar const& grammar) {
  record_full_grammar_validation();
  if (grammar.clades.size() >= static_cast<std::size_t>(no_clade))
    throw std::runtime_error("single-site chart: too many clades");
  if (grammar.productions.size() >= static_cast<std::size_t>(no_production))
    throw std::runtime_error("single-site chart: too many productions");
  if (grammar.productions_by_parent.size() != grammar.clades.size())
    throw std::runtime_error(
        "single-site chart: productions_by_parent size mismatch");
  if (grammar.productions_by_child.size() != grammar.clades.size())
    throw std::runtime_error(
        "single-site chart: productions_by_child size mismatch");

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto const& taxa = grammar.clades[cid].taxa;
    if (taxa.empty())
      throw std::runtime_error("single-site chart: empty clade is invalid");
    if (!std::is_sorted(taxa.begin(), taxa.end()) ||
        std::adjacent_find(taxa.begin(), taxa.end()) != taxa.end()) {
      throw std::runtime_error(
          "single-site chart: clade taxa must be sorted and unique");
    }
  }
}

}  // namespace parsimony_chart_detail

inline chart_cost single_site_chart::root_min_with_reference_edge(
    clade_id root, std::uint8_t reference_state) const {
  parsimony_chart_detail::validate_state(reference_state, "reference");
  if (root == no_clade || root >= inside.size())
    throw std::runtime_error("single-site chart: root clade out of range");

  chart_cost best = chart_inf;
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    auto candidate = parsimony_chart_detail::saturated_add(
        inside[root][state],
        parsimony_chart_detail::transition_cost(reference_state, state));
    best = std::min(best, candidate);
  }
  return best;
}

inline std::uint8_t extract_reference_site_state(phylo_dag& dag,
                                                 mutation_position pos) {
  return parsimony_chart_detail::strict_reference_state_at(
      get_reference_sequence(dag), pos);
}

inline chart_cost root_min(single_site_chart const& chart, clade_id root,
                           chart_options const& options) {
  if (options.score_ua_edge) {
    throw std::runtime_error(
        "single-site chart: reference state is required when "
        "chart_options::score_ua_edge is true");
  }
  return chart.root_min_excluding_ua(root);
}

inline chart_cost root_min(single_site_chart const& chart, clade_id root,
                           chart_options const& options,
                           std::uint8_t reference_state) {
  if (options.score_ua_edge)
    return chart.root_min_with_reference_edge(root, reference_state);
  return chart.root_min_excluding_ua(root);
}

inline chart_cost root_min(single_site_chart const& chart, clade_id root,
                           chart_options const& options, phylo_dag& dag,
                           mutation_position pos) {
  if (options.score_ua_edge) {
    return chart.root_min_with_reference_edge(
        root, extract_reference_site_state(dag, pos));
  }
  return chart.root_min_excluding_ua(root);
}

inline leaf_site_states extract_leaf_site_states(phylo_dag& dag,
                                                 clade_grammar const& grammar,
                                                 mutation_position pos) {
  auto const& reference = get_reference_sequence(dag);
  auto reference_state =
      parsimony_chart_detail::strict_reference_state_at(reference, pos);

  leaf_site_states result;
  result.state_by_taxon.assign(grammar.taxa.id_to_sample_id.size(),
                               static_cast<std::uint8_t>(nuc_state_count));
  std::vector<bool> seen(grammar.taxa.id_to_sample_id.size(), false);

  auto reachable = detail::collect_reachable(dag);
  for (auto node_idx : reachable.nodes) {
    auto nv = dag.get_node(node_idx);
    if (!detail::is_leaf_node(nv)) continue;

    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            std::string sample_id{node.sample_id()};
            auto it = grammar.taxa.sample_id_to_id.find(sample_id);
            if (it == grammar.taxa.sample_id_to_id.end()) {
              throw std::runtime_error(
                  "single-site chart: reachable leaf sample_id '" + sample_id +
                  "' is not present in the supplied clade grammar");
            }
            auto tid = it->second;
            if (tid >= result.state_by_taxon.size())
              throw std::runtime_error(
                  "single-site chart: taxon id out of range for sample '" +
                  sample_id + "'");

            auto observed =
                parsimony_chart_detail::strict_compact_genome_state_at(
                    node.cg(), reference, pos, reference_state, sample_id);
            if (seen[tid] && result.state_by_taxon[tid] != observed) {
              throw std::runtime_error(
                  "single-site chart: duplicate sample_id '" + sample_id +
                  "' has conflicting observed states at position " +
                  std::to_string(pos));
            }
            result.state_by_taxon[tid] = observed;
            seen[tid] = true;
          } else {
            throw std::runtime_error(
                "single-site chart: expected leaf node annotation");
          }
        },
        nv);
  }

  for (std::size_t tid = 0; tid < seen.size(); ++tid) {
    if (!seen[tid]) {
      throw std::runtime_error(
          "single-site chart: missing observed leaf state "
          "for taxon '" +
          grammar.taxa.id_to_sample_id[tid] + "'");
    }
  }
  return result;
}

inline single_site_chart build_single_site_chart(
    clade_grammar const& grammar, leaf_site_states_view leaf_states,
    chart_options const& options) {
  using namespace parsimony_chart_detail;

  validate_chart_grammar(grammar);
  if (options.keep_trace) {
    require_no_multifurcating_productions_for_consumer(
        grammar, arity_gate_consumer::single_site_chart_trace,
        "single-site chart keep_trace", "trace",
        "build without keep_trace, or expand polytomies before using the "
        "binary trace layer");
  }
  if (leaf_states.state_by_taxon.size() !=
      grammar.taxa.id_to_sample_id.size()) {
    throw std::runtime_error(
        "single-site chart: leaf state count does not match taxon count");
  }
  for (std::size_t tid = 0; tid < leaf_states.state_by_taxon.size(); ++tid)
    validate_state(leaf_states.state_by_taxon[tid],
                   "taxon " + std::to_string(tid));

  single_site_chart chart;
  chart.inside.assign(grammar.clades.size(), make_inf_row());
  if (options.keep_trace) chart.optimal_choices.resize(grammar.clades.size());

  std::vector<clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  record_clade_order_sort();
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto const& ltaxa = grammar.clades[lhs].taxa;
    auto const& rtaxa = grammar.clades[rhs].taxa;
    if (ltaxa.size() != rtaxa.size()) return ltaxa.size() < rtaxa.size();
    return lhs < rhs;
  });

  auto append_choice = [&](clade_id cid, std::uint8_t state,
                           chart_choice choice) {
    if (!options.keep_trace) return;
    if (options.max_trace_choices != 0 &&
        chart.trace_choice_count >= options.max_trace_choices) {
      throw std::runtime_error(
          "single-site chart: trace choice cap exceeded (cap=" +
          std::to_string(options.max_trace_choices) + ")");
    }
    chart.optimal_choices[cid][state].push_back(choice);
    ++chart.trace_choice_count;
  };

  auto clear_choices = [&](clade_id cid, std::uint8_t state) {
    if (!options.keep_trace) return;
    auto& choices = chart.optimal_choices[cid][state];
    chart.trace_choice_count -= choices.size();
    choices.clear();
  };

  for (auto cid : order) {
    if (cid >= grammar.clades.size())
      throw std::runtime_error("single-site chart: clade id out of range");

    auto const& clade = grammar.clades[cid];
    if (clade.taxa.empty())
      throw std::runtime_error("single-site chart: empty clade is invalid");

    if (clade.taxa.size() == 1) {
      if (!grammar.productions_by_parent[cid].empty()) {
        throw std::runtime_error(
            "single-site chart: singleton leaf clade has productions");
      }
      auto taxon = clade.taxa.front();
      if (taxon >= leaf_states.state_by_taxon.size())
        throw std::runtime_error(
            "single-site chart: singleton taxon id out of range");
      auto observed = leaf_states.state_by_taxon[taxon];
      for (std::uint8_t state = 0; state < nuc_state_count; ++state)
        chart.inside[cid][state] =
            (state == observed) ? chart_cost{0} : chart_inf;
      continue;
    }

    auto const& parent_productions = grammar.productions_by_parent[cid];
    if (parent_productions.empty()) {
      throw std::runtime_error(
          "single-site chart: non-singleton clade has no productions");
    }

    for (auto pid : parent_productions) {
      if (pid == no_production || pid >= grammar.productions.size())
        throw std::runtime_error(
            "single-site chart: production id out of range");
      auto const& prod = grammar.productions[pid];
      if (prod.parent != cid)
        throw std::runtime_error(
            "single-site chart: productions_by_parent contains production "
            "with mismatched parent");
      validate_production_inside_row_inputs(grammar, prod, pid,
                                            "single-site chart");
      if (prod.children.size() != 2) {
        ++chart.multifurcation_productions_scored;
      }
      if (options.keep_trace && prod.children.size() != 2) {
        throw std::runtime_error(
            "single-site chart: keep_trace uses the binary choice layer; "
            "production " +
            std::to_string(pid) + " has arity " +
            std::to_string(prod.children.size()));
      }

      for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
           ++parent_state) {
        auto row_provider = [&](clade_id child) -> auto const& {
          return chart.inside[child];
        };
        auto candidate =
            combine_production_inside_row(prod, parent_state, row_provider);
        if (candidate >= chart_inf) continue;

        if (!options.keep_trace) {
          auto& cell = chart.inside[cid][parent_state];
          cell = std::min(cell, candidate);
          continue;
        }

        std::array<clade_id, 2> children{prod.children[0], prod.children[1]};
        std::array<chart_cost, 2> best_child_cost{chart_inf, chart_inf};
        std::array<std::vector<std::uint8_t>, 2> best_child_states;

        for (std::size_t child_i = 0; child_i < children.size(); ++child_i) {
          auto child = children[child_i];
          for (std::uint8_t child_state = 0; child_state < nuc_state_count;
               ++child_state) {
            auto candidate =
                saturated_add(chart.inside[child][child_state],
                              transition_cost(parent_state, child_state));
            if (candidate < best_child_cost[child_i]) {
              best_child_cost[child_i] = candidate;
              if (options.keep_trace) {
                best_child_states[child_i].clear();
                if (candidate < chart_inf)
                  best_child_states[child_i].push_back(child_state);
              }
            } else if (options.keep_trace &&
                       candidate == best_child_cost[child_i] &&
                       candidate < chart_inf) {
              best_child_states[child_i].push_back(child_state);
            }
          }
        }

        auto& cell = chart.inside[cid][parent_state];
        if (candidate < cell) {
          cell = candidate;
          clear_choices(cid, parent_state);
        }
        if (options.keep_trace && candidate == cell) {
          for (auto left_state : best_child_states[0]) {
            for (auto right_state : best_child_states[1]) {
              append_choice(
                  cid, parent_state,
                  chart_choice{pid, {left_state, right_state}, candidate});
            }
          }
        }
      }
    }
  }

  return chart;
}

inline single_site_chart build_single_site_chart(
    clade_grammar const& grammar, leaf_site_states const& leaf_states,
    chart_options const& options) {
  return build_single_site_chart(grammar, view_leaf_site_states(leaf_states),
                                 options);
}

// Trusted recurrence over a checked, immutable structural plan.  The plan
// owns every ID/order/production descriptor used below, so pattern builds do
// not rescan the grammar, sort clades, or revalidate partitions.
inline single_site_chart build_single_site_chart(
    chart_execution_plan const& plan, leaf_site_states_view leaf_states,
    chart_options const& options) {
  using namespace parsimony_chart_detail;
  plan.assert_valid();

  if (options.keep_trace && !plan.all_binary()) {
    auto first =
        std::find_if(plan.productions().begin(), plan.productions().end(),
                     [](chart_plan_production_descriptor const& production) {
                       return !production.is_binary();
                     });
    record_arity_gate_throw(arity_gate_consumer::single_site_chart_trace);
    auto const production_id =
        first == plan.productions().end() ? no_production : first->source_id;
    auto const arity = first == plan.productions().end() ? plan.max_arity()
                                                         : first->child_count;
    throw std::runtime_error(
        "single-site chart keep_trace: WI6 arity gate: the chart supports "
        "multifurcations; this consumer's trace does not (production " +
        std::to_string(production_id) + " has arity " + std::to_string(arity) +
        ", grammar max arity " + std::to_string(plan.max_arity()) +
        "); build without keep_trace, or expand polytomies before using the "
        "binary trace layer");
  }
  if (leaf_states.state_by_taxon.size() != plan.taxon_count()) {
    throw std::runtime_error(
        "single-site chart: leaf state count does not match taxon count");
  }
  for (std::size_t tid = 0; tid < leaf_states.state_by_taxon.size(); ++tid) {
    validate_state(leaf_states.state_by_taxon[tid],
                   "taxon " + std::to_string(tid));
  }

  single_site_chart chart;
  chart.inside.assign(plan.clades().size(), make_inf_row());
  if (options.keep_trace) chart.optimal_choices.resize(plan.clades().size());

  auto append_choice = [&](clade_id cid, std::uint8_t state,
                           chart_choice choice) {
    if (!options.keep_trace) return;
    if (options.max_trace_choices != 0 &&
        chart.trace_choice_count >= options.max_trace_choices) {
      throw std::runtime_error(
          "single-site chart: trace choice cap exceeded (cap=" +
          std::to_string(options.max_trace_choices) + ")");
    }
    chart.optimal_choices[cid][state].push_back(choice);
    ++chart.trace_choice_count;
  };

  auto clear_choices = [&](clade_id cid, std::uint8_t state) {
    if (!options.keep_trace) return;
    auto& choices = chart.optimal_choices[cid][state];
    chart.trace_choice_count -= choices.size();
    choices.clear();
  };

  for (auto cid : plan.bottom_up_order()) {
    auto const& clade = plan.clade(cid);
    if (clade.is_leaf()) {
      auto observed = leaf_states.state_by_taxon[clade.leaf_taxon];
      for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
        chart.inside[cid][state] =
            state == observed ? chart_cost{0} : chart_inf;
      }
      continue;
    }

    for (auto pid : plan.productions_for_parent(cid)) {
      auto const& production = plan.production(pid);
      auto const children = plan.children(pid);
      if (!production.is_binary()) {
        ++chart.multifurcation_productions_scored;
      }

      for (std::uint8_t parent_state = 0; parent_state < nuc_state_count;
           ++parent_state) {
        chart_cost candidate = 0;
        for (auto child : children) {
          auto const& child_row = chart.inside[child];
          chart_cost best_child = chart_inf;
          for (std::uint8_t child_state = 0; child_state < nuc_state_count;
               ++child_state) {
            best_child = std::min(
                best_child,
                saturated_add(child_row[child_state],
                              static_cast<chart_cost>(plan.transition_cost(
                                  parent_state, child_state))));
          }
          candidate = saturated_add(candidate, best_child);
        }
        if (candidate >= chart_inf) continue;

        if (!options.keep_trace) {
          auto& cell = chart.inside[cid][parent_state];
          cell = std::min(cell, candidate);
          continue;
        }

        std::array<chart_cost, 2> best_child_cost{chart_inf, chart_inf};
        std::array<std::vector<std::uint8_t>, 2> best_child_states;
        for (std::size_t child_i = 0; child_i < 2; ++child_i) {
          auto child = production.binary_children[child_i];
          for (std::uint8_t child_state = 0; child_state < nuc_state_count;
               ++child_state) {
            auto child_candidate =
                saturated_add(chart.inside[child][child_state],
                              static_cast<chart_cost>(plan.transition_cost(
                                  parent_state, child_state)));
            if (child_candidate < best_child_cost[child_i]) {
              best_child_cost[child_i] = child_candidate;
              best_child_states[child_i].clear();
              if (child_candidate < chart_inf) {
                best_child_states[child_i].push_back(child_state);
              }
            } else if (child_candidate == best_child_cost[child_i] &&
                       child_candidate < chart_inf) {
              best_child_states[child_i].push_back(child_state);
            }
          }
        }

        auto& cell = chart.inside[cid][parent_state];
        if (candidate < cell) {
          cell = candidate;
          clear_choices(cid, parent_state);
        }
        if (candidate == cell) {
          for (auto left_state : best_child_states[0]) {
            for (auto right_state : best_child_states[1]) {
              append_choice(
                  cid, parent_state,
                  chart_choice{pid, {left_state, right_state}, candidate});
            }
          }
        }
      }
    }
  }
  return chart;
}

inline single_site_chart build_single_site_chart(
    chart_execution_plan const& plan, leaf_site_states const& leaf_states,
    chart_options const& options) {
  return build_single_site_chart(plan, view_leaf_site_states(leaf_states),
                                 options);
}

inline single_site_chart build_single_site_chart(
    chart_execution_plan const& plan, leaf_site_states_view leaf_states,
    bool keep_trace = false) {
  chart_options options;
  options.keep_trace = keep_trace;
  return build_single_site_chart(plan, leaf_states, options);
}

inline single_site_chart build_single_site_chart(
    clade_grammar const& grammar, leaf_site_states_view leaf_states,
    bool keep_trace = false) {
  chart_options options;
  options.keep_trace = keep_trace;
  return build_single_site_chart(grammar, leaf_states, options);
}

inline single_site_chart build_single_site_chart(
    chart_execution_plan const& plan, leaf_site_states const& leaf_states,
    bool keep_trace = false) {
  chart_options options;
  options.keep_trace = keep_trace;
  return build_single_site_chart(plan, leaf_states, options);
}

inline single_site_chart build_single_site_chart(
    clade_grammar const& grammar, leaf_site_states const& leaf_states,
    bool keep_trace = false) {
  chart_options options;
  options.keep_trace = keep_trace;
  return build_single_site_chart(grammar, leaf_states, options);
}

}  // namespace larch
