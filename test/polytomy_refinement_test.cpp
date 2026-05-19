#include <larch/chart_trim.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/rank3_rewrite.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
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

static bool throws_runtime_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

static larch::phylo_dag make_three_taxon_star() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA", tiny_inner("root", "AAAA",
                         {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA"),
                          tiny_leaf("C", "AAGA")}));
}

static larch::phylo_dag make_three_taxon_binary_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA",
      tiny_inner(
          "root", "AAAA",
          {tiny_leaf("A", "AAAA"),
           tiny_inner("BC", "AAAA",
                      {tiny_leaf("B", "CAAA"), tiny_leaf("C", "AAGA")})}));
}

static larch::phylo_dag make_four_taxon_star() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA", tiny_inner("root", "AAAA",
                         {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA"),
                          tiny_leaf("C", "AAGA"), tiny_leaf("D", "AAAT")}));
}

static larch::phylo_dag make_four_taxon_star_with_observed_bcd_alternatives() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  std::vector<larch::phylo_dag> trees;
  trees.push_back(make_four_taxon_star());
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAAA",
      tiny_inner("root", "AAAA",
                 {tiny_leaf("A", "AAAA"),
                  tiny_inner("BCD", "AAAA",
                             {tiny_leaf("B", "CAAA"),
                              tiny_inner("CD", "AAAA",
                                         {tiny_leaf("C", "AAGA"),
                                          tiny_leaf("D", "AAAT")})})})));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "AAAA",
      tiny_inner("root", "AAAA",
                 {tiny_leaf("A", "AAAA"),
                  tiny_inner("BCD", "AAAA",
                             {tiny_leaf("C", "AAGA"),
                              tiny_inner("BD", "AAAA",
                                         {tiny_leaf("B", "CAAA"),
                                          tiny_leaf("D", "AAAT")})})})));
  return larch::test::merge_tiny_trees(std::move(trees));
}

static larch::phylo_dag make_four_taxon_abcd_polytomy_with_ab() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return larch::test::make_tiny_labelled_tree(
      "AAAA",
      tiny_inner("root", "AAAA",
                 {tiny_inner("AB", "AAAA",
                             {tiny_leaf("A", "AAAA"), tiny_leaf("B", "CAAA")}),
                  tiny_leaf("C", "AAGA"), tiny_leaf("D", "AAAT")}));
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

static larch::production_id production_for(
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

static constexpr std::uint32_t k_no_test_shape_node =
    std::numeric_limits<std::uint32_t>::max();

struct test_refinement_shape_node {
  std::uint64_t mask = 0;
  std::uint32_t left = k_no_test_shape_node;
  std::uint32_t right = k_no_test_shape_node;
};

struct test_refinement_shape {
  std::vector<test_refinement_shape_node> nodes;
  std::uint32_t root = k_no_test_shape_node;
};

static std::map<std::string, std::string> sequences_from_columns(
    std::vector<std::string> const& labels,
    std::vector<std::vector<std::uint8_t>> const& columns) {
  std::map<std::string, std::string> result;
  for (auto const& label : labels) result.emplace(label, std::string{});
  for (auto const& column : columns) {
    CHECK(column.size() == labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
      CHECK(column[i] < larch::nuc_state_count);
      result[labels[i]].push_back("ACGT"[column[i]]);
    }
  }
  return result;
}

static std::map<std::string, std::string> sequences_from_states(
    std::vector<std::string> const& labels,
    std::vector<std::uint8_t> const& states) {
  return sequences_from_columns(labels, {states});
}

static larch::leaf_site_states leaf_states_for_labels(
    larch::clade_grammar const& grammar, std::vector<std::string> const& labels,
    std::vector<std::uint8_t> const& states) {
  CHECK(labels.size() == states.size());
  larch::leaf_site_states result;
  result.state_by_taxon.assign(grammar.taxa.id_to_sample_id.size(), 0);
  for (std::size_t i = 0; i < labels.size(); ++i) {
    CHECK(states[i] < larch::nuc_state_count);
    result.state_by_taxon[taxon_for(grammar, labels[i])] = states[i];
  }
  return result;
}

static std::string shape_serialized(test_refinement_shape const& shape,
                                    std::uint32_t node) {
  auto const& n = shape.nodes[node];
  if (n.left == k_no_test_shape_node) return std::to_string(n.mask);
  return "(" + shape_serialized(shape, n.left) + "," +
         shape_serialized(shape, n.right) + ")";
}

static test_refinement_shape combine_shapes(std::uint64_t mask,
                                            test_refinement_shape const& left,
                                            test_refinement_shape const& right) {
  test_refinement_shape result;
  result.nodes = left.nodes;
  auto right_offset = static_cast<std::uint32_t>(result.nodes.size());
  for (auto node : right.nodes) {
    if (node.left != k_no_test_shape_node) node.left += right_offset;
    if (node.right != k_no_test_shape_node) node.right += right_offset;
    result.nodes.push_back(node);
  }
  result.root = static_cast<std::uint32_t>(result.nodes.size());
  result.nodes.push_back(
      test_refinement_shape_node{mask, left.root, right.root + right_offset});
  return result;
}

static std::vector<test_refinement_shape> enumerate_shapes_for_mask(
    std::uint64_t mask) {
  CHECK(mask != 0);
  std::vector<test_refinement_shape> result;
  if (std::popcount(mask) == 1) {
    result.push_back(test_refinement_shape{
        std::vector<test_refinement_shape_node>{{mask}}, 0});
    return result;
  }

  auto lowest = mask & (~mask + 1);
  for (auto sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
    if ((sub & lowest) == 0) continue;
    auto other = mask ^ sub;
    if (other == 0) continue;
    auto left_shapes = enumerate_shapes_for_mask(sub);
    auto right_shapes = enumerate_shapes_for_mask(other);
    for (auto const& left : left_shapes) {
      for (auto const& right : right_shapes)
        result.push_back(combine_shapes(mask, left, right));
    }
  }

  std::sort(result.begin(), result.end(), [](auto const& lhs, auto const& rhs) {
    return shape_serialized(lhs, lhs.root) < shape_serialized(rhs, rhs.root);
  });
  result.erase(std::unique(result.begin(), result.end(), [](auto const& lhs,
                                                            auto const& rhs) {
                 return shape_serialized(lhs, lhs.root) ==
                        shape_serialized(rhs, rhs.root);
               }),
               result.end());
  return result;
}

static std::vector<test_refinement_shape> enumerate_refinement_shapes(
    std::size_t leaf_count) {
  CHECK(leaf_count > 0);
  CHECK(leaf_count < 64);
  return enumerate_shapes_for_mask((std::uint64_t{1} << leaf_count) - 1);
}

static std::string mask_name(std::vector<std::string> const& labels,
                             std::uint64_t mask) {
  std::string name = "I";
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if ((mask & (std::uint64_t{1} << i)) != 0) name += "_" + labels[i];
  }
  return name;
}

static larch::test::tiny_tree_node tiny_subtree_from_shape(
    test_refinement_shape const& shape, std::uint32_t node,
    std::vector<std::string> const& labels,
    std::map<std::string, std::string> const& sequences) {
  auto const& n = shape.nodes[node];
  if (n.left == k_no_test_shape_node) {
    auto leaf_index = static_cast<std::size_t>(std::countr_zero(n.mask));
    CHECK(leaf_index < labels.size());
    auto found = sequences.find(labels[leaf_index]);
    CHECK(found != sequences.end());
    return larch::test::tiny_leaf(labels[leaf_index], found->second);
  }
  return larch::test::tiny_inner(
      mask_name(labels, n.mask), "",
      {tiny_subtree_from_shape(shape, n.left, labels, sequences),
       tiny_subtree_from_shape(shape, n.right, labels, sequences)});
}

