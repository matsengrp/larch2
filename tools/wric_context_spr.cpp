// wric_context_spr (round 3 of ~/tmp/diversity-signals.md):
// exact context-aware subtree scoring + multi-start rSPR search.
//
// Model (unit parsimony, per site; O_p(s) = m_out,p + [s not in S_out,p]):
//   Q(T) = const_q + sum_{p variable} (m_T,p + [S_T,p & S_out,p == 0])
//   const_q = sum_all m_out,p + sum_{p const in target} [c_p not in S_out,p]
// Q(T) is EXACTLY the Fitch score of the witness with subtree(T) grafted at
// the attachment edge (Sankoff cut at that edge).  The outside message comes
// from ONE actual witness topology; the mask form retains ALL minimizing
// outside characters.  The isolated objective is
//   B_T(T) = sum_{p variable} m_T,p,
// the subtree's own parsimony.  One Fitch pass yields both.
//
// Modes:
//   eval <witness.pb.gz> <ids.txt> <cand1.pb.gz> [cand2...]
//     Prints the witness's full-tree Fitch total (both root conventions),
//     Q/B_T of the witness's own subtree, and Q/B_T of each candidate plus
//     the full spliced-tree Fitch score (independent verification of the
//     boundary evaluator).
//   search <witness.pb.gz> <ids.txt> <outdir> [key=value ...]
//     objective=context|isolated (default context), starts=N (100),
//     seed=S (1), sweeps=K (60), root=free|ref (free)
//     Writes best_sub.pb.gz, best_graft.pb.gz, best_newick.txt,
//     trajectory.tsv.

#include "wric_subtree_common.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>

namespace {

using wsub::tnode;

std::uint8_t bit_of_char(char c) {
  switch (c) {
    case 'A': case 'a': return 1;
    case 'C': case 'c': return 2;
    case 'G': case 'g': return 4;
    case 'T': case 't': return 8;
    default: return 0xF;
  }
}

struct Alignment {
  std::size_t L = 0;
  std::vector<std::string> ids;
  std::vector<std::uint8_t> code;  // [leaf * L + site]
  std::unordered_map<std::string, std::size_t> idx;
};

Alignment build_alignment(std::string const& reference,
                          std::map<std::string, larch::compact_genome> const& cgs) {
  Alignment al;
  al.L = reference.size();
  for (auto const& [sid, cg] : cgs) al.ids.push_back(sid);
  for (std::size_t i = 0; i < al.ids.size(); ++i) al.idx[al.ids[i]] = i;
  al.code.assign(al.ids.size() * al.L, 0);
  for (std::size_t s = 0; s < al.L; ++s) {
    auto b = bit_of_char(reference[s]);
    for (std::size_t i = 0; i < al.ids.size(); ++i) al.code[i * al.L + s] = b;
  }
  for (auto const& [sid, cg] : cgs) {
    auto i = al.idx.at(sid);
    for (auto& [pos, base] : cg) {
      if (pos == 0 || pos > al.L)
        throw std::runtime_error("context_spr: mutation position out of range");
      al.code[i * al.L + (pos - 1)] = bit_of_char(base.to_char());
    }
  }
  return al;
}

inline void combine2(std::uint8_t m1, std::uint32_t c1, std::uint8_t m2,
                     std::uint32_t c2, std::uint8_t& om, std::uint32_t& oc) {
  std::uint32_t base = c1 + c2;
  // cost(s) = c1 + c2 + [s not in S1] + [s not in S2]
  std::uint32_t b0 = base + 2 - ((m1 & 1) + (m2 & 1));
  std::uint32_t b1 = base + 2 - (((m1 >> 1) & 1) + ((m2 >> 1) & 1));
  std::uint32_t b2 = base + 2 - (((m1 >> 2) & 1) + ((m2 >> 2) & 1));
  std::uint32_t b3 = base + 2 - (((m1 >> 3) & 1) + ((m2 >> 3) & 1));
  std::uint32_t best = b0;
  std::uint8_t bmask = 1;
  if (b1 < best) { best = b1; bmask = 2; }
  else if (b1 == best) bmask |= 2;
  if (b2 < best) { best = b2; bmask = 4; }
  else if (b2 == best) bmask |= 4;
  if (b3 < best) { best = b3; bmask = 8; }
  else if (b3 == best) bmask |= 8;
  om = bmask;
  oc = best;
}

// ------------------------------------------------------------- witness arena

struct WArena {
  std::size_t n = 0;
  std::vector<int> leaf_align;  // alignment leaf idx or -1
  std::vector<std::array<int, 2>> ch;
  std::vector<int> parent;
  int root = -1;
};

WArena flatten_witness(tnode const& root, Alignment const& al,
                       tnode const* target, int* target_slot) {
  WArena w;
  std::function<std::size_t(tnode const&)> rec = [&](tnode const& node) {
    auto i = static_cast<int>(w.n);
    w.parent.push_back(-1);
    w.leaf_align.push_back(-1);
    w.ch.push_back({-1, -1});
    ++w.n;
    if (&node == target) *target_slot = i;
    if (node.leaf) {
      w.leaf_align[i] = static_cast<int>(al.idx.at(node.sid));
      return static_cast<std::size_t>(i);
    }
    auto l = rec(node.children[0]);
    auto r = rec(node.children[1]);
    w.ch[i] = {static_cast<int>(l), static_cast<int>(r)};
    w.parent[l] = i;
    w.parent[r] = i;
    return static_cast<std::size_t>(i);
  };
  auto root_i = rec(root);
  w.root = static_cast<int>(root_i);
  if (w.n != 2 * al.ids.size() - 1)
    throw std::runtime_error("context_spr: witness is not a full binary tree");
  return w;
}

struct Tables {
  std::vector<std::uint8_t> up_mask, down_mask;
  std::vector<std::uint32_t> up_cost, down_cost;
  std::vector<std::uint8_t> has_down;
  std::uint64_t total = 0;
};

Tables fitch_tables(WArena& w, Alignment const& al, bool ref_root,
                    std::string const& reference) {
  auto N = w.n, L = al.L;
  Tables t;
  t.up_mask.assign(N * L, 0);
  t.up_cost.assign(N * L, 0);
  t.down_mask.assign(N * L, 0);
  t.down_cost.assign(N * L, 0);
  t.has_down.assign(N, 0);
  std::vector<int> order;
  order.reserve(N);
  {
    std::vector<int> stack{w.root};
    while (!stack.empty()) {
      auto i = stack.back();
      stack.pop_back();
      order.push_back(i);
      if (w.leaf_align[i] < 0) {
        stack.push_back(w.ch[i][0]);
        stack.push_back(w.ch[i][1]);
      }
    }
    std::reverse(order.begin(), order.end());
  }
  for (auto i : order) {
    auto* um = &t.up_mask[static_cast<std::size_t>(i) * L];
    auto* uc = &t.up_cost[static_cast<std::size_t>(i) * L];
    if (w.leaf_align[i] >= 0) {
      auto base = static_cast<std::size_t>(w.leaf_align[i]) * L;
      for (std::size_t s = 0; s < L; ++s) {
        um[s] = al.code[base + s];
        uc[s] = 0;
      }
    } else {
      auto l = w.ch[i][0], r = w.ch[i][1];
      auto* lm = &t.up_mask[static_cast<std::size_t>(l) * L];
      auto* lc = &t.up_cost[static_cast<std::size_t>(l) * L];
      auto* rm = &t.up_mask[static_cast<std::size_t>(r) * L];
      auto* rc = &t.up_cost[static_cast<std::size_t>(r) * L];
      for (std::size_t s = 0; s < L; ++s)
        combine2(lm[s], lc[s], rm[s], rc[s], um[s], uc[s]);
    }
  }
  for (std::size_t s = 0; s < L; ++s)
    t.total += t.up_cost[static_cast<std::size_t>(w.root) * L + s];
  if (ref_root) {
    // reference pinned above the root (UA as reference leaf)
    std::uint64_t extra = 0;
    for (std::size_t s = 0; s < L; ++s) {
      auto rm = t.up_mask[static_cast<std::size_t>(w.root) * L + s];
      std::uint8_t rb = bit_of_char(reference[s]);
      if (!(rm & rb)) ++extra;
    }
    t.total += extra;
  }
  // down messages
  {
    std::vector<int> stack{w.root};
    while (!stack.empty()) {
      auto i = stack.back();
      stack.pop_back();
      if (w.leaf_align[i] >= 0) continue;
      for (int ci = 0; ci < 2; ++ci) {
        auto c = w.ch[i][ci];
        auto sib = w.ch[i][1 - ci];
        auto* sm = &t.up_mask[static_cast<std::size_t>(sib) * L];
        auto* sc = &t.up_cost[static_cast<std::size_t>(sib) * L];
        auto* dm = &t.down_mask[static_cast<std::size_t>(i) * L];
        auto* dc = &t.down_cost[static_cast<std::size_t>(i) * L];
        auto* cm = &t.down_mask[static_cast<std::size_t>(c) * L];
        auto* cc = &t.down_cost[static_cast<std::size_t>(c) * L];
        if (t.has_down[i]) {
          for (std::size_t s = 0; s < L; ++s)
            combine2(dm[s], dc[s], sm[s], sc[s], cm[s], cc[s]);
        } else {
          for (std::size_t s = 0; s < L; ++s) {
            cm[s] = sm[s];
            cc[s] = sc[s];
          }
        }
        t.has_down[c] = 1;
      }
      stack.push_back(w.ch[i][0]);
      stack.push_back(w.ch[i][1]);
    }
  }
  if (ref_root) {
    // root's down = the pinned reference state per site
    for (std::size_t s = 0; s < L; ++s) {
      t.down_mask[static_cast<std::size_t>(w.root) * L + s] =
          bit_of_char(reference[s]);
      t.down_cost[static_cast<std::size_t>(w.root) * L + s] = 0;
    }
    t.has_down[w.root] = 1;
  }
  return t;
}

// ------------------------------------------------------------------ context

struct Context {
  std::size_t nvar = 0;
  std::vector<std::vector<std::uint8_t>> leaf_code;  // [taxon][j]
  std::vector<std::uint8_t> out_mask;                // [j]
  std::uint64_t const_q = 0;
};

Context build_context(Tables const& t, WArena const& w, Alignment const& al,
                      int target_slot, std::vector<std::string> const& ids) {
  Context ctx;
  auto L = al.L;
  auto* dm = &t.down_mask[static_cast<std::size_t>(target_slot) * L];
  auto* dc = &t.down_cost[static_cast<std::size_t>(target_slot) * L];
  // target leaf alignment rows
  std::vector<std::size_t> leaf_row(ids.size());
  for (std::size_t i = 0; i < ids.size(); ++i)
    leaf_row[i] = al.idx.at(ids[i]) * L;
  // variable sites: not all target taxa share the same single state
  for (std::size_t s = 0; s < L; ++s) {
    ctx.const_q += dc[s];
    std::uint8_t c = al.code[leaf_row[0] + s];
    bool variable = false;
    for (std::size_t i = 1; i < ids.size(); ++i)
      if (al.code[leaf_row[i] + s] != c) {
        variable = true;
        break;
      }
    if (!variable && (c & (c - 1))) variable = true;  // ambiguous leaf char
    if (!variable) {
      if (!(c & dm[s])) ++ctx.const_q;  // [c_p not in S_out,p]
    } else {
      ctx.out_mask.push_back(dm[s]);
      ctx.nvar++;
    }
  }
  ctx.leaf_code.resize(ids.size());
  std::vector<std::size_t> var;
  for (std::size_t s = 0, j = 0; s < L; ++s) {
    std::uint8_t c0 = al.code[leaf_row[0] + s];
    bool variable = false;
    for (std::size_t i = 1; i < ids.size(); ++i)
      if (al.code[leaf_row[i] + s] != c0) variable = true;
    if (!variable && (c0 & (c0 - 1))) variable = true;
    if (variable) var.push_back(s), ++j;
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    ctx.leaf_code[i].resize(var.size());
    for (std::size_t j = 0; j < var.size(); ++j)
      ctx.leaf_code[i][j] = al.code[leaf_row[i] + var[j]];
  }
  return ctx;
}

// -------------------------------------------------------------------- STree

struct STree {
  int n = 0;
  int root = -1;
  std::vector<int> parent;
  std::vector<std::array<int, 2>> ch;
  std::vector<int> tax;  // leaf slot -> taxon idx; internal -1

