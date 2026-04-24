#pragma once

#include <larch/nuc.hpp>
#include <larch/edge_mutations.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace larch {

inline bool is_unambiguous_nuc_char(char c) {
  switch (c) {
    case 'A':
    case 'a':
    case 'C':
    case 'c':
    case 'G':
    case 'g':
    case 'T':
    case 't':
      return true;
    default:
      return false;
  }
}

inline bool is_iupac_ambiguity_char(char c) {
  switch (c) {
    case 'N':
    case 'n':
    case 'R':
    case 'r':
    case 'Y':
    case 'y':
    case 'S':
    case 's':
    case 'W':
    case 'w':
    case 'K':
    case 'k':
    case 'M':
    case 'm':
    case 'B':
    case 'b':
    case 'D':
    case 'd':
    case 'H':
    case 'h':
    case 'V':
    case 'v':
    case '-':
    case '?':
      return true;
    default:
      return false;
  }
}

inline char normalized_ambiguity_char(char c) {
  if (c == '-') return '-';
  if (c == '?') return '?';
  return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

struct ambiguity_counts {
  std::array<std::size_t, 256> by_char{};
  std::size_t total = 0;
  std::size_t observed_sites = 0;

  void add(char c) {
    auto uc = static_cast<unsigned char>(normalized_ambiguity_char(c));
    by_char[uc]++;
    total++;
  }
};

inline void warn_if_ambiguities(ambiguity_counts const& counts,
                                std::string_view label) {
  if (counts.total == 0) return;

  std::cerr << "warning: " << label << " contains ";
  bool first = true;
  auto emit = [&](char c) {
    auto n = counts.by_char[static_cast<unsigned char>(c)];
    if (n == 0) return;
    if (!first) std::cerr << ", ";
    first = false;
    std::cerr << c << "=" << n;
  };
  for (char c : std::string_view{"NRYWSKMBDHV-?"}) emit(c);

  double pct = counts.observed_sites > 0
                   ? (100.0 * static_cast<double>(counts.total) /
                      static_cast<double>(counts.observed_sites))
                   : 0.0;
  std::cerr << " ambiguity/no-call codes (" << pct
            << "% of observed sites). These will be treated as no-calls "
               "during compact-genome construction; no edge mutations are "
               "emitted at those sites and Fitch parsimony treats them as "
               "compatible with any nucleotide.\n";
}

class compact_genome {
  std::map<mutation_position, nuc_base> mutations_;
  std::set<mutation_position> ambiguity_mask_;
  std::size_t hash_ = 0;

  static void hash_combine(std::size_t& result, std::size_t value) {
    result ^= std::hash<std::size_t>{}(value) + 0x9e3779b9 + (result << 6) +
              (result >> 2);
  }

  static std::size_t compute_hash(
      std::map<mutation_position, nuc_base> const& mutations,
      std::set<mutation_position> const& ambiguity_mask) {
    std::size_t result = 0;
    for (auto [pos, base] : mutations) {
      hash_combine(result, pos);
      hash_combine(result, static_cast<std::size_t>(base.to_char()));
    }

    // Preserve the exact historical hash for ACGT-only compact genomes.
    if (!ambiguity_mask.empty()) {
      hash_combine(result, static_cast<std::size_t>('?'));
      for (auto pos : ambiguity_mask) hash_combine(result, pos);
    }
    return result;
  }

  void recompute_hash() { hash_ = compute_hash(mutations_, ambiguity_mask_); }

 public:
  compact_genome() = default;

  explicit compact_genome(std::map<mutation_position, nuc_base> mutations)
      : mutations_{std::move(mutations)},
        hash_{compute_hash(mutations_, ambiguity_mask_)} {}

  compact_genome(std::map<mutation_position, nuc_base> mutations,
                 std::set<mutation_position> ambiguity_mask)
      : mutations_{std::move(mutations)},
        ambiguity_mask_{std::move(ambiguity_mask)},
        hash_{compute_hash(mutations_, ambiguity_mask_)} {
    for (auto pos : ambiguity_mask_) mutations_.erase(pos);
    recompute_hash();
  }

  void add_parent_edge(edge_mutations const& muts, compact_genome const& parent,
                       std::string_view reference) {
    // Start with parent's concrete mutations. Edge mutations are fully
    // disambiguated; ambiguity masks are leaf-local no-calls and do not
    // propagate through internal nodes.
    ambiguity_mask_.clear();
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
    recompute_hash();
  }

  static edge_mutations to_edge_mutations(std::string_view reference,
                                          compact_genome const& parent,
                                          compact_genome const& child) {
    edge_mutations result;
    for (auto [pos, child_base] : child.mutations_) {
      if (parent.is_ambiguous(pos) || child.is_ambiguous(pos)) continue;
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
      if (parent.is_ambiguous(pos) || child.is_ambiguous(pos)) continue;
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

  bool is_ambiguous(mutation_position pos) const {
    return ambiguity_mask_.contains(pos);
  }

  std::set<mutation_position> const& ambiguity_mask() const {
    return ambiguity_mask_;
  }

  void set_ambiguity_mask(std::set<mutation_position> ambiguity_mask) {
    ambiguity_mask_ = std::move(ambiguity_mask);
    for (auto pos : ambiguity_mask_) mutations_.erase(pos);
    recompute_hash();
  }

  void add_ambiguous_site(mutation_position pos) {
    mutations_.erase(pos);
    ambiguity_mask_.insert(pos);
    recompute_hash();
  }

  std::string to_string() const {
    std::string result = "<";
    for (auto [pos, base] : mutations_) {
      result += std::to_string(pos);
      result += base.to_char();
      result += ",";
    }
    for (auto pos : ambiguity_mask_) {
      result += std::to_string(pos);
      result += "?,";
    }
    result += ">";
    return result;
  }

  bool empty() const { return mutations_.empty() && ambiguity_mask_.empty(); }
  std::size_t hash() const { return hash_; }

  auto begin() const { return mutations_.begin(); }
  auto end() const { return mutations_.end(); }

  bool operator==(compact_genome const& rhs) const {
    if (hash_ != rhs.hash_) return false;
    return mutations_ == rhs.mutations_ &&
           ambiguity_mask_ == rhs.ambiguity_mask_;
  }
};

inline compact_genome compact_genome_from_sequence(
    std::string_view sequence, std::string_view reference,
    ambiguity_counts* counts = nullptr) {
  std::map<mutation_position, nuc_base> muts;
  std::set<mutation_position> ambiguity_mask;
  auto n = std::min(sequence.size(), reference.size());
  if (counts) counts->observed_sites += n;

  for (std::size_t i = 0; i < n; ++i) {
    char c = sequence[i];
    if (is_iupac_ambiguity_char(c)) {
      ambiguity_mask.insert(static_cast<mutation_position>(i + 1));
      if (counts) counts->add(c);
      continue;
    }

    auto ref_base = nuc_base::from_char(reference[i]);
    auto seq_base = nuc_base::from_char(c);
    if (!(ref_base == seq_base)) muts[i + 1] = seq_base;
  }
  return compact_genome{std::move(muts), std::move(ambiguity_mask)};
}

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
