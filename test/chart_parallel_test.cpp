#include <larch/chart_execution_plan.hpp>
#include <larch/chart_trim.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/parsimony_chart.hpp>

#include <array>
#include <cstdint>
#include <print>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr)                                    \
  do {                                                 \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

template <class Fn>
static std::string runtime_error_message(Fn&& fn) {
  try {
    fn();
  } catch (std::runtime_error const& error) {
    return error.what();
  }
  CHECK(false && "expected std::runtime_error");
  return {};
}

static larch::clade_grammar make_binary_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};
  grammar.taxa.sample_id_to_id = {{"A", taxon_id{0}},
                                  {"B", taxon_id{1}},
                                  {"C", taxon_id{2}},
                                  {"D", taxon_id{3}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}}},
                    {{taxon_id{2}, taxon_id{3}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}, taxon_id{3}}}};
  grammar.productions = {
      larch::grammar_production{clade_id{4}, {clade_id{0}, clade_id{1}}, {}, 1},
      larch::grammar_production{clade_id{5}, {clade_id{2}, clade_id{3}}, {}, 1},
      larch::grammar_production{
          clade_id{6}, {clade_id{4}, clade_id{5}}, {}, 1}};
  grammar.productions_by_parent = {{},
                                   {},
                                   {},
                                   {},
                                   {production_id{0}},
                                   {production_id{1}},
                                   {production_id{2}}};
  grammar.productions_by_child = {{production_id{0}},
                                  {production_id{0}},
                                  {production_id{1}},
                                  {production_id{1}},
                                  {production_id{2}},
                                  {production_id{2}},
                                  {}};
  grammar.root_clade = clade_id{6};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::clade_grammar make_trinary_grammar() {
  using larch::clade_id;
  using larch::production_id;
  using larch::taxon_id;

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C"};
  grammar.taxa.sample_id_to_id = {
      {"A", taxon_id{0}}, {"B", taxon_id{1}}, {"C", taxon_id{2}}};
  grammar.clades = {{{taxon_id{0}}},
                    {{taxon_id{1}}},
                    {{taxon_id{2}}},
                    {{taxon_id{0}, taxon_id{1}, taxon_id{2}}}};
  grammar.productions = {larch::grammar_production{
      clade_id{3}, {clade_id{0}, clade_id{1}, clade_id{2}}, {}, 1}};
  grammar.productions_by_parent = {{}, {}, {}, {production_id{0}}};
  grammar.productions_by_child = {
      {production_id{0}}, {production_id{0}}, {production_id{0}}, {}};
  grammar.root_clade = clade_id{3};
  grammar.execution_generation =
      larch::detail::allocate_clade_grammar_execution_generation();
  return grammar;
}

static larch::site_pattern_set make_weighted_patterns(
    std::size_t taxon_count) {
  larch::site_pattern_set patterns;
  patterns.taxon_count = taxon_count;
  if (taxon_count == 4) {
    patterns.patterns = {
        larch::site_pattern{.state_by_taxon = {0, 0, 1, 1},
                            .weight = 2,
                            .reference_state_counts = {1, 1, 0, 0}},
        larch::site_pattern{.state_by_taxon = {0, 1, 0, 1},
                            .weight = 1,
                            .reference_state_counts = {0, 0, 1, 0}},
        larch::site_pattern{.state_by_taxon = {3, 2, 1, 0},
                            .weight = 3,
                            .reference_state_counts = {0, 1, 0, 2}}};
  } else if (taxon_count == 3) {
    patterns.patterns = {
        larch::site_pattern{.state_by_taxon = {0, 1, 2},
                            .weight = 2,
                            .reference_state_counts = {1, 0, 1, 0}},
        larch::site_pattern{.state_by_taxon = {3, 3, 0},
                            .weight = 1,
                            .reference_state_counts = {0, 0, 0, 1}}};
  } else {
    CHECK(false && "unsupported test pattern taxon count");
  }
  return patterns;
}

