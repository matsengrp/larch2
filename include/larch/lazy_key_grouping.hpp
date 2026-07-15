#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch::lazy_key_grouping_detail {

// Lazy-chart keys are packed pattern-major: all words for pattern zero are
// contiguous, followed by all words for pattern one, and so on. uint32_t is
// wide enough for the current clade/class identifiers while avoiding the
// allocator and pointer overhead of vector-valued map keys.
using packed_key_word = std::uint32_t;

inline std::size_t checked_packed_key_bytes_add(std::size_t lhs,
                                                std::size_t rhs,
                                                std::string_view context) {
  if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
    throw std::overflow_error(std::string{context} + " byte overflow");
  }
  return lhs + rhs;
}

inline std::size_t checked_packed_key_bytes_multiply(std::size_t lhs,
                                                     std::size_t rhs,
                                                     std::string_view context) {
  if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
    throw std::overflow_error(std::string{context} + " byte overflow");
  }
  return lhs * rhs;
}

inline std::size_t checked_packed_key_count_add(std::size_t lhs,
                                                std::size_t rhs,
                                                std::string_view context) {
  if (lhs > (std::numeric_limits<std::size_t>::max)() - rhs) {
    throw std::overflow_error(std::string{context} + " count overflow");
  }
  return lhs + rhs;
}

inline std::size_t checked_packed_key_count_multiply(std::size_t lhs,
                                                     std::size_t rhs,
                                                     std::string_view context) {
  if (lhs != 0 && rhs > (std::numeric_limits<std::size_t>::max)() / lhs) {
    throw std::overflow_error(std::string{context} + " count overflow");
  }
  return lhs * rhs;
}

struct packed_key_matrix_view {
  std::size_t key_count = 0;
  std::size_t key_width = 0;
  std::span<packed_key_word const> words;

  [[nodiscard]] std::size_t validated_word_count() const {
    auto const expected = checked_packed_key_count_multiply(
        key_count, key_width, "packed lazy-key matrix");
    if (words.size() != expected) {
      throw std::invalid_argument(
          "packed lazy-key matrix: expected " + std::to_string(expected) +
          " pattern-major words, got " + std::to_string(words.size()));
    }
    return expected;
  }
};

template <class Vector>
inline std::size_t packed_key_vector_capacity_bytes(Vector const& values,
                                                    std::string_view context) {
  return checked_packed_key_bytes_multiply(
      values.capacity(), sizeof(typename Vector::value_type), context);
}

struct packed_key_grouping_result {
  // Class IDs are assigned by the first input key in each equivalence class.
  std::vector<std::size_t> class_by_input;
  std::vector<std::size_t> representative_by_class;

  // CSR members retain input order within each class.
  std::vector<std::size_t> member_offsets_by_class;
  std::vector<std::size_t> members_by_class;

  // Class IDs in lexicographic packed-key order. This preserves the traversal
  // order of the former ordered vector-key implementation independently from
  // first-occurrence class numbering.
  std::vector<std::size_t> lexicographic_class_order;

  [[nodiscard]] std::size_t class_count() const noexcept {
    return representative_by_class.size();
  }

  [[nodiscard]] std::span<std::size_t const> members_for_class(
      std::size_t class_id) const {
    if (class_id >= class_count()) {
      throw std::out_of_range("packed lazy-key grouping: class out of range");
    }
    auto const begin = member_offsets_by_class[class_id];
    auto const end = member_offsets_by_class[class_id + 1];
    return std::span<std::size_t const>{members_by_class}.subspan(begin,
                                                                  end - begin);
  }

  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    std::size_t total = 0;
    auto add = [&](auto const& values, std::string_view context) {
      total = checked_packed_key_bytes_add(
          total, packed_key_vector_capacity_bytes(values, context),
          "packed lazy-key result capacity");
    };
    add(class_by_input, "packed lazy-key class-map capacity");
    add(representative_by_class, "packed lazy-key representative capacity");
    add(member_offsets_by_class, "packed lazy-key offset capacity");
    add(members_by_class, "packed lazy-key member capacity");
    add(lexicographic_class_order,
        "packed lazy-key lexicographic-order capacity");
    return total;
  }

  [[nodiscard]] std::size_t resident_bytes() const {
    return checked_packed_key_bytes_add(sizeof(*this), dynamic_capacity_bytes(),
                                        "packed lazy-key result resident");
  }

  bool operator==(packed_key_grouping_result const&) const = default;
};

