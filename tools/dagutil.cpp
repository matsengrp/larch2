#include <larch/load_proto_dag.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/fasta.hpp>
#include <larch/vcf.hpp>
#include <larch/merge.hpp>
#include <larch/compute.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/weight_accumulator.hpp>
#include <larch/rf_distance.hpp>
#include <larch/thread_pool.hpp>
#include <larch/version.hpp>
#include <larch/pmr_arena.hpp>
#include <larch/io_util.hpp>
#include <larch/newick.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// FASTA+Newick DAG builder (same logic as larch2.cpp)
// ---------------------------------------------------------------------------

static phylo_dag build_from_fasta_newick(std::string_view fasta_path,
                                         std::string_view newick_path,
                                         std::string_view refseq_path) {
  auto ref_bytes = read_file(refseq_path);
  std::string_view ref_content{ref_bytes.data(), ref_bytes.size()};
  std::string reference;
  if (!ref_content.empty() && ref_content[0] == '>') {
    auto entries = read_fasta(refseq_path);
    if (!entries.empty()) reference = std::move(entries[0].sequence);
  } else {
    for (char c : ref_content) {
      if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        reference +=
            static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }

  auto entries = read_fasta(fasta_path);
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto& e : entries) fasta_map[e.name] = std::move(e.sequence);

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
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [&](auto n) {
            if (n.index() == pi) edge.set_parent(n);
            if (n.index() == ci) edge.set_child(n);
          },
          nv);
    }
  }

  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = reference;
  d.set_root(ua);
  {
    auto edge = ua.append_child<edge_kind::clade>();
    auto root_dag_idx = nw_to_dag[newick_root];
    for (auto nv : d.get_all_nodes()) {
      std::visit(
          [&](auto n) {
            if (n.index() == root_dag_idx) edge.set_child(n);
          },
          nv);
    }
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
              auto ref_base = nuc_base::from_char(reference[i]);
              auto seq_base = nuc_base::from_char(seq[i]);
              if (!(ref_base == seq_base)) muts[i + 1] = seq_base;
            }
            node.cg() = compact_genome{std::move(muts)};
          }
        },
        nv);
  }

  recompute_edge_mutations(d);
  return d;
}

// ---------------------------------------------------------------------------
// Read reference sequence from file (FASTA or raw text)
// ---------------------------------------------------------------------------

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
  --seed <N>              Random seed for sampling

Analysis:
  --dag-info              Print all DAG statistics (tree count, parsimony, RF)
  --parsimony             Print parsimony score distribution
  --sum-rf-distance       Print sum RF distance distribution

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
  std::optional<std::uint32_t> seed;
  bool dag_info = false;
  bool print_parsimony = false;
  bool print_rf_distance = false;
  bool validate = false;
};

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
    } else if (arg == "--validate")
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
            build_from_fasta_newick(a.fastas[fi], a.newicks[fi], a.refseq);
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

  // ---- Analysis ----
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

  // ---- Output ----
  if (!a.output.empty()) {
    scoped_arena<4096> arena;
    auto* mr = arena.get();

    if (a.trim) {
      if (a.rf.empty()) {
        // Trim to best parsimony
        parsimony_score_ops pops;
        subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
        sw.compute_weight_below(root_idx, pops);
        if (a.sample) {
          std::cerr << "Sampling a tree from min-parsimony options...\n";
        } else {
          std::cerr << "Trimming to min parsimony...\n";
        }
        auto tree = sw.min_weight_sample_tree(pops);
        save_proto_dag(tree, a.output);
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
        } else {
          std::cerr << "Trimming to min RF distance...\n";
        }
        auto tree = sw.min_weight_sample_tree(srf);
        save_proto_dag(tree, a.output);
      }
    } else if (a.sample) {
      std::cerr << "Sampling a tree from the DAG...\n";
      parsimony_score_ops pops;
      subtree_weight<parsimony_score_ops> sw(result, a.seed, mr);
      sw.compute_weight_below(root_idx, pops);
      auto tree = sw.sample_tree(pops);
      save_proto_dag(tree, a.output);
    } else {
      save_proto_dag(result, a.output);
    }
  }

  return EXIT_SUCCESS;
} catch (std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return EXIT_FAILURE;
}
