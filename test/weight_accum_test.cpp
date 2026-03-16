#include <larch/weight_accumulator.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/compute.hpp>

#include <cassert>
#include <iostream>
#include <print>
#include <string_view>
#include <variant>

using accum_ops = larch::weight_accumulator<larch::parsimony_score_ops>;
using counter = larch::weight_counter<larch::parsimony_score_ops>;

static void test_weight_accum(std::string_view path, counter expected) {
  auto dag = larch::load_proto_dag(path);
  larch::recompute_compact_genomes(dag);
  larch::set_sample_ids_from_cg(dag);

  accum_ops accum;
  larch::subtree_weight<accum_ops> sw(dag);
  auto root_idx = std::visit([](auto n) { return n.index(); }, dag.get_root());
  auto scores = sw.compute_weight_below(root_idx, accum);

  std::cout << "  parsimony score counts:\n";
  larch::bigint total{0};
  for (auto const& [score, count] : scores.get_weights()) {
    std::cout << "    " << score << ": " << count << "\n";
    total += count;
  }
  std::cout << "  total count: " << total << "\n";

  assert(scores == expected);
}

static void test_testcase() {
  std::println("test_weight_accum_testcase");
  test_weight_accum("data/testcase/full_dag.pb.gz", counter({{78, 211},
                                                             {77, 206},
                                                             {79, 143},
                                                             {76, 106},
                                                             {80, 79},
                                                             {81, 27},
                                                             {75, 23},
                                                             {82, 11},
                                                             {83, 9},
                                                             {84, 3}}));
  std::println("  PASS");
}

static void test_testcase1() {
  std::println("test_weight_accum_testcase1");
  test_weight_accum("data/testcase1/full_dag.pb.gz",
                    counter({{78, 3}, {79, 2}, {76, 1}, {75, 1}}));
  std::println("  PASS");
}

int main() {
  test_testcase();
  test_testcase1();

  std::println("All weight_accum tests passed!");
  return 0;
}
