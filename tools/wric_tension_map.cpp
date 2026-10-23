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

#include "wric_subtree_common.hpp"

using wsub::load_tree;
using wsub::load_tree_any;
using wsub::read_ids;
using wsub::tnode;

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
    if (std::getenv("WRIC_WITNESS_DP_DEBUG")) {
      std::uint64_t again = ctx.invariant_offset;
      for (std::size_t a = 0; a < ctx.active.size(); ++a) {
        auto cls = root_classes.class_of_pattern[a];
        again += eval_row(res.rows[0].data() + cls * nuc_state_count,
                          ctx.active[a].reference_state_counts);
      }
      std::fprintf(stderr,
                   "witness_dp_debug: total=%llu again=%llu rows0=%zu "
                   "ncls=%zu nactive=%zu\n",
                   static_cast<unsigned long long>(total),
                   static_cast<unsigned long long>(again),
                   res.rows[0].size(), root_classes.class_count,
                   ctx.active.size());
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

// ---- v1 M0: response export (doc/WRIC-V1-KERNEL-PLAN.md) ----
// For every ACTIVE site pattern of the GLOBAL grammar, emit the part
// grammar's root response row m(a) = min cost inside the part over parses
// whose root CG has state a at that site (free labels, chart convention).
// M1's scaffold solver consumes these rows plus per-pattern reference
// counts to compute the exact coupled optimum.  Part-uniform restrictions
// are handled explicitly (uniform sites live in the part's invariant
// offset, not in active classes): m(a) = [a != u].
struct response_global_ctx {
  grammar_ctx ctx;
  std::unordered_map<std::string, std::uint32_t> gtaxon_of_sample;
};

response_global_ctx prepare_response_global(std::string const& path) {
  response_global_ctx g;
  g.ctx = prepare(path);
  for (std::uint32_t t = 0; t < g.ctx.grammar.taxa.id_to_sample_id.size(); ++t)
    g.gtaxon_of_sample.emplace(g.ctx.grammar.taxa.id_to_sample_id[t], t);
  return g;
}

void run_response_part(response_global_ctx const& g,
                        std::string const& part_path,
                        std::string const& out_path) {
  auto ctx = prepare(part_path);
  std::cerr << "response: " << part_path << " clades="
            << ctx.grammar.clades.size() << " active=" << ctx.active.size()
            << " invariant_offset=" << ctx.invariant_offset << "\n";
  auto mrow = class_chart_minima(ctx);
  auto const& root_classes = ctx.classes[ctx.grammar.root_clade];
  auto const& root_row = mrow[ctx.grammar.root_clade];

  // Part taxon ids sorted by GLOBAL taxon index (shared key order).
  std::vector<std::uint32_t> ptax(ctx.grammar.taxa.id_to_sample_id.size());
  for (std::uint32_t t = 0; t < ptax.size(); ++t) {
    auto it = g.gtaxon_of_sample.find(ctx.grammar.taxa.id_to_sample_id[t]);
    if (it == g.gtaxon_of_sample.end())
      throw std::runtime_error("response: part taxon absent from global");
    ptax[t] = it->second;
  }
  std::vector<std::uint32_t> order(ptax.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [&](std::uint32_t a, std::uint32_t b) { return ptax[a] < ptax[b]; });

  // Dict: canonical restriction key (states over `order`) -> root class.
  std::unordered_map<CanonKey, std::uint32_t, canon_key_hash> class_by_key;
  for (std::size_t a = 0; a < ctx.active.size(); ++a) {
    CanonKey key(order.size());
    for (std::size_t i = 0; i < order.size(); ++i)
      key[i] = ctx.active[a].state_by_taxon[order[i]];
    class_by_key.emplace(key, root_classes.class_of_pattern[a]);
  }

  // Part's composite LB: sum over patterns of the per-pattern-independent
  // root-state minima (unpruned inside DP).  This is a LOWER BOUND on the
  // part's true span optimum (coupling across patterns is not resolved
  // here); the certified optimum is >= this, with the gap being the part's
  // internal coupling gap.  Used as the M0 acceptance check together with
  // the witness score check (tension mode).
  std::uint64_t part_lb = ctx.invariant_offset;
  for (std::size_t a = 0; a < ctx.active.size(); ++a) {
    auto cls = root_classes.class_of_pattern[a];
    part_lb += eval_row(root_row.data() + cls * nuc_state_count,
                        ctx.active[a].reference_state_counts);
  }

  std::FILE* out = std::fopen(out_path.c_str(), "w");
  if (!out) throw std::runtime_error("response: cannot open " + out_path);
  std::fprintf(out, "# wric_response v1-M0\n");
  std::fprintf(out, "# part %s\n", part_path.c_str());
  std::fprintf(out, "# part_taxa %zu\n", ptax.size());
  std::fprintf(out, "# part_lb %llu\n",
               static_cast<unsigned long long>(part_lb));
  std::fprintf(out, "# global_active %zu global_invariant_offset %llu\n",
               g.ctx.active.size(),
               static_cast<unsigned long long>(g.ctx.invariant_offset));
  std::fprintf(out, "# columns gidx weight refA refC refG refT mA mC mG mT\n");
  std::uint64_t uniform_rows = 0, key_rows = 0;
  for (std::size_t gi = 0; gi < g.ctx.active.size(); ++gi) {
    auto const& act = g.ctx.active[gi];
    bool uniform = true;
    std::uint8_t u = act.state_by_taxon[ptax[order[0]]];
    for (std::size_t i = 1; i < order.size(); ++i)
      if (act.state_by_taxon[ptax[order[i]]] != u) {
        uniform = false;
        break;
      }
    std::uint32_t m[nuc_state_count];
    if (uniform) {
      for (std::uint8_t s = 0; s < nuc_state_count; ++s)
        m[s] = (s == u) ? 0 : 1;
      ++uniform_rows;
    } else {
      CanonKey key(order.size());
      for (std::size_t i = 0; i < order.size(); ++i)
        key[i] = act.state_by_taxon[ptax[order[i]]];
      auto it = class_by_key.find(key);
      if (it == class_by_key.end()) {
        if (std::getenv("WRIC_RESPONSE_DEBUG")) {
          // Nearest part pattern by Hamming distance over the restriction.
          std::size_t best_d = order.size() + 1;
          const CanonKey* best_key = nullptr;
          for (auto const& [pk, pc] : class_by_key) {
            std::size_t d = 0;
            for (std::size_t i = 0; i < key.size(); ++i)
              if (key[i] != pk[i]) ++d;
            if (d < best_d) {
              best_d = d;
              best_key = &pk;
            }
          }
          std::fprintf(stderr,
                       "response-debug: pattern %zu key-size %zu nearest "
                       "hamming %zu",
                       gi, key.size(), best_d);
          if (best_key) {
            std::fprintf(stderr, " mismatches:");
            std::size_t shown = 0;
            for (std::size_t i = 0; i < key.size() && shown < 20; ++i)
              if (key[i] != (*best_key)[i]) {
                std::fprintf(stderr, " %s:g%d!=p%d",
                             g.ctx.grammar.taxa.id_to_sample_id
                                            [ptax[order[i]]]
                                                .c_str(),
                             key[i], (*best_key)[i]);
                ++shown;
              }
          }
          std::fprintf(stderr, "\n");
        }
        throw std::runtime_error("response: global pattern " +
                                 std::to_string(gi) +
                                 " has no matching part class");
      }
      for (std::uint8_t s = 0; s < nuc_state_count; ++s)
        m[s] = static_cast<std::uint32_t>(
            root_row[it->second * nuc_state_count + s]);
      ++key_rows;
    }
    std::fprintf(out, "%zu %u %u %u %u %u %u %u %u %u\n", gi, act.weight,
                 act.reference_state_counts[0], act.reference_state_counts[1],
                 act.reference_state_counts[2], act.reference_state_counts[3],
                 m[0], m[1], m[2], m[3]);
  }
  std::fclose(out);
  std::cerr << "response: " << out_path << " rows=" << g.ctx.active.size()
            << " (uniform=" << uniform_rows << " keyed=" << key_rows
            << ") part_lb=" << part_lb << "\n";
}

void run_response(std::string const& global_path, std::string const& outdir,
                  std::vector<std::string> const& parts) {
  auto g = prepare_response_global(global_path);
  std::cerr << "response: global clades=" << g.ctx.grammar.clades.size()
            << " active=" << g.ctx.active.size() << "\n";
  for (auto const& p : parts) {
    auto slash = p.find_last_of('/');
    auto stem = p.substr(slash + 1);
    auto dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    run_response_part(g, p, outdir + "/" + stem + ".resp");
  }
}

// ---- v1 M1: scaffold-fixed coupled solve (doc/WRIC-V1-KERNEL-PLAN.md) ----
// Collapses each part clade of the binary scaffold comb to a response
// pseudo-leaf and runs a per-pattern Sankoff DP over the remaining fixed
// scaffold.  With grammar response rows this is the fold's composite LOWER
// BOUND with exact scaffold coupling (rows are per-pattern minima over part
// topologies); with witness-tree rows it reproduces the joined tree's exact
// free-label score (validation: must equal the greedy chain's 59,821).

struct scaffold_node {
  bool leaf = false;
  bool pseudo = false;
  int part = -1;
  std::uint32_t taxon = no_idx;  // real leaf: global taxon id
  std::vector<std::uint32_t> children;
};

std::uint32_t flatten_scaffold_node(tnode const& node,
                                    std::vector<scaffold_node>& out,
                                    std::unordered_map<std::string, std::uint32_t>& taxon_of_sid) {
  scaffold_node sn;
  if (node.leaf) {
    sn.leaf = true;
    if (node.sid.rfind("__PART_", 0) == 0) {
      sn.pseudo = true;
      sn.part = std::atoi(node.sid.c_str() + 7);
    } else {
      auto it = taxon_of_sid.find(node.sid);
      if (it == taxon_of_sid.end())
        throw std::runtime_error("scaffold: leaf not in global: " + node.sid);
      sn.taxon = it->second;
    }
    out.push_back(sn);
    return static_cast<std::uint32_t>(out.size() - 1);
  }
  sn.children.reserve(node.children.size());
  for (auto const& c : node.children)
    sn.children.push_back(flatten_scaffold_node(c, out, taxon_of_sid));
  out.push_back(sn);
  return static_cast<std::uint32_t>(out.size() - 1);
}

tnode collapse_parts(tnode node,
                     std::vector<std::vector<std::string>> const& parts) {
  if (!node.leaf) {
    for (std::size_t j = 0; j < parts.size(); ++j) {
      if (node.taxa.size() != parts[j].size()) continue;
      if (std::equal(node.taxa.begin(), node.taxa.end(), parts[j].begin())) {
        tnode pseudo;
        pseudo.leaf = true;
        pseudo.sid = "__PART_" + std::to_string(j) + "__";
        pseudo.taxa = {pseudo.sid};
        return pseudo;
      }
    }
    for (auto& c : node.children) c = collapse_parts(std::move(c), parts);
  }
  return node;
}

struct resp_rows {
  std::vector<std::array<std::uint32_t, nuc_state_count>> m;  // per pattern
};

resp_rows load_resp_rows(std::string const& path,
                         grammar_ctx const& gctx) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) throw std::runtime_error("scaffold: cannot open " + path);
  resp_rows rows;
  rows.m.resize(gctx.active.size());
  char line[256];
  std::size_t g = 0;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    std::uint64_t gidx, w, r0, r1, r2, r3, m0, m1, m2, m3;
    if (std::sscanf(line, "%llu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &gidx,
                    &w, &r0, &r1, &r2, &r3, &m0, &m1, &m2, &m3) != 10)
      throw std::runtime_error("scaffold: bad resp row: " + std::string(line));
    if (gidx != g)
      throw std::runtime_error("scaffold: resp row out of order");
    if (w != gctx.active[g].weight)
      throw std::runtime_error("scaffold: resp weight mismatch");
    rows.m[g] = {static_cast<std::uint32_t>(m0), static_cast<std::uint32_t>(m1),
                 static_cast<std::uint32_t>(m2), static_cast<std::uint32_t>(m3)};
    ++g;
  }
  std::fclose(f);
  if (g != gctx.active.size())
    throw std::runtime_error("scaffold: resp row count mismatch");
  return rows;
}

