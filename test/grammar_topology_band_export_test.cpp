#include <larch/grammar_topology_band_export.hpp>
#include <larch/grammar_topology_replay.hpp>

#include <larch/chart_spr_semantic_report.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/rooted_topology.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/sha256.hpp>
#include <larch/topology_landscape_bundle.hpp>

#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <print>
#include <set>
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

struct fixture {
  larch::phylo_dag source;
  larch::clade_grammar grammar;
  larch::site_pattern_set patterns;
};

static larch::clade_grammar mixed_grammar() {
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

static fixture mixed_fixture() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto source = larch::test::make_tiny_labelled_tree(
      "AAA", tiny_inner("root", "AAA",
                        {tiny_leaf("A", "AAA"), tiny_leaf("B", "CAA"),
                         tiny_leaf("C", "CAA"), tiny_leaf("D", "ACC"),
                         tiny_leaf("E", "ACC"), tiny_leaf("F", "AAA")}));
  auto grammar = mixed_grammar();
  auto patterns = larch::build_site_patterns(source, grammar);
  return {std::move(source), std::move(grammar), std::move(patterns)};
}

static std::string digest(std::string_view value) {
  larch::sha256 hash;
  hash.update(value);
  return hash.hex_digest();
}

static std::string digest_file(std::filesystem::path const& path) {
  std::ifstream input(path, std::ios::binary);
  CHECK(input.is_open());
  std::string bytes{std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{}};
  return digest(bytes);
}

static larch::grammar_topology_band_result band_for(
    fixture const& value, std::size_t workers, std::uint64_t low = 3,
    std::uint64_t high = 4) {
  larch::grammar_topology_band_options options;
  options.minimum_score = low;
  options.maximum_score = high;
  options.worker_count = workers;
  options.sankoff_verification_stride = 1;
  return larch::select_grammar_topology_band(
      value.grammar, value.patterns, {}, options);
}

static larch::grammar_topology_band_export_options export_options(
    fixture& value, larch::grammar_topology_band_result const& band,
    std::filesystem::path output) {
  larch::grammar_topology_band_export_options options;
  options.output_directory = output;
  auto const grammar_construction =
      std::string{"synthetic_mixed_binary_ternary_direct_v1"};
  auto const derived = larch::derive_topology_score_band_semantics(
      value.source, value.grammar, value.patterns, {}, grammar_construction);
  options.semantics = {
      .alignment_sha256 = derived.alignment_sha256,
      .ambiguity_policy = derived.ambiguity_policy,
      .grammar_construction = grammar_construction,
      .grammar_digest = derived.grammar_digest,
      .grammar_semantic_digest = derived.grammar_semantic_digest,
      .input_content_sha256 = derived.input_content_sha256,
      .parsimony_model = derived.parsimony_model,
      .reference_sha256 = derived.reference_sha256,
      .score_baseline = band.census.optimum,
      .site_pattern_digest = derived.site_pattern_digest,
      .taxon_labels_sha256 = derived.taxon_labels_sha256,
      .ua_scoring = derived.ua_scoring,
      .universe = "synthetic_four_ordinal_full_direct_grammar",
  };
  auto provider_path = std::filesystem::path{output.native() + ".provider.pb"};
  auto reference_path =
      std::filesystem::path{output.native() + ".reference.fa"};
  larch::save_proto_dag(value.source, provider_path.native());
  {
    std::ofstream reference(reference_path, std::ios::binary | std::ios::trunc);
    reference << ">reference\n" << larch::get_reference_sequence(value.source)
              << "\n";
    CHECK(static_cast<bool>(reference));
  }
  options.provenance = {
      .command = "grammar_topology_band_export_test;workers=" +
                 std::to_string(band.worker_count),
      .derivation_ref = "synthetic_fixture_v1",
      .producer_commit = "not_applicable",
      .producer_dirty = "not_applicable",
      .producer_repository = "larch2-spectral-test",
      .provider_input_sha256 = digest_file(provider_path),
      .provider_legacy_dag_semantic_sha256 =
          larch::topology_score_band_provider_legacy_dag_semantic_sha256(
              value.source, false),
      .reference_input_sha256 = digest_file(reference_path),
      .seed_tree_input_sha256 = digest_file(provider_path),
      .toolchain = "gcc-trunk-test",
      .provider_input_path = provider_path,
      .reference_input_path = reference_path,
      .seed_tree_input_path = provider_path,
  };
  return options;
}

