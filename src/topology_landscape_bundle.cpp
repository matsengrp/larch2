#include <larch/topology_landscape_bundle.hpp>

#include <larch/sha256.hpp>
#include <larch/rooted_topology.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <system_error>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace larch::topology_landscape {
namespace {

constexpr std::array members{
    "SHA256SUMS", "analysis_policy.tsv", "artifact_provenance.tsv",
    "completeness.tsv", "files.tsv", "grammar_topology_provenance.tsv",
    "landscape_semantics.tsv", "manifest.tsv", "move_witnesses.tsv",
    "search_run.tsv", "topology_edges.tsv", "trees.tsv"};

constexpr std::array members_v2{
    "SHA256SUMS", "analysis_policy.tsv", "artifact_provenance.tsv",
    "completeness.tsv", "files.tsv", "grammar_topology_provenance.tsv",
    "landscape_semantics.tsv", "manifest.tsv", "move_witnesses.tsv",
    "score_census.tsv", "search_run.tsv", "topology_edges.tsv", "trees.tsv"};

constexpr std::array raw_members{
    "SHA256SUMS", "files.tsv", "manifest.tsv", "provenance.tsv",
    "score_census.tsv", "selected_topologies.tsv", "semantics.tsv"};

using cell = std::optional<std::string>;
using row = std::vector<cell>;

struct table {
  std::vector<std::string> header;
  std::vector<row> rows;
};

std::string hash(std::string_view value) {
  sha256 digest;
  digest.update(value);
  return digest.hex_digest();
}

void reject(validation& result, std::string code, std::string file,
            std::size_t line, std::string detail) {
  result.problems.push_back(
      {std::move(code), std::move(file), line, std::move(detail)});
}

std::optional<std::string> slurp(std::filesystem::path const& path,
                                 validation& result) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    reject(result, "directory_layout", path.filename().native(), 0,
           "cannot open required file");
    return std::nullopt;
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

std::vector<std::string_view> split(std::string_view input, char separator) {
  std::vector<std::string_view> result;
  std::size_t begin = 0;
  while (true) {
    auto end = input.find(separator, begin);
    result.push_back(input.substr(begin, end == std::string_view::npos
                                            ? input.size() - begin
                                            : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}

std::vector<std::string_view> file_lines(std::string_view input) {
  auto result = split(input, '\n');
  if (!result.empty() && result.back().empty()) result.pop_back();
  return result;
}

bool canonical_file(std::string_view filename, std::string_view input,
                    validation& result) {
  if (input.empty() || input.back() != '\n' ||
      (input.size() > 1 && input[input.size() - 2] == '\n') ||
      input.contains('\r') || input.contains('\0')) {
    reject(result, "canonical_encoding", std::string(filename), 0,
           "file must be nonempty LF-only bytes with exactly one final LF");
    return false;
  }
  return true;
}

int upper_hex(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<cell> decode(std::string_view input, std::string_view filename,
                           std::size_t line, validation& result) {
  if (input == "-") return cell{};
  if (input.empty()) {
    reject(result, "canonical_encoding", std::string(filename), line,
           "empty cells and trailing TABs are forbidden; use - for null");
    return std::nullopt;
  }
  auto const literal_hyphen = input == "%2D";
  std::string output;
  for (std::size_t i = 0; i < input.size();) {
    unsigned char value = static_cast<unsigned char>(input[i]);
    if (input[i] == '%') {
      if (i + 2 >= input.size() || upper_hex(input[i + 1]) < 0 ||
          upper_hex(input[i + 2]) < 0) {
        reject(result, "canonical_encoding", std::string(filename), line,
               "percent escape must use two uppercase hex digits");
        return std::nullopt;
      }
      value = static_cast<unsigned char>((upper_hex(input[i + 1]) << 4) |
                                         upper_hex(input[i + 2]));
      if (value >= 0x21 && value <= 0x7e && value != '%' &&
          !(literal_hyphen && value == '-')) {
        reject(result, "canonical_encoding", std::string(filename), line,
               "safe raw byte was unnecessarily escaped");
        return std::nullopt;
      }
      i += 3;
    } else {
      if (value < 0x21 || value > 0x7e || value == '%') {
        reject(result, "canonical_encoding", std::string(filename), line,
               "unsafe byte was not percent escaped");
        return std::nullopt;
      }
      ++i;
    }
    output.push_back(static_cast<char>(value));
  }
  return cell{std::move(output)};
}

std::optional<table> read_table(std::string_view filename, std::string_view input,
                                std::vector<std::string> header,
                                validation& result, bool lexical_rows = true) {
  if (!canonical_file(filename, input, result)) return std::nullopt;
  auto lines = file_lines(input);
  if (lines.empty()) return std::nullopt;
  auto actual_header = split(lines.front(), '\t');
  if (actual_header.size() != header.size() ||
      !std::ranges::equal(actual_header, header)) {
    reject(result, "table_shape", std::string(filename), 1, "header mismatch");
    return std::nullopt;
  }
  table output{std::move(header), {}};
  for (std::size_t i = 1; i < lines.size(); ++i) {
    if (lines[i].empty() || lines[i].starts_with('#') ||
        (lexical_rows && i > 1 && !(lines[i - 1] < lines[i]))) {
      reject(result, "canonical_encoding", std::string(filename), i + 1,
             "rows must be nonblank, unique, and byte sorted");
      return std::nullopt;
    }
    auto raw = split(lines[i], '\t');
    if (raw.size() != output.header.size()) {
      reject(result, "table_shape", std::string(filename), i + 1,
             "wrong field count");
      return std::nullopt;
    }
    row decoded;
    for (auto value : raw) {
      auto parsed = decode(value, filename, i + 1, result);
      if (!parsed) return std::nullopt;
      decoded.push_back(std::move(*parsed));
    }
    output.rows.push_back(std::move(decoded));
  }
  return output;
}

std::optional<std::map<std::string, std::string>> key_values(
    std::string_view filename, std::string_view input,
    std::set<std::string> const& required, validation& result) {
  auto data = read_table(filename, input, {"key", "value"}, result);
  if (!data) return std::nullopt;
  std::map<std::string, std::string> output;
  for (std::size_t i = 0; i < data->rows.size(); ++i) {
    auto const& item = data->rows[i];
    if (!item[0] || !item[1] || !required.contains(*item[0]) ||
        !output.emplace(*item[0], *item[1]).second) {
      reject(result, "table_shape", std::string(filename), i + 2,
             "key set contains a null, unknown, or duplicate key");
    }
  }
  if (output.size() != required.size()) {
    reject(result, "table_shape", std::string(filename), 0,
           "required key set is incomplete");
    return std::nullopt;
  }
  return output;
}

template <typename Integer>
std::optional<Integer> number(std::string_view value) {
  if (value.empty() || (value.size() > 1 && value[0] == '0') || value == "-0" ||
      (value.size() > 2 && value[0] == '-' && value[1] == '0')) return std::nullopt;
  Integer result{};
  auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  return error == std::errc{} && last == value.data() + value.size()
             ? std::optional{result}
             : std::nullopt;
}

bool hash_text(std::string_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

struct tree_shape {
  std::uint64_t leaves{};
  std::uint64_t root_arity{};
  std::set<std::string> labels;
};

std::optional<tree_shape> tree_syntax(std::string_view text) {
  try {
    auto tree = topology::parse_canonical_tree(text);
    if (tree.is_leaf()) return std::nullopt;
    auto labels = topology::taxa(tree);
    return tree_shape{static_cast<std::uint64_t>(labels.size()),
                      static_cast<std::uint64_t>(tree.children.size()),
                      std::set<std::string>(labels.begin(), labels.end())};
  } catch (std::invalid_argument const&) {
    return std::nullopt;
  }
}

std::string identity(std::string_view domain, std::string_view bytes) {
  return hash(std::string(domain) + std::string(bytes));
}

std::string taxon_set_identity(std::set<std::string> const& labels) {
  std::string input = "topology-landscape.taxon-label-set.v1\n";
  for (auto const& label : labels) {
    input += std::to_string(label.size()) + ":" + label;
  }
  return hash(input);
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> parse_score_view(
    std::string_view value) {
  constexpr std::string_view prefix =
      "absolute_score_interval_inclusive_v1:";
  if (!value.starts_with(prefix)) return std::nullopt;
  value.remove_prefix(prefix.size());
  auto separator = value.find(':');
  if (separator == std::string_view::npos ||
      value.find(':', separator + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  auto low = number<std::uint64_t>(value.substr(0, separator));
  auto high = number<std::uint64_t>(value.substr(separator + 1));
  if (!low || !high || *low > *high ||
      *high > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return std::pair{*low, *high};
}

std::optional<std::uint64_t> data_row_count(std::string_view bytes) {
  auto line_feeds = static_cast<std::uint64_t>(
      std::ranges::count(bytes, '\n'));
  if (line_feeds == 0) return std::nullopt;
  return line_feeds - 1;
}

bool parse_score_census(
    std::string_view bytes, std::uint64_t grammar_count,
    std::pair<std::uint64_t, std::uint64_t> view, validation& result,
    std::vector<std::uint64_t>& scores,
    std::map<std::uint64_t, std::uint64_t>& selected) {
  constexpr std::string_view header = "grammar_ordinal\tabsolute_score\n";
  if (!canonical_file("score_census.tsv", bytes, result)) return false;
  if (!bytes.starts_with(header)) {
    reject(result, "table_shape", "score_census.tsv", 1, "header mismatch");
    return false;
  }
  std::size_t begin = header.size();
  std::uint64_t expected_ordinal = 0;
  std::size_t line = 2;
  while (begin < bytes.size()) {
    auto end = bytes.find('\n', begin);
    if (end == std::string_view::npos) end = bytes.size();
    auto row_bytes = bytes.substr(begin, end - begin);
    auto separator = row_bytes.find('\t');
    bool shape = separator != std::string_view::npos &&
                 row_bytes.find('\t', separator + 1) == std::string_view::npos;
    auto ordinal = shape
        ? number<std::uint64_t>(row_bytes.substr(0, separator))
        : std::nullopt;
    auto score = shape
        ? number<std::uint64_t>(row_bytes.substr(separator + 1))
        : std::nullopt;
    if (!shape || !ordinal || !score || *ordinal != expected_ordinal ||
        *score > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
      reject(result, shape ? "grammar_mapping" : "table_shape",
             "score_census.tsv", line,
             shape ? "ordinal must be the next contiguous numeric value"
                   : "wrong field count");
    } else {
      scores.push_back(*score);
      if (*score >= view.first && *score <= view.second) {
        selected.emplace(*ordinal, *score);
      }
    }
    ++expected_ordinal;
    ++line;
    begin = end + 1;
  }
  if (scores.size() != grammar_count) {
    reject(result, "grammar_mapping", "score_census.tsv", 0,
           "row count differs from grammar_topology_count");
  }
  return result.problems.empty();
}

bool publish_no_replace(std::filesystem::path const& staging,
                        std::filesystem::path const& output,
                        std::error_code& error) {
#if defined(__linux__)
  if (::syscall(SYS_renameat2, static_cast<long>(AT_FDCWD), staging.c_str(),
                static_cast<long>(AT_FDCWD), output.c_str(),
                static_cast<unsigned int>(RENAME_NOREPLACE)) == 0) {
    error.clear();
    return true;
  }
  error = std::error_code(errno, std::generic_category());
  return false;
#else
  // The preflight is not race-free on platforms without renameat2.  Refuse to
  // advertise atomic no-replace semantics there until an equivalent primitive
  // is wired for that platform.
  error = std::make_error_code(std::errc::operation_not_supported);
  return false;
#endif
}

}  // namespace

std::string topology_digest(std::string_view canonical_bytes) {
  return identity("topology-landscape.rooted-labelled-topology.v1\n",
                  canonical_bytes);
}

validation validate_bundle(std::filesystem::path const& directory) {
  validation result;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    reject(result, "directory_layout", directory.native(), 0,
           "bundle is not a directory");
    return result;
  }
  std::set<std::string> actual;
  for (auto const& entry : std::filesystem::directory_iterator(directory, error)) {
    auto name = entry.path().filename().native();
    if (entry.is_symlink(error) || !entry.is_regular_file(error)) {
      reject(result, "unsafe_path", name, 0, "bundle members must be regular files");
    }
    actual.insert(name);
  }
  std::set<std::string> expected(members.begin(), members.end());
  std::set<std::string> expected_v2(members_v2.begin(), members_v2.end());
  bool const version_2 = actual == expected_v2;
  if (actual != expected && !version_2) {
    reject(result, "directory_layout", directory.native(), 0,
           "member set matches neither supported bundle version");
    return result;
  }
  std::map<std::string, std::string> bytes;
  for (auto const& name : actual) {
    if (auto value = slurp(directory / name, result)) bytes.emplace(name, *value);
  }
  if (!result) return result;

  if (canonical_file("SHA256SUMS", bytes.at("SHA256SUMS"), result)) {
    std::string previous;
    std::set<std::string> seen;
    auto ledger = file_lines(bytes.at("SHA256SUMS"));
    for (std::size_t i = 0; i < ledger.size(); ++i) {
      auto values = split(ledger[i], '\t');
      if (values.size() != 2 || !hash_text(values[0]) ||
          !bytes.contains(std::string(values[1])) || values[1] == "SHA256SUMS" ||
          (i && !(previous < values[1])) ||
          !seen.emplace(values[1]).second || hash(bytes.at(std::string(values[1]))) != values[0]) {
        reject(result, "checksum_mismatch", "SHA256SUMS", i + 1,
               "ledger row is malformed, unsorted, duplicate, or mismatching");
      }
      if (values.size() == 2) previous = values[1];
    }
    if (seen.size() != actual.size() - 1) {
      reject(result, "checksum_mismatch", "SHA256SUMS", 0,
             "ledger coverage is incomplete");
    }
  }
  if (!result) return result;

  static std::set<std::string> const manifest_fields{
      "analysis_policy_hash", "artifact_provenance_id", "experiment_id",
      "files_row_count", "files_sha256", "landscape_semantics_hash",
      "schema_name", "schema_version", "search_run_id"};
  std::set<std::string> landscape_fields{
      "alignment_sha256", "alphabet", "ambiguity_policy",
      "canonical_topology_encoding", "digest_algorithm", "endpoint_filtration",
      "grammar_construction", "grammar_digest", "input_content_sha256",
      "move_family", "move_relation", "move_symmetry", "parsimony_model",
      "polytomy_policy", "reference_sha256", "root_policy", "score_baseline",
      "taxon_labels_sha256", "taxon_order", "universe", "ua_scoring"};
  if (version_2) {
    landscape_fields.insert("grammar_semantic_digest");
    landscape_fields.insert("grammar_topology_count");
    landscape_fields.insert("site_pattern_digest");
  }
  static std::set<std::string> const analysis_fields{
      "edge_weight", "face_policy", "hodge_metric", "landscape_semantics_hash",
      "null_family", "numeric_policy", "score_view"};
  static std::set<std::string> const search_fields{
      "budget", "initial_state", "landscape_semantics_hash", "mode", "seed",
      "trace_schema"};
  static std::set<std::string> const provenance_fields{
      "command", "derivation_ref", "input_artifact_sha256", "output_data_sha256",
      "producer_commit", "producer_dirty", "producer_repository", "toolchain",
      "worker_count"};

  auto manifest = key_values("manifest.tsv", bytes.at("manifest.tsv"), manifest_fields, result);
  auto landscape = key_values("landscape_semantics.tsv", bytes.at("landscape_semantics.tsv"), landscape_fields, result);
  auto analysis = key_values("analysis_policy.tsv", bytes.at("analysis_policy.tsv"), analysis_fields, result);
  auto search = key_values("search_run.tsv", bytes.at("search_run.tsv"), search_fields, result);
  auto provenance = key_values("artifact_provenance.tsv", bytes.at("artifact_provenance.tsv"), provenance_fields, result);
  if (!manifest || !landscape || !analysis || !search || !provenance) return result;

  auto suffix = version_2 ? "v2\n" : "v1\n";
  result.landscape_semantics_hash = identity(
      std::string("topology-landscape.landscape-semantics.") + suffix,
      bytes.at("landscape_semantics.tsv"));
  result.analysis_policy_hash = identity(
      std::string("topology-landscape.analysis-policy.") + suffix,
      bytes.at("analysis_policy.tsv"));
  result.search_run_id = identity(
      std::string("topology-landscape.search-run.") + suffix,
      bytes.at("search_run.tsv"));
  result.artifact_provenance_id = identity(
      std::string("topology-landscape.artifact-provenance.") + suffix,
      bytes.at("artifact_provenance.tsv"));
  auto grammar_count = version_2
      ? number<std::uint64_t>(landscape->at("grammar_topology_count"))
      : std::optional<std::uint64_t>{};
  auto selected_view = version_2
      ? parse_score_view(analysis->at("score_view"))
      : std::optional<std::pair<std::uint64_t, std::uint64_t>>{};
  auto baseline_v2 = version_2
      ? number<std::uint64_t>(landscape->at("score_baseline"))
      : std::optional<std::uint64_t>{};
  bool identity_values_ok =
      landscape->at("digest_algorithm") == "sha256" &&
      landscape->at("canonical_topology_encoding") ==
          "rooted-labelled-length-grammar-v1" &&
      landscape->at("endpoint_filtration") == "maximum_endpoint_score_delta" &&
      landscape->at("taxon_order") == "unsigned_utf8_byte_order" &&
      landscape->at("root_policy") == "rooted_no_synthetic_ua" &&
      analysis->at("numeric_policy") == "exact_integer" &&
      (version_2 ? selected_view.has_value()
                 : analysis->at("score_view") == "absolute_and_delta") &&
      (version_2
           ? baseline_v2 &&
                 *baseline_v2 <=
                     static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())
           : number<std::int64_t>(landscape->at("score_baseline")).has_value()) &&
      hash_text(landscape->at("alignment_sha256")) &&
      hash_text(landscape->at("grammar_digest")) &&
      hash_text(landscape->at("input_content_sha256")) &&
      hash_text(landscape->at("reference_sha256")) &&
      hash_text(landscape->at("taxon_labels_sha256")) &&
      (!version_2 ||
       (grammar_count && *grammar_count > 0 &&
        hash_text(landscape->at("grammar_semantic_digest")) &&
        hash_text(landscape->at("site_pattern_digest")))) &&
      hash_text(provenance->at("input_artifact_sha256")) &&
      hash_text(provenance->at("output_data_sha256")) &&
      number<std::uint64_t>(provenance->at("worker_count")).has_value() &&
      (provenance->at("producer_dirty") == "true" ||
       provenance->at("producer_dirty") == "false" ||
       provenance->at("producer_dirty") == "not_applicable");
  if (!identity_values_ok) {
    reject(result, "identity_mismatch", "landscape_semantics.tsv", 0,
           "identity-file SHA, integer, or fixed-token grammar failed");
  }
  if (manifest->at("schema_name") !=
          (version_2 ? "topology-landscape-neutral-v2"
                     : "topology-landscape-neutral-v1") ||
      manifest->at("schema_version") != (version_2 ? "2" : "1")) {
    reject(result, "schema", "manifest.tsv", 0, "unsupported schema name/version");
  }
  if (manifest->at("landscape_semantics_hash") != result.landscape_semantics_hash ||
      manifest->at("analysis_policy_hash") != result.analysis_policy_hash ||
      manifest->at("search_run_id") != result.search_run_id ||
      manifest->at("artifact_provenance_id") != result.artifact_provenance_id ||
      analysis->at("landscape_semantics_hash") != result.landscape_semantics_hash ||
      search->at("landscape_semantics_hash") != result.landscape_semantics_hash) {
    reject(result, "identity_mismatch", "manifest.tsv", 0,
           "four-layer identity recomputation or cross-link failed");
  }

  auto file_index = read_table("files.tsv", bytes.at("files.tsv"),
      {"filename", "role", "row_count", "sha256"}, result);
  std::set<std::string> file_names;
  std::map<std::string, std::string> fixed_roles{
      {"analysis_policy.tsv", "identity"},
      {"artifact_provenance.tsv", "identity"},
      {"completeness.tsv", "claim"},
      {"grammar_topology_provenance.tsv", "table"},
      {"landscape_semantics.tsv", "identity"},
      {"move_witnesses.tsv", "table"},
      {"search_run.tsv", "identity"},
      {"topology_edges.tsv", "table"},
      {"trees.tsv", "table"}};
  if (version_2) fixed_roles.emplace("score_census.tsv", "claim");
  if (file_index) {
    auto count = number<std::uint64_t>(manifest->at("files_row_count"));
    if (!count || *count != file_index->rows.size() ||
        manifest->at("files_sha256") != hash(bytes.at("files.tsv"))) {
      reject(result, "checksum_mismatch", "manifest.tsv", 0,
             "files.tsv count or hash mismatch");
    }
    for (std::size_t i = 0; i < file_index->rows.size(); ++i) {
      auto const& item = file_index->rows[i];
      if (!item[0] || !item[1] || !item[2] || !item[3] ||
          !bytes.contains(*item[0]) || !file_names.insert(*item[0]).second ||
          !fixed_roles.contains(*item[0]) || fixed_roles.at(*item[0]) != *item[1]) {
        reject(result, "table_shape", "files.tsv", i + 2, "invalid payload row");
        continue;
      }
      auto rows = number<std::uint64_t>(*item[2]);
      auto actual_rows = data_row_count(bytes.at(*item[0]));
      if (!rows || !actual_rows || *item[3] != hash(bytes.at(*item[0])) ||
          *actual_rows != *rows) {
        reject(result, "checksum_mismatch", "files.tsv", i + 2,
               "payload row count or digest mismatch");
      }
    }
  }
  std::set<std::string> payload_names;
  for (auto const& [name, _] : fixed_roles) payload_names.insert(name);
  if (file_names != payload_names) {
    reject(result, "table_shape", "files.tsv", 0, "payload set mismatch");
  }
  if (!result) return result;

  std::vector<std::uint64_t> census_scores;
  std::map<std::uint64_t, std::uint64_t> selected_scores;
  if (version_2) {
    if (!parse_score_census(bytes.at("score_census.tsv"), *grammar_count,
                            *selected_view, result, census_scores,
                            selected_scores)) return result;
    if (selected_scores.empty()) {
      reject(result, "grammar_mapping", "score_census.tsv", 0,
             "score_view must select at least one census row");
    }
  }

  auto baseline = number<std::int64_t>(landscape->at("score_baseline"));
  struct scored_tree {
    std::int64_t score;
    std::int64_t delta;
    std::optional<std::uint64_t> ordinal;
  };
  std::map<std::string, scored_tree> trees;
  std::set<std::string> encodings;
  std::set<std::uint64_t> ordinals;
  std::map<std::uint64_t, std::uint64_t> tree_selected_scores;
  std::map<std::string, std::uint64_t> verified_counts{
      {"incremental_fitch_verification", 0},
      {"selected_grammar_sankoff_verification", 0},
      {"materialized_fitch_verification", 0},
      {"reload_fitch_verification", 0},
      {"tree_native_sankoff_verification", 0}};
  std::optional<std::set<std::string>> common_taxa;
  std::vector<std::string> tree_header{
      "topology_sha256", "scope_ref", "canonical_bytes", "replay_encoding",
      "replay_bytes", "split_digest", "leaf_count", "root_arity_summary",
      "absolute_score", "score_delta", "score_baseline_ref"};
  if (version_2) {
    tree_header.insert(tree_header.end(), {
        "incremental_fitch_score", "incremental_fitch_status",
        "selected_grammar_sankoff_score", "selected_grammar_sankoff_status",
        "selected_grammar_sankoff_oracle"});
  }
  tree_header.insert(tree_header.end(), {
      "fitch_score", "fitch_status", "reload_score", "reload_status",
      "sankoff_score", "sankoff_status", "sankoff_oracle", "grammar_ordinal",
      "local_production_witness_count", "exact_compatible_source_history_count",
      "original_occurrence_count"});
  auto tree_rows = read_table("trees.tsv", bytes.at("trees.tsv"),
                              std::move(tree_header), result);
  auto const fitch_score_column = version_2 ? 16U : 11U;
  auto const fitch_status_column = version_2 ? 17U : 12U;
  auto const reload_score_column = version_2 ? 18U : 13U;
  auto const reload_status_column = version_2 ? 19U : 14U;
  auto const sankoff_score_column = version_2 ? 20U : 15U;
  auto const sankoff_status_column = version_2 ? 21U : 16U;
  auto const sankoff_oracle_column = version_2 ? 22U : 17U;
  auto const ordinal_column = version_2 ? 23U : 18U;
  auto const first_count_column = version_2 ? 24U : 19U;
  if (tree_rows) {
    for (std::size_t i = 0; i < tree_rows->rows.size(); ++i) {
      auto const& item = tree_rows->rows[i];
      auto leaf_count = item[6] ? number<std::uint64_t>(*item[6]) : std::nullopt;
      auto arity = item[7] ? number<std::uint64_t>(*item[7]) : std::nullopt;
      auto score = item[8] ? number<std::int64_t>(*item[8]) : std::nullopt;
      auto delta = item[9] ? number<std::int64_t>(*item[9]) : std::nullopt;
      auto ordinal = item[ordinal_column]
          ? number<std::uint64_t>(*item[ordinal_column]) : std::nullopt;
      auto shape = item[2] ? tree_syntax(*item[2]) : std::nullopt;
      auto oracle_status_ok = [&](std::size_t score_index, std::size_t status_index) {
        if (!item[status_index]) return false;
        auto const& status = *item[status_index];
        auto parsed_score = item[score_index]
                                ? number<std::int64_t>(*item[score_index])
                                : std::nullopt;
        if (status == "verified") {
          return parsed_score && score && *parsed_score == *score;
        }
        if (status == "mismatch") {
          return parsed_score && score && *parsed_score != *score;
        }
        if (status == "not_checked" || status == "not_applicable") {
          return !item[score_index];
        }
        return false;
      };
      auto optional_count_ok = [&](std::size_t index) {
        return !item[index] || number<std::uint64_t>(*item[index]).has_value();
      };
      bool grammar_oracles = !version_2 ||
          (item[15] && oracle_status_ok(11, 12) &&
           oracle_status_ok(13, 14) &&
           ((*item[14] == "not_applicable") ==
            (*item[15] == "not_applicable")));
      bool good = item[0] && item[1] && item[2] && item[3] && item[4] && item[10] &&
                  item[sankoff_oracle_column] && baseline && leaf_count && arity &&
                  score && delta && shape && grammar_oracles &&
                  oracle_status_ok(fitch_score_column, fitch_status_column) &&
                  oracle_status_ok(reload_score_column, reload_status_column) &&
                  oracle_status_ok(sankoff_score_column, sankoff_status_column) &&
                  (!item[5] || hash_text(*item[5])) &&
                  optional_count_ok(first_count_column) &&
                  optional_count_ok(first_count_column + 1) &&
                  optional_count_ok(first_count_column + 2) &&
                  (!version_2 || ordinal);
      if (version_2 && item[15] && item[sankoff_oracle_column]) {
        good = good &&
            ((*item[15] == "not_applicable" &&
              *item[sankoff_oracle_column] == "not_applicable") ||
             *item[15] != *item[sankoff_oracle_column]);
      }
      good = good && *item[0] == topology_digest(*item[2]) &&
             *item[1] == result.landscape_semantics_hash &&
             *item[10] == result.landscape_semantics_hash &&
             *item[3] == "canonical-bytes-v1" && *item[4] == *item[2] &&
             shape->leaves == *leaf_count && shape->root_arity == *arity &&
             static_cast<__int128>(*score) - static_cast<__int128>(*baseline) == *delta &&
             encodings.insert(*item[2]).second &&
             (!ordinal || ordinals.insert(*ordinal).second) &&
             (!version_2 ||
              ((*item[sankoff_status_column] == "not_applicable") ==
               (*item[sankoff_oracle_column] == "not_applicable"))) &&
             (version_2 ||
              *item[sankoff_status_column] != "not_applicable" ||
              *item[sankoff_oracle_column] == "not_applicable") &&
             (!version_2 ||
              (*score >= 0 && static_cast<std::uint64_t>(*score) >= selected_view->first &&
               static_cast<std::uint64_t>(*score) <= selected_view->second &&
               tree_selected_scores.emplace(*ordinal,
                   static_cast<std::uint64_t>(*score)).second));
      if (!good || !trees.emplace(item[0].value_or(""),
                                  scored_tree{score.value_or(0), delta.value_or(0),
                                              ordinal}).second) {
        reject(result, "topology_identity", "trees.tsv", i + 2,
               "canonical tree identity/replay/score/ordinal invariant failed");
      } else if (!common_taxa) {
        common_taxa = shape->labels;
      } else if (*common_taxa != shape->labels) {
        reject(result, "topology_identity", "trees.tsv", i + 2,
               "tree leaf-label set differs from the bundle taxon set");
      }
      if (good && version_2) {
        std::pair<std::size_t, std::string_view> coverage[]{
            {12, "incremental_fitch_verification"},
            {14, "selected_grammar_sankoff_verification"},
            {17, "materialized_fitch_verification"},
            {19, "reload_fitch_verification"},
            {21, "tree_native_sankoff_verification"}};
        for (auto const& [column, name] : coverage) {
          if (*item[column] == "verified") ++verified_counts[std::string(name)];
        }
      }
    }
  }
  if (!common_taxa || landscape->at("taxon_labels_sha256") !=
                          taxon_set_identity(*common_taxa)) {
    reject(result, "topology_identity", "landscape_semantics.tsv", 0,
           "taxon_labels_sha256 does not match the common parsed leaf set");
  }
  result.topology_count = trees.size();

  std::vector<std::string> grammar_header{
      "grammar_digest", "grammar_ordinal", "topology_sha256", "absolute_score",
      "selected_production_keys", "artifact_local_replay_ids",
      "source_artifact_sha256", "production_origin"};
  if (version_2) {
    grammar_header.insert(grammar_header.end(), {
        "legacy_selection_sha256", "materialized_dag_semantic_sha256",
        "materialized_dag_clades_sha256",
        "materialized_dag_productions_sha256"});
  }
  auto grammar = read_table("grammar_topology_provenance.tsv",
      bytes.at("grammar_topology_provenance.tsv"), std::move(grammar_header),
      result, false);
  std::set<std::uint64_t> grammar_ordinals;
  std::map<std::uint64_t, std::uint64_t> grammar_selected_scores;
  if (grammar) {
    std::optional<std::uint64_t> previous;
    for (std::size_t i = 0; i < grammar->rows.size(); ++i) {
      auto const& item = grammar->rows[i];
      auto ordinal = item[1] ? number<std::uint64_t>(*item[1]) : std::nullopt;
      auto score = item[3] ? number<std::int64_t>(*item[3]) : std::nullopt;
      bool origin_ok = item[7] && (*item[7] == "known" || *item[7] == "unknown" ||
                                   *item[7] == "not_applicable");
      bool audits_ok = !version_2 || std::ranges::all_of(
          std::views::iota(8U, 12U),
          [&](auto column) { return !item[column] || hash_text(*item[column]); });
      if (!item[0] || !hash_text(*item[0]) ||
          *item[0] != landscape->at("grammar_digest") || !ordinal ||
          !item[2] || !trees.contains(*item[2]) || !score ||
          !item[4] || !item[5] || !item[6] || !hash_text(*item[6]) || !origin_ok ||
          !audits_ok ||
          (version_2 && *item[6] != provenance->at("input_artifact_sha256")) ||
          trees.at(*item[2]).score != *score || !trees.at(*item[2]).ordinal ||
          *trees.at(*item[2]).ordinal != *ordinal ||
          (previous && *previous >= *ordinal) || !grammar_ordinals.insert(*ordinal).second ||
          (version_2 && (!score || *score < 0 ||
             !grammar_selected_scores.emplace(*ordinal,
                 static_cast<std::uint64_t>(*score)).second))) {
        reject(result, "grammar_mapping", "grammar_topology_provenance.tsv", i + 2,
               "ordinal/topology/score map is not one-to-one and ordered");
      }
      previous = ordinal;
    }
  }
  if (ordinals != grammar_ordinals) {
    reject(result, "grammar_mapping", "grammar_topology_provenance.tsv", 0,
           "non-null tree ordinals and provenance ordinals differ");
  }
  if (version_2 && (tree_selected_scores != selected_scores ||
                    grammar_selected_scores != selected_scores ||
                    tree_selected_scores != grammar_selected_scores)) {
    reject(result, "grammar_mapping", "score_census.tsv", 0,
           "selected tree and grammar sets differ from the census score view");
  }

  std::map<std::string, std::pair<std::string, std::string>> edge_map;
  std::set<std::pair<std::string, std::string>> pairs;
  auto edge_rows = read_table("topology_edges.tsv", bytes.at("topology_edges.tsv"),
      {"edge_id", "scope_ref", "topology_low", "topology_high", "move_scope",
       "endpoint_low_score", "endpoint_high_score", "endpoint_low_delta",
       "endpoint_high_delta", "filtration"}, result);
  if (edge_rows) {
    for (std::size_t i = 0; i < edge_rows->rows.size(); ++i) {
      auto const& item = edge_rows->rows[i];
      bool present = std::ranges::all_of(item, [](auto const& value) { return value.has_value(); });
      if (!present || !trees.contains(*item[2]) || !trees.contains(*item[3])) {
        reject(result, "simple_edge", "topology_edges.tsv", i + 2,
               "edge has a null or missing endpoint");
        continue;
      }
      auto low_score = number<std::int64_t>(*item[5]);
      auto high_score = number<std::int64_t>(*item[6]);
      auto low_delta = number<std::int64_t>(*item[7]);
      auto high_delta = number<std::int64_t>(*item[8]);
      auto filtration = number<std::int64_t>(*item[9]);
      auto edge_hash = hash(std::string("topology-landscape.simple-edge.") +
                            (version_2 ? "v2\n" : "v1\n") + *item[1] + "\n" +
                            *item[2] + "\n" + *item[3] + "\n" + *item[4] + "\n");
      auto const& edge_scope = version_2 ? result.analysis_policy_hash
                                         : result.landscape_semantics_hash;
      if (*item[1] != edge_scope || !(*item[2] < *item[3]) ||
          *item[0] != edge_hash || !low_score || !high_score || !low_delta ||
          !high_delta || !filtration || *low_score != trees.at(*item[2]).score ||
          *high_score != trees.at(*item[3]).score ||
          *low_delta != trees.at(*item[2]).delta ||
          *high_delta != trees.at(*item[3]).delta ||
          *filtration != std::max(*low_delta, *high_delta) ||
          !pairs.emplace(*item[2], *item[3]).second ||
          !edge_map.emplace(*item[0], std::pair{*item[2], *item[3]}).second) {
        reject(result, "simple_edge", "topology_edges.tsv", i + 2,
               "edge quotient identity, order, score, or uniqueness failed");
      }
    }
  }
  result.simple_edge_count = edge_map.size();

  std::set<std::string> witness_ids;
  auto witness_rows = read_table("move_witnesses.tsv", bytes.at("move_witnesses.tsv"),
      {"witness_id", "scope_ref", "source_topology", "destination_topology",
       "pi_edge_id", "move_family", "move_version", "prune_support",
       "regraft_support", "restrictions", "topology_valid", "leaf_set_valid",
       "root_valid", "reverse_status", "no_self_loop", "band_membership",
       "recomputed_score", "score_reference", "grammar_membership",
       "grammar_membership_witness"}, result);
  if (witness_rows) {
    for (std::size_t i = 0; i < witness_rows->rows.size(); ++i) {
      auto const& item = witness_rows->rows[i];
      bool present = std::ranges::all_of(item | std::views::take(10),
                                         [](auto const& value) { return value.has_value(); });
      if (!present || !trees.contains(*item[2]) || !trees.contains(*item[3]) ||
          !edge_map.contains(*item[4])) {
        reject(result, "move_witness", "move_witnesses.tsv", i + 2,
               "witness has missing field, endpoint, or quotient edge");
        continue;
      }
      auto boolean = [&](std::size_t index) {
        return item[index] && (*item[index] == "true" || *item[index] == "false");
      };
      auto recomputed = item[16] ? number<std::int64_t>(*item[16]) : std::nullopt;
      bool reverse_ok = item[13] &&
          (*item[13] == "certified" || *item[13] == "not_checked" ||
           *item[13] == "not_applicable" || *item[13] == "failed");
      bool membership_ok = item[18] &&
          (*item[18] == "verified" || *item[18] == "not_checked" ||
           *item[18] == "mismatch" || *item[18] == "not_applicable") &&
          ((*item[18] == "verified" || *item[18] == "mismatch")
               ? item[19].has_value()
               : !item[19]);
      if (*item[5] != landscape->at("move_family") || !boolean(10) || !boolean(11) ||
          !boolean(12) || !reverse_ok || !boolean(14) || !boolean(15) ||
          !recomputed || *recomputed != trees.at(*item[3]).score || !item[17] ||
          *item[17] != result.landscape_semantics_hash || !membership_ok) {
        reject(result, "move_witness", "move_witnesses.tsv", i + 2,
               "witness claim token, score/reference, or membership grammar failed");
        continue;
      }
      auto witness_hash = hash(std::string("topology-landscape.move-witness.") +
          (version_2 ? "v2\n" : "v1\n") + *item[1] +
          "\n" + *item[2] + "\n" + *item[3] + "\n" + *item[5] + "\n" +
          *item[6] + "\n" + *item[7] + "\n" + *item[8] + "\n" + *item[9] + "\n");
      auto projected = std::minmax(*item[2], *item[3]);
      auto const& edge = edge_map.at(*item[4]);
      auto const& witness_scope = version_2 ? result.analysis_policy_hash
                                            : result.landscape_semantics_hash;
      if (*item[1] != witness_scope || *item[2] == *item[3] ||
          *item[0] != witness_hash || edge.first != projected.first ||
          edge.second != projected.second || !witness_ids.insert(*item[0]).second) {
        reject(result, "move_witness", "move_witnesses.tsv", i + 2,
               "witness identity or quotient projection failed");
      }
    }
  }
  result.move_witness_count = witness_ids.size();

  auto claims = read_table("completeness.tsv", bytes.at("completeness.tsv"),
      {"name", "state", "scope_ref", "denominator_kind", "denominator_value",
       "observed_numerator", "method_id", "witness_ref", "reason"}, result);
  std::set<std::string> dimensions{
      "canonical_topology", "face", "grammar_membership_index", "grammar_ordinal",
      "move_witness", "simple_edge", "trace_event"};
  std::set<std::string> verification_dimensions;
  if (version_2) {
    verification_dimensions = {
        "incremental_fitch_verification", "materialized_fitch_verification",
        "reload_fitch_verification", "selected_grammar_sankoff_verification",
        "tree_native_sankoff_verification"};
    dimensions.insert(verification_dimensions.begin(),
                      verification_dimensions.end());
    dimensions.insert("score_census");
  }
  std::map<std::string, std::uint64_t> local_counts{
      {"canonical_topology", result.topology_count},
      {"face", 0},
      {"grammar_membership_index", 0},
      {"grammar_ordinal", grammar_ordinals.size()},
      {"move_witness", result.move_witness_count},
      {"simple_edge", result.simple_edge_count},
      {"trace_event", 0}};
  if (version_2) {
    local_counts.emplace("score_census", census_scores.size());
    for (auto const& name : verification_dimensions) {
      local_counts.emplace(name, verified_counts.at(name));
    }
  }
  std::set<std::string> seen;
  if (claims) {
    for (std::size_t i = 0; i < claims->rows.size(); ++i) {
      auto const& item = claims->rows[i];
      if (!item[0] || !item[1] || !item[2] || !item[5] ||
          !dimensions.contains(*item[0]) || !seen.insert(*item[0]).second) {
        reject(result, "completeness", "completeness.tsv", i + 2,
               "unknown, duplicate, or null claim");
        continue;
      }
      auto numerator = number<std::uint64_t>(*item[5]);
      auto denominator = item[4] ? number<std::uint64_t>(*item[4]) : std::nullopt;
      auto expected_scope = *item[0] == "trace_event"
                                ? result.search_run_id
                            : version_2 && *item[0] == "score_census"
                                ? result.landscape_semantics_hash
                            : version_2 ? result.analysis_policy_hash
                                        : result.landscape_semantics_hash;
      static std::map<std::string, std::string> const direct_witness{
          {"canonical_topology", "trees.tsv"},
          {"grammar_ordinal", "grammar_topology_provenance.tsv"},
          {"move_witness", "move_witnesses.tsv"},
          {"simple_edge", "topology_edges.tsv"}};
      bool valid = numerator.has_value() && *item[2] == expected_scope &&
                   (item[3].has_value() == item[4].has_value());
      if (*item[1] == "complete") {
        valid = valid && item[3] && denominator && item[6] && item[7] && !item[8];
        if (version_2 && *item[0] == "score_census") {
          valid = valid && *item[3] == "exact_grammar_ordinal_count" &&
                  *denominator == *grammar_count && *numerator == census_scores.size() &&
                  *item[7] == "score_census.tsv";
        } else if (version_2 &&
                   (*item[0] == "canonical_topology" ||
                    *item[0] == "grammar_ordinal" ||
                    verification_dimensions.contains(*item[0]))) {
          auto witness = *item[0] == "grammar_ordinal"
              ? "grammar_topology_provenance.tsv" : "trees.tsv";
          valid = valid && *item[3] == "exact_score_view_ordinal_count" &&
                  *denominator == selected_scores.size() &&
                  *numerator == selected_scores.size() && *item[7] == witness;
        } else {
          valid = valid && *denominator == *numerator &&
                  direct_witness.contains(*item[0]) &&
                  *item[3] == "exact_row_count" &&
                  *item[7] == direct_witness.at(*item[0]);
        }
      } else if (*item[1] == "partial" || *item[1] == "unknown") {
        valid = valid && item[8];
      } else if (*item[1] == "not_applicable") {
        valid = valid && !item[3] && !item[4] && *numerator == 0 && !item[6] &&
                !item[7] && item[8];
      } else {
        valid = false;
      }
      if (local_counts.contains(*item[0]) &&
          (!numerator || *numerator != local_counts.at(*item[0]))) valid = false;
      if (!valid) {
        reject(result, "completeness", "completeness.tsv", i + 2,
               "claim state grammar, denominator, or local count failed");
      } else {
        result.completeness.push_back(
            {*item[0], *item[1], *item[2], denominator, *numerator});
      }
    }
  }
  if (seen != dimensions) {
    reject(result, "completeness", "completeness.tsv", 0,
           "required claim dimensions are incomplete");
  }

  std::string output_index;
  std::vector<std::string> output_names{
      "completeness.tsv", "grammar_topology_provenance.tsv",
      "move_witnesses.tsv"};
  if (version_2) output_names.push_back("score_census.tsv");
  output_names.insert(output_names.end(), {"topology_edges.tsv", "trees.tsv"});
  std::ranges::sort(output_names);
  for (auto const& name : output_names) {
    output_index += std::string(name) + "\t" + hash(bytes.at(name)) + "\n";
  }
  if (provenance->at("output_data_sha256") !=
      hash(std::string("topology-landscape.output-data.") +
           (version_2 ? "v2\n" : "v1\n") + output_index)) {
    reject(result, "identity_mismatch", "artifact_provenance.tsv", 0,
           "output_data_sha256 does not bind the versioned data tables");
  }
  return result;
}

validation validate_score_band_raw(std::filesystem::path const& directory) {
  validation result;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    reject(result, "directory_layout", directory.native(), 0,
           "raw handoff is not a directory");
    return result;
  }

  std::set<std::string> actual;
  for (auto const& entry : std::filesystem::directory_iterator(directory, error)) {
    auto name = entry.path().filename().native();
    if (entry.is_symlink(error) || !entry.is_regular_file(error)) {
      reject(result, "unsafe_path", name, 0,
             "raw handoff members must be direct regular files");
    }
    actual.insert(name);
  }
  std::set<std::string> const expected(raw_members.begin(), raw_members.end());
  if (actual != expected) {
    reject(result, "directory_layout", directory.native(), 0,
           "raw handoff member set does not match the v1 contract");
    return result;
  }

  std::map<std::string, std::string> bytes;
  for (auto const& name : actual) {
    if (auto value = slurp(directory / name, result)) bytes.emplace(name, *value);
  }
  if (!result) return result;

  if (canonical_file("SHA256SUMS", bytes.at("SHA256SUMS"), result)) {
    std::string previous;
    std::set<std::string> seen;
    auto const ledger = file_lines(bytes.at("SHA256SUMS"));
    for (std::size_t i = 0; i < ledger.size(); ++i) {
      auto const values = split(ledger[i], '\t');
      if (values.size() != 2 || !hash_text(values[0]) ||
          !bytes.contains(std::string(values[1])) ||
          values[1] == "SHA256SUMS" || (i && !(previous < values[1])) ||
          !seen.emplace(values[1]).second ||
          hash(bytes.at(std::string(values[1]))) != values[0]) {
        reject(result, "checksum_mismatch", "SHA256SUMS", i + 1,
               "ledger row is malformed, unsorted, duplicate, or mismatching");
      }
      if (values.size() == 2) previous = values[1];
    }
    if (seen.size() != actual.size() - 1) {
      reject(result, "checksum_mismatch", "SHA256SUMS", 0,
             "ledger coverage is incomplete");
    }
  }
  if (!result) return result;

  static std::set<std::string> const manifest_fields{
      "files_row_count", "files_sha256", "provenance_id", "schema_name",
      "schema_version", "semantic_data_sha256", "semantics_hash"};
  static std::set<std::string> const semantics_fields{
      "alignment_sha256", "alphabet", "ambiguity_policy",
      "canonical_topology_encoding", "digest_algorithm", "edge_weight",
      "endpoint_filtration", "face_policy", "grammar_construction",
      "grammar_digest", "grammar_semantic_digest", "grammar_topology_count",
      "hodge_metric", "input_content_sha256", "move_family",
      "move_relation", "move_symmetry", "null_family", "numeric_policy",
      "parsimony_model", "polytomy_policy", "reference_sha256",
      "root_policy", "score_baseline", "score_view", "site_pattern_digest",
      "taxon_labels_sha256", "taxon_order", "ua_scoring", "universe"};
  static std::set<std::string> const provenance_fields{
      "command", "derivation_ref", "producer_commit", "producer_dirty",
      "producer_repository", "provider_input_sha256",
      "provider_legacy_dag_semantic_sha256", "reference_input_sha256",
      "seed_tree_input_sha256", "toolchain", "worker_count"};

  auto manifest = key_values("manifest.tsv", bytes.at("manifest.tsv"),
                             manifest_fields, result);
  auto semantics = key_values("semantics.tsv", bytes.at("semantics.tsv"),
                              semantics_fields, result);
  auto provenance = key_values("provenance.tsv", bytes.at("provenance.tsv"),
                               provenance_fields, result);
  if (!manifest || !semantics || !provenance) return result;

  result.landscape_semantics_hash = identity(
      "larch.topology-score-band.semantics.v1\n", bytes.at("semantics.tsv"));
  result.artifact_provenance_id = identity(
      "larch.topology-score-band.provenance.v1\n", bytes.at("provenance.tsv"));
  auto grammar_count =
      number<std::uint64_t>(semantics->at("grammar_topology_count"));
  auto baseline = number<std::uint64_t>(semantics->at("score_baseline"));
  auto score_view = parse_score_view(semantics->at("score_view"));
  constexpr std::array semantic_hash_fields{
      std::string_view{"alignment_sha256"}, std::string_view{"grammar_digest"},
      std::string_view{"grammar_semantic_digest"},
      std::string_view{"input_content_sha256"},
      std::string_view{"reference_sha256"},
      std::string_view{"site_pattern_digest"},
      std::string_view{"taxon_labels_sha256"}};
  constexpr std::array provenance_hash_fields{
      std::string_view{"provider_input_sha256"},
      std::string_view{"provider_legacy_dag_semantic_sha256"},
      std::string_view{"reference_input_sha256"},
      std::string_view{"seed_tree_input_sha256"}};
  bool hashes_ok = std::ranges::all_of(
      semantic_hash_fields,
      [&](std::string_view name) { return hash_text(semantics->at(std::string(name))); }) &&
      std::ranges::all_of(
          provenance_hash_fields,
          [&](std::string_view name) {
            return hash_text(provenance->at(std::string(name)));
          });
  bool const identity_values_ok =
      hashes_ok && grammar_count && *grammar_count > 0 && baseline &&
      *baseline <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max()) &&
      score_view &&
      semantics->at("digest_algorithm") == "sha256" &&
      semantics->at("canonical_topology_encoding") ==
          "rooted-labelled-length-grammar-v1" &&
      semantics->at("edge_weight") == "unit" &&
      semantics->at("endpoint_filtration") ==
          "maximum_endpoint_score_delta" &&
      semantics->at("hodge_metric") == "not_applicable" &&
      semantics->at("move_family") == "rooted_subtree_prune_regraft" &&
      semantics->at("move_relation") ==
          "rooted_common_prune_reduction_hard_rspr_v1" &&
      semantics->at("move_symmetry") == "intrinsic_bidirectional" &&
      semantics->at("null_family") == "not_applicable" &&
      semantics->at("face_policy") ==
          "certified_commuting_rspr_square_deferred_v1" &&
      semantics->at("numeric_policy") == "exact_integer" &&
      semantics->at("polytomy_policy") == "hard_multifurcation" &&
      semantics->at("taxon_order") == "unsigned_utf8_byte_order" &&
      semantics->at("root_policy") == "rooted_no_synthetic_ua" &&
      number<std::uint64_t>(provenance->at("worker_count")).has_value() &&
      (provenance->at("producer_dirty") == "true" ||
       provenance->at("producer_dirty") == "false" ||
       provenance->at("producer_dirty") == "not_applicable");
  if (!identity_values_ok) {
    reject(result, "identity_mismatch", "semantics.tsv", 0,
           "raw identity-file digest, integer, score-view, or fixed-token grammar failed");
  }
  if (manifest->at("schema_name") != "larch-topology-score-band-raw-v1" ||
      manifest->at("schema_version") != "1") {
    reject(result, "schema", "manifest.tsv", 0,
           "unsupported raw schema name/version");
  }
  if (manifest->at("semantics_hash") != result.landscape_semantics_hash ||
      manifest->at("provenance_id") != result.artifact_provenance_id) {
    reject(result, "identity_mismatch", "manifest.tsv", 0,
           "raw semantics/provenance identity recomputation failed");
  }
  if (!result) return result;

  auto file_index = read_table("files.tsv", bytes.at("files.tsv"),
      {"filename", "role", "row_count", "sha256"}, result);
  std::map<std::string, std::string> const fixed_roles{
      {"provenance.tsv", "identity"}, {"score_census.tsv", "claim"},
      {"selected_topologies.tsv", "table"}, {"semantics.tsv", "identity"}};
  std::set<std::string> file_names;
  if (file_index) {
    auto count = number<std::uint64_t>(manifest->at("files_row_count"));
    if (!count || *count != file_index->rows.size() ||
        manifest->at("files_sha256") != hash(bytes.at("files.tsv"))) {
      reject(result, "checksum_mismatch", "manifest.tsv", 0,
             "raw files.tsv count or hash mismatch");
    }
    for (std::size_t i = 0; i < file_index->rows.size(); ++i) {
      auto const& item = file_index->rows[i];
      bool shape_ok = item[0] && item[1] && item[2] && item[3] &&
                      fixed_roles.contains(*item[0]) &&
                      fixed_roles.at(*item[0]) == *item[1] &&
                      file_names.insert(*item[0]).second;
      if (!shape_ok) {
        reject(result, "table_shape", "files.tsv", i + 2,
               "invalid raw payload row");
        continue;
      }
      auto rows = number<std::uint64_t>(*item[2]);
      auto actual_rows = data_row_count(bytes.at(*item[0]));
      if (!rows || !actual_rows || *item[3] != hash(bytes.at(*item[0])) ||
          *actual_rows != *rows) {
        reject(result, "checksum_mismatch", "files.tsv", i + 2,
               "raw payload row count or digest mismatch");
      }
    }
  }
  std::set<std::string> payload_names;
  for (auto const& [name, _] : fixed_roles) payload_names.insert(name);
  if (file_names != payload_names) {
    reject(result, "table_shape", "files.tsv", 0,
           "raw payload set mismatch");
  }
  if (!result) return result;

  std::vector<std::uint64_t> census_scores;
  std::map<std::uint64_t, std::uint64_t> selected_scores;
  if (!parse_score_census(bytes.at("score_census.tsv"), *grammar_count,
                          *score_view, result, census_scores,
                          selected_scores)) return result;
  if (selected_scores.empty()) {
    reject(result, "grammar_mapping", "score_census.tsv", 0,
           "score_view must select at least one census row");
  }

  std::vector<std::string> const selected_header{
      "grammar_ordinal", "absolute_score", "incremental_fitch_score",
      "incremental_fitch_status", "selected_grammar_sankoff_score",
      "selected_grammar_sankoff_status", "selected_grammar_sankoff_oracle",
      "selected_production_keys", "artifact_local_replay_ids",
      "legacy_selection_sha256", "canonical_bytes", "topology_sha256",
      "leaf_count", "root_arity_summary", "materialized_fitch_score",
      "materialized_fitch_status", "reload_fitch_score", "reload_fitch_status",
      "tree_native_sankoff_score", "tree_native_sankoff_status",
      "tree_native_sankoff_oracle", "materialized_valid", "reload_valid",
      "production_origin", "legacy_dag_semantic_sha256",
      "legacy_dag_clades_sha256", "legacy_dag_productions_sha256"};
  auto selected = read_table("selected_topologies.tsv",
      bytes.at("selected_topologies.tsv"), selected_header, result, false);
  std::map<std::uint64_t, std::uint64_t> observed_selected;
  std::set<std::string> topology_ids;
  std::set<std::string> canonical_encodings;
  std::optional<std::set<std::string>> common_taxa;
  if (selected) {
    std::optional<std::uint64_t> previous;
    for (std::size_t i = 0; i < selected->rows.size(); ++i) {
      auto const& item = selected->rows[i];
      auto ordinal = item[0] ? number<std::uint64_t>(*item[0]) : std::nullopt;
      auto score = item[1] ? number<std::uint64_t>(*item[1]) : std::nullopt;
      auto leaf_count = item[12] ? number<std::uint64_t>(*item[12]) : std::nullopt;
      auto root_arity = item[13] ? number<std::uint64_t>(*item[13]) : std::nullopt;
      auto shape = item[10] ? tree_syntax(*item[10]) : std::nullopt;
      auto verified_score = [&](std::size_t score_column,
                                std::size_t status_column) {
        auto value = item[score_column]
            ? number<std::uint64_t>(*item[score_column]) : std::nullopt;
        return value && score && item[status_column] &&
               *item[status_column] == "verified" && *value == *score;
      };
      constexpr std::array audit_columns{9U, 24U, 25U, 26U};
      bool audits_ok = std::ranges::all_of(
          audit_columns,
          [&](std::size_t column) {
            return item[column] && hash_text(*item[column]);
          });
      bool origin_ok = item[23] &&
          (*item[23] == "known" || *item[23] == "unknown" ||
           *item[23] == "not_applicable");
      bool good = ordinal && score &&
          *score <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) &&
          leaf_count && root_arity && shape &&
          item[6] && item[7] && item[8] && item[10] && item[11] && item[20] &&
          *item[6] != *item[20] && !item[6]->empty() && !item[20]->empty() &&
          *item[6] != "not_applicable" && *item[20] != "not_applicable" &&
          verified_score(2, 3) && verified_score(4, 5) &&
          verified_score(14, 15) && verified_score(16, 17) &&
          verified_score(18, 19) && item[21] && *item[21] == "true" &&
          item[22] && *item[22] == "true" && origin_ok && audits_ok &&
          *item[11] == topology_digest(*item[10]) &&
          shape->leaves == *leaf_count && shape->root_arity == *root_arity &&
          *score >= score_view->first && *score <= score_view->second &&
          (!previous || *previous < *ordinal) &&
          observed_selected.emplace(*ordinal, *score).second &&
          topology_ids.insert(*item[11]).second &&
          canonical_encodings.insert(*item[10]).second;
      if (!good) {
        reject(result, "selected_topology", "selected_topologies.tsv", i + 2,
               "ordinal, five-oracle, validation, audit, or topology invariant failed");
      } else if (!common_taxa) {
        common_taxa = shape->labels;
      } else if (*common_taxa != shape->labels) {
        reject(result, "topology_identity", "selected_topologies.tsv", i + 2,
               "selected tree leaf-label set differs from the common taxon set");
      }
      previous = ordinal;
    }
  }
  if (observed_selected != selected_scores) {
    reject(result, "grammar_mapping", "selected_topologies.tsv", 0,
           "selected ordinal/score set differs from the census interval projection");
  }
  if (!common_taxa || semantics->at("taxon_labels_sha256") !=
                          taxon_set_identity(*common_taxa)) {
    reject(result, "topology_identity", "semantics.tsv", 0,
           "taxon_labels_sha256 does not match the common parsed leaf set");
  }
  result.topology_count = topology_ids.size();

  std::string semantic_index;
  for (std::string_view name :
       {"score_census.tsv", "selected_topologies.tsv", "semantics.tsv"}) {
    semantic_index += std::string(name) + "\t" +
                      hash(bytes.at(std::string(name))) + "\n";
  }
  auto semantic_data = identity(
      "larch.topology-score-band.semantic-data.v1\n", semantic_index);
  if (manifest->at("semantic_data_sha256") != semantic_data) {
    reject(result, "identity_mismatch", "manifest.tsv", 0,
           "semantic_data_sha256 does not bind the raw semantic payload");
  }
  return result;
}

validation rewrite_canonical_bundle(std::filesystem::path const& input,
                                    std::filesystem::path const& output) {
  auto checked = validate_bundle(input);
  if (!checked) return checked;
  std::error_code error;
  std::vector<std::string> rewrite_members;
  if (std::filesystem::exists(input / "score_census.tsv")) {
    rewrite_members.assign(members_v2.begin(), members_v2.end());
  } else {
    rewrite_members.assign(members.begin(), members.end());
  }
  auto output_status = std::filesystem::symlink_status(output, error);
  if (!error && output_status.type() != std::filesystem::file_type::not_found) {
    reject(checked, "output_exists", output.native(), 0,
           "canonical rewrite refuses to replace an existing path");
    return checked;
  }
  static std::atomic_uint64_t staging_serial{};
  auto staging = output;
#if defined(__linux__)
  staging += ".staging." + std::to_string(::getpid()) + "." +
             std::to_string(staging_serial.fetch_add(1));
#else
  staging += ".staging." + std::to_string(staging_serial.fetch_add(1));
#endif
  if (std::filesystem::exists(staging, error)) {
    reject(checked, "output_exists", staging.native(), 0,
           "staging path already exists");
    return checked;
  }
  std::filesystem::create_directories(staging, error);
  if (error) {
    reject(checked, "write_error", staging.native(), 0, error.message());
    return checked;
  }
  for (auto const& name : rewrite_members) {
    std::filesystem::copy_file(input / name, staging / name,
                               std::filesystem::copy_options::none, error);
    if (error) {
      reject(checked, "write_error", name, 0, error.message());
      std::filesystem::remove_all(staging, error);
      return checked;
    }
  }
  auto staged = validate_bundle(staging);
  if (!staged) {
    std::filesystem::remove_all(staging, error);
    return staged;
  }
  if (publish_no_replace(staging, output, error)) return staged;

  // ZFS through the Linux compatibility layer, and some older filesystems,
  // reject RENAME_NOREPLACE with EINVAL.  Reserve the destination name with an
  // atomic mkdir instead.  This never replaces a path; a concurrent reader may
  // briefly observe an incomplete directory and must reject it by the normal
  // member-set/checksum checks.
  if (error == std::errc::invalid_argument ||
      error == std::errc::operation_not_supported ||
      error == std::errc::function_not_supported) {
    error.clear();
    if (!std::filesystem::create_directory(output, error)) {
      reject(staged, "output_exists", output.native(), 0,
             error ? error.message() : "output was concurrently reserved");
      std::filesystem::remove_all(staging, error);
      return staged;
    }
    for (auto const& name : rewrite_members) {
      std::filesystem::copy_file(staging / name, output / name,
                                 std::filesystem::copy_options::none, error);
      if (error) {
        reject(staged, "write_error", name, 0, error.message());
        std::filesystem::remove_all(output, error);
        std::filesystem::remove_all(staging, error);
        return staged;
      }
    }
    auto fallback = validate_bundle(output);
    std::filesystem::remove_all(staging, error);
    if (!fallback) std::filesystem::remove_all(output, error);
    return fallback;
  }
  reject(staged, "write_error", output.native(), 0, error.message());
  std::filesystem::remove_all(staging, error);
  return staged;
}

}  // namespace larch::topology_landscape
