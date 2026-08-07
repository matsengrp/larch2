#include <larch/grammar_topology_replay.hpp>

#include <larch/fasta.hpp>
#include <larch/grammar_topology_enumerator.hpp>
#include <larch/io_util.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace larch {
namespace {

using values = std::map<std::string, std::string>;

std::uint64_t parse_u64(std::string_view token, std::string_view field) {
  if (token.empty() || (token.size() > 1 && token.front() == '0')) {
    throw std::runtime_error("score-band replay: noncanonical integer " +
                             std::string(field));
  }
  std::uint64_t result = 0;
  auto [end, error] =
      std::from_chars(token.data(), token.data() + token.size(), result);
  if (error != std::errc{} || end != token.data() + token.size()) {
    throw std::runtime_error("score-band replay: invalid integer " +
                             std::string(field));
  }
  return result;
}

bool is_sha256(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](char byte) {
           return (byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f');
         });
}

std::string take(values& source, std::string const& key) {
  auto found = source.find(key);
  if (found == source.end()) {
    throw std::runtime_error("score-band replay: missing manifest key " + key);
  }
  auto value = std::move(found->second);
  source.erase(found);
  if (value.empty()) {
    throw std::runtime_error("score-band replay: empty manifest value " + key);
  }
  return value;
}

std::string take_sha(values& source, std::string const& key) {
  auto value = take(source, key);
  if (!is_sha256(value)) {
    throw std::runtime_error("score-band replay: invalid SHA-256 " + key);
  }
  return value;
}

std::filesystem::path resolved(topology_score_band_replay_manifest const& m,
                               std::filesystem::path const& value) {
  if (value.is_absolute()) return value;
  return m.source_path.parent_path() / value;
}

void require_equal(std::string_view field, std::string const& expected,
                   std::string const& actual) {
  if (expected != actual) {
    throw std::runtime_error("score-band replay: " + std::string(field) +
                             " mismatch: " + actual + " != " + expected);
  }
}

std::vector<std::string_view> split_tabs(std::string const& line) {
  std::vector<std::string_view> fields;
  std::string_view remaining{line};
  for (;;) {
    auto tab = remaining.find('\t');
    if (tab == std::string_view::npos) {
      fields.push_back(remaining);
      return fields;
    }
    fields.push_back(remaining.substr(0, tab));
    remaining.remove_prefix(tab + 1);
  }
}

std::map<std::uint64_t, std::uint64_t> read_prior_histogram(
    std::filesystem::path const& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error(
        "score-band replay: cannot read prior histogram");
  }
  std::string line;
  if (!std::getline(input, line) ||
      line != "score\treduced_864_count\tfull_575168_count") {
    throw std::runtime_error(
        "score-band replay: prior histogram header mismatch");
  }
  std::map<std::uint64_t, std::uint64_t> result;
  std::size_t row = 1;
  while (std::getline(input, line)) {
    ++row;
    auto fields = split_tabs(line);
    if (fields.size() != 3) {
      throw std::runtime_error(
          "score-band replay: prior histogram row width mismatch");
    }
    auto score = parse_u64(fields[0], "prior histogram score");
    (void)parse_u64(fields[1], "prior reduced count");
    auto count = parse_u64(fields[2], "prior full count");
    if (!result.emplace(score, count).second) {
      throw std::runtime_error(
          "score-band replay: duplicate prior histogram score");
    }
  }
  if (!input.eof() || result.empty()) {
    throw std::runtime_error(
        "score-band replay: incomplete prior histogram");
  }
  return result;
}