static void check_charts_equal(larch::single_site_chart const& expected,
                               larch::single_site_chart const& actual) {
  CHECK(expected.inside == actual.inside);
  CHECK(expected.trace_choice_count == actual.trace_choice_count);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
  CHECK(expected.optimal_choices.size() == actual.optimal_choices.size());
  for (std::size_t clade = 0; clade < expected.optimal_choices.size();
       ++clade) {
    for (std::size_t state = 0; state < larch::nuc_state_count; ++state) {
      auto const& lhs = expected.optimal_choices[clade][state];
      auto const& rhs = actual.optimal_choices[clade][state];
      CHECK(lhs.size() == rhs.size());
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        CHECK(lhs[i].production == rhs[i].production);
        CHECK(lhs[i].child_states == rhs[i].child_states);
        CHECK(lhs[i].cost == rhs[i].cost);
      }
    }
  }
}

static void check_outside_charts_equal(
    larch::single_site_outside_chart const& expected,
    larch::single_site_outside_chart const& actual) {
  CHECK(expected.outside == actual.outside);
  CHECK(expected.global_min == actual.global_min);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
}

static void check_composite_scores_equal(
    larch::composite_chart_score const& expected,
    larch::composite_chart_score const& actual) {
  CHECK(expected.weighted_lower_bound == actual.weighted_lower_bound);
  CHECK(expected.per_pattern_root_min == actual.per_pattern_root_min);
  CHECK(expected.per_pattern_root_min_by_reference_state ==
        actual.per_pattern_root_min_by_reference_state);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
}

static void check_lazy_charts_equal(
    larch::lazy_multisite_chart const& expected,
    larch::lazy_multisite_chart const& actual) {
  CHECK(expected.inside_rows_by_clade == actual.inside_rows_by_clade);
  CHECK(expected.outside_rows_by_clade == actual.outside_rows_by_clade);
  CHECK(expected.class_index_by_pattern_by_clade ==
        actual.class_index_by_pattern_by_clade);
  CHECK(expected.structural_class_index_by_pattern_by_clade ==
        actual.structural_class_index_by_pattern_by_clade);
  CHECK(expected.outside_class_index_by_pattern_by_clade ==
        actual.outside_class_index_by_pattern_by_clade);
  CHECK(expected.structural_class_count_by_clade ==
        actual.structural_class_count_by_clade);
  CHECK(expected.class_weight_by_clade == actual.class_weight_by_clade);
  CHECK(expected.outside_class_weight_by_clade ==
        actual.outside_class_weight_by_clade);
  CHECK(expected.outside_global_min_by_pattern ==
        actual.outside_global_min_by_pattern);
  CHECK(expected.pattern_count == actual.pattern_count);
  CHECK(expected.total_pattern_weight == actual.total_pattern_weight);
  CHECK(expected.lazy_inside_rows_computed ==
        actual.lazy_inside_rows_computed);
  CHECK(expected.lazy_outside_rows_computed ==
        actual.lazy_outside_rows_computed);
  CHECK(expected.lazy_patterns_merged_max ==
        actual.lazy_patterns_merged_max);
  CHECK(expected.lazy_remerge_collisions == actual.lazy_remerge_collisions);
  CHECK(expected.lazy_structural_class_count_max ==
        actual.lazy_structural_class_count_max);
  CHECK(expected.multifurcation_productions_scored ==
        actual.multifurcation_productions_scored);
  CHECK(expected.outside_multifurcation_productions_scored ==
        actual.outside_multifurcation_productions_scored);
}

