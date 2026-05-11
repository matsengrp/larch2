#include <larch/nn_inference.hpp>

#include <larch/indep_rs_cnn_model.hpp>
#include <larch/kmer_encoder.hpp>
#include <larch/likelihood.hpp>
#include <larch/rs_fivemer_model.hpp>
#include <larch/vulkan_compute.hpp>

// Embedded SPIR-V bytecode — generated at build time.
#include <larch/cnn_r_branch_spv.hpp>
#include <larch/cnn_s_branch_spv.hpp>
#include <larch/fivemer_forward_spv.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace larch {

namespace {

// Upload float array to a newly created storage buffer.
vk_buffer upload_floats(vk_context& ctx, float const* data, std::size_t count) {
  auto buf = vk_buffer::create(ctx, count * sizeof(float),
                               vk_buffer::usage::storage_read);
  buf.upload({reinterpret_cast<const std::byte*>(data), count * sizeof(float)});
  return buf;
}

}  // namespace

// ============================================================================
// Push constant structures (must match shader layouts).
// ============================================================================

struct fivemer_pc {
  std::uint32_t site_count;
};

struct cnn_pc {
  std::uint32_t L;
  std::uint32_t E;
  std::uint32_t F;
  std::uint32_t K;
};

// ============================================================================
// nn_inference::impl
// ============================================================================

struct nn_inference::impl {
  enum class model_kind { fivemer, cnn };
  model_kind kind;

  vk_context& ctx;
  kmer_encoder encoder;
  std::size_t sc;  // site_count

  // --- Fivemer state ---
  std::optional<vk_buffer> r_weights_buf;
  std::optional<vk_buffer> s_weights_buf;
  std::optional<vk_pipeline> fivemer_pipe;

  // --- CNN state ---
  // Shared branch.
  std::optional<vk_buffer> embed_w_buf;
  std::optional<vk_buffer> conv_w_buf;
  std::optional<vk_buffer> conv_b_buf;
  std::optional<vk_buffer> linear_w_buf;
  std::optional<vk_buffer> linear_b_buf;
  // R-branch.
  std::optional<vk_buffer> r_embed_w_buf;
  std::optional<vk_buffer> r_conv_w_buf;
  std::optional<vk_buffer> r_conv_b_buf;
  std::optional<vk_buffer> r_linear_w_buf;
  std::optional<vk_buffer> r_linear_b_buf;
  // S-branch.
  std::optional<vk_buffer> s_embed_w_buf;
  std::optional<vk_buffer> s_conv_w_buf;
  std::optional<vk_buffer> s_conv_b_buf;
  std::optional<vk_buffer> s_linear_w_buf;
  std::optional<vk_buffer> s_linear_b_buf;
  // Pipelines.
  std::optional<vk_pipeline> cnn_r_pipe;
  std::optional<vk_pipeline> cnn_s_pipe;
  // CNN hyperparameters.
  cnn_pc cnn_params{};

  // --- Per-inference I/O buffers ---
  std::optional<vk_buffer> kmer_indices_buf;
  std::optional<vk_buffer> wt_modifier_buf;
  std::optional<vk_buffer> rates_buf;
  std::optional<vk_buffer> csp_logits_buf;

  // Fivemer constructor.
  impl(vk_context& ctx_, rs_fivemer_model const& model)
      : kind{model_kind::fivemer},
        ctx{ctx_},
        encoder{model.encoder()},
        sc{model.site_count()} {
    r_weights_buf.emplace(
        upload_floats(ctx_, model.r_weights_data(), model.kmer_count()));
    s_weights_buf.emplace(
        upload_floats(ctx_, model.s_weights_data(), model.kmer_count() * 4));

    fivemer_pipe.emplace(vk_pipeline::create(ctx_, shaders::fivemer_forward, 6,
                                             sizeof(fivemer_pc)));

    kmer_indices_buf.emplace(vk_buffer::create(ctx_, sc * sizeof(std::int32_t),
                                               vk_buffer::usage::storage_read));
    wt_modifier_buf.emplace(vk_buffer::create(ctx_, sc * 4 * sizeof(float),
                                              vk_buffer::usage::storage_read));
    rates_buf.emplace(vk_buffer::create(ctx_, sc * sizeof(float),
                                        vk_buffer::usage::storage_write));
    csp_logits_buf.emplace(vk_buffer::create(ctx_, sc * 4 * sizeof(float),
                                             vk_buffer::usage::storage_write));
  }

