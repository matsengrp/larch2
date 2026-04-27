// Unit tests for the checkpoint serialization layer:
//   1. mt19937 round-trip via std::ostringstream / operator<< — must match
//      bit-exact (the C++ standard guarantees this and the resume path
//      relies on it).
//   2. larch_checkpoint_msg round-trip — write the proto bytes and decode
//      them back, asserting every scalar field plus nested results /
//      patience / dag survive intact.
//   3. SHA-256 — verify the empty-string and "abc" KATs from FIPS 180-4 so
//      a future change to the digest doesn't silently break the args
//      fingerprint check.

#include <larch/checkpoint.hpp>

#include <cassert>
#include <cstdio>
#include <print>
#include <random>
#include <string>

using namespace larch;

static void test_mt19937_roundtrip() {
  std::mt19937 rng{12345};
  for (int i = 0; i < 1000; ++i) (void)rng();
  auto serialized = serialize_mt19937(rng);
  auto restored = deserialize_mt19937(serialized);
  for (int i = 0; i < 100; ++i) {
    auto a = rng();
    auto b = restored();
    assert(a == b);
  }
  std::println("test_mt19937_roundtrip: OK");
}

static void test_checkpoint_msg_roundtrip() {
  larch_checkpoint_msg c{};
  c.schema_version = checkpoint_schema_version;
  c.larch2_git_sha = "abc123";
  c.next_iteration = 5;
  c.optimizer = "native";
  c.patience.limit = 3;
  c.patience.count = 1;
  c.patience.best_score = 100;

  std::mt19937 rng{42};
  for (int i = 0; i < 10; ++i) (void)rng();
  c.main_rng.mt19937_state = serialize_mt19937(rng);

  optimize_result_msg_pb r{};
  r.iteration = 0;
  r.dag_node_count = 10;
  r.dag_edge_count = 9;
  r.trees_merged = 1;
  r.parsimony_score = 100;
  radius_result_msg_pb rd{};
  rd.radius = 2;
  rd.moves_found = 3;
  rd.moves_applied = 2;
  rd.parsimony_score = 100;
  r.radii.push_back(rd);
  c.results.push_back(r);

  c.dag.reference_seq = "ACGT";
  c.dag.reference_id = "ref";
  c.args_fingerprint = "deadbeef";
  c.args_payload = "{\"k\":\"v\"}";

  auto bytes = pb::encode(c);
  std::span<const uint8_t> span{bytes.data(), bytes.size()};
  assert(looks_like_checkpoint(span));

  auto decoded = pb::decode<larch_checkpoint_msg>(span);
  assert(decoded.schema_version == c.schema_version);
  assert(decoded.larch2_git_sha == c.larch2_git_sha);
  assert(decoded.next_iteration == c.next_iteration);
  assert(decoded.optimizer == c.optimizer);
  assert(decoded.patience.limit == c.patience.limit);
  assert(decoded.patience.count == c.patience.count);
  assert(decoded.patience.best_score == c.patience.best_score);
  assert(decoded.main_rng.mt19937_state == c.main_rng.mt19937_state);
  assert(decoded.drift_rng.mt19937_state.empty());
  assert(decoded.results.size() == 1);
  assert(decoded.results[0].dag_node_count == 10);
  assert(decoded.results[0].radii.size() == 1);
  assert(decoded.results[0].radii[0].radius == 2);
  assert(decoded.dag.reference_seq == "ACGT");
  assert(decoded.dag.reference_id == "ref");
  assert(decoded.args_fingerprint == "deadbeef");
  assert(decoded.args_payload == "{\"k\":\"v\"}");

  // Restore RNG and verify it produces identical draws.
  auto restored = deserialize_mt19937(decoded.main_rng.mt19937_state);
  for (int i = 0; i < 100; ++i) {
    assert(rng() == restored());
  }
  std::println("test_checkpoint_msg_roundtrip: OK");
}

static void test_sha256_known_vectors() {
  // FIPS 180-4 known answer tests
  assert(sha256_hex("") ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(sha256_hex("abc") ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::println("test_sha256_known_vectors: OK");
}

static void test_optional_int64_present_zero() {
  // optional<int64_t> = 0 must round-trip as 0 (not absent), distinct from
  // optional<int64_t> = nullopt. The patience_state requires this to
  // distinguish "best_score has been observed, and it's 0" from "no best_score
  // recorded yet".
  patience_state_msg p{};
  p.limit = 0;
  p.count = 0;
  p.best_score = 0;
  auto bytes = pb::encode(p);
  auto decoded = pb::decode<patience_state_msg>({bytes.data(), bytes.size()});
  assert(decoded.limit.has_value() && *decoded.limit == 0);
  assert(decoded.count == 0);
  assert(decoded.best_score.has_value() && *decoded.best_score == 0);

  patience_state_msg q{};
  q.count = 5;
  // limit and best_score left as nullopt
  auto b2 = pb::encode(q);
  auto d2 = pb::decode<patience_state_msg>({b2.data(), b2.size()});
  assert(!d2.limit.has_value());
  assert(d2.count == 5);
  assert(!d2.best_score.has_value());
  std::println("test_optional_int64_present_zero: OK");
}

int main() {
  test_mt19937_roundtrip();
  test_checkpoint_msg_roundtrip();
  test_sha256_known_vectors();
  test_optional_int64_present_zero();
  std::println("all checkpoint tests passed");
  return 0;
}