  bool leaf(int i) const { return tax[i] >= 0; }
};

void verify_stree(STree const& t) {
  auto slots = 2 * static_cast<std::size_t>(t.n) - 1;
  if (t.parent.size() != slots || t.ch.size() != slots || t.tax.size() != slots)
    throw std::runtime_error("stree: slot count mismatch");
  std::vector<int> seen(slots, 0);
  std::vector<int> stack{t.root};
  seen[t.root] = 1;
  std::size_t leaves = 0;
  while (!stack.empty()) {
    auto i = stack.back();
    stack.pop_back();
    if (t.leaf(i)) {
      ++leaves;
      continue;
    }
    for (auto c : t.ch[i]) {
      if (c < 0 || seen[c]) throw std::runtime_error("stree: bad child link");
      if (t.parent[c] != i) throw std::runtime_error("stree: parent mismatch");
      seen[c] = 1;
      stack.push_back(c);
    }
  }
  if (leaves != static_cast<std::size_t>(t.n))
    throw std::runtime_error("stree: leaf count mismatch");
  for (std::size_t i = 0; i < slots; ++i)
    if (!seen[i]) throw std::runtime_error("stree: unreachable slot");
}

STree stree_from_tnode(tnode const& node,
                       std::unordered_map<std::string, int> const& tid, int n) {
  STree t;
  t.n = n;
  auto slots = 2 * static_cast<std::size_t>(n) - 1;
  t.parent.assign(slots, -1);
  t.ch.assign(slots, {-1, -1});
  t.tax.assign(slots, -1);
  // Flatten iteratively with an explicit next-slot allocator.
  struct Job {
    tnode const* node;
    int slot;
    bool second;
  };
  int next = 0;
  std::vector<Job> stack{{&node, -1, false}};
  while (!stack.empty()) {
    auto job = stack.back();
    stack.pop_back();
    if (job.node->leaf) {
      int slot = next++;
      t.tax[slot] = tid.at(job.node->sid);
      if (job.slot >= 0) {
        auto& pc = t.ch[job.slot];
        (pc[0] == -1 ? pc[0] : pc[1]) = slot;
        t.parent[slot] = job.slot;
      } else {
        t.root = slot;
      }
      continue;
    }
    int slot = next++;
    t.tax[slot] = -1;
    if (job.slot >= 0) {
      auto& pc = t.ch[job.slot];
      (pc[0] == -1 ? pc[0] : pc[1]) = slot;
      t.parent[slot] = job.slot;
    } else {
      t.root = slot;
    }
    // push children left-first-pop-last so ch order = children[0], children[1]
    stack.push_back({&job.node->children[1], slot, false});
    stack.push_back({&job.node->children[0], slot, false});
  }
  verify_stree(t);
  return t;
}

tnode tnode_from_stree(STree const& t, std::vector<std::string> const& ids) {
  std::function<tnode(int)> rec = [&](int i) -> tnode {
    tnode nd;
    if (t.leaf(i)) {
      nd.leaf = true;
      nd.sid = ids[static_cast<std::size_t>(t.tax[i])];
      nd.taxa = {nd.sid};
      return nd;
    }
    nd.children.push_back(rec(t.ch[i][0]));
    nd.children.push_back(rec(t.ch[i][1]));
    nd.taxa = wsub::sorted_union(nd.children[0].taxa, nd.children[1].taxa);
    return nd;
  };
  return rec(t.root);
}

STree random_stree(int n, std::mt19937& rng) {
  STree t;
  t.n = n;
  auto slots = 2 * static_cast<std::size_t>(n) - 1;
  t.parent.assign(slots, -1);
  t.ch.assign(slots, {-1, -1});
  t.tax.assign(slots, -1);
  std::vector<int> perm(static_cast<std::size_t>(n));
  std::iota(perm.begin(), perm.end(), 0);
  std::shuffle(perm.begin(), perm.end(), rng);
  std::vector<int> forest;
  for (std::size_t i = 0; i < perm.size(); ++i) {
    int slot = static_cast<int>(i);
    t.tax[slot] = perm[i];
    forest.push_back(slot);
  }
  int next = n;
  while (forest.size() > 1) {
    std::uniform_int_distribution<int> d(0, static_cast<int>(forest.size()) - 1);
    int a = d(rng);
    int b = d(rng);
    while (b == a) b = d(rng);
    int w = next++;
    t.tax[w] = -1;
    t.ch[w] = {forest[static_cast<std::size_t>(a)], forest[static_cast<std::size_t>(b)]};
    t.parent[forest[static_cast<std::size_t>(a)]] = w;
    t.parent[forest[static_cast<std::size_t>(b)]] = w;
    forest[static_cast<std::size_t>(a)] = w;
    forest.erase(forest.begin() + b);
  }
  t.root = forest[0];
  verify_stree(t);
  return t;
}

std::string canonical_newick(STree const& t, std::vector<std::string> const& ids) {
  std::function<std::string(int)> rec = [&](int i) -> std::string {
    if (t.leaf(i)) return ids[static_cast<std::size_t>(t.tax[i])];
    auto a = rec(t.ch[i][0]);
    auto b = rec(t.ch[i][1]);
    if (b < a) std::swap(a, b);
    return "(" + a + "," + b + ")";
  };
  return rec(t.root) + ";";
}

// ------------------------------------------------------------------- scoring

struct Score {
  std::uint64_t B = 0;   // isolated: sum m_T over variable sites
  std::uint64_t Q = 0;   // context: exact grafted score
};

Score score_stree(STree const& t, Context const& ctx) {
  auto V = ctx.nvar;
  auto slots = 2 * static_cast<std::size_t>(t.n) - 1;
  std::vector<std::uint8_t> mask(slots * V, 0);
  std::vector<std::uint32_t> cost(slots * V, 0);
  std::vector<int> order;
  order.reserve(slots);
  {
    std::vector<int> stack{t.root};
    while (!stack.empty()) {
      auto i = stack.back();
      stack.pop_back();
      order.push_back(i);
      if (!t.leaf(i)) {
        stack.push_back(t.ch[i][0]);
        stack.push_back(t.ch[i][1]);
      }
    }
    std::reverse(order.begin(), order.end());
  }
  for (auto i : order) {
    auto* m = &mask[static_cast<std::size_t>(i) * V];
    auto* c = &cost[static_cast<std::size_t>(i) * V];
    if (t.leaf(i)) {
      auto const& lc = ctx.leaf_code[static_cast<std::size_t>(t.tax[i])];
      for (std::size_t j = 0; j < V; ++j) {
        m[j] = lc[j];
        c[j] = 0;
      }
    } else {
      auto l = t.ch[i][0], r = t.ch[i][1];
      auto* lm = &mask[static_cast<std::size_t>(l) * V];
      auto* lc = &cost[static_cast<std::size_t>(l) * V];
      auto* rm = &mask[static_cast<std::size_t>(r) * V];
      auto* rc = &cost[static_cast<std::size_t>(r) * V];
      for (std::size_t j = 0; j < V; ++j)
        combine2(lm[j], lc[j], rm[j], rc[j], m[j], c[j]);
    }
  }
  Score s;
  auto* rm = &mask[static_cast<std::size_t>(t.root) * V];
  auto* rc = &cost[static_cast<std::size_t>(t.root) * V];
  for (std::size_t j = 0; j < V; ++j) {
    s.B += rc[j];
    if (!(rm[j] & ctx.out_mask[j])) ++s.Q;
  }
  s.Q += ctx.const_q + s.B;
  return s;
}

// ----------------------------------------------------------------- rSPR ops

int sibling_of(STree const& t, int u) {
  auto p = t.parent[u];
  return t.ch[p][0] == u ? t.ch[p][1] : t.ch[p][0];
}

// Detach subtree(u); collapse its parent; return rest + freed slot (=old
// parent of u, which held exactly u and its sibling).
std::pair<STree, int> detach(STree const& t, int u) {
  auto p = t.parent[u];
  auto s = sibling_of(t, u);
  auto sp = t.parent[p];
  STree r = t;
  r.parent[s] = sp;
  if (sp >= 0) {
    auto& pc = r.ch[sp];
    (pc[0] == p ? pc[0] : pc[1]) = s;
  } else {
    r.root = s;
  }
  r.ch[p] = {-1, -1};
  return {r, p};
}

// Regraft u (hanging subtree) on the edge above b, reusing freed slot w.
STree regraft(STree const& r, int u, int w, int b) {
  STree g = r;
  auto a = g.parent[b];
  g.ch[w] = {u, b};
  g.parent[u] = w;
  g.parent[b] = w;
  if (a < 0) {
    g.parent[w] = -1;
    g.root = w;
  } else {
    auto& ac = g.ch[a];
    (ac[0] == b ? ac[0] : ac[1]) = w;
    g.parent[w] = a;
  }
  return g;
}

void collect_reachable(STree const& t, std::vector<int>& out) {
  out.clear();
  std::vector<int> stack{t.root};
  while (!stack.empty()) {
    auto i = stack.back();
    stack.pop_back();
    out.push_back(i);
    if (!t.leaf(i)) {
      stack.push_back(t.ch[i][0]);
      stack.push_back(t.ch[i][1]);
    }
  }
}

// --------------------------------------------------------------------- main

struct Opts {
  std::string objective = "context";
  int starts = 100;
  int seed = 1;
  int sweeps = 60;
  bool ref_root = false;
  std::string engine = "full";        // full | inc
  std::string starts_mode = "random"; // random | perturb
  int perturb = 25;
  int spr_samples = 40;
  int spr_rounds = 30;
};

int run_eval(std::string const& witness_path, std::vector<std::string> const& ids,
             std::vector<std::string> const& cands) {
  std::string reference;
  std::map<std::string, larch::compact_genome> leaf_cgs;
  auto wroot = wsub::load_tree(witness_path, &reference, &leaf_cgs);
  auto al = build_alignment(reference, leaf_cgs);
  // locate target subtree in the witness tnode
  tnode const* target = nullptr;
  {
    std::vector<tnode const*> stack{&wroot};
    while (!stack.empty()) {
      auto* n = stack.back();
      stack.pop_back();
      if (n->leaf) continue;
      if (n->taxa == ids) {
        target = n;
        break;
      }
      stack.push_back(&n->children[0]);
      stack.push_back(&n->children[1]);
    }
    if (!target) throw std::runtime_error("context_spr: target taxon set not found");
  }
  int slot = -1;
  auto w = flatten_witness(wroot, al, target, &slot);
  std::unordered_map<std::string, int> tid;
  for (std::size_t i = 0; i < ids.size(); ++i) tid[ids[i]] = static_cast<int>(i);
  for (int ref_model = 0; ref_model < 2; ++ref_model) {
    bool rr = ref_model == 1;
    auto t = fitch_tables(w, al, rr, reference);
    auto ctx = build_context(t, w, al, slot, ids);
    std::printf("[root=%s] witness full-tree fitch total = %llu\n", rr ? "ref" : "free",
                static_cast<unsigned long long>(t.total));
    auto wt = stree_from_tnode(*target, tid, static_cast<int>(ids.size()));
    auto ws = score_stree(wt, ctx);
    std::printf("[root=%s] witness subtree: Q = %llu (decomposition check %s) B_T = %llu\n",
                rr ? "ref" : "free", static_cast<unsigned long long>(ws.Q),
                ws.Q == t.total ? "OK" : "FAIL",
                static_cast<unsigned long long>(ws.B));
    for (auto const& c : cands) {
      std::string ref_ignored;
      auto croot = wsub::load_tree(c, &ref_ignored, nullptr);
      if (croot.taxa != ids)
        throw std::runtime_error("context_spr: candidate taxon set mismatch: " + c);
      auto ct = stree_from_tnode(croot, tid, static_cast<int>(ids.size()));
      auto cs = score_stree(ct, ctx);
      // independent check: full spliced-tree Fitch
      auto spliced = wsub::splice(wroot, ids, croot);
      int sslot = -1;
      auto sw = flatten_witness(spliced, al, nullptr, &sslot);
      (void)sslot;
      auto st = fitch_tables(sw, al, rr, reference);
      std::printf("[root=%s] %s: Q = %llu B_T = %llu  spliced_full = %llu  %s\n",
                  rr ? "ref" : "free", c.c_str(),
                  static_cast<unsigned long long>(cs.Q),
                  static_cast<unsigned long long>(cs.B),
                  static_cast<unsigned long long>(st.total),
                  cs.Q == st.total ? "VERIFIED" : "MISMATCH");
    }
  }
  return 0;
}

int run_search(std::string const& witness_path, std::vector<std::string> const& ids,
               std::string const& outdir, Opts const& opts) {
  std::string reference;
  std::map<std::string, larch::compact_genome> leaf_cgs;
  auto wroot = wsub::load_tree(witness_path, &reference, &leaf_cgs);
  auto al = build_alignment(reference, leaf_cgs);
  tnode const* target = nullptr;
  {
    std::vector<tnode const*> stack{&wroot};
    while (!stack.empty()) {
      auto* n = stack.back();
      stack.pop_back();
      if (n->leaf) continue;
      if (n->taxa == ids) {
        target = n;
        break;
      }
      stack.push_back(&n->children[0]);
      stack.push_back(&n->children[1]);
    }
    if (!target) throw std::runtime_error("context_spr: target taxon set not found");
  }
  int slot = -1;
  auto w = flatten_witness(wroot, al, target, &slot);
  auto t = fitch_tables(w, al, opts.ref_root, reference);
  auto ctx = build_context(t, w, al, slot, ids);
  std::unordered_map<std::string, int> tid;
  for (std::size_t i = 0; i < ids.size(); ++i) tid[ids[i]] = static_cast<int>(i);
  auto n = static_cast<int>(ids.size());

  auto wt = stree_from_tnode(*target, tid, n);
  auto wsc = score_stree(wt, ctx);
  std::printf("witness subtree: Q = %llu B_T = %llu (root=%s)\n",
              static_cast<unsigned long long>(wsc.Q),
              static_cast<unsigned long long>(wsc.B),
              opts.ref_root ? "ref" : "free");

  std::mt19937 rng(static_cast<std::uint32_t>(opts.seed));
  std::set<std::string> visited;
  std::uint64_t evals = 0;
  STree best_tree = wt;
  Score best_score = wsc;
  std::ostringstream traj;
  traj << "restart\tsweep\tevals\tcur_obj\tbest_obj\n";

  auto obj = [&](Score const& s) {
    return opts.objective == "isolated" ? s.B : s.Q;
  };

  std::vector<int> reachable;
  for (int restart = 0; restart <= opts.starts; ++restart) {
    STree cur;
    if (restart == 0) {
      cur = wt;  // witness start
    } else {
      cur = random_stree(n, rng);
    }
    auto cs = score_stree(cur, ctx);
    visited.insert(canonical_newick(cur, ids));
    ++evals;
    int sweep = 0;
    for (; sweep < opts.sweeps; ++sweep) {
      std::uint64_t cur_obj = obj(cs);
      STree sweep_best = cur;
      Score sweep_best_score = cs;
      auto slots = 2 * static_cast<std::size_t>(n) - 1;
      for (std::size_t u = 0; u < slots; ++u) {
        if (static_cast<int>(u) == cur.root || cur.leaf(static_cast<int>(u)))
          continue;
        auto [rest, freed] = detach(cur, static_cast<int>(u));
        collect_reachable(rest, reachable);
        for (auto b : reachable) {
          if (b == static_cast<int>(u)) continue;
          auto g = regraft(rest, static_cast<int>(u), freed, b);
          if (std::getenv("WRIC_VERIFY_STREE")) verify_stree(g);
          auto can = canonical_newick(g, ids);
          if (visited.count(can)) continue;
          visited.insert(can);
          ++evals;
          auto gs = score_stree(g, ctx);
          if (obj(gs) < obj(sweep_best_score)) {
            sweep_best = std::move(g);
            sweep_best_score = gs;
          }
        }
      }
      if (obj(sweep_best_score) >= cur_obj) break;
      cur = std::move(sweep_best);
      cs = sweep_best_score;
      traj << restart << "\t" << sweep << "\t" << evals << "\t" << obj(cs) << "\t"
           << obj(best_score) << "\n";
    }
    if (obj(cs) < obj(best_score)) {
      best_score = cs;
      best_tree = cur;
      std::printf("restart %d: new best obj=%llu (Q=%llu B=%llu) after %d sweeps\n",
                  restart, static_cast<unsigned long long>(obj(cs)),
                  static_cast<unsigned long long>(cs.Q),
                  static_cast<unsigned long long>(cs.B), sweep);
    }
  }
  std::printf("SEARCH DONE objective=%s evals=%llu distinct=%zu\n",
              opts.objective.c_str(), static_cast<unsigned long long>(evals),
              visited.size());
  std::printf("BEST obj=%llu Q=%llu B_T=%llu  witness: Q=%llu B_T=%llu\n",
              static_cast<unsigned long long>(obj(best_score)),
              static_cast<unsigned long long>(best_score.Q),
              static_cast<unsigned long long>(best_score.B),
              static_cast<unsigned long long>(wsc.Q),
              static_cast<unsigned long long>(wsc.B));
  if (best_score.Q < wsc.Q)
    std::printf("IMPROVEMENT: Q %llu -> %llu (delta %lld)\n",
                static_cast<unsigned long long>(wsc.Q),
                static_cast<unsigned long long>(best_score.Q),
                static_cast<std::int64_t>(wsc.Q) -
                    static_cast<std::int64_t>(best_score.Q));
  else
    std::printf("NO context improvement (best Q delta %lld)\n",
                static_cast<std::int64_t>(best_score.Q) -
                    static_cast<std::int64_t>(wsc.Q));

  // Emit artifacts.
  auto best_tnode = tnode_from_stree(best_tree, ids);
  wsub::write_tree(best_tnode, reference, leaf_cgs, outdir + "/best_sub.pb.gz");
  auto grafted = wsub::splice(wroot, ids, best_tnode);
  wsub::write_tree(grafted, reference, leaf_cgs, outdir + "/best_graft.pb.gz");
  {
    std::ofstream nk(outdir + "/best_newick.txt");
    nk << canonical_newick(best_tree, ids) << "\n";
    std::ofstream tj(outdir + "/trajectory.tsv");
    tj << traj.str();
  }
  return 0;
}


// ============================ incremental engine (round 4) =================
//
// For large subtrees (600+ taxa) full-rescore sweeps are too slow.  This
// engine keeps per-slot Fitch rows and recomputes only ancestor paths per
// move.  Undo = revert structural edits + recompute the same paths (no row
// copies).  A scratch slot hosts the SPR join node during evaluation and is
// permanentized on accept (the bypassed parent's slot is recycled).

struct IncTree {
  int n = 0;                     // taxa
  std::size_t V = 0;
  Context const* ctx = nullptr;
  std::vector<int> parent;       // slot -> parent (-1 root-side)
  std::vector<int> tax;          // >=0 taxon (leaf), -1 internal alive, -2 free
  std::vector<std::array<int, 2>> ch;
  std::vector<std::uint8_t> mask;
  std::vector<std::uint32_t> cost;
  int root = -1;
  int scratch = -1;
  std::vector<int> freelist;

