#pragma once

// WRIC DAG-native SPR & rank-3 rewrite: Phase 10 -- CLI / report / identity
// surface (cross-cutting).
//
// This header is the identity half of the Phase-10 surface: a JSON report of
// the overlay chain's entries (one per accepted delta) and their production
// taxon-set keys, plus a round-trip parser.  It exists so that "a JSON
// report's chain entries and Option-C rewrite identities survive materialize
// -> rebuild -> report without loss (keys preserved)" (Phase-10 exit
// criterion 2) is a concrete, testable property rather than a prose claim.
//
// Identity model (Work item 1):
//   * Chain position is an ORDINAL.  It orders the deltas for bottom-up /
//     top-down passes and for report readability; it is not an identity.
//   * Taxon-set key (`rank3_production_taxa_key`) is the identity that
//     survives materialization, rebuild, and report round trips.  Dedup of
//     reported intended productions uses key equality, as
//     `rank3_detail::append_unique_key` already does.
//   * The tombstone rule governs re-acceptance; it is enforced by
//     `overlay_chain::append`, not by this report.  This report merely
//     records, per entry, the base-production keys a delta tombstoned (resolved
//     against the frozen base, which never changes) so the report is faithful
//     to what the chain actually removed.
//   * `commit_source` labels each entry's commit path: `spr_overlay_delta`
//     (the Phase-4 local-commit path / Option A/B materialize-and-merge) or
//     `option_c_chain_commit` (a rank-3 Option-C rewrite committed via
//     `option_c_as_overlay_delta`, Phase 6/7).  The label is the only thing
//     that distinguishes an Option-C entry from an SPR entry; the taxon-set
//     keys are identical in kind.
//
// The added-production keys per delta are resolved against `chain.tip()` (the
// merged overlay) exactly as `overlay_chain_intended_production_key_sets`
// resolves them for Phase-5 compaction, so this report and compaction agree
// on identity.  The tombstoned keys are resolved against the frozen base via
// `production_key_from_id`, which is stable because the base never mutates.

#include <larch/overlay_chain.hpp>             // overlay_chain, materialize_overlay_chain
#include <larch/overlay_chain_compaction.hpp>  // overlay_chain_compaction_detail::
                                               //   production_key_from_overlay_production
