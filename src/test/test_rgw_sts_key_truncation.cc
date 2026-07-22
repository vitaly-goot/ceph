// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * These tests pin down how an operator-supplied `rgw_sts_key` is actually
 * consumed. RGW STS issuance (rgw_sts.cc, Credentials::generateCredentials)
 * and validation (rgw_rest_s3.cc, RGWHandler_REST_S3::decrypt_session_token)
 * both do exactly this, with the cipher hard-coded:
 *
 *   auto cryptohandler = cct->get_crypto_manager()->get_handler(CEPH_CRYPTO_AES);
 *   buffer::ptr secret(secret_s.c_str(), secret_s.length());
 *   cryptohandler->validate_secret(secret);
 *   auto keyhandler = cryptohandler->get_key_handler(secret, error);
 *   keyhandler->encrypt(cct, input, enc_output, &error);
 *
 * The sequence below is a faithful copy of it, so the results here describe the
 * STS token path even though no RGW code is linked in.
 *
 * The claim under test is that an operator can opt in to AES256KRB5 by rotating
 * `rgw_sts_key` to a 32-character value. If that were true, a 32-character key
 * would have to change something observable about the resulting ciphertext.
 */

#include <memory>
#include <string>

#include "auth/Crypto.h"
#include "common/ceph_context.h"
#include "global/global_context.h"
#include "gtest/gtest.h"

using ceph::bufferlist;
using ceph::bufferptr;

namespace {

// A 32-character key, and its first half. If the trailing 16 characters
// contributed to the key schedule these two would encrypt differently.
constexpr const char* KEY_32 = "0123456789abcdefGHIJKLMNOPQRSTUV";
constexpr const char* KEY_16_PREFIX = "0123456789abcdef";
// Same first 16 characters as KEY_32, different trailing 16.
constexpr const char* KEY_32_SAME_PREFIX = "0123456789abcdef################";
// Differs from KEY_32 within the first 16 characters.
constexpr const char* KEY_32_DIFF_PREFIX = "X123456789abcdefGHIJKLMNOPQRSTUV";

std::string to_hex(const bufferlist& bl)
{
  static constexpr const char* digits = "0123456789abcdef";
  const std::string raw = bl.to_str();
  std::string out;
  out.reserve(raw.size() * 2);
  for (const unsigned char c : raw) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 0xf]);
  }
  return out;
}

// Mirrors the STS encrypt path. Returns the ciphertext as hex, or "" on error.
std::string encrypt_like_sts(const std::string& key,
                             const std::string& plaintext)
{
  auto* cct = g_ceph_context;
  auto cryptohandler = cct->get_crypto_manager()->get_handler(CEPH_CRYPTO_AES);
  if (!cryptohandler) {
    return {};
  }
  bufferptr secret(key.c_str(), key.length());
  if (cryptohandler->validate_secret(secret) < 0) {
    return {};
  }
  std::string error;
  std::unique_ptr<CryptoKeyHandler> keyhandler(
    cryptohandler->get_key_handler(secret, error));
  if (!keyhandler) {
    return {};
  }
  bufferlist input, enc_output;
  input.append(plaintext);
  if (keyhandler->encrypt(cct, input, enc_output, &error) < 0) {
    return {};
  }
  return to_hex(enc_output);
}

// The STS path asks for CEPH_CRYPTO_AES by name, so no key can steer it to
// AES256KRB5. AES_KEY_LEN is 16, i.e. AES-128.
TEST(RgwStsKey, StsPathIsAlwaysAes128NeverAes256Krb5)
{
  auto* cct = g_ceph_context;
  auto cryptohandler = cct->get_crypto_manager()->get_handler(CEPH_CRYPTO_AES);
  ASSERT_NE(nullptr, cryptohandler);
  EXPECT_EQ(CEPH_CRYPTO_AES, cryptohandler->get_type());
  EXPECT_NE(CEPH_CRYPTO_AES256KRB5, cryptohandler->get_type());
}

// A 32-character key is accepted without complaint: validate_secret tests
// `length() < AES_KEY_LEN`, not `!= AES_KEY_LEN`. The operator therefore gets
// no error, no warning, and no signal that the longer key did nothing.
TEST(RgwStsKey, LongKeyIsAcceptedWithoutSignallingAnUpgrade)
{
  auto* cct = g_ceph_context;
  auto aes = cct->get_crypto_manager()->get_handler(CEPH_CRYPTO_AES);
  ASSERT_NE(nullptr, aes);

  bufferptr secret_16(KEY_16_PREFIX, strlen(KEY_16_PREFIX));
  bufferptr secret_32(KEY_32, strlen(KEY_32));
  EXPECT_EQ(0, aes->validate_secret(secret_16));
  EXPECT_EQ(0, aes->validate_secret(secret_32));

  // AES256KRB5 is a separate handler with its own 32-byte minimum. The STS
  // path never consults it, so this is the upgrade that is not reachable.
  auto krb5 = cct->get_crypto_manager()->get_handler(CEPH_CRYPTO_AES256KRB5);
  ASSERT_NE(nullptr, krb5);
  EXPECT_EQ(-EINVAL, krb5->validate_secret(secret_16));
  EXPECT_EQ(0, krb5->validate_secret(secret_32));
}

// The core result: CryptoAESKeyHandler::init() keys AES at a hard-coded
// AES_KEY_LEN * CHAR_BIT (128) bits, so only the first 16 bytes of the secret
// are used. Rotating `rgw_sts_key` to 32 characters is a silent no-op.
TEST(RgwStsKey, ThirtyTwoCharKeyIsSilentlyTruncatedToFirstSixteen)
{
  const std::string plaintext = "session-token-plaintext";

  const std::string ct_32 = encrypt_like_sts(KEY_32, plaintext);
  const std::string ct_16 = encrypt_like_sts(KEY_16_PREFIX, plaintext);
  ASSERT_FALSE(ct_32.empty());
  ASSERT_FALSE(ct_16.empty());

  // A 32-char key and its own first 16 chars produce identical ciphertext:
  // the trailing half of the operator's key is discarded.
  EXPECT_EQ(ct_16, ct_32)
    << "expected the trailing 16 characters of rgw_sts_key to be ignored";

  // Two different 32-char keys that share only their first 16 characters are
  // indistinguishable, which is the same statement from the other direction.
  const std::string ct_same_prefix = encrypt_like_sts(KEY_32_SAME_PREFIX,
                                                      plaintext);
  ASSERT_FALSE(ct_same_prefix.empty());
  EXPECT_EQ(ct_32, ct_same_prefix)
    << "two 32-char keys differing only after byte 16 must not collide";

  // Sanity check that the first 16 characters do matter, so the equalities
  // above are truncation and not a broken test.
  const std::string ct_diff_prefix = encrypt_like_sts(KEY_32_DIFF_PREFIX,
                                                      plaintext);
  ASSERT_FALSE(ct_diff_prefix.empty());
  EXPECT_NE(ct_32, ct_diff_prefix);
}

// The STS path uses the no-IV overload, which reinitialises the IV from the
// fixed CEPH_AES_IV constant on every call, so encryption is deterministic.
// The caller-supplied random-IV API added for user-secret encryption would
// make these two ciphertexts differ; STS was never migrated to it.
TEST(RgwStsKey, StsPathUsesStaticIvSoEncryptionIsDeterministic)
{
  const std::string plaintext = "session-token-plaintext";
  const std::string first = encrypt_like_sts(KEY_32, plaintext);
  const std::string second = encrypt_like_sts(KEY_32, plaintext);
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first, second)
    << "a per-record random IV would make these differ";
}

} // anonymous namespace
