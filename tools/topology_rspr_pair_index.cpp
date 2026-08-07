#include <larch/rooted_rspr_pair_index.hpp>
#include <larch/sha256.hpp>
#include <larch/topology_landscape_bundle.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::string_view tree_header =
    "topology_sha256\tscope_ref\tcanonical_bytes\treplay_encoding\t"
    "replay_bytes\tsplit_digest\tleaf_count\troot_arity_summary\t"
    "absolute_score\tscore_delta\tscore_baseline_ref\t"
    "incremental_fitch_score\tincremental_fitch_status\t"
    "selected_grammar_sankoff_score\tselected_grammar_sankoff_status\t"
    "selected_grammar_sankoff_oracle\tfitch_score\tfitch_status\t"
    "reload_score\treload_status\tsankoff_score\tsankoff_status\t"
    "sankoff_oracle\tgrammar_ordinal\tlocal_production_witness_count\t"
    "exact_compatible_source_history_count\toriginal_occurrence_count";

struct arguments {
  fs::path parent;
  fs::path output;
  std::int64_t score_min{};
  std::int64_t score_max{};
  std::uint64_t expected_tree_count{};
  std::string expected_analysis_policy;
  std::string expected_landscape_semantics;
  std::string expected_artifact_provenance;
  std::string expected_trees_sha256;
};

[[noreturn]] void fail(std::string const& message) {
  throw std::runtime_error(message);
}

std::string slurp(fs::path const& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) fail("cannot open " + path.native());
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  if (input.bad()) fail("cannot read " + path.native());
  return bytes;
}

void write_file(fs::path const& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) fail("cannot create " + path.native());
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) fail("cannot write " + path.native());
}

std::string sha256(std::string_view bytes) {
  larch::sha256 digest;
  digest.update(bytes);
  return digest.hex_digest();
}

bool is_sha256(std::string_view text) {
  return text.size() == 64 &&
         std::ranges::all_of(text, [](char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f');
         });
}

template <typename Integer>
Integer integer(std::string_view text, std::string_view name) {
  Integer value{};
  auto const* begin = text.data();
  auto const* end = begin + text.size();
  auto [parsed, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || parsed != end || text.empty() ||
      std::to_string(value) != text) {
    fail(std::string(name) + " must be a canonical decimal integer");
  }
  return value;
}

arguments parse_arguments(int argc, char const* const* argv) {
  if (argc != 19) {
    fail("usage: topology-rspr-pair-index --parent DIR --output DIR "
         "--score-min N --score-max N --expected-tree-count N "
         "--expected-analysis-policy HASH "
         "--expected-landscape-semantics HASH "
         "--expected-artifact-provenance HASH "
         "--expected-trees-sha256 HASH");
  }
  std::map<std::string, std::string, std::less<>> values;
  for (int index = 1; index < argc; index += 2) {
    std::string option(argv[index]);
    if (!option.starts_with("--") || !values.emplace(option, argv[index + 1]).second) {
      fail("duplicate or malformed option: " + option);
    }
  }
  auto required = [&values](std::string_view name) -> std::string const& {
    auto found = values.find(name);
    if (found == values.end()) fail("missing required option: " + std::string(name));
    return found->second;
  };
  static constexpr std::string_view known[] = {
      "--parent", "--output", "--score-min", "--score-max",
      "--expected-tree-count", "--expected-analysis-policy",
      "--expected-landscape-semantics", "--expected-artifact-provenance",
      "--expected-trees-sha256"};
  for (auto const& [name, value] : values) {
    (void)value;
    if (std::ranges::find(known, name) == std::ranges::end(known)) {
      fail("unknown option: " + name);
    }
  }

  arguments result;
  result.parent = required("--parent");
  result.output = required("--output");
  result.score_min = integer<std::int64_t>(required("--score-min"), "--score-min");
  result.score_max = integer<std::int64_t>(required("--score-max"), "--score-max");
  result.expected_tree_count = integer<std::uint64_t>(
      required("--expected-tree-count"), "--expected-tree-count");
  result.expected_analysis_policy = required("--expected-analysis-policy");
  result.expected_landscape_semantics =
      required("--expected-landscape-semantics");
  result.expected_artifact_provenance =
      required("--expected-artifact-provenance");
  result.expected_trees_sha256 = required("--expected-trees-sha256");
  if (result.parent.empty() || result.output.empty()) fail("paths must not be empty");
  if (result.score_min > result.score_max) fail("score interval is reversed");
  if (result.expected_tree_count == 0) fail("--expected-tree-count must be positive");
  for (auto const* value : {&result.expected_analysis_policy,
                            &result.expected_landscape_semantics,
                            &result.expected_artifact_provenance,
                            &result.expected_trees_sha256}) {
    if (!is_sha256(*value)) fail("expected identities must be lowercase SHA-256 values");
  }
  return result;
}

std::vector<std::string_view> lines(std::string_view bytes) {
  if (bytes.empty() || bytes.back() != '\n') fail("canonical TSV must end in LF");
  std::vector<std::string_view> result;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    auto end = bytes.find('\n', begin);
    result.push_back(bytes.substr(begin, end - begin));
    begin = end + 1;
  }
  return result;
}