  void init(int n_, Context const& c) {
    n = n_;
    V = c.nvar;
    ctx = &c;
    grow(2 * static_cast<std::size_t>(n) + 16);
    for (std::size_t i = 0; i < parent.size(); ++i) {
      parent[i] = -1;
      tax[i] = -2;
      ch[i] = {-1, -1};
    }
    for (int i = 0; i < n; ++i) tax[i] = i;  // leaves fixed
    freelist.clear();
  }
  void grow(std::size_t slots) {
    auto old = parent.size();
    if (slots <= old) return;
    parent.resize(slots, -1);
    tax.resize(slots, -2);
    ch.resize(slots, {-1, -1});
    mask.resize(slots * V, 0);
    cost.resize(slots * V, 0);
  }
  int alloc_slot() {
    if (!freelist.empty()) {
      auto s = freelist.back();
      freelist.pop_back();
      return s;
    }
    auto s = static_cast<int>(parent.size());
    grow(parent.size() + 1);
    return s;
  }
  bool leaf(int i) const { return tax[i] >= 0; }
  bool alive(int i) const { return tax[i] != -2; }

  int sib(int u) const {
    auto p = parent[u];
    return ch[p][0] == u ? ch[p][1] : ch[p][0];
  }

  void compute_row(int i) {
    auto* m = &mask[static_cast<std::size_t>(i) * V];
    auto* c = &cost[static_cast<std::size_t>(i) * V];
    if (leaf(i)) {
      auto const& lc = ctx->leaf_code[static_cast<std::size_t>(tax[i])];
      for (std::size_t j = 0; j < V; ++j) {
        m[j] = lc[j];
        c[j] = 0;
      }
      return;
    }
    auto l = ch[i][0], r = ch[i][1];
    auto* lm = &mask[static_cast<std::size_t>(l) * V];
    auto* lc = &cost[static_cast<std::size_t>(l) * V];
    auto* rm = &mask[static_cast<std::size_t>(r) * V];
    auto* rc = &cost[static_cast<std::size_t>(r) * V];
    for (std::size_t j = 0; j < V; ++j)
      combine2(lm[j], lc[j], rm[j], rc[j], m[j], c[j]);
  }