static larch::phylo_dag make_star_polytomy_tree(
    std::string const& reference, std::vector<std::string> const& labels,
    std::map<std::string, std::string> const& sequences) {
  std::vector<larch::test::tiny_tree_node> children;
  for (auto const& label : labels) {
    auto found = sequences.find(label);
    CHECK(found != sequences.end());
    children.push_back(larch::test::tiny_leaf(label, found->second));
  }
  return larch::test::make_tiny_labelled_tree(
      reference, larch::test::tiny_inner("root", "", std::move(children)));
}

static larch::phylo_dag make_binary_refinement_tree(
    std::string const& reference, std::vector<std::string> const& labels,
    std::map<std::string, std::string> const& sequences,
    test_refinement_shape const& shape) {
  return larch::test::make_tiny_labelled_tree(
      reference, tiny_subtree_from_shape(shape, shape.root, labels, sequences));
}

static larch::chart_cost explicit_refinement_score(
    std::string const& reference, std::vector<std::string> const& labels,
    std::map<std::string, std::string> const& sequences,
    test_refinement_shape const& shape) {
  auto tree = make_binary_refinement_tree(reference, labels, sequences, shape);
  return static_cast<larch::chart_cost>(
      larch::test::score_tree_fitch_parsimony(tree, false));
}

static larch::clade_id clade_for_mask(larch::clade_grammar const& grammar,
                                      std::vector<std::string> const& labels,
                                      std::uint64_t mask) {
  std::vector<std::string> sample_ids;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if ((mask & (std::uint64_t{1} << i)) != 0) sample_ids.push_back(labels[i]);
  }
  return clade_for(grammar, sample_ids);
}

static void collect_shape_productions(
    larch::clade_grammar const& grammar, std::vector<std::string> const& labels,
    test_refinement_shape const& shape, std::uint32_t node,
    std::vector<bool>& used) {
  auto const& n = shape.nodes[node];
  if (n.left == k_no_test_shape_node) return;
  auto parent = clade_for_mask(grammar, labels, n.mask);
  auto left = clade_for_mask(grammar, labels, shape.nodes[n.left].mask);
  auto right = clade_for_mask(grammar, labels, shape.nodes[n.right].mask);
  auto pid = production_for(grammar, parent, {left, right});
  CHECK(pid < used.size());
  used[pid] = true;
  collect_shape_productions(grammar, labels, shape, n.left, used);
  collect_shape_productions(grammar, labels, shape, n.right, used);
}

static std::vector<bool> shape_production_mask(
    larch::clade_grammar const& grammar, std::vector<std::string> const& labels,
    test_refinement_shape const& shape) {
  std::vector<bool> used(grammar.productions.size(), false);
  collect_shape_productions(grammar, labels, shape, shape.root, used);
  return used;
}

static std::vector<bool> traceback_production_mask(
    larch::clade_grammar const& grammar,
    larch::chart_traceback_result const& trace) {
  std::vector<bool> used(grammar.productions.size(), false);
  for (auto pid : trace.productions) {
    CHECK(pid != larch::no_production);
    CHECK(pid < grammar.productions.size());
    used[pid] = true;
  }
  return used;
}

static bool matches_any_enumerated_shape(
    larch::clade_grammar const& grammar, std::vector<std::string> const& labels,
    std::vector<test_refinement_shape> const& shapes,
    std::vector<bool> const& used) {
  for (auto const& shape : shapes) {
    if (shape_production_mask(grammar, labels, shape) == used) return true;
  }
  return false;
}

static std::map<std::vector<std::uint8_t>,
                std::pair<std::uint32_t,
                          std::array<std::uint32_t, larch::nuc_state_count>>>
canonical_pattern_weights(larch::site_pattern_set const& patterns) {
  std::map<std::vector<std::uint8_t>,
           std::pair<std::uint32_t,
                     std::array<std::uint32_t, larch::nuc_state_count>>>
      result;
  for (auto const& pattern : patterns.patterns) {
    auto [_, inserted] = result.emplace(
        pattern.state_by_taxon,
        std::make_pair(pattern.weight, pattern.reference_state_counts));
    CHECK(inserted);
  }
  return result;
}

static std::uint64_t count_derivations(larch::clade_grammar const& grammar) {
  std::vector<std::uint64_t> counts(grammar.clades.size(), 0);
  std::vector<larch::clade_id> order(grammar.clades.size());
  std::iota(order.begin(), order.end(), larch::clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](auto lhs, auto rhs) {
    auto const& ltaxa = grammar.clades[lhs].taxa;
    auto const& rtaxa = grammar.clades[rhs].taxa;
    if (ltaxa.size() != rtaxa.size()) return ltaxa.size() < rtaxa.size();
    return ltaxa < rtaxa;
  });

  for (auto cid : order) {
    if (grammar.clades[cid].taxa.size() == 1) {
      counts[cid] = 1;
      continue;
    }
    for (auto pid : grammar.productions_by_parent[cid]) {
      std::uint64_t prod_count = 1;
      for (auto child : grammar.productions[pid].children)
        prod_count *= counts[child];
      counts[cid] += prod_count;
    }
  }
  return counts[grammar.root_clade];
}

static larch::chart_cost brute_force_star_score(
    std::size_t leaf_count, std::vector<std::uint8_t> const& states) {
  using row_t = std::array<larch::chart_cost, larch::nuc_state_count>;
  constexpr auto inf = larch::chart_inf;

  std::map<std::uint64_t, std::vector<row_t>> memo;
  auto combine = [inf](row_t const& left, row_t const& right) {
    row_t out{};
    out.fill(inf);
    for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
         ++parent_state) {
      larch::chart_cost best_left = inf;
      larch::chart_cost best_right = inf;
      for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
           ++child_state) {
        auto left_cost =
            left[child_state] >= inf
                ? inf
                : left[child_state] + (parent_state == child_state ? 0 : 1);
        auto right_cost =
            right[child_state] >= inf
                ? inf
                : right[child_state] + (parent_state == child_state ? 0 : 1);
        best_left = std::min(best_left, left_cost);
        best_right = std::min(best_right, right_cost);
      }
      out[parent_state] = best_left + best_right;
    }
    return out;
  };

  auto rows_for = [&](auto& self,
                      std::uint64_t mask) -> std::vector<row_t> const& {
    auto found = memo.find(mask);
    if (found != memo.end()) return found->second;

    std::vector<row_t> rows;
    if (std::popcount(mask) == 1) {
      auto leaf_index = static_cast<std::size_t>(std::countr_zero(mask));
      row_t row{};
      row.fill(inf);
      row[states[leaf_index]] = 0;
      rows.push_back(row);
    } else {
      auto lowest = mask & (~mask + 1);
      for (auto sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
        if ((sub & lowest) == 0) continue;
        auto other = mask ^ sub;
        if (other == 0) continue;
        for (auto const& left : self(self, sub)) {
          for (auto const& right : self(self, other))
            rows.push_back(combine(left, right));
        }
      }
    }

    auto [it, _] = memo.emplace(mask, std::move(rows));
    return it->second;
  };

  auto full = (std::uint64_t{1} << leaf_count) - 1;
  larch::chart_cost best = inf;
  for (auto const& row : rows_for(rows_for, full))
    for (auto cost : row) best = std::min(best, cost);
  return best;
}

static larch::chart_cost best_explicit_star_refinement_score(
    std::string const& reference, std::vector<std::string> const& labels,
    std::map<std::string, std::string> const& sequences,
    std::vector<test_refinement_shape> const& shapes) {
  auto best = larch::chart_inf;
  for (auto const& shape : shapes)
    best = std::min(best,
                    explicit_refinement_score(reference, labels, sequences,
                                              shape));
  return best;
}

struct explicit_optimal_refinement_mask {
  larch::chart_cost score = larch::chart_inf;
  std::size_t topology_count = 0;
  std::vector<bool> keep_production;
};

