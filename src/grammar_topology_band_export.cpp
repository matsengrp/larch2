#include <larch/grammar_topology_band_export.hpp>

#include <larch/chart_bnb_trim_apply.hpp>
#include <larch/chart_spr_semantic_report.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/fasta.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/phylo_topology_adapter.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/sha256.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/topology_landscape_bundle.hpp>
#include <larch/tree_pattern_sankoff.hpp>
#include <larch/weight_ops.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace larch {
namespace {

using key_values = std::map<std::string, std::string>;

std::string hash(std::string_view value) {
  sha256 digest;
  digest.update(value);
  return digest.hex_digest();
}

std::string identity(std::string_view domain, std::string_view value) {
  sha256 digest;
  digest.update(domain);
  digest.update(value);
  return digest.hex_digest();
}

void append_framed(std::string& output, std::string_view key,
                   std::string_view value) {
  output.append(key);
  output.push_back('\t');
  output += std::to_string(value.size());
  output.push_back(':');
  output.append(value);
  output.push_back('\n');
}

template <typename Integer>
void append_integer(std::string& output, std::string_view key, Integer value) {
  append_framed(output, key, std::to_string(value));
}

bool unsigned_byte_less(std::string_view lhs, std::string_view rhs) {
  return std::lexicographical_compare(
      lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
      [](char a, char b) {
        return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
      });
}

void validate_taxon_registry(clade_grammar const& grammar) {
  auto const& labels = grammar.taxa.id_to_sample_id;
  if (labels.empty()) {
    throw std::runtime_error("score-band export: empty taxon registry");
  }
  if (!std::is_sorted(labels.begin(), labels.end(), unsigned_byte_less) ||
      std::adjacent_find(labels.begin(), labels.end()) != labels.end()) {
    throw std::runtime_error(
        "score-band export: taxon registry is not strict unsigned UTF-8 byte "
        "order");
  }
  if (grammar.taxa.sample_id_to_id.size() != labels.size()) {
    throw std::runtime_error(
        "score-band export: taxon registry inverse size mismatch");
  }
  for (std::size_t tid = 0; tid < labels.size(); ++tid) {
    auto found = grammar.taxa.sample_id_to_id.find(labels[tid]);
    if (found == grammar.taxa.sample_id_to_id.end() || found->second != tid) {
      throw std::runtime_error(
          "score-band export: taxon registry inverse mapping mismatch");
    }
  }
}

std::string hash_file(std::filesystem::path const& path,
                      std::string_view field) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("score-band export: cannot read " +
                             std::string(field) + " artifact");
  }
  sha256 digest;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    auto count = input.gcount();
    if (count > 0) {
      digest.update(std::string_view(buffer.data(),
                                     static_cast<std::size_t>(count)));
    }
  }
  if (!input.eof()) {
    throw std::runtime_error("score-band export: error reading " +
                             std::string(field) + " artifact");
  }
  return digest.hex_digest();
}

void require_identity(std::string_view field, std::string const& declared,
                      std::string const& derived) {
  if (declared != derived) {
    throw std::runtime_error("score-band export: " + std::string(field) +
                             " identity drift");
  }
}

std::string parsed_reference_artifact(std::filesystem::path const& path) {
  auto file_bytes = read_file(path.native());
  std::string bytes{file_bytes.begin(), file_bytes.end()};
  std::string sequence;
  if (!bytes.empty() && bytes.front() == '>') {
    auto entries = read_fasta(path.native());
    if (entries.size() != 1) {
      throw std::runtime_error(
          "score-band export: reference input must contain one FASTA record");
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
  for (char base : sequence) {
    if (base != 'A' && base != 'C' && base != 'G' && base != 'T') {
      throw std::runtime_error(
          "score-band export: reference input is not strict A/C/G/T");
    }
  }
  return sequence;
}

std::uint64_t provider_stored_history_minimum(
    phylo_dag& dag, bool score_reference_edge, std::string_view label) {
  validate_dag(dag, label, thread_pool::get_default());
  build_clade_offsets(dag);
  recompute_edge_mutations(dag);
  build_clade_offsets(dag);
  std::uint64_t exact_minimum = 0;
  if (score_reference_edge) {
    parsimony_score_ops operations;
    subtree_weight<parsimony_score_ops> scorer(dag);
    exact_minimum = scorer.compute_weight_below(get_root_idx(dag), operations);
  } else {
    ua_free_parsimony_score_ops operations;
    subtree_weight<ua_free_parsimony_score_ops> scorer(dag);
    exact_minimum = scorer.compute_weight_below(get_root_idx(dag), operations);
  }
  return exact_minimum;
}

std::string legacy_provider_semantic(phylo_dag& dag,
                                     bool score_reference_edge,
                                     std::string_view label) {
  auto const exact_minimum =
      provider_stored_history_minimum(dag, score_reference_edge, label);
  clade_grammar_options options;
  options.allow_polytomies = true;
  auto built = build_clade_grammar_with_audit(dag, options);
  return build_canonical_dag_digest_report(built.grammar, exact_minimum)
      .semantic_sha256;
}

std::string encode(std::string_view value) {
  if (value == "-") return "%2D";
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (unsigned char byte : value) {
    if (byte >= 0x21 && byte <= 0x7e && byte != '%') {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('%');
      result.push_back(hex[byte >> 4]);
      result.push_back(hex[byte & 0x0f]);
    }
  }
  return result;
}

std::string key_value_bytes(key_values const& values) {
  std::string result = "key\tvalue\n";
  for (auto const& [key, value] : values) {
    result += encode(key) + "\t" + encode(value) + "\n";
  }
  return result;
}

std::string table_bytes(std::span<std::string_view const> header,
                        std::span<std::vector<std::string> const> rows) {
  std::string result;
  for (std::size_t column = 0; column < header.size(); ++column) {
    if (column) result.push_back('\t');
    result += header[column];
  }
  result.push_back('\n');
  for (auto const& row : rows) {
    if (row.size() != header.size()) {
      throw std::logic_error("score-band export: table row width mismatch");
    }
    for (std::size_t column = 0; column < row.size(); ++column) {
      if (column) result.push_back('\t');
      result += encode(row[column]);
    }
    result.push_back('\n');
  }
  return result;
}

std::string stable_key_list(std::span<std::string const> keys) {
  std::string result = "K" + std::to_string(keys.size()) + "[";
  for (auto const& key : keys) {
    result += std::to_string(key.size()) + ":" + key;
  }
  result += "]";
  return result;
}

std::string production_id_list(std::span<production_id const> ids) {
  std::string result = "I" + std::to_string(ids.size()) + "[";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index) result.push_back(',');
    result += std::to_string(ids[index]);
  }
  result += "]";
  return result;
}

