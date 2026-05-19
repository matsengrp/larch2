#include <larch/load_proto_dag.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/sample_method.hpp>
#include <larch/fasta.hpp>
#include <larch/vcf.hpp>
#include <larch/merge.hpp>
#include <larch/compute.hpp>
#include <larch/model_variant.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/weight_accumulator.hpp>
#include <larch/rf_distance.hpp>
#include <larch/thread_pool.hpp>
#include <larch/version.hpp>
#include <larch/pmr_arena.hpp>
#include <larch/io_util.hpp>
#include <larch/newick.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/parsimony_chart.hpp>
#include <larch/site_patterns.hpp>
#include <larch/chart_trim.hpp>
#include <larch/plateau.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// Read reference sequence from file (FASTA or raw text)
// ---------------------------------------------------------------------------

static nuc_base strict_fasta_nuc_from_char(char c, std::string_view label,
                                           mutation_position pos) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base{nuc_base::A};
    case 'C':
    case 'c':
      return nuc_base{nuc_base::C};
    case 'G':
    case 'g':
      return nuc_base{nuc_base::G};
    case 'T':
    case 't':
      return nuc_base{nuc_base::T};
    default:
      throw std::runtime_error(std::string{"FASTA/Newick input: non-ACGT "} +
                               std::string{label} + " nucleotide '" + c +
                               "' at position " + std::to_string(pos));
  }
}

static void validate_fasta_acgt_sequence(std::string_view sequence,
                                         std::string_view label) {
  for (std::size_t i = 0; i < sequence.size(); ++i)
    (void)strict_fasta_nuc_from_char(sequence[i], label, i + 1);
}

static std::string read_refseq(std::string_view path) {
  auto bytes = read_file(path);
  std::string_view content{bytes.data(), bytes.size()};
  if (!content.empty() && content[0] == '>') {
    auto entries = read_fasta(path);
    if (!entries.empty()) return std::move(entries[0].sequence);
  }
  std::string ref;
  for (char c : content) {
    if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
      ref += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return ref;
}

// ---------------------------------------------------------------------------
// FASTA+Newick DAG builder (same logic as larch2.cpp)
// ---------------------------------------------------------------------------

static phylo_dag build_from_fasta_newick(std::string_view fasta_path,
                                         std::string_view newick_path,
                                         std::string const& reference) {
  validate_fasta_acgt_sequence(reference, "reference");

  auto entries = read_fasta(fasta_path);
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto& e : entries) {
    validate_fasta_acgt_sequence(e.sequence,
                                 std::string{"sample '"} + e.name + "'");
    fasta_map[e.name] = std::move(e.sequence);
  }

  auto nw_bytes = read_file(newick_path);
  std::string newick_str{nw_bytes.data(), nw_bytes.size()};

  phylo_dag d;

  std::unordered_map<std::size_t, std::size_t> num_children;
  std::map<std::size_t, std::string> seq_ids;
  struct nw_edge_t {
    std::size_t parent, child, clade;
  };
  std::vector<nw_edge_t> nw_edges;

  parse_newick(
      newick_str,
      [&](std::size_t id, std::string_view label, std::optional<double>) {
        seq_ids[id] = std::string{label};
      },
      [&](std::size_t parent, std::size_t child) {
        nw_edges.push_back({parent, child, num_children[parent]++});
      });

  std::unordered_map<std::size_t, bool> has_children;
  std::unordered_map<std::size_t, bool> is_child_map;
  for (auto& e : nw_edges) {
    has_children[e.parent] = true;
    is_child_map[e.child] = true;
  }

  std::size_t newick_root = 0;
  for (auto& [id, _] : seq_ids) {
    if (!is_child_map.contains(id)) {
      newick_root = id;
      break;
    }
  }

  std::unordered_map<std::size_t, std::size_t> nw_to_dag;
  for (auto& [nw_id, label] : seq_ids) {
    bool is_leaf_node = !has_children.contains(nw_id);
    if (is_leaf_node) {
      auto leaf = d.append_node<node_kind::leaf>();
      if (!label.empty()) leaf.sample_id() = label;
      nw_to_dag[nw_id] = leaf.index();
    } else {
      auto inner = d.append_node<node_kind::inner>();
      nw_to_dag[nw_id] = inner.index();
    }
  }

  for (auto& ne : nw_edges) {
    auto edge = d.append_edge<edge_kind::clade>();
    edge.clade_index() = ne.clade;
    auto pi = nw_to_dag[ne.parent];
    auto ci = nw_to_dag[ne.child];
    std::visit([&](auto n) { edge.set_parent(n); }, d.get_node(pi));
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(ci));
  }

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = reference;
  d.set_root(ua);
  {
    auto edge = ua.append_child<edge_kind::clade>();
    auto root_dag_idx = nw_to_dag[newick_root];
    std::visit([&](auto n) { edge.set_child(n); }, d.get_node(root_dag_idx));
    edge.clade_index() = 0;
  }

  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            auto it = fasta_map.find(node.sample_id());
            if (it == fasta_map.end()) return;
            auto& seq = it->second;
            std::map<mutation_position, nuc_base> muts;
            for (std::size_t i = 0; i < seq.size() && i < reference.size();
                 ++i) {
              auto ref_base = strict_fasta_nuc_from_char(reference[i],
                                                         "reference", i + 1);
              auto seq_base = strict_fasta_nuc_from_char(
                  seq[i], std::string{"sample '"} + node.sample_id() + "'",
                  i + 1);
              if (!(ref_base == seq_base)) muts[i + 1] = seq_base;
            }
            node.cg() = compact_genome{std::move(muts)};
          }
        },
        nv);
  }

  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);
  return d;
}

// ---------------------------------------------------------------------------
// WRIC/chart diagnostic helpers
// ---------------------------------------------------------------------------

static char chart_state_label(std::uint8_t state) {
  switch (state) {
    case nuc_base::A:
      return 'A';
    case nuc_base::C:
      return 'C';
    case nuc_base::G:
      return 'G';
    case nuc_base::T:
      return 'T';
    default:
      return '?';
  }
}

static void print_chart_cost(std::ostream& out, chart_cost cost) {
  if (cost >= chart_inf)
    out << "INF";
  else
    out << cost;
}

static void print_chart_row(
    std::ostream& out,
    std::array<chart_cost, nuc_state_count> const& row) {
  out << "[";
  for (std::uint8_t state = 0; state < nuc_state_count; ++state) {
    if (state != 0) out << ", ";
    out << chart_state_label(state) << ":";
    print_chart_cost(out, row[state]);
  }
  out << "]";
}