static void check_multisite_trim_equal(
    larch::multisite_trim_result const& expected,
    larch::multisite_trim_result const& actual) {
  CHECK(expected.optimum == actual.optimum);
  CHECK(expected.composite_lower_bound == actual.composite_lower_bound);
  CHECK(expected.initial_upper_bound == actual.initial_upper_bound);
  CHECK(expected.keep_production == actual.keep_production);
  CHECK(expected.frontier_sizes_by_clade == actual.frontier_sizes_by_clade);
  CHECK(expected.dominance_mode == actual.dominance_mode);
  CHECK(expected.keep_mask_kind == actual.keep_mask_kind);
  CHECK(expected.keep_production_exact == actual.keep_production_exact);
  CHECK(expected.dominance_candidates_considered ==
        actual.dominance_candidates_considered);
  CHECK(expected.dominance_pruned_score_pass ==
        actual.dominance_pruned_score_pass);
  CHECK(expected.dominance_pruned_mask_pass ==
        actual.dominance_pruned_mask_pass);
  CHECK(expected.dominance_pruned == actual.dominance_pruned);
  CHECK(expected.exact_mask_recovery_passes ==
        actual.exact_mask_recovery_passes);
  CHECK(expected.bound_pruned == actual.bound_pruned);
  CHECK(expected.equality_deduplicated == actual.equality_deduplicated);
  CHECK(expected.active_pattern_count == actual.active_pattern_count);
  CHECK(expected.invariant_constant_offset ==
        actual.invariant_constant_offset);
  CHECK(expected.lazy_chart_used == actual.lazy_chart_used);
  CHECK(expected.lazy_inside_rows_computed ==
        actual.lazy_inside_rows_computed);
  CHECK(expected.lazy_outside_rows_computed ==
        actual.lazy_outside_rows_computed);
  CHECK(expected.lazy_patterns_merged_max ==
        actual.lazy_patterns_merged_max);
  CHECK(expected.lazy_remerge_collisions == actual.lazy_remerge_collisions);
  CHECK(expected.lazy_structural_class_count_max ==
        actual.lazy_structural_class_count_max);
  CHECK(expected.lazy_structural_class_count_by_clade ==
        actual.lazy_structural_class_count_by_clade);
  CHECK(expected.optimal_root_provenance_classes ==
        actual.optimal_root_provenance_classes);
}

static void test_execution_plan_binary_and_generic_invariants() {
  std::println("test_execution_plan_binary_and_generic_invariants");
  auto binary = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(binary);

  CHECK(binary.execution_generation != 0);
  CHECK(plan.grammar_generation() == binary.execution_generation);
  CHECK(plan.root_clade() == binary.root_clade);
  CHECK(plan.taxon_count() == 4);
  CHECK(plan.all_binary());
  CHECK(plan.max_arity() == 2);
  CHECK(plan.build_stats().plan_builds == 1);
  CHECK(plan.build_stats().full_grammar_validations == 1);
  CHECK(plan.build_stats().production_partition_validations ==
        binary.productions.size());
  CHECK(plan.build_stats().clade_order_sorts == 2);
  CHECK(plan.build_stats().production_descriptors_compiled ==
        binary.productions.size());
  CHECK(std::vector<larch::clade_id>(plan.bottom_up_order().begin(),
                                     plan.bottom_up_order().end()) ==
        std::vector<larch::clade_id>({0, 1, 2, 3, 4, 5, 6}));
  CHECK(std::vector<larch::clade_id>(plan.top_down_order().begin(),
                                     plan.top_down_order().end()) ==
        std::vector<larch::clade_id>({6, 4, 5, 0, 1, 2, 3}));

  std::vector<bool> seen(plan.clades().size(), false);
  for (auto cid : plan.bottom_up_level_order()) {
    CHECK(cid < seen.size());
    CHECK(!seen[cid]);
    seen[cid] = true;
  }
  for (bool value : seen) CHECK(value);
  for (auto const& production : plan.productions()) {
    CHECK(production.source_id < binary.productions.size());
    CHECK(production.parent == binary.productions[production.source_id].parent);
    auto children = plan.children(production.source_id);
    CHECK(std::vector<larch::clade_id>(children.begin(), children.end()) ==
          binary.productions[production.source_id].children);
    for (auto child : children) {
      CHECK(plan.clade(child).dependency_level <
            plan.clade(production.parent).dependency_level);
    }
  }
  for (larch::clade_id child = 0; child < plan.clades().size(); ++child) {
    auto occurrences = plan.child_occurrences_for_clade(child);
    CHECK(occurrences.size() == binary.productions_by_child[child].size());
    for (std::size_t i = 0; i < occurrences.size(); ++i) {
      auto const& occurrence = occurrences[i];
      CHECK(occurrence.production == binary.productions_by_child[child][i]);
      auto children = plan.children(occurrence.production);
      CHECK(occurrence.child_slot < children.size());
      CHECK(children[occurrence.child_slot] == child);
    }
  }
  for (std::uint8_t parent = 0; parent < 4; ++parent) {
    for (std::uint8_t child = 0; child < 4; ++child) {
      CHECK(plan.transition_cost(parent, child) == (parent == child ? 0 : 1));
    }
  }

  auto trinary = make_trinary_grammar();
  auto generic = larch::build_chart_execution_plan(trinary);
  CHECK(!generic.all_binary());
  CHECK(generic.max_arity() == 3);
  CHECK(generic.production(0).child_count == 3);
  CHECK(std::vector<larch::clade_id>(generic.children(0).begin(),
                                     generic.children(0).end()) ==
        std::vector<larch::clade_id>({0, 1, 2}));
  for (larch::clade_id child = 0; child < 3; ++child) {
    auto occurrences = generic.child_occurrences_for_clade(child);
    CHECK(occurrences.size() == 1);
    CHECK(occurrences.front().production == 0);
    CHECK(occurrences.front().child_slot == child);
  }
}

