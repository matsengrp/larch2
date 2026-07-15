#include <larch/lazy_key_grouping.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

[[noreturn]] static void test_fail(char const* expression, char const* file,
                                   int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                          \
  do {                                                             \
    if (!(expression)) test_fail(#expression, __FILE__, __LINE__); \
  } while (false)

template <class Exception, class Function>
static Exception expect_throw(Function&& function) {
  try {
    std::forward<Function>(function)();
  } catch (Exception const& error) {
    return error;
  }
  test_fail("expected exception", __FILE__, __LINE__);
}

using larch::lazy_key_grouping_detail::group_packed_keys;
using larch::lazy_key_grouping_detail::packed_key_grouping_result;
using larch::lazy_key_grouping_detail::packed_key_grouping_workspace;
using larch::lazy_key_grouping_detail::packed_key_matrix_view;
using larch::lazy_key_grouping_detail::packed_key_word;

static_assert(std::is_same_v<packed_key_word, std::uint32_t>);

static packed_key_matrix_view matrix_view(
    std::size_t key_count, std::size_t key_width,
    std::span<packed_key_word const> words) {
  return packed_key_matrix_view{
      .key_count = key_count,
      .key_width = key_width,
      .words = words,
  };
}

static void check_members(packed_key_grouping_result const& result,
                          std::size_t class_id,
                          std::vector<std::size_t> const& expected) {
  auto const actual = result.members_for_class(class_id);
  CHECK(std::vector<std::size_t>(actual.begin(), actual.end()) == expected);
}

static packed_key_grouping_result quadratic_grouping_oracle(
    packed_key_matrix_view keys) {
  (void)keys.validated_word_count();
  auto key_equal = [&](std::size_t lhs, std::size_t rhs) {
    for (std::size_t word = 0; word < keys.key_width; ++word) {
      if (keys.words[lhs * keys.key_width + word] !=
          keys.words[rhs * keys.key_width + word]) {
        return false;
      }
    }
    return true;
  };
  auto key_less = [&](std::size_t lhs, std::size_t rhs) {
    for (std::size_t word = 0; word < keys.key_width; ++word) {
      auto const lhs_word = keys.words[lhs * keys.key_width + word];
      auto const rhs_word = keys.words[rhs * keys.key_width + word];
      if (lhs_word != rhs_word) return lhs_word < rhs_word;
    }
    return false;
  };

  packed_key_grouping_result result;
  result.class_by_input.resize(keys.key_count);
  for (std::size_t input = 0; input < keys.key_count; ++input) {
    std::size_t class_id = 0;
    while (class_id < result.representative_by_class.size() &&
           !key_equal(input, result.representative_by_class[class_id])) {
      ++class_id;
    }
    if (class_id == result.representative_by_class.size()) {
      result.representative_by_class.push_back(input);
    }
    result.class_by_input[input] = class_id;
  }

  auto const class_count = result.class_count();
  result.member_offsets_by_class.assign(class_count + 1, 0);
  for (auto class_id : result.class_by_input) {
    ++result.member_offsets_by_class[class_id + 1];
  }
  for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
    result.member_offsets_by_class[class_id + 1] +=
        result.member_offsets_by_class[class_id];
  }
  result.members_by_class.resize(keys.key_count);
  auto cursors = result.member_offsets_by_class;
  for (std::size_t input = 0; input < keys.key_count; ++input) {
    auto const class_id = result.class_by_input[input];
    result.members_by_class[cursors[class_id]++] = input;
  }

  result.lexicographic_class_order.resize(class_count);
  for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
    result.lexicographic_class_order[class_id] = class_id;
  }
  // Deliberately independent insertion-sort oracle for class representatives.
  for (std::size_t position = 1; position < class_count; ++position) {
    auto class_id = result.lexicographic_class_order[position];
    auto insertion = position;
    while (insertion != 0 &&
           key_less(result.representative_by_class[class_id],
                    result.representative_by_class
                        [result.lexicographic_class_order[insertion - 1]])) {
      result.lexicographic_class_order[insertion] =
          result.lexicographic_class_order[insertion - 1];
      --insertion;
    }
    result.lexicographic_class_order[insertion] = class_id;
  }
  return result;
}

static void test_explicit_width_one_contract() {
  std::println("test_explicit_width_one_contract");

  // This is the contract fixture from the implementation plan. Class IDs use
  // first occurrence (9, 1, 2), while traversal uses sorted keys (1, 2, 9).
  std::vector<packed_key_word> words{9, 1, 9, 2, 1};
  packed_key_grouping_workspace workspace;
  auto result = group_packed_keys(matrix_view(5, 1, words), workspace);

  CHECK((result.class_by_input == std::vector<std::size_t>{0, 1, 0, 2, 1}));
  CHECK((result.representative_by_class == std::vector<std::size_t>{0, 1, 3}));
  CHECK(
      (result.member_offsets_by_class == std::vector<std::size_t>{0, 2, 4, 5}));
  CHECK((result.members_by_class == std::vector<std::size_t>{0, 2, 1, 4, 3}));
  CHECK(
      (result.lexicographic_class_order == std::vector<std::size_t>{1, 2, 0}));
  CHECK(result.class_count() == 3);
  check_members(result, 0, {0, 2});
  check_members(result, 1, {1, 4});
  check_members(result, 2, {3});

  std::println("  PASS");
}

