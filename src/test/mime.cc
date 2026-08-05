// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#include "common/mime.h"
#include "rgw/rgw_http_fields.h"
#include "rgw/rgw_xattr.h"
#include "gtest/gtest.h"

#include <stdint.h>
#include <string>

using std::string;

TEST(MimeTests, SimpleEncode) {
  char output[256];
  memset(output, 0, sizeof(output));
  int len;

  len = mime_encode_as_qp("abc", NULL, 0);
  ASSERT_EQ(len, 4);
  len = mime_encode_as_qp("abc", output, 4);
  ASSERT_EQ(len, 4);
  ASSERT_EQ(string("abc"), string(output));

  len = mime_encode_as_qp("a=b", NULL, 0);
  ASSERT_EQ(len, 6);
  len = mime_encode_as_qp("a=b", output, 6);
  ASSERT_EQ(len, 6);
  ASSERT_EQ(string("a=3Db"), string(output));

  len = mime_encode_as_qp("Libert\xc3\xa9", NULL, 0);
  ASSERT_EQ(len, 13);
  len = mime_encode_as_qp("Libert\xc3\xa9", output, 13);
  ASSERT_EQ(len, 13);
  ASSERT_EQ(string("Libert=C3=A9"), string(output));
}

TEST(MimeTests, EncodeOutOfSpace) {
  char output[256];
  memset(output, 0, sizeof(output));
  int len;

  len = mime_encode_as_qp("abcdefg", NULL, 0);
  ASSERT_EQ(len, 8);
  len = mime_encode_as_qp("abcdefg", output, 4);
  ASSERT_EQ(len, 8);
  ASSERT_EQ(string("abc"), string(output));
  len = mime_encode_as_qp("abcdefg", output, 1);
  ASSERT_EQ(len, 8);
  ASSERT_EQ(string(""), string(output));

  len = mime_encode_as_qp("a=b", output, 2);
  ASSERT_EQ(len, 6);
  ASSERT_EQ(string("a"), string(output));
  len = mime_encode_as_qp("a=b", output, 3);
  ASSERT_EQ(len, 6);
  ASSERT_EQ(string("a"), string(output));
}

TEST(MimeTests, LengthAwareEncodeAllControls) {
  std::string input;
  for (int c = 0; c < 0x20; ++c) {
    input.push_back(static_cast<char>(c));
  }
  input.push_back('\x7f');

  const std::string expected =
      "=00=01=02=03=04=05=06=07=08=09=0A=0B=0C=0D=0E=0F"
      "=10=11=12=13=14=15=16=17=18=19=1A=1B=1C=1D=1E=1F=7F";
  char output[128] = {};

  int len = mime_encode_as_qp_len(input.data(), input.size(), nullptr, 0);
  ASSERT_EQ(expected.size() + 1, static_cast<size_t>(len));
  ASSERT_EQ(len, mime_encode_as_qp_len(input.data(), input.size(), output,
                                       sizeof(output)));
  EXPECT_EQ(expected, std::string(output));
}

TEST(MimeTests, LengthAwareEncodeMixedNullAndControls) {
  const std::string input("A\0B\rC", 5);
  const std::string expected = "A=00B=0DC";
  char output[32] = {};

  const int len = mime_encode_as_qp_len(input.data(), input.size(), output,
                                        sizeof(output));
  ASSERT_EQ(expected.size() + 1, static_cast<size_t>(len));
  EXPECT_EQ(expected, std::string(output));
}

TEST(MimeTests, FormatXattrPreservesMixedNullAndControls) {
  std::string mixed("A\0B\rC", 5);
  format_xattr(mixed);
  EXPECT_EQ("=?UTF-8?Q?A=00B=0DC?=", mixed);

  std::string null_only(1, '\0');
  format_xattr(null_only);
  EXPECT_EQ("=?UTF-8?Q?=00?=", null_only);
}

TEST(HttpFieldTests, NamesRequireAsciiTchar) {
  EXPECT_FALSE(rgw::http::is_valid_field_name(""));
  EXPECT_TRUE(rgw::http::is_valid_field_name(
      "AZaz09!#$%&'*+-.^_`|~"));
  EXPECT_TRUE(rgw::http::is_valid_field_name("x-amz-meta-safe_name"));

  for (int c = 0; c <= 0xff; ++c) {
    const std::string name(1, static_cast<char>(c));
    EXPECT_EQ(rgw::http::is_tchar(static_cast<unsigned char>(c)),
              rgw::http::is_valid_field_name(name)) << "byte " << c;
  }

  EXPECT_FALSE(rgw::http::is_valid_field_name("bad name"));
  EXPECT_FALSE(rgw::http::is_valid_field_name("bad\tname"));
  EXPECT_FALSE(rgw::http::is_valid_field_name("bad:name"));
  EXPECT_FALSE(rgw::http::is_valid_field_name("bad/name"));
  EXPECT_FALSE(rgw::http::is_valid_field_name(std::string("bad\0name", 8)));
  EXPECT_FALSE(rgw::http::is_valid_field_name("bad\xc3\xa9"));
}

