// larch2-bcr: Load multiple FASTA files + Newick trees, merge, save as DAG.
//
// Usage:
//   larch2-bcr --fasta FILE [--fasta FILE ...] --trees DIR --reference FILE -o
//   FILE
//              [--tree-suffix SUFFIX]

#include <larch/build_fasta_newick.hpp>
#include <larch/merge.hpp>
#include <larch/save_proto_dag.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace larch;

static void usage() {
  std::cerr
      << R"(larch2-bcr -- load and merge BCR trees from FASTA + Newick files

Usage:
  larch2-bcr --fasta FILE [--fasta FILE ...] --trees DIR --reference FILE -o FILE

Options:
  --fasta FILE        Path to FASTA file (can be specified multiple times)
  --trees DIR         Directory containing Newick tree files (*.treefile)
  --reference FILE    Path to reference sequence file (FASTA or raw text)
  -o, --output FILE   Path to output DAG protobuf file
  --tree-suffix STR   Suffix filter for tree files (default: -rerooted.treefile)
)";
}

int main(int argc, char** argv) try {
  std::vector<std::string> fasta_paths;
  std::string trees_dir;
  std::string reference_path;
  std::string output_path;
  std::string tree_suffix = "-rerooted.treefile";

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    auto next = [&]() -> std::string_view {
      if (++i >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(1);
      }
      return argv[i];
    };
    if (arg == "--fasta")
      fasta_paths.emplace_back(next());
    else if (arg == "--trees")
      trees_dir = next();
    else if (arg == "--reference")
      reference_path = next();
    else if (arg == "-o" || arg == "--output")
      output_path = next();
    else if (arg == "--tree-suffix")
      tree_suffix = next();
    else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      usage();
      return 1;
    }
  }

  if (fasta_paths.empty()) {
    std::cerr << "error: at least one --fasta is required\n";
    return 1;
  }
  if (trees_dir.empty()) {
    std::cerr << "error: --trees is required\n";
    return 1;
  }
  if (reference_path.empty()) {
    std::cerr << "error: --reference is required\n";
    return 1;
  }
  if (output_path.empty()) {
    std::cerr << "error: -o/--output is required\n";
    return 1;
  }

  // Read reference sequence (may be FASTA or raw text).
  auto ref_bytes = read_file(reference_path);
  std::string_view ref_content{ref_bytes.data(), ref_bytes.size()};
  std::string reference;
  if (!ref_content.empty() && ref_content[0] == '>') {
    auto entries = read_fasta(reference_path);
    if (!entries.empty()) reference = std::move(entries[0].sequence);
  } else {
    for (char c : ref_content) {
      if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        reference +=
            static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }
  if (reference.empty()) {
    std::cerr << "error: empty reference sequence from " << reference_path
              << "\n";
    return 1;
  }
  std::cerr << "Reference sequence: " << reference.size() << " bases\n";

  // Read all FASTA files into a combined map.
  std::unordered_map<std::string, std::string> fasta_map;
  for (auto const& path : fasta_paths) {
    auto entries = read_fasta(path);
    std::cerr << "  " << path << ": " << entries.size() << " sequences\n";
    for (auto& e : entries) fasta_map[e.name] = std::move(e.sequence);
  }
  std::cerr << "Total sequences: " << fasta_map.size() << "\n";

  // Scan trees directory for matching Newick files.
  std::vector<std::string> tree_paths;
  for (auto const& entry : std::filesystem::directory_iterator{trees_dir}) {
    if (entry.path().extension() != ".treefile") continue;
    auto filename = entry.path().filename().string();
    // Check if filename contains the suffix stem (before the extension).
    auto suffix_stem = tree_suffix.substr(0, tree_suffix.find('.'));
    if (filename.find(suffix_stem) == std::string::npos) continue;
    tree_paths.push_back(entry.path().string());
  }
  std::sort(tree_paths.begin(), tree_paths.end());

  if (tree_paths.empty()) {
    std::cerr << "error: no tree files matching '" << tree_suffix << "' in "
              << trees_dir << "\n";
    return 1;
  }
  std::cerr << "Found " << tree_paths.size() << " tree files\n";

  // Build each tree and merge.
  merge m(reference);
  for (auto const& tp : tree_paths) {
    std::cerr << "  Loading: " << tp << "\n";
    auto tree = build_from_fasta_newick(fasta_map, tp, reference);
    m.add_dag(std::move(tree));
  }

  auto& result = m.get_result();
  std::cerr << "Merged DAG: " << node_count(result) << " nodes, "
            << edge_count(result) << " edges\n";

  // Save.
  save_proto_dag(result, output_path);
  std::cerr << "Saved to: " << output_path << "\n";

  return 0;
} catch (std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return 1;
}
