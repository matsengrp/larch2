// Tests for kmer_encoder — ported from larch-rm-optimize
// test_netam_kmer_encoder.cpp
#include <larch/kmer_encoder.hpp>

#include <cassert>
#include <print>

using namespace larch;

// ---- encode_bases tests ----

void test_encode_bases_standard() {
  auto result = kmer_encoder::encode_bases("ACGT");
  assert(result.size() == 4);
  assert(result[0] == 0);  // A
  assert(result[1] == 1);  // C
  assert(result[2] == 2);  // G
  assert(result[3] == 3);  // T
}

void test_encode_bases_lowercase() {
  auto result = kmer_encoder::encode_bases("acgt");
  assert(result.size() == 4);
  assert(result[0] == 0);
  assert(result[1] == 1);
  assert(result[2] == 2);
  assert(result[3] == 3);
}

void test_encode_bases_unknown() {
  auto result = kmer_encoder::encode_bases("N");
  assert(result.size() == 1);
  assert(result[0] == 4);
}

void test_encode_bases_mixed_with_unknown() {
  auto result = kmer_encoder::encode_bases("ANCT");
  assert(result.size() == 4);
  assert(result[0] == 0);
  assert(result[1] == 4);
  assert(result[2] == 1);
  assert(result[3] == 3);
}

void test_encode_bases_empty() {
  auto result = kmer_encoder::encode_bases("");
  assert(result.empty());
}

// ---- Constructor tests ----

void test_constructor_kmer_length_3() {
  kmer_encoder enc{3, 100};
  assert(enc.kmer_length() == 3);
  assert(enc.site_count() == 100);
  assert(enc.kmer_count() == 65);  // 4^3 + 1
}

void test_constructor_kmer_length_5() {
  kmer_encoder enc{5, 200};
  assert(enc.kmer_length() == 5);
  assert(enc.site_count() == 200);
  assert(enc.kmer_count() == 1025);  // 4^5 + 1
}

// ---- encode_sequence tests ----

void test_encode_sequence_output_shape() {
  kmer_encoder enc{3, 10};
  auto [encoded, wt] = enc.encode_sequence("ACGTACGT");

  assert(encoded.size() == 10);
  assert(wt.size() == 10 * 4);
}

void test_encode_sequence_short_sequence() {
  kmer_encoder enc{3, 100};
  auto [encoded, wt] = enc.encode_sequence("ACG");

  assert(encoded.size() == 100);
  assert(wt.size() == 100 * 4);
}

void test_encode_sequence_lowercase() {
  kmer_encoder enc{3, 10};
  auto [enc_lower, wt_lower] = enc.encode_sequence("acgtacgt");
  auto [enc_upper, wt_upper] = enc.encode_sequence("ACGTACGT");

  assert(enc_lower == enc_upper);
  assert(wt_lower == wt_upper);
}

void test_encode_sequence_wt_modifier_values() {
  kmer_encoder enc{3, 5};
  auto [encoded, wt] = enc.encode_sequence("ACG");

  // Position 0 has 'A' (index 0): wt[0*4+0] should be -BIG
  assert(wt[0 * 4 + 0] < -1e8f);
  assert(wt[0 * 4 + 1] == 0.0f);
  assert(wt[0 * 4 + 2] == 0.0f);
  assert(wt[0 * 4 + 3] == 0.0f);

  // Position 1 has 'C' (index 1)
  assert(wt[1 * 4 + 0] == 0.0f);
  assert(wt[1 * 4 + 1] < -1e8f);
  assert(wt[1 * 4 + 2] == 0.0f);
  assert(wt[1 * 4 + 3] == 0.0f);

  // Position 2 has 'G' (index 2)
  assert(wt[2 * 4 + 0] == 0.0f);
  assert(wt[2 * 4 + 1] == 0.0f);
  assert(wt[2 * 4 + 2] < -1e8f);
  assert(wt[2 * 4 + 3] == 0.0f);

  // Positions beyond sequence: all zeros
  assert(wt[3 * 4 + 0] == 0.0f);
  assert(wt[3 * 4 + 1] == 0.0f);
  assert(wt[3 * 4 + 2] == 0.0f);
  assert(wt[3 * 4 + 3] == 0.0f);
}

void test_encode_sequence_with_n() {
  kmer_encoder enc{3, 10};
  auto [encoded, wt] = enc.encode_sequence("ACNGT");

  // Position 2 (N): all wt zeros
  assert(wt[2 * 4 + 0] == 0.0f);
  assert(wt[2 * 4 + 1] == 0.0f);
  assert(wt[2 * 4 + 2] == 0.0f);
  assert(wt[2 * 4 + 3] == 0.0f);
}

void test_encode_sequence_empty() {
  kmer_encoder enc{3, 10};
  auto [encoded, wt] = enc.encode_sequence("");

  assert(encoded.size() == 10);
  assert(wt.size() == 40);
  // All wt should be zero (no bases to mask).
  for (float v : wt) assert(v == 0.0f);
}

void test_encode_sequence_single_base() {
  kmer_encoder enc{3, 5};
  auto [encoded, wt] = enc.encode_sequence("T");

  assert(encoded.size() == 5);
  // Position 0 has 'T' (index 3)
  assert(wt[0 * 4 + 3] < -1e8f);
}

void test_encode_sequence_all_same_base() {
  kmer_encoder enc{3, 5};
  auto [encoded, wt] = enc.encode_sequence("AAAAA");

  // Middle positions should have the same k-mer index (AAA).
  assert(encoded[1] == encoded[2]);
  assert(encoded[2] == encoded[3]);
}

void test_encode_sequence_kmer_indices_valid() {
  kmer_encoder enc{3, 10};
  auto [encoded, wt] = enc.encode_sequence("ACGTACGTAC");

  for (int32_t idx : encoded) {
    assert(idx >= 0);
    assert(static_cast<std::size_t>(idx) < enc.kmer_count());
  }
}

void test_encode_sequence_padding_creates_n_index() {
  kmer_encoder enc{3, 5};
  auto [encoded, wt] = enc.encode_sequence("ACG");

  auto n_index = static_cast<int32_t>(enc.kmer_count() - 1);

  // Position 0: k-mer is "NAC" (contains N) -> N index
  assert(encoded[0] == n_index);
  // Position 2: k-mer is "CGN" (contains N) -> N index
  assert(encoded[2] == n_index);
}

void test_encode_sequence_long_truncated() {
  kmer_encoder enc{3, 5};
  auto [encoded, wt] = enc.encode_sequence("ACGTACGTACGT");

  assert(encoded.size() == 5);
  assert(wt.size() == 20);
}

int main() {
  std::println("=== encode_bases tests ===");
  test_encode_bases_standard();
  test_encode_bases_lowercase();
  test_encode_bases_unknown();
  test_encode_bases_mixed_with_unknown();
  test_encode_bases_empty();

  std::println("=== constructor tests ===");
  test_constructor_kmer_length_3();
  test_constructor_kmer_length_5();

  std::println("=== encode_sequence tests ===");
  test_encode_sequence_output_shape();
  test_encode_sequence_short_sequence();
  test_encode_sequence_lowercase();
  test_encode_sequence_wt_modifier_values();
  test_encode_sequence_with_n();
  test_encode_sequence_empty();
  test_encode_sequence_single_base();
  test_encode_sequence_all_same_base();
  test_encode_sequence_kmer_indices_valid();
  test_encode_sequence_padding_creates_n_index();
  test_encode_sequence_long_truncated();

  std::println("All kmer_encoder tests passed");
}
