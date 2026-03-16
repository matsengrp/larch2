#include <larch/protobuf_encode.hpp>

namespace pb {

void writer::write_varint(uint64_t v) {
  while (v > 0x7F) {
    buf_.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  buf_.push_back(static_cast<uint8_t>(v));
}

void writer::write_fixed32(uint32_t v) {
  buf_.push_back(static_cast<uint8_t>(v));
  buf_.push_back(static_cast<uint8_t>(v >> 8));
  buf_.push_back(static_cast<uint8_t>(v >> 16));
  buf_.push_back(static_cast<uint8_t>(v >> 24));
}

void writer::write_tag(uint32_t field_number, wire_type wt) {
  write_varint((static_cast<uint64_t>(field_number) << 3) |
               static_cast<uint64_t>(wt));
}

void writer::write_length_prefixed(std::span<const uint8_t> data) {
  write_varint(data.size());
  buf_.insert(buf_.end(), data.begin(), data.end());
}

std::vector<uint8_t> const& writer::data() const { return buf_; }

}  // namespace pb
