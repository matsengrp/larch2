#pragma once

#include <larch/kmer_encoder.hpp>
#include <larch/likelihood.hpp>
#include <larch/pth_loader.hpp>
#include <larch/yaml_reader.hpp>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

// IndepRSCNNModel: CNN-based model for mutation rates and conditional
// substitution probabilities.  Architecture:
//   shared = ReLU(conv1d(embed(kmer_indices)))          [L, F]
//   r      = ReLU(r_conv1d(r_embed(kmer_indices)))      [L, F]
//   rates  = exp(linear(shared) + r_linear(r))          [L]
//   s      = ReLU(s_conv1d(s_embed(kmer_indices)))      [L, F]
//   csp    = softmax(s_linear(shared + s) + wt_modifier) [L, 4]
class indep_rs_cnn_model {
  pth_file pth_;
  kmer_encoder encoder_;
  std::size_t embedding_dim_;
  std::size_t filter_count_;
  std::size_t kernel_size_;

  // Shared weights.
  float const* embed_w_;   // [kmer_count, E]
  float const* conv_w_;    // [F, E, K]
  float const* conv_b_;    // [F]
  float const* linear_w_;  // [1, F]
  float const* linear_b_;  // [1]

  // R-branch weights.
  float const* r_embed_w_;   // [kmer_count, E]
  float const* r_conv_w_;    // [F, E, K]
  float const* r_conv_b_;    // [F]
  float const* r_linear_w_;  // [1, F]
  float const* r_linear_b_;  // [1]

  // S-branch weights.
  float const* s_embed_w_;   // [kmer_count, E]
  float const* s_conv_w_;    // [F, E, K]
  float const* s_conv_b_;    // [F]
  float const* s_linear_w_;  // [4, F]
  float const* s_linear_b_;  // [4]

  // --- helpers (all operate on row-major [L, channels] layout) ---

  // Embedding lookup: indices[L] × weights[vocab, E] → out[L, E].
  static void embedding(float* out, int32_t const* indices, std::size_t L,
                        float const* w, std::size_t E) {
    for (std::size_t i = 0; i < L; ++i) {
      auto idx = static_cast<std::size_t>(indices[i]);
      for (std::size_t e = 0; e < E; ++e) out[i * E + e] = w[idx * E + e];
    }
  }

  // 1D convolution with same-padding (zero-pad K/2 on each side).
  // in[L, C_in] × w[C_out, C_in, K] + b[C_out] → out[L, C_out].
  static void conv1d_same(float* out, float const* in, std::size_t L,
                          float const* w, float const* b, std::size_t C_in,
                          std::size_t C_out, std::size_t K) {
    auto half = static_cast<int64_t>(K / 2);
    auto Li = static_cast<int64_t>(L);
    for (std::size_t l = 0; l < L; ++l) {
      for (std::size_t co = 0; co < C_out; ++co) {
        float val = b[co];
        for (std::size_t ci = 0; ci < C_in; ++ci) {
          for (std::size_t k = 0; k < K; ++k) {
            auto src = static_cast<int64_t>(l) + static_cast<int64_t>(k) - half;
            if (src >= 0 && src < Li)
              val += w[co * C_in * K + ci * K + k] *
                     in[static_cast<std::size_t>(src) * C_in + ci];
          }
        }
        out[l * C_out + co] = val;
      }
    }
  }

  static void relu_inplace(float* data, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
      if (data[i] < 0.0f) data[i] = 0.0f;
  }

  // Linear: in[L, F_in] × w[F_out, F_in] + b[F_out] → out[L, F_out].
  static void linear_fwd(float* out, float const* in, std::size_t L,
                         float const* w, float const* b, std::size_t F_in,
                         std::size_t F_out) {
    for (std::size_t l = 0; l < L; ++l) {
      for (std::size_t fo = 0; fo < F_out; ++fo) {
        float val = b[fo];
        for (std::size_t fi = 0; fi < F_in; ++fi)
          val += w[fo * F_in + fi] * in[l * F_in + fi];
        out[l * F_out + fo] = val;
      }
    }
  }

  float const* cache(std::string_view name,
                     std::vector<int64_t> const& expected) {
    auto& t = pth_.get(name);
    if (t.shape != expected)
      throw std::runtime_error{"indep_rs_cnn_model: unexpected shape for " +
                               std::string{name}};
    return t.data.data();
  }

 public:
  struct forward_result {
    std::vector<float> rates;  // [site_count]
    std::vector<float> csp;    // [site_count * 4]
  };

  indep_rs_cnn_model(pth_file pth, kmer_encoder encoder,
                     std::size_t embedding_dim, std::size_t filter_count,
                     std::size_t kernel_size)
      : pth_{std::move(pth)},
        encoder_{std::move(encoder)},
        embedding_dim_{embedding_dim},
        filter_count_{filter_count},
        kernel_size_{kernel_size} {
    auto kc = static_cast<int64_t>(encoder_.kmer_count());
    auto E = static_cast<int64_t>(embedding_dim_);
    auto F = static_cast<int64_t>(filter_count_);
    auto K = static_cast<int64_t>(kernel_size_);

    embed_w_ = cache("kmer_embedding.weight", {kc, E});
    conv_w_ = cache("conv.weight", {F, E, K});
    conv_b_ = cache("conv.bias", {F});
    linear_w_ = cache("linear.weight", {1, F});
    linear_b_ = cache("linear.bias", {1});

    r_embed_w_ = cache("r_kmer_embedding.weight", {kc, E});
    r_conv_w_ = cache("r_conv.weight", {F, E, K});
    r_conv_b_ = cache("r_conv.bias", {F});
    r_linear_w_ = cache("r_linear.weight", {1, F});
    r_linear_b_ = cache("r_linear.bias", {1});

    s_embed_w_ = cache("s_kmer_embedding.weight", {kc, E});
    s_conv_w_ = cache("s_conv.weight", {F, E, K});
    s_conv_b_ = cache("s_conv.bias", {F});
    s_linear_w_ = cache("s_linear.weight", {4, F});
    s_linear_b_ = cache("s_linear.bias", {4});
  }