static void test_zero_width_and_empty_inputs() {
  std::println("test_zero_width_and_empty_inputs");

  packed_key_grouping_workspace workspace;
  std::span<packed_key_word const> no_words;
  auto zero_width = group_packed_keys(matrix_view(5, 0, no_words), workspace);
  CHECK((zero_width.class_by_input == std::vector<std::size_t>{0, 0, 0, 0, 0}));
  CHECK((zero_width.representative_by_class == std::vector<std::size_t>{0}));
  CHECK((zero_width.member_offsets_by_class == std::vector<std::size_t>{0, 5}));
  CHECK(
      (zero_width.members_by_class == std::vector<std::size_t>{0, 1, 2, 3, 4}));
  CHECK((zero_width.lexicographic_class_order == std::vector<std::size_t>{0}));

  auto empty = group_packed_keys(matrix_view(0, 7, no_words), workspace);
  CHECK(empty.class_by_input.empty());
  CHECK(empty.representative_by_class.empty());
  CHECK((empty.member_offsets_by_class == std::vector<std::size_t>{0}));
  CHECK(empty.members_by_class.empty());
  CHECK(empty.lexicographic_class_order.empty());
  CHECK(empty.class_count() == 0);

  std::println("  PASS");
}

static void test_pattern_major_width_two() {
  std::println("test_pattern_major_width_two");

  // Each adjacent pair is one pattern key. Comparing the flattened stream as
  // scalar words would produce a different answer, so this also fixes the
  // pattern-major layout contract.
  std::vector<packed_key_word> words{
      1, 2,  // class 0
      1, 1,  // class 1
      1, 2,  // class 0
      0, 9,  // class 2
      1, 1,  // class 1
      1, 3,  // class 3
      0, 9,  // class 2
  };
  packed_key_grouping_workspace workspace;
  auto result = group_packed_keys(matrix_view(7, 2, words), workspace);

  CHECK(
      (result.class_by_input == std::vector<std::size_t>{0, 1, 0, 2, 1, 3, 2}));
  CHECK(
      (result.representative_by_class == std::vector<std::size_t>{0, 1, 3, 5}));
  CHECK((result.member_offsets_by_class ==
         std::vector<std::size_t>{0, 2, 4, 6, 7}));
  CHECK((result.members_by_class ==
         std::vector<std::size_t>{0, 2, 1, 4, 3, 6, 5}));
  CHECK((result.lexicographic_class_order ==
         std::vector<std::size_t>{2, 1, 0, 3}));

  std::println("  PASS");
}

static void test_width_seven_non_power_of_two() {
  std::println("test_width_seven_non_power_of_two");

  auto const max_word = (std::numeric_limits<packed_key_word>::max)();
  std::vector<packed_key_word> words{
      5,        4, 3, 2, 1, 0, max_word,      // A, class 0
      0,        8, 7, 6, 5, 4, 3,             // B, class 1
      5,        4, 3, 2, 1, 0, max_word,      // A
      5,        4, 3, 2, 1, 0, max_word - 1,  // C, class 2
      0,        8, 7, 6, 5, 4, 3,             // B
      5,        4, 3, 2, 1, 1, 0,             // D, class 3
      5,        4, 3, 2, 1, 0, max_word - 1,  // C
      max_word, 0, 0, 0, 0, 0, 0,             // E, class 4
      5,        4, 3, 2, 1, 0, max_word,      // A
  };
  packed_key_grouping_workspace workspace;
  auto result = group_packed_keys(matrix_view(9, 7, words), workspace);

  CHECK((result.class_by_input ==
         std::vector<std::size_t>{0, 1, 0, 2, 1, 3, 2, 4, 0}));
  CHECK((result.representative_by_class ==
         std::vector<std::size_t>{0, 1, 3, 5, 7}));
  CHECK((result.member_offsets_by_class ==
         std::vector<std::size_t>{0, 3, 5, 7, 8, 9}));
  CHECK((result.members_by_class ==
         std::vector<std::size_t>{0, 2, 8, 1, 4, 3, 6, 5, 7}));
  CHECK((result.lexicographic_class_order ==
         std::vector<std::size_t>{1, 2, 0, 3, 4}));

  std::println("  PASS");
}