resp_rows witness_rows(std::string const& wit_path,
                       grammar_ctx const& gctx,
                       std::unordered_map<std::string, std::uint32_t> const& taxon_of_sid) {
  std::string ref_ignored;
  auto root = load_tree_any(wit_path, &ref_ignored, nullptr);
  // flatten witness postorder
  struct wnode {
    bool leaf = false;
    std::uint32_t taxon = no_idx;
    std::vector<std::uint32_t> children;
  };
  std::vector<wnode> nodes;
  std::function<std::uint32_t(tnode const&)> fl = [&](tnode const& n) {
    wnode wn;
    if (n.leaf) {
      wn.leaf = true;
      auto it = taxon_of_sid.find(n.sid);
      if (it == taxon_of_sid.end())
        throw std::runtime_error("scaffold: witness leaf not in global");
      wn.taxon = it->second;
      nodes.push_back(wn);
      return static_cast<std::uint32_t>(nodes.size() - 1);
    }
    for (auto const& c : n.children)
      wn.children.push_back(fl(c));
    nodes.push_back(wn);
    return static_cast<std::uint32_t>(nodes.size() - 1);
  };
  fl(root);
  constexpr std::uint32_t INF_COST = 1u << 30;
  auto const& active = gctx.active;
  resp_rows rows;
  rows.m.resize(active.size());
#pragma omp parallel for schedule(dynamic, 8)
  for (std::size_t g = 0; g < active.size(); ++g) {
    std::vector<std::array<std::uint32_t, nuc_state_count>> vec(nodes.size());
    auto const& states = active[g].state_by_taxon;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      auto const& n = nodes[i];
      if (n.leaf) {
        auto& v = vec[i];
        for (std::uint8_t s = 0; s < nuc_state_count; ++s)
          v[s] = (s == states[n.taxon]) ? 0 : INF_COST;
        continue;
      }
      // children already computed (postorder)
      for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
        std::uint32_t best = ~0u;
        for (auto ci : n.children) {
          std::uint32_t b = ~0u;
          for (std::uint8_t c = 0; c < nuc_state_count; ++c) {
            auto v = static_cast<std::uint64_t>(vec[ci][c]) + (s == c ? 0 : 1);
            if (v < b) b = static_cast<std::uint32_t>(v);
          }
          best = (best == ~0u) ? b : best + b;
        }
        vec[i][s] = best;
      }
    }
    std::size_t r = nodes.size() - 1;
    for (std::uint8_t s = 0; s < nuc_state_count; ++s)
      rows.m[g][s] = vec[r][s];
  }
  return rows;
}

