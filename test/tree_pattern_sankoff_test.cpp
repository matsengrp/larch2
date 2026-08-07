#include <larch/tree_pattern_sankoff.hpp>

#include <cstdint>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

[[noreturn]] static void fail(char const* expression, char const* file,
                              int line) {
  throw std::runtime_error(std::string{file} + ":" + std::to_string(line) +
                           ": CHECK failed: " + expression);
}

#define CHECK(expression)                                      \
  do {                                                         \
    if (!(expression)) fail(#expression, __FILE__, __LINE__);  \
  } while (false)

using larch::topology::internal;
using larch::topology::leaf;

static void masks_weights_test() {
  auto tree = internal({internal({leaf("A"), leaf("B")}), leaf("C")});
  std::vector<std::string> taxa{"A", "B", "C"};
  larch::tree_sankoff_pattern ambiguous;
  ambiguous.state_mask_by_taxon = {0b0011, 0b0010, 0b0010};
  ambiguous.weight = 2;
  ambiguous.reference_state_counts[larch::nuc_base::A] = 2;
  larch::tree_sankoff_pattern weighted;
  weighted.state_mask_by_taxon = {0b0001, 0b0001, 0b0010};
  weighted.weight = 3;
  weighted.reference_state_counts[larch::nuc_base::A] = 3;
  std::vector<larch::tree_sankoff_pattern> patterns{
      ambiguous, weighted};
  CHECK(larch::score_rooted_tree_sankoff(tree, taxa, patterns, false) == 3);
}

static void reference_offset_test() {
  auto tree = internal({leaf("A"), leaf("B"), leaf("C")});
  std::vector<std::string> taxa{"A", "B", "C"};
  larch::tree_sankoff_pattern invariant;
  invariant.state_mask_by_taxon = {0b0001, 0b0001, 0b0001};
  invariant.weight = 3;
  invariant.reference_state_counts[larch::nuc_base::A] = 1;
  invariant.reference_state_counts[larch::nuc_base::C] = 2;
  std::vector<larch::tree_sankoff_pattern> patterns{invariant};
  CHECK(larch::score_rooted_tree_sankoff(tree, taxa, patterns, false, 4) == 0);
  CHECK(larch::score_rooted_tree_sankoff(tree, taxa, patterns, true, 4) == 6);
}

static void arbitrary_arity_test() {
  for (std::size_t arity : {std::size_t{3}, std::size_t{4}, std::size_t{5},
                            std::size_t{10}}) {
    std::vector<larch::topology::rooted_tree> children;
    std::vector<std::string> taxa;
    larch::tree_sankoff_pattern pattern;
    for (std::size_t index = 0; index < arity; ++index) {
      taxa.push_back("T" + std::to_string(index));
      children.push_back(leaf(taxa.back()));
      pattern.state_mask_by_taxon.push_back(index == 0 ? 0b0010 : 0b0001);
    }
    pattern.weight = 1;
    pattern.reference_state_counts[larch::nuc_base::A] = 1;
    std::vector<larch::tree_sankoff_pattern> patterns{pattern};
    CHECK(larch::score_rooted_tree_sankoff(
              internal(std::move(children)), taxa, patterns, false) == 1);
  }
}

static larch::site_pattern_set exact_fixture_patterns() {
  larch::site_pattern_set result;
  result.taxon_count = 6;
  for (auto row : {std::string_view{"ACCAAA"}, std::string_view{"AAACCA"}}) {
    larch::site_pattern pattern;
    for (auto state : row) {
      pattern.state_by_taxon.push_back(
          state == 'A' ? larch::nuc_base::A : larch::nuc_base::C);
    }
    pattern.weight = static_cast<std::uint32_t>(result.patterns.size() + 1);
    pattern.reference_state_counts[larch::nuc_base::A] = pattern.weight;
    result.patterns.push_back(std::move(pattern));
  }
  return result;
}

static void site_pattern_adapter_test() {
  auto tree = internal({
      internal({leaf("A"), internal({leaf("B"), leaf("C")})}),
      internal({internal({leaf("D"), leaf("E")}), leaf("F")}),
  });
  std::vector<std::string> taxa{"A", "B", "C", "D", "E", "F"};
  auto patterns = exact_fixture_patterns();
  CHECK(larch::score_rooted_tree_sankoff(tree, taxa, patterns) == 3);
}

static bool rejects(auto&& callback, std::string_view needle) {
  try {
    callback();
  } catch (std::exception const& error) {
    return std::string_view{error.what()}.contains(needle);
  }
  return false;
}

static void rejection_test() {
  auto tree = internal({leaf("A"), leaf("B")});
  larch::tree_sankoff_pattern pattern;
  pattern.state_mask_by_taxon = {0, 1};
  pattern.weight = 1;
  pattern.reference_state_counts[larch::nuc_base::A] = 1;
  std::vector<larch::tree_sankoff_pattern> patterns{pattern};
  std::vector<std::string> taxa{"A", "B"};
  CHECK(rejects(
      [&] { (void)larch::score_rooted_tree_sankoff(
                tree, taxa, patterns, false); },
      "state mask"));
  patterns[0].state_mask_by_taxon = {1, 1};
  patterns[0].reference_state_counts.fill(0);
  CHECK(rejects(
      [&] { (void)larch::score_rooted_tree_sankoff(
                tree, taxa, patterns, false); },
      "reference counts"));
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::string_view mode{argv[1]};
  if (mode == "masks_weights") masks_weights_test();
  else if (mode == "reference_offset") reference_offset_test();
  else if (mode == "arbitrary_arity") arbitrary_arity_test();
  else if (mode == "site_pattern_adapter") site_pattern_adapter_test();
  else if (mode == "rejection") rejection_test();
  else return 2;
  std::println("PASS {}", mode);
  return 0;
}
