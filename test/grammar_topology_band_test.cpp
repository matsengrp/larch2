#include <larch/grammar_topology_band.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

[[noreturn]] static void fail(char const* expression, char const* file,
                              int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                      \
  do {                                                         \
    if (!(expression)) fail(#expression, __FILE__, __LINE__);  \
  } while (false)

static larch::clade_grammar fixture_grammar() {
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B", "C", "D", "E", "F"};
  for (std::size_t i = 0; i < grammar.taxa.id_to_sample_id.size(); ++i) {
    grammar.taxa.sample_id_to_id.emplace(grammar.taxa.id_to_sample_id[i],
                                         static_cast<larch::taxon_id>(i));
  }
  grammar.clades = {
      {{0}}, {{1}}, {{2}}, {{3}}, {{4}}, {{5}}, {{1, 2}},
      {{0, 1, 2}}, {{3, 4}}, {{3, 4, 5}}, {{0, 1, 2, 3, 4, 5}},
  };
  grammar.root_clade = 10;
  grammar.productions_by_parent.resize(grammar.clades.size());
  grammar.productions_by_child.resize(grammar.clades.size());
  auto add = [&](larch::clade_id parent,
                 std::vector<larch::clade_id> children) {
    auto id = static_cast<larch::production_id>(grammar.productions.size());
    grammar.productions.push_back(
        {.parent = parent, .children = std::move(children)});
    grammar.productions_by_parent[parent].push_back(id);
    for (auto child : grammar.productions.back().children) {
      grammar.productions_by_child[child].push_back(id);
    }
  };
  add(6, {1, 2});
  add(7, {0, 1, 2});
  add(7, {0, 6});
  add(8, {3, 4});
  add(9, {3, 4, 5});
  add(9, {8, 5});
  add(10, {7, 9});
  return grammar;
}

static std::uint8_t state(char value) {
  switch (value) {
    case 'A': return larch::nuc_base::A;
    case 'C': return larch::nuc_base::C;
    case 'G': return larch::nuc_base::G;
    case 'T': return larch::nuc_base::T;
  }
  throw std::runtime_error("invalid fixture nucleotide");
}

static larch::site_pattern_set fixture_patterns() {
  larch::site_pattern_set result;
  result.taxon_count = 6;
  for (auto row : {std::string_view{"ACCAAA"}, std::string_view{"AAACCA"}}) {
    larch::site_pattern pattern;
    for (char value : row) pattern.state_by_taxon.push_back(state(value));
    pattern.weight = static_cast<std::uint32_t>(result.patterns.size() + 1);
    pattern.reference_state_counts[larch::nuc_base::A] = pattern.weight;
    for (std::uint32_t copy = 0; copy < pattern.weight; ++copy) {
      pattern.positions.push_back(
          static_cast<larch::mutation_position>(result.total_site_count + 1));
      result.original_site_to_pattern.push_back(result.patterns.size());
      ++result.total_site_count;
      ++result.variable_site_count;
    }
    result.patterns.push_back(std::move(pattern));
  }
  result.exact_pattern_to_normalized_binary_pattern.assign(
      result.patterns.size(), larch::no_site_pattern);
  result.exact_pattern_to_normalized_binary_state_map.assign(
      result.patterns.size(), larch::normalized_binary_state_map{});
  return result;
}

static larch::grammar_topology_band_result select(std::size_t workers) {
  auto grammar = fixture_grammar();
  auto patterns = fixture_patterns();
  larch::grammar_topology_band_options options;
  options.minimum_score = 3;
  options.maximum_score = 4;
  options.worker_count = workers;
  options.sankoff_verification_stride = 1;
  return larch::select_grammar_topology_band(grammar, patterns, {}, options);
}

static void selection_test() {
  auto result = select(1);
  CHECK(result.census.ordinal_scores ==
        std::vector<std::uint64_t>({6, 4, 5, 3}));
  std::map<std::uint64_t, std::uint64_t> const expected_histogram{
      {3, 1}, {4, 1}, {5, 1}, {6, 1}};
  CHECK(result.census.score_histogram == expected_histogram);
  CHECK(result.selected.size() == 2);
  CHECK(result.selected[0].grammar_ordinal == 1);
  CHECK(result.selected[0].absolute_score == 4);
  CHECK(result.selected[1].grammar_ordinal == 3);
  CHECK(result.selected[1].absolute_score == 3);
  for (auto const& row : result.selected) {
    CHECK(row.absolute_score == row.selected_grammar_sankoff_score);
    CHECK(!row.selected_productions.empty());
    CHECK(row.selected_productions.size() == row.selected_production_keys.size());
    CHECK(larch::direct_kary_selection_sha256(row.selected_production_keys)
              .size() == 64);
  }
  auto grammar = fixture_grammar();
  auto patterns = fixture_patterns();
  auto projected = larch::project_grammar_topology_band(
      grammar, patterns, {}, result.census, 3, 4, 1);
  CHECK(projected.census.ordinal_scores == result.census.ordinal_scores);
  CHECK(projected.selected == result.selected);
}

static void worker_parity_test() {
  auto serial = select(1);
  for (std::size_t workers : {std::size_t{3}, std::size_t{8}}) {
    auto parallel = select(workers);
    CHECK(parallel.census.ordinal_scores == serial.census.ordinal_scores);
    CHECK(parallel.census.score_histogram == serial.census.score_histogram);
    CHECK(parallel.census.optimum == serial.census.optimum);
    CHECK(parallel.census.optimal_ordinals == serial.census.optimal_ordinals);
    CHECK(parallel.selected == serial.selected);
  }
  auto grammar = fixture_grammar();
  auto patterns = fixture_patterns();
  auto census = larch::census_grammar_topologies(grammar, patterns);
  CHECK(census.ordinal_scores.empty());
}

static bool rejects(auto&& callback, std::string_view needle) {
  try {
    callback();
  } catch (std::exception const& error) {
    return std::string_view{error.what()}.contains(needle);
  }
  return false;
}

static void rejection_test() {
  auto grammar = fixture_grammar();
  auto patterns = fixture_patterns();
  larch::grammar_topology_band_options options;
  options.minimum_score = 7;
  options.maximum_score = 8;
  CHECK(rejects(
      [&] { (void)larch::select_grammar_topology_band(
                grammar, patterns, {}, options); },
      "selects no topology"));
  options.minimum_score = 4;
  options.maximum_score = 3;
  CHECK(rejects(
      [&] { (void)larch::select_grammar_topology_band(
                grammar, patterns, {}, options); },
      "invalid inclusive score interval"));
  options.minimum_score = 3;
  options.maximum_score = 4;
  options.worker_count = 0;
  CHECK(rejects(
      [&] { (void)larch::select_grammar_topology_band(
                grammar, patterns, {}, options); },
      "worker count must be positive"));

  options.worker_count = 1;
  options.maximum_score =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
  CHECK(rejects(
      [&] { (void)larch::select_grammar_topology_band(
                grammar, patterns, {}, options); },
      "invalid inclusive score interval"));

  auto complete = select(1);
  ++complete.census.score_histogram.begin()->second;
  CHECK(rejects(
      [&] {
        (void)larch::project_grammar_topology_band(
            grammar, patterns, {}, std::move(complete.census), 3, 4, 1);
      },
      "census ledger summary mismatch"));
}

static void saturation_test() {
  auto grammar = fixture_grammar();
  auto patterns = fixture_patterns();
  larch::chart_options chart_options;
  chart_options.score_ua_edge = true;

  larch::grammar_topology_enumerator enumerator(grammar);
  std::vector<std::uint64_t> base_scores;
  larch::grammar_topology_fitch_scorer scorer(grammar, patterns,
                                              chart_options);
  enumerator.stream([&](std::uint64_t,
                        larch::grammar_topology const& topology) {
    base_scores.push_back(scorer.score_enumerator_topology(topology));
  });
  auto const [minimum, maximum] =
      std::minmax_element(base_scores.begin(), base_scores.end());
  CHECK(*minimum < *maximum);
  CHECK(*maximum < larch::multisite_score_inf);

  // The lowest topology remains finite while a higher one saturates exactly
  // at the scorer sentinel.  A census must reject the whole run rather than
  // publishing that sentinel as an exact score-bin/ordinal value.
  patterns.skipped_invariant_constant_score_with_reference_edge =
      larch::multisite_score_inf - *maximum;
  CHECK(rejects(
      [&] {
        (void)larch::census_grammar_topologies(grammar, patterns,
                                               chart_options);
      },
      "non-finite or saturated score at ordinal"));
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::string_view mode{argv[1]};
  if (mode == "selection") selection_test();
  else if (mode == "worker_parity") worker_parity_test();
  else if (mode == "rejection") rejection_test();
  else if (mode == "saturation") saturation_test();
  else return 2;
  std::println("PASS {}", mode);
  return 0;
}
