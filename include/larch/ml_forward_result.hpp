#pragma once

#include <larch/kmer_encoder.hpp>
#include <larch/likelihood.hpp>

#include <string_view>
#include <vector>

namespace larch {

struct ml_forward_result {
  std::vector<float> rates;  // [site_count]
  std::vector<float> csp;    // [site_count * 4]
};

inline double forward_result_log_likelihood(ml_forward_result const& result,
                                            std::string_view parent_seq,
                                            std::string_view child_seq) {
  auto parent_bases = kmer_encoder::encode_bases(parent_seq);
  auto child_bases = kmer_encoder::encode_bases(child_seq);
  return poisson_context_log_likelihood(result.rates, result.csp, parent_bases,
                                        child_bases);
}

template <class Model>
inline double model_forward_log_likelihood(Model const& model,
                                           std::string_view parent_seq,
                                           std::string_view child_seq) {
  return forward_result_log_likelihood(model.forward(parent_seq), parent_seq,
                                       child_seq);
}

}  // namespace larch
