#include <larch/chart_trim.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/lazy_chart.hpp>
#include <larch/parsimony_chart.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <optional>
#include <print>
#include <random>
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

static larch::test::tiny_tree_node paper_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("C", "AC"),
                   tiny_inner("DE", "AA",
                              {tiny_leaf("D", "CA"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node paper_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("D", "CA"),
                   tiny_inner("CE", "AA",
                              {tiny_leaf("C", "AC"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node paper_tree1_with_invariant_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAA",
      {tiny_inner("AB", "AAA", {tiny_leaf("A", "AAC"), tiny_leaf("B", "AAC")}),
       tiny_inner(
           "CDE", "AAA",
           {tiny_leaf("C", "ACC"),
            tiny_inner("DE", "AAA",
                       {tiny_leaf("D", "CAC"), tiny_leaf("E", "CCC")})})});
}

static larch::test::tiny_tree_node paper_tree2_with_invariant_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAA",
      {tiny_inner("AB", "AAA", {tiny_leaf("A", "AAC"), tiny_leaf("B", "AAC")}),
       tiny_inner(
           "CDE", "AAA",
           {tiny_leaf("D", "CAC"),
            tiny_inner("CE", "AAA",
                       {tiny_leaf("C", "ACC"), tiny_leaf("E", "CCC")})})});
}

static larch::test::tiny_tree_node concordant_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("C", "AA"),
                   tiny_inner("DE", "AA",
                              {tiny_leaf("D", "CC"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node concordant_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"), tiny_leaf("B", "AA")}),
       tiny_inner("CDE", "AA",
                  {tiny_leaf("D", "CC"),
                   tiny_inner("CE", "AA",
                              {tiny_leaf("C", "AA"), tiny_leaf("E", "CC")})})});
}

static larch::test::tiny_tree_node invariant_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CDE", "A",
                  {tiny_leaf("C", "A"),
                   tiny_inner("DE", "A",
                              {tiny_leaf("D", "A"), tiny_leaf("E", "A")})})});
}

static larch::test::tiny_tree_node invariant_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CDE", "A",
                  {tiny_leaf("D", "A"),
                   tiny_inner("CE", "A",
                              {tiny_leaf("C", "A"), tiny_leaf("E", "A")})})});
}

static larch::test::tiny_tree_node invariant_reference_mismatch_tree1_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "C",
      {tiny_inner("AB", "C", {tiny_leaf("A", "C"), tiny_leaf("B", "C")}),
       tiny_inner("CDE", "C",
                  {tiny_leaf("C", "C"),
                   tiny_inner("DE", "C",
                              {tiny_leaf("D", "C"), tiny_leaf("E", "C")})})});
}

static larch::test::tiny_tree_node invariant_reference_mismatch_tree2_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "C",
      {tiny_inner("AB", "C", {tiny_leaf("A", "C"), tiny_leaf("B", "C")}),
       tiny_inner("CDE", "C",
                  {tiny_leaf("D", "C"),
                   tiny_inner("CE", "C",
                              {tiny_leaf("C", "C"), tiny_leaf("E", "C")})})});
}

static larch::test::tiny_tree_node dominance_dominating_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_leaf("C", "C")});
}

static larch::test::tiny_tree_node dominance_dominated_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_leaf("A", "A"),
       tiny_inner("BC", "A", {tiny_leaf("B", "A"), tiny_leaf("C", "C")})});
}

static larch::test::tiny_tree_node dominance_suboptimal_dominating_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_leaf("A", "A"),
       tiny_inner("BCD", "A",
                  {tiny_leaf("B", "A"),
                   tiny_inner("CD", "A",
                              {tiny_leaf("C", "C"), tiny_leaf("D", "C")})})});
}

static larch::test::tiny_tree_node dominance_suboptimal_dominated_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_leaf("A", "A"),
       tiny_inner(
           "BCD", "A",
           {tiny_inner("BC", "A", {tiny_leaf("B", "A"), tiny_leaf("C", "C")}),
            tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node strict_mask_safe_dominating_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node strict_mask_safe_dominated_tree_spec() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node random_tree_spec(
    std::vector<std::pair<std::string, std::string>> leaves,
    std::string const& reference, std::mt19937& rng, int& inner_id) {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;

  if (leaves.size() == 1) {
    return tiny_leaf(leaves.front().first, leaves.front().second);
  }

  std::shuffle(leaves.begin(), leaves.end(), rng);
  std::uniform_int_distribution<int> split_dist(
      1, static_cast<int>(leaves.size()) - 1);
  auto split = static_cast<std::size_t>(split_dist(rng));
  std::vector<std::pair<std::string, std::string>> left(leaves.begin(),
                                                        leaves.begin() + split);
  std::vector<std::pair<std::string, std::string>> right(leaves.begin() + split,
                                                         leaves.end());

  return tiny_inner(
      "R" + std::to_string(inner_id++), reference,
      {random_tree_spec(std::move(left), reference, rng, inner_id),
       random_tree_spec(std::move(right), reference, rng, inner_id)});
}

static larch::taxon_id taxon_for(larch::clade_grammar const& grammar,
                                 std::string const& sample_id) {
  auto it = grammar.taxa.sample_id_to_id.find(sample_id);
  CHECK(it != grammar.taxa.sample_id_to_id.end());
  return it->second;
}

static larch::clade_id clade_for(larch::clade_grammar const& grammar,
                                 std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  ids.reserve(sample_ids.size());
  for (auto const& sample_id : sample_ids)
    ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa == ids)
      return static_cast<larch::clade_id>(cid);
  }
  CHECK(false && "missing clade");
  return larch::no_clade;
}

static larch::production_id production_id_for(
    larch::clade_grammar const& grammar, larch::clade_id parent,
    std::vector<larch::clade_id> children) {
  std::sort(children.begin(), children.end());
  for (auto pid : grammar.productions_by_parent[parent]) {
    auto prod_children = grammar.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == children) return pid;
  }
  CHECK(false && "missing production");
  return larch::no_production;
}

static bool contains(std::vector<larch::production_id> const& ids,
                     larch::production_id needle) {
  return std::find(ids.begin(), ids.end(), needle) != ids.end();
}

static bool throws_runtime_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

static std::string runtime_error_message(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const& e) {
    return e.what();
  }
  return {};
}

static void check_multisite_trim_results_equal(
    larch::multisite_trim_result const& checked,
    larch::multisite_trim_result const& planned,
    bool compare_exact_setup_work = true) {
  CHECK(planned.optimum == checked.optimum);
  CHECK(planned.composite_lower_bound == checked.composite_lower_bound);
  CHECK(planned.initial_upper_bound == checked.initial_upper_bound);
  CHECK(planned.keep_production == checked.keep_production);
  CHECK(planned.frontier_sizes_by_clade == checked.frontier_sizes_by_clade);
  CHECK(planned.dominance_mode == checked.dominance_mode);
  CHECK(planned.keep_mask_kind == checked.keep_mask_kind);
  CHECK(planned.keep_production_exact == checked.keep_production_exact);
  CHECK(planned.dominance_candidates_considered ==
        checked.dominance_candidates_considered);
  CHECK(planned.dominance_pruned_score_pass ==
        checked.dominance_pruned_score_pass);
  CHECK(planned.dominance_pruned_mask_pass ==
        checked.dominance_pruned_mask_pass);
  CHECK(planned.dominance_pruned == checked.dominance_pruned);
  CHECK(planned.exact_mask_recovery_passes ==
        checked.exact_mask_recovery_passes);
  CHECK(planned.bound_pruned == checked.bound_pruned);
  CHECK(planned.equality_deduplicated == checked.equality_deduplicated);
  CHECK(planned.active_pattern_count == checked.active_pattern_count);
  CHECK(planned.invariant_constant_offset == checked.invariant_constant_offset);
  CHECK(planned.lazy_chart_used == checked.lazy_chart_used);
  CHECK(planned.lazy_inside_rows_computed == checked.lazy_inside_rows_computed);
  CHECK(planned.lazy_outside_rows_computed ==
        checked.lazy_outside_rows_computed);
  CHECK(planned.lazy_patterns_merged_max == checked.lazy_patterns_merged_max);
  CHECK(planned.lazy_remerge_collisions == checked.lazy_remerge_collisions);
  CHECK(planned.lazy_structural_class_count_max ==
        checked.lazy_structural_class_count_max);
  CHECK(planned.lazy_structural_class_count_by_clade ==
        checked.lazy_structural_class_count_by_clade);
  CHECK(planned.optimal_root_provenance_classes ==
        checked.optimal_root_provenance_classes);
  if (compare_exact_setup_work) {
    CHECK(planned.exact_setup_work == checked.exact_setup_work);
  }
}

static larch::multisite_trim_result build_plan_trim_without_structural_work(
    larch::chart_execution_plan const& plan,
    larch::site_pattern_set const& patterns,
    larch::chart_options const& chart_options,
    larch::multisite_trim_options const& trim_options) {
  std::size_t full_grammar_validations = 0;
  std::size_t production_partition_validations = 0;
  std::size_t clade_order_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer observer{
      &full_grammar_validations, &production_partition_validations,
      &clade_order_sorts};

  larch::multisite_trim_result result;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope scope{
        &observer};
    result = larch::build_multisite_trim(plan, patterns, chart_options,
                                         trim_options);
  }
  CHECK(full_grammar_validations == 0);
  CHECK(production_partition_validations == 0);
  CHECK(clade_order_sorts == 0);
  return result;
}

using row_t = std::array<larch::chart_cost, larch::nuc_state_count>;

static larch::chart_cost brute_add(larch::chart_cost lhs,
                                   larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf)
    return larch::chart_inf;
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static row_t brute_inf_row() {
  row_t row{};
  row.fill(larch::chart_inf);
  return row;
}

static row_t brute_leaf_row(std::uint8_t observed) {
  auto row = brute_inf_row();
  row[observed] = 0;
  return row;
}

static row_t brute_combine_multifurcation(std::vector<row_t> const& children) {
  CHECK(!children.empty());
  auto row = brute_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
       ++parent_state) {
    larch::chart_cost total = 0;
    for (auto const& child : children) {
      larch::chart_cost best_child = larch::chart_inf;
      for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
           ++child_state) {
        best_child = std::min(
            best_child,
            brute_add(child[child_state],
                      parent_state == child_state ? 0 : 1));
      }
      total = brute_add(total, best_child);
    }
    row[parent_state] = total;
  }
  return row;
}

struct brute_topology {
  std::vector<row_t> inside_by_clade;
  std::vector<larch::production_id> selected_prod_by_clade;
  std::vector<bool> used_production;
};

static std::vector<brute_topology> brute_enumerate_topologies(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::clade_id clade) {
  std::vector<brute_topology> result;
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    brute_topology topo;
    topo.inside_by_clade.assign(grammar.clades.size(), brute_inf_row());
    topo.selected_prod_by_clade.assign(grammar.clades.size(),
                                       larch::no_production);
    topo.used_production.assign(grammar.productions.size(), false);
    topo.inside_by_clade[clade] =
        brute_leaf_row(states.state_by_taxon[key.taxa.front()]);
    result.push_back(std::move(topo));
    return result;
  }

  for (auto pid : grammar.productions_by_parent[clade]) {
    auto const& prod = grammar.productions[pid];
    CHECK(prod.children.size() >= 2);

    std::vector<std::vector<brute_topology>> child_topologies;
    child_topologies.reserve(prod.children.size());
    for (auto child : prod.children) {
      child_topologies.push_back(
          brute_enumerate_topologies(grammar, states, child));
      CHECK(!child_topologies.back().empty());
    }

    std::vector<brute_topology const*> selected(prod.children.size(), nullptr);
    auto enumerate_product = [&](auto&& self, std::size_t child_i) -> void {
      if (child_i == child_topologies.size()) {
        brute_topology topo;
        topo.inside_by_clade.assign(grammar.clades.size(), brute_inf_row());
        topo.selected_prod_by_clade.assign(grammar.clades.size(),
                                           larch::no_production);
        topo.used_production.assign(grammar.productions.size(), false);

        for (auto const* child_topo : selected) {
          CHECK(child_topo != nullptr);
          for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
            if (child_topo->inside_by_clade[cid] != brute_inf_row())
              topo.inside_by_clade[cid] = child_topo->inside_by_clade[cid];
            if (child_topo->selected_prod_by_clade[cid] !=
                larch::no_production)
              topo.selected_prod_by_clade[cid] =
                  child_topo->selected_prod_by_clade[cid];
          }
          for (std::size_t i = 0; i < grammar.productions.size(); ++i)
            topo.used_production[i] =
                topo.used_production[i] || child_topo->used_production[i];
        }

        topo.selected_prod_by_clade[clade] = pid;
        topo.used_production[pid] = true;
        std::vector<row_t> child_rows;
        child_rows.reserve(prod.children.size());
        for (auto child : prod.children)
          child_rows.push_back(topo.inside_by_clade[child]);
        topo.inside_by_clade[clade] =
            brute_combine_multifurcation(child_rows);
        result.push_back(std::move(topo));
        CHECK(result.size() < 10000);
        return;
      }

      for (auto const& child_topo : child_topologies[child_i]) {
        selected[child_i] = &child_topo;
        self(self, child_i + 1);
      }
    };
    enumerate_product(enumerate_product, 0);
  }
  return result;
}

