// Differential test: dirty-list compute_move_score_cached vs the original
// full-row algorithm (verbatim from the pre-change implementation).
#include <larch/native_optimize.hpp>
#include <larch/compute.hpp>
#include <larch/load_proto_dag.hpp>

#include "test_util.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace larch;
using larch::test::cg_from_sequence;

static void add_edge(phylo_dag& d, std::size_t parent_idx,
                     std::size_t child_idx) {
  auto edge = d.append_edge<edge_kind::clade>();
  auto pv = d.get_node(parent_idx);
  std::visit([&](auto p) { edge.set_parent(p); }, pv);
  auto cv = d.get_node(child_idx);
  std::visit([&](auto c) { edge.set_child(c); }, cv);
}

// Verbatim original (pre-optimization) implementation.
static int reference_score(tree_index const& index_, std::size_t src,
                           std::size_t dst, std::size_t lca,
                           src_removal_result const& removal,
                           std::vector<uint8_t>& new_node_fitch,
                           std::vector<uint8_t>& prev_old_fitch,
                           std::vector<uint8_t>& prev_new_fitch) {
  std::size_t n_sites = index_.num_variable_sites();
  if (n_sites == 0) return 0;

  auto dst_parent = index_.get_parent(dst);

  int nodes_remaining = 0;
  {
    auto cur = dst_parent;
    while (cur != lca) {
      nodes_remaining++;
      cur = index_.get_parent(cur);
    }
    nodes_remaining++;
    nodes_remaining += static_cast<int>(index_.get_dfs_info(lca).level);
  }

  auto const* src_fitch_ptr = index_.get_fitch_set_ptr(src);
  auto const* dst_fitch_ptr = index_.get_fitch_set_ptr(dst);
  auto const* removal_old = removal.old_fitch.data();
  auto const* removal_new = removal.new_fitch.data();

  int total = removal.score_change;

  for (std::size_t si = 0; si < n_sites; si++) {
    uint8_t sf = src_fitch_ptr[si];
    uint8_t df = dst_fitch_ptr[si];
    uint8_t inter = sf & df;
    new_node_fitch[si] = inter ? inter : (sf | df);
    total += inter ? 0 : 1;
  }

  auto node = dst_parent;
  bool is_first = true;
  while (true) {
    if (index_.has_child_counts(node)) {
      auto const* node_fitch_ptr = index_.get_fitch_set_ptr(node);
      uint32_t nc = index_.get_num_children(node);
      bool is_lca = (node == lca);
      uint32_t effective_nc =
          is_lca ? static_cast<uint32_t>(static_cast<int>(nc) +
                                         removal.lca_nc_adjustment)
                 : nc;

      for (std::size_t si = 0; si < n_sites; si++) {
        auto counts = index_.get_child_counts(node, si);
        int old_cost = fitch_cost_from_counts(counts, nc);

        uint8_t old_child_f;
        uint8_t new_child_f;
        if (is_first) {
          old_child_f = dst_fitch_ptr[si];
          new_child_f = new_node_fitch[si];
        } else {
          old_child_f = prev_old_fitch[si];
          new_child_f = prev_new_fitch[si];
        }

        for (int j = 0; j < 4; j++) {
          if (old_child_f & (1 << j)) {
            if (counts[j] > 0) counts[j]--;
          }
          if (new_child_f & (1 << j)) counts[j]++;
        }

        if (is_lca) {
          uint8_t src_old_f = removal_old[si];
          uint8_t src_new_f = removal_new[si];
          for (int j = 0; j < 4; j++) {
            if (src_old_f & (1 << j)) {
              if (counts[j] > 0) counts[j]--;
            }
            if (src_new_f & (1 << j)) counts[j]++;
          }
        }

        int new_cost = fitch_cost_from_counts(counts, effective_nc);
        total += new_cost - old_cost;

        prev_old_fitch[si] = node_fitch_ptr[si];
        prev_new_fitch[si] = fitch_set_from_counts(counts, effective_nc);
      }
    }

    nodes_remaining--;

    if (total > 0 && nodes_remaining > 0 &&
        total > static_cast<int>(n_sites) * nodes_remaining) {
      return total;
    }

    bool fitch_changed = false;
    for (std::size_t si = 0; si < n_sites; si++) {
      if (prev_old_fitch[si] != prev_new_fitch[si]) {
        fitch_changed = true;
        break;
      }
    }

    bool is_current_lca = (node == lca);

    if (is_current_lca && !fitch_changed) break;
    if (!is_current_lca && nodes_remaining < 0 && !fitch_changed) break;

    if (node == index_.get_tree_root()) {
      auto const* ref_alleles = index_.get_ref_alleles_ptr();
      for (std::size_t si = 0; si < n_sites; si++) {
        uint8_t old_f = prev_old_fitch[si];
        uint8_t new_f = prev_new_fitch[si];
        if (old_f != new_f) {
          int old_ua = (old_f & ref_alleles[si]) ? 0 : 1;
          int new_ua = (new_f & ref_alleles[si]) ? 0 : 1;
          total += new_ua - old_ua;
        }
      }
      break;
    }
    node = index_.get_parent(node);
    is_first = false;
  }

  return total;
}

