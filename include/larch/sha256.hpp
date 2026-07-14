#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace larch {

// A small streaming SHA-256 implementation used for canonical semantic
// reports.  It deliberately owns no I/O and has no process-global state, so a
// report can feed the same bytes to this digest and (optionally) an output
// stream.  The implementation follows FIPS 180-4 and is tested against the
// published empty/"abc"/long-message vectors.
class sha256 {
 public:
  using digest_type = std::array<std::uint8_t, 32>;

  sha256() { reset(); }

  void reset() noexcept {
    state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    buffered_ = 0;
    total_bytes_ = 0;
  }

  void update(std::span<std::byte const> bytes) {
    if (bytes.size() > max_message_bytes - total_bytes_) {
      throw std::overflow_error("SHA-256 message length exceeds 64-bit bit length");
    }
    total_bytes_ += bytes.size();

    std::size_t offset = 0;
    if (buffered_ != 0) {
      auto const copy_count =
          std::min(block_size - buffered_, bytes.size());
      for (std::size_t i = 0; i < copy_count; ++i) {
        buffer_[buffered_ + i] = std::to_integer<std::uint8_t>(bytes[i]);
      }
      buffered_ += copy_count;
      offset += copy_count;
      if (buffered_ == block_size) {
        transform(buffer_);
        buffered_ = 0;
      }
    }

    while (bytes.size() - offset >= block_size) {
      std::array<std::uint8_t, block_size> block{};
      for (std::size_t i = 0; i < block_size; ++i) {
        block[i] = std::to_integer<std::uint8_t>(bytes[offset + i]);
      }
      transform(block);
      offset += block_size;
    }

    while (offset < bytes.size()) {
      buffer_[buffered_++] =
          std::to_integer<std::uint8_t>(bytes[offset++]);
    }
  }

  void update(std::span<std::uint8_t const> bytes) {
    update(std::as_bytes(bytes));
  }

  void update(std::string_view text) {
    update(std::as_bytes(std::span{text.data(), text.size()}));
  }

  [[nodiscard]] digest_type digest() const {
    auto finalized = *this;
    return finalized.finalize_in_place();
  }

  [[nodiscard]] std::string hex_digest() const {
    return to_hex(digest());
  }

  [[nodiscard]] static std::string to_hex(digest_type const& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
      result[2 * i] = hex[digest[i] >> 4];
      result[2 * i + 1] = hex[digest[i] & 0x0fU];
    }
    return result;
  }

 private:
  static constexpr std::size_t block_size = 64;
  static constexpr std::uint64_t max_message_bytes =
      (std::numeric_limits<std::uint64_t>::max)() / 8U;

  inline static constexpr std::array<std::uint32_t, 64> round_constants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  static constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                                        std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
  }

  static constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                                          std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
  }

  static constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
    return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
  }

  static constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
    return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
  }

  static constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
    return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
  }

  static constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
    return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
  }

  void transform(std::array<std::uint8_t, block_size> const& block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
      auto const j = i * 4;
      words[i] = (static_cast<std::uint32_t>(block[j]) << 24) |
                 (static_cast<std::uint32_t>(block[j + 1]) << 16) |
                 (static_cast<std::uint32_t>(block[j + 2]) << 8) |
                 static_cast<std::uint32_t>(block[j + 3]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
      words[i] = small_sigma1(words[i - 2]) + words[i - 7] +
                 small_sigma0(words[i - 15]) + words[i - 16];
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];

    for (std::size_t i = 0; i < words.size(); ++i) {
      auto const temp1 = h + big_sigma1(e) + choose(e, f, g) +
                         round_constants[i] + words[i];
      auto const temp2 = big_sigma0(a) + majority(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  digest_type finalize_in_place() {
    auto const bit_length = total_bytes_ * 8U;

    buffer_[buffered_++] = 0x80U;
    if (buffered_ > 56) {
      while (buffered_ < block_size) buffer_[buffered_++] = 0;
      transform(buffer_);
      buffered_ = 0;
    }
    while (buffered_ < 56) buffer_[buffered_++] = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
      buffer_[buffered_++] =
          static_cast<std::uint8_t>(bit_length >> shift);
    }
    transform(buffer_);
    buffered_ = 0;

    digest_type result{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
      result[4 * i] = static_cast<std::uint8_t>(state_[i] >> 24);
      result[4 * i + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
      result[4 * i + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
      result[4 * i + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return result;
  }

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, block_size> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

}  // namespace larch
