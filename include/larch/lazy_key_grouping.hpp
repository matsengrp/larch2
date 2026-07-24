#pragma once

#include <algorithm>
#include <array>
#include <bit>
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

// This project is frozen to GCC 17/libstdc++. Its allocate_at_least
// implementation rounds allocations of sub-default-alignment element types
// toward the next default-new-alignment byte boundary and reports the extra
// whole elements as vector capacity. Keep logical admission estimates in the
// same capacity coordinate; measured preparation remains the final guard.
template <class Element>
inline std::size_t frozen_libstdcxx_allocate_at_least_capacity(
    std::size_t requested_count, std::string_view context) {
  if (requested_count == 0) return 0;
  constexpr auto allocation_alignment =
      std::size_t{__STDCPP_DEFAULT_NEW_ALIGNMENT__};
  static_assert((allocation_alignment & (allocation_alignment - 1)) == 0);
  if constexpr (alignof(Element) > allocation_alignment ||
                sizeof(Element) >= allocation_alignment) {
    return requested_count;
  } else {
    auto const requested_bytes = checked_packed_key_bytes_multiply(
        requested_count, sizeof(Element), context);
    auto const rounded_bytes =
        checked_packed_key_bytes_add(requested_bytes, allocation_alignment - 1,
                                     context) &
        ~(allocation_alignment - 1);
    auto const spare_bytes = rounded_bytes - requested_bytes;
    std::size_t bonus_count = 0;
    if constexpr (sizeof(Element) < allocation_alignment / 2) {
      bonus_count = spare_bytes / sizeof(Element);
    } else if (sizeof(Element) <= spare_bytes) {
      bonus_count = 1;
    }
    return checked_packed_key_count_add(requested_count, bonus_count, context);
  }
}

template <class Element>
inline std::size_t frozen_libstdcxx_allocate_at_least_capacity_bytes(
    std::size_t requested_count, std::string_view context) {
  return checked_packed_key_bytes_multiply(
      frozen_libstdcxx_allocate_at_least_capacity<Element>(requested_count,
                                                           context),
      sizeof(Element), context);
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
  partition_class_out_of_range,
};

enum class packed_key_grouping_prepared_status_code : std::uint8_t {
  success,
  word_count_overflow,
  word_count_mismatch,
  offset_count_overflow,
  storage_not_prepared,
  resident_capacity_overflow,
  budget_exceeded,
  partition_class_out_of_range,
};

// Fixed-size value result for the scheduled grouping seam. `required` and
// `available`
// describe counts for shape/storage failures and bytes for resident/budget
// failures. `secondary_available` is the result-storage capacity when
// storage_not_prepared is reported, and the key count for word-count overflow.
// A published-partition range failure reports the exclusive class cardinality
// in `required`, the invalid class ID in `available`, and its input index in
// `secondary_available`. For resident-capacity overflow,
// `required == size_t::max()` is a sentinel: the true byte requirement is not
// representable. Returning this status never allocates exception storage.
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

enum class packed_key_grouping_result_mode : std::uint8_t {
  // Stable first-occurrence class map and representative only.
  classes,
  // `classes` plus deterministic lexicographic class traversal.
  ordered_classes,
  // `ordered_classes` plus input-order CSR membership.
  full,
};