std::vector<std::string_view> fields(std::string_view row) {
  std::vector<std::string_view> result;
  std::size_t begin = 0;
  while (true) {
    auto end = row.find('\t', begin);
    result.push_back(row.substr(begin, end - begin));
    if (end == std::string_view::npos) return result;
    begin = end + 1;
  }
}

unsigned hex_digit(char value) {
  if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
  if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
  fail("invalid percent escape in validated parent");
}

std::string decode_required(std::string_view value) {
  if (value.empty() || value == "-") fail("required tree field is null or empty");
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      result.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) fail("truncated percent escape in validated parent");
    auto decoded = static_cast<char>((hex_digit(value[index + 1]) << 4U) |
                                     hex_digit(value[index + 2]));
    result.push_back(decoded);
    index += 2;
  }
  return result;
}

std::map<std::string, std::string, std::less<>> parse_manifest(
    std::string_view bytes) {
  auto rows = lines(bytes);
  if (rows.empty() || rows.front() != "key\tvalue") fail("manifest header mismatch");
  std::map<std::string, std::string, std::less<>> result;
  for (std::size_t index = 1; index < rows.size(); ++index) {
    auto columns = fields(rows[index]);
    if (columns.size() != 2 || !result.emplace(columns[0], columns[1]).second) {
      fail("manifest key/value shape mismatch");
    }
  }
  return result;
}

void require_identity(std::string_view name, std::string const& actual,
                      std::string const& expected) {
  if (actual != expected) {
    fail(std::string(name) + " mismatch: expected " + expected + ", got " + actual);
  }
}

std::vector<larch::topology::rooted_tree> select_trees(
    std::string_view bytes, arguments const& args) {
  auto rows = lines(bytes);
  if (rows.empty() || rows.front() != tree_header) fail("neutral-v2 trees.tsv header mismatch");
  std::vector<larch::topology::rooted_tree> result;
  std::set<std::string> selected_hashes;
  for (std::size_t row = 1; row < rows.size(); ++row) {
    auto columns = fields(rows[row]);
    if (columns.size() != 27) fail("neutral-v2 trees.tsv row shape mismatch");
    auto score_text = decode_required(columns[8]);
    auto score = integer<std::int64_t>(score_text, "absolute_score");
    if (score < args.score_min || score > args.score_max) continue;
    auto expected_hash = decode_required(columns[0]);
    auto canonical_bytes = decode_required(columns[2]);
    auto tree = larch::topology::parse_canonical_tree(canonical_bytes);
    auto actual_hash = larch::topology::topology_sha256(tree);
    if (actual_hash != expected_hash) fail("selected tree topology identity mismatch");
    if (!selected_hashes.insert(actual_hash).second) fail("selected duplicate topology");
    result.push_back(std::move(tree));
  }
  if (result.size() != args.expected_tree_count) {
    fail("selected tree count mismatch: expected " +
         std::to_string(args.expected_tree_count) + ", got " +
         std::to_string(result.size()));
  }
  return result;
}

std::uint64_t unordered_pair_denominator(std::size_t tree_count) {
  if (tree_count > std::numeric_limits<std::uint64_t>::max()) {
    fail("tree count exceeds uint64 range");
  }
  auto count = static_cast<std::uint64_t>(tree_count);
  if (count < 2) return 0;
  auto low = count;
  auto high = count - 1;
  if ((low & 1U) == 0) {
    low /= 2;
  } else {
    high /= 2;
  }
  if (low > std::numeric_limits<std::uint64_t>::max() / high) {
    fail("unordered pair denominator exceeds uint64 range");
  }
  return low * high;
}

std::string results_bytes(
    arguments const& args,
    larch::topology_landscape::validation const& parent,
    larch::topology::rooted_rspr_pair_index_result const& index,
    std::string const& tree_sha, std::string const& edge_ledger_sha,
    std::uint64_t denominator) {
  auto const& stats = index.statistics;
  std::map<std::string, std::string, std::less<>> values = {
      {"accepted_edge_count", std::to_string(index.endpoint_pairs.size())},
      {"algorithm", "collision_safe_common_prune_signature_index_v1"},
      {"analysis_policy_hash", parent.analysis_policy_hash},
      {"candidate_bucket_count", std::to_string(stats.candidate_bucket_count)},
      {"cut_count", std::to_string(stats.cut_record_count)},
      {"endpoint_pair_ledger_sha256", edge_ledger_sha},
      {"exact_signature_class_count", std::to_string(stats.exact_signature_class_count)},
      {"fingerprint_collision_bucket_count",
       std::to_string(stats.fingerprint_collision_bucket_count)},
      {"landscape_semantics_hash", parent.landscape_semantics_hash},
      {"largest_exact_signature_class_size",
       std::to_string(stats.largest_exact_signature_class_size)},
      {"move_relation", "rooted_common_prune_reduction_hard_rspr_v1"},
      {"move_scope", "hard_rspr_induced_selected_view_v1"},
      {"pair_coverage_method", "complete_exact_signature_equivalence_partition"},
      {"parent_artifact_provenance_id", parent.artifact_provenance_id},
      {"parent_tree_table_sha256", tree_sha},
      {"score_max", std::to_string(args.score_max)},
      {"score_min", std::to_string(args.score_min)},
      {"tree_count", std::to_string(stats.tree_count)},
      {"unordered_pair_denominator", std::to_string(denominator)}};
  std::string result = "key\tvalue\n";
  for (auto const& [key, value] : values) result += key + '\t' + value + '\n';
  return result;
}