static explicit_optimal_refinement_mask explicit_optimal_refinement_mask_for(
    larch::clade_grammar const& grammar, std::string const& reference,
    std::vector<std::string> const& labels,
    std::map<std::string, std::string> const& sequences,
    std::vector<test_refinement_shape> const& shapes) {
  explicit_optimal_refinement_mask result;
  result.keep_production.assign(grammar.productions.size(), false);
  for (auto const& shape : shapes) {
    auto score = explicit_refinement_score(reference, labels, sequences, shape);
    auto used = shape_production_mask(grammar, labels, shape);
    if (score < result.score) {
      result.score = score;
      result.topology_count = 0;
      std::fill(result.keep_production.begin(), result.keep_production.end(),
                false);
    }
    if (score == result.score) {
      ++result.topology_count;
      for (std::size_t pid = 0; pid < result.keep_production.size(); ++pid)
        result.keep_production[pid] = result.keep_production[pid] || used[pid];
    }
  }
  return result;
}

static larch::phylo_dag make_embedded_polytomy_tree(
    std::string const& reference,
    std::map<std::string, std::string> const& sequences) {
  auto leaf = [&](std::string const& label) {
    auto found = sequences.find(label);
    CHECK(found != sequences.end());
    return larch::test::tiny_leaf(label, found->second);
  };
  return larch::test::make_tiny_labelled_tree(
      reference,
      larch::test::tiny_inner(
          "root", "",
          {leaf("A"), larch::test::tiny_inner(
                          "BCD", "", {leaf("B"), leaf("C"), leaf("D")})}));
}

static larch::phylo_dag make_embedded_refinement_tree(
    std::string const& reference,
    std::map<std::string, std::string> const& sequences,
    test_refinement_shape const& bcd_shape) {
  auto leaf_a = larch::test::tiny_leaf("A", sequences.at("A"));
  auto bcd = tiny_subtree_from_shape(bcd_shape, bcd_shape.root,
                                     {"B", "C", "D"}, sequences);
  return larch::test::make_tiny_labelled_tree(
      reference,
      larch::test::tiny_inner("root", "", {std::move(leaf_a), std::move(bcd)}));
}

static larch::phylo_dag make_two_polytomy_tree(
    std::string const& reference,
    std::map<std::string, std::string> const& sequences) {
  auto leaf = [&](std::string const& label) {
    auto found = sequences.find(label);
    CHECK(found != sequences.end());
    return larch::test::tiny_leaf(label, found->second);
  };
  return larch::test::make_tiny_labelled_tree(
      reference,
      larch::test::tiny_inner(
          "root", "",
          {larch::test::tiny_inner("ABC", "",
                                   {leaf("A"), leaf("B"), leaf("C")}),
           larch::test::tiny_inner("DEF", "",
                                   {leaf("D"), leaf("E"), leaf("F")})}));
}

static larch::phylo_dag make_two_four_polytomy_tree(
    std::string const& reference,
    std::map<std::string, std::string> const& sequences) {
  auto leaf = [&](std::string const& label) {
    auto found = sequences.find(label);
    CHECK(found != sequences.end());
    return larch::test::tiny_leaf(label, found->second);
  };
  return larch::test::make_tiny_labelled_tree(
      reference,
      larch::test::tiny_inner(
          "root", "",
          {larch::test::tiny_inner(
               "ABCD", "", {leaf("A"), leaf("B"), leaf("C"), leaf("D")}),
           larch::test::tiny_inner(
               "EFGH", "", {leaf("E"), leaf("F"), leaf("G"), leaf("H")})}));
}

static bool same_grammar_signature(larch::clade_grammar const& lhs,
                                   larch::clade_grammar const& rhs) {
  if (lhs.root_clade != rhs.root_clade ||
      lhs.clades.size() != rhs.clades.size() ||
      lhs.productions.size() != rhs.productions.size()) {
    return false;
  }
  for (std::size_t cid = 0; cid < lhs.clades.size(); ++cid) {
    if (lhs.clades[cid].taxa != rhs.clades[cid].taxa) return false;
  }
  for (std::size_t pid = 0; pid < lhs.productions.size(); ++pid) {
    auto const& lp = lhs.productions[pid];
    auto const& rp = rhs.productions[pid];
    if (lp.parent != rp.parent || lp.children != rp.children ||
        lp.multiplicity != rp.multiplicity) {
      return false;
    }
  }
  return lhs.productions_by_parent == rhs.productions_by_parent &&
         lhs.productions_by_child == rhs.productions_by_child;
}

static larch::phylo_dag make_two_polytomy_refinement_tree(
    std::string const& reference,
    std::map<std::string, std::string> const& sequences,
    test_refinement_shape const& abc_shape,
    test_refinement_shape const& def_shape) {
  auto abc = tiny_subtree_from_shape(abc_shape, abc_shape.root,
                                     {"A", "B", "C"}, sequences);
  auto def = tiny_subtree_from_shape(def_shape, def_shape.root,
                                     {"D", "E", "F"}, sequences);
  return larch::test::make_tiny_labelled_tree(
      reference,
      larch::test::tiny_inner("root", "", {std::move(abc), std::move(def)}));
}

static void test_default_reject_mode_rejects_kary() {
  std::println("test_default_reject_mode_rejects_kary");

  auto dag = make_three_taxon_star();
  CHECK(throws_runtime_error(
      [&] { (void)larch::build_polytomy_refined_clade_grammar(dag); }));

  std::println("  PASS");
}

static void test_reject_mode_accepts_binary_grammar() {
  std::println("test_reject_mode_accepts_binary_grammar");

  auto dag = make_three_taxon_binary_tree();
  auto result = larch::build_polytomy_refined_clade_grammar(dag);
  auto const& grammar = result.grammar;

  CHECK(!larch::grammar_has_kary_productions(grammar));
  CHECK(larch::kary_productions(grammar).empty());
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.source_kary_production_count == 0);
  CHECK(result.source_grammar_audit.non_binary_production_count == 0);
  CHECK(!result.audit.contains_kary_productions);
  CHECK(result.audit.binary_chart_compatible);
  CHECK(result.audit.exact_for_soft_polytomies);
  CHECK(result.audit.events.empty());
  CHECK(result.clade_info.size() == grammar.clades.size());
  CHECK(result.production_info.size() == grammar.productions.size());
  for (std::size_t cid = 0; cid < result.source_clade_to_refined.size(); ++cid)
    CHECK(result.source_clade_to_refined[cid] == cid);
  for (std::size_t pid = 0; pid < result.source_production_to_refined.size();
       ++pid) {
    CHECK(result.source_production_to_refined[pid] == pid);
    CHECK(result.production_info[pid].origin ==
          larch::refined_production_origin::observed_binary);
    CHECK(result.production_info[pid].source_productions.size() == 1);
    CHECK(result.production_info[pid].source_productions.front() == pid);
  }

  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);
  CHECK(chart.inside.size() == grammar.clades.size());

  std::println("  PASS");
}

static void test_audit_kary_returns_diagnostic_grammar() {
  std::println("test_audit_kary_returns_diagnostic_grammar");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::audit_kary;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(grammar.productions.size() == 1);
  CHECK(grammar.productions.front().children.size() == 3);
  CHECK(larch::grammar_has_kary_productions(grammar));
  auto kary = larch::kary_productions(grammar);
  CHECK(kary.size() == 1);
  CHECK(kary.front() == 0);
  CHECK(!larch::grammar_is_binary_chart_compatible(grammar));

  CHECK(result.audit.source_clade_count == grammar.clades.size());
  CHECK(result.source_grammar_audit.non_binary_production_count == 1);
  CHECK(result.audit.source_production_count == grammar.productions.size());
  CHECK(result.audit.source_kary_production_count == 1);
  CHECK(result.audit.refined_clade_count == grammar.clades.size());
  CHECK(result.audit.refined_production_count == grammar.productions.size());
  CHECK(result.audit.synthetic_clade_count == 0);
  CHECK(result.audit.synthetic_production_count == 0);
  CHECK(result.audit.contains_kary_productions);
  CHECK(!result.audit.binary_chart_compatible);
  CHECK(!result.audit.exact_for_soft_polytomies);
  CHECK(!result.audit.any_truncated);
  CHECK(!result.audit.any_refused);

  CHECK(result.audit.events.size() == 1);
  auto const& event = result.audit.events.front();
  CHECK(event.source_production == 0);
  CHECK(event.parent == grammar.root_clade);
  CHECK(event.arity == 3);
  CHECK(event.source_multiplicity == grammar.productions.front().multiplicity);
  CHECK(!event.expanded);
  CHECK(!event.exact);

  CHECK(result.production_info.size() == 1);
  auto const& info = result.production_info.front();
  CHECK(info.origin ==
        larch::refined_production_origin::observed_kary_unexpanded);
  CHECK(info.source_productions.size() == 1);
  CHECK(info.source_productions.front() == 0);
  CHECK(!info.source_parent_nodes.empty());
  CHECK(!info.exact_refinement_component);

  std::println("  PASS");
}