// The ordinary adaptive route is tuned for general serial callers. Search
// waves that are already executing several independent candidates may opt in
// to the wider radix route: it uses the same admitted storage and publishes
// exactly the same stable classes, while avoiding comparison-heavy wide-key
// merge sorts in the bandwidth-saturated lazy-local kernel.
enum class packed_key_grouping_sort_policy : std::uint8_t {
  adaptive,
  parallel_wide_radix,
};

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
    if (member_offsets_by_class.size() != class_count() + 1 ||
        members_by_class.size() != class_by_input.size()) {
      throw std::logic_error(
          "packed lazy-key grouping: class membership was not materialized");
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

// A small exact fast path can group tuples of already-published partition
// class maps without first narrowing and materializing a packed-key matrix.
// Components are ordered most-significant first, so the mixed-radix token has
// the same order as lexicographic tuple comparison. The optimization is
// deliberately bounded to at most three components and a token domain no
// larger than key_count; callers fall back to packed grouping otherwise.
struct bounded_partition_tuple_view {
  std::size_t key_count = 0;
  std::size_t tuple_width = 0;
  std::array<std::span<std::size_t const>, 3> class_by_input;
  std::array<std::size_t, 3> class_count;
};

struct bounded_partition_tuple_grouping_attempt {
  bool applicable = false;
  packed_key_grouping_prepared_status status;
};

// Arbitrary callers retain the transactional optimization-miss contract: every
// source map is checked before workspace/result mutation. Lazy-chart callers
// may identify maps that were just published by an exact grouping operation.
// That stronger contract lets grouping validate each component while building
// its injective mixed-radix token, avoiding a separate full-map pass.
enum class bounded_partition_tuple_input_contract : std::uint8_t {
  arbitrary,
  published_partitions,
};

class packed_key_grouping_workspace;

inline bounded_partition_tuple_grouping_attempt
try_group_bounded_partition_tuple_prepared(
    bounded_partition_tuple_view tuple,
    packed_key_grouping_workspace& workspace,
    packed_key_grouping_result& result,
    std::size_t admitted_owned_capacity_bytes,
    packed_key_grouping_result_mode result_mode =
        packed_key_grouping_result_mode::full,
    bounded_partition_tuple_input_contract input_contract =
        bounded_partition_tuple_input_contract::arbitrary) noexcept;

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

  [[nodiscard]] bool sizes_empty() const noexcept {
    return sort_order_.empty() && merge_buffer_.empty();
  }

  void swap(packed_key_grouping_workspace& other) noexcept {
    sort_order_.swap(other.sort_order_);
    merge_buffer_.swap(other.merge_buffer_);
    radix_buckets_.swap(other.radix_buckets_);
  }

  bool operator==(packed_key_grouping_workspace const&) const = default;

 private:
  std::vector<std::size_t> sort_order_;
  std::vector<std::size_t> merge_buffer_;
  // Keep radix scratch in the owned workspace so its fixed resident bytes are
  // included in every existing sizeof(workspace)-based estimate and admission
  // check. The two dynamic index buffers remain the only per-key scratch.
  std::array<std::size_t, 256> radix_buckets_{};

  friend packed_key_grouping_prepared_status try_group_packed_keys_prepared(
      packed_key_matrix_view, packed_key_grouping_workspace&,
      packed_key_grouping_result&, std::size_t,
      packed_key_grouping_result_mode,
      packed_key_grouping_sort_policy) noexcept;
  friend bounded_partition_tuple_grouping_attempt
  try_group_bounded_partition_tuple_prepared(
      bounded_partition_tuple_view, packed_key_grouping_workspace&,
      packed_key_grouping_result&, std::size_t,
      packed_key_grouping_result_mode,
      bounded_partition_tuple_input_contract) noexcept;
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
                                   std::size_t budget_bytes,
                                   std::size_t observed_peak_bytes = 0) noexcept
      : required_bytes_(required_bytes),
        budget_bytes_(budget_bytes),
        observed_peak_bytes_(observed_peak_bytes) {}

  [[nodiscard]] std::size_t required_bytes() const noexcept {
    return required_bytes_;
  }

  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return budget_bytes_;
  }

  // Zero means no unpublished staging peak is carried; callers can measure
  // their still-published live storage. Otherwise this is the measured
  // transient peak, expressed in the same resident-byte coordinate as
  // required_bytes().
  [[nodiscard]] std::size_t observed_peak_bytes() const noexcept {
    return observed_peak_bytes_;
  }
  [[nodiscard]] char const* what() const noexcept override {
    return "packed lazy-key grouping capacity exceeds its admitted limit";
  }

 private:
  std::size_t required_bytes_;
  std::size_t budget_bytes_;
  std::size_t observed_peak_bytes_;
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
        actual_peak, measured_capacity_acceptance_limit_bytes, actual_peak);
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
        actual_peak, measured_capacity_acceptance_limit_bytes, actual_peak);
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

