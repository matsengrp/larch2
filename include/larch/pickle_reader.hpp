#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace larch {

// Metadata for a single tensor in a PyTorch state dict.
struct tensor_info {
  std::string name;             // parameter name, e.g. "r_conv.weight"
  std::string storage_key;      // e.g. "0" -> "data/0" in the ZIP archive
  std::vector<int64_t> shape;   // e.g. {16, 7, 9}
  std::vector<int64_t> stride;  // e.g. {63, 9, 1}
  int64_t storage_offset = 0;   // byte offset into storage (usually 0)
  int64_t num_elements = 0;     // total element count in storage
};

// Result of parsing a torch.save'd state_dict pickle.
struct state_dict_info {
  std::vector<tensor_info> tensors;
};

// Parse a pickle protocol 2 stream produced by torch.save(state_dict, ...).
// Only handles the opcode subset that PyTorch uses for state dicts.
state_dict_info parse_state_dict(std::span<const uint8_t> pickle_data);

}  // namespace larch
