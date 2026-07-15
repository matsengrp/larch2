#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace larch::lazy_key_grouping_detail {

// Lazy-chart keys are packed pattern-major: all words for pattern zero are
// contiguous, followed by all words for pattern one, and so on. uint32_t is
// wide enough for the current clade/class identifiers while avoiding the
// allocator and pointer overhead of vector-valued map keys.
using packed_key_word = std::uint32_t;

inline packed_key_word checked_packed_key_word(std::size_t value,
                                               std::string_view context) {
  if (value > (std::numeric_limits<packed_key_word>::max)()) {
    throw std::overflow_error(std::string{context} +
                              " does not fit in a packed lazy-key word");
  }
  return static_cast<packed_key_word>(value);
}

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

enum class packed_key_grouping_shape_error_kind : std::uint8_t {
  word_count_overflow,
  word_count_mismatch,
  offset_count_overflow,
  resident_capacity_overflow,
};

enum class packed_key_grouping_prepared_status_code : std::uint8_t {
  success,
  word_count_overflow,
  word_count_mismatch,
  offset_count_overflow,
  storage_not_prepared,
  resident_capacity_overflow,
  budget_exceeded,
};

// Fixed-size value result for the scheduled grouping seam. `required` and
// `available`
// describe counts for shape/storage failures and bytes for resident/budget
// failures. `secondary_available` is the result-storage capacity when
// storage_not_prepared is reported, and the key count for word-count overflow.
// For resident-capacity overflow, `required == size_t::max()` is a sentinel:
// the true byte requirement is not representable. Returning this status never
// allocates exception storage.
struct packed_key_grouping_prepared_status {
  packed_key_grouping_prepared_status_code code =
      packed_key_grouping_prepared_status_code::success;
  std::size_t required = 0;
  std::size_t available = 0;
  std::size_t secondary_available = 0;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == packed_key_grouping_prepared_status_code::success;
  }

  bool operator==(packed_key_grouping_prepared_status const&) const = default;
};

static_assert(
    std::is_trivially_copyable_v<packed_key_grouping_prepared_status>);
static_assert(std::is_standard_layout_v<packed_key_grouping_prepared_status>);

class packed_key_grouping_shape_error : public std::exception {
 public:
  packed_key_grouping_shape_error(packed_key_grouping_shape_error_kind kind,
                                  std::size_t expected,
                                  std::size_t actual) noexcept
      : kind_(kind), expected_(expected), actual_(actual) {}

  [[nodiscard]] packed_key_grouping_shape_error_kind kind() const noexcept {
    return kind_;
  }
  [[nodiscard]] std::size_t expected() const noexcept { return expected_; }
  [[nodiscard]] std::size_t actual() const noexcept { return actual_; }
  [[nodiscard]] char const* what() const noexcept override {
    return "invalid packed lazy-key prepared-grouping shape";
  }

 private:
  packed_key_grouping_shape_error_kind kind_;
  std::size_t expected_;
  std::size_t actual_;
};

inline packed_key_grouping_prepared_status
try_validate_packed_key_matrix_for_prepared_grouping(
    packed_key_matrix_view keys) noexcept {
  auto const maximum = (std::numeric_limits<std::size_t>::max)();
  if (keys.key_count == maximum) {
    return packed_key_grouping_prepared_status{
        .code = packed_key_grouping_prepared_status_code::offset_count_overflow,
        .required = keys.key_count,
        .available = maximum - 1,
    };
  }
  if (keys.key_count != 0 && keys.key_width > maximum / keys.key_count) {
    return packed_key_grouping_prepared_status{
        .code = packed_key_grouping_prepared_status_code::word_count_overflow,
        .required = keys.key_width,
        .available = maximum / keys.key_count,
        .secondary_available = keys.key_count,
    };
  }
  auto const expected = keys.key_count * keys.key_width;
  if (keys.words.size() != expected) {
    return packed_key_grouping_prepared_status{
        .code = packed_key_grouping_prepared_status_code::word_count_mismatch,
        .required = expected,
        .available = keys.words.size(),
    };
  }
  return {};
}

