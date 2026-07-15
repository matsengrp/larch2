#pragma once

#include <larch/clade_grammar.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace larch {

inline constexpr taxon_id chart_plan_no_taxon =
    std::numeric_limits<taxon_id>::max();

struct chart_plan_fingerprint {
  static constexpr std::uint32_t current_schema_version = 2;
  static constexpr std::uint32_t unit_fitch_cost_model_version = 1;

  std::array<std::uint64_t, 2> structure{};
  std::uint32_t schema_version = current_schema_version;
  std::uint32_t cost_model_version = unit_fitch_cost_model_version;

  bool operator==(chart_plan_fingerprint const&) const = default;
};

struct chart_execution_plan_build_stats {
  std::size_t plan_builds = 0;
  std::size_t full_grammar_validations = 0;
  std::size_t production_index_validations = 0;
  std::size_t production_partition_validations = 0;
  std::size_t clade_order_sorts = 0;
  std::size_t production_descriptors_compiled = 0;
};

struct chart_plan_clade_descriptor {
  clade_id source_id = no_clade;
  std::size_t taxa_size = 0;
  taxon_id leaf_taxon = chart_plan_no_taxon;
  std::size_t parent_production_begin = 0;
  std::size_t parent_production_count = 0;
  std::size_t child_occurrence_begin = 0;
  std::size_t child_occurrence_count = 0;
  std::size_t dependency_level = 0;
  std::size_t upward_path_count = 0;

  [[nodiscard]] bool is_leaf() const noexcept {
    return leaf_taxon != chart_plan_no_taxon;
  }
};

struct chart_plan_child_occurrence {
  production_id production = no_production;
  std::size_t child_slot = 0;

  bool operator==(chart_plan_child_occurrence const&) const = default;
};

struct chart_plan_production_descriptor {
  production_id source_id = no_production;
  clade_id parent = no_clade;
  std::size_t child_begin = 0;
  std::size_t child_count = 0;
  std::array<clade_id, 2> binary_children{no_clade, no_clade};

  [[nodiscard]] bool is_binary() const noexcept { return child_count == 2; }
};

class chart_execution_plan;
chart_execution_plan build_chart_execution_plan(clade_grammar const& grammar);

class chart_execution_plan_mismatch : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

namespace chart_execution_plan_detail {

// Test/diagnostic instrumentation for checked publication boundaries.  A
// compatibility check fingerprints the complete grammar, so it must be paid
// once when an immutable grammar/plan snapshot is published to a batch, not
// once per candidate or per pattern.
struct compatibility_work_observer {
  std::size_t* compatibility_checks = nullptr;
  std::size_t* full_grammar_fingerprint_scans = nullptr;
  std::size_t* legacy_production_index_validations = nullptr;
  std::size_t* dynamic_overlay_partition_validations = nullptr;
};

inline thread_local compatibility_work_observer*
    active_compatibility_work_observer = nullptr;

class compatibility_work_observer_scope {
 public:
  explicit compatibility_work_observer_scope(
      compatibility_work_observer* observer) noexcept
      : previous_(active_compatibility_work_observer) {
    active_compatibility_work_observer = observer;
  }
  compatibility_work_observer_scope(
      compatibility_work_observer_scope const&) = delete;
  compatibility_work_observer_scope& operator=(
      compatibility_work_observer_scope const&) = delete;
  ~compatibility_work_observer_scope() {
    active_compatibility_work_observer = previous_;
  }