TEST(HttpFieldTests, ValuesReplaceAllForbiddenControls) {
  std::string input;
  std::string expected;
  for (int c = 0; c < 0x20; ++c) {
    input.push_back(static_cast<char>(c));
    expected.push_back(c == '\t' ? '\t' : ' ');
  }
  input.push_back('\x7f');
  expected.push_back(' ');

  std::string backing;
  const std::string_view sanitized =
      rgw::http::sanitize_field_value(input, backing);
  EXPECT_EQ(expected, sanitized);
}

TEST(MimeTests, SimpleDecode) {
  char output[256];
  memset(output, 0, sizeof(output));
  int len;

  len = mime_decode_from_qp("abc", NULL, 0);
  ASSERT_EQ(len, 4);
  len = mime_decode_from_qp("abc", output, 4);
  ASSERT_EQ(len, 4);
  ASSERT_EQ(string("abc"), string(output));

  len = mime_decode_from_qp("a=3Db", NULL, 0);
  ASSERT_EQ(len, 4);
  len = mime_decode_from_qp("a=3Db", output, 4);
  ASSERT_EQ(len, 4);
  ASSERT_EQ(string("a=b"), string(output));

  len = mime_decode_from_qp("Libert=C3=A9", NULL, 0);
  ASSERT_EQ(len, 9);
  len = mime_decode_from_qp("Libert=C3=A9", output, 9);
  ASSERT_EQ(len, 9);
  ASSERT_EQ(string("Libert\xc3\xa9"), string(output));
}

TEST(MimeTests, LowercaseDecode) {
  char output[256];
  memset(output, 0, sizeof(output));
  int len;

  len = mime_decode_from_qp("Libert=c3=a9", NULL, 0);
  ASSERT_EQ(len, 9);
  len = mime_decode_from_qp("Libert=c3=a9", output, 9);
  ASSERT_EQ(len, 9);
  ASSERT_EQ(string("Libert\xc3\xa9"), string(output));
}

TEST(MimeTests, DecodeOutOfSpace) {
  char output[256];
  memset(output, 0, sizeof(output));
  int len;

  len = mime_decode_from_qp("abcdefg", NULL, 0);
  ASSERT_EQ(len, 8);
  len = mime_decode_from_qp("abcdefg", output, 4);
  ASSERT_EQ(len, 8);
  ASSERT_EQ(string("abc"), string(output));
  len = mime_decode_from_qp("abcdefg", output, 1);
  ASSERT_EQ(len, 8);
  ASSERT_EQ(string(""), string(output));

  len = mime_decode_from_qp("a=3Db", output, 2);
  ASSERT_EQ(len, 4);
  ASSERT_EQ(string("a"), string(output));
  len = mime_decode_from_qp("a=3Db", output, 3);
  ASSERT_EQ(len, 4);
  ASSERT_EQ(string("a="), string(output));
}

TEST(MimeTests, DecodeErrors) {
  char output[128];
  memset(output, 0, sizeof(output));
  int len;

  // incomplete escape sequence
  len = mime_decode_from_qp("boo=", output, sizeof(output));
  ASSERT_LT(len, 0);

  // invalid escape sequences
  len = mime_decode_from_qp("boo=gg", output, sizeof(output));
  ASSERT_LT(len, 0);
  len = mime_decode_from_qp("boo=g", output, sizeof(output));
  ASSERT_LT(len, 0);
  len = mime_decode_from_qp("boo==", output, sizeof(output));
  ASSERT_LT(len, 0);
  len = mime_decode_from_qp("boo=44bar=z", output, sizeof(output));
  ASSERT_LT(len, 0);

  // high bit should not be set in quoted-printable mime output
  unsigned char bad_input2[] = { 0x81, 0x6a, 0x0 };
  len = mime_decode_from_qp(reinterpret_cast<const char*>(bad_input2),
			    output, sizeof(output));
  ASSERT_LT(len, 0);
}