static std::string read(std::filesystem::path const& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

static void write(std::filesystem::path const& path,
                  std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  CHECK(output.is_open());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  CHECK(static_cast<bool>(output));
}

static void remove_tree(std::filesystem::path const& path) {
  std::error_code ignored;
  std::filesystem::remove_all(path, ignored);
  std::filesystem::remove(path.native() + ".provider.pb", ignored);
  std::filesystem::remove(path.native() + ".reference.fa", ignored);
}

static bool rejects(auto&& callback, std::string_view needle) {
  try {
    callback();
  } catch (std::exception const& error) {
    return std::string_view{error.what()}.contains(needle);
  }
  return false;
}

static void synthetic_export_test() {
  auto value = mixed_fixture();
  auto band = band_for(value, 1);
  CHECK(band.census.ordinal_scores ==
        std::vector<std::uint64_t>({6, 4, 5, 3}));
  auto output = larch::test::unique_temp_path("ti2_band_export", ".raw");
  remove_tree(output);
  auto result = larch::export_grammar_topology_band_raw(
      value.source, value.grammar, value.patterns, {}, band,
      export_options(value, band, output));
  CHECK(result.selected.size() == 2);
  CHECK(result.selected[0].grammar_ordinal == 1);
  CHECK(result.selected[1].grammar_ordinal == 3);
  CHECK(result.semantic_data_sha256.size() == 64);
  CHECK(larch::topology_landscape::validate_score_band_raw(output));
  auto selected_bytes = read(output / "selected_topologies.tsv");
  CHECK(selected_bytes.contains("\tI4[1,3,5,6]\t"));
  CHECK(selected_bytes.contains("\tI5[0,2,3,5,6]\t"));
  remove_tree(output);
}

static void worker_parity_test() {
  auto serial_fixture = mixed_fixture();
  auto serial_band = band_for(serial_fixture, 1);
  auto serial_output =
      larch::test::unique_temp_path("ti2_band_export_w1", ".raw");
  remove_tree(serial_output);
  auto serial = larch::export_grammar_topology_band_raw(
      serial_fixture.source, serial_fixture.grammar, serial_fixture.patterns,
      {}, serial_band,
      export_options(serial_fixture, serial_band, serial_output));

  auto parallel_fixture = mixed_fixture();
  auto parallel_band = band_for(parallel_fixture, 3);
  auto parallel_output =
      larch::test::unique_temp_path("ti2_band_export_w3", ".raw");
  remove_tree(parallel_output);
  auto parallel = larch::export_grammar_topology_band_raw(
      parallel_fixture.source, parallel_fixture.grammar,
      parallel_fixture.patterns, {}, parallel_band,
      export_options(parallel_fixture, parallel_band, parallel_output));

  CHECK(serial.semantic_data_sha256 == parallel.semantic_data_sha256);
  CHECK(serial.provenance_id != parallel.provenance_id);
  for (auto name : {"semantics.tsv", "score_census.tsv",
                    "selected_topologies.tsv"}) {
    CHECK(read(serial_output / name) == read(parallel_output / name));
  }
  remove_tree(serial_output);
  remove_tree(parallel_output);
}

static void no_replace_test() {
  auto value = mixed_fixture();
  auto band = band_for(value, 1);
  auto output =
      larch::test::unique_temp_path("ti2_band_export_no_replace", ".raw");
  remove_tree(output);
  std::filesystem::create_directory(output);
  std::ofstream(output / "sentinel") << "unchanged\n";
  CHECK(rejects(
      [&] {
        (void)larch::export_grammar_topology_band_raw(
            value.source, value.grammar, value.patterns, {}, band,
            export_options(value, band, output));
      },
      "destination already exists"));
  CHECK(read(output / "sentinel") == "unchanged\n");
  remove_tree(output);
}

static void mismatch_test() {
  auto value = mixed_fixture();
  auto band = band_for(value, 1);
  ++band.selected.front().absolute_score;
  auto output =
      larch::test::unique_temp_path("ti2_band_export_mismatch", ".raw");
  remove_tree(output);
  CHECK(rejects(
      [&] {
        (void)larch::export_grammar_topology_band_raw(
            value.source, value.grammar, value.patterns, {}, band,
            export_options(value, band, output));
      },
      "selected ordinal/score ledger mismatch"));
  CHECK(!std::filesystem::exists(output));
}

static void rejection_matrix_test() {
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    band.selected.erase(band.selected.begin());
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_missing_selection", ".raw");
    remove_tree(output);
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band,
              export_options(value, band, output));
        },
        "selected interval omits a census ordinal"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    using larch::test::tiny_inner;
    using larch::test::tiny_leaf;
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_provider_correspondence", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    auto other = larch::test::make_tiny_labelled_tree(
        "AAA",
        tiny_inner("root", "AAA",
                   {tiny_inner("ab", "AAA",
                               {tiny_leaf("A", "AAA"),
                                tiny_leaf("B", "CAA")}),
                    tiny_leaf("C", "CAA"), tiny_leaf("D", "ACC"),
                    tiny_leaf("E", "ACC"), tiny_leaf("F", "AAA")}));
    larch::save_proto_dag(other,
                          options.provenance.provider_input_path.native());
    options.provenance.provider_input_sha256 =
        digest_file(options.provenance.provider_input_path);
    options.provenance.seed_tree_input_sha256 =
        options.provenance.provider_input_sha256;
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "provider artifact/live DAG semantic identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    using larch::test::tiny_inner;
    using larch::test::tiny_leaf;
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_provider_content_correspondence", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    // Uniformly relabel A->C and C->G. The topology and every mutation count
    // remain unchanged, so the legacy topology/minimum digest still matches;
    // the scientific reference/alignment content must nevertheless reject.
    auto other = larch::test::make_tiny_labelled_tree(
        "CCC", tiny_inner("root", "CCC",
                          {tiny_leaf("A", "CCC"), tiny_leaf("B", "GCC"),
                           tiny_leaf("C", "GCC"), tiny_leaf("D", "CGG"),
                           tiny_leaf("E", "CGG"), tiny_leaf("F", "CCC")}));
    larch::save_proto_dag(other,
                          options.provenance.provider_input_path.native());
    options.provenance.provider_input_sha256 =
        digest_file(options.provenance.provider_input_path);
    options.provenance.seed_tree_input_sha256 =
        options.provenance.provider_input_sha256;
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "provider artifact/live reference identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_pattern_correspondence", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    value.patterns.patterns.front().state_by_taxon.front() =
        larch::nuc_base::T;
    auto altered = larch::derive_topology_score_band_semantics(
        value.source, value.grammar, value.patterns, {},
        options.semantics.grammar_construction);
    options.semantics.alignment_sha256 = altered.alignment_sha256;
    options.semantics.site_pattern_digest = altered.site_pattern_digest;
    options.semantics.input_content_sha256 = altered.input_content_sha256;
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "source/live alignment identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_provider_semantic_identity", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    options.provenance.provider_legacy_dag_semantic_sha256 =
        std::string(64, 'f');
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "provider legacy DAG semantic identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    band.selected.front().selected_production_keys.front() += "-changed";
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_production_replay", ".raw");
    remove_tree(output);
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band,
              export_options(value, band, output));
        },
        "selected production replay mismatch"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_metadata", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    options.semantics.taxon_labels_sha256 = std::string(64, 'f');
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "taxon labels identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_grammar_identity", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    options.semantics.grammar_digest = std::string(64, 'f');
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "ordered grammar identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_pattern_identity", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    options.semantics.site_pattern_digest = std::string(64, 'f');
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "site pattern identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_provider_identity", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    options.provenance.provider_input_sha256 = std::string(64, 'f');
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "provider input identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_model_identity", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    options.semantics.parsimony_model = "changed";
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "parsimony model identity drift"));
    options = export_options(value, band, output);
    options.semantics.ua_scoring = "changed";
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "UA scoring identity drift"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    std::swap(value.grammar.taxa.id_to_sample_id[0],
              value.grammar.taxa.id_to_sample_id[1]);
    value.grammar.taxa.sample_id_to_id["A"] = 1;
    value.grammar.taxa.sample_id_to_id["B"] = 0;
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_taxon_order", ".raw");
    remove_tree(output);
    auto declared = mixed_fixture();
    auto options = export_options(declared, band, output);
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "taxon registry is not strict unsigned UTF-8 byte order"));
    CHECK(!std::filesystem::exists(output));
  }
  {
    auto value = mixed_fixture();
    auto band = band_for(value, 1);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_baseline", ".raw");
    remove_tree(output);
    auto options = export_options(value, band, output);
    ++options.semantics.score_baseline;
    CHECK(rejects(
        [&] {
          (void)larch::export_grammar_topology_band_raw(
              value.source, value.grammar, value.patterns, {}, band, options);
        },
        "inconsistent band interval, ledger, or baseline"));
    CHECK(!std::filesystem::exists(output));
  }
}