  // CNN constructor.
  impl(vk_context& ctx_, indep_rs_cnn_model const& model)
      : kind{model_kind::cnn},
        ctx{ctx_},
        encoder{model.encoder()},
        sc{model.site_count()} {
    auto kc = model.kmer_count();
    auto E = model.embedding_dim();
    auto F = model.filter_count();
    auto K = model.kernel_size();

    // Shared branch weights.
    embed_w_buf.emplace(upload_floats(ctx_, model.embed_w_data(), kc * E));
    conv_w_buf.emplace(upload_floats(ctx_, model.conv_w_data(), F * E * K));
    conv_b_buf.emplace(upload_floats(ctx_, model.conv_b_data(), F));
    linear_w_buf.emplace(upload_floats(ctx_, model.linear_w_data(), F));
    linear_b_buf.emplace(upload_floats(ctx_, model.linear_b_data(), 1));

    // R-branch weights.
    r_embed_w_buf.emplace(upload_floats(ctx_, model.r_embed_w_data(), kc * E));
    r_conv_w_buf.emplace(upload_floats(ctx_, model.r_conv_w_data(), F * E * K));
    r_conv_b_buf.emplace(upload_floats(ctx_, model.r_conv_b_data(), F));
    r_linear_w_buf.emplace(upload_floats(ctx_, model.r_linear_w_data(), F));
    r_linear_b_buf.emplace(upload_floats(ctx_, model.r_linear_b_data(), 1));

    // S-branch weights.
    s_embed_w_buf.emplace(upload_floats(ctx_, model.s_embed_w_data(), kc * E));
    s_conv_w_buf.emplace(upload_floats(ctx_, model.s_conv_w_data(), F * E * K));
    s_conv_b_buf.emplace(upload_floats(ctx_, model.s_conv_b_data(), F));
    s_linear_w_buf.emplace(upload_floats(ctx_, model.s_linear_w_data(), 4 * F));
    s_linear_b_buf.emplace(upload_floats(ctx_, model.s_linear_b_data(), 4));

    // Pipelines.
    cnn_r_pipe.emplace(
        vk_pipeline::create(ctx_, shaders::cnn_r_branch, 12, sizeof(cnn_pc)));
    cnn_s_pipe.emplace(
        vk_pipeline::create(ctx_, shaders::cnn_s_branch, 11, sizeof(cnn_pc)));

    cnn_params = {static_cast<std::uint32_t>(sc), static_cast<std::uint32_t>(E),
                  static_cast<std::uint32_t>(F), static_cast<std::uint32_t>(K)};

    // I/O buffers.
    kmer_indices_buf.emplace(vk_buffer::create(ctx_, sc * sizeof(std::int32_t),
                                               vk_buffer::usage::storage_read));
    wt_modifier_buf.emplace(vk_buffer::create(ctx_, sc * 4 * sizeof(float),
                                              vk_buffer::usage::storage_read));
    rates_buf.emplace(vk_buffer::create(ctx_, sc * sizeof(float),
                                        vk_buffer::usage::storage_write));
    csp_logits_buf.emplace(vk_buffer::create(ctx_, sc * 4 * sizeof(float),
                                             vk_buffer::usage::storage_write));
  }