void run_scaffold(std::string const& mode, std::string const& global_path,
                  std::string const& scaffold_path, std::string const& ids_prefix,
                  std::string const& rows_prefix, int np,
                  std::string const& profile_path,
                  std::uint64_t expected) {
  bool lb = mode == "lb";
  auto gctx = prepare(global_path);
  constexpr std::uint32_t INF_COST = 1u << 30;
  std::unordered_map<std::string, std::uint32_t> taxon_of_sid;
  for (std::uint32_t t = 0; t < gctx.grammar.taxa.id_to_sample_id.size(); ++t)
    taxon_of_sid.emplace(gctx.grammar.taxa.id_to_sample_id[t], t);

  // Load part leaf sets.
  std::vector<std::vector<std::string>> parts(np);
  for (int j = 0; j < np; ++j) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s%02d.ids.txt", ids_prefix.c_str(), j);
    parts[j] = read_ids(buf);
    std::sort(parts[j].begin(), parts[j].end());
  }

  // Collapse and flatten the scaffold.
  std::string scaffold_ref;
  auto root = load_tree(scaffold_path, &scaffold_ref, nullptr);
  auto collapsed = collapse_parts(std::move(root), parts);
  std::vector<scaffold_node> snodes;
  flatten_scaffold_node(collapsed, snodes, taxon_of_sid);
  std::size_t npseudo = 0, nreal = 0, ninternal = 0;
  for (auto const& n : snodes) {
    if (n.pseudo) ++npseudo;
    else if (n.leaf) ++nreal;
    else ++ninternal;
  }
  if (npseudo == 0 && np > 0)
    throw std::runtime_error("scaffold: no parts collapsed");
  if (npseudo != static_cast<std::size_t>(np))
    std::cerr << "scaffold: WARNING only " << npseudo << " of " << np
              << " parts are scaffold clades; the rest stay frozen "
                 "(original structure, free labels)\n";
  std::vector<int> part_of_slot(np, -1);
  {
    int slot = 0;
    for (auto const& n : snodes)
      if (n.pseudo) part_of_slot[n.part] = slot++;
  }
  std::cerr << "scaffold: nodes=" << snodes.size() << " internal=" << ninternal
            << " pseudo=" << npseudo << " real_leaves=" << nreal << "\n";

  // Rows per part (only for collapsed parts).
  std::vector<resp_rows> all_rows(np);
  std::uint64_t sum_part_lb = 0;
  for (int j = 0; j < np; ++j) {
    if (part_of_slot[j] < 0) continue;
    if (lb) {
      char buf[512];
      std::snprintf(buf, sizeof buf, "%s%02d.spr.pb.resp", rows_prefix.c_str(), j);
      all_rows[j] = load_resp_rows(buf, gctx);
      // header part_lb for reporting
      std::FILE* f = std::fopen(buf, "r");
      char line[256];
      while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "# part_lb ", 10) == 0) {
          sum_part_lb += std::strtoull(line + 10, nullptr, 10);
          break;
        }
      std::fclose(f);
    } else {
      char buf[512];
      std::snprintf(buf, sizeof buf, "%s%02d.wit.pb.gz", rows_prefix.c_str(), j);
      all_rows[j] = witness_rows(buf, gctx, taxon_of_sid);
    }
  }
  if (lb)
    std::cerr << "scaffold: sum part_lb (collapsed) = " << sum_part_lb << "\n";

  // Per-pattern DP over the collapsed scaffold.
  auto const& active = gctx.active;
  std::vector<std::uint64_t> per(active.size());
  std::vector<std::array<std::uint32_t, nuc_state_count>> rootvec(active.size());
