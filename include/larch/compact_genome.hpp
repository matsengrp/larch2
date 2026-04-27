#pragma once

#include <larch/nuc.hpp>
#include <larch/edge_mutations.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <string_view>

namespace larch {

using iupac_state_set_t = uint8_t;
using ambiguity_set_map = std::map<mutation_position, iupac_state_set_t>;

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

inline iupac_state_set_t iupac_state_set(char c) {
  switch (c) {
    case 'A':
    case 'a':
      return 0b0001;
    case 'C':
    case 'c':
      return 0b0010;
    case 'G':
    case 'g':
      return 0b0100;
    case 'T':
    case 't':
      return 0b1000;
    case 'R':
    case 'r':
      return 0b0101;  // A or G
    case 'Y':
    case 'y':
      return 0b1010;  // C or T
    case 'S':
    case 's':
      return 0b0110;  // C or G
    case 'W':
    case 'w':
      return 0b1001;  // A or T
    case 'K':
    case 'k':
      return 0b1100;  // G or T
    case 'M':
    case 'm':
      return 0b0011;  // A or C
    case 'B':
    case 'b':
      return 0b1110;  // C or G or T
    case 'D':
    case 'd':
      return 0b1101;  // A or G or T
    case 'H':
    case 'h':
      return 0b1011;  // A or C or T
    case 'V':
    case 'v':
      return 0b0111;  // A or C or G
    case 'N':
    case 'n':
    case '-':
    case '?':
      return 0b1111;
    default:
      return 0;
  }
}

inline bool is_iupac_ambiguity_char(char c) {
  auto s = iupac_state_set(c);
  return s != 0 && std::popcount(s) != 1;
}

inline char iupac_char_from_state_set(iupac_state_set_t state_set) {
  switch (state_set & 0b1111) {
    case 0b0001:
      return 'A';
    case 0b0010:
      return 'C';
    case 0b0100:
      return 'G';
    case 0b1000:
      return 'T';
    case 0b0101:
      return 'R';
    case 0b1010:
      return 'Y';
    case 0b0110:
      return 'S';
    case 0b1001:
      return 'W';
    case 0b1100:
      return 'K';
    case 0b0011:
      return 'M';
    case 0b1110:
      return 'B';
    case 0b1101:
      return 'D';
    case 0b1011:
      return 'H';
    case 0b0111:
      return 'V';
    case 0b1111:
      return 'N';
    default:
      return '?';
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
                                std::string_view label, std::ostream& out) {
  if (counts.total == 0) return;

  out << "warning: " << label << " contains ";
  bool first = true;
  auto emit = [&](char c) {
    auto n = counts.by_char[static_cast<unsigned char>(c)];
    if (n == 0) return;
    if (!first) out << ", ";
    first = false;
    out << c << "=" << n;
  };
  for (char c : std::string_view{"NRYWSKMBDHV-?"}) emit(c);

  double pct = counts.observed_sites > 0
                   ? (100.0 * static_cast<double>(counts.total) /
                      static_cast<double>(counts.observed_sites))
                   : 0.0;
  out << " IUPAC ambiguity/no-call codes (" << pct
      << "% of observed sites). Partial IUPAC codes will be treated as "
         "compatible state sets during Fitch parsimony; N, gap, and ? "
         "are treated as no-calls. No edge mutations are emitted at "
         "ambiguous sites.\n";
}

class compact_genome {
  std::map<mutation_position, nuc_base> mutations_;
  ambiguity_set_map ambiguity_sets_;
  std::size_t hash_ = 0;

  static void hash_combine(std::size_t& result, std::size_t value) {
    result ^= std::hash<std::size_t>{}(value) + 0x9e3779b9 + (result << 6) +
              (result >> 2);
  }

  static std::size_t compute_hash(
      std::map<mutation_position, nuc_base> const& mutations,
      ambiguity_set_map const& ambiguity_sets) {
    std::size_t result = 0;
    for (auto [pos, base] : mutations) {
      hash_combine(result, pos);
      hash_combine(result, static_cast<std::size_t>(base.to_char()));
    }

    // Preserve the exact historical hash for ACGT-only compact genomes.
    if (!ambiguity_sets.empty()) {
      hash_combine(result, static_cast<std::size_t>('?'));
      for (auto [pos, state_set] : ambiguity_sets) {
        hash_combine(result, pos);
        hash_combine(result, state_set & 0b1111);
      }
    }
    return result;
  }

  void recompute_hash() { hash_ = compute_hash(mutations_, ambiguity_sets_); }

 public:
  compact_genome() = default;

  explicit compact_genome(std::map<mutation_position, nuc_base> mutations)
      : mutations_{std::move(mutations)},
        hash_{compute_hash(mutations_, ambiguity_sets_)} {}

  compact_genome(std::map<mutation_position, nuc_base> mutations,
                 ambiguity_set_map ambiguity_sets)
      : mutations_{std::move(mutations)},
        ambiguity_sets_{std::move(ambiguity_sets)} {
    for (auto it = ambiguity_sets_.begin(); it != ambiguity_sets_.end();) {
      it->second &= 0b1111;
      if (it->second == 0 || std::popcount(it->second) == 1) {
        it = ambiguity_sets_.erase(it);
      } else {
        mutations_.erase(it->first);
        ++it;
      }
    }
    recompute_hash();
  }

  void add_parent_edge(edge_mutations const& muts, compact_genome const& parent,
                       std::string_view reference) {
    // Start with parent's concrete mutations. Edge mutations are fully
    // disambiguated; ambiguity sets are leaf-local and do not propagate
    // through internal nodes.
    ambiguity_sets_.clear();
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

  iupac_state_set_t get_state_set(mutation_position pos,
                                  std::string_view reference) const {
    auto ait = ambiguity_sets_.find(pos);
    if (ait != ambiguity_sets_.end()) return ait->second;
    return static_cast<uint8_t>(1 << get_base(pos, reference).raw());
  }

  bool is_ambiguous(mutation_position pos) const {
    return ambiguity_sets_.contains(pos);
  }

  ambiguity_set_map const& ambiguity_sets() const { return ambiguity_sets_; }

  std::set<mutation_position> ambiguity_mask() const {
    std::set<mutation_position> result;
    for (auto [pos, _] : ambiguity_sets_) result.insert(pos);
    return result;
  }

  void set_ambiguity_mask(std::set<mutation_position> ambiguity_mask) {
    ambiguity_sets_.clear();
    for (auto pos : ambiguity_mask) ambiguity_sets_[pos] = 0b1111;
    for (auto [pos, _] : ambiguity_sets_) mutations_.erase(pos);
    recompute_hash();
  }

  void set_ambiguity_sets(ambiguity_set_map ambiguity_sets) {
    ambiguity_sets_ = std::move(ambiguity_sets);
    for (auto it = ambiguity_sets_.begin(); it != ambiguity_sets_.end();) {
      it->second &= 0b1111;
      if (it->second == 0 || std::popcount(it->second) == 1) {
        it = ambiguity_sets_.erase(it);
      } else {
        mutations_.erase(it->first);
        ++it;
      }
    }
    recompute_hash();
  }

  void add_ambiguous_site(mutation_position pos,
                          iupac_state_set_t state_set = 0b1111) {
    state_set &= 0b1111;
    if (state_set == 0 || std::popcount(state_set) == 1) return;
    mutations_.erase(pos);
    ambiguity_sets_[pos] = state_set;
    recompute_hash();
  }

  std::string to_string() const {
    std::string result = "<";
    for (auto [pos, base] : mutations_) {
      result += std::to_string(pos);
      result += base.to_char();
      result += ",";
    }
    for (auto [pos, state_set] : ambiguity_sets_) {
      result += std::to_string(pos);
      result += iupac_char_from_state_set(state_set);
      result += ",";
    }
    result += ">";
    return result;
  }

  bool empty() const { return mutations_.empty() && ambiguity_sets_.empty(); }
  std::size_t hash() const { return hash_; }

  auto begin() const { return mutations_.begin(); }
  auto end() const { return mutations_.end(); }

  bool operator==(compact_genome const& rhs) const {
    if (hash_ != rhs.hash_) return false;
    return mutations_ == rhs.mutations_ &&
           ambiguity_sets_ == rhs.ambiguity_sets_;
  }
};

inline compact_genome compact_genome_from_sequence(
    std::string_view sequence, std::string_view reference,
    ambiguity_counts* counts = nullptr) {
  std::map<mutation_position, nuc_base> muts;
  ambiguity_set_map ambiguity_sets;
  auto n = std::min(sequence.size(), reference.size());
  if (counts) counts->observed_sites += n;

  for (std::size_t i = 0; i < n; ++i) {
    char c = sequence[i];
    auto state_set = iupac_state_set(c);
    if (state_set != 0 && std::popcount(state_set) != 1) {
      ambiguity_sets[static_cast<mutation_position>(i + 1)] = state_set;
      if (counts) counts->add(c);
      continue;
    }

    auto ref_base = nuc_base::from_char(reference[i]);
    auto seq_base = nuc_base::from_char(c);
    if (!(ref_base == seq_base)) muts[i + 1] = seq_base;
  }
  return compact_genome{std::move(muts), std::move(ambiguity_sets)};
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