 private:
  compatibility_work_observer* previous_ = nullptr;
};

inline void record_compatibility_check() {
  if (active_compatibility_work_observer != nullptr &&
      active_compatibility_work_observer->compatibility_checks != nullptr) {
    ++*active_compatibility_work_observer->compatibility_checks;
  }
}

inline void record_full_grammar_fingerprint_scan() {
  if (active_compatibility_work_observer != nullptr &&
      active_compatibility_work_observer->full_grammar_fingerprint_scans !=
          nullptr) {
    ++*active_compatibility_work_observer->full_grammar_fingerprint_scans;
  }
}

inline void record_legacy_production_index_validation() {
  if (active_compatibility_work_observer != nullptr &&
      active_compatibility_work_observer
              ->legacy_production_index_validations != nullptr) {
    ++*active_compatibility_work_observer
           ->legacy_production_index_validations;
  }
}

inline void record_dynamic_overlay_partition_validation() {
  if (active_compatibility_work_observer != nullptr &&
      active_compatibility_work_observer
              ->dynamic_overlay_partition_validations != nullptr) {
    ++*active_compatibility_work_observer
           ->dynamic_overlay_partition_validations;
  }
}

inline void fingerprint_mix(std::uint64_t& hash, std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
  hash *= 0x100000001b3ULL;
}

inline void fingerprint_string(std::uint64_t& hash, std::string_view value) {
  fingerprint_mix(hash, value.size());
  for (unsigned char byte : value) fingerprint_mix(hash, byte);
}

inline chart_plan_fingerprint fingerprint_chart_grammar(
    clade_grammar const& grammar) {
  chart_plan_fingerprint result;
  auto& first = result.structure[0];
  auto& second = result.structure[1];
  first = 0xcbf29ce484222325ULL;
  second = 0x84222325cbf29ce4ULL;

  auto mix = [&](std::uint64_t value) {
    fingerprint_mix(first, value);
    fingerprint_mix(second, value ^ 0xd6e8feb86659fd93ULL);
  };
  auto mix_string = [&](std::string_view value) {
    fingerprint_string(first, value);
    fingerprint_mix(second, value.size());
    for (auto it = value.rbegin(); it != value.rend(); ++it) {
      fingerprint_mix(second, static_cast<unsigned char>(*it));
    }
  };

  mix(result.schema_version);
  mix(result.cost_model_version);
  mix(grammar.taxa.id_to_sample_id.size());
  for (auto const& sample_id : grammar.taxa.id_to_sample_id) {
    mix_string(sample_id);
  }
  // Schema 2 fingerprints the reverse registry without copying or sorting its
  // owned strings. The forward registry supplies a deterministic O(T) order;
  // each reverse lookup contributes an explicit present/missing marker and
  // the mapped id. Mixing the map size additionally detects surplus keys.
  mix(grammar.taxa.sample_id_to_id.size());
  for (auto const& sample_id : grammar.taxa.id_to_sample_id) {
    auto const found = grammar.taxa.sample_id_to_id.find(sample_id);
    auto const present = found != grammar.taxa.sample_id_to_id.end();
    mix(present ? 0xb7f42a136d8ce901ULL : 0x498d3c6fe2157ab4ULL);
    if (present) mix(found->second);
  }
  mix(grammar.clades.size());
  for (auto const& clade : grammar.clades) {
    mix(clade.taxa.size());
    for (auto taxon : clade.taxa) mix(taxon);
  }
  mix(grammar.productions.size());
  for (auto const& production : grammar.productions) {
    mix(production.parent);
    mix(production.children.size());
    for (auto child : production.children) mix(child);
    mix(production.multiplicity);
    mix(production.witnesses.size());
    for (auto const& witness : production.witnesses) {
      mix(witness.parent_node);
      mix(witness.children.size());
      for (auto const& child_witness : witness.children) {
        mix(child_witness.child);
        mix(child_witness.edge_alternatives.size());
        for (auto edge : child_witness.edge_alternatives) mix(edge);
      }
    }
  }
  mix(grammar.productions_by_parent.size());
  for (auto const& production_ids : grammar.productions_by_parent) {
    mix(production_ids.size());
    for (auto production : production_ids) mix(production);
  }
  mix(grammar.productions_by_child.size());
  for (auto const& production_ids : grammar.productions_by_child) {
    mix(production_ids.size());
    for (auto production : production_ids) mix(production);
  }
  mix(grammar.root_clade);
  mix(grammar.node_to_clade.size());
  for (auto clade : grammar.node_to_clade) mix(clade);
  return result;
}

inline void validate_chart_execution_plan_input(
    clade_grammar const& grammar, chart_execution_plan_build_stats& stats) {
  ++stats.full_grammar_validations;
  auto const clade_count = grammar.clades.size();
  auto const production_count = grammar.productions.size();
  auto const taxon_count = grammar.taxa.id_to_sample_id.size();

  if (clade_count >= static_cast<std::size_t>(no_clade)) {
    throw std::runtime_error("chart execution plan: too many clades");
  }
  if (production_count >= static_cast<std::size_t>(no_production)) {
    throw std::runtime_error("chart execution plan: too many productions");
  }
  if (taxon_count >= static_cast<std::size_t>(chart_plan_no_taxon)) {
    throw std::runtime_error("chart execution plan: too many taxa");
  }
  if (grammar.root_clade == no_clade || grammar.root_clade >= clade_count) {
    throw std::runtime_error("chart execution plan: root clade out of range");
  }
  if (grammar.productions_by_parent.size() != clade_count) {
    throw std::runtime_error(
        "chart execution plan: productions_by_parent size mismatch");
  }
  if (grammar.productions_by_child.size() != clade_count) {
    throw std::runtime_error(
        "chart execution plan: productions_by_child size mismatch");
  }

  for (std::size_t tid = 0; tid < taxon_count; ++tid) {
    auto const& sample_id = grammar.taxa.id_to_sample_id[tid];
    auto found = grammar.taxa.sample_id_to_id.find(sample_id);
    if (found == grammar.taxa.sample_id_to_id.end() || found->second != tid) {
      throw std::runtime_error(
          "chart execution plan: taxon registry forward/reverse mismatch");
    }
  }
  if (grammar.taxa.sample_id_to_id.size() != taxon_count) {
    throw std::runtime_error(
        "chart execution plan: taxon registry size mismatch");
  }

  for (std::size_t cid = 0; cid < clade_count; ++cid) {
    auto const& taxa = grammar.clades[cid].taxa;
    if (taxa.empty()) {
      throw std::runtime_error("chart execution plan: clade " +
                               std::to_string(cid) + " is empty");
    }
    if (!std::is_sorted(taxa.begin(), taxa.end()) ||
        std::adjacent_find(taxa.begin(), taxa.end()) != taxa.end()) {
      throw std::runtime_error(
          "chart execution plan: clade taxa must be sorted and unique");
    }
    if (taxa.back() >= taxon_count) {
      throw std::runtime_error("chart execution plan: clade " +
                               std::to_string(cid) +
                               " has taxon id out of range");
    }
  }

  std::vector<std::size_t> parent_occurrences(production_count, 0);
  for (std::size_t parent = 0; parent < clade_count; ++parent) {
    for (auto pid : grammar.productions_by_parent[parent]) {
      ++stats.production_index_validations;
      if (pid == no_production || pid >= production_count) {
        throw std::runtime_error(
            "chart execution plan: productions_by_parent contains invalid "
            "production id");
      }
      if (grammar.productions[pid].parent != parent) {
        throw std::runtime_error(
            "chart execution plan: productions_by_parent contains mismatched "
            "parent");
      }
      if (++parent_occurrences[pid] != 1) {
        throw std::runtime_error(
            "chart execution plan: duplicate production in "
            "productions_by_parent");
      }
    }
  }
  for (std::size_t pid = 0; pid < production_count; ++pid) {
    if (parent_occurrences[pid] != 1) {
      throw std::runtime_error("chart execution plan: production " +
                               std::to_string(pid) +
                               " missing from productions_by_parent");
    }
  }

  std::vector<std::size_t> child_marks(production_count, 0);
  for (std::size_t child = 0; child < clade_count; ++child) {
    auto const mark = child + 1;
    for (auto pid : grammar.productions_by_child[child]) {
      ++stats.production_index_validations;
      if (pid == no_production || pid >= production_count) {
        throw std::runtime_error(
            "chart execution plan: productions_by_child contains invalid "
            "production id");
      }
      auto const& children = grammar.productions[pid].children;
      if (std::find(children.begin(), children.end(), child) ==
          children.end()) {
        throw std::runtime_error(
            "chart execution plan: productions_by_child contains mismatched "
            "child");
      }
      if (child_marks[pid] == mark) {
        throw std::runtime_error(
            "chart execution plan: duplicate production in "
            "productions_by_child");
      }
      child_marks[pid] = mark;
    }
  }

  for (std::size_t pid = 0; pid < production_count; ++pid) {
    auto const& production = grammar.productions[pid];
    auto const context =
        "chart execution plan: production " + std::to_string(pid);
    if (production.parent == no_clade || production.parent >= clade_count) {
      throw std::runtime_error(context + " parent out of range");
    }
    if (production.children.size() < 2) {
      throw std::runtime_error(context + " has arity " +
                               std::to_string(production.children.size()) +
                               "; expected at least 2 children");
    }
    ++stats.production_partition_validations;
    larch::detail::validate_production_partition(grammar, production.parent,
                                                 production.children, context);
    auto const parent_size = grammar.clades[production.parent].taxa.size();
    for (auto child : production.children) {
      if (grammar.clades[child].taxa.size() >= parent_size) {
        throw std::runtime_error(context + " child is not smaller than parent");
      }
      auto const& by_child = grammar.productions_by_child[child];
      if (std::find(by_child.begin(), by_child.end(), pid) == by_child.end()) {
        throw std::runtime_error(context +
                                 " missing from productions_by_child");
      }
    }
  }

  for (std::size_t cid = 0; cid < clade_count; ++cid) {
    auto const is_leaf = grammar.clades[cid].taxa.size() == 1;
    auto const has_productions = !grammar.productions_by_parent[cid].empty();
    if (is_leaf && has_productions) {
      throw std::runtime_error("chart execution plan: singleton clade " +
                               std::to_string(cid) + " has productions");
    }
    if (!is_leaf && !has_productions) {
      throw std::runtime_error("chart execution plan: non-singleton clade " +
                               std::to_string(cid) + " has no productions");
    }
  }
}

}  // namespace chart_execution_plan_detail

