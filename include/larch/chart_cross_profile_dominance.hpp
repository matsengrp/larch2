#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace larch {

struct cross_profile_dominance_profile {
  std::uint64_t offset = 0;
  std::vector<std::uint8_t> masks;
};
struct cross_profile_dominance_audit_pool {
  std::uint32_t clade = 0;
  std::vector<std::uint64_t> weights;
  std::vector<cross_profile_dominance_profile> profiles;
  std::vector<std::vector<std::uint32_t>> rows;
  std::vector<std::size_t> final_owner;
};

// Query-local unit-edge, scalar-optimum/one-witness optimization. This is not
// a quotient of the source grammar and does not preserve all optimal histories,
// exact production masks, or responses at internal cut/graft/pin interfaces.
struct cross_profile_dominance_options {
  bool enabled = false;
  std::size_t pair_limit = 2000000;
  std::size_t width_limit = 4096;
  // Diagnostic-only optional sink; timed callers leave it empty. A refusal
  // makes the whole pool a conservative no-op before any entry is moved.
  std::function<bool(cross_profile_dominance_audit_pool const&)> audit;
};

struct cross_profile_dominance_stats {
  std::size_t generated_products = 0;
  std::size_t completed_clades = 0;
  std::size_t removed = 0;
  std::size_t pairs = 0;
  std::size_t positions = 0;
  std::size_t roots_excluded = 0;
  std::size_t width_refusals = 0;
  std::size_t compare_censors = 0;
  std::size_t max_before = 0;
  std::size_t max_after = 0;
  std::size_t audit_refusals = 0;
};

namespace chart_cross_profile_detail {
using profile = cross_profile_dominance_profile;

inline std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    throw std::overflow_error("cross-profile offset overflow");
  return a + b;
}

inline __int128 defect(profile const& a, profile const& b,
                       std::vector<std::uint64_t> const& weights,
                       cross_profile_dominance_stats* stats = nullptr) {
  __int128 d = static_cast<__int128>(a.offset) - b.offset;
  for (std::size_t j = 0; j < weights.size(); ++j) {
    if (stats) ++stats->positions;
    // max_t ([t not in S_a] - [t not in S_b]) is one precisely
    // when S_b is not contained in S_a, otherwise zero.
    if ((b.masks[j] & static_cast<std::uint8_t>(~a.masks[j])) != 0)
      d += weights[j];
  }
  return d;
}

// Factored rows are common to every entry and cancel. Every discarded entry
// has an actual final surviving dominator; only whole entries/backpointers are
// moved. No structural decision is selected independently for separate sites.
template<class Entries, class Factor, class Classes>
void apply(Entries& entries, Factor const& fact, Classes const& classes,
           bool root, cross_profile_dominance_options const& options,
           cross_profile_dominance_stats& stats,
           std::uint32_t finite_limit, std::uint32_t clade = 0) {
  ++stats.completed_clades;
  auto const n = entries.size();
  stats.max_before = std::max(stats.max_before, n);
  auto finish_noop = [&] { stats.max_after = std::max(stats.max_after, n); };
  // When disabled there is no profile, key, mask, sorting, or audit creation.
  if (!options.enabled || root || n < 2 || fact.varying_classes.empty()) {
    if (options.enabled && root) ++stats.roots_excluded;
    finish_noop();
    return;
  }
  if (n > options.width_limit) {
    ++stats.width_refusals;
    finish_noop();
    return; // bounded comparison refusal is a conservative no-op
  }
  auto const slots = fact.varying_classes.size();
  std::vector<std::uint64_t> weights(slots);
  for (std::size_t j = 0; j < slots; ++j) {
    weights[j] = classes.weight_by_class.at(fact.varying_classes[j]);
    if (!weights[j]) throw std::runtime_error("cross-profile zero weight");
  }
  std::vector<profile> profiles(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (entries[i].rows.size() != 4 * slots)
      throw std::runtime_error("cross-profile row size mismatch");
    profiles[i].masks.resize(slots);
    for (std::size_t j = 0; j < slots; ++j) {
      auto const* row = entries[i].rows.data() + 4 * j;
      auto const m = *std::min_element(row, row + 4);
      if (m >= finite_limit)
        throw std::runtime_error("cross-profile nonfinite edge response");
      if (m && weights[j] > std::numeric_limits<std::uint64_t>::max() / m)
        throw std::overflow_error("cross-profile weighted offset overflow");
      profiles[i].offset = add(profiles[i].offset, weights[j] * m);
      for (unsigned s = 0; s < 4; ++s)
        if (row[s] == m) profiles[i].masks[j] |= static_cast<std::uint8_t>(1u << s);
    }
  }
  std::vector<std::size_t> order(n), owner(n), live;
  std::iota(order.begin(), order.end(), 0);
  std::iota(owner.begin(), owner.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
    return profiles[a].offset < profiles[b].offset;
  });
  live.reserve(n);
  bool censored = false;
  for (auto b : order) {
    bool dead = false;
    for (std::size_t k = 0; k < live.size();) {
      auto a = live[k];
      if (stats.pairs >= options.pair_limit) { censored = true; break; }
      ++stats.pairs;
      if (defect(profiles[a], profiles[b], weights, &stats) <= 0) {
        owner[b] = a;
        dead = true;
        break;
      }
      if (profiles[a].offset == profiles[b].offset) {
        if (stats.pairs >= options.pair_limit) { censored = true; break; }
        ++stats.pairs;
        if (defect(profiles[b], profiles[a], weights, &stats) <= 0) {
          owner[a] = b;
          live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
          continue;
        }
      }
      ++k;
    }
    if (!dead) live.push_back(b);
  }
  if (censored) ++stats.compare_censors;
  for (std::size_t i = 0; i < n; ++i) {
    auto a = i;
    for (std::size_t hops = 0; owner[a] != a; ++hops) {
      if (hops >= n) throw std::runtime_error("cross-profile dominator cycle");
      a = owner[a];
    }
    owner[i] = a;
    if (a != i && defect(profiles[a], profiles[i], weights) > 0)
      throw std::runtime_error("cross-profile final dominator invalid");
  }
  if (options.audit) {
    cross_profile_dominance_audit_pool pool;
    pool.clade = clade;
    pool.weights = weights;
    pool.profiles = profiles;
    pool.final_owner = owner;
    pool.rows.reserve(n);
    for (auto const& entry : entries)
      pool.rows.emplace_back(entry.rows.begin(), entry.rows.end());
    if (!options.audit(pool)) {
      ++stats.audit_refusals;
      finish_noop();
      return;
    }
  }
  Entries kept;
  kept.reserve(live.size());
  for (std::size_t i = 0; i < n; ++i)
    if (owner[i] == i) kept.push_back(std::move(entries[i]));
  stats.removed += n - kept.size();
  stats.max_after = std::max(stats.max_after, kept.size());
  entries = std::move(kept);
}
} // namespace chart_cross_profile_detail
} // namespace larch