inline std::size_t validate_packed_key_matrix_for_prepared_grouping(
    packed_key_matrix_view keys) {
  auto const status =
      try_validate_packed_key_matrix_for_prepared_grouping(keys);
  if (!status.succeeded()) {
    auto kind = packed_key_grouping_shape_error_kind::word_count_mismatch;
    if (status.code ==
        packed_key_grouping_prepared_status_code::word_count_overflow) {
      kind = packed_key_grouping_shape_error_kind::word_count_overflow;
    } else if (status.code == packed_key_grouping_prepared_status_code::
                                  offset_count_overflow) {
      kind = packed_key_grouping_shape_error_kind::offset_count_overflow;
    }
    throw packed_key_grouping_shape_error(kind, status.required,
                                          status.available);
  }
  return keys.key_count * keys.key_width;
}

inline std::size_t packed_key_payload_logical_bytes(
    packed_key_matrix_view keys) {
  return checked_packed_key_bytes_multiply(keys.validated_word_count(),
                                           sizeof(packed_key_word),
                                           "packed lazy-key payload");
}

template <class Vector>
inline std::size_t packed_key_vector_capacity_bytes(Vector const& values,
                                                    std::string_view context) {
  return checked_packed_key_bytes_multiply(
      values.capacity(), sizeof(typename Vector::value_type), context);
}

inline std::size_t packed_key_word_buffer_dynamic_capacity_bytes(
    std::vector<packed_key_word> const& words) {
  return packed_key_vector_capacity_bytes(
      words, "packed lazy-key word-buffer capacity");
}

