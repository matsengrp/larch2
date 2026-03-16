#pragma once

#include <larch/io_util.hpp>

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

struct fasta_entry {
  std::string name;
  std::string sequence;
};

inline std::vector<fasta_entry> read_fasta(std::string_view path) {
  auto bytes = read_file(path);
  std::string_view content{bytes.data(), bytes.size()};

  std::vector<fasta_entry> entries;
  fasta_entry* current = nullptr;

  std::size_t pos = 0;
  while (pos < content.size()) {
    auto eol = content.find('\n', pos);
    if (eol == std::string_view::npos) eol = content.size();
    auto line = content.substr(pos, eol - pos);
    pos = eol + 1;

    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;

    if (line[0] == '>') {
      entries.push_back({});
      current = &entries.back();
      line.remove_prefix(1);
      // Trim leading/trailing whitespace
      while (!line.empty() && line[0] == ' ') line.remove_prefix(1);
      while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
      current->name = std::string{line};
    } else if (current) {
      for (char c : line) {
        if (c != ' ' && c != '\t')
          current->sequence +=
              static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
    }
  }

  return entries;
}

}  // namespace larch
