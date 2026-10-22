// wric_dag_binarize: resolve >2-clade productions of a (possibly merged) DAG
// deterministically as a left comb over clade groups (ascending clade index),
// preserving every per-clade alternative edge set (alternatives attach at the
// comb slot of their group). Unary non-UA productions are squashed by
// expansion lists (single kid -> bypass; alternatives -> fan-out into each
// parent edge), which preserves the parse set exactly. Fresh comb nodes carry
// the original production's CG (annotation consistency; the chart re-derives
// inner labels from leaf CGs + reference). Optionally writes a first-parse
// witness tree (per clade, first child edge).
// usage: wric_dag_binarize <in.pb.gz> <out.pb.gz> [--sample-first <tree.pb.gz>]
#include "wric_subtree_common.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

using namespace wsub;

namespace {

struct group {
  std::uint32_t clade;
  std::vector<std::size_t> kids;  // old child indices (alternatives)
};

std::vector<std::size_t> topo_order(larch::phylo_dag& dag, std::size_t ua) {
  std::vector<std::size_t> order;
  std::vector<std::vector<std::size_t>> kids(dag.node_count());
  for (std::size_t i = 0; i < dag.node_count(); ++i)
    std::visit(
        [&](auto n) {
          for (auto const& ev : n.get_children())
            std::visit(
                [&](auto e) {
                  kids[i].push_back(larch::get_child_idx(dag, e.index()));
                },
                ev);
        },
        dag.get_node(i));
  std::vector<char> seen(dag.node_count(), 0);
  std::vector<std::pair<std::size_t, std::size_t>> work{{ua, 0}};
  seen[ua] = 1;
  while (!work.empty()) {
    auto [idx, next] = work.back();  // by value; back() dangles on push
    if (next < kids[idx].size()) {
      auto c = kids[idx][next];
      work.back().second = next + 1;
      if (!seen[c]) {
        seen[c] = 1;
        work.push_back({c, 0});
      }
    } else {
      order.push_back(idx);
      work.pop_back();
    }
  }
  return order;  // children before parents; UA last
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3 && !(argc == 5 && std::string(argv[3]) == "--sample-first")) {
      std::fprintf(stderr,
                   "usage: %s <in.pb.gz> <out.pb.gz> "
                   "[--sample-first <tree.pb.gz>]\n",
                   argv[0]);
      return 2;
    }
    using namespace larch;
    auto dag = load_proto_dag(argv[1]);
    std::size_t ua = no_idx;
    for (std::size_t i = 0; i < dag.node_count(); ++i)
      if (larch::detail::is_ua_node(dag.get_node(i))) {
        ua = i;
        break;
      }
    if (ua == no_idx) throw std::runtime_error("no UA node");
    auto order = topo_order(dag, ua);
    std::fprintf(stderr, "topo: %zu nodes\n", order.size());

    // Per-node clade groups (all inner + UA).
    std::vector<std::vector<group>> groups(dag.node_count());
    for (std::size_t i = 0; i < dag.node_count(); ++i) {
      if (larch::detail::is_leaf_node(dag.get_node(i))) continue;
      std::map<std::uint32_t, std::vector<std::size_t>> byc;
      std::visit(
          [&](auto n) {
            for (auto const& ev : n.get_children())
              std::visit(
                  [&](auto e) {
                    byc[e.clade_index()].push_back(
                        get_child_idx(dag, e.index()));
                  },
                  ev);
          },
          dag.get_node(i));
      for (auto& [cl, ks] : byc) groups[i].push_back({cl, std::move(ks)});
    }

