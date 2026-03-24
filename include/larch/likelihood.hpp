#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace larch {

// Apply softmax along the last dimension (classes) in-place.
// logits layout: [seq_len * classes], row-major.
inline void softmax_inplace(std::span<float> logits, std::size_t seq_len,
                            std::size_t classes) {
  for (std::size_t i = 0; i < seq_len; ++i) {
    float* row = logits.data() + i * classes;
    // Find max for numerical stability.
    float max_val = *std::max_element(row, row + classes);
    float sum = 0.0f;
    for (std::size_t j = 0; j < classes; ++j) {
      row[j] = std::exp(row[j] - max_val);
      sum += row[j];
    }
    for (std::size_t j = 0; j < classes; ++j) {
      row[j] /= sum;
    }
  }
}

// Poisson context log-likelihood for a parent-child edge.
//
// rates:  [seq_len] mutation rates per position (positive)
// csp:    [seq_len * 4] conditional substitution probabilities (after softmax)
// parent: [seq_len'] base indices (A=0,C=1,G=2,T=3)
// child:  [seq_len'] base indices
//
// rates/csp may be longer than parent/child (model pads to site_count).
// Only the first min(rates.size(), parent.size()) positions are used.
//
// Formula: log_lik = sum_j log(rate_j * csp_j[child_j]) + n * log(t_hat) - n
//          where t_hat = n / sum(rates), n = number of mutations
//
// Returns 0.0 if no mutations.
inline double poisson_context_log_likelihood(std::span<const float> rates,
                                             std::span<const float> csp,
                                             std::span<const int64_t> parent,
                                             std::span<const int64_t> child) {
  auto seq_len = std::min({parent.size(), child.size(), rates.size()});

  // Find mutations and count them.
  int64_t n_mutations = 0;
  for (std::size_t i = 0; i < seq_len; ++i) {
    if (parent[i] != child[i]) ++n_mutations;
  }

  if (n_mutations == 0) return 0.0;

  // Sum rates over the sequence length.
  double sum_rates = 0.0;
  for (std::size_t i = 0; i < seq_len; ++i) {
    sum_rates += static_cast<double>(rates[i]);
  }

  double t_hat = static_cast<double>(n_mutations) / sum_rates;

  // Sum log(lambda_j) for mutated positions.
  double log_lambda_sum = 0.0;
  for (std::size_t i = 0; i < seq_len; ++i) {
    if (parent[i] != child[i]) {
      auto child_base = static_cast<std::size_t>(child[i]);
      double rate = static_cast<double>(rates[i]);
      double sub_prob = static_cast<double>(csp[i * 4 + child_base]);
      // Clamp to avoid log(0) when float32 softmax underflows to exactly 0.
      if (sub_prob < 1e-30) sub_prob = 1e-30;
      log_lambda_sum += std::log(rate * sub_prob);
    }
  }

  return log_lambda_sum + std::log(t_hat) * static_cast<double>(n_mutations) -
         static_cast<double>(n_mutations);
}

}  // namespace larch