static larch::chart_cost row_min(row_t const& row) {
  larch::chart_cost best = larch::chart_inf;
  for (auto cost : row) best = std::min(best, cost);
  return best;
}

static std::vector<larch::clade_id> clades_by_decreasing_size(
    larch::clade_grammar const& grammar) {
  std::vector<larch::clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), larch::clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](auto lhs, auto rhs) {
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize > rsize;
    return lhs < rhs;
  });
  return order;
}

static std::vector<row_t> brute_topology_outside(
    larch::clade_grammar const& grammar, brute_topology const& topo) {
  std::vector<row_t> outside(grammar.clades.size(), brute_inf_row());
  outside[grammar.root_clade].fill(0);

  for (auto parent : clades_by_decreasing_size(grammar)) {
    auto pid = topo.selected_prod_by_clade[parent];
    if (pid == larch::no_production) continue;
    auto const& prod = grammar.productions[pid];
    CHECK(prod.children.size() >= 2);

    for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
         ++parent_state) {
      auto base = outside[parent][parent_state];
      if (base >= larch::chart_inf) continue;
      for (std::size_t child_i = 0; child_i < prod.children.size(); ++child_i) {
        auto child = prod.children[child_i];
        larch::chart_cost cochild_total = 0;
        for (std::size_t sibling_i = 0; sibling_i < prod.children.size();
             ++sibling_i) {
          if (sibling_i == child_i) continue;
          auto sibling = prod.children[sibling_i];
          larch::chart_cost sibling_best = larch::chart_inf;
          for (std::uint8_t sibling_state = 0;
               sibling_state < larch::nuc_state_count; ++sibling_state) {
            sibling_best =
                std::min(sibling_best,
                         brute_add(topo.inside_by_clade[sibling][sibling_state],
                                   parent_state == sibling_state ? 0 : 1));
          }
          cochild_total = brute_add(cochild_total, sibling_best);
        }
        for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
             ++child_state) {
          auto candidate =
              brute_add(brute_add(base, cochild_total),
                        parent_state == child_state ? larch::chart_cost{0}
                                                    : larch::chart_cost{1});
          outside[child][child_state] =
              std::min(outside[child][child_state], candidate);
        }
      }
    }
  }

  return outside;
}

static void compare_outside_and_mask_to_bruteforce(
    larch::clade_grammar const& grammar, larch::leaf_site_states const& states,
    larch::single_site_chart const& chart,
    larch::single_site_outside_chart const& outside,
    larch::chart_trim_mask const& mask) {
  auto topologies =
      brute_enumerate_topologies(grammar, states, grammar.root_clade);
  CHECK(!topologies.empty());

  larch::chart_cost optimum = larch::chart_inf;
  for (auto const& topo : topologies)
    optimum =
        std::min(optimum, row_min(topo.inside_by_clade[grammar.root_clade]));
  CHECK(optimum == outside.global_min);
  CHECK(optimum == chart.root_min_excluding_ua(grammar.root_clade));

  std::vector<row_t> brute_outside(grammar.clades.size(), brute_inf_row());
  std::vector<bool> brute_keep_production(grammar.productions.size(), false);

  for (auto const& topo : topologies) {
    auto root_cost = row_min(topo.inside_by_clade[grammar.root_clade]);
    if (root_cost == optimum) {
      for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid)
        brute_keep_production[pid] =
            brute_keep_production[pid] || topo.used_production[pid];
    }

    auto topo_outside = brute_topology_outside(grammar, topo);
    for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
      if (topo.inside_by_clade[cid] == brute_inf_row()) continue;
      for (std::uint8_t state = 0; state < larch::nuc_state_count; ++state) {
        brute_outside[cid][state] =
            std::min(brute_outside[cid][state], topo_outside[cid][state]);
      }
    }
  }

  CHECK(outside.outside == brute_outside);
  CHECK(mask.keep_production == brute_keep_production);
}

static void test_paper_counterexample_outside_trim_and_traceback() {
  std::println("test_paper_counterexample_outside_trim_and_traceback");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);

  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto de = clade_for(grammar, {"D", "E"});
  auto ce = clade_for(grammar, {"C", "E"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto prod_c_de = production_id_for(grammar, cde, {c, de});
  auto prod_d_ce = production_id_for(grammar, cde, {d, ce});

  auto states1 = larch::extract_leaf_site_states(merged, grammar, 1);
  larch::chart_options trace_options;
  trace_options.keep_trace = true;
  auto chart1 = larch::build_single_site_chart(grammar, states1, trace_options);
  auto outside1 = larch::build_single_site_outside_chart(grammar, chart1);
  auto mask1 = larch::build_single_site_trim_mask(grammar, chart1, outside1);

  CHECK(outside1.global_min == 1);
  CHECK(mask1.global_min == 1);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::A].size() == 1);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::A].front().production ==
        prod_c_de);
  CHECK(chart1.optimal_choices[cde][larch::nuc_base::C].size() == 2);
  CHECK(mask1.keep_clade_state[cde][larch::nuc_base::A]);
  CHECK(!mask1.keep_clade_state[cde][larch::nuc_base::C]);
  CHECK(mask1.keep_production[prod_c_de]);
  CHECK(!mask1.keep_production[prod_d_ce]);
  CHECK(!mask1.optimal_choices_by_production[prod_c_de].empty());

  larch::chart_trim_options bool_only_trim;
  bool_only_trim.store_optimal_choices = false;
  auto bool_only_mask1 = larch::build_single_site_trim_mask(
      grammar, chart1, outside1, bool_only_trim);
  CHECK(bool_only_mask1.keep_production == mask1.keep_production);
  CHECK(bool_only_mask1.optimal_choices_by_production.empty());
  CHECK(bool_only_mask1.kept_production_choice_count ==
        mask1.kept_production_choice_count);

  CHECK(mask1.kept_production_choice_count > 1);
  larch::chart_trim_options capped_trim;
  capped_trim.max_stored_optimal_choices = 1;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_single_site_trim_mask(grammar, chart1, outside1,
                                             capped_trim);
  }));

  auto traceback1 = larch::deterministic_optimal_single_site_traceback(
      grammar, chart1, outside1);
  CHECK(traceback1.score == 1);
  CHECK(contains(traceback1.productions, prod_c_de));
  CHECK(!contains(traceback1.productions, prod_d_ce));
  CHECK(traceback1.root_state_by_clade[cde] == larch::nuc_base::A);

  compare_outside_and_mask_to_bruteforce(grammar, states1, chart1, outside1,
                                         mask1);

  auto states2 = larch::extract_leaf_site_states(merged, grammar, 2);
  auto chart2 = larch::build_single_site_chart(grammar, states2, trace_options);
  auto outside2 = larch::build_single_site_outside_chart(grammar, chart2);
  auto mask2 = larch::build_single_site_trim_mask(grammar, chart2, outside2);

  CHECK(outside2.global_min == 1);
  CHECK(mask2.keep_production[prod_d_ce]);
  CHECK(!mask2.keep_production[prod_c_de]);
  auto traceback2 = larch::deterministic_optimal_single_site_traceback(
      grammar, chart2, outside2);
  CHECK(contains(traceback2.productions, prod_d_ce));
  CHECK(!contains(traceback2.productions, prod_c_de));

  compare_outside_and_mask_to_bruteforce(grammar, states2, chart2, outside2,
                                         mask2);

  std::println("  PASS");
}

static void test_single_tree_keeps_all_productions() {
  std::println("test_single_tree_keeps_all_productions");

  auto spec = larch::test::tiny_inner(
      "root", "A",
      {larch::test::tiny_inner("AB", "A",
                               {larch::test::tiny_leaf("A", "A"),
                                larch::test::tiny_leaf("B", "C")}),
       larch::test::tiny_inner("CD", "A",
                               {larch::test::tiny_leaf("C", "G"),
                                larch::test::tiny_leaf("D", "T")})});
  auto tree = larch::test::make_tiny_labelled_tree("A", spec);
  auto grammar = larch::build_clade_grammar(tree);
  auto states = larch::extract_leaf_site_states(tree, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);
  auto outside = larch::build_single_site_outside_chart(grammar, chart);
  auto mask = larch::build_single_site_trim_mask(grammar, chart, outside);

  CHECK(grammar.productions.size() == 3);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid)
    CHECK(mask.keep_production[pid]);

  auto traceback = larch::deterministic_optimal_single_site_traceback(
      grammar, chart, outside);
  CHECK(traceback.score == chart.root_min_excluding_ua(grammar.root_clade));
  CHECK(traceback.productions.size() == grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid)
    CHECK(contains(traceback.productions,
                   static_cast<larch::production_id>(pid)));

  compare_outside_and_mask_to_bruteforce(grammar, states, chart, outside, mask);

  std::println("  PASS");
}

static void test_reference_edge_outside_boundary() {
  std::println("test_reference_edge_outside_boundary");

  auto spec = larch::test::tiny_inner(
      "root", "A",
      {larch::test::tiny_leaf("A", "C"), larch::test::tiny_leaf("B", "C")});
  auto tree = larch::test::make_tiny_labelled_tree("A", spec);
  auto grammar = larch::build_clade_grammar(tree);
  auto states = larch::extract_leaf_site_states(tree, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);

  larch::chart_options opts;
  opts.score_ua_edge = true;
  auto outside = larch::build_single_site_outside_chart(
      grammar, chart, opts, larch::extract_reference_site_state(tree, 1));
  auto mask = larch::build_single_site_trim_mask(grammar, chart, outside);

  CHECK(outside.outside[grammar.root_clade][larch::nuc_base::A] == 0);
  CHECK(outside.outside[grammar.root_clade][larch::nuc_base::C] == 1);
  CHECK(outside.global_min == 1);
  CHECK(mask.keep_clade_state[grammar.root_clade][larch::nuc_base::C]);
  CHECK(!mask.keep_clade_state[grammar.root_clade][larch::nuc_base::A]);

  auto traceback = larch::deterministic_optimal_single_site_traceback(
      grammar, chart, outside);
  CHECK(traceback.score == 1);
  CHECK(traceback.root_state_by_clade[grammar.root_clade] ==
        larch::nuc_base::C);

  larch::chart_options ua_free_opts;
  CHECK(!throws_runtime_error([&] {
    (void)larch::build_single_site_outside_chart(grammar, chart, ua_free_opts,
                                                 std::uint8_t{99});
  }));
  CHECK(!throws_runtime_error([&] {
    (void)larch::build_single_site_outside_chart(grammar, chart, ua_free_opts,
                                                 tree, 2);
  }));

  auto impossible_chart = chart;
  impossible_chart.inside[grammar.root_clade].fill(larch::chart_inf);
  auto impossible_outside =
      larch::build_single_site_outside_chart(grammar, impossible_chart);
  CHECK(impossible_outside.global_min == larch::chart_inf);
  auto impossible_mask = larch::build_single_site_trim_mask(
      grammar, impossible_chart, impossible_outside);
  for (auto const& state_mask : impossible_mask.keep_clade_state)
    for (bool keep : state_mask) CHECK(!keep);
  for (bool keep : impossible_mask.keep_production) CHECK(!keep);
  CHECK(throws_runtime_error([&] {
    (void)larch::deterministic_optimal_single_site_traceback(
        grammar, impossible_chart, impossible_outside);
  }));

  std::println("  PASS");
}