static void test_binary_chart_rejects_audit_kary_grammar() {
  std::println("test_binary_chart_rejects_audit_kary_grammar");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::audit_kary;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto states = larch::extract_leaf_site_states(dag, result.grammar, 1);

  CHECK(throws_runtime_error(
      [&] { (void)larch::build_single_site_chart(result.grammar, states); }));

  std::println("  PASS");
}

static void test_exact_expansion_three_taxon_star() {
  std::println("test_exact_expansion_three_taxon_star");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(!larch::grammar_has_kary_productions(grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.binary_chart_compatible);
  CHECK(result.audit.exact_for_soft_polytomies);
  CHECK(!result.audit.contains_kary_productions);
  CHECK(result.audit.source_kary_production_count == 1);
  CHECK(result.audit.refined_clade_count == 7);
  CHECK(result.audit.refined_production_count == 6);
  CHECK(result.audit.synthetic_clade_count == 3);
  CHECK(result.audit.synthetic_production_count == 6);
  CHECK(result.audit.events.size() == 1);
  CHECK(result.audit.events.front().expanded);
  CHECK(result.audit.events.front().exact);
  CHECK(result.audit.events.front().arity == 3);
  CHECK(result.audit.events.front().new_clades_added == 3);
  CHECK(result.audit.events.front().new_productions_added == 6);
  CHECK(result.audit.events.front().represented_refinement_count == 3);
  CHECK(count_derivations(grammar) == 3);

  (void)clade_for(grammar, {"A", "B"});
  (void)clade_for(grammar, {"A", "C"});
  (void)clade_for(grammar, {"B", "C"});

  for (auto pid : result.source_production_to_refined)
    CHECK(pid == larch::no_production);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    CHECK(grammar.productions[pid].children.size() == 2);
    CHECK(grammar.productions[pid].multiplicity == 0);
    CHECK(result.production_info[pid].origin ==
          larch::refined_production_origin::synthetic_polytomy_refinement);
    CHECK(result.production_info[pid].source_productions.size() == 1);
  }

  auto states = larch::extract_leaf_site_states(dag, grammar, 1);
  auto chart = larch::build_single_site_chart(grammar, states);
  CHECK(chart.root_min_excluding_ua(grammar.root_clade) == 1);

  std::println("  PASS");
}

static void test_exact_expansion_four_taxon_star_matches_bruteforce() {
  std::println("test_exact_expansion_four_taxon_star_matches_bruteforce");

  auto dag = make_four_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.refined_clade_count == 15);
  CHECK(result.audit.refined_production_count == 25);
  CHECK(result.audit.events.size() == 1);
  CHECK(result.audit.events.front().represented_refinement_count == 15);
  CHECK(count_derivations(grammar) == 15);

  for (std::uint32_t packed = 0; packed < 256; ++packed) {
    larch::leaf_site_states states;
    states.state_by_taxon.assign(grammar.taxa.id_to_sample_id.size(), 0);
    std::vector<std::uint8_t> ordered_states(4, 0);
    for (std::size_t i = 0; i < 4; ++i) {
      auto state = static_cast<std::uint8_t>((packed >> (2 * i)) & 3U);
      ordered_states[i] = state;
      std::string sample(1, static_cast<char>('A' + i));
      states.state_by_taxon[taxon_for(grammar, sample)] = state;
    }

    auto chart = larch::build_single_site_chart(grammar, states);
    auto chart_best = chart.root_min_excluding_ua(grammar.root_clade);
    CHECK(chart_best == brute_force_star_score(4, ordered_states));
  }

  std::println("  PASS");
}

static void test_exact_expansion_reuses_existing_observed_clade() {
  std::println("test_exact_expansion_reuses_existing_observed_clade");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(make_three_taxon_star());
  trees.push_back(make_three_taxon_binary_tree());
  auto dag = larch::test::merge_tiny_trees(std::move(trees));

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(result.audit.source_kary_production_count == 1);
  CHECK(result.audit.synthetic_clade_count ==
        2);  // AB and AC; BC was observed.
  CHECK(result.audit.refined_clade_count == 7);
  CHECK(count_derivations(grammar) == 3);

  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto bc = clade_for(grammar, {"B", "C"});
  CHECK(result.clade_info[bc].origin ==
        larch::refined_clade_origin::observed_and_synthetic);

  auto bc_prod = production_for(grammar, bc, {b, c});
  CHECK(grammar.productions[bc_prod].multiplicity > 0);
  CHECK(result.production_info[bc_prod].origin ==
        larch::refined_production_origin::observed_and_synthetic);
  CHECK(result.production_info[bc_prod].source_productions.size() == 2);

  std::println("  PASS");
}

static void test_exact_expansion_merges_reversed_observed_witnesses() {
  std::println("test_exact_expansion_merges_reversed_observed_witnesses");

  larch::clade_grammar_build_result built;
  auto& source = built.grammar;
  source.taxa.id_to_sample_id = {"A", "B", "C"};
  source.taxa.sample_id_to_id = {{"A", 0}, {"B", 1}, {"C", 2}};
  source.clades = {larch::clade_key{{0}}, larch::clade_key{{1}},
                   larch::clade_key{{2}}, larch::clade_key{{0, 1}},
                   larch::clade_key{{0, 1, 2}}};
  source.root_clade = 4;

  auto witness = [](std::size_t parent_node,
                    std::vector<larch::clade_id> children) {
    larch::production_witness w;
    w.parent_node = parent_node;
    for (auto child : children) {
      larch::production_child_witness child_witness;
      child_witness.child = child;
      child_witness.edge_alternatives = {parent_node * 10 + child};
      w.children.push_back(std::move(child_witness));
    }
    return w;
  };

  auto make_prod = [&](larch::clade_id parent,
                       std::vector<larch::clade_id> children,
                       std::size_t parent_node) {
    larch::grammar_production prod;
    prod.parent = parent;
    prod.children = children;
    prod.witnesses = {witness(parent_node, children)};
    prod.multiplicity = 1;
    return prod;
  };

  source.productions.push_back(make_prod(3, {0, 1}, 10));
  source.productions.push_back(make_prod(3, {1, 0}, 11));
  source.productions.push_back(make_prod(4, {0, 1, 2}, 12));

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  opts.canonicalize_binary_children = true;
  auto result = larch::polytomy_refinement_detail::make_exact_expansion_result(
      std::move(built), opts);
  auto const& grammar = result.grammar;

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto ab_prod = production_for(grammar, ab, {a, b});

  CHECK(grammar.productions[ab_prod].multiplicity == 2);
  CHECK(grammar.productions[ab_prod].witnesses.size() == 2);
  CHECK(result.production_info[ab_prod].origin ==
        larch::refined_production_origin::observed_and_synthetic);
  CHECK(result.production_info[ab_prod].source_productions.size() == 3);

  std::println("  PASS");
}

