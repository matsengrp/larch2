#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace larch {

class bigint {
  std::vector<uint64_t>
      limbs_;  // little-endian (limbs_[0] = least significant)
  bool negative_ = false;

  void normalize() {
    while (limbs_.size() > 1 && limbs_.back() == 0) limbs_.pop_back();
    if (limbs_.size() == 1 && limbs_[0] == 0) negative_ = false;
  }

  // Compare magnitudes: -1 if |a| < |b|, 0 if equal, 1 if |a| > |b|
  static int compare_mag(std::vector<uint64_t> const& a,
                         std::vector<uint64_t> const& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = a.size(); i-- > 0;) {
      if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
  }

  // Add magnitudes: result = a + b
  static std::vector<uint64_t> add_mag(std::vector<uint64_t> const& a,
                                       std::vector<uint64_t> const& b) {
    std::size_t n = std::max(a.size(), b.size());
    std::vector<uint64_t> r(n + 1, 0);
    uint64_t carry = 0;
    for (std::size_t i = 0; i < n; ++i) {
      __uint128_t s = carry;
      if (i < a.size()) s += a[i];
      if (i < b.size()) s += b[i];
      r[i] = static_cast<uint64_t>(s);
      carry = static_cast<uint64_t>(s >> 64);
    }
    r[n] = carry;
    return r;
  }