static void test_repeated_determinism_and_workspace_reuse() {
  std::println("test_repeated_determinism_and_workspace_reuse");

  std::vector<packed_key_word> words{
      3, 1, 4, 1, 5, 9, 2,  // class 0
      2, 7, 1, 8, 2, 8, 1,  // class 1
      3, 1, 4, 1, 5, 9, 2,  // class 0
      0, 0, 0, 0, 0, 0, 0,  // class 2
      2, 7, 1, 8, 2, 8, 1,  // class 1
  };
  packed_key_grouping_workspace workspace;
  auto const expected = group_packed_keys(matrix_view(5, 7, words), workspace);
  auto const warmed_capacity = workspace.dynamic_capacity_bytes();
  for (std::size_t repetition = 0; repetition < 64; ++repetition) {
    auto actual = group_packed_keys(matrix_view(5, 7, words), workspace);
    CHECK(actual == expected);
    CHECK(workspace.dynamic_capacity_bytes() == warmed_capacity);
  }

  std::vector<packed_key_word> larger_words(33 * 7);
  for (std::size_t pattern = 0; pattern < 33; ++pattern) {
    for (std::size_t word = 0; word < 7; ++word) {
      larger_words[pattern * 7 + word] = static_cast<packed_key_word>(
          (pattern * 17 + word * 13 + pattern / 3) % 19);
    }
  }
  (void)group_packed_keys(matrix_view(33, 7, larger_words), workspace);
  auto const enlarged_capacity = workspace.dynamic_capacity_bytes();
  CHECK(enlarged_capacity >= warmed_capacity);
  CHECK(group_packed_keys(matrix_view(5, 7, words), workspace) == expected);
  CHECK(workspace.dynamic_capacity_bytes() == enlarged_capacity);

  workspace.release();
  CHECK(workspace.dynamic_capacity_bytes() == 0);
  CHECK(group_packed_keys(matrix_view(5, 7, words), workspace) == expected);

  std::println("  PASS");
}

static void test_merge_boundaries_against_quadratic_oracle() {
  std::println("test_merge_boundaries_against_quadratic_oracle");

  packed_key_grouping_workspace workspace;
  for (auto width :
       {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{7}}) {
    for (auto count :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
          std::size_t{4}, std::size_t{5}, std::size_t{7}, std::size_t{8},
          std::size_t{9}, std::size_t{15}, std::size_t{16}, std::size_t{17},
          std::size_t{31}, std::size_t{32}, std::size_t{33}}) {
      std::vector<packed_key_word> words(count * width);
      for (std::size_t input = 0; input < count; ++input) {
        auto const seed = (input * 7 + input / 3 + count) % 11;
        for (std::size_t word = 0; word < width; ++word) {
          words[input * width + word] = static_cast<packed_key_word>(
              (seed * 29 + word * 17 + seed * word * 3) % 37);
        }
      }
      auto const keys = matrix_view(count, width, words);
      CHECK(group_packed_keys(keys, workspace) ==
            quadratic_grouping_oracle(keys));
    }
  }

  std::println("  PASS");
}

static void test_checked_memory_estimates_and_budget_backstop() {
  std::println("test_checked_memory_estimates_and_budget_backstop");

  using namespace larch::lazy_key_grouping_detail;
  constexpr std::size_t key_count = 5;
  constexpr std::size_t key_width = 1;
  auto const estimate =
      estimate_packed_key_grouping_memory(key_count, key_width);
  CHECK(estimate.packed_word_count == key_count * key_width);
  CHECK(estimate.packed_payload_bytes ==
        key_count * key_width * sizeof(packed_key_word));
  CHECK(estimate.workspace_dynamic_bytes ==
        2 * key_count * sizeof(std::size_t));
  CHECK(estimate.result_dynamic_bytes ==
        (5 * key_count + 1) * sizeof(std::size_t));
  CHECK(estimate.worst_case_owned_resident_bytes ==
        sizeof(packed_key_grouping_workspace) +
            sizeof(packed_key_grouping_result) +
            estimate.workspace_dynamic_bytes + estimate.result_dynamic_bytes);
  CHECK(estimate.worst_case_total_resident_bytes ==
        estimate.packed_payload_bytes +
            estimate.worst_case_owned_resident_bytes);
  CHECK(estimate.staging_owned_resident_bytes <
        estimate.worst_case_owned_resident_bytes);

  std::vector<packed_key_word> words{9, 1, 9, 2, 1};
  packed_key_grouping_workspace workspace;
  auto result = group_packed_keys(matrix_view(5, 1, words), workspace);
  auto const exact_logical = estimate_packed_key_grouping_owned_resident_bytes(
      key_count, result.class_count());
  auto const actual =
      packed_key_grouping_owned_resident_bytes(workspace, result);
  CHECK(actual >= exact_logical);
  CHECK(result.dynamic_capacity_bytes() >=
        estimate_packed_key_grouping_result_dynamic_bytes(
            key_count, result.class_count()));
  CHECK(result.resident_bytes() ==
        sizeof(result) + result.dynamic_capacity_bytes());
  CHECK(workspace.resident_bytes() ==
        sizeof(workspace) + workspace.dynamic_capacity_bytes());

  auto exact_budget_result =
      group_packed_keys(matrix_view(5, 1, words), workspace, actual);
  CHECK(exact_budget_result == result);
  auto error = expect_throw<packed_key_grouping_budget_error>([&] {
    (void)group_packed_keys(matrix_view(5, 1, words), workspace, actual - 1);
  });
  CHECK(error.required_bytes() > error.budget_bytes());
  CHECK(error.budget_bytes() == actual - 1);

  packed_key_grouping_workspace cold_workspace;
  auto const preflight_budget = estimate.staging_owned_resident_bytes - 1;
  error = expect_throw<packed_key_grouping_budget_error>([&] {
    (void)group_packed_keys(matrix_view(5, 1, words), cold_workspace,
                            preflight_budget);
  });
  CHECK(error.required_bytes() == estimate.staging_owned_resident_bytes);
  CHECK(cold_workspace.dynamic_capacity_bytes() == 0);

  std::println("  PASS");
}