#pragma omp parallel for schedule(dynamic, 8)
  for (std::size_t g = 0; g < active.size(); ++g) {
    auto const& states = active[g].state_by_taxon;
    std::vector<std::array<std::uint32_t, nuc_state_count>> vec(snodes.size());
    for (std::size_t i = 0; i < snodes.size(); ++i) {
      auto const& n = snodes[i];
      if (n.leaf) {
        if (n.pseudo) {
          vec[i] = all_rows[n.part].m[g];
        } else {
          for (std::uint8_t s = 0; s < nuc_state_count; ++s)
            vec[i][s] = (s == states[n.taxon]) ? 0 : INF_COST;
        }
        continue;
      }
      for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
        std::uint64_t acc = 0;
        for (auto ci : n.children) {
          std::uint32_t b = ~0u;
          for (std::uint8_t c = 0; c < nuc_state_count; ++c) {
            auto v = static_cast<std::uint64_t>(vec[ci][c]) + (s == c ? 0 : 1);
            if (v < b) b = static_cast<std::uint32_t>(v);
          }
          acc += b;
        }
        vec[i][s] = static_cast<std::uint32_t>(acc);
      }
    }
    std::size_t r = snodes.size() - 1;
    rootvec[g] = vec[r];
    per[g] = eval_row(reinterpret_cast<chart_cost const*>(vec[r].data()),
                      active[g].reference_state_counts);
  }
  // NOTE: eval_row is already weight-aware (reference_state_counts sum to
  // the pattern's site weight) -- do NOT multiply by weight again.
  std::uint64_t total = gctx.invariant_offset;
  for (std::size_t g = 0; g < active.size(); ++g) total += per[g];

  // Self-test: per-pattern diff against the tension machinery's own
  // witness DP on the same tree (enabled by WRIC_SCAFFOLD_SELFTEST).
  if (std::getenv("WRIC_SCAFFOLD_SELFTEST")) {
    std::string wref;
    auto wnodes = load_witness(scaffold_path, gctx, &wref);
    auto wdp = witness_tree_dp(gctx, wnodes, /*compute_total=*/true);
    std::uint64_t ndiff = 0, sumdiff = 0, sww = 0, swp = 0;
    auto const& root_classes = gctx.classes[gctx.grammar.root_clade];
    for (std::size_t a = 0; a < active.size(); ++a) {
      auto cls = root_classes.class_of_pattern[a];
      auto wv = eval_row(wdp.rows[0].data() + cls * nuc_state_count,
                         active[a].reference_state_counts);
      sww += static_cast<std::uint64_t>(active[a].weight) * wv;
      swp += static_cast<std::uint64_t>(active[a].weight) * per[a];
      if (wv != per[a]) {
        ++ndiff;
        sumdiff += static_cast<std::uint64_t>(active[a].weight) *
                   (per[a] > wv ? per[a] - wv : wv - per[a]);
        if (ndiff <= 10)
          std::fprintf(stderr,
                       "selftest: pattern %zu w=%u mine=%llu wit=%llu\n", a,
                       active[a].weight,
                       static_cast<unsigned long long>(per[a]),
                       static_cast<unsigned long long>(wv));
      }
    }
    std::fprintf(stderr,
                 "selftest: witness_total=%llu my_total=%llu ndiff=%zu "
                 "sumdiff=%llu sww=%llu swp=%llu\n",
                 static_cast<unsigned long long>(wdp.total),
                 static_cast<unsigned long long>(total), ndiff,
                 static_cast<unsigned long long>(sumdiff),
                 static_cast<unsigned long long>(sww),
                 static_cast<unsigned long long>(swp));
  }

  if (expected && total != expected) {
    std::fprintf(stderr, "scaffold: total %llu != expected %llu\n",
                 static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(expected));
    throw std::runtime_error("scaffold: total mismatch");
  }
  std::fprintf(stderr, "scaffold[%s]: total = %llu (expected %llu) %s\n",
               lb ? "lb" : "wit",
               static_cast<unsigned long long>(total),
               static_cast<unsigned long long>(expected),
               expected ? (total == expected ? "OK" : "MISMATCH") : "");

  if (lb && profile_path != "-") {
    // Second pass: per pattern (and per reference state present), recompute
    // the DP vectors and run a top-down argmin pass to recover every part's
    // attachment state under the optimal root choice for that ref state.
    std::vector<std::string> prof(active.size());
#pragma omp parallel for schedule(dynamic, 8)
    for (std::size_t g = 0; g < active.size(); ++g) {
      auto const& states = active[g].state_by_taxon;
      std::vector<std::array<std::uint32_t, nuc_state_count>> vec(snodes.size());
      for (std::size_t i = 0; i < snodes.size(); ++i) {
        auto const& n = snodes[i];
        if (n.leaf) {
          if (n.pseudo) {
            vec[i] = all_rows[n.part].m[g];
          } else {
            for (std::uint8_t s = 0; s < nuc_state_count; ++s)
              vec[i][s] = (s == states[n.taxon]) ? 0 : INF_COST;
          }
          continue;
        }
        for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
          std::uint64_t acc = 0;
          for (auto ci : n.children) {
            std::uint32_t b = ~0u;
            for (std::uint8_t c = 0; c < nuc_state_count; ++c) {
              auto v = static_cast<std::uint64_t>(vec[ci][c]) + (s == c ? 0 : 1);
              if (v < b) b = static_cast<std::uint32_t>(v);
            }
            acc += b;
          }
          vec[i][s] = static_cast<std::uint32_t>(acc);
        }
      }
      auto const& rc = active[g].reference_state_counts;
      std::string out;
      for (std::uint8_t r = 0; r < nuc_state_count; ++r) {
        if (rc[r] == 0) continue;
        std::uint8_t root_s = 0;
        std::uint32_t best = ~0u;
        for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
          auto v = static_cast<std::uint32_t>(vec[snodes.size() - 1][s]) +
                   parsimony_chart_detail::transition_cost(r, s);
          if (v < best) {
            best = v;
            root_s = s;
          }
        }
        std::vector<std::uint8_t> state(snodes.size(), 0);
        state[snodes.size() - 1] = root_s;
        for (std::size_t i = snodes.size(); i-- > 0;) {
          if (snodes[i].leaf || snodes[i].children.empty()) continue;
          for (auto ci : snodes[i].children) {
            std::uint8_t cs = 0;
            std::uint32_t cb = ~0u;
            for (std::uint8_t c = 0; c < nuc_state_count; ++c) {
              auto v = static_cast<std::uint32_t>(vec[ci][c]) +
                       (state[i] == c ? 0 : 1);
              if (v < cb) {
                cb = v;
                cs = c;
              }
            }
            state[ci] = cs;
          }
        }
        char buf[16];
        std::snprintf(buf, sizeof buf, "%zu %u %u ", g, r, rc[r]);
        out += buf;
        for (std::size_t i = 0; i < snodes.size(); ++i)
          if (snodes[i].pseudo) out += 'A' + state[i];
        out += '\n';
      }
      prof[g] = std::move(out);
    }
    std::FILE* f = std::fopen(profile_path.c_str(), "w");
    std::fprintf(f, "# scaffold lb attachment profiles: gidx ref count states(A-per-collapsed-part, part-order)\n");
    for (auto const& s : prof) std::fputs(s.c_str(), f);
    std::fclose(f);
    std::cerr << "scaffold: profiles written to " << profile_path << "\n";
  }
}

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