std::vector<topology_score_band_replay_minimum> read_prior_minima(
    std::filesystem::path const& path, std::uint64_t baseline) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("score-band replay: cannot read prior minima");
  }
  std::string line;
  if (!std::getline(input, line) ||
      line != "class\tordinal\texact_score\ttopology_sha256\tcanonical_dag_"
              "semantic_sha256\tcanonical_dag_clades_sha256\tcanonical_dag_"
              "productions_sha256\tartifact\tartifact_sha256") {
    throw std::runtime_error(
        "score-band replay: prior minima header mismatch");
  }
  std::vector<topology_score_band_replay_minimum> result;
  while (std::getline(input, line)) {
    auto fields = split_tabs(line);
    if (fields.size() != 9) {
      throw std::runtime_error(
          "score-band replay: prior minima row width mismatch");
    }
    topology_score_band_replay_minimum row;
    row.class_index = parse_u64(fields[0], "prior minimum class");
    row.ordinal = parse_u64(fields[1], "prior minimum ordinal");
    row.exact_score = parse_u64(fields[2], "prior minimum score");
    if (row.exact_score != baseline) {
      throw std::runtime_error(
          "score-band replay: prior minimum score differs from baseline");
    }
    row.topology_sha256 = fields[3];
    row.canonical_dag_semantic_sha256 = fields[4];
    row.canonical_dag_clades_sha256 = fields[5];
    row.canonical_dag_productions_sha256 = fields[6];
    row.artifact_sha256 = fields[8];
    for (auto const& [field, digest] :
         std::array<std::pair<std::string_view, std::string const*>, 5>{
             {{"prior topology SHA-256", &row.topology_sha256},
              {"prior semantic DAG SHA-256",
               &row.canonical_dag_semantic_sha256},
              {"prior clade DAG SHA-256", &row.canonical_dag_clades_sha256},
              {"prior production DAG SHA-256",
               &row.canonical_dag_productions_sha256},
              {"prior artifact SHA-256", &row.artifact_sha256}}}) {
      if (!is_sha256(*digest)) {
        throw std::runtime_error("score-band replay: invalid " +
                                 std::string(field));
      }
    }
    if (fields[7].empty()) {
      throw std::runtime_error(
          "score-band replay: empty prior minimum artifact path");
    }
    result.push_back(std::move(row));
  }
  if (!input.eof() || result.empty()) {
    throw std::runtime_error("score-band replay: incomplete prior minima");
  }
  for (std::size_t index = 0; index < result.size(); ++index) {
    if (result[index].class_index != index) {
      throw std::runtime_error(
          "score-band replay: noncanonical prior minimum class order");
    }
  }
  auto ordinals = result;
  std::ranges::sort(ordinals, {},
                    &topology_score_band_replay_minimum::ordinal);
  if (std::adjacent_find(
          ordinals.begin(), ordinals.end(),
          [](auto const& lhs, auto const& rhs) {
            return lhs.ordinal == rhs.ordinal;
          }) != ordinals.end()) {
    throw std::runtime_error(
        "score-band replay: duplicate prior minimum ordinal");
  }
  return result;
}

std::string parsed_reference(std::filesystem::path const& path) {
  auto file_bytes = read_file(path.native());
  std::string bytes{file_bytes.begin(), file_bytes.end()};
  std::string sequence;
  if (!bytes.empty() && bytes.front() == '>') {
    auto entries = read_fasta(path.native());
    if (entries.size() != 1) {
      throw std::runtime_error(
          "score-band replay: reference must contain one FASTA record");
    }
    sequence = std::move(entries.front().sequence);
  } else {
    for (unsigned char byte : bytes) {
      if (byte == '\n' || byte == '\r' || byte == ' ' || byte == '\t') {
        continue;
      }
      sequence.push_back(static_cast<char>(std::toupper(byte)));
    }
  }
  if (!std::ranges::all_of(sequence, [](char base) {
        return base == 'A' || base == 'C' || base == 'G' || base == 'T';
      })) {
    throw std::runtime_error(
        "score-band replay: parsed reference is not strict A/C/G/T");
  }
  return sequence;
}

}  // namespace