  // Recompute i and all ancestors (children assumed valid).
  void recompute_up(int i) {
    for (int x = i; x >= 0; x = parent[x]) compute_row(x);
  }

  void full_recompute() {
    // postorder DFS from root over alive slots
    std::vector<int> order;
    std::vector<int> stack{root};
    while (!stack.empty()) {
      auto i = stack.back();
      stack.pop_back();
      order.push_back(i);
      if (!leaf(i)) {
        stack.push_back(ch[i][0]);
        stack.push_back(ch[i][1]);
      }
    }
    std::reverse(order.begin(), order.end());
    for (auto i : order) compute_row(i);
  }

  Score score() const {
    Score s;
    auto* m = &mask[static_cast<std::size_t>(root) * V];
    auto* c = &cost[static_cast<std::size_t>(root) * V];
    for (std::size_t j = 0; j < V; ++j) {
      s.B += c[j];
      if (!(m[j] & ctx->out_mask[j])) ++s.Q;
    }
    s.Q += ctx->const_q + s.B;
    return s;
  }

  std::size_t count_leaves() const {
    std::size_t k = 0;
    for (std::size_t i = 0; i < tax.size(); ++i)
      if (tax[i] >= 0) ++k;
    return k;
  }
};

// Evaluator with structural undo + recompute-on-undo.
struct Edits {
  struct Edit {
    int kind;  // 0 parent[a]=old, 1 ch[a][b]=old, 2 root=old, 3 tax[a]=old
    int a, b, old;
  };
  IncTree* T = nullptr;
  std::vector<Edit> edits;
  std::vector<int> undo_paths;  // starts for recompute-on-undo