static fixture duplicate_fixture() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  auto source = larch::test::make_tiny_labelled_tree(
      "A", tiny_inner("root", "A",
                      {tiny_leaf("A", "A"), tiny_leaf("B", "C")}));
  larch::clade_grammar grammar;
  grammar.taxa.id_to_sample_id = {"A", "B"};
  grammar.taxa.sample_id_to_id = {{"A", 0}, {"B", 1}};
  grammar.clades = {{{0}}, {{1}}, {{0, 1}}};
  grammar.root_clade = 2;
  grammar.productions_by_parent.resize(3);
  grammar.productions_by_child.resize(3);
  for (larch::production_id pid : {0U, 1U}) {
    grammar.productions.push_back({.parent = 2, .children = {0, 1}});
    grammar.productions_by_parent[2].push_back(pid);
    grammar.productions_by_child[0].push_back(pid);
    grammar.productions_by_child[1].push_back(pid);
  }
  auto patterns = larch::build_site_patterns(source, grammar);
  return {std::move(source), std::move(grammar), std::move(patterns)};
}

static void collision_test() {
  auto value = duplicate_fixture();
  auto band = band_for(value, 1, 1, 1);
  CHECK(band.selected.size() == 2);
  auto output =
      larch::test::unique_temp_path("ti2_band_export_collision", ".raw");
  remove_tree(output);
  CHECK(rejects(
      [&] {
        (void)larch::export_grammar_topology_band_raw(
            value.source, value.grammar, value.patterns, {}, band,
            export_options(value, band, output));
      },
      "duplicate canonical topology"));
  CHECK(!std::filesystem::exists(output));
}

