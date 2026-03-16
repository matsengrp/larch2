#pragma once

#include <larch/protobuf.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace pb {

class writer {
  std::vector<uint8_t> buf_;

 public:
  void write_varint(uint64_t v);
  void write_fixed32(uint32_t v);
  void write_tag(uint32_t field_number, wire_type wt);
  void write_length_prefixed(std::span<const uint8_t> data);
  std::vector<uint8_t> const& data() const;
};

// Forward declaration
template <typename T>
std::vector<uint8_t> encode(T const& msg);

// --- encode_field overloads (mirror of decode_field overloads) ---

inline void encode_field(int32_t f, uint32_t fnum, writer& w) {
  if (f == 0) return;
  w.write_tag(fnum, wire_type::varint);
  w.write_varint(static_cast<uint64_t>(f));
}

inline void encode_field(int64_t f, uint32_t fnum, writer& w) {
  if (f == 0) return;
  w.write_tag(fnum, wire_type::varint);
  w.write_varint(static_cast<uint64_t>(f));
}

inline void encode_field(float f, uint32_t fnum, writer& w) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  if (bits == 0) return;
  w.write_tag(fnum, wire_type::fixed32);
  w.write_fixed32(bits);
}

inline void encode_field(std::string const& f, uint32_t fnum, writer& w) {
  if (f.empty()) return;
  w.write_tag(fnum, wire_type::length_delimited);
  w.write_length_prefixed(
      {reinterpret_cast<const uint8_t*>(f.data()), f.size()});
}

// Packed varint: vector<int32_t>
inline void encode_field(std::vector<int32_t> const& f, uint32_t fnum,
                         writer& w) {
  if (f.empty()) return;
  writer packed;
  for (auto v : f) packed.write_varint(static_cast<uint64_t>(v));
  w.write_tag(fnum, wire_type::length_delimited);
  w.write_length_prefixed({packed.data().data(), packed.data().size()});
}

// Packed varint: vector<int64_t>
inline void encode_field(std::vector<int64_t> const& f, uint32_t fnum,
                         writer& w) {
  if (f.empty()) return;
  writer packed;
  for (auto v : f) packed.write_varint(static_cast<uint64_t>(v));
  w.write_tag(fnum, wire_type::length_delimited);
  w.write_length_prefixed({packed.data().data(), packed.data().size()});
}

// Repeated length-delimited: vector<string>
inline void encode_field(std::vector<std::string> const& f, uint32_t fnum,
                         writer& w) {
  for (auto const& s : f) {
    w.write_tag(fnum, wire_type::length_delimited);
    w.write_length_prefixed(
        {reinterpret_cast<const uint8_t*>(s.data()), s.size()});
  }
}

// Nested message (catch-all for struct types)
template <typename T>
void encode_field(T const& f, uint32_t fnum, writer& w) {
  auto bytes = encode(f);
  if (bytes.empty()) return;
  w.write_tag(fnum, wire_type::length_delimited);
  w.write_length_prefixed({bytes.data(), bytes.size()});
}

// Repeated message
template <typename T>
void encode_field(std::vector<T> const& f, uint32_t fnum, writer& w) {
  for (auto const& elem : f) {
    auto bytes = encode(elem);
    w.write_tag(fnum, wire_type::length_delimited);
    w.write_length_prefixed({bytes.data(), bytes.size()});
  }
}

// Main encoder using C++26 reflection
template <typename T>
std::vector<uint8_t> encode(T const& msg) {
  writer w;
  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
    (encode_field(msg.[:get_member<T, Is>():], field_numbers<T>[Is], w), ...);
  }(std::make_index_sequence<member_count<T>()>{});
  return w.data();
}

// Convenience: encode to file
template <typename T>
void encode_file(std::string_view path, T const& msg) {
  auto bytes = encode(msg);
  std::ofstream out{std::string{path}, std::ios::binary};
  if (!out)
    throw std::runtime_error{"cannot open " + std::string{path} +
                             " for writing"};
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

}  // namespace pb