static void test_exact_expansion_merges_synthetic_provenance() {
  std::println("test_exact_expansion_merges_synthetic_provenance");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(make_four_taxon_star());
  trees.push_back(make_four_taxon_abcd_polytomy_with_ab());
  auto dag = larch::test::merge_tiny_trees(std::move(trees));

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;

  CHECK(result.audit.source_kary_production_count == 2);
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto cd_prod = production_for(grammar, cd, {c, d});
  CHECK(result.production_info[cd_prod].origin ==
        larch::refined_production_origin::synthetic_polytomy_refinement);
  CHECK(result.production_info[cd_prod].source_productions.size() == 2);
  CHECK(grammar.productions[cd_prod].multiplicity == 0);

  std::println("  PASS");
}

static void test_exact_expansion_caps_throw() {
  std::println("test_exact_expansion_caps_throw");

  auto dag = make_four_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  opts.max_exact_arity = 3;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, opts);
  }));

  opts.max_exact_arity = 6;
  opts.max_new_productions_per_polytomy = 5;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, opts);
  }));

  std::println("  PASS");
}

static void test_binary_compatibility_rejects_out_of_range_taxa() {
  std::println("test_binary_compatibility_rejects_out_of_range_taxa");

  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A"};
  grammar.taxa.sample_id_to_id.emplace("A", 0);
  grammar.clades.push_back(larch::clade_key{{1}});
  grammar.productions_by_parent.push_back({});
  grammar.productions_by_child.push_back({});
  grammar.root_clade = 0;

  CHECK(!larch::grammar_is_binary_chart_compatible(grammar));

  std::println("  PASS");
}

static void test_phase2_three_taxon_all_patterns_match_explicit_refinements() {
  std::println(
      "test_phase2_three_taxon_all_patterns_match_explicit_refinements");

  std::vector<std::string> labels{"A", "B", "C"};
  std::string reference = "A";
  auto shapes = enumerate_refinement_shapes(labels.size());
  CHECK(shapes.size() == 3);

  auto base_dag = make_star_polytomy_tree(
      reference, labels, sequences_from_states(labels, {0, 0, 0}));
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      base_dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(count_derivations(grammar) == shapes.size());

  larch::chart_options chart_opts;
  chart_opts.keep_trace = true;

  for (std::uint32_t packed = 0; packed < 64; ++packed) {
    std::vector<std::uint8_t> states(labels.size(), 0);
    for (std::size_t i = 0; i < labels.size(); ++i)
      states[i] = static_cast<std::uint8_t>((packed >> (2 * i)) & 3U);

    auto leaf_states = leaf_states_for_labels(grammar, labels, states);
    auto chart = larch::build_single_site_chart(grammar, leaf_states,
                                                chart_opts);
    auto outside = larch::build_single_site_outside_chart(grammar, chart,
                                                          chart_opts);
    auto chart_best = chart.root_min_excluding_ua(grammar.root_clade);
    auto explicit_best = best_explicit_star_refinement_score(
        reference, labels, sequences_from_states(labels, states), shapes);
    CHECK(chart_best == explicit_best);
    CHECK(outside.global_min == chart_best);

    auto trace = larch::deterministic_optimal_single_site_traceback(
        grammar, chart, outside);
    CHECK(trace.score == chart_best);
    auto topology = larch::grammar_topology_from_productions(
        grammar, trace.productions);
    (void)larch::validate_grammar_topology(grammar, topology);
    CHECK(matches_any_enumerated_shape(
        grammar, labels, shapes, traceback_production_mask(grammar, trace)));
  }

  std::println("  PASS");
}

static void test_phase2_four_taxon_random_patterns_match_explicit_refinements() {
  std::println(
      "test_phase2_four_taxon_random_patterns_match_explicit_refinements");

  std::vector<std::string> labels{"A", "B", "C", "D"};
  std::string reference = "A";
  auto shapes = enumerate_refinement_shapes(labels.size());
  CHECK(shapes.size() == 15);

  auto base_dag = make_star_polytomy_tree(
      reference, labels, sequences_from_states(labels, {0, 0, 0, 0}));
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      base_dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;
  CHECK(larch::grammar_is_binary_chart_compatible(grammar));
  CHECK(count_derivations(grammar) == shapes.size());

  std::uint32_t rng = 0x5eed1234U;
  for (std::size_t trial = 0; trial < 32; ++trial) {
    std::vector<std::uint8_t> states(labels.size(), 0);
    for (std::size_t i = 0; i < labels.size(); ++i) {
      rng = rng * 1664525U + 1013904223U;
      states[i] = static_cast<std::uint8_t>((rng >> 30) & 3U);
    }

    auto leaf_states = leaf_states_for_labels(grammar, labels, states);
    auto chart = larch::build_single_site_chart(grammar, leaf_states);
    auto chart_best = chart.root_min_excluding_ua(grammar.root_clade);
    auto explicit_best = best_explicit_star_refinement_score(
        reference, labels, sequences_from_states(labels, states), shapes);
    CHECK(chart_best == explicit_best);
  }

  std::println("  PASS");
}

static void test_phase2_traceback_trim_and_materialization() {
  std::println("test_phase2_traceback_trim_and_materialization");

  std::vector<std::string> labels{"A", "B", "C"};
  std::string reference = "A";
  std::vector<std::uint8_t> states{0, 1, 2};
  auto sequences = sequences_from_states(labels, states);
  auto dag = make_star_polytomy_tree(reference, labels, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;
  auto shapes = enumerate_refinement_shapes(labels.size());

  larch::chart_options chart_opts;
  chart_opts.keep_trace = true;
  auto leaf_states = leaf_states_for_labels(grammar, labels, states);
  auto chart = larch::build_single_site_chart(grammar, leaf_states,
                                              chart_opts);
  auto outside = larch::build_single_site_outside_chart(grammar, chart,
                                                        chart_opts);
  auto trim = larch::build_single_site_trim_mask(grammar, chart, outside);

  auto optimal = explicit_optimal_refinement_mask_for(grammar, reference, labels,
                                                       sequences, shapes);
  CHECK(trim.global_min == optimal.score);
  CHECK(trim.keep_production == optimal.keep_production);

  auto trace = larch::deterministic_optimal_single_site_traceback(
      grammar, chart, outside);
  CHECK(trace.score == optimal.score);
  CHECK(matches_any_enumerated_shape(
      grammar, labels, shapes, traceback_production_mask(grammar, trace)));
  auto topology = larch::grammar_topology_from_productions(grammar,
                                                           trace.productions);
  (void)larch::validate_grammar_topology(grammar, topology);

  auto materialized = larch::materialize_rank3_tree_from_topology(
      dag, grammar, topology);
  CHECK(larch::test::score_tree_fitch_parsimony(materialized, false) ==
        optimal.score);
  auto materialized_grammar = larch::build_clade_grammar(materialized);
  CHECK(larch::grammar_is_binary_chart_compatible(materialized_grammar));
  CHECK(count_derivations(materialized_grammar) == 1);

  std::vector<std::string> labels4{"A", "B", "C", "D"};
  std::vector<std::uint8_t> states4{0, 0, 1, 1};
  auto sequences4 = sequences_from_states(labels4, states4);
  auto dag4 = make_star_polytomy_tree(reference, labels4, sequences4);
  auto result4 = larch::build_polytomy_refined_clade_grammar(
      dag4, larch::clade_grammar_options{}, opts);
  auto const& grammar4 = result4.grammar;
  auto shapes4 = enumerate_refinement_shapes(labels4.size());
  auto leaf_states4 = leaf_states_for_labels(grammar4, labels4, states4);
  auto chart4 = larch::build_single_site_chart(grammar4, leaf_states4);
  auto outside4 = larch::build_single_site_outside_chart(grammar4, chart4);
  auto trim4 = larch::build_single_site_trim_mask(grammar4, chart4, outside4);
  auto optimal4 = explicit_optimal_refinement_mask_for(
      grammar4, reference, labels4, sequences4, shapes4);

  auto kept4 = std::count(trim4.keep_production.begin(),
                          trim4.keep_production.end(), true);
  CHECK(optimal4.topology_count > 0);
  CHECK(optimal4.topology_count < shapes4.size());
  CHECK(trim4.global_min == optimal4.score);
  CHECK(trim4.keep_production == optimal4.keep_production);
  CHECK(kept4 < static_cast<std::ptrdiff_t>(grammar4.productions.size()));

  std::println("  PASS");
}

static void test_phase2_site_patterns_are_topology_independent() {
  std::println("test_phase2_site_patterns_are_topology_independent");

  std::vector<std::string> labels{"A", "B", "C", "D"};
  std::vector<std::vector<std::uint8_t>> columns{
      {0, 0, 1, 1}, {0, 1, 0, 1}, {0, 1, 1, 0}, {2, 2, 2, 2}, {3, 0, 1, 2}};
  std::string reference(columns.size(), 'A');
  auto sequences = sequences_from_columns(labels, columns);
  auto dag = make_star_polytomy_tree(reference, labels, sequences);

  larch::polytomy_refinement_options audit_opts;
  audit_opts.mode = larch::polytomy_mode::audit_kary;
  auto audit = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, audit_opts);
  CHECK(audit.audit.contains_kary_productions);

  larch::polytomy_refinement_options exact_opts;
  exact_opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto exact = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, exact_opts);
  CHECK(count_derivations(exact.grammar) == 15);

  larch::site_pattern_options pattern_opts;
  pattern_opts.build_normalized_binary_patterns = true;
  auto audit_patterns = larch::build_site_patterns(dag, audit.grammar,
                                                   pattern_opts);
  auto exact_patterns = larch::build_site_patterns(dag, exact.grammar,
                                                   pattern_opts);

  CHECK(audit_patterns.total_site_count == exact_patterns.total_site_count);
  CHECK(audit_patterns.taxon_count == exact_patterns.taxon_count);
  CHECK(audit_patterns.invariant_site_count ==
        exact_patterns.invariant_site_count);
  CHECK(audit_patterns.variable_site_count == exact_patterns.variable_site_count);
  CHECK(audit_patterns.binary_variable_site_count ==
        exact_patterns.binary_variable_site_count);
  CHECK(audit_patterns.nonbinary_variable_site_count ==
        exact_patterns.nonbinary_variable_site_count);
  CHECK(audit_patterns.patterns.size() == exact_patterns.patterns.size());
  CHECK(audit_patterns.normalized_binary_patterns.size() ==
        exact_patterns.normalized_binary_patterns.size());
  CHECK(canonical_pattern_weights(audit_patterns) ==
        canonical_pattern_weights(exact_patterns));

  std::println("  PASS");
}