// ---- v1 conditioned-materialize phase 1: virtual-leaf profile bias ----
// (doc/WRIC-CONDITIONED-MATERIALIZE.md).  The fold LB profile pins each
// part's attachment state per (pattern, reference state); a virtual leaf
// carrying exactly those states, hung under every root production of the
// part grammar, makes profile deviation cost 1/site inside the normal
// materialize path -- no chart surgery.

struct vleaf_profile {
  // state[g][r] = attachment state for active pattern g at reference
  // state r; 0xFF = absent (hard error if a site needs it).
  std::vector<std::array<std::uint8_t, nuc_state_count>> state;
};

vleaf_profile load_vleaf_profile(std::string const& path, int j,
                                 std::size_t nactive) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) throw std::runtime_error("vleaf: cannot open profile " + path);
  vleaf_profile prof;
  prof.state.assign(nactive, {0xFF, 0xFF, 0xFF, 0xFF});
  char line[512];
  std::size_t nline = 0;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    ++nline;
    char* p = line;
    unsigned long long g = std::strtoull(p, &p, 10);
    unsigned long r = std::strtoul(p, &p, 10);
    std::strtoul(p, &p, 10);  // count (unused here)
    while (*p == ' ') ++p;
    unsigned col = 0;
    std::uint8_t s = 0xFF;
    while (*p >= 'A' && *p <= 'T') {
      if (col == static_cast<unsigned>(j)) s = static_cast<std::uint8_t>(*p - 'A');
      ++col;
      ++p;
    }
    if (col <= static_cast<unsigned>(j) || s == 0xFF)
      throw std::runtime_error("vleaf: profile line lacks part column " +
                               std::to_string(j));
    if (g >= nactive) throw std::runtime_error("vleaf: profile gidx range");
    if (r >= nuc_state_count) throw std::runtime_error("vleaf: profile ref");
    prof.state[g][static_cast<std::uint8_t>(r)] = s;
  }
  std::fclose(f);
  std::fprintf(stderr, "vleaf: profile lines=%zu column=%d\n", nline, j);
  return prof;
}

compact_genome build_profile_cg(grammar_ctx const& gctx,
                                std::string const& reference,
                                vleaf_profile const& prof) {
  std::map<mutation_position, nuc_base> map;
  std::size_t active_seen = 0, inv_positions = 0;
  for (auto const& pattern : gctx.patterns.patterns) {
    if (chart_multisite_detail::is_active_pattern(pattern)) {
      auto const& st = prof.state[active_seen];
      for (auto pos : pattern.positions) {
        auto ref_state =
            nuc_base::from_char(reference.at(pos - 1)).raw();
        auto s = st[ref_state];
        if (s == 0xFF)
          throw std::runtime_error("vleaf: profile missing (g,r) at pos " +
                                   std::to_string(pos));
        if (s != ref_state) map.emplace(pos, nuc_base(s));
      }
      ++active_seen;
    } else {
      auto s0 = pattern.state_by_taxon.front();
      for (auto pos : pattern.positions) {
        ++inv_positions;
        auto ref_state =
            nuc_base::from_char(reference.at(pos - 1)).raw();
        if (s0 != ref_state) map.emplace(pos, nuc_base(s0));
      }
    }
  }
  if (active_seen != gctx.active.size())
    throw std::runtime_error("vleaf: active pattern count mismatch");
  std::fprintf(stderr,
               "vleaf: profile CG mutations=%zu (invariant positions=%zu)\n",
               map.size(), inv_positions);
  return compact_genome(std::move(map));
}