#include <larch/rank3_rewrite.hpp>             // rank3_production_taxa_key,
                                               //   production_key_from_id,
                                               //   normalize_production_key,
                                               //   append_unique_key,
                                               //   production_key_to_string

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace larch {

// One accepted delta's identity, as carried by the Phase-10 JSON report.
struct phase10_chain_identity_entry {
  // Chain position (ordinal).  Not an identity; orders the report.
  std::size_t position = 0;
  // Commit-path label: "spr_overlay_delta" or "option_c_chain_commit".
  std::string commit_source;
  // Taxon-set keys of the productions this delta added (resolved against
  // chain.tip(), stable across materialize/rebuild).
  std::vector<rank3_production_taxa_key> added_production_keys;
  // Taxon-set keys of the frozen-base productions this delta tombstoned
  // (resolved against the frozen base, stable by construction).
  std::vector<rank3_production_taxa_key> tombstoned_production_keys;
};

struct phase10_chain_identity_report {
  std::vector<phase10_chain_identity_entry> entries;
  // The frozen base grammar's production taxon-set keys, as an identity
  // reference for the round-trip oracle (the base is the unchanging substrate
  // every delta is rebased onto).
  std::vector<rank3_production_taxa_key> base_production_keys;
};

namespace phase10_report_detail {

// Default commit-source label for an SPR overlay delta (empty commit_source).
inline std::string commit_source_label(std::string const& commit_source) {
  return commit_source.empty() ? std::string{"spr_overlay_delta"}
                               : commit_source;
}

// Sort + unique a vector of production keys (by the key's built-in operator<).
inline std::vector<rank3_production_taxa_key> sort_unique_keys(
    std::vector<rank3_production_taxa_key> keys) {
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return keys;
}

// All production taxon-set keys of a grammar (one per production), sorted +
// unique.  Used to capture the frozen base's keys and to read the
// materialized/rebuilt grammar's keys for the round-trip oracle.
inline std::vector<rank3_production_taxa_key> grammar_production_keys(
    clade_grammar const& grammar) {
  std::vector<rank3_production_taxa_key> keys;
  keys.reserve(grammar.productions.size());
  for (std::size_t pid = 0; pid < grammar.productions.size(); ++pid) {
    if (grammar.productions[pid].parent == no_clade) continue;
    keys.push_back(rank3_detail::production_key_from_id(
        grammar, static_cast<production_id>(pid)));
  }
  return sort_unique_keys(std::move(keys));
}

}  // namespace phase10_report_detail

// Build the identity report from a chain.  No mutation, no chart work; reads
// the chain's tip overlay and frozen base only.
inline phase10_chain_identity_report build_phase10_chain_identity_report(
    overlay_chain const& chain) {
  phase10_chain_identity_report report;
  report.base_production_keys =
      phase10_report_detail::grammar_production_keys(chain.base());

  if (chain.empty()) return report;

  // Resolve added productions per delta against the merged tip overlay, the
  // same resolution compaction uses, so identities agree.
  auto overlay = chain.tip();
  report.entries.reserve(chain.size());
  for (std::size_t pos = 0; pos < chain.size(); ++pos) {
    auto const& delta = chain.at(pos);
    phase10_chain_identity_entry entry;
    entry.position = pos;
    entry.commit_source =
        phase10_report_detail::commit_source_label(delta.commit_source);

    entry.added_production_keys.reserve(delta.temp_productions.size());
    for (auto const& prod : delta.temp_productions) {
      rank3_detail::append_unique_key(
          entry.added_production_keys,
          overlay_chain_compaction_detail::production_key_from_overlay_production(
              overlay, prod,
              "phase10 chain identity report delta " + std::to_string(pos) +
                  " added production"));
    }

    entry.tombstoned_production_keys.reserve(
        delta.removed_base_productions.size());
    for (auto pid : delta.removed_base_productions) {
      if (pid == no_production || pid >= chain.base().productions.size()) {
        throw std::runtime_error(
            "phase10 chain identity report: tombstoned base production " +
            std::to_string(pid) + " out of range for the frozen base at "
            "chain position " + std::to_string(pos));
      }
      rank3_detail::append_unique_key(
          entry.tombstoned_production_keys,
          rank3_detail::production_key_from_id(chain.base(), pid));
    }

    report.entries.push_back(std::move(entry));
  }
  return report;
}

// The net set of production taxon-set keys the chain represents: every base
// key minus every tombstoned key plus every added key, sorted + unique.  This
// is the identity that must survive materialize -> rebuild -> report.
inline std::vector<rank3_production_taxa_key> phase10_chain_net_production_keys(
    phase10_chain_identity_report const& report) {
  std::vector<rank3_production_taxa_key> net = report.base_production_keys;
  for (auto const& entry : report.entries) {
    for (auto const& key : entry.tombstoned_production_keys) {
      auto it = std::lower_bound(net.begin(), net.end(), key);
      if (it != net.end() && *it == key) net.erase(it);
    }
    for (auto const& key : entry.added_production_keys) {
      rank3_detail::append_unique_key(net, key);
    }
  }
  return phase10_report_detail::sort_unique_keys(std::move(net));
}

// ---- JSON emission / parsing ------------------------------------------------
//
// A minimal, self-contained JSON serializer for this specific structure (no
// general JSON library).  The grammar is fixed and is exactly what the parser
// below accepts; the parser is intentionally strict so a malformed report
// fails loudly rather than silently dropping an identity.

namespace phase10_report_detail {

inline void append_taxa(std::string& out, std::vector<taxon_id> const& taxa) {
  out += "[";
  for (std::size_t i = 0; i < taxa.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(taxa[i]);
  }
  out += "]";
}

inline void append_key(std::string& out, std::string const& field,
                       rank3_production_taxa_key const& key) {
  out += "{\"field\":\"" + field + "\",\"parent\":";
  append_taxa(out, key.parent);
  out += ",\"children\":[";
  for (std::size_t i = 0; i < key.children.size(); ++i) {
    if (i != 0) out += ",";
    append_taxa(out, key.children[i]);
  }
  out += "]}";
}

inline void append_keys(std::string& out, std::string const& field,
                        std::vector<rank3_production_taxa_key> const& keys) {
  out += "[";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) out += ",";
    append_key(out, field, keys[i]);
  }
  out += "]";
}