static void test_checked_and_plan_dense_equivalence() {
  std::println("test_checked_and_plan_dense_equivalence");
  auto binary = make_binary_grammar();
  auto binary_plan = larch::build_chart_execution_plan(binary);
  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};
  for (std::uint32_t mask = 0; mask < 256; ++mask) {
    larch::leaf_site_states states;
    states.state_by_taxon.resize(4);
    auto value = mask;
    for (auto& state : states.state_by_taxon) {
      state = static_cast<std::uint8_t>(value & 3U);
      value >>= 2U;
    }
    auto checked = larch::build_single_site_chart(binary, states, false);
    larch::single_site_chart planned;
    larch::single_site_chart planned_from_view;
    auto const* state_storage = states.state_by_taxon.data();
    auto state_view = larch::view_leaf_site_states(states);
    CHECK(state_view.state_by_taxon.data() == state_storage);
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned = larch::build_single_site_chart(binary_plan, states, false);
      planned_from_view =
          larch::build_single_site_chart(binary_plan, state_view, false);
    }
    check_charts_equal(checked, planned);
    check_charts_equal(checked, planned_from_view);
    CHECK(states.state_by_taxon.data() == state_storage);

    auto checked_outside =
        larch::build_single_site_outside_chart(binary, checked);
    larch::single_site_outside_chart planned_outside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_outside =
          larch::build_single_site_outside_chart(binary_plan, planned);
    }
    check_outside_charts_equal(checked_outside, planned_outside);
    for (std::uint8_t reference = 0; reference < 4; ++reference) {
      larch::chart_options root_edge_options;
      root_edge_options.score_ua_edge = true;
      auto checked_with_reference = larch::build_single_site_outside_chart(
          binary, checked, root_edge_options, reference);
      larch::single_site_outside_chart planned_with_reference;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned_with_reference = larch::build_single_site_outside_chart(
            binary_plan, planned, root_edge_options, reference);
      }
      check_outside_charts_equal(checked_with_reference,
                                 planned_with_reference);
    }

    larch::chart_options trace_options;
    trace_options.keep_trace = true;
    auto checked_trace =
        larch::build_single_site_chart(binary, states, trace_options);
    larch::single_site_chart planned_trace;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_trace =
          larch::build_single_site_chart(binary_plan, states, trace_options);
    }
    check_charts_equal(checked_trace, planned_trace);
  }

  auto trinary = make_trinary_grammar();
  auto trinary_plan = larch::build_chart_execution_plan(trinary);
  for (std::uint32_t mask = 0; mask < 64; ++mask) {
    larch::leaf_site_states states;
    states.state_by_taxon.resize(3);
    auto value = mask;
    for (auto& state : states.state_by_taxon) {
      state = static_cast<std::uint8_t>(value & 3U);
      value >>= 2U;
    }
    auto checked = larch::build_single_site_chart(trinary, states, false);
    larch::single_site_chart planned;
    larch::single_site_chart planned_from_view;
    auto const* state_storage = states.state_by_taxon.data();
    auto state_view = larch::view_leaf_site_states(states);
    CHECK(state_view.state_by_taxon.data() == state_storage);
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned = larch::build_single_site_chart(trinary_plan, states, false);
      planned_from_view =
          larch::build_single_site_chart(trinary_plan, state_view, false);
    }
    check_charts_equal(checked, planned);
    check_charts_equal(checked, planned_from_view);
    CHECK(states.state_by_taxon.data() == state_storage);
    auto checked_outside =
        larch::build_single_site_outside_chart(trinary, checked);
    larch::single_site_outside_chart planned_outside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_outside =
          larch::build_single_site_outside_chart(trinary_plan, planned);
    }
    check_outside_charts_equal(checked_outside, planned_outside);
  }
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_stale_and_mismatched_plan_rejected() {
  std::println("test_stale_and_mismatched_plan_rejected");
  auto first = make_binary_grammar();
  auto second = make_binary_grammar();
  auto first_plan = larch::build_chart_execution_plan(first);
  auto second_plan = larch::build_chart_execution_plan(second);
  CHECK(first_plan.grammar_generation() != second_plan.grammar_generation());

  auto mismatch =
      runtime_error_message([&] { first_plan.assert_compatible(second); });
  CHECK(mismatch.find("chart execution plan") != std::string::npos);
  CHECK(mismatch.find("generation mismatch") != std::string::npos);

  std::swap(first.productions[0].children[0], first.productions[0].children[1]);
  auto stale =
      runtime_error_message([&] { first_plan.assert_compatible(first); });
  CHECK(stale.find("chart execution plan") != std::string::npos);
  CHECK(stale.find("fingerprint mismatch") != std::string::npos);

  auto reverse_registry = make_binary_grammar();
  auto reverse_registry_plan =
      larch::build_chart_execution_plan(reverse_registry);
  reverse_registry.taxa.sample_id_to_id["A"] = 1;
  auto reverse_stale = runtime_error_message(
      [&] { reverse_registry_plan.assert_compatible(reverse_registry); });
  CHECK(reverse_stale.find("fingerprint mismatch") != std::string::npos);
}

