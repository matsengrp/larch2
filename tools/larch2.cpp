#include <larch/load_proto_dag.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/fasta.hpp>
#include <larch/vcf.hpp>
#include <larch/merge.hpp>
#include <larch/compute.hpp>
#include <larch/native_optimize.hpp>
#include <larch/random_optimize.hpp>
#include <larch/spr_pipeline.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/weight_ops.hpp>
#include <larch/rf_distance.hpp>
#include <larch/newick.hpp>
#include <larch/overlay_spr.hpp>
#include <larch/thread_pool.hpp>
#include <larch/version.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace larch;

// ---------------------------------------------------------------------------
// Progress display
// ---------------------------------------------------------------------------

struct progress {
  using clock = std::chrono::steady_clock;

  std::string label_;
  clock::time_point last_update_{};
  bool has_update_ = false;  // true once an in-place update was printed

  // Start a new phase (prints label, no newline yet).
  void phase(std::string_view label) {
    label_.assign(label);
    has_update_ = false;
    std::cerr << "  " << label_ << "..." << std::flush;
    last_update_ = clock::now();
  }

  // Update with percentage (only if >= 1 second since last update).
  void pct(std::size_t current, std::size_t total) {
    auto now = clock::now();
    if (now - last_update_ < std::chrono::seconds(1)) return;
    last_update_ = now;
    has_update_ = true;
    int p = total > 0 ? static_cast<int>(100 * current / total) : 0;
    std::cerr << "\r  " << label_ << "... " << p << "%" << std::flush;
  }

  // Update with a counter (only if >= 1 second since last update).
  void counter(std::size_t current) {
    auto now = clock::now();
    if (now - last_update_ < std::chrono::seconds(1)) return;
    last_update_ = now;
    has_update_ = true;
    std::cerr << "\r  " << label_ << "... " << current << std::flush;
  }

  // Finish phase with a detail string appended.
  void done(std::string_view detail = {}) {
    if (has_update_)
      std::cerr << "\r  " << label_ << "... done";
    else
      std::cerr << " done";
    if (!detail.empty()) std::cerr << " (" << detail << ")";
    std::cerr << "\n";
  }
};

// Run tasks in parallel with a progress callback polled from the main thread.
template <typename F>
void parallel_with_progress(thread_pool& pool, std::size_t count, F&& work,
                            progress& prog) {
  if (count == 0) return;

  std::atomic<std::size_t> completed{0};
  std::vector<std::future<void>> futures;
  futures.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    futures.push_back(pool.submit([&work, &completed, i] {
      work(i);
      completed.fetch_add(1, std::memory_order_relaxed);
    }));
  }

  while (completed.load(std::memory_order_relaxed) < count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    prog.pct(completed.load(std::memory_order_relaxed), count);
  }

  for (auto& f : futures) f.get();  // re-throw any exceptions
}

// ---------------------------------------------------------------------------
// Random SPR strategy for optimize_dag
// ---------------------------------------------------------------------------

struct random_spr_strategy {
  std::size_t num_attempts = 100;

  std::vector<phylo_dag> operator()(phylo_dag& tree, std::mt19937& rng) {
    auto root = get_root_idx(tree);

    // Collect all node indices and searchable sources
    std::vector<std::size_t> all_nodes;
    std::vector<std::size_t> sources;  // non-UA, non-leaf
    for (auto nv : tree.get_all_nodes()) {
      auto idx = std::visit([](auto n) { return n.index(); }, nv);
      all_nodes.push_back(idx);
      if (idx != root && !is_ua(tree, idx) && !is_leaf(tree, idx))
        sources.push_back(idx);
    }
    // Also allow leaves as sources (move a leaf to a different spot)
    for (auto nv : tree.get_all_nodes()) {
      auto idx = std::visit([](auto n) { return n.index(); }, nv);
      if (is_leaf(tree, idx)) sources.push_back(idx);
    }

    if (sources.size() < 2 || all_nodes.size() < 4) return {};

    std::uniform_int_distribution<std::size_t> src_dist(0, sources.size() - 1);
    std::uniform_int_distribution<std::size_t> dst_dist(0,
                                                        all_nodes.size() - 1);

    std::vector<phylo_dag> results;

    for (std::size_t attempt = 0; attempt < num_attempts; ++attempt) {
      auto src = sources[src_dist(rng)];
      auto dst = all_nodes[dst_dist(rng)];

      // Validity checks
      if (src == dst) continue;
      if (is_ua(tree, dst)) continue;
      if (is_ua(tree, src)) continue;

      // Check dst is not a descendant of src (walk up from dst)
      bool dst_in_src_subtree = false;
      {
        auto cur = dst;
        while (true) {
          if (cur == src) {
            dst_in_src_subtree = true;
            break;
          }
          auto pe = get_parent_edges(tree, cur);
          if (pe.empty()) break;
          cur = get_parent_idx(tree, pe[0]);
        }
      }
      if (dst_in_src_subtree) continue;

      // Check src is not dst's parent (would be a no-op)
      auto dst_pe = get_parent_edges(tree, dst);
      if (!dst_pe.empty() && get_parent_idx(tree, dst_pe[0]) == src) continue;

      // Check src has a parent (not root)
      auto src_pe = get_parent_edges(tree, src);
      if (src_pe.empty()) continue;

      results.push_back(apply_spr_move(tree, src, dst));
      if (results.size() >= 10) break;  // cap results per iteration
    }

    return results;
  }
};

// ---------------------------------------------------------------------------
// FASTA+Newick DAG builder
// ---------------------------------------------------------------------------

