#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace larch {

// K-mer sequence encoder for neural network model input.
// Converts DNA sequences into k-mer index arrays and wild-type base modifier
// arrays. Ported from netam::kmer_sequence_encoder (larch-rm-optimize).
class kmer_encoder {
  std::size_t kmer_length_;
  std::size_t site_count_;
  std::size_t overhang_length_;
  std::vector<std::string> all_kmers_;
  std::unordered_map<std::string, int32_t> kmer_to_index_;

  static constexpr char const* BASES = "ACGT";
  static constexpr float BIG = 1e9f;

  static void generate_kmers_impl(std::vector<std::string>& out,
                                  std::string& current, std::size_t length) {
    if (current.size() == length) {
      out.push_back(current);
      return;
    }
    for (int i = 0; i < 4; ++i) {
      current.push_back(BASES[i]);
      generate_kmers_impl(out, current, length);
      current.pop_back();
    }
  }

  static std::vector<std::string> generate_kmers(std::size_t length) {
    std::vector<std::string> kmers;
    std::string buf;
    generate_kmers_impl(kmers, buf, length);
    // Placeholder for N-containing kmers at the end (index 4^k).
    kmers.push_back("N");
    return kmers;
  }

 public:
  // Result of encode_sequence().
  struct encoded_seq {
    std::vector<int32_t> kmer_indices;  // [site_count]
    std::vector<float> wt_modifier;     // [site_count * 4], row-major
  };

  kmer_encoder(std::size_t kmer_length, std::size_t site_count)
      : kmer_length_{kmer_length},
        site_count_{site_count},
        overhang_length_{(kmer_length - 1) / 2},
        all_kmers_{generate_kmers(kmer_length)},
        kmer_to_index_{[this] {
          std::unordered_map<std::string, int32_t> m;
          for (int32_t i = 0; i < static_cast<int32_t>(all_kmers_.size()); ++i)
            m[all_kmers_[static_cast<std::size_t>(i)]] = i;
          return m;
        }()} {}

  std::size_t kmer_count() const { return all_kmers_.size(); }
  std::size_t kmer_length() const { return kmer_length_; }
  std::size_t site_count() const { return site_count_; }

  // Encode a DNA sequence into k-mer indices and wild-type base modifier.
  encoded_seq encode_sequence(std::string_view seq) const {
    // Uppercase.
    std::string upper(seq.size(), '\0');
    std::transform(seq.begin(), seq.end(), upper.begin(), [](char c) {
      return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });

    // Pad with N on both sides.
    std::string padded(overhang_length_, 'N');
    padded += upper;
    padded.append(overhang_length_, 'N');

    int32_t n_index = static_cast<int32_t>(all_kmers_.size() - 1);

    // Encode k-mer indices.
    std::vector<int32_t> indices(site_count_);
    for (std::size_t i = 0; i < site_count_; ++i) {
      if (i + kmer_length_ <= padded.size()) {
        std::string kmer = padded.substr(i, kmer_length_);
        auto it = kmer_to_index_.find(kmer);
        indices[i] = (it != kmer_to_index_.end()) ? it->second : n_index;
      } else {
        indices[i] = n_index;
      }
    }

    // Compute wild-type base modifier [site_count * 4].
    // For each position, set -BIG at the index of the parent base.
    std::vector<float> wt(site_count_ * 4, 0.0f);
    std::size_t len = std::min(upper.size(), site_count_);
    for (std::size_t i = 0; i < len; ++i) {
      auto idx = base_index(upper[i]);
      if (idx < 4) {
        wt[i * 4 + idx] = -BIG;
      }
    }

    return {std::move(indices), std::move(wt)};
  }

  // Encode a sequence as base indices: A=0, C=1, G=2, T=3, other=4.
  // Used for likelihood computation (parent/child comparison).
  static std::vector<int64_t> encode_bases(std::string_view seq) {
    std::vector<int64_t> result;
    result.reserve(seq.size());
    for (char c : seq) {
      result.push_back(static_cast<int64_t>(base_index(
          static_cast<char>(std::toupper(static_cast<unsigned char>(c))))));
    }
    return result;
  }

  // Single base -> index mapping.
  static std::size_t base_index(char base) {
    switch (base) {
      case 'A':
        return 0;
      case 'C':
        return 1;
      case 'G':
        return 2;
      case 'T':
        return 3;
      default:
        return 4;
    }
  }
};

}  // namespace larch