class packed_key_grouping_workspace {
 public:
  [[nodiscard]] std::size_t dynamic_capacity_bytes() const {
    return checked_packed_key_bytes_add(
        packed_key_vector_capacity_bytes(sort_order_,
                                         "packed lazy-key sort-order capacity"),
        packed_key_vector_capacity_bytes(
            merge_buffer_, "packed lazy-key merge-buffer capacity"),
        "packed lazy-key workspace capacity");
  }

  [[nodiscard]] std::size_t resident_bytes() const {
    return checked_packed_key_bytes_add(sizeof(*this), dynamic_capacity_bytes(),
                                        "packed lazy-key workspace resident");
  }

  // Drop retained scratch explicitly when a level barrier releases a sparse
  // grouping stage. Ordinary clear/resize operations intentionally retain it.
  void release() noexcept {
    std::vector<std::size_t>{}.swap(sort_order_);
    std::vector<std::size_t>{}.swap(merge_buffer_);
  }

 private:
  std::vector<std::size_t> sort_order_;
  std::vector<std::size_t> merge_buffer_;

  friend packed_key_grouping_result group_packed_keys(
      packed_key_matrix_view, packed_key_grouping_workspace&, std::size_t);
};

struct packed_key_grouping_memory_estimate {
  std::size_t packed_word_count = 0;
  std::size_t packed_payload_bytes = 0;
  std::size_t workspace_dynamic_bytes = 0;
  std::size_t result_dynamic_bytes = 0;
  std::size_t staging_owned_resident_bytes = 0;
  std::size_t worst_case_owned_resident_bytes = 0;
  std::size_t worst_case_total_resident_bytes = 0;

  bool operator==(packed_key_grouping_memory_estimate const&) const = default;
};

inline std::size_t estimate_packed_key_grouping_result_dynamic_bytes(
    std::size_t key_count, std::size_t class_count) {
  if (class_count > key_count || (key_count != 0 && class_count == 0)) {
    throw std::invalid_argument(
        "packed lazy-key grouping: invalid class-count estimate");
  }

  // class_by_input + members_by_class each have key_count entries;
  // representatives and lexicographic order each have class_count entries;
  // CSR offsets have class_count + 1 entries.
  auto const twice_keys = checked_packed_key_count_multiply(
      key_count, 2, "packed lazy-key result key entries");
  auto const twice_classes = checked_packed_key_count_multiply(
      class_count, 2, "packed lazy-key result class entries");
  auto entries = checked_packed_key_count_add(twice_keys, twice_classes,
                                              "packed lazy-key result entries");
  entries = checked_packed_key_count_add(
      entries,
      checked_packed_key_count_add(class_count, 1,
                                   "packed lazy-key CSR offsets"),
      "packed lazy-key result entries");
  return checked_packed_key_bytes_multiply(entries, sizeof(std::size_t),
                                           "packed lazy-key result estimate");
}

inline std::size_t estimate_packed_key_grouping_workspace_dynamic_bytes(
    std::size_t key_count) {
  auto const entries = checked_packed_key_count_multiply(
      key_count, 2, "packed lazy-key workspace entries");
  return checked_packed_key_bytes_multiply(
      entries, sizeof(std::size_t), "packed lazy-key workspace estimate");
}

inline std::size_t estimate_packed_key_grouping_owned_resident_bytes(
    std::size_t key_count, std::size_t class_count) {
  auto total = checked_packed_key_bytes_add(
      sizeof(packed_key_grouping_workspace), sizeof(packed_key_grouping_result),
      "packed lazy-key fixed resident estimate");
  total = checked_packed_key_bytes_add(
      total, estimate_packed_key_grouping_workspace_dynamic_bytes(key_count),
      "packed lazy-key owned resident estimate");
  return checked_packed_key_bytes_add(
      total,
      estimate_packed_key_grouping_result_dynamic_bytes(key_count, class_count),
      "packed lazy-key owned resident estimate");
}