inline std::size_t packed_key_word_buffer_resident_bytes(
    std::vector<packed_key_word> const& words) {
  return checked_packed_key_bytes_add(
      sizeof(words), packed_key_word_buffer_dynamic_capacity_bytes(words),
      "packed lazy-key word-buffer resident");
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

  [[nodiscard]] bool has_worst_case_capacity_for(
      std::size_t key_count) const noexcept {
    if (key_count == (std::numeric_limits<std::size_t>::max)()) return false;
    auto const offset_count = key_count + 1;
    return class_by_input.capacity() >= key_count &&
           representative_by_class.capacity() >= key_count &&
           member_offsets_by_class.capacity() >= offset_count &&
           members_by_class.capacity() >= key_count &&
           lexicographic_class_order.capacity() >= key_count;
  }

  [[nodiscard]] std::array<std::size_t, 5> capacity_by_buffer() const noexcept {
    return {class_by_input.capacity(), representative_by_class.capacity(),
            member_offsets_by_class.capacity(), members_by_class.capacity(),
            lexicographic_class_order.capacity()};
  }

  // Preparation is deliberately separate from finite-budget grouping. A
  // scheduler first prepares and measures task storage, admits that measured
  // capacity, and only then calls the allocation-free grouping operation.
  void reserve_worst_case(std::size_t key_count) {
    auto const offset_count = checked_packed_key_count_add(
        key_count, 1, "packed lazy-key prepared CSR offsets");
    class_by_input.reserve(key_count);
    representative_by_class.reserve(key_count);
    member_offsets_by_class.reserve(offset_count);
    members_by_class.reserve(key_count);
    lexicographic_class_order.reserve(key_count);
  }

  void clear_sizes() noexcept {
    class_by_input.clear();
    representative_by_class.clear();
    member_offsets_by_class.clear();
    members_by_class.clear();
    lexicographic_class_order.clear();
  }

  void swap(packed_key_grouping_result& other) noexcept {
    class_by_input.swap(other.class_by_input);
    representative_by_class.swap(other.representative_by_class);
    member_offsets_by_class.swap(other.member_offsets_by_class);
    members_by_class.swap(other.members_by_class);
    lexicographic_class_order.swap(other.lexicographic_class_order);
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

  [[nodiscard]] bool has_capacity_for(std::size_t key_count) const noexcept {
    return sort_order_.capacity() >= key_count &&
           merge_buffer_.capacity() >= key_count;
  }

  [[nodiscard]] std::pair<std::size_t, std::size_t> capacity_by_buffer()
      const noexcept {
    return {sort_order_.capacity(), merge_buffer_.capacity()};
  }

  void reserve(std::size_t key_count) {
    sort_order_.reserve(key_count);
    merge_buffer_.reserve(key_count);
  }

  void clear_sizes() noexcept {
    sort_order_.clear();
    merge_buffer_.clear();
  }

  void swap(packed_key_grouping_workspace& other) noexcept {
    sort_order_.swap(other.sort_order_);
    merge_buffer_.swap(other.merge_buffer_);
  }

  bool operator==(packed_key_grouping_workspace const&) const = default;

 private:
  std::vector<std::size_t> sort_order_;
  std::vector<std::size_t> merge_buffer_;

  friend packed_key_grouping_prepared_status try_group_packed_keys_prepared(
      packed_key_matrix_view, packed_key_grouping_workspace&,
      packed_key_grouping_result&, std::size_t) noexcept;
  friend packed_key_grouping_result group_packed_keys(
      packed_key_matrix_view, packed_key_grouping_workspace&);
};

struct packed_key_grouping_memory_estimate {
  std::size_t packed_word_count = 0;
  std::size_t packed_payload_logical_bytes = 0;
  std::size_t workspace_logical_dynamic_bytes = 0;
  std::size_t result_logical_dynamic_bytes = 0;
  std::size_t worst_case_logical_owned_resident_bytes = 0;
  std::size_t worst_case_logical_total_resident_bytes = 0;

  bool operator==(packed_key_grouping_memory_estimate const&) const = default;
};

inline std::size_t estimate_packed_key_grouping_result_logical_dynamic_bytes(
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

inline std::size_t estimate_packed_key_grouping_workspace_logical_dynamic_bytes(
    std::size_t key_count) {
  auto const entries = checked_packed_key_count_multiply(
      key_count, 2, "packed lazy-key workspace entries");
  return checked_packed_key_bytes_multiply(
      entries, sizeof(std::size_t), "packed lazy-key workspace estimate");
}

inline std::size_t estimate_packed_key_grouping_logical_owned_resident_bytes(
    std::size_t key_count, std::size_t class_count) {
  auto total = checked_packed_key_bytes_add(
      sizeof(packed_key_grouping_workspace), sizeof(packed_key_grouping_result),
      "packed lazy-key fixed resident estimate");
  total = checked_packed_key_bytes_add(
      total,
      estimate_packed_key_grouping_workspace_logical_dynamic_bytes(key_count),
      "packed lazy-key owned resident estimate");
  return checked_packed_key_bytes_add(
      total,
      estimate_packed_key_grouping_result_logical_dynamic_bytes(key_count,
                                                                class_count),
      "packed lazy-key owned resident estimate");
}

inline packed_key_grouping_memory_estimate estimate_packed_key_grouping_memory(
    std::size_t key_count, std::size_t key_width) {
  auto const word_count = checked_packed_key_count_multiply(
      key_count, key_width, "packed lazy-key payload");
  auto const payload_bytes = checked_packed_key_bytes_multiply(
      word_count, sizeof(packed_key_word), "packed lazy-key payload");
  auto const workspace_bytes =
      estimate_packed_key_grouping_workspace_logical_dynamic_bytes(key_count);

  auto const result_bytes =
      estimate_packed_key_grouping_result_logical_dynamic_bytes(key_count,
                                                                key_count);
  auto const worst_owned =
      estimate_packed_key_grouping_logical_owned_resident_bytes(key_count,
                                                                key_count);
  auto const worst_total = checked_packed_key_bytes_add(
      payload_bytes, worst_owned, "packed lazy-key total resident estimate");
  return packed_key_grouping_memory_estimate{
      .packed_word_count = word_count,
      .packed_payload_logical_bytes = payload_bytes,
      .workspace_logical_dynamic_bytes = workspace_bytes,
      .result_logical_dynamic_bytes = result_bytes,
      .worst_case_logical_owned_resident_bytes = worst_owned,
      .worst_case_logical_total_resident_bytes = worst_total,
  };
}

class packed_key_grouping_budget_error : public std::exception {
 public:
  packed_key_grouping_budget_error(std::size_t required_bytes,
                                   std::size_t budget_bytes) noexcept
      : required_bytes_(required_bytes), budget_bytes_(budget_bytes) {}

  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }

  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return budget_bytes_;
  }
  [[nodiscard]] char const* what() const noexcept override {
    return "packed lazy-key grouping capacity exceeds its admitted limit";
  }

 private:
  std::size_t required_bytes_;
  std::size_t budget_bytes_;
};