static void test_phase2_multisite_bnb_matches_explicit_refinements() {
  std::println("test_phase2_multisite_bnb_matches_explicit_refinements");

  std::vector<std::string> labels{"A", "B", "C", "D"};
  std::vector<std::vector<std::uint8_t>> columns{
      {0, 0, 1, 1}, {0, 1, 0, 1}, {0, 1, 1, 0}, {0, 2, 3, 1}};
  std::string reference(columns.size(), 'A');
  auto sequences = sequences_from_columns(labels, columns);
  auto dag = make_star_polytomy_tree(reference, labels, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;
  auto shapes = enumerate_refinement_shapes(labels.size());
  CHECK(count_derivations(grammar) == shapes.size());

  auto patterns = larch::build_site_patterns(dag, grammar);
  larch::multisite_trim_options trim_opts;
  trim_opts.use_bound_pruning = false;
  auto exhaustive_trim = larch::build_multisite_trim(
      grammar, patterns, larch::chart_options{}, trim_opts);
  auto default_bnb = larch::build_multisite_trim(
      grammar, patterns, larch::chart_options{},
      larch::multisite_trim_options{});
  auto brute = larch::brute_force_multisite_topologies(
      grammar, patterns, larch::chart_options{}, 1000);
  CHECK(brute.topology_count == shapes.size());

  std::uint64_t explicit_best = larch::multisite_score_inf;
  for (auto const& shape : shapes) {
    auto tree = make_binary_refinement_tree(reference, labels, sequences, shape);
    auto score = static_cast<std::uint64_t>(
        larch::test::score_tree_fitch_parsimony(tree, false));
    explicit_best = std::min(explicit_best, score);
  }

  CHECK(exhaustive_trim.optimum == explicit_best);
  CHECK(default_bnb.optimum == explicit_best);
  CHECK(brute.optimum == explicit_best);
  CHECK(exhaustive_trim.keep_production == brute.keep_production);
  CHECK(default_bnb.keep_production == brute.keep_production);
  CHECK(default_bnb.initial_upper_bound < larch::multisite_score_inf);

  larch::multisite_topology_trace_options trace_opts;
  trace_opts.max_optimal_topologies = shapes.size();
  trace_opts.trim_options.use_bound_pruning = false;
  auto traced = larch::build_multisite_optimal_topologies(
      grammar, patterns, larch::chart_options{}, trace_opts);
  CHECK(traced.optimum == explicit_best);
  CHECK(!traced.topologies.empty());
  for (auto const& topology : traced.topologies) {
    (void)larch::validate_grammar_topology(grammar, topology);
    CHECK(larch::score_selected_topology(grammar, patterns, topology) ==
          explicit_best);
  }

  std::println("  PASS");
}

static void test_phase2_embedded_polytomy_matches_explicit_refinements() {
  std::println("test_phase2_embedded_polytomy_matches_explicit_refinements");

  std::vector<std::string> labels{"A", "B", "C", "D"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels, {0, 1, 2, 3});
  auto dag = make_embedded_polytomy_tree(reference, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;
  CHECK(result.audit.events.size() == 1);
  CHECK(result.audit.events.front().arity == 3);
  CHECK(count_derivations(grammar) == 3);

  auto states = leaf_states_for_labels(grammar, labels, {0, 1, 2, 3});
  auto chart = larch::build_single_site_chart(grammar, states);
  auto chart_best = chart.root_min_excluding_ua(grammar.root_clade);

  auto bcd_shapes = enumerate_refinement_shapes(3);
  auto explicit_best = larch::chart_inf;
  for (auto const& shape : bcd_shapes) {
    auto tree = make_embedded_refinement_tree(reference, sequences, shape);
    explicit_best = std::min(
        explicit_best, static_cast<larch::chart_cost>(
                           larch::test::score_tree_fitch_parsimony(tree,
                                                                    false)));
  }
  CHECK(chart_best == explicit_best);

  std::println("  PASS");
}

static void test_phase2_multiple_independent_polytomies_match_explicit() {
  std::println("test_phase2_multiple_independent_polytomies_match_explicit");

  std::vector<std::string> labels{"A", "B", "C", "D", "E", "F"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels, {0, 1, 2, 0, 1, 3});
  auto dag = make_two_polytomy_tree(reference, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto const& grammar = result.grammar;
  CHECK(result.audit.events.size() == 2);
  CHECK(count_derivations(grammar) == 9);

  auto states = leaf_states_for_labels(grammar, labels, {0, 1, 2, 0, 1, 3});
  auto chart = larch::build_single_site_chart(grammar, states);
  auto chart_best = chart.root_min_excluding_ua(grammar.root_clade);

  auto abc_shapes = enumerate_refinement_shapes(3);
  auto def_shapes = enumerate_refinement_shapes(3);
  auto explicit_best = larch::chart_inf;
  for (auto const& abc : abc_shapes) {
    for (auto const& def : def_shapes) {
      auto tree = make_two_polytomy_refinement_tree(reference, sequences, abc,
                                                    def);
      explicit_best = std::min(
          explicit_best, static_cast<larch::chart_cost>(
                             larch::test::score_tree_fitch_parsimony(tree,
                                                                      false)));
    }
  }
  CHECK(chart_best == explicit_best);

  std::println("  PASS");
}

static void test_phase3_bounded_large_star_truncated_repeatable() {
  std::println("test_phase3_bounded_large_star_truncated_repeatable");

  std::vector<std::string> labels{"A", "B", "C", "D", "E", "F", "G"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels, {0, 1, 2, 3, 0, 1, 2});

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 4;
  opts.max_bounded_productions_per_polytomy = 128;

  auto dag1 = make_star_polytomy_tree(reference, labels, sequences);
  auto result1 = larch::build_polytomy_refined_clade_grammar(
      dag1, larch::clade_grammar_options{}, opts);
  auto dag2 = make_star_polytomy_tree(reference, labels, sequences);
  auto result2 = larch::build_polytomy_refined_clade_grammar(
      dag2, larch::clade_grammar_options{}, opts);

  CHECK(same_grammar_signature(result1.grammar, result2.grammar));
  CHECK(larch::grammar_is_binary_chart_compatible(result1.grammar));
  CHECK(!result1.audit.contains_kary_productions);
  CHECK(!result1.audit.exact_for_soft_polytomies);
  CHECK(result1.audit.any_truncated);
  CHECK(result1.audit.events.size() == 1);
  auto const& event = result1.audit.events.front();
  CHECK(event.expanded);
  CHECK(!event.exact);
  CHECK(event.truncated_by_shape_cap);
  CHECK(!event.truncated_by_production_cap);
  CHECK(event.selected_seed_shape_count == 4);
  CHECK(event.represented_refinement_count >= event.selected_seed_shape_count);
  CHECK(throws_runtime_error([&] {
    larch::require_polytomy_refinement_exact_for_soft_polytomies(
        result1.audit, "bounded phase3 test");
  }));
  CHECK(count_derivations(result1.grammar) ==
        event.represented_refinement_count);

  auto states = leaf_states_for_labels(result1.grammar, labels,
                                       {0, 1, 2, 3, 0, 1, 2});
  auto chart = larch::build_single_site_chart(result1.grammar, states);
  CHECK(chart.root_min_excluding_ua(result1.grammar.root_clade) <
        larch::chart_inf);

  CHECK(result1.audit.events.front().represented_refinement_count ==
        result2.audit.events.front().represented_refinement_count);
  CHECK(result1.audit.synthetic_clade_count ==
        result2.audit.synthetic_clade_count);
  CHECK(result1.audit.synthetic_production_count ==
        result2.audit.synthetic_production_count);

  std::println("  PASS");
}

static void test_phase3_bounded_production_cap_rejects_incomplete_shape() {
  std::println("test_phase3_bounded_production_cap_rejects_incomplete_shape");

  std::vector<std::string> labels{"A", "B", "C", "D"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels, {0, 1, 2, 3});
  auto dag = make_star_polytomy_tree(reference, labels, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 16;
  opts.max_bounded_productions_per_polytomy = 2;
  CHECK(throws_runtime_error([&] {
    (void)larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, opts);
  }));

  std::println("  PASS");
}

static void test_phase3_bounded_multiple_polytomies_fair_budget() {
  std::println("test_phase3_bounded_multiple_polytomies_fair_budget");

  std::vector<std::string> labels{"A", "B", "C", "D",
                                  "E", "F", "G", "H"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels, {0, 1, 2, 3, 0, 1, 2, 3});
  auto dag = make_two_four_polytomy_tree(reference, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 16;
  opts.max_bounded_productions_per_polytomy = 64;
  opts.max_total_new_productions = 6;

  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  CHECK(larch::grammar_is_binary_chart_compatible(result.grammar));
  CHECK(result.audit.events.size() == 2);
  CHECK(result.audit.synthetic_production_count == 6);
  CHECK(result.audit.any_truncated);
  CHECK(!result.audit.exact_for_soft_polytomies);
  for (auto const& event : result.audit.events) {
    CHECK(event.selected_seed_shape_count == 1);
    CHECK(event.new_productions_added == 3);
    CHECK(event.truncated_by_production_cap);
  }

  std::println("  PASS");
}

static void test_phase3_bounded_reports_shared_intermediate_recombination() {
  std::println(
      "test_phase3_bounded_reports_shared_intermediate_recombination");

  std::vector<std::string> labels{"A", "B", "C", "D", "E", "F"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels, {0, 1, 2, 3, 0, 1});

  bool found_recombination = false;
  for (std::size_t shape_cap = 2; shape_cap <= 64 && !found_recombination;
       ++shape_cap) {
    auto dag = make_star_polytomy_tree(reference, labels, sequences);
    larch::polytomy_refinement_options opts;
    opts.mode = larch::polytomy_mode::expand_soft_bounded;
    opts.max_shapes_per_polytomy = shape_cap;
    opts.max_bounded_productions_per_polytomy = 1024;
    auto result = larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, opts);
    auto const& event = result.audit.events.front();
    CHECK(count_derivations(result.grammar) ==
          event.represented_refinement_count);
    if (event.represented_refinement_count > event.selected_seed_shape_count) {
      CHECK(!result.audit.exact_for_soft_polytomies);
      CHECK(result.audit.any_truncated);
      found_recombination = true;
    }
  }

  CHECK(found_recombination);

  std::println("  PASS");
}

static void test_phase3_bounded_large_arity_uses_capped_generator() {
  std::println("test_phase3_bounded_large_arity_uses_capped_generator");

  std::vector<std::string> labels{"A", "B", "C", "D", "E",
                                  "F", "G", "H", "I", "J"};
  std::string reference = "A";
  auto sequences = sequences_from_states(labels,
                                         {0, 1, 2, 3, 0, 1, 2, 3, 0, 1});
  auto dag = make_star_polytomy_tree(reference, labels, sequences);

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 2;
  opts.max_bounded_productions_per_polytomy = 64;

  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  CHECK(larch::grammar_is_binary_chart_compatible(result.grammar));
  CHECK(result.audit.events.size() == 1);
  auto const& event = result.audit.events.front();
  CHECK(event.arity == 10);
  CHECK(event.selected_seed_shape_count == 2);
  CHECK(event.truncated_by_shape_cap);
  CHECK(!event.exact);
  CHECK(event.represented_refinement_count >= event.selected_seed_shape_count);

  std::println("  PASS");
}

static void test_phase3_bounded_counts_final_reused_intermediates() {
  std::println("test_phase3_bounded_counts_final_reused_intermediates");

  auto dag = make_four_taxon_star_with_observed_bcd_alternatives();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 1;
  opts.max_bounded_productions_per_polytomy = 64;

  auto result = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  CHECK(larch::grammar_is_binary_chart_compatible(result.grammar));
  CHECK(result.audit.events.size() == 1);
  auto const& event = result.audit.events.front();
  CHECK(event.selected_seed_shape_count == 1);
  CHECK(event.represented_refinement_count ==
        count_derivations(result.grammar));
  CHECK(event.represented_refinement_count > event.selected_seed_shape_count);
  CHECK(!event.exact);
  CHECK(result.audit.any_truncated);

  std::println("  PASS");
}

static void set_all_edge_weights(larch::phylo_dag& dag, float weight) {
  for (auto ev : dag.get_all_edges()) {
    std::visit([&](auto edge) { edge.edge_weight() = weight; }, ev);
  }
}

static double min_edge_weight(larch::phylo_dag& dag) {
  double best = std::numeric_limits<double>::infinity();
  for (auto ev : dag.get_all_edges()) {
    std::visit([&](auto edge) {
      best = std::min(best, static_cast<double>(edge.edge_weight()));
    }, ev);
  }
  return best;
}

static larch::rank3_topology three_taxon_refined_topology_a_bc(
    larch::clade_grammar const& grammar) {
  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto bc = clade_for(grammar, {"B", "C"});
  auto root = grammar.root_clade;
  auto bc_prod = production_for(grammar, bc, {b, c});
  auto root_prod = production_for(grammar, root, {a, bc});
  return larch::rank3_topology_from_productions(grammar,
                                                {root_prod, bc_prod});
}

static void test_phase5_traceback_through_polytomy_materializes_and_merges() {
  std::println("test_phase5_traceback_through_polytomy_materializes_and_merges");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);

  larch::chart_options chart_opts;
  chart_opts.keep_trace = true;
  auto states = larch::extract_leaf_site_states(dag, refinement.grammar, 1);
  auto chart = larch::build_single_site_chart(refinement.grammar, states,
                                              chart_opts);
  auto outside = larch::build_single_site_outside_chart(refinement.grammar,
                                                        chart, chart_opts);
  auto trace = larch::deterministic_optimal_single_site_traceback(
      refinement.grammar, chart, outside);
  auto topology = larch::rank3_topology_from_traceback(refinement.grammar,
                                                       trace);

  std::size_t synthetic_trace_productions = 0;
  for (auto pid : trace.productions) {
    if (larch::refined_production_has_synthetic_polytomy_provenance(
            refinement, pid)) {
      ++synthetic_trace_productions;
    }
  }
  CHECK(synthetic_trace_productions == 2);

  auto result = larch::materialize_rank3_option_a(dag, refinement, topology);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.all_intended_productions_present());
  CHECK(result.all_intended_synthetic_splits_reappear());
  CHECK(result.materialization_audit.synthetic_productions_in_materialized_topologies ==
        synthetic_trace_productions);
  CHECK(larch::grammar_is_binary_chart_compatible(
      result.rebuilt_refinement.grammar));

  std::println("  PASS");
}

static void test_phase5_option_a_materializes_synthetic_polytomy_topology() {
  std::println("test_phase5_option_a_materializes_synthetic_polytomy_topology");

  auto dag = make_three_taxon_star();
  set_all_edge_weights(dag, 7.0f);
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto topology = three_taxon_refined_topology_a_bc(refinement.grammar);

  CHECK(throws_runtime_error([&] {
    (void)larch::materialize_rank3_option_a(dag, refinement.grammar,
                                            topology);
  }));

  auto result = larch::materialize_rank3_option_a(dag, refinement, topology);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.rebuilt_base.audit.non_binary_production_count == 1);
  CHECK(result.rebuilt_refinement.audit.source_kary_production_count == 1);
  CHECK(larch::grammar_is_binary_chart_compatible(
      result.rebuilt_refinement.grammar));
  CHECK(result.all_intended_productions_present());
  CHECK(result.all_intended_synthetic_splits_reappear());
  CHECK(result.materialization_audit.reran_polytomy_refinement);
  CHECK(result.materialization_audit.rebuilt_with_allow_polytomies);
  CHECK(result.materialization_audit.synthetic_productions_in_materialized_topologies ==
        2);
  CHECK(result.materialization_audit.intended_synthetic_productions.size() ==
        2);
  CHECK(!result.materialization_audit.bounded_refinement_used);
  CHECK(min_edge_weight(result.dag) == 7.0);

  auto const& rebuilt = result.rebuilt_refinement.grammar;
  auto a = clade_for(rebuilt, {"A"});
  auto b = clade_for(rebuilt, {"B"});
  auto c = clade_for(rebuilt, {"C"});
  auto bc = clade_for(rebuilt, {"B", "C"});
  CHECK(production_for(rebuilt, bc, {b, c}) != larch::no_production);
  CHECK(production_for(rebuilt, rebuilt.root_clade, {a, bc}) !=
        larch::no_production);

  auto repeated = larch::materialize_rank3_option_a(
      dag, refinement, std::vector<larch::rank3_topology>{topology, topology});
  CHECK(repeated.materialized_tree_count == 2);
  CHECK(repeated.materialization_audit.synthetic_productions_in_materialized_topologies ==
        4);
  CHECK(repeated.materialization_audit.intended_synthetic_productions.size() ==
        2);

  std::println("  PASS");
}