// Escape a JSON string literal body.  The labels used here are fixed
// identifiers, but escaping defensively keeps the emitter honest.
inline std::string escape_json_string(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  return out;
}

}  // namespace phase10_report_detail

// Emit the report as JSON.  Schema:
//   {"version":1,
//    "base_production_keys":[{"field":"base","parent":[...],"children":[...]}...],
//    "entries":[
//      {"position":0,"commit_source":"spr_overlay_delta",
//       "added_production_keys":[...],"tombstoned_production_keys":[...]}, ...]}
inline std::string emit_phase10_chain_identity_report_json(
    phase10_chain_identity_report const& report) {
  std::string out;
  out += "{\"version\":1,\"base_production_keys\":";
  phase10_report_detail::append_keys(out, "base",
                                     report.base_production_keys);
  out += ",\"entries\":[";
  for (std::size_t i = 0; i < report.entries.size(); ++i) {
    auto const& entry = report.entries[i];
    if (i != 0) out += ",";
    out += "{\"position\":" + std::to_string(entry.position);
    out += ",\"commit_source\":\"" +
           phase10_report_detail::escape_json_string(entry.commit_source) +
           "\"";
    out += ",\"added_production_keys\":";
    phase10_report_detail::append_keys(out, "added",
                                       entry.added_production_keys);
    out += ",\"tombstoned_production_keys\":";
    phase10_report_detail::append_keys(out, "tombstoned",
                                       entry.tombstoned_production_keys);
    out += "}";
  }
  out += "]}";
  return out;
}

namespace phase10_report_detail {

// Minimal strict JSON parser scoped to this report's schema.  It is NOT a
// general JSON parser; it accepts exactly the grammar `emit_*` produces and
// throws std::runtime_error on any deviation, so a truncated/corrupt report
// fails loudly.
class json_reader {
 public:
  explicit json_reader(std::string_view text) : text_(text) {}

  char peek() {
    skip_ws();
    if (pos_ >= text_.size()) {
      throw std::runtime_error("phase10 json: unexpected end of input");
    }
    return text_[pos_];
  }
  char get() {
    skip_ws();
    if (pos_ >= text_.size()) {
      throw std::runtime_error("phase10 json: unexpected end of input");
    }
    return text_[pos_++];
  }
  void expect(char c) {
    if (get() != c) {
      throw std::runtime_error(
          std::string{"phase10 json: expected '"} + c +
          "' at offset " + std::to_string(pos_));
    }
  }
  void skip_ws() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }
  std::string read_string() {
    expect('"');
    std::string out;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
        char esc = text_[pos_ + 1];
        switch (esc) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          default:
            throw std::runtime_error(
                std::string{"phase10 json: bad escape '\\"} + esc + "'");
        }
        pos_ += 2;
      } else {
        out += text_[pos_++];
      }
    }
    if (pos_ >= text_.size()) {
      throw std::runtime_error("phase10 json: unterminated string");
    }
    ++pos_;  // consume closing quote
    return out;
  }
  std::size_t read_size() {
    skip_ws();
    std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') {
      throw std::runtime_error("phase10 json: negative integer not allowed");
    }
    while (pos_ < text_.size() &&
           text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    if (start == pos_) {
      throw std::runtime_error("phase10 json: expected integer");
    }
    return static_cast<std::size_t>(
        std::stoul(std::string{text_.substr(start, pos_ - start)}));
  }
  taxon_id read_taxon() {
    skip_ws();
    std::size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    if (start == pos_) {
      throw std::runtime_error("phase10 json: expected taxon id");
    }
    return static_cast<taxon_id>(
        std::stoul(std::string{text_.substr(start, pos_ - start)}));
  }
  std::vector<taxon_id> read_taxa() {
    expect('[');
    std::vector<taxon_id> taxa;
    if (peek() != ']') {
      taxa.push_back(read_taxon());
      while (peek() == ',') {
        ++pos_;
        taxa.push_back(read_taxon());
      }
    }
    expect(']');
    return taxa;
  }
  rank3_production_taxa_key read_key(std::string const& expected_field) {
    expect('{');
    // "field"
    auto f = read_field_name();
    if (f != "field") {
      throw std::runtime_error("phase10 json: key object must start with field");
    }
    expect(':');
    auto field = read_string();
    if (!expected_field.empty() && field != expected_field) {
      throw std::runtime_error(
          "phase10 json: expected field '" + expected_field + "' got '" +
          field + "'");
    }
    expect(',');
    // "parent"
    auto p = read_field_name();
    if (p != "parent") {
      throw std::runtime_error("phase10 json: expected 'parent' field");
    }
    expect(':');
    rank3_production_taxa_key key;
    key.parent = read_taxa();
    expect(',');
    // "children"
    auto c = read_field_name();
    if (c != "children") {
      throw std::runtime_error("phase10 json: expected 'children' field");
    }
    expect(':');
    expect('[');
    if (peek() != ']') {
      key.children.push_back(read_taxa());
      while (peek() == ',') {
        ++pos_;
        key.children.push_back(read_taxa());
      }
    }
    expect(']');
    expect('}');
    rank3_detail::normalize_production_key(key);
    return key;
  }
  std::vector<rank3_production_taxa_key> read_keys(
      std::string const& expected_field) {
    expect('[');
    std::vector<rank3_production_taxa_key> keys;
    if (peek() != ']') {
      keys.push_back(read_key(expected_field));
      while (peek() == ',') {
        ++pos_;
        keys.push_back(read_key(expected_field));
      }
    }
    expect(']');
    return keys;
  }
  // Read a field name (assumes positioned at the opening quote).
  std::string read_field_name() {
    return read_string();
  }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

}  // namespace phase10_report_detail