// Random tree fixture: random compact genomes, mixed binary/polytomy topology.
static phylo_dag make_fixture(std::mt19937& rng, std::size_t n_leaves,
                              std::size_t n_sites) {
  std::string ref(n_sites, 'A');
  phylo_dag d;
  auto ua = d.append_node<node_kind::ua>();
  ua.reference_sequence() = ref;
  d.set_root(ua);

  std::vector<std::size_t> inner_nodes;
  auto rootn = d.append_node<node_kind::inner>();
  rootn.cg() = cg_from_sequence(ref, ref);
  add_edge(d, get_root_idx(d), rootn.index());
  inner_nodes.push_back(rootn.index());

  std::uniform_int_distribution<int> letter(0, 3);
  std::uniform_int_distribution<int> mutated(0, 3);
  std::uniform_int_distribution<std::size_t> pick_inner(0, 10);
  static char const* letters = "ACGT";
  for (std::size_t i = 0; i < n_leaves; i++) {
    std::string seq(n_sites, 'A');
    for (std::size_t s = 0; s < n_sites; s++) {
      if (mutated(rng) == 0) seq[s] = letters[letter(rng)];
    }
    auto leaf = d.append_node<node_kind::leaf>();
    leaf.cg() = cg_from_sequence(seq, ref);
    leaf.sample_id() = leaf.cg().to_string();
    auto parent = inner_nodes[pick_inner(rng) % inner_nodes.size()];
    add_edge(d, parent, leaf.index());
    if (i % 3 == 0 && inner_nodes.size() < n_leaves) {
      auto inner = d.append_node<node_kind::inner>();
      inner.cg() = cg_from_sequence(ref, ref);
      add_edge(d, parent, inner.index());
      inner_nodes.push_back(inner.index());
    }
  }
  return d;
}

static int enumeration_differential();

