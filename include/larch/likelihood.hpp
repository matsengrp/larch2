#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace larch {

// Apply softmax along the last dimension (classes) in-place.
// logits layout: [seq_len * classes], row-major.
inline void softmax_inplace(std::span<float> logits, std::size_t seq_len,
                            std::size_t classes) {
  if (classes == 0)
    throw std::runtime_error{"softmax_inplace: classes must be positive"};
  if (seq_len > std::numeric_limits<std::size_t>::max() / classes ||
      logits.size() < seq_len * classes)
    throw std::runtime_error{"softmax_inplace: logits span is too small"};

  for (std::size_t i = 0; i < seq_len; ++i) {
    float* row = logits.data() + i * classes;
    for (std::size_t j = 0; j < classes; ++j) {
      if (!std::isfinite(row[j]))
        throw std::runtime_error{"softmax_inplace: non-finite logit at row " +
                                 std::to_string(i)};
    }

    // Find max for numerical stability.
    float max_val = *std::max_element(row, row + classes);
    float sum = 0.0f;
    for (std::size_t j = 0; j < classes; ++j) {
      row[j] = std::exp(row[j] - max_val);
      sum += row[j];
    }
    if (!std::isfinite(sum) || sum <= 0.0f)
      throw std::runtime_error{"softmax_inplace: invalid normalization sum"};
    for (std::size_t j = 0; j < classes; ++j) {
      row[j] /= sum;
      if (!std::isfinite(row[j]) || row[j] < 0.0f)
        throw std::runtime_error{"softmax_inplace: invalid probability output"};
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
  if (seq_len == 0) return 0.0;
  if (seq_len > std::numeric_limits<std::size_t>::max() / 4 ||
      csp.size() < seq_len * 4)
    throw std::runtime_error{
        "poisson_context_log_likelihood: csp span is too small"};

  // Sum and validate rates over the sequence length before the no-mutation fast
  // path so malformed model output is reported even on identical sequences.
  double sum_rates = 0.0;
  for (std::size_t i = 0; i < seq_len; ++i) {
    double rate = static_cast<double>(rates[i]);
    if (!std::isfinite(rate) || rate <= 0.0)
      throw std::runtime_error{
          "poisson_context_log_likelihood: invalid rate at position " +
          std::to_string(i)};
    sum_rates += rate;
    if (!std::isfinite(sum_rates))
      throw std::runtime_error{
          "poisson_context_log_likelihood: non-finite sum_rates"};
  }
  if (sum_rates <= 0.0)
    throw std::runtime_error{
        "poisson_context_log_likelihood: sum_rates must be positive"};

  // Find mutations and count them.
  int64_t n_mutations = 0;
  for (std::size_t i = 0; i < seq_len; ++i) {
    if (parent[i] != child[i]) ++n_mutations;
  }

  if (n_mutations == 0) return 0.0;

  double t_hat = static_cast<double>(n_mutations) / sum_rates;
  if (!std::isfinite(t_hat) || t_hat <= 0.0)
    throw std::runtime_error{
        "poisson_context_log_likelihood: invalid t_hat"};

  // Sum log(lambda_j) for mutated positions.
  double log_lambda_sum = 0.0;
  for (std::size_t i = 0; i < seq_len; ++i) {
    if (parent[i] != child[i]) {
      if (parent[i] < 0 || parent[i] > 3 || child[i] < 0 || child[i] > 3)
        throw std::runtime_error{
            "poisson_context_log_likelihood: invalid base index at position " +
            std::to_string(i)};
      auto child_base = static_cast<std::size_t>(child[i]);
      double rate = static_cast<double>(rates[i]);
      double sub_prob = static_cast<double>(csp[i * 4 + child_base]);
      if (!std::isfinite(sub_prob) || sub_prob < 0.0)
        throw std::runtime_error{
            "poisson_context_log_likelihood: invalid substitution probability at position " +
            std::to_string(i)};
      // Float32 softmax can legitimately underflow extremely unlikely classes
      // to exactly zero.  Clamp only that underflow floor; NaN and negative
      // probabilities are rejected above.
      if (sub_prob < 1e-30) sub_prob = 1e-30;
      double lambda = rate * sub_prob;
      if (!std::isfinite(lambda) || lambda <= 0.0)
        throw std::runtime_error{
            "poisson_context_log_likelihood: invalid lambda at position " +
            std::to_string(i)};
      log_lambda_sum += std::log(lambda);
      if (!std::isfinite(log_lambda_sum))
        throw std::runtime_error{
            "poisson_context_log_likelihood: non-finite log-lambda sum"};
    }
  }

  double result = log_lambda_sum +
                  std::log(t_hat) * static_cast<double>(n_mutations) -
                  static_cast<double>(n_mutations);
  if (!std::isfinite(result))
    throw std::runtime_error{
        "poisson_context_log_likelihood: non-finite log-likelihood"};
  return result;
}

}  // namespace larch