topology_score_band_replay_manifest read_topology_score_band_replay_manifest(
    std::filesystem::path const& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("score-band replay: cannot read manifest");
  }
  std::string line;
  if (!std::getline(input, line) || line != "key\tvalue") {
    throw std::runtime_error("score-band replay: manifest header mismatch");
  }
  values parsed;
  while (std::getline(input, line)) {
    auto fields = split_tabs(line);
    if (fields.size() != 2 || fields[0].empty() || fields[1].empty() ||
        !parsed.emplace(std::string(fields[0]), std::string(fields[1])).second) {
      throw std::runtime_error(
          "score-band replay: malformed or duplicate manifest row");
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("score-band replay: manifest read failure");
  }

  topology_score_band_replay_manifest result;
  result.source_path = path;
  result.schema_name = take(parsed, "schema_name");
  result.schema_version =
      parse_u64(take(parsed, "schema_version"), "schema_version");
  if (result.schema_name != "larch-topology-score-band-replay-v1" ||
      result.schema_version != 1) {
    throw std::runtime_error("score-band replay: schema mismatch");
  }
  result.grammar_construction = take(parsed, "grammar_construction");
  result.universe = take(parsed, "universe");
  result.provider_input_sha256 = take_sha(parsed, "provider_input_sha256");
  result.provider_legacy_dag_semantic_sha256 =
      take_sha(parsed, "provider_legacy_dag_semantic_sha256");
  result.provider_stored_history_parsimony_min = parse_u64(
      take(parsed, "provider_stored_history_parsimony_min"),
      "provider_stored_history_parsimony_min");
  result.seed_tree_input_sha256 =
      take_sha(parsed, "seed_tree_input_sha256");
  result.reference_input_sha256 =
      take_sha(parsed, "reference_input_sha256");
  result.semantics.alignment_sha256 = take_sha(parsed, "alignment_sha256");
  result.semantics.ambiguity_policy = take(parsed, "ambiguity_policy");
  result.semantics.grammar_digest = take_sha(parsed, "grammar_digest");
  result.semantics.grammar_semantic_digest =
      take_sha(parsed, "grammar_semantic_digest");
  result.semantics.input_content_sha256 =
      take_sha(parsed, "input_content_sha256");
  result.semantics.parsimony_model = take(parsed, "parsimony_model");
  result.semantics.reference_sha256 = take_sha(parsed, "reference_sha256");
  result.semantics.site_pattern_digest =
      take_sha(parsed, "site_pattern_digest");
  result.semantics.taxon_labels_sha256 =
      take_sha(parsed, "taxon_labels_sha256");
  result.semantics.ua_scoring = take(parsed, "ua_scoring");
  result.grammar_topology_count = parse_u64(
      take(parsed, "grammar_topology_count"), "grammar_topology_count");
  result.score_baseline =
      parse_u64(take(parsed, "score_baseline"), "score_baseline");
  result.sankoff_verification_stride = parse_u64(
      take(parsed, "sankoff_verification_stride"),
      "sankoff_verification_stride");
  result.sankoff_verified_topology_count = parse_u64(
      take(parsed, "sankoff_verified_topology_count"),
      "sankoff_verified_topology_count");
  if (result.sankoff_verification_stride == 0 ||
      result.sankoff_verified_topology_count == 0) {
    throw std::runtime_error(
        "score-band replay: Sankoff audit policy must be nonzero");
  }
  result.prior_census_report_path = take(parsed, "prior_census_report_path");
  result.prior_census_report_sha256 =
      take_sha(parsed, "prior_census_report_sha256");
  result.prior_histogram_path = take(parsed, "prior_histogram_path");
  result.prior_histogram_sha256 =
      take_sha(parsed, "prior_histogram_sha256");
  result.prior_minima_path = take(parsed, "prior_minima_path");
  result.prior_minima_sha256 = take_sha(parsed, "prior_minima_sha256");
  if (!parsed.empty()) {
    throw std::runtime_error("score-band replay: unexpected manifest key " +
                             parsed.begin()->first);
  }
  return result;
}