static void arity_roundtrip_test() {
  using larch::test::tiny_inner;
  using larch::test::tiny_leaf;
  for (std::size_t arity : {std::size_t{3}, std::size_t{4}, std::size_t{5},
                            std::size_t{10}}) {
    std::vector<larch::test::tiny_tree_node> leaves;
    for (std::size_t index = 0; index < arity; ++index) {
      leaves.push_back(tiny_leaf("T" + std::to_string(index),
                                index == 0 ? "C" : "A"));
    }
    auto source = larch::test::make_tiny_labelled_tree(
        "A", tiny_inner("root", "A", std::move(leaves)));
    larch::polytomy_refinement_options refinement_options;
    refinement_options.mode = larch::polytomy_mode::allow;
    auto refinement = larch::build_polytomy_refined_clade_grammar(
        source, larch::clade_grammar_options{}, refinement_options);
    auto patterns = larch::build_site_patterns(source, refinement.grammar);
    fixture value{std::move(source), std::move(refinement.grammar),
                  std::move(patterns)};
    auto census = larch::census_grammar_topologies(
        value.grammar, value.patterns);
    auto band = band_for(value, 1, census.optimum, census.optimum);
    auto output = larch::test::unique_temp_path(
        "ti2_band_export_arity_" + std::to_string(arity), ".raw");
    remove_tree(output);
    auto result = larch::export_grammar_topology_band_raw(
        value.source, value.grammar, value.patterns, {}, band,
        export_options(value, band, output));
    CHECK(result.selected.size() == 1);
    CHECK(larch::topology::parse_canonical_tree(
              result.selected.front().canonical_bytes).children.size() == arity);
    CHECK(larch::topology_landscape::validate_score_band_raw(output));
    remove_tree(output);
  }
}