int main() {
  std::size_t failures = 0;
  for (int trial = 0; trial < 40; trial++) {
    std::mt19937 rng(static_cast<unsigned>(trial) * 7919u + 13u);
    auto dag = make_fixture(rng, 16, 10);
    tree_index idx{dag};
    move_enumerator enumerator{idx, -1000000};

    auto const& searchable = idx.get_searchable_nodes();
    std::size_t n_sites = idx.num_variable_sites();

    // Reference scratch buffers: deliberately NOT reset between candidates to
    // exercise stale-state behavior identical to the shared-slot production
    // scratch.
    std::vector<uint8_t> ref_new(n_sites, 0), ref_old(n_sites, 0),
        ref_pnew(n_sites, 0);

    for (auto src : searchable) {
      if (src == idx.get_tree_root()) continue;
      src_removal_result removal;
      removal.resize(n_sites);
      removal.score_change = 0;
      removal.lca_nc_adjustment = -1;
      auto const* src_fitch = idx.get_fitch_set_ptr(src);
      for (std::size_t si = 0; si < n_sites; si++) {
        removal.old_fitch[si] = src_fitch[si];
        removal.new_fitch[si] = 0;
      }
      auto current = idx.get_parent(src);
      std::size_t levels_up = 0;
      while (levels_up < 8) {
        levels_up++;
        for (std::size_t dst = 0; dst < idx.num_nodes(); dst++) {
          if (dst == src || !idx.is_valid(dst)) continue;
          if (idx.is_ancestor(src, dst)) continue;
          int want = reference_score(idx, src, dst, current, removal, ref_new,
                                     ref_old, ref_pnew);
          int got = enumerator.compute_move_score_cached(src, dst, current,
                                                         removal);
          if (want != got) {
            std::printf(
                "MISMATCH trial=%d src=%zu dst=%zu lca=%zu level=%zu "
                "want=%d got=%d\n",
                trial, src, dst, current, levels_up, want, got);
            ++failures;
            if (failures > 20) return 1;
          }
        }
        if (current == idx.get_tree_root()) break;
        if (levels_up == 1)
          enumerator.compute_initial_removal(src, removal);
        else
          enumerator.propagate_removal_upward(current, removal);
        current = idx.get_parent(current);
      }
    }
  }
  if (failures != 0) return 1;
  if (enumeration_differential() != 0) return 1;
  std::printf("differential score test: PASS\n");
  return 0;
}


// Reference enumeration mirroring upward_traversal/search_subtree_with_bound
// with the original scorer.
struct ref_move {
  std::size_t src, dst, lca;
  int score;
  bool operator==(ref_move const&) const = default;
};

static void ref_search_subtree(tree_index const& idx, std::size_t node,
                               std::size_t src, std::size_t lca,
                               std::size_t radius_left,
                               src_removal_result const& removal,
                               int score_threshold,
                               std::vector<uint8_t>& nn,
                               std::vector<uint8_t>& po,
                               std::vector<uint8_t>& pn,
                               std::vector<ref_move>& out) {
  if (idx.is_ancestor(src, node) || node == src) return;
  int score = reference_score(idx, src, node, lca, removal, nn, po, pn);
  if (score <= score_threshold) out.push_back({src, node, lca, score});
  if (radius_left == 0) return;
  for (auto child : idx.get_children(node))
    ref_search_subtree(idx, child, src, lca, radius_left - 1, removal,
                       score_threshold, nn, po, pn, out);
}

static std::vector<ref_move> ref_find_moves_for_source(
    tree_index const& idx, std::size_t src, std::size_t radius,
    int score_threshold) {
  std::vector<ref_move> out;
  if (src == idx.get_tree_root()) return out;
  std::size_t n_sites = idx.num_variable_sites();
  src_removal_result removal;
  removal.resize(n_sites);
  std::vector<uint8_t> nn(n_sites), po(n_sites), pn(n_sites);
  removal.score_change = 0;
  removal.lca_nc_adjustment = -1;
  auto const* src_fitch = idx.get_fitch_set_ptr(src);
  for (std::size_t si = 0; si < n_sites; si++) {
    removal.old_fitch[si] = src_fitch[si];
    removal.new_fitch[si] = 0;
  }
  move_enumerator helper{idx, score_threshold};
  auto current = idx.get_parent(src);
  auto prev = src;
  std::size_t levels_up = 0;
  while (levels_up < radius) {
    levels_up++;
    for (auto child : idx.get_children(current)) {
      if (child == prev) continue;
      ref_search_subtree(idx, child, src, current, radius - levels_up,
                         removal, score_threshold, nn, po, pn, out);
    }
    if (current == idx.get_tree_root()) break;
    if (levels_up == 1)
      helper.compute_initial_removal(src, removal);
    else
      helper.propagate_removal_upward(current, removal);
    prev = current;
    current = idx.get_parent(current);
  }
  return out;
}