static double elapsed_ms(std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

static bool within_limit(std::size_t printed, std::size_t limit) {
  return limit == 0 || printed < limit;
}

static std::size_t count_kept_clade_states(chart_trim_mask const& mask) {
  std::size_t count = 0;
  for (auto const& row : mask.keep_clade_state)
    for (bool keep : row)
      if (keep) ++count;
  return count;
}

static std::size_t count_true(std::vector<bool> const& values) {
  return static_cast<std::size_t>(
      std::count(values.begin(), values.end(), true));
}

static void print_frontier_size_stats(
    std::ostream& out, std::vector<std::size_t> const& frontier_sizes) {
  std::size_t sum = 0;
  std::size_t max_size = 0;
  std::map<std::size_t, std::size_t> hist;
  for (auto size : frontier_sizes) {
    sum += size;
    max_size = std::max(max_size, size);
    ++hist[size];
  }
  out << "  frontier_size_sum: " << sum << "\n";
  out << "  frontier_size_max: " << max_size << "\n";
  out << "  frontier_size_histogram:\n";
  if (hist.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [size, count] : hist)
      out << "    " << size << ": " << count << "\n";
  }
}

static std::vector<std::size_t> direct_parent_witness_nodes(
    grammar_production const& prod) {
  std::vector<std::size_t> nodes;
  nodes.reserve(prod.witnesses.size());
  for (auto const& witness : prod.witnesses) {
    if (witness.parent_node != std::numeric_limits<std::size_t>::max())
      nodes.push_back(witness.parent_node);
  }
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return nodes;
}

static void print_size_list(std::ostream& out,
                            std::vector<std::size_t> const& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << ", ";
    out << values[i];
  }
  out << "]";
}

static void print_wric_polytomy_audit_fields(
    std::ostream& out, polytomy_refinement_result const& refinement) {
  auto const& grammar = refinement.grammar;
  auto const& audit = refinement.audit;
  auto kary = kary_productions(grammar);
  std::map<std::size_t, std::size_t> arity_histogram;
  for (auto pid : kary)
    ++arity_histogram[grammar.productions[pid].children.size()];

  out << "  polytomy_refinement_label: "
      << polytomy_refinement_status_label(audit) << "\n";
  out << "  exact_for_soft_polytomies: "
      << (audit.exact_for_soft_polytomies ? "true" : "false") << "\n";
  out << "  any_truncated: " << (audit.any_truncated ? "true" : "false")
      << "\n";
  out << "  kary_productions: " << audit.source_kary_production_count << "\n";
  out << "  kary_production_arity_histogram:\n";
  if (arity_histogram.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& [arity, count] : arity_histogram)
      out << "    " << arity << ": " << count << "\n";
  }
  out << "  kary_production_details:\n";
  if (kary.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto pid : kary) {
      auto const& prod = grammar.productions[pid];
      auto parent_nodes =
          pid < refinement.production_info.size()
              ? refinement.production_info[pid].source_parent_nodes
              : direct_parent_witness_nodes(prod);
      out << "    - production: " << pid << "\n";
      out << "      parent_clade: " << prod.parent << "\n";
      out << "      arity: " << prod.children.size() << "\n";
      out << "      witness_multiplicity: " << prod.multiplicity << "\n";
      out << "      source_parent_nodes: ";
      print_size_list(out, parent_nodes);
      out << "\n";
    }
  }
  out << "  contains_kary_productions: "
      << (audit.contains_kary_productions ? "true" : "false") << "\n";
  out << "  binary_chart_compatible: "
      << (audit.binary_chart_compatible ? "true" : "false") << "\n";
  out << "  downstream_binary_charting_allowed: "
      << (audit.binary_chart_compatible && !audit.contains_kary_productions
              ? "true"
              : "false")
      << "\n";
  out << "  kary_grammar_diagnostic_only: "
      << (audit.contains_kary_productions ? "true" : "false") << "\n";
  out << "  polytomy_refinement_events:\n";
  if (audit.events.empty()) {
    out << "    <empty>\n";
  } else {
    for (auto const& event : audit.events) {
      out << "    - source_production: " << event.source_production << "\n";
      out << "      parent_clade: " << event.parent << "\n";
      out << "      arity: " << event.arity << "\n";
      out << "      selected_seed_shapes: "
          << event.selected_seed_shape_count << "\n";
      out << "      represented_refinement_count: "
          << event.represented_refinement_count
          << (event.refinement_count_saturated ? " (saturated)" : "")
          << "\n";
      out << "      exact: " << (event.exact ? "true" : "false") << "\n";
      out << "      truncated_by_shape_cap: "
          << (event.truncated_by_shape_cap ? "true" : "false") << "\n";
      out << "      truncated_by_production_cap: "
          << (event.truncated_by_production_cap ? "true" : "false")
          << "\n";
    }
  }
}

static void print_wric_polytomy_score_fields(
    std::ostream& out, polytomy_refinement_audit const& audit) {
  out << "  polytomy_refinement_label: "
      << polytomy_refinement_status_label(audit) << "\n";
  out << "  exact_for_full_soft_polytomy_space: "
      << (audit.exact_for_soft_polytomies ? "true" : "false") << "\n";
  out << "  binary_chart_compatible: "
      << (audit.binary_chart_compatible ? "true" : "false") << "\n";
  out << "  contains_kary_productions: "
      << (audit.contains_kary_productions ? "true" : "false") << "\n";
  if (!audit.exact_for_soft_polytomies) {
    out << "  score_scope: BOUNDED_REFINED_GRAMMAR\n";
  } else {
    out << "  score_scope: FULL_SOFT_POLYTOMY_SPACE\n";
  }
}

static char const* exact_or_bounded_score_kind(
    polytomy_refinement_audit const& audit) {
  return audit.exact_for_soft_polytomies ? "EXACT"
                                         : "BOUNDED_REFINED_GRAMMAR";
}

static void print_key_chart_clade_entries(
    std::ostream& out, clade_grammar const& grammar,
    single_site_chart const& chart, std::size_t entry_limit) {
  std::vector<clade_id> clades(grammar.clades.size());
  std::iota(clades.begin(), clades.end(), clade_id{0});
  std::stable_sort(clades.begin(), clades.end(), [&](clade_id lhs,
                                                     clade_id rhs) {
    if (lhs == grammar.root_clade) return true;
    if (rhs == grammar.root_clade) return false;
    auto lsize = grammar.clades[lhs].taxa.size();
    auto rsize = grammar.clades[rhs].taxa.size();
    if (lsize != rsize) return lsize > rsize;
    return lhs < rhs;
  });

  out << "  clade_entries:\n";
  std::size_t printed = 0;
  std::size_t eligible = 0;
  for (auto cid : clades) {
    auto size = grammar.clades[cid].taxa.size();
    if (cid != grammar.root_clade && size == 1) continue;
    ++eligible;
    if (!within_limit(printed, entry_limit)) continue;
    out << "    - clade: " << cid << "\n";
    out << "      size: " << size << "\n";
    out << "      parent_productions: "
        << grammar.productions_by_parent[cid].size() << "\n";
    out << "      inside: ";
    print_chart_row(out, chart.inside[cid]);
    out << "\n";
    ++printed;
  }
  out << "  clade_entries_printed: " << printed << "\n";
  out << "  clade_entries_total_internal_or_root: " << eligible << "\n";
  if (entry_limit != 0 && printed < eligible)
    out << "  clade_entries_truncated: true\n";
}