void run_vleaf_add(std::string const& part_path,
                   std::string const& global_path,
                   std::string const& profile_path, int j,
                   std::string const& out_path) {
  auto gctx = prepare(global_path);
  std::string reference;
  {
    auto gdag = load_proto_dag(global_path);
    for (std::size_t i = 0; i < gdag.node_count(); ++i)
      if (larch::detail::is_ua_node(gdag.get_node(i))) {
        std::visit(
            [&](auto n) {
              using N = std::remove_cvref_t<decltype(n)>;
              if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::ua>>)
                reference = n.reference_sequence();
            },
            gdag.get_node(i));
        break;
      }
  }
  auto prof = load_vleaf_profile(profile_path, j, gctx.active.size());
  auto vleaf_cg = build_profile_cg(gctx, reference, prof);

  auto dag = load_proto_dag(part_path);
  std::size_t ua = no_idx;
  for (std::size_t i = 0; i < dag.node_count(); ++i)
    if (larch::detail::is_ua_node(dag.get_node(i))) {
      ua = i;
      break;
    }
  if (ua == no_idx) throw std::runtime_error("vleaf: no UA in " + part_path);

  auto leaf = dag.append_node<node_kind::leaf>();
  leaf.sample_id() = "__VLEAF__";
  leaf.cg() = vleaf_cg;

  // Root production nodes = UA's children.  Each gains one edge to the
  // shared virtual leaf at a fresh clade index.
  std::vector<std::size_t> roots;
  std::visit(
      [&](auto n) {
        for (auto const& ev : n.get_children())
          std::visit(
              [&](auto e) {
                roots.push_back(get_child_idx(dag, e.index()));
              },
              ev);
      },
      dag.get_node(ua));
  if (roots.empty()) throw std::runtime_error("vleaf: UA has no children");
  std::size_t nedges = 0;
  for (auto root : roots) {
    std::uint32_t maxc = 0;
    compact_genome parent_cg;
    std::visit(
        [&](auto n) {
          for (auto const& ev : n.get_children())
            std::visit(
                [&](auto e) {
                  maxc = std::max(maxc, static_cast<std::uint32_t>(e.clade_index()));
                },
                ev);
          if constexpr (requires { n.cg(); }) parent_cg = n.cg();
        },
        dag.get_node(root));
    if (maxc + 1 > 62)
      throw std::runtime_error("vleaf: root arity would exceed 63");
    auto edge = dag.append_edge<edge_kind::clade>();
    edge.clade_index() = maxc + 1;
    edge.edge_weight() = 0.0f;
    std::visit([&](auto p) { edge.set_parent(p); }, dag.get_node(root));
    std::visit([&](auto c) { edge.set_child(c); }, dag.get_node(leaf.index()));
    edge.mutations() =
        compact_genome::to_edge_mutations(reference, parent_cg, vleaf_cg);
    ++nedges;
  }
  build_clade_offsets(dag);
  validate_dag(dag, "wric_vleaf add");
  save_proto_dag(dag, out_path);
  std::fprintf(stderr, "vleaf add: %s roots=%zu new_edges=%zu vleaf_muts=?\n",
               part_path.c_str(), roots.size(), nedges);
}

void run_vleaf_strip(std::string const& in_path, std::string const& out_path) {
  std::string reference;
  std::map<std::string, compact_genome> cgs;
  auto root = load_tree_any(in_path, &reference, &cgs);
  std::size_t dropped = 0;
  std::function<void(tnode&)> prune = [&](tnode& n) {
    for (auto& c : n.children) prune(c);
    auto& cs = n.children;
    cs.erase(std::remove_if(cs.begin(), cs.end(),
                            [&](tnode const& c) {
                              if (c.leaf && c.sid == "__VLEAF__") {
                                ++dropped;
                                return true;
                              }
                              return false;
                            }),
             cs.end());
  };
  prune(root);
  if (dropped != 1)
    throw std::runtime_error("vleaf strip: expected exactly 1 virtual leaf, got " +
                             std::to_string(dropped));
  // The materializer binarizes the augmented root production, so the virtual
  // leaf may hang below an internal node; squash unary internal nodes left
  // behind by its removal (child promoted, structure otherwise preserved).
  std::function<tnode(tnode)> squash = [&](tnode n) -> tnode {
    for (auto& c : n.children) c = squash(std::move(c));
    if (!n.leaf && n.children.size() == 1) return std::move(n.children[0]);
    return n;
  };
  root = squash(std::move(root));
  if (!root.leaf && root.children.size() < 2)
    throw std::runtime_error("vleaf strip: root would be unary");
  wsub::write_tree(root, reference, cgs, out_path);
  std::fprintf(stderr, "vleaf strip: %s -> %s ok\n", in_path.c_str(),
               out_path.c_str());
}