static phylo_dag build_from_fasta_newick(std::string_view fasta_path,
                                         std::string_view newick_path,
                                         std::string_view refseq_path) {
  // Read reference sequence (may be FASTA or raw text)
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

  // Read FASTA leaf sequences
  auto entries = read_fasta(fasta_path);
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto& e : entries) fasta_map[e.name] = std::move(e.sequence);

  // Read newick string
  auto nw_bytes = read_file(newick_path);
  std::string newick_str{nw_bytes.data(), nw_bytes.size()};

  // Build tree from newick (same pattern as load_parsimony_tree)
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

  // Determine leaf vs inner
  std::unordered_map<std::size_t, bool> has_children;
  std::unordered_map<std::size_t, bool> is_child;
  for (auto& e : nw_edges) {
    has_children[e.parent] = true;
    is_child[e.child] = true;
  }

  // Find newick root
  std::size_t newick_root = 0;
  for (auto& [id, _] : seq_ids) {
    if (!is_child.contains(id)) {
      newick_root = id;
      break;
    }
  }

  // Create nodes
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

  // Create edges
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

  // Add UA node
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

  // Assign leaf CGs from FASTA by diffing against reference
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

  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);
  return d;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage() {
  std::cerr <<
      R"(larch2 -- phylogenetic DAG optimizer

Input (one required):
  --dag-pb <path>         Input DAG in protobuf DAG format (.pb or .pb.gz)
  --tree-pb <path>        Input tree in parsimony protobuf format (.pb or .pb.gz)
  --fasta <path>          Input leaf sequences (FASTA)
  --newick <path>         Input tree topology (Newick string file)
  --refseq <path>         Reference sequence file (required with --tree-pb or --fasta)

  --vcf <path>            VCF file with ambiguous leaf sequences (optional)

Output (required):
  -o, --output <path>     Output DAG in protobuf DAG format

Optimization:
  -n, --iterations <N>    Number of optimization iterations (default: 10)
  --patience <P>          Stop after P consecutive zero-merge iterations (P >= 1)
  --optimizer <name>      "native" (default) or "random"
  --max-moves <N>         Max moves per iteration for native (default: 50)
  --seed <N>              Random seed
  --sample-per-radius     Re-sample tree and rebuild index between radii

Sampling:
  --sample-method <M>     parsimony (default), random, rf-minsum, rf-maxsum
  --sample-uniformly      Weight sampling proportional to subtree tree-counts
  --ignore-root-edge-mutations  Ignore UA->root edge mutations in parsimony

Move strategy:
  --callback-option <O>   best-moves (default) or all-moves
  --move-coeff-pscore <N>  Parsimony score coefficient for scoring moves (default: 1)
  --move-coeff-nodes <N>   New node coefficient for scoring moves (default: 0)
  --move-score-threshold <N>  Max parsimony score for enumerated moves (default: -1, or 0 with --move-coeff-nodes)

Metrics:
  --log-metrics           Print extended per-iteration metrics to stderr

Subtree optimization:
  --switch-subtrees <N>   After N iterations, optimize subtrees instead of whole tree
  --min-subtree-clade-size <N>  Min leaves in subtree (default: 100)
  --max-subtree-clade-size <N>  Max leaves in subtree (default: 1000)

Diverse tree extraction:
  --diverse-sample <K>    Extract K maximally diverse parsimony-optimal trees
  --diverse-pool <N>      Override pool size (default: max(10K, 100))
  --diverse-newick <path> Write selected trees as Newick strings (one per line)

Post-processing:
  --trim                  Trim result to minimum-parsimony trees

Debugging:
  --validate              Validate DAG invariants at key pipeline points

Other:
  --version               Print version (git commit) and exit
)";
}

