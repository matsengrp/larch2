#include <larch/compute.hpp>
#include <larch/load_parsimony.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/native_optimize.hpp>
#include <larch/protobuf_encode.hpp>
#include <larch/save_proto_dag.hpp>
#include <larch/subtree_weight.hpp>
#include <larch/vcf.hpp>
#include <larch/weight_ops.hpp>

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <set>
#include <string>
#include <unordered_set>

using namespace larch;

static compact_genome cg_from_sequence(std::string_view seq,
                                       std::string_view ref) {
  return compact_genome_from_sequence(seq, ref);
}

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx, std::size_t clade_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  edge.clade_index() = clade_idx;
  std::visit([&](auto p) { edge.set_parent(p); }, d.get_node(parent_idx));
  std::visit([&](auto c) { edge.set_child(c); }, d.get_node(child_idx));
}

static std::size_t edge_mutation_count(phylo_dag& d) {
  std::size_t score = 0;
  for (auto ev : d.get_all_edges()) {
    std::visit([&](auto e) { score += e.mutations().size(); }, ev);
  }
  return score;
}

static compact_genome leaf_cg(phylo_dag& d, std::string_view sample_id) {
  compact_genome result;
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); node.cg(); }) {
            if (node.sample_id() == sample_id) result = node.cg();
          }
        },
        nv);
  }
  return result;
}

static phylo_dag make_three_leaf_star(std::string_view ref,
                                      std::string_view a,
                                      std::string_view b,
                                      std::string_view c) {
  phylo_dag d;
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = std::string{ref};
  d.set_root(ua);

  auto root = d.append_node<node_kind::inner>();
  root.cg() = compact_genome{};
  add_edge(d, ua.index(), root.index(), 0);

  std::array<std::string_view, 3> seqs = {a, b, c};
  for (std::size_t i = 0; i < seqs.size(); ++i) {
    auto leaf = d.append_node<node_kind::leaf>();
    leaf.sample_id() = "L" + std::to_string(i + 1);
    leaf.cg() = cg_from_sequence(seqs[i], ref);
    add_edge(d, root.index(), leaf.index(), i);
  }
  return d;
}

static void test_to_edge_mutations_skips_no_call() {
  std::println("test_to_edge_mutations_skips_no_call");

  compact_genome parent;
  compact_genome child{{}, {1}};
  auto muts = compact_genome::to_edge_mutations("C", parent, child);

  assert(muts.empty());
  std::println("  PASS");
}

static void test_fitch_no_call_leaf_bitmask() {
  std::println("test_fitch_no_call_leaf_bitmask");

  // Gold test: with ref A and leaves C/N/C, treating N as concrete A costs 2
  // (UA->root plus one child conflict); treating N as any base costs 1.
  auto d = make_three_leaf_star("A", "C", "N", "C");
  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);

  assert(edge_mutation_count(d) == 1);

  parsimony_score_ops ops;
  subtree_weight<parsimony_score_ops> sw(d, 1u);
  auto score = sw.compute_weight_below(get_root_idx(d), ops);
  assert(score == 1);

  std::println("  score={}", score);
  std::println("  PASS");
}

static void test_vcf_alt_no_call_populates_mask() {
  std::println("test_vcf_alt_no_call_populates_mask");

  auto path = std::filesystem::temp_directory_path() / "larch2_iupac_alt_n.vcf";
  {
    std::ofstream out{path};
    out << "##fileformat=VCFv4.2\n";
    out << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\ts1\n";
    out << "ref\t1\t.\tA\tN\t.\t.\t.\tGT\t1\n";
  }

  auto vcf = read_vcf(path.string(), "A");
  auto it = vcf.sample_genomes.find("s1");
  assert(it != vcf.sample_genomes.end());
  assert(it->second.is_ambiguous(1));
  assert(it->second.begin() == it->second.end());

  auto d = make_three_leaf_star("A", "A", "A", "A");
  // Rename the first leaf to match the VCF sample and apply.
  bool renamed = false;
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            if (!renamed) {
              node.sample_id() = "s1";
              renamed = true;
            }
          }
        },
        nv);
  }
  apply_vcf_to_dag(d, vcf);
  assert(leaf_cg(d, "s1").is_ambiguous(1));
  assert(edge_mutation_count(d) == 0);

  std::filesystem::remove(path);
  std::println("  PASS");
}

static void test_parsimony_proto_loads_mask() {
  std::println("test_parsimony_proto_loads_mask");

  pars_data data;
  data.newick = "(s1);";
  data.node_mutations.resize(2);
  data.node_mutations[1].ambiguous_sites = {1};

  auto path = std::filesystem::temp_directory_path() / "larch2_iupac_parsimony.pb";
  pb::encode_file(path.string(), data);
  auto loaded = load_parsimony_tree(path.string(), "A");

  assert(leaf_cg(loaded, "s1").is_ambiguous(1));
  std::filesystem::remove(path);
  std::println("  PASS");
}

static void test_proto_dag_roundtrip_preserves_mask() {
  std::println("test_proto_dag_roundtrip_preserves_mask");

  auto d = make_three_leaf_star("A", "N", "A", "A");
  fitch_assign_compact_genomes(d);
  recompute_edge_mutations(d);

  auto path = std::filesystem::temp_directory_path() / "larch2_iupac_roundtrip.pb";
  save_proto_dag(d, path.string());
  auto loaded = load_proto_dag(path.string());

  assert(leaf_cg(loaded, "L1").is_ambiguous(1));
  assert(!leaf_cg(loaded, "L2").is_ambiguous(1));

  std::filesystem::remove(path);
  std::println("  PASS");
}

static void test_acgt_hash_unchanged() {
  std::println("test_acgt_hash_unchanged");

  std::map<mutation_position, nuc_base> muts{{2, nuc_base::from_char('C')}};
  compact_genome cg{muts};

  std::size_t expected = 0;
  expected ^= std::hash<std::size_t>{}(2) + 0x9e3779b9 + (expected << 6) +
              (expected >> 2);
  expected ^= std::hash<std::size_t>{}(static_cast<std::size_t>('C')) +
              0x9e3779b9 + (expected << 6) + (expected >> 2);

  assert(cg.hash() == expected);
  assert((cg == compact_genome{muts, {}}));
  std::println("  PASS");
}

static void test_ambiguity_mask_affects_leaf_dedup() {
  std::println("test_ambiguity_mask_affects_leaf_dedup");

  compact_genome concrete = cg_from_sequence("C", "A");
  compact_genome masked{{{1, nuc_base::from_char('C')}}, {1}};
  assert(!(concrete == masked));

  auto d = make_three_leaf_star("A", "C", "C", "A");
  // Give L2 the same concrete mutation as L1 plus an ambiguity mask. It must
  // not condense with L1 because their no-call semantics differ.
  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); node.cg(); }) {
            if (node.sample_id() == "L2") node.cg() = masked;
          }
        },
        nv);
  }
  recompute_edge_mutations(d);
  tree_index idx{d};
  assert(idx.num_condensed_leaves() == 0);

  std::println("  PASS");
}

int main() {
  test_to_edge_mutations_skips_no_call();
  test_fitch_no_call_leaf_bitmask();
  test_vcf_alt_no_call_populates_mask();
  test_parsimony_proto_loads_mask();
  test_proto_dag_roundtrip_preserves_mask();
  test_acgt_hash_unchanged();
  test_ambiguity_mask_affects_leaf_dedup();
  std::println("All IUPAC correctness tests passed!");
}
