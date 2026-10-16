#pragma once

#include <larch/compact_genome.hpp>
#include <larch/compute.hpp>
#include <larch/io_util.hpp>
#include <larch/nuc.hpp>

#include <cctype>
#include <charconv>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

struct vcf_data {
  std::vector<std::string> sample_names;
  std::map<std::string, compact_genome> sample_genomes;
  // Sites where the VCF REF allele disagrees with the reference sequence the
  // caller supplied (i.e. the VCF was generated against a different
  // reference).  Populated for diagnostics; read_vcf warns when non-empty.
  std::vector<mutation_position> ref_mismatch_sites;
};

inline vcf_data read_vcf(std::string_view path, std::string_view reference) {
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

    // Skip records with an ambiguous REF allele: sites where even the
    // reference base is an IUPAC code cannot be interpreted.
    if (!nuc_base::is_acgt(ref_allele[0])) continue;

    // Parse position (1-indexed)
    std::size_t site_pos = 0;
    std::from_chars(cols[1].data(), cols[1].data() + cols[1].size(), site_pos);
    auto mpos = static_cast<mutation_position>(site_pos);

    // Track REF-column disagreements with the supplied reference: applying a
    // VCF generated against a different reference silently corrupts leaf
    // sequence reconstruction.
    if (mpos >= 1 && static_cast<std::size_t>(mpos) <= reference.size()) {
      char ref_seq_char =
          reference.at(static_cast<std::size_t>(mpos) - 1);
      char vcf_ref_char = ref_allele[0];
      if (std::tolower(static_cast<unsigned char>(ref_seq_char)) !=
          std::tolower(static_cast<unsigned char>(vcf_ref_char))) {
        result.ref_mismatch_sites.push_back(mpos);
      }
    }

    // Per-sample genotype
    for (std::size_t i = 0;
         i < result.sample_names.size() && i + 9 < cols.size(); ++i) {
      auto gt = cols[i + 9];
      // Missing genotype ('.') is a no-call: the sample base at this site
      // is unknown (faToVcf writes it for N bases), so record no mutation.
      if (!gt.empty() && gt[0] == '.') continue;
      // Parse GT: take first allele index (before / or |)
      int allele_idx = 0;
      if (!gt.empty() && gt[0] >= '0' && gt[0] <= '9') allele_idx = gt[0] - '0';

      nuc_base sample_base = nuc_base::from_char(ref_allele[0]);
      if (allele_idx > 0 &&
          static_cast<std::size_t>(allele_idx - 1) < alt_alleles.size()) {
        char alt_char = alt_alleles[allele_idx - 1][0];
        // Ambiguous ALT allele (IUPAC code such as K or R, emitted by faToVcf
        // for ambiguous sample bases): treat like missing data (./. ) and
        // record no mutation. from_char() would silently coerce it to A and
        // fabricate a mutation.
        if (!nuc_base::is_acgt(alt_char)) continue;
        sample_base = nuc_base::from_char(alt_char);
      }

      // Record mutation if different from reference
      nuc_base ref_base = nuc_base::from_char(reference.at(mpos - 1));
      if (!(sample_base == ref_base)) {
        sample_muts[result.sample_names[i]][mpos] = sample_base;
      }
    }
  }

  // Convert accumulated mutations to compact_genomes
  for (auto& [name, muts] : sample_muts) {
    result.sample_genomes[name] = compact_genome{std::move(muts)};
  }

  if (!result.ref_mismatch_sites.empty()) {
    std::cerr << "warning: VCF REF allele disagrees with the reference "
                 "sequence at "
              << result.ref_mismatch_sites.size() << " sites (e.g. position "
              << result.ref_mismatch_sites.front()
              << "); was the VCF generated against a different reference?\n";
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
              auto ref_base = nuc_base::from_char(ref.at(pos - 1));
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