// Part-local clamp file generation for --chart-bnb-root-clamp: maps the
// global fold LB profile column <col> onto the part grammar's own pattern
// enumeration (signature matching over sorted part sample ids), emitting
// "<pattern_idx> <ref> <forced> <count>" subgroup lines.  Reports subgroup
// conflicts (distinct global rows restricting to the same local pattern
// with different pins) and validates full site coverage.
void run_vleaf_clampgen(std::string const& global_path,
                        std::string const& profile_path, int j,
                        std::string const& part_path,
                        std::string const& out_path) {
  auto gctx = prepare(global_path);
  auto prof = load_vleaf_profile(profile_path, j, gctx.active.size());
  auto pctx = prepare(part_path);

  // Global sample id -> global taxon.
  std::unordered_map<std::string, std::uint32_t> gtaxon_of_sid;
  for (std::uint32_t t = 0;
       t < gctx.grammar.taxa.id_to_sample_id.size(); ++t)
    gtaxon_of_sid.emplace(gctx.grammar.taxa.id_to_sample_id[t], t);

  // Part taxa in canonical (sorted sample id) order.
  std::size_t np = pctx.grammar.taxa.id_to_sample_id.size();
  std::vector<std::uint32_t> ptaxon_of_canon(np);
  for (std::uint32_t t = 0; t < np; ++t)
    ptaxon_of_canon[pctx.canon_of_taxon[t]] = t;

  // Signature dictionary over ALL part patterns (active and invariant).
  struct sig_hash {
    std::size_t operator()(std::vector<std::uint8_t> const& v) const noexcept {
      return std::hash<std::string_view>{}(
          std::string_view(reinterpret_cast<char const*>(v.data()), v.size()));
    }
  };
  std::unordered_map<std::vector<std::uint8_t>, std::size_t, sig_hash>
      part_of_sig;
  part_of_sig.reserve(pctx.patterns.patterns.size() * 2);
  for (std::size_t i = 0; i < pctx.patterns.patterns.size(); ++i) {
    auto const& pattern = pctx.patterns.patterns[i];
    std::vector<std::uint8_t> sig(np);
    for (std::uint32_t t = 0; t < np; ++t)
      sig[pctx.canon_of_taxon[t]] = pattern.state_by_taxon[t];
    if (!part_of_sig.emplace(std::move(sig), i).second)
      throw std::runtime_error("clampgen: duplicate part signature");
  }

  // Accumulate subgroups per part pattern; track conflicts + coverage.
  std::map<std::pair<std::size_t, std::uint8_t>,
           std::map<std::uint8_t, std::uint32_t>>
      pins;  // (pidx, r) -> forced -> count
  std::size_t conflicts = 0, matched = 0;
  std::vector<std::uint8_t> sig(np);
  for (std::size_t g = 0; g < gctx.active.size(); ++g) {
    auto const& info = gctx.active[g];
    for (std::uint32_t c = 0; c < np; ++c) {
      auto it = gtaxon_of_sid.find(
          pctx.grammar.taxa.id_to_sample_id[ptaxon_of_canon[c]]);
      if (it == gtaxon_of_sid.end())
        throw std::runtime_error("clampgen: part leaf not in global");
      sig[c] = info.state_by_taxon[it->second];
    }
    auto found = part_of_sig.find(sig);
    if (found == part_of_sig.end())
      throw std::runtime_error("clampgen: restriction signature not found");
    ++matched;
    auto const& rc = info.reference_state_counts;
    for (std::uint8_t r = 0; r < nuc_state_count; ++r) {
      if (!rc[r]) continue;
      auto s = prof.state[g][r];
      if (s == 0xFF)
        throw std::runtime_error("clampgen: profile entry missing");
      auto& slot = pins[{found->second, r}];
      if (!slot.empty() && slot.begin()->first != s) ++conflicts;
      slot[s] += rc[r];
    }
  }

  // Coverage check against the part patterns' own reference counts.
  std::size_t coverage_mismatch = 0;
  std::uint64_t clamped_invariant_delta = 0;
  for (auto const& [pr, slot] : pins) {
    auto [pidx, r] = pr;
    std::uint32_t covered = 0;
    for (auto& [s, n] : slot) covered += n;
    if (covered != pctx.patterns.patterns[pidx].reference_state_counts[r])
      ++coverage_mismatch;
    if (is_invariant_site_pattern( pctx.patterns.patterns[pidx])) {

      auto u = pctx.patterns.patterns[pidx].state_by_taxon.front();
      auto free_cost = (u != r) ? 1u : 0u;
      for (auto& [s, n] : slot) {
        auto clamped_cost = (s != u ? 1u : 0u) + (s != r ? 1u : 0u);
        clamped_invariant_delta +=
            static_cast<std::uint64_t>(n) * (clamped_cost - free_cost);
      }
    }
  }

  std::FILE* f = std::fopen(out_path.c_str(), "w");
  if (!f) throw std::runtime_error("clampgen: cannot write " + out_path);
  std::fprintf(f, "# vleaf clampgen col=%d global=%s part=%s\n", j,
               global_path.c_str(), part_path.c_str());
  for (auto const& [pr, slot] : pins) {
    for (auto const& [s, n] : slot)
      std::fprintf(f, "%zu %u %u %u\n", pr.first, pr.second, s, n);
  }
  std::fclose(f);
  std::fprintf(stderr,
               "clampgen: %zu global active matched, %zu (pattern,ref) "
               "groups, %zu conflicts, %zu coverage mismatches, clamped "
               "invariant delta=%llu\n",
               matched, pins.size(), conflicts, coverage_mismatch,
               static_cast<unsigned long long>(clamped_invariant_delta));
}

