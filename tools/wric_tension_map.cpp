// wric_tension_map: per-clade localization of the coupling gap (optimum -
// composite inside LB) on a certified witness tree, plus the closure
// attribution between two grammars (experiment 1 of
// ~/tmp/diversity-signals.md).
//
// Modes:
//   tension <grammar_dag.pb.gz> <witness_tree.pb.gz> <expected_total>
//   closure <union_dag> <union_witness> <union_total>
//           <enriched_dag> <enriched_witness> <enriched_total>
//
// Method (see doc section 3): row-key classes; unpruned per-class chart DP
// (inside minima mrow, min over all grammar derivations); per-class tree DP
// on the witness (F rows); then the exact top-down clamp chain per active
// pattern that decomposes cost_p(T*) - m_p(root) into nonnegative per-clade
// marginals.  All arithmetic reuses the trim's primitives
// (combine_class_row, transition_cost, root-edge conventions, invariant
// offset).  No frontier construction.

#include <larch/load_proto_dag.hpp>
#include <larch/chart_trim.hpp>
#include <larch/polytomy_refinement.hpp>
#include <larch/site_patterns.hpp>
#include <larch/clade_grammar.hpp>
#include <larch/parsimony_chart.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace larch;

namespace {

using chart_multisite_detail::active_pattern_info;
using chart_multisite_detail::combine_class_row;
using chart_multisite_detail::multisite_production_class_map;
using chart_multisite_detail::multisite_restriction_classes;
using chart_multisite_detail::build_production_class_map;
using chart_multisite_detail::build_restriction_classes;

struct canon_key_hash {
  std::size_t operator()(std::vector<std::uint32_t> const& key) const {
    std::size_t h = key.size();
    for (auto v : key) h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};
using CanonKey = std::vector<std::uint32_t>;  // sorted canonical taxon indices

struct witness_node {
  std::size_t dag_idx = no_idx;
  clade_id clade = no_clade;
  std::int64_t parent = -1;
  std::vector<std::uint32_t> children;  // [left, right] in production order
  std::vector<std::uint32_t> taxa;      // canonical, sorted
  std::uint32_t depth = 0;
};

struct grammar_ctx {
  clade_grammar grammar;
  std::vector<multisite_restriction_classes> classes;
  std::vector<multisite_production_class_map> production_maps;
  std::vector<active_pattern_info> active;
  site_pattern_set patterns;
  chart_options chart_opts;
  std::uint64_t invariant_offset = 0;
  std::vector<std::uint32_t> canon_of_taxon;  // taxon id -> canonical idx
  std::unordered_map<CanonKey, clade_id, canon_key_hash> clade_by_key;
  std::vector<CanonKey> clade_canon;          // per clade id
};

grammar_ctx prepare(std::string const& dag_path) {
  grammar_ctx ctx;
  auto dag = load_proto_dag(dag_path);

  clade_grammar_options grammar_opts;
  polytomy_refinement_options polytomy_opts;
  polytomy_opts.mode = polytomy_mode::expand_soft_bounded;
  polytomy_opts.max_shapes_per_polytomy = 1;
  auto refinement =
      build_polytomy_refined_clade_grammar(dag, grammar_opts, polytomy_opts);
  ctx.grammar = std::move(refinement.grammar);

  site_pattern_options pattern_opts;
  ctx.patterns = build_site_patterns(dag, ctx.grammar, pattern_opts);
  ctx.chart_opts.score_ua_edge = true;
  ctx.invariant_offset =
      chart_multisite_detail::invariant_constant_offset(ctx.patterns,
                                                        ctx.chart_opts);

  for (std::size_t i = 0; i < ctx.patterns.patterns.size(); ++i) {
    auto const& pattern = ctx.patterns.patterns[i];
    if (!chart_multisite_detail::is_active_pattern(pattern)) continue;
    active_pattern_info info;
    info.pattern_index = i;
    info.weight = pattern.weight;
    info.reference_state_counts = pattern.reference_state_counts;
    info.state_by_taxon = pattern.state_by_taxon;
    ctx.active.push_back(std::move(info));
  }

  // Canonical taxon order = sorted sample ids (identical leaf sets across
  // grammars give identical orders).
  std::vector<std::string> order = ctx.grammar.taxa.id_to_sample_id;
  std::sort(order.begin(), order.end());
  std::unordered_map<std::string, std::uint32_t> canon_of_sample;
  for (std::uint32_t i = 0; i < order.size(); ++i)
    canon_of_sample.emplace(order[i], i);
  ctx.canon_of_taxon.assign(ctx.grammar.taxa.id_to_sample_id.size(), 0);
  for (std::uint32_t t = 0; t < ctx.grammar.taxa.id_to_sample_id.size(); ++t) {
    auto it = canon_of_sample.find(ctx.grammar.taxa.id_to_sample_id[t]);
    if (it == canon_of_sample.end())
      throw std::runtime_error("tension map: sample id not in canonical order");
    ctx.canon_of_taxon[t] = it->second;
  }

  ctx.classes.assign(ctx.grammar.clades.size(), {});
  std::vector<clade_id> table_order(ctx.grammar.clades.size());
  std::iota(table_order.begin(), table_order.end(), clade_id{0});
  std::stable_sort(table_order.begin(), table_order.end(),
                   [&](clade_id lhs, clade_id rhs) {
                     auto l = ctx.grammar.clades[lhs].taxa.size();
                     auto r = ctx.grammar.clades[rhs].taxa.size();
                     if (l != r) return l < r;
                     return lhs < rhs;
                   });
  for (auto clade : table_order)
    ctx.classes[clade] =
        build_restriction_classes(ctx.grammar, clade, ctx.active, ctx.classes);

  ctx.production_maps.assign(ctx.grammar.productions.size(), {});
  for (std::size_t pid = 0; pid < ctx.grammar.productions.size(); ++pid)
    ctx.production_maps[pid] =
        build_production_class_map(ctx.grammar, ctx.grammar.productions[pid],
                                   ctx.classes);

  ctx.clade_canon.assign(ctx.grammar.clades.size(), {});
  for (std::uint32_t c = 0; c < ctx.grammar.clades.size(); ++c) {
    CanonKey key;
    key.reserve(ctx.grammar.clades[c].taxa.size());
    for (auto t : ctx.grammar.clades[c].taxa)
      key.push_back(ctx.canon_of_taxon[t]);
    std::sort(key.begin(), key.end());
    ctx.clade_canon[c] = std::move(key);
    ctx.clade_by_key.emplace(ctx.clade_canon[c], clade_id{c});
  }
  return ctx;
}

std::vector<witness_node> load_witness(std::string const& path,
                                       grammar_ctx const& ctx,
                                       std::string* reference_out = nullptr) {
  auto dag = load_proto_dag(path);
  std::size_t ua_idx = no_idx;
  std::size_t root_idx = no_idx;
  for (std::size_t i = 0; i < dag.node_count(); ++i) {
    if (larch::detail::is_ua_node(dag.get_node(i))) {
      ua_idx = i;
      break;
    }
  }
  if (ua_idx == no_idx)
    throw std::runtime_error("tension map: witness has no UA node");
  if (reference_out) {
    std::visit(
        [&](auto n) {
          using N = std::remove_cvref_t<decltype(n)>;
          if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::ua>>)
            *reference_out = n.reference_sequence();
        },
        dag.get_node(ua_idx));
  }
  std::visit(
      [&](auto node) {
        for (auto ev : node.get_children())
          std::visit(
              [&](auto edge) {
                if (root_idx == no_idx)
                  root_idx = larch::get_child_idx(dag, edge.index());
              },
              ev);
      },
      dag.get_node(ua_idx));
  if (root_idx == no_idx)
    throw std::runtime_error("tension map: witness UA has no child");