static std::filesystem::path write_replay_manifest(
    std::filesystem::path const& root, fixture& value,
    larch::grammar_topology_band_result const& band,
    larch::grammar_topology_band_export_options const& options,
    bool add_unknown_key = false) {
  auto report = root / "prior.md";
  auto histogram = root / "prior-histogram.tsv";
  auto minima = root / "prior-minima.tsv";
  write(report, "synthetic complete census report\n");
  std::string histogram_bytes =
      "score\treduced_864_count\tfull_575168_count\n";
  for (auto const& [score, count] : band.census.score_histogram) {
    histogram_bytes += std::to_string(score) + "\t" +
                       std::to_string(count) + "\t" +
                       std::to_string(count) + "\n";
  }
  write(histogram, histogram_bytes);
  std::string minima_bytes =
      "class\tordinal\texact_score\ttopology_sha256\tcanonical_dag_"
      "semantic_sha256\tcanonical_dag_clades_sha256\tcanonical_dag_"
      "productions_sha256\tartifact\tartifact_sha256\n";
  for (std::size_t index = 0; index < band.census.optimal_ordinals.size();
       ++index) {
    minima_bytes += std::to_string(index) + "\t" +
                    std::to_string(band.census.optimal_ordinals[index]) +
                    "\t" + std::to_string(band.census.optimum) + "\t" +
                    std::string(64, '0') + "\t" + std::string(64, '1') +
                    "\t" + std::string(64, '2') + "\t" +
                    std::string(64, '3') + "\tminimum.pb\t" +
                    std::string(64, '4') + "\n";
  }
  write(minima, minima_bytes);

  auto const derived = larch::derive_topology_score_band_semantics(
      value.source, value.grammar, value.patterns, {},
      options.semantics.grammar_construction);
  auto manifest = root / "replay.tsv";
  std::string bytes = "key\tvalue\n";
  auto field = [&](std::string_view key, std::string_view value) {
    bytes += key;
    bytes += '\t';
    bytes += value;
    bytes += '\n';
  };
  field("schema_name", "larch-topology-score-band-replay-v1");
  field("schema_version", "1");
  field("grammar_construction", options.semantics.grammar_construction);
  field("universe", options.semantics.universe);
  field("provider_input_sha256", options.provenance.provider_input_sha256);
  field("provider_legacy_dag_semantic_sha256",
        options.provenance.provider_legacy_dag_semantic_sha256);
  field("provider_stored_history_parsimony_min",
        std::to_string(
            larch::topology_score_band_provider_stored_history_parsimony_min(
                value.source, false)));
  field("seed_tree_input_sha256", options.provenance.seed_tree_input_sha256);
  field("reference_input_sha256", options.provenance.reference_input_sha256);
  field("alignment_sha256", derived.alignment_sha256);
  field("ambiguity_policy", derived.ambiguity_policy);
  field("grammar_digest", derived.grammar_digest);
  field("grammar_semantic_digest", derived.grammar_semantic_digest);
  field("input_content_sha256", derived.input_content_sha256);
  field("parsimony_model", derived.parsimony_model);
  field("reference_sha256", derived.reference_sha256);
  field("site_pattern_digest", derived.site_pattern_digest);
  field("taxon_labels_sha256", derived.taxon_labels_sha256);
  field("ua_scoring", derived.ua_scoring);
  field("grammar_topology_count",
        std::to_string(band.census.expected_topology_count));
  field("score_baseline", std::to_string(band.census.optimum));
  field("sankoff_verification_stride", "1");
  field("sankoff_verified_topology_count",
        std::to_string(band.census.sankoff_verified_topology_count));
  field("prior_census_report_path", report.filename().native());
  field("prior_census_report_sha256", digest_file(report));
  field("prior_histogram_path", histogram.filename().native());
  field("prior_histogram_sha256", digest_file(histogram));
  field("prior_minima_path", minima.filename().native());
  field("prior_minima_sha256", digest_file(minima));
  if (add_unknown_key) field("unexpected", "value");
  write(manifest, bytes);
  return manifest;
}