static void test_phase5_include_original_false_generated_binary_only() {
  std::println("test_phase5_include_original_false_generated_binary_only");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto topology = three_taxon_refined_topology_a_bc(refinement.grammar);

  larch::rank3_option_a_options materialize_opts;
  materialize_opts.include_original_dag = false;
  auto result = larch::materialize_rank3_option_a(dag, refinement, topology,
                                                  materialize_opts);
  CHECK(result.materialized_tree_count == 1);
  CHECK(result.rebuilt_base.audit.non_binary_production_count == 0);
  CHECK(!result.materialization_audit.rebuilt_with_allow_polytomies);
  CHECK(result.rebuilt_refinement.audit.source_kary_production_count == 0);
  CHECK(larch::grammar_is_binary_chart_compatible(
      result.rebuilt_refinement.grammar));
  CHECK(count_derivations(result.rebuilt_refinement.grammar) == 1);
  CHECK(result.all_intended_productions_present());
  CHECK(result.all_intended_synthetic_splits_reappear());

  std::println("  PASS");
}

static void test_phase5_direct_mutation_rejects_synthetic_productions() {
  std::println("test_phase5_direct_mutation_rejects_synthetic_productions");

  auto dag = make_three_taxon_star();
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      dag, larch::clade_grammar_options{}, opts);
  auto b = clade_for(refinement.grammar, {"B"});
  auto c = clade_for(refinement.grammar, {"C"});
  auto bc = clade_for(refinement.grammar, {"B", "C"});
  auto synthetic_pid = production_for(refinement.grammar, bc, {b, c});
  CHECK(throws_runtime_error([&] {
    larch::require_no_synthetic_polytomy_productions_for_direct_mutation(
        refinement, {synthetic_pid}, "phase5 direct mutation test");
  }));

  auto binary_dag = make_three_taxon_binary_tree();
  auto binary_refinement = larch::build_polytomy_refined_clade_grammar(
      binary_dag, larch::clade_grammar_options{},
      larch::polytomy_refinement_options{});
  auto bb = clade_for(binary_refinement.grammar, {"B"});
  auto cc = clade_for(binary_refinement.grammar, {"C"});
  auto bcbc = clade_for(binary_refinement.grammar, {"B", "C"});
  auto observed_pid = production_for(binary_refinement.grammar, bcbc,
                                     {bb, cc});
  larch::require_no_synthetic_polytomy_productions_for_direct_mutation(
      binary_refinement, {observed_pid}, "phase5 direct mutation test");

  std::println("  PASS");
}