static int enumeration_differential() {
  std::size_t failures = 0;
  {
    // Real fixture used by chart_spr_test's sampled-tree wave tests.
    auto dag = load_proto_dag("data/test_5_trees/tree_0.pb.gz");
    tree_index idx{dag};
    for (int threshold : {-1000000, 0, 2147483647}) {
      for (std::size_t radius = 1; radius <= 10; radius++) {
        move_enumerator enumerator{idx, threshold};
        scratch_buffers shared_scratch;
        std::vector<ref_move> got;
        for (auto src : idx.get_searchable_nodes()) {
          enumerator.find_moves_for_source(
              src, radius,
              [&](profitable_move const& m) {
                got.push_back({m.src, m.dst, m.lca, m.score_change});
              },
              shared_scratch);
        }
        std::vector<ref_move> want;
        for (auto src : idx.get_searchable_nodes()) {
          auto moves = ref_find_moves_for_source(idx, src, radius, threshold);
          want.insert(want.end(), moves.begin(), moves.end());
        }
        if (got != want) {
          std::printf("TREE0 MISMATCH threshold=%d radius=%zu got=%zu want=%zu\n",
                      threshold, radius, got.size(), want.size());
          for (std::size_t i = 0; i < got.size() && i < want.size() && i < 8;
               i++) {
            if (got[i].src != want[i].src || got[i].dst != want[i].dst ||
                got[i].lca != want[i].lca || got[i].score != want[i].score) {
              std::printf(
                  "  [%zu] got(src=%zu,dst=%zu,lca=%zu,%d) want(src=%zu,dst=%zu,lca=%zu,%d)\n",
                  i, got[i].src, got[i].dst, got[i].lca, got[i].score,
                  want[i].src, want[i].dst, want[i].lca, want[i].score);
            }
          }
          ++failures;
        }
      }
    }
  }
  for (int trial = 0; trial < 25; trial++) {
    std::mt19937 rng(static_cast<unsigned>(trial) * 104729u + 7u);
    auto dag = make_fixture(rng, 14, 9);
    tree_index idx{dag};
    for (int threshold : {-1000000, -3, -1, 0}) {
      for (std::size_t radius = 1; radius <= 7; radius++) {
        // New implementation: shared scratch across all sources, exactly like
        // the stable-slot production path.
        move_enumerator enumerator{idx, threshold};
        scratch_buffers shared_scratch;
        std::vector<ref_move> got;
        for (auto src : idx.get_searchable_nodes()) {
          enumerator.find_moves_for_source(
              src, radius,
              [&](profitable_move const& m) {
                got.push_back({m.src, m.dst, m.lca, m.score_change});
              },
              shared_scratch);
        }
        std::vector<ref_move> want;
        for (auto src : idx.get_searchable_nodes()) {
          auto moves =
              ref_find_moves_for_source(idx, src, radius, threshold);
          want.insert(want.end(), moves.begin(), moves.end());
        }
        if (got != want) {
          std::printf(
              "ENUM MISMATCH trial=%d threshold=%d radius=%zu got=%zu "
              "want=%zu\n",
              trial, threshold, radius, got.size(), want.size());
          for (std::size_t i = 0;
               i < got.size() && i < want.size() && failures < 8; i++) {
            if (got[i].src != want[i].src || got[i].dst != want[i].dst ||
                got[i].lca != want[i].lca || got[i].score != want[i].score) {
              std::printf(
                  "  [%zu] got(src=%zu,dst=%zu,lca=%zu,%d) "
                  "want(src=%zu,dst=%zu,lca=%zu,%d)\n",
                  i, got[i].src, got[i].dst, got[i].lca, got[i].score,
                  want[i].src, want[i].dst, want[i].lca, want[i].score);
              ++failures;
            }
          }
          ++failures;
          if (failures > 30) return 1;
        }
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
