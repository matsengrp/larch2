#pragma once

#include <larch/pickle_reader.hpp>
#include <larch/zip_reader.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

// Load a PyTorch .pth file (torch.save'd state_dict).
// Returns a pth_file with zero-copy tensor data spans into the mmap.
inline pth_file load_pth(std::string_view path) {
  zip_reader zip{path};

  // Determine the common prefix (e.g. "s5f-libtorch/").
  std::string pfx{zip.prefix()};

  // Read and parse the pickle.
  auto pkl_data = zip.get_by_suffix("data.pkl");
  auto info = parse_state_dict(pkl_data);

  // Read byte order.
  if (zip.contains_suffix("byteorder")) {
    auto bo = zip.get_by_suffix("byteorder");
    std::string_view bo_str{reinterpret_cast<const char*>(bo.data()),
                            bo.size()};
    // Trim whitespace/newlines.
    while (!bo_str.empty() && (bo_str.back() == '\n' || bo_str.back() == '\r' ||
                               bo_str.back() == ' '))
      bo_str.remove_suffix(1);

    if constexpr (std::endian::native == std::endian::little) {
      if (bo_str != "little")
        throw std::runtime_error{
            "pth_loader: model is big-endian, host is little-endian"};
    } else {
      if (bo_str != "big")
        throw std::runtime_error{
            "pth_loader: model is little-endian, host is big-endian"};
    }
  }

  // Build tensor_data entries.
  std::vector<tensor_data> tensors;
  tensors.reserve(info.tensors.size());

  for (auto& ti : info.tensors) {
    // Look up the raw data file in the ZIP.
    std::string data_path = pfx + "data/" + ti.storage_key;
    auto raw = zip.get(data_path);

    // Compute expected size from shape.
    int64_t numel = 1;
    for (auto d : ti.shape) numel *= d;

    std::size_t byte_offset =
        static_cast<std::size_t>(ti.storage_offset) * sizeof(float);
    std::size_t byte_size = static_cast<std::size_t>(numel) * sizeof(float);

    if (byte_offset + byte_size > raw.size())
      throw std::runtime_error{"pth_loader: tensor data out of bounds: " +
                               ti.name};

    // Verify alignment.
    const auto* base = raw.data() + byte_offset;
    if (reinterpret_cast<std::uintptr_t>(base) % alignof(float) != 0)
      throw std::runtime_error{"pth_loader: unaligned tensor data: " + ti.name};

    tensors.push_back({
        .name = std::move(ti.name),
        .shape = std::move(ti.shape),
        .data = {reinterpret_cast<const float*>(base),
                 static_cast<std::size_t>(numel)},
    });
  }

  return pth_file{
      .zip = std::move(zip),
      .prefix = std::move(pfx),
      .tensors = std::move(tensors),
  };
}

}  // namespace larch