class chart_execution_plan {
 public:
  chart_execution_plan() = default;
  chart_execution_plan(chart_execution_plan const&) = default;
  chart_execution_plan& operator=(chart_execution_plan const&) = default;
  chart_execution_plan(chart_execution_plan&& other) noexcept { swap(other); }
  chart_execution_plan& operator=(chart_execution_plan&& other) noexcept {
    if (this == &other) return *this;
    chart_execution_plan moved;
    moved.swap(other);
    swap(moved);
    return *this;
  }

  void swap(chart_execution_plan& other) noexcept {
    using std::swap;
    swap(valid_, other.valid_);
    swap(grammar_generation_, other.grammar_generation_);
    swap(fingerprint_, other.fingerprint_);
    swap(build_stats_, other.build_stats_);
    swap(taxon_count_, other.taxon_count_);
    swap(root_clade_, other.root_clade_);
    swap(all_binary_, other.all_binary_);
    swap(max_arity_, other.max_arity_);
    swap(clades_, other.clades_);
    swap(productions_, other.productions_);
    swap(children_, other.children_);
    swap(productions_by_parent_, other.productions_by_parent_);
    swap(child_occurrences_, other.child_occurrences_);
    swap(bottom_up_order_, other.bottom_up_order_);
    swap(top_down_order_, other.top_down_order_);
    swap(bottom_up_level_order_, other.bottom_up_level_order_);
    swap(bottom_up_level_offsets_, other.bottom_up_level_offsets_);
    swap(top_down_level_order_, other.top_down_level_order_);
    swap(top_down_level_offsets_, other.top_down_level_offsets_);
    swap(transition_costs_, other.transition_costs_);
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  void assert_valid() const {
    if (!valid_) {
      throw std::runtime_error("chart execution plan: uninitialized plan");
    }
  }

  [[nodiscard]] std::uint64_t grammar_generation() const noexcept {
    return grammar_generation_;
  }
  [[nodiscard]] chart_plan_fingerprint const& fingerprint() const noexcept {
    return fingerprint_;
  }
  [[nodiscard]] chart_execution_plan_build_stats const& build_stats()
      const noexcept {
    return build_stats_;
  }
  [[nodiscard]] std::size_t taxon_count() const noexcept {
    return taxon_count_;
  }
  [[nodiscard]] clade_id root_clade() const noexcept { return root_clade_; }
  [[nodiscard]] bool all_binary() const noexcept { return all_binary_; }
  [[nodiscard]] std::size_t max_arity() const noexcept { return max_arity_; }

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    std::size_t total = 0;
    auto add = [&](auto const& values) {
      using vector_type = std::remove_cvref_t<decltype(values)>;
      auto const capacity = values.capacity();
      if (capacity != 0 &&
          sizeof(typename vector_type::value_type) >
              (std::numeric_limits<std::size_t>::max)() / capacity) {
        throw std::overflow_error(
            "chart execution plan capacity byte overflow");
      }
      auto const bytes = capacity * sizeof(typename vector_type::value_type);
      if (total > (std::numeric_limits<std::size_t>::max)() - bytes) {
        throw std::overflow_error(
            "chart execution plan capacity byte overflow");
      }
      total += bytes;
    };
    add(clades_);
    add(productions_);
    add(children_);
    add(productions_by_parent_);
    add(child_occurrences_);
    add(bottom_up_order_);
    add(top_down_order_);
    add(bottom_up_level_order_);
    add(bottom_up_level_offsets_);
    add(top_down_level_order_);
    add(top_down_level_offsets_);
    return total;
  }