  void set_parent(int a, int v) {
    edits.push_back({0, a, 0, T->parent[a]});
    T->parent[a] = v;
  }
  void set_ch(int a, int ci, int v) {
    edits.push_back({1, a, ci, T->ch[a][ci]});
    T->ch[a][ci] = v;
  }
  void set_root(int v) {
    edits.push_back({2, 0, 0, T->root});
    T->root = v;
  }
  void set_tax(int a, int v) {
    edits.push_back({3, a, 0, T->tax[a]});
    T->tax[a] = v;
  }
  void undo() {
    for (std::size_t i = edits.size(); i-- > 0;) {
      auto const& e = edits[i];
      if (e.kind == 0) T->parent[e.a] = e.old;
      else if (e.kind == 1) T->ch[e.a][e.b] = e.old;
      else if (e.kind == 2) T->root = e.old;
      else T->tax[e.a] = e.old;
    }
    edits.clear();
    // restore rows along the same paths (bottom-up from the edit points)
    for (auto start : undo_paths) T->recompute_up(start);
    undo_paths.clear();
  }
  void accept() {
    edits.clear();
    undo_paths.clear();
  }
};

struct IncSearch {
  static bool verify_evals() {
    static bool v = std::getenv("WRIC_VERIFY_EVALS") != nullptr;
    return v;
  }
  void verify_eval_state(char const* where) {
    if (!verify_evals()) return;
    IncTree T2 = T;
    T2.full_recompute();
    auto s1 = T.score();
    auto s2 = T2.score();
    if (s1.Q != s2.Q || s1.B != s2.B) {
      std::fprintf(stderr, "EVAL VERIFY FAIL %s: inc Q=%llu full Q=%llu\n",
                   where, (unsigned long long)s1.Q, (unsigned long long)s2.Q);
      std::abort();
    }
  }
  IncTree T;
  Context const* ctx = nullptr;
  int n = 0;

  void build_from_stree(STree const& st) {
    // map stree slots -> inc slots (leaves by taxon; internal appended)
    T.init(n, *ctx);
    std::vector<int> map(st.parent.size(), -1);
    // postorder build
    std::vector<int> order;
    {
      std::vector<int> stack{st.root};
      while (!stack.empty()) {
        auto i = stack.back();
        stack.pop_back();
        order.push_back(i);
        if (!st.leaf(i)) {
          stack.push_back(st.ch[i][0]);
          stack.push_back(st.ch[i][1]);
        }
      }
      std::reverse(order.begin(), order.end());
    }
    int next_internal = n;
    for (auto i : order) {
      if (st.leaf(i)) {
        map[i] = st.tax[i];  // leaf slot == taxon id
        continue;
      }
      int slot = next_internal++;
      map[i] = slot;
      T.tax[slot] = -1;
      T.ch[slot] = {map[st.ch[i][0]], map[st.ch[i][1]]};
      T.parent[map[st.ch[i][0]]] = slot;
      T.parent[map[st.ch[i][1]]] = slot;
    }
    T.root = map[st.root];
    T.scratch = next_internal++;
    T.tax[T.scratch] = -2;
    T.full_recompute();
  }

  // ---- NNI: swap grandchild of y (child of x) with y's sibling ----
  // Returns eval'ed score of the move (structure restored unless accept).
  Score eval_nni(int x, int y, int side, bool accept_if_better, Score const& cur,
                 bool& improved, Edits& ed) {
    auto gy = T.ch[y][side];
    auto ox = T.ch[x][0] == y ? T.ch[x][1] : T.ch[x][0];
    auto gy_pos = T.ch[x][0] == y ? 1 : 0;
    auto ox_pos_in_y = side;
    // apply
    ed.set_ch(x, gy_pos, gy);
    ed.set_ch(y, ox_pos_in_y, ox);
    ed.set_parent(gy, x);
    ed.set_parent(ox, y);
    ed.undo_paths.push_back(y);
    T.recompute_up(y);
    auto sc = T.score();
    verify_eval_state("eval_nni");
    auto obj = [&](Score const& s) { return ctx_obj(s); };
    if (accept_if_better && obj(sc) < obj(cur)) {
      ed.accept();
      improved = true;
      return sc;
    }
    ed.undo();
    return sc;
  }
  // placeholder objective selector set later
  bool use_isolated = false;
  std::uint64_t ctx_obj(Score const& s) const {
    return use_isolated ? s.B : s.Q;
  }

