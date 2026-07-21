// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <cerrno>
#include <memory>
#include <utility>
#include <vector>

#include "auth/KeyRing.h"
#include "auth/cephx/CephxKeyServer.h"
#include "auth/cephx/CephxProtocol.h"
#include "auth/cephx/CephxServiceHandler.h"
#include "common/Clock.h"
#include "common/ceph_context.h"
#include "global/global_context.h"
#include "gtest/gtest.h"

using ceph::bufferlist;
using ceph::decode;
using ceph::encode;

namespace {

class TestKeyServer final : public KeyServer {
public:
  TestKeyServer(CephContext* cct, KeyRing* extra_secrets)
    : KeyServer(cct, extra_secrets)
  {}

  void set_service_cipher(int cipher)
  {
    service_cipher = cipher;
  }

  void set_allowed_ciphers(std::vector<int> ciphers)
  {
    allowed_ciphers = std::move(ciphers);
  }

  int get_service_cipher() const override
  {
    return service_cipher;
  }

  bool is_cipher_allowed(int cipher) const override
  {
    return std::find(allowed_ciphers.begin(), allowed_ciphers.end(), cipher) !=
           allowed_ciphers.end();
  }

  std::vector<int> get_ciphers_allowed() const override
  {
    return allowed_ciphers;
  }

private:
  int service_cipher = CEPH_CRYPTO_AES;
  std::vector<int> allowed_ciphers = {
    CEPH_CRYPTO_AES,
    CEPH_CRYPTO_AES256KRB5,
  };
};

void update_rotating_secrets(TestKeyServer& key_server, bool wipe)
{
  bufferlist rotating_bl;
  ASSERT_TRUE(key_server.prepare_rotating_update(rotating_bl, wipe));

  KeyServerData::Incremental inc;
  inc.op = KeyServerData::AUTH_INC_SET_ROTATING;
  inc.rotating_bl = std::move(rotating_bl);
  key_server.apply_data_incremental(inc);
}

TEST(CephxKeyRevocation, OldAuthTicketCannotMintServiceTicketAfterWipe)
{
  auto* cct = g_ceph_context;
  KeyRing extra_secrets;
  TestKeyServer key_server(cct, &extra_secrets);

  EntityName principal;
  ASSERT_TRUE(principal.from_str("client.revocation-test"));

  EntityAuth principal_auth;
  ASSERT_EQ(0, principal_auth.key.create(cct, CEPH_CRYPTO_AES));
  encode("allow *", principal_auth.caps["osd"]);
  key_server.add_auth(principal, principal_auth);

  // Create the pre-migration rotating secrets and an AUTH ticket protected by
  // the legacy AUTH service secret.
  key_server.set_service_cipher(CEPH_CRYPTO_AES);
  update_rotating_secrets(key_server, false);

  constexpr uint64_t global_id = 0x1234;
  CephXSessionAuthInfo auth_info;
  auth_info.service_id = CEPH_ENTITY_TYPE_AUTH;

  double auth_ttl = 0;
  ASSERT_TRUE(key_server.get_service_secret(
    CEPH_ENTITY_TYPE_AUTH,
    auth_info.service_secret,
    auth_info.secret_id,
    auth_ttl));
  ASSERT_EQ(CEPH_CRYPTO_AES, auth_info.service_secret.get_type());

  auth_info.ticket.name = principal;
  auth_info.ticket.global_id = global_id;
  auth_info.ticket.init_timestamps(ceph_clock_now(), auth_ttl);
  ASSERT_TRUE(key_server.generate_secret(
    auth_info.session_key, CEPH_CRYPTO_AES));

  CephXTicketBlob old_auth_ticket;
  ASSERT_TRUE(cephx_build_service_ticket_blob(
    cct, auth_info, old_auth_ticket));

  CryptoKey old_osd_secret;
  uint64_t old_osd_secret_id = 0;
  double osd_ttl = 0;
  ASSERT_TRUE(key_server.get_service_secret(
    CEPH_ENTITY_TYPE_OSD,
    old_osd_secret,
    old_osd_secret_id,
    osd_ttl));

  // Model the final migration step: new service tickets use AES256KRB5 and
  // legacy AES is no longer accepted for principal authentication.
  key_server.set_service_cipher(CEPH_CRYPTO_AES256KRB5);
  key_server.set_allowed_ciphers({CEPH_CRYPTO_AES256KRB5});
  update_rotating_secrets(key_server, true);

  CryptoKey retained_auth_secret;
  ASSERT_TRUE(key_server.get_service_secret(
    CEPH_ENTITY_TYPE_AUTH,
    auth_info.secret_id,
    retained_auth_secret));
  EXPECT_EQ(auth_info.service_secret.encode_base64(),
            retained_auth_secret.encode_base64());

  CryptoKey new_osd_secret;
  uint64_t new_osd_secret_id = 0;
  ASSERT_TRUE(key_server.get_service_secret(
    CEPH_ENTITY_TYPE_OSD,
    new_osd_secret,
    new_osd_secret_id,
    osd_ttl));
  EXPECT_EQ(CEPH_CRYPTO_AES256KRB5, new_osd_secret.get_type());
  EXPECT_NE(old_osd_secret.encode_base64(), new_osd_secret.encode_base64());

  CephXTicketHandler old_auth_handler(cct, CEPH_ENTITY_TYPE_AUTH);
  old_auth_handler.session_key = auth_info.session_key;
  old_auth_handler.ticket = old_auth_ticket;
  std::unique_ptr<CephXAuthorizer> authorizer{
    old_auth_handler.build_authorizer(global_id)};
  ASSERT_NE(nullptr, authorizer);

  bufferlist request;
  CephXRequestHeader request_header;
  request_header.request_type = CEPHX_GET_PRINCIPAL_SESSION_KEY;
  encode(request_header, request);
  request.claim_append(authorizer->bl);

  CephXServiceTicketRequest ticket_request;
  ticket_request.keys = CEPH_ENTITY_TYPE_OSD;
  encode(ticket_request, request);

  CephxServiceHandler service_handler(cct, &key_server);
  bufferlist challenge;
  AuthCapsInfo connection_caps;
  ASSERT_EQ(0, service_handler.start_session(
    principal, global_id, false, &challenge, &connection_caps));

  bufferlist reply;
  auto request_iter = request.cbegin();
  const int result = service_handler.handle_request(
    request_iter,
    0,
    &reply,
    &connection_caps,
    nullptr,
    nullptr);

  // A hard migration cutoff must reject the retained pre-wipe AUTH
  // credential rather than use it to mint a ticket under the new OSD key.
  EXPECT_EQ(-EACCES, result);

  bool issued_new_osd_ticket = false;
  if (result == 0) {
    auto reply_iter = reply.cbegin();
    CephXResponseHeader response_header;
    ASSERT_NO_THROW(decode(response_header, reply_iter));
    ASSERT_EQ(CEPHX_GET_PRINCIPAL_SESSION_KEY,
              response_header.request_type);
    ASSERT_EQ(0, response_header.status);

    CephXTicketManager returned_tickets(cct);
    ASSERT_TRUE(returned_tickets.verify_service_ticket_reply(
      auth_info.session_key, reply_iter));
    issued_new_osd_ticket =
      returned_tickets.have_key(CEPH_ENTITY_TYPE_OSD);
  }
  EXPECT_FALSE(issued_new_osd_ticket)
    << "a retained pre-wipe AUTH credential minted a post-wipe OSD ticket";
}

} // anonymous namespace
