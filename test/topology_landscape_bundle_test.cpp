#include <larch/topology_landscape_bundle.hpp>
#include <larch/sha256.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#ifndef LARCH_TOPOLOGY_LANDSCAPE_FIXTURE
#error fixture path definition is required
#endif

namespace {

std::string read(std::filesystem::path const& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

void write(std::filesystem::path const& path, std::string const& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value;
}

std::string digest(std::string_view value) {
  larch::sha256 state;
  state.update(value);
  return state.hex_digest();
}

std::vector<std::string> lines(std::string_view value) {
  std::vector<std::string> output;
  std::size_t begin = 0;
  while (begin < value.size()) {
    auto end = value.find('\n', begin);
    output.emplace_back(value.substr(begin, end - begin));
    begin = end + 1;
  }
  return output;
}

std::vector<std::string> fields(std::string_view line) {
  std::vector<std::string> output;
  std::size_t begin = 0;
  while (true) {
    auto end = line.find('\t', begin);
    output.emplace_back(line.substr(begin, end == std::string_view::npos
                                              ? line.size() - begin
                                              : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return output;
}

std::string join(std::vector<std::string> const& values) {
  std::string output;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) output += '\t';
    output += values[i];
  }
  return output;
}

void write_rows(std::filesystem::path const& path, std::vector<std::string> rows,
                bool sort_data = false) {
  if (sort_data && rows.size() > 2) {
    std::ranges::sort(rows.begin() + 1, rows.end());
  }
  std::string value;
  for (auto const& row : rows) value += row + "\n";
  write(path, value);
}

void replace_key(std::filesystem::path const& path, std::string const& key,
                 std::string const& value) {
  auto rows = lines(read(path));
  bool replaced = false;
  for (auto& row : rows) {
    if (row.starts_with(key + "\t")) {
      row = key + "\t" + value;
      replaced = true;
    }
  }
  if (!replaced) throw std::runtime_error("fixture key was absent");
  std::string output;
  for (auto const& row : rows) output += row + "\n";
  write(path, output);
}

void rebuild_outer(std::filesystem::path const& root) {
  constexpr std::array ledger_names{
      "analysis_policy.tsv", "artifact_provenance.tsv", "completeness.tsv",
      "files.tsv", "grammar_topology_provenance.tsv", "landscape_semantics.tsv",
      "manifest.tsv", "move_witnesses.tsv", "search_run.tsv",
      "topology_edges.tsv", "trees.tsv"};
  std::string ledger;
  for (auto const* name : ledger_names) {
    ledger += digest(read(root / name)) + "\t" + name + "\n";
  }
  write(root / "SHA256SUMS", ledger);
}

void rebuild_integrity(std::filesystem::path const& root) {
  constexpr std::array data_names{
      "completeness.tsv", "grammar_topology_provenance.tsv",
      "move_witnesses.tsv", "topology_edges.tsv", "trees.tsv"};
  std::string data_index;
  for (auto const* name : data_names) {
    data_index += std::string(name) + "\t" + digest(read(root / name)) + "\n";
  }
  replace_key(root / "artifact_provenance.tsv", "output_data_sha256",
              digest("topology-landscape.output-data.v1\n" + data_index));
  auto identity = [&](std::string_view domain, std::string_view name) {
    return digest(std::string(domain) + read(root / name));
  };
  replace_key(root / "manifest.tsv", "landscape_semantics_hash",
              identity("topology-landscape.landscape-semantics.v1\n",
                       "landscape_semantics.tsv"));
  replace_key(root / "manifest.tsv", "analysis_policy_hash",
              identity("topology-landscape.analysis-policy.v1\n",
                       "analysis_policy.tsv"));
  replace_key(root / "manifest.tsv", "search_run_id",
              identity("topology-landscape.search-run.v1\n", "search_run.tsv"));
  replace_key(root / "manifest.tsv", "artifact_provenance_id",
              identity("topology-landscape.artifact-provenance.v1\n",
                       "artifact_provenance.tsv"));

  constexpr std::array payloads{
      std::pair{"analysis_policy.tsv", "identity"},
      std::pair{"artifact_provenance.tsv", "identity"},
      std::pair{"completeness.tsv", "claim"},
      std::pair{"grammar_topology_provenance.tsv", "table"},
      std::pair{"landscape_semantics.tsv", "identity"},
      std::pair{"move_witnesses.tsv", "table"},
      std::pair{"search_run.tsv", "identity"},
      std::pair{"topology_edges.tsv", "table"},
      std::pair{"trees.tsv", "table"}};
  std::vector<std::string> rows;
  for (auto const& [name, role] : payloads) {
    auto value = read(root / name);
    auto row_count = std::ranges::count(value, '\n') - 1;
    rows.push_back(std::string(name) + "\t" + role + "\t" +
                   std::to_string(row_count) + "\t" + digest(value));
  }
  std::ranges::sort(rows);
  std::string file_index = "filename\trole\trow_count\tsha256\n";
  for (auto const& row : rows) file_index += row + "\n";
  write(root / "files.tsv", file_index);
  replace_key(root / "manifest.tsv", "files_row_count", "9");
  replace_key(root / "manifest.tsv", "files_sha256", digest(file_index));

  rebuild_outer(root);
}

struct scratch {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("larch-topology-landscape-" + std::to_string(::getpid()));
  scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::filesystem::copy(LARCH_TOPOLOGY_LANDSCAPE_FIXTURE, path,
                          std::filesystem::copy_options::recursive);
  }
  ~scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    auto rewrite = path;
    rewrite += "-rewrite";
    std::filesystem::remove_all(rewrite, ignored);
  }
};

bool equal_directories(std::filesystem::path const& left,
                       std::filesystem::path const& right) {
  constexpr std::array names{
      "SHA256SUMS", "analysis_policy.tsv", "artifact_provenance.tsv",
      "completeness.tsv", "files.tsv", "grammar_topology_provenance.tsv",
      "landscape_semantics.tsv", "manifest.tsv", "move_witnesses.tsv",
      "search_run.tsv", "topology_edges.tsv", "trees.tsv"};
  for (auto const* name : names) {
    if (read(left / name) != read(right / name)) return false;
  }
  return true;
}

int expect_invalid(std::filesystem::path const& path, std::string_view code) {
  auto result = larch::topology_landscape::validate_bundle(path);
  if (result) {
    std::cerr << "mutated bundle was accepted\n";
    return 1;
  }
  if (std::ranges::none_of(result.problems, [&](auto const& item) {
        return item.code == code;
      })) {
    std::cerr << "mutation missed expected layer " << code << '\n';
    for (auto const& item : result.problems) {
      std::cerr << item.code << ": " << item.detail << '\n';
    }
    return 1;
  }
  return 0;
}

int run(std::string_view mode) {
  using namespace larch::topology_landscape;
  auto fixture = std::filesystem::path(LARCH_TOPOLOGY_LANDSCAPE_FIXTURE);
  if (mode == "valid") {
    auto result = validate_bundle(fixture);
    if (!result) {
      for (auto const& item : result.problems) std::cerr << item.detail << '\n';
      return 1;
    }
    auto partial = std::ranges::find_if(result.completeness, [](auto const& claim) {
      return claim.name == "move_witness";
    });
    return digest(read(fixture / "SHA256SUMS")) ==
                       "4f87e5bc2b2adb1ada1fb326152a912bae59d28de9ca0504fca0c9d63ff1016f" &&
                   result.topology_count == 3 && result.simple_edge_count == 1 &&
                   result.move_witness_count == 2 &&
                   partial != result.completeness.end() && partial->state == "partial"
               ? 0 : 1;
  }
  if (mode == "rewrite") {
    scratch copy;
    auto output = copy.path;
    output += "-rewrite";
    auto result = rewrite_canonical_bundle(copy.path, output);
    if (!result || !equal_directories(copy.path, output)) {
      for (auto const& item : result.problems) {
        std::cerr << item.code << ": " << item.detail << '\n';
      }
      return 1;
    }
    auto refused = rewrite_canonical_bundle(copy.path, output);
    return refused || !equal_directories(copy.path, output) ? 1 : 0;
  }
  if (mode == "checksum_reject") {
    scratch copy;
    auto value = read(copy.path / "trees.tsv");
    auto position = value.find("canonical-bytes-v1");
    if (position == std::string::npos) return 1;
    value[position] = 'C';
    write(copy.path / "trees.tsv", value);
    return expect_invalid(copy.path, "checksum_mismatch");
  }
  if (mode == "canonical_reject") {
    scratch copy;
    replace_key(copy.path / "analysis_policy.tsv", "null_family", "%2d");
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "canonical_encoding");
  }
  if (mode == "identity_link_reject") {
    scratch copy;
    replace_key(copy.path / "analysis_policy.tsv", "landscape_semantics_hash",
                std::string(64, '0'));
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "identity_mismatch");
  }
  if (mode == "false_completeness_reject") {
    scratch copy;
    auto path = copy.path / "completeness.tsv";
    auto value = read(path);
    auto begin = value.find("canonical_topology\tcomplete");
    auto end = value.find('\n', begin);
    auto item = value.substr(begin, end - begin);
    auto old = std::string_view("\texact_row_count\t3\t3\t");
    item.replace(item.find(old), old.size(), "\texact_row_count\t4\t3\t");
    value.replace(begin, end - begin, item);
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "completeness");
  }
  if (mode == "schema_reject") {
    scratch copy;
    replace_key(copy.path / "manifest.tsv", "schema_version", "2");
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "schema");
  }
  if (mode == "duplicate_topology_reject") {
    scratch copy;
    auto path = copy.path / "trees.tsv";
    auto rows = lines(read(path));
    auto duplicate = fields(rows[1]);
    duplicate.back() = "2";
    rows.insert(rows.begin() + 2, join(duplicate));
    std::string value;
    for (auto const& row : rows) value += row + "\n";
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "topology_identity");
  }
  if (mode == "missing_endpoint_reject") {
    scratch copy;
    auto path = copy.path / "topology_edges.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    item[3] = std::string(64, 'f');
    item[0] = digest("topology-landscape.simple-edge.v1\n" + item[1] + "\n" +
                     item[2] + "\n" + item[3] + "\n" + item[4] + "\n");
    rows[1] = join(item);
    std::string value;
    for (auto const& row : rows) value += row + "\n";
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "simple_edge");
  }
  if (mode == "literal_hyphen") {
    scratch copy;
    replace_key(copy.path / "analysis_policy.tsv", "null_family", "%2D");
    rebuild_integrity(copy.path);
    return validate_bundle(copy.path) ? 0 : 1;
  }
  if (mode == "empty_cell_reject") {
    scratch copy;
    replace_key(copy.path / "analysis_policy.tsv", "null_family", "");
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "canonical_encoding");
  }
  if (mode == "fixed_role_reject") {
    scratch copy;
    auto path = copy.path / "files.tsv";
    auto value = read(path);
    auto position = value.find("completeness.tsv\tclaim");
    if (position == std::string::npos) return 1;
    value.replace(position, std::string_view("completeness.tsv\tclaim").size(),
                  "completeness.tsv\ttable");
    write(path, value);
    replace_key(copy.path / "manifest.tsv", "files_sha256", digest(value));
    rebuild_outer(copy.path);
    return expect_invalid(copy.path, "table_shape");
  }
  if (mode == "tree_status_reject") {
    scratch copy;
    auto path = copy.path / "trees.tsv";
    auto value = read(path);
    auto position = value.find("not_checked");
    if (position == std::string::npos) return 1;
    value.replace(position, std::string_view("not_checked").size(), "bogus_status");
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "topology_identity");
  }
  if (mode == "witness_status_reject") {
    scratch copy;
    auto path = copy.path / "move_witnesses.tsv";
    auto value = read(path);
    auto position = value.find("certified");
    if (position == std::string::npos) return 1;
    value.replace(position, std::string_view("certified").size(), "bogus_status");
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "move_witness");
  }
  if (mode == "completeness_scope_reject") {
    scratch copy;
    auto path = copy.path / "completeness.tsv";
    auto rows = lines(read(path));
    for (std::size_t i = 1; i < rows.size(); ++i) {
      auto item = fields(rows[i]);
      if (item[0] == "canonical_topology") {
        item[2] = std::string(64, '0');
        rows[i] = join(item);
      }
    }
    std::string value;
    for (auto const& row : rows) value += row + "\n";
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "completeness");
  }
  if (mode == "unsafe_symlink_reject") {
    scratch copy;
    std::filesystem::remove(copy.path / "analysis_policy.tsv");
    std::filesystem::create_symlink(fixture / "analysis_policy.tsv",
                                    copy.path / "analysis_policy.tsv");
    return expect_invalid(copy.path, "unsafe_path");
  }
  if (mode == "nullable_ordinal") {
    scratch copy;
    auto tree_path = copy.path / "trees.tsv";
    auto tree_rows = lines(read(tree_path));
    for (std::size_t i = 1; i < tree_rows.size(); ++i) {
      auto item = fields(tree_rows[i]);
      if (item[18] == "2") {
        item[18] = "-";
        tree_rows[i] = join(item);
      }
    }
    std::string tree_value;
    for (auto const& row : tree_rows) tree_value += row + "\n";
    write(tree_path, tree_value);

    auto grammar_path = copy.path / "grammar_topology_provenance.tsv";
    auto grammar_rows = lines(read(grammar_path));
    std::string grammar_value = grammar_rows.front() + "\n";
    for (std::size_t i = 1; i < grammar_rows.size(); ++i) {
      if (fields(grammar_rows[i])[1] != "2") grammar_value += grammar_rows[i] + "\n";
    }
    write(grammar_path, grammar_value);

    auto claim_path = copy.path / "completeness.tsv";
    auto claim_rows = lines(read(claim_path));
    for (std::size_t i = 1; i < claim_rows.size(); ++i) {
      auto item = fields(claim_rows[i]);
      if (item[0] == "grammar_ordinal") {
        item[1] = "partial";
        item[4] = "2";
        item[5] = "2";
        item[8] = "synthetic_partial_grammar_membership";
        claim_rows[i] = join(item);
      }
    }
    std::string claim_value;
    for (auto const& row : claim_rows) claim_value += row + "\n";
    write(claim_path, claim_value);
    rebuild_integrity(copy.path);
    auto result = validate_bundle(copy.path);
    auto claim = std::ranges::find_if(result.completeness, [](auto const& item) {
      return item.name == "grammar_ordinal";
    });
    return result && result.topology_count == 3 && claim != result.completeness.end() &&
                   claim->state == "partial"
               ? 0 : 1;
  }
  if (mode == "grammar_ordinal_conflict_reject") {
    scratch copy;
    auto path = copy.path / "grammar_topology_provenance.tsv";
    auto rows = lines(read(path));
    for (std::size_t i = 1; i < rows.size(); ++i) {
      auto item = fields(rows[i]);
      if (item[1] == "2") {
        item[1] = "1";
        rows[i] = join(item);
      }
    }
    write_rows(path, rows);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "grammar_mapping");
  }
  if (mode == "score_delta_reject") {
    scratch copy;
    auto path = copy.path / "trees.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    item[9] = std::to_string(std::stoll(item[9]) + 1);
    rows[1] = join(item);
    write_rows(path, rows);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "topology_identity");
  }
  if (mode == "edge_self_loop_reject" || mode == "edge_reversed_reject" ||
      mode == "edge_bad_id_reject") {
    scratch copy;
    auto path = copy.path / "topology_edges.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    if (mode == "edge_self_loop_reject") {
      item[3] = item[2];
      item[6] = item[5];
      item[8] = item[7];
      item[9] = item[7];
    } else if (mode == "edge_reversed_reject") {
      std::swap(item[2], item[3]);
      std::swap(item[5], item[6]);
      std::swap(item[7], item[8]);
    }
    item[0] = digest("topology-landscape.simple-edge.v1\n" + item[1] + "\n" +
                     item[2] + "\n" + item[3] + "\n" + item[4] + "\n");
    if (mode == "edge_bad_id_reject") item[0] = std::string(64, '0');
    rows[1] = join(item);
    write_rows(path, rows);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "simple_edge");
  }
  if (mode == "edge_duplicate_reject") {
    scratch copy;
    auto path = copy.path / "topology_edges.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    item[4] = "alternate_synthetic_move_scope";
    item[0] = digest("topology-landscape.simple-edge.v1\n" + item[1] + "\n" +
                     item[2] + "\n" + item[3] + "\n" + item[4] + "\n");
    rows.push_back(join(item));
    write_rows(path, rows, true);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "simple_edge");
  }
  if (mode == "witness_bad_id_reject" || mode == "witness_projection_reject") {
    scratch copy;
    auto path = copy.path / "move_witnesses.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    if (mode == "witness_projection_reject") {
      std::map<std::string, std::string> scores;
      auto tree_rows = lines(read(copy.path / "trees.tsv"));
      for (std::size_t i = 1; i < tree_rows.size(); ++i) {
        auto tree = fields(tree_rows[i]);
        scores.emplace(tree[0], tree[8]);
      }
      auto replacement = std::ranges::find_if(scores, [&](auto const& entry) {
        return entry.first != item[2] && entry.first != item[3];
      });
      if (replacement == scores.end()) return 1;
      item[3] = replacement->first;
      item[16] = replacement->second;
    }
    item[0] = digest("topology-landscape.move-witness.v1\n" + item[1] + "\n" +
                     item[2] + "\n" + item[3] + "\n" + item[5] + "\n" +
                     item[6] + "\n" + item[7] + "\n" + item[8] + "\n" +
                     item[9] + "\n");
    if (mode == "witness_bad_id_reject") item[0] = std::string(64, '0');
    rows[1] = join(item);
    write_rows(path, rows, true);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "move_witness");
  }
  if (mode == "crlf_reject") {
    scratch copy;
    auto path = copy.path / "analysis_policy.tsv";
    auto value = read(path);
    value.replace(value.find('\n'), 1, "\r\n");
    write(path, value);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "canonical_encoding");
  }
  if (mode == "row_order_reject") {
    scratch copy;
    auto path = copy.path / "analysis_policy.tsv";
    auto rows = lines(read(path));
    std::swap(rows[1], rows[2]);
    write_rows(path, rows);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "canonical_encoding");
  }
  if (mode == "topology_digest_reject") {
    scratch copy;
    auto path = copy.path / "trees.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    item[0] = std::string(64, '0');
    rows[1] = join(item);
    write_rows(path, rows, true);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "topology_identity");
  }
  if (mode == "utf8_label_reject") {
    scratch copy;
    auto path = copy.path / "trees.tsv";
    auto rows = lines(read(path));
    auto item = fields(rows[1]);
    for (auto index : {std::size_t{2}, std::size_t{4}}) {
      auto position = item[index].find("L1:A");
      if (position == std::string::npos) return 1;
      item[index].replace(position, 4, "L1:%FF");
    }
    rows[1] = join(item);
    write_rows(path, rows);
    rebuild_integrity(copy.path);
    return expect_invalid(copy.path, "topology_identity");
  }
  std::cerr << "unknown selector\n";
  return 2;
}

}

int main(int argc, char** argv) {
  return argc == 2 ? run(argv[1]) : 2;
}
