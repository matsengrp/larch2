#pragma once

#include <larch/ml_forward_result.hpp>

#include <cstddef>
#include <memory>
#include <string_view>

namespace larch {

class vk_context;
class rs_fivemer_model;
class indep_rs_cnn_model;

// GPU-accelerated neural network inference via Vulkan compute shaders.
// Move-only (pimpl). Provides the same forward_result / log_likelihood()
// interface as the CPU models, so it can be used with likelihood_score_ops.
class nn_inference {
 public:
  using forward_result = ml_forward_result;

  nn_inference(vk_context& ctx, rs_fivemer_model const& model);
  nn_inference(vk_context& ctx, indep_rs_cnn_model const& model);

  ~nn_inference();
  nn_inference(nn_inference&&) noexcept;
  nn_inference& operator=(nn_inference&&) noexcept;
  nn_inference(nn_inference const&) = delete;
  nn_inference& operator=(nn_inference const&) = delete;

  forward_result forward(std::string_view parent_seq) const;

  double log_likelihood(std::string_view parent_seq,
                        std::string_view child_seq) const;

  std::size_t site_count() const;

 private:
  struct impl;
  impl& require_impl() const;

  std::unique_ptr<impl> impl_;
};

}  // namespace larch