static void test_binary_outside_stack_recurrence_matches_generic() {
  std::println("test_binary_outside_stack_recurrence_matches_generic");

  larch::grammar_production binary;
  binary.children = {0, 1};
  std::array<larch::chart_trim_detail::outside_chart_row, 3> inside_rows;
  inside_rows[0].fill(larch::chart_inf - 2);
  inside_rows[1].fill(10);
  inside_rows[2].fill(3);
  auto inside_provider = [&](larch::clade_id child) -> auto const& {
    return inside_rows[child];
  };

  // The sum of the two child terms saturates.  A total-minus-child shortcut
  // would recover 2 for child 0's sibling context instead of the correct 10.
  // Both the fixed-array path and the generic prefix/suffix oracle must retain
  // the direct saturated-add semantics.
  auto const stack_rows =
      larch::chart_trim_detail::combine_binary_production_outside_rows(
          binary, larch::nuc_base::A, 0, inside_provider);
  auto const generic_rows =
      larch::chart_trim_detail::combine_production_outside_rows(
          binary, larch::nuc_base::A, 0, inside_provider);
  CHECK(generic_rows.size() == 2);
  CHECK(stack_rows[0] == generic_rows[0]);
  CHECK(stack_rows[1] == generic_rows[1]);
  CHECK(stack_rows[0][larch::nuc_base::A] == 10);
  CHECK(stack_rows[1][larch::nuc_base::A] == larch::chart_inf - 2);

  larch::chart_trim_detail::outside_chart_row parent_outside{};
  parent_outside.fill(larch::chart_inf);
  parent_outside[larch::nuc_base::A] = 0;
  std::array<larch::chart_trim_detail::outside_chart_row, 2> scattered{
      larch::parsimony_chart_detail::make_inf_row(),
      larch::parsimony_chart_detail::make_inf_row()};
  larch::outside_recurrence_work_stats work;
  auto consume_binary = [&](std::size_t child_i, auto const& row) {
    for (std::size_t state = 0; state < row.size(); ++state) {
      scattered[child_i][state] =
          std::min(scattered[child_i][state], row[state]);
    }
  };
  larch::chart_trim_detail::scatter_production_outside_rows(
      binary, parent_outside, inside_provider, consume_binary, work);
  CHECK(scattered == stack_rows);
  CHECK(work.binary_stack_productions_scored == 1);
  CHECK(work.generic_reusable_productions_scored == 0);

  larch::grammar_production ternary;
  ternary.children = {0, 1, 2};
  auto ignore_row = [](std::size_t, auto const&) {};
  larch::chart_trim_detail::scatter_production_outside_rows(
      ternary, parent_outside, inside_provider, ignore_row, work);
  CHECK(work.binary_stack_productions_scored == 1);
  CHECK(work.generic_reusable_productions_scored == 1);

  // Broad deterministic equivalence matrix.  This includes every parent
  // state, unreachable parents, costs on both sides of the saturation edge,
  // and arbitrary child-row mixtures.  The generic prefix/suffix recurrence
  // remains the arithmetic oracle for the fixed-array specialization.
  auto plan_tree = larch::test::make_tiny_labelled_tree(
      "A", larch::test::tiny_inner(
               "root", "A",
               {larch::test::tiny_leaf("L", "A"),
                larch::test::tiny_leaf("R", "C")}));
  auto plan_grammar = larch::build_clade_grammar(plan_tree);
  auto plan = larch::build_chart_execution_plan(plan_grammar);
  auto const plan_productions = plan.productions();
  auto binary_descriptor = std::find_if(
      plan_productions.begin(), plan_productions.end(),
      [](auto const& production) { return production.is_binary(); });
  CHECK(binary_descriptor != plan_productions.end());
  auto const plan_children = plan.children(binary_descriptor->source_id);
  CHECK(plan_children.size() == 2);
  std::vector<larch::chart_trim_detail::outside_chart_row> plan_inside_rows(
      plan.clades().size(), larch::parsimony_chart_detail::make_inf_row());
  auto plan_inside_provider = [&](larch::clade_id child) -> auto const& {
    return plan_inside_rows[child];
  };

  std::array<larch::chart_cost, 8> cost_pool{
      0,
      1,
      2,
      11,
      larch::chart_inf - 20,
      larch::chart_inf - 2,
      larch::chart_inf - 1,
      larch::chart_inf};
  std::mt19937 rng(0x5a17u);
  std::uniform_int_distribution<std::size_t> pick_cost(
      0, cost_pool.size() - 1);
  for (std::size_t sample = 0; sample < 64; ++sample) {
    for (auto& child_row : inside_rows) {
      for (auto& cost : child_row) cost = cost_pool[pick_cost(rng)];
    }
    plan_inside_rows[plan_children[0]] = inside_rows[0];
    plan_inside_rows[plan_children[1]] = inside_rows[1];

    for (std::uint8_t parent_state = 0;
         parent_state < larch::nuc_state_count; ++parent_state) {
      for (auto parent_cost : cost_pool) {
        auto const fixed =
            larch::chart_trim_detail::combine_binary_production_outside_rows(
                binary, parent_state, parent_cost, inside_provider);
        auto const generic =
            larch::chart_trim_detail::combine_production_outside_rows(
                binary, parent_state, parent_cost, inside_provider);
        CHECK(generic.size() == fixed.size());
        CHECK(generic[0] == fixed[0]);
        CHECK(generic[1] == fixed[1]);

        auto const planned_fixed =
            larch::chart_trim_detail::combine_binary_production_outside_rows(
                plan, plan_children, parent_state, parent_cost,
                plan_inside_provider);
        auto const planned_generic =
            larch::chart_trim_detail::combine_production_outside_rows(
                plan, plan_children, parent_state, parent_cost,
                plan_inside_provider);
        CHECK(planned_generic.size() == planned_fixed.size());
        CHECK(planned_generic[0] == planned_fixed[0]);
        CHECK(planned_generic[1] == planned_fixed[1]);
        CHECK(planned_fixed == fixed);
      }
    }
  }

  std::println("  PASS");
}

static void test_multisite_composite_counterexample() {
  std::println("test_multisite_composite_counterexample");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);

  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto de = clade_for(grammar, {"D", "E"});
  auto ce = clade_for(grammar, {"C", "E"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto prod_c_de = production_id_for(grammar, cde, {c, de});
  auto prod_d_ce = production_id_for(grammar, cde, {d, ce});

  CHECK(patterns.patterns.size() == 2);
  auto composite = larch::build_composite_chart_score(grammar, patterns);
  CHECK(composite.weighted_lower_bound == 2);
  CHECK(composite.per_pattern_root_min.size() == 2);
  for (auto root_min : composite.per_pattern_root_min) CHECK(root_min == 1);

  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);
  CHECK(brute.topology_count == 2);
  CHECK(brute.optimum == 3);

  auto bnb = larch::build_multisite_trim(grammar, patterns);
  CHECK(bnb.composite_lower_bound == 2);
  CHECK(bnb.initial_upper_bound == 3);
  CHECK(bnb.optimum == 3);
  CHECK(bnb.frontier_sizes_by_clade[grammar.root_clade] == 2);
  CHECK(bnb.keep_production == brute.keep_production);
  CHECK(bnb.keep_production[prod_c_de]);
  CHECK(bnb.keep_production[prod_d_ce]);

  larch::multisite_trim_options score_only_cap;
  score_only_cap.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only_cap.require_exact_keep_mask = false;
  score_only_cap.max_frontier_entries_per_clade = 1;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, score_only_cap);
  }));

  larch::multisite_topology_trace_options trace_opts;
  trace_opts.max_optimal_topologies = 0;
  auto traced = larch::build_multisite_optimal_topologies(grammar, patterns, {},
                                                          trace_opts);
  CHECK(traced.composite_lower_bound == 2);
  CHECK(traced.optimum == 3);
  CHECK(traced.topologies.size() == 2);
  CHECK(traced.keep_production == brute.keep_production);
  for (auto const& topology : traced.topologies) {
    CHECK(larch::score_selected_topology(grammar, patterns, topology) == 3);
  }

  std::println("  PASS");
}

static void test_lazy_multisite_bnb_feeding_matches_dense() {
  std::println("test_lazy_multisite_bnb_feeding_matches_dense");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns);
  larch::lazy_chart_options retained_options;
  retained_options.retain_all_inside_class_maps = true;
  auto retained_lazy =
      larch::build_lazy_inside_chart(grammar, patterns, retained_options);
  auto retained_lazy_outside =
      larch::build_lazy_outside_chart(grammar, patterns, retained_lazy);
  CHECK(retained_lazy_outside.outside_recurrence_work
            .binary_stack_productions_scored > 0);
  CHECK(retained_lazy_outside.outside_recurrence_work
            .generic_reusable_productions_scored == 0);

  auto dense_composite = larch::build_composite_chart_score(grammar, patterns);
  auto lazy_composite =
      larch::build_composite_chart_score(grammar, patterns, lazy);
  CHECK(lazy_composite.weighted_lower_bound ==
        dense_composite.weighted_lower_bound);
  CHECK(lazy_composite.per_pattern_root_min ==
        dense_composite.per_pattern_root_min);
  CHECK(lazy_composite.per_pattern_root_min_by_reference_state ==
        dense_composite.per_pattern_root_min_by_reference_state);

  auto check_trim = [&](larch::multisite_trim_options const& options) {
    auto dense_trim = larch::build_multisite_trim(grammar, patterns, {},
                                                  options);
    auto lazy_trim = larch::build_multisite_trim(grammar, patterns, lazy, {},
                                                 options);
    CHECK(lazy_trim.lazy_chart_used);
    CHECK(lazy_trim.optimum == dense_trim.optimum);
    CHECK(lazy_trim.composite_lower_bound ==
          dense_trim.composite_lower_bound);
    CHECK(lazy_trim.initial_upper_bound == dense_trim.initial_upper_bound);
    CHECK(lazy_trim.keep_production == dense_trim.keep_production);
    CHECK(lazy_trim.frontier_sizes_by_clade ==
          dense_trim.frontier_sizes_by_clade);
    CHECK(lazy_trim.dominance_candidates_considered ==
          dense_trim.dominance_candidates_considered);
    CHECK(lazy_trim.dominance_pruned == dense_trim.dominance_pruned);
    CHECK(lazy_trim.dominance_pruned_score_pass ==
          dense_trim.dominance_pruned_score_pass);
    CHECK(lazy_trim.dominance_pruned_mask_pass ==
          dense_trim.dominance_pruned_mask_pass);
    CHECK(lazy_trim.bound_pruned == dense_trim.bound_pruned);
    CHECK(lazy_trim.equality_deduplicated ==
          dense_trim.equality_deduplicated);
    CHECK(lazy_trim.exact_mask_recovery_passes ==
          dense_trim.exact_mask_recovery_passes);
    CHECK(lazy_trim.active_pattern_count == dense_trim.active_pattern_count);
    CHECK(lazy_trim.lazy_inside_rows_computed > 0);
    CHECK(lazy_trim.lazy_outside_rows_computed > 0);
    CHECK(lazy_trim.lazy_structural_class_count_max <=
          patterns.patterns.size());
    CHECK(lazy_trim.lazy_structural_class_count_by_clade ==
          retained_lazy.structural_class_count_by_clade);
    CHECK(lazy_trim.lazy_structural_class_count_by_clade.size() ==
          grammar.clades.size());
    for (auto count : lazy_trim.lazy_structural_class_count_by_clade) {
      CHECK(count <= patterns.patterns.size());
    }
  };

  check_trim({});

  larch::multisite_trim_options score_only;
  score_only.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only.require_exact_keep_mask = false;
  check_trim(score_only);

  larch::multisite_trim_options strict;
  strict.dominance_mode =
      larch::multisite_dominance_mode::strict_mask_safe;
  check_trim(strict);

  larch::multisite_trim_options two_pass;
  two_pass.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  check_trim(two_pass);

  std::println("  PASS");
}