std::uint64_t score_materialized_tree(phylo_dag& tree,
                                      bool score_reference_edge) {
  if (!is_tree(tree)) {
    throw std::runtime_error(
        "score-band export: materialized DAG is not a tree");
  }
  build_clade_offsets(tree);
  fitch_assign_compact_genomes(tree);
  recompute_edge_mutations(tree);
  build_clade_offsets(tree);
  if (score_reference_edge) {
    parsimony_score_ops operations;
    subtree_weight<parsimony_score_ops> scorer(tree);
    return scorer.compute_weight_below(get_root_idx(tree), operations);
  }
  ua_free_parsimony_score_ops operations;
  subtree_weight<ua_free_parsimony_score_ops> scorer(tree);
  return scorer.compute_weight_below(get_root_idx(tree), operations);
}

std::vector<production_id> selected_productions(
    grammar_topology const& topology) {
  std::vector<production_id> result;
  for (std::size_t pid = 0; pid < topology.used_production.size(); ++pid) {
    if (topology.used_production[pid]) {
      result.push_back(static_cast<production_id>(pid));
    }
  }
  return result;
}

std::vector<std::string> selected_keys(clade_grammar const& grammar,
                                       grammar_topology const& topology) {
  std::vector<std::string> result;
  for (auto pid : selected_productions(topology)) {
    result.push_back(
        chart_spr_canonical_production_sample_key(grammar, pid));
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::filesystem::path create_staging(std::filesystem::path const& output) {
  if (output.filename().empty()) {
    throw std::runtime_error(
        "score-band export: output directory has no filename");
  }
  auto parent = output.parent_path();
  if (parent.empty()) parent = ".";
  std::filesystem::create_directories(parent);
  static std::atomic<std::uint64_t> counter{0};
  auto tick = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  for (std::size_t attempt = 0; attempt < 1024; ++attempt) {
    auto candidate = parent /
        ("." + output.filename().native() + ".staging." +
         std::to_string(tick) + "." +
         std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) return candidate;
    if (error && error != std::errc::file_exists) {
      throw std::runtime_error(
          "score-band export: cannot create staging directory: " +
          error.message());
    }
  }
  throw std::runtime_error(
      "score-band export: cannot allocate unique staging directory");
}

struct staging_guard {
  std::filesystem::path path;
  ~staging_guard() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void write_file(std::filesystem::path const& path, std::string_view value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(value.data(),
                               static_cast<std::streamsize>(value.size()))) {
    throw std::runtime_error("score-band export: cannot write " +
                             path.filename().native());
  }
  output.close();
  if (!output) {
    throw std::runtime_error("score-band export: cannot close " +
                             path.filename().native());
  }
}

[[noreturn]] void throw_validation(
    std::string_view context,
    topology_landscape::validation const& validation) {
  std::ostringstream message;
  message << "score-band export: " << context << " raw validation failed";
  for (auto const& problem : validation.problems) {
    message << "\n" << problem.code << "\t" << problem.file << "\t"
            << problem.line << "\t" << problem.detail;
  }
  throw std::runtime_error(message.str());
}

bool publish_no_replace(std::filesystem::path const& staging,
                        std::filesystem::path const& output,
                        std::error_code& error) {
#if defined(__linux__)
  if (::syscall(SYS_renameat2, static_cast<long>(AT_FDCWD), staging.c_str(),
                static_cast<long>(AT_FDCWD), output.c_str(),
                static_cast<unsigned int>(RENAME_NOREPLACE)) == 0) {
    return true;
  }
  error = std::error_code(errno, std::generic_category());
  if (error != std::errc::invalid_argument &&
      error != std::errc::function_not_supported &&
      error.value() != EOPNOTSUPP) {
    return false;
  }
#endif
  error.clear();
  if (!std::filesystem::create_directory(output, error)) return false;
  try {
    for (auto const& entry : std::filesystem::directory_iterator(staging)) {
      auto destination = output / entry.path().filename();
      if (!std::filesystem::copy_file(entry.path(), destination,
                                      std::filesystem::copy_options::none,
                                      error)) {
        if (!error) error = std::make_error_code(std::errc::io_error);
        throw std::system_error(error);
      }
    }
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
    throw;
  }
  return true;
}

std::string semantic_data_identity(
    std::map<std::string, std::string> const& payload) {
  std::string index;
  for (auto const& name : {std::string{"score_census.tsv"},
                           std::string{"selected_topologies.tsv"},
                           std::string{"semantics.tsv"}}) {
    index += name + "\t" + hash(payload.at(name)) + "\n";
  }
  return identity("larch.topology-score-band.semantic-data.v1\n", index);
}

}  // namespace

std::string topology_score_band_taxon_labels_sha256(
    std::span<std::string const> labels) {
  std::set<std::string, std::less<>> ordered(labels.begin(), labels.end());
  if (ordered.size() != labels.size()) {
    throw std::runtime_error(
        "score-band export: duplicate taxon label in identity input");
  }
  std::string input = "topology-landscape.taxon-label-set.v1\n";
  for (auto const& label : ordered) {
    input += std::to_string(label.size()) + ":" + label;
  }
  return hash(input);
}

std::string topology_score_band_artifact_sha256(
    std::filesystem::path const& path) {
  return hash_file(path, "identity input");
}

topology_score_band_derived_semantics derive_topology_score_band_semantics(
    phylo_dag& source_dag, clade_grammar const& grammar,
    site_pattern_set const& patterns, chart_options const& chart_options,
    std::string_view grammar_construction) {
  validate_taxon_registry(grammar);
  if (patterns.taxon_count != grammar.taxa.id_to_sample_id.size() ||
      patterns.total_site_count != patterns.original_site_to_pattern.size() ||
      patterns.skipped_invariant_site_count != 0) {
    throw std::runtime_error(
        "score-band export: site-pattern taxon/site mapping is incomplete");
  }

  std::vector<std::size_t> position_owner(patterns.total_site_count,
                                          no_site_pattern);
  std::uint64_t represented_sites = 0;
  for (std::size_t pattern_id = 0; pattern_id < patterns.patterns.size();
       ++pattern_id) {
    auto const& pattern = patterns.patterns[pattern_id];
    if (pattern.state_by_taxon.size() != patterns.taxon_count ||
        pattern.weight == 0 || pattern.positions.size() != pattern.weight) {
      throw std::runtime_error(
          "score-band export: malformed exact site pattern");
    }
    std::uint64_t reference_count = 0;
    for (auto count : pattern.reference_state_counts) {
      if (count > std::numeric_limits<std::uint64_t>::max() -
                      reference_count) {
        throw std::runtime_error(
            "score-band export: site-pattern reference-count overflow");
      }
      reference_count += count;
    }
    if (reference_count != pattern.weight) {
      throw std::runtime_error(
          "score-band export: site-pattern reference-count mismatch");
    }
    for (auto state : pattern.state_by_taxon) {
      if (state >= nuc_state_count) {
        throw std::runtime_error(
            "score-band export: site-pattern state is not strict A/C/G/T");
      }
    }
    for (auto position : pattern.positions) {
      if (position == 0 || position > patterns.total_site_count) {
        throw std::runtime_error(
            "score-band export: site-pattern position is out of range");
      }
      auto index = static_cast<std::size_t>(position - 1);
      if (position_owner[index] != no_site_pattern) {
        throw std::runtime_error(
            "score-band export: duplicate site-pattern position");
      }
      position_owner[index] = pattern_id;
    }
    if (pattern.weight > std::numeric_limits<std::uint64_t>::max() -
                             represented_sites) {
      throw std::runtime_error(
          "score-band export: represented-site count overflow");
    }
    represented_sites += pattern.weight;
  }
  if (represented_sites != patterns.total_site_count ||
      patterns.invariant_site_count + patterns.variable_site_count !=
          patterns.total_site_count) {
    throw std::runtime_error(
        "score-band export: site-pattern aggregate count mismatch");
  }
  for (std::size_t site = 0; site < patterns.total_site_count; ++site) {
    auto pattern_id = patterns.original_site_to_pattern[site];
    if (pattern_id >= patterns.patterns.size() ||
        position_owner[site] != pattern_id) {
      throw std::runtime_error(
          "score-band export: original-site/pattern mapping mismatch");
    }
  }

  std::string ordered_grammar =
      "larch.topology-score-band.ordered-grammar.v1\n";
  append_integer(ordered_grammar, "taxon_count",
                 grammar.taxa.id_to_sample_id.size());
  for (std::size_t tid = 0; tid < grammar.taxa.id_to_sample_id.size(); ++tid) {
    append_integer(ordered_grammar, "taxon_id", tid);
    append_framed(ordered_grammar, "taxon_label",
                  grammar.taxa.id_to_sample_id[tid]);
  }
  append_integer(ordered_grammar, "root_clade", grammar.root_clade);
  append_integer(ordered_grammar, "clade_count", grammar.clades.size());
  for (std::size_t cid = 0; cid < grammar.clades.size(); ++cid) {
    append_integer(ordered_grammar, "clade_id", cid);
    append_integer(ordered_grammar, "clade_taxon_count",
                   grammar.clades[cid].taxa.size());
    for (auto taxon : grammar.clades[cid].taxa) {
      append_integer(ordered_grammar, "clade_taxon", taxon);
    }
  }
  append_integer(ordered_grammar, "production_count",
                 grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    auto const& production = grammar.productions[pid];
    append_integer(ordered_grammar, "production_id", pid);
    append_integer(ordered_grammar, "production_parent", production.parent);
    append_integer(ordered_grammar, "production_child_count",
                   production.children.size());
    for (auto child : production.children) {
      append_integer(ordered_grammar, "production_child", child);
    }
  }
  append_integer(ordered_grammar, "parents_count",
                 grammar.productions_by_parent.size());
  for (std::size_t cid = 0; cid < grammar.productions_by_parent.size(); ++cid) {
    auto ids = grammar.productions_by_parent[cid];
    std::sort(ids.begin(), ids.end());
    append_integer(ordered_grammar, "parent_clade", cid);
    append_integer(ordered_grammar, "parent_production_count", ids.size());
    for (auto pid : ids) {
      append_integer(ordered_grammar, "parent_production", pid);
    }
  }

  std::vector<std::string> clade_keys;
  clade_keys.reserve(grammar.clades.size());
  for (auto const& clade : grammar.clades) {
    clade_keys.push_back(
        chart_spr_canonical_clade_sample_key(grammar, clade.taxa));
  }
  std::sort(clade_keys.begin(), clade_keys.end());
  std::vector<std::string> production_keys;
  production_keys.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    production_keys.push_back(chart_spr_canonical_production_sample_key(
        grammar, static_cast<production_id>(pid)));
  }
  std::sort(production_keys.begin(), production_keys.end());
  std::string semantic_grammar =
      "larch.topology-score-band.semantic-grammar.v1\n";
  append_framed(semantic_grammar, "construction_policy",
                grammar_construction);
  append_framed(
      semantic_grammar, "root_clade_key",
      chart_spr_canonical_clade_sample_key(
          grammar, grammar.clades.at(grammar.root_clade).taxa));
  append_integer(semantic_grammar, "clade_key_count", clade_keys.size());
  for (auto const& key : clade_keys) {
    append_framed(semantic_grammar, "clade_key", key);
  }
  append_integer(semantic_grammar, "production_key_count",
                 production_keys.size());
  for (auto const& key : production_keys) {
    append_framed(semantic_grammar, "production_key", key);
  }

  std::string pattern_bytes =
      "larch.topology-score-band.site-patterns.v1\n";
  append_integer(pattern_bytes, "taxon_count", patterns.taxon_count);
  for (auto const& label : grammar.taxa.id_to_sample_id) {
    append_framed(pattern_bytes, "taxon_label", label);
  }
  append_integer(pattern_bytes, "pattern_count", patterns.patterns.size());
  for (std::size_t pattern_id = 0; pattern_id < patterns.patterns.size();
       ++pattern_id) {
    auto const& pattern = patterns.patterns[pattern_id];
    append_integer(pattern_bytes, "pattern_id", pattern_id);
    append_integer(pattern_bytes, "state_mask_count",
                   pattern.state_by_taxon.size());
    for (auto state : pattern.state_by_taxon) {
      append_integer(pattern_bytes, "state_mask",
                     std::uint32_t{1} << state);
    }
    append_integer(pattern_bytes, "weight", pattern.weight);
    for (auto count : pattern.reference_state_counts) {
      append_integer(pattern_bytes, "reference_state_count", count);
    }
  }
  append_integer(pattern_bytes, "invariant_constant_excluding_ua",
                 patterns.invariant_constant_score_excluding_ua);
  append_integer(pattern_bytes, "invariant_constant_with_reference_edge",
                 patterns.invariant_constant_score_with_reference_edge);
  append_integer(
      pattern_bytes, "skipped_invariant_constant_with_reference_edge",
      patterns.skipped_invariant_constant_score_with_reference_edge);

  std::string alignment_bytes =
      "larch.topology-score-band.normalized-alignment.v1\n";
  append_integer(alignment_bytes, "taxon_count",
                 grammar.taxa.id_to_sample_id.size());
  for (auto const& label : grammar.taxa.id_to_sample_id) {
    append_framed(alignment_bytes, "taxon_label", label);
  }
  append_integer(alignment_bytes, "site_count", patterns.total_site_count);
  for (std::size_t site = 0; site < patterns.total_site_count; ++site) {
    auto pattern_id = patterns.original_site_to_pattern[site];
    append_integer(alignment_bytes, "site_offset", site);
    append_integer(alignment_bytes, "original_position", site + 1);
    append_integer(alignment_bytes, "pattern_id", pattern_id);
    for (auto state : patterns.patterns[pattern_id].state_by_taxon) {
      append_integer(alignment_bytes, "state", state);
    }
  }

  topology_score_band_derived_semantics result;
  result.alignment_sha256 = hash(alignment_bytes);
  result.ambiguity_policy = "strict_acgt_singleton_masks_v1";
  result.grammar_digest = hash(ordered_grammar);
  result.grammar_semantic_digest = hash(semantic_grammar);
  result.parsimony_model = "weighted_unit_cost_acgt_v1";
  result.reference_sha256 = hash(get_reference_sequence(source_dag));
  result.site_pattern_digest = hash(pattern_bytes);
  result.taxon_labels_sha256 = topology_score_band_taxon_labels_sha256(
      grammar.taxa.id_to_sample_id);
  result.ua_scoring =
      chart_options.score_ua_edge
          ? "fixed_reference_root_boundary_included_v1"
          : "fixed_reference_root_boundary_excluded_v1";
  std::string composite = "topology-landscape.input-content.v2\n";
  append_framed(composite, "taxon_labels_sha256",
                result.taxon_labels_sha256);
  append_framed(composite, "alignment_sha256", result.alignment_sha256);
  append_framed(composite, "site_pattern_digest",
                result.site_pattern_digest);
  append_framed(composite, "reference_sha256", result.reference_sha256);
  result.input_content_sha256 = hash(composite);
  return result;
}

