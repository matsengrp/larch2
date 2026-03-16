#pragma once

#include <larch/nuc.hpp>

#include <cstddef>
#include <map>
#include <utility>

namespace larch {

using mutation_position = std::size_t;
using edge_mutations =
    std::map<mutation_position, std::pair<nuc_base, nuc_base>>;

}  // namespace larch
