// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace rgw::http {

// RFC 9110 section 5.6.2 tchar. HTTP field names must be nonempty ASCII
// tokens; whitespace, separators such as ':', controls, and obs-text are not
// valid in a field name.
constexpr bool is_tchar(const unsigned char c)
{
  if ((c >= '0' && c <= '9') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z')) {
    return true;
  }

  switch (c) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

inline bool is_valid_field_name(const std::string_view name)
{
  return !name.empty() &&
         std::all_of(name.begin(), name.end(), [](const char c) {
           return is_tchar(static_cast<unsigned char>(c));
         });
}

// Field values may contain HTAB, but no other C0 control byte or DEL. Replace
// every forbidden byte so lenient HTTP parsers cannot interpret it as framing.
constexpr bool is_forbidden_field_value_char(const unsigned char c)
{
  return (c < 0x20 && c != '\t') || c == 0x7f;
}

// Returns a view over field when it is already safe, or over backing after a
// copy-on-first-invalid-byte sanitization. The caller must keep backing alive.
inline std::string_view sanitize_field_value(const std::string_view field,
                                             std::string& backing)
{
  const auto is_forbidden = [](const char c) {
    return is_forbidden_field_value_char(static_cast<unsigned char>(c));
  };
  const auto pos = std::find_if(field.begin(), field.end(), is_forbidden);
  if (pos == field.end()) {
    return field;
  }

  backing.assign(field.data(), field.size());
  std::replace_if(backing.begin() + (pos - field.begin()), backing.end(),
                  is_forbidden, ' ');
  return backing;
}

} // namespace rgw::http