static void replay_contract_test() {
  auto root = larch::test::unique_temp_path("ti2_replay_contract", ".d");
  remove_tree(root);
  std::filesystem::create_directories(root);
  auto value = mixed_fixture();
  auto band = band_for(value, 1);
  auto options = export_options(value, band, root / "unused.raw");
  auto manifest_path =
      write_replay_manifest(root, value, band, options);
  auto manifest = larch::read_topology_score_band_replay_manifest(
      manifest_path);
  auto preflight = larch::validate_topology_score_band_replay_preflight(
      manifest, value.source, value.grammar, value.patterns, {},
      options.provenance.provider_input_path,
      options.provenance.reference_input_path,
      options.provenance.seed_tree_input_path);
  CHECK(preflight.manifest_sha256 == digest_file(manifest_path));
  CHECK(preflight.derived.grammar_digest ==
        manifest.semantics.grammar_digest);
  larch::validate_topology_score_band_replay_census(manifest, band.census);
  auto rejects_preflight = [&](auto mutate, std::string_view diagnostic) {
    auto altered = manifest;
    mutate(altered);
    CHECK(rejects(
        [&] {
          (void)larch::validate_topology_score_band_replay_preflight(
              altered, value.source, value.grammar, value.patterns, {},
              options.provenance.provider_input_path,
              options.provenance.reference_input_path,
              options.provenance.seed_tree_input_path);
        },
        diagnostic));
  };
  rejects_preflight(
      [](auto& altered) {
        altered.provider_input_sha256 = std::string(64, 'f');
      },
      "provider input mismatch");
  rejects_preflight(
      [](auto& altered) {
        altered.provider_legacy_dag_semantic_sha256 = std::string(64, 'f');
      },
      "provider legacy DAG semantic mismatch");
  rejects_preflight(
      [](auto& altered) {
        ++altered.provider_stored_history_parsimony_min;
      },
      "provider stored-history minimum mismatch");
  rejects_preflight(
      [](auto& altered) {
        altered.reference_input_sha256 = std::string(64, 'f');
      },
      "reference input mismatch");
  rejects_preflight(
      [](auto& altered) {
        altered.seed_tree_input_sha256 = std::string(64, 'f');
      },
      "seed-tree input mismatch");
  rejects_preflight(
      [](auto& altered) {
        altered.semantics.grammar_digest = std::string(64, 'f');
      },
      "ordered grammar mismatch");
  rejects_preflight(
      [](auto& altered) {
        altered.semantics.grammar_semantic_digest = std::string(64, 'f');
      },
      "semantic grammar mismatch");
  std::vector<larch::topology_score_band_replay_minimum> minima{{
      .class_index = 0,
      .ordinal = band.census.optimal_ordinals.front(),
      .exact_score = band.census.optimum,
      .topology_sha256 = std::string(64, '0'),
      .canonical_dag_semantic_sha256 = std::string(64, '1'),
      .canonical_dag_clades_sha256 = std::string(64, '2'),
      .canonical_dag_productions_sha256 = std::string(64, '3'),
      .artifact_sha256 = std::string(64, '4'),
  }};
  larch::validate_topology_score_band_replay_minima(manifest, minima);
  minima.front().canonical_dag_semantic_sha256 = std::string(64, 'e');
  CHECK(rejects(
      [&] {
        larch::validate_topology_score_band_replay_minima(manifest, minima);
      },
      "minimum witness identity mismatch"));
  minima.front().canonical_dag_semantic_sha256 = std::string(64, '1');
  ++minima.front().ordinal;
  CHECK(rejects(
      [&] {
        larch::validate_topology_score_band_replay_minima(manifest, minima);
      },
      "minimum witness identity mismatch"));

  auto bad_manifest = manifest;
  ++bad_manifest.grammar_topology_count;
  CHECK(rejects(
      [&] {
        (void)larch::validate_topology_score_band_replay_preflight(
            bad_manifest, value.source, value.grammar, value.patterns, {},
            options.provenance.provider_input_path,
            options.provenance.reference_input_path,
            options.provenance.seed_tree_input_path);
      },
      "grammar topology count mismatch"));

  bad_manifest = manifest;
  bad_manifest.semantics.site_pattern_digest = std::string(64, 'f');
  CHECK(rejects(
      [&] {
        (void)larch::validate_topology_score_band_replay_preflight(
            bad_manifest, value.source, value.grammar, value.patterns, {},
            options.provenance.provider_input_path,
            options.provenance.reference_input_path,
            options.provenance.seed_tree_input_path);
      },
      "site pattern mismatch"));

  auto bad_census = band.census;
  ++bad_census.score_histogram.begin()->second;
  CHECK(rejects(
      [&] {
        larch::validate_topology_score_band_replay_census(manifest,
                                                          bad_census);
      },
      "fresh/prior score histogram mismatch"));

  bad_census = band.census;
  ++bad_census.sankoff_verified_topology_count;
  CHECK(rejects(
      [&] {
        larch::validate_topology_score_band_replay_census(manifest,
                                                          bad_census);
      },
      "Sankoff audit coverage mismatch"));

  bad_census = band.census;
  ++bad_census.optimum;
  CHECK(rejects(
      [&] {
        larch::validate_topology_score_band_replay_census(manifest,
                                                          bad_census);
      },
      "fresh optimum mismatch"));

  write(root / "prior.md", "tampered census report\n");
  CHECK(rejects(
      [&] {
        larch::validate_topology_score_band_replay_census(manifest,
                                                          band.census);
      },
      "prior evidence artifact mismatch"));

  auto bad_manifest_path =
      write_replay_manifest(root, value, band, options, true);
  CHECK(rejects(
      [&] {
        (void)larch::read_topology_score_band_replay_manifest(
            bad_manifest_path);
      },
      "unexpected manifest key"));
  remove_tree(root);
}