static void test_validation_and_overflow_boundaries() {
  std::println("test_validation_and_overflow_boundaries");

  using namespace larch::lazy_key_grouping_detail;
  auto const max_size = (std::numeric_limits<std::size_t>::max)();
  CHECK(checked_packed_key_bytes_add(max_size - 7, 7, "boundary") == max_size);
  CHECK(checked_packed_key_count_add(max_size - 1, 1, "boundary") == max_size);
  CHECK(checked_packed_key_count_multiply(max_size / 2, 2, "boundary") ==
        (max_size / 2) * 2);

  (void)expect_throw<std::overflow_error>(
      [&] { (void)checked_packed_key_bytes_add(max_size, 1, "test"); });
  (void)expect_throw<std::overflow_error>(
      [&] { (void)checked_packed_key_bytes_multiply(max_size, 2, "test"); });
  (void)expect_throw<std::overflow_error>(
      [&] { (void)checked_packed_key_count_add(max_size, 1, "test"); });
  (void)expect_throw<std::overflow_error>(
      [&] { (void)checked_packed_key_count_multiply(max_size, 2, "test"); });

  std::span<packed_key_word const> no_words;
  auto overflow_view = matrix_view(max_size, 2, no_words);
  (void)expect_throw<std::overflow_error>(
      [&] { (void)overflow_view.validated_word_count(); });
  (void)expect_throw<std::overflow_error>(
      [&] { (void)estimate_packed_key_grouping_memory(max_size, 2); });
  (void)expect_throw<std::overflow_error>([&] {
    (void)estimate_packed_key_grouping_workspace_dynamic_bytes(max_size);
  });
  (void)expect_throw<std::overflow_error>([&] {
    (void)estimate_packed_key_grouping_result_dynamic_bytes(max_size, max_size);
  });

  (void)expect_throw<std::invalid_argument>(
      [&] { (void)estimate_packed_key_grouping_result_dynamic_bytes(2, 0); });
  (void)expect_throw<std::invalid_argument>(
      [&] { (void)estimate_packed_key_grouping_result_dynamic_bytes(2, 3); });
  CHECK(estimate_packed_key_grouping_result_dynamic_bytes(0, 0) ==
        sizeof(std::size_t));

  std::array<packed_key_word, 3> wrong_words{1, 2, 3};
  packed_key_grouping_workspace workspace;
  (void)expect_throw<std::invalid_argument>([&] {
    (void)group_packed_keys(matrix_view(2, 2, wrong_words), workspace);
  });
  (void)expect_throw<std::invalid_argument>([&] {
    (void)group_packed_keys(matrix_view(3, 0, wrong_words), workspace);
  });
  CHECK(workspace.dynamic_capacity_bytes() == 0);

  std::array<packed_key_word, 1> one_word{7};
  auto one = group_packed_keys(matrix_view(1, 1, one_word), workspace);
  (void)expect_throw<std::out_of_range>(
      [&] { (void)one.members_for_class(1); });

  std::println("  PASS");
}

int main() {
  test_explicit_width_one_contract();
  test_zero_width_and_empty_inputs();
  test_pattern_major_width_two();
  test_width_seven_non_power_of_two();
  test_repeated_determinism_and_workspace_reuse();
  test_merge_boundaries_against_quadratic_oracle();
  test_checked_memory_estimates_and_budget_backstop();
  test_validation_and_overflow_boundaries();
  std::println("lazy_key_grouping_test: PASS");
}
