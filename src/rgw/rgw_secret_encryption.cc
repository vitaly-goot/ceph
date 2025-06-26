#include <memory>
#include <map>
#include <vector>

#include "common/errno.h"
#include "common/dout.h"
#include "boost/container/flat_map.hpp"
#include "common/ceph_json.h"
#include "common/ceph_time.h"
#include "rgw_common.h"
#include "rgw_secret_encryption.h"
#include "auth/Crypto.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

namespace rgw { namespace secret {

struct RGWEncryptKey
{
  uint32_t id;
  std::string key;

  void decode_json(JSONObj *obj)
  {
    JSONDecoder::decode_json("id", id, obj);
    JSONDecoder::decode_json("key", key, obj);
  }
};

using RGWEncryptKeyMap = std::map<uint32_t, RGWEncryptKey>;

class RGWSecretEncrypterImpl : public RGWSecretEncrypter
{
public:
  RGWSecretEncrypterImpl(CephContext *const cct, bool enabled, const std::string &encrypt_key_file, uint64_t reload_interval) :
    cct(cct),
    enabled(enabled),
    encrypt_key_file(encrypt_key_file),
    curr_db(std::make_shared<RGWEncryptKeyMap>()),
    reload_interval(reload_interval)
  {
    ldout(cct, 1)  << "Create secret encrypter with enablement " << enabled << dendl;
    auto ret = reload_keys(0);
    if (enabled && ret < 0) {
      ldout(cct, 1)  << "Failure of key file reload when the feature is enabled is intolerable" << dendl;
      exit(1);
    }
  }

  std::tuple<uint32_t, std::string, bufferlist> encrypt(const bufferlist &secret, const std::string &differentiator) override;

  std::tuple<bool, uint32_t, bufferlist> decrypt(uint32_t key_id, const bufferlist &secret, bufferptr& iv, const std::string &differentiator) override;

protected:
  CephContext *const cct;

  bool enabled = false;

  const std::string encrypt_key_file;

  std::shared_ptr<RGWEncryptKeyMap> curr_db;

  uint64_t reload_interval;

  time_t last_reload_for_unknown_key = 0;

  CryptoKeyHandler *get_key_handler(const bufferptr &secret)
  {
    auto* cryptohandler = cct->get_crypto_handler(CEPH_CRYPTO_AES);

    if (cryptohandler->validate_secret(secret) < 0) {
      ldout(cct, 1) << "ERROR: Invalid rgw secret encryption key, please ensure its length is 16" << dendl;
      return nullptr;
    }

    std::string error;
    std::unique_ptr<CryptoKeyHandler> keyhandler(cryptohandler->get_key_handler(secret, error));
    if (!keyhandler) {
      ldout(cct, 1) << "ERROR: failed to get AES key handler: " << error << dendl;
    }
    return keyhandler.release();
  }

  int reload_keys(uint32_t expect_key_id);