  std::vector<witness_node> nodes;
  std::vector<std::pair<std::size_t, std::int64_t>> stack{{root_idx, -1}};
  while (!stack.empty()) {
    auto [idx, parent] = stack.back();
    stack.pop_back();
    witness_node node;
    node.dag_idx = idx;
    node.parent = parent;
    auto nv = dag.get_node(idx);
    if (larch::detail::is_leaf_node(nv)) {
      std::string sample_id;
      std::visit(
          [&](auto n) {
            using N = std::remove_cvref_t<decltype(n)>;
            if constexpr (std::is_same_v<N,
                               node_view<phylo_dag, node_kind::leaf>>)
              sample_id = n.sample_id();
          },
          nv);
      auto it = ctx.grammar.taxa.sample_id_to_id.find(sample_id);
      if (it == ctx.grammar.taxa.sample_id_to_id.end())
        throw std::runtime_error("tension map: witness leaf not in grammar: " +
                                 sample_id);
      node.taxa = {ctx.canon_of_taxon[it->second]};
      auto found = ctx.clade_by_key.find(node.taxa);
      if (found == ctx.clade_by_key.end())
        throw std::runtime_error("tension map: leaf clade missing for " +
                                 sample_id);
      node.clade = found->second;
      if (parent >= 0)
        nodes[static_cast<std::size_t>(parent)].children.push_back(
            static_cast<std::uint32_t>(nodes.size()));
      nodes.push_back(std::move(node));
      continue;
    }
    std::vector<std::size_t> child_idxs;
    std::visit(
        [&](auto n) {
          for (auto ev : n.get_children())
            std::visit(
                [&](auto edge) {
                  child_idxs.push_back(
                      larch::get_child_idx(dag, edge.index()));
                },
                ev);
        },
        nv);
    if (child_idxs.size() != 2)
      throw std::runtime_error("tension map: witness internal node with " +
                               std::to_string(child_idxs.size()) +
                               " children");
    if (parent >= 0)
      nodes[static_cast<std::size_t>(parent)].children.push_back(
          static_cast<std::uint32_t>(nodes.size()));
    nodes.push_back(std::move(node));
    auto self = static_cast<std::int64_t>(nodes.size() - 1);
    for (auto it = child_idxs.rbegin(); it != child_idxs.rend(); ++it)
      stack.push_back({*it, self});
  }