static larch::site_pattern_set make_pandemic_ratio_patterns(
    larch::clade_grammar const& grammar) {
  auto const group_configs = std::array<std::array<std::uint8_t, 4>, 10>{{
      {larch::nuc_base::C, larch::nuc_base::A, larch::nuc_base::A,
       larch::nuc_base::A},
      {larch::nuc_base::A, larch::nuc_base::C, larch::nuc_base::A,
       larch::nuc_base::A},
      {larch::nuc_base::A, larch::nuc_base::A, larch::nuc_base::C,
       larch::nuc_base::A},
      {larch::nuc_base::A, larch::nuc_base::A, larch::nuc_base::A,
       larch::nuc_base::C},
      {larch::nuc_base::C, larch::nuc_base::C, larch::nuc_base::A,
       larch::nuc_base::A},
      {larch::nuc_base::C, larch::nuc_base::A, larch::nuc_base::C,
       larch::nuc_base::A},
      {larch::nuc_base::C, larch::nuc_base::A, larch::nuc_base::A,
       larch::nuc_base::C},
      {larch::nuc_base::A, larch::nuc_base::C, larch::nuc_base::C,
       larch::nuc_base::A},
      {larch::nuc_base::A, larch::nuc_base::C, larch::nuc_base::A,
       larch::nuc_base::C},
      {larch::nuc_base::A, larch::nuc_base::A, larch::nuc_base::C,
       larch::nuc_base::C},
  }};
  auto const left_taxa =
      std::array<larch::taxon_id, 4>{taxon_for(grammar, "L0"),
                                     taxon_for(grammar, "L1"),
                                     taxon_for(grammar, "L2"),
                                     taxon_for(grammar, "L3")};
  auto const right_taxa =
      std::array<larch::taxon_id, 4>{taxon_for(grammar, "R0"),
                                     taxon_for(grammar, "R1"),
                                     taxon_for(grammar, "R2"),
                                     taxon_for(grammar, "R3")};

  auto apply_group = [](std::vector<std::uint8_t>& states,
                        std::array<larch::taxon_id, 4> const& taxa,
                        std::array<std::uint8_t, 4> const& config) {
    for (std::size_t i = 0; i < taxa.size(); ++i) {
      states[taxa[i]] = config[i];
    }
  };

  larch::site_pattern_set patterns;
  patterns.taxon_count = grammar.taxa.id_to_sample_id.size();
  patterns.patterns.reserve(group_configs.size() * group_configs.size());
  patterns.original_site_to_pattern.reserve(group_configs.size() *
                                            group_configs.size());
  for (auto const& left : group_configs) {
    for (auto const& right : group_configs) {
      auto states =
          std::vector<std::uint8_t>(patterns.taxon_count, larch::nuc_base::A);
      apply_group(states, left_taxa, left);
      apply_group(states, right_taxa, right);

      larch::site_pattern pattern;
      pattern.state_by_taxon = std::move(states);
      pattern.weight = 1;
      pattern.reference_state_counts[larch::nuc_base::A] = 1;
      patterns.patterns.push_back(std::move(pattern));
      patterns.original_site_to_pattern.push_back(patterns.patterns.size() - 1);
    }
  }
  patterns.total_site_count = patterns.patterns.size();
  patterns.variable_site_count = patterns.patterns.size();
  patterns.binary_variable_site_count = patterns.patterns.size();
  patterns.exact_pattern_to_normalized_binary_pattern.assign(
      patterns.patterns.size(), larch::no_site_pattern);
  patterns.exact_pattern_to_normalized_binary_state_map.assign(
      patterns.patterns.size(), larch::normalized_binary_state_map{});
  return patterns;
}

static void test_lazy_structural_pandemic_ratio_on_binary_tree() {
  std::println("test_lazy_structural_pandemic_ratio_on_binary_tree");

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto tree = tiny_inner(
      "root", "A",
      {tiny_inner("left", "A",
                  {tiny_inner("L01", "A",
                              {tiny_leaf("L0", "A"), tiny_leaf("L1", "A")}),
                   tiny_inner("L23", "A",
                              {tiny_leaf("L2", "A"), tiny_leaf("L3", "A")})}),
       tiny_inner("right", "A",
                  {tiny_inner("R01", "A",
                              {tiny_leaf("R0", "A"), tiny_leaf("R1", "A")}),
                   tiny_inner("R23", "A",
                              {tiny_leaf("R2", "A"), tiny_leaf("R3", "A")})})});
  auto dag = larch::test::make_tiny_labelled_tree("A", tree);
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = make_pandemic_ratio_patterns(grammar);
  CHECK(patterns.patterns.size() == 100);

  auto dense_trim = larch::build_multisite_trim(grammar, patterns);
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns);
  auto lazy_trim = larch::build_multisite_trim(grammar, patterns, lazy);

  CHECK(lazy_trim.lazy_chart_used);
  CHECK(lazy_trim.optimum == dense_trim.optimum);
  CHECK(lazy_trim.keep_production == dense_trim.keep_production);
  CHECK(lazy_trim.lazy_structural_class_count_by_clade ==
        lazy.structural_class_count_by_clade);

  std::size_t nonroot_internal_max = 0;
  for (larch::clade_id clade = 0; clade < grammar.clades.size(); ++clade) {
    auto const taxon_count = grammar.clades[clade].taxa.size();
    if (clade == grammar.root_clade || taxon_count <= 1) continue;
    nonroot_internal_max = std::max(
        nonroot_internal_max,
        lazy_trim.lazy_structural_class_count_by_clade[clade]);
  }
  CHECK(nonroot_internal_max > 0);
  CHECK(nonroot_internal_max * 10 <= patterns.patterns.size());
  CHECK(lazy_trim.lazy_structural_class_count_max == patterns.patterns.size());

  std::println("  PASS");
}

static void test_multisite_phase0_diagnostics_and_exactness_labels() {
  std::println("test_multisite_phase0_diagnostics_and_exactness_labels");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);

  auto bnb = larch::build_multisite_trim(grammar, patterns);
  CHECK(bnb.optimum == brute.optimum);
  CHECK(bnb.keep_production == brute.keep_production);
  CHECK(bnb.dominance_mode == larch::multisite_dominance_mode::off);
  CHECK(bnb.keep_mask_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(bnb.keep_production_exact);
  CHECK(bnb.dominance_candidates_considered == 0);
  CHECK(bnb.dominance_pruned_score_pass == 0);
  CHECK(bnb.dominance_pruned_mask_pass == 0);
  CHECK(bnb.dominance_pruned == 0);
  CHECK(bnb.dominance_pruned ==
        bnb.dominance_pruned_score_pass + bnb.dominance_pruned_mask_pass);
  CHECK(bnb.exact_mask_recovery_passes == 0);

  larch::multisite_trim_options score_only;
  score_only.require_exact_keep_mask = false;
  auto score_only_bnb =
      larch::build_multisite_trim(grammar, patterns, {}, score_only);
  CHECK(score_only_bnb.optimum == bnb.optimum);
  CHECK(score_only_bnb.dominance_mode == larch::multisite_dominance_mode::off);
  CHECK(score_only_bnb.keep_mask_kind ==
        larch::multisite_keep_mask_kind::score_only_not_exact);
  CHECK(!score_only_bnb.keep_production_exact);
  CHECK(std::none_of(score_only_bnb.keep_production.begin(),
                     score_only_bnb.keep_production.end(),
                     [](bool keep) { return keep; }));

  larch::multisite_trim_options with_known = score_only;
  with_known.require_exact_keep_mask = true;
  with_known.upper_bound_override = bnb.optimum;
  with_known.known_exact_optimum = bnb.optimum - 1;
  auto known_message = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, with_known);
  });
  CHECK(known_message.find("known_exact_optimum validation failed") !=
        std::string::npos);
  CHECK(known_message.find("upper_bound_override is pruning-only") !=
        std::string::npos);

  larch::multisite_trim_options with_upper_override;
  with_upper_override.upper_bound_override = bnb.optimum;
  auto override_bnb =
      larch::build_multisite_trim(grammar, patterns, {}, with_upper_override);
  CHECK(override_bnb.optimum == bnb.optimum);
  CHECK(override_bnb.initial_upper_bound == bnb.initial_upper_bound);
  CHECK(override_bnb.keep_mask_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);

  larch::multisite_trim_options exact_score_only;
  exact_score_only.dominance_mode = larch::multisite_dominance_mode::score_only;
  auto exact_score_only_message = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, exact_score_only);
  });
  CHECK(
      exact_score_only_message.find(
          "score-only dominance cannot return an exact keep-production mask") !=
      std::string::npos);

  larch::multisite_trim_options strict_labels;
  strict_labels.dominance_mode =
      larch::multisite_dominance_mode::strict_mask_safe;
  auto strict_bnb =
      larch::build_multisite_trim(grammar, patterns, {}, strict_labels);
  CHECK(strict_bnb.optimum == bnb.optimum);
  CHECK(strict_bnb.keep_production == bnb.keep_production);
  CHECK(strict_bnb.dominance_mode ==
        larch::multisite_dominance_mode::strict_mask_safe);
  CHECK(strict_bnb.keep_mask_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(strict_bnb.keep_production_exact);
  CHECK(strict_bnb.dominance_pruned_score_pass == 0);
  CHECK(strict_bnb.dominance_pruned == strict_bnb.dominance_pruned_mask_pass);
  CHECK(strict_bnb.exact_mask_recovery_passes == 0);

  larch::multisite_trim_options unsupported;
  unsupported.dominance_mode =
      larch::multisite_dominance_mode::provenance_preserving;
  auto dominance_message = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, unsupported);
  });
  CHECK(dominance_message.find("dominance mode 'provenance-preserving'") !=
        std::string::npos);
  CHECK(dominance_message.find("not implemented") != std::string::npos);

  std::println("  PASS");
}

static void test_multisite_coupled_frontier_annotation_exact() {
  std::println("test_multisite_coupled_frontier_annotation_exact");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);

  auto coupled =
      larch::build_multisite_coupled_frontier_trim(grammar, patterns);
  CHECK(coupled.annotated_optimal_trim);
  CHECK(coupled.coupled_frontier_exact);
  CHECK(coupled.optimum == brute.optimum);
  CHECK(coupled.keep_production == brute.keep_production);
  CHECK(coupled.optimal_root_frontier_entry_count > 0);
  CHECK(coupled.coupled_frontier_entry_count > 0);
  CHECK(coupled.coupled_frontier_entries_by_clade[grammar.root_clade] ==
        coupled.optimal_root_frontier_entry_count);

  auto enumerated = larch::enumerate_multisite_coupled_frontier_topologies(
      grammar, coupled, 0);
  CHECK(!enumerated.topology_cap_truncated);
  CHECK(enumerated.topologies.size() == brute.optimal_topology_count);
  std::vector<bool> enumerated_keep(grammar.productions.size(), false);
  for (auto const& topology : enumerated.topologies) {
    CHECK(larch::score_selected_topology(grammar, patterns, topology) ==
          brute.optimum);
    auto reachable = larch::validate_grammar_topology(grammar, topology);
    for (std::size_t pid = 0; pid < reachable.size(); ++pid) {
      if (reachable[pid]) enumerated_keep[pid] = true;
    }
  }
  CHECK(enumerated_keep == brute.keep_production);

  auto capped = larch::enumerate_multisite_coupled_frontier_topologies(
      grammar, coupled, 1);
  CHECK(capped.topology_cap_truncated);
  CHECK(capped.topologies.size() == 1);

  larch::multisite_coupled_frontier_trim_options bad_opts;
  bad_opts.trim_options.dominance_mode =
      larch::multisite_dominance_mode::score_only;
  bad_opts.trim_options.require_exact_keep_mask = false;
  auto message = runtime_error_message([&] {
    (void)larch::build_multisite_coupled_frontier_trim(grammar, patterns, {},
                                                       bad_opts);
  });
  CHECK(message.find("annotated optimal trim requires") != std::string::npos);

  std::println("  PASS");
}

