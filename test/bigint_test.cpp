#include <larch/bigint.hpp>

#include <cassert>
#include <print>
#include <sstream>
#include <string>

static void test_basic() {
  std::println("test_basic");
  larch::bigint zero;
  larch::bigint one{1};
  larch::bigint two{2};

  assert(zero == larch::bigint{0});
  assert(one != zero);
  assert(one == larch::bigint{1});
  assert(two == larch::bigint{2});
  std::println("  PASS");
}

static void test_addition() {
  std::println("test_addition");
  larch::bigint a{100};
  larch::bigint b{200};
  auto c = a + b;
  assert(c == larch::bigint{300});

  larch::bigint d{UINT64_MAX};
  auto e = d + larch::bigint{1};
  // Should overflow into 2 limbs
  assert(e != larch::bigint{0});
  assert(e > larch::bigint{0});

  // a + 0 == a
  assert(a + larch::bigint{0} == a);
  std::println("  PASS");
}

static void test_multiplication() {
  std::println("test_multiplication");
  larch::bigint a{1000000};
  larch::bigint b{1000000};
  auto c = a * b;
  assert(c == larch::bigint{1000000000000ULL});

  // Overflow: UINT64_MAX * 2
  larch::bigint big{UINT64_MAX};
  auto d = big * larch::bigint{2};
  // UINT64_MAX * 2 = 2^65 - 2
  assert(d > big);

  // Multiply by zero
  assert((big * larch::bigint{0}) == larch::bigint{0});

  // Multiply by one
  assert((big * larch::bigint{1}) == big);
  std::println("  PASS");
}

static void test_large_multiply() {
  std::println("test_large_multiply");
  // Compute 2^128 = (2^64) * (2^64)
  larch::bigint two_64{1};
  for (int i = 0; i < 64; ++i) two_64 = two_64 * larch::bigint{2};

  auto two_128 = two_64 * two_64;
  // 2^128 should need 3 limbs (most significant limb = 1)
  assert(two_128 > two_64);

  // 2^128 / 2^64 should give back 2^64
  auto quotient = two_128 / two_64;
  assert(quotient == two_64);
  std::println("  PASS");
}

static void test_division() {
  std::println("test_division");
  larch::bigint a{100};
  larch::bigint b{3};
  auto c = a / b;
  assert(c == larch::bigint{33});

  larch::bigint d{1000000000000ULL};
  auto e = d / larch::bigint{1000000};
  assert(e == larch::bigint{1000000});

  // Division by 1
  assert((a / larch::bigint{1}) == a);

  // Zero divided by anything
  assert((larch::bigint{0} / larch::bigint{5}) == larch::bigint{0});
  std::println("  PASS");
}

static void test_to_double() {
  std::println("test_to_double");
  larch::bigint a{42};
  assert(a.to_double() == 42.0);

  larch::bigint zero{0};
  assert(zero.to_double() == 0.0);

  larch::bigint big{UINT64_MAX};
  double d = big.to_double();
  assert(d > 0);
  // Should be approximately 1.8e19
  assert(d > 1e19);
  assert(d < 2e19);
  std::println("  PASS");
}

static void test_stream_output() {
  std::println("test_stream_output");
  {
    std::ostringstream oss;
    oss << larch::bigint{0};
    assert(oss.str() == "0");
  }
  {
    std::ostringstream oss;
    oss << larch::bigint{12345};
    assert(oss.str() == "12345");
  }
  {
    std::ostringstream oss;
    oss << larch::bigint{UINT64_MAX};
    assert(oss.str() == "18446744073709551615");
  }
  std::println("  PASS");
}

static void test_increment() {
  std::println("test_increment");
  larch::bigint a{5};
  ++a;
  assert(a == larch::bigint{6});

  larch::bigint b{0};
  b++;
  assert(b == larch::bigint{1});
  std::println("  PASS");
}

static void test_compound_assignment() {
  std::println("test_compound_assignment");
  larch::bigint a{10};
  a += larch::bigint{20};
  assert(a == larch::bigint{30});

  larch::bigint b{5};
  b *= larch::bigint{6};
  assert(b == larch::bigint{30});
  std::println("  PASS");
}

static void test_comparison() {
  std::println("test_comparison");
  assert(larch::bigint{5} > 0);
  assert(!(larch::bigint{0} > 0));
  assert(larch::bigint{1} > 0);

  assert(larch::bigint{3} < larch::bigint{5});
  assert(!(larch::bigint{5} < larch::bigint{3}));
  assert(larch::bigint{5} > larch::bigint{3});
  std::println("  PASS");
}

int main() {
  test_basic();
  test_addition();
  test_multiplication();
  test_large_multiply();
  test_division();
  test_to_double();
  test_stream_output();
  test_increment();
  test_compound_assignment();
  test_comparison();

  std::println("All bigint tests passed!");
  return 0;
}
