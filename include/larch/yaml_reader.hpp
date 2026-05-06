#pragma once

#include <larch/io_util.hpp>

#include <charconv>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace larch {

// A YAML value: either a scalar string or a map of key->value.
// Supports the simple subset used by netam model config files
// (flat key-value with at most one level of nesting).
struct yaml_value {
  std::string scalar;
  std::map<std::string, yaml_value, std::less<>> map;

  bool is_map() const { return scalar.empty(); }

  std::string const& as_string() const { return scalar; }

  int64_t as_int() const {
    int64_t v{};
    auto [ptr, ec] =
        std::from_chars(scalar.data(), scalar.data() + scalar.size(), v);
    if (ec != std::errc{} || ptr != scalar.data() + scalar.size())
      throw std::runtime_error{"yaml: cannot convert to int: " + scalar};
    return v;
  }

  double as_float() const {
    // std::from_chars for double may not handle all YAML float notations
    // (e.g. 1.0e-06), so use stod.
    try {
      std::size_t pos = 0;
      double value = std::stod(scalar, &pos);
      if (pos != scalar.size())
        throw std::runtime_error{"trailing characters"};
      return value;
    } catch (...) {
      throw std::runtime_error{"yaml: cannot convert to float: " + scalar};
    }
  }

  bool as_bool() const {
    if (scalar == "true" || scalar == "True" || scalar == "yes") return true;
    if (scalar == "false" || scalar == "False" || scalar == "no") return false;
    throw std::runtime_error{"yaml: cannot convert to bool: " + scalar};
  }
};

using yaml_doc = std::map<std::string, yaml_value, std::less<>>;

inline yaml_value const& yaml_require_key(yaml_doc const& map,
                                          std::string_view path,
                                          std::string_view key,
                                          std::string_view context = {}) {
  auto it = map.find(key);
  if (it == map.end()) {
    std::string msg = "yaml: " + std::string{path} + ": missing key '" +
                      std::string{key} + "'";
    if (!context.empty()) msg += " in '" + std::string{context} + "'";
    throw std::runtime_error{msg};
  }
  return it->second;
}

inline yaml_doc const& yaml_require_map(yaml_doc const& map,
                                        std::string_view path,
                                        std::string_view key,
                                        std::string_view context = {}) {
  auto const& value = yaml_require_key(map, path, key, context);
  if (!value.is_map()) {
    std::string msg = "yaml: " + std::string{path} + ": expected map at key '" +
                      std::string{key} + "'";
    if (!context.empty()) msg += " in '" + std::string{context} + "'";
    throw std::runtime_error{msg};
  }
  return value.map;
}

// Parse a simple YAML file.
// Supports: top-level key: value, indented subkey: value (one nesting level),
// # comments, blank lines, {} for empty maps.
inline yaml_doc read_yaml(std::string_view path) {
  auto bytes = read_file(path);
  std::string_view content{bytes.data(), bytes.size()};

  yaml_doc doc;
  std::string const* current_section = nullptr;  // top-level key owning a map

  auto parse_error = [&](std::size_t line_no, std::string_view msg) {
    throw std::runtime_error{"yaml: " + std::string{path} + ":" +
                             std::to_string(line_no) + ": " +
                             std::string{msg}};
  };

  std::size_t pos = 0;
  std::size_t line_no = 0;
  while (pos < content.size()) {
    ++line_no;
    auto eol = content.find('\n', pos);
    if (eol == std::string_view::npos) eol = content.size();
    auto line = content.substr(pos, eol - pos);
    pos = eol + 1;

    // Strip \r
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    // Skip blank lines and comments.
    auto first_nonspace = line.find_first_not_of(" \t");
    if (first_nonspace == std::string_view::npos) continue;
    if (line[first_nonspace] == '#') continue;

    if (line.find('\t') != std::string_view::npos)
      parse_error(line_no, "tabs are not supported");

    bool indented = false;
    if (first_nonspace == 0) {
      indented = false;
    } else if (first_nonspace == 2) {
      indented = true;
    } else {
      parse_error(line_no, "unsupported indentation (expected 0 or 2 spaces)");
    }

    // Trim leading whitespace for parsing.
    auto trimmed = line.substr(first_nonspace);

    // Find colon separator.
    auto colon = trimmed.find(':');
    if (colon == std::string_view::npos)
      parse_error(line_no, "expected ':' separator");

    auto key = trimmed.substr(0, colon);
    // Trim trailing whitespace from key.
    while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
    if (key.empty()) parse_error(line_no, "empty key");

    auto val = trimmed.substr(colon + 1);
    // Trim leading whitespace from value.
    auto val_start = val.find_first_not_of(' ');
    if (val_start != std::string_view::npos)
      val = val.substr(val_start);
    else
      val = {};

    // Trim trailing whitespace from value.
    while (!val.empty() && val.back() == ' ') val.remove_suffix(1);

    // Strip inline comments (only if preceded by space).
    if (auto hash = val.find(" #"); hash != std::string_view::npos) {
      val = val.substr(0, hash);
      while (!val.empty() && val.back() == ' ') val.remove_suffix(1);
    }

    std::string key_str{key};
    std::string val_str{val};

    if (indented) {
      if (!current_section)
        parse_error(line_no, "indented key without a parent map");
      if (val_str.empty() || val_str == "{}")
        parse_error(line_no, "nested maps are not supported");
      // Nested key under current section.
      doc[*current_section].map[std::move(key_str)] =
          yaml_value{.scalar = std::move(val_str)};
    } else {
      // Top-level key.
      if (val_str.empty()) {
        // Start of a map section.
        doc[key_str] = yaml_value{};
        current_section = &doc.find(key_str)->first;
      } else if (val_str == "{}") {
        // Explicit empty map.
        doc[std::move(key_str)] = yaml_value{};
        current_section = nullptr;
      } else {
        doc[std::move(key_str)] = yaml_value{.scalar = std::move(val_str)};
        current_section = nullptr;
      }
    }
  }

  return doc;
}

}  // namespace larch