static void test_checked_and_plan_composite_equivalence() {
  std::println("test_checked_and_plan_composite_equivalence");
  for (bool trinary : {false, true}) {
    auto grammar = trinary ? make_trinary_grammar() : make_binary_grammar();
    auto plan = larch::build_chart_execution_plan(grammar);
    auto patterns = make_weighted_patterns(plan.taxon_count());
    for (bool score_ua_edge : {false, true}) {
      larch::chart_options options;
      options.score_ua_edge = score_ua_edge;
      auto checked =
          larch::build_composite_chart_score(grammar, patterns, options);
      std::size_t full_validations = 0;
      std::size_t partition_validations = 0;
      std::size_t clade_sorts = 0;
      larch::parsimony_chart_detail::structural_work_observer observer{
          &full_validations, &partition_validations, &clade_sorts};
      larch::composite_chart_score planned;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned =
            larch::build_composite_chart_score(plan, patterns, options);
      }
      check_composite_scores_equal(checked, planned);
      CHECK(full_validations == 0);
      CHECK(partition_validations == 0);
      CHECK(clade_sorts == 0);
    }
  }
}

static void test_checked_and_plan_lazy_inside_outside_equivalence() {
  std::println("test_checked_and_plan_lazy_inside_outside_equivalence");
  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};

  for (bool retain_all : {false, true}) {
    larch::lazy_chart_options lazy_options;
    lazy_options.retain_all_inside_class_maps = retain_all;
    auto checked_inside =
        larch::build_lazy_inside_chart(grammar, patterns, lazy_options);
    larch::lazy_multisite_chart planned_inside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_inside =
          larch::build_lazy_inside_chart(plan, patterns, lazy_options);
    }
    check_lazy_charts_equal(checked_inside, planned_inside);

    for (larch::clade_id clade = 0; clade < plan.clades().size(); ++clade) {
      auto const should_retain = retain_all || clade == plan.root_clade();
      CHECK(planned_inside.class_index_by_pattern_by_clade[clade].has_value() ==
            should_retain);
      CHECK(planned_inside
                .structural_class_index_by_pattern_by_clade[clade]
                .has_value() == should_retain);
    }

    auto checked_outside = larch::build_lazy_outside_chart(
        grammar, patterns, checked_inside, larch::chart_options{});
    larch::lazy_multisite_chart planned_outside;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_outside = larch::build_lazy_outside_chart(
          plan, patterns, planned_inside, larch::chart_options{});
    }
    check_lazy_charts_equal(checked_outside, planned_outside);
    for (auto const& class_map :
         planned_outside.outside_class_index_by_pattern_by_clade) {
      CHECK(class_map.has_value());
    }

    larch::chart_options reference_options;
    reference_options.score_ua_edge = true;
    for (std::uint8_t reference_state = 0;
         reference_state < larch::nuc_state_count; ++reference_state) {
      auto checked_with_reference = larch::build_lazy_outside_chart(
          grammar, patterns, checked_inside, reference_options,
          reference_state);
      larch::lazy_multisite_chart planned_with_reference;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned_with_reference = larch::build_lazy_outside_chart(
            plan, patterns, planned_inside, reference_options,
            reference_state);
      }
      check_lazy_charts_equal(checked_with_reference,
                              planned_with_reference);
    }
  }

  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_checked_and_plan_lazy_score_and_exact_trim_equivalence() {
  std::println(
      "test_checked_and_plan_lazy_score_and_exact_trim_equivalence");
  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};

  for (bool retain_all : {false, true}) {
    larch::lazy_chart_options lazy_options;
    lazy_options.retain_all_inside_class_maps = retain_all;
    auto checked_lazy =
        larch::build_lazy_inside_chart(grammar, patterns, lazy_options);
    larch::lazy_multisite_chart planned_lazy;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_lazy = larch::build_lazy_inside_chart(plan, patterns,
                                                    lazy_options);
    }

    for (bool score_ua_edge : {false, true}) {
      larch::chart_options options;
      options.score_ua_edge = score_ua_edge;
      auto checked_score = larch::lazy_composite_chart_score(
          grammar, patterns, checked_lazy, options);
      auto const checked_lower_bound = larch::lazy_composite_lower_bound(
          grammar, patterns, checked_lazy, options);
      larch::composite_chart_score planned_score;
      std::uint64_t planned_lower_bound = 0;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned_score = larch::lazy_composite_chart_score(
            plan, patterns, planned_lazy, options);
        planned_lower_bound = larch::lazy_composite_lower_bound(
            plan, patterns, planned_lazy, options);
      }
      check_composite_scores_equal(checked_score, planned_score);
      CHECK(checked_lower_bound == planned_lower_bound);
      CHECK(planned_lower_bound == planned_score.weighted_lower_bound);
    }
  }

  auto checked_lazy = larch::build_lazy_inside_chart(grammar, patterns);
  larch::lazy_multisite_chart planned_lazy;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_lazy = larch::build_lazy_inside_chart(plan, patterns);
  }
  larch::multisite_trim_options trim_options;
  trim_options.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  trim_options.use_bound_pruning = true;
  trim_options.capture_optimal_root_provenance = true;
  for (bool score_ua_edge : {false, true}) {
    larch::chart_options options;
    options.score_ua_edge = score_ua_edge;
    auto checked_trim = larch::build_multisite_trim(
        grammar, patterns, checked_lazy, options, trim_options);
    larch::multisite_trim_result planned_trim;
    larch::multisite_trim_result bridged_trim;
    {
      larch::parsimony_chart_detail::structural_work_observer_scope scope{
          &observer};
      planned_trim = larch::build_multisite_trim(
          plan, patterns, planned_lazy, options, trim_options);
      bridged_trim = larch::build_multisite_trim(
          grammar, plan, patterns, planned_lazy, options, trim_options);
    }
    check_multisite_trim_equal(checked_trim, planned_trim);
    check_multisite_trim_equal(checked_trim, bridged_trim);
    CHECK(planned_trim.lazy_chart_used);
    CHECK(!planned_trim.optimal_root_provenance_classes.empty());
  }

  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_checked_and_plan_lazy_multifurcation_equivalence() {
  std::println("test_checked_and_plan_lazy_multifurcation_equivalence");
  auto grammar = make_trinary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_validations, &partition_validations, &clade_sorts};

  auto checked_inside = larch::build_lazy_inside_chart(grammar, patterns);
  larch::lazy_multisite_chart planned_inside;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_inside = larch::build_lazy_inside_chart(plan, patterns);
  }
  check_lazy_charts_equal(checked_inside, planned_inside);
  CHECK(planned_inside.multifurcation_productions_scored > 0);

  auto checked_outside = larch::build_lazy_outside_chart(
      grammar, patterns, checked_inside, larch::chart_options{});
  larch::lazy_multisite_chart planned_outside;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_outside = larch::build_lazy_outside_chart(
        plan, patterns, planned_inside, larch::chart_options{});
  }
  check_lazy_charts_equal(checked_outside, planned_outside);
  CHECK(planned_outside.outside_multifurcation_productions_scored > 0);

  auto const checked_error = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, checked_inside);
  });
  std::string planned_error;
  std::string bridged_error;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    planned_error = runtime_error_message([&] {
      (void)larch::build_multisite_trim(plan, patterns, planned_inside);
    });
    bridged_error = runtime_error_message([&] {
      (void)larch::build_multisite_trim(grammar, plan, patterns,
                                        planned_inside);
    });
  }
  CHECK(checked_error.find("WI6 arity gate") != std::string::npos);
  CHECK(planned_error == checked_error);
  CHECK(bridged_error == checked_error);

  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
}

