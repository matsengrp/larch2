#pragma once

#include <larch/compact_genome.hpp>
#include <larch/compute.hpp>
#include <larch/io_util.hpp>
#include <larch/nuc.hpp>

#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <string_view>
#include <vector>

namespace larch {

namespace vcf_detail {

inline nuc_base strict_nuc_from_char(char c, std::string_view label,
                                     mutation_position pos) {
  switch (c) {
    case 'A':
    case 'a':
      return nuc_base{nuc_base::A};
    case 'C':
    case 'c':
      return nuc_base{nuc_base::C};
    case 'G':
    case 'g':
      return nuc_base{nuc_base::G};
    case 'T':
    case 't':
      return nuc_base{nuc_base::T};
    default:
      throw std::runtime_error(std::string{"VCF: non-ACGT "} +
                               std::string{label} + " nucleotide '" + c +
                               "' at position " + std::to_string(pos));
  }
}

inline void validate_reference(std::string_view reference) {
  for (std::size_t i = 0; i < reference.size(); ++i)
    (void)strict_nuc_from_char(reference[i], "reference", i + 1);
}

inline mutation_position parse_position(std::string_view text) {
  std::size_t site_pos = 0;
  auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), site_pos);
  if (ec != std::errc{} || ptr != text.data() + text.size() || site_pos == 0) {
    throw std::runtime_error("VCF: invalid positive integer POS field '" +
                             std::string{text} + "'");
  }
  return static_cast<mutation_position>(site_pos);
}

}  // namespace vcf_detail

struct vcf_data {
  std::vector<std::string> sample_names;
  std::map<std::string, compact_genome> sample_genomes;
};

inline vcf_data read_vcf(std::string_view path, std::string_view reference) {
  vcf_detail::validate_reference(reference);
  auto bytes = read_file(path);
  std::string_view content{bytes.data(), bytes.size()};

  vcf_data result;
  // Accumulate per-sample mutations, convert to compact_genome at end
  std::map<std::string, std::map<mutation_position, nuc_base>> sample_muts;

  std::size_t pos = 0;
  while (pos < content.size()) {
    auto eol = content.find('\n', pos);
    if (eol == std::string_view::npos) eol = content.size();
    auto line = content.substr(pos, eol - pos);
    pos = eol + 1;

    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;

    // Skip meta-information lines
    if (line.starts_with("##")) continue;

    // Split line by tabs
    std::vector<std::string_view> cols;
    std::size_t p = 0;
    while (p < line.size()) {
      auto tab = line.find('\t', p);
      if (tab == std::string_view::npos) tab = line.size();
      cols.push_back(line.substr(p, tab - p));
      p = tab + 1;
    }

    // Parse #CHROM header for sample names (columns 9+)
    if (line.starts_with("#CHROM")) {
      for (std::size_t i = 9; i < cols.size(); ++i)
        result.sample_names.emplace_back(cols[i]);
      continue;
    }

    if (cols.size() < 10) continue;

    // Only handle SNPs (single-char REF and single-char ALT alleles)
    auto ref_allele = cols[3];
    auto alt_field = cols[4];
    if (ref_allele.size() != 1) continue;

    // Parse ALT alleles (comma-separated)
    std::vector<std::string_view> alt_alleles;
    p = 0;
    while (p < alt_field.size()) {
      auto comma = alt_field.find(',', p);
      if (comma == std::string_view::npos) comma = alt_field.size();
      alt_alleles.push_back(alt_field.substr(p, comma - p));
      p = comma + 1;
    }

    // Check all alleles are single char (SNP)
    bool all_snp = true;
    for (auto a : alt_alleles) {
      if (a.size() != 1) {
        all_snp = false;
        break;
      }
    }
    if (!all_snp) continue;

    // Parse position (1-indexed)
    auto mpos = vcf_detail::parse_position(cols[1]);
    if (mpos > reference.size()) {
      throw std::runtime_error("VCF: POS " + std::to_string(mpos) +
                               " outside reference length " +
                               std::to_string(reference.size()));
    }

    (void)vcf_detail::strict_nuc_from_char(ref_allele[0], "REF allele", mpos);
    for (auto allele : alt_alleles) {
      (void)vcf_detail::strict_nuc_from_char(allele[0], "ALT allele", mpos);
    }

    // Per-sample genotype
    for (std::size_t i = 0;
         i < result.sample_names.size() && i + 9 < cols.size(); ++i) {
      auto gt = cols[i + 9];
      // Parse GT: take first allele index (before / or |)
      int allele_idx = 0;
      if (!gt.empty() && gt[0] >= '0' && gt[0] <= '9') allele_idx = gt[0] - '0';

      nuc_base sample_base =
          vcf_detail::strict_nuc_from_char(ref_allele[0], "REF allele", mpos);
      if (allele_idx > 0 &&
          static_cast<std::size_t>(allele_idx - 1) < alt_alleles.size()) {
        sample_base = vcf_detail::strict_nuc_from_char(
            alt_alleles[allele_idx - 1][0], "ALT allele", mpos);
      }

      // Record mutation if different from reference
      nuc_base ref_base = vcf_detail::strict_nuc_from_char(
          reference.at(mpos - 1), "reference", mpos);
      if (!(sample_base == ref_base)) {
        sample_muts[result.sample_names[i]][mpos] = sample_base;
      }
    }
  }

  // Convert accumulated mutations to compact_genomes
  for (auto& [name, muts] : sample_muts) {
    result.sample_genomes[name] = compact_genome{std::move(muts)};
  }

  return result;
}

inline void apply_vcf_to_dag(phylo_dag& d, vcf_data const& vcf) {
  auto const& ref = get_reference_sequence(d);

  for (auto nv : d.get_all_nodes()) {
    std::visit(
        [&](auto node) {
          if constexpr (requires {
                          node.sample_id();
                          node.cg();
                        }) {
            auto it = vcf.sample_genomes.find(node.sample_id());
            if (it == vcf.sample_genomes.end()) return;

            // Build new mutations map: start from existing CG, overlay VCF
            std::map<mutation_position, nuc_base> muts;
            for (auto [pos, base] : node.cg()) muts[pos] = base;
            for (auto [pos, base] : it->second) {
              if (pos == 0 || pos > ref.size()) {
                throw std::runtime_error(
                    "VCF: compact-genome mutation position " +
                    std::to_string(pos) + " for sample '" +
                    std::string{node.sample_id()} +
                    "' outside reference length " + std::to_string(ref.size()));
              }
              auto ref_base = vcf_detail::strict_nuc_from_char(
                  ref.at(pos - 1), "reference", pos);
              if (base == ref_base)
                muts.erase(pos);
              else
                muts[pos] = base;
            }
            node.cg() = compact_genome{std::move(muts)};
          }
        },
        nv);
  }

  recompute_edge_mutations(d);
}

}  // namespace larch
