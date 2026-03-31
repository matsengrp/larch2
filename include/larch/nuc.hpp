#pragma once

#include <cstdint>
#include <compare>
#include <stdexcept>

namespace larch {

class nuc_base {
  std::uint8_t value_;

 public:
  static constexpr std::uint8_t A = 1;
  static constexpr std::uint8_t C = 2;
  static constexpr std::uint8_t G = 4;
  static constexpr std::uint8_t T = 8;

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
      case 'R':
      case 'r':
        return nuc_base{static_cast<std::uint8_t>(A | G)};
      case 'Y':
      case 'y':
        return nuc_base{static_cast<std::uint8_t>(C | T)};
      case 'S':
      case 's':
        return nuc_base{static_cast<std::uint8_t>(G | C)};
      case 'W':
      case 'w':
        return nuc_base{static_cast<std::uint8_t>(A | T)};
      case 'K':
      case 'k':
        return nuc_base{static_cast<std::uint8_t>(G | T)};
      case 'M':
      case 'm':
        return nuc_base{static_cast<std::uint8_t>(A | C)};
      case 'B':
      case 'b':
        return nuc_base{static_cast<std::uint8_t>(C | G | T)};
      case 'D':
      case 'd':
        return nuc_base{static_cast<std::uint8_t>(A | G | T)};
      case 'H':
      case 'h':
        return nuc_base{static_cast<std::uint8_t>(A | C | T)};
      case 'V':
      case 'v':
        return nuc_base{static_cast<std::uint8_t>(A | C | G)};
      case 'N':
      case 'n':
      case '-':
      case '?':
        return nuc_base{static_cast<std::uint8_t>(A | C | G | T)};
      default:
        return nuc_base{A};
    }
  }

  static constexpr nuc_base from_proto(int v) {
    if (v >= 0 && v <= 3) {
      constexpr std::uint8_t remap[] = {A, C, G, T};
      return nuc_base{remap[v]};
    }
    return nuc_base{static_cast<std::uint8_t>(v)};
  }

  constexpr char to_char() const {
    switch (value_) {
      case A:
        return 'A';
      case C:
        return 'C';
      case G:
        return 'G';
      case T:
        return 'T';
      case (A | G):
        return 'R';
      case (C | T):
        return 'Y';
      case (G | C):
        return 'S';
      case (A | T):
        return 'W';
      case (G | T):
        return 'K';
      case (A | C):
        return 'M';
      case (C | G | T):
        return 'B';
      case (A | G | T):
        return 'D';
      case (A | C | T):
        return 'H';
      case (A | C | G):
        return 'V';
      case (A | C | G | T):
        return 'N';
      default:
        return 'N';
    }
  }

  constexpr std::uint8_t raw() const { return value_; }

  constexpr bool operator==(nuc_base const& o) const = default;
  constexpr auto operator<=>(nuc_base const& o) const = default;
};

}  // namespace larch
