#include <larch/weight_counter.hpp>
#include <larch/weight_ops.hpp>

#include <cassert>
#include <iostream>
#include <print>
#include <vector>

using counter = larch::weight_counter<larch::parsimony_score_ops>;

static counter make_counter(std::vector<std::size_t> const& weights) {
  return counter(weights);
}

static void test_multiply(counter lhs, counter rhs, counter expected) {
  auto result = lhs * rhs;
  std::cout << "  lhs = " << lhs << "\n";
  std::cout << "  rhs = " << rhs << "\n";
  std::cout << "  result = " << result << "\n";
  assert(result == expected);
}

static void test_add(counter lhs, counter rhs, counter expected) {
  auto result = lhs + rhs;
  std::cout << "  lhs = " << lhs << "\n";
  std::cout << "  rhs = " << rhs << "\n";
  std::cout << "  result = " << result << "\n";
  assert(result == expected);
}

static void test_counter_multiply1() {
  std::println("test_counter_multiply1: empty * non-empty = empty");
  test_multiply(make_counter({}), make_counter({2, 2, 3, 3, 3, 4}),
                make_counter({}));
  std::println("  PASS");
}

static void test_counter_multiply2() {
  std::println("test_counter_multiply2: {{0}} * multi = identity");
  test_multiply(make_counter({0}), make_counter({2, 2, 3, 3, 3, 4}),
                make_counter({2, 2, 3, 3, 3, 4}));
  std::println("  PASS");
}

static void test_counter_multiply3() {
  std::println("test_counter_multiply3: cartesian product with sums");
  test_multiply(
      make_counter({2, 2, 3}), make_counter({2, 2, 3, 3, 3, 4}),
      make_counter({4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7}));
  std::println("  PASS");
}

static void test_counter_multiply4() {
  std::println("test_counter_multiply4: {{0}} * {{2}} = {{2}}");
  test_multiply(make_counter({0}), make_counter({2}), make_counter({2}));
  std::println("  PASS");
}

static void test_counter_add1() {
  std::println("test_counter_add1: empty + {{2}} = {{2}}");
  test_add(make_counter({}), make_counter({2}), make_counter({2}));
  std::println("  PASS");
}

static void test_counter_add2() {
  std::println("test_counter_add2: union of multisets");
  test_add(make_counter({0, 1, 2, 2, 3}), make_counter({2, 2, 2, 3, 4}),
           make_counter({0, 1, 2, 2, 3, 2, 2, 2, 3, 4}));
  std::println("  PASS");
}

int main() {
  test_counter_multiply1();
  test_counter_multiply2();
  test_counter_multiply3();
  test_counter_multiply4();
  test_counter_add1();
  test_counter_add2();

  std::println("All weight_counter tests passed!");
  return 0;
}