static void test_checked_and_plan_exact_frontier_equivalence() {
  std::println("test_checked_and_plan_exact_frontier_equivalence");
  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto patterns = make_weighted_patterns(plan.taxon_count());

  for (auto mode : {larch::multisite_dominance_mode::off,
                    larch::multisite_dominance_mode::score_only,
                    larch::multisite_dominance_mode::strict_mask_safe,
                    larch::multisite_dominance_mode::two_pass_exact_mask}) {
    for (bool bound_pruning : {false, true}) {
      larch::multisite_trim_options trim_options;
      trim_options.dominance_mode = mode;
      trim_options.use_bound_pruning = bound_pruning;
      trim_options.require_exact_keep_mask =
          mode != larch::multisite_dominance_mode::score_only;
      auto checked = larch::build_multisite_trim(
          grammar, patterns, larch::chart_options{}, trim_options);

      std::size_t full_validations = 0;
      std::size_t partition_validations = 0;
      std::size_t clade_sorts = 0;
      larch::parsimony_chart_detail::structural_work_observer observer{
          &full_validations, &partition_validations, &clade_sorts};
      larch::multisite_trim_result planned;
      {
        larch::parsimony_chart_detail::structural_work_observer_scope scope{
            &observer};
        planned = larch::build_multisite_trim(
            plan, patterns, larch::chart_options{}, trim_options);
      }
      check_multisite_trim_equal(checked, planned);
      CHECK(full_validations == 0);
      CHECK(partition_validations == 0);
      CHECK(clade_sorts == 0);
    }
  }

  larch::multisite_trim_options provenance_options;
  provenance_options.capture_optimal_root_provenance = true;
  auto checked = larch::build_multisite_trim(
      grammar, patterns, larch::chart_options{}, provenance_options);
  auto planned = larch::build_multisite_trim(
      plan, patterns, larch::chart_options{}, provenance_options);
  check_multisite_trim_equal(checked, planned);
  CHECK(!planned.optimal_root_provenance_classes.empty());

  auto trinary = make_trinary_grammar();
  auto trinary_plan = larch::build_chart_execution_plan(trinary);
  auto trinary_patterns = make_weighted_patterns(3);
  auto checked_error = runtime_error_message([&] {
    (void)larch::build_multisite_trim(trinary, trinary_patterns);
  });
  auto planned_error = runtime_error_message([&] {
    (void)larch::build_multisite_trim(trinary_plan, trinary_patterns);
  });
  CHECK(checked_error.find("WI6 arity gate") != std::string::npos);
  CHECK(planned_error.find("WI6 arity gate") != std::string::npos);
}