  [[nodiscard]] std::size_t resident_bytes() const {
    auto const dynamic = dynamic_capacity_bytes();
    if (dynamic > (std::numeric_limits<std::size_t>::max)() - sizeof(*this)) {
      throw std::overflow_error("chart execution plan resident byte overflow");
    }
    return sizeof(*this) + dynamic;
  }

  [[nodiscard]] std::span<chart_plan_clade_descriptor const> clades() const {
    return clades_;
  }
  [[nodiscard]] std::span<chart_plan_production_descriptor const> productions()
      const {
    return productions_;
  }
  [[nodiscard]] std::span<clade_id const> bottom_up_order() const {
    return bottom_up_order_;
  }
  [[nodiscard]] std::span<clade_id const> top_down_order() const {
    return top_down_order_;
  }
  [[nodiscard]] std::span<clade_id const> bottom_up_level_order() const {
    return bottom_up_level_order_;
  }
  [[nodiscard]] std::span<std::size_t const> bottom_up_level_offsets() const {
    return bottom_up_level_offsets_;
  }
  [[nodiscard]] std::span<clade_id const> top_down_level_order() const {
    return top_down_level_order_;
  }
  [[nodiscard]] std::span<std::size_t const> top_down_level_offsets() const {
    return top_down_level_offsets_;
  }