// Witness-argmin profile: per (global active pattern g, reference state r)
// the state minimizing (witness_row[s] + [s!=r]).  Any witness-argmin
// profile is jointly realizable by the witness tree (root labels are free
// per position), so a trim clamped at it must return exactly the witness's
// certified optimum -- the strongest regression fixture for the clamp.
void run_vleaf_profgen(std::string const& global_path,
                       std::string const& wit_path,
                       std::string const& out_path) {
  auto gctx = prepare(global_path);
  std::unordered_map<std::string, std::uint32_t> taxon_of_sid;
  for (std::uint32_t t = 0; t < gctx.grammar.taxa.id_to_sample_id.size(); ++t)
    taxon_of_sid.emplace(gctx.grammar.taxa.id_to_sample_id[t], t);
  auto rows = witness_rows(wit_path, gctx, taxon_of_sid);
  std::FILE* f = std::fopen(out_path.c_str(), "w");
  if (!f) throw std::runtime_error("profgen: cannot write " + out_path);
  std::fprintf(f, "# witness-argmin profile (single part)\n");
  for (std::size_t g = 0; g < gctx.active.size(); ++g) {
    auto const& rc = gctx.active[g].reference_state_counts;
    for (std::uint8_t r = 0; r < nuc_state_count; ++r) {
      if (!rc[r]) continue;
      std::uint8_t best = 0;
      std::uint32_t bestv = ~0u;
      for (std::uint8_t s2 = 0; s2 < nuc_state_count; ++s2) {
        auto v = rows.m[g][s2] + (s2 == r ? 0 : 1);
        if (v < bestv) { bestv = v; best = s2; }
      }
      std::fprintf(f, "%zu %u %u %c\n", g, r, rc[r], 'A' + best);
    }
  }
  std::fclose(f);
  std::fprintf(stderr, "profgen: witness-argmin profile written to %s\n",
               out_path.c_str());
}

void run_vleaf_ub(std::string const& global_path, std::string const& wit_path,
                  std::string const& profile_path, int j) {
  auto gctx = prepare(global_path);
  std::unordered_map<std::string, std::uint32_t> taxon_of_sid;
  for (std::uint32_t t = 0; t < gctx.grammar.taxa.id_to_sample_id.size(); ++t)
    taxon_of_sid.emplace(gctx.grammar.taxa.id_to_sample_id[t], t);
  auto prof = load_vleaf_profile(profile_path, j, gctx.active.size());
  auto rows = witness_rows(wit_path, gctx, taxon_of_sid);
  std::uint64_t total = gctx.invariant_offset;
  std::uint64_t at_profile = gctx.invariant_offset;
  for (std::size_t g = 0; g < gctx.active.size(); ++g) {
    auto const& rc = gctx.active[g].reference_state_counts;
    for (std::uint8_t r = 0; r < nuc_state_count; ++r) {
      if (!rc[r]) continue;
      auto s0 = prof.state[g][r];
      if (s0 == 0xFF) throw std::runtime_error("vleaf ub: missing profile entry");
      // Part-local convention (matches larch2 scoring of the augmented DAG):
      // per site min over root state s of inside cost + UA transition [s!=r]
      // + virtual-leaf bias [s!=profile].
      std::uint32_t best = ~0u;
      for (std::uint8_t s = 0; s < nuc_state_count; ++s) {
        auto v = rows.m[g][s] + (s == r ? 0 : 1) + (s == s0 ? 0 : 1);
        if (v < best) best = v;
      }
      total += static_cast<std::uint64_t>(rc[r]) * best;
      at_profile += static_cast<std::uint64_t>(rc[r]) *
                    (rows.m[g][s0] + (s0 == r ? 0 : 1));
    }
  }
  std::fprintf(stdout, "vleaf_ub=%llu at_profile=%llu\n",
               static_cast<unsigned long long>(total),
               static_cast<unsigned long long>(at_profile));
}

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: %s tension <dag.pb.gz> <witness.pb.gz> <expected_total>\n"
                 "       %s closure <union_dag> <union_witness> <union_total> "
                 "<enriched_dag> <enriched_witness> <enriched_total>\n"
                 "       %s response <global.pb.gz> <outdir> <part1.pb.gz> "
                 "[part2.pb.gz ...]\n"
                 "       %s scaffold <lb|wit> <global.pb.gz> <scaffold_bin.pb.gz> "
                 "<ids_prefix> <rows_prefix> <np> <out.profile> [expected]\n"
                 "       %s vleaf clampgen <global.pb.gz> <profile> <col> "
                 "<part.pb.gz> <out.clamp>\n"
                 "       %s vleaf add <part.pb.gz> <global.pb.gz> <profile> "
                 "<j> <out.pb.gz>\n"
                 "       %s vleaf strip <biased_tree.pb.gz> <out.pb.gz>\n"
                 "       %s vleaf ub <global.pb.gz> <witness.pb.gz> <profile> <j>\n",
                 argv[0], argv[0], argv[0], argv[0], argv[0]);
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
    } else if (mode == "vleaf" && std::string(argv[2]) == "profgen" &&
               argc == 6) {
      run_vleaf_profgen(argv[3], argv[4], argv[5]);
    } else if (mode == "vleaf" && std::string(argv[2]) == "clampgen" &&
               argc == 8) {
      run_vleaf_clampgen(argv[3], argv[4], std::atoi(argv[5]), argv[6],
                         argv[7]);
    } else if (mode == "vleaf" && argc >= 3 && std::string(argv[2]) == "add" &&
               argc == 8) {
      run_vleaf_add(argv[3], argv[4], argv[5], std::atoi(argv[6]), argv[7]);
    } else if (mode == "vleaf" && std::string(argv[2]) == "strip" &&
               argc == 5) {
      run_vleaf_strip(argv[3], argv[4]);
    } else if (mode == "vleaf" && std::string(argv[2]) == "ub" &&
               argc == 7) {
      run_vleaf_ub(argv[3], argv[4], argv[5], std::atoi(argv[6]));
    } else if (mode == "scaffold" && (argc == 9 || argc == 10)) {
      run_scaffold(argv[2], argv[3], argv[4], argv[5], argv[6],
                   std::atoi(argv[7]), argv[8],
                   argc == 10 ? std::strtoull(argv[9], nullptr, 10) : 0);
    } else if (mode == "response" && argc >= 5) {
      std::vector<std::string> parts(argv + 4, argv + argc);
      run_response(argv[2], argv[3], parts);
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
