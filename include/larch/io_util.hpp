#pragma once

#include <cstddef>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace larch {

inline bool is_gzipped(std::string_view path) {
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) return false;
  unsigned char h[2]{};
  in.read(reinterpret_cast<char*>(h), 2);
  return h[0] == 0x1f && h[1] == 0x8b;
}

// RAII wrapper for memory-mapped read-only files.
class mmap_file {
  void* addr_ = MAP_FAILED;
  std::size_t size_ = 0;
  int fd_ = -1;

 public:
  explicit mmap_file(std::string_view path) {
    fd_ = ::open(std::string{path}.c_str(), O_RDONLY);
    if (fd_ < 0) throw std::runtime_error{"cannot open " + std::string{path}};
    struct stat st{};
    if (::fstat(fd_, &st) < 0) {
      ::close(fd_);
      throw std::runtime_error{"fstat failed: " + std::string{path}};
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
      // Empty file: nothing to map
      ::close(fd_);
      fd_ = -1;
      addr_ = nullptr;
      return;
    }
    addr_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (addr_ == MAP_FAILED) {
      ::close(fd_);
      throw std::runtime_error{"mmap failed: " + std::string{path}};
    }
    ::madvise(addr_, size_, MADV_SEQUENTIAL);
  }

  mmap_file(mmap_file const&) = delete;
  mmap_file& operator=(mmap_file const&) = delete;
  mmap_file(mmap_file&& o) noexcept
      : addr_{o.addr_}, size_{o.size_}, fd_{o.fd_} {
    o.addr_ = MAP_FAILED;
    o.size_ = 0;
    o.fd_ = -1;
  }
  mmap_file& operator=(mmap_file&& o) noexcept {
    if (this != &o) {
      if (addr_ != MAP_FAILED && addr_ != nullptr) ::munmap(addr_, size_);
      if (fd_ >= 0) ::close(fd_);
      addr_ = o.addr_;
      size_ = o.size_;
      fd_ = o.fd_;
      o.addr_ = MAP_FAILED;
      o.size_ = 0;
      o.fd_ = -1;
    }
    return *this;
  }

  ~mmap_file() {
    if (addr_ != MAP_FAILED && addr_ != nullptr) ::munmap(addr_, size_);
    if (fd_ >= 0) ::close(fd_);
  }

  std::span<const uint8_t> span() const {
    if (size_ == 0) return {};
    return {static_cast<const uint8_t*>(addr_), size_};
  }
};

inline std::vector<char> read_file(std::string_view path) {
  if (is_gzipped(path)) {
    gzFile gz = gzopen(std::string{path}.c_str(), "rb");
    if (!gz) throw std::runtime_error{"cannot open " + std::string{path}};
    std::vector<char> result;
    char buf[262144];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
      result.insert(result.end(), buf, buf + n);
    }
    gzclose(gz);
    return result;
  }
  std::ifstream in{std::string{path}, std::ios::binary};
  if (!in) throw std::runtime_error{"cannot open " + std::string{path}};
  return {std::istreambuf_iterator<char>{in}, {}};
}

}  // namespace larch