inline bool try_packed_key_grouping_owned_resident_bytes(
    packed_key_grouping_workspace const& workspace,
    packed_key_grouping_result const& result,
    std::size_t& resident_bytes) noexcept {
  resident_bytes = sizeof(workspace) + sizeof(result);
  auto try_add = [&](std::size_t capacity,
                     std::size_t element_size) noexcept -> bool {
    auto const maximum = (std::numeric_limits<std::size_t>::max)();
    if (capacity != 0 && element_size > maximum / capacity) return false;
    auto const bytes = capacity * element_size;
    if (resident_bytes > maximum - bytes) return false;
    resident_bytes += bytes;
    return true;
  };
  auto const [sort_capacity, merge_capacity] = workspace.capacity_by_buffer();
  if (!try_add(sort_capacity, sizeof(std::size_t)) ||
      !try_add(merge_capacity, sizeof(std::size_t)) ||
      !try_add(result.class_by_input.capacity(), sizeof(std::size_t)) ||
      !try_add(result.representative_by_class.capacity(),
               sizeof(std::size_t)) ||
      !try_add(result.member_offsets_by_class.capacity(),
               sizeof(std::size_t)) ||
      !try_add(result.members_by_class.capacity(), sizeof(std::size_t)) ||
      !try_add(result.lexicographic_class_order.capacity(),
               sizeof(std::size_t))) {
    resident_bytes = 0;
    return false;
  }
  return true;
}

