// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

#pragma once

#include <string>

#include "common/mime.h"
#include "common/utf8.h"

inline void format_xattr(std::string& xattr)
{
  /* If the extended attribute is not valid UTF-8 or contains control bytes,
   * encode every input byte using a length-aware quoted-printable encoder.
   * Embedded nulls need an explicit check because the legacy UTF-8/control
   * helpers intentionally do not classify NUL as a control character. */
  if (xattr.find('\0') != std::string::npos ||
      check_utf8(xattr.data(), xattr.size()) != 0 ||
      check_for_control_characters(xattr.data(), xattr.size()) != 0) {
    static constexpr char MIME_PREFIX[] = "=?UTF-8?Q?";
    static constexpr char MIME_SUFFIX[] = "?=";

    const int encoded_len =
        mime_encode_as_qp_len(xattr.data(), xattr.size(), nullptr, 0);
    if (encoded_len < 0) {
      return;
    }

    std::string encoded(static_cast<size_t>(encoded_len), '\0');
    mime_encode_as_qp_len(xattr.data(), xattr.size(), encoded.data(),
                          encoded_len);
    encoded.resize(static_cast<size_t>(encoded_len - 1));

    xattr.assign(MIME_PREFIX);
    xattr.append(encoded);
    xattr.append(MIME_SUFFIX);
  }
}