struct args {
  std::string dag_pb;
  std::string tree_pb;
  std::string fasta;
  std::string newick;
  std::string refseq;
  std::string vcf;
  std::string output;
  std::size_t iterations = 10;
  std::string optimizer = "native";
  std::size_t max_moves = 50;
  std::optional<std::uint32_t> seed;
  bool sample_per_radius = false;
  bool trim = false;
  std::string sample_method = "parsimony";
  bool sample_uniformly = false;
  bool ignore_root_edge_mutations = false;
  std::string callback_option = "best-moves";
  bool log_metrics = false;
  std::optional<std::size_t> switch_subtrees;
  std::size_t min_subtree_clade_size = 100;
  std::size_t max_subtree_clade_size = 1000;
  bool validate = false;
  std::optional<std::size_t> diverse_sample;
  std::optional<std::size_t> diverse_pool;
  std::string diverse_newick;
  int move_coeff_pscore = 1;
  int move_coeff_nodes = 0;
  std::optional<int> move_score_threshold;
  std::optional<std::size_t> patience;
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
      a.dag_pb = next();
    else if (arg == "--tree-pb")
      a.tree_pb = next();
    else if (arg == "--fasta")
      a.fasta = next();
    else if (arg == "--newick")
      a.newick = next();
    else if (arg == "--refseq")
      a.refseq = next();
    else if (arg == "--vcf")
      a.vcf = next();
    else if (arg == "-o" || arg == "--output")
      a.output = next();
    else if (arg == "-n" || arg == "--iterations")
      a.iterations = std::stoull(std::string{next()});
    else if (arg == "--optimizer")
      a.optimizer = next();
    else if (arg == "--max-moves")
      a.max_moves = std::stoull(std::string{next()});
    else if (arg == "--seed")
      a.seed = static_cast<uint32_t>(std::stoull(std::string{next()}));
    else if (arg == "--sample-per-radius")
      a.sample_per_radius = true;
    else if (arg == "--sample-method")
      a.sample_method = next();
    else if (arg == "--sample-uniformly")
      a.sample_uniformly = true;
    else if (arg == "--ignore-root-edge-mutations")
      a.ignore_root_edge_mutations = true;
    else if (arg == "--callback-option")
      a.callback_option = next();
    else if (arg == "--log-metrics")
      a.log_metrics = true;
    else if (arg == "--switch-subtrees")
      a.switch_subtrees = std::stoull(std::string{next()});
    else if (arg == "--min-subtree-clade-size")
      a.min_subtree_clade_size = std::stoull(std::string{next()});
    else if (arg == "--max-subtree-clade-size")
      a.max_subtree_clade_size = std::stoull(std::string{next()});
    else if (arg == "--trim")
      a.trim = true;
    else if (arg == "--validate")
      a.validate = true;
    else if (arg == "--diverse-sample")
      a.diverse_sample = std::stoull(std::string{next()});
    else if (arg == "--diverse-pool")
      a.diverse_pool = std::stoull(std::string{next()});
    else if (arg == "--diverse-newick")
      a.diverse_newick = next();
    else if (arg == "--move-coeff-pscore")
      a.move_coeff_pscore = std::stoi(std::string{next()});
    else if (arg == "--move-coeff-nodes")
      a.move_coeff_nodes = std::stoi(std::string{next()});
    else if (arg == "--move-score-threshold")
      a.move_score_threshold = std::stoi(std::string{next()});
    else if (arg == "--patience") {
      auto p = std::stoull(std::string{next()});
      if (p == 0) {
        std::cerr << "error: --patience must be >= 1\n";
        std::exit(1);
      }
      a.patience = p;
    }
    else if (arg == "--version") {
      std::cerr << "larch2 " << larch::version << " (" << larch::git_commit
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
  if (a.output.empty()) {
    std::cerr << "error: --output/-o is required\n";
    usage();
    std::exit(1);
  }
  return a;
}

// ---------------------------------------------------------------------------
// Sampling dispatch
// ---------------------------------------------------------------------------

static phylo_dag sample_tree_from_dag(phylo_dag& dag, merge& m,
                                      std::string const& method, bool uniformly,
                                      bool ua_free, std::uint32_t seed,
                                      std::size_t& out_score) {
  auto root_idx = get_root_idx(dag);
  scoped_arena<4096> arena;
  auto* mr = arena.get();

  if (method == "parsimony" || method.empty()) {
    if (ua_free) {
      ua_free_parsimony_score_ops uops;
      subtree_weight<ua_free_parsimony_score_ops> sw(dag, seed, mr);
      out_score = sw.compute_weight_below(root_idx, uops);
      if (uniformly)
        return sw.min_weight_uniform_sample_tree(uops);
      else
        return sw.min_weight_sample_tree(uops);
    } else {
      parsimony_score_ops pops;
      subtree_weight<parsimony_score_ops> sw(dag, seed, mr);
      out_score = sw.compute_weight_below(root_idx, pops);
      if (uniformly)
        return sw.min_weight_uniform_sample_tree(pops);
      else
        return sw.min_weight_sample_tree(pops);
    }
  } else if (method == "random") {
    // Sample randomly, but still compute parsimony score
    parsimony_score_ops pops;
    subtree_weight<parsimony_score_ops> sw(dag, seed, mr);
    out_score = sw.compute_weight_below(root_idx, pops);
    if (uniformly)
      return sw.uniform_sample_tree(pops);
    else
      return sw.sample_tree(pops);
  } else if (method == "rf-minsum") {
    sum_rf_distance_ops rf_ops{m, m};
    sum_rf_distance rf_weight_ops{rf_ops};
    subtree_weight<sum_rf_distance> sw(dag, seed, mr);
    sw.compute_weight_below(root_idx, rf_weight_ops);
    auto sampled = uniformly ? sw.min_weight_uniform_sample_tree(rf_weight_ops)
                             : sw.min_weight_sample_tree(rf_weight_ops);
    // Compute parsimony score on the actual sampled tree
    out_score = 0;
    for (auto ev : sampled.get_all_edges()) {
      std::visit([&](auto e) { out_score += e.mutations().size(); }, ev);
    }
    return sampled;
  } else if (method == "rf-maxsum") {
    max_sum_rf_distance_ops rf_ops{m, m};
    max_sum_rf_distance rf_weight_ops{rf_ops};
    subtree_weight<max_sum_rf_distance> sw(dag, seed, mr);
    sw.compute_weight_below(root_idx, rf_weight_ops);
    auto sampled = uniformly ? sw.min_weight_uniform_sample_tree(rf_weight_ops)
                             : sw.min_weight_sample_tree(rf_weight_ops);
    // Compute parsimony score on the actual sampled tree
    out_score = 0;
    for (auto ev : sampled.get_all_edges()) {
      std::visit([&](auto e) { out_score += e.mutations().size(); }, ev);
    }
    return sampled;
  } else {
    std::cerr << "error: unknown sample method '" << method << "'\n";
    std::exit(1);
  }
}

// ---------------------------------------------------------------------------
// Subtree helpers
// ---------------------------------------------------------------------------

static std::size_t count_leaves_below(phylo_dag& tree, std::size_t node_idx) {
  if (is_leaf(tree, node_idx)) return 1;
  std::size_t count = 0;
  auto clades = get_clades(tree, node_idx);
  for (auto const& edges : clades) {
    for (auto edge_idx : edges) {
      auto child_idx = get_child_idx(tree, edge_idx);
      count += count_leaves_below(tree, child_idx);
    }
  }
  return count;
}

static phylo_dag extract_subtree_as_dag(phylo_dag& tree,
                                        std::size_t subtree_root_idx) {
  phylo_dag result;

  // Create UA node with same reference sequence
  auto ua = result.append_node<node_kind::ua>();
  ua.reference_sequence() = get_reference_sequence(tree);
  result.set_root(ua);

  // DFS copy of the subtree
  std::unordered_map<std::size_t, std::size_t> src_to_dst;

  // Copy subtree root node
  auto src_root_nv = tree.get_node(subtree_root_idx);
  auto src_root_kind = std::visit(
      [](auto n) {
        if constexpr (requires { n.reference_sequence(); })
          return node_kind::ua;
        else if constexpr (requires { n.sample_id(); })
          return node_kind::leaf;
        else
          return node_kind::inner;
      },
      src_root_nv);

  auto dst_root_nv = result.append_node(src_root_kind);
  auto dst_root_idx = std::visit([](auto n) { return n.index(); }, dst_root_nv);
  std::visit(
      [](auto src, auto dst) {
        if constexpr (requires {
                        src.cg();
                        dst.cg();
                      })
          dst.cg() = src.cg();
        if constexpr (requires {
                        src.sample_id();
                        dst.sample_id();
                      })
          dst.sample_id() = src.sample_id();
      },
      src_root_nv, dst_root_nv);
  src_to_dst[subtree_root_idx] = dst_root_idx;

  // Connect UA to subtree root
  auto ua_edge = result.template append_edge<edge_kind::clade>();
  ua_edge.clade_index() = 0;
  ua_edge.set_parent(ua);
  std::visit([&](auto child) { ua_edge.set_child(child); }, dst_root_nv);

  // Set UA edge mutations from the parent edge of subtree_root in the original
  // tree
  auto src_parent_edges = get_parent_edges(tree, subtree_root_idx);
  if (!src_parent_edges.empty()) {
    auto src_pe = tree.get_edge(src_parent_edges[0]);
    std::visit([&](auto se) { ua_edge.mutations() = se.mutations(); }, src_pe);
  }

  // DFS copy remaining nodes and edges
  std::vector<std::size_t> stack = {subtree_root_idx};
  while (!stack.empty()) {
    auto src_nidx = stack.back();
    stack.pop_back();

    if (is_leaf(tree, src_nidx)) continue;

    auto clades = get_clades(tree, src_nidx);
    for (auto const& edges : clades) {
      for (auto src_edge_idx : edges) {
        auto src_child_idx = get_child_idx(tree, src_edge_idx);

        // Copy child node if not already copied
        if (!src_to_dst.contains(src_child_idx)) {
          auto src_child_nv = tree.get_node(src_child_idx);
          auto child_kind = std::visit(
              [](auto n) {
                if constexpr (requires { n.reference_sequence(); })
                  return node_kind::ua;
                else if constexpr (requires { n.sample_id(); })
                  return node_kind::leaf;
                else
                  return node_kind::inner;
              },
              src_child_nv);

          auto dst_child_nv = result.append_node(child_kind);
          auto dst_child_idx =
              std::visit([](auto n) { return n.index(); }, dst_child_nv);
          std::visit(
              [](auto src, auto dst) {
                if constexpr (requires {
                                src.cg();
                                dst.cg();
                              })
                  dst.cg() = src.cg();
                if constexpr (requires {
                                src.sample_id();
                                dst.sample_id();
                              })
                  dst.sample_id() = src.sample_id();
              },
              src_child_nv, dst_child_nv);
          src_to_dst[src_child_idx] = dst_child_idx;
        }

        // Copy edge
        auto dst_edge = result.template append_edge<edge_kind::clade>();
        auto dst_parent_nv = result.get_node(src_to_dst[src_nidx]);
        auto dst_child_nv = result.get_node(src_to_dst[src_child_idx]);
        std::visit([&](auto p) { dst_edge.set_parent(p); }, dst_parent_nv);
        std::visit([&](auto c) { dst_edge.set_child(c); }, dst_child_nv);

        auto src_ev = tree.get_edge(src_edge_idx);
        std::visit(
            [&](auto se) {
              dst_edge.mutations() = se.mutations();
              dst_edge.clade_index() = se.clade_index();
            },
            src_ev);

        stack.push_back(src_child_idx);
      }
    }
  }

  return result;
}

static std::optional<std::size_t> select_subtree_root(
    phylo_dag& tree, std::size_t min_clade_size, std::size_t max_clade_size,
    std::mt19937& rng) {
  auto root_idx = get_root_idx(tree);

  struct candidate {
    std::size_t node_idx;
    double weight;
  };
  std::vector<candidate> candidates;

  for (auto nv : tree.get_all_nodes()) {
    auto idx = std::visit([](auto n) { return n.index(); }, nv);
    if (is_leaf(tree, idx)) continue;
    if (is_ua(tree, idx)) continue;

    auto leaf_count = count_leaves_below(tree, idx);
    if (leaf_count < min_clade_size || leaf_count > max_clade_size) continue;

    // Weight by (parent_edge_mutation_count)^2 + 1
    std::size_t mut_count = 0;
    auto pe = get_parent_edges(tree, idx);
    if (!pe.empty()) {
      auto ev = tree.get_edge(pe[0]);
      mut_count = std::visit(
          [](auto edge) -> std::size_t { return edge.mutations().size(); }, ev);
    }
    double w = static_cast<double>(mut_count * mut_count + 1);
    candidates.push_back({idx, w});
  }

  if (candidates.empty()) return std::nullopt;

  std::vector<double> weights;
  weights.reserve(candidates.size());
  for (auto& c : candidates) weights.push_back(c.weight);
  std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
  return candidates[dist(rng)].node_idx;
}

// ---------------------------------------------------------------------------
// Pool-based diverse tree sampling (farthest-point-first with true min-RF)
// ---------------------------------------------------------------------------

static std::vector<phylo_dag> pool_diverse_sample(phylo_dag& dag, std::size_t k,
                                                  std::size_t pool_size,
                                                  std::uint32_t seed) {
  assert(k >= 1 && pool_size >= k);
  auto root_idx = get_root_idx(dag);

  // 1. Sample pool of parsimony-optimal trees.
  std::vector<phylo_dag> pool;
  pool.reserve(pool_size);
  std::cerr << "  Sampling pool of " << pool_size
            << " parsimony-optimal trees...\n";
  for (std::size_t i = 0; i < pool_size; ++i) {
    auto seed_i = [](std::uint32_t x) -> std::uint32_t {
      x ^= x >> 16;
      x *= 0x45d9f3bU;
      x ^= x >> 16;
      return x;
    }(seed + static_cast<std::uint32_t>(i));
    parsimony_score_ops pops;
    scoped_arena<4096> arena;
    subtree_weight<parsimony_score_ops> sw(dag, seed_i, arena.get());
    sw.compute_weight_below(root_idx, pops);
    auto tree = sw.min_weight_sample_tree(pops);
    fitch_assign_compact_genomes(tree);
    recompute_edge_mutations(tree);
    set_sample_ids_from_cg(tree);
    pool.push_back(std::move(tree));
  }

  // 2. Deduplicate: remove topologically identical trees (same clade set).
  //    Build leaf-id map from the DAG for integer-based clade representation.
  auto leaf_map = build_leaf_id_map(dag);
  auto num_leaves = static_cast<uint32_t>(leaf_map.size());

  std::vector<std::vector<clade_bitset>> pool_clades;
  pool_clades.reserve(pool.size());
  for (auto& t : pool)
    pool_clades.push_back(collect_clade_bitsets(t, leaf_map, num_leaves));

  // Sort indices by clade set, then walk to find unique representatives.
  std::vector<std::size_t> order(pool.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(),
            [&](auto a, auto b) { return pool_clades[a] < pool_clades[b]; });

  std::vector<std::size_t> unique_indices;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (i == 0 || pool_clades[order[i]] != pool_clades[order[i - 1]])
      unique_indices.push_back(order[i]);
  }
  // Re-sort by original pool index so first-occurrence ordering is preserved.
  std::sort(unique_indices.begin(), unique_indices.end());

