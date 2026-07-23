#pragma once

#include <larch/nuc.hpp>
#include <larch/edge_mutations.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace larch {

class compact_genome {
  using mutation_map = std::map<mutation_position, nuc_base>;

  // Compact genomes are immutable outside add_parent_edge(). Empty edges and
  // the many value copies made by merge/search can therefore share the same
  // ordered mutation map. The mutator always detaches before publishing a
  // changed value, so readers and copied parents remain immutable.
  std::shared_ptr<mutation_map const> mutations_;
  std::size_t hash_ = 0;
  // A failed edge application historically leaves its prior scalar hash next
  // to the partially changed map. Track that state internally so a later
  // successful empty-edge application recomputes rather than propagating a
  // stale parent hash through the O(1) sharing path.
  bool hash_valid_ = true;

  static mutation_map const& empty_mutations() noexcept {
    static mutation_map const empty;
    return empty;
  }

  [[nodiscard]] mutation_map const& mutation_values() const noexcept {
    return mutations_ ? *mutations_ : empty_mutations();
  }

  static std::size_t compute_hash(mutation_map const& mutations) {
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

  explicit compact_genome(mutation_map mutations)
      : hash_{compute_hash(mutations)} {
    if (!mutations.empty()) {
      mutations_ =
          std::make_shared<mutation_map const>(std::move(mutations));
    }
  }

  void add_parent_edge(edge_mutations const& muts, compact_genome const& parent,
                       std::string_view reference) {
    auto const& current = mutation_values();
    auto const& parent_values = parent.mutation_values();
    if (muts.empty() && current.empty()) {
      // The overwhelmingly common loader case inherits a parent unchanged.
      // Sharing also copies a known-valid hash and allocates nothing. A prior
      // failed mutation application deliberately leaves a stale hash, which
      // the historical successful call repaired by scanning the copied map.
      mutations_ = parent.mutations_;
      hash_ = parent.hash_valid_ ? parent.hash_ : compute_hash(parent_values);
      hash_valid_ = true;
      return;
    }

    auto const inherit_parent_directly = current.empty() && this != &parent;
    mutation_map next =
        inherit_parent_directly ? parent_values : current;
    if (this != &parent && !inherit_parent_directly &&
        mutations_ != parent.mutations_) {
      // Preserve the historical merge behavior for nonempty targets.
      auto mutation_it = next.begin();
      for (auto [pos, base] : parent_values) {
        while (mutation_it != next.end() && mutation_it->first < pos) {
          ++mutation_it;
        }
        if (mutation_it != next.end() && mutation_it->first == pos) {
          mutation_it->second = base;
        } else {
          mutation_it = next.emplace_hint(mutation_it, pos, base);
        }
      }
    }

    // Publish the detached map before validating edge positions. If
    // reference.at() throws below, callers observe the same parent-plus-prior-
    // edge partial state as the historical in-place implementation.
    auto writable = std::make_shared<mutation_map>(std::move(next));
    mutations_ = writable;
    hash_valid_ = false;
    auto& mutation_values = *writable;

    // Apply edge mutations
    auto mutation_it = mutation_values.begin();
    for (auto [pos, nucs] : muts) {
      bool is_ref = (nucs.second == nuc_base::from_char(reference.at(pos - 1)));
      while (mutation_it != mutation_values.end() && mutation_it->first < pos) {
        ++mutation_it;
      }
      if (mutation_it != mutation_values.end() && mutation_it->first == pos) {
        if (is_ref) {
          mutation_it = mutation_values.erase(mutation_it);
        } else {
          mutation_it->second = nucs.second;
        }
      } else {
        if (!is_ref) {
          mutation_it = mutation_values.emplace_hint(mutation_it, pos,
                                                      nucs.second);
        }
      }
    }
    hash_ = compute_hash(mutation_values);
    hash_valid_ = true;
    if (mutation_values.empty()) mutations_.reset();
  }

  static edge_mutations to_edge_mutations(std::string_view reference,
                                          compact_genome const& parent,
                                          compact_genome const& child) {
    edge_mutations result;
    auto const& parent_values = parent.mutation_values();
    auto const& child_values = child.mutation_values();
    auto parent_it = parent_values.begin();
    auto result_hint = result.end();
    for (auto [pos, child_base] : child_values) {
      nuc_base parent_base = nuc_base::from_char(reference.at(pos - 1));
      while (parent_it != parent_values.end() &&
             parent_it->first < pos) {
        ++parent_it;
      }
      if (parent_it != parent_values.end() && parent_it->first == pos) {
        parent_base = parent_it->second;
      }
      if (!(parent_base == child_base)) {
        result_hint =
            result.emplace_hint(result_hint, pos,
                                std::pair{parent_base, child_base});
      }
    }

    auto child_it = child_values.begin();
    for (auto [pos, parent_base] : parent_values) {
      nuc_base child_base = nuc_base::from_char(reference.at(pos - 1));
      while (child_it != child_values.end() && child_it->first < pos) {
        ++child_it;
      }
      if (child_it != child_values.end() && child_it->first == pos) {
        // The child-first pass already published this union position. Keep
        // its historical validation order (including reference.at above)
        // without repeating a logarithmic result-map assignment.
        continue;
      }
      if (!(child_base == parent_base)) {
        result.insert_or_assign(pos, std::pair{parent_base, child_base});
      }
    }
    return result;
  }

  nuc_base get_base(mutation_position pos, std::string_view reference) const {
    auto const& values = mutation_values();
    auto it = values.find(pos);
    if (it != values.end()) {
      return it->second;
    }
    return nuc_base::from_char(reference.at(pos - 1));
  }

  std::string to_string() const {
    std::string result = "<";
    for (auto [pos, base] : mutation_values()) {
      result += std::to_string(pos);
      result += base.to_char();
      result += ",";
    }
    result += ">";
    return result;
  }

  bool empty() const { return mutation_values().empty(); }
  std::size_t hash() const { return hash_; }

  auto begin() const { return mutation_values().begin(); }
  auto end() const { return mutation_values().end(); }

  bool operator==(compact_genome const& rhs) const {
    if (hash_ != rhs.hash_) return false;
    if (mutations_ == rhs.mutations_) return true;
    return mutation_values() == rhs.mutation_values();
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