static void print_limited_per_pattern_roots(
    std::ostream& out, site_pattern_set const& patterns,
    composite_chart_score const& score, std::size_t entry_limit) {
  out << "  per_pattern_root_min:\n";
  std::size_t printed = 0;
  for (std::size_t i = 0; i < patterns.patterns.size(); ++i) {
    if (!within_limit(printed, entry_limit)) continue;
    out << "    - pattern: " << i << "\n";
    out << "      weight: " << patterns.patterns[i].weight << "\n";
    out << "      positions: " << patterns.patterns[i].positions.size()
        << "\n";
    out << "      root_min: ";
    print_chart_cost(out, score.per_pattern_root_min[i]);
    out << "\n";
    ++printed;
  }
  out << "  per_pattern_root_min_printed: " << printed << "\n";
  if (entry_limit != 0 && printed < patterns.patterns.size())
    out << "  per_pattern_root_min_truncated: true\n";
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage() {
  std::cerr <<
      R"(dagutil -- phylogenetic DAG merge / prune / inspect utility

Input (repeatable, at least one required):
  --dag-pb <path>         Input DAG in protobuf DAG format (.pb or .pb.gz)
  --tree-pb <path>        Input tree in parsimony protobuf format (requires --refseq)
  --fasta <path>          Input leaf sequences (FASTA, requires --newick and --refseq)
  --newick <path>         Newick tree file (paired with --fasta)
  --refseq <path>         Reference sequence file

  --vcf <path>            VCF file (required unless --force-no-vcf)
  --force-no-vcf          Skip VCF requirement

Output:
  -o, --output <path>     Output DAG in protobuf DAG format (optional)

Pruning:
  -t, --trim              Trim to best parsimony score
  --rf <path>             Trim to minimize RF distance to this DAG file
  -s, --sample            Sample a single tree from the DAG
  --sample-method <M>     random (default), parsimony, ml/thrifty, edge-weight
  --sample-uniformly      Weight sampling proportional to subtree tree-counts
  --model-dir <path>      Model directory for ml/thrifty sampling, --edge-ml,
                          or --edge-thrifty
  --model-name <name>     Model name for ml/thrifty sampling, --edge-ml,
                          or --edge-thrifty
  --score-ua-edge-ml      ML ignores UA->root by default; opt in to score it
  --ignore-ua-edge-ml     Explicitly keep the default ML UA-edge-ignore policy
  --seed <N>              Random seed for sampling

Notes:
  --sample-method applies only to --sample.  --edge-parsimony/--edge-ml write
  output edge_weight penalties and cannot combine with --trim or --sample.

Analysis:
  --dag-info              Print all DAG statistics (tree count, parsimony, RF)
  --wric-audit            Build collapsed clade grammar and print multiplicity
                          diagnostics (allows polytomies for audit only)
  --wric-polytomy-mode <M>
                          reject (default), audit-kary, expand-exact,
                          or expand-bounded for WRIC chart diagnostics
  --wric-polytomy-max-exact-arity <N>
                          Exact expansion arity cap (default 6)
  --wric-polytomy-max-shapes <N>
                          Bounded seed-shape cap per polytomy (default 16)
  --wric-polytomy-max-productions <N>
                          Per-polytomy production cap for expansion
  --chart-site <POS>      Build a one-site chart and print root optimum plus
                          key clade entries (POS is 1-based)
  --chart-pattern-info    Print exact/invariant/normalized site-pattern counts
                          using the collapsed clade grammar taxa
  --chart-trim-site <POS> Print single-site optimal production/clade-state
                          counts (POS is 1-based)
  --chart-composite-score Print summed per-pattern chart score labelled as a
                          LOWER_BOUND diagnostic
  --chart-bnb-trim        Run exact multi-site B&B trimming and print frontier
                          statistics (intended for small/medium DAGs)
  --chart-fluidity-site <POS>
                          Print a single-site chart trace/fluidity report
                          (alias: --plateau-site)
  --chart-score-ua-edge   Include the UA/reference edge in chart diagnostics
  --chart-entry-limit <N> Limit printed chart/pattern entries (default 20;
                          0 means no limit)
  --chart-bnb-max-frontier <N>
                          Fail if any B&B clade frontier exceeds N entries
                          (default 0, no cap)
  --chart-bnb-no-bound-pruning
                          Disable B&B lower-bound pruning for diagnostics
  --parsimony             Print parsimony score distribution
  --sum-rf-distance       Print sum RF distance distribution
  --edge-parsimony        Compute per-edge parsimony penalties (store in output;
                          cannot combine with --trim/--sample)
  --edge-ml, --edge-thrifty
                          Compute per-edge ML penalties (store in output;
                          cannot combine with --trim/--sample)

Debugging:
  --validate              Validate DAG invariants

Other:
  --version               Print version and exit
  -h, --help              Print this help
)";
}

struct args {
  std::vector<std::string> dag_pbs;
  std::vector<std::string> tree_pbs;
  std::vector<std::string> fastas;
  std::vector<std::string> newicks;
  std::string refseq;
  std::string vcf;
  bool force_no_vcf = false;
  std::string output;
  bool trim = false;
  std::string rf;
  bool sample = false;
  std::string sample_method_text = "random";
  sample_method sampling_method = sample_method::random;
  bool sample_method_explicit = false;
  bool sample_uniformly = false;
  std::string model_dir;
  std::string model_name;
  bool ignore_ua_edge_ml = true;
  std::optional<std::uint32_t> seed;
  bool dag_info = false;
  bool print_parsimony = false;
  bool print_rf_distance = false;
  bool edge_parsimony = false;
  bool edge_ml = false;
  bool validate = false;
  bool wric_audit = false;
  polytomy_refinement_options wric_polytomy_opts;
  bool wric_polytomy_mode_explicit = false;
  std::optional<mutation_position> chart_site;
  bool chart_pattern_info = false;
  std::optional<mutation_position> chart_trim_site;
  bool chart_composite_score = false;
  bool chart_bnb_trim = false;
  std::optional<mutation_position> chart_fluidity_site;
  bool chart_score_ua_edge = false;
  std::size_t chart_entry_limit = 20;
  std::size_t chart_bnb_max_frontier = 0;
  bool chart_bnb_no_bound_pruning = false;
};

static bool has_any_model_arg(args const& a) {
  return !a.model_dir.empty() || !a.model_name.empty();
}

static bool has_complete_model_args(args const& a) {
  return !a.model_dir.empty() && !a.model_name.empty();
}

static bool ml_model_requested(args const& a) {
  return a.edge_ml ||
         (a.sample && !a.trim && is_ml_sample_method(a.sampling_method));
}

static std::optional<polytomy_mode> parse_wric_polytomy_mode(
    std::string_view text) {
  if (text == "reject") return polytomy_mode::reject;
  if (text == "audit-kary" || text == "audit_kary")
    return polytomy_mode::audit_kary;
  if (text == "expand-exact" || text == "expand_exact" ||
      text == "expand_soft_exact_or_fail")
    return polytomy_mode::expand_soft_exact_or_fail;
  if (text == "expand-bounded" || text == "expand_bounded" ||
      text == "expand_soft_bounded")
    return polytomy_mode::expand_soft_bounded;
  return std::nullopt;
}