std::string topology_score_band_provider_legacy_dag_semantic_sha256(
    phylo_dag& provider_dag, bool score_reference_edge) {
  return legacy_provider_semantic(provider_dag, score_reference_edge,
                                  "score-band provider identity");
}

std::uint64_t topology_score_band_provider_stored_history_parsimony_min(
    phylo_dag& provider_dag, bool score_reference_edge) {
  return provider_stored_history_minimum(
      provider_dag, score_reference_edge,
      "score-band provider stored-history minimum");
}

grammar_topology_band_export_result export_grammar_topology_band_raw(
    phylo_dag& source_dag, clade_grammar const& grammar,
    site_pattern_set const& patterns, chart_options const& chart_options,
    grammar_topology_band_result const& band,
    grammar_topology_band_export_options const& options) {
  if (band.worker_count == 0 || band.minimum_score > band.maximum_score ||
      band.maximum_score > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max()) ||
      band.census.expected_topology_count == 0 || band.selected.empty() ||
      band.census.ordinal_scores.size() !=
          band.census.expected_topology_count ||
      options.semantics.score_baseline != band.census.optimum) {
    throw std::runtime_error(
        "score-band export: inconsistent band interval, ledger, or baseline");
  }
  auto const derived = derive_topology_score_band_semantics(
      source_dag, grammar, patterns, chart_options,
      options.semantics.grammar_construction);
  auto const source_patterns = build_site_patterns(source_dag, grammar);
  auto const source_derived = derive_topology_score_band_semantics(
      source_dag, grammar, source_patterns, chart_options,
      options.semantics.grammar_construction);
  require_identity("source/live alignment", derived.alignment_sha256,
                   source_derived.alignment_sha256);
  require_identity("source/live site pattern", derived.site_pattern_digest,
                   source_derived.site_pattern_digest);
  require_identity("source/live input content", derived.input_content_sha256,
                   source_derived.input_content_sha256);
  require_identity("alignment", options.semantics.alignment_sha256,
                   derived.alignment_sha256);
  require_identity("ambiguity policy", options.semantics.ambiguity_policy,
                   derived.ambiguity_policy);
  require_identity("ordered grammar", options.semantics.grammar_digest,
                   derived.grammar_digest);
  require_identity("semantic grammar",
                   options.semantics.grammar_semantic_digest,
                   derived.grammar_semantic_digest);
  require_identity("input content", options.semantics.input_content_sha256,
                   derived.input_content_sha256);
  require_identity("parsimony model", options.semantics.parsimony_model,
                   derived.parsimony_model);
  require_identity("reference", options.semantics.reference_sha256,
                   derived.reference_sha256);
  require_identity("site pattern", options.semantics.site_pattern_digest,
                   derived.site_pattern_digest);
  require_identity("taxon labels", options.semantics.taxon_labels_sha256,
                   derived.taxon_labels_sha256);
  require_identity("UA scoring", options.semantics.ua_scoring,
                   derived.ua_scoring);
  auto const provider_input_sha256 = hash_file(
      options.provenance.provider_input_path, "provider input");
  auto const reference_input_sha256 = hash_file(
      options.provenance.reference_input_path, "reference input");
  auto const seed_tree_input_sha256 = hash_file(
      options.provenance.seed_tree_input_path, "seed-tree input");
  auto provider_reloaded =
      load_proto_dag(options.provenance.provider_input_path.native());
  auto const provider_artifact_semantic_sha256 = legacy_provider_semantic(
      provider_reloaded, chart_options.score_ua_edge,
      "score-band provider artifact");
  auto const provider_legacy_dag_semantic_sha256 = legacy_provider_semantic(
      source_dag, chart_options.score_ua_edge, "score-band live provider");
  require_identity("provider artifact/live DAG semantic",
                   provider_artifact_semantic_sha256,
                   provider_legacy_dag_semantic_sha256);
  if (get_reference_sequence(provider_reloaded) !=
      get_reference_sequence(source_dag)) {
    throw std::runtime_error(
        "score-band export: provider artifact/live reference identity drift");
  }
  auto const provider_patterns = build_site_patterns(provider_reloaded, grammar);
  auto const provider_derived = derive_topology_score_band_semantics(
      provider_reloaded, grammar, provider_patterns, chart_options,
      options.semantics.grammar_construction);
  require_identity("provider artifact/live alignment",
                   provider_derived.alignment_sha256,
                   source_derived.alignment_sha256);
  require_identity("provider artifact/live site pattern",
                   provider_derived.site_pattern_digest,
                   source_derived.site_pattern_digest);
  require_identity("provider artifact/live input content",
                   provider_derived.input_content_sha256,
                   source_derived.input_content_sha256);
  auto const parsed_reference =
      parsed_reference_artifact(options.provenance.reference_input_path);
  if (parsed_reference != get_reference_sequence(source_dag)) {
    throw std::runtime_error(
        "score-band export: reference artifact/live sequence identity drift");
  }
  require_identity("provider input",
                   options.provenance.provider_input_sha256,
                   provider_input_sha256);
  require_identity("reference input",
                   options.provenance.reference_input_sha256,
                   reference_input_sha256);
  require_identity("seed-tree input",
                   options.provenance.seed_tree_input_sha256,
                   seed_tree_input_sha256);
  require_identity("provider legacy DAG semantic",
                   options.provenance.provider_legacy_dag_semantic_sha256,
                   provider_legacy_dag_semantic_sha256);
  auto status = std::filesystem::symlink_status(options.output_directory);
  if (status.type() != std::filesystem::file_type::not_found) {
    throw std::runtime_error("score-band export: destination already exists");
  }
  auto staging = create_staging(options.output_directory);
  staging_guard cleanup{staging};

  grammar_topology_enumerator enumerator(grammar);
  std::set<std::uint64_t> ordinals;
  std::set<std::string> canonical_values;
  std::map<std::string, std::string> canonical_by_hash;
  std::vector<std::vector<std::string>> selected_rows;
  grammar_topology_band_export_result export_result;
  export_result.output_directory = options.output_directory;
  export_result.selected.reserve(band.selected.size());
  selected_rows.reserve(band.selected.size());

  std::optional<std::uint64_t> previous_ordinal;
  for (auto const& selected : band.selected) {
    if (selected.grammar_ordinal >= band.census.expected_topology_count ||
        (previous_ordinal && *previous_ordinal >= selected.grammar_ordinal) ||
        !ordinals.insert(selected.grammar_ordinal).second ||
        band.census.ordinal_scores[selected.grammar_ordinal] !=
            selected.absolute_score ||
        selected.absolute_score < band.minimum_score ||
        selected.absolute_score > band.maximum_score ||
        selected.absolute_score != selected.selected_grammar_sankoff_score) {
      throw std::runtime_error(
          "score-band export: selected ordinal/score ledger mismatch");
    }
    previous_ordinal = selected.grammar_ordinal;
    auto topology = enumerator.topology_at(selected.grammar_ordinal);
    auto actual_productions = selected_productions(topology);
    auto actual_keys = selected_keys(grammar, topology);
    if (actual_productions != selected.selected_productions ||
        actual_keys != selected.selected_production_keys) {
      throw std::runtime_error(
          "score-band export: selected production replay mismatch");
    }
    auto grammar_sankoff =
        score_selected_topology(grammar, patterns, topology, chart_options);
    if (grammar_sankoff != selected.absolute_score) {
      throw std::runtime_error(
          "score-band export: selected grammar Sankoff mismatch");
    }

    auto materialized =
        materialize_grammar_topology_tree(source_dag, grammar, topology);
    validate_dag(materialized, "score-band materialized topology",
                 thread_pool::get_default());
    auto materialized_fitch = score_materialized_tree(
        materialized, chart_options.score_ua_edge);
    auto materialized_rooted =
        topology::rooted_topology_from_phylo_tree(materialized);
    auto materialized_canonical =
        topology::canonical_tree_bytes(materialized_rooted);

    auto replay_path = staging /
        (".ordinal-" + std::to_string(selected.grammar_ordinal) + ".pb.gz");
    save_proto_dag(materialized, replay_path.native());
    auto reloaded = load_proto_dag(replay_path.native());
    std::filesystem::remove(replay_path);
    validate_dag(reloaded, "score-band reloaded topology",
                 thread_pool::get_default());
    if (!is_tree(reloaded)) {
      throw std::runtime_error(
          "score-band export: protobuf reload is not a tree");
    }
    auto reload_fitch =
        score_materialized_tree(reloaded, chart_options.score_ua_edge);
    auto reloaded_rooted = topology::rooted_topology_from_phylo_tree(reloaded);
    auto canonical = topology::canonical_tree_bytes(reloaded_rooted);
    if (canonical != materialized_canonical) {
      throw std::runtime_error(
          "score-band export: protobuf reload changed canonical topology");
    }
    auto parsed = topology::parse_canonical_tree(canonical);
    auto topology_hash = topology::topology_sha256(parsed);
    auto tree_sankoff = score_rooted_tree_sankoff(
        parsed, grammar.taxa.id_to_sample_id, patterns, chart_options);
    if (materialized_fitch != selected.absolute_score ||
        reload_fitch != selected.absolute_score ||
        tree_sankoff != selected.absolute_score) {
      throw std::runtime_error(
          "score-band export: five-oracle score mismatch");
    }

    clade_grammar_options grammar_options;
    grammar_options.allow_polytomies = true;
    auto reloaded_grammar =
        build_clade_grammar_with_audit(reloaded, grammar_options);
    auto dag_digest = build_canonical_dag_digest_report(
        reloaded_grammar.grammar, reload_fitch);
    auto legacy_selection =
        direct_kary_selection_sha256(selected.selected_production_keys);

    if (!canonical_values.insert(canonical).second) {
      throw std::runtime_error(
          "score-band export: duplicate canonical topology bytes");
    }
    auto [hash_it, inserted] =
        canonical_by_hash.emplace(topology_hash, canonical);
    if (!inserted) {
      if (hash_it->second != canonical) {
        throw std::runtime_error(
            "score-band export: canonical topology SHA-256 collision");
      }
      throw std::runtime_error(
          "score-band export: duplicate canonical topology hash");
    }

    selected_rows.push_back({
        std::to_string(selected.grammar_ordinal),
        std::to_string(selected.absolute_score),
        std::to_string(selected.absolute_score), "verified",
        std::to_string(grammar_sankoff), "verified",
        "larch_selected_grammar_stateless_sankoff_v1",
        stable_key_list(selected.selected_production_keys),
        production_id_list(selected.selected_productions),
        legacy_selection, canonical, topology_hash,
        std::to_string(topology::taxa(parsed).size()),
        std::to_string(parsed.children.size()),
        std::to_string(materialized_fitch), "verified",
        std::to_string(reload_fitch), "verified",
        std::to_string(tree_sankoff), "verified",
        "larch_canonical_tree_stateless_sankoff_v1", "true", "true",
        options.production_origin, dag_digest.semantic_sha256,
        dag_digest.clades_sha256, dag_digest.productions_sha256,
    });
    export_result.selected.push_back({
        .grammar_ordinal = selected.grammar_ordinal,
        .absolute_score = selected.absolute_score,
        .canonical_bytes = std::move(canonical),
        .topology_sha256 = std::move(topology_hash),
        .legacy_selection_sha256 = std::move(legacy_selection),
        .legacy_dag_semantic_sha256 = dag_digest.semantic_sha256,
        .legacy_dag_clades_sha256 = dag_digest.clades_sha256,
        .legacy_dag_productions_sha256 = dag_digest.productions_sha256,
    });
  }

  std::size_t expected_selected = 0;
  for (std::size_t ordinal = 0; ordinal < band.census.ordinal_scores.size();
       ++ordinal) {
    auto score = band.census.ordinal_scores[ordinal];
    if (score >= band.minimum_score && score <= band.maximum_score) {
      ++expected_selected;
      if (!ordinals.contains(ordinal)) {
        throw std::runtime_error(
            "score-band export: selected interval omits a census ordinal");
      }
    } else if (ordinals.contains(ordinal)) {
      throw std::runtime_error(
          "score-band export: selected interval contains an extra ordinal");
    }
  }
  if (expected_selected != band.selected.size()) {
    throw std::runtime_error(
        "score-band export: selected interval cardinality mismatch");
  }

  key_values semantics{
      {"alignment_sha256", derived.alignment_sha256},
      {"alphabet", "DNA"},
      {"ambiguity_policy", derived.ambiguity_policy},
      {"canonical_topology_encoding", "rooted-labelled-length-grammar-v1"},
      {"digest_algorithm", "sha256"},
      {"edge_weight", "unit"},
      {"endpoint_filtration", "maximum_endpoint_score_delta"},
      {"face_policy", "certified_commuting_rspr_square_deferred_v1"},
      {"grammar_construction", options.semantics.grammar_construction},
      {"grammar_digest", derived.grammar_digest},
      {"grammar_semantic_digest", derived.grammar_semantic_digest},
      {"grammar_topology_count",
       std::to_string(band.census.expected_topology_count)},
      {"hodge_metric", "not_applicable"},
      {"input_content_sha256", derived.input_content_sha256},
      {"move_family", "rooted_subtree_prune_regraft"},
      {"move_relation", "rooted_common_prune_reduction_hard_rspr_v1"},
      {"move_symmetry", "intrinsic_bidirectional"},
      {"null_family", "not_applicable"},
      {"numeric_policy", "exact_integer"},
      {"parsimony_model", derived.parsimony_model},
      {"polytomy_policy", "hard_multifurcation"},
      {"reference_sha256", derived.reference_sha256},
      {"root_policy", "rooted_no_synthetic_ua"},
      {"score_baseline", std::to_string(options.semantics.score_baseline)},
      {"score_view", "absolute_score_interval_inclusive_v1:" +
                         std::to_string(band.minimum_score) + ":" +
                         std::to_string(band.maximum_score)},
      {"site_pattern_digest", derived.site_pattern_digest},
      {"taxon_labels_sha256", derived.taxon_labels_sha256},
      {"taxon_order", "unsigned_utf8_byte_order"},
      {"ua_scoring", derived.ua_scoring},
      {"universe", options.semantics.universe},
  };
  key_values provenance{
      {"command", options.provenance.command},
      {"derivation_ref", options.provenance.derivation_ref},
      {"producer_commit", options.provenance.producer_commit},
      {"producer_dirty", options.provenance.producer_dirty},
      {"producer_repository", options.provenance.producer_repository},
      {"provider_input_sha256", provider_input_sha256},
      {"provider_legacy_dag_semantic_sha256",
       provider_legacy_dag_semantic_sha256},
      {"reference_input_sha256", reference_input_sha256},
      {"seed_tree_input_sha256", seed_tree_input_sha256},
      {"toolchain", options.provenance.toolchain},
      {"worker_count", std::to_string(band.worker_count)},
  };

  constexpr std::array census_header{std::string_view{"grammar_ordinal"},
                                     std::string_view{"absolute_score"}};
  std::vector<std::vector<std::string>> census_rows;
  census_rows.reserve(band.census.ordinal_scores.size());
  for (std::size_t ordinal = 0; ordinal < band.census.ordinal_scores.size();
       ++ordinal) {
    census_rows.push_back({std::to_string(ordinal),
                           std::to_string(band.census.ordinal_scores[ordinal])});
  }
  constexpr std::array selected_header{
      std::string_view{"grammar_ordinal"},
      std::string_view{"absolute_score"},
      std::string_view{"incremental_fitch_score"},
      std::string_view{"incremental_fitch_status"},
      std::string_view{"selected_grammar_sankoff_score"},
      std::string_view{"selected_grammar_sankoff_status"},
      std::string_view{"selected_grammar_sankoff_oracle"},
      std::string_view{"selected_production_keys"},
      std::string_view{"artifact_local_replay_ids"},
      std::string_view{"legacy_selection_sha256"},
      std::string_view{"canonical_bytes"},
      std::string_view{"topology_sha256"},
      std::string_view{"leaf_count"},
      std::string_view{"root_arity_summary"},
      std::string_view{"materialized_fitch_score"},
      std::string_view{"materialized_fitch_status"},
      std::string_view{"reload_fitch_score"},
      std::string_view{"reload_fitch_status"},
      std::string_view{"tree_native_sankoff_score"},
      std::string_view{"tree_native_sankoff_status"},
      std::string_view{"tree_native_sankoff_oracle"},
      std::string_view{"materialized_valid"},
      std::string_view{"reload_valid"},
      std::string_view{"production_origin"},
      std::string_view{"legacy_dag_semantic_sha256"},
      std::string_view{"legacy_dag_clades_sha256"},
      std::string_view{"legacy_dag_productions_sha256"},
  };

  std::map<std::string, std::string> payload;
  payload.emplace("semantics.tsv", key_value_bytes(semantics));
  payload.emplace("provenance.tsv", key_value_bytes(provenance));
  payload.emplace("score_census.tsv", table_bytes(census_header, census_rows));
  payload.emplace("selected_topologies.tsv",
                  table_bytes(selected_header, selected_rows));
  export_result.semantics_hash = identity(
      "larch.topology-score-band.semantics.v1\n", payload.at("semantics.tsv"));
  export_result.provenance_id = identity(
      "larch.topology-score-band.provenance.v1\n", payload.at("provenance.tsv"));
  export_result.semantic_data_sha256 = semantic_data_identity(payload);

  constexpr std::array file_header{
      std::string_view{"filename"}, std::string_view{"role"},
      std::string_view{"row_count"}, std::string_view{"sha256"}};
  std::vector<std::vector<std::string>> file_rows{
      {"provenance.tsv", "identity", "11", hash(payload.at("provenance.tsv"))},
      {"score_census.tsv", "claim",
       std::to_string(band.census.ordinal_scores.size()),
       hash(payload.at("score_census.tsv"))},
      {"selected_topologies.tsv", "table",
       std::to_string(selected_rows.size()),
       hash(payload.at("selected_topologies.tsv"))},
      {"semantics.tsv", "identity", "30", hash(payload.at("semantics.tsv"))},
  };
  payload.emplace("files.tsv", table_bytes(file_header, file_rows));
  key_values manifest{
      {"files_row_count", "4"},
      {"files_sha256", hash(payload.at("files.tsv"))},
      {"provenance_id", export_result.provenance_id},
      {"schema_name", "larch-topology-score-band-raw-v1"},
      {"schema_version", "1"},
      {"semantic_data_sha256", export_result.semantic_data_sha256},
      {"semantics_hash", export_result.semantics_hash},
  };
  payload.emplace("manifest.tsv", key_value_bytes(manifest));
  std::string outer_ledger;
  for (auto const& [name, bytes] : payload) {
    outer_ledger += hash(bytes) + "\t" + name + "\n";
  }
  payload.emplace("SHA256SUMS", std::move(outer_ledger));

  for (auto const& [name, bytes] : payload) {
    write_file(staging / name, bytes);
  }
  auto staged_validation =
      topology_landscape::validate_score_band_raw(staging);
  if (!staged_validation) throw_validation("staged", staged_validation);

  std::error_code publish_error;
  if (!publish_no_replace(staging, options.output_directory, publish_error)) {
    if (publish_error == std::errc::file_exists) {
      throw std::runtime_error(
          "score-band export: destination already exists");
    }
    throw std::runtime_error(
        "score-band export: no-replace publication failed: " +
        publish_error.message());
  }
  auto published_validation =
      topology_landscape::validate_score_band_raw(options.output_directory);
  if (!published_validation) {
    std::error_code ignored;
    std::filesystem::remove_all(options.output_directory, ignored);
    throw_validation("published", published_validation);
  }
  return export_result;
}

}  // namespace larch