  // ---- SPR: prune subtree(u), regraft above b ----
  Score eval_spr(int u, int b, bool accept_if_better, Score const& cur,
                 bool& improved, Edits& ed) {
    auto p = T.parent[u];
    auto s = T.sib(u);
    auto sp = T.parent[p];
    auto w = T.scratch;
    // bypass p
    if (sp >= 0) {
      auto p_pos = T.ch[sp][0] == p ? 0 : 1;
      ed.set_ch(sp, p_pos, s);
      ed.set_parent(s, sp);
    } else {
      ed.set_root(s);
      ed.set_parent(s, -1);
    }
    auto a = T.parent[b];  // AFTER bypass: covers b == sibling(u)
    // join at edge above b
    ed.set_ch(w, 0, u);
    ed.set_ch(w, 1, b);
    ed.set_parent(u, w);
    ed.set_parent(b, w);
    if (a >= 0) {
      auto b_pos = T.ch[a][0] == b ? 0 : 1;
      ed.set_ch(a, b_pos, w);
      ed.set_parent(w, a);
      ed.undo_paths.push_back(a);
    } else {
      ed.set_root(w);
      ed.set_parent(w, -1);
      ed.undo_paths.push_back(w);
    }
    // Recompute BOTH affected paths before scoring: the sp-side chain
    // (bypass) and the a-side chain (join).  Two bottom-up passes converge
    // (the second pass re-derives any shared suffix from fixed children).
    T.compute_row(w);
    if (sp >= 0) T.recompute_up(sp);
    if (a >= 0) T.recompute_up(a);
    if (sp >= 0) ed.undo_paths.push_back(sp);
    auto sc = T.score();
    verify_eval_state("eval_spr");
    if (accept_if_better && ctx_obj(sc) < ctx_obj(cur)) {
      // permanentize: w alive, old p recycled, fresh scratch
      ed.set_tax(w, -1);
      ed.accept();
      T.tax[p] = -2;
      T.freelist.push_back(p);
      T.scratch = T.alloc_slot();
      improved = true;
      return sc;
    }
    ed.undo();
    return sc;
  }