static void test_plan_lifetime_and_uninitialized_guards() {
  std::println("test_plan_lifetime_and_uninitialized_guards");
  larch::chart_execution_plan empty;
  CHECK(!empty.valid());
  auto uninitialized = runtime_error_message([&] {
    (void)larch::build_single_site_chart(empty, larch::leaf_site_states{},
                                         larch::chart_options{});
  });
  CHECK(uninitialized.find("uninitialized plan") != std::string::npos);

  auto grammar = make_binary_grammar();
  auto plan = larch::build_chart_execution_plan(grammar);
  auto grammar_copy = grammar;
  plan.assert_compatible(grammar_copy);
  auto copied_plan = plan;
  auto moved_plan = std::move(copied_plan);
  CHECK(moved_plan.valid());
  CHECK(!copied_plan.valid());
  moved_plan.assert_compatible(grammar_copy);

  grammar_copy.productions[0].children = {1, 0};
  auto diverged = runtime_error_message(
      [&] { moved_plan.assert_compatible(grammar_copy); });
  CHECK(diverged.find("fingerprint mismatch") != std::string::npos);

  auto unpublished = make_binary_grammar();
  unpublished.execution_generation = 0;
  auto unpublished_error = runtime_error_message(
      [&] { (void)larch::build_chart_execution_plan(unpublished); });
  CHECK(unpublished_error.find("no published execution generation") !=
        std::string::npos);
  CHECK(unpublished.execution_generation == 0);
}

