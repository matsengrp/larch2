#include <larch/protobuf.hpp>

namespace pb {

reader::reader(std::span<const uint8_t> data)
    : pos_{data.data()}, end_{data.data() + data.size()} {}

bool reader::done() const { return pos_ >= end_; }

uint64_t reader::read_varint() {
  uint64_t result = 0;
  int shift = 0;
  while (pos_ < end_) {
    uint8_t byte = *pos_++;
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) return result;
    shift += 7;
  }
  throw std::runtime_error{"truncated varint"};
}

uint32_t reader::read_fixed32() {
  if (end_ - pos_ < 4) throw std::runtime_error{"truncated fixed32"};
  uint32_t val;
  std::memcpy(&val, pos_, 4);
  pos_ += 4;
  return val;
}

std::pair<uint32_t, wire_type> reader::read_tag() {
  auto v = read_varint();
  return {static_cast<uint32_t>(v >> 3), static_cast<wire_type>(v & 7)};
}

std::span<const uint8_t> reader::read_bytes() {
  auto len = read_varint();
  if (static_cast<uint64_t>(end_ - pos_) < len)
    throw std::runtime_error{"truncated length-delimited field"};
  auto start = pos_;
  pos_ += len;
  return {start, static_cast<std::size_t>(len)};
}

void reader::skip(wire_type wt) {
  switch (wt) {
    case wire_type::varint:
      read_varint();
      break;
    case wire_type::fixed64:
      if (end_ - pos_ < 8) throw std::runtime_error{"truncated fixed64"};
      pos_ += 8;
      break;
    case wire_type::length_delimited:
      read_bytes();
      break;
    case wire_type::fixed32:
      if (end_ - pos_ < 4) throw std::runtime_error{"truncated fixed32"};
      pos_ += 4;
      break;
    default:
      throw std::runtime_error{"unknown wire type"};
  }
}

}  // namespace pb