static void test_multisite_strict_mask_safe_dominance_matches_bruteforce() {
  std::println("test_multisite_strict_mask_safe_dominance_matches_bruteforce");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", strict_mask_safe_dominating_tree_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", strict_mask_safe_dominated_tree_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto ac = clade_for(grammar, {"A", "C"});
  auto bd = clade_for(grammar, {"B", "D"});
  auto abcd = clade_for(grammar, {"A", "B", "C", "D"});
  CHECK(abcd == grammar.root_clade);
  auto prod_ab_cd = production_id_for(grammar, abcd, {ab, cd});
  auto prod_ac_bd = production_id_for(grammar, abcd, {ac, bd});
  auto prod_a_b = production_id_for(grammar, ab, {a, b});
  auto prod_c_d = production_id_for(grammar, cd, {c, d});
  auto prod_a_c = production_id_for(grammar, ac, {a, c});
  auto prod_b_d = production_id_for(grammar, bd, {b, d});

  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);
  CHECK(brute.topology_count == 2);
  CHECK(brute.optimum == 1);

  larch::multisite_trim_options no_bound;
  no_bound.use_bound_pruning = false;
  auto exact = larch::build_multisite_trim(grammar, patterns, {}, no_bound);
  CHECK(exact.optimum == brute.optimum);
  CHECK(exact.keep_production == brute.keep_production);
  CHECK(exact.frontier_sizes_by_clade[grammar.root_clade] == 2);
  CHECK(exact.keep_production[prod_ab_cd]);
  CHECK(exact.keep_production[prod_a_b]);
  CHECK(exact.keep_production[prod_c_d]);
  CHECK(!exact.keep_production[prod_ac_bd]);
  CHECK(!exact.keep_production[prod_a_c]);
  CHECK(!exact.keep_production[prod_b_d]);

  larch::multisite_trim_options strict_opts;
  strict_opts.dominance_mode =
      larch::multisite_dominance_mode::strict_mask_safe;
  strict_opts.use_bound_pruning = false;
  auto strict = larch::build_multisite_trim(grammar, patterns, {}, strict_opts);
  CHECK(strict.optimum == brute.optimum);
  CHECK(strict.keep_production == brute.keep_production);
  CHECK(strict.dominance_mode ==
        larch::multisite_dominance_mode::strict_mask_safe);
  CHECK(strict.keep_mask_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(strict.keep_production_exact);
  CHECK(strict.exact_mask_recovery_passes == 0);
  CHECK(strict.dominance_candidates_considered > 0);
  CHECK(strict.dominance_pruned_score_pass == 0);
  CHECK(strict.dominance_pruned_mask_pass > 0);
  CHECK(strict.dominance_pruned == strict.dominance_pruned_mask_pass);
  CHECK(strict.frontier_sizes_by_clade[grammar.root_clade] == 1);

  auto zero_weight_patterns = patterns;
  for (auto& pattern : zero_weight_patterns.patterns) pattern.weight = 0;
  auto brute_zero =
      larch::brute_force_multisite_topologies(grammar, zero_weight_patterns);
  CHECK(brute_zero.topology_count == 2);
  CHECK(brute_zero.optimum == 0);
  CHECK(brute_zero.keep_production[prod_ab_cd]);
  CHECK(brute_zero.keep_production[prod_ac_bd]);
  CHECK(brute_zero.keep_production[prod_a_b]);
  CHECK(brute_zero.keep_production[prod_c_d]);
  CHECK(brute_zero.keep_production[prod_a_c]);
  CHECK(brute_zero.keep_production[prod_b_d]);
  auto exact_zero =
      larch::build_multisite_trim(grammar, zero_weight_patterns, {}, no_bound);
  CHECK(exact_zero.keep_production == brute_zero.keep_production);
  auto strict_zero = larch::build_multisite_trim(grammar, zero_weight_patterns,
                                                 {}, strict_opts);
  CHECK(strict_zero.optimum == brute_zero.optimum);
  CHECK(strict_zero.keep_production == brute_zero.keep_production);
  CHECK(strict_zero.dominance_pruned_mask_pass == 0);
  CHECK(strict_zero.frontier_sizes_by_clade ==
        exact_zero.frontier_sizes_by_clade);

  larch::multisite_topology_trace_options trace_opts;
  trace_opts.max_optimal_topologies = 0;
  trace_opts.trim_options = strict_opts;
  auto traced = larch::build_multisite_optimal_topologies(grammar, patterns, {},
                                                          trace_opts);
  CHECK(traced.optimum == brute.optimum);
  CHECK(traced.topologies.size() == 1);
  CHECK(traced.keep_production == brute.keep_production);
  CHECK(traced.dominance_pruned > 0);
  for (auto const& topology : traced.topologies) {
    CHECK(larch::score_selected_topology(grammar, patterns, topology) ==
          brute.optimum);
  }

  larch::multisite_coupled_frontier_trim_options coupled_strict_opts;
  coupled_strict_opts.trim_options = strict_opts;
  auto coupled_strict = larch::build_multisite_coupled_frontier_trim(
      grammar, patterns, {}, coupled_strict_opts);
  CHECK(coupled_strict.optimum == brute.optimum);
  CHECK(coupled_strict.keep_production == brute.keep_production);
  CHECK(coupled_strict.dominance_pruned > 0);
  auto coupled_strict_enumerated =
      larch::enumerate_multisite_coupled_frontier_topologies(grammar,
                                                             coupled_strict, 0);
  CHECK(!coupled_strict_enumerated.topology_cap_truncated);
  CHECK(coupled_strict_enumerated.topologies.size() ==
        brute.optimal_topology_count);

  std::println("  PASS");
}

static void
test_multisite_score_only_dominance_matches_bruteforce_and_labels() {
  std::println(
      "test_multisite_score_only_dominance_matches_bruteforce_and_labels");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", dominance_dominating_tree_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", dominance_dominated_tree_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);

  CHECK(brute.topology_count == 2);
  auto exact = larch::build_multisite_trim(grammar, patterns);
  CHECK(exact.optimum == brute.optimum);
  CHECK(exact.keep_production == brute.keep_production);
  CHECK(exact.frontier_sizes_by_clade[grammar.root_clade] == 2);

  larch::multisite_trim_options score_only_opts;
  score_only_opts.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only_opts.require_exact_keep_mask = false;
  auto score_only =
      larch::build_multisite_trim(grammar, patterns, {}, score_only_opts);
  CHECK(score_only.optimum == brute.optimum);
  CHECK(score_only.dominance_mode ==
        larch::multisite_dominance_mode::score_only);
  CHECK(score_only.keep_mask_kind ==
        larch::multisite_keep_mask_kind::score_only_not_exact);
  CHECK(!score_only.keep_production_exact);
  CHECK(score_only.dominance_candidates_considered > 0);
  CHECK(score_only.dominance_pruned_score_pass > 0);
  CHECK(score_only.dominance_pruned_mask_pass == 0);
  CHECK(score_only.dominance_pruned == score_only.dominance_pruned_score_pass);
  CHECK(score_only.frontier_sizes_by_clade[grammar.root_clade] == 1);
  CHECK(score_only.keep_production != brute.keep_production);
  CHECK(std::none_of(score_only.keep_production.begin(),
                     score_only.keep_production.end(),
                     [](bool keep) { return keep; }));

  larch::multisite_trim_options off_cap;
  off_cap.max_frontier_entries_per_clade = 1;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, off_cap);
  }));

  auto score_only_cap = score_only_opts;
  score_only_cap.max_frontier_entries_per_clade = 1;
  auto capped =
      larch::build_multisite_trim(grammar, patterns, {}, score_only_cap);
  CHECK(capped.optimum == brute.optimum);
  CHECK(capped.frontier_sizes_by_clade[grammar.root_clade] == 1);

  larch::multisite_topology_trace_options trace_opts;
  trace_opts.trim_options.dominance_mode =
      larch::multisite_dominance_mode::score_only;
  trace_opts.trim_options.require_exact_keep_mask = false;
  auto trace_message = runtime_error_message([&] {
    (void)larch::build_multisite_optimal_topologies(grammar, patterns, {},
                                                    trace_opts);
  });
  CHECK(trace_message.find(
            "score-only dominance cannot emit exact topology witnesses") !=
        std::string::npos);

  std::println("  PASS");
}

static void test_multisite_two_pass_dominated_suboptimal_does_not_overkeep() {
  std::println(
      "test_multisite_two_pass_dominated_suboptimal_does_not_overkeep");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", dominance_suboptimal_dominating_tree_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", dominance_suboptimal_dominated_tree_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);

  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto bc = clade_for(grammar, {"B", "C"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto bcd = clade_for(grammar, {"B", "C", "D"});
  auto prod_b_cd = production_id_for(grammar, bcd, {b, cd});
  auto prod_bc_d = production_id_for(grammar, bcd, {bc, d});
  auto prod_b_c = production_id_for(grammar, bc, {b, c});
  auto prod_c_d = production_id_for(grammar, cd, {c, d});

  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);
  auto exact = larch::build_multisite_trim(grammar, patterns);
  CHECK(brute.topology_count == 2);
  CHECK(brute.optimum == 1);
  CHECK(exact.optimum == brute.optimum);
  CHECK(exact.keep_production == brute.keep_production);
  CHECK(exact.keep_production[prod_b_cd]);
  CHECK(exact.keep_production[prod_c_d]);
  CHECK(!exact.keep_production[prod_bc_d]);
  CHECK(!exact.keep_production[prod_b_c]);

  larch::multisite_trim_options score_only_opts;
  score_only_opts.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only_opts.require_exact_keep_mask = false;
  score_only_opts.use_bound_pruning = false;
  auto score_only =
      larch::build_multisite_trim(grammar, patterns, {}, score_only_opts);
  CHECK(score_only.optimum == brute.optimum);
  CHECK(score_only.dominance_pruned_score_pass > 0);
  CHECK(score_only.frontier_sizes_by_clade[bcd] == 1);

  larch::multisite_trim_options two_pass_opts;
  two_pass_opts.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  two_pass_opts.use_bound_pruning = false;
  auto two_pass =
      larch::build_multisite_trim(grammar, patterns, {}, two_pass_opts);
  CHECK(two_pass.optimum == brute.optimum);
  CHECK(two_pass.keep_production == brute.keep_production);
  CHECK(two_pass.keep_production_exact);
  CHECK(two_pass.exact_mask_recovery_passes == 1);
  CHECK(two_pass.dominance_pruned_score_pass > 0);
  CHECK(two_pass.dominance_pruned_mask_pass == 0);
  // A dominance implementation that merged dominated provenance into the
  // dominator would over-keep these suboptimal productions. The Phase 3
  // recovery pass rebuilds without dominance and excludes them.
  CHECK(!two_pass.keep_production[prod_bc_d]);
  CHECK(!two_pass.keep_production[prod_b_c]);

  std::println("  PASS");
}

static void test_multisite_two_pass_exact_mask_matches_bruteforce() {
  std::println("test_multisite_two_pass_exact_mask_matches_bruteforce");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", dominance_dominating_tree_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", dominance_dominated_tree_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);

  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);
  auto exact = larch::build_multisite_trim(grammar, patterns);

  larch::multisite_trim_options two_pass_opts;
  two_pass_opts.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  auto two_pass =
      larch::build_multisite_trim(grammar, patterns, {}, two_pass_opts);
  CHECK(two_pass.optimum == brute.optimum);
  CHECK(two_pass.keep_production == brute.keep_production);
  CHECK(two_pass.keep_production == exact.keep_production);
  CHECK(two_pass.dominance_mode ==
        larch::multisite_dominance_mode::two_pass_exact_mask);
  CHECK(two_pass.keep_mask_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(two_pass.keep_production_exact);
  CHECK(two_pass.exact_mask_recovery_passes == 1);
  CHECK(two_pass.dominance_candidates_considered > 0);
  CHECK(two_pass.dominance_pruned_score_pass > 0);
  CHECK(two_pass.dominance_pruned_mask_pass == 0);
  CHECK(two_pass.dominance_pruned == two_pass.dominance_pruned_score_pass +
                                         two_pass.dominance_pruned_mask_pass);
  CHECK(two_pass.frontier_sizes_by_clade == exact.frontier_sizes_by_clade);

  larch::multisite_trim_options no_bound = two_pass_opts;
  no_bound.use_bound_pruning = false;
  auto two_pass_no_bound =
      larch::build_multisite_trim(grammar, patterns, {}, no_bound);
  CHECK(two_pass_no_bound.optimum == two_pass.optimum);
  CHECK(two_pass_no_bound.keep_production == two_pass.keep_production);
  CHECK(two_pass_no_bound.bound_pruned == 0);

  larch::multisite_trim_options cap = two_pass_opts;
  cap.max_frontier_entries_per_clade = 1;
  auto cap_message = runtime_error_message(
      [&] { (void)larch::build_multisite_trim(grammar, patterns, {}, cap); });
  CHECK(cap_message.find("exact mask recovery pass") != std::string::npos);
  CHECK(cap_message.find("frontier entry cap exceeded") != std::string::npos);

  larch::multisite_trim_options wrong_known = two_pass_opts;
  wrong_known.known_exact_optimum = brute.optimum + 1;
  auto known_message = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, wrong_known);
  });
  CHECK(known_message.find("known_exact_optimum validation failed") !=
        std::string::npos);

  larch::multisite_trim_options not_exact = two_pass_opts;
  not_exact.require_exact_keep_mask = false;
  auto score_only_message = runtime_error_message([&] {
    (void)larch::build_multisite_trim(grammar, patterns, {}, not_exact);
  });
  CHECK(score_only_message.find("two-pass-exact-mask dominance is an "
                                "exact-mask recovery mode") !=
        std::string::npos);

  larch::multisite_topology_trace_options trace_opts;
  trace_opts.trim_options.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  auto trace_message = runtime_error_message([&] {
    (void)larch::build_multisite_optimal_topologies(grammar, patterns, {},
                                                    trace_opts);
  });
  CHECK(trace_message.find("two-pass exact-mask dominance cannot emit exact "
                           "topology witnesses") != std::string::npos);

  std::println("  PASS");
}

static void test_multisite_parent_combine_fixes_child_topology() {
  std::println("test_multisite_parent_combine_fixes_child_topology");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", paper_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  auto cde = clade_for(grammar, {"C", "D", "E"});

  larch::multisite_trim_options no_bound_pruning;
  no_bound_pruning.use_bound_pruning = false;
  auto bnb =
      larch::build_multisite_trim(grammar, patterns, {}, no_bound_pruning);
  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);

  // The CDE child has two fixed topology entries: one preferred by site 1 and
  // one by site 2.  The parent/root combine must choose one child entry before
  // minimizing over states; if it re-minimized child topology independently per
  // pattern, this would incorrectly collapse to the composite lower bound 2.
  CHECK(bnb.frontier_sizes_by_clade[cde] == 2);
  CHECK(bnb.composite_lower_bound == 2);
  CHECK(bnb.optimum == 3);
  CHECK(brute.optimum == 3);
  CHECK(bnb.keep_production == brute.keep_production);

  std::println("  PASS");
}