  for (std::size_t i = nodes.size(); i-- > 0;) {
    auto& node = nodes[i];
    if (node.children.empty()) continue;
    node.taxa = nodes[node.children[0]].taxa;
    auto rhs = nodes[node.children[1]].taxa;
    node.taxa.insert(node.taxa.end(), rhs.begin(), rhs.end());
    std::sort(node.taxa.begin(), node.taxa.end());
    node.depth =
        1 + std::max(nodes[node.children[0]].depth, nodes[node.children[1]].depth);
    auto found = ctx.clade_by_key.find(node.taxa);
    if (found == ctx.clade_by_key.end())
      throw std::runtime_error(
          "tension map: witness internal taxon set has no grammar clade (" +
          std::to_string(node.taxa.size()) + " taxa)");
    node.clade = found->second;
  }
  if (nodes.empty() || nodes.front().clade != ctx.grammar.root_clade)
    throw std::runtime_error("tension map: witness root clade mismatch");
  return nodes;
}

// Production used by a witness internal node; returns (pid, swapped) where
// swapped means the tree's [left, right] edge order is reversed relative to
// grammar.productions[pid].children.
std::pair<production_id, bool> witness_production(
    grammar_ctx const& ctx, std::vector<witness_node> const& nodes,
    std::uint32_t i) {
  auto const& node = nodes[i];
  auto lc = nodes[node.children[0]].clade;
  auto rc = nodes[node.children[1]].clade;
  for (auto pid : ctx.grammar.productions_by_parent[node.clade]) {
    auto const& prod = ctx.grammar.productions[pid];
    if (prod.children[0] == lc && prod.children[1] == rc) return {pid, false};
    if (prod.children[0] == rc && prod.children[1] == lc) return {pid, true};
  }
  throw std::runtime_error("tension map: witness production not in grammar");
}

std::uint64_t eval_row(
    chart_cost const* row,
    std::array<std::uint32_t, nuc_state_count> const& counts) {
  std::uint64_t total = 0;
  for (std::uint8_t ref = 0; ref < nuc_state_count; ++ref) {
    auto count = counts[ref];
    if (count == 0) continue;
    chart_cost best = chart_inf;
    for (std::uint8_t s = 0; s < nuc_state_count; ++s)
      best = std::min(
          best, parsimony_chart_detail::saturated_add(
                    row[s],
                    parsimony_chart_detail::transition_cost(ref, s)));
    total += static_cast<std::uint64_t>(count) * best;
  }
  return total;
}

// Unpruned per-class chart DP over the whole grammar.
std::vector<std::vector<chart_cost>> class_chart_minima(grammar_ctx const& ctx) {
  auto const& g = ctx.grammar;
  std::vector<std::vector<chart_cost>> mrow(g.clades.size());
  std::vector<clade_id> order(g.clades.size());
  std::iota(order.begin(), order.end(), clade_id{0});
  std::stable_sort(order.begin(), order.end(), [&](clade_id lhs, clade_id rhs) {
    auto l = g.clades[lhs].taxa.size(), r = g.clades[rhs].taxa.size();
    if (l != r) return l < r;
    return lhs < rhs;
  });
  for (auto clade : order) {
    auto n = ctx.classes[clade].class_count;
    if (g.clades[clade].taxa.size() == 1) {
      mrow[clade].assign(n * nuc_state_count, chart_inf);
      for (std::uint32_t c = 0; c < n; ++c)
        mrow[clade][c * nuc_state_count +
                    ctx.classes[clade].leaf_state_by_class[c]] = 0;
      continue;
    }
    mrow[clade].assign(n * nuc_state_count, chart_inf);
    for (auto pid : g.productions_by_parent[clade]) {
      auto const& map = ctx.production_maps[pid].children_by_class;
      auto const& prod = g.productions[pid];
      auto const& lrows = mrow[prod.children[0]];
      auto const& rrows = mrow[prod.children[1]];
      for (std::uint32_t c = 0; c < n; ++c) {
        auto [lc, rc] = map[c];
        chart_cost combined[nuc_state_count];
        combine_class_row(lrows.data() + lc * nuc_state_count,
                          rrows.data() + rc * nuc_state_count, combined);
        auto* out = mrow[clade].data() + c * nuc_state_count;
        for (std::uint8_t s = 0; s < nuc_state_count; ++s)
          out[s] = std::min(out[s], combined[s]);
      }
    }
  }
  return mrow;
}

struct tree_dp_result {
  std::vector<std::vector<chart_cost>> rows;  // per node, class-major
  std::uint64_t total = 0;
};

tree_dp_result witness_tree_dp(grammar_ctx const& ctx,
                               std::vector<witness_node> const& nodes,
                               bool compute_total) {
  tree_dp_result res;
  res.rows.assign(nodes.size(), {});
  for (std::size_t i = nodes.size(); i-- > 0;) {
    auto const& node = nodes[i];
    auto n = ctx.classes[node.clade].class_count;
    if (node.children.empty()) {
      res.rows[i].assign(n * nuc_state_count, chart_inf);
      for (std::uint32_t c = 0; c < n; ++c)
        res.rows[i][c * nuc_state_count +
                    ctx.classes[node.clade].leaf_state_by_class[c]] = 0;
      continue;
    }
    auto [pid, swapped] =
        witness_production(ctx, nodes, static_cast<std::uint32_t>(i));
    auto const& map = ctx.production_maps[pid].children_by_class;
    res.rows[i].assign(n * nuc_state_count, chart_inf);
    auto left = swapped ? node.children[1] : node.children[0];
    auto right = swapped ? node.children[0] : node.children[1];
    auto const& lrows = res.rows[left];
    auto const& rrows = res.rows[right];
    for (std::uint32_t c = 0; c < n; ++c) {
      auto [lc, rc] = map[c];
      combine_class_row(lrows.data() + lc * nuc_state_count,
                        rrows.data() + rc * nuc_state_count,
                        res.rows[i].data() + c * nuc_state_count);
    }
  }
  if (compute_total) {
    std::uint64_t total = ctx.invariant_offset;
    auto const& root_classes = ctx.classes[nodes.front().clade];
    for (std::size_t a = 0; a < ctx.active.size(); ++a) {
      auto cls = root_classes.class_of_pattern[a];
      total += eval_row(res.rows[0].data() + cls * nuc_state_count,
                        ctx.active[a].reference_state_counts);
    }
    res.total = total;
  }
  return res;
}

struct closure_ctx {
  grammar_ctx* second = nullptr;
  std::string second_witness_path;
  std::uint64_t expected_total2 = 0;
};

void run_tension(std::string const& dag_path, std::string const& witness_path,
                 std::uint64_t expected_total, closure_ctx closure) {
  auto ctx = prepare(dag_path);
  std::string witness_reference;
  auto nodes = load_witness(witness_path, ctx, &witness_reference);
  std::cerr << "tension: grammar clades=" << ctx.grammar.clades.size()
            << " productions=" << ctx.grammar.productions.size()
            << " active_patterns=" << ctx.active.size()
            << " witness_nodes=" << nodes.size()
            << " invariant_offset=" << ctx.invariant_offset << "\n";

  auto mrow = class_chart_minima(ctx);
  auto fdp = witness_tree_dp(ctx, nodes, /*compute_total=*/true);
  std::cerr << "tension: witness total score = " << fdp.total << " (expected "
            << expected_total << ")"
            << (fdp.total == expected_total ? " OK" : " MISMATCH") << "\n";
  if (fdp.total != expected_total)
    throw std::runtime_error("tension map: witness score check failed");

  std::uint64_t lb = ctx.invariant_offset;
  {
    auto const& root_classes = ctx.classes[ctx.grammar.root_clade];
    for (std::size_t a = 0; a < ctx.active.size(); ++a) {
      auto cls = root_classes.class_of_pattern[a];
      lb += eval_row(
          mrow[ctx.grammar.root_clade].data() + cls * nuc_state_count,
          ctx.active[a].reference_state_counts);
    }
  }
  std::cerr << "tension: composite inside LB = " << lb << " gap = "
            << (fdp.total - lb) << "\n";

  // Preorder (root first).  WRIC_REVERSE_SIBLINGS flips the sibling
  // traversal order, changing the order of restrictions in the clamp chain
  // (individual marginals shift; the telescoped total is invariant).
  bool reverse_siblings = false;
  if (const char* rs = std::getenv("WRIC_REVERSE_SIBLINGS"))
    reverse_siblings = std::atoi(rs) != 0;
  std::vector<std::uint32_t> preorder;
  {
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
      auto i = stack.back();
      stack.pop_back();
      preorder.push_back(i);
      if (reverse_siblings)
        for (auto c : nodes[i].children) stack.push_back(c);
      else
        for (auto it = nodes[i].children.rbegin();
             it != nodes[i].children.rend(); ++it)
          stack.push_back(*it);
    }
  }
  if (reverse_siblings)
    std::cerr << "tension: sibling traversal REVERSED (attribution "
                 "stability probe)\n";