  nn_inference::forward_result forward_fivemer(std::string_view parent_seq) {
    auto encoded = encoder.encode_sequence(parent_seq);

    kmer_indices_buf->upload(
        {reinterpret_cast<const std::byte*>(encoded.kmer_indices.data()),
         sc * sizeof(std::int32_t)});
    wt_modifier_buf->upload(
        {reinterpret_cast<const std::byte*>(encoded.wt_modifier.data()),
         sc * 4 * sizeof(float)});

    fivemer_pc pc{static_cast<std::uint32_t>(sc)};
    auto pc_span =
        std::span{reinterpret_cast<const std::byte*>(&pc), sizeof(pc)};
    std::uint32_t wg = (static_cast<std::uint32_t>(sc) + 63u) / 64u;

    fivemer_pipe->dispatch(
        {&*kmer_indices_buf, &*r_weights_buf, &*s_weights_buf,
         &*wt_modifier_buf, &*rates_buf, &*csp_logits_buf},
        pc_span, {wg, 1, 1});

    std::vector<float> rates(sc);
    rates_buf->download(
        {reinterpret_cast<std::byte*>(rates.data()), sc * sizeof(float)});

    std::vector<float> csp(sc * 4);
    csp_logits_buf->download(
        {reinterpret_cast<std::byte*>(csp.data()), sc * 4 * sizeof(float)});

    softmax_inplace(csp, sc, 4);
    return {std::move(rates), std::move(csp)};
  }

  nn_inference::forward_result forward_cnn(std::string_view parent_seq) {
    auto encoded = encoder.encode_sequence(parent_seq);

    kmer_indices_buf->upload(
        {reinterpret_cast<const std::byte*>(encoded.kmer_indices.data()),
         sc * sizeof(std::int32_t)});
    wt_modifier_buf->upload(
        {reinterpret_cast<const std::byte*>(encoded.wt_modifier.data()),
         sc * 4 * sizeof(float)});

    auto pc_span = std::span{reinterpret_cast<const std::byte*>(&cnn_params),
                             sizeof(cnn_params)};
    std::uint32_t wg = (static_cast<std::uint32_t>(sc) + 63u) / 64u;

    // Dispatch R-branch (rates).
    cnn_r_pipe->dispatch(
        {&*kmer_indices_buf, &*embed_w_buf, &*conv_w_buf, &*conv_b_buf,
         &*linear_w_buf, &*linear_b_buf, &*r_embed_w_buf, &*r_conv_w_buf,
         &*r_conv_b_buf, &*r_linear_w_buf, &*r_linear_b_buf, &*rates_buf},
        pc_span, {wg, 1, 1});

    // Dispatch S-branch (CSP logits).
    cnn_s_pipe->dispatch(
        {&*kmer_indices_buf, &*embed_w_buf, &*conv_w_buf, &*conv_b_buf,
         &*s_embed_w_buf, &*s_conv_w_buf, &*s_conv_b_buf, &*s_linear_w_buf,
         &*s_linear_b_buf, &*wt_modifier_buf, &*csp_logits_buf},
        pc_span, {wg, 1, 1});

    std::vector<float> rates(sc);
    rates_buf->download(
        {reinterpret_cast<std::byte*>(rates.data()), sc * sizeof(float)});

    std::vector<float> csp(sc * 4);
    csp_logits_buf->download(
        {reinterpret_cast<std::byte*>(csp.data()), sc * 4 * sizeof(float)});

    softmax_inplace(csp, sc, 4);
    return {std::move(rates), std::move(csp)};
  }
};

// ============================================================================
// nn_inference public API
// ============================================================================

nn_inference::nn_inference(vk_context& ctx, rs_fivemer_model const& model)
    : impl_{std::make_unique<impl>(ctx, model)} {}

nn_inference::nn_inference(vk_context& ctx, indep_rs_cnn_model const& model)
    : impl_{std::make_unique<impl>(ctx, model)} {}

nn_inference::~nn_inference() = default;
nn_inference::nn_inference(nn_inference&&) noexcept = default;
nn_inference& nn_inference::operator=(nn_inference&&) noexcept = default;

nn_inference::impl& nn_inference::require_impl() const {
  if (!impl_)
    throw std::logic_error{
        "nn_inference: operation on moved-from object"};
  return *impl_;
}

nn_inference::forward_result nn_inference::forward(
    std::string_view parent_seq) const {
  auto& impl = require_impl();
  if (impl.kind == impl::model_kind::fivemer)
    return impl.forward_fivemer(parent_seq);
  return impl.forward_cnn(parent_seq);
}

double nn_inference::log_likelihood(std::string_view parent_seq,
                                    std::string_view child_seq) const {
  return model_forward_log_likelihood(*this, parent_seq, child_seq);
}

std::size_t nn_inference::site_count() const { return require_impl().sc; }

}  // namespace larch