  [[nodiscard]] chart_plan_clade_descriptor const& clade(clade_id cid) const {
    if (cid == no_clade || cid >= clades_.size()) {
      throw std::runtime_error("chart execution plan: clade id out of range");
    }
    return clades_[cid];
  }
  [[nodiscard]] chart_plan_production_descriptor const& production(
      production_id pid) const {
    if (pid == no_production || pid >= productions_.size()) {
      throw std::runtime_error(
          "chart execution plan: production id out of range");
    }
    return productions_[pid];
  }
  [[nodiscard]] std::span<clade_id const> children(production_id pid) const {
    auto const& descriptor = production(pid);
    return std::span<clade_id const>{children_}.subspan(descriptor.child_begin,
                                                        descriptor.child_count);
  }
  [[nodiscard]] std::span<production_id const> productions_for_parent(
      clade_id parent) const {
    auto const& descriptor = clade(parent);
    return std::span<production_id const>{productions_by_parent_}.subspan(
        descriptor.parent_production_begin, descriptor.parent_production_count);
  }
  [[nodiscard]] std::span<chart_plan_child_occurrence const>
  child_occurrences_for_clade(clade_id child) const {
    auto const& descriptor = clade(child);
    return std::span<chart_plan_child_occurrence const>{child_occurrences_}
        .subspan(descriptor.child_occurrence_begin,
                 descriptor.child_occurrence_count);
  }
  [[nodiscard]] std::uint8_t transition_cost(std::uint8_t parent_state,
                                             std::uint8_t child_state) const {
    if (parent_state >= transition_costs_.size() ||
        child_state >= transition_costs_[parent_state].size()) {
      throw std::runtime_error(
          "chart execution plan: nucleotide state out of range");
    }
    return transition_costs_[parent_state][child_state];
  }

