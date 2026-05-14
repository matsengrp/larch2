#pragma once

#include <larch/pickle_reader.hpp>
#include <larch/zip_reader.hpp>

#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace larch {

// A named tensor with shape and a span into the mmap'd .pth file data.
struct tensor_data {
  std::string name;
  std::vector<int64_t> shape;
  std::span<const float> data;
};

// A loaded .pth file. Owns the zip_reader (which owns the mmap).
// tensor_data spans point into the mmap'd memory and are valid for the
// lifetime of this object.
struct pth_file {
  zip_reader zip;
  std::string prefix;  // common prefix of entries (e.g. "s5f-libtorch/")
  std::vector<tensor_data> tensors;

  // Find a tensor by exact name.
  tensor_data const* find(std::string_view name) const {
    for (auto& t : tensors) {
      if (t.name == name) return &t;
    }
    return nullptr;
  }

  // Find a tensor by name, throw if not found.
  tensor_data const& get(std::string_view name) const {
    auto* t = find(name);
    if (!t)
      throw std::runtime_error{"pth_file: tensor not found: " +
                               std::string{name}};
    return *t;
  }
};

namespace pth_detail {

struct tensor_layout {
  std::size_t numel = 0;
  std::size_t byte_offset = 0;
  std::size_t byte_size = 0;
};

inline tensor_layout checked_tensor_layout(tensor_info const& ti,
                                           std::size_t raw_size) {
  std::size_t numel = 1;
  for (auto dim : ti.shape) {
    if (dim < 0)
      throw std::runtime_error{"pth_loader: negative shape dimension for tensor: " +
                               ti.name};
    auto d = static_cast<std::size_t>(dim);
    if (d != 0 && numel > std::numeric_limits<std::size_t>::max() / d)
      throw std::runtime_error{"pth_loader: tensor element count overflow: " +
                               ti.name};
    numel *= d;
  }

  if (ti.storage_offset < 0)
    throw std::runtime_error{"pth_loader: negative storage offset for tensor: " +
                             ti.name};
  if (ti.num_elements < 0)
    throw std::runtime_error{"pth_loader: negative storage size for tensor: " +
                             ti.name};

  auto storage_offset = static_cast<std::size_t>(ti.storage_offset);
  if (ti.num_elements > 0 &&
      (storage_offset > static_cast<std::size_t>(ti.num_elements) ||
       numel > static_cast<std::size_t>(ti.num_elements) - storage_offset))
    throw std::runtime_error{"pth_loader: tensor shape exceeds storage: " +
                             ti.name};

  constexpr auto float_size = sizeof(float);
  if (storage_offset > std::numeric_limits<std::size_t>::max() / float_size)
    throw std::runtime_error{"pth_loader: tensor byte offset overflow: " +
                             ti.name};
  auto byte_offset = storage_offset * float_size;

  if (numel > std::numeric_limits<std::size_t>::max() / float_size)
    throw std::runtime_error{"pth_loader: tensor byte size overflow: " +
                             ti.name};
  auto byte_size = numel * float_size;

  if (byte_offset > std::numeric_limits<std::size_t>::max() - byte_size)
    throw std::runtime_error{"pth_loader: tensor byte range overflow: " +
                             ti.name};
  if (byte_offset + byte_size > raw_size)
    throw std::runtime_error{"pth_loader: tensor data out of bounds: " +
                             ti.name};

  return {.numel = numel, .byte_offset = byte_offset, .byte_size = byte_size};
}

}  // namespace pth_detail

// Load a PyTorch .pth file (torch.save'd state_dict).
// Returns a pth_file with zero-copy tensor data spans into the mmap.
inline pth_file load_pth(std::string_view path) {
  zip_reader zip{path};

  // Determine the common prefix (e.g. "s5f-libtorch/").
  std::string pfx{zip.prefix()};

  // Read and parse the pickle.
  auto pkl_data = zip.get_by_suffix("data.pkl");
  auto info = parse_state_dict(pkl_data);

  // Read byte order.  PyTorch's byteorder entry makes the raw float storage
  // interpretation explicit; reject missing/unknown byteorder rather than
  // silently assuming host endianness.
  if (!zip.contains_suffix("byteorder"))
    throw std::runtime_error{"pth_loader: missing byteorder entry"};
  auto bo = zip.get_by_suffix("byteorder");
  std::string_view bo_str{reinterpret_cast<const char*>(bo.data()), bo.size()};
  // Trim whitespace/newlines.
  while (!bo_str.empty() && (bo_str.back() == '\n' || bo_str.back() == '\r' ||
                             bo_str.back() == ' '))
    bo_str.remove_suffix(1);

  if (bo_str != "little" && bo_str != "big")
    throw std::runtime_error{"pth_loader: unsupported byteorder entry: " +
                             std::string{bo_str}};

  if constexpr (std::endian::native == std::endian::little) {
    if (bo_str != "little")
      throw std::runtime_error{
          "pth_loader: model is big-endian, host is little-endian"};
  } else {
    if (bo_str != "big")
      throw std::runtime_error{
          "pth_loader: model is little-endian, host is big-endian"};
  }

  // Build tensor_data entries.
  std::vector<tensor_data> tensors;
  tensors.reserve(info.tensors.size());

  for (auto& ti : info.tensors) {
    // Look up the raw data file in the ZIP.
    std::string data_path = pfx + "data/" + ti.storage_key;
    auto raw = zip.get(data_path);

    auto layout = pth_detail::checked_tensor_layout(ti, raw.size());

    // Verify alignment.
    const auto* base = raw.data() + layout.byte_offset;
    if (layout.byte_size != 0 &&
        reinterpret_cast<std::uintptr_t>(base) % alignof(float) != 0)
      throw std::runtime_error{"pth_loader: unaligned tensor data: " + ti.name};

    tensors.push_back({
        .name = std::move(ti.name),
        .shape = std::move(ti.shape),
        .data = {reinterpret_cast<const float*>(base), layout.numel},
    });
  }

  return pth_file{
      .zip = std::move(zip),
      .prefix = std::move(pfx),
      .tensors = std::move(tensors),
  };
}

}  // namespace larch
