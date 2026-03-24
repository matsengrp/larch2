#pragma once

#include <larch/kmer_encoder.hpp>
#include <larch/likelihood.hpp>
#include <larch/pth_loader.hpp>
#include <larch/yaml_reader.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

// RSFivemerModel: per-k-mer embedding model for mutation rates and conditional
// substitution probabilities.  Forward pass is a simple embedding lookup:
//   rates[i] = exp(r_kmer_embedding[kmer_idx[i]])
//   csp[i]   = softmax(s_kmer_embedding[kmer_idx[i]] + wt_modifier[i])
class rs_fivemer_model {
  pth_file pth_;
  kmer_encoder encoder_;
  float const* r_weights_;      // [kmer_count] log-rates (column vector, col 0)
  float const* s_weights_;      // [kmer_count * 4] substitution logits
  double rate_bias_log_ = 0.0;  // additive log-bias applied to all rates

 public:
  struct forward_result {
    std::vector<float> rates;  // [site_count]
    std::vector<float> csp;    // [site_count * 4]
  };

  rs_fivemer_model(pth_file pth, kmer_encoder encoder)
      : pth_{std::move(pth)}, encoder_{std::move(encoder)} {
    auto& r = pth_.get("r_kmer_embedding.weight");
    if (r.shape.size() != 2 ||
        static_cast<std::size_t>(r.shape[0]) != encoder_.kmer_count() ||
        r.shape[1] != 1)
      throw std::runtime_error{
          "rs_fivemer_model: unexpected r_kmer_embedding.weight shape"};
    r_weights_ = r.data.data();

    auto& s = pth_.get("s_kmer_embedding.weight");
    if (s.shape.size() != 2 ||
        static_cast<std::size_t>(s.shape[0]) != encoder_.kmer_count() ||
        s.shape[1] != 4)
      throw std::runtime_error{
          "rs_fivemer_model: unexpected s_kmer_embedding.weight shape"};
    s_weights_ = s.data.data();
  }

  // Load from a model directory and model name.
  // Expects {dir}/{name}-libtorch.pth and {dir}/{name}.yml.
  static rs_fivemer_model load(std::string_view dir, std::string_view name) {
    auto pth_path =
        std::string{dir} + "/" + std::string{name} + "-libtorch.pth";
    auto yml_path = std::string{dir} + "/" + std::string{name} + ".yml";

    auto doc = read_yaml(yml_path);
    if (doc.at("model_class").as_string() != "RSFivemerModel")
      throw std::runtime_error{
          "rs_fivemer_model::load: expected model_class=RSFivemerModel, got " +
          doc.at("model_class").as_string()};

    auto kmer_length = static_cast<std::size_t>(
        doc.at("encoder_parameters").map.at("kmer_length").as_int());
    auto site_count = static_cast<std::size_t>(
        doc.at("encoder_parameters").map.at("site_count").as_int());

    return rs_fivemer_model{load_pth(pth_path),
                            kmer_encoder{kmer_length, site_count}};
  }

  // Run forward pass on a parent DNA sequence.
  forward_result forward(std::string_view parent_seq) const {
    auto sc = encoder_.site_count();
    auto encoded = encoder_.encode_sequence(parent_seq);

    std::vector<float> rates(sc);
    std::vector<float> csp(sc * 4);

    for (std::size_t i = 0; i < sc; ++i) {
      auto idx = static_cast<std::size_t>(encoded.kmer_indices[i]);
      rates[i] = std::exp(r_weights_[idx] + static_cast<float>(rate_bias_log_));
      for (std::size_t j = 0; j < 4; ++j) {
        csp[i * 4 + j] =
            s_weights_[idx * 4 + j] + encoded.wt_modifier[i * 4 + j];
      }
    }

    softmax_inplace(csp, sc, 4);
    return {.rates = std::move(rates), .csp = std::move(csp)};
  }

  // Compute Poisson context log-likelihood for a parent→child edge.
  double log_likelihood(std::string_view parent_seq,
                        std::string_view child_seq) const {
    auto [rates, csp] = forward(parent_seq);
    auto parent_bases = kmer_encoder::encode_bases(parent_seq);
    auto child_bases = kmer_encoder::encode_bases(child_seq);
    return poisson_context_log_likelihood(rates, csp, parent_bases,
                                          child_bases);
  }

  std::size_t kmer_length() const { return encoder_.kmer_length(); }
  std::size_t site_count() const { return encoder_.site_count(); }
  std::size_t kmer_count() const { return encoder_.kmer_count(); }

  // Adjust all output rates by exp(log_adjustment_factor).
  void adjust_rate_bias_by(double log_adjustment_factor) {
    rate_bias_log_ += log_adjustment_factor;
  }
  double rate_bias_log() const { return rate_bias_log_; }

  // Accessors for GPU inference (weight data pointers).
  float const* r_weights_data() const { return r_weights_; }
  float const* s_weights_data() const { return s_weights_; }
  kmer_encoder const& encoder() const { return encoder_; }
};

}  // namespace larch