  void assert_compatible(clade_grammar const& grammar) const {
    chart_execution_plan_detail::record_compatibility_check();
    assert_valid();
    if (grammar.execution_generation != grammar_generation_) {
      throw chart_execution_plan_mismatch(
          "chart execution plan: grammar generation mismatch");
    }
    chart_execution_plan_detail::record_full_grammar_fingerprint_scan();
    if (chart_execution_plan_detail::fingerprint_chart_grammar(grammar) !=
        fingerprint_) {
      throw chart_execution_plan_mismatch(
          "chart execution plan: stale grammar fingerprint mismatch");
    }
  }

 private:
  friend chart_execution_plan build_chart_execution_plan(
      clade_grammar const& grammar);

  bool valid_ = false;
  std::uint64_t grammar_generation_ = 0;
  chart_plan_fingerprint fingerprint_;
  chart_execution_plan_build_stats build_stats_;
  std::size_t taxon_count_ = 0;
  clade_id root_clade_ = no_clade;
  bool all_binary_ = true;
  std::size_t max_arity_ = 0;
  std::vector<chart_plan_clade_descriptor> clades_;
  std::vector<chart_plan_production_descriptor> productions_;
  std::vector<clade_id> children_;
  std::vector<production_id> productions_by_parent_;
  std::vector<chart_plan_child_occurrence> child_occurrences_;
  std::vector<clade_id> bottom_up_order_;
  std::vector<clade_id> top_down_order_;
  std::vector<clade_id> bottom_up_level_order_;
  std::vector<std::size_t> bottom_up_level_offsets_;
  std::vector<clade_id> top_down_level_order_;
  std::vector<std::size_t> top_down_level_offsets_;
  std::array<std::array<std::uint8_t, 4>, 4> transition_costs_{};
};

// Capability minted by the one O(|grammar|) checked publication boundary.
// Internal batch APIs accept this object instead of a bool/tag, making it
// impossible to select a trusted path without first proving compatibility.
// It is deliberately non-owning and is valid only while both referenced
// objects remain at stable addresses and the grammar snapshot is immutable.
class checked_chart_execution_plan_ref {
 public:
  checked_chart_execution_plan_ref(checked_chart_execution_plan_ref const&) =
      default;
  checked_chart_execution_plan_ref& operator=(
      checked_chart_execution_plan_ref const&) = default;

  [[nodiscard]] clade_grammar const& grammar() const noexcept {
    return *grammar_;
  }
  [[nodiscard]] chart_execution_plan const& plan() const noexcept {
    return *plan_;
  }

