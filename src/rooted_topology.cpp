#include <larch/rooted_topology.hpp>

#include <larch/sha256.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace larch::topology {
namespace {

bool valid_utf8(std::string_view value) {
  std::size_t i = 0;
  while (i < value.size()) {
    auto const c = static_cast<unsigned char>(value[i]);
    if (c <= 0x7f) {
      ++i;
      continue;
    }
    std::size_t continuation = c >= 0xc2 && c <= 0xdf ? 1
                             : c >= 0xe0 && c <= 0xef ? 2
                             : c >= 0xf0 && c <= 0xf4 ? 3
                                                      : 99;
    if (continuation == 99 || i + continuation >= value.size()) return false;
    for (std::size_t j = 1; j <= continuation; ++j) {
      if ((static_cast<unsigned char>(value[i + j]) & 0xc0U) != 0x80U)
        return false;
    }
    if ((c == 0xe0 && static_cast<unsigned char>(value[i + 1]) < 0xa0) ||
        (c == 0xed && static_cast<unsigned char>(value[i + 1]) >= 0xa0) ||
        (c == 0xf0 && static_cast<unsigned char>(value[i + 1]) < 0x90) ||
        (c == 0xf4 && static_cast<unsigned char>(value[i + 1]) >= 0x90))
      return false;
    i += continuation + 1;
  }
  return true;
}

std::string canonical_unchecked(rooted_tree const& tree) {
  if (tree.is_leaf()) {
    return "L" + std::to_string(tree.label.size()) + ":" + tree.label;
  }
  std::vector<std::string> children;
  children.reserve(tree.children.size());
  for (auto const& child : tree.children) {
    children.push_back(canonical_unchecked(child));
  }
  std::ranges::sort(children);
  std::string result = "I" + std::to_string(children.size()) + "[";
  for (auto const& child : children) {
    result += std::to_string(child.size()) + ":" + child;
  }
  result += ']';
  return result;
}

void collect_taxa(rooted_tree const& tree, std::vector<std::string>& result) {
  if (tree.is_leaf()) {
    result.push_back(tree.label);
    return;
  }
  for (auto const& child : tree.children) collect_taxa(child, result);
}

std::size_t parse_unsigned(std::string_view bytes, std::size_t& position,
                           char terminator) {
  auto const begin = position;
  std::size_t value = 0;
  while (position < bytes.size() && bytes[position] != terminator) {
    auto const c = bytes[position];
    if (c < '0' || c > '9') {
      throw std::invalid_argument("canonical tree has a non-decimal length");
    }
    auto const digit = static_cast<std::size_t>(c - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      throw std::invalid_argument("canonical tree length overflows size_t");
    }
    value = value * 10 + digit;
    ++position;
  }
  if (position == begin || position == bytes.size()) {
    throw std::invalid_argument("canonical tree has a missing length delimiter");
  }
  if (position - begin > 1 && bytes[begin] == '0') {
    throw std::invalid_argument("canonical tree length has a leading zero");
  }
  ++position;
  return value;
}

rooted_tree parse_node(std::string_view bytes, std::size_t& position) {
  if (position == bytes.size()) {
    throw std::invalid_argument("canonical tree ends before a node");
  }
  auto const tag = bytes[position++];
  if (tag == 'L') {
    auto const length = parse_unsigned(bytes, position, ':');
    if (length > bytes.size() - position) {
      throw std::invalid_argument("canonical leaf label length is invalid");
    }
    auto label = std::string(bytes.substr(position, length));
    position += length;
    return leaf(std::move(label));
  }
  if (tag != 'I') {
    throw std::invalid_argument("canonical tree has an unknown node tag");
  }
  auto const count = parse_unsigned(bytes, position, '[');
  if (count < 2) {
    throw std::invalid_argument("canonical internal arity is less than two");
  }
  std::vector<rooted_tree> children;
  children.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    auto const child_length = parse_unsigned(bytes, position, ':');
    if (child_length == 0 || child_length > bytes.size() - position) {
      throw std::invalid_argument("canonical child length is invalid");
    }
    auto const child_end = position + child_length;
    auto child = parse_node(bytes, position);
    if (position != child_end) {
      throw std::invalid_argument("canonical child length does not match payload");
    }
    children.push_back(std::move(child));
  }
  if (position == bytes.size() || bytes[position++] != ']') {
    throw std::invalid_argument("canonical internal is missing its closing bracket");
  }
  return internal(std::move(children));
}

