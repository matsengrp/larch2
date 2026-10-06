// Debug: dump the UA-rooted structure of a pb DAG.
#include <larch/load_proto_dag.hpp>
#include <larch/clade_grammar.hpp>
#include <cstdio>
#include <vector>

using namespace larch;

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  auto dag = load_proto_dag(argv[1]);
  std::printf("nodes=%zu edges=%zu\n", dag.node_count(), dag.edge_count());
  std::size_t ua = no_idx;
  std::size_t ua_count = 0;
  for (std::size_t i = 0; i < dag.node_count(); ++i) {
    if (larch::detail::is_ua_node(dag.get_node(i))) {
      if (ua == no_idx) ua = i;
      ++ua_count;
    }
  }
  std::printf("ua=%zu ua_count=%zu root_setting=%zu\n", ua, ua_count,
              [&] {
                auto rv = dag.get_root();
                return std::visit([](auto n) { return n.index(); }, rv);
              }());
  // BFS from ua
  std::vector<std::size_t> frontier{ua};
  int level = 0;
  while (!frontier.empty() && level < 6) {
    std::printf("level %d:", level);
    std::vector<std::size_t> next;
    for (auto idx : frontier) {
      char kind = '?';
      std::size_t kids = 0;
      std::vector<std::size_t> kid_idx;
      std::visit(
          [&](auto n) {
            using N = std::remove_cvref_t<decltype(n)>;
            if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::ua>>)
              kind = 'U';
            else if constexpr (std::is_same_v<N, node_view<phylo_dag, node_kind::inner>>)
              kind = 'I';
            else
              kind = 'L';
            for (auto ev : n.get_children()) {
              std::visit(
                  [&](auto e) {
                    std::visit(
                        [&](auto c) {
                          kid_idx.push_back(c.index());
                          ++kids;
                        },
                        e.get_child());
                  },
                  ev);
            }
          },
          dag.get_node(idx));
    std::printf(" [%zu:%c/%zu->{", idx, kind, kids);
    for (auto k : kid_idx) std::printf("%zu,", k);
    std::printf("}]");
    for (auto k : kid_idx) next.push_back(k);
    }
    std::printf("\n");
    frontier = std::move(next);
    ++level;
  }
  return 0;
}
