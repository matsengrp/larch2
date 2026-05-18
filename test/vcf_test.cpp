#include <larch/compute.hpp>
#include <larch/vcf.hpp>

#include "test_util.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

void write_text(std::filesystem::path const& path, std::string_view contents) {
  std::ofstream out{path};
  out << contents;
  assert(out.good());
}

bool throws_runtime_error(auto&& f) {
  try {
    f();
  } catch (std::runtime_error const&) {
    return true;
  }
  return false;
}

std::string leaf_sequence(larch::phylo_dag& dag, std::string_view sample_id) {
  for (auto nv : dag.get_all_nodes()) {
    std::string result;
    bool found = false;
    std::visit(
        [&](auto node) {
          if constexpr (requires { node.sample_id(); }) {
            if (node.sample_id() == sample_id) {
              result = larch::test::node_sequence(dag, node.index());
              found = true;
            }
          }
        },
        nv);
    if (found) return result;
  }
  throw std::runtime_error("missing leaf");
}

}  // namespace

int main() {
  auto tmp = larch::test::unique_temp_path("vcf_test", "");
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);

  auto valid_path = tmp / "valid.vcf";
  write_text(valid_path,
             "##fileformat=VCFv4.2\n"
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tA\tB\n"
             "chr1\t2\t.\tA\tC\t.\t.\t.\tGT\t1\t0\n");

  auto vcf = larch::read_vcf(valid_path.string(), "AAAA");
  auto dag = larch::test::make_tiny_labelled_tree(
      "AAAA", larch::test::tiny_inner(
                  "root", "AAAA", {larch::test::tiny_leaf("A", "AAAA"),
                                      larch::test::tiny_leaf("B", "AAAA")}));
  larch::apply_vcf_to_dag(dag, vcf);
  assert(leaf_sequence(dag, "A") == "ACAA");
  assert(leaf_sequence(dag, "B") == "AAAA");

  auto bad_alt_path = tmp / "bad_alt.vcf";
  write_text(bad_alt_path,
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tA\n"
             "chr1\t2\t.\tA\tN\t.\t.\t.\tGT\t1\n");
  assert(throws_runtime_error([&] {
    (void)larch::read_vcf(bad_alt_path.string(), "AAAA");
  }));

  auto bad_ref_allele_path = tmp / "bad_ref_allele.vcf";
  write_text(bad_ref_allele_path,
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\tA\n"
             "chr1\t2\t.\tN\tC\t.\t.\t.\tGT\t0\n");
  assert(throws_runtime_error([&] {
    (void)larch::read_vcf(bad_ref_allele_path.string(), "AAAA");
  }));

  assert(throws_runtime_error([&] {
    (void)larch::read_vcf(valid_path.string(), "AANA");
  }));

  std::filesystem::remove_all(tmp);
  std::println("vcf test passed");
}