std::vector<rooted_tree> enumerate_binary_sorted(
    std::vector<std::string> const& labels) {
  if (labels.size() == 1) return {leaf(labels.front())};
  if (labels.size() >= 64) {
    throw std::invalid_argument("bounded binary enumerator supports fewer than 64 taxa");
  }
  std::vector<rooted_tree> result;
  auto const remaining = labels.size() - 1;
  auto const subset_count = std::uint64_t{1} << remaining;
  for (std::uint64_t mask = 0; mask + 1 < subset_count; ++mask) {
    std::vector<std::string> left{labels.front()};
    std::vector<std::string> right;
    for (std::size_t i = 0; i < remaining; ++i) {
      ((mask >> i) & 1U ? left : right).push_back(labels[i + 1]);
    }
    if (right.empty()) continue;
    auto left_trees = enumerate_binary_sorted(left);
    auto right_trees = enumerate_binary_sorted(right);
    for (auto const& lhs : left_trees) {
      for (auto const& rhs : right_trees) {
        result.push_back(internal({lhs, rhs}));
      }
    }
  }
  std::ranges::sort(result, {}, canonical_unchecked);
  result.erase(std::ranges::unique(result, {}, canonical_unchecked).begin(),
               result.end());
  return result;
}

void collect_contractible_supports(rooted_tree const& tree, bool is_root,
                                   std::vector<std::string>& result) {
  if (tree.is_leaf()) return;
  if (!is_root) result.push_back(taxon_support_bytes(tree));
  for (auto const& child : tree.children) {
    collect_contractible_supports(child, false, result);
  }
}

rooted_tree contract_selected(rooted_tree const& tree, bool is_root,
                              std::set<std::string> const& selected) {
  if (tree.is_leaf()) return tree;
  std::vector<rooted_tree> children;
  for (auto const& child : tree.children) {
    auto contracted = contract_selected(child, false, selected);
    auto const contract_edge = !child.is_leaf() &&
                               selected.contains(taxon_support_bytes(child));
    if (contract_edge) {
      children.insert(children.end(), contracted.children.begin(),
                      contracted.children.end());
    } else {
      children.push_back(std::move(contracted));
    }
  }
  (void)is_root;
  return internal(std::move(children));
}

std::vector<std::uint64_t> sankoff_rows(
    rooted_tree const& tree, std::map<std::string, char> const& observations,
    std::string_view alphabet) {
  constexpr auto infinity = (std::numeric_limits<std::uint64_t>::max)() / 4;
  if (tree.is_leaf()) {
    auto found = observations.find(tree.label);
    if (found == observations.end()) {
      throw std::invalid_argument("Sankoff observations omit taxon " + tree.label);
    }
    std::vector<std::uint64_t> result(alphabet.size(), infinity);
    auto state = alphabet.find(found->second);
    if (state == std::string_view::npos) {
      throw std::invalid_argument("Sankoff observation is outside the alphabet");
    }
    result[state] = 0;
    return result;
  }
  std::vector<std::uint64_t> result(alphabet.size(), 0);
  for (auto const& child : tree.children) {
    auto child_cost = sankoff_rows(child, observations, alphabet);
    for (std::size_t parent_state = 0; parent_state < alphabet.size();
         ++parent_state) {
      auto best = infinity;
      for (std::size_t child_state = 0; child_state < alphabet.size();
           ++child_state) {
        best = std::min(best, child_cost[child_state] +
                                  (parent_state == child_state ? 0U : 1U));
      }
      result[parent_state] += best;
    }
  }
  return result;
}

}  // namespace

rooted_tree leaf(std::string label) {
  return rooted_tree{std::move(label), {}};
}

rooted_tree internal(std::vector<rooted_tree> children) {
  if (children.size() < 2) {
    throw std::invalid_argument("internal vertex must have at least two children");
  }
  std::ranges::sort(children, {}, canonical_unchecked);
  return rooted_tree{"", std::move(children)};
}

void validate(rooted_tree const& tree) {
  if (tree.is_leaf()) {
    if (!valid_utf8(tree.label)) {
      throw std::invalid_argument("leaf label must be valid UTF-8");
    }
  } else {
    if (!tree.label.empty()) {
      throw std::invalid_argument("internal vertex cannot carry a leaf label");
    }
    if (tree.children.size() < 2) {
      throw std::invalid_argument("internal vertex must have at least two children");
    }
    for (auto const& child : tree.children) validate(child);
  }
  auto labels = taxa(tree);
  if (std::ranges::adjacent_find(labels) != labels.end()) {
    throw std::invalid_argument("tree has duplicate leaf labels");
  }
}

