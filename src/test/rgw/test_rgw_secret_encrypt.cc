// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "global/global_init.h"
#include <unistd.h>
#include <cstdio>
#include <string>
#include <tuple>
#include "common/ceph_argparse.h"
#include "rgw/rgw_common.h"
#include "rgw/rgw_rados.h"
#include "rgw/rgw_secret_encryption.h"
#include <gtest/gtest.h>
#include "include/ceph_assert.h"

#define dout_subsys ceph_subsys_rgw

using namespace std;

static ceph::bufferlist string_buffer(std::string_view value) {
  ceph::bufferlist bl;
  bl.append(value);
  return bl;
}

static auto cct = new CephContext(CEPH_ENTITY_TYPE_CLIENT);

static constexpr bool ENABLED = true;
static constexpr bool DISABLED = false;
static constexpr bool WITH_ERROR = true;
static constexpr bool WITHOUT_ERROR = false;
static constexpr uint64_t RELOAD_INTERVAL = 0; // To speed up test

// Only to increase readability
using EXPECT_KEY_ID = uint32_t;

class TestRGWSecretEncrypt : public ::testing::Test {
protected:
  TestRGWSecretEncrypt() : 
    key_file(std::string("/tmp/ceph_rgw_secret_encrypt_keys.json.") + std::to_string(getpid())),
    secret_to_protect(string_buffer("OurVeryBigSecret")) {
  }

  ~TestRGWSecretEncrypt() {
    remove(key_file.c_str());
  }

  void update_key_file(const string &content)
  {
    ofstream kf(key_file);
    kf << content;
  }

  std::string key_file;

  ceph::bufferlist secret_to_protect;
};

static void ASSERT_DECRYPT_SUCC(const ceph::bufferlist& source,
                                const ceph::bufferlist& encrypted,
                                uint32_t key_id,
                                uint32_t key_id_to_expect, 
                                const std::string& iv, 
                                const std::string& differentiator)
{
  std::string iv_copy = iv; // decrypt() damage IV input so use a copy
  ceph::bufferptr iv_for_decrypt(iv_copy.data(), iv_copy.length());

  bool success;
  uint32_t expect_key_id;
  ceph::bufferlist decrypted;
  std::tie(success, expect_key_id, decrypted) = rgw::secret::encrypter()->decrypt(key_id, encrypted, iv_for_decrypt, differentiator);
  ASSERT_TRUE(success);
  ASSERT_EQ(expect_key_id, key_id_to_expect);
  ASSERT_EQ(source, decrypted);
}

static void ASSERT_DECRYPT_FAIL(const ceph::bufferlist& source,
                                const ceph::bufferlist& encrypted,
                                uint32_t key_id,
                                uint32_t key_id_to_expect, 
                                const std::string& iv, 
                                const std::string& differentiator,
                                bool with_error)
{
  std::string iv_copy = iv;
  ceph::bufferptr iv_for_decrypt(iv_copy.data(), iv_copy.length());

  bool success;
  uint32_t expect_key_id;
  ceph::bufferlist decrypted;
  std::tie(success, expect_key_id, decrypted) = rgw::secret::encrypter()->decrypt(key_id, encrypted, iv_for_decrypt, differentiator);
  if (with_error) {
    ASSERT_FALSE(success);
  } else {
    ASSERT_TRUE(success);
  }
  ASSERT_EQ(expect_key_id, key_id_to_expect);
  ASSERT_NE(source, decrypted);
}

TEST_F(TestRGWSecretEncrypt, disabled)
{
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" }
      ]
  )");
  rgw::secret::init_encrypter(cct, DISABLED, key_file, RELOAD_INTERVAL);

  uint32_t key_id;
  string iv;
  ceph::bufferlist encrypted;

  std::tie(key_id, iv, encrypted) = rgw::secret::encrypter()->encrypt(secret_to_protect, "differentiator");
  ASSERT_EQ(key_id, 0);
  ASSERT_EQ(secret_to_protect, encrypted); // Not encrypted
}