inline std::size_t packed_key_grouping_owned_resident_bytes(
    packed_key_grouping_workspace const& workspace,
    packed_key_grouping_result const& result) {
  std::size_t resident_bytes = 0;
  if (!try_packed_key_grouping_owned_resident_bytes(workspace, result,
                                                    resident_bytes)) {
    throw std::overflow_error(
        "packed lazy-key actual owned resident byte overflow");
  }
  return resident_bytes;
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

class packed_key_grouping_preparation_required : public std::exception {
 public:
  packed_key_grouping_preparation_required(std::size_t key_count,
                                           std::size_t workspace_capacity,
                                           std::size_t result_capacity) noexcept
      : key_count_(key_count),
        workspace_capacity_(workspace_capacity),
        result_capacity_(result_capacity) {}

  [[nodiscard]] std::size_t key_count() const noexcept { return key_count_; }
  [[nodiscard]] std::size_t workspace_capacity() const noexcept {
    return workspace_capacity_;
  }
  [[nodiscard]] std::size_t result_capacity() const noexcept {
    return result_capacity_;
  }
  [[nodiscard]] char const* what() const noexcept override {
    return "packed lazy-key grouping storage is not prepared";
  }

 private:
  std::size_t key_count_;
  std::size_t workspace_capacity_;
  std::size_t result_capacity_;
};

struct packed_key_grouping_preparation_report {
  std::size_t previous_owned_capacity_resident_bytes = 0;
  std::size_t prepared_owned_capacity_resident_bytes = 0;
  std::size_t observed_prepublication_peak_owned_capacity_resident_bytes = 0;
  std::size_t newly_allocated_dynamic_capacity_bytes = 0;
  bool reused_existing_capacity = false;

  bool operator==(packed_key_grouping_preparation_report const&) const =
      default;
};

struct packed_key_word_buffer_preparation_report {
  std::size_t previous_capacity_resident_bytes = 0;
  std::size_t prepared_capacity_resident_bytes = 0;
  std::size_t observed_prepublication_peak_capacity_resident_bytes = 0;
  std::size_t newly_allocated_dynamic_capacity_bytes = 0;
  bool reused_existing_capacity = false;

  bool operator==(packed_key_word_buffer_preparation_report const&) const =
      default;
};

// This serial preparation follows the same publication contract as grouping
// storage below. The logical lower bound is checked before reserve(), then the
// allocator-selected capacity is measured before publication. The acceptance
// limit is not an allocator hard cap: staging may transiently exceed it, but a
// rejection leaves the caller's words, size, and capacity unchanged.
inline packed_key_word_buffer_preparation_report prepare_packed_key_word_buffer(
    std::size_t word_count, std::vector<packed_key_word>& words,
    std::size_t measured_capacity_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  auto const previous = packed_key_word_buffer_resident_bytes(words);
  if (words.capacity() >= word_count) {
    if (previous > measured_capacity_acceptance_limit_bytes) {
      throw packed_key_grouping_budget_error(
          previous, measured_capacity_acceptance_limit_bytes);
    }
    return packed_key_word_buffer_preparation_report{
        .previous_capacity_resident_bytes = previous,
        .prepared_capacity_resident_bytes = previous,
        .observed_prepublication_peak_capacity_resident_bytes = previous,
        .newly_allocated_dynamic_capacity_bytes = 0,
        .reused_existing_capacity = true,
    };
  }

  auto const logical_new = checked_packed_key_bytes_add(
      sizeof(words),
      checked_packed_key_bytes_multiply(
          word_count, sizeof(packed_key_word),
          "packed lazy-key word-buffer logical preparation"),
      "packed lazy-key word-buffer logical preparation");
  auto const logical_peak = checked_packed_key_bytes_add(
      previous, logical_new,
      "packed lazy-key word-buffer logical preparation peak");
  if (logical_peak > measured_capacity_acceptance_limit_bytes) {
    throw packed_key_grouping_budget_error(
        logical_peak, measured_capacity_acceptance_limit_bytes);
  }

  std::vector<packed_key_word> staged;
  staged.reserve(word_count);
  auto const prepared = packed_key_word_buffer_resident_bytes(staged);
  auto const actual_peak = checked_packed_key_bytes_add(
      previous, prepared,
      "packed lazy-key word-buffer measured preparation peak");
  if (actual_peak > measured_capacity_acceptance_limit_bytes) {
    throw packed_key_grouping_budget_error(
        actual_peak, measured_capacity_acceptance_limit_bytes);
  }
  auto const newly_allocated =
      packed_key_word_buffer_dynamic_capacity_bytes(staged);
  words.swap(staged);
  return packed_key_word_buffer_preparation_report{
      .previous_capacity_resident_bytes = previous,
      .prepared_capacity_resident_bytes = prepared,
      .observed_prepublication_peak_capacity_resident_bytes = actual_peak,
      .newly_allocated_dynamic_capacity_bytes = newly_allocated,
      .reused_existing_capacity = false,
  };
}

// Preparation is a serial operation outside scheduled grouping work. A
// logical lower-bound peak is checked before staging allocations. The
// allocator-selected capacities are then measured while the old storage is
// still intact. The acceptance limit controls publication, not allocation:
// staging may transiently exceed it when std::vector chooses extra capacity.
// Rejection destroys staging and leaves old values, sizes, and capacities
// unchanged. The observed report, not the logical estimate, must be included
// in the caller's serial preparation high-water; stable prepared capacity must
// then be admitted before try_group_packed_keys_prepared().
inline packed_key_grouping_preparation_report
prepare_packed_key_grouping_storage(
    std::size_t key_count, packed_key_grouping_workspace& workspace,
    packed_key_grouping_result& result,
    std::size_t measured_capacity_acceptance_limit_bytes =
        (std::numeric_limits<std::size_t>::max)()) {
  // Validate every worst-case size before inspecting or mutating storage.
  auto const logical_new =
      estimate_packed_key_grouping_logical_owned_resident_bytes(key_count,
                                                                key_count);
  auto const previous =
      packed_key_grouping_owned_resident_bytes(workspace, result);

  if (workspace.has_capacity_for(key_count) &&
      result.has_worst_case_capacity_for(key_count)) {
    if (previous > measured_capacity_acceptance_limit_bytes) {
      throw packed_key_grouping_budget_error(
          previous, measured_capacity_acceptance_limit_bytes);
    }
    return packed_key_grouping_preparation_report{
        .previous_owned_capacity_resident_bytes = previous,
        .prepared_owned_capacity_resident_bytes = previous,
        .observed_prepublication_peak_owned_capacity_resident_bytes = previous,
        .newly_allocated_dynamic_capacity_bytes = 0,
        .reused_existing_capacity = true,
    };
  }

  auto const logical_peak = checked_packed_key_bytes_add(
      previous, logical_new, "packed lazy-key preparation logical peak");
  if (logical_peak > measured_capacity_acceptance_limit_bytes) {
    throw packed_key_grouping_budget_error(
        logical_peak, measured_capacity_acceptance_limit_bytes);
  }

  packed_key_grouping_workspace staged_workspace;
  packed_key_grouping_result staged_result;
  staged_workspace.reserve(key_count);
  staged_result.reserve_worst_case(key_count);

  auto const prepared =
      packed_key_grouping_owned_resident_bytes(staged_workspace, staged_result);
  auto const actual_peak = checked_packed_key_bytes_add(
      previous, prepared, "packed lazy-key preparation measured peak");
  if (actual_peak > measured_capacity_acceptance_limit_bytes) {
    throw packed_key_grouping_budget_error(
        actual_peak, measured_capacity_acceptance_limit_bytes);
  }
  auto const newly_allocated = checked_packed_key_bytes_add(
      staged_workspace.dynamic_capacity_bytes(),
      staged_result.dynamic_capacity_bytes(),
      "packed lazy-key preparation allocated capacity");

  workspace.swap(staged_workspace);
  result.swap(staged_result);
  return packed_key_grouping_preparation_report{
      .previous_owned_capacity_resident_bytes = previous,
      .prepared_owned_capacity_resident_bytes = prepared,
      .observed_prepublication_peak_owned_capacity_resident_bytes = actual_peak,
      .newly_allocated_dynamic_capacity_bytes = newly_allocated,
      .reused_existing_capacity = false,
  };
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

// Allocation-free grouping over pre-admitted storage. The budget covers the
// measured stable capacities of workspace and result. Caller-owned packed
// words are separate and must be admitted with
// packed_key_word_buffer_resident_bytes(). Success and every rejection return
// without allocating; every rejection occurs before result/workspace sizes,
// values, or capacities are changed.
inline packed_key_grouping_prepared_status try_group_packed_keys_prepared(
    packed_key_matrix_view keys, packed_key_grouping_workspace& workspace,
    packed_key_grouping_result& result,
    std::size_t admitted_owned_capacity_bytes) noexcept {
  auto const shape_status =
      try_validate_packed_key_matrix_for_prepared_grouping(keys);
  if (!shape_status.succeeded()) return shape_status;

  auto const workspace_capacity = std::min(workspace.sort_order_.capacity(),
                                           workspace.merge_buffer_.capacity());
  auto const result_capacity =
      std::min({result.class_by_input.capacity(),
                result.representative_by_class.capacity(),
                result.members_by_class.capacity(),
                result.lexicographic_class_order.capacity(),
                result.member_offsets_by_class.capacity() == 0
                    ? std::size_t{0}
                    : result.member_offsets_by_class.capacity() - 1});
  if (!workspace.has_capacity_for(keys.key_count) ||
      !result.has_worst_case_capacity_for(keys.key_count)) {
    return packed_key_grouping_prepared_status{
        .code = packed_key_grouping_prepared_status_code::storage_not_prepared,
        .required = keys.key_count,
        .available = workspace_capacity,
        .secondary_available = result_capacity,
    };
  }
  std::size_t actual_owned_capacity_bytes = 0;
  if (!try_packed_key_grouping_owned_resident_bytes(
          workspace, result, actual_owned_capacity_bytes)) {
    return packed_key_grouping_prepared_status{
        .code = packed_key_grouping_prepared_status_code::
            resident_capacity_overflow,
        .required = (std::numeric_limits<std::size_t>::max)(),
        .available = admitted_owned_capacity_bytes,
    };
  }
  if (actual_owned_capacity_bytes > admitted_owned_capacity_bytes) {
    return packed_key_grouping_prepared_status{
        .code = packed_key_grouping_prepared_status_code::budget_exceeded,
        .required = actual_owned_capacity_bytes,
        .available = admitted_owned_capacity_bytes,
    };
  }

  workspace.sort_order_.resize(keys.key_count);
  workspace.merge_buffer_.resize(keys.key_count);
  result.clear_sizes();
  result.class_by_input.resize(keys.key_count);

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

  result.representative_by_class.resize(class_count);
  // Validation proved key_count < size_t::max(), and class_count <= key_count.
  result.member_offsets_by_class.resize(class_count + 1);
  std::fill(result.member_offsets_by_class.begin(),
            result.member_offsets_by_class.end(), std::size_t{0});
  result.members_by_class.resize(keys.key_count);
  result.lexicographic_class_order.resize(class_count);

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

  return {};
}

[[noreturn]] inline void throw_packed_key_grouping_prepared_failure(
    packed_key_grouping_prepared_status status) {
  switch (status.code) {
    case packed_key_grouping_prepared_status_code::word_count_overflow:
      throw packed_key_grouping_shape_error(
          packed_key_grouping_shape_error_kind::word_count_overflow,
          status.required, status.available);
    case packed_key_grouping_prepared_status_code::word_count_mismatch:
      throw packed_key_grouping_shape_error(
          packed_key_grouping_shape_error_kind::word_count_mismatch,
          status.required, status.available);
    case packed_key_grouping_prepared_status_code::offset_count_overflow:
      throw packed_key_grouping_shape_error(
          packed_key_grouping_shape_error_kind::offset_count_overflow,
          status.required, status.available);
    case packed_key_grouping_prepared_status_code::storage_not_prepared:
      throw packed_key_grouping_preparation_required(
          status.required, status.available, status.secondary_available);
    case packed_key_grouping_prepared_status_code::resident_capacity_overflow:
      throw packed_key_grouping_shape_error(
          packed_key_grouping_shape_error_kind::resident_capacity_overflow,
          status.required, status.available);
    case packed_key_grouping_prepared_status_code::budget_exceeded:
      throw packed_key_grouping_budget_error(status.required, status.available);
    case packed_key_grouping_prepared_status_code::success:
      throw std::logic_error(
          "cannot throw a successful packed lazy-key grouping status");
  }
  std::terminate();
}

// Throwing adapter for serial callers and compatibility boundaries. C++ ABI
// exception storage may allocate, so scheduled/admission-sensitive code must
// call try_group_packed_keys_prepared() and propagate its value status instead.
inline packed_key_grouping_result const& group_packed_keys_prepared(
    packed_key_matrix_view keys, packed_key_grouping_workspace& workspace,
    packed_key_grouping_result& result,
    std::size_t admitted_owned_capacity_bytes) {
  auto const status = try_group_packed_keys_prepared(
      keys, workspace, result, admitted_owned_capacity_bytes);
  if (!status.succeeded()) {
    throw_packed_key_grouping_prepared_failure(status);
  }
  return result;
}

// Convenience for serial/unbounded callers. Finite-budget and scheduled code
// must prepare, measure, admit, and call try_group_packed_keys_prepared()
// instead.
inline packed_key_grouping_result group_packed_keys(
    packed_key_matrix_view keys, packed_key_grouping_workspace& workspace) {
  (void)keys.validated_word_count();
  (void)estimate_packed_key_grouping_memory(keys.key_count, keys.key_width);
  workspace.reserve(keys.key_count);
  packed_key_grouping_result result;
  result.reserve_worst_case(keys.key_count);
  (void)group_packed_keys_prepared(keys, workspace, result,
                                   (std::numeric_limits<std::size_t>::max)());
  return result;
}

}  // namespace larch::lazy_key_grouping_detail
