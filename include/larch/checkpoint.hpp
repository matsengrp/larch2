#pragma once

#include <larch/io_util.hpp>
#include <larch/load_proto_dag.hpp>
#include <larch/protobuf.hpp>
#include <larch/protobuf_encode.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

constexpr int32_t checkpoint_schema_version = 1;

// --- Proto-mirror structs (see proto/checkpoint.proto) -----------------

struct patience_state_msg {
  std::optional<int64_t> limit;
  int64_t count;
  std::optional<int64_t> best_score;
};

struct rng_state_msg {
  std::string mt19937_state;
};

struct radius_result_msg {
  int64_t radius;
  int64_t moves_found;
  int64_t moves_applied;
  int64_t parsimony_score;
};

struct optimize_result_msg {
  int64_t iteration;
  int64_t dag_node_count;
  int64_t dag_edge_count;
  int64_t trees_merged;
  int64_t parsimony_score;
  std::vector<radius_result_msg> radii;
};

struct larch_checkpoint_msg {
  int32_t schema_version;
  std::string larch2_git_sha;
  int64_t next_iteration;
  std::string optimizer;
  patience_state_msg patience;
  rng_state_msg main_rng;
  rng_state_msg drift_rng;  // empty mt19937_state == not present
  std::vector<optimize_result_msg> results;
  dag_data dag;
  std::string args_fingerprint;  // SHA-256 hex digest of args_payload
  std::string args_payload;      // JSON of fingerprinted args (for diff
                                 // reporting on mismatch)
};

}  // namespace larch

// --- std::optional<int64_t> protobuf overloads ------------------------------
// The default int64_t encoder skips zero values, but proto3 "optional uint64"
// must distinguish "0" from "unset". We emit on has_value() and decode into
// the optional unconditionally.
namespace pb {

inline void encode_field(std::optional<int64_t> const& f, uint32_t fnum,
                         writer& w) {
  if (!f.has_value()) return;
  w.write_tag(fnum, wire_type::varint);
  w.write_varint(static_cast<uint64_t>(*f));
}

inline void decode_field(std::optional<int64_t>& f, wire_type, reader& r) {
  f = static_cast<int64_t>(r.read_varint());
}

}  // namespace pb

namespace larch {

// --- mt19937 (de)serialization (round-trips exactly per C++ standard) ------

inline std::string serialize_mt19937(std::mt19937 const& rng) {
  std::ostringstream oss;
  oss << rng;
  return oss.str();
}

inline std::mt19937 deserialize_mt19937(std::string const& s) {
  std::istringstream iss{s};
  std::mt19937 rng;
  iss >> rng;
  if (iss.fail())
    throw std::runtime_error{"deserialize_mt19937: parse failed"};
  return rng;
}

// --- Format detection -----------------------------------------------------
// Phase 2 checkpoints always have schema_version >= 1, so the encoded file
// starts with the bytes [0x08, schema_version_varint]. A raw dag_data file
// (Phase 1 / legacy) starts with field 1 (edges, length-delimited) tagged
// 0x0a, or another length-delimited field if edges is empty. The byte 0x08
// uniquely identifies a larch_checkpoint. Detection assumes
// checkpoint_schema_version > 0 — proto3 omits zero-default fields on the
// wire, which would invert the magic byte test.
static_assert(checkpoint_schema_version > 0,
              "checkpoint_schema_version must be > 0 for looks_like_checkpoint "
              "to reliably detect Phase 2 files");

inline bool looks_like_checkpoint(std::span<const uint8_t> data) {
  return !data.empty() && data[0] == 0x08;
}

// --- File IO --------------------------------------------------------------

inline std::vector<char> read_checkpoint_bytes(std::string_view path) {
  return read_file(path);  // handles both gzipped and plain
}

// Atomic write: encode -> "<path>.tmp" -> rename to "<path>".
inline void write_checkpoint(larch_checkpoint_msg const& ckpt,
                             std::string_view path) {
  auto bytes = pb::encode(ckpt);
  std::string tmp = std::string{path} + ".tmp";
  {
    std::ofstream out{tmp, std::ios::binary};
    if (!out)
      throw std::runtime_error{"cannot open " + tmp + " for writing"};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error{"write failed: " + tmp};
  }
  if (std::rename(tmp.c_str(), std::string{path}.c_str()) != 0)
    throw std::runtime_error{"rename failed: " + tmp + " -> " +
                             std::string{path}};
}

inline larch_checkpoint_msg load_checkpoint_from_bytes(
    std::span<const uint8_t> data) {
  return pb::decode<larch_checkpoint_msg>(data);
}

// --- SHA-256 (for args fingerprint) ---------------------------------------

std::string sha256_hex(std::string_view data);

}  // namespace larch