  std::vector<phylo_dag> unique_pool;
  std::vector<std::vector<clade_bitset>> unique_clades;
  unique_pool.reserve(unique_indices.size());
  unique_clades.reserve(unique_indices.size());
  for (auto idx : unique_indices) {
    unique_pool.push_back(std::move(pool[idx]));
    unique_clades.push_back(std::move(pool_clades[idx]));
  }
  pool.clear();
  pool_clades.clear();

  std::cerr << "  Pool: " << pool_size << " sampled, " << unique_pool.size()
            << " unique after deduplication\n";

  // 2b. Re-rank by Fitch parsimony: each tree was Fitch-assigned above, so
  //     its edge mutations reflect true Fitch parsimony.  The DAG's stored
  //     edge mutations (used by min_weight_sample_tree) can diverge from
  //     per-tree Fitch parsimony when the DAG has shared nodes with compact
  //     genomes that are suboptimal for some topologies.  Sort the pool by
  //     actual Fitch score and keep the best trees.
  if (!unique_pool.empty()) {
    auto compute_fitch_parsimony = [](phylo_dag& tree) -> std::size_t {
      std::size_t score = 0;
      for (auto ev : tree.get_all_edges()) {
        std::visit(
            [&](auto edge) { score += edge.mutations().size(); }, ev);
      }
      return score;
    };

    std::vector<std::size_t> fitch_scores;
    fitch_scores.reserve(unique_pool.size());
    for (auto& t : unique_pool)
      fitch_scores.push_back(compute_fitch_parsimony(t));

    // Sort pool indices by Fitch score (ascending = best first).
    std::vector<std::size_t> order(unique_pool.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [&](auto a, auto b) { return fitch_scores[a] < fitch_scores[b]; });

    std::size_t min_fitch = fitch_scores[order[0]];
    std::size_t max_fitch = fitch_scores[order.back()];

    // Keep at least max(2*k, 20) trees, but include all trees tied at the
    // cutoff score to avoid arbitrary exclusion.
    std::size_t keep_target = std::max(2 * k, std::size_t{20});
    keep_target = std::min(keep_target, unique_pool.size());

    // Find the Fitch cutoff: the score of the keep_target-th tree.
    std::size_t cutoff_fitch = fitch_scores[order[keep_target - 1]];

    // Include all trees at or below the cutoff.
    std::vector<phylo_dag> filtered_pool;
    std::vector<std::vector<clade_bitset>> filtered_clades;
    for (auto idx : order) {
      if (fitch_scores[idx] <= cutoff_fitch) {
        filtered_pool.push_back(std::move(unique_pool[idx]));
        filtered_clades.push_back(std::move(unique_clades[idx]));
      }
    }

    if (min_fitch < max_fitch) {
      std::cerr << "  Fitch re-rank: keeping " << filtered_pool.size() << "/"
                << unique_pool.size() << " trees (Fitch " << min_fitch << "-"
                << cutoff_fitch << ", pool max " << max_fitch << ")\n";
    }

    unique_pool = std::move(filtered_pool);
    unique_clades = std::move(filtered_clades);
  }

  // 3. Cap K at deduplicated pool size.
  if (k > unique_pool.size()) {
    std::cerr << "  Warning: requested " << k << " diverse trees but only "
              << unique_pool.size()
              << " unique trees in pool; capping K=" << unique_pool.size()
              << "\n";
    k = unique_pool.size();
  }