static void test_invalid_input_fails_at_plan_construction() {
  std::println("test_invalid_input_fails_at_plan_construction");
  {
    auto grammar = make_trinary_grammar();
    grammar.productions[0].children = {0, 0, 2};
    grammar.productions_by_child = {{0}, {}, {0}, {}};
    auto message = runtime_error_message(
        [&] { (void)larch::build_chart_execution_plan(grammar); });
    CHECK(message.find("chart execution plan: production 0") !=
          std::string::npos);
    CHECK(message.find("not pairwise disjoint") != std::string::npos);
  }
  {
    auto grammar = make_trinary_grammar();
    grammar.productions[0].children = {0, 1};
    grammar.productions_by_child = {{0}, {0}, {}, {}};
    auto message = runtime_error_message(
        [&] { (void)larch::build_chart_execution_plan(grammar); });
    CHECK(message.find("chart execution plan: production 0") !=
          std::string::npos);
    CHECK(message.find("do not union to parent clade") != std::string::npos);
  }
  {
    auto grammar = make_binary_grammar();
    grammar.productions[0].parent = 5;
    auto message = runtime_error_message(
        [&] { (void)larch::build_chart_execution_plan(grammar); });
    CHECK(message.find("chart execution plan") != std::string::npos);
    CHECK(message.find("mismatched parent") != std::string::npos);
  }
}

int main() {
  test_execution_plan_binary_and_generic_invariants();
  test_checked_and_plan_dense_equivalence();
  test_checked_and_plan_composite_equivalence();
  test_checked_and_plan_lazy_inside_outside_equivalence();
  test_checked_and_plan_lazy_score_and_exact_trim_equivalence();
  test_checked_and_plan_lazy_multifurcation_equivalence();
  test_checked_and_plan_exact_frontier_equivalence();
  test_stale_and_mismatched_plan_rejected();
  test_plan_lifetime_and_uninitialized_guards();
  test_invalid_input_fails_at_plan_construction();
  std::println("All chart parallel tests passed!");
}