static bool directories_equal(std::filesystem::path const& lhs,
                              std::filesystem::path const& rhs) {
  std::set<std::string> names;
  for (auto const& entry : std::filesystem::directory_iterator(lhs)) {
    names.insert(entry.path().filename().native());
  }
  std::set<std::string> other;
  for (auto const& entry : std::filesystem::directory_iterator(rhs)) {
    other.insert(entry.path().filename().native());
  }
  if (names != other) return false;
  for (auto const& name : names) {
    if (read(lhs / name) != read(rhs / name)) return false;
  }
  return true;
}

static void golden_test() {
  auto value = mixed_fixture();
  auto band = band_for(value, 1);
  auto output =
      larch::test::unique_temp_path("ti2_band_export_golden", ".raw");
  remove_tree(output);
  (void)larch::export_grammar_topology_band_raw(
      value.source, value.grammar, value.patterns, {}, band,
      export_options(value, band, output));
  CHECK(directories_equal(output, LARCH_TOPOLOGY_BAND_EXPORT_FIXTURE));
  remove_tree(output);
}

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view{argv[1]} == "emit") {
    auto value = mixed_fixture();
    auto workers = static_cast<std::size_t>(std::stoull(argv[3]));
    auto band = band_for(value, workers);
    auto result = larch::export_grammar_topology_band_raw(
        value.source, value.grammar, value.patterns, {}, band,
        export_options(value, band, argv[2]));
    std::println("semantic_data_sha256={}", result.semantic_data_sha256);
    std::println("provenance_id={}", result.provenance_id);
    return 0;
  }
  if (argc != 2) return 2;
  std::string_view mode{argv[1]};
  if (mode == "synthetic_export") synthetic_export_test();
  else if (mode == "worker_parity") worker_parity_test();
  else if (mode == "no_replace") no_replace_test();
  else if (mode == "mismatch") mismatch_test();
  else if (mode == "rejection_matrix") rejection_matrix_test();
  else if (mode == "collision") collision_test();
  else if (mode == "arity_roundtrip") arity_roundtrip_test();
  else if (mode == "replay_contract") replay_contract_test();
  else if (mode == "golden") golden_test();
  else return 2;
  std::println("PASS {}", mode);
  return 0;
}