  // O(1) identity/generation guard for trusted callees.  Same-generation
  // in-place mutation is caught when this capability is minted; callers must
  // not mutate the published snapshot during its scoped lifetime.
  void assert_same(clade_grammar const& grammar,
                   chart_execution_plan const& plan) const {
    if (&grammar != grammar_ || &plan != plan_ ||
        grammar.execution_generation != grammar_generation_ ||
        plan.grammar_generation() != grammar_generation_ ||
        plan.fingerprint() != fingerprint_) {
      throw chart_execution_plan_mismatch(
          "chart execution plan: checked snapshot identity mismatch");
    }
  }

 private:
  friend checked_chart_execution_plan_ref check_chart_execution_plan(
      clade_grammar const&, chart_execution_plan const&);

  checked_chart_execution_plan_ref(clade_grammar const& grammar,
                                   chart_execution_plan const& plan)
      : grammar_(&grammar),
        plan_(&plan),
        grammar_generation_(grammar.execution_generation),
        fingerprint_(plan.fingerprint()) {}

  clade_grammar const* grammar_ = nullptr;
  chart_execution_plan const* plan_ = nullptr;
  std::uint64_t grammar_generation_ = 0;
  chart_plan_fingerprint fingerprint_{};
};

inline checked_chart_execution_plan_ref check_chart_execution_plan(
    clade_grammar const& grammar, chart_execution_plan const& plan) {
  plan.assert_compatible(grammar);
  return checked_chart_execution_plan_ref{grammar, plan};
}