inline bool should_use_stable_lsd_radix_sort(
    std::size_t key_count, std::size_t key_width,
    packed_key_grouping_sort_policy policy =
        packed_key_grouping_sort_policy::adaptive) noexcept {
  // Retain merge sort for small inputs and zero-width keys. Ordinary callers
  // use radix through width four; an explicitly parallel lazy-local wave may
  // extend that allocation-free route through the profiled width-eight range.
  auto const maximum_width =
      policy == packed_key_grouping_sort_policy::parallel_wide_radix
          ? std::size_t{8}
          : std::size_t{4};
  return key_count >= 128 && key_width >= 1 && key_width <= maximum_width;
}

inline bool should_try_first_occurrence_hash_grouping(
    std::size_t key_count, std::size_t key_width) noexcept {
  // Hash interning pays when a large matrix collapses to relatively few
  // classes, which is precisely the lazy-chart compression case.  The same
  // bounded-width gate retains merge sort for small/wide compatibility inputs;
  // a deterministic radix fallback below handles weakly compressed matrices.
  return key_count >= 128 && key_width >= 1 && key_width <= 4;
}

inline std::uint64_t packed_key_hash(packed_key_matrix_view keys,
                                     std::size_t input) noexcept {
  auto const offset = input * keys.key_width;
  auto pack_pair = [&](std::size_t first) noexcept {
    return (static_cast<std::uint64_t>(keys.words[offset + first]) << 32) |
           static_cast<std::uint64_t>(keys.words[offset + first + 1]);
  };

  // These are the only widths admitted to the hash route. Pack widths one
  // and two injectively and combine the second pair for widths three/four.
  // One final multiply per complete tuple is materially cheaper than the old
  // two-multiply SplitMix round per word. Hash equality is only a filter:
  // packed_key_equal() below remains the exact collision oracle.
  std::uint64_t value = 0;
  switch (keys.key_width) {
    case 1:
      value = keys.words[offset];
      break;
    case 2:
      value = pack_pair(0);
      break;
    case 3:
      value = pack_pair(0) ^
              std::rotl(static_cast<std::uint64_t>(keys.words[offset + 2]),
                        29);
      break;
    case 4:
      value = pack_pair(0) ^ std::rotl(pack_pair(2), 29);
      break;
    default:
      // Kept total for defensive direct callers; the adaptive gate rejects
      // this case before reaching the hash route.
      return 0;
  }

  value ^= std::uint64_t{0x9e3779b97f4a7c15ULL} * keys.key_width;
  value ^= value >> 30;
  value *= std::uint64_t{0xbf58476d1ce4e5b9ULL};
  value ^= value >> 27;
  return value;
}