  std::size_t n = unique_pool.size();

  // 4. Compute pairwise RF distance matrix.
  std::cerr << "  Computing pairwise RF distances (" << n * (n - 1) / 2
            << " pairs)...\n";
  std::vector<std::vector<std::size_t>> rf(n, std::vector<std::size_t>(n, 0));
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      rf[i][j] = pairwise_rf_distance(unique_clades[i], unique_clades[j]);
      rf[j][i] = rf[i][j];
    }
  }

  // 5. Greedy farthest-point-first selection.
  std::vector<std::size_t> selected;
  selected.reserve(k);
  std::vector<bool> is_selected(n, false);

  // First tree: maximize total RF to all others.
  {
    std::size_t best = 0;
    std::size_t best_sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
      std::size_t total = 0;
      for (std::size_t j = 0; j < n; ++j) total += rf[i][j];
      if (total > best_sum) {
        best_sum = total;
        best = i;
      }
    }
    selected.push_back(best);
    is_selected[best] = true;
    std::cerr << "  Selected tree 1: pool index " << best << " (total RF "
              << best_sum << ")\n";
  }

  // Maintain min-RF from each candidate to the selected set.
  std::vector<std::size_t> min_dist(n);
  for (std::size_t i = 0; i < n; ++i) min_dist[i] = rf[i][selected[0]];

  // Select remaining trees.
  for (std::size_t s = 1; s < k; ++s) {
    std::size_t best = 0;
    std::size_t best_min_rf = 0;
    std::size_t best_sum_rf = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (is_selected[i]) continue;
      std::size_t sum_rf = 0;
      for (auto si : selected) sum_rf += rf[i][si];
      if (min_dist[i] > best_min_rf ||
          (min_dist[i] == best_min_rf && sum_rf > best_sum_rf)) {
        best = i;
        best_min_rf = min_dist[i];
        best_sum_rf = sum_rf;
      }
    }
    selected.push_back(best);
    is_selected[best] = true;
    // Update min_dist.
    for (std::size_t i = 0; i < n; ++i) {
      min_dist[i] = std::min(min_dist[i], rf[i][best]);
    }
    std::cerr << "  Selected tree " << (s + 1) << ": pool index " << best
              << " (min-RF " << best_min_rf << ")\n";
  }

  // Print pairwise RF submatrix for selected trees.
  std::cerr << "  Pairwise RF among selected trees:\n";
  for (std::size_t i = 0; i < selected.size(); ++i) {
    std::cerr << "   ";
    for (std::size_t j = 0; j < selected.size(); ++j) {
      std::cerr << " " << rf[selected[i]][selected[j]];
    }
    std::cerr << "\n";
  }

  // Collect selected trees.
  std::vector<phylo_dag> result;
  result.reserve(k);
  for (auto idx : selected) result.push_back(std::move(unique_pool[idx]));
  return result;
}

// ---------------------------------------------------------------------------
// Per-iteration metrics
// ---------------------------------------------------------------------------

static void print_metrics(phylo_dag& dag, merge& m, std::uint32_t seed) {
  auto root_idx = get_root_idx(dag);
  scoped_arena<4096> arena;
  auto* mr = arena.get();

  // Tree count
  tree_count_ops tc_ops;
  subtree_weight<tree_count_ops> tc_sw(dag, seed, mr);
  auto tree_count = tc_sw.compute_weight_below(root_idx, tc_ops);

  // Min parsimony
  parsimony_score_ops pops;
  subtree_weight<parsimony_score_ops> psw(dag, seed, mr);
  auto min_pars = psw.compute_weight_below(root_idx, pops);

  // Optimal tree count
  auto optimal_count = psw.min_weight_count(root_idx, pops);

  // Max parsimony
  max_parsimony_binary_ops mp_bops;
  max_parsimony_score_ops mp_ops{mp_bops};
  subtree_weight<max_parsimony_score_ops> mp_sw(dag, seed, mr);
  auto max_pars = mp_sw.compute_weight_below(root_idx, mp_ops);

  // RF distances
  sum_rf_distance_ops min_rf_ops{m, m};
  sum_rf_distance min_rf_weight{min_rf_ops};
  subtree_weight<sum_rf_distance> min_rf_sw(dag, seed, mr);
  auto min_rf = min_rf_sw.compute_weight_below(root_idx, min_rf_weight);
  auto min_rf_shift = min_rf_weight.get_ops().get_shift_sum();

  max_sum_rf_distance_ops max_rf_ops{m, m};
  max_sum_rf_distance max_rf_weight{max_rf_ops};
  subtree_weight<max_sum_rf_distance> max_rf_sw(dag, seed, mr);
  auto max_rf = max_rf_sw.compute_weight_below(root_idx, max_rf_weight);

  std::cerr << "  Metrics: " << tree_count.to_string() << " trees"
            << ", parsimony [" << min_pars << ", " << max_pars << "]"
            << ", " << optimal_count.to_string() << " optimal"
            << ", RF [" << (min_rf + min_rf_shift).to_string() << ", "
            << (max_rf + min_rf_shift).to_string() << "]"
            << "\n";
}

// ---------------------------------------------------------------------------
// Native optimizer loop with progress
// ---------------------------------------------------------------------------