inline chart_execution_plan build_chart_execution_plan(
    clade_grammar const& grammar) {
  if (grammar.execution_generation == 0) {
    throw std::runtime_error(
        "chart execution plan: grammar has no published execution "
        "generation");
  }

  chart_execution_plan result;
  ++result.build_stats_.plan_builds;
  chart_execution_plan_detail::validate_chart_execution_plan_input(
      grammar, result.build_stats_);

  result.grammar_generation_ = grammar.execution_generation;
  result.fingerprint_ =
      chart_execution_plan_detail::fingerprint_chart_grammar(grammar);
  result.taxon_count_ = grammar.taxa.id_to_sample_id.size();
  result.root_clade_ = grammar.root_clade;
  result.clades_.resize(grammar.clades.size());
  result.productions_.resize(grammar.productions.size());

  std::size_t child_count = 0;
  for (auto const& production : grammar.productions) {
    child_count += production.children.size();
  }
  result.children_.reserve(child_count);
  result.productions_by_parent_.reserve(grammar.productions.size());
  result.child_occurrences_.reserve(child_count);

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto& descriptor = result.clades_[cid];
    descriptor.source_id = static_cast<clade_id>(cid);
    descriptor.taxa_size = grammar.clades[cid].taxa.size();
    if (descriptor.taxa_size == 1) {
      descriptor.leaf_taxon = grammar.clades[cid].taxa.front();
    }
    descriptor.parent_production_begin = result.productions_by_parent_.size();
    descriptor.parent_production_count =
        grammar.productions_by_parent[cid].size();
    result.productions_by_parent_.insert(
        result.productions_by_parent_.end(),
        grammar.productions_by_parent[cid].begin(),
        grammar.productions_by_parent[cid].end());
  }

  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& production = grammar.productions[pid];
    auto& descriptor = result.productions_[pid];
    descriptor.source_id = static_cast<production_id>(pid);
    descriptor.parent = production.parent;
    descriptor.child_begin = result.children_.size();
    descriptor.child_count = production.children.size();
    if (descriptor.child_count == 2) {
      descriptor.binary_children = {production.children[0],
                                    production.children[1]};
    } else {
      result.all_binary_ = false;
    }
    result.max_arity_ = std::max(result.max_arity_, descriptor.child_count);
    result.children_.insert(result.children_.end(), production.children.begin(),
                            production.children.end());
    ++result.build_stats_.production_descriptors_compiled;
  }

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    auto& descriptor = result.clades_[cid];
    descriptor.child_occurrence_begin = result.child_occurrences_.size();
    descriptor.child_occurrence_count =
        grammar.productions_by_child[cid].size();
    for (auto pid : grammar.productions_by_child[cid]) {
      auto children = result.children(pid);
      auto found = std::find(children.begin(), children.end(), cid);
      if (found == children.end()) {
        throw std::runtime_error(
            "chart execution plan: child occurrence missing from production");
      }
      result.child_occurrences_.push_back(
          {pid, static_cast<std::size_t>(found - children.begin())});
    }
  }

  result.bottom_up_order_.resize(grammar.clades.size());
  std::iota(result.bottom_up_order_.begin(), result.bottom_up_order_.end(),
            clade_id{0});
  std::stable_sort(result.bottom_up_order_.begin(),
                   result.bottom_up_order_.end(),
                   [&](clade_id lhs, clade_id rhs) {
                     auto const lsize = grammar.clades[lhs].taxa.size();
                     auto const rsize = grammar.clades[rhs].taxa.size();
                     if (lsize != rsize) return lsize < rsize;
                     return lhs < rhs;
                   });
  ++result.build_stats_.clade_order_sorts;

  result.top_down_order_.resize(grammar.clades.size());
  std::iota(result.top_down_order_.begin(), result.top_down_order_.end(),
            clade_id{0});
  std::stable_sort(result.top_down_order_.begin(), result.top_down_order_.end(),
                   [&](clade_id lhs, clade_id rhs) {
                     auto const lsize = grammar.clades[lhs].taxa.size();
                     auto const rsize = grammar.clades[rhs].taxa.size();
                     if (lsize != rsize) return lsize > rsize;
                     return lhs < rhs;
                   });
  ++result.build_stats_.clade_order_sorts;

  std::size_t max_level = 0;
  for (auto cid : result.bottom_up_order_) {
    auto& clade = result.clades_[cid];
    if (clade.is_leaf()) continue;
    std::size_t level = 0;
    for (auto pid : result.productions_for_parent(cid)) {
      for (auto child : result.children(pid)) {
        level = std::max(level, result.clades_[child].dependency_level + 1);
      }
    }
    clade.dependency_level = level;
    max_level = std::max(max_level, level);
  }

  std::vector<std::vector<clade_id>> levels(max_level + 1);
  for (clade_id cid = 0; cid < result.clades_.size(); ++cid) {
    levels[result.clades_[cid].dependency_level].push_back(cid);
  }
  result.bottom_up_level_offsets_.push_back(0);
  for (auto const& level : levels) {
    result.bottom_up_level_order_.insert(result.bottom_up_level_order_.end(),
                                         level.begin(), level.end());
    result.bottom_up_level_offsets_.push_back(
        result.bottom_up_level_order_.size());
  }
  result.top_down_level_offsets_.push_back(0);
  for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
    result.top_down_level_order_.insert(result.top_down_level_order_.end(),
                                        it->begin(), it->end());
    result.top_down_level_offsets_.push_back(
        result.top_down_level_order_.size());
  }

  // Count production-distinct upward paths without retaining any path
  // objects. The top-down order visits every parent before its children;
  // saturation lets finite consumers fail closed if the exact count is not
  // representable while keeping plan construction total.
  result.clades_[result.root_clade_].upward_path_count = 1;
  for (auto parent : result.top_down_order_) {
    auto const parent_paths = result.clades_[parent].upward_path_count;
    for (auto pid : result.productions_for_parent(parent)) {
      for (auto child : result.children(pid)) {
        auto& child_paths = result.clades_[child].upward_path_count;
        if (child_paths >
            (std::numeric_limits<std::size_t>::max)() - parent_paths) {
          child_paths = (std::numeric_limits<std::size_t>::max)();
        } else {
          child_paths += parent_paths;
        }
      }
    }
  }

  for (std::uint8_t parent = 0; parent < 4; ++parent) {
    for (std::uint8_t child = 0; child < 4; ++child) {
      result.transition_costs_[parent][child] = parent == child ? 0 : 1;
    }
  }
  result.valid_ = true;
  return result;
}

}  // namespace larch