bool publish_no_replace(fs::path const& staging, fs::path const& output,
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
  (void)staging;
  (void)output;
  error = std::make_error_code(std::errc::operation_not_supported);
  return false;
#endif
}

void run(arguments const& args) {
  std::error_code error;
  if (fs::exists(args.output, error) || error) fail("output path already exists or cannot be inspected");

  auto parent = larch::topology_landscape::validate_bundle(args.parent);
  if (!parent) {
    auto const& problem = parent.problems.front();
    fail("parent validation failed: " + problem.code + " " + problem.file +
         ":" + std::to_string(problem.line) + " " + problem.detail);
  }
  auto manifest = parse_manifest(slurp(args.parent / "manifest.tsv"));
  if (manifest["schema_name"] != "topology-landscape-neutral-v2" ||
      manifest["schema_version"] != "2") {
    fail("parent must be topology-landscape-neutral-v2 schema version 2");
  }
  require_identity("analysis_policy_hash", parent.analysis_policy_hash,
                   args.expected_analysis_policy);
  require_identity("landscape_semantics_hash", parent.landscape_semantics_hash,
                   args.expected_landscape_semantics);
  require_identity("artifact_provenance_id", parent.artifact_provenance_id,
                   args.expected_artifact_provenance);

  auto tree_bytes = slurp(args.parent / "trees.tsv");
  auto tree_sha = sha256(tree_bytes);
  require_identity("trees.tsv SHA-256", tree_sha, args.expected_trees_sha256);
  auto trees = select_trees(tree_bytes, args);
  auto denominator = unordered_pair_denominator(trees.size());
  auto index = larch::topology::build_rooted_rspr_pair_index(trees);

  std::set<std::string> selected;
  for (auto const& tree : trees) selected.insert(larch::topology::topology_sha256(tree));
  std::string edge_rows;
  for (auto const& pair : index.endpoint_pairs) {
    if (!(pair.low < pair.high) || !selected.contains(pair.low) ||
        !selected.contains(pair.high)) {
      fail("pair index returned an invalid endpoint pair");
    }
    edge_rows += pair.low + '\t' + pair.high + '\n';
  }
  auto edge_ledger_sha = sha256(
      std::string("topology-endpoint-pair.edge-set.v1\n") + edge_rows);
  auto endpoint_bytes = std::string("topology_low\ttopology_high\n") + edge_rows;
  auto result_bytes = results_bytes(args, parent, index, tree_sha,
                                    edge_ledger_sha, denominator);

  auto parent_directory = args.output.parent_path();
  if (parent_directory.empty()) parent_directory = ".";
  fs::create_directories(parent_directory, error);
  if (error) fail("cannot create output parent: " + error.message());
#if defined(__linux__)
  auto staging = parent_directory /
      ("." + args.output.filename().native() + ".staging-" +
       std::to_string(static_cast<long long>(::getpid())));
#else
  auto staging = parent_directory / ("." + args.output.filename().native() + ".staging");
#endif
  if (!fs::create_directory(staging, error) || error) {
    fail("cannot create staging directory: " + error.message());
  }
  try {
    write_file(staging / "RESULTS.tsv", result_bytes);
    write_file(staging / "endpoint_pairs.tsv", endpoint_bytes);
    auto sums = sha256(result_bytes) + "\tRESULTS.tsv\n" +
                sha256(endpoint_bytes) + "\tendpoint_pairs.tsv\n";
    write_file(staging / "SHA256SUMS", sums);
    if (slurp(staging / "RESULTS.tsv") != result_bytes ||
        slurp(staging / "endpoint_pairs.tsv") != endpoint_bytes ||
        slurp(staging / "SHA256SUMS") != sums) {
      fail("staging replay mismatch");
    }
    if (publish_no_replace(staging, args.output, error)) return;

    // ZFS through the Linux compatibility layer and some older filesystems
    // reject RENAME_NOREPLACE. Publish a same-directory relative symlink as
    // one atomic no-replace namespace operation. The complete payload keeps
    // its staging name and is unreachable through OUTPUT until the link exists.
    if (error != std::errc::invalid_argument &&
        error != std::errc::operation_not_supported &&
        error != std::errc::function_not_supported) {
      fail("no-replace publication failed: " + error.message());
    }
    error.clear();
    fs::create_symlink(staging.filename(), args.output, error);
    if (error) fail("no-replace symlink publication failed: " + error.message());
    return;
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    throw;
  }
}

}  // namespace

int main(int argc, char const* const* argv) {
  try {
    run(parse_arguments(argc, argv));
    return 0;
  } catch (std::exception const& error) {
    std::cerr << "topology-rspr-pair-index: " << error.what() << '\n';
    return 1;
  }
}