topology_score_band_replay_preflight
validate_topology_score_band_replay_preflight(
    topology_score_band_replay_manifest const& manifest,
    phylo_dag& provider_dag, clade_grammar const& grammar,
    site_pattern_set const& patterns, chart_options const& chart_options,
    std::filesystem::path const& provider_input_path,
    std::filesystem::path const& reference_input_path,
    std::filesystem::path const& seed_tree_input_path) {
  topology_score_band_replay_preflight result;
  result.manifest_sha256 =
      topology_score_band_artifact_sha256(manifest.source_path);
  result.provider_input_sha256 =
      topology_score_band_artifact_sha256(provider_input_path);
  result.reference_input_sha256 =
      topology_score_band_artifact_sha256(reference_input_path);
  result.seed_tree_input_sha256 =
      topology_score_band_artifact_sha256(seed_tree_input_path);
  result.provider_legacy_dag_semantic_sha256 =
      topology_score_band_provider_legacy_dag_semantic_sha256(
          provider_dag, chart_options.score_ua_edge);
  result.provider_stored_history_parsimony_min =
      topology_score_band_provider_stored_history_parsimony_min(
          provider_dag, chart_options.score_ua_edge);
  result.derived = derive_topology_score_band_semantics(
      provider_dag, grammar, patterns, chart_options,
      manifest.grammar_construction);
  auto rebuilt_patterns = build_site_patterns(provider_dag, grammar);
  auto rebuilt = derive_topology_score_band_semantics(
      provider_dag, grammar, rebuilt_patterns, chart_options,
      manifest.grammar_construction);
  require_equal("provided/source alignment", result.derived.alignment_sha256,
                rebuilt.alignment_sha256);
  require_equal("provided/source site patterns",
                result.derived.site_pattern_digest,
                rebuilt.site_pattern_digest);
  require_equal("provided/source input content",
                result.derived.input_content_sha256,
                rebuilt.input_content_sha256);
  if (parsed_reference(reference_input_path) !=
      get_reference_sequence(provider_dag)) {
    throw std::runtime_error(
        "score-band replay: reference artifact/provider mismatch");
  }
  grammar_topology_enumerator enumerator(grammar);
  if (enumerator.topology_count() != manifest.grammar_topology_count) {
    throw std::runtime_error(
        "score-band replay: grammar topology count mismatch");
  }

  require_equal("provider input", manifest.provider_input_sha256,
                result.provider_input_sha256);
  require_equal("provider legacy DAG semantic",
                manifest.provider_legacy_dag_semantic_sha256,
                result.provider_legacy_dag_semantic_sha256);
  if (manifest.provider_stored_history_parsimony_min !=
      result.provider_stored_history_parsimony_min) {
    throw std::runtime_error(
        "score-band replay: provider stored-history minimum mismatch");
  }
  require_equal("seed-tree input", manifest.seed_tree_input_sha256,
                result.seed_tree_input_sha256);
  require_equal("reference input", manifest.reference_input_sha256,
                result.reference_input_sha256);
  require_equal("alignment", manifest.semantics.alignment_sha256,
                result.derived.alignment_sha256);
  require_equal("ambiguity policy", manifest.semantics.ambiguity_policy,
                result.derived.ambiguity_policy);
  require_equal("ordered grammar", manifest.semantics.grammar_digest,
                result.derived.grammar_digest);
  require_equal("semantic grammar",
                manifest.semantics.grammar_semantic_digest,
                result.derived.grammar_semantic_digest);
  require_equal("input content", manifest.semantics.input_content_sha256,
                result.derived.input_content_sha256);
  require_equal("parsimony model", manifest.semantics.parsimony_model,
                result.derived.parsimony_model);
  require_equal("reference", manifest.semantics.reference_sha256,
                result.derived.reference_sha256);
  require_equal("site pattern", manifest.semantics.site_pattern_digest,
                result.derived.site_pattern_digest);
  require_equal("taxon labels", manifest.semantics.taxon_labels_sha256,
                result.derived.taxon_labels_sha256);
  require_equal("UA scoring", manifest.semantics.ua_scoring,
                result.derived.ua_scoring);
  return result;
}