    phylo_dag out;
    auto uan = out.append_node<node_kind::ua>();
    out.set_root(uan);
    std::string reference;
    std::map<std::string, compact_genome> leaf_cgs;
    std::visit(
        [&](auto n) {
          using N = std::remove_cvref_t<decltype(n)>;
          if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::ua>>)
            reference = n.reference_sequence();
        },
        dag.get_node(ua));
    uan.reference_sequence() = reference;

    auto get_cg = [&](std::size_t i) {
      compact_genome cg;
      std::visit(
          [&](auto n) {
            if constexpr (requires { n.cg(); }) cg = n.cg();
          },
          dag.get_node(i));
      return cg;
    };
    auto set_cg = [&](std::size_t i, compact_genome const& cg) {
      std::visit(
          [&](auto n) {
            if constexpr (requires { n.cg(); }) n.cg() = cg;
          },
          out.get_node(i));
    };
    auto add_edge = [&](std::size_t parent, std::size_t child,
                        std::uint32_t clade) {
      auto edge = out.append_edge<edge_kind::clade>();
      edge.clade_index() = clade;
      edge.edge_weight() = 0.0f;
      std::visit([&](auto p) { edge.set_parent(p); }, out.get_node(parent));
      std::visit([&](auto c) { edge.set_child(c); }, out.get_node(child));
    };

    // New-index mapping with unary-squash expansion lists. For each old node
    // (post-processing), expand[i] = list of new node indices representing it.
    std::vector<std::vector<std::size_t>> expand(dag.node_count());
    auto kids_of = [&](std::size_t old_idx) -> std::vector<std::size_t>& {
      if (expand[old_idx].empty())
        throw std::runtime_error("child not yet processed: " +
                                 std::to_string(old_idx));
      return expand[old_idx];
    };
    std::size_t combed = 0, squashed = 0;

    // Build a node with the given slots: slots[r] = list of new child indices
    // competing at clade r. Requires slots.size() in {1(only UA-adjacent),2}.
    auto build_binary = [&](std::vector<std::vector<std::size_t>> slots,
                            compact_genome const& cg) -> std::size_t {
      if (slots.size() != 2)
        throw std::runtime_error("build_binary needs 2 slots");
      auto inner = out.append_node<node_kind::inner>();
      for (std::size_t r = 0; r < 2; ++r)
        for (auto c : slots[r])
          add_edge(inner.index(), c, static_cast<std::uint32_t>(r));
      set_cg(inner.index(), cg);
      return inner.index();
    };

    for (auto idx : order) {
      if (idx == ua) continue;
      auto nv = dag.get_node(idx);
      if (larch::detail::is_leaf_node(nv)) {
        auto leaf = out.append_node<node_kind::leaf>();
        std::visit(
            [&](auto n) {
              using N = std::remove_cvref_t<decltype(n)>;
              if constexpr (std::is_same_v<N,
                               node_view<phylo_dag, node_kind::leaf>>) {
                leaf.sample_id() = n.sample_id();
                leaf.cg() = n.cg();
                leaf_cgs[n.sample_id()] = n.cg();
              }
            },
            nv);
        expand[idx] = {leaf.index()};
        continue;
      }
      auto& gs = groups[idx];
      auto cg = get_cg(idx);
      if (gs.size() == 1) {
        // Unary: squash into expansion list.
        ++squashed;
        expand[idx].clear();
        for (auto k : gs[0].kids)
          for (auto e : kids_of(k)) expand[idx].push_back(e);
      } else if (gs.size() == 2) {
        std::vector<std::vector<std::size_t>> slots;
        for (auto& g : gs) {
          std::vector<std::size_t> s;
          for (auto k : g.kids)
            for (auto e : kids_of(k)) s.push_back(e);
          slots.push_back(std::move(s));
        }
        expand[idx] = {build_binary(std::move(slots), cg)};
      } else {
        ++combed;
        // Left comb: bottom = {g0,g1}; folds = {acc, g_j}.
        auto slot = [&](group const& g) {
          std::vector<std::size_t> s;
          for (auto k : g.kids)
            for (auto e : kids_of(k)) s.push_back(e);
          return s;
        };
        std::size_t acc = build_binary({slot(gs[0]), slot(gs[1])}, cg);
        for (std::size_t j = 2; j < gs.size(); ++j)
          acc = build_binary({{acc}, slot(gs[j])}, cg);
        expand[idx] = {acc};
      }
    }
    // UA: fan out (combing under UA if >2 groups keeps UA simple; unary under
    // UA is grammar-legal, so a chain is fine).
    {
      auto& gs = groups[ua];
      std::vector<std::size_t> all;
      for (auto& g : gs)
        for (auto k : g.kids)
          for (auto e : kids_of(k)) all.push_back(e);
      if (gs.size() <= 2) {
        std::size_t r = 0;
        for (auto& g : gs) {
          for (auto k : g.kids)
            for (auto e : kids_of(k))
              add_edge(uan.index(), e, static_cast<std::uint32_t>(r));
          ++r;
        }
      } else {
        std::size_t acc = all.empty() ? no_idx : all[0];
        for (std::size_t j = 1; j < all.size(); ++j) {
          auto inner = out.append_node<node_kind::inner>();
          set_cg(inner.index(), compact_genome{});  // empty = reference
          add_edge(inner.index(), acc, 0u);
          add_edge(inner.index(), all[j], 1u);
          acc = inner.index();
        }
        add_edge(uan.index(), acc, 0u);
      }
    }
    build_clade_offsets(out);
    recompute_edge_mutations(out);
    build_clade_offsets(out);
    validate_dag(out, "wric_dag_binarize");
    save_proto_dag(out, argv[2]);
    std::fprintf(stderr,
                 "dag_binarize: %zu nodes in, combed %zu, squashed %zu unary, "
                 "%zu nodes out\n",
                 dag.node_count(), combed, squashed, out.node_count());

    if (argc == 5) {
      // First-parse sample: per clade, first child edge.
      std::function<tnode(std::size_t)> descend = [&](std::size_t i) -> tnode {
        tnode n;
        auto nv2 = out.get_node(i);
        if (larch::detail::is_leaf_node(nv2)) {
          std::visit(
              [&](auto nv3) {
                using N = std::remove_cvref_t<decltype(nv3)>;
                if constexpr (std::is_same_v<N,
                                 node_view<phylo_dag, node_kind::leaf>>) {
                  n.leaf = true;
                  n.sid = nv3.sample_id();
                  n.taxa = {n.sid};
                }
              },
              nv2);
          return n;
        }
        std::map<std::uint32_t, std::size_t> firstc;
        std::visit(
            [&](auto nn) {
              for (auto const& ev : nn.get_children())
                std::visit(
                    [&](auto e) {
                      auto c = get_child_idx(out, e.index());
                      auto it = firstc.find(e.clade_index());
                      if (it == firstc.end())
                        firstc.emplace(e.clade_index(), c);
                    },
                    ev);
            },
            nv2);
        for (auto& [cl, c] : firstc) {
          auto child = descend(c);
          n.taxa = sorted_union(n.taxa, child.taxa);
          n.children.push_back(std::move(child));
        }
        return n;
      };
      std::size_t root_idx = no_idx;
      std::visit(
          [&](auto nn) {
            for (auto const& ev : nn.get_children())
              std::visit(
                  [&](auto e) {
                    if (root_idx == no_idx)
                      root_idx = get_child_idx(out, e.index());
                  },
                  ev);
          },
          out.get_node(uan.index()));
      auto root = descend(root_idx);
      write_tree(root, reference, leaf_cgs, argv[4]);
      std::fprintf(stderr, "sample-first parse written: %s\n", argv[4]);
    }
    return 0;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