  // Subtract magnitudes: result = a - b, assumes |a| >= |b|
  static std::vector<uint64_t> sub_mag(std::vector<uint64_t> const& a,
                                       std::vector<uint64_t> const& b) {
    std::vector<uint64_t> r(a.size(), 0);
    uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      uint64_t ai = a[i];
      uint64_t bi = i < b.size() ? b[i] : 0;
      uint64_t diff = ai - bi - borrow;
      borrow = (ai < bi + borrow || (borrow && bi == UINT64_MAX)) ? 1 : 0;
      r[i] = diff;
    }
    return r;
  }

  // Multiply magnitudes: schoolbook O(n*m)
  static std::vector<uint64_t> mul_mag(std::vector<uint64_t> const& a,
                                       std::vector<uint64_t> const& b) {
    if (a.size() == 1 && a[0] == 0) return {0};
    if (b.size() == 1 && b[0] == 0) return {0};
    std::vector<uint64_t> r(a.size() + b.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
      uint64_t carry = 0;
      for (std::size_t j = 0; j < b.size(); ++j) {
        __uint128_t prod =
            static_cast<__uint128_t>(a[i]) * b[j] + r[i + j] + carry;
        r[i + j] = static_cast<uint64_t>(prod);
        carry = static_cast<uint64_t>(prod >> 64);
      }
      r[i + b.size()] += carry;
    }
    return r;
  }

  // Division by single limb, returns quotient and remainder
  static std::pair<std::vector<uint64_t>, uint64_t> divmod_single(
      std::vector<uint64_t> const& a, uint64_t d) {
    std::vector<uint64_t> q(a.size(), 0);
    __uint128_t rem = 0;
    for (std::size_t i = a.size(); i-- > 0;) {
      __uint128_t cur = (rem << 64) | a[i];
      q[i] = static_cast<uint64_t>(cur / d);
      rem = cur % d;
    }
    return {q, static_cast<uint64_t>(rem)};
  }

  // General division (Knuth Algorithm D simplified)
  // Returns {quotient, remainder}
  static std::pair<bigint, bigint> divmod(bigint const& num,
                                          bigint const& den) {
    int cmp = compare_mag(num.limbs_, den.limbs_);
    if (cmp < 0) return {bigint{0}, num};
    if (den.limbs_.size() == 1) {
      auto [q, r] = divmod_single(num.limbs_, den.limbs_[0]);
      bigint qb;
      qb.limbs_ = std::move(q);
      qb.normalize();
      bigint rb;
      rb.limbs_ = {r};
      return {qb, rb};
    }
    // Multi-limb division: use repeated subtraction with shifting
    // This is a simple but correct approach
    bigint remainder = num;
    remainder.negative_ = false;
    bigint divisor = den;
    divisor.negative_ = false;

    // Find how many limbs to shift
    bigint quotient{0};
    while (compare_mag(remainder.limbs_, divisor.limbs_) >= 0) {
      // Estimate quotient digit using top limbs
      std::size_t shift = remainder.limbs_.size() - divisor.limbs_.size();
      bigint shifted = divisor;
      if (shift > 0) {
        shifted.limbs_.insert(shifted.limbs_.begin(), shift, 0);
      }
      if (compare_mag(remainder.limbs_, shifted.limbs_) < 0) {
        if (shift == 0) break;
        --shift;
        shifted = divisor;
        if (shift > 0) shifted.limbs_.insert(shifted.limbs_.begin(), shift, 0);
      }

      // Estimate how many times shifted divisor fits
      __uint128_t rtop = remainder.limbs_.back();
      if (remainder.limbs_.size() > shifted.limbs_.size())
        rtop = (rtop << 64) | remainder.limbs_[remainder.limbs_.size() - 2];
      uint64_t dtop = shifted.limbs_.back();
      uint64_t est =
          static_cast<uint64_t>(std::min(rtop / dtop, __uint128_t{UINT64_MAX}));
      if (est == 0) est = 1;

      bigint sub = shifted * bigint{est};
      while (compare_mag(sub.limbs_, remainder.limbs_) > 0) {
        --est;
        sub = shifted * bigint{est};
      }

      remainder.limbs_ = sub_mag(remainder.limbs_, sub.limbs_);
      remainder.normalize();

      bigint q_part{est};
      if (shift > 0) q_part.limbs_.insert(q_part.limbs_.begin(), shift, 0);
      quotient = quotient + q_part;
    }
    quotient.normalize();
    remainder.normalize();
    return {quotient, remainder};
  }

 public:
  bigint() : limbs_{0} {}

  template <std::unsigned_integral U>
  bigint(U v) : limbs_{static_cast<uint64_t>(v)} {}

  template <std::signed_integral S>
  bigint(S v) {
    if (v < 0) {
      negative_ = true;
      limbs_ = {static_cast<uint64_t>(-static_cast<int64_t>(v))};
    } else {
      limbs_ = {static_cast<uint64_t>(v)};
    }
  }

  // Comparison
  friend bool operator==(bigint const& a, bigint const& b) {
    return a.negative_ == b.negative_ && a.limbs_ == b.limbs_;
  }

  friend bool operator!=(bigint const& a, bigint const& b) { return !(a == b); }

  friend bool operator>(bigint const& a, int v) {
    bigint bv(v);
    if (a.negative_ != bv.negative_) return !a.negative_;
    int cmp = compare_mag(a.limbs_, bv.limbs_);
    return a.negative_ ? cmp < 0 : cmp > 0;
  }

  friend bool operator<(bigint const& a, bigint const& b) {
    if (a.negative_ != b.negative_) return a.negative_;
    int cmp = compare_mag(a.limbs_, b.limbs_);
    return a.negative_ ? cmp > 0 : cmp < 0;
  }

  friend bool operator>(bigint const& a, bigint const& b) { return b < a; }

  // Addition
  friend bigint operator+(bigint const& a, bigint const& b) {
    bigint r;
    if (a.negative_ == b.negative_) {
      r.limbs_ = add_mag(a.limbs_, b.limbs_);
      r.negative_ = a.negative_;
    } else {
      int cmp = compare_mag(a.limbs_, b.limbs_);
      if (cmp == 0) return bigint{0};
      if (cmp > 0) {
        r.limbs_ = sub_mag(a.limbs_, b.limbs_);
        r.negative_ = a.negative_;
      } else {
        r.limbs_ = sub_mag(b.limbs_, a.limbs_);
        r.negative_ = b.negative_;
      }
    }
    r.normalize();
    return r;
  }

  bigint& operator+=(bigint const& other) {
    *this = *this + other;
    return *this;
  }

  // Subtraction
  friend bigint operator-(bigint const& a, bigint const& b) {
    bigint neg_b = b;
    neg_b.negative_ = !neg_b.negative_;
    if (neg_b.is_zero()) neg_b.negative_ = false;
    return a + neg_b;
  }

  bigint& operator-=(bigint const& other) {
    *this = *this - other;
    return *this;
  }

  // Prefix increment
  bigint& operator++() {
    *this += bigint{1};
    return *this;
  }

  // Postfix increment
  bigint operator++(int) {
    bigint tmp = *this;
    ++(*this);
    return tmp;
  }

  // Multiplication
  friend bigint operator*(bigint const& a, bigint const& b) {
    bigint r;
    r.limbs_ = mul_mag(a.limbs_, b.limbs_);
    r.negative_ = a.negative_ != b.negative_;
    r.normalize();
    return r;
  }

  bigint& operator*=(bigint const& other) {
    *this = *this * other;
    return *this;
  }

  // Division
  friend bigint operator/(bigint const& a, bigint const& b) {
    auto [q, r] = divmod(a, b);
    q.negative_ = (a.negative_ != b.negative_) && !(q == bigint{0});
    return q;
  }

  // Convert to double (take top ~53 significant bits)
  double to_double() const {
    if (limbs_.size() == 1)
      return negative_ ? -static_cast<double>(limbs_[0])
                       : static_cast<double>(limbs_[0]);

    // Find highest non-zero limb
    std::size_t top = limbs_.size() - 1;
    while (top > 0 && limbs_[top] == 0) --top;

    double result = static_cast<double>(limbs_[top]);
    if (top > 0)
      result = std::ldexp(result, 64) + static_cast<double>(limbs_[top - 1]);
    if (top > 1)
      result = std::ldexp(result, static_cast<int>((top - 1) * 64));
    else if (top > 0)
      result = std::ldexp(static_cast<double>(limbs_[top]), 64) +
               static_cast<double>(limbs_[top - 1]);

    return negative_ ? -result : result;
  }

  explicit operator double() const { return to_double(); }

  bool is_zero() const { return limbs_.size() == 1 && limbs_[0] == 0; }

  // Stream output: decimal
  friend std::ostream& operator<<(std::ostream& os, bigint const& v) {
    if (v.is_zero()) return os << '0';
    if (v.negative_) os << '-';

    // Repeated division by 10^18
    constexpr uint64_t base = 1000000000000000000ULL;  // 10^18
    std::vector<std::string> groups;
    std::vector<uint64_t> tmp = v.limbs_;

    auto is_zero_vec = [](std::vector<uint64_t> const& v) {
      for (auto x : v)
        if (x != 0) return false;
      return true;
    };

    while (!is_zero_vec(tmp)) {
      auto [q, r] = divmod_single(tmp, base);
      // Strip leading zeros from quotient
      while (q.size() > 1 && q.back() == 0) q.pop_back();
      groups.push_back(std::to_string(r));
      tmp = std::move(q);
    }

    if (groups.empty()) return os << '0';

    // Last group has no leading zeros; others are padded to 18 digits
    os << groups.back();
    for (std::size_t i = groups.size() - 1; i-- > 0;) {
      auto& g = groups[i];
      for (std::size_t pad = g.size(); pad < 18; ++pad) os << '0';
      os << g;
    }
    return os;
  }

  std::string to_string() const {
    std::ostringstream oss;
    oss << *this;
    return oss.str();
  }
};

}  // namespace larch