static std::vector<optimize_result> run_native(merge& m, args const& a) {
  auto& pool = thread_pool::get_default();
  std::mt19937 rng(a.seed.value_or(std::random_device{}()));
  std::vector<optimize_result> results;
  results.reserve(a.iterations);
  progress prog;
  move_coefficients coeffs{a.move_coeff_pscore, a.move_coeff_nodes};
  int score_threshold =
      a.move_score_threshold.value_or(coeffs.has_node_penalty() ? 0 : -1);
  std::size_t consecutive_zero_merges = 0;
  auto check_patience = [&](std::size_t merges) -> bool {
    if (!a.patience) return false;
    consecutive_zero_merges = (merges == 0) ? consecutive_zero_merges + 1 : 0;
    if (consecutive_zero_merges >= *a.patience) {
      std::cerr << "Early stopping: no new merges for " << *a.patience
                << " consecutive iterations\n";
      return true;
    }
    return false;
  };

  for (std::size_t iter = 0; iter < a.iterations; ++iter) {
    std::cerr << "Iteration " << (iter + 1) << "/" << a.iterations << ":\n";

    // 1. Build merged DAG
    prog.phase("Building merged DAG");
    auto& dag = m.get_result();
    auto nc = m.result_node_count();
    auto ec = m.result_edge_count();
    prog.done(std::to_string(nc) + " nodes, " + std::to_string(ec) + " edges");

    // Metrics (opt-in)
    if (a.log_metrics) {
      std::uint32_t metrics_seed = rng();
      print_metrics(dag, m, metrics_seed);
    }

    // 2. Sample tree using configured method
    prog.phase("Sampling tree");
    std::uint32_t iter_seed = rng();
    std::size_t min_score = 0;
    auto sampled = sample_tree_from_dag(
        dag, m, a.sample_method, a.sample_uniformly,
        a.ignore_root_edge_mutations, iter_seed, min_score);
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);
    prog.done("score " + std::to_string(min_score));

    // Check if we should do subtree optimization this iteration
    bool use_subtrees =
        a.switch_subtrees.has_value() && iter >= *a.switch_subtrees;

    if (use_subtrees) {
      // Select a subtree root
      auto subtree_root = select_subtree_root(sampled, a.min_subtree_clade_size,
                                              a.max_subtree_clade_size, rng);

      if (!subtree_root) {
        std::cerr
            << "  No suitable subtree found, falling back to whole tree\n";
        use_subtrees = false;
      } else {
        auto leaf_ct = count_leaves_below(sampled, *subtree_root);
        std::cerr << "  Selected subtree with " << leaf_ct << " leaves\n";

        // Extract subtree as a standalone DAG
        prog.phase("Extracting subtree");
        auto subtree_dag = extract_subtree_as_dag(sampled, *subtree_root);
        prog.done(std::to_string(node_count(subtree_dag)) + " nodes");

        // Build tree index on subtree
        prog.phase("Building subtree tree index");
        std::optional<tree_index> idx_storage;
        idx_storage.emplace(subtree_dag, pool);
        auto n_searchable = idx_storage->get_searchable_nodes().size();
        prog.done(std::to_string(n_searchable) + " searchable nodes");

        auto max_radius = compute_tree_max_depth(subtree_dag) * 2;
        if (max_radius == 0) max_radius = 1;

        std::size_t total_trees_merged = 0;
        std::vector<radius_result> radii_results;

        for (std::size_t radius = 2; radius <= max_radius; radius *= 2) {
          std::cerr << "  [subtree radius " << radius << "]\n";

          prog.phase("  Scoring moves");
          move_enumerator enumerator{*idx_storage, score_threshold};
          auto& searchable = idx_storage->get_searchable_nodes();
          std::vector<std::vector<profitable_move>> per_source(
              searchable.size());

          parallel_with_progress(
              pool, searchable.size(),
              [&](std::size_t i) {
                enumerator.find_moves_for_source(
                    searchable[i], radius, [&](profitable_move const& mv) {
                      per_source[i].push_back(mv);
                    });
              },
              prog);

          std::vector<profitable_move> moves;
          for (auto& v : per_source)
            for (auto& mv : v) moves.push_back(mv);
          prog.done(std::to_string(moves.size()) + " profitable moves");

          if (a.callback_option == "best-moves") {
            std::sort(moves.begin(), moves.end(), [](auto& x, auto& y) {
              return x.score_change < y.score_change;
            });
            std::size_t keep = coeffs.has_node_penalty()
                                   ? std::min(moves.size(), a.max_moves * 3)
                                   : a.max_moves;
            if (moves.size() > keep) moves.resize(keep);
          }

          prog.phase("  Generating " + std::to_string(moves.size()) +
                     " fragments");
          std::vector<spr_move> spr_moves;
          spr_moves.reserve(moves.size());
          for (auto& mv : moves)
            spr_moves.push_back(spr_move{.src = mv.src,
                                         .dst = mv.dst,
                                         .lca = mv.lca,
                                         .score_change = mv.score_change});
          std::vector<phylo_dag> fragments(spr_moves.size());
          parallel_with_progress(
              pool, spr_moves.size(),
              [&](std::size_t i) {
                fragments[i] = apply_spr_as_fragment(subtree_dag, spr_moves[i]);
              },
              prog);
          prog.done();

          std::size_t moves_applied = 0;
          prog.phase("  Merging subtree fragments");
          if (coeffs.has_node_penalty()) {
            struct scored_frag {
              std::size_t idx;
              int final_score;
            };
            std::vector<scored_frag> scored(fragments.size());
            for (std::size_t i = 0; i < fragments.size(); ++i) {
              int novel = static_cast<int>(m.count_novel_nodes(fragments[i]));
              scored[i] = {
                  i,
                  coeffs.apply(spr_moves[i].score_change.value_or(0), novel)};
            }
            std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) {
              return a.final_score < b.final_score;
            });
            std::erase_if(scored, [](auto& s) { return s.final_score >= 0; });
            if (scored.size() > a.max_moves) scored.resize(a.max_moves);
            for (auto& s : scored) m.add_dag(std::move(fragments[s.idx]));
            moves_applied = scored.size();
          } else {
            for (auto& frag : fragments) m.add_dag(std::move(frag));
            moves_applied = moves.size();
          }
          m.add_dag(subtree_dag);
          prog.done(std::to_string(m.result_node_count()) + " nodes, " +
                    std::to_string(m.result_edge_count()) + " edges");
          if (a.validate)
            validate_dag(m.get_result(),
                         "iteration " + std::to_string(iter + 1) +
                             " subtree radius " + std::to_string(radius) +
                             " merge",
                         thread_pool::get_default());

          total_trees_merged += moves_applied;
          radii_results.push_back(radius_result{
              .radius = radius,
              .moves_found = moves.size(),
              .moves_applied = moves_applied,
              .parsimony_score = 0,
          });
        }

        results.push_back(optimize_result{
            .iteration = iter,
            .dag_node_count = m.result_node_count(),
            .dag_edge_count = m.result_edge_count(),
            .trees_merged = total_trees_merged,
            .parsimony_score = min_score,
            .radii = std::move(radii_results),
        });
        if (check_patience(total_trees_merged)) return results;
        continue;  // skip whole-tree path below
      }
    }

    // Whole-tree optimization path
    auto max_radius = compute_tree_max_depth(sampled) * 2;
    if (max_radius == 0) max_radius = 1;

    // 3. Build tree index
    prog.phase("Building tree index");
    std::optional<tree_index> idx_storage;
    idx_storage.emplace(sampled, pool);
    auto n_searchable = idx_storage->get_searchable_nodes().size();
    prog.done(std::to_string(n_searchable) + " searchable nodes, " +
              std::to_string(idx_storage->num_variable_sites()) + " sites");

    std::size_t total_trees_merged = 0;
    std::vector<radius_result> radii_results;

    for (std::size_t radius = 2; radius <= max_radius; radius *= 2) {
      std::cerr << "  [radius " << radius << "]\n";

      // 4. Score moves
      prog.phase("  Scoring moves");
      move_enumerator enumerator{*idx_storage, score_threshold};

      auto& searchable = idx_storage->get_searchable_nodes();
      std::vector<std::vector<profitable_move>> per_source(searchable.size());

      parallel_with_progress(
          pool, searchable.size(),
          [&](std::size_t i) {
            enumerator.find_moves_for_source(searchable[i], radius,
                                             [&](profitable_move const& mv) {
                                               per_source[i].push_back(mv);
                                             });
          },
          prog);

      std::vector<profitable_move> moves;
      for (auto& v : per_source)
        for (auto& mv : v) moves.push_back(mv);

      prog.done(std::to_string(moves.size()) + " profitable moves");

      // 5. Sort and trim (or skip for all-moves)
      if (a.callback_option == "best-moves") {
        std::sort(moves.begin(), moves.end(), [](auto& x, auto& y) {
          return x.score_change < y.score_change;
        });
        std::size_t keep = coeffs.has_node_penalty()
                               ? std::min(moves.size(), a.max_moves * 3)
                               : a.max_moves;
        if (moves.size() > keep) moves.resize(keep);
      }

      // 6. Generate fragments
      prog.phase("  Generating " + std::to_string(moves.size()) + " fragments");

      std::vector<spr_move> spr_moves;
      spr_moves.reserve(moves.size());
      for (auto& mv : moves)
        spr_moves.push_back(spr_move{.src = mv.src,
                                     .dst = mv.dst,
                                     .lca = mv.lca,
                                     .score_change = mv.score_change});

      std::vector<phylo_dag> fragments(spr_moves.size());
      parallel_with_progress(
          pool, spr_moves.size(),
          [&](std::size_t i) {
            fragments[i] = apply_spr_as_fragment(sampled, spr_moves[i]);
          },
          prog);

      prog.done();

      // 7. Merge fragments
      std::size_t moves_applied = 0;
      prog.phase("  Merging");
      if (coeffs.has_node_penalty()) {
        struct scored_frag {
          std::size_t idx;
          int final_score;
        };
        std::vector<scored_frag> scored(fragments.size());
        for (std::size_t i = 0; i < fragments.size(); ++i) {
          int novel = static_cast<int>(m.count_novel_nodes(fragments[i]));
          scored[i] = {
              i, coeffs.apply(spr_moves[i].score_change.value_or(0), novel)};
        }
        std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) {
          return a.final_score < b.final_score;
        });
        std::erase_if(scored, [](auto& s) { return s.final_score >= 0; });
        if (scored.size() > a.max_moves) scored.resize(a.max_moves);
        for (auto& s : scored) m.add_dag(std::move(fragments[s.idx]));
        moves_applied = scored.size();
      } else {
        for (auto& frag : fragments) m.add_dag(std::move(frag));
        moves_applied = moves.size();
      }
      m.add_dag(sampled);

      auto final_nc = m.result_node_count();
      auto final_ec = m.result_edge_count();
      prog.done(std::to_string(final_nc) + " nodes, " +
                std::to_string(final_ec) + " edges");
      if (a.validate)
        validate_dag(m.get_result(),
                     "iteration " + std::to_string(iter + 1) + " radius " +
                         std::to_string(radius) + " merge",
                     thread_pool::get_default());

      total_trees_merged += moves_applied;

      // Re-sample and rebuild index if --sample-per-radius
      std::size_t resample_score = 0;
      std::size_t next_radius = radius * 2;
      if (a.sample_per_radius && !moves.empty() && next_radius <= max_radius) {
        prog.phase("  Re-sampling");
        auto& new_dag = m.get_result();
        std::uint32_t resample_seed = rng();
        sampled = sample_tree_from_dag(
            new_dag, m, a.sample_method, a.sample_uniformly,
            a.ignore_root_edge_mutations, resample_seed, resample_score);
        fitch_assign_compact_genomes(sampled);
        recompute_edge_mutations(sampled);
        set_sample_ids_from_cg(sampled);
        prog.done("score " + std::to_string(resample_score));

        // Recompute max_radius from new tree depth
        auto new_max = compute_tree_max_depth(sampled) * 2;
        if (new_max == 0) new_max = 1;
        max_radius = new_max;

        // Rebuild tree index for next radius
        prog.phase("  Rebuilding tree index");
        idx_storage.emplace(sampled, pool);
        n_searchable = idx_storage->get_searchable_nodes().size();
        prog.done(std::to_string(n_searchable) + " searchable nodes, " +
                  std::to_string(idx_storage->num_variable_sites()) + " sites");
      }

      radii_results.push_back(radius_result{
          .radius = radius,
          .moves_found = moves.size(),
          .moves_applied = moves_applied,
          .parsimony_score = resample_score,
      });
    }

    results.push_back(optimize_result{
        .iteration = iter,
        .dag_node_count = m.result_node_count(),
        .dag_edge_count = m.result_edge_count(),
        .trees_merged = total_trees_merged,
        .parsimony_score = min_score,
        .radii = std::move(radii_results),
    });
    if (check_patience(total_trees_merged)) return results;
  }

  return results;
}