std::vector<std::string> taxa(rooted_tree const& tree) {
  std::vector<std::string> result;
  collect_taxa(tree, result);
  std::ranges::sort(result);
  return result;
}

bool is_binary(rooted_tree const& tree) noexcept {
  if (tree.is_leaf()) return true;
  if (tree.children.size() != 2) return false;
  return std::ranges::all_of(tree.children, is_binary);
}

std::string canonical_tree_bytes(rooted_tree const& tree) {
  validate(tree);
  return canonical_unchecked(tree);
}

rooted_tree parse_canonical_tree(std::string_view bytes) {
  std::size_t position = 0;
  auto result = parse_node(bytes, position);
  if (position != bytes.size()) {
    throw std::invalid_argument("canonical tree has trailing bytes");
  }
  validate(result);
  if (canonical_unchecked(result) != bytes) {
    throw std::invalid_argument("tree bytes are not in canonical child order");
  }
  return result;
}

std::string topology_sha256(rooted_tree const& tree) {
  larch::sha256 digest;
  digest.update("topology-landscape.rooted-labelled-topology.v1\n");
  digest.update(canonical_tree_bytes(tree));
  return digest.hex_digest();
}

std::string taxon_support_bytes(rooted_tree const& tree) {
  validate(tree);
  auto labels = taxa(tree);
  std::string result = "S" + std::to_string(labels.size()) + "[";
  for (auto const& label : labels) {
    result += std::to_string(label.size()) + ":" + label;
  }
  result += ']';
  return result;
}

std::vector<rooted_tree> enumerate_rooted_binary(
    std::span<std::string const> input_labels) {
  std::vector<std::string> labels(input_labels.begin(), input_labels.end());
  std::ranges::sort(labels);
  if (labels.size() < 2 || std::ranges::adjacent_find(labels) != labels.end() ||
      std::ranges::any_of(labels, &std::string::empty)) {
    throw std::invalid_argument(
        "binary topology enumeration needs at least two unique nonempty labels");
  }
  return enumerate_binary_sorted(labels);
}

std::vector<rooted_tree> enumerate_rooted_hard(
    std::span<std::string const> labels) {
  auto binary = enumerate_rooted_binary(labels);
  std::map<std::string, rooted_tree> unique;
  for (auto const& tree : binary) {
    std::vector<std::string> supports;
    collect_contractible_supports(tree, true, supports);
    if (supports.size() >= 64) {
      throw std::invalid_argument("bounded hard enumerator has too many contractions");
    }
    auto const subset_count = std::uint64_t{1} << supports.size();
    for (std::uint64_t mask = 0; mask < subset_count; ++mask) {
      std::set<std::string> selected;
      for (std::size_t i = 0; i < supports.size(); ++i) {
        if ((mask >> i) & 1U) selected.insert(supports[i]);
      }
      auto contracted = contract_selected(tree, true, selected);
      auto bytes = canonical_tree_bytes(contracted);
      unique.emplace(std::move(bytes), std::move(contracted));
    }
  }
  std::vector<rooted_tree> result;
  result.reserve(unique.size());
  for (auto& [bytes, tree] : unique) {
    (void)bytes;
    result.push_back(std::move(tree));
  }
  return result;
}

std::uint64_t unit_cost_sankoff_score(
    rooted_tree const& tree,
    std::map<std::string, char> const& leaf_observations,
    std::string_view alphabet,
    char reference_state,
    bool score_root_edge) {
  validate(tree);
  if (alphabet.empty() ||
      std::set<char>(alphabet.begin(), alphabet.end()).size() != alphabet.size()) {
    throw std::invalid_argument("Sankoff alphabet must be nonempty and unique");
  }
  auto expected = taxa(tree);
  std::vector<std::string> observed;
  for (auto const& [label, state] : leaf_observations) {
    (void)state;
    observed.push_back(label);
  }
  if (observed != expected) {
    throw std::invalid_argument("Sankoff observations do not equal the tree taxon set");
  }
  auto reference_index = alphabet.find(reference_state);
  if (reference_index == std::string_view::npos) {
    throw std::invalid_argument("Sankoff reference state is outside the alphabet");
  }
  auto root_cost = sankoff_rows(tree, leaf_observations, alphabet);
  auto best = (std::numeric_limits<std::uint64_t>::max)();
  for (std::size_t state = 0; state < root_cost.size(); ++state) {
    best = std::min(best, root_cost[state] +
                              (score_root_edge && state != reference_index ? 1U : 0U));
  }
  return best;
}

}  // namespace larch::topology