  std::size_t const P = ctx.active.size();
  std::size_t const N = nodes.size();
  std::vector<std::uint32_t> class_at(P * N, 0);
  for (std::size_t a = 0; a < P; ++a) {
    class_at[a * N] =
        ctx.classes[ctx.grammar.root_clade].class_of_pattern[a];
    for (auto i : preorder) {
      auto const& node = nodes[i];
      if (node.children.empty()) continue;
      auto [pid, swapped] =
          witness_production(ctx, nodes, i);
      auto const& map = ctx.production_maps[pid].children_by_class;
      auto [lc, rc] = map[class_at[a * N + i]];
      if (swapped) std::swap(lc, rc);
      class_at[a * N + node.children[0]] = lc;
      class_at[a * N + node.children[1]] = rc;
    }
  }

  // Exact clamp-chain per active pattern.
  std::vector<std::int64_t> tau_point(N, 0);
  std::vector<std::uint32_t> patterns_pos(N, 0);
  std::unordered_map<std::uint64_t, std::int64_t> tau_by_class;

  std::vector<chart_cost> cur(N * nuc_state_count);
  std::vector<chart_cost> tmp(nuc_state_count);
  for (std::size_t a = 0; a < P; ++a) {
    auto const& counts = ctx.active[a].reference_state_counts;
    for (std::size_t i = 0; i < N; ++i) {
      auto cls = class_at[a * N + i];
      std::copy_n(mrow[nodes[i].clade].data() + cls * nuc_state_count,
                  nuc_state_count, cur.data() + i * nuc_state_count);
    }
    std::uint64_t v_prev = eval_row(cur.data(), counts);
    for (auto i : preorder) {
      auto const& node = nodes[i];
      if (node.children.empty()) continue;
      auto l = node.children[0], r = node.children[1];
      combine_class_row(cur.data() + l * nuc_state_count,
                        cur.data() + r * nuc_state_count, tmp.data());
      bool changed = false;
      for (std::uint8_t s = 0; s < nuc_state_count; ++s)
        if (tmp[s] != cur[i * nuc_state_count + s]) {
          changed = true;
          break;
        }
      if (!changed) continue;
      std::copy_n(tmp.data(), nuc_state_count,
                  cur.data() + i * nuc_state_count);
      auto anc = node.parent;
      while (anc >= 0) {
        auto an = static_cast<std::size_t>(anc);
        auto al = nodes[an].children[0], ar = nodes[an].children[1];
        combine_class_row(cur.data() + al * nuc_state_count,
                          cur.data() + ar * nuc_state_count, tmp.data());
        bool a_changed = false;
        for (std::uint8_t s = 0; s < nuc_state_count; ++s)
          if (tmp[s] != cur[an * nuc_state_count + s]) {
            a_changed = true;
            break;
          }
        if (!a_changed) break;
        std::copy_n(tmp.data(), nuc_state_count,
                    cur.data() + an * nuc_state_count);
        anc = nodes[an].parent;
      }
      std::uint64_t v_now = eval_row(cur.data(), counts);
      if (v_now < v_prev)
        throw std::runtime_error("tension map: negative marginal");
      auto delta = static_cast<std::int64_t>(v_now - v_prev);
      v_prev = v_now;
      tau_point[i] += delta;
      if (delta > 0) patterns_pos[i] += 1;
      tau_by_class[(static_cast<std::uint64_t>(i) << 32) |
                   class_at[a * N + i]] += delta;
    }
    auto root_cls = class_at[a * N];
    for (std::uint8_t s = 0; s < nuc_state_count; ++s)
      if (cur[s] != fdp.rows[0][root_cls * nuc_state_count + s])
        throw std::runtime_error("tension map: chain did not reach witness row");
  }

