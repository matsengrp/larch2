// vcf_test.cpp — VCF loading tests.
//
// Regression test for the rotavirusA parsimony discrepancy: faToVcf encodes
// IUPAC-ambiguous sample bases as single-character ALT alleles (e.g. 'K').
// The reader must treat those like missing data ('.') and must not coerce
// them through nuc_base::from_char, which silently maps any non-ACGT
// character to 'A' and fabricates a mutation.

#include <larch/vcf.hpp>
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace {

class temp_dir {
 public:
  explicit temp_dir(std::string_view name)
      : path_{larch::test::unique_temp_path(name, "")} {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~temp_dir() { std::filesystem::remove_all(path_); }

  [[nodiscard]] std::filesystem::path const& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

void write_text(std::filesystem::path const& path, std::string_view contents) {
  std::ofstream out{path};
  out << contents;
}

void test_basic_snp_loading() {
  std::println("test_basic_snp_loading");
  temp_dir tmp{"vcf_test"};

  std::string_view reference = "ACGTACGT";
  auto const vcf_path = tmp.path() / "basic.vcf";
  write_text(vcf_path,
             "##fileformat=VCFv4.2\n"
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\ts1\ts2\n"
             "ref\t2\tC2G\tC\tG\t.\t.\t.\tGT\t1\t0\n"
             "ref\t4\tT4A\tT\tA\t.\t.\t.\tGT\t0\t1\n");

  auto vcf = larch::read_vcf(vcf_path.string(), reference);

  larch::test::require(vcf.sample_names.size() == 2, "two samples parsed");
  larch::test::require(vcf.sample_names[0] == "s1" &&
                           vcf.sample_names[1] == "s2",
                       "sample names parsed");

  auto& s1 = vcf.sample_genomes.at("s1");
  auto& s2 = vcf.sample_genomes.at("s2");
  larch::test::require(s1.to_string() == "<2G,>", "s1 has 2G only");
  larch::test::require(s2.to_string() == "<4A,>", "s2 has 4A only");
  std::println("  PASS");
}

void test_ambiguous_alt_allele_is_skipped() {
  std::println("test_ambiguous_alt_allele_is_skipped");
  temp_dir tmp{"vcf_test"};

  // faToVcf-style records for samples whose base is ambiguous (K = G/T).
  std::string_view reference = "ACGTACGT";
  auto const vcf_path = tmp.path() / "ambiguous.vcf";
  write_text(vcf_path,
             "##fileformat=VCFv4.2\n"
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT"
             "\tamb\tother\n"
             "ref\t4\tG4K\tG\tK,C\t.\t.\t.\tGT\t1\t2\n"
             "ref\t6\tC6R\tC\tR\t.\t.\t.\tGT\t1\t.\n");

  auto vcf = larch::read_vcf(vcf_path.string(), reference);

  // 'K' and 'R' are ambiguous: no mutation must be recorded for 'amb'.
  // Previously from_char('K') fabricated a G>A mutation here.
  larch::test::require(vcf.sample_genomes.find("amb") ==
                           vcf.sample_genomes.end(),
                       "ambiguous ALT records no mutation");
  // The unambiguous allele on the same (multi-allelic) record still applies
  // to 'other': only the ambiguous sample is skipped, not the whole record.
  larch::test::require(vcf.sample_genomes.at("other").to_string() == "<4C,>",
                       "unambiguous allele on mixed record kept");
  std::println("  PASS");
}

void test_missing_genotype_is_no_call() {
  std::println("test_missing_genotype_is_no_call");
  temp_dir tmp{"vcf_test"};

  // '.' GT (faToVcf writes it for N bases) must record no mutation, even if
  // the record's REF column disagrees with the DAG reference.
  std::string_view reference = "ACGT";
  auto const vcf_path = tmp.path() / "missing.vcf";
  write_text(vcf_path,
             "##fileformat=VCFv4.2\n"
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\ts1\n"
             "ref\t2\tC2T\tC\tT\t.\t.\t.\tGT\t.\n");

  auto vcf = larch::read_vcf(vcf_path.string(), reference);
  larch::test::require(vcf.sample_genomes.find("s1") == vcf.sample_genomes.end(),
                       "missing GT records no mutation");
  std::println("  PASS");
}

void test_lowercase_alleles() {
  std::println("test_lowercase_alleles");
  temp_dir tmp{"vcf_test"};

  std::string_view reference = "ACGT";
  auto const vcf_path = tmp.path() / "lower.vcf";
  write_text(vcf_path,
             "##fileformat=VCFv4.2\n"
             "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\ts1\n"
             "ref\t1\tA1g\tA\tg\t.\t.\t.\tGT\t1\n");

  auto vcf = larch::read_vcf(vcf_path.string(), reference);
  larch::test::require(vcf.sample_genomes.at("s1").to_string() == "<1G,>",
                       "lowercase allele accepted");
  std::println("  PASS");
}

}  // namespace

int main() {
  std::println("vcf_test");
  test_basic_snp_loading();
  test_ambiguous_alt_allele_is_skipped();
  test_missing_genotype_is_no_call();
  test_lowercase_alleles();
  std::println("vcf_test all passed");
  return 0;
}
