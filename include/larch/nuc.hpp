#pragma once

#include <cstdint>
#include <compare>
#include <stdexcept>

namespace larch {

class nuc_base {
  std::uint8_t value_;

 public:
  static constexpr std::uint8_t A = 0;
  static constexpr std::uint8_t C = 1;
  static constexpr std::uint8_t G = 2;
  static constexpr std::uint8_t T = 3;

  constexpr nuc_base() : value_{A} {}
  constexpr explicit nuc_base(std::uint8_t v) : value_{v} {}

  static constexpr nuc_base from_char(char c) {
    switch (c) {
      case 'A':
      case 'a':
        return nuc_base{A};
      case 'C':
      case 'c':
        return nuc_base{C};
      case 'G':
      case 'g':
        return nuc_base{G};
      case 'T':
      case 't':
        return nuc_base{T};
      default:
        return nuc_base{A};
    }
  }

  static constexpr nuc_base from_proto(int v) {
    return nuc_base{static_cast<std::uint8_t>(v)};
  }

  constexpr char to_char() const {
    constexpr char decode[] = {'A', 'C', 'G', 'T'};
    return decode[value_];
  }

  constexpr std::uint8_t raw() const { return value_; }

  constexpr bool operator==(nuc_base const& o) const = default;
  constexpr auto operator<=>(nuc_base const& o) const = default;
};

}  // namespace larch
