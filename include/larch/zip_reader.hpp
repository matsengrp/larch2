#pragma once

#include <larch/io_util.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

// Minimal read-only ZIP parser for uncompressed (stored) archives.
// Designed for PyTorch .pth files which use compression_method=0.
// Zero-copy: returned spans point directly into the mmap'd file.
class zip_reader {
  mmap_file file_;

  struct entry {
    std::string name;
    std::size_t data_offset;  // offset of file data in the archive
    std::size_t size;         // uncompressed size
  };
  std::vector<entry> entries_;

  static uint16_t read_u16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;  // little-endian on x86
  }

  static uint32_t read_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
  }

  void parse() {
    auto data = file_.span();
    if (data.size() < 22)
      throw std::runtime_error{"zip_reader: file too small for ZIP"};

    // Find End of Central Directory record.
    // Signature: PK\x05\x06.  Search backward from end (max comment = 65535).
    const uint8_t* eocd = nullptr;
    std::size_t search_start = data.size() > 65557 ? data.size() - 65557 : 0;
    for (std::size_t i = data.size() - 22; i >= search_start; --i) {
      if (data[i] == 0x50 && data[i + 1] == 0x4b && data[i + 2] == 0x05 &&
          data[i + 3] == 0x06) {
        eocd = data.data() + i;
        break;
      }
      if (i == 0) break;
    }
    if (!eocd) throw std::runtime_error{"zip_reader: EOCD not found"};

    uint16_t entry_count = read_u16(eocd + 10);
    uint32_t cd_offset = read_u32(eocd + 16);

    if (cd_offset >= data.size())
      throw std::runtime_error{"zip_reader: invalid central directory offset"};

    // Walk Central Directory entries.
    const uint8_t* p = data.data() + cd_offset;
    const uint8_t* end = data.data() + data.size();

    entries_.reserve(entry_count);
    for (uint16_t i = 0; i < entry_count; ++i) {
      if (p + 46 > end)
        throw std::runtime_error{"zip_reader: truncated central directory"};
      if (read_u32(p) != 0x02014b50)
        throw std::runtime_error{"zip_reader: bad CD signature"};

      uint16_t compression = read_u16(p + 10);
      uint32_t uncomp_size = read_u32(p + 24);
      uint16_t name_len = read_u16(p + 28);
      uint16_t extra_len = read_u16(p + 30);
      uint16_t comment_len = read_u16(p + 32);
      uint32_t local_offset = read_u32(p + 42);

      if (p + 46 + name_len > end)
        throw std::runtime_error{"zip_reader: truncated CD entry name"};

      std::string name{reinterpret_cast<const char*>(p + 46), name_len};

      if (compression != 0)
        throw std::runtime_error{
            "zip_reader: compressed entry not supported: " + name};

      // Resolve local file header to find actual data offset.
      if (local_offset + 30 > data.size())
        throw std::runtime_error{"zip_reader: invalid local header offset"};
      const uint8_t* lh = data.data() + local_offset;
      if (read_u32(lh) != 0x04034b50)
        throw std::runtime_error{"zip_reader: bad local header signature"};
      uint16_t lh_name_len = read_u16(lh + 26);
      uint16_t lh_extra_len = read_u16(lh + 28);
      std::size_t data_start = local_offset + 30 + lh_name_len + lh_extra_len;

      if (data_start + uncomp_size > data.size())
        throw std::runtime_error{"zip_reader: data extends past EOF: " + name};

      entries_.push_back({std::move(name), data_start, uncomp_size});

      p += 46 + name_len + extra_len + comment_len;
    }
  }

 public:
  explicit zip_reader(std::string_view path) : file_{path} { parse(); }

  zip_reader(zip_reader&&) noexcept = default;
  zip_reader& operator=(zip_reader&&) noexcept = default;

  // Get file contents by exact name.
  std::span<const uint8_t> get(std::string_view name) const {
    for (auto& e : entries_) {
      if (e.name == name) {
        return file_.span().subspan(e.data_offset, e.size);
      }
    }
    throw std::runtime_error{"zip_reader: entry not found: " +
                             std::string{name}};
  }

  // Get file contents by suffix match (e.g. "data.pkl" matches
  // "prefix/data.pkl").
  std::span<const uint8_t> get_by_suffix(std::string_view suffix) const {
    for (auto& e : entries_) {
      if (e.name.size() >= suffix.size() &&
          e.name.compare(e.name.size() - suffix.size(), suffix.size(),
                         suffix) == 0) {
        return file_.span().subspan(e.data_offset, e.size);
      }
    }
    throw std::runtime_error{"zip_reader: no entry with suffix: " +
                             std::string{suffix}};
  }

  bool contains(std::string_view name) const {
    return std::ranges::any_of(entries_,
                               [&](auto& e) { return e.name == name; });
  }

  bool contains_suffix(std::string_view suffix) const {
    return std::ranges::any_of(entries_, [&](auto& e) {
      return e.name.size() >= suffix.size() &&
             e.name.compare(e.name.size() - suffix.size(), suffix.size(),
                            suffix) == 0;
    });
  }

  // Return all entry names.
  std::vector<std::string_view> entries() const {
    std::vector<std::string_view> result;
    result.reserve(entries_.size());
    for (auto& e : entries_) result.push_back(e.name);
    return result;
  }

  // Find the common prefix for all entries (e.g. "s5f-libtorch/").
  std::string_view prefix() const {
    if (entries_.empty()) return {};
    std::string_view first = entries_[0].name;
    auto slash = first.find('/');
    if (slash == std::string_view::npos) return {};
    std::string_view pfx = first.substr(0, slash + 1);
    for (auto& e : entries_) {
      if (!e.name.starts_with(pfx)) return {};
    }
    return pfx;
  }
};

}  // namespace larch
