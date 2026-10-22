// wric_leaf_cg: dump one sample's compact genome from a pb DAG.
#include <larch/load_proto_dag.hpp>
#include <larch/site_patterns.hpp>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <dag.pb[.gz]> <sample_id>\n", argv[0]);
    return 2;
  }
  auto dag = larch::load_proto_dag(argv[1]);
  std::string want = argv[2];
  for (std::size_t i = 0; i < dag.node_count(); ++i) {
    auto nv = dag.get_node(i);
    if (!larch::detail::is_leaf_node(nv)) continue;
    std::string id;
    const larch::compact_genome* cg = nullptr;
    std::visit(
        [&](auto n) {
          using N = std::remove_cvref_t<decltype(n)>;
          if constexpr (std::is_same_v<N,
                           larch::node_view<larch::phylo_dag,
                                            larch::node_kind::leaf>>) {
            if (n.sample_id() == want) {
              id = n.sample_id();
              cg = &n.cg();
            }
          }
        },
        nv);
    if (!cg) continue;
    std::printf("sample %s n_mutations %zu\n", id.c_str(), std::distance(cg->begin(), cg->end()));
    for (auto const& [pos, base] : *cg)
      std::printf("%zu %c\n", pos, base.to_char());
    return 0;
  }
  std::fprintf(stderr, "sample not found\n");
  return 1;
}