int main() {
  test_default_reject_mode_rejects_kary();
  test_reject_mode_accepts_binary_grammar();
  test_audit_kary_returns_diagnostic_grammar();
  test_binary_chart_rejects_audit_kary_grammar();
  test_exact_expansion_three_taxon_star();
  test_exact_expansion_four_taxon_star_matches_bruteforce();
  test_exact_expansion_reuses_existing_observed_clade();
  test_exact_expansion_merges_reversed_observed_witnesses();
  test_exact_expansion_merges_synthetic_provenance();
  test_exact_expansion_caps_throw();
  test_binary_compatibility_rejects_out_of_range_taxa();
  test_phase2_three_taxon_all_patterns_match_explicit_refinements();
  test_phase2_four_taxon_random_patterns_match_explicit_refinements();
  test_phase2_traceback_trim_and_materialization();
  test_phase2_site_patterns_are_topology_independent();
  test_phase2_multisite_bnb_matches_explicit_refinements();
  test_phase2_embedded_polytomy_matches_explicit_refinements();
  test_phase2_multiple_independent_polytomies_match_explicit();
  test_phase3_bounded_large_star_truncated_repeatable();
  test_phase3_bounded_production_cap_rejects_incomplete_shape();
  test_phase3_bounded_multiple_polytomies_fair_budget();
  test_phase3_bounded_reports_shared_intermediate_recombination();
  test_phase3_bounded_large_arity_uses_capped_generator();
  test_phase3_bounded_counts_final_reused_intermediates();
  test_phase5_traceback_through_polytomy_materializes_and_merges();
  test_phase5_option_a_materializes_synthetic_polytomy_topology();
  test_phase5_include_original_false_generated_binary_only();
  test_phase5_direct_mutation_rejects_synthetic_productions();

  std::println("All polytomy refinement tests passed!");
  return 0;
}
