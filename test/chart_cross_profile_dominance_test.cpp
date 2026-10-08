#include <larch/chart_cross_profile_dominance.hpp>
#include <array>
#include <iostream>

struct entry { std::vector<std::uint32_t> rows; unsigned witness; };
struct factor { std::vector<unsigned> varying_classes; };
struct classes { std::vector<std::uint64_t> weight_by_class; };

int main() {
  using namespace larch;
  std::size_t checks = 0;
  auto run = [&](std::vector<entry> entries, std::vector<std::uint64_t> weights,
                 cross_profile_dominance_options opts, bool root,
                 std::vector<unsigned> expected) {
    factor f;
    for (unsigned j = 0; j < weights.size(); ++j) f.varying_classes.push_back(j);
    cross_profile_dominance_stats stats;
    opts.audit = [&](cross_profile_dominance_audit_pool const& p) {
      for (std::size_t i = 0; i < p.profiles.size(); ++i) {
        auto owner = p.final_owner[i];
        if (p.final_owner[owner] != owner)
          throw std::runtime_error("non-surviving direct cover");
        if (chart_cross_profile_detail::defect(p.profiles[owner], p.profiles[i], p.weights) > 0)
          throw std::runtime_error("invalid direct cover");
      }
      return true;
    };
    chart_cross_profile_detail::apply(entries, f, classes{weights}, root, opts,
                                       stats, 0x3ffffffeU);
    std::vector<unsigned> observed;
    for (auto const& e : entries) observed.push_back(e.witness);
    if (observed != expected) throw std::runtime_error("control selection");
    ++checks;
  };
  cross_profile_dominance_options on; on.enabled = true;
  auto a = entry{{0,5,5,5},11};
  auto b = entry{{1,1,1,1},22};
  run({a,b}, {1}, on, false, {11});
  run({a,b}, {1}, {}, false, {11,22});
  run({a,{{5,0,5,5},22}}, {1}, on, false, {11,22});
  run({a,b}, {1}, on, true, {11,22});
  auto capped = on; capped.pair_limit = 0;
  run({a,b}, {1}, capped, false, {11,22});
  capped = on; capped.width_limit = 1;
  run({a,b}, {1}, capped, false, {11,22});
  run({a,{a.rows,22}}, {1}, on, false, {11});
  run({{{0,5,5,5,0,0,0,0},11},{{0,0,0,0,3,8,8,8},22}},
      {1,2}, on, false, {11});
  run({{{0,5,5,5},11},{{0,0,5,5},22}}, {1}, on, false, {22});
  // Four independent boundary characters: the weighted test is the exact
  // maximum of ordinary-edge response differences, not raw inside dominance.
  for (unsigned am = 1; am < 16; ++am) for (unsigned bm = 1; bm < 16; ++bm)
    for (unsigned ba = 0; ba < 3; ++ba) for (unsigned bb = 0; bb < 3; ++bb) {
      chart_cross_profile_detail::profile aa{ba,{static_cast<std::uint8_t>(am)}};
      chart_cross_profile_detail::profile be{bb,{static_cast<std::uint8_t>(bm)}};
      int dense = -100;
      for (unsigned t = 0; t < 4; ++t)
        dense = std::max(dense, static_cast<int>(ba) - static_cast<int>(bb) +
                        ((am & (1u << t)) ? 0 : 1) - ((bm & (1u << t)) ? 0 : 1));
      if (chart_cross_profile_detail::defect(aa,be,{1}) != dense)
        throw std::runtime_error("dense edge maximum mismatch");
      ++checks;
    }
  // Disabled means no keys/profile/audit construction, including invalid rows.
  auto invalid = std::vector<entry>{{{},1},{ {},2 }};
  cross_profile_dominance_stats s;
  chart_cross_profile_detail::apply(invalid, factor{{0}}, classes{{1}}, false,
                                     {}, s, 0x3ffffffeU);
  if (s.pairs != 0 || s.positions != 0 || invalid.size() != 2)
    throw std::runtime_error("disabled path constructs profile");
  ++checks;
  std::cout << "{\"ok\":true,\"checks\":" << checks << "}\n";
}