inline void stable_merge_sort_class_ids_by_representative(
    packed_key_matrix_view keys,
    std::vector<std::size_t> const& representative_by_class,
    std::vector<std::size_t>& order,
    std::vector<std::size_t>& merge_buffer) noexcept {
  auto const count = representative_by_class.size();
  order.resize(count);
  merge_buffer.resize(count);
  for (std::size_t class_id = 0; class_id < count; ++class_id) {
    order[class_id] = class_id;
  }
  if (count < 2) return;

  // Sort only the distinct representatives. This is the exact lexicographic
  // class order produced by sorting all inputs, while retaining the prepared,
  // allocation-free storage contract.
  for (std::size_t run_width = 1;;) {
    for (std::size_t begin = 0; begin < count;) {
      auto const middle = begin + std::min(run_width, count - begin);
      auto const end = middle + std::min(run_width, count - middle);
      auto left = begin;
      auto right = middle;
      auto output = begin;
      while (left < middle && right < end) {
        auto const left_input = representative_by_class[order[left]];
        auto const right_input = representative_by_class[order[right]];
        if (packed_key_less(keys, right_input, left_input)) {
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
    if (run_width >= count - run_width) break;
    run_width *= 2;
  }
}

inline bool try_first_occurrence_hash_grouping(
    packed_key_matrix_view keys, std::vector<std::size_t>& buckets,
    std::vector<std::size_t>& bucket_hashes_and_sort_scratch,
    packed_key_grouping_result& result,
    bool build_lexicographic_order = true) noexcept {
  if (!should_try_first_occurrence_hash_grouping(keys.key_count,
                                                  keys.key_width)) {
    return false;
  }

  // Reuse the already admitted sort-order array as an open-address table. A
  // power-of-two prefix avoids division, and abandoning the attempt before a
  // 50% load factor gives bounded expected probes on compressed inputs. Any
  // adversarial collision chain or weakly compressed input falls back to the
  // exact stable radix implementation using that same admitted storage.
  auto const bucket_count = std::bit_floor(keys.key_count);
  auto const bucket_mask = bucket_count - 1;
  auto const maximum_classes = bucket_count / 2;
  auto const no_class = (std::numeric_limits<std::size_t>::max)();
  buckets.resize(keys.key_count);
  bucket_hashes_and_sort_scratch.resize(keys.key_count);
  std::fill_n(buckets.begin(), bucket_count, no_class);
  result.representative_by_class.clear();

  constexpr std::size_t maximum_probe_count = 32;
  for (std::size_t input = 0; input < keys.key_count; ++input) {
    // Lazy-chart keys are emitted in pattern order and commonly form long
    // runs.  The preceding input already has its canonical first-occurrence
    // class, so an exact adjacent equality can reuse it without hashing or
    // probing.  This does not alter table occupancy, representative order, or
    // the deterministic radix fallback when compression is weak.
    if (input != 0 && packed_key_equal(keys, input - 1, input)) {
      result.class_by_input[input] = result.class_by_input[input - 1];
      continue;
    }
    auto const hash = static_cast<std::size_t>(packed_key_hash(keys, input));
    auto bucket = hash & bucket_mask;
    bool assigned = false;
    for (std::size_t probe = 0; probe < maximum_probe_count; ++probe) {
      auto& class_id = buckets[bucket];
      if (class_id == no_class) {
        if (result.representative_by_class.size() >= maximum_classes) {
          return false;
        }
        class_id = result.representative_by_class.size();
        bucket_hashes_and_sort_scratch[bucket] = hash;
        result.representative_by_class.push_back(input);
        result.class_by_input[input] = class_id;
        assigned = true;
        break;
      }
      if (bucket_hashes_and_sort_scratch[bucket] == hash &&
          packed_key_equal(keys, result.representative_by_class[class_id],
                           input)) {
        result.class_by_input[input] = class_id;
        assigned = true;
        break;
      }
      bucket = (bucket + 1) & bucket_mask;
    }
    if (!assigned) return false;
  }

  if (build_lexicographic_order) {
    stable_merge_sort_class_ids_by_representative(
        keys, result.representative_by_class,
        result.lexicographic_class_order, bucket_hashes_and_sort_scratch);
  }
  return true;
}

inline void stable_lsd_radix_sort_key_indices(
    packed_key_matrix_view keys, std::vector<std::size_t>& order,
    std::vector<std::size_t>& merge_buffer,
    std::array<std::size_t, 256>& buckets) noexcept {
  for (std::size_t index = 0; index < keys.key_count; ++index) {
    order[index] = index;
  }

  auto* source = &order;
  auto* destination = &merge_buffer;
  constexpr unsigned bits_per_byte = 8;
  constexpr packed_key_word byte_mask = 0xff;

  // A packed key is a lexicographic tuple of uint32_t words. Stable LSD passes
  // therefore visit tuple words last-to-first and each numeric word's bytes
  // low-to-high. Shift/mask extraction makes this independent of host byte
  // order. Scattering source indices left-to-right preserves input order for
  // equal complete keys.
  for (auto word = keys.key_width; word-- != 0;) {
    packed_key_word maximum_value = 0;
    for (auto index : *source) {
      maximum_value =
          std::max(maximum_value, keys.words[index * keys.key_width + word]);
    }
    auto const significant_bits = std::bit_width(maximum_value);
    auto const significant_bytes =
        (significant_bits + bits_per_byte - 1) / bits_per_byte;
    for (unsigned shift = 0; shift < significant_bytes * bits_per_byte;
         shift += bits_per_byte) {
      buckets.fill(0);
      for (auto index : *source) {
        auto const value = keys.words[index * keys.key_width + word];
        auto const digit =
            static_cast<std::size_t>((value >> shift) & byte_mask);
        ++buckets[digit];
      }

      std::size_t occupied_buckets = 0;
      for (auto count : buckets) occupied_buckets += count != 0;
      if (occupied_buckets < 2) continue;

      std::size_t next_offset = 0;
      for (auto& bucket : buckets) {
        auto const count = bucket;
        bucket = next_offset;
        next_offset += count;
      }
      for (auto index : *source) {
        auto const value = keys.words[index * keys.key_width + word];
        auto const digit =
            static_cast<std::size_t>((value >> shift) & byte_mask);
        (*destination)[buckets[digit]++] = index;
      }
      std::swap(source, destination);
    }
  }

  // An odd number of nonconstant byte passes leaves the sorted indices in the
  // secondary vector. Preserve the established postcondition that `order`
  // owns them without copying or allocating.
  if (source != &order) order.swap(merge_buffer);
}

}  // namespace implementation

// Allocation-free grouping over a bounded Cartesian product of already
// published partition maps. `applicable == false` is an optimization miss and
// leaves workspace/result untouched, so the caller can run the ordinary
// packed-key path. An applicable attempt has the same preparation/admission
// contract and result semantics as try_group_packed_keys_prepared().
inline bounded_partition_tuple_grouping_attempt
try_group_bounded_partition_tuple_prepared(
    bounded_partition_tuple_view tuple,
    packed_key_grouping_workspace& workspace,
    packed_key_grouping_result& result,
    std::size_t admitted_owned_capacity_bytes,
    packed_key_grouping_result_mode result_mode,
    bounded_partition_tuple_input_contract input_contract) noexcept {
  if (tuple.tuple_width < 2 || tuple.tuple_width > tuple.class_by_input.size()) {
    return {};
  }
  if (tuple.key_count == (std::numeric_limits<std::size_t>::max)()) {
    return bounded_partition_tuple_grouping_attempt{
        .applicable = true,
        .status = packed_key_grouping_prepared_status{
            .code = packed_key_grouping_prepared_status_code::
                offset_count_overflow,
            .required = tuple.key_count,
            .available = tuple.key_count - 1,
        },
    };
  }

  std::size_t token_count = 1;
  for (std::size_t component = 0; component < tuple.tuple_width;
       ++component) {
    if (tuple.class_by_input[component].size() != tuple.key_count ||
        tuple.class_count[component] == 0) {
      return {};
    }
    if (token_count > tuple.key_count / tuple.class_count[component]) {
      return {};
    }
    token_count *= tuple.class_count[component];
  }
  if (token_count > tuple.key_count) return {};

  if (input_contract == bounded_partition_tuple_input_contract::arbitrary) {
    // An optimization miss must leave both caller-owned objects untouched.
    // Check arbitrary maps before admission or mutation so the packed-key
    // fallback sees exactly the state supplied by its caller.
    for (std::size_t component = 0; component < tuple.tuple_width;
         ++component) {
      auto const cardinality = tuple.class_count[component];
      for (auto const class_id : tuple.class_by_input[component]) {
        if (class_id >= cardinality ||
            class_id > (std::numeric_limits<packed_key_word>::max)()) {
          return {};
        }
      }
    }
  }

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
  if (!workspace.has_capacity_for(tuple.key_count) ||
      !result.has_worst_case_capacity_for(tuple.key_count)) {
    return bounded_partition_tuple_grouping_attempt{
        .applicable = true,
        .status = packed_key_grouping_prepared_status{
            .code = packed_key_grouping_prepared_status_code::
                storage_not_prepared,
            .required = tuple.key_count,
            .available = workspace_capacity,
            .secondary_available = result_capacity,
        },
    };
  }
  std::size_t actual_owned_capacity_bytes = 0;
  if (!try_packed_key_grouping_owned_resident_bytes(
          workspace, result, actual_owned_capacity_bytes)) {
    return bounded_partition_tuple_grouping_attempt{
        .applicable = true,
        .status = packed_key_grouping_prepared_status{
            .code = packed_key_grouping_prepared_status_code::
                resident_capacity_overflow,
            .required = (std::numeric_limits<std::size_t>::max)(),
            .available = admitted_owned_capacity_bytes,
        },
    };
  }
  if (actual_owned_capacity_bytes > admitted_owned_capacity_bytes) {
    return bounded_partition_tuple_grouping_attempt{
        .applicable = true,
        .status = packed_key_grouping_prepared_status{
            .code = packed_key_grouping_prepared_status_code::budget_exceeded,
            .required = actual_owned_capacity_bytes,
            .available = admitted_owned_capacity_bytes,
        },
    };
  }

  auto const no_class = (std::numeric_limits<std::size_t>::max)();
  // Materialize one injective mixed-radix token per input into already-admitted
  // scratch. The previous implementation compared adjacent tuples by rereading
  // two complete source tuples and then reread a non-adjacent tuple to compute
  // its token. Published chart partitions are valid by construction, but retain
  // a fused release-mode bounds check before using a component as a radix
  // digit. An invalid published map is an invariant failure, not an
  // optimization miss; result remains unpublished and the caller propagates
  // the returned status.
  workspace.merge_buffer_.resize(tuple.key_count);
  for (std::size_t input = 0; input < tuple.key_count; ++input) {
    std::size_t token = 0;
    for (std::size_t component = 0; component < tuple.tuple_width;
         ++component) {
      auto const class_id = tuple.class_by_input[component][input];
      auto const cardinality = tuple.class_count[component];
      if (input_contract ==
              bounded_partition_tuple_input_contract::published_partitions &&
          (class_id >= cardinality ||
           class_id > (std::numeric_limits<packed_key_word>::max)())) {
        return bounded_partition_tuple_grouping_attempt{
            .applicable = true,
            .status = packed_key_grouping_prepared_status{
                .code = packed_key_grouping_prepared_status_code::
                    partition_class_out_of_range,
                .required = cardinality,
                .available = class_id,
                .secondary_available = input,
            },
        };
      }
      token = token * cardinality + class_id;
    }
    workspace.merge_buffer_[input] = token;
  }

  workspace.sort_order_.resize(token_count);
  std::fill(workspace.sort_order_.begin(), workspace.sort_order_.end(),
            no_class);
  result.clear_sizes();
  result.class_by_input.resize(tuple.key_count);

  for (std::size_t input = 0; input < tuple.key_count; ++input) {
    auto const token = workspace.merge_buffer_[input];
    if (input != 0 && workspace.merge_buffer_[input - 1] == token) {
      result.class_by_input[input] = result.class_by_input[input - 1];
      continue;
    }
    auto& class_id = workspace.sort_order_[token];
    if (class_id == no_class) {
      class_id = result.representative_by_class.size();
      result.representative_by_class.push_back(input);
    }
    result.class_by_input[input] = class_id;
  }

  if (result_mode != packed_key_grouping_result_mode::classes) {
    for (auto const class_id : workspace.sort_order_) {
      if (class_id != no_class) {
        result.lexicographic_class_order.push_back(class_id);
      }
    }
  }
  if (result_mode == packed_key_grouping_result_mode::full) {
    auto const class_count = result.representative_by_class.size();
    result.member_offsets_by_class.resize(class_count + 1);
    std::fill(result.member_offsets_by_class.begin(),
              result.member_offsets_by_class.end(), std::size_t{0});
    result.members_by_class.resize(tuple.key_count);
    for (auto const class_id : result.class_by_input) {
      ++result.member_offsets_by_class[class_id + 1];
    }
    for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
      result.member_offsets_by_class[class_id + 1] +=
          result.member_offsets_by_class[class_id];
      workspace.merge_buffer_[class_id] =
          result.member_offsets_by_class[class_id];
    }
    for (std::size_t input = 0; input < tuple.key_count; ++input) {
      auto const class_id = result.class_by_input[input];
      result.members_by_class[workspace.merge_buffer_[class_id]++] = input;
    }
  }

  return bounded_partition_tuple_grouping_attempt{.applicable = true};
}

// Allocation-free grouping over pre-admitted storage. The budget covers the
// measured stable capacities of workspace and result. Caller-owned packed
// words are separate and must be admitted with
// packed_key_word_buffer_resident_bytes(). Success and every rejection return
// without allocating; every rejection occurs before result/workspace sizes,
// values, or capacities are changed.
inline packed_key_grouping_prepared_status try_group_packed_keys_prepared(
    packed_key_matrix_view keys, packed_key_grouping_workspace& workspace,
    packed_key_grouping_result& result,
    std::size_t admitted_owned_capacity_bytes,
    packed_key_grouping_result_mode result_mode =
        packed_key_grouping_result_mode::full,
    packed_key_grouping_sort_policy sort_policy =
        packed_key_grouping_sort_policy::adaptive) noexcept {
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

  auto const hash_grouped =
      implementation::try_first_occurrence_hash_grouping(
          keys, workspace.sort_order_, workspace.merge_buffer_, result,
          result_mode != packed_key_grouping_result_mode::classes);
  if (!hash_grouped) {
    // A failed hash attempt has modified only prepared result values/sizes;
    // clear them before the deterministic sort fallback. Capacities and the
    // already completed admission decision remain unchanged.
    result.clear_sizes();
    result.class_by_input.resize(keys.key_count);
    workspace.sort_order_.resize(keys.key_count);
    workspace.merge_buffer_.resize(keys.key_count);

    if (implementation::should_use_stable_lsd_radix_sort(
            keys.key_count, keys.key_width, sort_policy)) {
      implementation::stable_lsd_radix_sort_key_indices(
          keys, workspace.sort_order_, workspace.merge_buffer_,
          workspace.radix_buckets_);
    } else {
      implementation::stable_merge_sort_key_indices(
          keys, workspace.sort_order_, workspace.merge_buffer_);
    }

    std::size_t lexicographic_class_count = 0;
    for (std::size_t begin = 0; begin < keys.key_count;) {
      auto end = begin + 1;
      while (end < keys.key_count &&
             implementation::packed_key_equal(
                 keys, workspace.sort_order_[begin],
                 workspace.sort_order_[end])) {
        ++end;
      }
      for (auto position = begin; position < end; ++position) {
        result.class_by_input[workspace.sort_order_[position]] =
            lexicographic_class_count;
      }
      ++lexicographic_class_count;
      begin = end;
    }

    result.representative_by_class.resize(lexicographic_class_count);
    if (result_mode != packed_key_grouping_result_mode::classes) {
      result.lexicographic_class_order.resize(lexicographic_class_count);
    }

    // The sort pass assigned temporary lexicographic class IDs. Convert them
    // to stable first-occurrence IDs with one input-order scan. merge_buffer is
    // no longer needed by the sort and becomes the lexicographic-to-stable map.
    auto const no_class = (std::numeric_limits<std::size_t>::max)();
    workspace.merge_buffer_.resize(lexicographic_class_count);
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
    if (result_mode != packed_key_grouping_result_mode::classes) {
      for (std::size_t lexicographic_class = 0;
           lexicographic_class < lexicographic_class_count;
           ++lexicographic_class) {
        result.lexicographic_class_order[lexicographic_class] =
            workspace.merge_buffer_[lexicographic_class];
      }
    }
  }

  if (result_mode != packed_key_grouping_result_mode::full) return {};

  auto const class_count = result.representative_by_class.size();
  // Validation proved key_count < size_t::max(), and class_count <= key_count.
  result.member_offsets_by_class.resize(class_count + 1);
  std::fill(result.member_offsets_by_class.begin(),
            result.member_offsets_by_class.end(), std::size_t{0});
  result.members_by_class.resize(keys.key_count);

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
    case packed_key_grouping_prepared_status_code::
        partition_class_out_of_range:
      throw packed_key_grouping_shape_error(
          packed_key_grouping_shape_error_kind::partition_class_out_of_range,
          status.required, status.available);
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
    std::size_t admitted_owned_capacity_bytes,
    packed_key_grouping_result_mode result_mode =
        packed_key_grouping_result_mode::full) {
  auto const status = try_group_packed_keys_prepared(
      keys, workspace, result, admitted_owned_capacity_bytes, result_mode);
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
