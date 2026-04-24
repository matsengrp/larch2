#pragma once

#include <larch/compact_genome.hpp>
#include <larch/compute.hpp>
#include <larch/io_util.hpp>
#include <larch/nuc.hpp>

#include <charconv>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

struct vcf_data {
  std::vector<std::string> sample_names;
  std::map<std::string, compact_genome> sample_genomes;
};

inline vcf_data read_vcf(std::string_view path, std::string_view reference) {
  auto bytes = read_file(path);
  std::string_view content{bytes.data(), bytes.size()};

  vcf_data result;
  // Accumulate per-sample mutations/IUPAC state sets, convert to compact_genome
  // at end.
  std::map<std::string, std::map<mutation_position, nuc_base>> sample_muts;
  std::map<std::string, ambiguity_set_map> sample_ambiguity;
  ambiguity_counts ambiguity;

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
    std::size_t site_pos = 0;
    std::from_chars(cols[1].data(), cols[1].data() + cols[1].size(), site_pos);
    auto mpos = static_cast<mutation_position>(site_pos);

    // Per-sample genotype
    for (std::size_t i = 0;
         i < result.sample_names.size() && i + 9 < cols.size(); ++i) {
      auto gt = cols[i + 9];
      // Parse GT: take first allele index (before / or |)
      int allele_idx = 0;
      if (!gt.empty() && gt[0] >= '0' && gt[0] <= '9') allele_idx = gt[0] - '0';

      char allele_char = ref_allele[0];
      if (allele_idx > 0 &&
          static_cast<std::size_t>(allele_idx - 1) < alt_alleles.size()) {
        allele_char = alt_alleles[allele_idx - 1][0];
      }
      auto state_set = iupac_state_set(allele_char);

      ambiguity.observed_sites++;
      if (state_set != 0 && std::popcount(state_set) != 1) {
        sample_muts[result.sample_names[i]].erase(mpos);
        sample_ambiguity[result.sample_names[i]][mpos] = state_set;
        ambiguity.add(allele_char);
        continue;
      }

      // Record mutation if different from reference
      nuc_base sample_base = nuc_base::from_char(allele_char);
      nuc_base ref_base = nuc_base::from_char(reference.at(mpos - 1));
      sample_ambiguity[result.sample_names[i]].erase(mpos);
      if (!(sample_base == ref_base)) {
        sample_muts[result.sample_names[i]][mpos] = sample_base;
      }
    }
  }

  warn_if_ambiguities(ambiguity, "VCF genotypes");

  // Convert accumulated mutations/IUPAC state sets to compact_genomes.
  for (auto& [name, muts] : sample_muts) {
    auto amb_it = sample_ambiguity.find(name);
    ambiguity_set_map ambiguity_sets;
    if (amb_it != sample_ambiguity.end())
      ambiguity_sets = std::move(amb_it->second);
    result.sample_genomes[name] =
        compact_genome{std::move(muts), std::move(ambiguity_sets)};
  }
  for (auto& [name, ambiguity_sets] : sample_ambiguity) {
    if (!result.sample_genomes.contains(name)) {
      result.sample_genomes[name] =
          compact_genome{{}, std::move(ambiguity_sets)};
    }
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

            // Build new compact genome: start from existing CG, overlay VCF
            // concrete calls, then apply VCF IUPAC state sets.
            std::map<mutation_position, nuc_base> muts;
            for (auto [pos, base] : node.cg()) muts[pos] = base;
            ambiguity_set_map ambiguity_sets = node.cg().ambiguity_sets();
            for (auto [pos, base] : it->second) {
              ambiguity_sets.erase(pos);
              auto ref_base = nuc_base::from_char(ref.at(pos - 1));
              if (base == ref_base)
                muts.erase(pos);
              else
                muts[pos] = base;
            }
            for (auto [pos, state_set] : it->second.ambiguity_sets()) {
              muts.erase(pos);
              ambiguity_sets[pos] = state_set;
            }
            node.cg() =
                compact_genome{std::move(muts), std::move(ambiguity_sets)};
          }
        },
        nv);
  }

  recompute_edge_mutations(d);
}

}  // namespace larch