  std::vector<std::int64_t> tau_prefix(N, 0);
  for (std::size_t i = N; i-- > 0;) {
    tau_prefix[i] = tau_point[i];
    for (auto c : nodes[i].children) tau_prefix[i] += tau_prefix[c];
  }

  // Env-gated target/refseq emission for the directed-injection loop.
  if (const char* tpath = std::getenv("WRIC_TARGETS_OUT")) {
    int topk = 8;
    if (const char* k = std::getenv("WRIC_TARGETS_TOPK"))
      topk = std::atoi(k);
    std::vector<std::uint32_t> by_tau;
    for (std::size_t i = 0; i < N; ++i)
      if (tau_point[i] > 0) by_tau.push_back(static_cast<std::uint32_t>(i));
    if (const char* nsel = std::getenv("WRIC_TARGETS_NODES")) {
      by_tau.clear();
      std::stringstream ss(nsel);
      std::string item;
      while (std::getline(ss, item, ','))
        if (!item.empty())
          by_tau.push_back(static_cast<std::uint32_t>(std::atoi(item.c_str())));
    }
    std::stable_sort(by_tau.begin(), by_tau.end(), [&](auto a, auto b) {
      if (tau_point[a] != tau_point[b]) return tau_point[a] > tau_point[b];
      return a < b;
    });
    std::FILE* out = std::fopen(tpath, "w");
    if (!out)
      throw std::runtime_error("tension map: cannot write targets file");
    std::fprintf(out, "clade\ttau\tsubtense\tntaxa\tids\n");
    for (std::size_t r = 0; r < by_tau.size() && r < (std::size_t)topk; ++r) {
      auto i = by_tau[r];
      auto clade = nodes[i].clade;
      std::fprintf(out, "%u\t%lld\t%lld\t%zu\t", clade,
                   (long long)tau_point[i], (long long)tau_prefix[i],
                   nodes[i].taxa.size());
      for (std::size_t t = 0; t < ctx.grammar.clades[clade].taxa.size(); ++t) {
        if (t) std::fputc(',', out);
        std::fprintf(out, "%s",
                     ctx.grammar.taxa.id_to_sample_id[
                         ctx.grammar.clades[clade].taxa[t]]
                         .c_str());
      }
      std::fputc('\n', out);
    }
    std::fclose(out);
    std::cerr << "tension: wrote " << (by_tau.size() < (std::size_t)topk
                                             ? by_tau.size()
                                             : (std::size_t)topk)
              << " targets to " << tpath << "\n";
  }
  if (const char* rpath = std::getenv("WRIC_REFSEQ_OUT")) {
    std::FILE* out = std::fopen(rpath, "w");
    if (!out)
      throw std::runtime_error("tension map: cannot write refseq file");
    std::fprintf(out, "%s\n", witness_reference.c_str());
    std::fclose(out);
  }
  std::int64_t chain_total = 0;
  for (std::size_t i = 0; i < N; ++i) chain_total += tau_point[i];
  std::cerr << "tension: chain total = " << chain_total << " (gap "
            << (fdp.total - lb) << ")"
            << (chain_total == static_cast<std::int64_t>(fdp.total - lb)
                    ? " OK"
                    : " MISMATCH")
            << "\n";

