#include <larch/build_fasta_newick.hpp>
#include <larch/chart_spr_search.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/option_c_chain_commit.hpp>
#include <larch/overlay_chain_compaction.hpp>

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

[[noreturn]] static void test_fail(char const* expr, char const* file, int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expr);
}

#define CHECK(expr) \
  do { \
    if (!(expr)) test_fail(#expr, __FILE__, __LINE__); \
  } while (false)

class phase6_exact_candidate_pair_rendezvous {
 public:
  void arrive_and_wait(std::size_t rank) {
    if (rank >= 2) return;
    std::unique_lock lock{mutex_};
    ++arrivals_;
    cv_.notify_all();
    if (!cv_.wait_for(lock, std::chrono::seconds{5},
                      [&] { return arrivals_ >= 2; })) {
      throw std::runtime_error(
          "phase-6 exact-candidate pair rendezvous timed out");
    }
  }

  [[nodiscard]] std::size_t arrivals() const {
    std::lock_guard lock{mutex_};
    return arrivals_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t arrivals_ = 0;
};

// The exact-candidate runner catches task failures into stable rank-indexed
// slots. Hold every other rank in the hook while rank 1 fails, then keep rank
// 0 parked until the scheduler confirms that rank 1's worker task completed.
// This makes reverse completion order deterministic rather than timing-based.
class phase6_reverse_rank_failure_rendezvous {
 public:
  explicit phase6_reverse_rank_failure_rendezvous(
      larch::chart_scheduler& scheduler)
      : scheduler_(&scheduler) {}

  void fail_in_reverse_completion_order(std::size_t rank) {
    auto const deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    {
      std::unique_lock lock{mutex_};
      if (!completion_floor_) {
        completion_floor_ = scheduler_->metrics().tasks_completed;
      }
      ++invocations_;
      if (rank < seen_rank_.size()) seen_rank_[rank] = true;

      if (rank >= 2) {
        cv_.notify_all();
        if (!cv_.wait_until(lock, deadline,
                            [&] { return release_other_ranks_; })) {
          release_other_ranks_ = true;
          cv_.notify_all();
          throw std::runtime_error("phase-6 reverse-rank rendezvous timed out");
        }
        return;
      }

      seen_failure_rank_[rank] = true;
      cv_.notify_all();
      if (!cv_.wait_until(lock, deadline, [&] {
            return seen_failure_rank_[0] && seen_failure_rank_[1];
          })) {
        release_other_ranks_ = true;
        cv_.notify_all();
        throw std::runtime_error("phase-6 reverse-rank rendezvous timed out");
      }

      if (rank == 1) {
        failure_order_.push_back(1);
        lock.unlock();
        throw std::runtime_error("phase-6 exact candidate rank 1 failure");
      }
    }

    // Ranks >= 2 cannot complete while waiting for release, so the first
    // completed worker task above the hook-entry floor is necessarily rank 1.
    while (scheduler_->metrics().tasks_completed <= *completion_floor_) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::lock_guard lock{mutex_};
        release_other_ranks_ = true;
        cv_.notify_all();
        throw std::runtime_error(
            "phase-6 reverse-rank completion wait timed out");
      }
      std::this_thread::yield();
    }

    {
      std::lock_guard lock{mutex_};
      rank_one_completed_before_rank_zero_failure_ = true;
      failure_order_.push_back(0);
      release_other_ranks_ = true;
      cv_.notify_all();
    }
    throw std::runtime_error("phase-6 exact candidate rank 0 failure");
  }

  [[nodiscard]] std::size_t invocations() const {
    std::lock_guard lock{mutex_};
    return invocations_;
  }

  [[nodiscard]] bool saw_ranks_zero_through_three() const {
    std::lock_guard lock{mutex_};
    return std::ranges::all_of(seen_rank_, [](bool seen) { return seen; });
  }

  [[nodiscard]] bool rank_one_completed_before_rank_zero_failure() const {
    std::lock_guard lock{mutex_};
    return rank_one_completed_before_rank_zero_failure_;
  }

  [[nodiscard]] std::vector<std::size_t> failure_order() const {
    std::lock_guard lock{mutex_};
    return failure_order_;
  }

 private:
  larch::chart_scheduler* scheduler_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<std::uint64_t> completion_floor_;
  std::array<bool, 4> seen_rank_{};
  std::array<bool, 2> seen_failure_rank_{};
  std::size_t invocations_ = 0;
  bool release_other_ranks_ = false;
  bool rank_one_completed_before_rank_zero_failure_ = false;
  std::vector<std::size_t> failure_order_;
};

static larch::test::tiny_tree_node four_taxon_base_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AB", "A", {tiny_leaf("A", "A"), tiny_leaf("B", "A")}),
       tiny_inner("CD", "C", {tiny_leaf("C", "C"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node four_taxon_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_inner("BD", "A", {tiny_leaf("B", "A"), tiny_leaf("D", "C")})});
}

static larch::test::tiny_tree_node four_taxon_two_site_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AC", "AA", {tiny_leaf("A", "AC"), tiny_leaf("C", "CG")}),
       tiny_inner("BD", "AA", {tiny_leaf("B", "AC"), tiny_leaf("D", "CG")})});
}

static larch::test::tiny_tree_node six_taxon_paired_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AD", "A", {tiny_leaf("A", "A"), tiny_leaf("D", "C")}),
       tiny_inner("rest", "A",
                  {tiny_inner("BE", "A", {tiny_leaf("B", "A"),
                                             tiny_leaf("E", "C")}),
                   tiny_inner("CF", "A", {tiny_leaf("C", "A"),
                                             tiny_leaf("F", "C")})})});
}

struct phase7_auto_on_accepted_fixture_spec {
  std::string reference;
  larch::test::tiny_tree_node tree;
};

static phase7_auto_on_accepted_fixture_spec
make_phase7_auto_on_accepted_fixture_spec() {
  constexpr std::size_t distinct_binary_sites = 31;
  constexpr std::size_t signal_repetitions = 96;
  constexpr std::size_t site_count = distinct_binary_sites + signal_repetitions;
  std::array<std::string, 6> sequences;
  for (auto& sequence : sequences) sequence.assign(site_count, 'A');

  // Thirty-one distinct binary columns keep every non-root structural class
  // count at most nine.  The retained signal is then repeated without adding
  // a pattern: A/B/C share A while D/E/F share C, so the crossed input tree
  // has a strict improving SPR but the automatic pilot remains safely in its
  // high-compression branch.
  for (std::size_t pattern = 0; pattern < distinct_binary_sites; ++pattern) {
    constexpr std::array<std::size_t, 5> taxon_by_bit = {0, 3, 1, 4, 2};
    for (std::size_t bit = 0; bit < taxon_by_bit.size(); ++bit) {
      if ((pattern >> bit) & 1U) sequences[taxon_by_bit[bit]][pattern] = 'C';
    }
  }
  for (std::size_t site = distinct_binary_sites; site < site_count; ++site) {
    sequences[3][site] = 'C';
    sequences[4][site] = 'C';
    sequences[5][site] = 'C';
  }

  auto reference = std::string(site_count, 'A');
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto tree = tiny_inner(
      "root", reference,
      {tiny_inner("AD", reference,
                  {tiny_leaf("A", sequences[0]), tiny_leaf("D", sequences[3])}),
       tiny_inner("rest", reference,
                  {tiny_inner("BE", reference,
                              {tiny_leaf("B", sequences[1]),
                               tiny_leaf("E", sequences[4])}),
                   tiny_inner("CF", reference,
                              {tiny_leaf("C", sequences[2]),
                               tiny_leaf("F", sequences[5])})})});
  return {.reference = std::move(reference), .tree = std::move(tree)};
}

static larch::test::tiny_tree_node phase5_arity3_selected_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_leaf("A", "AA"), tiny_leaf("B", "AC"),
       tiny_inner("CD", "CA", {tiny_leaf("C", "CA"),
                                  tiny_leaf("D", "CC")})});
}

static larch::test::tiny_tree_node phase5_arity4_selected_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner("root", "AA",
                    {tiny_leaf("A", "AA"), tiny_leaf("B", "AC"),
                     tiny_leaf("C", "CA"), tiny_leaf("D", "CC")});
}

static larch::test::tiny_tree_node phase6_arity3_misplaced_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"), tiny_leaf("C", "C")}),
       tiny_leaf("B", "A"), tiny_leaf("D", "C")});
}

static larch::test::tiny_tree_node phase6_internal_arity3_source_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABC", "A",
                  {tiny_leaf("A", "A"), tiny_leaf("B", "A"),
                   tiny_leaf("C", "C")}),
       tiny_leaf("D", "C")});
}

struct tiny_chart_spr_fixture {
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
  larch::site_pattern_set patterns;
  std::vector<larch::grammar_spr_candidate> candidates;
};

struct phase4_fixture {
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
};

static phase4_fixture make_three_misplaced_groups_fixture();

static tiny_chart_spr_fixture make_fixture() {
  tiny_chart_spr_fixture fixture;
  fixture.dag = larch::test::make_tiny_labelled_tree("A", four_taxon_base_tree());
  fixture.grammar = larch::build_clade_grammar(fixture.dag);
  fixture.patterns = larch::build_site_patterns(fixture.dag, fixture.grammar);
  fixture.candidates = larch::enumerate_grammar_spr_candidates(fixture.grammar);
  CHECK(!fixture.candidates.empty());
  return fixture;
}

// A deterministic, deliberately wide pattern axis for Phase-4 scheduler
// tests.  site_pattern_set is already the compressed input boundary, so
// repeated state vectors with distinct weights are legitimate independent
// entries here; every generated vector is non-invariant.
static larch::site_pattern_set make_phase4_wide_patterns(
    std::size_t pattern_count = 48) {
  larch::site_pattern_set patterns;
  patterns.taxon_count = 4;
  patterns.patterns.reserve(pattern_count);
  for (std::size_t index = 0; index < pattern_count; ++index) {
    auto a = static_cast<std::uint8_t>(index % larch::nuc_state_count);
    auto b = static_cast<std::uint8_t>((index / larch::nuc_state_count) %
                                       larch::nuc_state_count);
    auto c = static_cast<std::uint8_t>(
        (index / (larch::nuc_state_count * larch::nuc_state_count)) %
        larch::nuc_state_count);
    auto d =
        static_cast<std::uint8_t>((index * 3 + 1) % larch::nuc_state_count);
    if (a == b && b == c && c == d) {
      d = static_cast<std::uint8_t>((d + 1) % larch::nuc_state_count);
    }
    patterns.patterns.push_back(larch::site_pattern{
        .state_by_taxon = {a, b, c, d},
        .weight = static_cast<std::uint32_t>(index % 5 + 1),
    });
  }
  return patterns;
}

static void check_phase4_scheduler_axis_reconciliation(
    larch::chart_scheduler_metrics const& scheduler,
    larch::chart_spr_scheduler_axis_counters const& counters);

static larch::site_pattern_set make_phase7_compressed_patterns(
    std::size_t pattern_count) {
  larch::site_pattern_set patterns;
  patterns.taxon_count = 4;
  patterns.patterns.reserve(pattern_count);
  for (std::size_t index = 0; index < pattern_count; ++index) {
    patterns.patterns.push_back(larch::site_pattern{
        .state_by_taxon = {0, 0, 1, 1},
        .weight = static_cast<std::uint32_t>(index % 3 + 1),
    });
  }
  return patterns;
}

static larch::site_pattern_set make_phase7_dense_favoring_patterns(
    std::size_t pattern_count) {
  larch::site_pattern_set patterns;
  patterns.taxon_count = 4;
  patterns.patterns.reserve(pattern_count);
  for (std::size_t index = 0; index < pattern_count; ++index) {
    auto const pair_code = index / 2;
    auto a = static_cast<std::uint8_t>(pair_code % larch::nuc_state_count);
    auto b = static_cast<std::uint8_t>((pair_code / larch::nuc_state_count) %
                                       larch::nuc_state_count);
    auto c =
        static_cast<std::uint8_t>((index * 3 + 1) % larch::nuc_state_count);
    auto d =
        static_cast<std::uint8_t>((index * 5 + 2) % larch::nuc_state_count);
    if (a == b && b == c && c == d) {
      d = static_cast<std::uint8_t>((d + 1) % larch::nuc_state_count);
    }
    patterns.patterns.push_back(larch::site_pattern{
        .state_by_taxon = {a, b, c, d},
        .weight = 1,
    });
  }
  return patterns;
}

static larch::chart_spr_lazy_policy_diagnostics resolve_phase7_policy(
    larch::clade_grammar const& grammar,
    larch::site_pattern_set const& patterns,
    larch::chart_cache_options const& cache = {}) {
  auto active = larch::make_active_search_patterns(patterns);
  auto plan = larch::build_chart_execution_plan(grammar);
  auto const dense_bytes = larch::estimate_chart_spr_full_pattern_cache_bytes(
      grammar, active.active_patterns);
  return larch::resolve_chart_spr_lazy_policy(plan, active.active_patterns, {},
                                              cache, dense_bytes);
}

static void test_phase7_lazy_auto_policy_contract() {
  std::println("test_phase7_lazy_auto_policy_contract");
  auto fixture = make_fixture();

  auto compressed = make_phase7_compressed_patterns(65);
  larch::chart_cache_options automatic;
  automatic.lazy_policy = larch::chart_spr_lazy_policy::automatic;
  auto expected = resolve_phase7_policy(fixture.grammar, compressed, automatic);
  CHECK(expected.requested == larch::chart_spr_lazy_policy::automatic);
  CHECK(expected.resolved == larch::chart_spr_lazy_policy::on);
  CHECK(expected.reason == larch::chart_spr_lazy_policy_reason::
                               compression_thresholds_and_budget_safe);
  CHECK(expected.active_pattern_count == 65);
  CHECK(expected.pilot_pattern_count == 32);
  CHECK(expected.pilot_inside_chart_builds == 1);
  CHECK(expected.pilot_outside_chart_builds == 0);
  CHECK(expected.pilot_exact_builds == 0);
  CHECK(expected.pilot_scheduler_submissions == 0);
  CHECK(expected.pilot_internal_structural_class_count_max <=
        expected.pilot_pattern_count / 3);
  CHECK(expected.pilot_inside_rows <= expected.pilot_dense_rows / 2);

  std::uint64_t expected_index_hash = 1469598103934665603ULL;
  for (std::size_t stratum = 0; stratum < 32; ++stratum) {
    auto const index =
        larch::chart_spr_search_detail::lazy_policy_stratum_midpoint(65, 32,
                                                                     stratum);
    expected_index_hash =
        larch::chart_spr_search_detail::mix_u64(expected_index_hash, index);
  }
  CHECK(expected.pilot_pattern_index_hash == expected_index_hash);

  // Exercise production state construction with a real persistent scheduler
  // at every required count, not merely the direct resolver.
  auto published_expected = expected;
  published_expected.frozen = true;
  for (auto workers :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    auto active = larch::make_active_search_patterns(compressed);
    larch::chart_scheduler scheduler{larch::chart_scheduler_options{
        .requested_workers = workers,
        .default_minimum_grain = 1,
        .default_target_ranges_per_worker = 4,
    }};
    auto state = larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), {}, false, {},
        automatic, {}, &scheduler);
    CHECK(state.lazy_policy == published_expected);
    CHECK(state.cache_strategy ==
          larch::chart_spr_cache_strategy::lazy_multisite_chart);
    CHECK(state.counters.lazy_policy_pilot_runs == 1);
    CHECK(state.counters.lazy_policy_frozen_reuses == 0);
    CHECK(state.lazy_policy.pilot_pattern_index_hash == expected_index_hash);
    CHECK(state.lazy_policy.pilot_inside_chart_builds == 1);
    CHECK(state.lazy_policy.pilot_outside_chart_builds == 0);
    CHECK(state.lazy_policy.pilot_exact_builds == 0);
    CHECK(state.lazy_policy.pilot_scheduler_submissions == 0);
    // The full production lazy chart does materialize both surfaces.  Its
    // counters are deliberately distinct from the inside-only, unscheduled
    // pilot diagnostics above; scheduler totals may be nonzero on the
    // wavefront implementation.
    CHECK(state.counters.lazy_inside_rows_computed > 0);
    CHECK(state.counters.lazy_outside_rows_computed > 0);
    CHECK(state.counters.lazy_inside_rows_computed >=
          state.lazy_policy.pilot_inside_rows);
    CHECK(state.counters.scheduler_axes.lazy_inside_clades.operations > 0);
    CHECK(state.counters.scheduler_axes.lazy_outside_clades.operations > 0);
    check_phase4_scheduler_axis_reconciliation(scheduler.metrics(),
                                               state.counters.scheduler_axes);
    CHECK(scheduler.worker_resolution().resolved_workers == workers);
    scheduler.shutdown();
  }

  auto small = resolve_phase7_policy(
      fixture.grammar, make_phase7_compressed_patterns(7), automatic);
  CHECK(small.pilot_pattern_count == 7);
  CHECK(small.resolved == larch::chart_spr_lazy_policy::on);

  auto empty = resolve_phase7_policy(
      fixture.grammar, make_phase7_compressed_patterns(0), automatic);
  CHECK(empty.active_pattern_count == 0);
  CHECK(empty.pilot_pattern_count == 0);
  CHECK(empty.pilot_inside_chart_builds == 0);
  CHECK(empty.resolved == larch::chart_spr_lazy_policy::off);
  CHECK(empty.reason ==
        larch::chart_spr_lazy_policy_reason::no_active_patterns);

  auto dense_patterns = make_phase7_dense_favoring_patterns(65);
  auto dense =
      resolve_phase7_policy(fixture.grammar, dense_patterns, automatic);
  CHECK(dense.pilot_pattern_count == 32);
  CHECK(dense.resolved == larch::chart_spr_lazy_policy::off);
  CHECK(dense.reason == larch::chart_spr_lazy_policy_reason::
                            structural_and_strong_row_ratios_exceeded);

  // Default off and both explicit-on spellings never allocate or execute a
  // pilot.  The compatibility rule is unambiguous: legacy true wins over the
  // enum, including an enum value of auto.
  auto explicit_off = resolve_phase7_policy(fixture.grammar, compressed);
  CHECK(explicit_off.requested == larch::chart_spr_lazy_policy::off);
  CHECK(explicit_off.resolved == larch::chart_spr_lazy_policy::off);
  CHECK(explicit_off.reason ==
        larch::chart_spr_lazy_policy_reason::explicit_off);
  CHECK(explicit_off.pilot_pattern_count == 0);
  CHECK(explicit_off.pilot_inside_chart_builds == 0);
  CHECK(explicit_off.measurements_available);
  CHECK(explicit_off.estimated_lazy_cache_bytes > 0);
  CHECK(explicit_off.estimated_dense_cache_bytes > 0);

  larch::chart_cache_options explicit_on;
  explicit_on.lazy_policy = larch::chart_spr_lazy_policy::on;
  auto enum_on =
      resolve_phase7_policy(fixture.grammar, compressed, explicit_on);
  CHECK(enum_on.requested == larch::chart_spr_lazy_policy::on);
  CHECK(enum_on.reason == larch::chart_spr_lazy_policy_reason::explicit_on);
  CHECK(enum_on.pilot_pattern_count == 0);
  CHECK(enum_on.measurements_available);
  CHECK(enum_on.estimated_lazy_cache_bytes > 0);
  CHECK(enum_on.estimated_dense_cache_bytes > 0);

  larch::chart_cache_options legacy_on = automatic;
  legacy_on.use_lazy_multisite_chart = true;
  auto compatibility_on =
      resolve_phase7_policy(fixture.grammar, compressed, legacy_on);
  CHECK(compatibility_on.requested == larch::chart_spr_lazy_policy::on);
  CHECK(compatibility_on.resolved == larch::chart_spr_lazy_policy::on);
  CHECK(compatibility_on.reason ==
        larch::chart_spr_lazy_policy_reason::explicit_on);
  CHECK(compatibility_on.pilot_pattern_count == 0);

  bool unresolved_threw = false;
  try {
    auto active = larch::make_active_search_patterns(compressed);
    (void)larch::choose_chart_spr_cache_strategy(
        fixture.grammar, active.active_patterns, automatic);
  } catch (std::runtime_error const&) {
    unresolved_threw = true;
  }
  CHECK(unresolved_threw);

  // Every public entry point rejects an out-of-range enum value.  The legacy
  // explicit-on alias must not hide a corrupt enum payload.
  auto invalid = automatic;
  invalid.lazy_policy = static_cast<larch::chart_spr_lazy_policy>(255);
  invalid.use_lazy_multisite_chart = true;
  bool invalid_resolver_threw = false;
  try {
    (void)resolve_phase7_policy(fixture.grammar, compressed, invalid);
  } catch (std::invalid_argument const& e) {
    invalid_resolver_threw = true;
    CHECK(std::string{e.what()}.find("invalid lazy policy") !=
          std::string::npos);
  }
  CHECK(invalid_resolver_threw);
  bool invalid_selector_threw = false;
  try {
    auto active = larch::make_active_search_patterns(compressed);
    (void)larch::choose_chart_spr_cache_strategy(
        fixture.grammar, active.active_patterns, invalid);
  } catch (std::invalid_argument const&) {
    invalid_selector_threw = true;
  }
  CHECK(invalid_selector_threw);

  std::println("  PASS");
}

static void test_phase7_lazy_auto_budget_and_freeze_contract() {
  std::println("test_phase7_lazy_auto_budget_and_freeze_contract");
  auto fixture = make_fixture();
  auto compressed = make_phase7_compressed_patterns(256);
  auto dense_patterns = make_phase7_dense_favoring_patterns(64);
  larch::chart_cache_options automatic;
  automatic.lazy_policy = larch::chart_spr_lazy_policy::automatic;
  auto pilot = resolve_phase7_policy(fixture.grammar, compressed, automatic);
  CHECK(pilot.pilot_estimated_allocation_bytes > 0);

  auto make_scheduler = [] {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = 4,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };
  auto make_state = [&](std::size_t budget, larch::chart_scheduler& scheduler) {
    auto cache = automatic;
    cache.memory_budget_bytes = budget;
    auto active = larch::make_active_search_patterns(compressed);
    active.pattern_source_fingerprint =
        larch::build_chart_spr_pattern_source_fingerprint(fixture.dag,
                                                          fixture.grammar);
    return larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), {}, false, {}, cache,
        {}, &scheduler);
  };

  constexpr auto calibration_budget = std::size_t{1} << 40;
  auto calibration_scheduler = make_scheduler();
  auto calibration = make_state(calibration_budget, *calibration_scheduler);
  CHECK(calibration.lazy_policy.resolved == larch::chart_spr_lazy_policy::on);
  CHECK(calibration.counters.lazy_policy_pilot_runs == 1);
  auto const pilot_required =
      larch::estimate_chart_spr_lazy_policy_pilot_required_bytes(
          calibration, calibration.lazy_policy.pilot_estimated_allocation_bytes,
          calibration_scheduler.get());
  auto const lazy_admission =
      larch::estimate_chart_spr_lazy_cache_admission_bytes(
          calibration.grammar.clades.size(),
          calibration.active_patterns.patterns.patterns.size());
  CHECK(lazy_admission >= sizeof(larch::lazy_multisite_chart));
  auto const selected_required =
      larch::estimate_chart_spr_lazy_state_build_required_bytes(
          calibration, lazy_admission - sizeof(larch::lazy_multisite_chart),
          calibration_scheduler.get());
  CHECK(selected_required > pilot_required);
  auto const exact_budget = std::max(pilot_required, selected_required);
  CHECK(exact_budget == selected_required);
  CHECK(exact_budget < pilot_required + selected_required);

  auto fit_scheduler = make_scheduler();
  auto fit = make_state(exact_budget, *fit_scheduler);
  CHECK(fit.lazy_policy.resolved == larch::chart_spr_lazy_policy::on);
  CHECK(fit.counters.lazy_policy_pilot_runs == 1);
  CHECK(fit.counters.lazy_chart_preflight_peak_bytes <= exact_budget);
  CHECK(fit.counters.lazy_chart_actual_peak_bytes <= exact_budget);
  CHECK(fit.counters.scheduler_axes.lazy_inside_clades.operations > 0);
  CHECK(fit.counters.scheduler_axes.lazy_outside_clades.operations > 0);

  auto reject_scheduler = make_scheduler();
  auto const reject_metrics_before = reject_scheduler->metrics();
  bool rejected = false;
  try {
    (void)make_state(exact_budget - 1, *reject_scheduler);
  } catch (std::runtime_error const& error) {
    rejected = true;
    CHECK(std::string{error.what()}.find("estimated live chart state build") !=
          std::string::npos);
  }
  CHECK(rejected);
  CHECK(reject_scheduler->metrics().operations ==
        reject_metrics_before.operations);
  CHECK(reject_scheduler->metrics().tasks_submitted ==
        reject_metrics_before.tasks_submitted);

  // Exercise the actual accepted-state frozen-token path at its strict
  // old-published + new-build boundary.  The frozen decision skips the pilot,
  // but current grammar/pattern capacities and the chosen lazy representation
  // are projected again while the preceding state remains live.
  auto const overlapping_published_state_bytes =
      larch::estimate_chart_spr_published_state_resident_bytes(calibration);
  auto frozen_scheduler = make_scheduler();
  auto const frozen_required =
      larch::estimate_chart_spr_lazy_state_build_required_bytes(
          calibration, lazy_admission - sizeof(larch::lazy_multisite_chart),
          frozen_scheduler.get(), overlapping_published_state_bytes);
  CHECK(frozen_required ==
        selected_required + overlapping_published_state_bytes);
  larch::chart_spr_search_options frozen_options;
  frozen_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  frozen_options.cache = automatic;
  frozen_options.cache.memory_budget_bytes = frozen_required;
  auto frozen = larch::chart_spr_search_detail::
      rebuild_chart_spr_search_state_after_accept_for_tests(
          calibration, fixture.dag, fixture.grammar, frozen_options,
          *frozen_scheduler);
  CHECK(frozen.lazy_policy == calibration.lazy_policy);
  CHECK(frozen.counters.lazy_policy_pilot_runs == 0);
  CHECK(frozen.counters.lazy_policy_frozen_reuses == 1);
  CHECK(frozen.counters.scheduler_axes.lazy_inside_clades.operations > 0);
  CHECK(frozen.counters.scheduler_axes.lazy_outside_clades.operations > 0);
  check_phase4_scheduler_axis_reconciliation(frozen_scheduler->metrics(),
                                             frozen.counters.scheduler_axes);

  auto frozen_reject_scheduler = make_scheduler();
  auto const frozen_reject_metrics_before = frozen_reject_scheduler->metrics();
  frozen_options.cache.memory_budget_bytes = frozen_required - 1;
  bool frozen_rejected = false;
  try {
    (void)larch::chart_spr_search_detail::
        rebuild_chart_spr_search_state_after_accept_for_tests(
            calibration, fixture.dag, fixture.grammar, frozen_options,
            *frozen_reject_scheduler);
  } catch (std::runtime_error const& error) {
    frozen_rejected = true;
    CHECK(std::string{error.what()}.find("estimated live chart state build") !=
          std::string::npos);
  }
  CHECK(frozen_rejected);
  CHECK(frozen_reject_scheduler->metrics().operations ==
        frozen_reject_metrics_before.operations);
  CHECK(frozen_reject_scheduler->metrics().tasks_submitted ==
        frozen_reject_metrics_before.tasks_submitted);
  calibration_scheduler->shutdown();
  fit_scheduler->shutdown();
  reject_scheduler->shutdown();
  frozen_scheduler->shutdown();
  frozen_reject_scheduler->shutdown();

  // Public cache options never carry frozen diagnostics. Reusing them for an
  // unrelated state runs a fresh pilot and cannot publish stale measurements.
  // Accepted-state rebuilds use a private token and are exercised below by the
  // non-vacuous accepted-move search tests.
  auto compressed_active = larch::make_active_search_patterns(compressed);
  auto state = larch::build_chart_spr_search_state_from_active(
      fixture.dag, fixture.grammar, std::move(compressed_active), {}, false, {},
      automatic);
  CHECK(state.lazy_policy.frozen);
  CHECK(state.counters.lazy_policy_pilot_runs == 1);
  CHECK(state.counters.lazy_policy_frozen_reuses == 0);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  auto dense_active = larch::make_active_search_patterns(dense_patterns);
  auto rebuilt = larch::build_chart_spr_search_state_from_active(
      fixture.dag, fixture.grammar, std::move(dense_active), {}, false, {},
      state.cache_opts);
  CHECK(rebuilt.lazy_policy.resolved == larch::chart_spr_lazy_policy::off);
  CHECK(rebuilt.cache_strategy != state.cache_strategy);
  CHECK(rebuilt.counters.lazy_policy_pilot_runs == 1);
  CHECK(rebuilt.counters.lazy_policy_frozen_reuses == 0);

  std::println("  PASS");
}

static std::array<larch::chart_spr_scheduler_axis_metrics const*, 13>
phase4_scheduler_axes(
    larch::chart_spr_scheduler_axis_counters const& counters) {
  return {
      &counters.initial_chart_patterns,
      &counters.candidate_generation,
      &counters.exact_setup_patterns,
      &counters.exact_frontier_clades,
      &counters.exact_candidates,
      &counters.lazy_inside_clades,
      &counters.lazy_outside_clades,
      &counters.inside_cache_patterns,
      &counters.outside_cache_patterns,
      &counters.fixed_topology_patterns,
      &counters.local_score_candidates,
      &counters.local_score_candidate_patterns,
      &counters.other,
  };
}

static void check_phase4_scheduler_axis_reconciliation(
    larch::chart_scheduler_metrics const& scheduler,
    larch::chart_spr_scheduler_axis_counters const& counters) {
  std::uint64_t operations = 0;
  std::uint64_t ranges = 0;
  std::uint64_t tasks = 0;
  std::size_t active_worker_high_water = 0;
  for (auto const* axis : phase4_scheduler_axes(counters)) {
    operations += axis->operations;
    ranges += axis->ranges;
    tasks += axis->worker_tasks;
    active_worker_high_water =
        std::max(active_worker_high_water, axis->active_worker_high_water);
  }
  CHECK(operations == scheduler.operations);
  CHECK(ranges == scheduler.ranges_created);
  CHECK(tasks == scheduler.tasks_submitted);
  CHECK(active_worker_high_water == scheduler.active_worker_high_water);
}

static void check_phase4_scheduler_axis_reconciliation(
    larch::chart_spr_search_result const& search) {
  CHECK(search.summary.scheduler_axes == search.counters.scheduler_axes);
  check_phase4_scheduler_axis_reconciliation(search.summary.scheduler,
                                             search.summary.scheduler_axes);
}

static larch::test::tiny_tree_node four_taxon_offset_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "CCAA",
      {tiny_inner("AB", "CCAA", {tiny_leaf("A", "CCAA"),
                                    tiny_leaf("B", "CCAA")}),
       tiny_inner("CD", "CCGA", {tiny_leaf("C", "CCGA"),
                                    tiny_leaf("D", "CCGA")})});
}

static larch::test::tiny_tree_node four_taxon_repeated_reference_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AA",
      {tiny_inner("AB", "AA", {tiny_leaf("A", "AA"),
                                  tiny_leaf("B", "AA")}),
       tiny_inner("CD", "AC", {tiny_leaf("C", "AA"),
                                  tiny_leaf("D", "CC")})});
}

static larch::test::tiny_tree_node four_taxon_two_pattern_tree() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "AAA",
      {tiny_inner("AB", "AAA", {tiny_leaf("A", "AAA"),
                                   tiny_leaf("B", "AAC")}),
       tiny_inner("CD", "CCC", {tiny_leaf("C", "CCA"),
                                   tiny_leaf("D", "CCC")})});
}

static larch::test::tiny_tree_node five_taxon_multiparent_tree_one() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABC", "A",
                  {tiny_inner("AB", "A", {tiny_leaf("A", "A"),
                                             tiny_leaf("B", "C")}),
                   tiny_leaf("C", "A")}),
       tiny_inner("DE", "A", {tiny_leaf("D", "C"),
                                tiny_leaf("E", "A")})});
}

static larch::test::tiny_tree_node five_taxon_multiparent_tree_two() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABD", "A",
                  {tiny_inner("AB", "A", {tiny_leaf("A", "A"),
                                             tiny_leaf("B", "C")}),
                   tiny_leaf("D", "C")}),
       tiny_inner("CE", "A", {tiny_leaf("C", "A"),
                                tiny_leaf("E", "A")})});
}

static larch::taxon_id taxon_for(larch::clade_grammar const& grammar,
                                 std::string const& sample_id) {
  auto it = grammar.taxa.sample_id_to_id.find(sample_id);
  CHECK(it != grammar.taxa.sample_id_to_id.end());
  return it->second;
}

static std::vector<larch::taxon_id> taxa_for(
    larch::clade_grammar const& grammar, std::vector<std::string> sample_ids) {
  std::vector<larch::taxon_id> ids;
  ids.reserve(sample_ids.size());
  for (auto const& sample_id : sample_ids) ids.push_back(taxon_for(grammar, sample_id));
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

static larch::clade_id clade_for(larch::clade_grammar const& grammar,
                                 std::vector<std::string> sample_ids) {
  auto ids = taxa_for(grammar, std::move(sample_ids));
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    if (grammar.clades[cid].taxa == ids) return static_cast<larch::clade_id>(cid);
  }
  CHECK(false && "missing clade");
  return larch::no_clade;
}

static larch::production_id production_id_for(
    larch::clade_grammar const& grammar, larch::clade_id parent,
    std::vector<larch::clade_id> children) {
  std::sort(children.begin(), children.end());
  for (auto pid : grammar.productions_by_parent[parent]) {
    auto prod_children = grammar.productions[pid].children;
    std::sort(prod_children.begin(), prod_children.end());
    if (prod_children == children) return pid;
  }
  CHECK(false && "missing production");
  return larch::no_production;
}

static larch::chart_spr_production_signature production_signature_for_id(
    larch::clade_grammar const& grammar, larch::production_id pid) {
  auto const& prod = grammar.productions[pid];
  larch::chart_spr_production_signature signature;
  signature.parent_taxa = grammar.clades[prod.parent].taxa;
  std::sort(signature.parent_taxa.begin(), signature.parent_taxa.end());
  signature.child_taxa.reserve(prod.children.size());
  for (auto child : prod.children) {
    auto child_taxa = grammar.clades[child].taxa;
    std::sort(child_taxa.begin(), child_taxa.end());
    signature.child_taxa.push_back(std::move(child_taxa));
  }
  std::sort(signature.child_taxa.begin(), signature.child_taxa.end());
  return signature;
}

static bool grammar_contains_production_signature(
    larch::clade_grammar const& grammar,
    larch::chart_spr_production_signature const& signature) {
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (production_signature_for_id(
            grammar, static_cast<larch::production_id>(pid)) == signature) {
      return true;
    }
  }
  return false;
}

static larch::overlay_grammar_production temp_prod(
    larch::overlay_clade_ref parent,
    std::vector<larch::overlay_clade_ref> children) {
  std::sort(children.begin(), children.end());
  larch::overlay_grammar_production prod;
  prod.parent = parent;
  prod.children = std::move(children);
  prod.multiplicity = 1;
  return prod;
}

static void check_production_witness_equal(
    larch::production_witness const& actual,
    larch::production_witness const& expected) {
  CHECK(actual.parent_node == expected.parent_node);
  CHECK(actual.children.size() == expected.children.size());
  for (std::size_t i = 0; i < actual.children.size(); ++i) {
    CHECK(actual.children[i].child == expected.children[i].child);
    CHECK(actual.children[i].edge_alternatives ==
          expected.children[i].edge_alternatives);
  }
}

static void check_overlay_production_equal(
    larch::overlay_grammar_production const& actual,
    larch::overlay_grammar_production const& expected) {
  CHECK(actual.parent == expected.parent);
  CHECK(actual.children == expected.children);
  CHECK(actual.multiplicity == expected.multiplicity);
  CHECK(actual.witnesses.size() == expected.witnesses.size());
  for (std::size_t i = 0; i < actual.witnesses.size(); ++i) {
    check_production_witness_equal(actual.witnesses[i], expected.witnesses[i]);
  }
}

static void check_spr_overlay_delta_equal(
    larch::spr_overlay_delta const& actual,
    larch::spr_overlay_delta const& expected) {
  CHECK(actual.base == expected.base);
  CHECK(actual.candidate_old_parent == expected.candidate_old_parent);
  CHECK(actual.candidate_new_sibling_or_target ==
        expected.candidate_new_sibling_or_target);
  CHECK(actual.temp_clades == expected.temp_clades);
  CHECK(actual.temp_productions.size() == expected.temp_productions.size());
  for (std::size_t i = 0; i < actual.temp_productions.size(); ++i) {
    check_overlay_production_equal(actual.temp_productions[i],
                                   expected.temp_productions[i]);
  }
  CHECK(actual.removed_base_productions == expected.removed_base_productions);
  CHECK(actual.commit_source == expected.commit_source);
  CHECK(actual.affected_order == expected.affected_order);
  CHECK(actual.affected_base_clade == expected.affected_base_clade);
  CHECK(actual.affected_temp_clade == expected.affected_temp_clade);
  CHECK(actual.affected_base_row_slot == expected.affected_base_row_slot);
  CHECK(actual.affected_temp_row_slot == expected.affected_temp_row_slot);
  CHECK(actual.root == expected.root);
  CHECK(actual.reachability_stats == expected.reachability_stats);
  CHECK(actual.base_plan_generation == expected.base_plan_generation);
  CHECK(actual.base_plan_fingerprint == expected.base_plan_fingerprint);
  CHECK(actual.candidate_plan_build_stats ==
        expected.candidate_plan_build_stats);
  CHECK(actual.compiled_rows == expected.compiled_rows);
  CHECK(actual.compiled_productions == expected.compiled_productions);
  CHECK(actual.compiled_children == expected.compiled_children);
  CHECK(actual.removed_base_production == expected.removed_base_production);
  CHECK(actual.reachable_base_clade == expected.reachable_base_clade);
  CHECK(actual.reachable_temp_clade == expected.reachable_temp_clade);
  CHECK(actual.temp_productions_by_base_parent ==
        expected.temp_productions_by_base_parent);
  CHECK(actual.temp_productions_by_temp_parent ==
        expected.temp_productions_by_temp_parent);
  CHECK(actual.temp_productions_by_base_child ==
        expected.temp_productions_by_base_child);
  CHECK(actual.temp_productions_by_temp_child ==
        expected.temp_productions_by_temp_child);
}

struct spr_overlay_delta_capacity_snapshot {
  std::size_t temp_clades = 0;
  std::size_t temp_productions = 0;
  std::size_t removed_base_productions = 0;
  std::size_t commit_source = 0;
  std::size_t affected_order = 0;
  std::size_t affected_base_clade = 0;
  std::size_t affected_temp_clade = 0;
  std::size_t affected_base_row_slot = 0;
  std::size_t affected_temp_row_slot = 0;
  std::size_t compiled_rows = 0;
  std::size_t compiled_productions = 0;
  std::size_t compiled_children = 0;
  std::size_t removed_base_production = 0;
  std::size_t reachable_base_clade = 0;
  std::size_t reachable_temp_clade = 0;
  std::size_t temp_productions_by_base_parent = 0;
  std::size_t temp_productions_by_temp_parent = 0;
  std::size_t temp_productions_by_base_child = 0;
  std::size_t temp_productions_by_temp_child = 0;
  std::vector<std::size_t> temp_clade_taxa;
  std::vector<std::size_t> temp_production_children;
  std::vector<std::size_t> temp_production_witnesses;
  std::vector<std::vector<std::size_t>> witness_children;
  std::vector<std::vector<std::vector<std::size_t>>> witness_edges;
  std::vector<std::size_t> base_parent_rows;
  std::vector<std::size_t> temp_parent_rows;
  std::vector<std::size_t> base_child_rows;
  std::vector<std::size_t> temp_child_rows;
};

static std::vector<std::size_t> index_row_capacities(
    std::vector<std::vector<larch::production_id>> const& index) {
  std::vector<std::size_t> capacities;
  capacities.reserve(index.size());
  for (auto const& row : index) capacities.push_back(row.capacity());
  return capacities;
}

static spr_overlay_delta_capacity_snapshot snapshot_delta_capacities(
    larch::spr_overlay_delta const& delta) {
  spr_overlay_delta_capacity_snapshot snapshot;
#define SNAPSHOT_CAPACITY(field) snapshot.field = delta.field.capacity()
  SNAPSHOT_CAPACITY(temp_clades);
  SNAPSHOT_CAPACITY(temp_productions);
  SNAPSHOT_CAPACITY(removed_base_productions);
  SNAPSHOT_CAPACITY(commit_source);
  SNAPSHOT_CAPACITY(affected_order);
  SNAPSHOT_CAPACITY(affected_base_clade);
  SNAPSHOT_CAPACITY(affected_temp_clade);
  SNAPSHOT_CAPACITY(affected_base_row_slot);
  SNAPSHOT_CAPACITY(affected_temp_row_slot);
  SNAPSHOT_CAPACITY(compiled_rows);
  SNAPSHOT_CAPACITY(compiled_productions);
  SNAPSHOT_CAPACITY(compiled_children);
  SNAPSHOT_CAPACITY(removed_base_production);
  SNAPSHOT_CAPACITY(reachable_base_clade);
  SNAPSHOT_CAPACITY(reachable_temp_clade);
  SNAPSHOT_CAPACITY(temp_productions_by_base_parent);
  SNAPSHOT_CAPACITY(temp_productions_by_temp_parent);
  SNAPSHOT_CAPACITY(temp_productions_by_base_child);
  SNAPSHOT_CAPACITY(temp_productions_by_temp_child);
#undef SNAPSHOT_CAPACITY
  for (auto const& clade : delta.temp_clades) {
    snapshot.temp_clade_taxa.push_back(clade.taxa.capacity());
  }
  for (auto const& production : delta.temp_productions) {
    snapshot.temp_production_children.push_back(production.children.capacity());
    snapshot.temp_production_witnesses.push_back(
        production.witnesses.capacity());
    std::vector<std::size_t> child_capacities;
    std::vector<std::vector<std::size_t>> edge_capacities;
    for (auto const& witness : production.witnesses) {
      child_capacities.push_back(witness.children.capacity());
      std::vector<std::size_t> witness_edge_capacities;
      for (auto const& child : witness.children) {
        witness_edge_capacities.push_back(child.edge_alternatives.capacity());
      }
      edge_capacities.push_back(std::move(witness_edge_capacities));
    }
    snapshot.witness_children.push_back(std::move(child_capacities));
    snapshot.witness_edges.push_back(std::move(edge_capacities));
  }
  snapshot.base_parent_rows =
      index_row_capacities(delta.temp_productions_by_base_parent);
  snapshot.temp_parent_rows =
      index_row_capacities(delta.temp_productions_by_temp_parent);
  snapshot.base_child_rows =
      index_row_capacities(delta.temp_productions_by_base_child);
  snapshot.temp_child_rows =
      index_row_capacities(delta.temp_productions_by_temp_child);
  return snapshot;
}

static void check_capacity_vector_floor(std::vector<std::size_t> const& actual,
                                        std::vector<std::size_t> const& floor) {
  CHECK(actual.size() == floor.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    CHECK(actual[i] >= floor[i]);
  }
}

static void check_delta_capacity_floor(
    larch::spr_overlay_delta const& delta,
    spr_overlay_delta_capacity_snapshot const& floor) {
#define CHECK_CAPACITY_FLOOR(field) CHECK(delta.field.capacity() >= floor.field)
  CHECK_CAPACITY_FLOOR(temp_clades);
  CHECK_CAPACITY_FLOOR(temp_productions);
  CHECK_CAPACITY_FLOOR(removed_base_productions);
  CHECK_CAPACITY_FLOOR(commit_source);
  CHECK_CAPACITY_FLOOR(affected_order);
  CHECK_CAPACITY_FLOOR(affected_base_clade);
  CHECK_CAPACITY_FLOOR(affected_temp_clade);
  CHECK_CAPACITY_FLOOR(affected_base_row_slot);
  CHECK_CAPACITY_FLOOR(affected_temp_row_slot);
  CHECK_CAPACITY_FLOOR(compiled_rows);
  CHECK_CAPACITY_FLOOR(compiled_productions);
  CHECK_CAPACITY_FLOOR(compiled_children);
  CHECK_CAPACITY_FLOOR(removed_base_production);
  CHECK_CAPACITY_FLOOR(reachable_base_clade);
  CHECK_CAPACITY_FLOOR(reachable_temp_clade);
  CHECK_CAPACITY_FLOOR(temp_productions_by_base_parent);
  CHECK_CAPACITY_FLOOR(temp_productions_by_temp_parent);
  CHECK_CAPACITY_FLOOR(temp_productions_by_base_child);
  CHECK_CAPACITY_FLOOR(temp_productions_by_temp_child);
#undef CHECK_CAPACITY_FLOOR
  auto snapshot = snapshot_delta_capacities(delta);
  check_capacity_vector_floor(snapshot.temp_clade_taxa, floor.temp_clade_taxa);
  check_capacity_vector_floor(snapshot.temp_production_children,
                              floor.temp_production_children);
  check_capacity_vector_floor(snapshot.temp_production_witnesses,
                              floor.temp_production_witnesses);
  CHECK(snapshot.witness_children.size() == floor.witness_children.size());
  CHECK(snapshot.witness_edges.size() == floor.witness_edges.size());
  for (std::size_t i = 0; i < snapshot.witness_children.size(); ++i) {
    check_capacity_vector_floor(snapshot.witness_children[i],
                                floor.witness_children[i]);
    CHECK(snapshot.witness_edges[i].size() == floor.witness_edges[i].size());
    for (std::size_t j = 0; j < snapshot.witness_edges[i].size(); ++j) {
      check_capacity_vector_floor(snapshot.witness_edges[i][j],
                                  floor.witness_edges[i][j]);
    }
  }
  check_capacity_vector_floor(snapshot.base_parent_rows,
                              floor.base_parent_rows);
  check_capacity_vector_floor(snapshot.temp_parent_rows,
                              floor.temp_parent_rows);
  check_capacity_vector_floor(snapshot.base_child_rows, floor.base_child_rows);
  check_capacity_vector_floor(snapshot.temp_child_rows, floor.temp_child_rows);
}

static std::vector<larch::overlay_production_ref> all_base_production_refs(
    larch::clade_grammar const& grammar) {
  std::vector<larch::overlay_production_ref> refs;
  refs.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    refs.push_back(larch::base_production_ref(
        static_cast<larch::production_id>(pid)));
  }
  return refs;
}

static std::size_t max_production_arity(
    larch::clade_grammar const& grammar) {
  std::size_t max_arity = 0;
  for (auto const& prod : grammar.productions) {
    max_arity = std::max(max_arity, prod.children.size());
  }
  return max_arity;
}

static larch::chart_cost phase5_brute_add(larch::chart_cost lhs,
                                           larch::chart_cost rhs) {
  if (lhs >= larch::chart_inf || rhs >= larch::chart_inf) {
    return larch::chart_inf;
  }
  if (lhs > larch::chart_inf - rhs) return larch::chart_inf;
  return lhs + rhs;
}

static larch::chart_multisite_detail::chart_row phase5_brute_inf_row() {
  auto row = larch::chart_multisite_detail::chart_row{};
  row.fill(larch::chart_inf);
  return row;
}

static larch::chart_multisite_detail::chart_row
phase5_brute_combine_rows(
    std::vector<larch::chart_multisite_detail::chart_row> const& children) {
  CHECK(!children.empty());
  auto row = phase5_brute_inf_row();
  for (std::uint8_t parent_state = 0; parent_state < larch::nuc_state_count;
       ++parent_state) {
    larch::chart_cost total = 0;
    for (auto const& child : children) {
      larch::chart_cost best_child = larch::chart_inf;
      for (std::uint8_t child_state = 0; child_state < larch::nuc_state_count;
           ++child_state) {
        auto term = phase5_brute_add(
            child[child_state],
            larch::parsimony_chart_detail::transition_cost(parent_state,
                                                           child_state));
        best_child = std::min(best_child, term);
      }
      total = phase5_brute_add(total, best_child);
    }
    row[parent_state] = total;
  }
  return row;
}

static larch::chart_multisite_detail::chart_row
phase5_brute_selected_topology_row_impl(
    larch::clade_grammar const& grammar, larch::site_pattern const& pattern,
    larch::grammar_topology const& topology, larch::clade_id clade,
    std::vector<std::optional<larch::chart_multisite_detail::chart_row>>&
        memo) {
  if (memo[clade].has_value()) return *memo[clade];

  auto row = phase5_brute_inf_row();
  auto const& key = grammar.clades[clade];
  if (key.taxa.size() == 1) {
    auto taxon = key.taxa.front();
    CHECK(taxon < pattern.state_by_taxon.size());
    auto observed = pattern.state_by_taxon[taxon];
    larch::parsimony_chart_detail::validate_state(
        observed, "phase5 brute selected topology leaf");
    row[observed] = 0;
  } else {
    auto pid = topology.selected_production_by_clade[clade];
    CHECK(pid != larch::no_production);
    CHECK(pid < grammar.productions.size());
    auto const& prod = grammar.productions[pid];
    CHECK(prod.parent == clade);
    CHECK(prod.children.size() >= 2);
    std::vector<larch::chart_multisite_detail::chart_row> child_rows;
    child_rows.reserve(prod.children.size());
    for (auto child : prod.children) {
      child_rows.push_back(phase5_brute_selected_topology_row_impl(
          grammar, pattern, topology, child, memo));
    }
    row = phase5_brute_combine_rows(child_rows);
  }

  memo[clade] = row;
  return row;
}

static larch::chart_multisite_detail::chart_row
phase5_brute_selected_topology_row(
    larch::clade_grammar const& grammar, larch::site_pattern const& pattern,
    larch::grammar_topology const& topology) {
  std::vector<std::optional<larch::chart_multisite_detail::chart_row>> memo(
      grammar.clades.size());
  return phase5_brute_selected_topology_row_impl(
      grammar, pattern, topology, grammar.root_clade, memo);
}

static larch::chart_spr_search_state make_phase5_direct_state(
    larch::phylo_dag& dag, larch::clade_grammar grammar) {
  auto active_build = larch::make_active_search_patterns(dag, grammar);
  CHECK(!active_build.active_patterns.patterns.patterns.empty());
  larch::chart_spr_search_state state;
  state.dag = &dag;
  state.grammar = std::move(grammar);
  state.active_patterns = std::move(active_build.active_patterns);
  state.pattern_source_fingerprint =
      std::move(active_build.pattern_source_fingerprint);
  state.invariant_constant_offset = active_build.invariant_constant_offset;
  state.skipped_invariant_site_count =
      active_build.skipped_invariant_site_count;
  return state;
}

static void clear_optional_candidate_metadata(
    larch::grammar_spr_candidate& candidate) {
  candidate.moved_clade = {};
  candidate.old_parent = {};
  candidate.old_sibling = {};
  candidate.new_sibling_or_target = {};
  candidate.source_tree_move.reset();
  candidate.source_before_topology_productions.reset();
  candidate.source_after_topology_productions.reset();
}

static void test_lower_bound_oracle_counters_show_full_rebuild_cost() {
  std::println("test_lower_bound_oracle_counters_show_full_rebuild_cost");

  auto fixture = make_fixture();
  larch::chart_spr_search_counters counters;
  auto score = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front(), {}, &counters);

  CHECK(score.old_score > 0);
  CHECK(counters.full_overlay_materializations == 1);
  CHECK(counters.overlay_materializations_for_oracle == 1);
  CHECK(counters.overlay_materializations_for_exact_verification == 0);
  CHECK(counters.overlay_materializations_for_accept_materialization == 0);
  CHECK(counters.full_composite_rebuilds == 2);
  CHECK(counters.local_candidate_scores == 0);

  std::println("  PASS");
}

static void test_local_rejected_candidate_counter_guardrail() {
  std::println("test_local_rejected_candidate_counter_guardrail");

  auto fixture = make_fixture();
  auto states = larch::leaf_site_states{
      .state_by_taxon = fixture.patterns.patterns.front().state_by_taxon};
  auto base_chart = larch::build_single_site_chart(fixture.grammar, states);
  auto overlay = larch::overlay_from_candidate(fixture.grammar,
                                               fixture.candidates.front());

  larch::chart_spr_search_counters counters;
  auto local = larch::score_rejected_candidate_with_local_recompute_oracle(
      overlay, base_chart, states, {}, &counters);
  larch::record_chart_spr_rejected_candidate(counters);

  CHECK(local.affected_clade_count > 0);
  CHECK(counters.local_candidate_scores == 1);
  CHECK(counters.rejected_moves == 1);
  CHECK(counters.full_composite_rebuilds == 0);
  CHECK(counters.full_overlay_materializations == 1);
  CHECK(counters.overlay_materializations_for_oracle == 1);

  std::println("  PASS");
}

static std::vector<std::string> candidate_taxon_signatures(
    larch::clade_grammar const& grammar,
    std::vector<larch::grammar_spr_candidate> const& candidates) {
  std::vector<std::string> signatures;
  signatures.reserve(candidates.size());
  for (auto const& candidate : candidates) {
    signatures.push_back(
        larch::chart_spr_candidate_taxon_signature(grammar, candidate));
  }
  return signatures;
}

static void test_streaming_and_eager_candidate_apis_match() {
  std::println("test_streaming_and_eager_candidate_apis_match");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.include_root_moves = true;

  std::vector<larch::grammar_spr_candidate> streamed;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const& candidate) {
        streamed.push_back(candidate);
        return true;
      });
  auto eager = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options);

  CHECK(stats.stop_reason == larch::chart_spr_candidate_stop_reason::exhausted);
  CHECK(stats.candidates_generated_after_dedup == streamed.size());
  CHECK(candidate_taxon_signatures(fixture.grammar, streamed) ==
        candidate_taxon_signatures(fixture.grammar, eager.candidates));

  std::println("  PASS");
}

static void test_search_state_local_score_entry_point_matches_oracle() {
  std::println("test_search_state_local_score_entry_point_matches_oracle");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto scored = larch::score_candidate_locally(state, fixture.candidates.front());
  CHECK(scored.valid);

  larch::chart_spr_search_counters oracle_counters;
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front(), {},
      &oracle_counters);

  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(scored.affected_clade_count > 0);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_composite_rebuilds == 0);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.overlay_materializations_for_local_scoring_bridge == 0);
  CHECK(state.counters.overlay_reachability_validations == 1);
  CHECK(state.counters.reachable_clades_traversed > 0);

  std::println("  PASS");
}

static void test_local_score_with_ua_edge_and_invariant_offset_matches_oracle() {
  std::println("test_local_score_with_ua_edge_and_invariant_offset_matches_oracle");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(!candidates.empty());
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto state = larch::build_chart_spr_search_state(
      dag, grammar, patterns, opts);
  auto scored = larch::score_candidate_locally(state, candidates.front());
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      grammar, patterns, candidates.front(), opts);

  CHECK(scored.valid);
  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(scored.lower_bound.invariant_offset_applied ==
        state.invariant_constant_offset);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  larch::chart_spr_search_options lazy_options;
  lazy_options.chart = opts;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      dag, grammar, lazy_options);
  auto lazy_scored =
      larch::score_candidate_locally(lazy_state, candidates.front());
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.counters.lazy_inside_rows_computed > 0);
  CHECK(lazy_scored.valid);
  CHECK(lazy_scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(lazy_scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(lazy_scored.lower_bound.value.delta == oracle.delta);
  CHECK(lazy_scored.lower_bound.invariant_offset_applied ==
        lazy_state.invariant_constant_offset);
  CHECK(lazy_state.invariant_constant_offset == 2);
  CHECK(lazy_state.counters.local_rows_recomputed > 0);

  std::println("  PASS");
}

static void test_production_delta_only_candidate_metadata_absent_matches_oracle() {
  std::println("test_production_delta_only_candidate_metadata_absent_matches_oracle");

  auto fixture = make_fixture();
  auto candidate = fixture.candidates.front();
  clear_optional_candidate_metadata(candidate);

  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto scored = larch::score_candidate_locally(state, candidate);
  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, fixture.patterns, candidate);
  auto signature = larch::chart_spr_candidate_taxon_signature(
      fixture.grammar, candidate);

  CHECK(scored.valid);
  CHECK(signature.find("m={};op={};os={};nt={}") !=
        std::string::npos);
  CHECK(scored.lower_bound.value.old_score == oracle.old_score);
  CHECK(scored.lower_bound.value.new_score == oracle.new_score);
  CHECK(scored.lower_bound.value.delta == oracle.delta);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_multiparent_dag_affected_closure_and_slot_maps() {
  std::println("test_multiparent_dag_affected_closure_and_slot_maps");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto abc = clade_for(grammar, {"A", "B", "C"});
  auto abd = clade_for(grammar, {"A", "B", "D"});
  auto ab_prod = production_id_for(grammar, ab, {a, b});
  CHECK(grammar.productions_by_child[ab].size() >= 2);

  larch::grammar_spr_candidate candidate;
  candidate.removed_productions.push_back(larch::base_production_ref(ab_prod));
  candidate.added_productions.push_back(temp_prod(
      larch::base_clade_ref(ab), {larch::base_clade_ref(a),
                                  larch::base_clade_ref(b)}));

  auto delta = larch::build_spr_overlay_delta(grammar, candidate);
  CHECK(delta.reachability_stats.full_grammar_like);
  CHECK(delta.affected_base_clade[ab]);
  CHECK(delta.affected_base_clade[abc]);
  CHECK(delta.affected_base_clade[abd]);
  CHECK(delta.affected_base_clade[grammar.root_clade]);
  CHECK(delta.affected_base_row_slot[ab] !=
        larch::local_overlay_chart_rows::npos);
  CHECK(delta.affected_order.size() >= 4);

  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto scored = larch::score_candidate_locally(state, candidate);
  CHECK(scored.valid);
  CHECK(scored.lower_bound.value.delta == 0);
  CHECK(state.counters.reachability_full_grammar_like_passes == 1);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_unreachable_dead_clades_ignored_reachable_dead_clades_fail() {
  std::println("test_unreachable_dead_clades_ignored_reachable_dead_clades_fail");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto ab = clade_for(fixture.grammar, {"A", "B"});
  auto cd = clade_for(fixture.grammar, {"C", "D"});

  larch::grammar_spr_candidate unreachable_dead;
  unreachable_dead.added_clades.push_back(fixture.grammar.clades[ab]);
  auto delta = larch::build_spr_overlay_delta(
      fixture.grammar, unreachable_dead);
  CHECK(delta.reachability_stats.reachable_temp_clades == 0);
  CHECK(delta.affected_order.empty());
  auto scored = larch::score_candidate_locally(state, unreachable_dead);
  CHECK(scored.valid);
  CHECK(scored.lower_bound.value.delta == 0);

  larch::grammar_spr_candidate reachable_dead;
  reachable_dead.added_clades.push_back(fixture.grammar.clades[ab]);
  reachable_dead.added_productions.push_back(temp_prod(
      larch::base_clade_ref(fixture.grammar.root_clade),
      {larch::temp_clade_ref(0), larch::base_clade_ref(cd)}));
  auto bad_scored = larch::score_candidate_locally(state, reachable_dead);
  CHECK(!bad_scored.valid);
  CHECK(bad_scored.invalid_reason.find("no available productions") !=
        std::string::npos);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_nonbinary_overlay_production_scored_locally() {
  std::println("test_nonbinary_overlay_production_scored_locally");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto a = clade_for(fixture.grammar, {"A"});
  auto b = clade_for(fixture.grammar, {"B"});
  auto cd = clade_for(fixture.grammar, {"C", "D"});

  larch::grammar_spr_candidate candidate;
  candidate.added_productions.push_back(temp_prod(
      larch::base_clade_ref(fixture.grammar.root_clade),
      {larch::base_clade_ref(a), larch::base_clade_ref(b),
       larch::base_clade_ref(cd)}));

  auto const counters_before = state.counters;
  auto scored = larch::score_candidate_locally(state, candidate);
  CHECK(scored.valid);
  CHECK(scored.invalid_reason.empty());
  CHECK(scored.affected_clade_count > 0);
  auto const active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  CHECK(active_pattern_count > 0);
  CHECK(state.counters.local_leaf_state_view_uses ==
        counters_before.local_leaf_state_view_uses + active_pattern_count);
  CHECK(state.counters.local_leaf_state_owned_copies ==
        counters_before.local_leaf_state_owned_copies);
  CHECK(state.counters.local_row_scratch_capacity_growths ==
        counters_before.local_row_scratch_capacity_growths + 1);
  CHECK(state.counters.multifurcation_productions_scored >
        counters_before.multifurcation_productions_scored);
  CHECK(state.counters.full_overlay_materializations == 0);

  auto plan = larch::build_chart_execution_plan(fixture.grammar);
  auto checked = larch::check_chart_execution_plan(fixture.grammar, plan);
  auto delta = larch::build_spr_overlay_delta(fixture.grammar, plan, candidate);
  larch::spr_overlay_delta into_delta;
  larch::spr_overlay_delta_build_scratch build_scratch;
  larch::candidate_chart_execution_plan_build_stats into_stats;
  larch::build_spr_overlay_delta_from_resident_plan_into(
      fixture.grammar, checked, candidate, into_delta, build_scratch, {},
      &into_stats);
  CHECK(build_scratch.operation_boundary_clean());
  CHECK(into_stats == into_delta.candidate_plan_build_stats);
  check_spr_overlay_delta_equal(into_delta, delta);
  CHECK(std::any_of(
      into_delta.compiled_productions.begin(),
      into_delta.compiled_productions.end(),
      [](auto const& production) { return production.child_count == 3; }));
  auto overlay = larch::overlay_from_candidate(fixture.grammar, candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  for (auto const& pattern : fixture.patterns.patterns) {
    larch::leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
    auto base_chart = larch::build_single_site_chart(fixture.grammar, states);
    auto local_rows = larch::build_local_overlay_chart_rows(
        delta, base_chart, states);
    larch::verify_local_overlay_rows_against_full(
        delta, local_rows, base_chart, materialized, states,
        larch::chart_options{});
  }

  std::println("  PASS");
}

static void test_overlay_delta_rows_match_full_overlay_for_tiny_candidates() {
  std::println("test_overlay_delta_rows_match_full_overlay_for_tiny_candidates");

  auto fixture = make_fixture();
  auto const& pattern = fixture.patterns.patterns.front();
  larch::leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
  auto base_chart = larch::build_single_site_chart(fixture.grammar, states);

  bool saw_strictly_local_candidate = false;
  for (auto const& candidate : fixture.candidates) {
    auto delta = larch::build_spr_overlay_delta(fixture.grammar, candidate);
    auto overlay = larch::overlay_from_candidate(fixture.grammar, candidate);
    auto materialized = larch::materialize_overlay_grammar(overlay);
    auto full = larch::build_single_site_chart(materialized.grammar, states);
    auto local_rows = larch::build_local_overlay_chart_rows(
        delta, base_chart, states);

    CHECK(delta.reachability_stats.reachable_clades ==
          materialized.grammar.clades.size());
    CHECK(delta.affected_order.size() > 0);
    if (delta.affected_order.size() < materialized.grammar.clades.size()) {
      saw_strictly_local_candidate = true;
    }
    for (std::size_t dense = 0;
         dense < materialized.dense_clade_to_ref.size(); ++dense) {
      auto ref = materialized.dense_clade_to_ref[dense];
      CHECK(larch::local_overlay_chart_row(local_rows, base_chart, ref) ==
            full.inside[dense]);
    }
  }
  CHECK(saw_strictly_local_candidate);

  std::println("  PASS");
}

static std::size_t candidate_storage_demand(
    larch::grammar_spr_candidate const& candidate) {
  std::size_t demand = candidate.removed_productions.size() +
                       candidate.added_clades.size() +
                       candidate.added_productions.size();
  for (auto const& clade : candidate.added_clades) {
    demand += clade.taxa.size();
  }
  for (auto const& production : candidate.added_productions) {
    demand += production.children.size() + production.witnesses.size();
    for (auto const& witness : production.witnesses) {
      demand += witness.children.size();
      for (auto const& child : witness.children) {
        demand += child.edge_alternatives.size();
      }
    }
  }
  return demand;
}

static void test_overlay_delta_into_reuse_and_fail_closed_contract() {
  std::println("test_overlay_delta_into_reuse_and_fail_closed_contract");

  auto fixture = make_fixture();
  auto plan = larch::build_chart_execution_plan(fixture.grammar);
  auto checked = larch::check_chart_execution_plan(fixture.grammar, plan);
  auto const& pattern = fixture.patterns.patterns.front();
  larch::leaf_site_states states{.state_by_taxon = pattern.state_by_taxon};
  auto base_chart = larch::build_single_site_chart(fixture.grammar, states);

  larch::spr_overlay_delta delta;
  larch::spr_overlay_delta_build_scratch scratch;
  CHECK(scratch.operation_boundary_clean());
  for (auto const& candidate : fixture.candidates) {
    auto expected =
        larch::build_spr_overlay_delta(fixture.grammar, plan, candidate);
    larch::candidate_chart_execution_plan_build_stats external_stats;
    larch::build_spr_overlay_delta_from_resident_plan_into(
        fixture.grammar, checked, candidate, delta, scratch, {},
        &external_stats);
    CHECK(scratch.operation_boundary_clean());
    CHECK(external_stats == expected.candidate_plan_build_stats);
    check_spr_overlay_delta_equal(delta, expected);

    // Value-vs-into parity is backed by the independent materialized-grammar
    // row oracle, so delegation cannot make this test tautological.
    auto overlay = larch::overlay_from_candidate(fixture.grammar, candidate);
    auto materialized = larch::materialize_overlay_grammar(overlay);
    auto local_rows =
        larch::build_local_overlay_chart_rows(delta, base_chart, states);
    larch::verify_local_overlay_rows_against_full(delta, local_rows, base_chart,
                                                  materialized, states,
                                                  larch::chart_options{});
  }

  auto largest = std::max_element(
      fixture.candidates.begin(), fixture.candidates.end(),
      [](auto const& lhs, auto const& rhs) {
        return candidate_storage_demand(lhs) < candidate_storage_demand(rhs);
      });
  CHECK(largest != fixture.candidates.end());
  CHECK(candidate_storage_demand(*largest) > 0);
  auto expected_large =
      larch::build_spr_overlay_delta(fixture.grammar, plan, *largest);
  larch::build_spr_overlay_delta_from_resident_plan_into(
      fixture.grammar, checked, *largest, delta, scratch);
  check_spr_overlay_delta_equal(delta, expected_large);
  auto high_water = snapshot_delta_capacities(delta);

  // A valid identity candidate shrinks every candidate-local payload. Building
  // the large descriptor again must recover every visible outer and nested
  // capacity, while all semantic/build counters reset to the fresh value.
  larch::grammar_spr_candidate identity;
  auto expected_identity =
      larch::build_spr_overlay_delta(fixture.grammar, plan, identity);
  larch::build_spr_overlay_delta_from_resident_plan_into(
      fixture.grammar, checked, identity, delta, scratch);
  CHECK(scratch.operation_boundary_clean());
  check_spr_overlay_delta_equal(delta, expected_identity);
  CHECK(delta.candidate_plan_build_stats.candidate_partition_validations == 0);
  CHECK(delta.candidate_plan_build_stats.production_descriptors_compiled == 0);

  larch::build_spr_overlay_delta_from_resident_plan_into(
      fixture.grammar, checked, *largest, delta, scratch);
  CHECK(scratch.operation_boundary_clean());
  check_spr_overlay_delta_equal(delta, expected_large);
  check_delta_capacity_floor(delta, high_water);

  // Force a failure after payload copy and candidate-partition accounting.
  // The previous successful descriptor must not remain consumable, and the
  // same destination/scratch must recover on the next valid build.
  auto malformed = *largest;
  auto leaf = clade_for(fixture.grammar, {"A"});
  malformed.added_productions.push_back(
      temp_prod(larch::base_clade_ref(fixture.grammar.root_clade),
                {larch::base_clade_ref(leaf)}));
  bool invalid_threw = false;
  try {
    larch::build_spr_overlay_delta_from_resident_plan_into(
        fixture.grammar, checked, malformed, delta, scratch);
  } catch (std::runtime_error const& e) {
    invalid_threw = true;
    CHECK(std::string{e.what()}.find("at least 2 children") !=
          std::string::npos);
  }
  CHECK(invalid_threw);
  CHECK(delta.base == nullptr);
  CHECK(delta.base_plan_generation == 0);
  CHECK(delta.affected_order.empty());
  CHECK(delta.affected_base_row_slot.empty());
  CHECK(delta.affected_temp_row_slot.empty());
  CHECK(delta.compiled_rows.empty());
  CHECK(delta.compiled_productions.empty());
  CHECK(delta.compiled_children.empty());
  CHECK(scratch.operation_boundary_clean());

  larch::local_overlay_chart_rows failed_rows;
  bool failed_delta_rejected = false;
  try {
    larch::build_local_overlay_chart_rows_into(fixture.grammar, delta,
                                               base_chart, states, failed_rows);
  } catch (std::runtime_error const& e) {
    failed_delta_rejected = true;
    CHECK(std::string{e.what()}.find("row slot map size mismatch") !=
          std::string::npos);
  }
  CHECK(failed_delta_rejected);

  larch::build_spr_overlay_delta_from_resident_plan_into(
      fixture.grammar, checked, *largest, delta, scratch);
  check_spr_overlay_delta_equal(delta, expected_large);
  check_delta_capacity_floor(delta, high_water);
  CHECK(scratch.operation_boundary_clean());

  // A stale checked capability also fails closed before candidate work. Once
  // the immutable generation is restored, the same capability and storage are
  // usable again.
  auto generation = fixture.grammar.execution_generation;
  ++fixture.grammar.execution_generation;
  bool stale_threw = false;
  try {
    larch::build_spr_overlay_delta_from_resident_plan_into(
        fixture.grammar, checked, *largest, delta, scratch);
  } catch (larch::chart_execution_plan_mismatch const&) {
    stale_threw = true;
  }
  CHECK(stale_threw);
  CHECK(delta.base == nullptr);
  CHECK(delta.base_plan_generation == 0);
  CHECK(scratch.operation_boundary_clean());
  fixture.grammar.execution_generation = generation;

  larch::build_spr_overlay_delta_from_resident_plan_into(
      fixture.grammar, checked, *largest, delta, scratch);
  CHECK(scratch.operation_boundary_clean());
  check_spr_overlay_delta_equal(delta, expected_large);
  check_delta_capacity_floor(delta, high_water);

  std::println("  PASS");
}

static void test_local_scoring_verify_option_counts_oracle_materialization() {
  std::println("test_local_scoring_verify_option_counts_oracle_materialization");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  larch::local_spr_score_options opts;
  opts.verify_against_full_overlay = true;
  auto scored = larch::score_candidate_locally(
      state, fixture.candidates.front(), opts);

  CHECK(scored.valid);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_overlay_materializations == 1);
  CHECK(state.counters.overlay_materializations_for_oracle == 1);
  CHECK(state.counters.overlay_materializations_for_local_scoring_bridge == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_invalid_disconnected_overlay_returns_invalid_score() {
  std::println("test_invalid_disconnected_overlay_returns_invalid_score");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  CHECK(!fixture.grammar.productions_by_parent[fixture.grammar.root_clade]
             .empty());
  larch::grammar_spr_candidate bad;
  bad.removed_productions.push_back(larch::base_production_ref(
      fixture.grammar.productions_by_parent[fixture.grammar.root_clade]
          .front()));

  auto scored = larch::score_candidate_locally(state, bad);
  CHECK(!scored.valid);
  CHECK(!scored.invalid_reason.empty());
  CHECK(!scored.lower_bound.value.improves());
  CHECK(scored.lower_bound.value.old_score ==
        state.composite_lower_bound_with_invariants);
  CHECK(scored.lower_bound.value.new_score ==
        state.composite_lower_bound_with_invariants);
  CHECK(state.counters.local_candidate_scores == 1);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);

  std::println("  PASS");
}

static void test_persistent_active_pattern_cache_matches_full_composite() {
  std::println("test_persistent_active_pattern_cache_matches_full_composite");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto full_patterns = larch::build_site_patterns(dag, grammar);
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto active_build = larch::make_active_search_patterns(full_patterns, opts);
  CHECK(active_build.skipped_invariant_site_count == 3);
  CHECK(active_build.invariant_constant_offset == 2);
  active_build.active_patterns.assert_no_skipped_invariant_metadata();

  auto active_composite = larch::build_composite_chart_score_active(
      grammar, active_build.active_patterns, opts);
  auto full_composite = larch::build_composite_chart_score(
      grammar, full_patterns, opts);

  auto state = larch::build_chart_spr_search_state(
      dag, grammar, full_patterns, opts);
  CHECK(state.pattern_charts.size() ==
        active_build.active_patterns.patterns.patterns.size());
  CHECK(state.skipped_invariant_site_count == 3);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(state.composite_lower_bound_without_invariants ==
        active_composite.weighted_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        full_composite.weighted_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        state.composite_lower_bound_without_invariants +
            state.invariant_constant_offset);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);

  std::println("  PASS");
}

static void test_root_reference_counts_preserved_in_cache() {
  std::println("test_root_reference_counts_preserved_in_cache");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AC", four_taxon_repeated_reference_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  CHECK(patterns.patterns.size() == 1);
  CHECK(patterns.patterns.front().weight == 2);
  CHECK(patterns.patterns.front()
            .reference_state_counts[larch::nuc_base::A] == 1);
  CHECK(patterns.patterns.front()
            .reference_state_counts[larch::nuc_base::C] == 1);

  larch::chart_options opts;
  opts.score_ua_edge = true;
  auto state = larch::build_chart_spr_search_state(
      dag, grammar, patterns, opts);
  CHECK(state.pattern_charts.size() == 1);
  auto const& entry = state.pattern_charts.front();
  CHECK(entry.reference_state_counts[larch::nuc_base::A] == 1);
  CHECK(entry.reference_state_counts[larch::nuc_base::C] == 1);
  CHECK(entry.root_min_excluding_ua == 1);
  CHECK(entry.weighted_root_score == 3);
  CHECK(entry.weighted_root_score >
        patterns.patterns.front().weight * entry.root_min_excluding_ua);
  CHECK(state.composite_lower_bound_without_invariants == 3);
  CHECK(state.composite_lower_bound_with_invariants == 3);

  std::println("  PASS");
}

static void test_active_pattern_assertions_reject_skipped_metadata() {
  std::println("test_active_pattern_assertions_reject_skipped_metadata");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::site_pattern_options pattern_opts;
  pattern_opts.skip_invariant_sites = true;
  auto skipped = larch::build_site_patterns(dag, grammar, pattern_opts);
  CHECK(skipped.skipped_invariant_site_count == 3);

  larch::active_site_pattern_set bad{skipped};
  bool threw = false;
  try {
    bad.assert_no_skipped_invariant_metadata();
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_state_builder_from_dag_rebuilds_patterns_once() {
  std::println("test_state_builder_from_dag_rebuilds_patterns_once");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_options opts;
  opts.score_ua_edge = true;

  auto state = larch::build_chart_spr_search_state(dag, grammar, opts);
  CHECK(state.counters.pattern_rebuilds == 1);
  CHECK(state.counters.base_chart_cache_rebuilds == 1);
  CHECK(state.skipped_invariant_site_count == 3);
  CHECK(state.invariant_constant_offset == 2);
  CHECK(larch::chart_spr_pattern_source_fingerprint_matches(
      dag, grammar, state.pattern_source_fingerprint));
  state.active_patterns.assert_no_skipped_invariant_metadata();

  std::println("  PASS");
}

static void test_semantic_capture_reuses_primary_exact_provenance() {
  std::println("test_semantic_capture_reuses_primary_exact_provenance");

  auto fixture = make_fixture();
  auto plan = larch::build_chart_execution_plan(fixture.grammar);
  auto active_build = larch::make_active_search_patterns(fixture.patterns);
  auto const& active = active_build.active_patterns;

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;
  CHECK(!options.exact_trim.capture_optimal_root_provenance);
  larch::configure_chart_spr_primary_exact_provenance(options);
  CHECK(options.exact_trim.capture_optimal_root_provenance);

  auto primary = larch::build_multisite_trim_active(plan, active, options.chart,
                                                    options.exact_trim);
  CHECK(primary.keep_production_exact);
  CHECK(!primary.optimal_root_provenance_classes.empty());
  auto primary_evidence = larch::chart_spr_canonicalize_search_trim_evidence(
      fixture.grammar, plan, active, options.chart, options.exact_trim, primary,
      active_build.invariant_constant_offset);
  auto const primary_memory =
      larch::estimate_chart_spr_canonical_exact_evidence_memory(
          fixture.grammar, primary);
  auto const primary_actual =
      larch::estimate_chart_spr_canonical_exact_evidence_resident_bytes(
          primary_evidence);
  CHECK(primary_memory.retained_bytes >= primary_actual);
  CHECK(primary_memory.construction_peak_bytes >=
        primary_memory.retained_bytes);

  // Preserve the frozen canonical bytes: the primary-capture path must emit
  // the exact evidence formerly produced by the checked companion fallback.
  auto fallback_options = options.exact_trim;
  fallback_options.capture_optimal_root_provenance = false;
  auto fallback = larch::build_multisite_trim_active(
      plan, active, options.chart, fallback_options);
  CHECK(fallback.optimal_root_provenance_classes.empty());
  auto fallback_evidence = larch::chart_spr_canonicalize_search_trim_evidence(
      fixture.grammar, plan, active, options.chart, fallback_options, fallback,
      active_build.invariant_constant_offset);
  CHECK(primary_evidence.evidence_kind == fallback_evidence.evidence_kind);
  CHECK(primary_evidence.keep_mask_kind == fallback_evidence.keep_mask_kind);
  CHECK(primary_evidence.keep_production_exact ==
        fallback_evidence.keep_production_exact);
  CHECK(primary_evidence.optimum_active == fallback_evidence.optimum_active);
  CHECK(primary_evidence.invariant_offset ==
        fallback_evidence.invariant_offset);
  CHECK(primary_evidence.kept_production_keys ==
        fallback_evidence.kept_production_keys);
  CHECK(primary_evidence.frontier_sizes == fallback_evidence.frontier_sizes);
  CHECK(primary_evidence.optimal_root_provenance_classes.size() ==
        fallback_evidence.optimal_root_provenance_classes.size());
  for (std::size_t index = 0;
       index < primary_evidence.optimal_root_provenance_classes.size();
       ++index) {
    CHECK(primary_evidence.optimal_root_provenance_classes[index].cost ==
          fallback_evidence.optimal_root_provenance_classes[index].cost);
    CHECK(primary_evidence.optimal_root_provenance_classes[index]
              .production_keys ==
          fallback_evidence.optimal_root_provenance_classes[index]
              .production_keys);
  }

  larch::chart_spr_search_options score_only;
  score_only.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  score_only.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;
  score_only.exact_trim.require_exact_keep_mask = false;
  score_only.exact_trim.dominance_mode =
      larch::multisite_dominance_mode::score_only;
  larch::configure_chart_spr_primary_exact_provenance(score_only);
  CHECK(!score_only.exact_trim.capture_optimal_root_provenance);

  // Sample text is repeated in clade, production, and provenance keys. A
  // 5,000-byte registry exercises the encoded-length estimator rather than a
  // topology-only grammar-capacity multiplier.
  auto long_grammar = fixture.grammar;
  for (std::size_t taxon = 0;
       taxon < long_grammar.taxa.id_to_sample_id.size(); ++taxon) {
    long_grammar.taxa.id_to_sample_id[taxon] =
        std::string(5000, static_cast<char>('a' + taxon % 26)) + "-" +
        std::to_string(taxon);
  }
  long_grammar.taxa.sample_id_to_id.clear();
  long_grammar.taxa.sample_id_to_id.reserve(
      long_grammar.taxa.id_to_sample_id.size());
  for (std::size_t taxon = 0;
       taxon < long_grammar.taxa.id_to_sample_id.size(); ++taxon) {
    long_grammar.taxa.sample_id_to_id.emplace(
        long_grammar.taxa.id_to_sample_id[taxon],
        static_cast<larch::taxon_id>(taxon));
  }
  auto long_plan = larch::build_chart_execution_plan(long_grammar);
  auto long_trim = larch::build_multisite_trim_active(
      long_plan, active, options.chart, options.exact_trim);
  auto const long_memory =
      larch::estimate_chart_spr_canonical_exact_evidence_memory(long_grammar,
                                                                 long_trim);
  auto long_evidence = larch::chart_spr_canonicalize_search_trim_evidence(
      long_grammar, long_plan, active, options.chart, options.exact_trim,
      long_trim, active_build.invariant_constant_offset);
  auto const long_actual =
      larch::estimate_chart_spr_canonical_exact_evidence_resident_bytes(
          long_evidence);
  CHECK(long_memory.retained_bytes >= long_actual);
  CHECK(long_memory.construction_peak_bytes >= long_memory.retained_bytes);
  CHECK(long_memory.retained_bytes > primary_memory.retained_bytes + 4096);

  std::println("  PASS");
}

static void test_primary_provenance_capture_failure_is_hard_error() {
  std::println("test_primary_provenance_capture_failure_is_hard_error");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   options);
  CHECK(state.exact_trim_active_only.has_value());
  CHECK(state.exact_trim_active_only->optimal_root_provenance_classes.empty());

  // Cache the ordinary old-state trim first, then fail only while the
  // candidate's primary B&B copies its already-established optimal root
  // classes.  This reaches the verifier catch boundary that must not convert a
  // report-only failure into candidate invalidity.
  auto trim_options = options.exact_trim;
  trim_options.capture_optimal_root_provenance = true;
  trim_options.force_optimal_root_provenance_capture_failure_for_tests = true;
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  auto candidate =
      larch::score_candidate_locally(state, fixture.candidates.front());
  CHECK(candidate.valid);

  bool hard_failure_escaped = false;
  try {
    (void)larch::verify_candidate_exact_against_state(
        state, std::move(candidate), checked, scheduler, trim_options);
  } catch (
      larch::multisite_optimal_root_provenance_capture_error const& error) {
    hard_failure_escaped = true;
    CHECK(std::string_view{error.what()}.find(
              "forced optimal-root provenance capture failure for test") !=
          std::string_view::npos);
  }
  CHECK(hard_failure_escaped);
  CHECK(state.counters.exact_verifications == 1);
  CHECK(scheduler.metrics().pending_tasks == 0);

  // The joined scheduler and state remain reusable after the hard exception.
  trim_options.force_optimal_root_provenance_capture_failure_for_tests = false;
  auto recovery_candidate =
      larch::score_candidate_locally(state, fixture.candidates.front());
  auto recovered = larch::verify_candidate_exact_against_state(
      state, std::move(recovery_candidate), checked, scheduler, trim_options);
  CHECK(recovered.valid);
  CHECK(recovered.exact.has_value());
  CHECK(state.counters.exact_verifications == 2);
  CHECK(state.counters.scheduler_axes.exact_frontier_clades.operations > 0);
  scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(scheduler.metrics(),
                                             state.counters.scheduler_axes);

  std::println("  PASS");
}

static void test_lazy_local_exact_semantic_evidence_envelope() {
  std::println("test_lazy_local_exact_semantic_evidence_envelope");

  auto run_case = [&](bool long_sample_ids) {
    auto fixture = make_fixture();
    if (long_sample_ids) {
      for (std::size_t taxon = 0;
           taxon < fixture.grammar.taxa.id_to_sample_id.size(); ++taxon) {
        fixture.grammar.taxa.id_to_sample_id[taxon] =
            std::string(5000, static_cast<char>('a' + taxon % 26)) + "-" +
            std::to_string(taxon);
      }
      fixture.grammar.taxa.sample_id_to_id.clear();
      fixture.grammar.taxa.sample_id_to_id.reserve(
          fixture.grammar.taxa.id_to_sample_id.size());
      for (std::size_t taxon = 0;
           taxon < fixture.grammar.taxa.id_to_sample_id.size(); ++taxon) {
        fixture.grammar.taxa.sample_id_to_id.emplace(
            fixture.grammar.taxa.id_to_sample_id[taxon],
            static_cast<larch::taxon_id>(taxon));
      }
    }

    larch::chart_spr_search_options options;
    options.acceptance_mode =
        larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 1;
    options.cache.use_lazy_multisite_chart = true;
    options.cache.candidate_batch_size = 1;
    options.enumeration.max_candidates = 1;
    options.enumeration.max_candidates_is_post_dedup = true;
    // Keep this boundary test focused on the deferred state-evidence envelope:
    // exact-candidate admission has its own independently tested finite
    // envelope and can otherwise be the larger requirement on this fixture.
    options.enumeration.max_estimated_affected_clades = 1;
    options.semantic_capture =
        larch::chart_spr_semantic_capture_mode::digest;
    larch::configure_chart_spr_primary_exact_provenance(options);
    options.exact_trim.max_frontier_entries_per_clade = 1024;
    CHECK(options.exact_trim.capture_optimal_root_provenance);

    auto active = larch::make_active_search_patterns(fixture.patterns);
    auto state = larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), options.chart,
        /*build_exact_trim=*/true, options.exact_trim, options.cache);
    CHECK(state.cache_strategy ==
          larch::chart_spr_cache_strategy::lazy_multisite_chart);
    CHECK(state.exact_trim_active_only.has_value());
    CHECK(!state.exact_trim_active_only->optimal_root_provenance_classes.empty());
    auto const trim_bytes =
        larch::estimate_chart_spr_trim_resident_bytes(
            *state.exact_trim_active_only);
    auto const evidence_memory =
        larch::estimate_chart_spr_canonical_exact_evidence_memory(
            state.grammar, *state.exact_trim_active_only);
    larch::chart_scheduler scheduler{
        larch::chart_scheduler_options{.requested_workers = 1}};
    auto const envelope = larch::chart_spr_search_detail::
        estimate_grammar_spr_finite_iteration_memory_envelope(
            state, 1, 1, 1, true, scheduler, 1);
    CHECK(envelope.planned_required_bytes >
          larch::estimate_chart_spr_published_state_resident_bytes(state));
    CHECK(envelope.planned_required_bytes ==
          std::max(envelope.planned_generation_phase_required_bytes,
                   envelope.planned_evidence_phase_required_bytes));
    CHECK(envelope.planned_generation_phase_required_bytes > 0);
    CHECK(envelope.planned_evidence_phase_required_bytes > 0);
    CHECK(envelope.planned_required_bytes <
          larch::chart_spr_exact_candidate_checked_bytes_add(
              envelope.planned_generation_phase_required_bytes,
              envelope.planned_evidence_phase_required_bytes,
              "test temporal phase sum"));
    CHECK(envelope
              .planned_canonical_state_exact_evidence_resident_bytes ==
          evidence_memory.retained_bytes);
    CHECK(envelope
              .planned_canonical_state_exact_evidence_construction_peak_bytes ==
          evidence_memory.construction_peak_bytes);
    if (long_sample_ids) {
      CHECK(evidence_memory.retained_bytes > 4096);
    }

    auto finite_options = options;
    finite_options.cache.memory_budget_bytes = envelope.planned_required_bytes;
    larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
        workspace;
    {
      auto result = larch::run_chart_spr_acceptance_iteration(
          state, finite_options, 0, workspace, scheduler);
      CHECK(result.candidates_generated == 0);
      CHECK(result.candidates_scored == 0);
      CHECK(result.canonical_state_exact_before.has_value());
      auto const actual_evidence =
          larch::estimate_chart_spr_canonical_exact_evidence_resident_bytes(
              *result.canonical_state_exact_before);
      CHECK(actual_evidence <= evidence_memory.retained_bytes);
      CHECK(state.counters.lazy_local_retained_exact_trim_bytes_max ==
            trim_bytes);
      CHECK(state.counters
                .lazy_local_canonical_exact_evidence_resident_bytes_max ==
            actual_evidence);
      CHECK(state.counters
                .lazy_local_canonical_exact_evidence_construction_peak_bytes_max ==
            evidence_memory.construction_peak_bytes);
    }
    CHECK(workspace.local_score.operation_boundary_clean());

    finite_options.cache.memory_budget_bytes =
        envelope.planned_required_bytes - 1;
    bool rejected = false;
    try {
      (void)larch::run_chart_spr_acceptance_iteration(
          state, finite_options, 1, workspace, scheduler);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const& e) {
      rejected = true;
      CHECK(e.required_bytes() == envelope.planned_required_bytes);
    }
    CHECK(rejected);
    CHECK(workspace.candidate_slots.capacity() == 0);
    CHECK(workspace.candidate_copy_scratch.capacity() == 0);
    CHECK(workspace.local_results.capacity() == 0);
    scheduler.shutdown();
  };

  run_case(false);
  run_case(true);

  // Direct finite acceptance cannot invoke the compatibility companion B&B;
  // it must request primary provenance before rank/batch reserves.
  auto fixture = make_fixture();
  larch::chart_spr_search_options uncaptured;
  uncaptured.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  uncaptured.semantic_capture =
      larch::chart_spr_semantic_capture_mode::digest;
  uncaptured.cache.use_lazy_multisite_chart = true;
  uncaptured.enumeration.max_candidates = 1;
  uncaptured.enumeration.max_candidates_is_post_dedup = true;
  CHECK(!uncaptured.exact_trim.capture_optimal_root_provenance);
  auto uncaptured_state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, uncaptured);
  uncaptured.cache.memory_budget_bytes = std::size_t{1} << 30;
  larch::chart_scheduler scheduler{
      larch::chart_scheduler_options{.requested_workers = 1}};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  bool provenance_rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        uncaptured_state, uncaptured, 0, workspace, scheduler);
  } catch (larch::chart_spr_search_detail::
               chart_spr_canonical_exact_evidence_budget_error const&) {
    provenance_rejected = true;
  }
  CHECK(provenance_rejected);
  CHECK(workspace.candidate_slots.capacity() == 0);
  CHECK(workspace.local_results.capacity() == 0);
  scheduler.shutdown();

  std::println("  PASS");
}

static void test_published_state_admission_component_identity() {
  std::println("test_published_state_admission_component_identity");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto make_state = [&](larch::chart_cache_options cache, bool build_trim) {
    auto active = larch::make_active_search_patterns(patterns);
    return larch::build_chart_spr_search_state_from_active(
        dag, grammar, std::move(active), {}, build_trim, {}, cache);
  };
  auto check_identity = [](larch::chart_spr_search_state const& state) {
    auto expected = larch::chart_spr_exact_candidate_checked_bytes_add(
        larch::estimate_chart_spr_state_core_resident_bytes(state),
        larch::estimate_chart_spr_selected_cache_dynamic_resident_bytes(state),
        "test published-state core/cache identity");
    if (state.exact_trim_active_only) {
      auto const dynamic =
          larch::estimate_chart_spr_trim_dynamic_resident_bytes(
              *state.exact_trim_active_only);
      expected = larch::chart_spr_exact_candidate_checked_bytes_add(
          expected, dynamic, "test published-state trim identity");
      CHECK(larch::estimate_chart_spr_trim_resident_bytes(
                *state.exact_trim_active_only) ==
            sizeof(larch::multisite_trim_result) + dynamic);
    }
    CHECK(larch::estimate_chart_spr_published_state_resident_bytes(state) ==
          expected);
  };

  larch::chart_cache_options all_active;
  larch::chart_cache_options lazy;
  lazy.use_lazy_multisite_chart = true;
  larch::chart_cache_options pattern_batch;
  pattern_batch.max_cached_patterns = 1;
  constexpr std::array expected_strategies{
      larch::chart_spr_cache_strategy::all_active_patterns,
      larch::chart_spr_cache_strategy::lazy_multisite_chart,
      larch::chart_spr_cache_strategy::pattern_batches,
  };
  std::array const caches{all_active, lazy, pattern_batch};
  for (std::size_t index = 0; index < caches.size(); ++index) {
    auto const& cache = caches[index];
    auto without_trim = make_state(cache, false);
    auto with_trim = make_state(cache, true);
    CHECK(without_trim.cache_strategy == expected_strategies[index]);
    CHECK(with_trim.cache_strategy == expected_strategies[index]);
    CHECK(!without_trim.exact_trim_active_only.has_value());
    CHECK(with_trim.exact_trim_active_only.has_value());
    check_identity(without_trim);
    check_identity(with_trim);
  }
  // The pattern-batch term above is deliberately an admitted future batch
  // reservation, not a claim that empty pattern_charts owns those bytes now.

  // Heap-owned all-active entry slots belong to the selected cache, not the
  // cache-independent state core, and therefore enter the sum exactly once.
  larch::chart_spr_search_state dense_slots;
  dense_slots.cache_strategy =
      larch::chart_spr_cache_strategy::all_active_patterns;
  auto const dense_core_before =
      larch::estimate_chart_spr_state_core_resident_bytes(dense_slots);
  auto const dense_published_before =
      larch::estimate_chart_spr_published_state_resident_bytes(dense_slots);
  dense_slots.pattern_charts.reserve(7);
  dense_slots.resident_pattern_cache_bytes =
      larch::estimate_chart_spr_pattern_cache_bytes(dense_slots);
  auto const dense_slot_bytes = dense_slots.pattern_charts.capacity() *
                                sizeof(larch::pattern_chart_cache_entry);
  CHECK(larch::estimate_chart_spr_state_core_resident_bytes(dense_slots) ==
        dense_core_before);
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(dense_slots) -
            dense_published_before ==
        dense_slot_bytes);

  // Fixed optional payload is already inline in sizeof(state). Engaging an
  // empty lazy chart or trim changes no bytes; only their heap capacities do.
  larch::chart_spr_search_state optionals;
  auto const empty_state_bytes =
      larch::estimate_chart_spr_published_state_resident_bytes(optionals);
  optionals.cache_strategy =
      larch::chart_spr_cache_strategy::lazy_multisite_chart;
  optionals.lazy_chart.emplace();
  optionals.resident_pattern_cache_bytes =
      larch::estimate_chart_spr_pattern_cache_bytes(optionals);
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(optionals) ==
        empty_state_bytes);
  optionals.lazy_chart->inside_rows_by_clade.reserve(5);
  optionals.resident_pattern_cache_bytes =
      larch::estimate_chart_spr_pattern_cache_bytes(optionals);
  auto const lazy_dynamic =
      optionals.lazy_chart->inside_rows_by_clade.capacity() *
      sizeof(decltype(optionals.lazy_chart->inside_rows_by_clade)::value_type);
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(optionals) -
            empty_state_bytes ==
        lazy_dynamic);
  auto const before_empty_trim =
      larch::estimate_chart_spr_published_state_resident_bytes(optionals);
  optionals.exact_trim_active_only.emplace();
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(optionals) ==
        before_empty_trim);
  optionals.exact_trim_active_only->frontier_sizes_by_clade.reserve(9);
  auto const trim_dynamic =
      larch::estimate_chart_spr_trim_dynamic_resident_bytes(
          *optionals.exact_trim_active_only);
  CHECK(trim_dynamic ==
        optionals.exact_trim_active_only->frontier_sizes_by_clade.capacity() *
            sizeof(std::size_t));
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(optionals) -
            before_empty_trim ==
        trim_dynamic);

  // Local-commit ownership is already folded into the resident-cache
  // aggregate. Simulate its publication on a real lazy scoring state and
  // prove the persistent contribution appears once, not once per field.
  auto local_commit = make_state(lazy, false);
  CHECK(local_commit.lazy_chart.has_value());
  auto const cache_before =
      larch::estimate_chart_spr_selected_cache_dynamic_resident_bytes(
          local_commit);
  auto const published_before =
      larch::estimate_chart_spr_published_state_resident_bytes(local_commit);
  constexpr auto persistent_bytes = std::size_t{16384};
  local_commit.local_commit_persistent_cache_bytes = persistent_bytes;
  local_commit.resident_pattern_cache_bytes =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          local_commit.resident_pattern_cache_bytes, persistent_bytes,
          "test local-commit published cache");
  CHECK(larch::estimate_chart_spr_selected_cache_dynamic_resident_bytes(
            local_commit) == cache_before + persistent_bytes);
  CHECK(larch::estimate_chart_spr_selected_cache_dynamic_resident_bytes(
            local_commit) == local_commit.resident_pattern_cache_bytes -
                                 sizeof(larch::lazy_multisite_chart));
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(local_commit) -
            published_before ==
        persistent_bytes);
  check_identity(local_commit);

  // Opaque callback targets are one persistent state component. An installed
  // target without a declaration fails closed; declaration adds its aggregate
  // exactly once, and a same-cardinality replacement invalidates the stale
  // mutation-token snapshot until it is explicitly re-declared.
  larch::chart_spr_search_state callbacks;
  auto const callback_core_before =
      larch::estimate_chart_spr_state_core_resident_bytes(callbacks);
  auto callback_owner = std::make_shared<std::size_t>(7);
  callbacks.exact_setup_provider =
      [callback_owner](larch::chart_spr_search_state const&,
                       larch::checked_chart_execution_plan_ref const&) {
        (void)callback_owner;
        return larch::multisite_exact_setup{};
      };
  bool missing_contract_rejected = false;
  try {
    (void)larch::estimate_chart_spr_state_core_resident_bytes(callbacks);
  } catch (std::runtime_error const& error) {
    missing_contract_rejected =
        std::string_view{error.what()}.contains("callback targets");
  }
  CHECK(missing_contract_rejected);
  constexpr auto callback_bytes = std::size_t{12345};
  larch::declare_chart_spr_state_callback_target_resident_bytes(callbacks,
                                                                callback_bytes);
  CHECK(larch::estimate_chart_spr_state_core_resident_bytes(callbacks) ==
        callback_core_before + callback_bytes);
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(callbacks) ==
        callback_core_before + callback_bytes);

  callbacks.exact_setup_provider =
      [](larch::chart_spr_search_state const&,
         larch::checked_chart_execution_plan_ref const&) {
        return larch::multisite_exact_setup{};
      };
  bool stale_replacement_rejected = false;
  try {
    (void)larch::estimate_chart_spr_state_core_resident_bytes(callbacks);
  } catch (std::runtime_error const& error) {
    stale_replacement_rejected =
        std::string_view{error.what()}.contains("callback targets");
  }
  CHECK(stale_replacement_rejected);
  constexpr auto replacement_bytes = std::size_t{321};
  larch::declare_chart_spr_state_callback_target_resident_bytes(
      callbacks, replacement_bytes);
  auto const callback_boundary = callback_core_before + replacement_bytes;
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(callbacks) ==
        callback_boundary);
  larch::multisite_trim_result empty_trim;
  larch::require_chart_spr_retained_exact_state_memory_budget(
      callbacks, empty_trim, callback_boundary);
  bool callback_one_under_rejected = false;
  try {
    larch::require_chart_spr_retained_exact_state_memory_budget(
        callbacks, empty_trim, callback_boundary - 1);
  } catch (larch::chart_spr_exact_state_budget_error const& error) {
    callback_one_under_rejected = true;
    CHECK(error.required_bytes() == callback_boundary);
  }
  CHECK(callback_one_under_rejected);

  std::println("  PASS");
}

static void test_lazy_exact_semantic_dynamic_evidence_boundary() {
  std::println("test_lazy_exact_semantic_dynamic_evidence_boundary");

  auto fixture = make_fixture();
  for (std::size_t taxon = 0;
       taxon < fixture.grammar.taxa.id_to_sample_id.size(); ++taxon) {
    fixture.grammar.taxa.id_to_sample_id[taxon] =
        std::string(10000, static_cast<char>('a' + taxon % 26)) + "-" +
        std::to_string(taxon);
  }
  fixture.grammar.taxa.sample_id_to_id.clear();
  fixture.grammar.taxa.sample_id_to_id.reserve(
      fixture.grammar.taxa.id_to_sample_id.size());
  for (std::size_t taxon = 0;
       taxon < fixture.grammar.taxa.id_to_sample_id.size(); ++taxon) {
    fixture.grammar.taxa.sample_id_to_id.emplace(
        fixture.grammar.taxa.id_to_sample_id[taxon],
        static_cast<larch::taxon_id>(taxon));
  }

  larch::chart_spr_search_options base_options;
  base_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  base_options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  base_options.top_k_exact_verify = 1;
  base_options.cache.use_lazy_multisite_chart = true;
  base_options.cache.candidate_batch_size = 1;
  base_options.enumeration.max_candidates = 1;
  base_options.enumeration.max_candidates_is_post_dedup = true;
  base_options.semantic_capture =
      larch::chart_spr_semantic_capture_mode::digest;
  base_options.verification_mode =
      larch::chart_spr_verification_mode::transient;
  larch::configure_chart_spr_primary_exact_provenance(base_options);
  CHECK(base_options.exact_trim.max_frontier_entries_per_clade == 0);

  auto make_scheduler = [] {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = 4,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 1,
        });
  };
  auto make_state = [&](std::size_t budget) {
    auto options = base_options;
    options.cache.memory_budget_bytes = budget;
    auto active = larch::make_active_search_patterns(fixture.patterns);
    auto state = larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), options.chart,
        /*build_exact_trim=*/true, options.exact_trim, options.cache);
    state.contextual_exact_multisite_verifier =
        [](larch::chart_spr_search_state const& verifier_state,
           larch::chart_spr_candidate_score candidate,
           larch::checked_chart_execution_plan_ref const& checked,
           larch::chart_spr_exact_verification_context& context,
           larch::multisite_trim_options const& trim_options) {
          return larch::verify_candidate_exact_against_state_impl(
              verifier_state, std::move(candidate), checked, context,
              trim_options);
        };
    state.exact_multisite_transient_memory_estimator =
        [](larch::grammar_spr_candidate const&) { return std::size_t{0}; };
    state.exact_multisite_transient_retained_memory_estimator =
        [](larch::grammar_spr_candidate const&) {
          // Make the post-ranking evidence phase unambiguously dominant while
          // retaining the ordinary exact-evidence verifier result.
          return std::size_t{128} << 20;
        };
    state.exact_multisite_verifier_parallel_safe = true;
    larch::declare_chart_spr_state_callback_target_resident_bytes(state, 0);
    return state;
  };

  constexpr auto calibration_budget = std::size_t{1} << 40;
  auto calibration_state = make_state(calibration_budget);
  auto calibration_scheduler = make_scheduler();
  auto calibration_options = base_options;
  calibration_options.cache.memory_budget_bytes = calibration_budget;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      calibration_workspace;
  auto calibration = larch::run_chart_spr_acceptance_iteration(
      calibration_state, calibration_options, 0, calibration_workspace,
      *calibration_scheduler);
  CHECK(calibration.candidates_generated == 1);
  CHECK(calibration.locally_ranked_candidates_retained == 1);
  CHECK(calibration.candidates_exact_verified == 1);
  auto const exact_boundary =
      calibration_state.counters.lazy_local_iteration_envelope_bytes_max;
  auto const generation_phase =
      calibration_state.counters
          .lazy_local_iteration_generation_phase_bytes_max;
  auto const evidence_phase =
      calibration_state.counters.lazy_local_iteration_evidence_phase_bytes_max;
  auto const ranked_evidence =
      calibration_state.counters
          .lazy_local_ranked_candidate_exact_evidence_bytes_max;
  CHECK(ranked_evidence > 0);
  CHECK(evidence_phase > generation_phase);
  CHECK(evidence_phase >= ranked_evidence);
  CHECK(exact_boundary == std::max(generation_phase, evidence_phase));
  CHECK(exact_boundary < larch::chart_spr_exact_candidate_checked_bytes_add(
                             generation_phase, evidence_phase,
                             "test dynamic temporal phase sum"));
  calibration_scheduler->shutdown();

  auto fit_state = make_state(exact_boundary);
  auto fit_scheduler = make_scheduler();
  auto fit_options = base_options;
  fit_options.cache.memory_budget_bytes = exact_boundary;
  auto fit_calls = std::make_shared<std::atomic<std::size_t>>(0);
  fit_options.before_exact_candidate_verification_for_tests =
      [fit_calls](std::size_t) {
        fit_calls->fetch_add(1, std::memory_order_relaxed);
      };
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      fit_workspace;
  auto fit = larch::run_chart_spr_acceptance_iteration(
      fit_state, fit_options, 1, fit_workspace, *fit_scheduler);
  CHECK(fit.candidates_exact_verified == 1);
  CHECK(fit_calls->load(std::memory_order_relaxed) == 1);
  CHECK(fit_state.counters.lazy_local_iteration_envelope_bytes_max ==
        exact_boundary);
  CHECK(
      fit_state.counters.lazy_local_ranked_candidate_exact_evidence_bytes_max >
      0);
  fit_scheduler->shutdown();

  auto reject_state = make_state(exact_boundary - 1);
  auto reject_scheduler = make_scheduler();
  auto reject_options = base_options;
  reject_options.cache.memory_budget_bytes = exact_boundary - 1;
  auto reject_calls = std::make_shared<std::atomic<std::size_t>>(0);
  reject_options.before_exact_candidate_verification_for_tests =
      [reject_calls](std::size_t) {
        reject_calls->fetch_add(1, std::memory_order_relaxed);
      };
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      reject_workspace;
  auto const exact_verifications_before =
      reject_state.counters.exact_verifications;
  auto const exact_setup_axis_before =
      reject_state.counters.scheduler_axes.exact_setup_patterns;
  auto const exact_frontier_axis_before =
      reject_state.counters.scheduler_axes.exact_frontier_clades;
  auto const exact_candidate_axis_before =
      reject_state.counters.scheduler_axes.exact_candidates;
  bool rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        reject_state, reject_options, 2, reject_workspace, *reject_scheduler);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          error) {
    rejected = true;
    CHECK(error.required_bytes() == exact_boundary);
    CHECK(error.available_bytes() == exact_boundary - 1);
  }
  CHECK(rejected);
  CHECK(reject_state.counters.candidates_generated_after_dedup > 0);
  CHECK(reject_state.counters
            .lazy_local_ranked_candidate_exact_evidence_bytes_max > 0);
  CHECK(reject_calls->load(std::memory_order_relaxed) == 0);
  CHECK(reject_state.counters.exact_verifications ==
        exact_verifications_before);
  CHECK(reject_state.counters.scheduler_axes.exact_setup_patterns ==
        exact_setup_axis_before);
  CHECK(reject_state.counters.scheduler_axes.exact_frontier_clades ==
        exact_frontier_axis_before);
  CHECK(reject_state.counters.scheduler_axes.exact_candidates ==
        exact_candidate_axis_before);
  CHECK(reject_state.counters
            .lazy_local_canonical_exact_evidence_resident_bytes_max == 0);
  reject_scheduler->shutdown();

  std::println("  PASS");
}

static void test_finite_lazy_search_summary_reports_temporal_envelope() {
  std::println(
      "test_finite_lazy_search_summary_reports_temporal_envelope");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.max_candidates_per_iteration = 8;
  options.cache.use_lazy_multisite_chart = true;
  options.cache.candidate_batch_size = 1;
  options.cache.memory_budget_bytes = std::size_t{1} << 40;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;

  auto search =
      larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().candidates_exact_verified > 0);

  auto const& counters = search.counters;
  auto const& summary = search.summary;
  CHECK(counters.lazy_local_iteration_generation_phase_bytes_max > 0);
  CHECK(counters.lazy_local_iteration_evidence_phase_bytes_max > 0);
  CHECK(counters.lazy_local_ranked_candidate_exact_evidence_bytes_max > 0);
  CHECK(summary.lazy_local_iteration_generation_phase_bytes_max ==
        counters.lazy_local_iteration_generation_phase_bytes_max);
  CHECK(summary.lazy_local_iteration_evidence_phase_bytes_max ==
        counters.lazy_local_iteration_evidence_phase_bytes_max);
  CHECK(summary.lazy_local_ranked_candidate_exact_evidence_bytes_max ==
        counters.lazy_local_ranked_candidate_exact_evidence_bytes_max);
  CHECK(summary.lazy_local_iteration_envelope_bytes_max ==
        counters.lazy_local_iteration_envelope_bytes_max);
  CHECK(summary.lazy_local_iteration_envelope_bytes_max ==
        std::max(summary.lazy_local_iteration_generation_phase_bytes_max,
                 summary.lazy_local_iteration_evidence_phase_bytes_max));
  CHECK(summary.lazy_local_iteration_evidence_phase_bytes_max >=
        summary.lazy_local_ranked_candidate_exact_evidence_bytes_max);

  std::println("  PASS");
}

static void test_lazy_exact_w4_combined_envelope_boundary() {
  std::println("test_lazy_exact_w4_combined_envelope_boundary");

  auto fixture = make_fixture();
  CHECK(fixture.candidates.size() >= 4);
  larch::chart_spr_search_options base_options;
  base_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  base_options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  base_options.top_k_exact_verify = 1;
  base_options.cache.use_lazy_multisite_chart = true;
  base_options.cache.candidate_batch_size = 4;
  base_options.enumeration.max_candidates = 4;
  base_options.enumeration.max_candidates_is_post_dedup = true;
  // Keep this boundary focused on the combined lazy-local/exact envelope.
  // Grammar-wave admission has an independent exact-boundary matrix and would
  // otherwise legitimately reduce its width at E-1 before rejecting.
  base_options.enumeration.grammar_candidate_maximum_wave_size = 1;

  auto make_scheduler = [] {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = 4,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 1,
        });
  };
  auto make_state = [&](std::size_t budget, larch::chart_scheduler& scheduler) {
    auto options = base_options;
    options.cache.memory_budget_bytes = budget;
    auto active = larch::make_active_search_patterns(
        fixture.dag, fixture.grammar, options.chart);
    auto state = larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), options.chart,
        /*build_exact_trim=*/true, options.exact_trim, options.cache, {},
        &scheduler);
    CHECK(state.cache_strategy ==
          larch::chart_spr_cache_strategy::lazy_multisite_chart);
    CHECK(state.exact_trim_active_only.has_value());
    CHECK(state.counters.lazy_chart_preflight_peak_bytes <= budget);
    CHECK(state.counters.lazy_chart_actual_peak_bytes <= budget);
    CHECK(larch::estimate_chart_spr_published_state_resident_bytes(state) <=
          budget);
    return state;
  };
  auto envelope_for = [&](larch::chart_spr_search_state const& state,
                          larch::chart_scheduler const& scheduler) {
    return larch::chart_spr_search_detail::
        estimate_grammar_spr_finite_iteration_memory_envelope(
            state, base_options.enumeration.max_candidates,
            base_options.cache.candidate_batch_size,
            base_options.top_k_exact_verify, false, scheduler, 4,
            &base_options.enumeration,
            /*candidate_buffer_count=*/2,
            /*include_pipeline_control=*/true);
  };

  constexpr auto calibration_budget = std::size_t{1} << 40;
  auto calibration_scheduler = make_scheduler();
  auto calibration = make_state(calibration_budget, *calibration_scheduler);
  auto const calibration_envelope =
      envelope_for(calibration, *calibration_scheduler);
  auto const exact_budget = calibration_envelope.planned_required_bytes;
  CHECK(exact_budget > 1);
  CHECK(
      calibration_envelope.planned_scheduler_resident_bytes ==
      larch::chart_spr_search_detail::
          estimate_chart_spr_scheduler_resident_bytes(*calibration_scheduler));
  auto const local_range_options = larch::chart_indexed_range_options{
      .minimum_grain = 1, .target_ranges_per_worker = 1};
  auto const local_range_plan =
      calibration_scheduler->plan_indexed_ranges(4, local_range_options);
  CHECK(calibration_envelope.planned_scheduler_operation_peak_bytes ==
        larch::estimate_chart_scheduler_operation_peak_bytes(local_range_plan));
  CHECK(calibration_envelope.planned_scheduler_operation_peak_bytes > 0);

  auto fit_scheduler = make_scheduler();
  auto fit_state = make_state(exact_budget, *fit_scheduler);
  auto const fit_envelope = envelope_for(fit_state, *fit_scheduler);
  CHECK(fit_envelope.planned_required_bytes == exact_budget);
  auto fit_options = base_options;
  fit_options.cache.memory_budget_bytes = exact_budget;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      fit_workspace;
  auto const prepared_before = fit_state.counters.lazy_local_prepared_tasks;
  auto const fit_metrics_before = fit_scheduler->metrics();
  auto const fit_local_axis_before =
      fit_state.counters.scheduler_axes.local_score_candidates;
  auto const fit_exact_verifications_before =
      fit_state.counters.exact_verifications;
  auto fit = larch::run_chart_spr_acceptance_iteration(
      fit_state, fit_options, 0, fit_workspace, *fit_scheduler);
  CHECK(fit.candidates_generated == 4);
  CHECK(fit.candidates_scored == 4);
  CHECK(fit.candidates_exact_verified == 1);
  CHECK(fit_state.counters.lazy_local_prepared_tasks > prepared_before);
  CHECK(fit_state.counters.scheduler_axes.local_score_candidates.operations >
        fit_local_axis_before.operations);
  CHECK(fit_state.counters.exact_verifications >
        fit_exact_verifications_before);
  CHECK(fit_scheduler->metrics().operations > fit_metrics_before.operations);
  CHECK(fit_scheduler->metrics().tasks_submitted >
        fit_metrics_before.tasks_submitted);
  CHECK(fit_state.counters.lazy_local_pre_submit_budget_failures == 0);
  CHECK(fit_state.counters.lazy_local_peak_projected_resident_bytes <=
        exact_budget);
  CHECK(fit_state.counters.lazy_local_preparation_peak_bytes <= exact_budget);
  CHECK(fit_state.counters.exact_candidate_peak_projected_resident_bytes <=
        exact_budget);
  CHECK(fit_workspace.local_score.operation_boundary_clean());
  fit_scheduler->shutdown();

  // The state itself fits at E-1. The combined state+iteration gate then
  // rejects with the exact E before candidate generation, local preparation,
  // workspace reserve, any additional pool construction, or submission.
  auto reject_scheduler = make_scheduler();
  auto reject_state = make_state(exact_budget - 1, *reject_scheduler);
  CHECK(reject_state.counters.lazy_chart_preflight_peak_bytes <=
        exact_budget - 1);
  CHECK(reject_state.counters.lazy_chart_actual_peak_bytes <= exact_budget - 1);
  auto const reject_envelope = envelope_for(reject_state, *reject_scheduler);
  CHECK(reject_envelope.planned_required_bytes == exact_budget);
  auto reject_options = base_options;
  reject_options.cache.memory_budget_bytes = exact_budget - 1;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      reject_workspace;
  auto const reject_prepared_before =
      reject_state.counters.lazy_local_prepared_tasks;
  auto const reject_batches_before =
      reject_state.counters.candidate_batches_scored;
  auto const reject_generated_before =
      reject_state.counters.candidates_generated_after_dedup;
  auto const reject_scheduler_axes_before =
      larch::chart_spr_scheduler_axis_operation_count(
          reject_state.counters.scheduler_axes);
  auto const reject_metrics_before = reject_scheduler->metrics();
  bool rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        reject_state, reject_options, 1, reject_workspace, *reject_scheduler);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    rejected = true;
    CHECK(e.candidate_index() == 0);
    CHECK(e.required_bytes() == exact_budget);
    CHECK(e.available_bytes() == exact_budget - 1);
  }
  CHECK(rejected);
  CHECK(reject_state.counters.lazy_local_prepared_tasks ==
        reject_prepared_before);
  CHECK(reject_state.counters.candidate_batches_scored ==
        reject_batches_before);
  CHECK(reject_state.counters.candidates_generated_after_dedup ==
        reject_generated_before);
  CHECK(larch::chart_spr_scheduler_axis_operation_count(
            reject_state.counters.scheduler_axes) ==
        reject_scheduler_axes_before);
  auto const reject_metrics_after = reject_scheduler->metrics();
  CHECK(reject_metrics_after.operations == reject_metrics_before.operations);
  CHECK(reject_metrics_after.tasks_submitted ==
        reject_metrics_before.tasks_submitted);
  CHECK(reject_metrics_after.pool_lifetimes ==
        reject_metrics_before.pool_lifetimes);
  CHECK(reject_metrics_after.pending_tasks == 0);
  CHECK(reject_workspace.candidate_slots.capacity() == 0);
  CHECK(reject_workspace.candidate_copy_scratch.capacity() == 0);
  CHECK(reject_workspace.local_results.capacity() == 0);
  CHECK(reject_workspace.local_score.operation_boundary_clean());
  reject_scheduler->shutdown();
  calibration_scheduler->shutdown();

  std::println("  PASS");
}

static void test_scheduled_exact_state_w4_boundary() {
  std::println("test_scheduled_exact_state_w4_boundary");

  auto fixture = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(fixture);
  auto patterns = larch::build_site_patterns(fixture, grammar);
  auto make_state = [&] {
    auto active = larch::make_active_search_patterns(patterns);
    return larch::build_chart_spr_search_state_from_active(
        fixture, grammar, std::move(active), {},
        /*build_exact_trim=*/false);
  };
  auto make_scheduler = [] {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = 4,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };

  auto calibration_state = make_state();
  CHECK(calibration_state.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);
  CHECK(!calibration_state.exact_trim_active_only.has_value());
  auto calibration_scheduler = make_scheduler();
  auto const resident_base = larch::chart_spr_exact_candidate_checked_bytes_add(
      larch::estimate_chart_spr_published_state_resident_bytes(
          calibration_state),
      larch::chart_spr_search_detail::
          estimate_chart_spr_scheduler_resident_bytes(*calibration_scheduler),
      "scheduled exact-state test resident base");
  larch::chart_spr_candidate_score identity_candidate;
  auto const estimator_scratch = larch::chart_spr_search_detail::
      estimate_exact_loop_estimator_peak_scratch_bytes(
          calibration_state, std::span<larch::chart_spr_candidate_score const>{
                                 &identity_candidate, 1});
  auto const exact_estimate = larch::estimate_chart_spr_state_exact_memory(
      calibration_state, {}, 4, std::size_t{1} << 40);
  CHECK(exact_estimate.safely_bounded);
  auto construction_scratch =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          exact_estimate.inner_parallel_scratch_bytes,
          larch::chart_spr_search_detail::
              estimate_chart_spr_state_exact_scheduler_operation_peak_bytes(
                  calibration_state, *calibration_scheduler),
          "scheduled exact-state test operation scratch");
  construction_scratch = larch::chart_spr_exact_candidate_checked_bytes_add(
      construction_scratch,
      larch::chart_spr_search_detail::
          estimate_chart_spr_state_exact_scheduler_summary_bytes(
              calibration_state, {}),
      "scheduled exact-state test summary scratch");
  auto const exact_boundary =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          resident_base, std::max(estimator_scratch, construction_scratch),
          "scheduled exact-state test boundary");
  CHECK(exact_boundary > 1);

  // The construction envelope must also dominate the actual retained trim
  // backstop, or S-1 could start scheduler work and fail only after building.
  auto const calibration_checked = larch::check_chart_execution_plan(
      calibration_state.grammar, calibration_state.execution_plan);
  auto const& calibration_trim = larch::ensure_chart_spr_state_exact_trim(
      calibration_state, calibration_checked, *calibration_scheduler, {});
  auto retained_required = larch::chart_spr_exact_candidate_checked_bytes_add(
      larch::estimate_chart_spr_state_core_resident_bytes(calibration_state),
      larch::estimate_chart_spr_selected_cache_dynamic_resident_bytes(
          calibration_state),
      "scheduled exact-state test retained core/cache");
  retained_required = larch::chart_spr_exact_candidate_checked_bytes_add(
      retained_required,
      larch::estimate_chart_spr_trim_dynamic_resident_bytes(calibration_trim),
      "scheduled exact-state test retained trim");
  retained_required = larch::chart_spr_exact_candidate_checked_bytes_add(
      retained_required,
      larch::chart_spr_search_detail::
          estimate_chart_spr_scheduler_resident_bytes(*calibration_scheduler),
      "scheduled exact-state test retained scheduler");
  CHECK(retained_required <= exact_boundary);

  auto fit_state = make_state();
  fit_state.cache_opts.memory_budget_bytes = exact_boundary;
  auto fit_scheduler = make_scheduler();
  auto fit_checked = larch::check_chart_execution_plan(
      fit_state.grammar, fit_state.execution_plan);
  auto const fit_metrics_before = fit_scheduler->metrics();
  auto const fit_axes_before = fit_state.counters.scheduler_axes;
  (void)larch::ensure_chart_spr_state_exact_trim(fit_state, fit_checked,
                                                 *fit_scheduler, {});
  CHECK(fit_state.exact_trim_active_only.has_value());
  CHECK(fit_scheduler->metrics().operations > fit_metrics_before.operations);
  CHECK(fit_state.counters.scheduler_axes.exact_setup_patterns.operations >
        fit_axes_before.exact_setup_patterns.operations);
  CHECK(fit_state.counters.scheduler_axes.exact_frontier_clades.operations >
        fit_axes_before.exact_frontier_clades.operations);

  auto reject_state = make_state();
  reject_state.cache_opts.memory_budget_bytes = exact_boundary - 1;
  auto reject_scheduler = make_scheduler();
  auto reject_checked = larch::check_chart_execution_plan(
      reject_state.grammar, reject_state.execution_plan);
  auto const reject_metrics_before = reject_scheduler->metrics();
  auto const reject_axes_before = reject_state.counters.scheduler_axes;
  bool rejected = false;
  try {
    (void)larch::ensure_chart_spr_state_exact_trim(reject_state, reject_checked,
                                                   *reject_scheduler, {});
  } catch (larch::chart_spr_exact_state_budget_error const& error) {
    rejected = true;
    CHECK(error.required_bytes() == exact_boundary);
    CHECK(error.budget_bytes() == exact_boundary - 1);
  }
  CHECK(rejected);
  CHECK(!reject_state.exact_trim_active_only.has_value());
  CHECK(reject_scheduler->metrics().operations ==
        reject_metrics_before.operations);
  CHECK(reject_scheduler->metrics().tasks_submitted ==
        reject_metrics_before.tasks_submitted);
  CHECK(reject_state.counters.scheduler_axes == reject_axes_before);

  larch::multisite_trim_options two_pass_options;
  two_pass_options.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  auto const& level_offsets =
      calibration_state.execution_plan.bottom_up_level_offsets();
  auto const level_count = level_offsets.empty() ? 0 : level_offsets.size() - 1;
  auto const one_pass_summaries = larch::chart_spr_search_detail::
      estimate_chart_spr_state_exact_scheduler_summary_bytes(calibration_state,
                                                             {});
  auto const two_pass_summaries = larch::chart_spr_search_detail::
      estimate_chart_spr_state_exact_scheduler_summary_bytes(calibration_state,
                                                             two_pass_options);
  CHECK(two_pass_summaries ==
        (2 + 2 * level_count) * sizeof(larch::chart_scheduler_run_summary));
  CHECK(two_pass_summaries - one_pass_summaries ==
        level_count * sizeof(larch::chart_scheduler_run_summary));

  auto const two_pass_estimate = larch::estimate_chart_spr_state_exact_memory(
      make_state(), two_pass_options, 4, std::size_t{1} << 40);
  CHECK(two_pass_estimate.safely_bounded);
  auto const expected_diagnostic_delta =
      calibration_state.grammar.clades.size() *
      sizeof(larch::multisite_frontier_level_diagnostic);
  CHECK(two_pass_estimate.serial_scratch_bytes -
            exact_estimate.serial_scratch_bytes ==
        expected_diagnostic_delta);
  CHECK(two_pass_estimate.inner_parallel_scratch_bytes -
            exact_estimate.inner_parallel_scratch_bytes ==
        expected_diagnostic_delta);
  auto two_pass_construction =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          two_pass_estimate.inner_parallel_scratch_bytes,
          larch::chart_spr_search_detail::
              estimate_chart_spr_state_exact_scheduler_operation_peak_bytes(
                  calibration_state, *calibration_scheduler),
          "scheduled two-pass exact-state operation scratch");
  two_pass_construction = larch::chart_spr_exact_candidate_checked_bytes_add(
      two_pass_construction, two_pass_summaries,
      "scheduled two-pass exact-state summary scratch");
  auto const two_pass_boundary =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          resident_base, std::max(estimator_scratch, two_pass_construction),
          "scheduled two-pass exact-state boundary");
  auto two_pass_fit_state = make_state();
  two_pass_fit_state.cache_opts.memory_budget_bytes = two_pass_boundary;
  auto two_pass_fit_scheduler = make_scheduler();
  auto two_pass_fit_checked = larch::check_chart_execution_plan(
      two_pass_fit_state.grammar, two_pass_fit_state.execution_plan);
  (void)larch::ensure_chart_spr_state_exact_trim(
      two_pass_fit_state, two_pass_fit_checked, *two_pass_fit_scheduler,
      two_pass_options);
  CHECK(two_pass_fit_state.exact_trim_active_only.has_value());
  CHECK(two_pass_fit_state.counters.scheduler_axes.exact_frontier_clades
            .operations >= 2 * level_count);

  auto two_pass_reject_state = make_state();
  two_pass_reject_state.cache_opts.memory_budget_bytes = two_pass_boundary - 1;
  auto two_pass_reject_scheduler = make_scheduler();
  auto two_pass_reject_checked = larch::check_chart_execution_plan(
      two_pass_reject_state.grammar, two_pass_reject_state.execution_plan);
  auto const two_pass_reject_metrics_before =
      two_pass_reject_scheduler->metrics();
  auto const two_pass_reject_axes_before =
      two_pass_reject_state.counters.scheduler_axes;
  bool two_pass_rejected = false;
  try {
    (void)larch::ensure_chart_spr_state_exact_trim(
        two_pass_reject_state, two_pass_reject_checked,
        *two_pass_reject_scheduler, two_pass_options);
  } catch (larch::chart_spr_exact_state_budget_error const& error) {
    two_pass_rejected = true;
    CHECK(error.required_bytes() == two_pass_boundary);
  }
  CHECK(two_pass_rejected);
  CHECK(!two_pass_reject_state.exact_trim_active_only.has_value());
  CHECK(two_pass_reject_scheduler->metrics().operations ==
        two_pass_reject_metrics_before.operations);
  CHECK(two_pass_reject_state.counters.scheduler_axes ==
        two_pass_reject_axes_before);

  auto candidate_fixture = make_fixture();
  auto candidate_state = larch::build_chart_spr_search_state(
      candidate_fixture.dag, candidate_fixture.grammar,
      candidate_fixture.patterns);
  auto candidate_score = larch::score_candidate_locally(
      candidate_state, candidate_fixture.candidates.front());
  larch::chart_spr_search_options one_pass_candidate_options;
  one_pass_candidate_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  auto two_pass_candidate_options = one_pass_candidate_options;
  two_pass_candidate_options.exact_trim = two_pass_options;
  auto const one_pass_candidate_scheduler_scratch =
      larch::chart_spr_search_detail::
          estimate_chart_spr_exact_candidate_inner_scheduler_scratch_bytes(
              candidate_state, candidate_score, one_pass_candidate_options,
              *calibration_scheduler);
  auto const two_pass_candidate_scheduler_scratch =
      larch::chart_spr_search_detail::
          estimate_chart_spr_exact_candidate_inner_scheduler_scratch_bytes(
              candidate_state, candidate_score, two_pass_candidate_options,
              *calibration_scheduler);
  auto const candidate_clade_count =
      candidate_state.grammar.clades.size() +
      candidate_score.candidate.added_clades.size();
  CHECK(two_pass_candidate_scheduler_scratch -
            one_pass_candidate_scheduler_scratch ==
        candidate_clade_count * sizeof(larch::chart_scheduler_run_summary));

  calibration_scheduler->shutdown();
  fit_scheduler->shutdown();
  reject_scheduler->shutdown();
  two_pass_fit_scheduler->shutdown();
  two_pass_reject_scheduler->shutdown();
  std::println("  PASS");
}

static void test_pattern_batch_cache_options_match_all_cache() {
  std::println("test_pattern_batch_cache_options_match_all_cache");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 2);

  auto all_state = larch::build_chart_spr_search_state(dag, grammar);
  CHECK(all_state.active_patterns.patterns.patterns.size() >= 2);
  CHECK(all_state.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);

  larch::chart_spr_search_options options;
  options.cache.max_cached_patterns = 1;
  options.cache.candidate_batch_size = 2;
  auto batched_state = larch::build_chart_spr_search_state(
      dag, grammar, options);
  CHECK(batched_state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(batched_state.pattern_charts.empty());
  CHECK(batched_state.effective_pattern_batch_size == 1);
  CHECK(batched_state.composite_lower_bound_with_invariants ==
        all_state.composite_lower_bound_with_invariants);

  std::vector<larch::grammar_spr_candidate> subset{
      candidates[0], candidates[1]};
  auto expected_candidate_partitions = std::size_t{0};
  auto expected_candidate_descriptors = std::size_t{0};
  for (auto const& candidate : subset) {
    auto delta = larch::build_spr_overlay_delta(
        all_state.grammar, all_state.execution_plan, candidate);
    CHECK(delta.base == &all_state.grammar);
    CHECK(delta.base_plan_generation ==
          all_state.execution_plan.grammar_generation());
    CHECK(delta.base_plan_fingerprint == all_state.execution_plan.fingerprint());
    CHECK(delta.compiled_rows.size() == delta.affected_order.size());
    CHECK(delta.candidate_plan_build_stats.candidate_partition_validations ==
          candidate.added_productions.size());
    CHECK(delta.candidate_plan_build_stats.clade_order_sorts == 1);
    CHECK(delta.candidate_plan_build_stats.production_descriptors_compiled ==
          delta.compiled_productions.size());
    expected_candidate_partitions += candidate.added_productions.size();
    expected_candidate_descriptors += delta.compiled_productions.size();
  }

  auto all_counters_before = all_state.counters;
  auto all_scores = larch::score_candidates_locally(all_state, subset, {}, 1);
  auto batched_counters_before = batched_state.counters;
  auto batched_scores = larch::score_candidates_locally(
      batched_state, subset, {}, 1);
  larch::chart_spr_search_options lazy_options;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      dag, grammar, lazy_options);
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.pattern_charts.empty());
  CHECK(lazy_state.lazy_chart.has_value());
  CHECK(lazy_state.composite_lower_bound_with_invariants ==
        all_state.composite_lower_bound_with_invariants);
  auto lazy_counters_before = lazy_state.counters;
  auto lazy_scores = larch::score_candidates_locally(lazy_state, subset, {}, 1);
  CHECK(all_scores.size() == batched_scores.size());
  CHECK(all_scores.size() == lazy_scores.size());
  for (std::size_t i = 0; i < all_scores.size(); ++i) {
    auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
        grammar, patterns, subset[i]);
    CHECK(all_scores[i].valid);
    CHECK(batched_scores[i].valid);
    CHECK(lazy_scores[i].valid);
    CHECK(all_scores[i].valid == batched_scores[i].valid);
    CHECK(all_scores[i].lower_bound.value.old_score ==
          batched_scores[i].lower_bound.value.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score ==
          batched_scores[i].lower_bound.value.new_score);
    CHECK(all_scores[i].lower_bound.value.delta ==
          batched_scores[i].lower_bound.value.delta);
    CHECK(all_scores[i].affected_clade_count ==
          batched_scores[i].affected_clade_count);
    CHECK(all_scores[i].valid == lazy_scores[i].valid);
    CHECK(all_scores[i].lower_bound.value.old_score ==
          lazy_scores[i].lower_bound.value.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score ==
          lazy_scores[i].lower_bound.value.new_score);
    CHECK(all_scores[i].lower_bound.value.delta ==
          lazy_scores[i].lower_bound.value.delta);
    CHECK(all_scores[i].affected_clade_count ==
          lazy_scores[i].affected_clade_count);
    CHECK(all_scores[i].lower_bound.value.old_score == oracle.old_score);
    CHECK(all_scores[i].lower_bound.value.new_score == oracle.new_score);
    CHECK(all_scores[i].lower_bound.value.delta == oracle.delta);
  }

  auto const active_pattern_count =
      all_state.active_patterns.patterns.patterns.size();
  for (auto const& score : all_scores) {
    CHECK(score.valid);
    CHECK(score.affected_clade_count > 0);
  }
  auto const observed_row_scratch_growths =
      all_state.counters.local_row_scratch_capacity_growths -
      all_counters_before.local_row_scratch_capacity_growths;
  // reserve(n) may allocate more than n, so the number of capacity changes is
  // deliberately observed rather than inferred from record-high row counts.
  CHECK(observed_row_scratch_growths > 0);
  CHECK(observed_row_scratch_growths <= subset.size());
  auto const observed_lazy_row_scratch_growths =
      lazy_state.counters.local_row_scratch_capacity_growths -
      lazy_counters_before.local_row_scratch_capacity_growths;
  CHECK(observed_lazy_row_scratch_growths > 0);
  CHECK(observed_lazy_row_scratch_growths <= subset.size());
  auto const expected_leaf_state_views =
      subset.size() * active_pattern_count;
  CHECK(expected_leaf_state_views > 0);
  auto check_candidate_plan_counters =
      [&](auto const& state, auto const& before,
          std::size_t pattern_chart_plan_hits_during_scoring,
          std::size_t expected_leaf_views,
          std::size_t expected_scratch_growths) {
    CHECK(state.active_patterns.patterns.patterns.size() ==
          active_pattern_count);
    CHECK(state.counters.chart_execution_plan_builds ==
          before.chart_execution_plan_builds);
    CHECK(state.counters.chart_execution_plan_cache_hits ==
          before.chart_execution_plan_cache_hits + subset.size() +
              pattern_chart_plan_hits_during_scoring);
    CHECK(state.counters.candidate_execution_plan_builds ==
          before.candidate_execution_plan_builds + subset.size());
    CHECK(state.counters.candidate_execution_plan_cache_hits ==
          before.candidate_execution_plan_cache_hits +
              subset.size() * active_pattern_count);
    CHECK(state.counters.candidate_partition_validations ==
          before.candidate_partition_validations +
              expected_candidate_partitions);
    CHECK(state.counters.clade_order_sorts ==
          before.clade_order_sorts + subset.size());
    CHECK(state.counters.production_descriptors_compiled ==
          before.production_descriptors_compiled +
              expected_candidate_descriptors);
    CHECK(state.counters.plan_mismatch_rejections ==
          before.plan_mismatch_rejections);
    CHECK(state.counters.candidate_pattern_full_grammar_validations ==
          before.candidate_pattern_full_grammar_validations);
    CHECK(state.counters.candidate_pattern_partition_validations ==
          before.candidate_pattern_partition_validations);
    CHECK(state.counters.candidate_pattern_clade_order_sorts ==
          before.candidate_pattern_clade_order_sorts);
    CHECK(state.counters.full_grammar_validations ==
          before.full_grammar_validations);
    CHECK(state.counters.production_partition_validations ==
          before.production_partition_validations);
    CHECK(state.counters.dynamic_overlay_payload_partition_validations ==
          before.dynamic_overlay_payload_partition_validations);
    CHECK(state.counters.local_candidate_scores ==
          before.local_candidate_scores + subset.size());
    CHECK(active_pattern_count >= 2);
    CHECK(state.counters.local_rows_recomputed >
          before.local_rows_recomputed);
    CHECK(state.counters.local_unit_fitch_fast_path_productions_scored ==
          before.local_unit_fitch_fast_path_productions_scored +
              expected_candidate_descriptors * active_pattern_count);
    CHECK(state.counters.local_leaf_state_view_uses ==
          before.local_leaf_state_view_uses + expected_leaf_views);
    CHECK(state.counters.local_leaf_state_owned_copies ==
          before.local_leaf_state_owned_copies);
    CHECK(state.counters.local_row_scratch_capacity_growths ==
          before.local_row_scratch_capacity_growths +
              expected_scratch_growths);
  };
  check_candidate_plan_counters(
      all_state, all_counters_before, 0, expected_leaf_state_views,
      observed_row_scratch_growths);
  check_candidate_plan_counters(batched_state, batched_counters_before,
                                active_pattern_count,
                                expected_leaf_state_views,
                                observed_row_scratch_growths);
  check_candidate_plan_counters(lazy_state, lazy_counters_before, 0, 0,
                                observed_lazy_row_scratch_growths);
  CHECK(batched_state.counters.local_candidate_scores == subset.size());
  CHECK(batched_state.counters.pattern_batch_cache_builds >=
        batched_state.active_patterns.patterns.patterns.size());
  CHECK(batched_scores.front().local_score_ms > 0.0);
  CHECK(lazy_state.counters.local_candidate_scores == subset.size());
  CHECK(lazy_state.counters.local_rows_recomputed > 0);
  CHECK(lazy_state.counters.local_unit_fitch_fast_path_productions_scored >
        lazy_counters_before.local_unit_fitch_fast_path_productions_scored);
  CHECK(lazy_scores.front().local_score_ms > 0.0);

  bool single_threw = false;
  try {
    (void)larch::score_candidate_locally(batched_state, subset.front());
  } catch (std::exception const& e) {
    single_threw = true;
    CHECK(std::string{e.what()}.find("score_candidates_locally") !=
          std::string::npos);
  }
  CHECK(single_threw);

  larch::chart_spr_search_options memory_options;
  // This case exercises one bounded local-score entry. The finite unified
  // budget includes the cache-independent published state plus that entry:
  // small enough to force replay, but large enough to satisfy the complete
  // state contract.
  memory_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  auto const state_core_bytes =
      larch::estimate_chart_spr_state_core_resident_bytes(all_state);
  auto const one_entry_bytes =
      larch::estimate_chart_spr_pattern_entry_cache_bytes(grammar);
  auto const minimum_pattern_batch_budget =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          state_core_bytes, one_entry_bytes,
          "test pattern-batch unified state budget");
  memory_options.cache.memory_budget_bytes = minimum_pattern_batch_budget;
  memory_options.cache.candidate_batch_size = 2;
  auto memory_state = larch::build_chart_spr_search_state(
      dag, grammar, memory_options);
  CHECK(memory_state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(larch::estimate_chart_spr_published_state_resident_bytes(
            memory_state) <= memory_options.cache.memory_budget_bytes);
  auto one_under_options = memory_options;
  one_under_options.cache.memory_budget_bytes =
      minimum_pattern_batch_budget - 1;
  bool one_under_rejected = false;
  try {
    (void)larch::build_chart_spr_search_state(dag, grammar, one_under_options);
  } catch (std::runtime_error const& e) {
    one_under_rejected = true;
    CHECK(std::string_view{e.what()}.find(
              "state core plus one scoring pattern") != std::string_view::npos);
  }
  CHECK(one_under_rejected);
  CHECK(state_core_bytes > 1);
  auto core_one_under_options = memory_options;
  core_one_under_options.cache.memory_budget_bytes = state_core_bytes - 1;
  bool core_one_under_rejected = false;
  try {
    (void)larch::build_chart_spr_search_state(dag, grammar,
                                              core_one_under_options);
  } catch (std::runtime_error const& e) {
    core_one_under_rejected = true;
    CHECK(std::string_view{e.what()}.find(
              "cache-independent state core requires") !=
          std::string_view::npos);
  }
  CHECK(core_one_under_rejected);
  auto memory_scores = larch::score_candidates_locally(
      memory_state, subset, {}, 1);
  CHECK(memory_scores.size() == all_scores.size());
  for (std::size_t i = 0; i < all_scores.size(); ++i) {
    CHECK(memory_scores[i].lower_bound.value.new_score ==
          all_scores[i].lower_bound.value.new_score);
    CHECK(memory_scores[i].lower_bound.value.delta ==
          all_scores[i].lower_bound.value.delta);
  }

  std::println("  PASS");
}

static void test_lazy_local_admission_planner() {
  std::println("test_lazy_local_admission_planner");
  constexpr std::array<std::size_t, 4> task_bytes{8, 5, 7, 3};

  auto unlimited =
      larch::chart_spr_search_detail::plan_lazy_local_admission_wave(
          task_bytes, 4, 3, 0, false);
  CHECK(unlimited.admitted_count == 3);
  CHECK(unlimited.admitted_bytes == 20);
  CHECK(!unlimited.memory_limited);

  auto exact_prefix =
      larch::chart_spr_search_detail::plan_lazy_local_admission_wave(
          task_bytes, 9, 4, 13, true);
  CHECK(exact_prefix.admitted_count == 2);
  CHECK(exact_prefix.admitted_bytes == 13);
  CHECK(exact_prefix.memory_limited);

  auto shortened =
      larch::chart_spr_search_detail::plan_lazy_local_admission_wave(
          task_bytes, 9, 2, 100, true);
  CHECK(shortened.admitted_count == 2);
  CHECK(shortened.admitted_bytes == 13);
  CHECK(!shortened.memory_limited);

  bool failed = false;
  try {
    (void)larch::chart_spr_search_detail::plan_lazy_local_admission_wave(
        task_bytes, 17, 4, 7, true);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    failed = true;
    CHECK(e.candidate_index() == 17);
    CHECK(e.required_bytes() == 8);
    CHECK(e.available_bytes() == 7);
  }
  CHECK(failed);
  std::println("  PASS");
}

static void test_lazy_local_packed_context_grouping_reuses_scratch() {
  std::println("test_lazy_local_packed_context_grouping_reuses_scratch");

  auto fixture = make_fixture();
  larch::site_pattern_set patterns;
  patterns.taxon_count = 4;
  auto append_pattern = [&](std::array<std::uint8_t, 4> states,
                            std::uint32_t weight,
                            std::array<std::uint32_t, larch::nuc_state_count>
                                reference_state_counts) {
    patterns.patterns.push_back(larch::site_pattern{
        .state_by_taxon = {states.begin(), states.end()},
        .weight = weight,
        .reference_state_counts = reference_state_counts,
    });
  };
  // A full-rank modular permutation makes later patterns recombine class IDs
  // first introduced by earlier patterns, so first-occurrence class numbering
  // is observably different from lexicographic traversal.  Exact duplicates
  // at the tail carry different weights/reference states and force merged
  // context accumulation.
  for (std::size_t input = 0; patterns.patterns.size() < 64; ++input) {
    auto code = (input * 73 + 19) % 256;
    std::array<std::uint8_t, 4> states{};
    auto remaining = code;
    for (auto& state : states) {
      state = static_cast<std::uint8_t>(remaining % larch::nuc_state_count);
      remaining /= larch::nuc_state_count;
    }
    if (std::ranges::all_of(states,
                            [&](auto state) { return state == states[0]; })) {
      continue;
    }
    auto const weight = static_cast<std::uint32_t>(input % 5 + 1);
    std::array<std::uint32_t, larch::nuc_state_count> reference_counts{};
    reference_counts[(code / 7) % larch::nuc_state_count] = weight;
    append_pattern(states, weight, reference_counts);
  }
  for (std::size_t duplicate = 0; duplicate < 8; ++duplicate) {
    std::array<std::uint8_t, 4> states{};
    std::ranges::copy(patterns.patterns[duplicate].state_by_taxon,
                      states.begin());
    auto const weight = static_cast<std::uint32_t>(duplicate + 7);
    std::array<std::uint32_t, larch::nuc_state_count> reference_counts{};
    reference_counts[(duplicate + 1) % larch::nuc_state_count] = weight;
    append_pattern(states, weight, reference_counts);
  }

  larch::chart_options chart_options;
  chart_options.score_ua_edge = true;
  auto active_build =
      larch::make_active_search_patterns(patterns, chart_options);
  larch::chart_cache_options cache_options;
  cache_options.use_lazy_multisite_chart = true;
  auto state = larch::build_chart_spr_search_state_from_active(
      fixture.dag, fixture.grammar, std::move(active_build), chart_options,
      false, {}, cache_options);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(state.active_patterns.patterns.patterns.size() ==
        patterns.patterns.size());

  auto const candidate_it = std::max_element(
      fixture.candidates.begin(), fixture.candidates.end(),
      [](auto const& lhs, auto const& rhs) {
        return larch::chart_spr_detail::estimate_candidate_affected_clades(
                   lhs) <
               larch::chart_spr_detail::estimate_candidate_affected_clades(rhs);
      });
  CHECK(candidate_it != fixture.candidates.end());
  auto const& candidate = *candidate_it;

  larch::chart_spr_search_counters counters;
  larch::chart_spr_local_score_scratch scratch;
  auto run_once = [&] {
    auto prepared =
        larch::chart_spr_search_detail::prepare_local_candidate_score(
            state, candidate, {}, &counters);
    CHECK(prepared.valid_for_accumulation);
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_lazy(
        state, prepared, {}, &counters, scratch);
    CHECK(prepared.valid_for_accumulation);
    auto score =
        larch::chart_spr_search_detail::finish_prepared_local_candidate_score(
            state, prepared);
    prepared.release_operation_borrows();
    return score;
  };

  auto first = run_once();
  CHECK(first.valid);
  CHECK(!scratch.lazy_context_key_words.empty());
  CHECK(!scratch.lazy_context_grouping_result.class_by_input.empty());
  CHECK(scratch.lazy_context_grouping_result.class_count() ==
        scratch.lazy_contexts.size());
  CHECK(scratch.lazy_context_grouping_result.class_count() <
        patterns.patterns.size());

  // Reconstruct the former vector<size_t>-key ordered map independently from
  // the packed adapter.  This covers component order, first representatives,
  // input-order accumulation, and lexicographic class traversal together.
  auto oracle_prepared =
      larch::chart_spr_search_detail::prepare_local_candidate_score(
          state, candidate, {}, nullptr);
  CHECK(oracle_prepared.valid_for_accumulation);
  auto const& delta = oracle_prepared.delta();
  auto const& lazy = *state.lazy_chart;
  auto const& active_patterns = state.active_patterns.patterns.patterns;
  std::map<std::vector<std::size_t>, std::vector<std::size_t>> old_groups;
  for (std::size_t pattern_index = 0; pattern_index < active_patterns.size();
       ++pattern_index) {
    std::vector<std::size_t> key;
    key.push_back(
        larch::chart_spr_search_detail::lazy_overlay_inside_class_index(
            lazy, state.grammar.root_clade, pattern_index));
    auto append_component = [&](larch::overlay_clade_ref clade,
                                larch::taxon_id leaf_taxon) {
      if (clade.space == larch::overlay_id_space::base) {
        key.push_back(
            larch::chart_spr_search_detail::lazy_overlay_inside_class_index(
                lazy, clade.id, pattern_index));
      }
      if (leaf_taxon != larch::chart_plan_no_taxon) {
        key.push_back(
            active_patterns[pattern_index].state_by_taxon[leaf_taxon]);
      }
    };
    for (auto const& row : delta.compiled_rows) {
      append_component(row.clade, row.leaf_taxon);
      for (auto const& production :
           larch::chart_spr_search_detail::candidate_chart_productions_for_row(
               delta, row)) {
        for (auto const& child :
             larch::chart_spr_search_detail::candidate_chart_children(
                 delta, production)) {
          append_component(child.clade, child.leaf_taxon);
        }
      }
    }
    CHECK(
        key.size() ==
        larch::chart_spr_search_detail::lazy_overlay_context_key_width(delta));
    old_groups[std::move(key)].push_back(pattern_index);
  }

  auto const& grouping = scratch.lazy_context_grouping_result;
  std::vector<std::size_t> expected_lexicographic_class_order;
  expected_lexicographic_class_order.reserve(old_groups.size());
  bool saw_merged_context = false;
  for (auto const& [key, members] : old_groups) {
    (void)key;
    CHECK(!members.empty());
    auto const class_id = grouping.class_by_input[members.front()];
    CHECK(class_id < grouping.class_count());
    expected_lexicographic_class_order.push_back(class_id);
    CHECK(grouping.representative_by_class[class_id] == members.front());
    CHECK(std::ranges::equal(grouping.members_for_class(class_id), members));
    saw_merged_context = saw_merged_context || members.size() > 1;

    larch::lazy_overlay_context_accumulator expected;
    expected.representative = members.front();
    for (auto pattern_index : members) {
      auto const& pattern = active_patterns[pattern_index];
      expected.weight += pattern.weight;
      for (std::size_t state_index = 0; state_index < larch::nuc_state_count;
           ++state_index) {
        expected.reference_state_counts[state_index] +=
            pattern.reference_state_counts[state_index];
      }
    }
    CHECK(scratch.lazy_contexts[class_id].representative ==
          expected.representative);
    CHECK(scratch.lazy_contexts[class_id].weight == expected.weight);
    CHECK(scratch.lazy_contexts[class_id].reference_state_counts ==
          expected.reference_state_counts);
  }
  CHECK(saw_merged_context);
  CHECK(grouping.lexicographic_class_order ==
        expected_lexicographic_class_order);
  bool lexicographic_order_differs_from_class_ids = false;
  for (std::size_t index = 0; index < grouping.lexicographic_class_order.size();
       ++index) {
    lexicographic_order_differs_from_class_ids =
        lexicographic_order_differs_from_class_ids ||
        grouping.lexicographic_class_order[index] != index;
  }
  CHECK(lexicographic_order_differs_from_class_ids);
  oracle_prepared.release_operation_borrows();

  auto const first_grouping = scratch.lazy_context_grouping_result;
  auto const key_capacity = scratch.lazy_context_key_words.capacity();
  auto const grouping_workspace_capacities =
      scratch.lazy_context_grouping_workspace.capacity_by_buffer();
  auto const grouping_result_capacities =
      scratch.lazy_context_grouping_result.capacity_by_buffer();
  auto const context_capacity = scratch.lazy_contexts.capacity();

  scratch.clear_borrows();
  CHECK(scratch.operation_boundary_clean());
  auto second = run_once();
  CHECK(second.valid);
  CHECK(scratch.lazy_context_grouping_result == first_grouping);
  CHECK(scratch.lazy_context_key_words.capacity() == key_capacity);
  CHECK(scratch.lazy_context_grouping_workspace.capacity_by_buffer() ==
        grouping_workspace_capacities);
  CHECK(scratch.lazy_context_grouping_result.capacity_by_buffer() ==
        grouping_result_capacities);
  CHECK(scratch.lazy_contexts.capacity() == context_capacity);
  CHECK(second.lower_bound.value.old_score ==
        first.lower_bound.value.old_score);
  CHECK(second.lower_bound.value.new_score ==
        first.lower_bound.value.new_score);
  CHECK(second.lower_bound.value.delta == first.lower_bound.value.delta);

  auto oracle = larch::score_multisite_spr_candidate_lower_bound_oracle(
      fixture.grammar, patterns, candidate, chart_options);
  CHECK(first.lower_bound.value.old_score == oracle.old_score);
  CHECK(first.lower_bound.value.new_score == oracle.new_score);
  CHECK(first.lower_bound.value.delta == oracle.delta);
  scratch.clear_borrows();
  CHECK(scratch.operation_boundary_clean());

  auto checked_for_corruption =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  auto expect_corrupt_scratch_hard_failure = [&](auto&& corrupt) {
    auto prepared =
        larch::chart_spr_search_detail::prepare_local_candidate_score(
            state, candidate, {}, nullptr);
    CHECK(prepared.valid_for_accumulation);
    (void)larch::chart_spr_search_detail::
        prepare_lazy_local_score_scratch_for_candidate(
            state, prepared.delta(), scratch, nullptr);
    corrupt(scratch);
    larch::chart_spr_search_detail::
        accumulate_prepared_local_candidate_lazy_prepared(
            state, prepared, {}, nullptr, scratch, checked_for_corruption);
    CHECK(!prepared.valid_for_accumulation);
    bool failed = false;
    try {
      larch::chart_spr_search_detail::finalize_lazy_local_grouping_status(
          state, prepared, scratch, 3);
    } catch (larch::chart_spr_search_detail::
                 lazy_local_worker_failure_error const& e) {
      failed = true;
      CHECK(e.slot() == 3);
      CHECK(e.kind() == larch::chart_spr_search_detail::
                            lazy_local_worker_failure_kind::scratch_invariant);
    }
    CHECK(failed);
    scratch.clear_borrows();
    prepared.release_operation_borrows();
    CHECK(scratch.operation_boundary_clean());
  };
  expect_corrupt_scratch_hard_failure([](auto& value) {
    ++value.lazy_prepared_key_width;
  });
  expect_corrupt_scratch_hard_failure([](auto& value) {
    std::vector<larch::lazy_overlay_context_accumulator>{}.swap(
        value.lazy_contexts);
  });

  auto missing_lazy_prepared =
      larch::chart_spr_search_detail::prepare_local_candidate_score(
          state, candidate, {}, nullptr);
  (void)larch::chart_spr_search_detail::
      prepare_lazy_local_score_scratch_for_candidate(
          state, missing_lazy_prepared.delta(), scratch, nullptr);
  auto saved_lazy_chart = std::exchange(state.lazy_chart, std::nullopt);
  larch::chart_spr_search_detail::
      accumulate_prepared_local_candidate_lazy_prepared(
          state, missing_lazy_prepared, {}, nullptr, scratch,
          checked_for_corruption);
  state.lazy_chart = std::move(saved_lazy_chart);
  bool missing_lazy_failed = false;
  try {
    larch::chart_spr_search_detail::finalize_lazy_local_grouping_status(
        state, missing_lazy_prepared, scratch, 2);
  } catch (larch::chart_spr_search_detail::lazy_local_worker_failure_error const&
               e) {
    missing_lazy_failed = true;
    CHECK(e.slot() == 2);
    CHECK(e.kind() == larch::chart_spr_search_detail::
                          lazy_local_worker_failure_kind::missing_lazy_chart);
  }
  CHECK(missing_lazy_failed);
  scratch.clear_borrows();
  missing_lazy_prepared.release_operation_borrows();
  CHECK(scratch.operation_boundary_clean());

  CHECK(fixture.candidates.size() >= 4);
  std::vector<larch::grammar_spr_candidate> candidates(
      fixture.candidates.begin(), fixture.candidates.begin() + 4);
  std::vector<larch::chart_spr_local_score_result> w1(candidates.size());
  std::vector<larch::chart_spr_local_score_result> w4(candidates.size());
  larch::chart_spr_local_score_workspace w1_workspace;
  larch::chart_spr_local_score_workspace w4_workspace;
  constexpr std::size_t oversized_prior_slots = 64;
  constexpr std::size_t oversized_nested_capacity = 4096;
  larch::chart_spr_search_detail::local_score_workspace_access::begin(
      w4_workspace, oversized_prior_slots, oversized_prior_slots);
  for (std::size_t slot = 0; slot < oversized_prior_slots; ++slot) {
    larch::chart_spr_search_detail::local_score_workspace_access::prepared(
        w4_workspace, slot)
        .result.invalid_reason.reserve(oversized_nested_capacity);
    larch::chart_spr_search_detail::local_score_workspace_access::worker(
        w4_workspace, slot)
        .scratch.lazy_contexts.reserve(oversized_nested_capacity);
  }
  larch::chart_spr_search_detail::local_score_workspace_access::finish(
      w4_workspace);
  auto oversized_capacities = larch::chart_spr_search_detail::
      local_score_workspace_access::task_slot_capacities(w4_workspace);
  CHECK(oversized_capacities.first >= oversized_prior_slots);
  CHECK(oversized_capacities.second >= oversized_prior_slots);
  larch::score_candidates_locally_into(state, candidates, w1, w1_workspace, {},
                                       1);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 1,
  }};
  larch::local_score_worker_barrier_for_tests barrier;
  larch::local_spr_score_options parallel_options;
  parallel_options.worker_barrier_for_tests = &barrier;
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  auto const admission_waves_before = state.counters.lazy_local_admission_waves;
  auto const parallel_waves_before = state.counters.lazy_local_parallel_waves;
  larch::score_candidates_locally_into(state, candidates, w4, w4_workspace,
                                       parallel_options, scheduler, checked);
  CHECK(barrier.release.load());
  CHECK(barrier.started.load() >= 4);
  CHECK(w1_workspace.operation_boundary_clean());
  CHECK(w4_workspace.operation_boundary_clean());
  auto bounded_capacities = larch::chart_spr_search_detail::
      local_score_workspace_access::task_slot_capacities(w4_workspace);
  CHECK(bounded_capacities.first <= 4);
  CHECK(bounded_capacities.second <= 4);
  CHECK(state.counters.lazy_local_admission_waves - admission_waves_before ==
        1);
  CHECK(state.counters.lazy_local_parallel_waves - parallel_waves_before == 1);
  CHECK(state.counters.lazy_local_admitted_concurrency_max >= 4);
  CHECK(state.counters.lazy_local_peak_admitted_bytes > 0);
  CHECK(state.counters.lazy_local_peak_projected_resident_bytes >
        state.resident_pattern_cache_bytes);
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    CHECK(w4[index].valid == w1[index].valid);
    CHECK(w4[index].lower_bound.value.old_score ==
          w1[index].lower_bound.value.old_score);
    CHECK(w4[index].lower_bound.value.new_score ==
          w1[index].lower_bound.value.new_score);
    CHECK(w4[index].lower_bound.value.delta ==
          w1[index].lower_bound.value.delta);
    auto candidate_oracle =
        larch::score_multisite_spr_candidate_lower_bound_oracle(
            fixture.grammar, patterns, candidates[index], chart_options);
    CHECK(w4[index].lower_bound.value.old_score == candidate_oracle.old_score);
    CHECK(w4[index].lower_bound.value.new_score == candidate_oracle.new_score);
    CHECK(w4[index].lower_bound.value.delta == candidate_oracle.delta);
  }
  for (std::size_t worker = 0; worker < 4; ++worker) {
    auto const& prepared_slot =
        larch::chart_spr_search_detail::local_score_workspace_access::prepared(
            w4_workspace, worker);
    auto const& worker_scratch =
        larch::chart_spr_search_detail::local_score_workspace_access::worker(
            w4_workspace, worker)
            .scratch;
    CHECK(prepared_slot.result.invalid_reason.capacity() <
          oversized_nested_capacity);
    CHECK(worker_scratch.lazy_contexts.capacity() < oversized_nested_capacity);
    CHECK(worker_scratch.operation_boundary_clean());
    CHECK(worker_scratch.lazy_context_key_words.capacity() >=
          patterns.patterns.size());
    CHECK(worker_scratch.lazy_context_grouping_workspace.has_capacity_for(
        patterns.patterns.size()));
    CHECK(
        worker_scratch.lazy_context_grouping_result.has_worst_case_capacity_for(
            patterns.patterns.size()));
    CHECK(worker_scratch.lazy_contexts.capacity() > 0);
  }

  // Discover the allocation-free conservative preflight requirement through
  // the fixed-size budget exception. At an exact fixed-resident boundary the
  // task preflight (not the shared-resident guard) must be the rejection.
  auto finite_active =
      larch::make_active_search_patterns(patterns, chart_options);
  auto finite_state = larch::build_chart_spr_search_state_from_active(
      fixture.dag, fixture.grammar, std::move(finite_active), chart_options,
      false, {}, cache_options);
  auto finite_checked = larch::check_chart_execution_plan(
      finite_state.grammar, finite_state.execution_plan);
  larch::chart_spr_local_score_workspace finite_w1_workspace;
  std::array<larch::chart_spr_local_score_result, 1> singleton_result;
  auto discover_fixed_resident = [&](auto candidate_span, auto result_span,
                                     auto& workspace, auto&& score) {
    larch::local_spr_score_options options;
    options.admission_memory_budget_bytes = 1;
    try {
      score(candidate_span, result_span, workspace, options);
    } catch (
        larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
            e) {
      CHECK(e.available_bytes() == 1);
      return e.required_bytes();
    }
    CHECK(false);
    return std::size_t{0};
  };
  auto score_finite_w1 = [&](auto candidate_span, auto result_span,
                             auto& workspace,
                             larch::local_spr_score_options const& options) {
    larch::score_candidates_locally_into(finite_state, candidate_span,
                                         result_span, workspace, options, 1,
                                         finite_checked);
  };
  auto first_candidate =
      std::span<larch::grammar_spr_candidate const>{candidates}.first(1);
  auto singleton_results =
      std::span<larch::chart_spr_local_score_result>{singleton_result};
  auto const w1_fixed_resident = discover_fixed_resident(
      first_candidate, singleton_results, finite_w1_workspace, score_finite_w1);
  CHECK(w1_fixed_resident > 1);

  std::vector<std::size_t> preflight_requirements;
  std::vector<std::size_t> steady_resident_bases;
  preflight_requirements.reserve(candidates.size());
  steady_resident_bases.reserve(candidates.size());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    larch::local_spr_score_options boundary_options;
    boundary_options.admission_memory_budget_bytes = w1_fixed_resident;
    bool boundary_failed = false;
    try {
      score_finite_w1(
          std::span<larch::grammar_spr_candidate const>{&candidates[index], 1},
          singleton_results, finite_w1_workspace, boundary_options);
    } catch (
        larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
            e) {
      boundary_failed = true;
      CHECK(e.candidate_index() == 0);
      CHECK(e.available_bytes() < w1_fixed_resident);
      CHECK(e.required_bytes() > 0);
      steady_resident_bases.push_back(w1_fixed_resident -
                                      e.available_bytes());
      preflight_requirements.push_back(e.required_bytes());
    }
    CHECK(boundary_failed);
  }
  CHECK(preflight_requirements.size() == candidates.size());
  CHECK(steady_resident_bases.size() == candidates.size());

  // Finite explicit W1 must use coordinator admission. Exact fit preserves
  // semantics and the observed preparation high-water never exceeds budget;
  // one byte less fails before a task is prepared or submitted.
  auto const singleton_budget =
      steady_resident_bases.front() + preflight_requirements.front();
  larch::local_spr_score_options singleton_fit_options;
  singleton_fit_options.admission_memory_budget_bytes = singleton_budget;
  auto const singleton_counters_before = finite_state.counters;
  score_finite_w1(first_candidate, singleton_results, finite_w1_workspace,
                  singleton_fit_options);
  CHECK(singleton_result.front().valid == w1.front().valid);
  CHECK(singleton_result.front().lower_bound.value.new_score ==
        w1.front().lower_bound.value.new_score);
  CHECK(finite_state.counters.local_candidate_scores ==
        singleton_counters_before.local_candidate_scores + 1);
  CHECK(finite_state.counters.candidate_execution_plan_builds ==
        singleton_counters_before.candidate_execution_plan_builds + 1);
  CHECK(finite_state.counters.lazy_local_preparation_peak_bytes <=
        singleton_budget);
  CHECK(finite_w1_workspace.operation_boundary_clean());

  larch::local_spr_score_options singleton_reject_options;
  singleton_reject_options.admission_memory_budget_bytes = singleton_budget - 1;
  auto const singleton_prepared_before =
      finite_state.counters.lazy_local_prepared_tasks;
  auto const singleton_failures_before =
      finite_state.counters.lazy_local_pre_submit_budget_failures;
  auto const singleton_scheduler_operations_before =
      larch::chart_spr_scheduler_axis_operation_count(
          finite_state.counters.scheduler_axes);
  bool singleton_rejected = false;
  try {
    score_finite_w1(first_candidate, singleton_results, finite_w1_workspace,
                    singleton_reject_options);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    singleton_rejected = true;
    CHECK(e.required_bytes() == preflight_requirements.front());
  }
  CHECK(singleton_rejected);
  CHECK(finite_state.counters.lazy_local_prepared_tasks ==
        singleton_prepared_before);
  CHECK(finite_state.counters.lazy_local_pre_submit_budget_failures ==
        singleton_failures_before + 1);
  CHECK(larch::chart_spr_scheduler_axis_operation_count(
            finite_state.counters.scheduler_axes) ==
        singleton_scheduler_operations_before);
  CHECK(finite_w1_workspace.operation_boundary_clean());

  // The maximum singleton preparation envelope is enough for every task when
  // it starts a wave, but intentionally not for all four stable task slots at
  // once. The maximal stable prefix therefore shrinks M without changing
  // publication order or scores.
  larch::chart_spr_local_score_workspace finite_w4_workspace;
  std::vector<larch::chart_spr_local_score_result> limited(candidates.size());
  auto score_finite_w4 = [&](auto candidate_span, auto result_span,
                             auto& workspace,
                             larch::local_spr_score_options const& options) {
    larch::score_candidates_locally_into(finite_state, candidate_span,
                                         result_span, workspace, options,
                                         scheduler, finite_checked);
  };
  auto const w4_fixed_resident = discover_fixed_resident(
      std::span<larch::grammar_spr_candidate const>{candidates},
      std::span<larch::chart_spr_local_score_result>{limited},
      finite_w4_workspace, score_finite_w4);
  larch::local_spr_score_options w4_boundary_options;
  w4_boundary_options.admission_memory_budget_bytes = w4_fixed_resident;
  std::size_t w4_steady_resident = 0;
  try {
    score_finite_w4(
        std::span<larch::grammar_spr_candidate const>{candidates},
        std::span<larch::chart_spr_local_score_result>{limited},
        finite_w4_workspace, w4_boundary_options);
  } catch (larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error
               const& e) {
    CHECK(e.available_bytes() < w4_fixed_resident);
    w4_steady_resident = w4_fixed_resident - e.available_bytes();
  }
  CHECK(w4_steady_resident > 0);
  auto const maximum_preflight = *std::max_element(
      preflight_requirements.begin(), preflight_requirements.end());
  larch::local_spr_score_options limited_options;
  limited_options.admission_memory_budget_bytes =
      w4_steady_resident + maximum_preflight;
  auto const limited_waves_before =
      finite_state.counters.lazy_local_admission_waves;
  auto const memory_waves_before =
      finite_state.counters.lazy_local_memory_limited_waves;
  score_finite_w4(std::span<larch::grammar_spr_candidate const>{candidates},
                  std::span<larch::chart_spr_local_score_result>{limited},
                  finite_w4_workspace, limited_options);
  CHECK(finite_w4_workspace.operation_boundary_clean());
  CHECK(finite_state.counters.lazy_local_admission_waves -
            limited_waves_before >
        1);
  CHECK(finite_state.counters.lazy_local_memory_limited_waves -
            memory_waves_before >=
        1);
  CHECK(finite_state.counters.lazy_local_preparation_peak_bytes <=
        limited_options.admission_memory_budget_bytes);
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    CHECK(limited[index].valid == w1[index].valid);
    CHECK(limited[index].lower_bound.value.old_score ==
          w1[index].lower_bound.value.old_score);
    CHECK(limited[index].lower_bound.value.new_score ==
          w1[index].lower_bound.value.new_score);
    CHECK(limited[index].lower_bound.value.delta ==
          w1[index].lower_bound.value.delta);
  }

  // With ample finite headroom, a second wave reuses the first wave's
  // descriptor/grouping capacities. The public call still releases all task
  // payload before returning to its enumeration/promotion caller.
  std::vector<larch::grammar_spr_candidate> repeated_candidates = candidates;
  repeated_candidates.insert(repeated_candidates.end(), candidates.begin(),
                             candidates.end());
  std::vector<larch::chart_spr_local_score_result> repeated_results(
      repeated_candidates.size());
  larch::chart_spr_local_score_workspace reuse_workspace;
  larch::local_spr_score_options reuse_options;
  reuse_options.admission_memory_budget_bytes = std::size_t{1} << 30;
  auto const reuse_waves_before =
      finite_state.counters.lazy_local_admission_waves;
  auto const reused_tasks_before =
      finite_state.counters.lazy_local_reused_prepared_tasks;
  score_finite_w4(
      std::span<larch::grammar_spr_candidate const>{repeated_candidates},
      std::span<larch::chart_spr_local_score_result>{repeated_results},
      reuse_workspace, reuse_options);
  CHECK(finite_state.counters.lazy_local_admission_waves - reuse_waves_before ==
        2);
  CHECK(finite_state.counters.lazy_local_reused_prepared_tasks -
            reused_tasks_before >=
        candidates.size());
  CHECK(reuse_workspace.operation_boundary_clean());
  for (std::size_t index = 0; index < repeated_results.size(); ++index) {
    CHECK(repeated_results[index].lower_bound.value.delta ==
          w1[index % candidates.size()].lower_bound.value.delta);
  }

  // Genuine candidate invalidity publishes only a policy-bounded diagnostic.
  // Pin the exact finite boundary independently for checked W1 and checked W4
  // (which may split into singleton waves); one byte less rejects before the
  // first invalid task is prepared.
  larch::grammar_spr_candidate invalid_candidate;
  invalid_candidate.removed_productions.push_back(
      larch::base_production_ref(
          finite_state.grammar
              .productions_by_parent[finite_state.grammar.root_clade]
              .front()));
  std::array<larch::grammar_spr_candidate, 4> invalid_candidates;
  invalid_candidates.fill(invalid_candidate);
  auto pin_invalid_boundary = [&](std::size_t workers) {
    std::array<larch::chart_spr_local_score_result, 4> invalid_results;
    larch::chart_spr_local_score_workspace invalid_workspace;
    auto score = [&](larch::local_spr_score_options const& score_options) {
      if (workers == 1) {
        larch::score_candidates_locally_into(
            finite_state, invalid_candidates, invalid_results,
            invalid_workspace, score_options, 1, finite_checked);
      } else {
        larch::score_candidates_locally_into(
            finite_state, invalid_candidates, invalid_results,
            invalid_workspace, score_options, scheduler, finite_checked);
      }
    };
    larch::local_spr_score_options cold;
    cold.admission_memory_budget_bytes = 1;
    std::size_t fixed = 0;
    try {
      score(cold);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const& e) {
      fixed = e.required_bytes();
    }
    CHECK(fixed > 1);
    larch::local_spr_score_options task_probe;
    task_probe.admission_memory_budget_bytes = fixed;
    std::size_t task = 0;
    std::size_t steady_resident = 0;
    try {
      score(task_probe);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const& e) {
      CHECK(e.available_bytes() < fixed);
      steady_resident = fixed - e.available_bytes();
      task = e.required_bytes();
    }
    CHECK(task > 0);
    CHECK(steady_resident > 0);
    auto const exact_budget = steady_resident + task;
    larch::local_spr_score_options fit;
    fit.admission_memory_budget_bytes = exact_budget;
    score(fit);
    CHECK(invalid_workspace.operation_boundary_clean());
    for (auto const& result : invalid_results) {
      CHECK(!result.valid);
      CHECK(!result.invalid_reason.empty());
      CHECK(result.invalid_reason.size() <=
            larch::chart_spr_search_detail::
                lazy_local_invalid_reason_max_size);
    }

    larch::local_spr_score_options one_under;
    one_under.admission_memory_budget_bytes = exact_budget - 1;
    std::array<larch::chart_spr_local_score_result, 4> reject_results;
    larch::chart_spr_local_score_workspace reject_workspace;
    auto const prepared_before =
        finite_state.counters.lazy_local_prepared_tasks;
    bool rejected = false;
    try {
      if (workers == 1) {
        larch::score_candidates_locally_into(
            finite_state, invalid_candidates, reject_results,
            reject_workspace, one_under, 1, finite_checked);
      } else {
        larch::score_candidates_locally_into(
            finite_state, invalid_candidates, reject_results,
            reject_workspace, one_under, scheduler, finite_checked);
      }
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(finite_state.counters.lazy_local_prepared_tasks == prepared_before);
    CHECK(reject_workspace.operation_boundary_clean());
    return invalid_results.front().invalid_reason;
  };
  auto const invalid_w1_reason = pin_invalid_boundary(1);
  auto const invalid_w4_reason = pin_invalid_boundary(4);
  CHECK(invalid_w1_reason == invalid_w4_reason);

  // The checked direct finite boundary must charge caller-owned candidate
  // inputs independently of result growth. Inflate only nested source
  // capacities (not logical candidate shape), prove that the cold boundary
  // moves by exactly that visible resident delta, then pin task E/E-1. The
  // one-under call must reject before preparation or scheduler submission.
  std::array<larch::grammar_spr_candidate, 4> baseline_capacity_candidates;
  baseline_capacity_candidates.fill(candidates.front());
  auto oversized_capacity_candidates = baseline_capacity_candidates;
  for (auto& oversized : oversized_capacity_candidates) {
    oversized.removed_productions.reserve(4096);
    oversized.added_clades.reserve(4096);
    oversized.added_productions.reserve(4096);
  }
  auto const baseline_input_resident = larch::chart_spr_search_detail::
      estimate_lazy_local_candidate_input_resident_bytes(
          baseline_capacity_candidates);
  auto const oversized_input_resident = larch::chart_spr_search_detail::
      estimate_lazy_local_candidate_input_resident_bytes(
          oversized_capacity_candidates);
  CHECK(oversized_input_resident > baseline_input_resident + 4096);

  auto discover_input_fixed = [&](auto const& candidate_inputs,
                                  auto& result_outputs, auto& score_workspace) {
    larch::local_spr_score_options cold;
    cold.admission_memory_budget_bytes = 1;
    try {
      larch::score_candidates_locally_into(
          finite_state, candidate_inputs, result_outputs, score_workspace,
          cold, scheduler, finite_checked);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_budget_error const& e) {
      CHECK(e.available_bytes() == 1);
      return e.required_bytes();
    }
    CHECK(false);
    return std::size_t{0};
  };
  std::array<larch::chart_spr_local_score_result, 4>
      baseline_capacity_results;
  std::array<larch::chart_spr_local_score_result, 4>
      oversized_capacity_results;
  larch::chart_spr_local_score_workspace baseline_capacity_workspace;
  larch::chart_spr_local_score_workspace oversized_capacity_workspace;
  auto const baseline_capacity_fixed = discover_input_fixed(
      baseline_capacity_candidates, baseline_capacity_results,
      baseline_capacity_workspace);
  auto const oversized_capacity_fixed = discover_input_fixed(
      oversized_capacity_candidates, oversized_capacity_results,
      oversized_capacity_workspace);
  CHECK(oversized_capacity_fixed - baseline_capacity_fixed ==
        oversized_input_resident - baseline_input_resident);

  larch::local_spr_score_options oversized_task_probe;
  oversized_task_probe.admission_memory_budget_bytes =
      oversized_capacity_fixed;
  std::size_t oversized_task_requirement = 0;
  std::size_t oversized_steady_resident = 0;
  try {
    larch::score_candidates_locally_into(
        finite_state, oversized_capacity_candidates,
        oversized_capacity_results, oversized_capacity_workspace,
        oversized_task_probe, scheduler, finite_checked);
  } catch (larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error
               const& e) {
    CHECK(e.available_bytes() < oversized_capacity_fixed);
    oversized_steady_resident =
        oversized_capacity_fixed - e.available_bytes();
    oversized_task_requirement = e.required_bytes();
  }
  CHECK(oversized_steady_resident > 0);
  CHECK(oversized_task_requirement > 0);
  auto const oversized_exact_budget =
      oversized_steady_resident + oversized_task_requirement;
  larch::local_spr_score_options oversized_fit;
  oversized_fit.admission_memory_budget_bytes = oversized_exact_budget;
  larch::score_candidates_locally_into(
      finite_state, oversized_capacity_candidates, oversized_capacity_results,
      oversized_capacity_workspace, oversized_fit, scheduler, finite_checked);
  CHECK(oversized_capacity_workspace.operation_boundary_clean());
  for (auto const& result : oversized_capacity_results) {
    CHECK(result.valid == w1.front().valid);
    CHECK(result.lower_bound.value.delta ==
          w1.front().lower_bound.value.delta);
  }

  std::array<larch::chart_spr_local_score_result, 4>
      oversized_reject_results;
  larch::chart_spr_local_score_workspace oversized_reject_workspace;
  larch::local_spr_score_options oversized_one_under;
  oversized_one_under.admission_memory_budget_bytes =
      oversized_exact_budget - 1;
  auto const oversized_prepared_before =
      finite_state.counters.lazy_local_prepared_tasks;
  auto const oversized_axis_operations_before =
      larch::chart_spr_scheduler_axis_operation_count(
          finite_state.counters.scheduler_axes);
  auto const oversized_scheduler_before = scheduler.metrics();
  bool oversized_rejected = false;
  try {
    larch::score_candidates_locally_into(
        finite_state, oversized_capacity_candidates, oversized_reject_results,
        oversized_reject_workspace, oversized_one_under, scheduler,
        finite_checked);
  } catch (larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error
               const& e) {
    oversized_rejected = true;
    CHECK(e.required_bytes() == oversized_task_requirement);
  }
  CHECK(oversized_rejected);
  CHECK(finite_state.counters.lazy_local_prepared_tasks ==
        oversized_prepared_before);
  CHECK(larch::chart_spr_scheduler_axis_operation_count(
            finite_state.counters.scheduler_axes) ==
        oversized_axis_operations_before);
  auto const oversized_scheduler_after = scheduler.metrics();
  CHECK(oversized_scheduler_after.operations ==
        oversized_scheduler_before.operations);
  CHECK(oversized_scheduler_after.tasks_submitted ==
        oversized_scheduler_before.tasks_submitted);
  CHECK(oversized_reject_workspace.operation_boundary_clean());
  for (auto const& result : oversized_reject_results) {
    CHECK(result.affected_clade_count == 0);
    CHECK(result.valid);
    CHECK(result.invalid_reason.empty());
  }

  // Uncapped finite-budget enumeration ownership graphs fail closed before
  // rank/batch/enumerator storage is allocated for every source. Bounded
  // sampled/hybrid coverage lives in chart_spr_pipeline_test.
  for (auto source : {larch::chart_spr_candidate_source::sampled_tree,
                      larch::chart_spr_candidate_source::hybrid}) {
    larch::chart_spr_search_options source_options;
    source_options.acceptance_mode =
        larch::chart_spr_acceptance_mode::lower_bound_heuristic;
    source_options.cache.use_lazy_multisite_chart = true;
    source_options.cache.memory_budget_bytes = 1;
    source_options.enumeration.source = source;
    larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
        source_workspace;
    bool source_rejected = false;
    try {
      (void)larch::run_chart_spr_acceptance_iteration(
          finite_state, source_options, 0, source_workspace, scheduler);
    } catch (larch::chart_spr_search_detail::
                 chart_spr_lazy_local_enumeration_budget_error const& e) {
      source_rejected = true;
      CHECK(e.source() == source);
    }
    CHECK(source_rejected);
    CHECK(source_workspace.local_score.operation_boundary_clean());
    CHECK(source_workspace.candidate_slots.empty());
  }

  larch::chart_spr_search_options reservoir_options;
  reservoir_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  reservoir_options.cache.use_lazy_multisite_chart = true;
  reservoir_options.cache.memory_budget_bytes = 1;
  reservoir_options.enumeration.source =
      larch::chart_spr_candidate_source::grammar;
  reservoir_options.enumeration.reservoir_sample = true;
  reservoir_options.enumeration.max_candidates = 1;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      reservoir_workspace;
  bool reservoir_rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        finite_state, reservoir_options, 0, reservoir_workspace, scheduler);
  } catch (larch::chart_spr_search_detail::
               chart_spr_lazy_local_enumeration_budget_error const& e) {
    reservoir_rejected = true;
    CHECK(e.source() == larch::chart_spr_candidate_source::grammar);
  }
  CHECK(reservoir_rejected);
  CHECK(reservoir_workspace.candidate_slots.empty());

  larch::local_spr_score_options submit_failure_options;
  larch::local_score_worker_barrier_for_tests submit_failure_barrier;
  submit_failure_options.force_worker_submit_failure_after_for_tests = 1;
  submit_failure_options.worker_barrier_for_tests = &submit_failure_barrier;
  std::array<larch::chart_spr_local_score_result, 1>
      submit_failure_row_oracle_result;
  larch::chart_spr_local_score_workspace submit_failure_row_oracle_workspace;
  larch::chart_scheduler submit_failure_row_oracle_scheduler{
      larch::chart_scheduler_options{.requested_workers = 1}};
  auto const submit_failure_row_oracle_before =
      state.counters.local_rows_recomputed;
  larch::score_candidates_locally_into(
      state,
      std::span<larch::grammar_spr_candidate const>{candidates}.first(1),
      submit_failure_row_oracle_result, submit_failure_row_oracle_workspace,
      {}, submit_failure_row_oracle_scheduler, checked);
  auto const expected_submit_failure_rows =
      state.counters.local_rows_recomputed -
      submit_failure_row_oracle_before;
  CHECK(expected_submit_failure_rows > 0);
  CHECK(submit_failure_row_oracle_workspace.operation_boundary_clean());
  submit_failure_row_oracle_scheduler.shutdown();
  auto const submit_failure_counters_before = state.counters;
  auto const submit_failure_scheduler_before = scheduler.metrics();
  bool submit_failed = false;
  try {
    larch::score_candidates_locally_into(state, candidates, limited,
                                         w4_workspace, submit_failure_options,
                                         scheduler, checked);
  } catch (std::runtime_error const& e) {
    submit_failed = true;
    CHECK(std::string{e.what()}.find("forced worker submission failure") !=
          std::string::npos);
  }
  CHECK(submit_failed);
  CHECK(submit_failure_barrier.started.load() == 1);
  CHECK(submit_failure_barrier.release.load());
  CHECK(w4_workspace.operation_boundary_clean());
  auto const submit_failure_scheduler_after = scheduler.metrics();
  CHECK(submit_failure_scheduler_after.operations ==
        submit_failure_scheduler_before.operations + 1);
  CHECK(submit_failure_scheduler_after.tasks_submitted ==
        submit_failure_scheduler_before.tasks_submitted + 1);
  CHECK(submit_failure_scheduler_after.tasks_completed ==
        submit_failure_scheduler_before.tasks_completed + 1);
  CHECK(submit_failure_scheduler_after.tasks_joined ==
        submit_failure_scheduler_before.tasks_joined + 1);
  CHECK(submit_failure_scheduler_after.tasks_submitted ==
        submit_failure_scheduler_after.tasks_completed);
  CHECK(submit_failure_scheduler_after.tasks_submitted ==
        submit_failure_scheduler_after.tasks_joined);
  CHECK(submit_failure_scheduler_after.pending_tasks == 0);
  CHECK(state.counters.candidate_batches_scored ==
        submit_failure_counters_before.candidate_batches_scored + 1);
  CHECK(state.counters.local_rows_recomputed ==
        submit_failure_counters_before.local_rows_recomputed +
            expected_submit_failure_rows);
  for (std::size_t slot = 0; slot < 4; ++slot) {
    auto const& folded = larch::chart_spr_search_detail::
        local_score_workspace_access::worker(w4_workspace, slot)
            .counters;
    CHECK(folded.local_rows_recomputed == 0);
    CHECK(folded.candidate_execution_plan_cache_hits == 0);
    CHECK(larch::chart_spr_scheduler_axis_operation_count(
              folded.scheduler_axes) == 0);
  }
  auto submit_failure_global_axes = state.counters.scheduler_axes;
  larch::add_chart_spr_scheduler_axis_counters(
      submit_failure_global_axes, finite_state.counters.scheduler_axes);
  check_phase4_scheduler_axis_reconciliation(submit_failure_scheduler_after,
                                             submit_failure_global_axes);
  for (auto const& result : limited) {
    CHECK(result.affected_clade_count == 0);
    CHECK(result.local_score_ms == 0.0);
    CHECK(result.valid);
    CHECK(result.invalid_reason.empty());
  }

  auto check_no_lazy_results_published = [&] {
    for (auto const& result : limited) {
      CHECK(result.affected_clade_count == 0);
      CHECK(result.local_score_ms == 0.0);
      CHECK(result.valid);
      CHECK(result.invalid_reason.empty());
      CHECK(result.lower_bound.value.old_score == 0);
      CHECK(result.lower_bound.value.new_score == 0);
    }
  };

  // Worker invariants are fixed-size hard failures, never biological invalid
  // candidates. All simultaneously failed workers are joined and the stable
  // lowest slot wins; cleanup folds each worker payload once.
  larch::local_spr_score_options invariant_failure_options;
  invariant_failure_options.force_all_lazy_worker_invariant_failures_for_tests =
      true;
  auto const invariant_batches_before =
      state.counters.candidate_batches_scored;
  bool invariant_failed = false;
  try {
    larch::score_candidates_locally_into(
        state, candidates, limited, w4_workspace, invariant_failure_options,
        scheduler, checked);
  } catch (larch::chart_spr_search_detail::lazy_local_worker_failure_error const&
               e) {
    invariant_failed = true;
    CHECK(e.slot() == 0);
    CHECK(e.kind() == larch::chart_spr_search_detail::
                          lazy_local_worker_failure_kind::forced_for_tests);
  }
  CHECK(invariant_failed);
  CHECK(state.counters.candidate_batches_scored ==
        invariant_batches_before + 1);
  CHECK(w4_workspace.operation_boundary_clean());
  check_no_lazy_results_published();

  auto run_finish_failure = [&](bool allocation) {
    larch::local_spr_score_options failure_options;
    if (allocation) {
      failure_options.force_lazy_finish_allocation_for_tests = 2;
    } else {
      failure_options.force_lazy_finish_overflow_for_tests = 2;
    }
    auto const counters_before = state.counters;
    bool failed = false;
    try {
      larch::score_candidates_locally_into(
          state, candidates, limited, w4_workspace, failure_options, scheduler,
          checked);
    } catch (std::bad_alloc const&) {
      CHECK(allocation);
      failed = true;
    } catch (std::overflow_error const& e) {
      CHECK(!allocation);
      CHECK(std::string{e.what()}.find("forced finish overflow") !=
            std::string::npos);
      failed = true;
    }
    CHECK(failed);
    CHECK(state.counters.candidate_batches_scored ==
          counters_before.candidate_batches_scored + 1);
    CHECK(state.counters.local_rows_recomputed >
          counters_before.local_rows_recomputed);
    CHECK(w4_workspace.operation_boundary_clean());
    check_no_lazy_results_published();
    return state.counters.local_rows_recomputed -
           counters_before.local_rows_recomputed;
  };
  auto const overflow_rows = run_finish_failure(false);
  auto const allocation_rows = run_finish_failure(true);
  CHECK(overflow_rows == allocation_rows);

  // The same caller-owned workspace remains reusable after every hard-failure
  // path and a later successful publication is complete and ordered.
  larch::score_candidates_locally_into(state, candidates, limited, w4_workspace,
                                       {}, scheduler, checked);
  CHECK(w4_workspace.operation_boundary_clean());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    CHECK(limited[index].valid == w1[index].valid);
    CHECK(limited[index].lower_bound.value.new_score ==
          w1[index].lower_bound.value.new_score);
  }

  // An impossible singleton is rejected before scheduler submission and the
  // operation boundary remains reusable.
  larch::local_spr_score_options impossible_options;
  impossible_options.admission_memory_budget_bytes = 1;
  auto const scheduler_operations_before =
      larch::chart_spr_scheduler_axis_operation_count(
          state.counters.scheduler_axes);
  auto const failures_before =
      state.counters.lazy_local_pre_submit_budget_failures;
  bool budget_failed = false;
  try {
    larch::score_candidates_locally_into(state, candidates, limited,
                                         w4_workspace, impossible_options,
                                         scheduler, checked);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    budget_failed = true;
    CHECK(e.candidate_index() == 0);
  }
  CHECK(budget_failed);
  CHECK(w4_workspace.operation_boundary_clean());
  CHECK(state.counters.lazy_local_pre_submit_budget_failures -
            failures_before ==
        1);
  CHECK(larch::chart_spr_scheduler_axis_operation_count(
            state.counters.scheduler_axes) == scheduler_operations_before);
  scheduler.shutdown();

  std::println("  PASS");
}

static void test_lazy_local_production_multibatch_retained_budget() {
  std::println("test_lazy_local_production_multibatch_retained_budget");

  auto fixture = make_fixture();
  CHECK(fixture.candidates.size() >= 4);
  larch::chart_spr_search_options base_options;
  base_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  base_options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  base_options.cache.use_lazy_multisite_chart = true;
  base_options.cache.candidate_batch_size = 1;
  base_options.enumeration.max_candidates =
      std::min<std::size_t>(64, fixture.candidates.size());
  base_options.enumeration.max_candidates_is_post_dedup = true;
  base_options.semantic_capture =
      larch::chart_spr_semantic_capture_mode::digest;

  auto make_state = [&] {
    return larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                               base_options);
  };
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 1,
  }};

  auto discover_envelope = [&] {
    auto state = make_state();
    auto options = base_options;
    options.cache.memory_budget_bytes = 1;
    larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
        workspace;
    try {
      (void)larch::run_chart_spr_acceptance_iteration(state, options, 0,
                                                      workspace, scheduler);
    } catch (
        larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
            e) {
      CHECK(e.available_bytes() == 1);
      CHECK(state.counters.lazy_local_admission_waves == 0);
      CHECK(workspace.candidate_slots.capacity() == 0);
      return e.required_bytes();
    }
    CHECK(false);
    return std::size_t{0};
  };

  auto const envelope = discover_envelope();
  CHECK(envelope > 1);

  // The advertised global envelope includes all retained enumeration,
  // ranked/canonical/affected growth plus the worst local task preparation.
  // Its exact boundary therefore completes every one-candidate batch without
  // a late local-admission rejection.
  auto state = make_state();
  auto fit_options = base_options;
  fit_options.cache.memory_budget_bytes = envelope;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      fit_workspace;
  auto fit = larch::run_chart_spr_acceptance_iteration(
      state, fit_options, 0, fit_workspace, scheduler);
  CHECK(fit.candidates_generated == base_options.enumeration.max_candidates);
  CHECK(fit.candidates_scored == base_options.enumeration.max_candidates);
  CHECK(fit.canonical_candidates.size() == fit.candidates_scored);
  if (fit.accepted) {
    CHECK(!fit.accepted_candidate_signature.empty());
  }
  CHECK(state.counters.lazy_local_admission_waves == fit.candidates_scored);
  CHECK(state.counters.lazy_local_pre_submit_budget_failures == 0);
  CHECK(state.counters.lazy_local_iteration_envelope_bytes_max == envelope);
  CHECK(state.counters.lazy_local_iteration_task_stable_bytes_max > 0);
  CHECK(state.counters.lazy_local_iteration_task_preparation_peak_bytes_max >
        0);
  CHECK(state.counters.lazy_local_peak_projected_resident_bytes <= envelope);
  CHECK(state.counters.lazy_local_preparation_peak_bytes <= envelope);
  CHECK(fit_workspace.local_score.operation_boundary_clean());
  CHECK(fit_workspace.candidate_slots.capacity() == 0);
  CHECK(fit_workspace.candidate_copy_scratch.capacity() == 0);
  CHECK(fit_workspace.local_results.capacity() == 0);

  // A canonical sample signature is not bounded by a fixed diagnostic-sized
  // guess: sample IDs may themselves be arbitrarily long. The finite grammar
  // envelope derives the retained record charge from their encoded lengths.
  auto long_sample_state = make_state();
  for (std::size_t taxon = 0;
       taxon < long_sample_state.grammar.taxa.id_to_sample_id.size(); ++taxon) {
    long_sample_state.grammar.taxa.id_to_sample_id[taxon] =
        std::string(5000, static_cast<char>('a' + taxon % 26)) +
        std::to_string(taxon);
  }
  auto const long_sample_envelope = larch::chart_spr_search_detail::
      estimate_grammar_spr_finite_iteration_memory_envelope(
          long_sample_state, base_options.enumeration.max_candidates, 1, 1,
          true, scheduler, 1);
  auto long_sample_signature = larch::chart_spr_candidate_sample_signature(
      long_sample_state.grammar, fixture.candidates.front());
  CHECK(long_sample_envelope.planned_canonical_record_dynamic_bytes > 4096);
  CHECK(long_sample_envelope.planned_canonical_record_dynamic_bytes >=
        long_sample_signature.capacity() + 1);
  CHECK(long_sample_envelope.planned_accepted_candidate_signature_bytes >=
        long_sample_signature.capacity() + 1);

  auto reject_state = make_state();
  auto reject_options = base_options;
  reject_options.cache.memory_budget_bytes = envelope - 1;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      reject_workspace;
  bool rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        reject_state, reject_options, 0, reject_workspace, scheduler);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    rejected = true;
    CHECK(e.required_bytes() == envelope);
  }
  CHECK(rejected);
  CHECK(reject_state.counters.lazy_local_admission_waves == 0);
  CHECK(reject_workspace.candidate_slots.capacity() == 0);

  // The accepted-signature allocation itself is covered at the strict finite
  // boundary.  A one-candidate improving fixture prevents E-1 from shrinking
  // any source/batch wave, so the rejection proves the new result ownership
  // was included before candidate generation.
  auto improving_dag =
      larch::test::make_tiny_labelled_tree("A", four_taxon_misplaced_tree());
  auto improving_grammar = larch::build_clade_grammar(improving_dag);
  auto signature_options = base_options;
  signature_options.semantic_capture =
      larch::chart_spr_semantic_capture_mode::digest;
  signature_options.enumeration.max_candidates = 1;
  signature_options.cache.candidate_batch_size = 1;
  signature_options.enable_candidate_generation_pipeline = false;
  auto const signature_state_options = signature_options;
  auto make_signature_state = [&] {
    return larch::build_chart_spr_search_state(improving_dag, improving_grammar,
                                               signature_state_options);
  };
  auto signature_state = make_signature_state();
  auto const signature_envelope = larch::chart_spr_search_detail::
      estimate_grammar_spr_finite_iteration_memory_envelope(
          signature_state, 1, 1, 1, true, scheduler, 1);
  CHECK(signature_envelope.planned_accepted_candidate_signature_bytes > 0);

  auto signature_discovery_state = make_signature_state();
  auto signature_discovery_options = signature_options;
  signature_discovery_options.cache.memory_budget_bytes = 1;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      signature_discovery_workspace;
  std::size_t signature_budget = 0;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        signature_discovery_state, signature_discovery_options, 0,
        signature_discovery_workspace, scheduler);
    CHECK(false);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    signature_budget = e.required_bytes();
  }
  CHECK(signature_budget > 1);
  signature_options.cache.memory_budget_bytes =
      signature_budget;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      signature_workspace;
  auto signature_fit = larch::run_chart_spr_acceptance_iteration(
      signature_state, signature_options, 0, signature_workspace, scheduler);
  CHECK(signature_fit.accepted.has_value());
  CHECK(!signature_fit.accepted_candidate_signature.empty());
  CHECK(signature_fit.accepted_candidate_signature.capacity() + 1 <=
        signature_envelope.planned_accepted_candidate_signature_bytes);

  auto signature_reject_state = make_signature_state();
  signature_options.cache.memory_budget_bytes =
      signature_budget - 1;
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      signature_reject_workspace;
  bool signature_rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        signature_reject_state, signature_options, 0,
        signature_reject_workspace, scheduler);
  } catch (
      larch::chart_spr_search_detail::chart_spr_lazy_local_budget_error const&
          e) {
    signature_rejected = true;
    CHECK(e.required_bytes() == signature_budget);
  }
  CHECK(signature_rejected);
  CHECK(signature_reject_state.counters.lazy_local_admission_waves == 0);

  scheduler.shutdown();
  std::println("  PASS");
}

static void test_lazy_local_named_fixture_finite_production_gates() {
  std::println("test_lazy_local_named_fixture_finite_production_gates");
  constexpr std::size_t budget = 12884901888ULL;

  auto run_fixture = [&](std::string_view name, larch::phylo_dag dag,
                         std::size_t expected_taxa, std::size_t expected_clades,
                         std::size_t expected_patterns,
                         std::size_t candidate_count,
                         std::size_t candidate_batch_size,
                         std::size_t worker_count) {
    larch::recompute_compact_genomes(dag);
    larch::set_sample_ids_from_cg(dag);
    larch::polytomy_refinement_options refinement_options;
    refinement_options.mode = larch::polytomy_mode::expand_soft_bounded;
    refinement_options.max_shapes_per_polytomy = 1;
    auto refinement = larch::build_polytomy_refined_clade_grammar(
        dag, larch::clade_grammar_options{}, refinement_options);

    larch::chart_spr_search_options options;
    options.acceptance_mode =
        larch::chart_spr_acceptance_mode::lower_bound_heuristic;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.cache.use_lazy_multisite_chart = true;
    options.cache.memory_budget_bytes = budget;
    options.cache.candidate_batch_size = candidate_batch_size;
    options.enumeration.max_candidates = candidate_count;
    options.enumeration.max_candidates_is_post_dedup = true;
    options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;
    auto state = larch::build_chart_spr_search_state(
        dag, std::move(refinement.grammar), options);
    CHECK(state.cache_strategy ==
          larch::chart_spr_cache_strategy::lazy_multisite_chart);
    CHECK(state.grammar.taxa.id_to_sample_id.size() == expected_taxa);
    CHECK(state.grammar.clades.size() == expected_clades);
    CHECK(state.active_patterns.patterns.patterns.size() == expected_patterns);

    auto const task_slots =
        std::max<std::size_t>(1, std::min(worker_count, candidate_batch_size));
    larch::chart_scheduler scheduler{
        larch::chart_scheduler_options{.requested_workers = worker_count}};
    auto const use_pipeline =
        options.enable_candidate_generation_pipeline && worker_count > 1;
    auto const envelope = larch::chart_spr_search_detail::
        estimate_grammar_spr_finite_iteration_memory_envelope(
            state, candidate_count, candidate_batch_size, 1, true, scheduler,
            task_slots, &options.enumeration,
            use_pipeline ? std::size_t{2} : std::size_t{1}, use_pipeline);
    CHECK(envelope.planned_required_bytes > 0);
    CHECK(envelope.planned_required_bytes < budget);
    options.cache.memory_budget_bytes = envelope.planned_required_bytes;

    larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
        workspace;
    auto result = larch::run_chart_spr_acceptance_iteration(
        state, options, 0, workspace, scheduler);
    CHECK(result.candidates_generated == candidate_count);
    CHECK(result.candidates_scored == candidate_count);
    CHECK(result.canonical_candidates.size() == candidate_count);
    CHECK(state.counters.lazy_local_iteration_envelope_bytes_max ==
          envelope.planned_required_bytes);
    CHECK(state.counters.lazy_local_admission_waves > 0);
    CHECK(state.counters.lazy_local_pre_submit_budget_failures == 0);
    CHECK(state.counters.lazy_local_peak_projected_resident_bytes <=
          envelope.planned_required_bytes);
    CHECK(state.counters.lazy_local_preparation_peak_bytes <=
          envelope.planned_required_bytes);
    CHECK(workspace.local_score.operation_boundary_clean());
    scheduler.shutdown();
    std::println("  {} finite envelope bytes: {}", name,
                 envelope.planned_required_bytes);
  };

  run_fixture("small", larch::load_proto_dag("data/test_5_trees/tree_0.pb.gz"),
              70, 139, 113, 64, 32, 8);
  run_fixture("high",
              larch::load_proto_dag("test/wric_lazy_high_compression.pb.gz"),
              512, 1023, 2046, 64, 32, 8);

  auto reference_bytes = larch::read_file("data/seedtree/refseq.txt.gz");
  std::string reference;
  for (unsigned char byte : reference_bytes) {
    if (!std::isspace(byte)) {
      reference.push_back(static_cast<char>(std::toupper(byte)));
    }
  }
  CHECK(!reference.empty());
  run_fixture(
      "medium",
      larch::load_parsimony_tree("data/seedtree/seedtree.pb.gz", reference),
      597, 1193, 1106, 4, 4, 4);
  std::println("  PASS");
}

static void check_local_result_matches_owned_score(
    larch::chart_spr_local_score_result const& local,
    larch::chart_spr_candidate_score const& owned) {
  CHECK(local.lower_bound.value.delta == owned.lower_bound.value.delta);
  CHECK(local.lower_bound.value.old_score == owned.lower_bound.value.old_score);
  CHECK(local.lower_bound.value.new_score == owned.lower_bound.value.new_score);
  CHECK(local.lower_bound.value.exact_multisite ==
        owned.lower_bound.value.exact_multisite);
  CHECK(local.lower_bound.kind == owned.lower_bound.kind);
  CHECK(local.lower_bound.convention == owned.lower_bound.convention);
  CHECK(local.lower_bound.invariant_offset_applied ==
        owned.lower_bound.invariant_offset_applied);
  CHECK(local.affected_clade_count == owned.affected_clade_count);
  CHECK(local.valid == owned.valid);
  CHECK(local.invalid_reason == owned.invalid_reason);
  CHECK(local.local_score_ms >= 0.0);
}

static void test_local_score_into_workspace_contract() {
  std::println("test_local_score_into_workspace_contract");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   fixture.patterns);
  CHECK(fixture.candidates.size() >= 4);
  constexpr std::size_t candidate_count = 4;
  std::vector<larch::grammar_spr_candidate> candidates(
      fixture.candidates.begin(), fixture.candidates.begin() + candidate_count);

  // The old owning API remains the semantic oracle, but now promotes its
  // candidate payload only after delegating to `_into`.
  auto owning = larch::score_candidates_locally(state, candidates, {}, 1);
  CHECK(owning.size() == candidates.size());
  larch::chart_spr_local_score_workspace workspace;
  CHECK(workspace.operation_boundary_clean());
  std::vector<larch::chart_spr_local_score_result> local(candidates.size());
  auto growths_before = state.counters.local_row_scratch_capacity_growths;
  larch::score_candidates_locally_into(state, candidates, local, workspace, {},
                                       1);
  CHECK(workspace.operation_boundary_clean());
  auto growths_after_warm = state.counters.local_row_scratch_capacity_growths;
  CHECK(growths_after_warm > growths_before);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    check_local_result_matches_owned_score(local[i], owning[i]);
    auto promoted =
        larch::promote_chart_spr_local_score(candidates[i], local[i]);
    CHECK(larch::chart_spr_candidate_taxon_signature(state.grammar,
                                                     promoted.candidate) ==
          larch::chart_spr_candidate_taxon_signature(state.grammar,
                                                     candidates[i]));
    CHECK(!promoted.exact.has_value());
    CHECK(promoted.topology_selection.kind ==
          larch::chart_spr_topology_selection_kind::none);
    CHECK(promoted.canonical_exact_evidence == nullptr);
  }

  // Grow -> shrink -> reorder uses only the active span and retains the serial
  // row high-water mark.  Stale inactive result and prepared slots are never
  // observed.
  std::vector<larch::grammar_spr_candidate> reordered(candidates.rbegin(),
                                                      candidates.rend());
  reordered.pop_back();
  std::vector<larch::chart_spr_local_score_result> reordered_local(
      reordered.size());
  larch::score_candidates_locally_into(state, reordered, reordered_local,
                                       workspace, {}, 1);
  CHECK(workspace.operation_boundary_clean());
  CHECK(state.counters.local_row_scratch_capacity_growths ==
        growths_after_warm);
  for (std::size_t i = 0; i < reordered.size(); ++i) {
    check_local_result_matches_owned_score(reordered_local[i],
                                           owning[candidates.size() - 1 - i]);
  }

  std::reverse(reordered.begin(), reordered.end());
  reordered.push_back(candidates.back());
  reordered_local.resize(reordered.size());
  larch::score_candidates_locally_into(state, reordered, reordered_local,
                                       workspace, {}, 1);
  CHECK(workspace.operation_boundary_clean());
  CHECK(state.counters.local_row_scratch_capacity_growths ==
        growths_after_warm);

  // Checked and unchecked calls agree, and two-worker scratch reaches a stable
  // high-water mark when the same partition is replayed.
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  larch::chart_spr_local_score_workspace parallel_workspace;
  std::vector<larch::chart_spr_local_score_result> parallel(candidates.size());
  larch::score_candidates_locally_into(state, candidates, parallel,
                                       parallel_workspace, {}, 2, checked);
  CHECK(parallel_workspace.operation_boundary_clean());
  auto parallel_growths_after_warm =
      state.counters.local_row_scratch_capacity_growths;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    check_local_result_matches_owned_score(parallel[i], owning[i]);
  }
  larch::score_candidates_locally_into(state, candidates, parallel,
                                       parallel_workspace, {}, 2);
  CHECK(parallel_workspace.operation_boundary_clean());
  CHECK(state.counters.local_row_scratch_capacity_growths ==
        parallel_growths_after_warm);

  // Phase-4 candidate-axis scoring exposes four unit-grain ranges to three
  // requested workers, producing three persistent-scheduler runner tasks.
  // Counters describe actual successful submissions and the same workspace
  // remains reusable after a forced partial-submission failure.
  auto tasks_before = state.counters.local_score_worker_tasks;
  auto parallel_batches_before = state.counters.local_score_parallel_batches;
  larch::score_candidates_locally_into(state, candidates, parallel,
                                       parallel_workspace, {}, 3, checked);
  CHECK(parallel_workspace.operation_boundary_clean());
  CHECK(state.counters.local_score_worker_tasks - tasks_before == 3);
  CHECK(state.counters.local_score_parallel_batches - parallel_batches_before ==
        1);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    check_local_result_matches_owned_score(parallel[i], owning[i]);
  }

  larch::local_spr_score_options forced_submit_failure;
  larch::local_score_worker_barrier_for_tests submit_barrier;
  forced_submit_failure.force_worker_submit_failure_after_for_tests = 1;
  forced_submit_failure.worker_barrier_for_tests = &submit_barrier;
  auto failed_tasks_before = state.counters.local_score_worker_tasks;
  bool submit_threw = false;
  try {
    larch::score_candidates_locally_into(state, candidates, parallel,
                                         parallel_workspace,
                                         forced_submit_failure, 3, checked);
  } catch (std::runtime_error const& e) {
    submit_threw = true;
    CHECK(std::string{e.what()}.find("forced worker submission failure") !=
          std::string::npos);
  }
  CHECK(submit_threw);
  CHECK(submit_barrier.started.load() == 1);
  CHECK(submit_barrier.release.load());
  CHECK(state.counters.local_score_worker_tasks == failed_tasks_before);
  CHECK(parallel_workspace.operation_boundary_clean());

  larch::local_spr_score_options forced_worker_failure;
  larch::local_score_worker_barrier_for_tests worker_barrier;
  forced_worker_failure.worker_barrier_for_tests = &worker_barrier;
  forced_worker_failure.force_worker_failure_for_tests = 0;
  bool worker_threw = false;
  try {
    larch::score_candidates_locally_into(state, candidates, parallel,
                                         parallel_workspace,
                                         forced_worker_failure, 3, checked);
  } catch (std::runtime_error const& e) {
    worker_threw = true;
    CHECK(std::string{e.what()}.find("forced worker task failure") !=
          std::string::npos);
  }
  CHECK(worker_threw);
  CHECK(worker_barrier.started.load() >= 3);
  CHECK(worker_barrier.release.load());
  CHECK(state.counters.local_score_worker_tasks == failed_tasks_before);
  CHECK(parallel_workspace.operation_boundary_clean());

  larch::score_candidates_locally_into(state, candidates, parallel,
                                       parallel_workspace, {}, 3, checked);
  CHECK(parallel_workspace.operation_boundary_clean());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    check_local_result_matches_owned_score(parallel[i], owning[i]);
  }

  // A candidate-local failure does not poison either adjacent result or the
  // next operation.  In particular, valid output clears an old invalid reason.
  larch::grammar_spr_candidate bad;
  bad.removed_productions.push_back(larch::base_production_ref(
      state.grammar.productions_by_parent[state.grammar.root_clade].front()));
  std::vector<larch::grammar_spr_candidate> mixed{candidates.front(), bad,
                                                  candidates.back()};
  std::vector<larch::chart_spr_local_score_result> mixed_results(mixed.size());
  larch::score_candidates_locally_into(state, mixed, mixed_results, workspace,
                                       {}, 1);
  CHECK(workspace.operation_boundary_clean());
  CHECK(mixed_results[0].valid);
  CHECK(!mixed_results[1].valid);
  CHECK(!mixed_results[1].invalid_reason.empty());
  CHECK(mixed_results[2].valid);
  CHECK(mixed_results[2].invalid_reason.empty());

  larch::chart_spr_local_score_result one_result;
  one_result.valid = false;
  one_result.invalid_reason = "stale result sentinel";
  larch::score_candidates_locally_into(
      state,
      std::span<larch::grammar_spr_candidate const>{&candidates.front(), 1},
      std::span<larch::chart_spr_local_score_result>{&one_result, 1}, workspace,
      {}, 1);
  CHECK(workspace.operation_boundary_clean());
  CHECK(one_result.valid);
  CHECK(one_result.invalid_reason.empty());

  // Candidate and result owners may die immediately at the operation boundary;
  // the next operation cannot observe either address.
  {
    std::vector<larch::grammar_spr_candidate> ephemeral{candidates.back()};
    std::vector<larch::chart_spr_local_score_result> ephemeral_results(1);
    larch::score_candidates_locally_into(state, ephemeral, ephemeral_results,
                                         workspace, {}, 1);
    CHECK(ephemeral_results.front().valid);
  }
  CHECK(workspace.operation_boundary_clean());
  larch::score_candidates_locally_into(
      state,
      std::span<larch::grammar_spr_candidate const>{&candidates.front(), 1},
      std::span<larch::chart_spr_local_score_result>{&one_result, 1}, workspace,
      {}, 1);
  CHECK(workspace.operation_boundary_clean());
  CHECK(one_result.valid);

  // Empty batches are no-ops.  Size mismatch and exceptional scorer options
  // fail without publishing an operation-boundary borrow.
  larch::score_candidates_locally_into(
      state, std::span<larch::grammar_spr_candidate const>{},
      std::span<larch::chart_spr_local_score_result>{}, workspace, {}, 2);
  CHECK(workspace.operation_boundary_clean());
  bool mismatch_threw = false;
  try {
    larch::score_candidates_locally_into(
        state,
        std::span<larch::grammar_spr_candidate const>{&candidates.front(), 1},
        std::span<larch::chart_spr_local_score_result>{}, workspace, {}, 1);
  } catch (std::invalid_argument const& e) {
    mismatch_threw = true;
    CHECK(std::string{e.what()}.find("span size mismatch") !=
          std::string::npos);
  }
  CHECK(mismatch_threw);
  CHECK(workspace.operation_boundary_clean());

  larch::local_spr_score_options invalid_options;
  invalid_options.exact_multisite = true;
  bool option_threw = false;
  try {
    larch::score_candidates_locally_into(
        state,
        std::span<larch::grammar_spr_candidate const>{&candidates.front(), 1},
        std::span<larch::chart_spr_local_score_result>{&one_result, 1},
        workspace, invalid_options, 1);
  } catch (std::runtime_error const&) {
    option_threw = true;
  }
  CHECK(option_threw);
  CHECK(workspace.operation_boundary_clean());

  // A capability from another state and a same-generation grammar mutation
  // are both rejected before the workspace begins an operation.
  auto other_fixture = make_fixture();
  auto other_state = larch::build_chart_spr_search_state(
      other_fixture.dag, other_fixture.grammar, other_fixture.patterns);
  bool capability_threw = false;
  try {
    larch::score_candidates_locally_into(
        other_state,
        std::span<larch::grammar_spr_candidate const>{
            &other_fixture.candidates.front(), 1},
        std::span<larch::chart_spr_local_score_result>{&one_result, 1},
        workspace, {}, 1, checked);
  } catch (larch::chart_execution_plan_mismatch const&) {
    capability_threw = true;
  }
  CHECK(capability_threw);
  CHECK(workspace.operation_boundary_clean());

  auto stale_fixture = make_fixture();
  auto stale_state = larch::build_chart_spr_search_state(
      stale_fixture.dag, stale_fixture.grammar, stale_fixture.patterns);
  auto generation_before = stale_state.grammar.execution_generation;
  std::swap(stale_state.grammar.productions.front().children[0],
            stale_state.grammar.productions.front().children[1]);
  CHECK(stale_state.grammar.execution_generation == generation_before);
  auto mismatch_rejections_before =
      stale_state.counters.plan_mismatch_rejections;
  bool generation_threw = false;
  try {
    larch::score_candidates_locally_into(
        stale_state,
        std::span<larch::grammar_spr_candidate const>{
            &stale_fixture.candidates.front(), 1},
        std::span<larch::chart_spr_local_score_result>{&one_result, 1},
        workspace, {}, 1);
  } catch (larch::chart_execution_plan_mismatch const&) {
    generation_threw = true;
  }
  CHECK(generation_threw);
  CHECK(stale_state.counters.plan_mismatch_rejections ==
        mismatch_rejections_before + 1);
  CHECK(workspace.operation_boundary_clean());

  std::println("  PASS");
}

static void test_pattern_batch_into_parallel_scratch_plateau() {
  std::println("test_pattern_batch_into_parallel_scratch_plateau");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 4);
  candidates.resize(4);
  larch::chart_spr_search_options options;
  options.cache.max_cached_patterns = 1;
  options.cache.candidate_batch_size = candidates.size();
  auto state = larch::build_chart_spr_search_state(dag, grammar, options);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);

  auto owning = larch::score_candidates_locally(state, candidates, {}, 2);
  larch::chart_spr_local_score_workspace workspace;
  std::vector<larch::chart_spr_local_score_result> results(candidates.size());
  auto growths_before = state.counters.local_row_scratch_capacity_growths;
  larch::score_candidates_locally_into(state, candidates, results, workspace,
                                       {}, 2);
  CHECK(workspace.operation_boundary_clean());
  auto growths_after_warm = state.counters.local_row_scratch_capacity_growths;
  CHECK(growths_after_warm > growths_before);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    check_local_result_matches_owned_score(results[i], owning[i]);
  }

  larch::score_candidates_locally_into(state, candidates, results, workspace,
                                       {}, 2);
  CHECK(workspace.operation_boundary_clean());
  CHECK(state.counters.local_row_scratch_capacity_growths ==
        growths_after_warm);

  auto tasks_before = state.counters.local_score_worker_tasks;
  auto batches_before = state.counters.pattern_batch_cache_builds;
  larch::score_candidates_locally_into(state, candidates, results, workspace,
                                       {}, 3);
  CHECK(workspace.operation_boundary_clean());
  auto scored_pattern_batches =
      state.counters.pattern_batch_cache_builds - batches_before;
  CHECK(scored_pattern_batches > 0);
  CHECK(state.counters.local_score_worker_tasks - tasks_before ==
        3 * scored_pattern_batches);

  // Fail after one queued task.  The scorer must join that task while the
  // loop-local pattern-cache entries it references are still alive, then
  // return a clean, reusable operation boundary.
  larch::local_spr_score_options forced_submit_failure;
  larch::local_score_worker_barrier_for_tests submit_barrier;
  forced_submit_failure.force_worker_submit_failure_after_for_tests = 1;
  forced_submit_failure.worker_barrier_for_tests = &submit_barrier;
  auto failed_tasks_before = state.counters.local_score_worker_tasks;
  bool submit_threw = false;
  try {
    larch::score_candidates_locally_into(state, candidates, results, workspace,
                                         forced_submit_failure, 3);
  } catch (std::runtime_error const& e) {
    submit_threw = true;
    CHECK(std::string{e.what()}.find("forced worker submission failure") !=
          std::string::npos);
  }
  CHECK(submit_threw);
  CHECK(submit_barrier.started.load() == 1);
  CHECK(submit_barrier.release.load());
  CHECK(state.counters.local_score_worker_tasks == failed_tasks_before);
  CHECK(workspace.operation_boundary_clean());

  // A task-side exception is also selected deterministically only after every
  // submitted peer has reached the barrier and all futures have joined.
  larch::local_spr_score_options forced_worker_failure;
  larch::local_score_worker_barrier_for_tests worker_barrier;
  forced_worker_failure.worker_barrier_for_tests = &worker_barrier;
  forced_worker_failure.force_worker_failure_for_tests = 0;
  bool worker_threw = false;
  try {
    larch::score_candidates_locally_into(state, candidates, results, workspace,
                                         forced_worker_failure, 3);
  } catch (std::runtime_error const& e) {
    worker_threw = true;
    CHECK(std::string{e.what()}.find("forced worker task failure") !=
          std::string::npos);
  }
  CHECK(worker_threw);
  CHECK(worker_barrier.started.load() >= 3);
  CHECK(worker_barrier.release.load());
  CHECK(state.counters.local_score_worker_tasks == failed_tasks_before);
  CHECK(workspace.operation_boundary_clean());

  larch::score_candidates_locally_into(state, candidates, results, workspace,
                                       {}, 3);
  CHECK(workspace.operation_boundary_clean());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    check_local_result_matches_owned_score(results[i], owning[i]);
  }

  std::println("  PASS");
}

static void test_candidate_execution_plan_lifetime_and_mismatch_guards() {
  std::println(
      "test_candidate_execution_plan_lifetime_and_mismatch_guards");

  {
    auto fixture = make_fixture();
    auto state = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    auto source = std::make_unique<larch::grammar_spr_candidate>(
        fixture.candidates.front());
    larch::chart_spr_search_counters counters;
    auto prepared =
        larch::chart_spr_search_detail::prepare_local_candidate_score(
            state, *source, {}, &counters);
    CHECK(prepared.valid_for_accumulation);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);
    CHECK(counters.plan_mismatch_rejections == 0);
    source.reset();

    auto broken_entries = state.pattern_charts;
    CHECK(!broken_entries.empty());
    broken_entries.front().chart.inside.clear();
    auto builds_before = counters.candidate_execution_plan_builds;
    auto plan_hits_before = counters.chart_execution_plan_cache_hits;
    larch::chart_spr_local_score_scratch scratch;
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_patterns(
        state, prepared, 0, broken_entries, {}, &counters, scratch);
    CHECK(!prepared.valid_for_accumulation);
    CHECK(counters.candidate_execution_plan_builds == builds_before);
    CHECK(counters.chart_execution_plan_cache_hits == plan_hits_before);
  }

  {
    auto fixture = make_fixture();
    auto resident = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    std::optional<
        larch::chart_spr_search_detail::prepared_local_candidate_score>
        prepared;
    larch::chart_spr_search_counters counters;
    {
      auto source = larch::build_chart_spr_search_state(
          fixture.dag, fixture.grammar, fixture.patterns);
      CHECK(source.grammar.execution_generation ==
            resident.grammar.execution_generation);
      CHECK(source.execution_plan.fingerprint() ==
            resident.execution_plan.fingerprint());
      prepared.emplace(
          larch::chart_spr_search_detail::prepare_local_candidate_score(
              source, fixture.candidates.front(), {}, &counters));
      CHECK(prepared->valid_for_accumulation);
      CHECK(prepared->delta().base == nullptr);
    }

    // The source state is now dead.  The published descriptor owns its raw and
    // compiled arrays, retains no grammar pointer, and consumes the separately
    // checked equivalent resident snapshot without rebinding shared state.
    auto rows_before = counters.local_rows_recomputed;
    larch::chart_spr_local_score_scratch scratch;
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_patterns(
        resident, *prepared, 0, resident.pattern_charts, {}, &counters,
        scratch);
    CHECK(prepared->valid_for_accumulation);
    CHECK(prepared->delta().base == nullptr);
    CHECK(counters.plan_mismatch_rejections == 0);
    CHECK(counters.local_rows_recomputed > rows_before);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);
    CHECK(counters.candidate_execution_plan_cache_hits ==
          resident.active_patterns.patterns.patterns.size());
    auto scored =
        larch::chart_spr_search_detail::finish_prepared_local_candidate_score(
            resident, *prepared);
    CHECK(scored.valid);

    // A published descriptor is a shared, const recurrence input.  Exercise
    // the intended future pattern-axis use directly: several workers consume
    // the same descriptor and resident base while owning all row scratch.
    CHECK(!resident.pattern_charts.empty());
    auto const& shared_descriptor = prepared->delta();
    auto descriptor_snapshot = shared_descriptor;
    larch::leaf_site_states leaf_states{
        .state_by_taxon = resident.active_patterns.patterns.patterns.front()
                              .state_by_taxon};
    auto serial_rows = larch::build_local_overlay_chart_rows(
        resident.grammar, shared_descriptor,
        resident.pattern_charts.front().chart, leaf_states);
    std::vector<std::future<larch::local_overlay_chart_rows>> futures;
    for (std::size_t worker = 0; worker < 4; ++worker) {
      futures.push_back(std::async(std::launch::async, [&] {
        return larch::build_local_overlay_chart_rows(
            resident.grammar, shared_descriptor,
            resident.pattern_charts.front().chart, leaf_states);
      }));
    }
    for (auto& future : futures) {
      auto rows = future.get();
      CHECK(rows.rows == serial_rows.rows);
      CHECK(rows.base_row_slot.size() == serial_rows.base_row_slot.size());
      CHECK(rows.temp_row_slot.size() == serial_rows.temp_row_slot.size());
    }
    CHECK(shared_descriptor.base == nullptr);
    CHECK(shared_descriptor.temp_clades == descriptor_snapshot.temp_clades);
    CHECK(shared_descriptor.temp_productions.size() ==
          descriptor_snapshot.temp_productions.size());
    for (std::size_t i = 0; i < shared_descriptor.temp_productions.size();
         ++i) {
      CHECK(shared_descriptor.temp_productions[i].parent ==
            descriptor_snapshot.temp_productions[i].parent);
      CHECK(shared_descriptor.temp_productions[i].children ==
            descriptor_snapshot.temp_productions[i].children);
      CHECK(shared_descriptor.temp_productions[i].multiplicity ==
            descriptor_snapshot.temp_productions[i].multiplicity);
    }
    CHECK(shared_descriptor.affected_order ==
          descriptor_snapshot.affected_order);
    CHECK(shared_descriptor.affected_base_row_slot ==
          descriptor_snapshot.affected_base_row_slot);
    CHECK(shared_descriptor.affected_temp_row_slot ==
          descriptor_snapshot.affected_temp_row_slot);
    CHECK(shared_descriptor.reachability_stats ==
          descriptor_snapshot.reachability_stats);
    CHECK(shared_descriptor.base_plan_generation ==
          descriptor_snapshot.base_plan_generation);
    CHECK(shared_descriptor.base_plan_fingerprint ==
          descriptor_snapshot.base_plan_fingerprint);
    CHECK(shared_descriptor.candidate_plan_build_stats ==
          descriptor_snapshot.candidate_plan_build_stats);
    CHECK(shared_descriptor.compiled_rows ==
          descriptor_snapshot.compiled_rows);
    CHECK(shared_descriptor.compiled_productions ==
          descriptor_snapshot.compiled_productions);
    CHECK(shared_descriptor.compiled_children ==
          descriptor_snapshot.compiled_children);
    CHECK(shared_descriptor.removed_base_production ==
          descriptor_snapshot.removed_base_production);
    CHECK(shared_descriptor.reachable_base_clade ==
          descriptor_snapshot.reachable_base_clade);
    CHECK(shared_descriptor.reachable_temp_clade ==
          descriptor_snapshot.reachable_temp_clade);
    CHECK(shared_descriptor.temp_productions_by_base_parent ==
          descriptor_snapshot.temp_productions_by_base_parent);
    CHECK(shared_descriptor.temp_productions_by_temp_parent ==
          descriptor_snapshot.temp_productions_by_temp_parent);
    CHECK(shared_descriptor.temp_productions_by_base_child ==
          descriptor_snapshot.temp_productions_by_base_child);
    CHECK(shared_descriptor.temp_productions_by_temp_child ==
          descriptor_snapshot.temp_productions_by_temp_child);
  }

  {
    auto fixture = make_fixture();
    auto state = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    auto const generation_before = state.grammar.execution_generation;
    CHECK(!state.grammar.productions.empty());
    CHECK(state.grammar.productions.front().children.size() >= 2);
    std::swap(state.grammar.productions.front().children[0],
              state.grammar.productions.front().children[1]);
    CHECK(state.grammar.execution_generation == generation_before);

    auto const counters_before = state.counters;
    bool threw = false;
    std::string message;
    try {
      (void)larch::score_candidates_locally(
          state, {fixture.candidates.front()}, {}, 1);
    } catch (larch::chart_execution_plan_mismatch const& e) {
      threw = true;
      message = e.what();
    }
    CHECK(threw);
    CHECK(message.find("stale grammar fingerprint mismatch") !=
          std::string::npos);
    CHECK(state.counters.plan_mismatch_rejections ==
          counters_before.plan_mismatch_rejections + 1);
    CHECK(state.counters.local_rows_recomputed ==
          counters_before.local_rows_recomputed);
    CHECK(state.counters.local_candidate_scores ==
          counters_before.local_candidate_scores);
    CHECK(state.counters.candidate_execution_plan_builds ==
          counters_before.candidate_execution_plan_builds);
    CHECK(state.counters.chart_execution_plan_cache_hits ==
          counters_before.chart_execution_plan_cache_hits);
    CHECK(state.counters.candidate_execution_plan_cache_hits ==
          counters_before.candidate_execution_plan_cache_hits);
  }

  {
    auto fixture = make_fixture();
    auto state = larch::build_chart_spr_search_state(
        fixture.dag, fixture.grammar, fixture.patterns);
    larch::chart_spr_search_counters counters;
    auto prepared =
        larch::chart_spr_search_detail::prepare_local_candidate_score(
            state, fixture.candidates.front(), {}, &counters);
    CHECK(prepared.valid_for_accumulation);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);

    auto overlay = larch::overlay_from_candidate(
        state.grammar, fixture.candidates.front());
    auto materialized = larch::materialize_overlay_grammar(overlay);
    CHECK(materialized.grammar.execution_generation != 0);
    CHECK(materialized.grammar.execution_generation !=
          state.grammar.execution_generation);
    auto accepted_state = larch::build_chart_spr_search_state(
        fixture.dag, materialized.grammar, fixture.patterns);
    CHECK(accepted_state.grammar.execution_generation ==
          materialized.grammar.execution_generation);
    CHECK(accepted_state.execution_plan.grammar_generation() ==
          accepted_state.grammar.execution_generation);
    accepted_state.execution_plan.assert_compatible(accepted_state.grammar);

    auto rows_before = counters.local_rows_recomputed;
    auto candidate_hits_before = counters.candidate_execution_plan_cache_hits;
    larch::chart_spr_local_score_scratch scratch;
    larch::chart_spr_search_detail::accumulate_prepared_local_candidate_patterns(
        accepted_state, prepared, 0, accepted_state.pattern_charts, {},
        &counters, scratch);
    CHECK(!prepared.valid_for_accumulation);
    CHECK(counters.plan_mismatch_rejections == 1);
    CHECK(counters.local_rows_recomputed == rows_before);
    CHECK(counters.candidate_execution_plan_cache_hits ==
          candidate_hits_before);
    CHECK(counters.candidate_execution_plan_builds == 1);
    CHECK(counters.chart_execution_plan_cache_hits == 1);
    CHECK(counters.candidate_pattern_full_grammar_validations == 0);
    CHECK(counters.candidate_pattern_partition_validations == 0);
    CHECK(counters.candidate_pattern_clade_order_sorts == 0);
  }

  std::println("  PASS");
}

static void test_checked_candidate_sources_and_planned_materialization() {
  std::println(
      "test_checked_candidate_sources_and_planned_materialization");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto checked = larch::check_chart_execution_plan(state.grammar,
                                                    state.execution_plan);

  std::size_t full_validations = 0;
  std::size_t partition_validations = 0;
  std::size_t clade_sorts = 0;
  larch::parsimony_chart_detail::structural_work_observer structural_observer{
      &full_validations, &partition_validations, &clade_sorts};
  std::size_t compatibility_checks = 0;
  std::size_t fingerprint_scans = 0;
  std::size_t legacy_index_validations = 0;
  std::size_t dynamic_overlay_partition_validations = 0;
  larch::chart_execution_plan_detail::compatibility_work_observer
      compatibility_observer{&compatibility_checks, &fingerprint_scans,
                             &legacy_index_validations,
                             &dynamic_overlay_partition_validations};

  auto enumerate = [&](larch::chart_spr_candidate_source source,
                       bool trusted) {
    larch::grammar_spr_enumeration_options options;
    options.source = source;
    options.max_candidates = 2;
    options.max_candidates_is_post_dedup = true;
    if (source != larch::chart_spr_candidate_source::grammar) {
      options.sampled_tree_source_dag = &fixture.dag;
      options.sampled_tree_count = 1;
    }
    std::size_t emitted = 0;
    if (trusted) {
      (void)larch::for_each_grammar_spr_candidate(
          state.grammar, checked, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    } else if (source == larch::chart_spr_candidate_source::sampled_tree) {
      (void)larch::for_each_sampled_tree_spr_candidate(
          state.grammar, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    } else if (source == larch::chart_spr_candidate_source::hybrid) {
      (void)larch::for_each_hybrid_spr_candidate(
          state.grammar, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    } else {
      (void)larch::for_each_grammar_spr_candidate(
          state.grammar, options,
          [&](larch::grammar_spr_candidate const&) {
            ++emitted;
            return true;
          });
    }
    return emitted;
  };

  {
    larch::parsimony_chart_detail::structural_work_observer_scope structural{
        &structural_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    (void)enumerate(larch::chart_spr_candidate_source::grammar, true);
    (void)enumerate(larch::chart_spr_candidate_source::sampled_tree, true);
    (void)enumerate(larch::chart_spr_candidate_source::hybrid, true);
  }
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
  CHECK(compatibility_checks == 0);
  CHECK(fingerprint_scans == 0);
  CHECK(legacy_index_validations == 0);
  CHECK(dynamic_overlay_partition_validations == 0);

  {
    larch::parsimony_chart_detail::structural_work_observer_scope structural{
        &structural_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    (void)enumerate(larch::chart_spr_candidate_source::grammar, false);
    (void)enumerate(larch::chart_spr_candidate_source::sampled_tree, false);
    (void)enumerate(larch::chart_spr_candidate_source::hybrid, false);
  }
  CHECK(full_validations == 3);
  CHECK(legacy_index_validations == 3);
  // The recurrence observer is intentionally independent of the legacy
  // whole-grammar/index boundary observer used by candidate enumeration.
  CHECK(partition_validations == 0);
  CHECK(compatibility_checks == 0);
  CHECK(fingerprint_scans == 0);
  CHECK(dynamic_overlay_partition_validations == 0);

  full_validations = 0;
  partition_validations = 0;
  clade_sorts = 0;
  legacy_index_validations = 0;
  dynamic_overlay_partition_validations = 0;
  larch::planned_overlay_materialization_result planned;
  {
    larch::parsimony_chart_detail::structural_work_observer_scope structural{
        &structural_observer};
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    planned = larch::materialize_candidate_overlay_grammar_with_plan(
        state.grammar, checked, fixture.candidates.front());
  }
  CHECK(full_validations == 0);
  CHECK(partition_validations == 0);
  CHECK(clade_sorts == 0);
  CHECK(legacy_index_validations == 0);
  CHECK(compatibility_checks == 0);
  CHECK(fingerprint_scans == 0);
  CHECK(dynamic_overlay_partition_validations ==
        fixture.candidates.front().added_productions.size());
  CHECK(planned.payload_validation_stats.production_partition_validations ==
        fixture.candidates.front().added_productions.size());
  CHECK(planned.execution_plan.build_stats().plan_builds == 1);
  CHECK(planned.execution_plan.build_stats().full_grammar_validations == 1);
  CHECK(planned.execution_plan.build_stats().clade_order_sorts == 2);
  CHECK(planned.execution_plan.grammar_generation() ==
        planned.materialized.grammar.execution_generation);
  CHECK(planned.materialized.grammar.execution_generation !=
        state.grammar.execution_generation);

  // A valid but unreachable dynamic production is deliberately absent from
  // the dense grammar/plan while remaining visible in the planned-build and
  // search-level boundary counters.
  auto unreachable = larch::overlay_from_candidate(
      state.grammar, checked, fixture.candidates.front());
  unreachable.temp_clades.push_back(larch::clade_key{{0, 1}});
  larch::clade_id leaf_zero = larch::no_clade;
  larch::clade_id leaf_one = larch::no_clade;
  for (larch::clade_id cid = 0; cid < state.grammar.clades.size(); ++cid) {
    if (state.grammar.clades[cid].taxa == std::vector<larch::taxon_id>{0}) {
      leaf_zero = cid;
    } else if (state.grammar.clades[cid].taxa ==
               std::vector<larch::taxon_id>{1}) {
      leaf_one = cid;
    }
  }
  CHECK(leaf_zero != larch::no_clade);
  CHECK(leaf_one != larch::no_clade);
  larch::overlay_grammar_production unreachable_production;
  unreachable_production.parent = larch::temp_clade_ref(
      static_cast<larch::clade_id>(unreachable.temp_clades.size() - 1));
  unreachable_production.children = {larch::base_clade_ref(leaf_zero),
                                     larch::base_clade_ref(leaf_one)};
  unreachable.temp_productions.push_back(
      std::move(unreachable_production));
  dynamic_overlay_partition_validations = 0;
  larch::planned_overlay_materialization_result unreachable_planned;
  {
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    unreachable_planned = larch::materialize_overlay_grammar_with_plan(
        unreachable, checked);
  }
  CHECK(unreachable_planned.materialized.temp_clade_to_dense.back() ==
        larch::no_clade);
  CHECK(unreachable_planned.materialized.temp_production_to_dense.back() ==
        larch::no_production);
  CHECK(dynamic_overlay_partition_validations ==
        unreachable.temp_productions.size());
  CHECK(unreachable_planned.payload_validation_stats
            .production_partition_validations ==
        unreachable.temp_productions.size());
  larch::chart_spr_search_counters boundary_counters;
  larch::record_planned_overlay_materialization_stats(
      boundary_counters, unreachable_planned);
  CHECK(boundary_counters.dynamic_overlay_payload_partition_validations ==
        unreachable.temp_productions.size());
  CHECK(boundary_counters.candidate_pattern_partition_validations == 0);

  // The callback overload is constrained: a literal null selects the
  // no-callback stats-pointer shape for overlay, candidate, and chain APIs.
  // A typed stats pointer remains available on both success and failure.
  bool typed_dense_completed = false;
  larch::overlay_payload_validation_stats typed_payload_stats;
  auto typed_stats_planned = larch::materialize_overlay_grammar_with_plan(
      unreachable, checked, &typed_dense_completed, &typed_payload_stats);
  CHECK(typed_dense_completed);
  CHECK(typed_payload_stats.production_partition_validations ==
        unreachable.temp_productions.size());
  CHECK(typed_stats_planned.payload_validation_stats
            .production_partition_validations ==
        typed_payload_stats.production_partition_validations);

  auto literal_null_overlay = larch::materialize_overlay_grammar_with_plan(
      unreachable, checked, nullptr, nullptr);
  CHECK(literal_null_overlay.payload_validation_stats
            .production_partition_validations ==
        unreachable.temp_productions.size());
  auto literal_null_candidate =
      larch::materialize_candidate_overlay_grammar_with_plan(
          state.grammar, checked, fixture.candidates.front(), nullptr,
          nullptr);
  CHECK(literal_null_candidate.payload_validation_stats
            .production_partition_validations ==
        fixture.candidates.front().added_productions.size());
  larch::overlay_chain empty_chain{state.grammar};
  auto literal_null_chain = larch::materialize_overlay_chain_with_plan(
      empty_chain, checked, nullptr, nullptr);
  CHECK(literal_null_chain.payload_validation_stats
            .production_partition_validations == 0);
  CHECK(literal_null_chain.materialized.grammar.productions.size() ==
        state.grammar.productions.size());

  // An invalid dynamic production must likewise be rejected even though it is
  // unreachable and therefore absent from the dense output plan.
  auto malformed = unreachable;
  malformed.temp_productions.back().children = {
      larch::base_clade_ref(leaf_zero), larch::base_clade_ref(leaf_zero)};
  bool dense_completed = false;
  bool threw = false;
  larch::overlay_payload_validation_stats failed_payload_stats;
  dynamic_overlay_partition_validations = 0;
  {
    larch::chart_execution_plan_detail::compatibility_work_observer_scope
        compatibility{&compatibility_observer};
    try {
      (void)larch::materialize_overlay_grammar_with_plan(
          malformed, checked, &dense_completed, &failed_payload_stats);
    } catch (std::runtime_error const& e) {
      threw = true;
      CHECK(std::string{e.what()}.find("children overlap") !=
            std::string::npos);
    }
  }
  CHECK(threw);
  CHECK(!dense_completed);
  CHECK(dynamic_overlay_partition_validations ==
        malformed.temp_productions.size());
  CHECK(failed_payload_stats.production_partition_validations ==
        malformed.temp_productions.size());

  std::println("  PASS");
}

static void test_acceptance_iteration_checks_resident_plan_once() {
  std::println("test_acceptance_iteration_checks_resident_plan_once");

  for (std::size_t candidate_batch_size : {std::size_t{1}, std::size_t{4}}) {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", four_taxon_misplaced_tree());
    auto grammar = larch::build_clade_grammar(dag);
    auto state = larch::build_chart_spr_search_state(dag, grammar);

    larch::chart_spr_search_options options;
    options.acceptance_mode =
        larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 3;
    options.enumeration.max_candidates = 6;
    options.enumeration.max_candidates_is_post_dedup = true;
    options.cache.candidate_batch_size = candidate_batch_size;
    options.semantic_capture =
        larch::chart_spr_semantic_capture_mode::digest;

    std::size_t compatibility_checks = 0;
    std::size_t fingerprint_scans = 0;
    std::size_t legacy_index_validations = 0;
    std::size_t dynamic_overlay_partition_validations = 0;
    larch::chart_execution_plan_detail::compatibility_work_observer observer{
        &compatibility_checks, &fingerprint_scans,
        &legacy_index_validations,
        &dynamic_overlay_partition_validations};
    auto dynamic_counter_before =
        state.counters.dynamic_overlay_payload_partition_validations;
    larch::chart_spr_iteration_result iteration;
    {
      larch::chart_execution_plan_detail::compatibility_work_observer_scope
          scope{&observer};
      iteration =
          larch::run_chart_spr_acceptance_iteration(state, options);
    }
    CHECK(iteration.candidates_scored > 0);
    CHECK(iteration.candidates_exact_verified > 0);
    CHECK(compatibility_checks == 1);
    CHECK(fingerprint_scans == 1);
    CHECK(legacy_index_validations == 0);
    CHECK(dynamic_overlay_partition_validations > 0);
    CHECK(state.counters.dynamic_overlay_payload_partition_validations -
              dynamic_counter_before ==
          dynamic_overlay_partition_validations);
    CHECK(state.counters.candidate_pattern_partition_validations == 0);
  }

  std::println("  PASS");
}

static larch::chart_spr_lazy_selected_topology_entry
make_phase7_selected_topology_child(
    std::vector<std::size_t> class_index_by_pattern, std::size_t row_count) {
  larch::chart_spr_lazy_selected_topology_entry entry;
  entry.class_index_by_pattern = std::move(class_index_by_pattern);
  for (std::size_t state = 0; state < row_count; ++state) {
    auto row = larch::parsimony_chart_detail::make_inf_row();
    row[state] = 0;
    entry.rows.push_back(row);
  }
  return entry;
}

// This is the removed implementation, retained only as a test oracle: ordered
// vector keys define traversal order, and the first row seen in that order
// defines each selected-topology row-class ID.
static larch::chart_spr_lazy_selected_topology_entry
phase7_ordered_selected_topology_entry_oracle(
    std::size_t pattern_count,
    std::vector<larch::chart_spr_lazy_selected_topology_entry const*> const&
        child_entries) {
  larch::chart_spr_lazy_selected_topology_entry entry;
  entry.class_index_by_pattern.assign(pattern_count, 0);
  std::map<std::vector<std::size_t>, std::vector<std::size_t>>
      patterns_by_context;
  for (std::size_t pattern = 0; pattern < pattern_count; ++pattern) {
    std::vector<std::size_t> key;
    key.reserve(child_entries.size());
    for (auto const* child_entry : child_entries) {
      key.push_back(child_entry->class_index_by_pattern[pattern]);
    }
    patterns_by_context[std::move(key)].push_back(pattern);
  }

  std::map<larch::chart_multisite_detail::chart_row, std::size_t> class_by_row;
  for (auto const& [key, members] : patterns_by_context) {
    std::vector<larch::chart_multisite_detail::chart_row> child_rows;
    child_rows.reserve(child_entries.size());
    for (std::size_t child = 0; child < child_entries.size(); ++child) {
      child_rows.push_back(child_entries[child]->rows.at(key[child]));
    }
    auto const row = larch::chart_multisite_detail::combine_rows(
        std::span<larch::chart_multisite_detail::chart_row const>{
            child_rows.data(), child_rows.size()});
    auto [row_it, inserted] = class_by_row.emplace(row, class_by_row.size());
    if (inserted) entry.rows.push_back(row);
    for (auto pattern : members) {
      entry.class_index_by_pattern[pattern] = row_it->second;
    }
  }
  return entry;
}

static void test_phase7_packed_lazy_selected_topology_grouping_contract() {
  std::println("test_phase7_packed_lazy_selected_topology_grouping_contract");
  using larch::lazy_key_grouping_detail::packed_key_word;
  using selected_entry = larch::chart_spr_lazy_selected_topology_entry;
  using selected_workspace =
      larch::chart_spr_lazy_selected_topology_key_workspace;

  auto binary_left = make_phase7_selected_topology_child({0, 1, 0, 2, 2, 3}, 4);
  auto binary_right =
      make_phase7_selected_topology_child({0, 0, 1, 2, 2, 2}, 3);
  std::vector<selected_entry const*> binary_children{&binary_left,
                                                     &binary_right};
  selected_workspace binary_workspace;
  {
    auto const keys =
        larch::chart_spr_collect_lazy_selected_topology_context_keys(
            6, binary_children, binary_workspace);
    CHECK(keys.key_width == 2);
    CHECK((std::vector<packed_key_word>{keys.key_words().begin(),
                                        keys.key_words().end()} ==
           std::vector<packed_key_word>{0, 0, 1, 0, 0, 1, 2, 2, 2, 2, 3, 2}));
    CHECK((keys.classes().class_by_input ==
           std::vector<std::size_t>{0, 1, 2, 3, 3, 4}));
    CHECK((keys.classes().representative_by_class ==
           std::vector<std::size_t>{0, 1, 2, 3, 5}));
    CHECK((keys.classes().lexicographic_class_order ==
           std::vector<std::size_t>{0, 2, 1, 3, 4}));
    CHECK(
        (std::vector<std::size_t>{keys.classes().members_for_class(3).begin(),
                                  keys.classes().members_for_class(3).end()} ==
         std::vector<std::size_t>{3, 4}));
    CHECK(keys.memory.key_count == 6);
    CHECK(keys.memory.key_width == 2);
    CHECK(keys.memory.class_count == 5);
    CHECK(
        keys.memory.logical_resident_bytes ==
        larch::
            estimate_chart_spr_lazy_selected_topology_key_grouping_logical_resident_bytes(
                6, 2, 5));
    CHECK(keys.memory.actual_capacity_resident_bytes ==
          sizeof(larch::chart_spr_packed_lazy_selected_topology_keys) +
              binary_workspace.resident_bytes());
    CHECK(keys.memory.actual_capacity_resident_bytes >=
          keys.memory.logical_resident_bytes);
    CHECK(keys.memory.observed_prepublication_peak_capacity_resident_bytes >=
          keys.memory.actual_capacity_resident_bytes);
  }
  {
    auto const reused_binary =
        larch::chart_spr_collect_lazy_selected_topology_context_keys(
            6, binary_children, binary_workspace);
    CHECK(reused_binary.memory.word_preparation.reused_existing_capacity);
    CHECK(reused_binary.memory.grouping_preparation.reused_existing_capacity);
  }

  auto binary_oracle =
      phase7_ordered_selected_topology_entry_oracle(6, binary_children);
  selected_entry binary_actual;
  larch::chart_spr_search_counters binary_counters;
  larch::chart_spr_assign_lazy_selected_topology_internal_entry(
      binary_actual, 6, binary_children, binary_workspace, binary_counters);
  CHECK(binary_actual.rows == binary_oracle.rows);
  CHECK(binary_actual.class_index_by_pattern ==
        binary_oracle.class_index_by_pattern);
  CHECK(binary_counters.selected_topology_multifurcation_rows == 0);

  auto multifurc_first =
      make_phase7_selected_topology_child({0, 1, 0, 1, 0, 2}, 3);
  auto multifurc_second =
      make_phase7_selected_topology_child({0, 1, 0, 0, 1, 2}, 3);
  auto multifurc_third =
      make_phase7_selected_topology_child({0, 0, 1, 2, 2, 2}, 3);
  std::vector<selected_entry const*> multifurc_children{
      &multifurc_first, &multifurc_second, &multifurc_third};
  selected_workspace multifurc_workspace;
  {
    auto const keys =
        larch::chart_spr_collect_lazy_selected_topology_context_keys(
            6, multifurc_children, multifurc_workspace);
    CHECK(keys.key_width == 3);
    CHECK((keys.classes().class_by_input ==
           std::vector<std::size_t>{0, 1, 2, 3, 4, 5}));
    CHECK((keys.classes().representative_by_class ==
           std::vector<std::size_t>{0, 1, 2, 3, 4, 5}));
    CHECK((keys.classes().lexicographic_class_order ==
           std::vector<std::size_t>{0, 2, 4, 3, 1, 5}));
  }
  auto multifurc_oracle =
      phase7_ordered_selected_topology_entry_oracle(6, multifurc_children);
  selected_entry multifurc_actual;
  larch::chart_spr_search_counters multifurc_counters;
  multifurc_counters.selected_topology_multifurcation_rows = 7;
  larch::chart_spr_assign_lazy_selected_topology_internal_entry(
      multifurc_actual, 6, multifurc_children, multifurc_workspace,
      multifurc_counters);
  CHECK(multifurc_actual.rows == multifurc_oracle.rows);
  CHECK(multifurc_actual.class_index_by_pattern ==
        multifurc_oracle.class_index_by_pattern);
  CHECK(multifurc_counters.selected_topology_multifurcation_rows == 13);

  auto const word_max =
      static_cast<std::size_t>((std::numeric_limits<packed_key_word>::max)());
  auto narrowing_child = make_phase7_selected_topology_child({word_max}, 0);
  std::vector<selected_entry const*> narrowing_children{&narrowing_child};
  selected_workspace narrowing_workspace;
  {
    auto const keys =
        larch::chart_spr_collect_lazy_selected_topology_context_keys(
            1, narrowing_children, narrowing_workspace);
    CHECK(keys.key_words().front() ==
          (std::numeric_limits<packed_key_word>::max)());
  }
  if ((std::numeric_limits<std::size_t>::max)() > word_max) {
    narrowing_child.class_index_by_pattern[0] = word_max + 1;
    bool threw = false;
    try {
      (void)larch::chart_spr_collect_lazy_selected_topology_context_keys(
          1, narrowing_children, narrowing_workspace);
    } catch (std::overflow_error const& e) {
      threw = true;
      auto const message = std::string{e.what()};
      CHECK(message.find("selected-topology child class index") !=
            std::string::npos);
      CHECK(message.find("does not fit") != std::string::npos);
    }
    CHECK(threw);
  }

  CHECK(
      larch::
          estimate_chart_spr_lazy_selected_topology_key_grouping_logical_resident_bytes(
              6, 3, 6) > sizeof(selected_workspace));
  bool accounting_overflow = false;
  try {
    (void)larch::
        estimate_chart_spr_lazy_selected_topology_key_grouping_logical_resident_bytes(
            (std::numeric_limits<std::size_t>::max)(), 2, 1);
  } catch (std::overflow_error const& e) {
    accounting_overflow = true;
    CHECK(std::string{e.what()}.find("overflow") != std::string::npos);
  }
  CHECK(accounting_overflow);

  // Pattern zero is first-occurrence class zero but lexicographically second.
  // Its middle-child class is invalid. The valid lexicographic predecessor
  // must publish one row before the error, while the multifurcation counter
  // remains delayed until the whole production succeeds.
  auto failure_first = make_phase7_selected_topology_child({0, 0}, 1);
  auto failure_middle = make_phase7_selected_topology_child({1, 0}, 1);
  auto failure_last = make_phase7_selected_topology_child({0, 0}, 1);
  std::vector<selected_entry const*> failure_children{
      &failure_first, &failure_middle, &failure_last};
  selected_entry partial_entry;
  selected_workspace failure_workspace;
  larch::chart_spr_search_counters failure_counters;
  failure_counters.selected_topology_multifurcation_rows = 7;
  bool failed_in_lex_order = false;
  try {
    larch::chart_spr_assign_lazy_selected_topology_internal_entry(
        partial_entry, 2, failure_children, failure_workspace,
        failure_counters);
  } catch (std::runtime_error const& e) {
    failed_in_lex_order = true;
    CHECK(std::string{e.what()}.find("child class index out of range") !=
          std::string::npos);
  }
  CHECK(failed_in_lex_order);
  CHECK(partial_entry.rows.size() == 1);
  CHECK(failure_counters.selected_topology_multifurcation_rows == 7);

  std::println("  PASS");
}

static void test_lazy_cache_fixed_topology_conservative_search() {
  std::println("test_lazy_cache_fixed_topology_conservative_search");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);
  auto dense_state = larch::build_chart_spr_search_state(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;
  options.cache.use_lazy_multisite_chart = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  auto dense_scores = larch::fixed_topology_direct_selected_pattern_scores(
      dense_state, *iteration.accepted);
  auto dense_old_full = larch::chart_spr_add_invariant_offset(
      dense_scores.old_active_total, dense_state,
      "lazy fixed-topology dense oracle old score");
  auto dense_new_full = larch::chart_spr_add_invariant_offset(
      dense_scores.new_active_total, dense_state,
      "lazy fixed-topology dense oracle new score");
  CHECK(iteration.accepted->exact->value.old_score == dense_old_full);
  CHECK(iteration.accepted->exact->value.new_score == dense_new_full);
  CHECK(search.counters.local_candidate_scores > 0);
  CHECK(search.counters.local_rows_recomputed > 0);
  CHECK(search.counters.fixed_topology_selected_rows_computed > 0);
  CHECK(search.counters.selected_topology_class_rows_computed ==
        search.counters.fixed_topology_selected_rows_computed);
  CHECK(search.counters.selected_topology_multifurcation_rows > 0);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(search.counters.multifurcation_productions_scored > 0);
  CHECK(search.counters.lazy_local_admission_waves > 0);
  CHECK(search.summary.lazy_local_admission_waves ==
        search.counters.lazy_local_admission_waves);
  CHECK(search.summary.lazy_local_parallel_waves ==
        search.counters.lazy_local_parallel_waves);
  CHECK(search.summary.lazy_local_memory_limited_waves ==
        search.counters.lazy_local_memory_limited_waves);
  CHECK(search.summary.lazy_local_admitted_concurrency_max ==
        search.counters.lazy_local_admitted_concurrency_max);
  CHECK(search.summary.lazy_local_prepared_tasks ==
        search.counters.lazy_local_prepared_tasks);
  CHECK(search.summary.lazy_local_reused_prepared_tasks ==
        search.counters.lazy_local_reused_prepared_tasks);
  CHECK(search.summary.lazy_local_pre_submit_budget_failures ==
        search.counters.lazy_local_pre_submit_budget_failures);
  CHECK(search.summary.lazy_local_peak_admitted_bytes ==
        search.counters.lazy_local_peak_admitted_bytes);
  CHECK(search.summary.lazy_local_peak_projected_resident_bytes ==
        search.counters.lazy_local_peak_projected_resident_bytes);
  CHECK(search.summary.lazy_local_preparation_peak_bytes ==
        search.counters.lazy_local_preparation_peak_bytes);
  CHECK(search.summary.lazy_local_result_output_resident_bytes_max ==
        search.counters.lazy_local_result_output_resident_bytes_max);
  CHECK(search.summary.lazy_local_retained_exact_trim_bytes_max ==
        search.counters.lazy_local_retained_exact_trim_bytes_max);
  CHECK(search.summary.lazy_local_canonical_exact_evidence_resident_bytes_max ==
        search.counters
            .lazy_local_canonical_exact_evidence_resident_bytes_max);
  CHECK(
      search.summary
              .lazy_local_canonical_exact_evidence_construction_peak_bytes_max ==
      search.counters
          .lazy_local_canonical_exact_evidence_construction_peak_bytes_max);
  CHECK(search.summary.lazy_local_runtime_transient_reservation_bytes_max ==
        search.counters.lazy_local_runtime_transient_reservation_bytes_max);
  CHECK(search.summary.lazy_local_iteration_envelope_bytes_max ==
        search.counters.lazy_local_iteration_envelope_bytes_max);
  CHECK(search.summary.lazy_local_iteration_task_stable_bytes_max ==
        search.counters.lazy_local_iteration_task_stable_bytes_max);
  CHECK(search.summary.lazy_local_iteration_task_preparation_peak_bytes_max ==
        search.counters.lazy_local_iteration_task_preparation_peak_bytes_max);
  // The explicit lazy-on request is rebuilt directly; only automatic
  // decisions use the private frozen-policy handoff.
  CHECK(search.counters.lazy_policy_pilot_runs == 0);
  CHECK(search.counters.lazy_policy_frozen_reuses == 0);
  CHECK(search.summary.lazy_inside_rows_computed > 0);
  CHECK(search.summary.lazy_merge_ratio > 0.0);
  CHECK(search.summary.selected_topology_class_rows_computed ==
        search.counters.selected_topology_class_rows_computed);
  CHECK(search.summary.final_score < search.summary.initial_score);
  auto rebuilt = larch::build_clade_grammar(search.dag, gopts);
  CHECK(max_production_arity(rebuilt) == 3);

  std::println("  PASS");
}

static void test_lazy_cache_local_commit_updates_lazy_chart() {
  std::println("test_lazy_cache_local_commit_updates_lazy_chart");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.cache.use_lazy_multisite_chart = true;
  options.verify_local_commit_two_chart_oracle_for_tests = true;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.counters.local_commit_two_chart_oracle_runs == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_incremental_rows_recomputed ==
        search.counters.lazy_inside_rows_recomputed_on_commit +
            search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.counters.lazy_inside_rows_recomputed_on_commit <=
        search.counters.inside_rows_recomputed_on_commit);
  CHECK(search.counters.lazy_outside_rows_recomputed_on_commit <=
        search.counters.outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_inside_rows_recomputed_on_commit ==
        search.counters.lazy_inside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_outside_rows_recomputed_on_commit ==
        search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_incremental_rows_recomputed ==
        search.counters.lazy_incremental_rows_recomputed);
  CHECK(search.summary.lazy_inside_rows_computed > 0);
  CHECK(search.summary.lazy_merge_ratio > 0.0);
  auto lazy_denominator =
      static_cast<double>(search.summary.active_pattern_count) *
      static_cast<double>(search.summary.final_grammar_clade_count);
  CHECK(lazy_denominator > 0.0);
  CHECK(search.summary.lazy_merge_ratio ==
        static_cast<double>(search.summary.lazy_inside_rows_computed) /
            lazy_denominator);
  CHECK(search.canonical_digest.has_value());

  std::println("  PASS");
}

static void test_parallel_local_scores_match_serial() {
  std::println("test_parallel_local_scores_match_serial");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  CHECK(candidates.size() >= 2);
  if (candidates.size() > 6) candidates.resize(6);

  auto serial_state = larch::build_chart_spr_search_state(dag, grammar);
  auto parallel_state = larch::build_chart_spr_search_state(dag, grammar);
  auto serial = larch::score_candidates_locally(serial_state, candidates, {}, 1);
  auto parallel = larch::score_candidates_locally(
      parallel_state, candidates, {}, 2);

  CHECK(serial.size() == parallel.size());
  for (std::size_t i = 0; i < serial.size(); ++i) {
    CHECK(serial[i].valid == parallel[i].valid);
    CHECK(serial[i].lower_bound.value.old_score ==
          parallel[i].lower_bound.value.old_score);
    CHECK(serial[i].lower_bound.value.new_score ==
          parallel[i].lower_bound.value.new_score);
    CHECK(serial[i].lower_bound.value.delta ==
          parallel[i].lower_bound.value.delta);
    CHECK(serial[i].affected_clade_count == parallel[i].affected_clade_count);
  }
  CHECK(parallel_state.counters.local_candidate_scores == candidates.size());
  CHECK(parallel_state.counters.local_score_parallel_batches == 1);
  CHECK(parallel_state.counters.local_score_worker_tasks == 2);
  CHECK(parallel_state.counters.local_rows_recomputed ==
        serial_state.counters.local_rows_recomputed);

  larch::chart_spr_search_options batched_options;
  batched_options.cache.max_cached_patterns = 1;
  batched_options.cache.candidate_batch_size = candidates.size();
  auto batched_serial_state = larch::build_chart_spr_search_state(
      dag, grammar, batched_options);
  auto batched_parallel_state = larch::build_chart_spr_search_state(
      dag, grammar, batched_options);
  auto batched_serial = larch::score_candidates_locally(
      batched_serial_state, candidates, {}, 1);
  auto batched_parallel = larch::score_candidates_locally(
      batched_parallel_state, candidates, {}, 2);
  CHECK(batched_serial.size() == batched_parallel.size());
  for (std::size_t i = 0; i < batched_serial.size(); ++i) {
    CHECK(batched_serial[i].valid == batched_parallel[i].valid);
    CHECK(batched_serial[i].lower_bound.value.old_score ==
          batched_parallel[i].lower_bound.value.old_score);
    CHECK(batched_serial[i].lower_bound.value.new_score ==
          batched_parallel[i].lower_bound.value.new_score);
    CHECK(batched_serial[i].lower_bound.value.delta ==
          batched_parallel[i].lower_bound.value.delta);
  }
  CHECK(batched_parallel_state.counters.local_score_parallel_batches == 1);
  CHECK(batched_parallel_state.counters.pattern_batch_cache_builds >=
        batched_parallel_state.active_patterns.patterns.patterns.size());
  CHECK(batched_parallel_state.counters.local_rows_recomputed ==
        batched_serial_state.counters.local_rows_recomputed);

  std::println("  PASS");
}

// Phase 4 switches adaptively between coarse candidate tasks and
// candidate-by-pattern tiles.  Barriers make real worker overlap deterministic
// in both branches, while stable result slots preserve exact W1 ordering and
// reductions.  The same scheduler must remain reusable after a tile worker
// throws and every launched peer has joined.
static void test_phase4_candidate_and_pattern_tile_axes() {
  std::println("test_phase4_candidate_and_pattern_tile_axes");

  auto fixture = make_fixture();
  auto patterns = make_phase4_wide_patterns();
  auto serial_state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, patterns);
  auto parallel_state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, patterns);
  CHECK(serial_state.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);
  CHECK(fixture.candidates.size() >= 4);
  std::vector<larch::grammar_spr_candidate> candidates(
      fixture.candidates.begin(), fixture.candidates.begin() + 4);

  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  auto checked = larch::check_chart_execution_plan(
      parallel_state.grammar, parallel_state.execution_plan);

  std::vector<larch::chart_spr_local_score_result> serial_candidates(
      candidates.size());
  std::vector<larch::chart_spr_local_score_result> parallel_candidates(
      candidates.size());
  larch::chart_spr_local_score_workspace serial_workspace;
  larch::chart_spr_local_score_workspace parallel_workspace;
  larch::score_candidates_locally_into(
      serial_state, candidates, serial_candidates, serial_workspace, {}, 1);
  larch::local_score_worker_barrier_for_tests candidate_barrier;
  larch::local_spr_score_options candidate_options;
  candidate_options.worker_barrier_for_tests = &candidate_barrier;
  larch::score_candidates_locally_into(parallel_state, candidates,
                                       parallel_candidates, parallel_workspace,
                                       candidate_options, scheduler, checked);
  CHECK(candidate_barrier.release.load());
  CHECK(candidate_barrier.started.load() >= 4);
  CHECK(parallel_workspace.operation_boundary_clean());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    check_local_result_matches_owned_score(
        parallel_candidates[index],
        larch::promote_chart_spr_local_score(candidates[index],
                                             serial_candidates[index]));
  }
  auto const& candidate_axis =
      parallel_state.counters.scheduler_axes.local_score_candidates;
  CHECK(candidate_axis.operations == 1);
  CHECK(candidate_axis.parallel_operations == 1);
  CHECK(candidate_axis.items == candidates.size());
  CHECK(candidate_axis.worker_tasks == 4);
  CHECK(candidate_axis.active_worker_high_water >= 2);
  CHECK(parallel_state.counters.scheduler_axes.local_score_candidate_patterns
            .operations == 0);

  auto one_candidate =
      std::span<larch::grammar_spr_candidate const>{&candidates.front(), 1};
  larch::chart_spr_local_score_result serial_tile;
  larch::chart_spr_local_score_result parallel_tile;
  larch::score_candidates_locally_into(
      serial_state, one_candidate,
      std::span<larch::chart_spr_local_score_result>{&serial_tile, 1},
      serial_workspace, {}, 1);
  larch::local_score_worker_barrier_for_tests tile_barrier;
  larch::local_spr_score_options tile_options;
  tile_options.worker_barrier_for_tests = &tile_barrier;
  larch::score_candidates_locally_into(
      parallel_state, one_candidate,
      std::span<larch::chart_spr_local_score_result>{&parallel_tile, 1},
      parallel_workspace, tile_options, scheduler, checked);
  CHECK(tile_barrier.release.load());
  CHECK(tile_barrier.started.load() >= 4);
  CHECK(parallel_workspace.operation_boundary_clean());
  check_local_result_matches_owned_score(
      parallel_tile,
      larch::promote_chart_spr_local_score(candidates.front(), serial_tile));
  auto const& tile_axis =
      parallel_state.counters.scheduler_axes.local_score_candidate_patterns;
  CHECK(tile_axis.operations == 1);
  CHECK(tile_axis.parallel_operations == 1);
  CHECK(tile_axis.items > 1);
  CHECK(tile_axis.worker_tasks == 4);
  CHECK(tile_axis.active_worker_high_water >= 2);

  larch::local_score_worker_barrier_for_tests failure_barrier;
  larch::local_spr_score_options failure_options;
  failure_options.worker_barrier_for_tests = &failure_barrier;
  failure_options.force_worker_failure_for_tests = 0;
  bool worker_threw = false;
  try {
    larch::score_candidates_locally_into(
        parallel_state, one_candidate,
        std::span<larch::chart_spr_local_score_result>{&parallel_tile, 1},
        parallel_workspace, failure_options, scheduler, checked);
  } catch (std::runtime_error const& error) {
    worker_threw = true;
    CHECK(std::string{error.what()}.find("forced worker task failure") !=
          std::string::npos);
  }
  CHECK(worker_threw);
  CHECK(failure_barrier.release.load());
  CHECK(failure_barrier.started.load() >= 4);
  CHECK(parallel_workspace.operation_boundary_clean());

  // Recovery is semantic, not merely scheduler liveness: replay the same tile
  // operation and require the exact serial result.
  larch::score_candidates_locally_into(
      parallel_state, one_candidate,
      std::span<larch::chart_spr_local_score_result>{&parallel_tile, 1},
      parallel_workspace, {}, scheduler, checked);
  CHECK(parallel_workspace.operation_boundary_clean());
  check_local_result_matches_owned_score(
      parallel_tile,
      larch::promote_chart_spr_local_score(candidates.front(), serial_tile));
  CHECK(parallel_state.counters.scheduler_axes.local_score_candidate_patterns
            .operations == 2);

  scheduler.shutdown();
  auto metrics = scheduler.metrics();
  CHECK(metrics.pending_tasks == 0);
  CHECK(metrics.tasks_submitted == metrics.tasks_completed);
  CHECK(metrics.tasks_submitted == metrics.tasks_joined);
  CHECK(metrics.live_pool_threads == 0);
  CHECK(metrics.pool_lifetimes == 1);
  CHECK(metrics.pool_lifetimes_stopped == 1);
  CHECK(metrics.shutdown);

  std::println("  PASS");
}

static void test_phase4_small_search_uses_one_scheduler_and_quiesces() {
  std::println("test_phase4_small_search_uses_one_scheduler_and_quiesces");

  auto const live_before =
      larch::chart_scheduler::global_live_pool_threads();
  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.max_iterations = 1;
  options.max_candidates_per_iteration = 64;
  options.cache.candidate_batch_size = 64;
  options.worker_count = 8;

  auto search =
      larch::run_chart_spr_search(std::move(dag), grammar, options);
  auto const& scheduler = search.summary.scheduler;
  CHECK(search.summary.candidates_locally_scored > 0);
  CHECK(scheduler.requested_workers == 8);
  CHECK(scheduler.resolved_workers == 8);
  CHECK(scheduler.operations > 1);
  CHECK(scheduler.parallel_operations > 0);
  CHECK(scheduler.operations ==
        scheduler.parallel_operations + scheduler.serial_fallbacks);
  CHECK(scheduler.ranges_created == scheduler.ranges_completed);
  CHECK(scheduler.ranges_cancelled == 0);
  CHECK(scheduler.minimum_effective_grain > 0);
  CHECK(scheduler.maximum_effective_grain >= scheduler.minimum_effective_grain);
  CHECK(scheduler.active_worker_high_water > 0);
  CHECK(scheduler.tasks_submitted > 0);
  CHECK(scheduler.tasks_completed == scheduler.tasks_submitted);
  CHECK(scheduler.tasks_joined == scheduler.tasks_submitted);
  CHECK(scheduler.pool_lifetimes == 1);
  CHECK(scheduler.pool_lifetimes_stopped == 1);
  CHECK(scheduler.pending_tasks == 0);
  CHECK(scheduler.pending_tasks_at_shutdown == 0);
  CHECK(scheduler.live_pool_threads == 0);
  CHECK(scheduler.shutdown);
  CHECK(search.summary.scheduler_axes.initial_chart_patterns.operations > 0);
  CHECK(search.summary.scheduler_axes.local_score_candidates.operations +
            search.summary.scheduler_axes.local_score_candidate_patterns
                .operations >
        0);
  check_phase4_scheduler_axis_reconciliation(search);
  CHECK(larch::chart_scheduler::global_live_pool_threads() == live_before);

  std::println("  PASS");
}

static void test_pattern_batch_nonreplayable_uses_automatic_candidate_batch() {
  std::println(
      "test_pattern_batch_nonreplayable_uses_automatic_candidate_batch");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.cache.max_cached_patterns = 1;
  auto state = larch::build_chart_spr_search_state(dag, grammar, options);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);

  auto enumeration = options.enumeration;
  enumeration.source = larch::chart_spr_candidate_source::sampled_tree;
  auto automatic_batch_size = larch::chart_spr_effective_candidate_batch_size(
      state, options);
  CHECK(automatic_batch_size != 0);
  larch::validate_chart_spr_pattern_batch_replay_strategy(
      state, options, enumeration, automatic_batch_size);

  options.cache.candidate_batch_size = 2;
  larch::validate_chart_spr_pattern_batch_replay_strategy(
      state, options, enumeration,
      larch::chart_spr_effective_candidate_batch_size(state, options));

  std::println("  PASS");
}

static void test_unchartable_grammar_rejected_with_empty_active_patterns() {
  std::println("test_unchartable_grammar_rejected_with_empty_active_patterns");

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", tiny_inner("root", "A", {tiny_leaf("A", "A"),
                                      tiny_leaf("B", "A")}));
  auto grammar = larch::build_clade_grammar(dag);
  grammar.productions.clear();
  grammar.productions_by_parent.assign(grammar.clades.size(), {});
  grammar.productions_by_child.assign(grammar.clades.size(), {});
  auto patterns = larch::build_site_patterns(dag, grammar);

  bool threw = false;
  try {
    (void)larch::build_chart_spr_search_state(dag, grammar, patterns);
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_unsupported_enumeration_options_fail_explicitly() {
  std::println("test_unsupported_enumeration_options_fail_explicitly");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.include_neutral_or_reversal_candidates = true;

  bool threw = false;
  try {
    (void)larch::for_each_grammar_spr_candidate(
        fixture.grammar, options,
        [](larch::grammar_spr_candidate const&) { return true; });
  } catch (std::exception const&) {
    threw = true;
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_max_affected_estimate_prunes_before_construction() {
  std::println("test_max_affected_estimate_prunes_before_construction");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_estimated_affected_clades = 1;

  std::size_t callbacks = 0;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const&) {
        ++callbacks;
        return true;
      });

  CHECK(callbacks == 0);
  CHECK(stats.candidates_constructed == 0);
  CHECK(stats.candidates_pruned_before_construction > 0);

  std::println("  PASS");
}

static void test_streaming_candidate_cap_stops_before_eager_path_precompute() {
  std::println("test_streaming_candidate_cap_stops_before_eager_path_precompute");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_candidates = 1;

  std::vector<larch::grammar_spr_candidate> streamed;
  auto streaming_stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const& candidate) {
        streamed.push_back(candidate);
        return true;
      });
  auto eager = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options);

  CHECK(streamed.size() == 1);
  CHECK(streaming_stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(streaming_stats.candidates_generated_after_dedup == 1);
  CHECK(streaming_stats.candidates_constructed == 1);
  CHECK(streaming_stats.upward_paths_completed <
        eager.stats.upward_paths_completed);

  std::println("  PASS");
}

static void test_streaming_path_pair_budget_stops_early() {
  std::println("test_streaming_path_pair_budget_stops_early");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_path_pairs_considered = 1;

  std::size_t callbacks = 0;
  auto stats = larch::for_each_grammar_spr_candidate(
      fixture.grammar, options,
      [&](larch::grammar_spr_candidate const&) {
        ++callbacks;
        return true;
      });

  CHECK(stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::path_budget);
  CHECK(stats.path_pairs_considered == 1);
  CHECK(stats.candidates_constructed <= 1);
  CHECK(callbacks <= 1);

  std::println("  PASS");
}

static void test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute() {
  std::println("test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute");

  auto fixture = make_fixture();
  larch::grammar_spr_enumeration_options options;
  options.max_candidates = 1;
  larch::chart_spr_search_counters counters;
  auto result = larch::enumerate_grammar_spr_candidates_eager_diagnostic(
      fixture.grammar, options, &counters);

  CHECK(result.candidates.size() == 1);
  CHECK(result.stats.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(result.stats.upward_paths_completed > result.candidates.size());
  CHECK(result.stats.upward_path_iterator_steps > 0);
  CHECK(result.stats.candidates_constructed >= result.candidates.size());
  CHECK(counters.candidate_cap_cutoffs == 1);
  CHECK(counters.candidates_generated_after_dedup == result.candidates.size());
  CHECK(counters.upward_paths_completed == result.stats.upward_paths_completed);
  CHECK(counters.path_pairs_considered == result.stats.path_pairs_considered);

  std::println("  PASS");
}

static std::array<std::size_t, 10> phase2b_exact_counter_snapshot(
    larch::chart_spr_search_counters const& counters) {
  return {
      counters.exact_setup_builds,
      counters.exact_setup_inside_charts_built,
      counters.exact_setup_resident_inside_charts_consumed,
      counters.exact_setup_active_leaf_state_vectors_copied,
      counters.exact_setup_active_leaf_states_copied,
      counters.exact_setup_outside_boundary_charts_built,
      counters.exact_setup_upper_bound_topologies_generated,
      counters.exact_setup_upper_bound_topologies_unique,
      counters.exact_setup_frontier_passes,
      counters.exact_trim_lazy_chart_uses,
  };
}

static void check_phase2b_trim_semantic_parity(
    larch::multisite_trim_result const& expected,
    larch::multisite_trim_result const& actual) {
  CHECK(actual.optimum == expected.optimum);
  CHECK(actual.composite_lower_bound == expected.composite_lower_bound);
  CHECK(actual.initial_upper_bound == expected.initial_upper_bound);
  CHECK(actual.keep_production == expected.keep_production);
  CHECK(actual.frontier_sizes_by_clade == expected.frontier_sizes_by_clade);
  CHECK(actual.dominance_mode == expected.dominance_mode);
  CHECK(actual.keep_mask_kind == expected.keep_mask_kind);
  CHECK(actual.keep_production_exact == expected.keep_production_exact);
  CHECK(actual.dominance_candidates_considered ==
        expected.dominance_candidates_considered);
  CHECK(actual.dominance_pruned == expected.dominance_pruned);
  CHECK(actual.bound_pruned == expected.bound_pruned);
  CHECK(actual.equality_deduplicated == expected.equality_deduplicated);
  CHECK(actual.active_pattern_count == expected.active_pattern_count);
  CHECK(actual.invariant_constant_offset == expected.invariant_constant_offset);
}

static void check_phase4_pattern_cache_parity(
    larch::chart_spr_search_state const& expected,
    larch::chart_spr_search_state const& actual) {
  CHECK(actual.cache_strategy == expected.cache_strategy);
  CHECK(actual.composite_lower_bound_without_invariants ==
        expected.composite_lower_bound_without_invariants);
  CHECK(actual.composite_lower_bound_with_invariants ==
        expected.composite_lower_bound_with_invariants);
  CHECK(actual.pattern_charts.size() == expected.pattern_charts.size());
  for (std::size_t index = 0; index < expected.pattern_charts.size(); ++index) {
    auto const& lhs = expected.pattern_charts[index];
    auto const& rhs = actual.pattern_charts[index];
    CHECK(rhs.chart.inside == lhs.chart.inside);
    CHECK(rhs.chart.optimal_choices.empty() ==
          lhs.chart.optimal_choices.empty());
    CHECK(rhs.chart.trace_choice_count == lhs.chart.trace_choice_count);
    CHECK(rhs.chart.multifurcation_productions_scored ==
          lhs.chart.multifurcation_productions_scored);
    CHECK(rhs.root_row == lhs.root_row);
    CHECK(rhs.root_min_excluding_ua == lhs.root_min_excluding_ua);
    CHECK(rhs.root_min_by_reference_state == lhs.root_min_by_reference_state);
    CHECK(rhs.reference_state_counts == lhs.reference_state_counts);
    CHECK(rhs.weighted_root_score == lhs.weighted_root_score);
  }
}

// The Phase-4 state path owns three independent pattern axes: initial dense
// charts, exact active-pattern setup, and the fixed-topology direct oracle.
// Exercise all three through the same persistent scheduler and compare W1/W4
// against the legacy serial state.  Axis totals are reduced only after each
// join, so their accounting must exactly reconcile with scheduler totals.
static void test_phase4_scheduled_pattern_axes_match_w1() {
  std::println("test_phase4_scheduled_pattern_axes_match_w1");

  auto fixture = make_fixture();
  auto patterns = make_phase4_wide_patterns();
  auto serial = larch::build_chart_spr_search_state(fixture.dag,
                                                    fixture.grammar, patterns);
  CHECK(serial.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);
  CHECK(serial.active_patterns.patterns.patterns.size() ==
        patterns.patterns.size());

  auto make_scheduler = [](std::size_t workers) {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = workers,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };
  auto w1_scheduler = make_scheduler(1);
  auto w4_scheduler = make_scheduler(4);

  auto build_scheduled = [&](larch::chart_scheduler& scheduler) {
    auto active = larch::make_active_search_patterns(patterns);
    return larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), {}, false, {}, {}, {},
        &scheduler);
  };
  auto w1 = build_scheduled(*w1_scheduler);
  auto w4 = build_scheduled(*w4_scheduler);
  check_phase4_pattern_cache_parity(serial, w1);
  check_phase4_pattern_cache_parity(serial, w4);
  CHECK(w1.counters.scheduler_axes.initial_chart_patterns.operations == 1);
  CHECK(w4.counters.scheduler_axes.initial_chart_patterns.operations == 1);
  CHECK(w4.counters.scheduler_axes.initial_chart_patterns.parallel_operations ==
        1);
  CHECK(w4.counters.scheduler_axes.initial_chart_patterns.items ==
        patterns.patterns.size());
  CHECK(w4.counters.scheduler_axes.initial_chart_patterns.worker_tasks > 1);

  auto serial_trim = larch::build_chart_spr_state_exact_trim(serial);
  auto w1_checked =
      larch::check_chart_execution_plan(w1.grammar, w1.execution_plan);
  auto w4_checked =
      larch::check_chart_execution_plan(w4.grammar, w4.execution_plan);
  auto w1_trim =
      larch::build_chart_spr_state_exact_trim(w1, w1_checked, *w1_scheduler);
  auto w4_trim =
      larch::build_chart_spr_state_exact_trim(w4, w4_checked, *w4_scheduler);
  check_phase2b_trim_semantic_parity(serial_trim, w1_trim);
  check_phase2b_trim_semantic_parity(serial_trim, w4_trim);
  CHECK(w1.counters.scheduler_axes.exact_setup_patterns.operations > 0);
  CHECK(w4.counters.scheduler_axes.exact_setup_patterns.operations ==
        w1.counters.scheduler_axes.exact_setup_patterns.operations);
  CHECK(w4.counters.scheduler_axes.exact_setup_patterns.parallel_operations >
        0);

  larch::chart_spr_search_options fixed_options;
  fixed_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  std::optional<larch::chart_spr_candidate_score> selected;
  for (auto const& candidate : fixture.candidates) {
    auto scored = larch::score_candidate_locally(serial, candidate);
    larch::attach_fixed_topology_selection_for_acceptance(serial, scored,
                                                          fixed_options);
    if (scored.valid && scored.topology_selection.certificate.has_value()) {
      selected = std::move(scored);
      break;
    }
  }
  CHECK(selected.has_value());
  auto fixed_serial =
      larch::fixed_topology_direct_selected_pattern_scores(serial, *selected);
  auto fixed_w1 = larch::fixed_topology_direct_selected_pattern_scores(
      w1, *selected, *w1_scheduler);
  auto fixed_w4 = larch::fixed_topology_direct_selected_pattern_scores(
      w4, *selected, *w4_scheduler);
  CHECK(fixed_w1.old_pattern_scores == fixed_serial.old_pattern_scores);
  CHECK(fixed_w1.new_pattern_scores == fixed_serial.new_pattern_scores);
  CHECK(fixed_w1.old_active_total == fixed_serial.old_active_total);
  CHECK(fixed_w1.new_active_total == fixed_serial.new_active_total);
  CHECK(fixed_w4.old_pattern_scores == fixed_serial.old_pattern_scores);
  CHECK(fixed_w4.new_pattern_scores == fixed_serial.new_pattern_scores);
  CHECK(fixed_w4.old_active_total == fixed_serial.old_active_total);
  CHECK(fixed_w4.new_active_total == fixed_serial.new_active_total);
  CHECK(w4.counters.scheduler_axes.fixed_topology_patterns.operations == 1);
  CHECK(
      w4.counters.scheduler_axes.fixed_topology_patterns.parallel_operations ==
      1);
  CHECK(w4.counters.scheduler_axes.fixed_topology_patterns.items ==
        patterns.patterns.size());

  // A partial scheduler submission is infrastructure failure, never an
  // invalid biological candidate. The one accepted runner is joined, the
  // exception escapes with its scheduler type, and the scheduler remains
  // reusable for an exact replay.
  auto failing_scheduler = make_scheduler(4);
  auto failure_state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, patterns);
  larch::chart_scheduler_test_detail::access::fail_submission_after(
      *failing_scheduler, 1);
  auto metrics_before_failure = failing_scheduler->metrics();
  bool submit_failure_escaped = false;
  try {
    (void)larch::verify_candidate_fixed_topology_exact(failure_state, *selected,
                                                       *failing_scheduler);
  } catch (larch::chart_scheduler_submit_error const&) {
    submit_failure_escaped = true;
  }
  CHECK(submit_failure_escaped);
  auto metrics_after_failure = failing_scheduler->metrics();
  CHECK(metrics_after_failure.tasks_submitted -
            metrics_before_failure.tasks_submitted ==
        1);
  CHECK(metrics_after_failure.tasks_completed -
            metrics_before_failure.tasks_completed ==
        1);
  CHECK(metrics_after_failure.tasks_joined -
            metrics_before_failure.tasks_joined ==
        1);
  CHECK(metrics_after_failure.pending_tasks == 0);
  auto recovered_fixed = larch::verify_candidate_fixed_topology_exact(
      failure_state, *selected, *failing_scheduler);
  CHECK(recovered_fixed.valid);
  CHECK(recovered_fixed.exact.has_value());
  CHECK(recovered_fixed.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  failing_scheduler->shutdown();

  w1_scheduler->shutdown();
  w4_scheduler->shutdown();
  auto w1_metrics = w1_scheduler->metrics();
  auto w4_metrics = w4_scheduler->metrics();
  check_phase4_scheduler_axis_reconciliation(w1_metrics,
                                             w1.counters.scheduler_axes);
  check_phase4_scheduler_axis_reconciliation(w4_metrics,
                                             w4.counters.scheduler_axes);
  CHECK(w1_metrics.parallel_operations == 0);
  CHECK(w1_metrics.tasks_submitted == 0);
  CHECK(w4_metrics.parallel_operations > 0);
  CHECK(w4_metrics.pool_lifetimes == 1);
  CHECK(w4_metrics.pool_lifetimes_stopped == 1);
  CHECK(w4_metrics.pending_tasks == 0);
  CHECK(w4_metrics.live_pool_threads == 0);
  CHECK(w4_metrics.shutdown);

  std::println("  PASS");
}

static void test_phase7_scheduled_lazy_chart_axes_match_w1() {
  std::println("test_phase7_scheduled_lazy_chart_axes_match_w1");

  auto fixture = make_fixture();
  auto patterns = make_phase4_wide_patterns();
  larch::chart_cache_options cache;
  cache.use_lazy_multisite_chart = true;
  auto make_scheduler = [](std::size_t workers) {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = workers,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };
  auto w1_scheduler = make_scheduler(1);
  auto w4_scheduler = make_scheduler(4);
  auto build = [&](larch::chart_scheduler& scheduler) {
    auto active = larch::make_active_search_patterns(patterns);
    return larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active), {}, false, {}, cache,
        {}, &scheduler);
  };
  auto w1 = build(*w1_scheduler);
  auto w4 = build(*w4_scheduler);
  CHECK(w1.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(w4.cache_strategy == w1.cache_strategy);
  CHECK(w1.lazy_chart.has_value());
  CHECK(w4.lazy_chart.has_value());
  auto const& lhs = *w1.lazy_chart;
  auto const& rhs = *w4.lazy_chart;
  CHECK(lhs.inside_rows_by_clade == rhs.inside_rows_by_clade);
  CHECK(lhs.outside_rows_by_clade == rhs.outside_rows_by_clade);
  CHECK(lhs.class_index_by_pattern_by_clade ==
        rhs.class_index_by_pattern_by_clade);
  CHECK(lhs.structural_class_index_by_pattern_by_clade ==
        rhs.structural_class_index_by_pattern_by_clade);
  CHECK(lhs.outside_class_index_by_pattern_by_clade ==
        rhs.outside_class_index_by_pattern_by_clade);
  CHECK(lhs.structural_class_count_by_clade ==
        rhs.structural_class_count_by_clade);
  CHECK(lhs.class_weight_by_clade == rhs.class_weight_by_clade);
  CHECK(lhs.outside_class_weight_by_clade == rhs.outside_class_weight_by_clade);
  CHECK(lhs.outside_global_min_by_pattern == rhs.outside_global_min_by_pattern);
  CHECK(lhs.lazy_inside_rows_computed == rhs.lazy_inside_rows_computed);
  CHECK(lhs.lazy_outside_rows_computed == rhs.lazy_outside_rows_computed);
  CHECK(lhs.lazy_remerge_collisions == rhs.lazy_remerge_collisions);
  CHECK(lhs.multifurcation_productions_scored ==
        rhs.multifurcation_productions_scored);
  CHECK(lhs.outside_multifurcation_productions_scored ==
        rhs.outside_multifurcation_productions_scored);
  CHECK(lhs.outside_recurrence_work == rhs.outside_recurrence_work);
  CHECK(w1.composite_lower_bound_with_invariants ==
        w4.composite_lower_bound_with_invariants);
  CHECK(w1.counters.lazy_inside_rows_computed ==
        w4.counters.lazy_inside_rows_computed);
  CHECK(w1.counters.lazy_outside_rows_computed ==
        w4.counters.lazy_outside_rows_computed);

  auto const level_count =
      w1.execution_plan.bottom_up_level_offsets().size() - 1;
  CHECK(w1.counters.scheduler_axes.lazy_inside_clades.operations ==
        level_count);
  CHECK(w4.counters.scheduler_axes.lazy_inside_clades.operations ==
        level_count);
  CHECK(w1.counters.scheduler_axes.lazy_inside_clades.items ==
        w1.execution_plan.clades().size());
  CHECK(w4.counters.scheduler_axes.lazy_inside_clades.items ==
        w4.execution_plan.clades().size());
  CHECK(w1.counters.scheduler_axes.lazy_outside_clades.items + 1 ==
        w1.execution_plan.clades().size());
  CHECK(w4.counters.scheduler_axes.lazy_outside_clades.items + 1 ==
        w4.execution_plan.clades().size());
  CHECK(w1.counters.scheduler_axes.lazy_inside_clades.parallel_operations == 0);
  CHECK(w1.counters.scheduler_axes.lazy_outside_clades.parallel_operations ==
        0);
  CHECK(w4.counters.scheduler_axes.lazy_inside_clades.parallel_operations > 0);
  CHECK(w4.counters.scheduler_axes.lazy_outside_clades.parallel_operations > 0);
  CHECK(w1.counters.scheduler_axes.initial_chart_patterns.operations == 0);
  CHECK(w4.counters.scheduler_axes.initial_chart_patterns.operations == 0);

  // A real finite lazy search-state build publishes both halves of the
  // admission report through the public counter contract.
  constexpr auto finite_budget = std::size_t{1} << 40;
  auto finite_cache = cache;
  finite_cache.memory_budget_bytes = finite_budget;
  // The tiny balanced fixture has level widths 4/2/1. W1 deliberately creates
  // repeated same-shape waves, so both reuse counters are nonzero and their
  // propagation through the public state contract is observable. The focused
  // finite-admission suite exercises genuinely parallel W4 admission.
  auto finite_scheduler = make_scheduler(1);
  auto finite_active = larch::make_active_search_patterns(patterns);
  auto finite = larch::build_chart_spr_search_state_from_active(
      fixture.dag, fixture.grammar, std::move(finite_active), {}, false, {},
      finite_cache, {}, finite_scheduler.get());
  CHECK(finite.counters.lazy_chart_memory_budget_bytes == finite_budget);
  CHECK(finite.counters.lazy_chart_inside_max_admitted_slots > 0);
  CHECK(finite.counters.lazy_chart_outside_max_admitted_slots > 0);
  CHECK(finite.counters.lazy_chart_inside_admission_waves > 0);
  CHECK(finite.counters.lazy_chart_outside_admission_waves > 0);
  CHECK(finite.counters.lazy_chart_inside_reused_slot_waves > 0);
  CHECK(finite.counters.lazy_chart_outside_reused_slot_waves > 0);
  CHECK(finite.counters.lazy_chart_preflight_peak_bytes <= finite_budget);
  CHECK(finite.counters.lazy_chart_actual_peak_bytes <= finite_budget);
  CHECK(finite.counters.lazy_chart_pre_submit_rejections == 0);
  CHECK(finite.counters.scheduler_axes.lazy_inside_clades.operations ==
        finite.counters.lazy_chart_inside_admission_waves);
  CHECK(finite.counters.scheduler_axes.lazy_outside_clades.operations ==
        finite.counters.lazy_chart_outside_admission_waves);
  finite_scheduler->shutdown();
  check_phase4_scheduler_axis_reconciliation(w1_scheduler->metrics(),
                                             w1.counters.scheduler_axes);
  check_phase4_scheduler_axis_reconciliation(w4_scheduler->metrics(),
                                             w4.counters.scheduler_axes);
  w1_scheduler->shutdown();
  w4_scheduler->shutdown();

  std::println("  PASS");
}

static void test_phase7_lazy_state_retained_long_taxon_name_accounting() {
  std::println("test_phase7_lazy_state_retained_long_taxon_name_accounting");

  auto fixture = make_fixture();
  auto patterns = make_phase4_wide_patterns();
  larch::chart_spr_search_state retained_state;
  retained_state.grammar = fixture.grammar;
  retained_state.execution_plan =
      larch::build_chart_execution_plan(retained_state.grammar);
  auto active_build = larch::make_active_search_patterns(patterns);
  retained_state.active_patterns = std::move(active_build.active_patterns);

  auto const taxon = larch::taxon_id{0};
  auto const old_name = retained_state.grammar.taxa.id_to_sample_id[taxon];
  auto const old_map =
      retained_state.grammar.taxa.sample_id_to_id.find(old_name);
  CHECK(old_map != retained_state.grammar.taxa.sample_id_to_id.end());
  CHECK(old_map->second == taxon);
  auto const old_vector_name_bytes =
      larch::chart_spr_search_detail::estimate_owned_string_capacity_bytes(
          retained_state.grammar.taxa.id_to_sample_id[taxon],
          "test retained vector name");
  auto const old_map_name_bytes =
      larch::chart_spr_search_detail::estimate_owned_string_capacity_bytes(
          old_map->first, "test retained map name");
  auto const retained_before =
      larch::estimate_chart_spr_lazy_state_build_retained_bytes(retained_state);
  auto const plan_bytes_before =
      retained_state.execution_plan.dynamic_capacity_bytes();
  auto const bucket_count_before =
      retained_state.grammar.taxa.sample_id_to_id.bucket_count();

  std::string long_name(513, 'x');
  long_name.front() = 'A';
  long_name.shrink_to_fit();
  CHECK(long_name.capacity() >= long_name.size());
  CHECK(long_name.capacity() > 64);
  retained_state.grammar.taxa.sample_id_to_id.erase(old_name);
  retained_state.grammar.taxa.id_to_sample_id[taxon] = long_name;
  auto const [inserted, unique] =
      retained_state.grammar.taxa.sample_id_to_id.emplace(long_name, taxon);
  CHECK(unique);
  CHECK(inserted->second == taxon);
  CHECK(retained_state.grammar.taxa.sample_id_to_id.bucket_count() ==
        bucket_count_before);
  retained_state.execution_plan =
      larch::build_chart_execution_plan(retained_state.grammar);
  CHECK(retained_state.execution_plan.dynamic_capacity_bytes() ==
        plan_bytes_before);

  auto const& retained_vector_name =
      retained_state.grammar.taxa.id_to_sample_id[taxon];
  auto const& retained_map_name = inserted->first;
  CHECK(retained_vector_name.capacity() > 64);
  CHECK(retained_map_name.capacity() > 64);
  auto const vector_name_bytes =
      larch::chart_spr_search_detail::estimate_owned_string_capacity_bytes(
          retained_vector_name, "test retained vector name");
  auto const map_name_bytes =
      larch::chart_spr_search_detail::estimate_owned_string_capacity_bytes(
          retained_map_name, "test retained map name");
  CHECK(vector_name_bytes == retained_vector_name.capacity() + 1);
  CHECK(map_name_bytes == retained_map_name.capacity() + 1);
  auto const retained_bytes =
      larch::estimate_chart_spr_lazy_state_build_retained_bytes(retained_state);
  CHECK(retained_before >= old_vector_name_bytes + old_map_name_bytes);
  CHECK(retained_bytes == retained_before - old_vector_name_bytes -
                              old_map_name_bytes + vector_name_bytes +
                              map_name_bytes);

  auto const& plan = retained_state.execution_plan;
  auto const& active_patterns = retained_state.active_patterns.patterns;
  larch::lazy_chart_options lazy_options;
  lazy_options.retain_all_inside_class_maps = true;
  auto const oracle =
      larch::build_lazy_inside_chart(plan, active_patterns, lazy_options);
  auto make_scheduler = [] {
    return std::make_unique<larch::chart_scheduler>(
        larch::chart_scheduler_options{
            .requested_workers = 1,
            .default_minimum_grain = 1,
            .default_target_ranges_per_worker = 4,
        });
  };
  constexpr auto huge_budget = std::size_t{1} << 40;
  auto calibration_scheduler = make_scheduler();
  larch::lazy_chart_detail::plan_lazy_chart_memory_report calibration_report;
  auto calibration = larch::build_lazy_inside_chart_scheduled(
      plan, active_patterns, lazy_options, *calibration_scheduler, nullptr,
      nullptr, nullptr,
      larch::lazy_chart_detail::plan_lazy_chart_memory_options{
          .memory_budget_bytes = huge_budget,
          .retained_resident_bytes = retained_bytes,
      },
      &calibration_report);
  CHECK(calibration.inside_rows_by_clade == oracle.inside_rows_by_clade);
  auto const exact_budget =
      std::max(calibration_report.preflight_peak_capacity_resident_bytes,
               calibration_report.actual_peak_capacity_resident_bytes);
  CHECK(exact_budget ==
        calibration_report.preflight_peak_capacity_resident_bytes);
  CHECK(exact_budget > retained_bytes);

  auto exact_scheduler = make_scheduler();
  larch::lazy_chart_detail::plan_lazy_chart_memory_report exact_report;
  auto exact = larch::build_lazy_inside_chart_scheduled(
      plan, active_patterns, lazy_options, *exact_scheduler, nullptr, nullptr,
      nullptr,
      larch::lazy_chart_detail::plan_lazy_chart_memory_options{
          .memory_budget_bytes = exact_budget,
          .retained_resident_bytes = retained_bytes,
      },
      &exact_report);
  CHECK(exact.inside_rows_by_clade == oracle.inside_rows_by_clade);
  CHECK(exact_report.preflight_peak_capacity_resident_bytes <= exact_budget);
  CHECK(exact_report.actual_peak_capacity_resident_bytes <= exact_budget);

  auto one_under_scheduler = make_scheduler();
  auto const operations_before = one_under_scheduler->metrics().operations;
  larch::lazy_chart_detail::plan_lazy_chart_memory_report one_under_report;
  bool rejected = false;
  try {
    (void)larch::build_lazy_inside_chart_scheduled(
        plan, active_patterns, lazy_options, *one_under_scheduler, nullptr,
        nullptr, nullptr,
        larch::lazy_chart_detail::plan_lazy_chart_memory_options{
            .memory_budget_bytes = exact_budget - 1,
            .retained_resident_bytes = retained_bytes,
        },
        &one_under_report);
  } catch (larch::lazy_chart_detail::plan_lazy_chart_memory_budget_error const&
               error) {
    rejected = true;
    CHECK(error.required_bytes() == exact_budget);
    CHECK(error.budget_bytes() == exact_budget - 1);
  }
  CHECK(rejected);
  CHECK(one_under_report.pre_submit_rejections == 1);
  CHECK(one_under_report.inside_admission_waves == 0);
  CHECK(one_under_scheduler->metrics().operations == operations_before);

  std::println("  PASS");
}

static void test_phase2b_exact_setup_reuses_resident_state_charts() {
  std::println("test_phase2b_exact_setup_reuses_resident_state_charts");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  // Lazy initialization of an ordinary all-active state: exact setup consumes
  // the already-resident inside chart for every active pattern and builds no
  // replacement inside chart.  The direct cold trim is the semantic oracle.
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::all_active_patterns);
  auto const pattern_count =
      state.active_patterns.patterns.patterns.size();
  CHECK(pattern_count >= 2);
  CHECK(state.counters.initial_state_inside_charts_built == pattern_count);
  auto cold = larch::build_multisite_trim_active(
      state.execution_plan, state.active_patterns, state.chart_opts);
  CHECK(cold.exact_setup_work.setup_builds == 1);
  CHECK(cold.exact_setup_work.inside_charts_built == pattern_count);
  CHECK(cold.exact_setup_work.resident_inside_charts_consumed == 0);

  auto const before_lazy_ensure = phase2b_exact_counter_snapshot(state.counters);
  auto const zero_exact_counters = std::array<std::size_t, 10>{};
  CHECK(before_lazy_ensure == zero_exact_counters);
  auto const& resident = larch::ensure_chart_spr_state_exact_trim(state);
  check_phase2b_trim_semantic_parity(cold, resident);
  CHECK(resident.exact_setup_work.setup_builds == 1);
  CHECK(resident.exact_setup_work.inside_charts_built == 0);
  CHECK(resident.exact_setup_work.resident_inside_charts_consumed ==
        pattern_count);
  CHECK(resident.exact_setup_work.active_leaf_state_vectors_copied ==
        pattern_count);
  CHECK(resident.exact_setup_work.active_leaf_states_copied ==
        pattern_count * state.active_patterns.patterns.taxon_count);
  CHECK(resident.exact_setup_work.outside_boundary_charts_built ==
        pattern_count);
  // This tree has one production at every reachable internal clade.  Exact
  // setup proves the sole feasible topology structurally, so the composite
  // lower bound is already a feasible upper bound and no per-pattern
  // traceback/topology-rescore candidates are materialized.
  CHECK(resident.composite_lower_bound == resident.initial_upper_bound);
  CHECK(resident.exact_setup_work.upper_bound_topologies_generated == 1);
  CHECK(resident.exact_setup_work.upper_bound_topologies_unique == 1);
  CHECK(resident.exact_setup_work.upper_bound_topologies_unique <=
        resident.exact_setup_work.upper_bound_topologies_generated);
  CHECK(resident.exact_setup_work.frontier_passes > 0);
  CHECK(state.counters.exact_setup_builds == 1);
  CHECK(state.counters.exact_setup_inside_charts_built == 0);
  CHECK(state.counters.exact_setup_resident_inside_charts_consumed ==
        pattern_count);
  CHECK(state.counters.exact_setup_active_leaf_state_vectors_copied ==
        pattern_count);
  CHECK(state.counters.exact_setup_active_leaf_states_copied ==
        pattern_count * state.active_patterns.patterns.taxon_count);
  CHECK(state.counters.exact_setup_outside_boundary_charts_built ==
        pattern_count);
  CHECK(state.counters.exact_setup_upper_bound_topologies_generated == 1);
  CHECK(state.counters.exact_setup_upper_bound_topologies_unique ==
        resident.exact_setup_work.upper_bound_topologies_unique);
  CHECK(state.counters.exact_setup_frontier_passes ==
        resident.exact_setup_work.frontier_passes);
  CHECK(state.counters.exact_trim_lazy_chart_uses == 0);

  auto const after_first_ensure = phase2b_exact_counter_snapshot(state.counters);
  auto const* resident_address = &resident;
  auto const& repeated = larch::ensure_chart_spr_state_exact_trim(state);
  CHECK(&repeated == resident_address);
  CHECK(phase2b_exact_counter_snapshot(state.counters) == after_first_ensure);

  // Eager exact initialization takes the same resident path, and a repeated
  // ensure neither rebuilds the finalized setup nor advances its work counters.
  larch::chart_spr_search_options eager_options;
  eager_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  auto eager =
      larch::build_chart_spr_search_state(dag, grammar, eager_options);
  CHECK(eager.exact_trim_active_only.has_value());
  check_phase2b_trim_semantic_parity(resident,
                                    *eager.exact_trim_active_only);
  CHECK(eager.counters.exact_setup_builds == 1);
  CHECK(eager.counters.exact_setup_inside_charts_built == 0);
  CHECK(eager.counters.exact_setup_resident_inside_charts_consumed ==
        pattern_count);
  CHECK(eager.counters.initial_state_inside_charts_built == pattern_count);
  auto const eager_before_repeat =
      phase2b_exact_counter_snapshot(eager.counters);
  (void)larch::ensure_chart_spr_state_exact_trim(eager,
                                                 eager_options.exact_trim);
  CHECK(phase2b_exact_counter_snapshot(eager.counters) ==
        eager_before_repeat);

  // Pattern batches own no compatible resident single-site charts.  Exact
  // initialization therefore makes one cold setup the sole owner of both the
  // initial composite and exact frontier: there is no throwaway batch scan
  // before it.
  auto batched_options = eager_options;
  batched_options.cache.max_cached_patterns = 1;
  auto batched =
      larch::build_chart_spr_search_state(dag, grammar, batched_options);
  CHECK(batched.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(batched.pattern_charts.empty());
  CHECK(batched.exact_trim_active_only.has_value());
  check_phase2b_trim_semantic_parity(resident,
                                    *batched.exact_trim_active_only);
  CHECK(batched.counters.exact_setup_builds == 1);
  CHECK(batched.counters.exact_setup_inside_charts_built == pattern_count);
  CHECK(batched.counters.exact_setup_resident_inside_charts_consumed == 0);
  CHECK(batched.counters.initial_state_inside_charts_built == 0);
  CHECK(batched.counters.pattern_batch_cache_builds == 0);
  CHECK(batched.counters.exact_setup_outside_boundary_charts_built ==
        pattern_count);
  CHECK(batched.counters.exact_trim_lazy_chart_uses == 0);
  auto const batched_before_repeat =
      phase2b_exact_counter_snapshot(batched.counters);
  (void)larch::ensure_chart_spr_state_exact_trim(batched,
                                                 batched_options.exact_trim);
  CHECK(phase2b_exact_counter_snapshot(batched.counters) ==
        batched_before_repeat);

  // The class-compressed lazy chart remains its own semantically-identical
  // representation; it does not pretend to have built/consumed a dense exact
  // setup, and its successful use is counted separately.
  auto lazy_options = eager_options;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy = larch::build_chart_spr_search_state(dag, grammar, lazy_options);
  CHECK(lazy.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy.exact_trim_active_only.has_value());
  CHECK(lazy.exact_trim_active_only->lazy_chart_used);
  check_phase2b_trim_semantic_parity(resident, *lazy.exact_trim_active_only);
  CHECK(lazy.counters.exact_setup_builds == 0);
  CHECK(lazy.counters.exact_setup_inside_charts_built == 0);
  CHECK(lazy.counters.exact_setup_resident_inside_charts_consumed == 0);
  CHECK(lazy.counters.exact_setup_active_leaf_state_vectors_copied == 0);
  CHECK(lazy.counters.exact_setup_active_leaf_states_copied == 0);
  CHECK(lazy.counters.exact_setup_outside_boundary_charts_built == 0);
  CHECK(lazy.counters.exact_setup_upper_bound_topologies_generated == 0);
  CHECK(lazy.counters.exact_setup_upper_bound_topologies_unique == 0);
  CHECK(lazy.counters.exact_setup_frontier_passes == 0);
  CHECK(lazy.counters.exact_trim_lazy_chart_uses == 1);
  CHECK(lazy.counters.initial_state_inside_charts_built == 0);
  auto const lazy_before_repeat =
      phase2b_exact_counter_snapshot(lazy.counters);
  (void)larch::ensure_chart_spr_state_exact_trim(lazy,
                                                 lazy_options.exact_trim);
  CHECK(phase2b_exact_counter_snapshot(lazy.counters) == lazy_before_repeat);

  // Identity is checked before any resident chart/setup work.  A same-generation
  // in-place mutation must reject the stale execution plan and leave every
  // exact-setup counter untouched.
  auto stale = larch::build_chart_spr_search_state(dag, grammar, patterns);
  CHECK(!stale.grammar.productions.empty());
  CHECK(stale.grammar.productions.front().children.size() >= 2);
  std::swap(stale.grammar.productions.front().children[0],
            stale.grammar.productions.front().children[1]);
  auto const stale_before = phase2b_exact_counter_snapshot(stale.counters);
  auto const mismatch_before = stale.counters.plan_mismatch_rejections;
  bool stale_threw = false;
  try {
    (void)larch::ensure_chart_spr_state_exact_trim(stale);
  } catch (larch::chart_execution_plan_mismatch const&) {
    stale_threw = true;
  }
  CHECK(stale_threw);
  CHECK(stale.counters.plan_mismatch_rejections == mismatch_before + 1);
  CHECK(phase2b_exact_counter_snapshot(stale.counters) == stale_before);

  std::println("  PASS");
}

static void test_phase2b_deferred_pattern_batch_uses_owning_setup_provider() {
  std::println(
      "test_phase2b_deferred_pattern_batch_uses_owning_setup_provider");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AAA", four_taxon_two_pattern_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto active_build = larch::make_active_search_patterns(patterns, {});

  larch::chart_cache_options cache;
  cache.max_cached_patterns = 1;
  larch::chart_spr_search_detail::chart_spr_state_build_policy policy;
  policy.defer_pattern_batch_bootstrap_to_local_cache = true;
  auto state = larch::build_chart_spr_search_state_from_active(
      dag, grammar, std::move(active_build), {}, false, {}, cache, policy);
  auto const pattern_count = state.active_patterns.patterns.patterns.size();
  CHECK(pattern_count >= 2);
  CHECK(state.cache_strategy ==
        larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(state.pattern_batch_bootstrap_deferred);
  CHECK(state.pattern_charts.empty());
  CHECK(!state.lazy_chart.has_value());
  CHECK(!state.exact_trim_active_only.has_value());
  CHECK(state.counters.pattern_batch_cache_builds == 0);
  CHECK(state.counters.initial_state_inside_charts_built == 0);

  // A two-stage internal state fails closed: even an installed provider must
  // not be called before the local cache publishes the initial composite.
  std::size_t provider_calls = 0;
  state.exact_setup_provider =
      [&](larch::chart_spr_search_state const&,
          larch::checked_chart_execution_plan_ref const&)
      -> larch::multisite_exact_setup {
    ++provider_calls;
    throw std::runtime_error("provider called before setup installation");
  };
  larch::declare_chart_spr_state_callback_target_resident_bytes(state, 4096);
  std::string deferred_rejection;
  try {
    (void)larch::ensure_chart_spr_state_exact_trim(state);
  } catch (std::runtime_error const& e) {
    deferred_rejection = e.what();
  }
  CHECK(deferred_rejection.find("deferred local-cache bootstrap") !=
        std::string::npos);
  CHECK(provider_calls == 0);
  CHECK(state.counters.exact_setup_builds == 0);

  // Model the local cache publication with ordinary resident charts, then
  // finalize one owning setup from them.  Destroying every source chart before
  // `ensure` proves the hook result retains no chart/provider borrow.
  larch::chart_options build_options = state.chart_opts;
  build_options.keep_trace = false;
  build_options.max_trace_choices = 0;
  std::vector<larch::single_site_chart> source_charts;
  source_charts.reserve(pattern_count);
  for (auto const& pattern : state.active_patterns.patterns.patterns) {
    source_charts.push_back(larch::build_single_site_chart(
        state.execution_plan,
        larch::view_leaf_site_states(pattern.state_by_taxon), build_options));
  }
  auto owning_setup = larch::build_multisite_exact_setup_from_resident_inside(
      state.execution_plan, state.active_patterns.patterns,
      [&](std::size_t pattern_index,
          larch::site_pattern const&) -> larch::single_site_chart const& {
        return source_charts.at(pattern_index);
      },
      state.chart_opts);
  CHECK(owning_setup.work.inside_charts_built == 0);
  CHECK(owning_setup.work.resident_inside_charts_consumed == pattern_count);
  auto cold = larch::build_multisite_trim_active(
      state.execution_plan, state.active_patterns, state.chart_opts);
  source_charts.clear();
  source_charts.shrink_to_fit();

  provider_calls = 0;
  state.exact_setup_provider =
      [owning_setup = std::move(owning_setup), &provider_calls](
          larch::chart_spr_search_state const& provided_state,
          larch::checked_chart_execution_plan_ref const& checked) {
        ++provider_calls;
        checked.assert_same(provided_state.grammar,
                            provided_state.execution_plan);
        return owning_setup;
      };
  larch::declare_chart_spr_state_callback_target_resident_bytes(
      state, std::size_t{1} << 20);
  larch::record_inside_chart_cache_build_work(state.counters, pattern_count, 0);
  auto const composite_with_invariants =
      cold.composite_lower_bound + state.invariant_constant_offset;
  larch::chart_spr_search_detail::finalize_deferred_pattern_batch_bootstrap(
      state, composite_with_invariants, 1.25);
  CHECK(!state.pattern_batch_bootstrap_deferred);
  CHECK(state.composite_lower_bound_without_invariants ==
        cold.composite_lower_bound);
  CHECK(state.composite_lower_bound_with_invariants ==
        composite_with_invariants);
  CHECK(state.chart_construction_ms == 1.25);
  CHECK(state.counters.initial_state_inside_charts_built == 0);
  CHECK(state.counters.inside_cache_inside_charts_built == pattern_count);
  CHECK(state.counters.inside_cache_resident_inside_charts_consumed == 0);

  bool duplicate_finalize_threw = false;
  try {
    larch::chart_spr_search_detail::finalize_deferred_pattern_batch_bootstrap(
        state, composite_with_invariants);
  } catch (std::runtime_error const&) {
    duplicate_finalize_threw = true;
  }
  CHECK(duplicate_finalize_threw);

  auto const& provided = larch::ensure_chart_spr_state_exact_trim(state);
  CHECK(provider_calls == 1);
  check_phase2b_trim_semantic_parity(cold, provided);
  CHECK(provided.exact_setup_work.inside_charts_built == 0);
  CHECK(provided.exact_setup_work.resident_inside_charts_consumed ==
        pattern_count);
  CHECK(state.counters.exact_setup_inside_charts_built == 0);
  CHECK(state.counters.exact_setup_resident_inside_charts_consumed ==
        pattern_count);
  CHECK(state.counters.initial_state_inside_charts_built +
            state.counters.exact_setup_inside_charts_built +
            state.counters.inside_cache_inside_charts_built ==
        pattern_count);

  auto const before_repeat = phase2b_exact_counter_snapshot(state.counters);
  auto const* provided_address = &provided;
  auto const& repeated = larch::ensure_chart_spr_state_exact_trim(state);
  CHECK(&repeated == provided_address);
  CHECK(provider_calls == 1);
  CHECK(phase2b_exact_counter_snapshot(state.counters) == before_repeat);

  std::println("  PASS");
}

static void test_exact_verification_reuses_state_old_score() {
  std::println("test_exact_verification_reuses_state_old_score");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);
  CHECK(state.exact_trim_active_only.has_value());
  auto const active_pattern_count =
      state.active_patterns.patterns.patterns.size();
  CHECK(active_pattern_count > 0);
  CHECK(state.counters.exact_setup_builds == 1);
  CHECK(state.counters.exact_setup_inside_charts_built == 0);
  CHECK(state.counters.exact_setup_resident_inside_charts_consumed ==
        active_pattern_count);

  auto lazy_options = options;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, lazy_options);
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.exact_trim_active_only.has_value());
  CHECK(lazy_state.exact_trim_active_only->lazy_chart_used);
  CHECK(lazy_state.exact_trim_active_only->optimum ==
        state.exact_trim_active_only->optimum);

  auto local = larch::score_candidate_locally(state, fixture.candidates.front());
  auto exact = larch::verify_candidate_exact_against_state(
      state, local, options.exact_trim);
  auto lazy_local = larch::score_candidate_locally(
      lazy_state, fixture.candidates.front());
  auto lazy_exact = larch::verify_candidate_exact_against_state(
      lazy_state, lazy_local, lazy_options.exact_trim);
  auto oracle = larch::score_multisite_spr_candidate_exact_oracle(
      fixture.grammar, fixture.patterns, fixture.candidates.front());

  CHECK(exact.valid);
  CHECK(exact.exact.has_value());
  CHECK(exact.exact->kind == larch::chart_spr_score_kind::grammar_exact);
  CHECK(exact.exact->convention ==
        larch::chart_spr_score_convention::full_with_invariants);
  CHECK(lazy_exact.valid);
  CHECK(lazy_exact.exact.has_value());
  CHECK(lazy_exact.exact->kind ==
        larch::chart_spr_score_kind::grammar_exact);
  CHECK(lazy_exact.exact->convention ==
        larch::chart_spr_score_convention::full_with_invariants);
  CHECK(lazy_exact.exact->value.old_score == exact.exact->value.old_score);
  CHECK(lazy_exact.exact->value.new_score == exact.exact->value.new_score);
  CHECK(lazy_exact.exact->value.delta == exact.exact->value.delta);
  CHECK(exact.exact->value.old_score == oracle.old_score);
  CHECK(exact.exact->value.new_score == oracle.new_score);
  CHECK(exact.exact->value.delta == oracle.delta);
  CHECK(state.counters.exact_verifications == 1);
  CHECK(state.counters.full_overlay_materializations == 1);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 1);
  CHECK(state.counters.overlay_materializations_for_oracle == 0);
  CHECK(state.counters.full_composite_rebuilds == 0);
  // The old state setup used resident charts; only the materialized candidate
  // grammar takes the explicit cold path.
  CHECK(state.counters.exact_setup_builds == 2);
  CHECK(state.counters.exact_setup_inside_charts_built ==
        active_pattern_count);
  CHECK(state.counters.exact_setup_resident_inside_charts_consumed ==
        active_pattern_count);
  CHECK(state.counters.exact_setup_outside_boundary_charts_built ==
        2 * active_pattern_count);
  CHECK(lazy_state.counters.exact_setup_builds == 0);
  CHECK(lazy_state.counters.exact_trim_lazy_chart_uses == 2);

  std::println("  PASS");
}

static void test_failed_exact_materialization_is_timed() {
  std::println("test_failed_exact_materialization_is_timed");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);

  // The verifier converts exact-materializer exceptions into invalid
  // candidates.  Force that call boundary to throw so the failed span must be
  // charged even though the success-only materialization counters stay zero.
  auto candidate =
      larch::score_candidate_locally(state, fixture.candidates.front());
  CHECK(candidate.valid);
  candidate.force_exact_materializer_failure_for_tests = true;

  auto const before =
      state.counters.materialization_exact_verification_ms;
  auto verified = larch::verify_candidate_exact_against_state(
      state, std::move(candidate), options.exact_trim);

  CHECK(!verified.valid);
  CHECK(verified.invalid_reason ==
        "forced exact materializer failure for tests");
  CHECK(state.counters.exact_verifications == 1);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.materialization_exact_verification_ms > before);

  std::println("  PASS");
}

static void test_top_k_exact_verification_count_is_bounded() {
  std::println("test_top_k_exact_verification_count_is_bounded");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.enumeration.max_candidates = 6;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);

  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidates_scored > 1);
  CHECK(iteration.candidates_exact_verified <= 1);
  CHECK(state.counters.exact_verifications <= 1);
  CHECK(state.counters.overlay_materializations_for_exact_verification ==
        state.counters.exact_verifications);
  CHECK(state.counters.local_candidate_scores == iteration.candidates_scored);
  CHECK(state.counters.full_composite_rebuilds == 0);
  CHECK(iteration.unverified_candidates_may_contain_improvements);
  CHECK(std::string{larch::chart_spr_candidate_selection_mode_name(
            iteration.candidate_selection)} == "lower_bound_top_k");

  std::println("  PASS");
}

static void test_phase6_top_k_four_uses_exact_candidate_axis_only() {
  std::println("test_phase6_top_k_four_uses_exact_candidate_axis_only");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  options.enumeration.max_candidates = 0;
  options.worker_count = 4;

  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   options);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  CHECK(scheduler.worker_resolution().resolved_workers == 4);
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  (void)larch::ensure_chart_spr_state_exact_trim(state, checked, scheduler,
                                                 options.exact_trim);

  auto const candidate_before = state.counters.scheduler_axes.exact_candidates;
  auto const exact_setup_before =
      state.counters.scheduler_axes.exact_setup_patterns;
  auto const exact_frontier_before =
      state.counters.scheduler_axes.exact_frontier_clades;
  auto const fixed_before =
      state.counters.scheduler_axes.fixed_topology_patterns;

  auto rendezvous = std::make_shared<phase6_exact_candidate_pair_rendezvous>();
  options.before_exact_candidate_verification_for_tests =
      [rendezvous](std::size_t rank) { rendezvous->arrive_and_wait(rank); };
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  auto iteration = larch::run_chart_spr_acceptance_iteration(
      state, options, 0, workspace, scheduler);

  CHECK(iteration.locally_ranked_candidates_retained == 4);
  CHECK(iteration.candidates_exact_verified == 4);
  CHECK(rendezvous->arrivals() == 2);
  auto const& candidate_after = state.counters.scheduler_axes.exact_candidates;
  CHECK(candidate_after.operations == candidate_before.operations + 1);
  CHECK(candidate_after.parallel_operations ==
        candidate_before.parallel_operations + 1);
  CHECK(candidate_after.items == candidate_before.items + 4);
  CHECK(candidate_after.worker_tasks == candidate_before.worker_tasks + 4);
  CHECK(candidate_after.active_worker_high_water >= 2);
  CHECK(state.exact_verifier_concurrency->peak() >= 2);
  CHECK(state.counters.exact_candidate_admission_batches == 1);
  CHECK(state.counters.exact_candidate_parallel_batches == 1);
  CHECK(state.counters.exact_candidate_inner_parallel_batches == 0);
  CHECK(state.counters.exact_candidate_memory_limited_batches == 0);
  CHECK(state.counters.exact_candidate_peak_admitted_bytes > 0);
  CHECK(state.counters.exact_candidate_peak_projected_resident_bytes >=
        state.counters.exact_candidate_peak_admitted_bytes);
  CHECK(state.counters.exact_candidate_queued_for_memory_ms == 0.0);

  // The outer candidate wave owns the scheduler. Candidate-local exact work
  // must therefore stay serial and publish no nested inner-axis operation.
  CHECK(state.counters.scheduler_axes.exact_setup_patterns ==
        exact_setup_before);
  CHECK(state.counters.scheduler_axes.exact_frontier_clades ==
        exact_frontier_before);
  CHECK(state.counters.scheduler_axes.fixed_topology_patterns == fixed_before);

  scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(scheduler.metrics(),
                                             state.counters.scheduler_axes);
  std::println("  PASS");
}

static void test_phase6_top_k_one_preserves_inner_exact_parallelism() {
  std::println("test_phase6_top_k_one_preserves_inner_exact_parallelism");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.enumeration.max_candidates = 0;
  options.worker_count = 4;

  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   options);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  CHECK(scheduler.worker_resolution().resolved_workers == 4);
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  (void)larch::ensure_chart_spr_state_exact_trim(state, checked, scheduler,
                                                 options.exact_trim);

  auto const candidate_before = state.counters.scheduler_axes.exact_candidates;
  auto const exact_setup_operations_before =
      state.counters.scheduler_axes.exact_setup_patterns.operations;
  auto const exact_frontier_operations_before =
      state.counters.scheduler_axes.exact_frontier_clades.operations;

  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  auto iteration = larch::run_chart_spr_acceptance_iteration(
      state, options, 0, workspace, scheduler);

  CHECK(iteration.locally_ranked_candidates_retained == 1);
  CHECK(iteration.candidates_exact_verified == 1);
  CHECK(state.counters.scheduler_axes.exact_candidates == candidate_before);
  auto const inner_exact_operation_delta =
      state.counters.scheduler_axes.exact_setup_patterns.operations -
      exact_setup_operations_before +
      state.counters.scheduler_axes.exact_frontier_clades.operations -
      exact_frontier_operations_before;
  CHECK(inner_exact_operation_delta > 0);
  CHECK(state.counters.exact_candidate_admission_batches == 1);
  CHECK(state.counters.exact_candidate_parallel_batches == 0);
  CHECK(state.counters.exact_candidate_inner_parallel_batches == 1);
  CHECK(state.counters.exact_candidate_memory_limited_batches == 0);
  CHECK(state.counters.exact_candidate_peak_admitted_bytes > 0);
  CHECK(state.counters.exact_candidate_peak_projected_resident_bytes >=
        state.counters.exact_candidate_peak_admitted_bytes);
  CHECK(state.counters.exact_candidate_queued_for_memory_ms == 0.0);
  CHECK(state.exact_verifier_concurrency->peak() == 1);

  scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(scheduler.metrics(),
                                             state.counters.scheduler_axes);
  std::println("  PASS");
}

static void test_phase6_exact_candidate_failures_choose_stable_rank_and_join() {
  std::println(
      "test_phase6_exact_candidate_failures_choose_stable_rank_and_join");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  options.enumeration.max_candidates = 0;
  options.worker_count = 4;

  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   options);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  CHECK(scheduler.worker_resolution().resolved_workers == 4);
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  (void)larch::ensure_chart_spr_state_exact_trim(state, checked, scheduler,
                                                 options.exact_trim);

  auto rendezvous =
      std::make_shared<phase6_reverse_rank_failure_rendezvous>(scheduler);
  options.before_exact_candidate_verification_for_tests =
      [rendezvous](std::size_t rank) {
        rendezvous->fail_in_reverse_completion_order(rank);
      };
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;

  bool lower_rank_failure_selected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(state, options, 0,
                                                    workspace, scheduler);
  } catch (std::runtime_error const& error) {
    lower_rank_failure_selected =
        std::string{error.what()} == "phase-6 exact candidate rank 0 failure";
  }
  CHECK(lower_rank_failure_selected);
  CHECK(rendezvous->invocations() == 4);
  CHECK(rendezvous->saw_ranks_zero_through_three());
  CHECK(rendezvous->rank_one_completed_before_rank_zero_failure());
  CHECK((rendezvous->failure_order() == std::vector<std::size_t>{1, 0}));
  CHECK(state.counters.scheduler_axes.exact_candidates.operations == 1);
  CHECK(state.counters.scheduler_axes.exact_candidates.parallel_operations ==
        1);
  CHECK(state.counters.scheduler_axes.exact_candidates.worker_tasks == 4);
  CHECK(
      state.counters.scheduler_axes.exact_candidates.active_worker_high_water >=
      2);
  auto failed_metrics = scheduler.metrics();
  CHECK(failed_metrics.tasks_submitted == failed_metrics.tasks_completed);
  CHECK(failed_metrics.tasks_submitted == failed_metrics.tasks_joined);
  CHECK(failed_metrics.pending_tasks == 0);

  // The failed operation joined every launched task before selecting rank 0.
  // Reusing the same workspace and pool must therefore be safe immediately.
  options.before_exact_candidate_verification_for_tests = {};
  auto const operations_before_reuse = scheduler.metrics().operations;
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, options, 1, workspace, scheduler);
  CHECK(recovered.locally_ranked_candidates_retained == 4);
  CHECK(recovered.candidates_exact_verified == 4);
  CHECK(scheduler.metrics().operations > operations_before_reuse);
  CHECK(state.counters.scheduler_axes.exact_candidates.operations == 2);
  CHECK(scheduler.metrics().rejected_concurrent_operations == 0);

  scheduler.shutdown();
  auto const recovered_metrics = scheduler.metrics();
  CHECK(recovered_metrics.tasks_submitted == recovered_metrics.tasks_completed);
  CHECK(recovered_metrics.tasks_submitted == recovered_metrics.tasks_joined);
  CHECK(recovered_metrics.pending_tasks == 0);
  CHECK(recovered_metrics.live_pool_threads == 0);
  check_phase4_scheduler_axis_reconciliation(recovered_metrics,
                                             state.counters.scheduler_axes);
  std::println("  PASS");
}

static void test_phase6_exact_candidate_partial_submit_joins_and_reconciles() {
  std::println(
      "test_phase6_exact_candidate_partial_submit_joins_and_reconciles");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  options.enumeration.max_candidates = 0;
  options.worker_count = 4;

  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   options);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  CHECK(scheduler.worker_resolution().resolved_workers == 4);
  auto checked =
      larch::check_chart_execution_plan(state.grammar, state.execution_plan);
  (void)larch::ensure_chart_spr_state_exact_trim(state, checked, scheduler,
                                                 options.exact_trim);

  auto hook_calls = std::make_shared<std::atomic<std::size_t>>(0);
  options.force_exact_candidate_submit_failure_after_for_tests = 1;
  options.before_exact_candidate_verification_for_tests =
      [hook_calls](std::size_t rank) {
        hook_calls->fetch_add(1, std::memory_order_relaxed);
        if (rank == 0) {
          throw std::runtime_error(
              "phase-6 accepted runner rank failure must lose to submit");
        }
      };
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  auto const metrics_before = scheduler.metrics();
  auto const accepts_before = state.counters.candidate_accepts_attempted;

  bool submission_failure_selected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        state, options, 0, workspace, scheduler);
  } catch (larch::chart_scheduler_submit_error const&) {
    submission_failure_selected = true;
  }
  CHECK(submission_failure_selected);
  CHECK(hook_calls->load(std::memory_order_relaxed) == 1);
  CHECK(state.counters.candidate_accepts_attempted == accepts_before);
  auto const failed_metrics = scheduler.metrics();
  auto const submitted_delta =
      failed_metrics.tasks_submitted - metrics_before.tasks_submitted;
  CHECK(submitted_delta >= 1);
  CHECK(failed_metrics.tasks_completed - metrics_before.tasks_completed ==
        submitted_delta);
  CHECK(failed_metrics.tasks_joined - metrics_before.tasks_joined ==
        submitted_delta);
  CHECK(failed_metrics.pending_tasks == 0);
  CHECK(state.counters.scheduler_axes.exact_candidates.operations == 1);
  CHECK(state.counters.scheduler_axes.exact_candidates.parallel_operations ==
        1);
  CHECK(state.counters.scheduler_axes.exact_candidates.items == 4);
  CHECK(state.counters.scheduler_axes.exact_candidates.ranges == 4);
  CHECK(state.counters.scheduler_axes.exact_candidates.worker_tasks == 1);
  CHECK(state.counters.exact_candidate_admission_batches == 1);
  CHECK(state.counters.exact_candidate_parallel_batches == 1);

  // The failed wave left no published result and no live runner. Clearing the
  // test hooks must make the same state/workspace/pool immediately reusable.
  options.force_exact_candidate_submit_failure_after_for_tests.reset();
  options.before_exact_candidate_verification_for_tests = {};
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, options, 1, workspace, scheduler);
  CHECK(recovered.locally_ranked_candidates_retained == 4);
  CHECK(recovered.candidates_exact_verified == 4);
  CHECK(state.counters.scheduler_axes.exact_candidates.operations == 2);
  CHECK(state.counters.exact_candidate_admission_batches == 2);
  CHECK(scheduler.metrics().rejected_concurrent_operations == 0);

  scheduler.shutdown();
  auto const recovered_metrics = scheduler.metrics();
  CHECK(recovered_metrics.tasks_submitted == recovered_metrics.tasks_completed);
  CHECK(recovered_metrics.tasks_submitted == recovered_metrics.tasks_joined);
  CHECK(recovered_metrics.pending_tasks == 0);
  CHECK(recovered_metrics.live_pool_threads == 0);
  check_phase4_scheduler_axis_reconciliation(recovered_metrics,
                                             state.counters.scheduler_axes);
  std::println("  PASS");
}

static void test_phase6_exact_candidate_admission_stable_prefixes() {
  std::println("test_phase6_exact_candidate_admission_stable_prefixes");

  using estimate = larch::chart_spr_exact_candidate_memory_estimate;
  std::array<estimate, 4> worker_limited{
      estimate{.serial_scratch_bytes = 10,
               .inner_parallel_scratch_bytes = 100,
               .retained_result_bytes = 2},
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 200,
               .retained_result_bytes = 3},
      estimate{.serial_scratch_bytes = 31,
               .inner_parallel_scratch_bytes = 310,
               .retained_result_bytes = 4,
               .safely_bounded = false},
      estimate{.serial_scratch_bytes = 1,
               .inner_parallel_scratch_bytes = 10,
               .retained_result_bytes = 1},
  };

  // Unlimited mode preserves the historical worker-limited stable prefix and
  // deliberately ignores the finite-budget safety sentinel.
  auto unlimited = larch::plan_chart_spr_exact_candidate_admission_wave(
      worker_limited, 1, 2, 0, false);
  CHECK(unlimited.begin_rank == 1);
  CHECK(unlimited.end_rank == 3);
  CHECK(unlimited.admitted_bytes == 58);
  CHECK(!unlimited.memory_limited);
  CHECK(!unlimited.use_inner_parallelism);

  std::array<estimate, 3> exact_two_fit{
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 90,
               .retained_result_bytes = 5},
      estimate{.serial_scratch_bytes = 30,
               .inner_parallel_scratch_bytes = 90,
               .retained_result_bytes = 7},
      estimate{.serial_scratch_bytes = 1,
               .inner_parallel_scratch_bytes = 90,
               .retained_result_bytes = 1},
  };
  auto parallel = larch::plan_chart_spr_exact_candidate_admission_wave(
      exact_two_fit, 0, 4, 62, true);
  CHECK(parallel.begin_rank == 0);
  CHECK(parallel.end_rank == 2);
  CHECK(parallel.admitted_bytes == 62);
  CHECK(parallel.memory_limited);
  CHECK(!parallel.use_inner_parallelism);

  // Rank 1 blocks the stable prefix even though the smaller rank 2 would fit.
  std::array<estimate, 3> no_skip{
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 80,
               .retained_result_bytes = 5},
      estimate{.serial_scratch_bytes = 100,
               .inner_parallel_scratch_bytes = 100},
      estimate{.serial_scratch_bytes = 1, .inner_parallel_scratch_bytes = 1},
  };
  auto one_fit = larch::plan_chart_spr_exact_candidate_admission_wave(
      no_skip, 0, 3, 25, true);
  CHECK(one_fit.begin_rank == 0);
  CHECK(one_fit.end_rank == 1);
  CHECK(one_fit.admitted_bytes == 25);
  CHECK(one_fit.memory_limited);
  CHECK(!one_fit.use_inner_parallelism);

  std::println("  PASS");
}

static void test_phase6_exact_candidate_admission_singleton_policy() {
  std::println("test_phase6_exact_candidate_admission_singleton_policy");

  using estimate = larch::chart_spr_exact_candidate_memory_estimate;
  std::array<estimate, 1> estimates{
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 40,
               .retained_result_bytes = 5},
  };

  auto inner = larch::plan_chart_spr_exact_candidate_admission_wave(
      estimates, 0, 4, 45, true);
  CHECK(inner.begin_rank == 0);
  CHECK(inner.end_rank == 1);
  CHECK(inner.admitted_bytes == 45);
  CHECK(inner.use_inner_parallelism);

  auto serial = larch::plan_chart_spr_exact_candidate_admission_wave(
      estimates, 0, 4, 25, true);
  CHECK(serial.begin_rank == 0);
  CHECK(serial.end_rank == 1);
  CHECK(serial.admitted_bytes == 25);
  CHECK(!serial.use_inner_parallelism);

  // Scheduler storage is phase-specific: a candidate-parallel wave pays its
  // actual outer target=1 operation, while a memory-forced singleton pays the
  // inner operation/summary contract and never outer+inner together.
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  auto const outer_options = larch::chart_indexed_range_options{
      .minimum_grain = 1, .target_ranges_per_worker = 1};
  auto const outer_two = larch::estimate_chart_scheduler_operation_peak_bytes(
      scheduler.plan_indexed_ranges(2, outer_options));
  CHECK(outer_two > 0);
  std::array<estimate, 2> phase_estimates{
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 40,
               .inner_scheduler_scratch_bytes = 30,
               .retained_result_bytes = 5},
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 40,
               .inner_scheduler_scratch_bytes = 30,
               .retained_result_bytes = 5},
  };
  auto const parallel_budget = 50 + outer_two;
  auto outer = larch::plan_chart_spr_exact_candidate_admission_wave(
      phase_estimates, 0, 4, parallel_budget, true, &scheduler, outer_options);
  CHECK(outer.end_rank == 2);
  CHECK(outer.admitted_bytes == parallel_budget);
  CHECK(!outer.use_inner_parallelism);

  auto singleton = larch::plan_chart_spr_exact_candidate_admission_wave(
      phase_estimates, 0, 4, 75, true, &scheduler, outer_options);
  CHECK(singleton.end_rank == 1);
  CHECK(singleton.admitted_bytes == 75);
  CHECK(singleton.use_inner_parallelism);
  auto singleton_serial = larch::plan_chart_spr_exact_candidate_admission_wave(
      phase_estimates, 0, 4, 74, true, &scheduler, outer_options);
  CHECK(singleton_serial.end_rank == 1);
  CHECK(singleton_serial.admitted_bytes == 25);
  CHECK(!singleton_serial.use_inner_parallelism);
  scheduler.shutdown();

  std::println("  PASS");
}

static void test_phase6_exact_candidate_admission_budget_fail_closed() {
  std::println("test_phase6_exact_candidate_admission_budget_fail_closed");

  using estimate = larch::chart_spr_exact_candidate_memory_estimate;
  std::array<estimate, 1> too_large{
      estimate{.serial_scratch_bytes = 51,
               .inner_parallel_scratch_bytes = 80,
               .retained_result_bytes = 4},
  };
  bool saw_too_large = false;
  try {
    (void)larch::plan_chart_spr_exact_candidate_admission_wave(too_large, 0, 4,
                                                               54, true);
  } catch (larch::chart_spr_exact_candidate_budget_error const& error) {
    saw_too_large = true;
    CHECK(error.stable_rank() == 0);
    CHECK(error.required_bytes() == 55);
    CHECK(error.available_bytes() == 54);
    CHECK(std::string_view{error.what()}.contains("stable rank 0"));
  }
  CHECK(saw_too_large);

  std::array<estimate, 1> unsafe{
      estimate{.serial_scratch_bytes = 1,
               .inner_parallel_scratch_bytes = 2,
               .retained_result_bytes = 1,
               .safely_bounded = false},
  };
  auto unlimited = larch::plan_chart_spr_exact_candidate_admission_wave(
      unsafe, 0, 4, 0, false);
  CHECK(unlimited.end_rank == 1);
  CHECK(unlimited.admitted_bytes == 3);
  CHECK(unlimited.use_inner_parallelism);

  bool unsafe_failed_closed = false;
  try {
    (void)larch::plan_chart_spr_exact_candidate_admission_wave(
        unsafe, 0, 4, (std::numeric_limits<std::size_t>::max)(), true);
  } catch (larch::chart_spr_exact_candidate_budget_error const& error) {
    unsafe_failed_closed = true;
    CHECK(error.stable_rank() == 0);
    CHECK(error.required_bytes() == (std::numeric_limits<std::size_t>::max)());
    CHECK(error.available_bytes() == (std::numeric_limits<std::size_t>::max)());
  }
  CHECK(unsafe_failed_closed);

  std::println("  PASS");
}

static void test_phase6_exact_candidate_admission_retained_carryover() {
  std::println("test_phase6_exact_candidate_admission_retained_carryover");

  using estimate = larch::chart_spr_exact_candidate_memory_estimate;
  std::array<estimate, 3> estimates{
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 100,
               .retained_result_bytes = 10},
      estimate{.serial_scratch_bytes = 20,
               .inner_parallel_scratch_bytes = 100,
               .retained_result_bytes = 10},
      estimate{.serial_scratch_bytes = 35,
               .inner_parallel_scratch_bytes = 35,
               .retained_result_bytes = 5},
  };
  constexpr std::size_t candidate_budget = 60;
  auto first = larch::plan_chart_spr_exact_candidate_admission_wave(
      estimates, 0, 2, candidate_budget, true);
  CHECK(first.end_rank == 2);
  CHECK(first.admitted_bytes == candidate_budget);

  // Task scratch is released after the joined wave. Only the two retained
  // results reduce the next wave's available memory.
  auto const carried_retained =
      estimates[0].retained_result_bytes + estimates[1].retained_result_bytes;
  CHECK(carried_retained == 20);
  auto second = larch::plan_chart_spr_exact_candidate_admission_wave(
      estimates, first.end_rank, 2, candidate_budget - carried_retained, true);
  CHECK(second.begin_rank == 2);
  CHECK(second.end_rank == 3);
  CHECK(second.admitted_bytes == 40);
  CHECK(second.use_inner_parallelism);

  std::println("  PASS");
}

static void test_phase6_exact_candidate_admission_arithmetic_guards() {
  std::println("test_phase6_exact_candidate_admission_arithmetic_guards");

  auto const maximum = (std::numeric_limits<std::size_t>::max)();
  CHECK(larch::chart_spr_exact_candidate_saturating_bytes_add(maximum - 3, 4) ==
        maximum);
  CHECK(larch::chart_spr_exact_candidate_saturating_bytes_add(8, 9) == 17);

  bool checked_overflow = false;
  try {
    (void)larch::chart_spr_exact_candidate_checked_bytes_add(
        maximum, 1, "phase-6 admission test");
  } catch (std::overflow_error const& error) {
    checked_overflow = true;
    CHECK(std::string_view{error.what()}.contains("phase-6 admission test"));
  }
  CHECK(checked_overflow);

  using estimate = larch::chart_spr_exact_candidate_memory_estimate;
  std::array<estimate, 2> estimates{
      estimate{.serial_scratch_bytes = maximum - 5,
               .inner_parallel_scratch_bytes = maximum - 5,
               .retained_result_bytes = 3},
      estimate{.serial_scratch_bytes = 10, .inner_parallel_scratch_bytes = 10},
  };
  auto unlimited = larch::plan_chart_spr_exact_candidate_admission_wave(
      estimates, 0, 2, 0, false);
  CHECK(unlimited.end_rank == 2);
  CHECK(unlimited.admitted_bytes == maximum);

  std::println("  PASS");
}

static void test_phase6_exact_state_fails_before_unified_budget_overrun() {
  std::println(
      "test_phase6_exact_state_fails_before_unified_budget_overrun");

  auto fixture = make_fixture();
  auto active_build =
      larch::make_active_search_patterns(fixture.patterns, larch::chart_options{});
  auto const active_pattern_count =
      active_build.active_patterns.patterns.patterns.size();
  auto const resident_cache_only_budget =
      larch::estimate_chart_spr_pattern_batch_cache_bytes(
          fixture.grammar, active_pattern_count);
  CHECK(resident_cache_only_budget > 1);

  larch::chart_cache_options cache;
  cache.memory_budget_bytes = resident_cache_only_budget;
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  bool rejected = false;
  try {
    (void)larch::build_chart_spr_search_state_from_active(
        fixture.dag, fixture.grammar, std::move(active_build),
        larch::chart_options{}, /*build_exact_trim=*/true,
        larch::multisite_trim_options{}, cache, {}, &scheduler);
  } catch (larch::chart_spr_exact_state_budget_error const& error) {
    rejected = true;
    CHECK(error.budget_bytes() == resident_cache_only_budget);
    CHECK(error.required_bytes() > error.budget_bytes());
    CHECK(std::string{error.what()}.find("exact-state admission") !=
          std::string::npos);
  }
  CHECK(rejected);
  scheduler.shutdown();
  auto const metrics = scheduler.metrics();
  // The finite budget can hold the selected resident cache but not the exact
  // setup.  Rejection must precede even the first scheduled chart allocation.
  CHECK(metrics.operations == 0);
  CHECK(metrics.tasks_submitted == 0);
  CHECK(metrics.tasks_completed == 0);
  CHECK(metrics.tasks_joined == 0);
  CHECK(metrics.pending_tasks == 0);

  std::println("  PASS");
}

static void test_phase6_finite_budget_splits_exact_candidate_wave() {
  std::println("test_phase6_finite_budget_splits_exact_candidate_wave");

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 16;
  options.max_iterations = 1;
  options.worker_count = 16;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
  // Isolate the Phase-6 exact-candidate admission axis from the independent
  // Phase-8 finite candidate-generation pipeline envelope.
  options.enable_candidate_generation_pipeline = false;

  auto unlimited_fixture = make_three_misplaced_groups_fixture();
  auto unlimited = larch::run_chart_spr_search(
      std::move(unlimited_fixture.dag), unlimited_fixture.grammar, options);
  CHECK(unlimited.iterations.size() == 1);
  CHECK(unlimited.iterations.front().locally_ranked_candidates_retained == 16);
  CHECK(unlimited.iterations.front().candidates_exact_verified == 16);
  CHECK(unlimited.summary.exact_candidate_admission_batches == 1);
  CHECK(unlimited.summary.exact_candidate_memory_limited_batches == 0);
  CHECK(unlimited.summary.exact_candidate_peak_admitted_bytes > 0);
  CHECK(unlimited.summary.exact_candidate_peak_projected_resident_bytes >
        unlimited.summary.exact_candidate_peak_admitted_bytes);
  CHECK(unlimited.canonical_digest.has_value());
  CHECK(!unlimited.canonical_digest->full_sidecar.empty());

  auto const finite_budget =
      unlimited.summary.exact_candidate_peak_projected_resident_bytes - 1;
  CHECK(finite_budget > 0);
  auto finite_options = options;
  finite_options.cache.memory_budget_bytes = finite_budget;
  auto hook_count = std::make_shared<std::atomic<std::size_t>>(0);
  finite_options.before_exact_candidate_verification_for_tests =
      [hook_count](std::size_t) {
        hook_count->fetch_add(1, std::memory_order_relaxed);
      };

  auto finite_fixture = make_three_misplaced_groups_fixture();
  auto finite = larch::run_chart_spr_search(
      std::move(finite_fixture.dag), finite_fixture.grammar, finite_options);
  CHECK(finite.iterations.size() == 1);
  CHECK(finite.iterations.front().locally_ranked_candidates_retained == 16);
  CHECK(finite.iterations.front().candidates_exact_verified == 16);
  CHECK(finite.iterations.front().exact_candidate_verification_ms.size() == 16);
  CHECK(hook_count->load(std::memory_order_relaxed) == 16);

  CHECK(finite.summary.exact_candidate_admission_batches > 1);
  CHECK(finite.summary.exact_candidate_admission_batches >
        unlimited.summary.exact_candidate_admission_batches);
  CHECK(finite.summary.exact_candidate_parallel_batches >= 1);
  CHECK(finite.summary.exact_candidate_memory_limited_batches >= 1);
  CHECK(finite.summary.exact_candidate_queued_for_memory_ms > 0.0);
  CHECK(finite.summary.exact_candidate_peak_admitted_bytes > 0);
  CHECK(finite.summary.exact_candidate_peak_admitted_bytes <
        unlimited.summary.exact_candidate_peak_admitted_bytes);
  CHECK(finite.summary.exact_candidate_peak_projected_resident_bytes <=
        finite_budget);
  CHECK(finite.summary.exact_candidate_timing_count == 16);

  CHECK(finite.summary.exact_candidate_admission_batches ==
        finite.counters.exact_candidate_admission_batches);
  CHECK(finite.summary.exact_candidate_parallel_batches ==
        finite.counters.exact_candidate_parallel_batches);
  CHECK(finite.summary.exact_candidate_inner_parallel_batches ==
        finite.counters.exact_candidate_inner_parallel_batches);
  CHECK(finite.summary.exact_candidate_memory_limited_batches ==
        finite.counters.exact_candidate_memory_limited_batches);
  CHECK(finite.summary.exact_candidate_peak_admitted_bytes ==
        finite.counters.exact_candidate_peak_admitted_bytes);
  CHECK(finite.summary.exact_candidate_peak_projected_resident_bytes ==
        finite.counters.exact_candidate_peak_projected_resident_bytes);
  CHECK(finite.summary.exact_candidate_queued_for_memory_ms ==
        finite.counters.exact_candidate_queued_for_memory_ms);

  CHECK(finite.canonical_digest.has_value());
  CHECK(larch::emit_chart_spr_semantic_digest_json(
            *finite.canonical_digest) ==
        larch::emit_chart_spr_semantic_digest_json(
            *unlimited.canonical_digest));
  CHECK(finite.canonical_digest->full_sidecar ==
        unlimited.canonical_digest->full_sidecar);

  std::println("  PASS");
}

static void test_phase6_finite_budget_rejects_before_verifier_hook() {
  std::println("test_phase6_finite_budget_rejects_before_verifier_hook");

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.worker_count = 4;
  // Isolate the Phase-6 exact-candidate admission axis from the independent
  // Phase-8 finite candidate-generation pipeline envelope.
  options.enable_candidate_generation_pipeline = false;

  // Calibrate the coordinator-resident base from an otherwise identical
  // unlimited TopK-1 operation. Its single admitted amount is exactly the
  // candidate's inner-parallel scratch plus retained result.
  auto probe_fixture = make_fixture();
  auto probe_state = larch::build_chart_spr_search_state(
      probe_fixture.dag, probe_fixture.grammar, options);
  larch::chart_scheduler probe_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  CHECK(probe_scheduler.worker_resolution().resolved_workers == 4);
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      probe_workspace;
  auto probe = larch::run_chart_spr_acceptance_iteration(
      probe_state, options, 0, probe_workspace, probe_scheduler);
  CHECK(probe.locally_ranked_candidates_retained == 1);
  CHECK(probe.candidates_exact_verified == 1);
  CHECK(probe_state.counters.exact_candidate_peak_admitted_bytes > 0);
  CHECK(probe_state.counters.exact_candidate_peak_projected_resident_bytes >
        probe_state.counters.exact_candidate_peak_admitted_bytes);
  auto const resident_base =
      probe_state.counters.exact_candidate_peak_projected_resident_bytes -
      probe_state.counters.exact_candidate_peak_admitted_bytes;
  CHECK(resident_base > 0);
  probe_scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(
      probe_scheduler.metrics(), probe_state.counters.scheduler_axes);

  // Build the fresh state under the unlimited cache policy, then constrain
  // only this iteration to the calibrated resident base. This isolates the
  // exact-candidate admission boundary from earlier cache-build preflights.
  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                   options);
  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  CHECK(scheduler.worker_resolution().resolved_workers == 4);
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;

  auto finite_options = options;
  finite_options.cache.memory_budget_bytes = resident_base;
  auto hook_count = std::make_shared<std::atomic<std::size_t>>(0);
  finite_options.before_exact_candidate_verification_for_tests =
      [hook_count](std::size_t) {
        hook_count->fetch_add(1, std::memory_order_relaxed);
      };
  auto const exact_verifications_before = state.counters.exact_verifications;
  auto const admission_batches_before =
      state.counters.exact_candidate_admission_batches;
  auto const peak_admitted_before =
      state.counters.exact_candidate_peak_admitted_bytes;
  auto const peak_projected_before =
      state.counters.exact_candidate_peak_projected_resident_bytes;

  bool rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        state, finite_options, 0, workspace, scheduler);
  } catch (larch::chart_spr_exact_candidate_budget_error const& error) {
    rejected = true;
    CHECK(error.stable_rank() == 0);
    CHECK(error.required_bytes() > 0);
    CHECK(error.available_bytes() == 0);
  }
  CHECK(rejected);
  CHECK(hook_count->load(std::memory_order_relaxed) == 0);
  CHECK(state.exact_verifier_concurrency->peak() == 0);
  CHECK(state.counters.exact_verifications == exact_verifications_before);
  CHECK(state.counters.exact_candidate_admission_batches ==
        admission_batches_before);
  CHECK(state.counters.exact_candidate_peak_admitted_bytes ==
        peak_admitted_before);
  CHECK(state.counters.exact_candidate_peak_projected_resident_bytes ==
        peak_projected_before);

  // Pre-admission failure must leave the same workspace, state, and scheduler
  // immediately reusable once the explicit limit is removed.
  finite_options.cache.memory_budget_bytes = 0;
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, finite_options, 1, workspace, scheduler);
  CHECK(recovered.locally_ranked_candidates_retained == 1);
  CHECK(recovered.candidates_exact_verified == 1);
  CHECK(hook_count->load(std::memory_order_relaxed) == 1);
  CHECK(state.counters.exact_verifications == exact_verifications_before + 1);
  CHECK(state.counters.exact_candidate_admission_batches ==
        admission_batches_before + 1);
  CHECK(state.counters.exact_candidate_peak_admitted_bytes > 0);
  CHECK(state.counters.exact_candidate_peak_projected_resident_bytes > 0);

  scheduler.shutdown();
  auto const metrics = scheduler.metrics();
  CHECK(metrics.tasks_submitted == metrics.tasks_completed);
  CHECK(metrics.tasks_submitted == metrics.tasks_joined);
  CHECK(metrics.pending_tasks == 0);
  CHECK(metrics.live_pool_threads == 0);
  check_phase4_scheduler_axis_reconciliation(
      metrics, state.counters.scheduler_axes);

  std::println("  PASS");
}

static void test_phase6_fixed_tightened_budget_rejects_before_generation() {
  std::println(
      "test_phase6_fixed_tightened_budget_rejects_before_generation");

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.worker_count = 4;
  // Isolate the Phase-6 exact-state gate from the independent Phase-8 finite
  // candidate-generation pipeline envelope.
  options.enable_candidate_generation_pipeline = false;

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);
  auto const published_state_bytes =
      larch::estimate_chart_spr_published_state_resident_bytes(state);
  CHECK(published_state_bytes > 1);
  auto finite_options = options;
  finite_options.cache.memory_budget_bytes = published_state_bytes - 1;

  larch::chart_scheduler scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      workspace;
  auto const generated_before =
      state.counters.candidates_generated_after_dedup;
  auto const operations_before = scheduler.metrics().operations;
  bool rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        state, finite_options, 0, workspace, scheduler);
  } catch (larch::chart_spr_exact_state_budget_error const& error) {
    rejected = true;
    CHECK(error.required_bytes() == published_state_bytes);
    CHECK(error.budget_bytes() == published_state_bytes - 1);
  }
  CHECK(rejected);
  CHECK(state.counters.candidates_generated_after_dedup == generated_before);
  CHECK(state.counters.exact_candidate_admission_batches == 0);
  CHECK(workspace.candidate_slots.capacity() == 0);
  CHECK(workspace.candidate_copy_scratch.capacity() == 0);
  CHECK(workspace.local_results.capacity() == 0);
  CHECK(scheduler.metrics().operations == operations_before);

  finite_options.cache.memory_budget_bytes = 0;
  auto recovered = larch::run_chart_spr_acceptance_iteration(
      state, finite_options, 1, workspace, scheduler);
  CHECK(recovered.locally_ranked_candidates_retained == 1);
  CHECK(recovered.candidates_exact_verified == 1);
  CHECK(state.counters.exact_candidate_admission_batches == 1);
  scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(
      scheduler.metrics(), state.counters.scheduler_axes);

  std::println("  PASS");
}

static void test_phase6_custom_exact_contracts_fail_closed_and_serialize() {
  std::println(
      "test_phase6_custom_exact_contracts_fail_closed_and_serialize");

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  options.worker_count = 4;
  options.cache.memory_budget_bytes = std::size_t{1} << 40;
  // Isolate the Phase-6 exact-provider contracts from the independent
  // Phase-8 finite candidate-generation pipeline envelope.
  options.enable_candidate_generation_pipeline = false;

  auto missing_fixture = make_fixture();
  auto missing_state = larch::build_chart_spr_search_state(
      missing_fixture.dag, missing_fixture.grammar, options);
  auto missing_calls = std::make_shared<std::atomic<std::size_t>>(0);
  missing_state.contextual_exact_multisite_verifier =
      [missing_calls](larch::chart_spr_search_state const& verifier_state,
                      larch::chart_spr_candidate_score candidate,
                      larch::checked_chart_execution_plan_ref const& checked,
                      larch::chart_spr_exact_verification_context& context,
                      larch::multisite_trim_options const& trim_options) {
        missing_calls->fetch_add(1, std::memory_order_relaxed);
        return larch::verify_candidate_exact_against_state_impl(
            verifier_state, std::move(candidate), checked, context,
            trim_options);
      };
  larch::declare_chart_spr_state_callback_target_resident_bytes(missing_state,
                                                                4096);
  missing_state.exact_multisite_verifier_parallel_safe = true;
  larch::chart_scheduler missing_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      missing_workspace;
  bool missing_estimator_rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        missing_state, options, 0, missing_workspace, missing_scheduler);
  } catch (larch::chart_spr_exact_candidate_budget_error const&) {
    missing_estimator_rejected = true;
  }
  CHECK(missing_estimator_rejected);
  CHECK(missing_calls->load(std::memory_order_relaxed) == 0);

  // Cold mode is an explicit generic-verifier choice. An installed transient
  // hook must neither be called nor require its provider-specific estimator.
  auto cold_options = options;
  cold_options.verification_mode = larch::chart_spr_verification_mode::cold;
  auto cold = larch::run_chart_spr_acceptance_iteration(
      missing_state, cold_options, 1, missing_workspace, missing_scheduler);
  CHECK(cold.candidates_exact_verified == 4);
  CHECK(missing_calls->load(std::memory_order_relaxed) == 0);
  missing_scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(
      missing_scheduler.metrics(), missing_state.counters.scheduler_axes);

  auto serialized_fixture = make_fixture();
  auto serialized_state = larch::build_chart_spr_search_state(
      serialized_fixture.dag, serialized_fixture.grammar, options);
  auto serialized_calls = std::make_shared<std::atomic<std::size_t>>(0);
  // Exercise the source-compatible pre-Phase-6 callback signature. The
  // coordinator must keep it singleton and supply the admitted inner
  // scheduler exactly as the historical API did.
  serialized_state.exact_multisite_verifier =
      [serialized_calls](
          larch::chart_spr_search_state const& verifier_state,
          larch::chart_spr_candidate_score candidate,
          larch::checked_chart_execution_plan_ref const& checked,
          larch::chart_scheduler& callback_scheduler,
          larch::multisite_trim_options const& trim_options) {
        serialized_calls->fetch_add(1, std::memory_order_relaxed);
        return larch::verify_candidate_exact_against_state(
            verifier_state, std::move(candidate), checked,
            callback_scheduler, trim_options);
      };
  serialized_state.exact_multisite_transient_memory_estimator =
      [](larch::grammar_spr_candidate const&) { return 0; };
  serialized_state.exact_multisite_transient_retained_memory_estimator =
      [](larch::grammar_spr_candidate const&) { return 0; };
  larch::declare_chart_spr_state_callback_target_resident_bytes(
      serialized_state, 4096);
  // Leave parallel_safe false: legacy/custom callbacks retain serial
  // candidate invocation while each singleton may use the inner scheduler.
  larch::chart_scheduler serialized_scheduler{
      larch::chart_scheduler_options{
          .requested_workers = 4,
          .default_minimum_grain = 1,
          .default_target_ranges_per_worker = 4,
      }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      serialized_workspace;
  auto serialized = larch::run_chart_spr_acceptance_iteration(
      serialized_state, options, 0, serialized_workspace,
      serialized_scheduler);
  CHECK(serialized.candidates_exact_verified == 4);
  CHECK(serialized_calls->load(std::memory_order_relaxed) == 4);
  CHECK(serialized_state.exact_verifier_concurrency->peak() == 1);
  CHECK(serialized_state.counters.scheduler_axes.exact_candidates.operations ==
        0);
  CHECK(serialized_state.counters.exact_candidate_admission_batches == 4);
  CHECK(serialized_state.counters.exact_candidate_parallel_batches == 0);
  CHECK(serialized_state.counters.exact_candidate_inner_parallel_batches ==
        4);
  serialized_scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(
      serialized_scheduler.metrics(),
      serialized_state.counters.scheduler_axes);

  auto provider_fixture = make_fixture();
  auto provider_state = larch::build_chart_spr_search_state(
      provider_fixture.dag, provider_fixture.grammar);
  provider_state.cache_opts.memory_budget_bytes = std::size_t{1} << 40;
  auto provider_calls = std::make_shared<std::atomic<std::size_t>>(0);
  provider_state.exact_setup_provider =
      [provider_calls](larch::chart_spr_search_state const&,
                       larch::checked_chart_execution_plan_ref const&)
      -> larch::multisite_exact_setup {
    provider_calls->fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("custom exact setup must not run in estimator");
  };
  larch::declare_chart_spr_state_callback_target_resident_bytes(provider_state,
                                                                4096);
  auto unbounded_setup = larch::estimate_chart_spr_state_exact_memory(
      provider_state, {}, 4, std::size_t{1} << 40);
  CHECK(!unbounded_setup.safely_bounded);
  larch::chart_scheduler provider_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  auto provider_checked = larch::check_chart_execution_plan(
      provider_state.grammar, provider_state.execution_plan);
  bool provider_preflight_rejected = false;
  try {
    (void)larch::ensure_chart_spr_state_exact_trim(
        provider_state, provider_checked, provider_scheduler, {});
  } catch (larch::chart_spr_exact_state_budget_error const&) {
    provider_preflight_rejected = true;
  }
  CHECK(provider_preflight_rejected);
  CHECK(provider_calls->load(std::memory_order_relaxed) == 0);
  provider_scheduler.shutdown();
  provider_state.exact_setup_provider_additional_memory_estimator =
      [](larch::multisite_trim_options const&, std::size_t) { return 0; };
  larch::declare_chart_spr_state_callback_target_resident_bytes(provider_state,
                                                                4096);
  auto bounded_setup = larch::estimate_chart_spr_state_exact_memory(
      provider_state, {}, 4, std::size_t{1} << 40);
  CHECK(bounded_setup.safely_bounded);

  auto retained_fixture = make_fixture();
  auto retained_state = larch::build_chart_spr_search_state(
      retained_fixture.dag, retained_fixture.grammar, options);
  retained_state.contextual_exact_multisite_verifier =
      [](larch::chart_spr_search_state const& verifier_state,
         larch::chart_spr_candidate_score candidate,
         larch::checked_chart_execution_plan_ref const& checked,
         larch::chart_spr_exact_verification_context& context,
         larch::multisite_trim_options const& trim_options) {
        return larch::verify_candidate_exact_against_state_impl(
            verifier_state, std::move(candidate), checked, context,
            trim_options);
      };
  retained_state.exact_multisite_transient_memory_estimator =
      [](larch::grammar_spr_candidate const&) { return 0; };
  larch::declare_chart_spr_state_callback_target_resident_bytes(retained_state,
                                                                0);
  auto retained_candidate = larch::score_candidate_locally(
      retained_state, retained_fixture.candidates.front());
  auto missing_retained = larch::estimate_chart_spr_exact_candidate_memory(
      retained_state, retained_candidate, options, 4);
  CHECK(!missing_retained.safely_bounded);
  retained_state.exact_multisite_transient_retained_memory_estimator =
      [](larch::grammar_spr_candidate const&) { return 4096; };
  larch::declare_chart_spr_state_callback_target_resident_bytes(retained_state,
                                                                0);
  auto bounded_retained = larch::estimate_chart_spr_exact_candidate_memory(
      retained_state, retained_candidate, options, 4);
  CHECK(bounded_retained.safely_bounded);
  // One provider result remains in `verified` while an improving winner is
  // copied into result.accepted, so admission must carry two owning copies.
  CHECK(bounded_retained.retained_result_bytes >= 2 * 4096);

  auto fixed_contract_fixture = make_fixture();
  auto fixed_contract_state = larch::build_chart_spr_search_state(
      fixed_contract_fixture.dag, fixed_contract_fixture.grammar);
  fixed_contract_state.fixed_topology_exact_verifier =
      [](larch::chart_spr_search_state const&,
         larch::chart_spr_candidate_score candidate) {
        return candidate;
      };
  fixed_contract_state.fixed_topology_exact_additional_memory_estimator =
      [](larch::grammar_spr_candidate const&) { return 0; };
  larch::declare_chart_spr_state_callback_target_resident_bytes(
      fixed_contract_state, 0);
  auto fixed_options = options;
  fixed_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  auto fixed_candidate = larch::score_candidate_locally(
      fixed_contract_state, fixed_contract_fixture.candidates.front());
  auto fixed_missing_retained =
      larch::estimate_chart_spr_exact_candidate_memory(
          fixed_contract_state, fixed_candidate, fixed_options, 4);
  CHECK(!fixed_missing_retained.safely_bounded);
  fixed_contract_state
      .fixed_topology_exact_additional_retained_memory_estimator =
      [](larch::grammar_spr_candidate const&) { return 0; };
  larch::declare_chart_spr_state_callback_target_resident_bytes(
      fixed_contract_state, 0);
  auto fixed_bounded = larch::estimate_chart_spr_exact_candidate_memory(
      fixed_contract_state, fixed_candidate, fixed_options, 4);
  CHECK(fixed_bounded.safely_bounded);

  auto selector_fixture = make_fixture();
  larch::chart_spr_search_options selector_options;
  selector_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  selector_options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  selector_options.top_k_exact_verify = 1;
  selector_options.worker_count = 4;
  selector_options.cache.memory_budget_bytes = std::size_t{1} << 40;
  selector_options.enable_candidate_generation_pipeline = false;
  auto selector_calls = std::make_shared<std::atomic<std::size_t>>(0);
  selector_options.topology_selection_provider =
      [selector_calls](larch::chart_spr_search_state const& selector_state,
                       larch::grammar_spr_candidate const& candidate)
      -> std::optional<larch::chart_spr_topology_selection> {
    selector_calls->fetch_add(1, std::memory_order_relaxed);
    return larch::make_chart_spr_builtin_fixed_topology_selection(
        selector_state.grammar, candidate);
  };
  auto selector_state = larch::build_chart_spr_search_state(
      selector_fixture.dag, selector_fixture.grammar, selector_options);
  larch::chart_scheduler selector_scheduler{larch::chart_scheduler_options{
      .requested_workers = 4,
      .default_minimum_grain = 1,
      .default_target_ranges_per_worker = 4,
  }};
  larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
      selector_workspace;
  bool selector_rejected = false;
  try {
    (void)larch::run_chart_spr_acceptance_iteration(
        selector_state, selector_options, 0, selector_workspace,
        selector_scheduler);
  } catch (larch::chart_spr_exact_candidate_budget_error const&) {
    selector_rejected = true;
  }
  CHECK(selector_rejected);
  CHECK(selector_calls->load(std::memory_order_relaxed) == 0);
  selector_options.topology_selection_additional_memory_estimator =
      [](larch::chart_spr_search_state const&,
         larch::grammar_spr_candidate const&) {
        return larch::chart_spr_topology_selection_memory_estimate{};
      };
  auto selector_result = larch::run_chart_spr_acceptance_iteration(
      selector_state, selector_options, 1, selector_workspace,
      selector_scheduler);
  CHECK(selector_result.candidates_exact_verified == 1);
  CHECK(selector_calls->load(std::memory_order_relaxed) == 1);
  selector_scheduler.shutdown();
  check_phase4_scheduler_axis_reconciliation(
      selector_scheduler.metrics(), selector_state.counters.scheduler_axes);

  std::println("  PASS");
}

static void test_lower_bound_heuristic_acceptance_is_explicit() {
  std::println("test_lower_bound_heuristic_acceptance_is_explicit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.enumeration.max_candidates = 0;
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.local_improving_candidates > 0);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->lower_bound.value.improves());
  CHECK(!iteration.accepted->exact.has_value());
  CHECK(state.counters.exact_verifications == 0);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.accepted_moves == 0);
  CHECK(state.counters.candidate_accepts_attempted == 1);
  CHECK(std::string{larch::chart_spr_acceptance_mode_name(
            iteration.acceptance_mode)} == "lower_bound_heuristic");

  std::println("  PASS");
}

static void test_fixed_topology_exact_rejects_bare_candidate() {
  std::println("test_fixed_topology_exact_rejects_bare_candidate");

  auto fixture = make_fixture();
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, fixture.patterns);
  auto local = larch::score_candidate_locally(state, fixture.candidates.front());
  auto rejected = larch::verify_candidate_fixed_topology_exact(state, local);

  CHECK(!rejected.valid);
  CHECK(rejected.invalid_reason.find("requires") != std::string::npos);
  CHECK(!rejected.exact.has_value());
  CHECK(state.counters.exact_verifications == 0);

  std::println("  PASS");
}

static void test_fixed_topology_iteration_uses_default_selector() {
  std::println("test_fixed_topology_iteration_uses_default_selector");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 4;
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidates_exact_verified > 0);
  CHECK(state.counters.exact_verifications ==
        iteration.candidates_exact_verified);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->value.improves());
  CHECK(iteration.accepted->topology_selection.kind ==
        larch::chart_spr_topology_selection_kind::deterministic_selector);
  CHECK(iteration.accepted->topology_selection.selector_name ==
        "first_reachable_overlay_topology");
  CHECK(iteration.accepted->topology_selection.certificate.has_value());
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);
  CHECK(state.counters.accepted_moves == 0);
  CHECK(state.counters.candidate_accepts_attempted == 1);

  std::println("  PASS");
}

static void test_sampled_tree_fixed_topology_uses_source_certificate() {
  std::println("test_sampled_tree_fixed_topology_uses_source_certificate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.enumeration.source = larch::chart_spr_candidate_source::sampled_tree;
  options.enumeration.max_candidates = 8;
  options.top_k_exact_verify = 8;

  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidates_scored > 0);
  CHECK(
      iteration.candidate_generation.sampled_tree_projection_moves_preassigned >
      0);
  CHECK(iteration.candidate_generation
            .sampled_tree_projection_scheduler_operations > 0);
  CHECK(
      iteration.candidate_generation
          .sampled_tree_projection_move_enumeration_visits ==
      iteration.candidate_generation.sampled_tree_projection_moves_preassigned);
  CHECK(iteration.candidate_generation
            .sampled_tree_projection_enumeration_passes == 1);
  CHECK(
      iteration.candidate_generation.sampled_tree_source_one_pass_move_visits ==
      iteration.candidate_generation.sampled_tree_projection_moves_preassigned);
  CHECK(iteration.candidates_exact_verified > 0);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->candidate.source_tree_move.has_value());
  CHECK(iteration.accepted->candidate.source_before_topology_productions
            .has_value());
  CHECK(iteration.accepted->candidate.source_after_topology_productions
            .has_value());
  CHECK(iteration.accepted->topology_selection.kind ==
        larch::chart_spr_topology_selection_kind::explicit_certificate);
  CHECK(iteration.accepted->topology_selection.selector_name ==
        "source_tree_move_certificate");
  CHECK(iteration.accepted->topology_selection.certificate.has_value());
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->value.improves());
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::println("  PASS");
}

static void test_fixed_topology_exact_certificate_scores_selected_topology() {
  std::println("test_fixed_topology_exact_certificate_scores_selected_topology");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);
  auto candidates = larch::enumerate_grammar_spr_candidates(grammar);
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);

  std::optional<larch::chart_spr_candidate_score> chosen;
  for (auto const& candidate : candidates) {
    auto local = larch::score_candidate_locally(state, candidate);
    if (local.valid && local.lower_bound.value.improves()) {
      chosen = std::move(local);
      break;
    }
  }
  CHECK(chosen.has_value());

  std::vector<larch::overlay_production_ref> before_refs;
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    before_refs.push_back(larch::base_production_ref(
        static_cast<larch::production_id>(pid)));
  }
  auto overlay = larch::overlay_from_candidate(grammar, chosen->candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  auto after_refs = materialized.dense_production_to_ref;

  chosen->topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  chosen->topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(
          grammar, chosen->candidate, before_refs, after_refs);

  auto fixed = larch::verify_candidate_fixed_topology_exact(state, *chosen);
  CHECK(fixed.valid);
  CHECK(fixed.exact.has_value());
  CHECK(fixed.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(fixed.exact->value.improves());
  CHECK(state.counters.exact_verifications == 1);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);

  std::vector<larch::production_id> before_ids;
  for (auto ref : before_refs) before_ids.push_back(ref.id);
  auto before_topology = larch::grammar_topology_from_productions(
      grammar, before_ids);
  std::vector<larch::production_id> after_ids;
  for (std::size_t pid = 0; pid < materialized.grammar.productions.size(); ++pid) {
    after_ids.push_back(static_cast<larch::production_id>(pid));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);
  auto old_score = larch::score_selected_topology(
      grammar, patterns, before_topology);
  auto new_score = larch::score_selected_topology(
      materialized.grammar, patterns, after_topology);
  CHECK(fixed.exact->value.old_score == old_score);
  CHECK(fixed.exact->value.new_score == new_score);

  std::println("  PASS");
}

struct phase8_fixed_fixture {
  std::string name;
  larch::phylo_dag dag;
  larch::clade_grammar grammar;
};

static phase8_fixed_fixture load_phase8_binary_four_fixture() {
  phase8_fixed_fixture f;
  f.name = "wric_binary_four";
  f.dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_binary_four.fa"),
      larch::test::source_path_string("test/wric_binary_four.nwk"),
      larch::test::source_path_string("test/wric_binary_four.ref"));
  f.grammar = larch::build_clade_grammar(f.dag);
  return f;
}

static phase8_fixed_fixture load_phase8_two_polytomy_fixture() {
  phase8_fixed_fixture f;
  f.name = "wric_two_polytomy";
  f.dag = larch::build_from_fasta_newick(
      larch::test::source_path_string("test/wric_two_polytomy.fa"),
      larch::test::source_path_string("test/wric_two_polytomy.nwk"),
      larch::test::source_path_string("test/wric_two_polytomy.ref"));
  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_exact_or_fail;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      f.dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "phase8 fixed-topology two-polytomy");
  f.grammar = std::move(refinement.grammar);
  return f;
}

static phase8_fixed_fixture load_phase8_test_5_trees_fixture() {
  phase8_fixed_fixture f;
  f.name = "data/test_5_trees";
  constexpr std::array<char const*, 5> paths = {
      "data/test_5_trees/tree_0.pb.gz", "data/test_5_trees/tree_1.pb.gz",
      "data/test_5_trees/tree_2.pb.gz", "data/test_5_trees/tree_3.pb.gz",
      "data/test_5_trees/tree_4.pb.gz",
  };
  std::vector<larch::phylo_dag> trees;
  for (auto* path : paths) {
    trees.emplace_back(larch::load_proto_dag(path));
    larch::recompute_compact_genomes(trees.back());
    larch::set_sample_ids_from_cg(trees.back());
  }
  larch::merge merger{larch::get_reference_sequence(trees.front())};
  for (auto& tree : trees) merger.add_dag(tree);
  f.dag = larch::phylo_dag{std::move(merger.get_result())};

  larch::polytomy_refinement_options opts;
  opts.mode = larch::polytomy_mode::expand_soft_bounded;
  opts.max_shapes_per_polytomy = 1;
  auto refinement = larch::build_polytomy_refined_clade_grammar(
      f.dag, larch::clade_grammar_options{}, opts);
  larch::require_polytomy_refinement_binary_charting(
      refinement.audit, "phase8 fixed-topology test_5_trees");
  f.grammar = std::move(refinement.grammar);
  return f;
}

static bool phase8_candidate_matches_class(
    larch::grammar_spr_candidate const& candidate, std::string const& klass) {
  if (klass == "binary_spr") {
    return !candidate.removed_productions.empty() &&
           candidate.removed_productions.size() <= 2;
  }
  if (klass == "spr_with_collapse") {
    return candidate.removed_productions.size() > 1;
  }
  if (klass == "child_set_change") {
    return !candidate.removed_productions.empty() &&
           !candidate.added_productions.empty();
  }
  return true;
}

static larch::chart_spr_candidate_score phase8_choose_fixed_candidate(
    larch::chart_spr_search_state const& state, std::string const& klass,
    std::size_t max_candidates = 0) {
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;

  larch::grammar_spr_enumeration_options enumeration;
  enumeration.max_candidates = max_candidates;
  enumeration.max_candidates_is_post_dedup = true;

  std::optional<larch::chart_spr_candidate_score> chosen;
  (void)larch::for_each_grammar_spr_candidate(
      state.grammar, enumeration,
      [&](larch::grammar_spr_candidate const& candidate) {
        if (!phase8_candidate_matches_class(candidate, klass)) return true;
        auto local = larch::score_candidate_locally(state, candidate);
        if (!local.valid) return true;
        larch::attach_fixed_topology_selection_for_acceptance(state, local,
                                                              options);
        if (!local.valid || !local.topology_selection.certificate) return true;
        chosen = std::move(local);
        return false;
      });
  CHECK(chosen.has_value());
  return *chosen;
}

static void phase8_assert_fixed_candidate_matches_materialized_per_pattern(
    phase8_fixed_fixture& fixture, std::string const& klass) {
  auto state = larch::build_chart_spr_search_state(fixture.dag,
                                                    fixture.grammar);
  auto chosen = phase8_choose_fixed_candidate(state, klass);

  auto fixed = larch::verify_candidate_fixed_topology_exact(state, chosen);
  CHECK(fixed.valid);
  CHECK(fixed.exact.has_value());
  CHECK(fixed.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(state.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(state.counters.full_overlay_materializations == 0);

  auto const& certificate = *chosen.topology_selection.certificate;
  auto overlay = larch::overlay_from_candidate(fixture.grammar,
                                               chosen.candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);

  std::vector<larch::production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(
        larch::chart_spr_dense_production_id_for_ref(materialized, ref));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);

  auto selected = larch::chart_spr_overlay_selected_production_by_parent(
      fixture.grammar, chosen.candidate,
      certificate.after_overlay_productions);
  auto direct_scores = larch::fixed_topology_direct_selected_pattern_scores(
      state, chosen);
  auto const& active = state.active_patterns.patterns.patterns;
  CHECK(!active.empty());
  CHECK(direct_scores.old_pattern_scores.size() == active.size());
  CHECK(direct_scores.new_pattern_scores.size() == active.size());

  std::uint64_t materialized_new_active = 0;
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto direct_row = larch::chart_spr_restricted_overlay_topology_row(
        fixture.grammar, chosen.candidate, active[p], selected);
    auto materialized_row =
        larch::chart_multisite_detail::restricted_topology_row(
            materialized.grammar, active[p], after_topology);
    CHECK(direct_row == materialized_row);
    auto materialized_score = larch::chart_spr_weighted_root_score_from_row(
        materialized_row, active[p], state.chart_opts);
    CHECK(direct_scores.new_pattern_scores[p] == materialized_score);
    materialized_new_active = larch::chart_multisite_detail::checked_add_u64(
        materialized_new_active, materialized_score,
        "phase8 materialized per-pattern total");
  }

  auto new_full = larch::chart_spr_add_invariant_offset(
      materialized_new_active, state,
      "phase8 fixed-topology per-pattern new oracle");
  CHECK(fixed.exact->value.new_score == new_full);

  std::println("    {} / {} PASS", fixture.name, klass);
}

static void
    test_phase8_fixed_topology_cache_score_matches_materialized_per_pattern() {
  std::println(
      "test_phase8_fixed_topology_cache_score_matches_materialized_per_pattern");

  auto binary = load_phase8_binary_four_fixture();
  phase8_assert_fixed_candidate_matches_materialized_per_pattern(
      binary, "binary_spr");

  auto polytomy = load_phase8_two_polytomy_fixture();
  phase8_assert_fixed_candidate_matches_materialized_per_pattern(
      polytomy, "spr_with_collapse");

  auto five = load_phase8_test_5_trees_fixture();
  phase8_assert_fixed_candidate_matches_materialized_per_pattern(
      five, "child_set_change");

  std::println("  PASS");
}

static void test_phase8_fixed_topology_rejects_bad_selected_partition() {
  std::println("test_phase8_fixed_topology_rejects_bad_selected_partition");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto state = larch::build_chart_spr_search_state(dag, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto root = grammar.root_clade;
  auto root_prod = production_id_for(grammar, root, {ab, cd});
  auto ab_prod = production_id_for(grammar, ab, {a, b});
  auto cd_prod = production_id_for(grammar, cd, {c, d});

  larch::grammar_spr_candidate candidate;
  candidate.added_clades.push_back(larch::clade_key{{0, 2}});  // AC
  larch::overlay_grammar_production bad_root;
  bad_root.parent = larch::base_clade_ref(root);
  bad_root.children = {larch::base_clade_ref(ab), larch::temp_clade_ref(0)};
  bad_root.multiplicity = 1;
  candidate.added_productions.push_back(bad_root);
  larch::overlay_grammar_production ac_prod;
  ac_prod.parent = larch::temp_clade_ref(0);
  ac_prod.children = {larch::base_clade_ref(a), larch::base_clade_ref(c)};
  ac_prod.multiplicity = 1;
  candidate.added_productions.push_back(ac_prod);

  larch::chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  scored.topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(
          grammar, candidate,
          {larch::base_production_ref(root_prod),
           larch::base_production_ref(ab_prod),
           larch::base_production_ref(cd_prod)},
          {larch::temp_production_ref(0),
           larch::base_production_ref(ab_prod),
           larch::temp_production_ref(1)});

  auto verified = larch::verify_candidate_fixed_topology_exact(state, scored);
  CHECK(!verified.valid);
  CHECK(verified.invalid_reason.find("selected production children overlap") !=
            std::string::npos ||
        verified.invalid_reason.find("selected production children do not union") !=
            std::string::npos);

  std::println("  PASS");
}

static void test_phase8_fixed_topology_rejects_unreachable_selected_after_ref() {
  std::println(
      "test_phase8_fixed_topology_rejects_unreachable_selected_after_ref");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_base_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto state = larch::build_chart_spr_search_state(dag, grammar);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto d = clade_for(grammar, {"D"});
  auto ab = clade_for(grammar, {"A", "B"});
  auto cd = clade_for(grammar, {"C", "D"});
  auto root = grammar.root_clade;
  auto root_prod = production_id_for(grammar, root, {ab, cd});
  auto ab_prod = production_id_for(grammar, ab, {a, b});
  auto cd_prod = production_id_for(grammar, cd, {c, d});

  larch::grammar_spr_candidate candidate;
  candidate.added_clades.push_back(larch::clade_key{{0, 2}});  // AC
  candidate.added_productions.push_back(temp_prod(
      larch::temp_clade_ref(0), {larch::base_clade_ref(a),
                                 larch::base_clade_ref(c)}));

  larch::chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  scored.topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(
          grammar, candidate,
          {larch::base_production_ref(root_prod),
           larch::base_production_ref(ab_prod),
           larch::base_production_ref(cd_prod)},
          {larch::base_production_ref(root_prod),
           larch::base_production_ref(ab_prod),
           larch::base_production_ref(cd_prod),
           larch::temp_production_ref(0)});

  auto verified = larch::verify_candidate_fixed_topology_exact(state, scored);
  CHECK(!verified.valid);
  CHECK(verified.invalid_reason.find("unreachable selected production") !=
        std::string::npos);

  std::println("  PASS");
}

static void test_phase8_persistent_cache_local_commit_matches_materialized() {
  std::println(
      "test_phase8_persistent_cache_local_commit_matches_materialized");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto oracle_state = larch::build_chart_spr_search_state(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_fixed_topology_materialized_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);
  CHECK(!search.iterations.empty());
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->exact.has_value());
  CHECK(iteration.accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.summary.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.overlay_materializations_for_oracle ==
        search.counters.fixed_topology_persistent_cache_verifications);

  auto const& chosen = *iteration.accepted;
  auto const& certificate = *chosen.topology_selection.certificate;
  auto overlay = larch::overlay_from_candidate(oracle_state.grammar,
                                               chosen.candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  std::vector<larch::production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(
        larch::chart_spr_dense_production_id_for_ref(materialized, ref));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);
  (void)larch::validate_grammar_topology(materialized.grammar,
                                         after_topology);

  auto direct_scores = larch::fixed_topology_direct_selected_pattern_scores(
      oracle_state, chosen);
  auto const& active = oracle_state.active_patterns.patterns.patterns;
  std::uint64_t materialized_new_active = 0;
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto materialized_row =
        larch::chart_multisite_detail::restricted_topology_row(
            materialized.grammar, active[p], after_topology);
    auto materialized_score = larch::chart_spr_weighted_root_score_from_row(
        materialized_row, active[p], oracle_state.chart_opts);
    CHECK(direct_scores.new_pattern_scores[p] == materialized_score);
    materialized_new_active = larch::chart_multisite_detail::checked_add_u64(
        materialized_new_active, materialized_score,
        "phase8 persistent local-commit materialized total");
  }
  auto materialized_new_full = larch::chart_spr_add_invariant_offset(
      materialized_new_active, oracle_state,
      "phase8 persistent local-commit materialized full");
  CHECK(chosen.exact->value.new_score == materialized_new_full);

  std::println("  PASS");
}

static void test_phase8_persistent_cache_dag_verification_has_no_fallback() {
  std::println(
      "test_phase8_persistent_cache_dag_verification_has_no_fallback");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", five_taxon_multiparent_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_fixed_topology_materialized_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.overlay_materializations_for_oracle ==
        search.counters.fixed_topology_persistent_cache_verifications);

  std::println("  PASS");
}

static void test_phase8_per_pattern_oracle_catches_independent_moved_state() {
  std::println(
      "test_phase8_per_pattern_oracle_catches_independent_moved_state");

  auto inf = larch::chart_inf;
  std::array<larch::chart_cost, larch::nuc_state_count> moved{0, 0, inf, inf};
  std::array<larch::chart_cost, larch::nuc_state_count> detach{0, 10, inf, inf};
  std::array<larch::chart_cost, larch::nuc_state_count> reattach{10, 0, inf, inf};

  auto shared = inf;
  auto detach_only = inf;
  auto reattach_only = inf;
  for (std::uint8_t s = 0; s < larch::nuc_state_count; ++s) {
    shared = std::min(
        shared, larch::parsimony_chart_detail::saturated_add(
                    moved[s], larch::parsimony_chart_detail::saturated_add(
                                  detach[s], reattach[s])));
    detach_only = std::min(
        detach_only, larch::parsimony_chart_detail::saturated_add(moved[s],
                                                                  detach[s]));
    reattach_only = std::min(
        reattach_only, larch::parsimony_chart_detail::saturated_add(moved[s],
                                                                    reattach[s]));
  }
  auto independent = larch::parsimony_chart_detail::saturated_add(detach_only,
                                                                  reattach_only);
  CHECK(independent < shared);

  // The Phase-8 oracle is per pattern, not aggregate-only: a scorer that lets
  // the detach and reattach terms choose independent moved-subtree states would
  // under-count this pattern and fail equality immediately.
  std::vector<std::uint64_t> oracle_new{static_cast<std::uint64_t>(shared)};
  std::vector<std::uint64_t> bad_new{static_cast<std::uint64_t>(independent)};
  CHECK(bad_new != oracle_new);

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto same_state_pair = [](char const* name, char const* left,
                            char const* right, char const* state) {
    return tiny_inner(name, state,
                      {tiny_leaf(left, state), tiny_leaf(right, state)});
  };
  auto four_a_context = [&] {
    return tiny_inner("S", "A",
                      {same_state_pair("S1", "B", "C", "A"),
                       same_state_pair("S2", "D", "E", "A")});
  };
  auto four_c_context = [&] {
    return tiny_inner("T", "C",
                      {same_state_pair("T1", "F", "G", "C"),
                       same_state_pair("T2", "H", "I", "C")});
  };
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", tiny_inner("root", "A",
                       {tiny_inner("P", "A",
                                   {tiny_inner("M", "A",
                                               {tiny_leaf("A", "A"),
                                                tiny_leaf("J", "C")}),
                                    four_a_context()}),
                        four_c_context()}));
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_fixed_topology_independent_sm_bug_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests >
        0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks ==
        search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches ==
        search.counters.fixed_topology_persistent_cache_fallbacks);
  auto perturbations =
      search.counters.fixed_topology_independent_sm_bug_perturbations_for_tests;
  CHECK(perturbations == 0);
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);

  std::println("  PASS");
}

static void test_phase5_combine_rows_binary_regression() {
  std::println("test_phase5_combine_rows_binary_regression");

  larch::chart_multisite_detail::chart_row left{
      larch::chart_cost{0}, larch::chart_cost{2}, larch::chart_cost{5},
      larch::chart_inf};
  larch::chart_multisite_detail::chart_row right{
      larch::chart_cost{3}, larch::chart_cost{0}, larch::chart_cost{4},
      larch::chart_cost{6}};
  std::array<larch::chart_multisite_detail::chart_row, 2> children{left,
                                                                   right};
  auto variadic = larch::chart_multisite_detail::combine_rows(
      std::span<larch::chart_multisite_detail::chart_row const>{
          children.data(), children.size()});
  auto binary = larch::chart_multisite_detail::combine_binary_rows(left,
                                                                   right);
  CHECK(variadic == binary);

  std::println("  PASS");
}

static void phase5_check_selected_topology_fixture(
    larch::test::tiny_tree_node const& tree, std::size_t expected_max_arity) {
  auto dag = larch::test::make_tiny_labelled_tree("AA", tree);
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == expected_max_arity);

  auto refs = all_base_production_refs(grammar);
  larch::grammar_spr_candidate candidate;
  auto topology_ids = std::vector<larch::production_id>{};
  topology_ids.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    topology_ids.push_back(static_cast<larch::production_id>(pid));
  }
  auto topology = larch::grammar_topology_from_productions(grammar,
                                                           topology_ids);

  auto state = make_phase5_direct_state(dag, grammar);
  larch::chart_spr_candidate_score scored;
  scored.candidate = candidate;
  scored.topology_selection.kind =
      larch::chart_spr_topology_selection_kind::explicit_certificate;
  scored.topology_selection.certificate =
      larch::make_chart_spr_topology_certificate(grammar, candidate, refs,
                                                 refs);

  auto selected = larch::chart_spr_overlay_selected_production_by_parent(
      grammar, candidate, refs);
  auto const& active = state.active_patterns.patterns.patterns;
  std::vector<std::uint64_t> brute_scores;
  brute_scores.reserve(active.size());
  std::uint64_t brute_active_total = 0;
  for (auto const& pattern : active) {
    auto brute_row = phase5_brute_selected_topology_row(grammar, pattern,
                                                        topology);
    auto direct_row = larch::chart_spr_restricted_overlay_topology_row(
        grammar, candidate, pattern, selected);
    auto selected_row = larch::chart_multisite_detail::restricted_topology_row(
        grammar, pattern, topology);
    CHECK(direct_row == brute_row);
    CHECK(selected_row == brute_row);
    auto brute_score = larch::chart_spr_weighted_root_score_from_row(
        brute_row, pattern, state.chart_opts);
    brute_scores.push_back(brute_score);
    brute_active_total = larch::chart_multisite_detail::checked_add_u64(
        brute_active_total, brute_score, "phase5 brute active total");
  }

  auto cache_scores =
      larch::fixed_topology_selected_cache_pattern_scores_for_tests(state,
                                                                    scored);
  CHECK(cache_scores.old_pattern_scores == brute_scores);
  CHECK(cache_scores.new_pattern_scores == brute_scores);
  CHECK(cache_scores.old_active_total == brute_active_total);
  CHECK(cache_scores.new_active_total == brute_active_total);

  larch::chart_spr_search_options lazy_options;
  lazy_options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  lazy_options.cache.use_lazy_multisite_chart = true;
  auto lazy_state = larch::build_chart_spr_search_state(
      dag, grammar, lazy_options);
  CHECK(lazy_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(lazy_state.active_patterns.patterns.patterns.size() == active.size());
  auto lazy_scores =
      larch::fixed_topology_direct_selected_pattern_scores(lazy_state, scored);
  CHECK(lazy_scores.old_pattern_scores == brute_scores);
  CHECK(lazy_scores.new_pattern_scores == brute_scores);
  CHECK(lazy_scores.old_active_total == brute_active_total);
  CHECK(lazy_scores.new_active_total == brute_active_total);
  CHECK(lazy_state.counters.fixed_topology_selected_rows_computed > 0);
  CHECK(lazy_state.counters.selected_topology_multifurcation_rows > 0);

  auto verified = larch::verify_candidate_fixed_topology_exact(state, scored);
  CHECK(verified.valid);
  CHECK(verified.exact.has_value());
  CHECK(verified.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);

  auto selected_full = larch::chart_multisite_detail::checked_add_u64(
      brute_active_total, state.invariant_constant_offset,
      "phase5 selected topology invariant offset");
  CHECK(verified.exact->value.old_score == selected_full);
  CHECK(verified.exact->value.new_score == selected_full);
  CHECK(state.counters.selected_topology_multifurcation_rows >=
        active.size());
}

static void test_phase5_multifurcation_selected_topology_rows() {
  std::println("test_phase5_multifurcation_selected_topology_rows");

  phase5_check_selected_topology_fixture(phase5_arity3_selected_tree(), 3);
  phase5_check_selected_topology_fixture(phase5_arity4_selected_tree(), 4);

  std::println("  PASS");
}

static void
test_phase5_multifurcation_outside_rows_exercise_shared_state_guard() {
  std::println(
      "test_phase5_multifurcation_outside_rows_exercise_shared_state_guard");

  auto inf = larch::chart_inf;
  larch::chart_multisite_detail::chart_row moved_context{1, 1, inf, inf};
  larch::chart_multisite_detail::chart_row a_context{0, inf, inf, inf};
  larch::chart_multisite_detail::chart_row c_context{inf, 0, inf, inf};
  larch::chart_multisite_detail::chart_row parent_outside{};
  parent_outside.fill(0);

  auto detach_rows = larch::chart_spr_selected_overlay_child_outside_rows(
      parent_outside, {moved_context, a_context, a_context});
  auto reattach_rows = larch::chart_spr_selected_overlay_child_outside_rows(
      parent_outside, {moved_context, c_context, c_context});
  CHECK(detach_rows.size() == 3);
  CHECK(reattach_rows.size() == 3);
  CHECK(detach_rows[0][larch::nuc_base::A] == 0);
  CHECK(detach_rows[0][larch::nuc_base::C] == 1);
  CHECK(reattach_rows[0][larch::nuc_base::A] == 1);
  CHECK(reattach_rows[0][larch::nuc_base::C] == 0);

  // The rows above prove the selected-outside context builder accepts k-ary
  // productions.  The adversarial context below is the shared-s_M guard: a
  // scorer that minimizes detach and reattach states independently would
  // under-count the same moved-subtree root state.
  larch::chart_multisite_detail::chart_row moved{0, 0, inf, inf};
  larch::chart_multisite_detail::chart_row detach{0, 10, inf, inf};
  larch::chart_multisite_detail::chart_row reattach{10, 0, inf, inf};
  auto shared = inf;
  auto detach_only = inf;
  auto reattach_only = inf;
  for (std::uint8_t state = 0; state < larch::nuc_state_count; ++state) {
    shared = std::min(
        shared, larch::chart_trim_detail::add3(
                    moved[state], detach[state], reattach[state]));
    detach_only = std::min(
        detach_only, larch::parsimony_chart_detail::saturated_add(
                         moved[state], detach[state]));
    reattach_only = std::min(
        reattach_only, larch::parsimony_chart_detail::saturated_add(
                           moved[state], reattach[state]));
  }
  auto independent = larch::parsimony_chart_detail::saturated_add(
      detach_only, reattach_only);
  CHECK(independent < shared);

  std::println("  PASS");
}

static larch::rank3_production_taxa_key phase6_key_from_signature(
    larch::chart_spr_production_signature signature) {
  larch::rank3_production_taxa_key key;
  key.parent = std::move(signature.parent_taxa);
  key.children = std::move(signature.child_taxa);
  larch::rank3_detail::normalize_production_key(key);
  return key;
}

static larch::production_id phase6_production_id_for_key(
    larch::clade_grammar const& grammar,
    larch::rank3_production_taxa_key key) {
  larch::rank3_detail::normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto id = static_cast<larch::production_id>(pid);
    if (larch::rank3_detail::production_key_from_id(grammar, id) == key) {
      return id;
    }
  }
  return larch::no_production;
}

static std::uint64_t phase6_score_certificate_after_topology(
    larch::phylo_dag& dag, larch::clade_grammar const& grammar,
    larch::chart_spr_topology_certificate const& certificate) {
  std::vector<larch::production_id> pids;
  pids.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    auto pid = phase6_production_id_for_key(
        grammar, phase6_key_from_signature(signature));
    CHECK(pid != larch::no_production);
    pids.push_back(pid);
  }
  auto topology = larch::rank3_topology_from_productions(grammar, pids);
  auto patterns = larch::build_site_patterns(dag, grammar);
  std::uint64_t total = 0;
  for (auto const& pattern : patterns.patterns) {
    auto row = larch::chart_multisite_detail::restricted_topology_row(
        grammar, pattern, topology);
    total = larch::chart_multisite_detail::checked_add_u64(
        total,
        larch::chart_spr_weighted_root_score_from_row(row, pattern, {}),
        "phase6 rebuilt certificate score");
  }
  return total;
}

static void
test_phase6_source_multifurcation_candidate_has_no_unreachable_helper() {
  std::println(
      "test_phase6_source_multifurcation_candidate_has_no_unreachable_helper");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_internal_arity3_source_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  auto a_taxa = taxa_for(grammar, {"A"});
  auto b_taxa = taxa_for(grammar, {"B"});
  auto abc = clade_for(grammar, {"A", "B", "C"});
  CHECK(max_production_arity(grammar) == 3);

  bool found = false;
  for (auto const& candidate :
       larch::enumerate_grammar_spr_candidates(grammar)) {
    auto moved_taxa = larch::chart_spr_clade_taxa_for_ref(
        grammar, candidate, candidate.moved_clade);
    auto target_taxa = larch::chart_spr_clade_taxa_for_ref(
        grammar, candidate, candidate.new_sibling_or_target);
    if (candidate.old_parent != larch::base_clade_ref(abc) ||
        moved_taxa != a_taxa || target_taxa != b_taxa) {
      continue;
    }
    auto materialized =
        larch::materialize_overlay_grammar(larch::overlay_from_candidate(
            grammar, candidate));
    for (auto dense_pid : materialized.temp_production_to_dense) {
      CHECK(dense_pid != larch::no_production);
    }
    found = true;
  }
  CHECK(found);

  std::println("  PASS");
}

static void test_phase6_multifurcation_fixed_topology_local_commit() {
  std::println("test_phase6_multifurcation_fixed_topology_local_commit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(iteration.state_score_after < iteration.state_score_before);
  CHECK(search.summary.final_score < search.summary.initial_score);
  CHECK(search.counters.local_commit_accepted_moves == 1);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.local_commit_two_chart_oracle_runs == 1);
  CHECK(search.summary.active_pattern_count > 0);
  CHECK(search.counters.outside_cache_inside_charts_built == 0);
  CHECK(search.counters.outside_cache_inside_charts_reused ==
        search.summary.active_pattern_count);
  CHECK(search.counters.outside_cache_outside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.summary.outside_cache_inside_charts_built == 0);
  CHECK(search.summary.outside_cache_inside_charts_reused ==
        search.counters.outside_cache_inside_charts_reused);
  CHECK(search.summary.outside_cache_outside_charts_built ==
        search.counters.outside_cache_outside_charts_built);
  // Local non-lazy publication owns no preliminary dense projection: the
  // persistent inside cache builds the sole initial recurrence surface.
  // The cumulative initial-state counter contains only the later final-
  // compaction rebuild; the local search tip itself was not projected first.
  CHECK(search.counters.initial_state_inside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.counters.inside_cache_inside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.counters.inside_cache_resident_inside_charts_consumed == 0);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(search.summary.spr_multifurcation_moves_generated ==
        search.counters.spr_multifurcation_moves_generated);
  CHECK(search.counters.multifurcation_productions_scored > 0);
  CHECK(search.summary.multifurcation_productions_scored ==
        search.counters.multifurcation_productions_scored);
  CHECK(search.summary.local_leaf_state_view_uses ==
        search.counters.local_leaf_state_view_uses);
  CHECK(search.summary.local_leaf_state_view_uses > 0);
  CHECK(search.summary.local_leaf_state_owned_copies == 0);
  CHECK(search.summary.local_row_scratch_capacity_growths ==
        search.counters.local_row_scratch_capacity_growths);
  CHECK(search.summary.local_row_scratch_capacity_growths > 0);
  CHECK(search.summary.local_unit_fitch_fast_path_productions_scored ==
        search.counters.local_unit_fitch_fast_path_productions_scored);
  CHECK(search.summary.local_unit_fitch_fast_path_productions_scored > 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag, gopts);
  CHECK(max_production_arity(rebuilt) == 3);
  CHECK(iteration.accepted->topology_selection.certificate.has_value());
  CHECK(phase6_score_certificate_after_topology(
            search.dag, rebuilt,
            *iteration.accepted->topology_selection.certificate) ==
        search.summary.final_score);

  std::println("  PASS");
}

static void test_phase6_multifurcation_lower_bound_conservative_commit() {
  std::println(
      "test_phase6_multifurcation_lower_bound_conservative_commit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(!iteration.post_materialization_rejected);
  CHECK(iteration.state_score_after < iteration.state_score_before);
  CHECK(search.summary.final_score < search.summary.initial_score);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.local_commit_accepted_moves == 0);
  CHECK(search.counters.spr_multifurcation_moves_generated > 0);
  CHECK(search.summary.spr_multifurcation_moves_generated ==
        search.counters.spr_multifurcation_moves_generated);
  CHECK(search.counters.multifurcation_productions_scored > 0);
  CHECK(search.summary.multifurcation_productions_scored ==
        search.counters.multifurcation_productions_scored);

  std::println("  PASS");
}

static void test_phase6_exact_multisite_multifurcation_gate() {
  std::println("test_phase6_exact_multisite_multifurcation_gate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", phase6_arity3_misplaced_tree());
  larch::clade_grammar_options gopts;
  gopts.allow_polytomies = true;
  auto grammar = larch::build_clade_grammar(dag, gopts);
  CHECK(max_production_arity(grammar) == 3);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;

  larch::parsimony_chart_detail::reset_arity_gate_throw_counters_for_tests();
  bool threw = false;
  std::string message;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("WI6") != std::string::npos);
  CHECK(message.find("exact_multisite") != std::string::npos);
  CHECK(message.find("multifurcating") != std::string::npos);
  auto arity_gate_throws =
      larch::parsimony_chart_detail::arity_gate_throws_snapshot();
  CHECK(arity_gate_throws.total == 1);
  CHECK(arity_gate_throws.count(
            larch::arity_gate_consumer::chart_spr_exact_multisite) == 1);
  CHECK(larch::parsimony_chart_detail::arity_gate_consumer_reason(
            larch::arity_gate_consumer::chart_spr_exact_multisite)
            .find("exact_multisite") != std::string_view::npos);

  std::println("  PASS");
}

static void test_phase8_persistent_cache_invariant_failure_is_hard_error() {
  std::println(
      "test_phase8_persistent_cache_invariant_failure_is_hard_error");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_fixed_topology_cache_epoch_mismatch_for_tests = true;

  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    CHECK(std::string(e.what()).find("persistent cache epochs") !=
          std::string::npos);
  }
  CHECK(threw);

  std::println("  PASS");
}

// Phase 8 issue 2 (production gate + persistent-verifier/materialized-oracle
// coverage for every required move class).  Runs the persistent local-commit
// verifier through run_chart_spr_search on each Phase-8 fixture/move-class
// pair, with the materialized from-scratch oracle enabled, and asserts the
// per-pattern oracle contract: no cache/oracle mismatch, no fallback, the
// production direct-overlay gate stayed clean, and the persistent inside cache
// was actually consulted (issue 1: icache participation, not just epoch/shape).
static void phase8_run_persistent_verifier_on_fixture(
    phase8_fixed_fixture& fixture) {
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_fixed_topology_materialized_oracle_for_tests = true;
  // The materialized from-scratch oracle materializes once per verified
  // candidate, so bound the candidate stream to keep the fixture matrix
  // affordable while still exercising the persistent verifier + materialized
  // oracle on every required move class.
  options.enumeration.max_candidates = 12;
  options.enumeration.max_candidates_is_post_dedup = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  // The persistent-cache value agreed with BOTH the production direct-overlay
  // per-pattern gate (issue 2a) and the materialized from-scratch oracle
  // (issue 2b), so neither fallback path fired.
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  CHECK(search.counters
            .fixed_topology_persistent_cache_direct_oracle_mismatches == 0);
  // Issue 1: the persistent inside cache participated in the delta (rows were
  // cross-checked against icache), not just consulted for epoch/shape.  At
  // least one base-clade selected row must have been reused from icache on a
  // real fixture, proving the delta is "from persistent inside+outside cache,
  // restricted to affected rows".
  CHECK(search.counters.fixed_topology_icache_rows_reused > 0);
  // No dense materialization happened for exact verification (the persistent
  // path and the direct gate are both non-materializing); the only
  // materialization charged is the diagnostic materialized oracle, counted
  // separately under overlay_materializations_for_oracle.
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.overlay_materializations_for_oracle ==
        search.counters.fixed_topology_persistent_cache_verifications);
  // Every accepted move (if any) is labelled fixed_topology_exact.
  for (auto const& iter : search.iterations) {
    if (iter.accepted) {
      CHECK(iter.accepted->exact.has_value());
      CHECK(iter.accepted->exact->kind ==
            larch::chart_spr_score_kind::fixed_topology_exact);
    }
  }
  std::println("    {} PASS", fixture.name);
}

static void
    test_phase8_persistent_verifier_materialized_oracle_all_move_classes() {
  std::println(
      "test_phase8_persistent_verifier_materialized_oracle_all_move_classes");

  auto binary = load_phase8_binary_four_fixture();
  phase8_run_persistent_verifier_on_fixture(binary);

  auto polytomy = load_phase8_two_polytomy_fixture();
  phase8_run_persistent_verifier_on_fixture(polytomy);

  auto five = load_phase8_test_5_trees_fixture();
  phase8_run_persistent_verifier_on_fixture(five);

  std::println("  PASS");
}

// Phase 8 issue 4: when the independent materialized from-scratch oracle finds
// a per-pattern mismatch (forced here by the independent-s_M corruption hook),
// the verifier must use the materialized oracle's own scores as the authority
// rather than re-running the direct overlay scorer (which shares overlay-space
// machinery with the cache path).  We assert this indirectly: the fallback
// counter equals the witness count, the materialized-oracle mismatch counter
// equals the fallback count, and -- critically -- the resulting accepted
// candidate's exact score is still labelled fixed_topology_exact and matches the
// materialized oracle's per-pattern new-score total (the authority value).
static void test_phase8_oracle_mismatch_uses_materialized_oracle_result() {
  std::println(
      "test_phase8_oracle_mismatch_uses_materialized_oracle_result");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto oracle_state = larch::build_chart_spr_search_state(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_fixed_topology_independent_sm_bug_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(!search.iterations.empty());
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted->topology_selection.certificate);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 0);
  CHECK(search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests >
        0);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks ==
        search.counters.fixed_topology_independent_sm_bug_witnesses_for_tests);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches ==
        search.counters.fixed_topology_persistent_cache_fallbacks);

  // The accepted exact score is the materialized oracle's authority value:
  // recompute the materialized selected-after score per pattern and confirm the
  // accepted new_score equals it (not the cache path's corrupted value and not
  // a re-run direct-overlay value).
  auto const& chosen = *iteration.accepted;
  auto const& certificate = *chosen.topology_selection.certificate;
  auto overlay = larch::overlay_from_candidate(oracle_state.grammar,
                                               chosen.candidate);
  auto materialized = larch::materialize_overlay_grammar(overlay);
  std::vector<larch::production_id> after_ids;
  after_ids.reserve(certificate.after_overlay_productions.size());
  for (auto ref : certificate.after_overlay_productions) {
    after_ids.push_back(
        larch::chart_spr_dense_production_id_for_ref(materialized, ref));
  }
  auto after_topology = larch::grammar_topology_from_productions(
      materialized.grammar, after_ids);
  auto const& active = oracle_state.active_patterns.patterns.patterns;
  std::uint64_t materialized_new_active = 0;
  for (std::size_t p = 0; p < active.size(); ++p) {
    auto row = larch::chart_multisite_detail::restricted_topology_row(
        materialized.grammar, active[p], after_topology);
    materialized_new_active = larch::chart_multisite_detail::checked_add_u64(
        materialized_new_active,
        larch::chart_spr_weighted_root_score_from_row(
            row, active[p], oracle_state.chart_opts),
        "phase8 oracle-mismatch materialized authority total");
  }
  auto materialized_new_full = larch::chart_spr_add_invariant_offset(
      materialized_new_active, oracle_state,
      "phase8 oracle-mismatch materialized authority full");
  CHECK(chosen.exact.has_value());
  CHECK(chosen.exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(chosen.exact->value.new_score == materialized_new_full);

  std::println("  PASS");
}

static void test_phase8_chain_objective_gate_is_monotone_across_commits() {
  std::println(
      "test_phase8_chain_objective_gate_is_monotone_across_commits");

  // Issue 3: sequential fixed_topology_exact local commits must be gated
  // against the recorded chain objective (the previous accepted after-topology
  // score), not the candidate's own selected before-topology score.  Run a
  // multi-iteration search; every accepted move's new_score must be <= the
  // chain objective carried into that iteration, and the chain objective is
  // non-increasing across accepts.
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 4;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  std::uint64_t previous_chain_objective = search.summary.initial_score;
  bool any_accept = false;
  for (auto const& iter : search.iterations) {
    if (!iter.accepted || !iter.accepted_move_committed) continue;
    any_accept = true;
    CHECK(iter.accepted->exact.has_value());
    auto const accepted_new = iter.accepted->exact->value.new_score;
    // The chain-objective gate: the accepted new score must not exceed the
    // objective carried into this iteration (state_score_before for the
    // local-commit branch).
    CHECK(accepted_new <= iter.state_score_before);
    // And the chain objective is non-increasing across accepted commits.
    CHECK(accepted_new <= previous_chain_objective);
    previous_chain_objective = accepted_new;
  }
  CHECK(any_accept);

  std::println("  PASS");
}

// Phase 8 issue 1 (icache participation across local commits).  The persistent
// inside cache is chain-keyed while the selected-topology recurrence runs in
// tip space; after the first commit the two diverge, so cross-checking an
// icache row requires translating the tip clade id through
// dense_clade_to_chain_ref.  This test runs a multi-iteration local-commit
// search and confirms (a) the persistent inside cache was actually consulted
// (icache_rows_reused > 0), and (b) the cross-check did not silently invalidate
// candidates on the post-commit iterations (no spurious direct-oracle
// mismatch / fallback, and verified candidates remain valid).
static void test_phase8_icache_participation_across_local_commits() {
  std::println("test_phase8_icache_participation_across_local_commits");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 3;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  // The persistent inside cache participated in the delta.
  CHECK(search.counters.fixed_topology_icache_rows_reused > 0);
  // No spurious fallback from a thrown/misaligned icache cross-check on any
  // iteration (including post-commit iterations where tip and chain ids differ).
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters
            .fixed_topology_persistent_cache_direct_oracle_mismatches == 0);
  // The verifier ran on more than one iteration's worth of candidates (a
  // regression to "invalidate every post-commit candidate" would still pass the
  // fallback checks above but would collapse the verification count).
  CHECK(search.counters.fixed_topology_persistent_cache_verifications > 1);

  std::println("  PASS");
}

static void test_enumeration_truncation_sets_unverified_flag_even_exhaustive() {
  std::println("test_enumeration_truncation_sets_unverified_flag_even_exhaustive");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.enumeration.max_candidates = 1;
  auto state = larch::build_chart_spr_search_state(
      fixture.dag, fixture.grammar, options);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  CHECK(iteration.candidate_generation.stop_reason ==
        larch::chart_spr_candidate_stop_reason::candidate_cap);
  CHECK(iteration.candidates_scored == 1);
  CHECK(iteration.candidates_exact_verified == 1);
  CHECK(iteration.unverified_candidates_may_contain_improvements);

  std::println("  PASS");
}

static void test_phase5_no_improvement_search_stops_without_commit() {
  std::println("test_phase5_no_improvement_search_stops_without_commit");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 3;

  auto initial_nodes = larch::node_count(fixture.dag);
  auto initial_edges = larch::edge_count(fixture.dag);
  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  CHECK(search.iterations.size() == 1);
  CHECK(!search.iterations.front().accepted_move_committed);
  CHECK(search.iterations.front().accepted_inside_rows_recomputed == 0);
  CHECK(search.iterations.front().accepted_outside_rows_recomputed == 0);
  CHECK(search.iterations.front().accepted_candidate_signature.empty());
  CHECK(search.counters.accepted_moves == 0);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.initial_search_state_rebuilds == 1);
  CHECK(search.summary.full_search_state_rebuilds == 1);
  CHECK(search.summary.final_compaction_rebuilds == 0);
  CHECK(search.counters.local_candidate_scores ==
        search.iterations.front().candidates_scored);
  CHECK(larch::node_count(search.dag) == initial_nodes);
  CHECK(larch::edge_count(search.dag) == initial_edges);

  std::println("  PASS");
}

static void test_phase5_known_improving_search_commits_once() {
  std::println("test_phase5_known_improving_search_commits_once");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.cache.lazy_policy = larch::chart_spr_lazy_policy::automatic;

  auto taxon_count = grammar.taxa.id_to_sample_id.size();
  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().post_materialization_rejected);
  CHECK(search.iterations.front().reused_patterns_after_accept);
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.lazy_policy_pilot_runs == 1);
  CHECK(search.counters.lazy_policy_frozen_reuses == 1);
  CHECK(search.summary.lazy_policy.requested ==
        larch::chart_spr_lazy_policy::automatic);
  CHECK(search.counters.overlay_materializations_for_accept_materialization == 1);
  CHECK(search.summary.initial_search_state_rebuilds == 1);
  CHECK(search.summary.full_search_state_rebuilds == 2);
  CHECK(search.summary.final_compaction_rebuilds == 0);
  CHECK(search.summary.final_score <= search.summary.initial_score);
  CHECK(search.iterations.front().state_score_after <=
        search.iterations.front().state_score_before);
  CHECK(search.summary.local_scoring_ms >=
        search.iterations.front().local_scoring_ms);
  CHECK(search.summary.exact_verification_ms >=
        search.iterations.front().exact_verification_ms);
  CHECK(search.summary.initial_chart_construction_ms >= 0.0);
  CHECK(search.summary.initial_chart_construction_ms <=
        search.summary.cache_build_ms);
  CHECK(search.summary.materialization_exact_verification_ms >= 0.0);
  CHECK(search.summary.materialization_accepted_update_ms >= 0.0);
  CHECK(search.summary.materialization_final_compaction_ms >= 0.0);
  CHECK(search.summary.materialization_ms ==
        search.summary.materialization_exact_verification_ms +
            search.summary.materialization_accepted_update_ms +
            search.summary.materialization_final_compaction_ms);
  CHECK(search.summary.peak_concurrent_exact_verifiers >= 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  CHECK(rebuilt.taxa.id_to_sample_id.size() == taxon_count);
  auto const& accepted = *search.iterations.front().accepted;
  CHECK(!accepted.candidate.added_productions.empty());
  for (std::size_t pid = 0; pid < accepted.candidate.added_productions.size();
       ++pid) {
    auto signature = larch::chart_spr_production_signature_for_ref(
        grammar, accepted.candidate,
        larch::temp_production_ref(static_cast<larch::production_id>(pid)));
    CHECK(grammar_contains_production_signature(rebuilt, signature));
  }

  std::println("  PASS");
}

static void test_phase9_known_improving_search_uses_local_accept_update() {
  std::println("test_phase9_known_improving_search_uses_local_accept_update");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_local_against_full_for_tests = true;
  options.cache.lazy_policy = larch::chart_spr_lazy_policy::automatic;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().post_materialization_rejected);
  CHECK(search.iterations.front().reused_patterns_after_accept);
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.initial_search_state_rebuilds == 1);
  CHECK(search.summary.full_search_state_rebuilds == 1);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.counters.lazy_policy_pilot_runs == 1);
  CHECK(search.counters.lazy_policy_frozen_reuses == 1);
  CHECK(search.summary.final_compaction_ms > 0.0);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);
  CHECK(search.summary.final_score <= search.summary.initial_score);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  CHECK(rebuilt.clades.size() > 0);
  CHECK(search.summary.final_grammar_clade_count == rebuilt.clades.size());
  CHECK(search.summary.final_grammar_production_count ==
        rebuilt.productions.size());
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase5_final_compaction_uses_grammar_oracle_not_tree_override() {
  std::println("test_phase5_final_compaction_uses_grammar_oracle_not_tree_override");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  // Legacy Phase-4 tree-valued hook: Phase 5 local compaction must ignore it
  // and report the grammar-valued exact B&B optimum of the output DAG.
  options.override_final_compaction_rebuilt_score_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.summary.final_score != std::numeric_limits<std::uint64_t>::max());

  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase5_final_compaction_checks_recorded_exact_objective() {
  std::println("test_phase5_final_compaction_checks_recorded_exact_objective");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.override_local_commit_recorded_objective_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  bool threw = false;
  std::string message;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("recorded exact chain objective") != std::string::npos);
  CHECK(message.find("fresh exact diagnostic") != std::string::npos);

  std::println("  PASS");
}

static void test_phase9_pattern_batch_local_update_matches_output_dag() {
  std::println("test_phase9_pattern_batch_local_update_matches_output_dag");

  auto dag = larch::test::make_tiny_labelled_tree(
      "AA", four_taxon_two_site_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.verify_local_against_full_for_tests = true;
  options.cache.pattern_batch_size = 1;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.summary.effective_pattern_batch_size == 1);
  CHECK(search.counters.pattern_batch_cache_builds == 0);
  CHECK(search.counters.local_commit_inside_row_view_pattern_visits > 0);
  CHECK(search.summary.local_commit_inside_row_view_pattern_visits ==
        search.counters.local_commit_inside_row_view_pattern_visits);
  CHECK(search.counters.initial_state_inside_charts_built == 0);
  CHECK(search.counters.inside_cache_inside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.counters.inside_cache_resident_inside_charts_consumed == 0);
  // Candidate verification still builds exact setups for each transient
  // candidate grammar. The production initialization gate runs before that
  // work and proves state+cache recurrence ownership sums to P; the resident
  // count below proves the current-state exact setup consumed the cache.
  CHECK(search.counters.exact_setup_resident_inside_charts_consumed >=
        search.summary.active_pattern_count);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase9_fixed_topology_compaction_uses_certificate() {
  std::println("test_phase9_fixed_topology_compaction_uses_certificate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.iterations.front().accepted->exact.has_value());
  CHECK(search.summary.final_score <=
        search.iterations.front().accepted->exact->value.new_score);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(larch::build_clade_grammar(search.dag).clades.size() > 0);

  std::println("  PASS");
}

static void test_phase9_lower_bound_compaction_matches_output_dag() {
  std::println("test_phase9_lower_bound_compaction_matches_output_dag");

  // Phase 4 gate enforcement (Work item 1 exactness contract): a local commit
  // (rebuild_after_accept = false) under the lower_bound_heuristic gate is
  // rejected BEFORE the first accept and performs zero commits.  Admitting
  // heuristic-gated local commits is the deferred-verification extension,
  // explicitly out of scope and never a silent choice.  The
  // lower_bound_heuristic gate remains usable with rebuild_after_accept = true
  // (conservative mode), verified by test_phase5_rejected_candidates_* and
  // test_lower_bound_heuristic_acceptance_is_explicit.
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;

  std::string message;
  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("local commit") != std::string::npos);
  CHECK(message.find("lower_bound_heuristic") != std::string::npos);
  CHECK(message.find("exact") != std::string::npos);

  std::println("  PASS");
}

static void test_phase9_multi_iteration_local_updates_match_output_dag() {
  std::println("test_phase9_multi_iteration_local_updates_match_output_dag");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", six_taxon_paired_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 32;
  options.max_iterations = 3;
  options.rebuild_after_accept = false;
  options.verify_local_against_full_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  // Phase 4 tombstone-scope gate (resolution (a)): after the first accept the
  // productions near the move site are temp-sourced, so a sequential SPR that
  // would tombstone one is a labelled, counted skip rather than a silent
  // no-op.  On this small single-topology fixture the search therefore commits
  // the first improving move and then stops at a tombstone-scope skip; the
  // multi-accept path itself is exercised on the larger data/test_5_trees
  // fixture in the Phase 4 tests below.
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  // This fixture deterministically reaches the skip after its first commit;
  // keep the regression non-vacuous and require the aborted transaction to
  // publish no per-accept commit evidence.
  CHECK(search.counters.local_commit_tombstone_scope_skips == 1);
  CHECK(search.iterations.size() == 2);
  auto const& skipped = search.iterations.back();
  CHECK(skipped.accepted.has_value());
  CHECK(!skipped.accepted_move_committed);
  CHECK(skipped.post_materialization_rejected);
  CHECK(skipped.no_accept_reason.find("tombstone-scope") != std::string::npos);
  CHECK(skipped.accepted_inside_rows_recomputed == 0);
  CHECK(skipped.accepted_outside_rows_recomputed == 0);
  CHECK(skipped.accepted_candidate_signature.empty());
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_phase7_auto_on_accepted_rebuild_preserves_frozen_policy() {
  std::println("test_phase7_auto_on_accepted_rebuild_preserves_frozen_policy");

  auto expected_spec = make_phase7_auto_on_accepted_fixture_spec();
  auto expected_dag = larch::test::make_tiny_labelled_tree(
      expected_spec.reference, expected_spec.tree);
  auto expected_grammar = larch::build_clade_grammar(expected_dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 16;
  options.max_iterations = 1;
  options.worker_count = 4;
  options.cache.lazy_policy = larch::chart_spr_lazy_policy::automatic;
  auto expected_state = larch::build_chart_spr_search_state(
      expected_dag, expected_grammar, options);
  CHECK(expected_state.active_patterns.patterns.patterns.size() == 31);
  CHECK(expected_state.lazy_policy.requested ==
        larch::chart_spr_lazy_policy::automatic);
  CHECK(expected_state.lazy_policy.resolved ==
        larch::chart_spr_lazy_policy::on);
  CHECK(expected_state.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  auto const frozen_policy = expected_state.lazy_policy;

  auto run_spec = make_phase7_auto_on_accepted_fixture_spec();
  auto dag =
      larch::test::make_tiny_labelled_tree(run_spec.reference, run_spec.tree);
  auto grammar = larch::build_clade_grammar(dag);
  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.lazy_policy_pilot_runs == 1);
  CHECK(search.counters.lazy_policy_frozen_reuses == 1);
  CHECK(search.summary.lazy_policy == frozen_policy);
  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.summary.scheduler_axes.lazy_inside_clades.operations >= 2);
  CHECK(search.summary.scheduler_axes.lazy_outside_clades.operations >= 2);
  check_phase4_scheduler_axis_reconciliation(search);

  auto rebuilt_grammar = larch::build_clade_grammar(search.dag);
  auto dense_options = options;
  dense_options.cache.lazy_policy = larch::chart_spr_lazy_policy::off;
  auto dense_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt_grammar, dense_options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            dense_state, dense_options.exact_trim) ==
        search.summary.final_score);

  std::println("  PASS");
}

static void test_phase5_rejected_candidates_do_not_rebuild_sidecar() {
  std::println("test_phase5_rejected_candidates_do_not_rebuild_sidecar");

  auto fixture = make_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::lower_bound_heuristic;
  options.max_iterations = 1;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  CHECK(search.counters.local_candidate_scores ==
        search.iterations.front().candidates_scored);
  CHECK(search.counters.full_composite_rebuilds == 0);
  CHECK(search.counters.sidecar_rebuilds_after_accept ==
        search.counters.accepted_moves);
  CHECK(search.summary.initial_chart_construction_ms >= 0.0);
  CHECK(search.summary.materialization_ms ==
        search.summary.materialization_exact_verification_ms +
            search.summary.materialization_accepted_update_ms +
            search.summary.materialization_final_compaction_ms);
  CHECK(search.summary.peak_concurrent_exact_verifiers == 0);
  if (!search.iterations.front().accepted_move_committed) {
    CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  }

  std::println("  PASS");
}

static void test_phase5_post_materialization_worsening_rejects_commit() {
  std::println("test_phase5_post_materialization_worsening_rejects_commit");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto initial_nodes = larch::node_count(dag);
  auto initial_edges = larch::edge_count(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
  options.override_post_materialization_rebuilt_score_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(!iteration.accepted_move_committed);
  CHECK(iteration.post_materialization_rejected);
  CHECK(iteration.post_materialization_rebuilt_score ==
        std::numeric_limits<std::uint64_t>::max());
  CHECK(iteration.accepted_inside_rows_recomputed == 0);
  CHECK(iteration.accepted_outside_rows_recomputed == 0);
  CHECK(iteration.accepted_candidate_signature.empty());
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 0);
  CHECK(search.counters.post_materialization_rejections == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  // The explicit default-off request is rebuilt directly; only automatic
  // decisions use the private frozen-policy handoff.
  CHECK(search.counters.lazy_policy_pilot_runs == 0);
  CHECK(search.counters.lazy_policy_frozen_reuses == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization == 1);
  CHECK(search.summary.full_search_state_rebuilds == 2);
  CHECK(search.summary.final_score == search.summary.initial_score);
  CHECK(larch::node_count(search.dag) == initial_nodes);
  CHECK(larch::edge_count(search.dag) == initial_edges);
  CHECK(search.canonical_report.has_value());
  CHECK(search.canonical_report->iterations.size() == 1);
  auto const& canonical_iteration = search.canonical_report->iterations.front();
  CHECK(canonical_iteration.accepted_move_present);
  CHECK(!canonical_iteration.accepted_move_committed);
  CHECK(canonical_iteration.selected_stream_index.has_value());
  CHECK(!canonical_iteration.selected_signature.empty());

  std::println("  PASS");
}

static void test_phase9_local_objective_worsening_rejects_commit_evidence() {
  std::println(
      "test_phase9_local_objective_worsening_rejects_commit_evidence");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto initial_nodes = larch::node_count(dag);
  auto initial_edges = larch::edge_count(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
  options.override_post_materialization_rebuilt_score_for_tests =
      std::numeric_limits<std::uint64_t>::max();

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.accepted.has_value());
  CHECK(!iteration.accepted_move_committed);
  CHECK(iteration.post_materialization_rejected);
  CHECK(iteration.no_accept_reason.find("local commit objective worsened") !=
        std::string::npos);
  CHECK(iteration.accepted_inside_rows_recomputed == 0);
  CHECK(iteration.accepted_outside_rows_recomputed == 0);
  CHECK(iteration.accepted_candidate_signature.empty());
  CHECK(search.counters.candidate_accepts_attempted == 1);
  CHECK(search.counters.accepted_moves == 0);
  CHECK(search.counters.local_commit_accepted_moves == 0);
  CHECK(search.counters.post_materialization_rejections == 1);
  CHECK(search.counters.inside_rows_recomputed_on_commit == 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit == 0);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.summary.final_score == search.summary.initial_score);
  CHECK(larch::node_count(search.dag) == initial_nodes);
  CHECK(larch::edge_count(search.dag) == initial_edges);
  CHECK(search.canonical_report.has_value());
  CHECK(search.canonical_report->iterations.size() == 1);
  auto const& canonical_iteration = search.canonical_report->iterations.front();
  CHECK(canonical_iteration.accepted_move_present);
  CHECK(!canonical_iteration.accepted_move_committed);
  CHECK(canonical_iteration.selected_stream_index.has_value());
  CHECK(!canonical_iteration.selected_signature.empty());

  std::println("  PASS");
}

static void
    test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate() {
  std::println(
      "test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted.has_value());
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().post_materialization_rejected);
  CHECK(search.iterations.front().accepted->exact.has_value());
  CHECK(search.iterations.front().accepted->exact->kind ==
        larch::chart_spr_score_kind::fixed_topology_exact);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.iterations.front().state_score_after <=
        search.iterations.front().state_score_before);
  CHECK(search.summary.final_grammar_clade_count > 0);
  CHECK(search.summary.final_grammar_production_count > 0);

  std::println("  PASS");
}

static void test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns() {
  std::println("test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.force_pattern_fingerprint_mismatch_for_tests = true;
  options.cache.lazy_policy = larch::chart_spr_lazy_policy::automatic;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar,
                                            options);

  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(!search.iterations.front().reused_patterns_after_accept);
  CHECK(search.counters.accepted_moves == 1);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.pattern_rebuilds == 2);
  CHECK(search.counters.lazy_policy_pilot_runs == 1);
  CHECK(search.counters.lazy_policy_frozen_reuses == 1);
  CHECK(search.summary.lazy_policy.requested ==
        larch::chart_spr_lazy_policy::automatic);

  std::println("  PASS");
}

static void test_phase5_seeded_multi_iteration_is_deterministic() {
  std::println("test_phase5_seeded_multi_iteration_is_deterministic");

  auto run_once = [] {
    auto dag = larch::test::make_tiny_labelled_tree(
        "A", four_taxon_misplaced_tree());
    auto grammar = larch::build_clade_grammar(dag);

    larch::chart_spr_search_options options;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 8;
    options.max_iterations = 2;
    options.seed = 12345;
    return larch::run_chart_spr_search(std::move(dag), grammar, options);
  };

  auto first = run_once();
  auto second = run_once();

  CHECK(first.iterations.size() == 2);
  CHECK(first.iterations.size() == second.iterations.size());
  CHECK(first.summary.accepted_moves == second.summary.accepted_moves);
  CHECK(first.summary.initial_score == second.summary.initial_score);
  CHECK(first.summary.final_score == second.summary.final_score);
  CHECK(larch::node_count(first.dag) == larch::node_count(second.dag));
  CHECK(larch::edge_count(first.dag) == larch::edge_count(second.dag));

  std::size_t max_iter_affected = 0;
  std::vector<std::size_t> aggregate_affected_counts;
  for (std::size_t i = 0; i < first.iterations.size(); ++i) {
    auto const& a = first.iterations[i];
    auto const& b = second.iterations[i];
    CHECK(a.candidates_generated == b.candidates_generated);
    CHECK(a.candidates_scored == b.candidates_scored);
    CHECK(a.candidates_exact_verified == b.candidates_exact_verified);
    CHECK(a.accepted.has_value() == b.accepted.has_value());
    CHECK(a.accepted_move_committed == b.accepted_move_committed);
    CHECK(a.state_score_before == b.state_score_before);
    CHECK(a.state_score_after == b.state_score_after);
    if (a.accepted) {
      CHECK(a.accepted->lower_bound.value.delta ==
            b.accepted->lower_bound.value.delta);
      CHECK(a.accepted->exact.has_value() == b.accepted->exact.has_value());
      if (a.accepted->exact) {
        CHECK(a.accepted->exact->value.delta ==
              b.accepted->exact->value.delta);
      }
    }
    max_iter_affected = std::max(max_iter_affected,
                                 a.affected_distribution.max);
    aggregate_affected_counts.insert(aggregate_affected_counts.end(),
                                     a.affected_clade_counts.begin(),
                                     a.affected_clade_counts.end());
  }
  auto aggregate_affected = larch::summarize_affected_clade_counts(
      aggregate_affected_counts);
  CHECK(first.summary.affected_distribution.mean == aggregate_affected.mean);
  CHECK(first.summary.affected_distribution.p50 == aggregate_affected.p50);
  CHECK(first.summary.affected_distribution.p95 == aggregate_affected.p95);
  CHECK(first.summary.affected_distribution.max == max_iter_affected);
  CHECK(first.summary.affected_distribution.max == aggregate_affected.max);
  CHECK(first.summary.local_scoring_ms >= 0.0);
  CHECK(first.summary.exact_verification_ms >= 0.0);

  std::println("  PASS");
}

static void test_canonical_evidence_failure_is_hard_error() {
  std::println("test_canonical_evidence_failure_is_hard_error");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 1;
  options.max_iterations = 1;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::digest;
  options.force_canonical_evidence_failure_for_tests = true;

  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& error) {
    threw = true;
    CHECK(std::string{error.what()}.find(
              "forced exact-evidence failure for test") !=
          std::string::npos);
  }
  CHECK(threw);

  std::println("  PASS");
}

static void test_candidate_exact_bnb_overflow_is_hard_error() {
  std::println("test_candidate_exact_bnb_overflow_is_hard_error");

  for (auto const verification_mode :
       {larch::chart_spr_verification_mode::cold,
        larch::chart_spr_verification_mode::transient}) {
    auto dag =
        larch::test::make_tiny_labelled_tree("A", four_taxon_misplaced_tree());
    auto grammar = larch::build_clade_grammar(dag);
    larch::chart_spr_search_options options;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
    options.top_k_exact_verify = 1;
    options.max_iterations = 1;
    options.rebuild_after_accept = false;
    options.verification_mode = verification_mode;
    options.force_candidate_exact_bnb_overflow_for_tests = true;

    bool threw = false;
    try {
      (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
    } catch (std::overflow_error const& error) {
      threw = true;
      CHECK(std::string{error.what()} ==
            "forced candidate exact B&B arithmetic overflow for tests");
    }
    CHECK(threw);
  }

  std::println("  PASS");
}

static void test_exhaustive_exact_acceptance_matches_oracle() {
  std::println("test_exhaustive_exact_acceptance_matches_oracle");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);
  auto patterns = larch::build_site_patterns(dag, grammar);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.enumeration.max_candidates = 0;
  auto state = larch::build_chart_spr_search_state(dag, grammar, patterns);
  auto iteration = larch::run_chart_spr_acceptance_iteration(state, options);

  auto candidates = larch::enumerate_grammar_spr_candidates(
      grammar, options.enumeration);
  std::optional<larch::spr_score_result> best_oracle;
  for (auto const& candidate : candidates) {
    auto oracle = larch::score_multisite_spr_candidate_exact_oracle(
        grammar, patterns, candidate);
    if (!oracle.improves()) continue;
    if (!best_oracle || oracle.new_score < best_oracle->new_score ||
        (oracle.new_score == best_oracle->new_score &&
         oracle.delta < best_oracle->delta)) {
      best_oracle = oracle;
    }
  }

  CHECK(iteration.candidates_exact_verified == iteration.candidates_scored);
  CHECK(state.counters.exact_verifications == iteration.candidates_scored);
  CHECK(iteration.unverified_candidates_may_contain_improvements == false);
  if (best_oracle) {
    CHECK(iteration.accepted.has_value());
    CHECK(iteration.accepted->exact.has_value());
    CHECK(iteration.accepted->exact->value.new_score ==
          best_oracle->new_score);
    CHECK(iteration.accepted->exact->value.delta == best_oracle->delta);
  } else {
    CHECK(!iteration.accepted.has_value());
  }

  std::println("  PASS");
}

// ---------------------------------------------------------------------------
// Phase 4 tests: local commit into the search loop (Work items 1 + 3).
// ---------------------------------------------------------------------------
//
// These exercise the Phase 4 accept path: accepted SPR moves commit to the
// overlay chain (Phase 1) + persistent inside/outside caches (Phases 2/3)
// instead of dense-materializing per accept.  The exit criteria they pin:
//   * counter contract: sidecar_rebuilds_after_accept == 0 and
//     overlay_materializations_for_accept_materialization == 0 for a
//     local-commit run (per-accept dense materializations gone);
//   * two-chart oracle green after every accept (via the self-check flag);
//   * the exact-trim cache is never stale (absent after each commit, to be
//     recomputed lazily -- the WI3 lazy-invalidation rule);
//   * conservative mode's counters are unchanged (regression guard);
//   * multi-worker local-commit run is TSAN-clean (the epoch barrier is
//     load-bearing).
//
// Counter-granularity note (Phase 4 issue #1, updated after Phase 8).  The
// plan's literal Phase-4 exit criterion reads `full_overlay_materializations ==
// 0`, but that counter is an UMBRELLA over every dense materialization in the
// run -- per-accept (conservative path), per-candidate exact_multisite
// verification (`verify_candidate_exact_against_state`), per-candidate oracle
// scoring, and final compaction.  Phase 8 removes per-candidate materialization
// for the fixed_topology_exact gate; exact_multisite still materializes until
// Phase 9's transient chain extension lands.  The per-accept dense
// materialization -- the quantity Phase 4's reasoning was actually about -- is
// counted separately by `overlay_materializations_for_accept_materialization`,
// and that remains the load-bearing local-commit counter.

// data/test_5_trees: five protobuf trees merged into one DAG, polytomy-
// refined to a binary chart-compatible grammar the way the benchmark scripts
// do.  The merged DAG carries enough base productions that several sequential
// SPR moves can tombstone only frozen-base productions and remain committable
// under the Phase 4 tombstone-scope gate.
// A single suboptimal tree over 12 taxa carrying THREE independent misplaced
// cherry-pairs in DISJOINT subtrees (groups {A,B,C,D}, {E,F,G,H}, {I,J,K,L}),
// each flagged by its own site.  The grammar optimum (the tree itself) is
// suboptimal, so each cherry-fixing SPR strictly improves the exact parsimony.
// Because the three misplacements live in disjoint subtrees, each fix's reroute
// path is contained in its own subtree and tombstones only frozen-base
// productions -- so all three accepts stay committable under the Phase 4
// tombstone-scope gate (the temp productions introduced by an earlier accept
// live in a different subtree and are never on a later fix's path).  This is
// the fixture the Phase 4 exit criterion needs: a k >= 3 local-commit run.
static larch::phylo_dag make_three_misplaced_groups_tree() {
  using namespace larch::test;
  constexpr std::string_view ref = "AAA";
  auto leaf = [](std::string id, std::string seq) {
    return tiny_leaf(std::move(id), std::move(seq));
  };
  auto inner = [](std::string name, std::vector<tiny_tree_node> children) {
    return tiny_inner(std::move(name), "AAA", std::move(children));
  };
  // site0 separates {A,B} from {C,D}; site1 {E,F} from {G,H}; site2 {I,J} from
  // {K,L}.  Each group's start topology pairs the wrong cherries.
  auto group1 = inner("g1", {
    inner("ac", {leaf("A", "AAA"), leaf("C", "CAA")}),
    inner("bd", {leaf("B", "AAA"), leaf("D", "CAA")}),
  });
  auto group2 = inner("g2", {
    inner("eg", {leaf("E", "ACA"), leaf("G", "AAA")}),
    inner("fh", {leaf("F", "ACA"), leaf("H", "AAA")}),
  });
  auto group3 = inner("g3", {
    inner("ik", {leaf("I", "AAA"), leaf("K", "AAC")}),
    inner("jl", {leaf("J", "AAA"), leaf("L", "AAC")}),
  });
  // Root joins the three disjoint groups via a ladder so each group's fix path
  // stays below the root production.
  auto root = inner("root", {inner("g12", {group1, group2}), group3});
  return make_tiny_labelled_tree(ref, root);
}

static phase4_fixture make_three_misplaced_groups_fixture() {
  phase4_fixture f;
  f.dag = make_three_misplaced_groups_tree();
  f.grammar = larch::build_clade_grammar(f.dag);
  return f;
}

static void test_phase9_serialized_three_accept_fixture_contract() {
  std::println("test_phase9_serialized_three_accept_fixture_contract");

  auto dag = larch::load_proto_dag("test/wric_chart_three_accepts.pb.gz");
  auto grammar = larch::build_clade_grammar(dag);
  CHECK(grammar.taxa.id_to_sample_id.size() == 12);
  CHECK(grammar.clades.size() == 23);
  CHECK(grammar.productions.size() == 11);
  CHECK(larch::get_reference_sequence(dag).size() == 2592);

  // The committed topology is exactly the three-disjoint-misplaced-quartet
  // shape used by the in-memory multi-accept oracle.
  for (auto const& pair : std::array<std::array<std::string_view, 2>, 6>{{
           {{"A", "C"}},
           {{"B", "D"}},
           {{"E", "G"}},
           {{"F", "H"}},
           {{"I", "K"}},
           {{"J", "L"}},
       }}) {
    (void)clade_for(grammar, {std::string{pair[0]}, std::string{pair[1]}});
  }

  auto patterns = larch::build_site_patterns(dag, grammar);
  CHECK(patterns.total_site_count == 2592);
  CHECK(patterns.taxon_count == 12);
  CHECK(patterns.patterns.size() == 2592);
  CHECK(patterns.invariant_site_count == 0);
  CHECK(patterns.variable_site_count == 2592);

  std::array<std::array<larch::taxon_id, 4>, 3> quartet_taxa{{
      {{taxon_for(grammar, "A"), taxon_for(grammar, "B"),
        taxon_for(grammar, "C"), taxon_for(grammar, "D")}},
      {{taxon_for(grammar, "E"), taxon_for(grammar, "F"),
        taxon_for(grammar, "G"), taxon_for(grammar, "H")}},
      {{taxon_for(grammar, "I"), taxon_for(grammar, "J"),
        taxon_for(grammar, "K"), taxon_for(grammar, "L")}},
  }};
  std::size_t preferred_only_patterns = 0;
  std::size_t one_neutral_patterns = 0;
  std::array<std::size_t, 3> neutral_patterns_by_quartet{};

  for (auto const& pattern : patterns.patterns) {
    CHECK(pattern.weight == 1);
    CHECK(pattern.positions.size() == 1);
    std::size_t neutral_quartets = 0;
    for (std::size_t quartet_index = 0; quartet_index < quartet_taxa.size();
         ++quartet_index) {
      auto const& taxa = quartet_taxa[quartet_index];
      auto const first = pattern.state_by_taxon[taxa[0]];
      auto const second = pattern.state_by_taxon[taxa[1]];
      auto const third = pattern.state_by_taxon[taxa[2]];
      auto const fourth = pattern.state_by_taxon[taxa[3]];
      if (first == second && second == third && third == fourth) {
        CHECK(first == larch::nuc_base::A || first == larch::nuc_base::C);
        ++neutral_quartets;
        ++neutral_patterns_by_quartet[quartet_index];
      } else {
        CHECK(first == second);
        CHECK(third == fourth);
        CHECK(first != third);
      }
    }
    CHECK(neutral_quartets <= 1);
    if (neutral_quartets == 0)
      ++preferred_only_patterns;
    else
      ++one_neutral_patterns;
  }

  CHECK(preferred_only_patterns == 1728);
  CHECK(one_neutral_patterns == 864);
  CHECK(neutral_patterns_by_quartet ==
        (std::array<std::size_t, 3>{288, 288, 288}));

  auto active = larch::make_active_search_patterns(dag, grammar);
  CHECK(active.skipped_invariant_site_count == 0);
  CHECK(active.active_patterns.patterns.patterns.size() == 2592);

  std::println("  PASS");
}

enum class phase6_semantic_matrix_case {
  dense_cold_ua_two_pass,
  lazy_transient_exact,
  pattern_batch_fixed_exact,
};

static std::string_view phase6_semantic_matrix_case_name(
    phase6_semantic_matrix_case matrix_case) {
  switch (matrix_case) {
    case phase6_semantic_matrix_case::dense_cold_ua_two_pass:
      return "dense-cold-ua-two-pass-conservative";
    case phase6_semantic_matrix_case::lazy_transient_exact:
      return "forced-lazy-transient-exact-local";
    case phase6_semantic_matrix_case::pattern_batch_fixed_exact:
      return "pattern-batch-fixed-exact-local";
  }
  return "unknown";
}

static larch::chart_spr_search_options phase6_semantic_matrix_options(
    phase6_semantic_matrix_case matrix_case, std::size_t top_k,
    std::size_t workers) {
  larch::chart_spr_search_options options;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = top_k;
  options.max_iterations = 1;
  options.worker_count = workers;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
  // Thirty-two post-dedup candidates exercise the largest requested rank
  // buffer and include a strict improvement even with UA-edge scoring.  The
  // first improving candidate in this fixture is outside the first sixteen in
  // the canonical stream, so a 16-candidate cap would make the winner checks
  // below vacuous for that semantic axis.
  options.enumeration.max_candidates = 32;
  options.enumeration.max_candidates_is_post_dedup = true;
  options.cache.candidate_batch_size = 16;

  switch (matrix_case) {
    case phase6_semantic_matrix_case::dense_cold_ua_two_pass:
      options.acceptance_mode =
          larch::chart_spr_acceptance_mode::exact_multisite;
      options.rebuild_after_accept = true;
      options.verification_mode = larch::chart_spr_verification_mode::cold;
      options.chart.score_ua_edge = true;
      options.exact_trim.dominance_mode =
          larch::multisite_dominance_mode::two_pass_exact_mask;
      break;
    case phase6_semantic_matrix_case::lazy_transient_exact:
      options.acceptance_mode =
          larch::chart_spr_acceptance_mode::exact_multisite;
      options.rebuild_after_accept = false;
      options.verification_mode =
          larch::chart_spr_verification_mode::transient;
      options.cache.use_lazy_multisite_chart = true;
      break;
    case phase6_semantic_matrix_case::pattern_batch_fixed_exact:
      options.acceptance_mode =
          larch::chart_spr_acceptance_mode::fixed_topology_exact;
      options.rebuild_after_accept = false;
      options.cache.max_cached_patterns = 1;
      break;
  }
  return options;
}

static larch::chart_spr_search_result run_phase6_semantic_matrix_case(
    phase6_semantic_matrix_case matrix_case, std::size_t top_k,
    std::size_t workers) {
  auto fixture = make_three_misplaced_groups_fixture();
  auto options =
      phase6_semantic_matrix_options(matrix_case, top_k, workers);
  std::shared_ptr<phase6_exact_candidate_pair_rendezvous> rendezvous;
  if (top_k > 1 && workers > 1) {
    rendezvous = std::make_shared<phase6_exact_candidate_pair_rendezvous>();
    options.before_exact_candidate_verification_for_tests =
        [rendezvous](std::size_t rank) {
          rendezvous->arrive_and_wait(rank);
        };
  }

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);
  CHECK(search.iterations.size() == 1);
  auto const& iteration = search.iterations.front();
  CHECK(iteration.candidates_scored == 32);
  CHECK(iteration.locally_ranked_candidates_retained == top_k);
  CHECK(iteration.candidates_exact_verified == top_k);
  CHECK(iteration.exact_candidate_verification_ms.size() == top_k);
  CHECK(search.counters.exact_verifications == top_k);
  CHECK(search.summary.exact_candidate_timing_count == top_k);
  CHECK(search.summary.requested_worker_count == workers);
  CHECK(search.summary.resolved_worker_count == workers);
  CHECK(search.summary.local_score_worker_count == workers);
  CHECK(search.canonical_report.has_value());
  CHECK(search.canonical_digest.has_value());
  CHECK(!search.canonical_digest->full_sidecar.empty());
  CHECK(search.canonical_digest->exact_candidate_count == top_k);

  // Make the byte oracle below non-vacuous.  Every requested rank must carry
  // an exact score and the mode-appropriate retained evidence, and this
  // fixture must select and commit an improving winner.  The full canonical
  // sidecar comparison then proves equality of these values rather than merely
  // equality of an empty/default report surface.
  CHECK(search.canonical_report->iterations.size() == 1);
  auto const& canonical_iteration =
      search.canonical_report->iterations.front();
  CHECK(canonical_iteration.candidates.size() == 32);
  CHECK(canonical_iteration.ranked_stream_indices.size() == top_k);
  CHECK(canonical_iteration.exact_verified_stream_indices.size() == top_k);
  CHECK(canonical_iteration.exact_verified_stream_indices ==
        canonical_iteration.ranked_stream_indices);
  CHECK(iteration.accepted.has_value());
  CHECK(iteration.accepted_move_committed);
  CHECK(canonical_iteration.accepted_move_present);
  CHECK(canonical_iteration.accepted_move_committed);
  CHECK(canonical_iteration.selected_stream_index.has_value());
  CHECK(!canonical_iteration.selected_signature.empty());
  CHECK(search.summary.accepted_moves == 1);

  auto const selected_stream_index =
      *canonical_iteration.selected_stream_index;
  CHECK(selected_stream_index < canonical_iteration.candidates.size());
  CHECK(canonical_iteration.candidates[selected_stream_index].signature ==
        canonical_iteration.selected_signature);
  CHECK(iteration.accepted->canonical_stream_index == selected_stream_index);
  CHECK(std::ranges::find(
            canonical_iteration.exact_verified_stream_indices,
            selected_stream_index) !=
        canonical_iteration.exact_verified_stream_indices.end());

  for (std::size_t exact_rank = 0; exact_rank < top_k; ++exact_rank) {
    auto const stream_index =
        canonical_iteration.exact_verified_stream_indices[exact_rank];
    CHECK(stream_index < canonical_iteration.candidates.size());
    auto const& candidate = canonical_iteration.candidates[stream_index];
    CHECK(candidate.valid);
    CHECK(candidate.invalid_reason.empty());
    CHECK(candidate.ranked_index == exact_rank);
    CHECK(candidate.exact_verification_index == exact_rank);
    CHECK(candidate.exact.has_value());
    CHECK(candidate.exact_evidence.has_value());
    CHECK(candidate.exact->convention == "full_with_invariants");
    CHECK(candidate.exact->exact_multisite);

    auto const& evidence = *candidate.exact_evidence;
    if (matrix_case ==
        phase6_semantic_matrix_case::pattern_batch_fixed_exact) {
      CHECK(candidate.exact->kind == "fixed_topology_exact");
      CHECK(evidence.evidence_kind == "fixed_topology_certificate");
      CHECK(evidence.keep_mask_kind == "not_applicable_fixed_topology");
      CHECK(!evidence.before_topology_production_keys.empty());
      CHECK(!evidence.after_topology_production_keys.empty());
    } else {
      CHECK(candidate.exact->kind == "grammar_exact");
      CHECK(evidence.keep_production_exact);
      CHECK(evidence.keep_mask_kind ==
            "exact_optimal_production_union");
      CHECK(!evidence.kept_production_keys.empty());
      CHECK(!evidence.frontier_sizes.empty());
      CHECK(!evidence.optimal_root_provenance_classes.empty());
      for (auto const& provenance :
           evidence.optimal_root_provenance_classes) {
        CHECK(!provenance.cost.empty());
        CHECK(!provenance.production_keys.empty());
      }
    }
  }

  auto const expected_batches = (top_k + workers - 1) / workers;
  auto const candidate_parallel = top_k > 1 && workers > 1;
  auto const expected_parallel_batches =
      candidate_parallel ? expected_batches : std::size_t{0};
  auto const expected_inner_batches =
      candidate_parallel ? std::size_t{0} : expected_batches;
  CHECK(search.summary.exact_candidate_admission_batches == expected_batches);
  CHECK(search.summary.exact_candidate_parallel_batches ==
        expected_parallel_batches);
  CHECK(search.summary.exact_candidate_inner_parallel_batches ==
        expected_inner_batches);
  CHECK(search.summary.exact_candidate_memory_limited_batches == 0);
  CHECK(search.summary.exact_candidate_queued_for_memory_ms == 0.0);
  CHECK(search.summary.exact_candidate_peak_admitted_bytes > 0);
  CHECK(search.summary.exact_candidate_peak_projected_resident_bytes >=
        search.summary.exact_candidate_peak_admitted_bytes);

  CHECK(search.summary.exact_candidate_admission_batches ==
        search.counters.exact_candidate_admission_batches);
  CHECK(search.summary.exact_candidate_parallel_batches ==
        search.counters.exact_candidate_parallel_batches);
  CHECK(search.summary.exact_candidate_inner_parallel_batches ==
        search.counters.exact_candidate_inner_parallel_batches);
  CHECK(search.summary.exact_candidate_memory_limited_batches ==
        search.counters.exact_candidate_memory_limited_batches);
  CHECK(search.summary.exact_candidate_peak_admitted_bytes ==
        search.counters.exact_candidate_peak_admitted_bytes);
  CHECK(search.summary.exact_candidate_peak_projected_resident_bytes ==
        search.counters.exact_candidate_peak_projected_resident_bytes);
  CHECK(search.summary.exact_candidate_queued_for_memory_ms ==
        search.counters.exact_candidate_queued_for_memory_ms);

  auto const& exact_axis = search.summary.scheduler_axes.exact_candidates;
  CHECK(exact_axis.operations == expected_parallel_batches);
  CHECK(exact_axis.parallel_operations == expected_parallel_batches);
  if (candidate_parallel) {
    CHECK(rendezvous != nullptr);
    CHECK(rendezvous->arrivals() == 2);
    CHECK(exact_axis.items == top_k);
    CHECK(exact_axis.ranges == top_k);
    CHECK(exact_axis.worker_tasks == top_k);
    CHECK(exact_axis.active_worker_high_water >= 2);
    CHECK(search.summary.peak_concurrent_exact_verifiers >= 2);
  } else {
    CHECK(rendezvous == nullptr);
    CHECK(exact_axis.items == 0);
    CHECK(exact_axis.ranges == 0);
    CHECK(exact_axis.worker_tasks == 0);
    CHECK(exact_axis.active_worker_high_water == 0);
    CHECK(search.summary.peak_concurrent_exact_verifiers == 1);
  }
  check_phase4_scheduler_axis_reconciliation(search);

  auto const& contract = search.canonical_report->contract;
  switch (matrix_case) {
    case phase6_semantic_matrix_case::dense_cold_ua_two_pass:
      CHECK(search.summary.cache_strategy ==
            larch::chart_spr_cache_strategy::all_active_patterns);
      CHECK(search.summary.acceptance_mode ==
            larch::chart_spr_acceptance_mode::exact_multisite);
      CHECK(search.summary.verification_mode ==
            larch::chart_spr_verification_mode::cold);
      CHECK(contract.accepted_state_update == "materialize_rebuild");
      CHECK(contract.score_ua_edge);
      CHECK(contract.dominance_mode == "two-pass-exact-mask");
      break;
    case phase6_semantic_matrix_case::lazy_transient_exact:
      CHECK(search.summary.cache_strategy ==
            larch::chart_spr_cache_strategy::lazy_multisite_chart);
      CHECK(search.summary.acceptance_mode ==
            larch::chart_spr_acceptance_mode::exact_multisite);
      CHECK(search.summary.verification_mode ==
            larch::chart_spr_verification_mode::transient);
      CHECK(contract.accepted_state_update == "overlay_chain_local_commit");
      CHECK(!contract.score_ua_edge);
      break;
    case phase6_semantic_matrix_case::pattern_batch_fixed_exact:
      CHECK(search.summary.active_pattern_count > 1);
      CHECK(search.summary.cache_strategy ==
            larch::chart_spr_cache_strategy::pattern_batches);
      CHECK(search.summary.acceptance_mode ==
            larch::chart_spr_acceptance_mode::fixed_topology_exact);
      CHECK(contract.accepted_state_update == "overlay_chain_local_commit");
      CHECK(!contract.score_ua_edge);
      break;
  }
  return search;
}

static void test_phase6_top_k_worker_semantic_matrix() {
  std::println("test_phase6_top_k_worker_semantic_matrix");

  constexpr std::array matrix_cases{
      phase6_semantic_matrix_case::dense_cold_ua_two_pass,
      phase6_semantic_matrix_case::lazy_transient_exact,
      phase6_semantic_matrix_case::pattern_batch_fixed_exact,
  };
  constexpr std::array<std::size_t, 3> top_ks{1, 4, 16};
  constexpr std::array<std::size_t, 4> worker_counts{1, 2, 4, 8};

  for (auto matrix_case : matrix_cases) {
    for (auto top_k : top_ks) {
      std::optional<std::string> w1_digest_json;
      std::optional<std::string> w1_full_sidecar;
      for (auto workers : worker_counts) {
        std::println("  case={} top_k={} workers={}",
                     phase6_semantic_matrix_case_name(matrix_case), top_k,
                     workers);
        auto search =
            run_phase6_semantic_matrix_case(matrix_case, top_k, workers);
        auto digest_json = larch::emit_chart_spr_semantic_digest_json(
            *search.canonical_digest);
        if (workers == 1) {
          w1_digest_json = std::move(digest_json);
          w1_full_sidecar = search.canonical_digest->full_sidecar;
        } else {
          CHECK(w1_digest_json.has_value());
          CHECK(w1_full_sidecar.has_value());
          CHECK(digest_json == *w1_digest_json);
          CHECK(search.canonical_digest->full_sidecar == *w1_full_sidecar);
        }
        if (workers == 8) {
          auto repeated =
              run_phase6_semantic_matrix_case(matrix_case, top_k, workers);
          auto repeated_digest_json =
              larch::emit_chart_spr_semantic_digest_json(
                  *repeated.canonical_digest);
          CHECK(repeated_digest_json == digest_json);
          CHECK(repeated.canonical_digest->full_sidecar ==
                search.canonical_digest->full_sidecar);
        }
      }
    }
  }

  std::println("  PASS");
}

static void test_phase6_forced_exception_top_k_worker_parity_and_quiescence() {
  std::println(
      "test_phase6_forced_exception_top_k_worker_parity_and_quiescence");

  constexpr std::array<std::size_t, 3> top_ks{1, 4, 16};
  constexpr std::array<std::size_t, 4> worker_counts{1, 2, 4, 8};
  constexpr std::string_view forced_failure =
      "phase-6 forced stable-rank-zero exception";

  for (auto top_k : top_ks) {
    for (auto workers : worker_counts) {
      auto fixture = make_three_misplaced_groups_fixture();
      auto options = phase6_semantic_matrix_options(
          phase6_semantic_matrix_case::dense_cold_ua_two_pass, top_k,
          workers);
      options.semantic_capture = larch::chart_spr_semantic_capture_mode::off;
      auto hook_calls = std::make_shared<std::atomic<std::size_t>>(0);
      options.before_exact_candidate_verification_for_tests =
          [hook_calls](std::size_t rank) {
            hook_calls->fetch_add(1, std::memory_order_relaxed);
            if (rank == 0) {
              throw std::runtime_error(
                  "phase-6 forced stable-rank-zero exception");
            }
          };

      auto state = larch::build_chart_spr_search_state(
          fixture.dag, fixture.grammar, options);
      larch::chart_scheduler scheduler{larch::chart_scheduler_options{
          .requested_workers = workers,
          .default_minimum_grain = 1,
          .default_target_ranges_per_worker = 4,
      }};
      CHECK(scheduler.worker_resolution().resolved_workers == workers);
      larch::chart_spr_search_detail::chart_spr_acceptance_iteration_workspace
          workspace;

      std::string observed_failure;
      try {
        (void)larch::run_chart_spr_acceptance_iteration(
            state, options, 0, workspace, scheduler);
      } catch (std::runtime_error const& error) {
        observed_failure = error.what();
      }
      CHECK(observed_failure == forced_failure);
      CHECK(hook_calls->load(std::memory_order_relaxed) ==
            std::min(top_k, workers));
      CHECK(state.counters.candidate_accepts_attempted == 0);
      CHECK(state.counters.exact_candidate_admission_batches == 1);
      CHECK(state.exact_verifier_concurrency->peak() >= 1);

      auto const quiescent_metrics = scheduler.metrics();
      CHECK(quiescent_metrics.tasks_submitted ==
            quiescent_metrics.tasks_completed);
      CHECK(quiescent_metrics.tasks_submitted ==
            quiescent_metrics.tasks_joined);
      CHECK(quiescent_metrics.pending_tasks == 0);

      scheduler.shutdown();
      auto const shutdown_metrics = scheduler.metrics();
      CHECK(shutdown_metrics.tasks_submitted ==
            shutdown_metrics.tasks_completed);
      CHECK(shutdown_metrics.tasks_submitted == shutdown_metrics.tasks_joined);
      CHECK(shutdown_metrics.pending_tasks == 0);
      CHECK(shutdown_metrics.live_pool_threads == 0);
      check_phase4_scheduler_axis_reconciliation(
          shutdown_metrics, state.counters.scheduler_axes);
    }
  }

  std::println("  PASS");
}

static bool is_chart_spr_expected_overlay_chain_rejection(
    std::string const& msg) {
  return msg.find("overlay_chain") != std::string::npos &&
         (msg.find("not a frozen-base production") != std::string::npos ||
          msg.find("double tombstone") != std::string::npos);
}

static void append_first_committable_delta_for_compaction_test(
    larch::overlay_chain& chain, larch::clade_grammar& tip) {
  auto candidates = larch::enumerate_grammar_spr_candidates(tip);
  for (auto const& candidate : candidates) {
    auto delta = larch::build_spr_overlay_delta(tip, candidate);
    try {
      auto probe = chain;
      probe.append(delta);
      chain.append(delta);
      tip = larch::materialize_overlay_chain(chain).grammar;
      return;
    } catch (std::runtime_error const& e) {
      if (is_chart_spr_expected_overlay_chain_rejection(e.what())) continue;
      throw;
    }
  }
  CHECK(false && "no committable candidate for compaction test");
}

struct alternative_root_witness_grammar {
  larch::clade_grammar grammar;
  larch::clade_id root = larch::no_clade;
  larch::production_id root_acbd_production = larch::no_production;
  larch::production_id target_ac_production = larch::no_production;
};

static alternative_root_witness_grammar
make_alternative_root_witness_grammar() {
  alternative_root_witness_grammar result;
  auto& grammar = result.grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D"};

  auto add_clade = [&](std::vector<larch::taxon_id> taxa) {
    std::sort(taxa.begin(), taxa.end());
    grammar.clades.push_back(larch::clade_key{std::move(taxa)});
    return static_cast<larch::clade_id>(grammar.clades.size() - 1);
  };

  auto a = add_clade({0});
  auto b = add_clade({1});
  auto c = add_clade({2});
  auto d = add_clade({3});
  auto ab = add_clade({0, 1});
  auto cd = add_clade({2, 3});
  auto ac = add_clade({0, 2});
  auto bd = add_clade({1, 3});
  auto root = add_clade({0, 1, 2, 3});
  result.root = root;
  grammar.root_clade = root;
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());

  auto add_production = [&](larch::clade_id parent,
                            std::vector<larch::clade_id> children) {
    larch::grammar_production prod;
    prod.parent = parent;
    prod.children = std::move(children);
    auto pid = static_cast<larch::production_id>(grammar.productions.size());
    grammar.productions.push_back(std::move(prod));
    grammar.productions_by_parent[parent].push_back(pid);
    for (auto child : grammar.productions[pid].children) {
      grammar.productions_by_child[child].push_back(pid);
    }
    return pid;
  };

  (void)add_production(root, {ab, cd});
  result.root_acbd_production = add_production(root, {ac, bd});
  (void)add_production(ab, {a, b});
  (void)add_production(cd, {c, d});
  result.target_ac_production = add_production(ac, {a, c});
  (void)add_production(bd, {b, d});
  return result;
}

static void test_phase5_witness_topology_selects_required_ancestor_path() {
  std::println("test_phase5_witness_topology_selects_required_ancestor_path");

  auto fixture = make_alternative_root_witness_grammar();
  bool old_helper_threw = false;
  try {
    (void)larch::rank3_topology_preferring_productions(
        fixture.grammar, {fixture.target_ac_production});
  } catch (std::runtime_error const& e) {
    old_helper_threw = std::string{e.what()}.find("not reachable") !=
                       std::string::npos;
  }
  CHECK(old_helper_threw);

  auto topology = larch::overlay_chain_compaction_detail::
      concrete_topology_containing_production(
          fixture.grammar, fixture.target_ac_production);
  auto reachable = larch::rank3_detail::validate_topology(fixture.grammar,
                                                          topology);
  CHECK(reachable[fixture.target_ac_production]);
  CHECK(topology.selected_production_by_clade[fixture.root] ==
        fixture.root_acbd_production);

  std::println("  PASS");
}

static void run_phase5_compaction_with_trim_options(
    larch::multisite_trim_options trim_options) {
  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.exact_trim = trim_options;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);

  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto active_build = larch::make_active_search_patterns(
      search.dag, rebuilt, options.chart);
  auto oracle = larch::grammar_level_exact_parsimony(
      rebuilt, active_build.active_patterns, options.chart,
      active_build.invariant_constant_offset, trim_options);
  CHECK(oracle.exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(oracle.value == search.summary.final_score);
}

static void test_phase5_final_compaction_normalizes_trim_options() {
  std::println("test_phase5_final_compaction_normalizes_trim_options");

  larch::multisite_trim_options score_only;
  score_only.dominance_mode = larch::multisite_dominance_mode::score_only;
  score_only.require_exact_keep_mask = false;
  run_phase5_compaction_with_trim_options(score_only);

  larch::multisite_trim_options two_pass;
  two_pass.dominance_mode =
      larch::multisite_dominance_mode::two_pass_exact_mask;
  run_phase5_compaction_with_trim_options(two_pass);

  std::println("  PASS");
}

static larch::rank3_production_taxa_key phase5_key_from_signature(
    larch::chart_spr_production_signature signature) {
  larch::rank3_production_taxa_key key;
  key.parent = std::move(signature.parent_taxa);
  key.children = std::move(signature.child_taxa);
  larch::rank3_detail::normalize_production_key(key);
  return key;
}

static larch::production_id phase5_find_unique_production_by_signature(
    larch::clade_grammar const& grammar,
    larch::chart_spr_production_signature const& signature) {
  auto key = phase5_key_from_signature(signature);
  larch::production_id match = larch::no_production;
  for (std::size_t i = 0; i < grammar.productions.size(); ++i) {
    auto pid = static_cast<larch::production_id>(i);
    if (larch::rank3_detail::production_key_from_id(grammar, pid) != key) {
      continue;
    }
    CHECK(match == larch::no_production);
    match = pid;
  }
  CHECK(match != larch::no_production);
  return match;
}

static void phase5_assert_certificate_topology_present(
    larch::clade_grammar const& grammar,
    larch::chart_spr_topology_certificate const& certificate) {
  CHECK(!certificate.after_signatures.empty());
  std::vector<larch::production_id> pids;
  pids.reserve(certificate.after_signatures.size());
  for (auto const& signature : certificate.after_signatures) {
    pids.push_back(
        phase5_find_unique_production_by_signature(grammar, signature));
  }
  auto topology = larch::rank3_topology_from_productions(grammar, pids);
  auto reachable = larch::rank3_detail::validate_topology(grammar, topology);
  for (auto pid : pids) {
    CHECK(pid < reachable.size());
    CHECK(reachable[pid]);
  }
}

static void test_phase5_overlay_chain_compaction_preserves_intended_keys() {
  std::println("test_phase5_overlay_chain_compaction_preserves_intended_keys");

  auto fixture = make_three_misplaced_groups_fixture();
  auto active_build = larch::make_active_search_patterns(fixture.dag,
                                                          fixture.grammar);
  larch::overlay_chain chain(fixture.grammar);
  auto tip = fixture.grammar;
  append_first_committable_delta_for_compaction_test(chain, tip);
  append_first_committable_delta_for_compaction_test(chain, tip);

  auto intended = larch::overlay_chain_intended_production_keys(chain);
  CHECK(!intended.empty());
  auto compacted = larch::compact_overlay_chain_to_dag(fixture.dag, chain);
  CHECK(compacted.materialized_tree_count >= 1);
  CHECK(compacted.all_intended_productions_present());
  CHECK(compacted.all_witness_topologies_present());
  for (auto const& key : intended) {
    CHECK(larch::rank3_detail::has_production_key(compacted.rebuilt.grammar,
                                                   key));
  }

  auto oracle = larch::grammar_level_exact_parsimony(
      compacted.rebuilt.grammar, active_build.active_patterns, {},
      active_build.invariant_constant_offset);
  CHECK(oracle.exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(oracle.value < larch::multisite_score_inf);

  std::println("  PASS");
}

static larch::test::tiny_tree_node phase5_history_tree_one() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("ABC", "A",
                  {tiny_inner("AB", "A", {tiny_leaf("A", "A"),
                                             tiny_leaf("B", "A")}),
                   tiny_leaf("C", "A")}),
       tiny_inner("DE", "A", {tiny_leaf("D", "A"),
                                tiny_leaf("E", "A")})});
}

static larch::test::tiny_tree_node phase5_history_tree_two() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  return tiny_inner(
      "root", "A",
      {tiny_inner("AC", "A", {tiny_leaf("A", "A"),
                                tiny_leaf("C", "A")}),
       tiny_inner("BDE", "A",
                  {tiny_leaf("B", "A"),
                   tiny_inner("DE", "A", {tiny_leaf("D", "A"),
                                             tiny_leaf("E", "A")})})});
}

static larch::production_id phase5_find_production_by_key(
    larch::clade_grammar const& grammar,
    larch::rank3_production_taxa_key key) {
  larch::rank3_detail::normalize_production_key(key);
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto dense_pid = static_cast<larch::production_id>(pid);
    if (larch::rank3_detail::production_key_from_id(grammar, dense_pid) ==
        key) {
      return dense_pid;
    }
  }
  return larch::no_production;
}

static void test_phase5_compaction_augments_historical_intended_keys() {
  std::println("test_phase5_compaction_augments_historical_intended_keys");

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", phase5_history_tree_one()));
  trees.push_back(larch::test::make_tiny_labelled_tree(
      "A", phase5_history_tree_two()));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto grammar = larch::build_clade_grammar(dag);

  auto a = clade_for(grammar, {"A"});
  auto b = clade_for(grammar, {"B"});
  auto c = clade_for(grammar, {"C"});
  auto abc = clade_for(grammar, {"A", "B", "C"});

  larch::overlay_chain chain(grammar);
  larch::spr_overlay_delta delta1;
  delta1.base = &grammar;
  delta1.temp_clades.push_back(
      larch::clade_key{taxa_for(grammar, {"B", "C"})});
  delta1.temp_productions.push_back(temp_prod(
      larch::temp_clade_ref(0),
      {larch::base_clade_ref(b), larch::base_clade_ref(c)}));
  delta1.temp_productions.push_back(temp_prod(
      larch::base_clade_ref(abc),
      {larch::base_clade_ref(a), larch::temp_clade_ref(0)}));
  chain.append(delta1);

  auto after_first = larch::materialize_overlay_chain(chain);
  auto const& tip1 = after_first.grammar;
  auto abc_after_pid = after_first.temp_production_to_dense.at(1);
  CHECK(abc_after_pid != larch::no_production);
  auto abc_after_key = larch::rank3_detail::production_key_from_id(
      tip1, abc_after_pid);
  auto root1 = clade_for(tip1, {"A", "B", "C", "D", "E"});
  auto abc1 = clade_for(tip1, {"A", "B", "C"});
  auto de1 = clade_for(tip1, {"D", "E"});
  auto root_abc_de = production_id_for(tip1, root1, {abc1, de1});

  larch::spr_overlay_delta delta2;
  delta2.base = &tip1;
  delta2.removed_base_productions.push_back(root_abc_de);
  chain.append(delta2);

  auto final_materialized = larch::materialize_overlay_chain(chain);
  CHECK(phase5_find_production_by_key(final_materialized.grammar,
                                      abc_after_key) == larch::no_production);

  // Standalone compaction derives prefix witness topologies itself: callers do
  // not need to provide the historical ancestor path for abc_after_key, even
  // though that production's parent is unreachable in the final chain.
  auto compacted = larch::compact_overlay_chain_to_dag(dag, chain);
  CHECK(compacted.all_intended_productions_present());
  CHECK(compacted.all_witness_topologies_present());
  CHECK(larch::rank3_detail::has_production_key(compacted.rebuilt.grammar,
                                                 abc_after_key));

  std::println("  PASS");
}

static void test_phase4_local_commit_counter_contract_and_oracle() {
  std::println("test_phase4_local_commit_counter_contract_and_oracle");

  auto fixture = make_three_misplaced_groups_fixture();
  std::println("  fixture: {} clades, {} productions",
               fixture.grammar.clades.size(),
               fixture.grammar.productions.size());

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;  // allow enough iterations to reach k >= 3
  options.rebuild_after_accept = false;
  // Phase 3 correctness invariant: after every local commit, recompute BOTH
  // charts from scratch and assert the persistent caches agree.
  options.verify_local_commit_two_chart_oracle_for_tests = true;
  // Exercise dense-tip -> persistent-chain row translation after multiple
  // accepts, not only the initial identity map.
  options.verify_local_against_full_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit_accepted={}), "
      "tombstone_scope_skips={}, exact_verifications={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.local_commit_tombstone_scope_skips,
      search.counters.exact_verifications);

  // The Phase 4 exit criterion asks for k >= 3 local commits.  The
  // tombstone-scope gate may stop the search early if the best move at some
  // iteration tombstones a temp-sourced production; the test reports the
  // achieved count and asserts the contract on whatever was committed.
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);

  // Phase-2B cold local-commit cache construction builds inside rows once in
  // icache, then derives every outside chart from those resident rows.  The
  // binary production path reports the exact one-per-active-pattern contract.
  CHECK(search.summary.active_pattern_count > 0);
  CHECK(search.counters.outside_cache_inside_charts_built == 0);
  CHECK(search.counters.outside_cache_inside_charts_reused ==
        search.summary.active_pattern_count);
  CHECK(search.counters.outside_cache_outside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.summary.outside_cache_inside_charts_built == 0);
  CHECK(search.summary.outside_cache_inside_charts_reused ==
        search.counters.outside_cache_inside_charts_reused);
  CHECK(search.summary.outside_cache_outside_charts_built ==
        search.counters.outside_cache_outside_charts_built);

  // The initial local tip is built directly into the persistent cache. The
  // only dense-state build counted separately is the final compaction.
  CHECK(search.counters.inside_cache_inside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.counters.inside_cache_resident_inside_charts_consumed == 0);
  CHECK(search.counters.initial_state_inside_charts_built ==
        search.summary.active_pattern_count);

  // Exact setup work is cumulative across the resident current-state setup and
  // cold candidate/oracle setups, and every allocation-relevant field reaches
  // the public summary without being lost during final-compaction rebuild.
  CHECK(search.counters.exact_setup_builds > 0);
  CHECK(search.counters.exact_setup_resident_inside_charts_consumed >=
        search.summary.active_pattern_count);
  CHECK(search.counters.exact_setup_inside_charts_built > 0);
  CHECK(search.counters.exact_setup_active_leaf_state_vectors_copied > 0);
  CHECK(search.counters.exact_setup_active_leaf_states_copied > 0);
  CHECK(search.counters.exact_setup_outside_boundary_charts_built > 0);
  CHECK(search.counters.exact_setup_upper_bound_topologies_generated >=
        search.counters.exact_setup_upper_bound_topologies_unique);
  CHECK(search.counters.exact_setup_upper_bound_topologies_unique > 0);
  CHECK(search.counters.exact_setup_frontier_passes > 0);
  CHECK(search.summary.exact_setup_builds ==
        search.counters.exact_setup_builds);
  CHECK(search.summary.exact_setup_inside_charts_built ==
        search.counters.exact_setup_inside_charts_built);
  CHECK(search.summary.exact_setup_resident_inside_charts_consumed ==
        search.counters.exact_setup_resident_inside_charts_consumed);
  CHECK(search.summary.exact_setup_active_leaf_state_vectors_copied ==
        search.counters.exact_setup_active_leaf_state_vectors_copied);
  CHECK(search.summary.exact_setup_active_leaf_states_copied ==
        search.counters.exact_setup_active_leaf_states_copied);
  CHECK(search.summary.exact_setup_outside_boundary_charts_built ==
        search.counters.exact_setup_outside_boundary_charts_built);
  CHECK(search.summary.exact_setup_upper_bound_topologies_generated ==
        search.counters.exact_setup_upper_bound_topologies_generated);
  CHECK(search.summary.exact_setup_upper_bound_topologies_unique ==
        search.counters.exact_setup_upper_bound_topologies_unique);
  CHECK(search.summary.exact_setup_frontier_passes ==
        search.counters.exact_setup_frontier_passes);
  CHECK(search.counters.exact_bnb_levels > 0);
  CHECK(search.counters.exact_bnb_clades > 0);
  CHECK(search.counters.exact_bnb_product_combinations > 0);
  CHECK(search.counters.exact_bnb_frontier_entries > 0);
  CHECK(search.counters.exact_bnb_ms >= 0.0);
  CHECK(search.summary.exact_bnb_levels == search.counters.exact_bnb_levels);
  CHECK(search.summary.exact_bnb_clades == search.counters.exact_bnb_clades);
  CHECK(search.summary.exact_bnb_product_combinations ==
        search.counters.exact_bnb_product_combinations);
  CHECK(search.summary.exact_bnb_frontier_entries ==
        search.counters.exact_bnb_frontier_entries);
  CHECK(search.summary.exact_bnb_ms == search.counters.exact_bnb_ms);

  // Counter contract (cross-cutting): no per-accept sidecar rebuilds and no
  // per-accept dense accept-materializations.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  // The caches did real (affected-set-scoped) work, visible in both
  // directions -- a regression to "recompute everything" would still be > 0,
  // but a regression to "no commit at all" would be 0.
  if (search.counters.accepted_moves > 0) {
    CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
    CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
    // The two-chart oracle self-check ran after every commit without throwing
    // (the run completed), so the caches agreed with the from-scratch charts.
    CHECK(search.counters.local_commit_two_chart_oracle_runs ==
          search.counters.local_commit_accepted_moves);
    // Tip grammar was refreshed once per commit.  Exact candidate,
    // accepted-tip, and final-compaction grammars each publish their own plan.
    CHECK(search.counters.local_commit_tip_grammar_refreshes ==
          search.counters.local_commit_accepted_moves);
    CHECK(search.counters.chart_execution_plan_builds ==
          1 + search.counters.local_commit_tip_grammar_refreshes +
              search.summary.final_compaction_rebuilds +
              search.counters.transient_chain_extensions_for_verification +
              search.counters
                  .overlay_materializations_for_exact_verification);
    CHECK(search.counters.base_chart_cache_rebuilds ==
          1 + search.summary.final_compaction_rebuilds);
  }
  // Every accepted move is exact-gated: a locally committed chain's recorded
  // objective is exact.  The exact_multisite gate records grammar_exact; the
  // deferred lower_bound_heuristic-gated commit is rejected entirely
  // (test_phase9_lower_bound_compaction_matches_output_dag).
  for (auto const& it : search.iterations) {
    if (it.accepted_move_committed) {
      CHECK(it.accepted.has_value());
      CHECK(it.accepted->exact.has_value());
      CHECK(it.accepted->exact->kind ==
            larch::chart_spr_score_kind::grammar_exact);
    }
  }
  // The Phase 4 exit criterion: a k >= 3 local-commit run on a fixture with
  // three disjoint committable improving moves.
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  // The initial state and final compaction each consume one resident setup.
  // Every accepted chain tip instead adopts its already-verified exact trim,
  // so no per-accept old-state setup is rebuilt.
  CHECK(search.counters.exact_setup_resident_inside_charts_consumed ==
        2 * search.summary.active_pattern_count);
  CHECK(search.counters.accepted_exact_trims_reused ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.accepted_exact_trim_reuse_rejections == 0);
  CHECK(search.counters.local_commit_inside_row_view_pattern_visits > 0);
  CHECK(search.summary.local_commit_inside_row_view_pattern_visits ==
        search.counters.local_commit_inside_row_view_pattern_visits);
  CHECK(search.counters.pattern_batch_cache_builds == 0);
  // Final score is non-increasing (every committed move improved or held).
  CHECK(search.summary.final_score <= search.summary.initial_score);

  // Compaction produced a valid DAG whose grammar-level exact B&B optimum
  // matches the reported final score.
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// Regression guard: conservative mode (rebuild_after_accept = true) keeps its
// existing counter behavior -- per-accept sidecar rebuilds and per-accept
// accept-materializations -- unchanged from the Phase 0 baseline.
static void test_phase4_conservative_mode_counters_unchanged() {
  std::println("test_phase4_conservative_mode_counters_unchanged");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = true;  // conservative (default)

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  CHECK(search.counters.accepted_moves == 1);
  // Conservative path: one sidecar rebuild and one accept-materialization per
  // accept -- the Phase 0 baseline behavior, unchanged by Phase 4.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 1);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        1);
  CHECK(search.counters.chart_execution_plan_builds ==
        2 + search.counters.overlay_materializations_for_exact_verification);
  CHECK(search.counters.full_grammar_validations ==
        search.counters.chart_execution_plan_builds);
  CHECK(search.counters.production_partition_validations >
        grammar.productions.size());
  // Conservative mode never uses the local-commit caches.
  CHECK(search.counters.local_commit_accepted_moves == 0);
  CHECK(search.counters.inside_rows_recomputed_on_commit == 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit == 0);
  CHECK(search.iterations.size() == 1);
  CHECK(search.iterations.front().accepted_move_committed);
  CHECK(search.iterations.front().accepted_inside_rows_recomputed == 0);
  CHECK(search.iterations.front().accepted_outside_rows_recomputed == 0);
  CHECK(!search.iterations.front().accepted_candidate_signature.empty());

  std::println("  PASS");
}

static void test_phase4_local_commit_score_ua_edge_rejected() {
  std::println("test_phase4_local_commit_score_ua_edge_rejected");

  auto dag = larch::test::make_tiny_labelled_tree(
      "ATGA", four_taxon_offset_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.chart.score_ua_edge = true;

  std::string message;
  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("local commit") != std::string::npos);
  CHECK(message.find("score_ua_edge=true") != std::string::npos);
  CHECK(message.find("Phase 4 limitation") != std::string::npos);
  CHECK(message.find("not a silent fallback") != std::string::npos);

  std::println("  PASS");
}

static void test_phase4_local_commit_post_append_failure_is_hard_error() {
  std::println("test_phase4_local_commit_post_append_failure_is_hard_error");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", four_taxon_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 8;
  options.max_iterations = 1;
  options.rebuild_after_accept = false;
  options.force_local_commit_post_append_failure_for_tests = true;

  std::string message;
  bool threw = false;
  try {
    (void)larch::run_chart_spr_search(std::move(dag), grammar, options);
  } catch (std::runtime_error const& e) {
    threw = true;
    message = e.what();
  }
  CHECK(threw);
  CHECK(message.find("local commit") != std::string::npos);
  CHECK(message.find("after the append committed") != std::string::npos);
  CHECK(message.find("forced local commit post-append failure for tests") !=
        std::string::npos);
  CHECK(message.find("post-materialization rejection") != std::string::npos);

  std::println("  PASS");
}

// Exact-trim cache invariant: across a local-commit run, every compatible
// accepted verifier result becomes the current state's exact_trim_active_only
// without rebuilding the old state.  Uses the tiny three-misplaced-groups
// fixture (k >= 3 accepts under exact_multisite) so the run exercises repeated
// lazy invalidation and recomputation.  The pandemic-scale data/test_5_trees
// fixture is avoided here because a heavy exact-multisite search on it trips a
// PRE-EXISTING race in the global thread_pool used by build_clade_grammar /
// validate_dag (verified on a clean baseline checkout: rebuild_after_accept =
// true reproduces the same arity-3 throw).  Fixing that race is unrelated to
// Phase 4 and out of scope; the local-commit correctness it would exercise is
// already covered on the tiny fixture plus the two-chart oracle self-check in
// test_phase4_local_commit_counter_contract_and_oracle.
static void test_phase4_exact_trim_cache_never_stale() {
  std::println("test_phase4_exact_trim_cache_never_stale");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  // Every committed exact accept on this built-in verifier path has a reusable
  // trim and no compatibility rejection.  The compacted output's independent
  // rebuild must still match the reported final score.
  CHECK(search.counters.local_commit_accepted_moves >= 1);
  CHECK(search.counters.accepted_exact_trims_reused ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.accepted_exact_trim_reuse_rejections == 0);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// Multi-worker local-commit run: the epoch/snapshot barrier keeps scoring
// readers from observing a partially-updated cache.  This is the TSAN-
// load-bearing case (verified separately under -DENABLE_TSAN=ON).  Here we
// confirm the multi-worker run produces the same accepted-move count and final
// score as a serial run, so the barrier does not change semantics.  Uses the
// tiny fixture (see the note on test_phase4_exact_trim_cache_never_stale for
// why the pandemic-scale fixture is avoided).
static void test_phase4_multi_worker_matches_serial() {
  std::println("test_phase4_multi_worker_matches_serial");

  auto run_once = [](std::size_t workers) {
    auto fixture = make_three_misplaced_groups_fixture();
    larch::chart_spr_search_options options;
    options.acceptance_mode =
        larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::exhaustive_exact;
    options.max_iterations = 12;
    options.rebuild_after_accept = false;
    options.worker_count = workers;
    options.cache.candidate_batch_size = 128;
    options.semantic_capture =
        larch::chart_spr_semantic_capture_mode::digest;
    options.verify_local_commit_two_chart_oracle_for_tests = true;
    return larch::run_chart_spr_search(std::move(fixture.dag),
                                       fixture.grammar, options);
  };

  auto const live_before =
      larch::chart_scheduler::global_live_pool_threads();
  auto serial = run_once(1);
  CHECK(larch::chart_scheduler::global_live_pool_threads() == live_before);
  auto parallel = run_once(8);
  CHECK(larch::chart_scheduler::global_live_pool_threads() == live_before);

  CHECK(serial.counters.accepted_moves == parallel.counters.accepted_moves);
  CHECK(serial.summary.final_score == parallel.summary.final_score);
  CHECK(serial.summary.initial_score == parallel.summary.initial_score);
  CHECK(parallel.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(parallel.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(serial.canonical_digest.has_value());
  CHECK(parallel.canonical_digest.has_value());
  CHECK(larch::emit_chart_spr_semantic_digest_json(*serial.canonical_digest) ==
        larch::emit_chart_spr_semantic_digest_json(
            *parallel.canonical_digest));
  CHECK(serial.iterations.size() > 1);
  CHECK(serial.summary.scheduler.operations > 1);
  // Phase 6 deliberately changes the scheduler decomposition at W>1: retained
  // candidates run on the exact-candidate axis, whereas W1 preserves the
  // inner exact-setup/frontier axes.  Total operation counts therefore need
  // not match even though the canonical result must.
  CHECK(serial.summary.scheduler_axes.exact_candidates.operations == 0);
  CHECK(parallel.summary.scheduler_axes.exact_candidates.operations > 0);
  CHECK(parallel.summary.scheduler_axes.exact_candidates
            .parallel_operations > 0);
  CHECK(serial.summary.scheduler.requested_workers == 1);
  CHECK(serial.summary.scheduler.resolved_workers == 1);
  CHECK(serial.summary.scheduler.parallel_operations == 0);
  CHECK(serial.summary.scheduler.tasks_submitted == 0);
  CHECK(serial.summary.scheduler.pool_lifetimes == 0);
  CHECK(serial.summary.scheduler.pool_lifetimes_stopped == 0);
  CHECK(parallel.summary.scheduler.requested_workers == 8);
  CHECK(parallel.summary.scheduler.resolved_workers == 8);
  CHECK(parallel.summary.scheduler.parallel_operations > 0);
  CHECK(parallel.summary.scheduler.pool_lifetimes == 1);
  CHECK(parallel.summary.scheduler.pool_lifetimes_stopped == 1);
  for (auto const* scheduler : {&serial.summary.scheduler,
                                &parallel.summary.scheduler}) {
    CHECK(scheduler->operations ==
          scheduler->parallel_operations + scheduler->serial_fallbacks);
    CHECK(scheduler->ranges_created ==
          scheduler->ranges_completed + scheduler->ranges_cancelled);
    CHECK(scheduler->ranges_cancelled == 0);
    CHECK(scheduler->tasks_submitted == scheduler->tasks_completed);
    CHECK(scheduler->tasks_submitted == scheduler->tasks_joined);
    CHECK(scheduler->tasks_submitted == scheduler->queue_wait_samples);
    CHECK(scheduler->pending_tasks == 0);
    CHECK(scheduler->pending_tasks_at_shutdown == 0);
    CHECK(scheduler->live_pool_threads == 0);
    CHECK(scheduler->shutdown);
  }
  check_phase4_scheduler_axis_reconciliation(serial);
  check_phase4_scheduler_axis_reconciliation(parallel);
  CHECK(parallel.summary.scheduler_axes.initial_chart_patterns.operations > 0);
  CHECK(parallel.summary.scheduler_axes.exact_setup_patterns.operations > 0);
  CHECK(parallel.summary.scheduler_axes.inside_cache_patterns.operations > 0);
  CHECK(parallel.summary.scheduler_axes.outside_cache_patterns.operations > 0);
  CHECK(parallel.summary.scheduler_axes.local_score_candidates.operations +
            parallel.summary.scheduler_axes.local_score_candidate_patterns
                .operations >
        0);
  CHECK(parallel.summary.scheduler_axes.other.operations == 0);
  // Parallel scoring actually used the workers.
  CHECK(parallel.counters.local_score_parallel_batches > 0);

  std::println("  PASS");
}

// fixed_topology_exact + local commit.  This is the second exact gate that
// may commit locally (Work item 1 exactness contract).  Phase 8 serves this
// gate without hidden per-candidate materialization or a hidden full direct
// selected-tree oracle: selected-subtree cache work is visible in its own
// counters, and the only full materialization in this local-commit run is
// Phase 5's final compaction.
static void test_phase4_fixed_topology_exact_local_commit() {
  std::println("test_phase4_fixed_topology_exact_local_commit");

  auto fixture = make_three_misplaced_groups_fixture();

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  // exhaustive_exact verifies every candidate through the fixed-topology gate
  // (default selector resolves a topology certificate for each), so this run
  // exercises the Phase 8 no-materialization verification path repeatedly.
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.worker_count = 4;
  options.cache.memory_budget_bytes = std::size_t{1} << 30;
  // Isolate the fixed-topology/local-commit contract from the independent
  // Phase-8 finite candidate-generation pipeline envelope.
  options.enable_candidate_generation_pipeline = false;
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit_accepted={}), "
      "tombstone_scope_skips={}, exact_verifications={}, "
      "full_overlay_materializations={}, accept_materializations={}, "
      "exact_verification_materializations={}, "
      "selected_cache_hits={}, selected_cache_misses={}, "
      "selected_rows_computed={}, persistent_verifications={}, "
      "persistent_fallbacks={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.local_commit_tombstone_scope_skips,
      search.counters.exact_verifications,
      search.counters.full_overlay_materializations,
      search.counters.overlay_materializations_for_accept_materialization,
      search.counters.overlay_materializations_for_exact_verification,
      search.counters.fixed_topology_selected_cache_hits,
      search.counters.fixed_topology_selected_cache_misses,
      search.counters.fixed_topology_selected_rows_computed,
      search.counters.fixed_topology_persistent_cache_verifications,
      search.counters.fixed_topology_persistent_cache_fallbacks);

  // Every accepted move is fixed_topology_exact-gated (an exact gate; the
  // chain's recorded objective is exact for the one selected topology).
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);
  for (auto const& it : search.iterations) {
    if (it.accepted_move_committed) {
      CHECK(it.accepted.has_value());
      CHECK(it.accepted->exact.has_value());
      CHECK(it.accepted->exact->kind ==
            larch::chart_spr_score_kind::fixed_topology_exact);
    }
  }

  // Counter contract (corrected, per the known-issue note): no per-accept
  // sidecar rebuilds and no per-accept dense accept-materializations.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  // Phase 8 counter contract: fixed-topology verification itself performs no
  // dense overlay materialization.  The umbrella counter is exactly the single
  // final-compaction materialization added after the local-commit run.
  CHECK(search.counters.overlay_materializations_for_exact_verification == 0);
  CHECK(search.counters.full_overlay_materializations ==
        search.counters.overlay_materializations_for_final_compaction);
  CHECK(search.counters.fixed_topology_selected_cache_hits > 0);
  CHECK(search.counters.fixed_topology_selected_cache_misses > 0);
  CHECK(search.counters.fixed_topology_selected_rows_computed > 0);
  CHECK(search.counters.fixed_topology_persistent_cache_verifications ==
        search.counters.exact_verifications);
  CHECK(search.counters.fixed_topology_persistent_cache_fallbacks == 0);
  CHECK(search.counters.fixed_topology_persistent_cache_oracle_mismatches == 0);
  check_phase4_scheduler_axis_reconciliation(search);
  // Phase 6 selects the outer candidate axis when multiple fixed-topology
  // verifications are retained; those tasks deliberately keep their inner
  // per-pattern recurrence serial to avoid nested waits on the same pool.
  CHECK(search.summary.scheduler_axes.exact_candidates.operations > 0);
  CHECK(search.summary.scheduler_axes.exact_candidates.parallel_operations >
        0);
  CHECK(search.summary.fixed_topology_selected_cache_hits ==
        search.counters.fixed_topology_selected_cache_hits);
  CHECK(search.summary.fixed_topology_selected_cache_misses ==
        search.counters.fixed_topology_selected_cache_misses);
  CHECK(search.summary.fixed_topology_selected_rows_computed ==
        search.counters.fixed_topology_selected_rows_computed);
  CHECK(search.summary.fixed_topology_persistent_cache_verifications ==
        search.counters.fixed_topology_persistent_cache_verifications);
  CHECK(search.summary.fixed_topology_persistent_cache_fallbacks == 0);
  // The selected-topology map is deliberately cleared at each candidate
  // boundary to make retention bounded.  Every miss publishes one full
  // active-pattern row vector, while hits now prove reuse only within that
  // candidate (not unbounded cross-candidate retention).
  CHECK(search.counters.fixed_topology_selected_rows_computed ==
        search.counters.fixed_topology_selected_cache_misses *
            search.summary.active_pattern_count);
  CHECK(search.summary.chart_cache_resident_bytes <=
        options.cache.memory_budget_bytes);
  // The caches did real affected-set-scoped work and the two-chart oracle ran
  // after every commit without throwing.
  if (search.counters.accepted_moves > 0) {
    CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
    CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
    CHECK(search.counters.local_commit_two_chart_oracle_runs ==
          search.counters.local_commit_accepted_moves);
    CHECK(search.counters.local_commit_tip_grammar_refreshes ==
          search.counters.local_commit_accepted_moves);
  }

  // Three disjoint committable improving moves: fixed_topology_exact commits
  // the same k >= 3 the exact_multisite path does.
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  CHECK(search.summary.final_score <= search.summary.initial_score);

  // Compaction produced a valid DAG whose grammar-level exact B&B optimum
  // matches the reported final score, and preserves every accepted fixed-
  // topology certificate as a complete witness topology.
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  CHECK(search.counters.overlay_materializations_for_final_compaction == 1);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  std::size_t accepted_certificate_count = 0;
  for (auto const& it : search.iterations) {
    if (!it.accepted_move_committed) continue;
    CHECK(it.accepted.has_value());
    CHECK(it.accepted->topology_selection.certificate.has_value());
    phase5_assert_certificate_topology_present(
        rebuilt, *it.accepted->topology_selection.certificate);
    ++accepted_certificate_count;
  }
  CHECK(accepted_certificate_count ==
        search.counters.local_commit_accepted_moves);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

static void test_lazy_cache_local_commit_sequence_projects_lazy_chart() {
  std::println("test_lazy_cache_local_commit_sequence_projects_lazy_chart");

  auto fixture = make_three_misplaced_groups_fixture();

  larch::chart_spr_search_options options;
  options.acceptance_mode =
      larch::chart_spr_acceptance_mode::fixed_topology_exact;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.cache.use_lazy_multisite_chart = true;
  options.verify_local_commit_two_chart_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  CHECK(search.summary.cache_strategy ==
        larch::chart_spr_cache_strategy::lazy_multisite_chart);
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.local_commit_two_chart_oracle_runs ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.local_commit_tip_grammar_refreshes ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_inside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_outside_rows_recomputed_on_commit > 0);
  CHECK(search.counters.lazy_incremental_rows_recomputed ==
        search.counters.lazy_inside_rows_recomputed_on_commit +
            search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_inside_rows_recomputed_on_commit ==
        search.counters.lazy_inside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_outside_rows_recomputed_on_commit ==
        search.counters.lazy_outside_rows_recomputed_on_commit);
  CHECK(search.summary.lazy_incremental_rows_recomputed ==
        search.counters.lazy_incremental_rows_recomputed);
  CHECK(search.summary.lazy_inside_rows_computed > 0);
  CHECK(search.summary.lazy_merge_ratio > 0.0);

  std::println("  PASS");
}

static larch::option_c_after_subtree phase4_reactivation_leaf(
    std::vector<larch::taxon_id> taxa) {
  return {std::move(taxa), {}};
}

static larch::option_c_after_subtree phase4_reactivation_pair(
    std::vector<larch::taxon_id> taxa, larch::option_c_after_subtree left,
    larch::option_c_after_subtree right) {
  return {std::move(taxa), {std::move(left), std::move(right)}};
}

static larch::rank3_production_taxa_key phase4_reactivation_key(
    std::vector<larch::taxon_id> parent,
    std::vector<std::vector<larch::taxon_id>> children) {
  larch::rank3_production_taxa_key result{std::move(parent),
                                          std::move(children)};
  larch::rank3_detail::normalize_production_key(result);
  return result;
}

static void test_lazy_local_commit_recomputes_reactivated_clades() {
  std::println("test_lazy_local_commit_recomputes_reactivated_clades");

  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto a_leaf = [] { return tiny_leaf("A", "A"); };
  auto b_leaf = [] { return tiny_leaf("B", "A"); };
  auto c_leaf = [] { return tiny_leaf("C", "C"); };
  auto d_leaf = [] { return tiny_leaf("D", "C"); };
  auto tree_ab = tiny_inner(
      "R", "A",
      {tiny_inner("ABC", "A",
                  {tiny_inner("AB", "A", {a_leaf(), b_leaf()}), c_leaf()}),
       d_leaf()});
  auto tree_ac = tiny_inner(
      "R", "A",
      {tiny_inner("ABC", "A",
                  {tiny_inner("AC", "A", {a_leaf(), c_leaf()}), b_leaf()}),
       d_leaf()});
  auto tree_ad_bc = tiny_inner("R", "A",
                               {tiny_inner("AD", "A", {a_leaf(), d_leaf()}),
                                tiny_inner("BC", "A", {b_leaf(), c_leaf()})});

  std::vector<larch::phylo_dag> trees;
  trees.push_back(larch::test::make_tiny_labelled_tree("A", tree_ab));
  trees.push_back(larch::test::make_tiny_labelled_tree("A", tree_ac));
  trees.push_back(larch::test::make_tiny_labelled_tree("A", tree_ad_bc));
  auto dag = larch::test::merge_tiny_trees(std::move(trees));
  auto base = larch::build_clade_grammar(dag);

  auto a = taxa_for(base, {"A"});
  auto b = taxa_for(base, {"B"});
  auto c = taxa_for(base, {"C"});
  auto d = taxa_for(base, {"D"});
  auto ab = taxa_for(base, {"A", "B"});
  auto ac = taxa_for(base, {"A", "C"});
  auto ad = taxa_for(base, {"A", "D"});
  auto bc = taxa_for(base, {"B", "C"});
  auto abc = taxa_for(base, {"A", "B", "C"});
  auto abcd = taxa_for(base, {"A", "B", "C", "D"});

  larch::overlay_chain chain(base);
  larch::option_c_after_production select_ad_bc;
  select_ad_bc.parent_taxa = abcd;
  select_ad_bc.children.push_back(phase4_reactivation_pair(
      ad, phase4_reactivation_leaf(a), phase4_reactivation_leaf(d)));
  select_ad_bc.children.push_back(phase4_reactivation_pair(
      bc, phase4_reactivation_leaf(b), phase4_reactivation_leaf(c)));
  auto first = larch::option_c_as_overlay_delta(
      base, phase4_reactivation_key(abcd, {abc, d}), select_ad_bc);
  chain.append(first.delta);
  auto tip_one = larch::materialize_overlay_chain(chain);

  larch::chart_spr_search_options state_options;
  state_options.cache.use_lazy_multisite_chart = true;
  auto state =
      larch::build_chart_spr_search_state(dag, tip_one.grammar, state_options);
  CHECK(state.lazy_chart.has_value());

  larch::option_c_after_production select_ab_c;
  select_ab_c.parent_taxa = abcd;
  select_ab_c.children.push_back(larch::option_c_after_subtree{
      abc,
      {phase4_reactivation_pair(ab, phase4_reactivation_leaf(a),
                                phase4_reactivation_leaf(b)),
       phase4_reactivation_leaf(c)}});
  select_ab_c.children.push_back(phase4_reactivation_leaf(d));
  auto second = larch::option_c_as_overlay_delta(
      tip_one.grammar, phase4_reactivation_key(abcd, {ad, bc}), select_ab_c);
  chain.append(second.delta);
  auto tip_two = larch::materialize_overlay_chain(chain);
  auto plan_two = larch::build_chart_execution_plan(tip_two.grammar);

  auto ac_ref = larch::base_clade_ref(clade_for(base, {"A", "C"}));
  CHECK(std::find(tip_one.dense_clade_to_ref.begin(),
                  tip_one.dense_clade_to_ref.end(),
                  ac_ref) == tip_one.dense_clade_to_ref.end());
  CHECK(std::find(tip_two.dense_clade_to_ref.begin(),
                  tip_two.dense_clade_to_ref.end(),
                  ac_ref) != tip_two.dense_clade_to_ref.end());
  auto ordinary_inside_affected =
      larch::compute_chain_inside_affected_set(chain);
  CHECK(std::find(ordinary_inside_affected.begin(),
                  ordinary_inside_affected.end(),
                  ac_ref) == ordinary_inside_affected.end());

  larch::refresh_chart_spr_lazy_chart_after_local_commit_for_tests(
      state, chain, tip_two, plan_two, tip_one.dense_clade_to_ref);

  larch::lazy_chart_options lazy_options;
  lazy_options.chart = state.chart_opts;
  lazy_options.retain_all_inside_class_maps = true;
  auto oracle = larch::build_lazy_inside_chart_active(
      plan_two, state.active_patterns, lazy_options);
  larch::build_lazy_outside_chart_in_place(
      plan_two, state.active_patterns.patterns, oracle, state.chart_opts);
  CHECK(state.lazy_chart->pattern_count == oracle.pattern_count);
  for (std::size_t dense = 0; dense < tip_two.grammar.clades.size(); ++dense) {
    for (std::size_t pattern = 0; pattern < oracle.pattern_count; ++pattern) {
      CHECK(state.lazy_chart->inside_row(dense, pattern) ==
            oracle.inside_row(dense, pattern));
      CHECK(state.lazy_chart->outside_row(dense, pattern) ==
            oracle.outside_row(dense, pattern));
    }
  }
  CHECK(state.lazy_chart->outside_global_min_by_pattern ==
        oracle.outside_global_min_by_pattern);

  std::println("  PASS");
}

// pattern_batches cache strategy + local commit (Phase 4 known-issue #3).
// Non-lazy local commit now scores through the authoritative persistent inside
// cache even when the published policy label is pattern_batches. This forces
// cache_strategy = pattern_batches
// (max_cached_patterns = 1 on the three-misplaced-groups fixture, which carries
// several active patterns) through a local-commit run so that branch is
// exercised end-to-end and the two-chart oracle still agrees.
static void test_phase4_pattern_batches_local_commit() {
  std::println("test_phase4_pattern_batches_local_commit");

  auto fixture = make_three_misplaced_groups_fixture();

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.worker_count = 4;
  options.verify_local_commit_two_chart_oracle_for_tests = true;
  // Force pattern_batches: cap the resident pattern cache below the fixture's
  // active-pattern count so choose_chart_spr_cache_strategy selects batching.
  options.cache.max_cached_patterns = 1;

  // Confirm the configured options actually select pattern_batches for this
  // fixture (the local-commit refresh branch under test is gated on it).
  auto probe = larch::build_chart_spr_search_state(fixture.dag, fixture.grammar,
                                                    options);
  CHECK(probe.cache_strategy == larch::chart_spr_cache_strategy::pattern_batches);
  CHECK(probe.pattern_charts.empty());
  std::println("  active_patterns={}, cache_strategy=pattern_batches forced",
               probe.active_patterns.patterns.patterns.size());

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit_accepted={}), "
      "tombstone_scope_skips={}, pattern_batch_cache_builds={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.local_commit_tombstone_scope_skips,
      search.counters.pattern_batch_cache_builds);

  // The local-commit run completed (no throw) with at least one commit and the
  // next scoring batch read the refreshed persistent rows directly.
  CHECK(search.counters.accepted_moves ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.local_commit_accepted_moves >= 1);
  // Counter contract holds under pattern_batches exactly as under
  // all_active_patterns.
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  // Two-chart oracle ran after every commit without throwing (the caches are
  // fully populated regardless of cache_strategy; strategy only governs whether
  // pattern_charts is resident).
  CHECK(search.counters.local_commit_two_chart_oracle_runs ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.summary.active_pattern_count > 1);
  CHECK(search.counters.outside_cache_inside_charts_built == 0);
  CHECK(search.counters.outside_cache_inside_charts_reused ==
        search.summary.active_pattern_count);
  CHECK(search.counters.outside_cache_outside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.counters.initial_state_inside_charts_built == 0);
  CHECK(search.counters.inside_cache_inside_charts_built ==
        search.summary.active_pattern_count);
  CHECK(search.counters.inside_cache_resident_inside_charts_consumed == 0);
  CHECK(search.counters.exact_setup_builds > 0);
  CHECK(search.counters.exact_setup_resident_inside_charts_consumed >=
        search.summary.active_pattern_count);
  check_phase4_scheduler_axis_reconciliation(search);
  CHECK(search.summary.scheduler_axes.inside_cache_patterns.operations > 0);
  CHECK(search.summary.scheduler_axes.outside_cache_patterns.operations > 0);
  CHECK(search.summary.scheduler_axes.exact_setup_patterns.operations > 0);
  CHECK(search.summary.scheduler_axes.local_score_candidates.operations > 0);
  // No cold pattern batch is rebuilt: the cache-backed view is the sole dense
  // row source for current-tip local scoring.
  CHECK(search.counters.pattern_batch_cache_builds == 0);
  CHECK(search.counters.local_commit_inside_row_view_pattern_visits > 0);
  CHECK(search.summary.local_commit_inside_row_view_pattern_visits ==
        search.counters.local_commit_inside_row_view_pattern_visits);
  CHECK(search.summary.final_score <= search.summary.initial_score);

  // Compaction produced a valid DAG whose grammar-level exact B&B optimum
  // matches the reported final score.
  CHECK(search.summary.final_compaction_rebuilds == 1);
  CHECK(search.summary.final_compaction_exactness_kind ==
        larch::multisite_keep_mask_kind::exact_optimal_production_union);
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  // Pattern batching no longer owns a separate scoring batch in local mode,
  // but the complete persistent inside/outside pair remains mandatory. A
  // budget too small for that pair is rejected explicitly.
  auto budget_fixture = make_three_misplaced_groups_fixture();
  auto budget_options = options;
  // This subcase isolates the mandatory persistent-cache admission gate; its
  // finite budget is not a candidate-generation pipeline boundary.
  budget_options.enable_candidate_generation_pipeline = false;
  auto budget_calibration_state = larch::build_chart_spr_search_state(
      budget_fixture.dag, budget_fixture.grammar, budget_options);
  auto const mandatory_local_commit_bytes =
      larch::chart_spr_exact_candidate_checked_bytes_add(
          larch::estimate_chart_spr_state_core_resident_bytes(
              budget_calibration_state),
          2 * larch::estimate_chart_spr_full_pattern_cache_bytes(
                  budget_calibration_state),
          "pattern-batch local-commit mandatory state test");
  CHECK(mandatory_local_commit_bytes > 1);
  budget_options.cache.memory_budget_bytes = mandatory_local_commit_bytes - 1;
  bool budget_rejected = false;
  try {
    (void)larch::run_chart_spr_search(std::move(budget_fixture.dag),
                                      budget_fixture.grammar, budget_options);
  } catch (std::runtime_error const& error) {
    budget_rejected = true;
    CHECK(std::string{error.what()}.find(
              "mandatory full inside/outside caches") != std::string::npos);
  }
  CHECK(budget_rejected);

  std::println("  PASS");
}

// ===========================================================================
// Phase 9 (Work item 4a, technique 2): transient chain extension for
// grammar-exact verification.
//
// The exact_multisite gate verifies an unaccepted candidate by transiently
// extending the overlay chain in reader-local scratch storage (never mutating
// shared state), reading the exact frontier on the extended grammar, and
// discarding.  Inside/outside scratch caches are constructed only for the
// opt-in two-chart diagnostic.  These tests cover the four Phase 9 exit
// criteria plus Phase-2B's dead-work gate:
//   1. transient-extension exact score == from-scratch exact score (oracle).
//   2. full_overlay_materializations does not increase for a transient run;
//      work counted under transient_chain_extensions_for_verification.
//   3. on oracle mismatch, the cold from-scratch result is authoritative (the
//      corruption hook forces a mismatch and asserts the fallback fires); the
//      exactness label stays grammar_exact because the cold B&B is exact (a
//      justified deviation from the plan's literal "weakened" wording -- see
//      doc/WRIC-SPR-SEARCH.md).
//   4. TSAN-clean multi-worker (transient extensions are reader-local).
//
// Production B&B still rebuilds its exact setup from the materialized grammar,
// but it no longer pays to copy/advance caches that only the diagnostic reads.
// ============================================================================

// Exit criterion 2: a local-commit exact_multisite run that uses the
// transient path does NOT bump full_overlay_materializations for verification
// (only the single final-compaction materialization, plus any cold-path
// fallbacks for tombstone-scope candidates); the per-candidate work is counted
// under transient_chain_extensions_for_verification, never folded into
// full_overlay_materializations.
static void test_phase9_transient_no_full_overlay_materialization() {
  std::println("test_phase9_transient_no_full_overlay_materialization");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  // Oracle OFF: the production path trusts the transient result and never
  // materializes per candidate.  (With the oracle ON, each verified candidate
  // materializes a cold overlay for cross-checking -- the no-materialization
  // contract is asserted with the oracle OFF.)
  options.verify_transient_chain_extension_oracle_for_tests = false;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  accepted_moves={} (local_commit={}), exact_verifications={}, "
      "transient_extensions={}, diagnostic_cache_extensions={}, "
      "exact_verification_materializations={}, "
      "full_overlay_materializations={}, final_compaction_materializations={}",
      search.counters.accepted_moves,
      search.counters.local_commit_accepted_moves,
      search.counters.exact_verifications,
      search.counters.transient_chain_extensions_for_verification,
      search.counters.transient_chain_diagnostic_cache_extensions,
      search.counters.overlay_materializations_for_exact_verification,
      search.counters.full_overlay_materializations,
      search.counters.overlay_materializations_for_final_compaction);

  // The transient path was actually used.
  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  // Production exact B&B never consumes the persistent row caches, so no
  // diagnostic cache snapshot may be copied or advanced with the oracle off.
  CHECK(search.counters.transient_chain_diagnostic_cache_extensions == 0);
  CHECK(search.summary.transient_chain_diagnostic_cache_extensions == 0);
  // Every exact verification either used the transient path or fell back to
  // the cold path (tombstone-scope candidates whose delta cannot be appended
  // to the chain).  Cold fallbacks are the only source of per-candidate exact-
  // verification materializations.
  CHECK(search.counters.exact_verifications ==
        search.counters.transient_chain_extensions_for_verification +
            search.counters.overlay_materializations_for_exact_verification);
  // The umbrella full_overlay_materializations counter is exactly the sum of
  // its reason-coded splits for this run: no per-accept materialization
  // (local-commit contract), no oracle materialization (oracle off), the
  // single final compaction, and any cold-fallback exact verifications.
  CHECK(search.counters.full_overlay_materializations ==
        search.counters.overlay_materializations_for_final_compaction +
            search.counters.overlay_materializations_for_exact_verification);
  CHECK(search.counters.overlay_materializations_for_accept_materialization ==
        0);
  CHECK(search.counters.sidecar_rebuilds_after_accept == 0);
  // Every accepted move is grammar_exact (the transient path preserves the
  // exactness label).
  for (auto const& it : search.iterations) {
    if (it.accepted_move_committed) {
      CHECK(it.accepted.has_value());
      CHECK(it.accepted->exact.has_value());
      CHECK(it.accepted->exact->kind ==
            larch::chart_spr_score_kind::grammar_exact);
    }
  }
  // k >= 3 disjoint committable improving moves on this fixture.
  CHECK(search.counters.local_commit_accepted_moves >= 3);
  CHECK(search.counters.accepted_exact_trims_reused ==
        search.counters.local_commit_accepted_moves);
  CHECK(search.counters.accepted_exact_trim_reuse_rejections == 0);
  CHECK(search.summary.final_score <= search.summary.initial_score);

  std::println("  PASS");
}

// Exit criterion 1: the transient-extension exact score equals the from-
// scratch exact score on every fixture candidate, both charts.  The oracle
// (enabled) recomputes BOTH charts from scratch on the extended grammar and
// compares against the scratch caches, plus runs a cold from-scratch B&B and
// compares the exact optimum.  A green oracle (zero mismatches, zero
// fallbacks) proves the transient result is exact.
static void test_phase9_transient_oracle_both_charts_green() {
  std::println("test_phase9_transient_oracle_both_charts_green");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  // Oracle ON: every transient extension is cross-checked against the cold
  // from-scratch path (both charts + exact optimum), per Work item 4a's
  // correctness invariant.
  options.verify_transient_chain_extension_oracle_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  transient_extensions={}, oracle_rows_checked={}, "
      "oracle_mismatches={}, fallbacks={}",
      search.counters.transient_chain_extensions_for_verification,
      search.counters.transient_chain_extension_oracle_rows_checked_for_tests,
      search.counters.transient_chain_extension_oracle_mismatches,
      search.counters.transient_chain_extension_fallbacks);

  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  CHECK(search.counters.transient_chain_diagnostic_cache_extensions ==
        search.counters.transient_chain_extensions_for_verification);
  CHECK(search.summary.transient_chain_diagnostic_cache_extensions ==
        search.counters.transient_chain_diagnostic_cache_extensions);
  // The two-chart oracle ran on every transient extension: it checked both
  // the inside and outside scratch rows against the from-scratch charts.
  CHECK(search.counters.transient_chain_extension_oracle_rows_checked_for_tests >
        0);
  // Green oracle: no mismatches and no fallbacks on the fixture matrix.
  CHECK(search.counters.transient_chain_extension_oracle_mismatches == 0);
  CHECK(search.counters.transient_chain_extension_fallbacks == 0);
  // The oracle materializations are counted under the oracle bucket, separate
  // from the transient extension counter (never folded).
  CHECK(search.counters.overlay_materializations_for_oracle >=
        search.counters.transient_chain_extensions_for_verification);
  // Final score matches a from-scratch rebuild of the output DAG.
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// Exit criterion 1 on a multi-parent DAG fixture: the transient oracle stays
// green across a grammar with multiple productions per parent (the case the
// B&B coupling is designed for).
static void test_phase9_transient_oracle_green_on_multiparent_dag() {
  std::println("test_phase9_transient_oracle_green_on_multiparent_dag");

  auto run_once = [](std::size_t workers) {
    std::vector<larch::phylo_dag> trees;
    trees.push_back(larch::test::make_tiny_labelled_tree(
        "A", five_taxon_multiparent_tree_one()));
    trees.push_back(larch::test::make_tiny_labelled_tree(
        "A", five_taxon_multiparent_tree_two()));
    auto dag = larch::test::merge_tiny_trees(std::move(trees));
    auto grammar = larch::build_clade_grammar(dag);

    // Non-vacuous structural witness: the shared AB clade occurs below at
    // least two distinct parents in the merged DAG.
    auto ab = clade_for(grammar, {"A", "B"});
    CHECK(grammar.productions_by_child[ab].size() >= 2);

    larch::chart_spr_search_options options;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::exhaustive_exact;
    options.max_iterations = 1;
    options.rebuild_after_accept = false;
    options.worker_count = workers;
    options.local_score_worker_count = workers;
    options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
    options.verify_transient_chain_extension_oracle_for_tests = true;

    return larch::run_chart_spr_search(std::move(dag), grammar, options);
  };

  auto check_run = [](larch::chart_spr_search_result const& search,
                      std::size_t workers) {
    CHECK(search.summary.requested_worker_count == workers);
    CHECK(search.summary.resolved_worker_count == workers);
    CHECK(search.summary.local_score_worker_count == workers);
    CHECK(search.counters.exact_verifications > 0);
    CHECK(!search.iterations.empty());
    CHECK(search.iterations.front().candidates_exact_verified > 0);
    CHECK(search.counters.transient_chain_extensions_for_verification > 0);
    CHECK(search.counters.transient_chain_diagnostic_cache_extensions ==
          search.counters.transient_chain_extensions_for_verification);
    CHECK(search.counters.transient_chain_extension_oracle_rows_checked_for_tests >
          0);
    CHECK(search.counters.transient_chain_extension_oracle_mismatches == 0);
    CHECK(search.counters.transient_chain_extension_fallbacks == 0);
    CHECK(search.counters.overlay_materializations_for_oracle >=
          search.counters.transient_chain_extensions_for_verification);
    CHECK(search.canonical_report.has_value());
    CHECK(search.canonical_digest.has_value());
    CHECK(!search.canonical_digest->full_sidecar.empty());
  };

  auto serial = run_once(1);
  auto parallel = run_once(8);
  check_run(serial, 1);
  check_run(parallel, 8);

  CHECK(serial.summary.initial_score == parallel.summary.initial_score);
  CHECK(serial.summary.final_score == parallel.summary.final_score);
  CHECK(serial.counters.accepted_moves == parallel.counters.accepted_moves);
  CHECK(serial.iterations.size() == parallel.iterations.size());
  CHECK(larch::emit_chart_spr_semantic_digest_json(*serial.canonical_digest) ==
        larch::emit_chart_spr_semantic_digest_json(
            *parallel.canonical_digest));
  CHECK(serial.canonical_digest->full_sidecar ==
        parallel.canonical_digest->full_sidecar);

  std::println("  PASS");
}

// Exit criterion 3: when the transient result cannot be trusted (the oracle
// finds a mismatch), the verifier uses the cold from-scratch path's result and
// records the fallback.  The corruption hook perturbs a scratch outside row so
// the two-chart oracle deterministically disagrees; the gate must fire rather
// than silently trusting the wrong transient result.
static void test_phase9_transient_oracle_catches_corruption() {
  std::println("test_phase9_transient_oracle_catches_corruption");

  auto fixture = make_three_misplaced_groups_fixture();
  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::exhaustive_exact;
  options.max_iterations = 12;
  options.rebuild_after_accept = false;
  options.verify_transient_chain_extension_oracle_for_tests = true;
  // Perturb a scratch outside row so the per-candidate two-chart oracle
  // catches the disagreement.
  options.force_transient_chain_extension_oracle_mismatch_for_tests = true;

  auto search = larch::run_chart_spr_search(std::move(fixture.dag),
                                            fixture.grammar, options);

  std::println(
      "  transient_extensions={}, oracle_mismatches={}, fallbacks={}",
      search.counters.transient_chain_extensions_for_verification,
      search.counters.transient_chain_extension_oracle_mismatches,
      search.counters.transient_chain_extension_fallbacks);

  CHECK(search.counters.transient_chain_extensions_for_verification > 0);
  CHECK(search.counters.transient_chain_diagnostic_cache_extensions ==
        search.counters.transient_chain_extensions_for_verification);
  // The corruption hook forces at least one mismatch, and every mismatch
  // triggers a fallback to the authoritative cold result.
  CHECK(search.counters.transient_chain_extension_oracle_mismatches > 0);
  CHECK(search.counters.transient_chain_extension_fallbacks ==
        search.counters.transient_chain_extension_oracle_mismatches);
  // A diagnostic mismatch invalidates the transient frontier even if its
  // scalar optimum happens to equal the cold oracle.  Such a payload is never
  // published as the next state's exact trim.
  CHECK(search.counters.accepted_exact_trims_reused == 0);
  CHECK(search.counters.accepted_exact_trim_reuse_rejections == 0);
  // Despite the forced corruption, the final score still matches a from-
  // scratch rebuild of the output DAG: the cold path's authoritative result
  // keeps the reported objective correct.
  auto rebuilt = larch::build_clade_grammar(search.dag);
  auto rebuilt_state = larch::build_chart_spr_search_state(
      search.dag, rebuilt, options);
  CHECK(larch::chart_spr_state_exact_score_with_invariants(
            rebuilt_state, options.exact_trim) == search.summary.final_score);

  std::println("  PASS");
}

// A candidate whose delta cannot be appended to the chain (tombstone scope:
// it tombstones a production that does not resolve to a frozen-base
// production) falls back to the cold path during verification, so it can still
// be accepted and reach the Phase 4 commit-time tombstone-scope skip.  This
// preserves the existing search semantics: the transient extension never
// silently rejects a candidate the cold path would accept.
static void test_phase9_transient_tombstone_scope_falls_back_to_cold() {
  std::println("test_phase9_transient_tombstone_scope_falls_back_to_cold");

  auto dag = larch::test::make_tiny_labelled_tree(
      "A", six_taxon_paired_misplaced_tree());
  auto grammar = larch::build_clade_grammar(dag);

  larch::chart_spr_search_options options;
  options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
  options.candidate_selection =
      larch::chart_spr_candidate_selection_mode::lower_bound_top_k;
  options.top_k_exact_verify = 32;
  options.max_iterations = 3;
  options.rebuild_after_accept = false;
  options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;

  auto search = larch::run_chart_spr_search(std::move(dag), grammar, options);

  // Exactly one improving move is committed before the deterministic
  // tombstone-scope abort.
  CHECK(search.counters.accepted_moves == 1);
  // After the first accept, a sequential SPR whose tombstones do not resolve
  // to frozen-base productions falls back to the cold path for verification
  // and then reaches the commit-time tombstone-scope skip.  On this small
  // single-topology fixture the search commits one move and stops at the
  // labelled skip.
  CHECK(search.counters.local_commit_tombstone_scope_skips == 1);
  CHECK(search.iterations.size() == 2);
  auto const& skipped = search.iterations.back();
  CHECK(skipped.accepted.has_value());
  CHECK(!skipped.accepted_move_committed);
  CHECK(skipped.post_materialization_rejected);
  CHECK(skipped.no_accept_reason.find("tombstone-scope") != std::string::npos);
  CHECK(skipped.accepted_inside_rows_recomputed == 0);
  CHECK(skipped.accepted_outside_rows_recomputed == 0);
  CHECK(skipped.accepted_candidate_signature.empty());
  CHECK(search.canonical_report.has_value());
  CHECK(search.canonical_report->iterations.size() == search.iterations.size());
  auto const& canonical_skipped = search.canonical_report->iterations.back();
  CHECK(canonical_skipped.accepted_move_present);
  CHECK(!canonical_skipped.accepted_move_committed);
  CHECK(canonical_skipped.selected_stream_index.has_value());
  CHECK(!canonical_skipped.selected_signature.empty());

  std::println("  PASS");
}

// Exit criterion 4: transient extensions are reader-local (the chain and, for
// diagnostics, optional caches are copied into scratch and never mutate the
// shared cache), so they run cleanly alongside parallel local scoring under the
// epoch/snapshot model.
// A multi-worker local-commit exact_multisite run must agree with the serial
// run and stay TSAN-clean (verified separately under -DENABLE_TSAN=ON).
static void test_phase9_transient_multi_worker_matches_serial() {
  std::println("test_phase9_transient_multi_worker_matches_serial");

  auto run_once = [](std::size_t workers, bool oracle) {
    auto fixture = make_three_misplaced_groups_fixture();
    larch::chart_spr_search_options options;
    options.acceptance_mode = larch::chart_spr_acceptance_mode::exact_multisite;
    options.candidate_selection =
        larch::chart_spr_candidate_selection_mode::exhaustive_exact;
    options.max_iterations = 12;
    options.rebuild_after_accept = false;
    options.worker_count = workers;
    options.local_score_worker_count = workers;
    options.cache.candidate_batch_size = 128;
    options.semantic_capture = larch::chart_spr_semantic_capture_mode::full;
    options.verify_local_commit_two_chart_oracle_for_tests = true;
    options.verify_transient_chain_extension_oracle_for_tests = oracle;
    return larch::run_chart_spr_search(std::move(fixture.dag),
                                       fixture.grammar, options);
  };

  auto serial = run_once(1, true);
  auto parallel = run_once(4, true);

  CHECK(serial.counters.accepted_moves == parallel.counters.accepted_moves);
  CHECK(serial.counters.accepted_moves >= 3);
  CHECK(serial.summary.final_score == parallel.summary.final_score);
  CHECK(serial.summary.initial_score == parallel.summary.initial_score);
  CHECK(serial.iterations.size() == parallel.iterations.size());
  CHECK(serial.canonical_report.has_value());
  CHECK(parallel.canonical_report.has_value());
  CHECK(serial.canonical_digest.has_value());
  CHECK(parallel.canonical_digest.has_value());
  CHECK(serial.canonical_digest->full_sidecar ==
        parallel.canonical_digest->full_sidecar);

  std::size_t committed = 0;
  std::size_t serial_inside_rows = 0;
  std::size_t serial_outside_rows = 0;
  for (std::size_t index = 0; index < serial.iterations.size(); ++index) {
    auto const& serial_iteration = serial.iterations[index];
    auto const& parallel_iteration = parallel.iterations[index];
    CHECK(serial_iteration.accepted_move_committed ==
          parallel_iteration.accepted_move_committed);
    CHECK(serial_iteration.accepted_inside_rows_recomputed ==
          parallel_iteration.accepted_inside_rows_recomputed);
    CHECK(serial_iteration.accepted_outside_rows_recomputed ==
          parallel_iteration.accepted_outside_rows_recomputed);
    CHECK(serial_iteration.accepted_candidate_signature ==
          parallel_iteration.accepted_candidate_signature);
    if (serial_iteration.accepted_move_committed) {
      ++committed;
      CHECK(serial_iteration.accepted_inside_rows_recomputed > 0);
      CHECK(serial_iteration.accepted_outside_rows_recomputed > 0);
      CHECK(!serial_iteration.accepted_candidate_signature.empty());
      CHECK(serial.canonical_report->iterations[index].selected_signature ==
            serial_iteration.accepted_candidate_signature);
      serial_inside_rows += serial_iteration.accepted_inside_rows_recomputed;
      serial_outside_rows += serial_iteration.accepted_outside_rows_recomputed;
    } else {
      CHECK(serial_iteration.accepted_inside_rows_recomputed == 0);
      CHECK(serial_iteration.accepted_outside_rows_recomputed == 0);
      CHECK(serial_iteration.accepted_candidate_signature.empty());
    }
  }
  CHECK(committed == serial.counters.local_commit_accepted_moves);
  CHECK(serial_inside_rows == serial.counters.inside_rows_recomputed_on_commit);
  CHECK(serial_outside_rows ==
        serial.counters.outside_rows_recomputed_on_commit);
  CHECK(parallel.counters.transient_chain_extensions_for_verification > 0);
  CHECK(parallel.counters.transient_chain_diagnostic_cache_extensions ==
        parallel.counters.transient_chain_extensions_for_verification);
  // Parallel scoring actually used the workers.
  CHECK(parallel.counters.local_score_parallel_batches > 0);
  // The transient oracle stays green under parallel local scoring (the
  // transient extensions are reader-local and do not race the scoring
  // workers).
  CHECK(parallel.counters.transient_chain_extension_oracle_mismatches == 0);
  CHECK(parallel.counters.transient_chain_extension_fallbacks == 0);

  std::println("  PASS");
}

static void test_exact_verifier_activity_is_exception_safe() {
  std::println("test_exact_verifier_activity_is_exception_safe");

  auto tracker = std::make_shared<
      larch::chart_spr_exact_verifier_concurrency_tracker>();
  try {
    larch::chart_spr_exact_verifier_activity activity{tracker};
    throw std::runtime_error("expected verifier failure");
  } catch (std::runtime_error const&) {
  }
  {
    larch::chart_spr_exact_verifier_activity activity{tracker};
  }
  // A leaked active count from the throwing scope would make this second,
  // sequential entry observe a peak of two.
  CHECK(tracker->peak() == 1);

  std::println("  PASS");
}

int main() {
  test_exact_verifier_activity_is_exception_safe();
  test_phase9_serialized_three_accept_fixture_contract();
  test_lower_bound_oracle_counters_show_full_rebuild_cost();
  test_local_rejected_candidate_counter_guardrail();
  test_streaming_and_eager_candidate_apis_match();
  test_search_state_local_score_entry_point_matches_oracle();
  test_local_score_with_ua_edge_and_invariant_offset_matches_oracle();
  test_production_delta_only_candidate_metadata_absent_matches_oracle();
  test_multiparent_dag_affected_closure_and_slot_maps();
  test_unreachable_dead_clades_ignored_reachable_dead_clades_fail();
  test_nonbinary_overlay_production_scored_locally();
  test_overlay_delta_rows_match_full_overlay_for_tiny_candidates();
  test_overlay_delta_into_reuse_and_fail_closed_contract();
  test_local_scoring_verify_option_counts_oracle_materialization();
  test_invalid_disconnected_overlay_returns_invalid_score();
  test_persistent_active_pattern_cache_matches_full_composite();
  test_root_reference_counts_preserved_in_cache();
  test_active_pattern_assertions_reject_skipped_metadata();
  test_state_builder_from_dag_rebuilds_patterns_once();
  test_semantic_capture_reuses_primary_exact_provenance();
  test_primary_provenance_capture_failure_is_hard_error();
  test_lazy_local_exact_semantic_evidence_envelope();
  test_published_state_admission_component_identity();
  test_lazy_exact_semantic_dynamic_evidence_boundary();
  test_finite_lazy_search_summary_reports_temporal_envelope();
  test_lazy_exact_w4_combined_envelope_boundary();
  test_scheduled_exact_state_w4_boundary();
  test_phase7_lazy_auto_policy_contract();
  test_phase7_lazy_auto_budget_and_freeze_contract();
  test_pattern_batch_cache_options_match_all_cache();
  test_lazy_local_admission_planner();
  test_lazy_local_packed_context_grouping_reuses_scratch();
  test_lazy_local_production_multibatch_retained_budget();
  test_lazy_local_named_fixture_finite_production_gates();
  test_local_score_into_workspace_contract();
  test_pattern_batch_into_parallel_scratch_plateau();
  test_candidate_execution_plan_lifetime_and_mismatch_guards();
  test_checked_candidate_sources_and_planned_materialization();
  test_acceptance_iteration_checks_resident_plan_once();
  test_phase7_packed_lazy_selected_topology_grouping_contract();
  test_lazy_cache_fixed_topology_conservative_search();
  test_lazy_cache_local_commit_updates_lazy_chart();
  test_parallel_local_scores_match_serial();
  test_phase4_candidate_and_pattern_tile_axes();
  test_phase4_small_search_uses_one_scheduler_and_quiesces();
  test_pattern_batch_nonreplayable_uses_automatic_candidate_batch();
  test_unchartable_grammar_rejected_with_empty_active_patterns();
  test_unsupported_enumeration_options_fail_explicitly();
  test_max_affected_estimate_prunes_before_construction();
  test_streaming_candidate_cap_stops_before_eager_path_precompute();
  test_streaming_path_pair_budget_stops_early();
  test_eager_diagnostic_enumeration_exposes_cap_after_path_precompute();
  test_phase4_scheduled_pattern_axes_match_w1();
  test_phase7_scheduled_lazy_chart_axes_match_w1();
  test_phase7_lazy_state_retained_long_taxon_name_accounting();
  test_phase2b_exact_setup_reuses_resident_state_charts();
  test_phase2b_deferred_pattern_batch_uses_owning_setup_provider();
  test_exact_verification_reuses_state_old_score();
  test_failed_exact_materialization_is_timed();
  test_top_k_exact_verification_count_is_bounded();
  test_phase6_top_k_four_uses_exact_candidate_axis_only();
  test_phase6_top_k_one_preserves_inner_exact_parallelism();
  test_phase6_exact_candidate_failures_choose_stable_rank_and_join();
  test_phase6_exact_candidate_partial_submit_joins_and_reconciles();
  test_phase6_exact_candidate_admission_stable_prefixes();
  test_phase6_exact_candidate_admission_singleton_policy();
  test_phase6_exact_candidate_admission_budget_fail_closed();
  test_phase6_exact_candidate_admission_retained_carryover();
  test_phase6_exact_candidate_admission_arithmetic_guards();
  test_phase6_exact_state_fails_before_unified_budget_overrun();
  test_phase6_finite_budget_splits_exact_candidate_wave();
  test_phase6_finite_budget_rejects_before_verifier_hook();
  test_phase6_fixed_tightened_budget_rejects_before_generation();
  test_phase6_custom_exact_contracts_fail_closed_and_serialize();
  test_phase6_top_k_worker_semantic_matrix();
  test_phase6_forced_exception_top_k_worker_parity_and_quiescence();
  test_lower_bound_heuristic_acceptance_is_explicit();
  test_fixed_topology_exact_rejects_bare_candidate();
  test_fixed_topology_iteration_uses_default_selector();
  test_sampled_tree_fixed_topology_uses_source_certificate();
  test_fixed_topology_exact_certificate_scores_selected_topology();
  test_phase8_fixed_topology_cache_score_matches_materialized_per_pattern();
  test_phase8_fixed_topology_rejects_bad_selected_partition();
  test_phase8_fixed_topology_rejects_unreachable_selected_after_ref();
  test_phase8_persistent_cache_local_commit_matches_materialized();
  test_phase8_persistent_cache_dag_verification_has_no_fallback();
  test_phase8_per_pattern_oracle_catches_independent_moved_state();
  test_phase5_combine_rows_binary_regression();
  test_phase5_multifurcation_selected_topology_rows();
  test_phase5_multifurcation_outside_rows_exercise_shared_state_guard();
  test_phase6_source_multifurcation_candidate_has_no_unreachable_helper();
  test_phase6_multifurcation_fixed_topology_local_commit();
  test_phase6_multifurcation_lower_bound_conservative_commit();
  test_phase6_exact_multisite_multifurcation_gate();
  test_phase8_persistent_cache_invariant_failure_is_hard_error();
  test_phase8_persistent_verifier_materialized_oracle_all_move_classes();
  test_phase8_oracle_mismatch_uses_materialized_oracle_result();
  test_phase8_chain_objective_gate_is_monotone_across_commits();
  test_phase8_icache_participation_across_local_commits();
  test_enumeration_truncation_sets_unverified_flag_even_exhaustive();
  test_phase5_no_improvement_search_stops_without_commit();
  test_phase5_known_improving_search_commits_once();
  test_phase7_auto_on_accepted_rebuild_preserves_frozen_policy();
  test_phase9_known_improving_search_uses_local_accept_update();
  test_phase5_final_compaction_uses_grammar_oracle_not_tree_override();
  test_phase5_final_compaction_checks_recorded_exact_objective();
  test_phase9_pattern_batch_local_update_matches_output_dag();
  test_phase9_fixed_topology_compaction_uses_certificate();
  test_phase9_lower_bound_compaction_matches_output_dag();
  test_phase9_multi_iteration_local_updates_match_output_dag();
  test_phase5_rejected_candidates_do_not_rebuild_sidecar();
  test_phase5_post_materialization_worsening_rejects_commit();
  test_phase9_local_objective_worsening_rejects_commit_evidence();
  test_phase5_fixed_topology_mode_commits_with_rebuilt_certificate_gate();
  test_phase5_pattern_fingerprint_mismatch_rebuilds_patterns();
  test_phase5_seeded_multi_iteration_is_deterministic();
  test_canonical_evidence_failure_is_hard_error();
  test_candidate_exact_bnb_overflow_is_hard_error();
  test_exhaustive_exact_acceptance_matches_oracle();
  test_phase5_witness_topology_selects_required_ancestor_path();
  test_phase5_final_compaction_normalizes_trim_options();
  test_phase5_overlay_chain_compaction_preserves_intended_keys();
  test_phase5_compaction_augments_historical_intended_keys();
  test_phase4_local_commit_counter_contract_and_oracle();
  test_phase4_conservative_mode_counters_unchanged();
  test_phase4_local_commit_score_ua_edge_rejected();
  test_phase4_local_commit_post_append_failure_is_hard_error();
  test_phase4_exact_trim_cache_never_stale();
  test_phase4_multi_worker_matches_serial();
  test_phase4_fixed_topology_exact_local_commit();
  test_lazy_cache_local_commit_sequence_projects_lazy_chart();
  test_lazy_local_commit_recomputes_reactivated_clades();
  test_phase4_pattern_batches_local_commit();
  test_phase9_transient_no_full_overlay_materialization();
  test_phase9_transient_oracle_both_charts_green();
  test_phase9_transient_oracle_green_on_multiparent_dag();
  test_phase9_transient_oracle_catches_corruption();
  test_phase9_transient_tombstone_scope_falls_back_to_cold();
  test_phase9_transient_multi_worker_matches_serial();
  std::println("chart_spr_search_test PASS");
  return 0;
}