inline packed_key_grouping_memory_estimate estimate_packed_key_grouping_memory(
    std::size_t key_count, std::size_t key_width) {
  auto const word_count = checked_packed_key_count_multiply(
      key_count, key_width, "packed lazy-key payload");
  auto const payload_bytes = checked_packed_key_bytes_multiply(
      word_count, sizeof(packed_key_word), "packed lazy-key payload");
  auto const workspace_bytes =
      estimate_packed_key_grouping_workspace_dynamic_bytes(key_count);

  // The staging floor owns the two sort vectors and class_by_input. It is
  // useful for failing before representative/CSR allocation. The final bound
  // assumes every key forms a distinct class.
  auto staging = checked_packed_key_bytes_add(
      sizeof(packed_key_grouping_workspace), sizeof(packed_key_grouping_result),
      "packed lazy-key staging resident estimate");
  staging = checked_packed_key_bytes_add(
      staging, workspace_bytes, "packed lazy-key staging resident estimate");
  staging = checked_packed_key_bytes_add(
      staging,
      checked_packed_key_bytes_multiply(
          key_count, sizeof(std::size_t),
          "packed lazy-key staging class-map estimate"),
      "packed lazy-key staging resident estimate");

  auto const result_bytes =
      estimate_packed_key_grouping_result_dynamic_bytes(key_count, key_count);
  auto const worst_owned =
      estimate_packed_key_grouping_owned_resident_bytes(key_count, key_count);
  auto const worst_total = checked_packed_key_bytes_add(
      payload_bytes, worst_owned, "packed lazy-key total resident estimate");
  return packed_key_grouping_memory_estimate{
      .packed_word_count = word_count,
      .packed_payload_bytes = payload_bytes,
      .workspace_dynamic_bytes = workspace_bytes,
      .result_dynamic_bytes = result_bytes,
      .staging_owned_resident_bytes = staging,
      .worst_case_owned_resident_bytes = worst_owned,
      .worst_case_total_resident_bytes = worst_total,
  };
}

class packed_key_grouping_budget_error : public std::runtime_error {
 public:
  packed_key_grouping_budget_error(std::size_t required_bytes,
                                   std::size_t budget_bytes)
      : std::runtime_error("packed lazy-key grouping requires " +
                           std::to_string(required_bytes) +
                           " owned resident bytes, exceeding budget " +
                           std::to_string(budget_bytes)),
        required_bytes_(required_bytes),
        budget_bytes_(budget_bytes) {}

  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }

  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return budget_bytes_;
  }

 private:
  std::size_t required_bytes_;
  std::size_t budget_bytes_;
};

inline std::size_t packed_key_grouping_owned_resident_bytes(
    packed_key_grouping_workspace const& workspace,
    packed_key_grouping_result const& result) {
  return checked_packed_key_bytes_add(workspace.resident_bytes(),
                                      result.resident_bytes(),
                                      "packed lazy-key actual owned resident");
}

inline void require_packed_key_grouping_owned_resident_budget(
    packed_key_grouping_workspace const& workspace,
    packed_key_grouping_result const& result,
    std::size_t owned_resident_budget_bytes) {
  auto const actual =
      packed_key_grouping_owned_resident_bytes(workspace, result);
  if (actual > owned_resident_budget_bytes) {
    throw packed_key_grouping_budget_error(actual, owned_resident_budget_bytes);
  }
}

namespace implementation {

inline bool packed_key_less(packed_key_matrix_view keys, std::size_t lhs,
                            std::size_t rhs) noexcept {
  auto const lhs_offset = lhs * keys.key_width;
  auto const rhs_offset = rhs * keys.key_width;
  for (std::size_t word = 0; word < keys.key_width; ++word) {
    auto const lhs_word = keys.words[lhs_offset + word];
    auto const rhs_word = keys.words[rhs_offset + word];
    if (lhs_word != rhs_word) return lhs_word < rhs_word;
  }
  return false;
}

inline bool packed_key_equal(packed_key_matrix_view keys, std::size_t lhs,
                             std::size_t rhs) noexcept {
  auto const lhs_offset = lhs * keys.key_width;
  auto const rhs_offset = rhs * keys.key_width;
  for (std::size_t word = 0; word < keys.key_width; ++word) {
    if (keys.words[lhs_offset + word] != keys.words[rhs_offset + word]) {
      return false;
    }
  }
  return true;
}

inline void stable_merge_sort_key_indices(
    packed_key_matrix_view keys, std::vector<std::size_t>& order,
    std::vector<std::size_t>& merge_buffer) {
  auto const count = keys.key_count;
  for (std::size_t index = 0; index < count; ++index) order[index] = index;
  if (count < 2) return;

  // Bottom-up merge sort avoids recursion and keeps every temporary index in
  // caller-owned reusable storage. Taking the left side on equality is the
  // stability rule that preserves input order within a key class.
  for (std::size_t run_width = 1;;) {
    for (std::size_t begin = 0; begin < count;) {
      auto const middle = begin + std::min(run_width, count - begin);
      auto const end = middle + std::min(run_width, count - middle);
      auto left = begin;
      auto right = middle;
      auto output = begin;
      while (left < middle && right < end) {
        if (packed_key_less(keys, order[right], order[left])) {
          merge_buffer[output++] = order[right++];
        } else {
          merge_buffer[output++] = order[left++];
        }
      }
      while (left < middle) merge_buffer[output++] = order[left++];
      while (right < end) merge_buffer[output++] = order[right++];
      begin = end;
    }
    order.swap(merge_buffer);

    // This form both detects the final pass and avoids overflowing 2*width.
    if (run_width >= count - run_width) break;
    run_width *= 2;
  }
}

}  // namespace implementation