  // random SPR apply (no acceptance test) for perturbation starts
  void apply_random_spr(std::mt19937& rng) {
    std::vector<int> alive;
    for (std::size_t i = 0; i < T.tax.size(); ++i)
      if (T.alive(static_cast<int>(i)) && static_cast<int>(i) != T.root)
        alive.push_back(static_cast<int>(i));
    std::uniform_int_distribution<std::size_t> dpick(0, alive.size() - 1);
    for (int tries = 0; tries < 200; ++tries) {
      auto u = alive[dpick(rng)];
      // collect rest nodes
      std::vector<int> in_sub;
      {
        std::vector<int> stack{u};
        while (!stack.empty()) {
          auto i = stack.back();
          stack.pop_back();
          in_sub.push_back(i);
          if (!T.leaf(i)) {
            stack.push_back(T.ch[i][0]);
            stack.push_back(T.ch[i][1]);
          }
        }
      }
      std::vector<char> in(T.tax.size(), 0);
      for (auto i : in_sub) in[i] = 1;
      std::vector<int> rest;
      for (std::size_t i = 0; i < T.tax.size(); ++i)
        if (T.alive(static_cast<int>(i)) && !in[i] &&
            static_cast<int>(i) != T.parent[u])
          rest.push_back(static_cast<int>(i));
      if (rest.empty()) continue;
      std::uniform_int_distribution<std::size_t> bpick(0, rest.size() - 1);
      auto b = rest[bpick(rng)];
      bool improved_dummy = false;
      Score dummy;
      dummy.Q = ~0ull;
      dummy.B = ~0ull;
      Edits ed;
      ed.T = &T;
      eval_spr(u, b, /*accept_if_better=*/true, dummy, improved_dummy, ed);
      // dummy has max objective so accept_if_better triggers iff score < max:
      // guaranteed; move applied.
      return;
    }
  }
};


// ---------------------------- big-search driver ----------------------------

struct IncSnapshot {
  std::vector<int> parent, tax, root1;
  std::vector<std::array<int, 2>> ch;
  int root = -1;
  std::vector<std::uint8_t> mask;
  std::vector<std::uint32_t> cost;
};

void snapshot(IncTree const& T, IncSnapshot& s) {
  s.parent = T.parent;
  s.tax = T.tax;
  s.ch = T.ch;
  s.root = T.root;
  s.mask = T.mask;
  s.cost = T.cost;
}
void restore(IncSnapshot const& s, IncTree& T) {
  T.parent = s.parent;
  T.tax = s.tax;
  T.ch = s.ch;
  T.root = s.root;
  T.mask = s.mask;
  T.cost = s.cost;
  T.freelist.clear();
  T.scratch = -1;
  // re-establish a scratch slot
  for (std::size_t i = 0; i < T.tax.size(); ++i)
    if (T.tax[i] == -2) {
      T.scratch = static_cast<int>(i);
      break;
    }
  if (T.scratch < 0) {
    T.scratch = T.alloc_slot();
  }
}

tnode tnode_from_inc(IncTree const& T, std::vector<std::string> const& ids) {
  std::function<tnode(int)> rec = [&](int i) -> tnode {
    tnode nd;
    if (T.leaf(i)) {
      nd.leaf = true;
      nd.sid = ids[static_cast<std::size_t>(T.tax[i])];
      nd.taxa = {nd.sid};
      return nd;
    }
    nd.children.push_back(rec(T.ch[i][0]));
    nd.children.push_back(rec(T.ch[i][1]));
    nd.taxa = wsub::sorted_union(nd.children[0].taxa, nd.children[1].taxa);
    return nd;
  };
  return rec(T.root);
}

void verify_incremental(IncTree const& T, char const* where) {
  // structural check: reachability, arity, leaf count, parent consistency
  {
    std::vector<char> seen(T.tax.size(), 0);
    std::vector<int> stack{T.root};
    seen[T.root] = 1;
    std::size_t leaves = 0;
    while (!stack.empty()) {
      auto i = stack.back();
      stack.pop_back();
      if (T.leaf(i)) {
        ++leaves;
        continue;
      }
      for (auto c : T.ch[i]) {
        if (c < 0 || seen[c] || !T.alive(c))
          throw std::runtime_error(std::string("inc structure FAIL ") + where);
        if (T.parent[c] != i)
          throw std::runtime_error(std::string("inc parent FAIL ") + where);
        seen[c] = 1;
        stack.push_back(c);
      }
    }
    std::size_t alive_count = 0;
    for (std::size_t i = 0; i < T.tax.size(); ++i)
      if (T.alive(static_cast<int>(i))) ++alive_count;
    std::size_t reached = 0;
    for (std::size_t i = 0; i < T.tax.size(); ++i)
      if (seen[i]) ++reached;
    if (leaves != static_cast<std::size_t>(T.n) ||
        reached != alive_count ||
        T.parent[T.root] != -1)
      throw std::runtime_error(std::string("inc leaves/root FAIL ") + where);
    for (std::size_t i = 0; i < T.tax.size(); ++i)
      if (T.alive(static_cast<int>(i)) && static_cast<int>(i) != T.root &&
          (!T.alive(T.parent[static_cast<int>(i)]) ||
           T.parent[static_cast<int>(i)] < 0))
        throw std::runtime_error(std::string("inc dangling FAIL ") + where);
  }
  IncSnapshot snap;
  snapshot(T, snap);
  IncTree T2;
  T2 = T;  // copy
  T2.full_recompute();
  bool ok = true;
  for (std::size_t i = 0; i < T.tax.size() && ok; ++i) {
    if (!T.alive(static_cast<int>(i))) continue;
    for (std::size_t j = 0; j < T.V; ++j) {
      if (T.mask[i * T.V + j] != T2.mask[i * T.V + j] ||
          T.cost[i * T.V + j] != T2.cost[i * T.V + j]) {
        std::fprintf(stderr, "INC VERIFY FAIL %s slot %zu site %zu\n", where, i, j);
        ok = false;
        break;
      }
    }
  }
  auto s1 = const_cast<IncTree&>(T).score();
  (void)snap;
  if (ok) std::fprintf(stderr, "inc verify OK (%s): Q=%llu B=%llu\n", where,
                       (unsigned long long)s1.Q, (unsigned long long)s1.B);
}

int run_bigsearch(std::string const& witness_path, std::vector<std::string> const& ids,
                  std::string const& outdir, Opts const& opts) {
  std::string reference;
  std::map<std::string, larch::compact_genome> leaf_cgs;
  auto wroot = wsub::load_tree(witness_path, &reference, &leaf_cgs);
  auto al = build_alignment(reference, leaf_cgs);
  tnode const* target = nullptr;
  {
    std::vector<tnode const*> stack{&wroot};
    while (!stack.empty()) {
      auto* n = stack.back();
      stack.pop_back();
      if (n->leaf) continue;
      if (n->taxa == ids) {
        target = n;
        break;
      }
      stack.push_back(&n->children[0]);
      stack.push_back(&n->children[1]);
    }
    if (!target) throw std::runtime_error("bigsearch: target taxon set not found");
  }
  int slot = -1;
  auto w = flatten_witness(wroot, al, target, &slot);
  auto t = fitch_tables(w, al, opts.ref_root, reference);
  auto ctx = build_context(t, w, al, slot, ids);
  std::unordered_map<std::string, int> tid;
  for (std::size_t i = 0; i < ids.size(); ++i) tid[ids[i]] = static_cast<int>(i);
  auto n = static_cast<int>(ids.size());
  std::printf("context: nvar=%zu const_q=%llu\n", ctx.nvar,
              (unsigned long long)ctx.const_q);

  IncSearch is;
  is.ctx = &ctx;
  is.n = n;
  is.use_isolated = (opts.objective == "isolated");
  auto wt = stree_from_tnode(*target, tid, n);
  is.build_from_stree(wt);
  auto wsc = is.T.score();
  std::printf("witness subtree: Q = %llu B_T = %llu\n",
              (unsigned long long)wsc.Q, (unsigned long long)wsc.B);
  if (std::getenv("WRIC_VERIFY_INCREMENTAL"))
    verify_incremental(is.T, "initial");

  auto obj = [&](Score const& s) { return is.use_isolated ? s.B : s.Q; };

  IncSnapshot best_snap;
  snapshot(is.T, best_snap);
  Score best_score = wsc;
  std::uint64_t evals = 0, accepts = 0;
  std::ostringstream traj;
  traj << "restart\tphase\tevals\tobj\tbest\n";

  std::mt19937 rng(static_cast<std::uint32_t>(opts.seed));
  Edits ed;
  ed.T = &is.T;

  auto nni_phase = [&](int cap) {
    int sweeps = 0;
    for (; sweeps < cap; ++sweeps) {
      bool any = false;
      for (int y = 0; y < static_cast<int>(is.T.tax.size()); ++y) {
        if (y == is.T.root || !is.T.alive(y) || is.T.leaf(y)) continue;
        auto x = is.T.parent[y];
        if (x < 0 || is.T.leaf(x)) continue;
        for (int side = 0; side < 2; ++side) {
          bool improved = false;
          auto cur = is.T.score();
          evals++;
          auto sc = is.eval_nni(x, y, side, /*accept_if_better=*/true, cur,
                                improved, ed);
          if (improved) {
            ++accepts;
            any = true;
          }
        }
      }
      if (!any) break;
    }
    return sweeps;
  };

  auto spr_phase = [&](int rounds, int samples) -> bool {
    bool ever = false;
    for (int r = 0; r < rounds; ++r) {
      bool any = false;
      std::vector<int> nodes;
      for (int i = 0; i < static_cast<int>(is.T.tax.size()); ++i)
        if (is.T.alive(i) && i != is.T.root) nodes.push_back(i);
      std::uniform_int_distribution<std::size_t> dpick(0, nodes.size() - 1);
      auto heartbeat = std::chrono::steady_clock::now();
      for (auto u : nodes) {
        if (!is.T.alive(u) || u == is.T.root) continue;
        if (std::getenv("WRIC_HEARTBEAT")) {
          auto now = std::chrono::steady_clock::now();
          if (std::chrono::duration_cast<std::chrono::seconds>(now - heartbeat)
                  .count() > 300) {
            heartbeat = now;
            auto live = is.T.score();
            std::fprintf(stderr, "hb spr r=%d u=%d Q=%llu B=%llu\n", r, u,
                         (unsigned long long)live.Q,
                         (unsigned long long)live.B);
          }
        }
        // candidate b's: random alive nodes outside subtree(u) and != parent
        auto p = is.T.parent[u];
        std::vector<int> cands;
        std::uniform_int_distribution<std::size_t> pick(0, nodes.size() - 1);
        for (int k = 0; k < samples; ++k) {
          auto b = nodes[pick(rng)];
          if (b == u || b == p || !is.T.alive(b)) continue;
          // reject if b inside subtree(u): walk parents
          bool inside = false;
          for (int q = b; q >= 0; q = is.T.parent[q])
            if (q == u) {
              inside = true;
              break;
            }
          if (inside) continue;
          cands.push_back(b);
        }
        for (auto b : cands) {
          bool improved = false;
          auto cur = is.T.score();
          ++evals;
          is.eval_spr(u, b, /*accept_if_better=*/true, cur, improved, ed);
          if (improved) {
            ++accepts;
            any = true;
            break;  // first improvement per u
          }
        }
      }
      if (!any) break;
      ever = true;
    }
    return ever;
  };

  for (int restart = 0; restart <= opts.starts; ++restart) {
    if (restart == 0) {
      is.build_from_stree(wt);  // witness start
    } else if (opts.starts_mode == "random") {
      auto rs = random_stree(n, rng);
      is.build_from_stree(rs);
    } else {
      is.build_from_stree(wt);
      for (int k = 0; k < opts.perturb; ++k) is.apply_random_spr(rng);
    }
    auto cur0 = is.T.score();
    for (int phase = 0; phase < 12; ++phase) {
      auto sw = nni_phase(opts.sweeps);
      auto spr_improved = spr_phase(opts.spr_rounds, opts.spr_samples);
      if (sw == 0 && !spr_improved) break;
    }
    auto sc = is.T.score();
    traj << restart << "\tend\t" << evals << "\t" << obj(sc) << "\t"
         << obj(best_score) << "\n";
    if (obj(sc) < obj(best_score)) {
      best_score = sc;
      snapshot(is.T, best_snap);
      std::printf("restart %d: new best Q=%llu B=%llu (perturb=%d)\n", restart,
                  (unsigned long long)sc.Q, (unsigned long long)sc.B,
                  opts.perturb);
    }
    if (std::getenv("WRIC_VERIFY_INCREMENTAL") && restart < 3)
      verify_incremental(is.T, "restart");
    (void)cur0;
  }

  std::printf("BIGSEARCH DONE objective=%s evals=%llu accepts=%llu\n",
              opts.objective.c_str(), (unsigned long long)evals,
              (unsigned long long)accepts);
  std::printf("BEST Q=%llu B_T=%llu  witness: Q=%llu B_T=%llu\n",
              (unsigned long long)best_score.Q,
              (unsigned long long)best_score.B,
              (unsigned long long)wsc.Q, (unsigned long long)wsc.B);
  if (best_score.Q < wsc.Q)
    std::printf("IMPROVEMENT: Q %llu -> %llu (delta %lld)\n",
                (unsigned long long)wsc.Q, (unsigned long long)best_score.Q,
                (std::int64_t)wsc.Q - (std::int64_t)best_score.Q);
  else
    std::printf("NO context improvement (best Q delta %lld)\n",
                (std::int64_t)best_score.Q - (std::int64_t)wsc.Q);

  restore(best_snap, is.T);
  if (std::getenv("WRIC_VERIFY_INCREMENTAL")) {
    auto chk = is.T.score();
    if (chk.Q != best_score.Q || chk.B != best_score.B)
      throw std::runtime_error("bigsearch: restore mismatch");
    verify_incremental(is.T, "restored-best");
  }
  auto best_tnode = tnode_from_inc(is.T, ids);
  wsub::write_tree(best_tnode, reference, leaf_cgs, outdir + "/best_sub.pb.gz");
  auto grafted = wsub::splice(wroot, ids, best_tnode);
  wsub::write_tree(grafted, reference, leaf_cgs, outdir + "/best_graft.pb.gz");
  {
    // canonical newick via existing STree machinery
    std::ofstream nk(outdir + "/best_newick.txt");
    std::function<void(int, std::ostream&)> emit = [&](int i, std::ostream& os) {
      if (is.T.leaf(i)) {
        os << ids[(std::size_t)is.T.tax[i]];
        return;
      }
      os << "(";
      emit(is.T.ch[i][0], os);
      os << ",";
      emit(is.T.ch[i][1], os);
      os << ")";
    };
    emit(is.T.root, nk);
    nk << ";\n";
    std::ofstream tj(outdir + "/trajectory.tsv");
    tj << traj.str();
  }
  return 0;
}


int run_diag(std::string const& witness_path, std::vector<std::string> const& ids,
             std::string const& best_sub_path) {
  std::string reference;
  std::map<std::string, larch::compact_genome> leaf_cgs;
  auto wroot = wsub::load_tree(witness_path, &reference, &leaf_cgs);
  auto al = build_alignment(reference, leaf_cgs);
  tnode const* target = nullptr;
  {
    std::vector<tnode const*> stack{&wroot};
    while (!stack.empty()) {
      auto* n = stack.back();
      stack.pop_back();
      if (n->leaf) continue;
      if (n->taxa == ids) { target = n; break; }
      stack.push_back(&n->children[0]);
      stack.push_back(&n->children[1]);
    }
    if (!target) throw std::runtime_error("diag: target not found");
  }
  int slot = -1;
  auto w = flatten_witness(wroot, al, target, &slot);
  auto t = fitch_tables(w, al, false, reference);
  auto ctx = build_context(t, w, al, slot, ids);
  std::unordered_map<std::string, int> tid;
  for (std::size_t i = 0; i < ids.size(); ++i) tid[ids[i]] = static_cast<int>(i);
  auto n = static_cast<int>(ids.size());

  IncSearch is;
  is.ctx = &ctx;
  is.n = n;
  auto wt = stree_from_tnode(*target, tid, n);
  is.build_from_stree(wt);
  auto wsc = is.T.score();
  std::printf("witness subtree: Q=%llu B=%llu\n",
              (unsigned long long)wsc.Q, (unsigned long long)wsc.B);

  // best over ALL SPR evals (no acceptance)
  Edits ed;
  ed.T = &is.T;
  Score best_all = wsc;
  int best_u = -1, best_b = -1;
  std::uint64_t count = 0;
  std::vector<int> nodes;
  for (int i = 0; i < static_cast<int>(is.T.tax.size()); ++i)
    if (is.T.alive(i)) nodes.push_back(i);
  for (auto u : nodes) {
    if (u == is.T.root) continue;
    for (auto b : nodes) {
      if (b == u || b == is.T.parent[u] || !is.T.alive(b)) continue;
      bool inside = false;
      for (int q = b; q >= 0; q = is.T.parent[q])
        if (q == u) { inside = true; break; }
      if (inside) continue;
      bool improved = false;
      ++count;
      auto sc = is.eval_spr(u, b, /*accept_if_better=*/false, wsc, improved, ed);
      if (sc.Q < best_all.Q) {
        best_all = sc;
        best_u = u;
        best_b = b;
      }
    }
  }
  std::printf("diag: %llu SPR evals; best Q=%llu B=%llu (witness Q=%llu)\n",
              (unsigned long long)count, (unsigned long long)best_all.Q,
              (unsigned long long)best_all.B, (unsigned long long)wsc.Q);
  if (best_u >= 0) std::printf("diag: best move u=%d b=%d\n", best_u, best_b);
  // NNI sweep too
  Score best_nni = wsc;
  for (int y = 0; y < static_cast<int>(is.T.tax.size()); ++y) {
    if (y == is.T.root || !is.T.alive(y) || is.T.leaf(y)) continue;
    auto x = is.T.parent[y];
    if (x < 0 || is.T.leaf(x)) continue;
    for (int side = 0; side < 2; ++side) {
      bool improved = false;
      auto sc = is.eval_nni(x, y, side, false, wsc, improved, ed);
      if (sc.Q < best_nni.Q) best_nni = sc;
    }
  }
  std::printf("diag: best NNI Q=%llu B=%llu\n",
              (unsigned long long)best_nni.Q, (unsigned long long)best_nni.B);
  // compare with the known-best subtree
  if (!best_sub_path.empty()) {
    std::string ref_ignored;
    auto broot = wsub::load_tree(best_sub_path, &ref_ignored, nullptr);
    auto bt = stree_from_tnode(broot, tid, n);
    IncSearch is2;
    is2.ctx = &ctx;
    is2.n = n;
    is2.build_from_stree(bt);
    auto bsc = is2.T.score();
    std::printf("diag: known-best subtree scores Q=%llu B=%llu under inc engine\n",
                (unsigned long long)bsc.Q, (unsigned long long)bsc.B);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc >= 5 && std::string(argv[1]) == "diag") {
      return run_diag(argv[2], wsub::read_ids(argv[3]), argv[4]);
    }
    if (argc >= 4 && std::string(argv[1]) == "eval") {
      std::vector<std::string> cands;
      for (int i = 4; i < argc; ++i) cands.push_back(argv[i]);
      return run_eval(argv[2], wsub::read_ids(argv[3]), cands);
    }
    if (argc >= 5 && std::string(argv[1]) == "search") {
      Opts opts;
      for (int i = 5; i < argc; ++i) {
        std::string kv = argv[i];
        auto eq = kv.find('=');
        if (eq == std::string::npos)
          throw std::runtime_error("context_spr: expected key=value: " + kv);
        auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (k == "objective") opts.objective = v;
        else if (k == "starts") opts.starts = std::atoi(v.c_str());
        else if (k == "seed") opts.seed = std::atoi(v.c_str());
        else if (k == "sweeps") opts.sweeps = std::atoi(v.c_str());
        else if (k == "root") opts.ref_root = (v == "ref");
        else if (k == "engine") opts.engine = v;
        else if (k == "starts_mode") opts.starts_mode = v;
        else if (k == "perturb") opts.perturb = std::atoi(v.c_str());
        else if (k == "spr_samples") opts.spr_samples = std::atoi(v.c_str());
        else if (k == "spr_rounds") opts.spr_rounds = std::atoi(v.c_str());
        else throw std::runtime_error("context_spr: unknown option " + kv);
      }
      if (opts.engine == "inc")
        return run_bigsearch(argv[2], wsub::read_ids(argv[3]), argv[4], opts);
      return run_search(argv[2], wsub::read_ids(argv[3]), argv[4], opts);
    }
    std::fprintf(stderr,
                 "usage: %s eval <witness.pb.gz> <ids.txt> [cand.pb.gz ...]\n"
                 "       %s search <witness.pb.gz> <ids.txt> <outdir> "
                 "[key=value ...]\n",
                 argv[0], argv[0]);
    return 2;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