static void test_multisite_concordant_sites_equal_lower_bound() {
  std::println("test_multisite_concordant_sites_equal_lower_bound");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", concordant_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("AA", concordant_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);

  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto de = clade_for(grammar, {"D", "E"});
  auto ce = clade_for(grammar, {"C", "E"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto prod_c_de = production_id_for(grammar, cde, {c, de});
  auto prod_d_ce = production_id_for(grammar, cde, {d, ce});

  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns.front().weight == 2);

  auto composite = larch::build_composite_chart_score(grammar, patterns);
  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);
  auto bnb = larch::build_multisite_trim(grammar, patterns);

  CHECK(composite.weighted_lower_bound == 2);
  CHECK(brute.optimum == 2);
  CHECK(bnb.optimum == 2);
  CHECK(bnb.optimum == bnb.composite_lower_bound);
  CHECK(bnb.keep_production == brute.keep_production);
  CHECK(bnb.keep_production[prod_c_de]);
  CHECK(!bnb.keep_production[prod_d_ce]);

  std::println("  PASS");
}

static void test_multisite_invariant_sites_and_reference_edge_constant() {
  std::println("test_multisite_invariant_sites_and_reference_edge_constant");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAA", paper_tree1_with_invariant_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAA", paper_tree2_with_invariant_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);

  auto keep_patterns = larch::build_site_patterns(merged, grammar);
  CHECK(keep_patterns.invariant_site_count == 1);
  CHECK(keep_patterns.variable_site_count == 2);
  auto keep_bnb = larch::build_multisite_trim(grammar, keep_patterns);
  CHECK(keep_bnb.optimum == 3);
  CHECK(keep_bnb.invariant_constant_offset == 0);

  larch::site_pattern_options skip_invariant;
  skip_invariant.skip_invariant_sites = true;
  auto skipped_patterns =
      larch::build_site_patterns(merged, grammar, skip_invariant);
  CHECK(skipped_patterns.patterns.size() == 2);
  CHECK(skipped_patterns.skipped_invariant_site_count == 1);
  auto skipped_bnb = larch::build_multisite_trim(grammar, skipped_patterns);
  CHECK(skipped_bnb.optimum == keep_bnb.optimum);
  CHECK(skipped_bnb.keep_production == keep_bnb.keep_production);

  larch::chart_options with_reference_edge;
  with_reference_edge.score_ua_edge = true;
  auto with_ua_bnb = larch::build_multisite_trim(grammar, skipped_patterns,
                                                 with_reference_edge);
  CHECK(with_ua_bnb.invariant_constant_offset == 1);
  CHECK(with_ua_bnb.optimum == keep_bnb.optimum + 1);
  CHECK(with_ua_bnb.keep_production == keep_bnb.keep_production);

  larch::multisite_trim_options two_pass_opts;
  two_pass_opts.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  auto skipped_two_pass =
      larch::build_multisite_trim(grammar, skipped_patterns, {}, two_pass_opts);
  CHECK(skipped_two_pass.optimum == skipped_bnb.optimum);
  CHECK(skipped_two_pass.keep_production == skipped_bnb.keep_production);
  CHECK(skipped_two_pass.exact_mask_recovery_passes == 1);
  auto with_ua_two_pass = larch::build_multisite_trim(
      grammar, skipped_patterns, with_reference_edge, two_pass_opts);
  CHECK(with_ua_two_pass.invariant_constant_offset == 1);
  CHECK(with_ua_two_pass.optimum == with_ua_bnb.optimum);
  CHECK(with_ua_two_pass.keep_production == with_ua_bnb.keep_production);

  auto brute_ua = larch::brute_force_multisite_topologies(
      grammar, skipped_patterns, with_reference_edge);
  auto with_ua_coupled = larch::build_multisite_coupled_frontier_trim(
      grammar, skipped_patterns, with_reference_edge);
  CHECK(with_ua_coupled.invariant_constant_offset == 1);
  CHECK(with_ua_coupled.optimum == brute_ua.optimum);
  CHECK(with_ua_coupled.keep_production == brute_ua.keep_production);
  auto with_ua_coupled_enumerated =
      larch::enumerate_multisite_coupled_frontier_topologies(
          grammar, with_ua_coupled, 0);
  CHECK(!with_ua_coupled_enumerated.topology_cap_truncated);
  CHECK(with_ua_coupled_enumerated.topologies.size() ==
        brute_ua.optimal_topology_count);

  std::println("  PASS");
}

static void test_plan_multisite_trim_strict_semantic_equivalence() {
  std::println("test_plan_multisite_trim_strict_semantic_equivalence");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAA", paper_tree1_with_invariant_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAA", paper_tree2_with_invariant_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  larch::site_pattern_options pattern_options;
  pattern_options.skip_invariant_sites = true;
  auto patterns =
      larch::build_site_patterns(merged, grammar, pattern_options);

  // Construction is deliberately outside the observer scope below.  The
  // acceptance condition is that every subsequent exact/frontier pass,
  // including initial-upper-bound traceback and restricted rescoring, uses
  // only the prevalidated plan.
  auto plan = larch::build_chart_execution_plan(grammar);
  for (bool score_ua_edge : {false, true}) {
    larch::chart_options chart_options;
    chart_options.score_ua_edge = score_ua_edge;
    for (auto dominance_mode :
         {larch::multisite_dominance_mode::off,
          larch::multisite_dominance_mode::score_only,
          larch::multisite_dominance_mode::strict_mask_safe,
          larch::multisite_dominance_mode::two_pass_exact_mask}) {
      for (bool use_bound_pruning : {false, true}) {
        larch::multisite_trim_options trim_options;
        trim_options.dominance_mode = dominance_mode;
        trim_options.use_bound_pruning = use_bound_pruning;
        trim_options.require_exact_keep_mask =
            dominance_mode != larch::multisite_dominance_mode::score_only;
        trim_options.capture_optimal_root_provenance =
            trim_options.require_exact_keep_mask;

        auto checked = larch::build_multisite_trim(
            grammar, patterns, chart_options, trim_options);
        auto planned = build_plan_trim_without_structural_work(
            plan, patterns, chart_options, trim_options);
        check_multisite_trim_results_equal(checked, planned);

        CHECK(planned.initial_upper_bound < larch::multisite_score_inf);
        if (trim_options.capture_optimal_root_provenance) {
          CHECK(!planned.optimal_root_provenance_classes.empty());
        } else {
          CHECK(planned.optimal_root_provenance_classes.empty());
        }
        if (score_ua_edge) {
          CHECK(planned.invariant_constant_offset == 1);
        }
      }
    }
  }

  std::println("  PASS");
}