// ---------------------------------------------------------------------------
// Random optimizer loop with progress
// ---------------------------------------------------------------------------

static std::vector<optimize_result> run_random(merge& m, args const& a) {
  std::mt19937 rng(a.seed.value_or(std::random_device{}()));
  random_spr_strategy strategy{};
  std::vector<optimize_result> results;
  results.reserve(a.iterations);
  progress prog;
  std::size_t consecutive_zero_merges = 0;
  auto check_patience = [&](std::size_t merges) -> bool {
    if (!a.patience) return false;
    consecutive_zero_merges = (merges == 0) ? consecutive_zero_merges + 1 : 0;
    if (consecutive_zero_merges >= *a.patience) {
      std::cerr << "Early stopping: no new merges for " << *a.patience
                << " consecutive iterations\n";
      return true;
    }
    return false;
  };

  for (std::size_t iter = 0; iter < a.iterations; ++iter) {
    std::cerr << "Iteration " << (iter + 1) << "/" << a.iterations << ":\n";

    prog.phase("Building merged DAG");
    auto& dag = m.get_result();
    auto nodes_before = m.result_node_count();
    auto edges_before = m.result_edge_count();
    prog.done(std::to_string(nodes_before) + " nodes");

    if (a.log_metrics) {
      std::uint32_t metrics_seed = rng();
      print_metrics(dag, m, metrics_seed);
    }

    prog.phase("Sampling tree");
    std::uint32_t iter_seed = rng();
    std::size_t min_score = 0;
    auto sampled = sample_tree_from_dag(
        dag, m, a.sample_method, a.sample_uniformly,
        a.ignore_root_edge_mutations, iter_seed, min_score);
    fitch_assign_compact_genomes(sampled);
    recompute_edge_mutations(sampled);
    set_sample_ids_from_cg(sampled);
    prog.done("score " + std::to_string(min_score));

    prog.phase("Generating random SPR moves");
    auto new_trees = strategy(sampled, rng);
    prog.done(std::to_string(new_trees.size()) + " trees");

    prog.phase("Merging");
    for (auto& t : new_trees) m.add_dag(t);
    m.add_dag(sampled);
    auto nodes_after = m.result_node_count();
    auto edges_after = m.result_edge_count();
    prog.done(std::to_string(nodes_after) + " nodes, " +
              std::to_string(edges_after) + " edges");
    if (a.validate)
      validate_dag(m.get_result(),
                   "random iteration " + std::to_string(iter + 1) + " merge",
                   thread_pool::get_default());

    // Use DAG growth as convergence signal: random SPR always generates
    // candidates, but only novel topologies grow the DAG.
    bool dag_grew = (nodes_after != nodes_before || edges_after != edges_before);
    results.push_back(optimize_result{
        .iteration = iter,
        .dag_node_count = nodes_after,
        .dag_edge_count = edges_after,
        .trees_merged = new_trees.size(),
        .parsimony_score = min_score,
    });
    if (check_patience(dag_grew ? 1 : 0)) return results;
  }

  return results;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
  auto a = parse_args(argc, argv);

  // ---- Load input ----
  phylo_dag dag;
  if (!a.dag_pb.empty()) {
    std::cerr << "Loading DAG from " << a.dag_pb << "...\n";
    dag = load_proto_dag(a.dag_pb);
  } else if (!a.tree_pb.empty()) {
    if (a.refseq.empty()) {
      std::cerr << "error: --refseq is required with --tree-pb\n";
      return 1;
    }
    std::cerr << "Loading parsimony tree from " << a.tree_pb << "...\n";
    auto ref_bytes = read_file(a.refseq);
    std::string ref;
    std::string_view rc{ref_bytes.data(), ref_bytes.size()};
    if (!rc.empty() && rc[0] == '>') {
      auto entries = read_fasta(a.refseq);
      if (!entries.empty()) ref = std::move(entries[0].sequence);
    } else {
      for (char c : rc)
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
          ref += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    dag = load_parsimony_tree(a.tree_pb, ref);
  } else if (!a.fasta.empty()) {
    if (a.newick.empty()) {
      std::cerr << "error: --newick is required with --fasta\n";
      return 1;
    }
    if (a.refseq.empty()) {
      std::cerr << "error: --refseq is required with --fasta\n";
      return 1;
    }
    std::cerr << "Building DAG from FASTA + Newick...\n";
    dag = build_from_fasta_newick(a.fasta, a.newick, a.refseq);
  } else {
    std::cerr << "error: one of --dag-pb, --tree-pb, or --fasta is required\n";
    usage();
    return 1;
  }

  if (a.validate) validate_dag(dag, "after load", thread_pool::get_default());

  // ---- Optionally apply VCF ----
  if (!a.vcf.empty()) {
    std::cerr << "Applying VCF from " << a.vcf << "...\n";
    auto const& ref = get_reference_sequence(dag);
    auto vcf = read_vcf(a.vcf, ref);
    apply_vcf_to_dag(dag, vcf);
    if (a.validate) validate_dag(dag, "after VCF", thread_pool::get_default());
  }

  // ---- Initialize merge ----
  auto const& ref = get_reference_sequence(dag);
  merge m{ref};
  m.add_dag(dag);

  std::cerr << "Initial: " << leaf_count(dag) << " leaves, " << node_count(dag)
            << " nodes, " << edge_count(dag) << " edges\n";

  // ---- Optimize ----
  std::vector<optimize_result> results;

  if (a.optimizer == "native") {
    results = run_native(m, a);
  } else if (a.optimizer == "random") {
    results = run_random(m, a);
  } else {
    std::cerr << "error: unknown optimizer '" << a.optimizer
              << "' (expected 'native' or 'random')\n";
    return 1;
  }

  // ---- Summary ----
  std::cerr << "\nSummary:\n";
  for (auto& r : results) {
    std::cerr << "  iter " << (r.iteration + 1)
              << ": score=" << r.parsimony_score
              << " nodes=" << r.dag_node_count << " edges=" << r.dag_edge_count
              << " merged=" << r.trees_merged << "\n";
    for (auto& rd : r.radii) {
      std::cerr << "    radius " << rd.radius << ": found=" << rd.moves_found
                << " applied=" << rd.moves_applied;
      if (rd.parsimony_score > 0)
        std::cerr << " resample_score=" << rd.parsimony_score;
      std::cerr << "\n";
    }
  }

  // ---- Trim inconsistent clade edges ----
  // SPR optimization can produce DAGs where some clade alternatives lead to
  // subtrees missing leaves.  Trim them before any sampling or output.
  {
    auto& result_dag = m.get_result();
    auto tr = trim_inconsistent_clade_edges(result_dag);
    if (tr.unresolvable_clades > 0) {
      std::cerr << "warning: " << tr.unresolvable_clades
                << " clade(s) have ALL alternatives with incomplete leaf "
                   "sets; sampled trees may be missing leaves\n";
    }
  }

  // ---- Diverse tree extraction ----
  if (a.diverse_sample.has_value()) {
    std::size_t k = *a.diverse_sample;
    if (k == 0) {
      std::cerr << "error: --diverse-sample must be >= 1\n";
      return 1;
    }
    std::size_t pool_n = a.diverse_pool.value_or(
        std::max(std::size_t{10} * k, std::size_t{100}));
    std::uint32_t div_seed = a.seed.value_or(std::random_device{}());

    auto& result_dag = m.get_result();
    std::cerr << "Extracting " << k << " diverse trees (pool=" << pool_n
              << ")...\n";
    auto diverse_trees = pool_diverse_sample(result_dag, k, pool_n, div_seed);

    // Merge selected trees into output protobuf.
    merge m_out{ref};
    for (auto& t : diverse_trees) m_out.add_dag(t);
    auto& out_dag = m_out.get_result();

    std::cerr << "Diverse output: " << node_count(out_dag) << " nodes, "
              << edge_count(out_dag) << " edges\n";
    if (a.validate)
      validate_dag(out_dag, "diverse output", thread_pool::get_default());
    save_proto_dag(out_dag, a.output);

    // Optionally write Newick.
    if (!a.diverse_newick.empty()) {
      std::ofstream nwk(a.diverse_newick);
      if (!nwk) {
        std::cerr << "error: cannot open " << a.diverse_newick
                  << " for writing\n";
        return 1;
      }
      for (auto& t : diverse_trees) nwk << to_newick(t) << "\n";
      if (!nwk) {
        std::cerr << "error: write failed to " << a.diverse_newick << "\n";
        return 1;
      }
      std::cerr << "Wrote " << diverse_trees.size() << " Newick trees to "
                << a.diverse_newick << "\n";
    }

    std::cerr << "Wrote " << a.output << "\n";
    return 0;
  }

  // ---- Output ----
  if (a.trim) {
    std::cerr << "Trimming DAG to minimum-parsimony edges...\n";
    auto& result_dag = m.get_result();
    parsimony_score_ops pops;
    std::uint32_t trim_seed = a.seed.value_or(42);
    scoped_arena<4096> trim_arena;
    subtree_weight<parsimony_score_ops> sw(result_dag, trim_seed,
                                           trim_arena.get());
    auto trimmed = sw.trim_to_min_weight(pops);

    std::cerr << "Trimmed: " << node_count(trimmed) << " nodes, "
              << edge_count(trimmed) << " edges\n";
    if (a.validate)
      validate_dag(trimmed, "before output (trimmed)",
                    thread_pool::get_default());
    save_proto_dag(trimmed, a.output);
  } else {
    auto& result_dag = m.get_result();
    std::cerr << "Final: " << node_count(result_dag) << " nodes, "
              << edge_count(result_dag) << " edges\n";
    if (a.validate)
      validate_dag(result_dag, "before output", thread_pool::get_default());
    save_proto_dag(result_dag, a.output);
  }

  std::cerr << "Wrote " << a.output << "\n";
  return 0;
}