static args parse_args(int argc, char** argv) {
  args a;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto next = [&]() -> std::string_view {
      if (++i >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(1);
      }
      return argv[i];
    };
    if (arg == "--dag-pb")
      a.dag_pbs.emplace_back(next());
    else if (arg == "--tree-pb")
      a.tree_pbs.emplace_back(next());
    else if (arg == "--fasta")
      a.fastas.emplace_back(next());
    else if (arg == "--newick")
      a.newicks.emplace_back(next());
    else if (arg == "--refseq")
      a.refseq = next();
    else if (arg == "--vcf")
      a.vcf = next();
    else if (arg == "--force-no-vcf")
      a.force_no_vcf = true;
    else if (arg == "-o" || arg == "--output")
      a.output = next();
    else if (arg == "-t" || arg == "--trim")
      a.trim = true;
    else if (arg == "--rf")
      a.rf = next();
    else if (arg == "-s" || arg == "--sample")
      a.sample = true;
    else if (arg == "--sample-method") {
      a.sample_method_text = next();
      a.sample_method_explicit = true;
    } else if (arg == "--sample-uniformly")
      a.sample_uniformly = true;
    else if (arg == "--model-dir")
      a.model_dir = next();
    else if (arg == "--model-name")
      a.model_name = next();
    else if (arg == "--ignore-ua-edge-ml")
      a.ignore_ua_edge_ml = true;
    else if (arg == "--score-ua-edge-ml")
      a.ignore_ua_edge_ml = false;
    else if (arg == "--seed")
      a.seed = static_cast<uint32_t>(std::stoull(std::string{next()}));
    else if (arg == "--dag-info") {
      a.dag_info = true;
      a.print_parsimony = true;
      a.print_rf_distance = true;
    } else if (arg == "--parsimony") {
      a.dag_info = true;
      a.print_parsimony = true;
    } else if (arg == "--sum-rf-distance") {
      a.dag_info = true;
      a.print_rf_distance = true;
    } else if (arg == "--wric-audit") {
      a.wric_audit = true;
    } else if (arg == "--wric-polytomy-mode") {
      auto value = next();
      auto mode = parse_wric_polytomy_mode(value);
      if (!mode) {
        std::cerr << "error: unknown --wric-polytomy-mode '" << value
                  << "'\n";
        std::exit(1);
      }
      a.wric_polytomy_opts.mode = *mode;
      a.wric_polytomy_mode_explicit = true;
    } else if (arg == "--wric-polytomy-max-exact-arity") {
      a.wric_polytomy_opts.max_exact_arity = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
    } else if (arg == "--wric-polytomy-max-shapes") {
      a.wric_polytomy_opts.max_shapes_per_polytomy = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
    } else if (arg == "--wric-polytomy-max-productions") {
      auto value = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
      a.wric_polytomy_opts.max_new_productions_per_polytomy = value;
      a.wric_polytomy_opts.max_bounded_productions_per_polytomy = value;
    } else if (arg == "--chart-site") {
      auto pos =
          static_cast<mutation_position>(std::stoull(std::string{next()}));
      if (pos == 0) {
        std::cerr << "error: chart site positions are 1-based\n";
        std::exit(1);
      }
      a.chart_site = pos;
    } else if (arg == "--chart-pattern-info") {
      a.chart_pattern_info = true;
    } else if (arg == "--chart-trim-site") {
      auto pos =
          static_cast<mutation_position>(std::stoull(std::string{next()}));
      if (pos == 0) {
        std::cerr << "error: chart trim site positions are 1-based\n";
        std::exit(1);
      }
      a.chart_trim_site = pos;
    } else if (arg == "--chart-composite-score") {
      a.chart_composite_score = true;
    } else if (arg == "--chart-bnb-trim") {
      a.chart_bnb_trim = true;
    } else if (arg == "--chart-fluidity-site" || arg == "--plateau-site") {
      auto pos =
          static_cast<mutation_position>(std::stoull(std::string{next()}));
      if (pos == 0) {
        std::cerr << "error: chart fluidity site positions are 1-based\n";
        std::exit(1);
      }
      a.chart_fluidity_site = pos;
    } else if (arg == "--chart-score-ua-edge") {
      a.chart_score_ua_edge = true;
    } else if (arg == "--chart-entry-limit") {
      a.chart_entry_limit = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
    } else if (arg == "--chart-bnb-max-frontier") {
      a.chart_bnb_max_frontier = static_cast<std::size_t>(
          std::stoull(std::string{next()}));
    } else if (arg == "--chart-bnb-no-bound-pruning") {
      a.chart_bnb_no_bound_pruning = true;
    } else if (arg == "--edge-parsimony")
      a.edge_parsimony = true;
    else if (arg == "--edge-ml" || arg == "--edge-thrifty")
      a.edge_ml = true;
    else if (arg == "--validate")
      a.validate = true;
    else if (arg == "--version") {
      std::cerr << "dagutil " << larch::version << " (" << larch::git_commit
                << ")\n";
      std::exit(0);
    } else if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      usage();
      std::exit(1);
    }
  }

  auto total_inputs = a.dag_pbs.size() + a.tree_pbs.size() + a.fastas.size();
  if (total_inputs == 0) {
    std::cerr << "error: at least one input required (--dag-pb, --tree-pb, or "
                 "--fasta)\n";
    usage();
    std::exit(1);
  }
  if (!a.tree_pbs.empty() && a.refseq.empty()) {
    std::cerr << "error: --refseq is required with --tree-pb\n";
    std::exit(1);
  }
  if (!a.fastas.empty()) {
    if (a.fastas.size() != a.newicks.size()) {
      std::cerr << "error: each --fasta must be paired with a --newick\n";
      std::exit(1);
    }
    if (a.refseq.empty()) {
      std::cerr << "error: --refseq is required with --fasta\n";
      std::exit(1);
    }
  }
  if (a.vcf.empty() && !a.force_no_vcf) {
    std::cerr << "error: --vcf is required (use --force-no-vcf to skip)\n";
    std::exit(1);
  }
  auto parsed_method = parse_sample_method(a.sample_method_text);
  if (!parsed_method) {
    std::cerr << "error: unknown sample method '" << a.sample_method_text
              << "'\n";
    std::exit(1);
  }
  a.sampling_method = *parsed_method;

  if (has_any_model_arg(a) && !has_complete_model_args(a)) {
    std::cerr << "error: --model-dir and --model-name must be provided "
                 "together\n";
    std::exit(1);
  }
  if (a.sample_method_explicit && !a.sample) {
    std::cerr << "error: --sample-method requires --sample\n";
    std::exit(1);
  }
  if (a.sample_method_explicit && a.sample && a.output.empty()) {
    std::cerr << "error: --sample-method requires -o/--output because "
                 "sampling writes an output tree\n";
    std::exit(1);
  }
  if (a.trim && a.sample && a.sample_method_explicit) {
    if (!a.rf.empty()) {
      std::cerr << "error: --sample-method is not used with --trim --rf "
                   "--sample; omit --sample-method because --rf selects the "
                   "criterion\n";
      std::exit(1);
    }
    if (a.sampling_method != sample_method::parsimony) {
      std::cerr << "error: --sample-method applies to --sample without "
                   "--trim; omit --trim for "
                << a.sample_method_text << " sampling\n";
      std::exit(1);
    }
  }
  if (a.sample && !a.trim && is_ml_sample_method(a.sampling_method) &&
      !has_complete_model_args(a)) {
    std::cerr << "error: --model-dir and --model-name required with "
                 "--sample-method "
              << a.sample_method_text << "\n";
    std::exit(1);
  }
  if (a.edge_parsimony && a.edge_ml) {
    std::cerr << "error: choose only one of --edge-parsimony or --edge-ml\n";
    std::exit(1);
  }
  if ((a.edge_parsimony || a.edge_ml) && (a.trim || a.sample)) {
    std::cerr << "error: --edge-parsimony/--edge-ml cannot be combined with "
                 "--trim or --sample; write edge penalties to an output DAG "
                 "first, then run sampling/trimming in a second command\n";
    std::exit(1);
  }
  if (a.edge_ml && !has_complete_model_args(a)) {
    std::cerr << "error: --model-dir and --model-name required with "
                 "--edge-ml/--edge-thrifty\n";
    std::exit(1);
  }

  return a;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) try {
  auto a = parse_args(argc, argv);

  // ---- Load all inputs (in parallel) ----
  std::string refseq;
  if (!a.refseq.empty()) refseq = read_refseq(a.refseq);

  auto total_inputs = a.dag_pbs.size() + a.tree_pbs.size() + a.fastas.size();
  std::vector<phylo_dag> dags(total_inputs);
  {
    auto& pool = thread_pool::get_default();
    std::vector<std::size_t> indices(total_inputs);
    std::iota(indices.begin(), indices.end(), std::size_t{0});

    std::cerr << "Loading " << total_inputs << " input(s)...\n";
    parallel_for_each(pool, indices, [&](std::size_t idx) {
      auto dag_pb_count = a.dag_pbs.size();
      auto tree_pb_count = a.tree_pbs.size();
      if (idx < dag_pb_count) {
        dags[idx] = load_proto_dag(a.dag_pbs[idx]);
      } else if (idx < dag_pb_count + tree_pb_count) {
        auto ti = idx - dag_pb_count;
        dags[idx] = load_parsimony_tree(a.tree_pbs[ti], refseq);
      } else {
        auto fi = idx - dag_pb_count - tree_pb_count;
        dags[idx] =
            build_from_fasta_newick(a.fastas[fi], a.newicks[fi], refseq);
      }
    });
    std::cerr << "Loading done.\n";
  }

  // ---- Apply VCF (in parallel) ----
  if (!a.vcf.empty()) {
    auto& pool = thread_pool::get_default();
    std::vector<std::size_t> indices(dags.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    parallel_for_each(pool, indices, [&](std::size_t idx) {
      auto const& ref = get_reference_sequence(dags[idx]);
      auto vcf = read_vcf(a.vcf, ref);
      apply_vcf_to_dag(dags[idx], vcf);
    });
  }

  // ---- Merge ----
  auto const& ref = get_reference_sequence(dags.front());
  merge m{ref};
  for (auto& dag : dags) m.add_dag(dag);

  auto& result = m.get_result();
  auto root_idx = get_root_idx(result);

  std::cout << "leaves: " << leaf_count(result) << "\n";
  std::cout << "nodes: " << node_count(result) << "\n";
  std::cout << "edges: " << edge_count(result) << "\n";

  if (a.validate)
    validate_dag(result, "after merge", thread_pool::get_default());

  std::unique_ptr<ml_model> ml_model_storage;
  if (ml_model_requested(a)) {
    assert(has_complete_model_args(a) &&
           "parse_args must require model args before ML loading");
    std::cerr << "Loading ML model " << a.model_name << "...\n";
    ml_model_storage =
        std::make_unique<ml_model>(load_ml_model(a.model_dir, a.model_name));
    std::cerr << "  ML model loaded\n";
  }

  // ---- Analysis ----
  std::optional<polytomy_refinement_result> chart_refinement_cache;
  double chart_grammar_build_ms = 0.0;
  auto get_chart_refinement = [&]() -> polytomy_refinement_result& {
    if (!chart_refinement_cache) {
      auto start = std::chrono::steady_clock::now();
      clade_grammar_options grammar_opts;
      chart_refinement_cache = build_polytomy_refined_clade_grammar(
          result, grammar_opts, a.wric_polytomy_opts);
      chart_grammar_build_ms =
          elapsed_ms(start, std::chrono::steady_clock::now());
      require_polytomy_refinement_binary_charting(
          chart_refinement_cache->audit,
          "WRIC chart diagnostics (use --wric-polytomy-mode expand-exact "
          "or expand-bounded for soft polytomies)");
    }
    return *chart_refinement_cache;
  };
  auto get_chart_grammar = [&]() -> clade_grammar& {
    return get_chart_refinement().grammar;
  };

  std::optional<site_pattern_set> exact_patterns_cache;
  double exact_pattern_build_ms = 0.0;
  auto get_exact_patterns = [&]() -> site_pattern_set& {
    if (!exact_patterns_cache) {
      auto& grammar = get_chart_grammar();
      site_pattern_options pattern_opts;
      auto start = std::chrono::steady_clock::now();
      exact_patterns_cache = build_site_patterns(result, grammar, pattern_opts);
      exact_pattern_build_ms = elapsed_ms(start, std::chrono::steady_clock::now());
    }
    return *exact_patterns_cache;
  };

  if (a.wric_audit) {
    clade_grammar_options grammar_opts;
    auto refinement_opts = a.wric_polytomy_opts;
    if (!a.wric_polytomy_mode_explicit)
      refinement_opts.mode = polytomy_mode::audit_kary;
    auto start = std::chrono::steady_clock::now();
    auto refined = build_polytomy_refined_clade_grammar(
        result, grammar_opts, refinement_opts);
    auto build_ms = elapsed_ms(start, std::chrono::steady_clock::now());
    print_clade_grammar_audit(std::cout, refined.source_grammar_audit);
    print_wric_polytomy_audit_fields(std::cout, refined);
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << build_ms << "\n";
  }

  if (a.chart_site) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;

    auto state_start = std::chrono::steady_clock::now();
    auto states = extract_leaf_site_states(result, grammar, *a.chart_site);
    auto state_ms = elapsed_ms(state_start, std::chrono::steady_clock::now());

    auto chart_start = std::chrono::steady_clock::now();
    auto chart = build_single_site_chart(grammar, states, chart_opts);
    auto chart_ms = elapsed_ms(chart_start, std::chrono::steady_clock::now());

    auto reference_state = extract_reference_site_state(result, *a.chart_site);
    auto optimum = root_min(chart, grammar.root_clade, chart_opts,
                            reference_state);

    std::cout << "chart_site:\n";
    std::cout << "  pos: " << *a.chart_site << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    std::cout << "  reference_state: " << chart_state_label(reference_state)
              << "\n";
    std::cout << "  taxa: " << grammar.taxa.id_to_sample_id.size() << "\n";
    std::cout << "  clades: " << grammar.clades.size() << "\n";
    std::cout << "  productions: " << grammar.productions.size() << "\n";
    std::cout << "  root_clade: " << grammar.root_clade << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement.audit);
    std::cout << "  root_min: ";
    print_chart_cost(std::cout, optimum);
    std::cout << "\n";
    std::cout << "  root_inside: ";
    print_chart_row(std::cout, chart.inside[grammar.root_clade]);
    std::cout << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  leaf_state_extract_ms: " << std::fixed
              << std::setprecision(3) << state_ms << "\n";
    std::cout << "  chart_build_ms: " << std::fixed << std::setprecision(3)
              << chart_ms << "\n";
    print_key_chart_clade_entries(std::cout, grammar, chart,
                                  a.chart_entry_limit);
  }

  if (a.chart_pattern_info) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    site_pattern_options pattern_opts;
    pattern_opts.build_normalized_binary_patterns = true;
    auto pattern_start = std::chrono::steady_clock::now();
    auto patterns = build_site_patterns(result, grammar, pattern_opts);
    auto pattern_ms = elapsed_ms(pattern_start, std::chrono::steady_clock::now());
    print_site_pattern_summary(std::cout, patterns);
    print_wric_polytomy_score_fields(std::cout, refinement.audit);
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  pattern_build_ms: " << std::fixed
              << std::setprecision(3) << pattern_ms << "\n";
  }

  if (a.chart_trim_site) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;

    auto state_start = std::chrono::steady_clock::now();
    auto states = extract_leaf_site_states(result, grammar, *a.chart_trim_site);
    auto state_ms = elapsed_ms(state_start, std::chrono::steady_clock::now());

    auto chart_start = std::chrono::steady_clock::now();
    auto chart = build_single_site_chart(grammar, states, chart_opts);
    auto chart_ms = elapsed_ms(chart_start, std::chrono::steady_clock::now());

    auto outside_start = std::chrono::steady_clock::now();
    auto outside = build_single_site_outside_chart(
        grammar, chart, chart_opts, result, *a.chart_trim_site);
    auto outside_ms = elapsed_ms(outside_start, std::chrono::steady_clock::now());

    chart_trim_options trim_opts;
    trim_opts.store_optimal_choices = true;
    auto trim_start = std::chrono::steady_clock::now();
    auto mask = build_single_site_trim_mask(grammar, chart, outside, trim_opts);
    auto trim_ms = elapsed_ms(trim_start, std::chrono::steady_clock::now());

    std::cout << "chart_trim_site:\n";
    std::cout << "  pos: " << *a.chart_trim_site << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement.audit);
    std::cout << "  global_min: ";
    print_chart_cost(std::cout, mask.global_min);
    std::cout << "\n";
    std::cout << "  kept_productions: " << count_true(mask.keep_production)
              << "\n";
    std::cout << "  total_productions: " << grammar.productions.size()
              << "\n";
    std::cout << "  kept_clade_states: "
              << count_kept_clade_states(mask) << "\n";
    std::cout << "  total_clade_states: "
              << grammar.clades.size() * nuc_state_count << "\n";
    std::cout << "  kept_production_choice_count: "
              << mask.kept_production_choice_count << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  leaf_state_extract_ms: " << std::fixed
              << std::setprecision(3) << state_ms << "\n";
    std::cout << "  chart_build_ms: " << std::fixed << std::setprecision(3)
              << chart_ms << "\n";
    std::cout << "  outside_build_ms: " << std::fixed << std::setprecision(3)
              << outside_ms << "\n";
    std::cout << "  trim_mask_build_ms: " << std::fixed
              << std::setprecision(3) << trim_ms << "\n";
    std::cout << "  kept_productions_by_parent_clade:\n";
    std::size_t printed = 0;
    std::size_t eligible = 0;
    for (clade_id cid = 0; cid < grammar.productions_by_parent.size(); ++cid) {
      std::size_t kept = 0;
      for (auto pid : grammar.productions_by_parent[cid])
        if (mask.keep_production[pid]) ++kept;
      if (kept == 0) continue;
      ++eligible;
      if (!within_limit(printed, a.chart_entry_limit)) continue;
      std::cout << "    " << cid << ": " << kept << "/"
                << grammar.productions_by_parent[cid].size() << "\n";
      ++printed;
    }
    std::cout << "  kept_parent_clades_printed: " << printed << "\n";
    if (a.chart_entry_limit != 0 && printed < eligible)
      std::cout << "  kept_parent_clades_truncated: true\n";
  }

  if (a.chart_composite_score) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    auto& patterns = get_exact_patterns();
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;

    auto composite_start = std::chrono::steady_clock::now();
    auto composite = build_composite_chart_score(grammar, patterns, chart_opts);
    auto composite_ms = elapsed_ms(composite_start,
                                   std::chrono::steady_clock::now());

    std::cout << "chart_composite_score:\n";
    std::cout << "  score_kind: LOWER_BOUND\n";
    print_wric_polytomy_score_fields(std::cout, refinement.audit);
    std::cout << "  weighted_lower_bound: "
              << composite.weighted_lower_bound << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    std::cout << "  exact_patterns: " << patterns.patterns.size() << "\n";
    std::cout << "  total_sites: " << patterns.total_site_count << "\n";
    std::cout << "  invariant_sites: " << patterns.invariant_site_count
              << "\n";
    std::cout << "  skipped_invariant_sites: "
              << patterns.skipped_invariant_site_count << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  pattern_build_ms: " << std::fixed
              << std::setprecision(3) << exact_pattern_build_ms << "\n";
    std::cout << "  composite_chart_ms: " << std::fixed
              << std::setprecision(3) << composite_ms << "\n";
    print_limited_per_pattern_roots(std::cout, patterns, composite,
                                    a.chart_entry_limit);
  }

  if (a.chart_bnb_trim) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    auto& patterns = get_exact_patterns();
    chart_options chart_opts;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;
    multisite_trim_options trim_opts;
    trim_opts.use_bound_pruning = !a.chart_bnb_no_bound_pruning;
    trim_opts.max_frontier_entries_per_clade = a.chart_bnb_max_frontier;

    auto bnb_start = std::chrono::steady_clock::now();
    auto trim = build_multisite_trim(grammar, patterns, chart_opts, trim_opts);
    auto bnb_ms = elapsed_ms(bnb_start, std::chrono::steady_clock::now());

    std::cout << "chart_bnb_trim:\n";
    std::cout << "  score_kind: "
              << exact_or_bounded_score_kind(refinement.audit) << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement.audit);
    std::cout << "  optimum: " << trim.optimum << "\n";
    std::cout << "  composite_lower_bound_kind: LOWER_BOUND\n";
    std::cout << "  composite_lower_bound: "
              << trim.composite_lower_bound << "\n";
    std::cout << "  initial_upper_bound: " << trim.initial_upper_bound
              << "\n";
    std::cout << "  score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    std::cout << "  active_patterns: " << trim.active_pattern_count << "\n";
    std::cout << "  exact_patterns: " << patterns.patterns.size() << "\n";
    std::cout << "  invariant_constant_offset: "
              << trim.invariant_constant_offset << "\n";
    std::cout << "  kept_productions: " << count_true(trim.keep_production)
              << "\n";
    std::cout << "  total_productions: " << grammar.productions.size()
              << "\n";
    std::cout << "  equality_deduplicated: "
              << trim.equality_deduplicated << "\n";
    std::cout << "  dominance_pruned: " << trim.dominance_pruned << "\n";
    std::cout << "  bound_pruned: " << trim.bound_pruned << "\n";
    std::cout << "  bound_pruning: "
              << (trim_opts.use_bound_pruning ? "true" : "false") << "\n";
    std::cout << "  max_frontier_entries_per_clade: "
              << trim_opts.max_frontier_entries_per_clade << "\n";
    std::cout << "  grammar_build_ms: " << std::fixed
              << std::setprecision(3) << chart_grammar_build_ms << "\n";
    std::cout << "  pattern_build_ms: " << std::fixed
              << std::setprecision(3) << exact_pattern_build_ms << "\n";
    std::cout << "  bnb_trim_ms: " << std::fixed << std::setprecision(3)
              << bnb_ms << "\n";
    print_frontier_size_stats(std::cout, trim.frontier_sizes_by_clade);
  }

  if (a.chart_fluidity_site) {
    auto& refinement = get_chart_refinement();
    auto& grammar = refinement.grammar;
    auto states =
        extract_leaf_site_states(result, grammar, *a.chart_fluidity_site);
    chart_options chart_opts;
    chart_opts.keep_trace = true;
    chart_opts.score_ua_edge = a.chart_score_ua_edge;
    auto chart = build_single_site_chart(grammar, states, chart_opts);
    auto outside = build_single_site_outside_chart(
        grammar, chart, chart_opts, result, *a.chart_fluidity_site);
    auto report = build_single_site_fluidity_report(grammar, chart, outside);
    std::cout << "chart_fluidity_site: " << *a.chart_fluidity_site << "\n";
    std::cout << "chart_fluidity_score_ua_edge: "
              << (a.chart_score_ua_edge ? "true" : "false") << "\n";
    print_wric_polytomy_score_fields(std::cout, refinement.audit);
    print_fluidity_report(std::cout, grammar, report);
  }

  if (a.dag_info) {
    tree_count_ops tc_ops;
    subtree_weight<tree_count_ops> tc_sw(result, a.seed);
    auto tree_count = tc_sw.compute_weight_below(root_idx, tc_ops);
    std::cout << "tree_count: " << tree_count << "\n";

    if (a.print_parsimony) {
      parsimony_score_ops pops;
      weight_accumulator<parsimony_score_ops> wa_pops(pops);
      subtree_weight<weight_accumulator<parsimony_score_ops>> psw(result,
                                                                  a.seed);
      auto all_parsimony = psw.compute_weight_below(root_idx, wa_pops);
      auto const& pweights = all_parsimony.get_weights();

      std::cout << "parsimony_all: " << pweights.size() << "\n"
                << all_parsimony << "\n";
      if (!pweights.empty()) {
        auto min_it = pweights.begin();
        auto max_it = std::prev(pweights.end());
        std::cout << "parsimony_min: score:" << min_it->first
                  << ", count:" << min_it->second << "\n";
        std::cout << "parsimony_max: score:" << max_it->first
                  << ", count:" << max_it->second << "\n";
      }
    }

    if (a.print_rf_distance) {
      sum_rf_distance_ops rf_ops{m, m};
      sum_rf_distance srf(rf_ops);
      weight_accumulator<sum_rf_distance> wa_rf(srf);
      subtree_weight<weight_accumulator<sum_rf_distance>> rf_sw(result, a.seed);
      auto all_rf = rf_sw.compute_weight_below(root_idx, wa_rf);
      auto const& rf_weights = all_rf.get_weights();

      auto shift_sum = srf.get_ops().get_shift_sum();

      // Shift scores and rebuild map
      using rf_wc = weight_counter<sum_rf_distance>;
      rf_wc::map_type shifted;
      for (auto const& [score, count] : rf_weights)
        shifted[score + shift_sum] += count;
      rf_wc shifted_wc(std::move(shifted));

      std::cout << "sum_rf_dist_all: " << shifted_wc.get_weights().size()
                << "\n"
                << shifted_wc << "\n";
      if (!shifted_wc.get_weights().empty()) {
        auto min_it = shifted_wc.get_weights().begin();
        auto max_it = std::prev(shifted_wc.get_weights().end());
        std::cout << "sum_rf_dist_min: score:" << min_it->first
                  << ", count:" << min_it->second << "\n";
        std::cout << "sum_rf_dist_max: score:" << max_it->first
                  << ", count:" << max_it->second << "\n";
      }
    }
  }

  // ---- Per-edge global penalties ----
  std::vector<float> edge_penalties;
  if (a.edge_parsimony) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
    auto global_min = sw.compute_weight_below(root_idx, pops);
    auto scores = sw.compute_edge_min_global_scores(pops);

    // Convert to penalties (float), preserving edge-index addressing for
    // save_proto_dag() while reporting counts over live edges only.
    edge_penalties.assign(scores.size(), 0.0f);
    std::size_t zero_penalty = 0;
    std::size_t nonzero_penalty = 0;
    std::size_t max_penalty = 0;
    for (auto ev : result.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            auto i = edge.index();
            assert(scores[i] >= global_min);
            auto penalty = scores[i] - global_min;
            edge_penalties[i] = static_cast<float>(penalty);
            if (penalty == 0)
              ++zero_penalty;
            else
              ++nonzero_penalty;
            if (penalty > max_penalty) max_penalty = penalty;
          },
          ev);
    }

    std::cerr << "edge_parsimony: global_min=" << global_min
              << " edges=" << edge_count(result)
              << " zero_penalty=" << zero_penalty
              << " nonzero_penalty=" << nonzero_penalty
              << " max_penalty=" << max_penalty << "\n";
  }
  if (a.edge_ml) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();
    assert(ml_model_storage != nullptr);

    auto const& ml_ref = get_reference_sequence(result);
    ml_model_likelihood_score_ops ml_ops{.model = *ml_model_storage,
                                         .reference = ml_ref,
                                         .ignore_ua_edge =
                                             a.ignore_ua_edge_ml};
    subtree_weight<ml_model_likelihood_score_ops> sw(result, a.seed, mr);
    auto global_min = sw.compute_weight_below(root_idx, ml_ops);
    if (!std::isfinite(global_min)) {
      std::cerr << "error: non-finite edge-ML global minimum\n";
      std::exit(1);
    }
    auto scores = sw.compute_edge_min_global_scores(ml_ops);

    // Convert to penalties (float).  Treat tiny negative/positive deviations
    // around zero as floating-point noise so globally optimal edges round to 0.
    double eps = 1e-8 * std::max(1.0, std::abs(global_min));
    edge_penalties.assign(scores.size(), 0.0f);
    std::size_t zero_penalty = 0;
    std::size_t nonzero_penalty = 0;
    double max_penalty = 0.0;
    for (auto ev : result.get_all_edges()) {
      std::visit(
          [&](auto edge) {
            auto i = edge.index();
            if (!std::isfinite(scores[i])) {
              std::cerr << "error: non-finite edge-ML score at edge " << i
                        << "\n";
              std::exit(1);
            }
            double penalty = scores[i] - global_min;
            if (penalty < 0.0 && penalty >= -eps) penalty = 0.0;
            if (penalty < 0.0) {
              std::cerr << "error: edge-ML score below global minimum at edge "
                        << i << " (score=" << scores[i]
                        << ", global_min=" << global_min << ")\n";
              std::exit(1);
            }
            if (penalty <= eps) penalty = 0.0;
            edge_penalties[i] = static_cast<float>(penalty);
            if (penalty == 0.0)
              ++zero_penalty;
            else
              ++nonzero_penalty;
            if (penalty > max_penalty) max_penalty = penalty;
          },
          ev);
    }

    std::cerr << "edge_ml: global_min=" << std::fixed << std::setprecision(6)
              << global_min << " edges=" << edge_count(result)
              << " zero_penalty=" << zero_penalty
              << " nonzero_penalty=" << nonzero_penalty
              << " max_penalty=" << max_penalty
              << (a.ignore_ua_edge_ml ? " (UA edge ignored)"
                                      : " (UA edge scored)")
              << "\n";
  }

  // ---- Output ----
  if (!a.output.empty()) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();

    if ((a.edge_parsimony || a.edge_ml) && !a.trim && !a.sample) {
      save_proto_dag(result, a.output, edge_penalties);
    } else if (a.trim) {
      if (a.rf.empty()) {
        // Trim to best parsimony
        parsimony_score_ops pops;
        subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
        sw.compute_weight_below(root_idx, pops);
        if (a.sample) {
          std::cerr << "Sampling a tree from min-parsimony options...\n";
          auto tree = a.sample_uniformly ? sw.min_weight_uniform_sample_tree(pops)
                                         : sw.min_weight_sample_tree(pops);
          save_proto_dag(tree, a.output);
        } else {
          std::cerr << "Trimming DAG to min parsimony...\n";
          auto trimmed = sw.trim_to_min_weight(pops);
          save_proto_dag(trimmed, a.output);
        }
      } else {
        // Trim to minimize RF distance to provided DAG
        auto rf_dag = load_proto_dag(a.rf);
        merge rf_m{get_reference_sequence(rf_dag)};
        rf_m.add_dag(rf_dag);

        sum_rf_distance_ops rf_ops{rf_m, m};
        sum_rf_distance srf(rf_ops);
        subtree_weight<sum_rf_distance> sw(result, a.seed, mr);
        sw.compute_weight_below(root_idx, srf);
        if (a.sample) {
          std::cerr << "Sampling a tree from min-RF options...\n";
          auto tree = a.sample_uniformly ? sw.min_weight_uniform_sample_tree(srf)
                                         : sw.min_weight_sample_tree(srf);
          save_proto_dag(tree, a.output);
        } else {
          std::cerr << "Trimming DAG to min RF distance...\n";
          auto trimmed = sw.trim_to_min_weight(srf);
          save_proto_dag(trimmed, a.output);
        }
      }
    } else if (a.sample) {
      switch (a.sampling_method) {
        case sample_method::random: {
          std::cerr << "Sampling a random tree from the DAG...\n";
          parsimony_score_ops pops;
          subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
          sw.compute_weight_below(root_idx, pops);
          auto tree = a.sample_uniformly ? sw.uniform_sample_tree(pops)
                                         : sw.sample_tree(pops);
          if (a.validate)
            validate_dag(tree, "sampled random tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::parsimony: {
          std::cerr << "Sampling a tree from min-parsimony options...\n";
          parsimony_score_ops pops;
          subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
          auto min_score = sw.compute_weight_below(root_idx, pops);
          auto tree = a.sample_uniformly
                          ? sw.min_weight_uniform_sample_tree(pops)
                          : sw.min_weight_sample_tree(pops);
          std::cerr << "sampled_tree: parsimony_min=" << min_score << "\n";
          if (a.validate)
            validate_dag(tree, "sampled min-parsimony tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::edge_weight: {
          std::cerr << "Sampling a tree from min-edge-weight options...\n";
          edge_weight_score_ops ew_ops;
          subtree_weight<edge_weight_score_ops> sw(result, a.seed, mr);
          auto min_score = sw.compute_weight_below(root_idx, ew_ops);
          auto tree = a.sample_uniformly
                          ? sw.min_weight_uniform_sample_tree(ew_ops)
                          : sw.min_weight_sample_tree(ew_ops);
          std::cerr << "sampled_tree: edge_weight_min=" << std::fixed
                    << std::setprecision(6) << min_score << "\n";
          if (a.validate)
            validate_dag(tree, "sampled min-edge-weight tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::ml: {
          assert(ml_model_storage != nullptr);
          std::cerr << "Sampling a tree from min-ML options...\n";
          auto const& ml_ref = get_reference_sequence(result);
          ml_model_likelihood_score_ops ml_ops{.model = *ml_model_storage,
                                               .reference = ml_ref,
                                               .ignore_ua_edge =
                                                   a.ignore_ua_edge_ml};
          subtree_weight<ml_model_likelihood_score_ops> sw(result, a.seed, mr);
          auto ml_min = sw.compute_weight_below(root_idx, ml_ops);
          if (!std::isfinite(ml_min)) {
            std::cerr << "error: non-finite ML sample score\n";
            std::exit(1);
          }
          auto tree = a.sample_uniformly
                          ? sw.min_weight_uniform_sample_tree(ml_ops)
                          : sw.min_weight_sample_tree(ml_ops);
          std::cerr << "sampled_tree: ML_NLL=" << std::fixed
                    << std::setprecision(6) << ml_min
                    << (a.ignore_ua_edge_ml ? " (UA edge ignored)"
                                            : " (UA edge scored)")
                    << "\n";
          if (a.validate)
            validate_dag(tree, "sampled min-ML tree",
                         thread_pool::get_default());
          save_proto_dag(tree, a.output);
          break;
        }
        case sample_method::rf_minsum:
        case sample_method::rf_maxsum:
          std::cerr << "error: --sample-method "
                    << format_sample_method(a.sampling_method)
                    << " is not supported by dagutil sampling\n";
          std::exit(1);
      }
    } else {
      save_proto_dag(result, a.output);
    }
  }

  return EXIT_SUCCESS;
} catch (std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return EXIT_FAILURE;
}