static void test_multisite_exact_setup_cold_resident_and_lifetime() {
  std::println("test_multisite_exact_setup_cold_resident_and_lifetime");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAA", paper_tree1_with_invariant_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAA", paper_tree2_with_invariant_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto plan = larch::build_chart_execution_plan(grammar);
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto de = clade_for(grammar, {"D", "E"});
  auto ce = clade_for(grammar, {"C", "E"});
  auto cde = clade_for(grammar, {"C", "D", "E"});
  auto prod_c_de = production_id_for(grammar, cde, {c, de});
  auto prod_d_ce = production_id_for(grammar, cde, {d, ce});
  larch::site_pattern_options pattern_options;
  pattern_options.skip_invariant_sites = true;
  auto patterns =
      larch::build_site_patterns(merged, grammar, pattern_options);

  auto const active_pattern_count = static_cast<std::size_t>(std::count_if(
      patterns.patterns.begin(), patterns.patterns.end(),
      [](auto const& pattern) {
        return !larch::is_invariant_site_pattern(pattern);
      }));
  CHECK(active_pattern_count == 2);
  auto const binary_production_count =
      static_cast<std::size_t>(std::count_if(
          grammar.productions.begin(), grammar.productions.end(),
          [](auto const& production) {
            return production.children.size() == 2;
          }));
  CHECK(binary_production_count == grammar.productions.size());

  for (bool score_ua_edge : {false, true}) {
    larch::chart_options chart_options;
    chart_options.score_ua_edge = score_ua_edge;

    std::size_t expected_outside_builds = 0;
    for (auto const& pattern : patterns.patterns) {
      if (larch::is_invariant_site_pattern(pattern)) continue;
      if (!score_ua_edge) {
        ++expected_outside_builds;
        continue;
      }
      expected_outside_builds += static_cast<std::size_t>(std::count_if(
          pattern.reference_state_counts.begin(),
          pattern.reference_state_counts.end(),
          [](std::uint32_t count) { return count != 0; }));
    }

    auto cold_setup =
        larch::build_multisite_exact_setup(plan, patterns, chart_options);
    CHECK(cold_setup.work.setup_builds == 1);
    CHECK(cold_setup.work.inside_charts_built == active_pattern_count);
    CHECK(cold_setup.work.resident_inside_charts_consumed == 0);
    CHECK(cold_setup.work.active_leaf_state_vectors_copied ==
          active_pattern_count);
    CHECK(cold_setup.work.active_leaf_states_copied ==
          active_pattern_count * patterns.taxon_count);
    CHECK(cold_setup.work.outside_boundary_charts_built ==
          expected_outside_builds);
    CHECK(cold_setup.work.outside_recurrence_work
              .binary_stack_productions_scored ==
          expected_outside_builds * binary_production_count);
    CHECK(cold_setup.work.outside_recurrence_work
              .generic_reusable_productions_scored == 0);
    CHECK(cold_setup.work.upper_bound_topologies_generated ==
          active_pattern_count + 1);
    // The two active paper-counterexample patterns have opposite uniquely
    // preferred CDE topologies.  The deterministic seed topology duplicates
    // one of those per-pattern tracebacks, so this exercises actual setup-time
    // topology deduplication rather than only its accounting surface.
    CHECK(cold_setup.work.upper_bound_topologies_generated == 3);
    CHECK(cold_setup.work.upper_bound_topologies_unique == 2);
    CHECK(cold_setup.work.upper_bound_topologies_generated >
          cold_setup.work.upper_bound_topologies_unique);
    CHECK(cold_setup.work.upper_bound_topologies_unique != 0);
    CHECK(cold_setup.work.upper_bound_topologies_unique <=
          cold_setup.work.upper_bound_topologies_generated);
    CHECK(cold_setup.work.frontier_passes == 0);
    for (auto const& info : cold_setup.active_patterns) {
      CHECK(info.chart.inside.empty());
      CHECK(info.chart.optimal_choices.empty());
    }

    if (!score_ua_edge) {
      // Shape is not identity: a same-generation structure change must be
      // caught by the full fingerprint, and a generation change must be
      // caught even when the full structure remains byte-for-byte equivalent.
      auto same_shape_distinct = grammar;
      auto reordered = std::find_if(
          same_shape_distinct.productions_by_parent.begin(),
          same_shape_distinct.productions_by_parent.end(),
          [](auto const& productions) { return productions.size() > 1; });
      CHECK(reordered != same_shape_distinct.productions_by_parent.end());
      std::swap((*reordered)[0], (*reordered)[1]);
      auto distinct_plan =
          larch::build_chart_execution_plan(same_shape_distinct);
      CHECK(distinct_plan.clades().size() == plan.clades().size());
      CHECK(distinct_plan.productions().size() == plan.productions().size());
      CHECK(distinct_plan.grammar_generation() == plan.grammar_generation());
      CHECK(distinct_plan.fingerprint() != plan.fingerprint());
      CHECK(throws_runtime_error([&] {
        (void)larch::build_multisite_trim_from_exact_setup(
            distinct_plan, cold_setup, chart_options);
      }));
      CHECK(throws_runtime_error([&] {
        (void)larch::build_multisite_trim_from_exact_setup(
            same_shape_distinct, cold_setup, chart_options);
      }));

      auto next_generation = grammar;
      ++next_generation.execution_generation;
      auto next_generation_plan =
          larch::build_chart_execution_plan(next_generation);
      CHECK(next_generation_plan.fingerprint() == plan.fingerprint());
      CHECK(next_generation_plan.grammar_generation() !=
            plan.grammar_generation());
      CHECK(throws_runtime_error([&] {
        (void)larch::build_multisite_trim_from_exact_setup(
            next_generation_plan, cold_setup, chart_options);
      }));
      CHECK(throws_runtime_error([&] {
        (void)larch::build_multisite_trim_from_exact_setup(
            next_generation, cold_setup, chart_options);
      }));
    }

    std::size_t resident_provider_calls = 0;
    larch::multisite_exact_setup resident_setup;
    {
      auto local_patterns = patterns;
      std::vector<std::optional<larch::single_site_chart>> resident_charts(
          local_patterns.patterns.size());
      larch::chart_options inside_options = chart_options;
      inside_options.keep_trace = false;
      inside_options.max_trace_choices = 0;
      for (std::size_t pattern_index = 0;
           pattern_index < local_patterns.patterns.size(); ++pattern_index) {
        auto const& pattern = local_patterns.patterns[pattern_index];
        if (larch::is_invariant_site_pattern(pattern)) continue;
        resident_charts[pattern_index].emplace(larch::build_single_site_chart(
            plan, larch::view_leaf_site_states(pattern.state_by_taxon),
            inside_options));
      }

      resident_setup = larch::build_multisite_exact_setup_from_resident_inside(
          plan, local_patterns,
          [&](std::size_t pattern_index,
              larch::site_pattern const&) -> larch::single_site_chart const& {
            ++resident_provider_calls;
            CHECK(resident_charts[pattern_index].has_value());
            return *resident_charts[pattern_index];
          },
          chart_options);

      // Poison and release both possible borrow sources before the setup is
      // consumed below.  A dangling pattern/chart view would fail this test.
      for (auto& chart : resident_charts) chart.reset();
      resident_charts.clear();
      resident_charts.shrink_to_fit();
      for (auto& pattern : local_patterns.patterns) {
        std::fill(pattern.state_by_taxon.begin(), pattern.state_by_taxon.end(),
                  larch::no_nuc_state);
        pattern.state_by_taxon.clear();
        pattern.state_by_taxon.shrink_to_fit();
      }
      local_patterns.patterns.clear();
      local_patterns.patterns.shrink_to_fit();
    }

    CHECK(resident_provider_calls == active_pattern_count);
    CHECK(resident_setup.work.setup_builds == 1);
    CHECK(resident_setup.work.inside_charts_built == 0);
    CHECK(resident_setup.work.resident_inside_charts_consumed ==
          active_pattern_count);
    CHECK(resident_setup.work.active_leaf_state_vectors_copied ==
          active_pattern_count);
    CHECK(resident_setup.work.active_leaf_states_copied ==
          active_pattern_count * patterns.taxon_count);
    CHECK(resident_setup.work.outside_boundary_charts_built ==
          expected_outside_builds);
    CHECK(resident_setup.work.outside_recurrence_work ==
          cold_setup.work.outside_recurrence_work);
    CHECK(resident_setup.work.outside_recurrence_work
              .binary_stack_productions_scored ==
          expected_outside_builds * binary_production_count);
    CHECK(resident_setup.work.outside_recurrence_work
              .generic_reusable_productions_scored == 0);
    CHECK(resident_setup.work.upper_bound_topologies_generated ==
          active_pattern_count + 1);
    CHECK(resident_setup.work.upper_bound_topologies_generated == 3);
    CHECK(resident_setup.work.upper_bound_topologies_unique == 2);
    CHECK(resident_setup.work.upper_bound_topologies_generated >
          resident_setup.work.upper_bound_topologies_unique);
    CHECK(resident_setup.work.upper_bound_topologies_unique ==
          cold_setup.work.upper_bound_topologies_unique);
    for (auto const& info : resident_setup.active_patterns) {
      CHECK(info.chart.inside.empty());
      CHECK(info.chart.optimal_choices.empty());
    }

    auto brute = larch::brute_force_multisite_topologies(
        grammar, patterns, chart_options);
    CHECK(brute.topology_count == 2);
    CHECK(brute.optimum == (score_ua_edge ? 4 : 3));
    CHECK(brute.keep_production[prod_c_de]);
    CHECK(brute.keep_production[prod_d_ce]);
    for (auto dominance_mode :
         {larch::multisite_dominance_mode::off,
          larch::multisite_dominance_mode::two_pass_exact_mask}) {
      larch::multisite_trim_options trim_options;
      trim_options.dominance_mode = dominance_mode;
      trim_options.capture_optimal_root_provenance = true;
      auto const expected_frontier_passes =
          dominance_mode ==
                  larch::multisite_dominance_mode::two_pass_exact_mask
              ? std::size_t{2}
              : std::size_t{1};

      auto checked = larch::build_multisite_trim(
          grammar, patterns, chart_options, trim_options);
      auto integrated = larch::build_multisite_trim(
          plan, patterns, chart_options, trim_options);
      auto cold = larch::build_multisite_trim_from_exact_setup(
          plan, cold_setup, chart_options, trim_options);
      auto resident = larch::build_multisite_trim_from_exact_setup(
          plan, resident_setup, chart_options, trim_options);

      check_multisite_trim_results_equal(checked, integrated);
      check_multisite_trim_results_equal(integrated, cold);
      check_multisite_trim_results_equal(cold, resident, false);
      CHECK(cold.optimum == checked.optimum);
      CHECK(cold.keep_production == checked.keep_production);
      CHECK(cold.optimal_root_provenance_classes ==
            checked.optimal_root_provenance_classes);
      CHECK(resident.optimum == checked.optimum);
      CHECK(resident.keep_production == checked.keep_production);
      CHECK(resident.optimal_root_provenance_classes ==
            checked.optimal_root_provenance_classes);
      CHECK(resident.optimum == brute.optimum);
      CHECK(resident.keep_production == brute.keep_production);
      CHECK(resident.keep_production_exact);
      CHECK(!resident.optimal_root_provenance_classes.empty());
      CHECK(integrated.exact_setup_work.frontier_passes ==
            expected_frontier_passes);
      CHECK(cold.exact_setup_work.frontier_passes ==
            expected_frontier_passes);
      CHECK(resident.exact_setup_work.frontier_passes ==
            expected_frontier_passes);
      CHECK(cold.exact_setup_work.inside_charts_built ==
            active_pattern_count);
      CHECK(resident.exact_setup_work.resident_inside_charts_consumed ==
            active_pattern_count);
      CHECK(resident_provider_calls == active_pattern_count);
    }
  }

  std::println("  PASS");
}

static void test_composite_reference_state_diagnostics() {
  std::println("test_composite_reference_state_diagnostics");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AG", larch::test::tiny_inner("root", "AG",
                                    {larch::test::tiny_leaf("L1", "AA"),
                                     larch::test::tiny_leaf("L2", "CC")}));
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns.front().weight == 2);
  CHECK(patterns.patterns.front().reference_state_counts[larch::nuc_base::A] ==
        1);
  CHECK(patterns.patterns.front().reference_state_counts[larch::nuc_base::G] ==
        1);

  larch::chart_options with_reference_edge;
  with_reference_edge.score_ua_edge = true;
  auto composite = larch::build_composite_chart_score(grammar, patterns,
                                                      with_reference_edge);
  auto lazy = larch::build_lazy_inside_chart(grammar, patterns);
  auto lazy_composite = larch::build_composite_chart_score(
      grammar, patterns, lazy, with_reference_edge);
  CHECK(composite.weighted_lower_bound == 3);
  CHECK(lazy_composite.weighted_lower_bound == composite.weighted_lower_bound);
  CHECK(lazy_composite.per_pattern_root_min == composite.per_pattern_root_min);
  CHECK(lazy_composite.per_pattern_root_min_by_reference_state ==
        composite.per_pattern_root_min_by_reference_state);
  CHECK(composite.per_pattern_root_min.size() == 1);
  CHECK(composite.per_pattern_root_min.front() == 1);
  CHECK(composite.per_pattern_root_min_by_reference_state.size() == 1);
  auto const& by_reference =
      composite.per_pattern_root_min_by_reference_state.front();
  CHECK(by_reference[larch::nuc_base::A] == 1);
  CHECK(by_reference[larch::nuc_base::G] == 2);
  CHECK(by_reference[larch::nuc_base::C] == larch::chart_inf);
  CHECK(by_reference[larch::nuc_base::T] == larch::chart_inf);

  auto dense_trim = larch::build_multisite_trim(grammar, patterns,
                                                with_reference_edge);
  auto lazy_trim = larch::build_multisite_trim(grammar, patterns, lazy,
                                               with_reference_edge);
  CHECK(lazy_trim.lazy_chart_used);
  CHECK(lazy_trim.lazy_outside_rows_computed > 0);
  CHECK(lazy_trim.optimum == dense_trim.optimum);
  CHECK(lazy_trim.composite_lower_bound == dense_trim.composite_lower_bound);
  CHECK(lazy_trim.keep_production == dense_trim.keep_production);
  CHECK(lazy_trim.frontier_sizes_by_clade ==
        dense_trim.frontier_sizes_by_clade);

  std::println("  PASS");
}

static void test_multisite_rejects_pattern_taxon_count_mismatch() {
  std::println("test_multisite_rejects_pattern_taxon_count_mismatch");

  auto dag = larch::test::make_tiny_labelled_tree("A", invariant_tree1_spec());
  auto grammar = larch::build_clade_grammar(dag);
  larch::site_pattern_options pattern_options;
  pattern_options.skip_invariant_sites = true;
  auto patterns = larch::build_site_patterns(dag, grammar, pattern_options);
  CHECK(patterns.patterns.empty());
  ++patterns.taxon_count;

  CHECK(throws_runtime_error(
      [&] { (void)larch::build_multisite_trim(grammar, patterns); }));

  std::println("  PASS");
}

static void test_multisite_equal_dedup_merges_provenance() {
  std::println("test_multisite_equal_dedup_merges_provenance");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(
      larch::test::make_tiny_labelled_tree("A", invariant_tree1_spec()));
  trees.push_back(
      larch::test::make_tiny_labelled_tree("A", invariant_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);

  auto brute = larch::brute_force_multisite_topologies(grammar, patterns);
  auto bnb = larch::build_multisite_trim(grammar, patterns);

  CHECK(patterns.invariant_site_count == 1);
  CHECK(bnb.active_pattern_count == 0);
  CHECK(brute.optimum == 0);
  CHECK(bnb.optimum == 0);
  CHECK(bnb.keep_production == brute.keep_production);
  CHECK(bnb.equality_deduplicated > 0);
  for (bool keep : bnb.keep_production) CHECK(keep);

  larch::multisite_topology_trace_options trace_opts;
  trace_opts.max_optimal_topologies = 0;
  auto traced = larch::build_multisite_optimal_topologies(grammar, patterns, {},
                                                          trace_opts);
  CHECK(traced.equality_deduplicated > 0);
  CHECK(traced.topologies.size() == brute.topology_count);
  CHECK(traced.keep_production == brute.keep_production);
  for (bool keep : traced.keep_production) CHECK(keep);

  auto coupled = larch::build_multisite_coupled_frontier_trim(grammar,
                                                              patterns);
  CHECK(coupled.active_pattern_count == 0);
  CHECK(coupled.optimum == brute.optimum);
  CHECK(coupled.keep_production == brute.keep_production);
  CHECK(coupled.equality_deduplicated > 0);
  for (bool keep : coupled.keep_production) CHECK(keep);
  auto coupled_enumerated =
      larch::enumerate_multisite_coupled_frontier_topologies(grammar, coupled,
                                                             0);
  CHECK(!coupled_enumerated.topology_cap_truncated);
  CHECK(coupled_enumerated.topologies.size() == brute.topology_count);

  std::println("  PASS");
}

static void test_multisite_all_invariant_coupled_frontier_with_reference_edge() {
  std::println(
      "test_multisite_all_invariant_coupled_frontier_with_reference_edge");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", invariant_reference_mismatch_tree1_spec()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", invariant_reference_mismatch_tree2_spec()));
  auto merged = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(merged);
  auto patterns = larch::build_site_patterns(merged, grammar);
  CHECK(patterns.invariant_site_count == 1);

  larch::chart_options with_reference_edge;
  with_reference_edge.score_ua_edge = true;
  auto brute = larch::brute_force_multisite_topologies(
      grammar, patterns, with_reference_edge);
  auto bnb = larch::build_multisite_trim(grammar, patterns,
                                         with_reference_edge);
  auto coupled = larch::build_multisite_coupled_frontier_trim(
      grammar, patterns, with_reference_edge);

  CHECK(brute.optimum == 1);
  CHECK(bnb.optimum == brute.optimum);
  CHECK(coupled.optimum == brute.optimum);
  CHECK(bnb.active_pattern_count == 0);
  CHECK(coupled.active_pattern_count == 0);
  CHECK(bnb.invariant_constant_offset == 1);
  CHECK(coupled.invariant_constant_offset == 1);
  CHECK(coupled.keep_production == brute.keep_production);
  for (bool keep : coupled.keep_production) CHECK(keep);

  auto enumerated = larch::enumerate_multisite_coupled_frontier_topologies(
      grammar, coupled, 0);
  CHECK(!enumerated.topology_cap_truncated);
  CHECK(enumerated.topologies.size() == brute.optimal_topology_count);
  for (auto const& topology : enumerated.topologies) {
    CHECK(larch::score_selected_topology(grammar, patterns, topology,
                                         with_reference_edge) ==
          brute.optimum);
  }

  std::println("  PASS");
}