TEST_F(TestRGWSecretEncrypt, disabled_after_enabled)
{
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  uint32_t key_id;
  string iv;
  ceph::bufferlist encrypted;

  std::tie(key_id, iv, encrypted) = rgw::secret::encrypter()->encrypt(secret_to_protect, "differentiator");
  ASSERT_EQ(key_id, 1);
  ASSERT_NE(secret_to_protect, encrypted); // Encrypted

  // Now disable it
  rgw::secret::init_encrypter(cct, DISABLED, key_file, RELOAD_INTERVAL);

  ASSERT_DECRYPT_SUCC(secret_to_protect, encrypted, key_id, EXPECT_KEY_ID(0), iv, "differentiator");
}

TEST_F(TestRGWSecretEncrypt, encrypt_decrypt)
{
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  uint32_t key_id;
  string iv;
  ceph::bufferlist encrypted;

  std::tie(key_id, iv, encrypted) = rgw::secret::encrypter()->encrypt(secret_to_protect, "differentiator");
  ASSERT_EQ(key_id, 1);
  ASSERT_NE(secret_to_protect, encrypted);

  ASSERT_DECRYPT_FAIL(secret_to_protect, encrypted, key_id, key_id, iv, "different_differentiator", WITHOUT_ERROR);

  ASSERT_DECRYPT_SUCC(secret_to_protect, encrypted, key_id, key_id, iv, "differentiator");
}

TEST_F(TestRGWSecretEncrypt, key_rotation)
{
  // First emulate encryption done by a machine with newer keys
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" },
        { "id": 3, "key": "2345678901234501" },
        { "id": 4, "key": "3456789012345012" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  uint32_t key_id;
  string iv;
  ceph::bufferlist encrypted;

  std::tie(key_id, iv, encrypted) = rgw::secret::encrypter()->encrypt(secret_to_protect, "differentiator");
  ASSERT_EQ(key_id, 3);
  ASSERT_NE(secret_to_protect, encrypted);

  // Then emulate decryption done by a machine with older keys but still being able to decrypt
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" },
        { "id": 3, "key": "2345678901234501" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  ASSERT_DECRYPT_SUCC(secret_to_protect, encrypted, key_id, EXPECT_KEY_ID(2), iv, "differentiator");

  // Finally emulate decryption done by a machine with much older keys. It has to reload key file in order to decrypt
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  ASSERT_DECRYPT_FAIL(secret_to_protect, encrypted, key_id, EXPECT_KEY_ID(0), iv, "differentiator", WITH_ERROR);

  // Bring key file up-to-date with even newer keys
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" },
        { "id": 3, "key": "2345678901234501" },
        { "id": 4, "key": "3456789012345012" },
        { "id": 5, "key": "4567890123450123" },
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  ASSERT_DECRYPT_SUCC(secret_to_protect, encrypted, key_id, EXPECT_KEY_ID(4), iv, "differentiator");
}

TEST_F(TestRGWSecretEncrypt, missing_key)
{
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  uint32_t key_id;
  string iv;
  ceph::bufferlist encrypted;

  std::tie(key_id, iv, encrypted) = rgw::secret::encrypter()->encrypt(secret_to_protect, "differentiator");
  ASSERT_EQ(key_id, 1);
  ASSERT_NE(secret_to_protect, encrypted);

  // Make key 1 disappear
  update_key_file(R"(
      [ 
        { "id": 2, "key": "1234567890123450" },
        { "id": 3, "key": "2345678901234501" }
      ]
  )");
  rgw::secret::init_encrypter(cct, ENABLED, key_file, RELOAD_INTERVAL);

  ASSERT_DECRYPT_FAIL(secret_to_protect, encrypted, key_id, EXPECT_KEY_ID(0), iv, "differentiator", WITH_ERROR);

  // Bring key 1 back
  update_key_file(R"(
      [ 
        { "id": 1, "key": "0123456789012345" },
        { "id": 2, "key": "1234567890123450" },
        { "id": 3, "key": "2345678901234501" }
      ]
  )");

  // Here is when 0 reload_interval helps
  ASSERT_DECRYPT_SUCC(secret_to_protect, encrypted, key_id, EXPECT_KEY_ID(2), iv, "differentiator");
}

int main(int argc, char **argv) {
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