  std::vector<std::int64_t> regret_prefix(N, 0);
  for (std::size_t i = 0; i < N; ++i) {
    std::int64_t acc = 0;
    auto clade = nodes[i].clade;
    auto const& cls_table = ctx.classes[clade];
    for (std::size_t a = 0; a < P; ++a) {
      auto cls = class_at[a * N + i];
      acc += static_cast<std::int64_t>(
                 eval_row(fdp.rows[i].data() + cls * nuc_state_count,
                          ctx.active[a].reference_state_counts)) -
             static_cast<std::int64_t>(
                 eval_row(mrow[clade].data() + cls * nuc_state_count,
                          ctx.active[a].reference_state_counts));
    }
    regret_prefix[i] = acc;
  }

  std::cout << "node\tclade\tntaxa\tdepth\ttau_point\ttau_prefix\tregret_prefix\t"
               "patterns_pos\tclasses_here\n";
  for (std::size_t i = 0; i < N; ++i)
    std::cout << i << "\t" << nodes[i].clade << "\t" << nodes[i].taxa.size()
              << "\t" << nodes[i].depth << "\t" << tau_point[i] << "\t"
              << tau_prefix[i] << "\t" << regret_prefix[i] << "\t"
              << patterns_pos[i] << "\t"
              << ctx.classes[nodes[i].clade].class_count << "\n";

  std::cout << "\n# per-class tension (node, clade, class, class weight, tau)\n";
  for (auto const& [key, val] : tau_by_class) {
    if (val <= 0) continue;
    auto node = static_cast<std::uint32_t>(key >> 32);
    auto cls = static_cast<std::uint32_t>(key & 0xffffffffu);
    std::cout << "class_tau\t" << node << "\t" << nodes[node].clade << "\t"
              << cls << "\t"
              << ctx.classes[nodes[node].clade].weight_by_class[cls] << "\t"
              << val << "\n";
  }