static void test_multisite_randomized_tiny_bnb_matches_bruteforce() {
  std::println("test_multisite_randomized_tiny_bnb_matches_bruteforce");

  std::mt19937 rng{20260518};
  auto random_base = [&]() -> char {
    static constexpr std::array<char, 4> bases{'A', 'C', 'G', 'T'};
    std::uniform_int_distribution<int> dist(0, 3);
    return bases[dist(rng)];
  };

  for (std::size_t trial = 0; trial < 18; ++trial) {
    auto site_count = 3 + (trial % 3);
    std::string reference;
    reference.reserve(site_count);
    for (std::size_t pos = 0; pos < site_count; ++pos)
      reference.push_back(random_base());

    std::vector<std::pair<std::string, std::string>> leaves;
    for (std::size_t taxon = 0; taxon < 5; ++taxon) {
      std::string sequence;
      sequence.reserve(site_count);
      for (std::size_t pos = 0; pos < site_count; ++pos) {
        // Bias toward A/C to create repeated patterns and ties, with occasional
        // other states to exercise non-binary exact patterns.
        std::uniform_int_distribution<int> biased(0, 7);
        auto draw = biased(rng);
        sequence.push_back(draw < 3 ? 'A' : (draw < 6 ? 'C' : random_base()));
      }
      leaves.emplace_back("T" + std::to_string(taxon), std::move(sequence));
    }

    std::vector<larch::phylo_dag> trees;
    for (std::size_t tree_idx = 0; tree_idx < 3; ++tree_idx) {
      int inner_id = static_cast<int>(1000 * trial + 100 * tree_idx);
      auto spec = random_tree_spec(leaves, reference, rng, inner_id);
      trees.push_back(larch::test::make_tiny_labelled_tree(reference, spec));
    }

    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto patterns = larch::build_site_patterns(merged, grammar);
    std::optional<larch::chart_execution_plan> parity_plan;
    if (trial < 2) {
      parity_plan.emplace(larch::build_chart_execution_plan(grammar));
    }

    auto brute =
        larch::brute_force_multisite_topologies(grammar, patterns, {}, 200000);
    auto bnb = larch::build_multisite_trim(grammar, patterns);
    CHECK(bnb.optimum == brute.optimum);
    CHECK(bnb.keep_production == brute.keep_production);
    if (parity_plan) {
      auto planned = build_plan_trim_without_structural_work(
          *parity_plan, patterns, {}, {});
      check_multisite_trim_results_equal(bnb, planned);
    }

    if (trial < 6) {
      auto coupled = larch::build_multisite_coupled_frontier_trim(grammar,
                                                                  patterns);
      CHECK(coupled.optimum == brute.optimum);
      CHECK(coupled.keep_production == brute.keep_production);
      auto enumerated = larch::enumerate_multisite_coupled_frontier_topologies(
          grammar, coupled, 0);
      CHECK(!enumerated.topology_cap_truncated);
      CHECK(enumerated.topologies.size() == brute.optimal_topology_count);
      for (auto const& topology : enumerated.topologies) {
        CHECK(larch::score_selected_topology(grammar, patterns, topology) ==
              brute.optimum);
      }
    }

    larch::multisite_trim_options score_only_opts;
    score_only_opts.dominance_mode =
        larch::multisite_dominance_mode::score_only;
    score_only_opts.require_exact_keep_mask = false;
    auto score_only =
        larch::build_multisite_trim(grammar, patterns, {}, score_only_opts);
    CHECK(score_only.optimum == brute.optimum);
    CHECK(score_only.keep_mask_kind ==
          larch::multisite_keep_mask_kind::score_only_not_exact);
    CHECK(!score_only.keep_production_exact);
    if (parity_plan) {
      auto planned = build_plan_trim_without_structural_work(
          *parity_plan, patterns, {}, score_only_opts);
      check_multisite_trim_results_equal(score_only, planned);
    }

    larch::multisite_trim_options strict_opts;
    strict_opts.dominance_mode =
        larch::multisite_dominance_mode::strict_mask_safe;
    auto strict =
        larch::build_multisite_trim(grammar, patterns, {}, strict_opts);
    CHECK(strict.optimum == brute.optimum);
    CHECK(strict.keep_production == brute.keep_production);
    CHECK(strict.keep_production_exact);
    CHECK(strict.exact_mask_recovery_passes == 0);
    if (parity_plan) {
      auto planned = build_plan_trim_without_structural_work(
          *parity_plan, patterns, {}, strict_opts);
      check_multisite_trim_results_equal(strict, planned);
    }

    larch::multisite_trim_options two_pass_opts;
    two_pass_opts.dominance_mode =
        larch::multisite_dominance_mode::two_pass_exact_mask;
    auto two_pass =
        larch::build_multisite_trim(grammar, patterns, {}, two_pass_opts);
    CHECK(two_pass.optimum == brute.optimum);
    CHECK(two_pass.keep_production == brute.keep_production);
    CHECK(two_pass.keep_production_exact);
    CHECK(two_pass.exact_mask_recovery_passes == 1);
    if (parity_plan) {
      auto planned = build_plan_trim_without_structural_work(
          *parity_plan, patterns, {}, two_pass_opts);
      check_multisite_trim_results_equal(two_pass, planned);
    }

    if (trial % 3 == 0) {
      larch::chart_options with_reference_edge;
      with_reference_edge.score_ua_edge = true;
      auto brute_ua = larch::brute_force_multisite_topologies(
          grammar, patterns, with_reference_edge, 200000);
      auto bnb_ua =
          larch::build_multisite_trim(grammar, patterns, with_reference_edge);
      CHECK(bnb_ua.optimum == brute_ua.optimum);
      CHECK(bnb_ua.keep_production == brute_ua.keep_production);
      if (parity_plan) {
        auto planned = build_plan_trim_without_structural_work(
            *parity_plan, patterns, with_reference_edge, {});
        check_multisite_trim_results_equal(bnb_ua, planned);
      }
      auto coupled_ua = larch::build_multisite_coupled_frontier_trim(
          grammar, patterns, with_reference_edge);
      CHECK(coupled_ua.optimum == brute_ua.optimum);
      CHECK(coupled_ua.keep_production == brute_ua.keep_production);
      if (trial < 6) {
        auto enumerated_ua =
            larch::enumerate_multisite_coupled_frontier_topologies(
                grammar, coupled_ua, 0);
        CHECK(!enumerated_ua.topology_cap_truncated);
        CHECK(enumerated_ua.topologies.size() ==
              brute_ua.optimal_topology_count);
        for (auto const& topology : enumerated_ua.topologies) {
          CHECK(larch::score_selected_topology(
                    grammar, patterns, topology, with_reference_edge) ==
                brute_ua.optimum);
        }
      }
      auto score_only_ua = larch::build_multisite_trim(
          grammar, patterns, with_reference_edge, score_only_opts);
      CHECK(score_only_ua.optimum == brute_ua.optimum);
      CHECK(!score_only_ua.keep_production_exact);
      if (parity_plan) {
        auto planned = build_plan_trim_without_structural_work(
            *parity_plan, patterns, with_reference_edge, score_only_opts);
        check_multisite_trim_results_equal(score_only_ua, planned);
      }
      auto strict_ua = larch::build_multisite_trim(
          grammar, patterns, with_reference_edge, strict_opts);
      CHECK(strict_ua.optimum == brute_ua.optimum);
      CHECK(strict_ua.keep_production == brute_ua.keep_production);
      CHECK(strict_ua.keep_production_exact);
      if (parity_plan) {
        auto planned = build_plan_trim_without_structural_work(
            *parity_plan, patterns, with_reference_edge, strict_opts);
        check_multisite_trim_results_equal(strict_ua, planned);
      }
      auto two_pass_ua = larch::build_multisite_trim(
          grammar, patterns, with_reference_edge, two_pass_opts);
      CHECK(two_pass_ua.optimum == brute_ua.optimum);
      CHECK(two_pass_ua.keep_production == brute_ua.keep_production);
      CHECK(two_pass_ua.keep_production_exact);
      if (parity_plan) {
        auto planned = build_plan_trim_without_structural_work(
            *parity_plan, patterns, with_reference_edge, two_pass_opts);
        check_multisite_trim_results_equal(two_pass_ua, planned);
      }
    }
  }

  std::println("  PASS");
}

static void test_exhaustive_binary_assignments() {
  std::println("test_exhaustive_binary_assignments");

  for (std::uint32_t mask_bits = 0; mask_bits < 32; ++mask_bits) {
    std::array<char, 5> states{};
    for (std::size_t i = 0; i < states.size(); ++i)
      states[i] = (mask_bits & (1u << i)) ? 'C' : 'A';

    auto s = [](char c) { return std::string(1, c); };
    auto tree1 = larch::test::tiny_inner(
        "root", "A",
        {larch::test::tiny_inner("AB", "A",
                                 {larch::test::tiny_leaf("A", s(states[0])),
                                  larch::test::tiny_leaf("B", s(states[1]))}),
         larch::test::tiny_inner(
             "CDE", "A",
             {larch::test::tiny_leaf("C", s(states[2])),
              larch::test::tiny_inner(
                  "DE", "A",
                  {larch::test::tiny_leaf("D", s(states[3])),
                   larch::test::tiny_leaf("E", s(states[4]))})})});
    auto tree2 = larch::test::tiny_inner(
        "root", "A",
        {larch::test::tiny_inner("AB", "A",
                                 {larch::test::tiny_leaf("A", s(states[0])),
                                  larch::test::tiny_leaf("B", s(states[1]))}),
         larch::test::tiny_inner(
             "CDE", "A",
             {larch::test::tiny_leaf("D", s(states[3])),
              larch::test::tiny_inner(
                  "CE", "A",
                  {larch::test::tiny_leaf("C", s(states[2])),
                   larch::test::tiny_leaf("E", s(states[4]))})})});

    std::vector<larch::phylo_dag> trees;
    trees.push_back(larch::test::make_tiny_labelled_tree("A", tree1));
    trees.push_back(larch::test::make_tiny_labelled_tree("A", tree2));
    auto merged = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(merged);
    auto leaf_states = larch::extract_leaf_site_states(merged, grammar, 1);
    auto chart = larch::build_single_site_chart(grammar, leaf_states);
    auto outside = larch::build_single_site_outside_chart(grammar, chart);
    auto trim = larch::build_single_site_trim_mask(grammar, chart, outside);
    compare_outside_and_mask_to_bruteforce(grammar, leaf_states, chart, outside,
                                           trim);
  }

  std::println("  PASS");
}

int main() {
  test_paper_counterexample_outside_trim_and_traceback();
  test_single_tree_keeps_all_productions();
  test_reference_edge_outside_boundary();
  test_binary_outside_stack_recurrence_matches_generic();
  test_multisite_composite_counterexample();
  test_lazy_multisite_bnb_feeding_matches_dense();
  test_lazy_structural_pandemic_ratio_on_binary_tree();
  test_multisite_phase0_diagnostics_and_exactness_labels();
  test_multisite_coupled_frontier_annotation_exact();
  test_multisite_strict_mask_safe_dominance_matches_bruteforce();
  test_multisite_score_only_dominance_matches_bruteforce_and_labels();
  test_multisite_two_pass_dominated_suboptimal_does_not_overkeep();
  test_multisite_two_pass_exact_mask_matches_bruteforce();
  test_multisite_parent_combine_fixes_child_topology();
  test_multisite_concordant_sites_equal_lower_bound();
  test_multisite_invariant_sites_and_reference_edge_constant();
  test_plan_multisite_trim_strict_semantic_equivalence();
  test_multisite_exact_setup_cold_resident_and_lifetime();
  test_composite_reference_state_diagnostics();
  test_multisite_rejects_pattern_taxon_count_mismatch();
  test_multisite_equal_dedup_merges_provenance();
  test_multisite_all_invariant_coupled_frontier_with_reference_edge();
  test_multisite_randomized_tiny_bnb_matches_bruteforce();
  test_exhaustive_binary_assignments();

  std::println("All chart trim tests passed!");
  return 0;
}