  // Make an encryption key that is unique to the differentiator and the master encryption key.
  CryptoKey make_unique_key(const RGWEncryptKey& encrypt_key, const std::string& differentiator);
};

// It is made shared_ptr and re-initializable to help unit tests.
static std::unique_ptr<RGWSecretEncrypterImpl> TheSecretEncrypter;

int init_encrypter(CephContext *const cct, bool enable, const std::string &encrypt_key_file, uint64_t reload_interval)
{
  TheSecretEncrypter = std::make_unique<RGWSecretEncrypterImpl>(cct, enable, encrypt_key_file, reload_interval);
  return 0;
}

RGWSecretEncrypter *encrypter()
{
  return TheSecretEncrypter.get();
}

// Stolen from rgw_admin.cc
static int read_input(CephContext *const cct, const std::string& infile, bufferlist& bl)
{
  int fd = 0;
  if (infile.size()) {
    fd = open(infile.c_str(), O_RDONLY);
    if (fd < 0) {
      int err = -errno;
      ldout(cct, 1)  << "error reading input file " << infile << " " << err << dendl;
      return err;
    }
  }

#define READ_CHUNK 8192
  int r;
  int err = 0;

  do {
    char buf[READ_CHUNK];

    r = safe_read(fd, buf, READ_CHUNK);
    if (r < 0) {
      err = -errno;
      ldout(cct, 1) << "error while reading input: " << err << dendl;
      goto out;
    }
    bl.append(buf, r);
  } while (r > 0);
  err = 0;

 out:
  if (infile.size()) {
    close(fd);
  }
  return err;
}

template <class T>
static int read_decode_json(CephContext *const cct, const std::string& infile, T& t)
{
  bufferlist bl;
  int ret = read_input(cct, infile, bl);
  if (ret < 0) {
    ldout(cct, 1) << "ERROR: failed to read input: " << cpp_strerror(-ret) << dendl;
    return ret;
  }
  JSONParser p;
  if (!p.parse(bl.c_str(), bl.length())) {
    ldout(cct, 1) << "failed to parse JSON" << dendl;
    return -EINVAL;
  }

  try {
    decode_json_obj(t, &p);
  } catch (const JSONDecoder::err& e) {
    ldout(cct, 1) << "failed to decode JSON input: " << e.what() << dendl;
    return -EINVAL;
  }
  return 0;
}

static RGWEncryptKey get_key_to_use(const RGWEncryptKeyMap &db_to_use)
{
  if (db_to_use.empty() || (db_to_use.size() == 1)) {
    // We expect at least 2 keys available before actually encrypting.
    return RGWEncryptKey{0, ""};
  } else {
    return std::next(db_to_use.rbegin())->second;
  }
}

int RGWSecretEncrypterImpl::reload_keys(uint32_t expect_key_id)
{
  ldout(cct, 1) << "Reload keys from " << encrypt_key_file << dendl;
  std::list<RGWEncryptKey> key_list;
  int r = read_decode_json(cct, encrypt_key_file, key_list);
  if (r < 0) {
    ldout(cct, 1) << "WARNING: failed to load secret encrypt keys" << dendl;
    return r;
  }

  std::shared_ptr<RGWEncryptKeyMap> new_db = std::make_shared<RGWEncryptKeyMap>();
  auto now = ceph_clock_now();
  for (const auto& key : key_list) {
    if (key.id == 0) {
      ldout(cct, 1) << "ERROR: key with id 0 is invalid and it is ignored" << dendl;
      continue;
    }
    bufferptr key_to_check(key.key.data(), key.key.length());
    CryptoKey ck{CEPH_CRYPTO_AES, now, key_to_check};
    if (ck.empty()) {
      ldout(cct, 1) << "ERROR: key with id " << key.id << " is invalid, likely shorter than 16 bytes" << dendl;
    } else {
      (*new_db)[key.id] = key;
    }
  }
  if (new_db->empty() || 
     (!new_db->empty() && new_db->rbegin()->first >= expect_key_id)) {
    curr_db.swap(new_db);
    if (!curr_db->empty()) {
      ldout(cct, 1) << "Reloaded keys with the latest key be " << curr_db->rbegin()->first << " and expected key be " << expect_key_id << dendl;
    }
    return 0;
  } else {
    ldout(cct, 1) << "WARNING: key reloading doesn't cover key id " << expect_key_id << " with " << (new_db->empty() ? 0 : new_db->rbegin()->first) << dendl;
    return -EIO;
  }
}

CryptoKey RGWSecretEncrypterImpl::make_unique_key(const RGWEncryptKey &encrypt_key, const std::string &differentiator)
{
  auto now = ceph_clock_now();
  bufferptr key(encrypt_key.key.data(), encrypt_key.key.length());
  CryptoKey ck{CEPH_CRYPTO_AES, now, key};

  using ceph::encode;
  bufferlist bl;
  encode(differentiator, bl);

  auto hash = ck.hmac_sha256(cct, bl);

  bufferptr unique_key((const char*)hash.v, hash.SIZE);
  return CryptoKey{CEPH_CRYPTO_AES, now, unique_key};
}

std::tuple<bool, uint32_t, bufferlist> RGWSecretEncrypterImpl::decrypt(uint32_t key_id, const bufferlist &secret, bufferptr&iv, const std::string &differentiator)
{
  auto db_in_use = curr_db; // Hold a reference of it

  auto suggested_key = enabled ? get_key_to_use(*db_in_use.get()) : RGWEncryptKey{0, ""};

  if (key_id == 0) {
    // Not encrypted. Toss it back.
    return std::make_tuple(true, suggested_key.id, std::move(secret));
  }

  auto key_found = db_in_use->find(key_id);
  auto now = ceph_clock_now().sec();
  // Condition to reload key file:
  // 1. Secret is encrypted by an unknown key.
  // 2. Either this key is newer than all knowns keys or it has been at least 5 seconds since the last reload
  if (key_found == db_in_use->end() &&
      (now >= (last_reload_for_unknown_key + (time_t)reload_interval) ||
      (!db_in_use->empty() && db_in_use->rbegin()->first < key_id))) {
    last_reload_for_unknown_key = now;
    if (reload_keys(key_id) < 0) {
      ldout(cct, 1) << "ERROR: Unknown encrypt key ID [" << key_id << "] after failed key reload" << dendl;
      return std::make_tuple(false, 0, std::move(secret));
    }
    db_in_use = curr_db; // Pick up the newly reloaded key DB.
    key_found = db_in_use->find(key_id);
  }

  if (key_found == db_in_use->end()) {
    ldout(cct, 1) << "ERROR: Unknown encrypt key ID [" << key_id << "] provided for decryption" << dendl;
    return std::make_tuple(false, 0, std::move(secret));
  }

  auto unique_key = make_unique_key(key_found->second, differentiator);

  std::unique_ptr<CryptoKeyHandler> keyhandler(get_key_handler(unique_key.get_secret()));
  if (!keyhandler) {
    ldout(cct, 1) << "ERROR: No Key handler found" << dendl;
    return std::make_tuple(false, 0, std::move(secret));
  }

  bufferlist out;
  std::string error;
  int ret = keyhandler->decrypt(secret, out, iv, &error);
  if (ret < 0) {
    ldout(cct, 1) << "ERROR: fail to decrypt secret: " << ret << " error " << error << dendl;
    return std::make_tuple(false, 0, std::move(secret));
  }
  return std::make_tuple(true, suggested_key.id, out);
}

// It is hidden in auth/Crypto.cc. It is for AES-128.
static constexpr const std::size_t AES_BLOCK_LEN{16};

std::tuple<uint32_t, std::string, bufferlist> RGWSecretEncrypterImpl::encrypt(const bufferlist &secret, const std::string &differentiator)
{
  if (!enabled) {
    return std::make_tuple(0, "", std::move(secret));
  }

  auto db_in_use = curr_db;
  auto suggested_key = get_key_to_use(*db_in_use);
  // Key id 0 also means no encryption.
  if (suggested_key.id == 0) {
    return std::make_tuple(0, "", std::move(secret));
  }

  auto unique_key = make_unique_key(suggested_key, differentiator);

  std::unique_ptr<CryptoKeyHandler> keyhandler(get_key_handler(unique_key.get_secret()));
  if (! keyhandler) {
    ldout(cct, 1) << "ERROR: No Key handler" << dendl;
    return std::make_tuple(0, "", std::move(secret));
  }

  char iv[AES_BLOCK_LEN];
  cct->random()->get_bytes(iv, AES_BLOCK_LEN);
  std::string iv_str(iv, AES_BLOCK_LEN); // AES API modified IV input. So we may a copy

  bufferlist out;
  std::string error;
  bufferptr iv_buf(iv, AES_BLOCK_LEN);
  int ret = keyhandler->encrypt(secret, out, iv_buf, &error);
  if (ret < 0) {
    ldout(cct, 1) << "ERROR: fail to encrypt secret: " << error << dendl;
    return std::make_tuple(0, "", std::move(secret));
  }

  return std::make_tuple(suggested_key.id, std::move(iv_str), out);
}

} // namespace secret
} // namespace rgw
