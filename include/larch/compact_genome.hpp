#pragma once

#include <larch/nuc.hpp>
#include <larch/edge_mutations.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace larch {

class compact_genome {
  std::map<mutation_position, nuc_base> mutations_;
  std::size_t hash_ = 0;

  static std::size_t compute_hash(
      std::map<mutation_position, nuc_base> const& mutations) {
    std::size_t result = 0;
    for (auto [pos, base] : mutations) {
      result ^= std::hash<std::size_t>{}(pos) + 0x9e3779b9 + (result << 6) +
                (result >> 2);
      // Hash the raw stored value rather than to_char(); callers such as the
      // WRIC strict nucleotide audit must be able to construct/test invalid
      // nuc_base values and reject them explicitly without hash-time UB.
      result ^= std::hash<std::size_t>{}(static_cast<std::size_t>(base.raw())) +
                0x9e3779b9 + (result << 6) + (result >> 2);
    }
    return result;
  }

 public:
  compact_genome() = default;

  explicit compact_genome(std::map<mutation_position, nuc_base> mutations)
      : mutations_{std::move(mutations)}, hash_{compute_hash(mutations_)} {}

  void add_parent_edge(edge_mutations const& muts, compact_genome const& parent,
                       std::string_view reference) {
    // Start with parent's mutations
    for (auto [pos, base] : parent.mutations_) {
      mutations_.insert_or_assign(pos, base);
    }
    // Apply edge mutations
    for (auto [pos, nucs] : muts) {
      bool is_ref = (nucs.second == nuc_base::from_char(reference.at(pos - 1)));
      auto it = mutations_.find(pos);
      if (it != mutations_.end()) {
        if (is_ref) {
          mutations_.erase(it);
        } else {
          it->second = nucs.second;
        }
      } else {
        if (!is_ref) {
          mutations_.insert_or_assign(pos, nucs.second);
        }
      }
    }
    hash_ = compute_hash(mutations_);
  }

  static edge_mutations to_edge_mutations(std::string_view reference,
                                          compact_genome const& parent,
                                          compact_genome const& child) {
    edge_mutations result;
    for (auto [pos, child_base] : child.mutations_) {
      nuc_base parent_base = nuc_base::from_char(reference.at(pos - 1));
      auto it = parent.mutations_.find(pos);
      if (it != parent.mutations_.end()) {
        parent_base = it->second;
      }
      if (!(parent_base == child_base)) {
        result[pos] = {parent_base, child_base};
      }
    }
    for (auto [pos, parent_base] : parent.mutations_) {
      nuc_base child_base = nuc_base::from_char(reference.at(pos - 1));
      auto it = child.mutations_.find(pos);
      if (it != child.mutations_.end()) {
        child_base = it->second;
      }
      if (!(child_base == parent_base)) {
        result[pos] = {parent_base, child_base};
      }
    }
    return result;
  }

  nuc_base get_base(mutation_position pos, std::string_view reference) const {
    auto it = mutations_.find(pos);
    if (it != mutations_.end()) {
      return it->second;
    }
    return nuc_base::from_char(reference.at(pos - 1));
  }

  std::string to_string() const {
    std::string result = "<";
    for (auto [pos, base] : mutations_) {
      result += std::to_string(pos);
      result += base.to_char();
      result += ",";
    }
    result += ">";
    return result;
  }

  bool empty() const { return mutations_.empty(); }
  std::size_t hash() const { return hash_; }

  auto begin() const { return mutations_.begin(); }
  auto end() const { return mutations_.end(); }

  bool operator==(compact_genome const& rhs) const {
    if (hash_ != rhs.hash_) return false;
    return mutations_ == rhs.mutations_;
  }
};

}  // namespace larch

template <>
struct std::hash<larch::compact_genome> {
  std::size_t operator()(larch::compact_genome const& cg) const noexcept {
    return cg.hash();
  }
};

template <>
struct std::equal_to<larch::compact_genome> {
  bool operator()(larch::compact_genome const& a,
                  larch::compact_genome const& b) const noexcept {
    return a == b;
  }
};
