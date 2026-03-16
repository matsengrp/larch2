#pragma once

#include <larch/io_util.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <meta>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pb {

enum class wire_type : uint8_t {
  varint = 0,
  fixed64 = 1,
  length_delimited = 2,
  fixed32 = 5,
};

class reader {
  const uint8_t* pos_;
  const uint8_t* end_;

 public:
  explicit reader(std::span<const uint8_t> data);
  bool done() const;
  uint64_t read_varint();
  uint32_t read_fixed32();
  std::pair<uint32_t, wire_type> read_tag();
  std::span<const uint8_t> read_bytes();
  void skip(wire_type wt);
};

// Reflection helpers (consteval to avoid storing vector<info>)
template <typename T>
consteval std::size_t member_count() {
  return nonstatic_data_members_of(^^T,
                                   std::meta::access_context::unprivileged())
      .size();
}

template <typename T, std::size_t I>
consteval std::meta::info get_member() {
  return nonstatic_data_members_of(
      ^^T, std::meta::access_context::unprivileged())[I];
}

// Default field numbers: sequential 1..N
template <typename T>
consteval auto make_default_field_numbers() {
  std::array<uint32_t, member_count<T>()> nums{};
  for (uint32_t i = 0; i < nums.size(); ++i) nums[i] = i + 1;
  return nums;
}

template <typename T>
inline constexpr auto field_numbers = make_default_field_numbers<T>();

// Forward declaration
template <typename T>
T decode(std::span<const uint8_t> data);

// --- decode_field overloads (non-template overloads win for exact types) ---

inline void decode_field(int32_t& f, wire_type, reader& r) {
  f = static_cast<int32_t>(r.read_varint());
}

inline void decode_field(int64_t& f, wire_type, reader& r) {
  f = static_cast<int64_t>(r.read_varint());
}

inline void decode_field(float& f, wire_type, reader& r) {
  auto bits = r.read_fixed32();
  std::memcpy(&f, &bits, 4);
}

inline void decode_field(std::string& f, wire_type, reader& r) {
  auto bytes = r.read_bytes();
  f.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

inline void decode_field(std::vector<int32_t>& f, wire_type wt, reader& r) {
  if (wt == wire_type::length_delimited) {
    auto bytes = r.read_bytes();
    reader sub{bytes};
    while (!sub.done()) f.push_back(static_cast<int32_t>(sub.read_varint()));
  } else {
    f.push_back(static_cast<int32_t>(r.read_varint()));
  }
}

inline void decode_field(std::vector<int64_t>& f, wire_type wt, reader& r) {
  if (wt == wire_type::length_delimited) {
    auto bytes = r.read_bytes();
    reader sub{bytes};
    while (!sub.done()) f.push_back(static_cast<int64_t>(sub.read_varint()));
  } else {
    f.push_back(static_cast<int64_t>(r.read_varint()));
  }
}

inline void decode_field(std::vector<std::string>& f, wire_type, reader& r) {
  auto bytes = r.read_bytes();
  f.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Nested message (catch-all for struct types)
template <typename T>
void decode_field(T& f, wire_type, reader& r) {
  auto bytes = r.read_bytes();
  f = decode<T>(bytes);
}

// Repeated message
template <typename T>
void decode_field(std::vector<T>& f, wire_type, reader& r) {
  auto bytes = r.read_bytes();
  f.push_back(decode<T>(bytes));
}

// Main decoder using C++26 reflection
template <typename T>
T decode(std::span<const uint8_t> data) {
  T msg{};
  reader r{data};
  while (!r.done()) {
    auto [fnum, wt] = r.read_tag();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      bool matched =
          ((field_numbers<T>[Is] == fnum
                ? (decode_field(msg.[:get_member<T, Is>():], wt, r), true)
                : false) ||
           ...);
      if (!matched) r.skip(wt);
    }(std::make_index_sequence<member_count<T>()>{});
  }
  return msg;
}

// Collect spans for all occurrences of a repeated field at the top level.
// Only reads tag+length, skips content — fast sequential scan.
inline std::vector<std::span<const uint8_t>> collect_field_spans(
    std::span<const uint8_t> data, uint32_t field_number) {
  std::vector<std::span<const uint8_t>> result;
  reader r{data};
  while (!r.done()) {
    auto [fnum, wt] = r.read_tag();
    if (fnum == field_number && wt == wire_type::length_delimited) {
      result.push_back(r.read_bytes());
    } else {
      r.skip(wt);
    }
  }
  return result;
}

// Convenience: decode from file (supports .gz via io_util)
template <typename T>
T decode_file(std::string_view path) {
  if (!larch::is_gzipped(path)) {
    larch::mmap_file mf{path};
    return decode<T>(mf.span());
  }
  auto bytes = larch::read_file(path);
  return decode<T>(
      {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
}

}  // namespace pb