// Parse a JSON report (as emitted by emit_phase10_chain_identity_report_json)
// back into the structured report.  Throws std::runtime_error on any
// deviation from the emitter's schema.
inline phase10_chain_identity_report parse_phase10_chain_identity_report_json(
    std::string_view json) {
  phase10_chain_identity_report report;
  phase10_report_detail::json_reader r{json};

  r.expect('{');
  // "version": 1
  if (r.read_field_name() != "version") {
    throw std::runtime_error("phase10 json: expected 'version' first");
  }
  r.expect(':');
  if (r.read_size() != 1) {
    throw std::runtime_error("phase10 json: unsupported report version");
  }
  r.expect(',');

  // "base_production_keys": [...]
  if (r.read_field_name() != "base_production_keys") {
    throw std::runtime_error("phase10 json: expected 'base_production_keys'");
  }
  r.expect(':');
  report.base_production_keys = r.read_keys("base");
  r.expect(',');

  // "entries": [...]
  if (r.read_field_name() != "entries") {
    throw std::runtime_error("phase10 json: expected 'entries'");
  }
  r.expect(':');
  r.expect('[');
  if (r.peek() != ']') {
    do {
      if (r.peek() == ',') r.get();
      phase10_chain_identity_entry entry;
      r.expect('{');
      if (r.read_field_name() != "position") {
        throw std::runtime_error("phase10 json: expected 'position'");
      }
      r.expect(':');
      entry.position = r.read_size();
      r.expect(',');
      if (r.read_field_name() != "commit_source") {
        throw std::runtime_error("phase10 json: expected 'commit_source'");
      }
      r.expect(':');
      entry.commit_source = r.read_string();
      r.expect(',');
      if (r.read_field_name() != "added_production_keys") {
        throw std::runtime_error("phase10 json: expected 'added_production_keys'");
      }
      r.expect(':');
      entry.added_production_keys = r.read_keys("added");
      r.expect(',');
      if (r.read_field_name() != "tombstoned_production_keys") {
        throw std::runtime_error(
            "phase10 json: expected 'tombstoned_production_keys'");
      }
      r.expect(':');
      entry.tombstoned_production_keys = r.read_keys("tombstoned");
      r.expect('}');
      report.entries.push_back(std::move(entry));
    } while (r.peek() == ',');
  }
  r.expect(']');
  r.expect('}');
  return report;
}

}  // namespace larch