void validate_topology_score_band_replay_census(
    topology_score_band_replay_manifest const& manifest,
    grammar_topology_census_result const& census) {
  if (census.expected_topology_count != manifest.grammar_topology_count ||
      census.scored_topology_count != manifest.grammar_topology_count ||
      census.ordinal_scores.size() != manifest.grammar_topology_count) {
    throw std::runtime_error(
        "score-band replay: fresh census count/ledger mismatch");
  }
  if (census.optimum != manifest.score_baseline) {
    throw std::runtime_error("score-band replay: fresh optimum mismatch");
  }
  if (census.sankoff_verified_topology_count !=
      manifest.sankoff_verified_topology_count) {
    throw std::runtime_error(
        "score-band replay: Sankoff audit coverage mismatch");
  }
  for (auto const& [path, expected] :
       std::array<std::pair<std::filesystem::path, std::string>, 3>{
           {{resolved(manifest, manifest.prior_census_report_path),
             manifest.prior_census_report_sha256},
            {resolved(manifest, manifest.prior_histogram_path),
             manifest.prior_histogram_sha256},
            {resolved(manifest, manifest.prior_minima_path),
             manifest.prior_minima_sha256}}}) {
    require_equal("prior evidence artifact", expected,
                  topology_score_band_artifact_sha256(path));
  }
  auto prior_histogram =
      read_prior_histogram(resolved(manifest, manifest.prior_histogram_path));
  if (prior_histogram != census.score_histogram) {
    throw std::runtime_error(
        "score-band replay: fresh/prior score histogram mismatch");
  }
  auto prior_minima = read_prior_minima(
      resolved(manifest, manifest.prior_minima_path), manifest.score_baseline);
  std::vector<std::uint64_t> prior_ordinals;
  prior_ordinals.reserve(prior_minima.size());
  for (auto const& minimum : prior_minima) {
    prior_ordinals.push_back(minimum.ordinal);
  }
  std::sort(prior_ordinals.begin(), prior_ordinals.end());
  auto actual_ordinals = census.optimal_ordinals;
  std::sort(actual_ordinals.begin(), actual_ordinals.end());
  if (prior_ordinals != actual_ordinals) {
    throw std::runtime_error(
        "score-band replay: fresh/prior minimum ordinal mismatch");
  }
}

void validate_topology_score_band_replay_minima(
    topology_score_band_replay_manifest const& manifest,
    std::span<topology_score_band_replay_minimum const> minima) {
  auto prior = read_prior_minima(
      resolved(manifest, manifest.prior_minima_path), manifest.score_baseline);
  if (prior.size() != minima.size()) {
    throw std::runtime_error(
        "score-band replay: fresh/prior minimum witness count mismatch");
  }
  for (std::size_t index = 0; index < prior.size(); ++index) {
    auto const& expected = prior[index];
    auto const& actual = minima[index];
    if (actual.class_index != expected.class_index ||
        actual.ordinal != expected.ordinal ||
        actual.exact_score != expected.exact_score ||
        actual.topology_sha256 != expected.topology_sha256 ||
        actual.canonical_dag_semantic_sha256 !=
            expected.canonical_dag_semantic_sha256 ||
        actual.canonical_dag_clades_sha256 !=
            expected.canonical_dag_clades_sha256 ||
        actual.canonical_dag_productions_sha256 !=
            expected.canonical_dag_productions_sha256 ||
        actual.artifact_sha256 != expected.artifact_sha256) {
      throw std::runtime_error(
          "score-band replay: fresh/prior minimum witness identity mismatch "
          "at class " +
          std::to_string(index));
    }
  }
}

}  // namespace larch