// The budget covers the stable resident capacities of workspace and result;
// caller-owned packed words are reported separately by the estimator. The
// default is unlimited. Logical estimates are checked before each allocation
// stage and actual vector capacities are checked afterward as a backstop.
inline packed_key_grouping_result group_packed_keys(
    packed_key_matrix_view keys, packed_key_grouping_workspace& workspace,
    std::size_t owned_resident_budget_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  (void)keys.validated_word_count();
  auto const estimate =
      estimate_packed_key_grouping_memory(keys.key_count, keys.key_width);
  if (estimate.staging_owned_resident_bytes > owned_resident_budget_bytes) {
    throw packed_key_grouping_budget_error(
        estimate.staging_owned_resident_bytes, owned_resident_budget_bytes);
  }

  workspace.sort_order_.resize(keys.key_count);
  workspace.merge_buffer_.resize(keys.key_count);
  packed_key_grouping_result result;
  result.class_by_input.resize(keys.key_count);
  require_packed_key_grouping_owned_resident_budget(
      workspace, result, owned_resident_budget_bytes);

  implementation::stable_merge_sort_key_indices(keys, workspace.sort_order_,
                                                workspace.merge_buffer_);

  std::size_t class_count = 0;
  for (std::size_t begin = 0; begin < keys.key_count;) {
    auto end = begin + 1;
    while (end < keys.key_count &&
           implementation::packed_key_equal(keys, workspace.sort_order_[begin],
                                            workspace.sort_order_[end])) {
      ++end;
    }
    for (auto position = begin; position < end; ++position) {
      result.class_by_input[workspace.sort_order_[position]] = class_count;
    }
    ++class_count;
    begin = end;
  }

  auto const exact_logical_resident =
      estimate_packed_key_grouping_owned_resident_bytes(keys.key_count,
                                                        class_count);
  if (exact_logical_resident > owned_resident_budget_bytes) {
    throw packed_key_grouping_budget_error(exact_logical_resident,
                                           owned_resident_budget_bytes);
  }

  result.representative_by_class.resize(class_count);
  result.member_offsets_by_class.assign(
      checked_packed_key_count_add(class_count, 1,
                                   "packed lazy-key CSR offsets"),
      std::size_t{0});
  result.members_by_class.resize(keys.key_count);
  result.lexicographic_class_order.resize(class_count);
  require_packed_key_grouping_owned_resident_budget(
      workspace, result, owned_resident_budget_bytes);

  // The sort pass assigned temporary lexicographic class IDs. Convert them to
  // stable first-occurrence IDs with one input-order scan. merge_buffer is no
  // longer needed by the sort and becomes the lexicographic-to-stable map.
  auto const no_class = (std::numeric_limits<std::size_t>::max)();
  workspace.merge_buffer_.resize(class_count);
  std::fill(workspace.merge_buffer_.begin(), workspace.merge_buffer_.end(),
            no_class);
  std::size_t next_class = 0;
  for (std::size_t input = 0; input < keys.key_count; ++input) {
    auto const lexicographic_class = result.class_by_input[input];
    auto& stable_class = workspace.merge_buffer_[lexicographic_class];
    if (stable_class == no_class) {
      stable_class = next_class++;
      result.representative_by_class[stable_class] = input;
    }
    result.class_by_input[input] = stable_class;
  }
  for (std::size_t lexicographic_class = 0; lexicographic_class < class_count;
       ++lexicographic_class) {
    result.lexicographic_class_order[lexicographic_class] =
        workspace.merge_buffer_[lexicographic_class];
  }

  for (auto class_id : result.class_by_input) {
    ++result.member_offsets_by_class[class_id + 1];
  }
  for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
    result.member_offsets_by_class[class_id + 1] +=
        result.member_offsets_by_class[class_id];
  }

  workspace.merge_buffer_.resize(class_count);
  for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
    workspace.merge_buffer_[class_id] =
        result.member_offsets_by_class[class_id];
  }
  for (std::size_t input = 0; input < keys.key_count; ++input) {
    auto const class_id = result.class_by_input[input];
    result.members_by_class[workspace.merge_buffer_[class_id]++] = input;
  }

  return result;
}

}  // namespace larch::lazy_key_grouping_detail