  if (const char* spath = std::getenv("WRIC_CLADE_STATS_OUT")) {
    // Per witness node: distinct clades/productions reachable in the grammar
    // below its clade (BFS over productions_by_parent).  alt_prods = extra
    // productions beyond a single binary tree on the same taxa; tree_like =
    // no alternatives at all below (the strongest saturation form).
    std::FILE* out = std::fopen(spath, "w");
    if (!out)
      throw std::runtime_error("tension map: cannot write clade stats");
    std::fprintf(out,
                 "node\tclade\tntaxa\ttau_point\ttau_prefix\tclades_below\t"
                 "prods_below\talt_prods\ttree_like\n");
    for (std::size_t i = 0; i < N; ++i) {
      auto start = nodes[i].clade;
      std::vector<bool> seenc(ctx.grammar.clades.size(), false);
      std::vector<bool> seenp(ctx.grammar.productions.size(), false);
      std::vector<clade_id> queue{start};
      seenc[start] = true;
      std::size_t ncl = 1, npr = 0;
      for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        for (auto pid : ctx.grammar.productions_by_parent[queue[qi]]) {
          if (seenp[pid]) continue;
          seenp[pid] = true;
          ++npr;
          for (auto ch : ctx.grammar.productions[pid].children)
            if (!seenc[ch]) {
              seenc[ch] = true;
              ++ncl;
              queue.push_back(ch);
            }
        }
      }
      auto k = nodes[i].taxa.size();
      std::int64_t alt = static_cast<std::int64_t>(npr) -
                         static_cast<std::int64_t>(k) + 1;
      std::fprintf(out, "%zu\t%u\t%zu\t%lld\t%lld\t%zu\t%zu\t%lld\t%d\n", i,
                   start, k, (long long)tau_point[i],
                   (long long)tau_prefix[i], ncl, npr, (long long)alt,
                   alt == 0 ? 1 : 0);
    }
    std::fclose(out);
    std::cerr << "tension: clade stats written\n";
  }

  if (!closure.second) return;

  // ---- closure mode ----
  auto& ectx = *closure.second;
  auto enodes = load_witness(closure.second_witness_path, ectx);
  auto efdp = witness_tree_dp(ectx, enodes, /*compute_total=*/true);
  std::cerr << "closure: enriched grammar clades=" << ectx.grammar.clades.size()
            << " productions=" << ectx.grammar.productions.size()
            << " witness_nodes=" << enodes.size() << "\n";
  std::cerr << "closure: enriched witness total = " << efdp.total
            << " (expected " << closure.expected_total2 << ")"
            << (efdp.total == closure.expected_total2 ? " OK" : " MISMATCH")
            << "\n";
  if (efdp.total != closure.expected_total2)
    throw std::runtime_error("closure: enriched score check failed");
  std::cerr << "closure: total closure = " << (fdp.total - efdp.total) << "\n";

  auto fsum = [&](grammar_ctx const& c, std::vector<witness_node> const& ns,
                  tree_dp_result const& dp) {
    std::unordered_map<CanonKey, std::int64_t, canon_key_hash> out;
    for (std::size_t i = 0; i < ns.size(); ++i) {
      std::int64_t acc = 0;
      auto const& cls_table = c.classes[ns[i].clade];
      for (std::size_t a = 0; a < c.active.size(); ++a) {
        auto cls = cls_table.class_of_pattern[a];
        acc += static_cast<std::int64_t>(
            eval_row(dp.rows[i].data() + cls * nuc_state_count,
                     c.active[a].reference_state_counts));
      }
      out.emplace(c.clade_canon[ns[i].clade], acc);
    }
    return out;
  };
  auto fun = fsum(ctx, nodes, fdp);
  auto fen = fsum(ectx, enodes, efdp);

  // Paid sites: enriched-witness productions absent from the union grammar.
  std::cout << "\n# paid sites (node, clade, ntaxa, clade_new, graft_union_clade)\n";
  for (std::size_t i = 0; i < enodes.size(); ++i) {
    auto const& node = enodes[i];
    if (node.children.empty()) continue;
    auto in_union = ctx.clade_by_key.find(ectx.clade_canon[node.clade]);
    bool clade_new = in_union == ctx.clade_by_key.end();
    bool prod_new = clade_new;
    if (!clade_new) {
      auto lkey = ectx.clade_canon[enodes[node.children[0]].clade];
      auto rkey = ectx.clade_canon[enodes[node.children[1]].clade];
      bool found = false;
      for (auto pid : ctx.grammar.productions_by_parent[in_union->second]) {
        auto const& prod = ctx.grammar.productions[pid];
        auto pl = ctx.clade_canon[prod.children[0]];
        auto pr = ctx.clade_canon[prod.children[1]];
        if ((pl == lkey && pr == rkey) || (pl == rkey && pr == lkey)) {
          found = true;
          break;
        }
      }
      prod_new = !found;
    }
    if (!prod_new) continue;
    std::int64_t anc = node.parent;
    clade_id graft = no_clade;
    while (anc >= 0) {
      auto an = static_cast<std::size_t>(anc);
      auto it = ctx.clade_by_key.find(ectx.clade_canon[enodes[an].clade]);
      if (it != ctx.clade_by_key.end()) {
        graft = it->second;
        break;
      }
      anc = enodes[an].parent;
    }
    std::cout << "paid\t" << i << "\t" << node.clade << "\t"
              << node.taxa.size() << "\t" << (clade_new ? 1 : 0) << "\t"
              << graft << "\n";
  }

  // dF per shared clade + local attribution on the enriched tree.  A node
  // emits a local term iff it is shared, internal, and its parent emits
  // (root always emits); flow through non-emitting chains lands exactly once
  // at the nearest emitting ancestor, so emitted terms sum to the root
  // closure exactly.
  std::vector<std::int64_t> df(enodes.size(), 0);
  std::vector<char> shared(enodes.size(), 0);
  std::vector<char> emits(enodes.size(), 0);
  std::vector<clade_id> union_clade_of(enodes.size(), no_clade);
  for (std::size_t i = 0; i < enodes.size(); ++i) {
    auto it = ctx.clade_by_key.find(ectx.clade_canon[enodes[i].clade]);
    if (it == ctx.clade_by_key.end()) continue;
    auto fu = fun.find(ctx.clade_canon[it->second]);
    auto fe = fen.find(ectx.clade_canon[enodes[i].clade]);
    if (fu == fun.end() || fe == fen.end()) continue;
    df[i] = fu->second - fe->second;
    shared[i] = 1;
    union_clade_of[i] = it->second;
  }
  for (std::size_t i = 0; i < enodes.size(); ++i)
    emits[i] = shared[i] && !enodes[i].children.empty() &&
               (enodes[i].parent < 0 || emits[static_cast<std::size_t>(
                                             enodes[i].parent)]);
  std::cout << "\n# dF per shared internal node (union clade, node, ntaxa, dF, emits, local)\n";
  std::int64_t local_sum = 0;
  for (std::size_t i = 0; i < enodes.size(); ++i) {
    if (!shared[i] || enodes[i].children.empty()) continue;
    std::int64_t child_sub = 0;
    for (auto c : enodes[i].children)
      if (emits[c]) child_sub += df[c];
    auto local = emits[i] ? df[i] - child_sub : 0;
    if (emits[i]) local_sum += local;
    std::cout << "df\t" << union_clade_of[i] << "\t" << i << "\t"
              << enodes[i].taxa.size() << "\t" << df[i] << "\t"
              << static_cast<int>(emits[i]) << "\t" << local << "\n";
  }
  std::cout << "\n# shared-child edges of shared internal nodes (parent node, child node)\n";
  for (std::size_t i = 0; i < enodes.size(); ++i) {
    if (!shared[i] || enodes[i].children.empty()) continue;
    for (auto c : enodes[i].children)
      if (shared[c]) std::cout << "eedge\t" << i << "\t" << c << "\n";
  }
  std::cerr << "closure: clean local attribution sum = " << local_sum
            << " (root closure " << (fdp.total - efdp.total) << ")\n";

  // Enriched witness tree structure for downstream joins.
  std::cout << "\n# enriched witness tree (node, clade, parent, ntaxa, shared)\n";
  for (std::size_t i = 0; i < enodes.size(); ++i)
    std::cout << "enode\t" << i << "\t" << enodes[i].clade << "\t"
              << (enodes[i].parent < 0 ? -1 : enodes[i].parent) << "\t"
              << enodes[i].taxa.size() << "\t"
              << static_cast<int>(shared[i]) << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: %s tension <dag.pb.gz> <witness.pb.gz> <expected_total>\n"
                 "       %s closure <union_dag> <union_witness> <union_total> "
                 "<enriched_dag> <enriched_witness> <enriched_total>\n",
                 argv[0], argv[0]);
    return 2;
  }
  std::string mode = argv[1];
  try {
    if (mode == "tension" && argc == 5) {
      run_tension(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10), {});
    } else if (mode == "closure" && argc == 8) {
      grammar_ctx enriched = prepare(argv[5]);
      closure_ctx closure;
      closure.second = &enriched;
      closure.second_witness_path = argv[6];
      closure.expected_total2 = std::strtoull(argv[7], nullptr, 10);
      run_tension(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10),
                  closure);
    } else {
      std::fprintf(stderr, "bad arguments for mode %s\n", mode.c_str());
      return 2;
    }
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