  static indep_rs_cnn_model load(std::string_view dir, std::string_view name) {
    auto pth_path =
        std::string{dir} + "/" + std::string{name} + "-libtorch.pth";
    auto yml_path = std::string{dir} + "/" + std::string{name} + ".yml";

    auto doc = read_yaml(yml_path);
    if (doc.at("model_class").as_string() != "IndepRSCNNModel")
      throw std::runtime_error{
          "indep_rs_cnn_model::load: expected IndepRSCNNModel, got " +
          doc.at("model_class").as_string()};

    auto& enc = doc.at("encoder_parameters").map;
    auto kmer_length = static_cast<std::size_t>(enc.at("kmer_length").as_int());
    auto site_count = static_cast<std::size_t>(enc.at("site_count").as_int());

    auto& hyp = doc.at("model_hyperparameters").map;
    auto embedding_dim =
        static_cast<std::size_t>(hyp.at("embedding_dim").as_int());
    auto filter_count =
        static_cast<std::size_t>(hyp.at("filter_count").as_int());
    auto kernel_size = static_cast<std::size_t>(hyp.at("kernel_size").as_int());

    return indep_rs_cnn_model{load_pth(pth_path),
                              kmer_encoder{kmer_length, site_count},
                              embedding_dim, filter_count, kernel_size};
  }

  forward_result forward(std::string_view parent_seq) const {
    auto L = encoder_.site_count();
    auto E = embedding_dim_;
    auto F = filter_count_;
    auto K = kernel_size_;
    auto encoded = encoder_.encode_sequence(parent_seq);

    // Shared path: embed → conv → ReLU.
    std::vector<float> shared_emb(L * E);
    embedding(shared_emb.data(), encoded.kmer_indices.data(), L, embed_w_, E);
    std::vector<float> shared(L * F);
    conv1d_same(shared.data(), shared_emb.data(), L, conv_w_, conv_b_, E, F, K);
    relu_inplace(shared.data(), L * F);

    // R-branch: embed → conv → ReLU.
    std::vector<float> r_emb(L * E);
    embedding(r_emb.data(), encoded.kmer_indices.data(), L, r_embed_w_, E);
    std::vector<float> r_feat(L * F);
    conv1d_same(r_feat.data(), r_emb.data(), L, r_conv_w_, r_conv_b_, E, F, K);
    relu_inplace(r_feat.data(), L * F);

    // Rates: exp(linear(shared) + r_linear(r_feat)).
    std::vector<float> base_rate(L);
    linear_fwd(base_rate.data(), shared.data(), L, linear_w_, linear_b_, F, 1);
    std::vector<float> r_rate(L);
    linear_fwd(r_rate.data(), r_feat.data(), L, r_linear_w_, r_linear_b_, F, 1);
    std::vector<float> rates(L);
    for (std::size_t i = 0; i < L; ++i)
      rates[i] = std::exp(base_rate[i] + r_rate[i]);

    // S-branch: embed → conv → ReLU.
    std::vector<float> s_emb(L * E);
    embedding(s_emb.data(), encoded.kmer_indices.data(), L, s_embed_w_, E);
    std::vector<float> s_feat(L * F);
    conv1d_same(s_feat.data(), s_emb.data(), L, s_conv_w_, s_conv_b_, E, F, K);
    relu_inplace(s_feat.data(), L * F);

    // CSP: softmax(s_linear(shared + s_feat) + wt_modifier).
    for (std::size_t i = 0; i < L * F; ++i) s_feat[i] += shared[i];
    std::vector<float> csp(L * 4);
    linear_fwd(csp.data(), s_feat.data(), L, s_linear_w_, s_linear_b_, F, 4);
    for (std::size_t i = 0; i < L * 4; ++i) csp[i] += encoded.wt_modifier[i];
    softmax_inplace(csp, L, 4);

    return {.rates = std::move(rates), .csp = std::move(csp)};
  }

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
  std::size_t embedding_dim() const { return embedding_dim_; }
  std::size_t filter_count() const { return filter_count_; }
  std::size_t kernel_size() const { return kernel_size_; }

  // Accessors for GPU inference (weight data pointers).
  kmer_encoder const& encoder() const { return encoder_; }
  float const* embed_w_data() const { return embed_w_; }
  float const* conv_w_data() const { return conv_w_; }
  float const* conv_b_data() const { return conv_b_; }
  float const* linear_w_data() const { return linear_w_; }
  float const* linear_b_data() const { return linear_b_; }
  float const* r_embed_w_data() const { return r_embed_w_; }
  float const* r_conv_w_data() const { return r_conv_w_; }
  float const* r_conv_b_data() const { return r_conv_b_; }
  float const* r_linear_w_data() const { return r_linear_w_; }
  float const* r_linear_b_data() const { return r_linear_b_; }
  float const* s_embed_w_data() const { return s_embed_w_; }
  float const* s_conv_w_data() const { return s_conv_w_; }
  float const* s_conv_b_data() const { return s_conv_b_; }
  float const* s_linear_w_data() const { return s_linear_w_; }
  float const* s_linear_b_data() const { return s_linear_b_; }
};

}  // namespace larch
