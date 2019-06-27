// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"
#include "ds/files.h"
#include "ds/logger.h"
#include "enclave/appinterface.h"
#include "kv/replicator.h"
#include "node/entities.h"
#include "node/networkstate.h"
#include "node/rpc/jsonrpc.h"
#include "node/rpc/memberfrontend.h"
#include "node/rpc/userfrontend.h"
#include "node_stub.h"

#include <iostream>
#include <string>

extern "C"
{
#include <evercrypt/EverCrypt_AutoConfig2.h>
}

using namespace ccfapp;
using namespace ccf;
using namespace std;

class TestUserFrontend : public ccf::UserRpcFrontend
{
public:
  TestUserFrontend(Store& tables) : UserRpcFrontend(tables)
  {
    auto empty_function = [this](RequestArgs& args) {
      return jsonrpc::success(true);
    };
    install("empty_function", empty_function, Read);
  }
};

class TestReqNotStoredFrontend : public ccf::UserRpcFrontend
{
public:
  TestReqNotStoredFrontend(Store& tables) : UserRpcFrontend(tables)
  {
    auto empty_function = [this](RequestArgs& args) {
      return jsonrpc::success(true);
    };
    install("empty_function", empty_function, Read);
    disable_request_storing();
  }
};

class TestMinimalHandleFunction : public ccf::UserRpcFrontend
{
public:
  TestMinimalHandleFunction(Store& tables) : UserRpcFrontend(tables)
  {
    auto echo_function = [this](Store::Tx& tx, const nlohmann::json& params) {
      return jsonrpc::success(params);
    };
    install("echo_function", echo_function, Read);
  }
};

class TestMemberFrontend : public ccf::MemberCallRpcFrontend
{
public:
  TestMemberFrontend(
    Store& tables, ccf::NetworkState& network, ccf::StubNodeState& node) :
    MemberCallRpcFrontend(network, node)
  {
    auto empty_function = [this](RequestArgs& args) {
      return jsonrpc::success(true);
    };
    install("empty_function", empty_function, Read);
  }
};

class TestNoCertsFrontend : public ccf::RpcFrontend
{
public:
  TestNoCertsFrontend(Store& tables) : RpcFrontend(tables)
  {
    auto empty_function = [this](RequestArgs& args) {
      return jsonrpc::success(true);
    };
    install("empty_function", empty_function, Read);
  }
};

class TestForwardingFrontEnd : public ccf::RpcFrontend
{
public:
  TestForwardingFrontEnd(Store& tables) : RpcFrontend(tables)
  {
    auto empty_function = [this](RequestArgs& args) {
      return jsonrpc::success(true);
    };
    // Note that this a Write function so that a follower executing this command
    // will forward it to the leader
    install("empty_function", empty_function, Write);
  }
};

class TestNoForwardingFrontEnd : public ccf::RpcFrontend
{
public:
  TestNoForwardingFrontEnd(Store& tables) : RpcFrontend(tables)
  {
    auto empty_function = [this](RequestArgs& args) {
      return jsonrpc::success(true);
    };
    // Note that this a Write function that cannot be forwarded
    install("empty_function", empty_function, Write, Forwardable::DoNotForward);
  }
};

// used throughout
tls::KeyPair kp;
ccf::NetworkState network;
ccf::NetworkState network2;
ccf::StubNodeState node;

std::vector<uint8_t> sign_json(nlohmann::json j)
{
  auto contents = nlohmann::json::to_msgpack(j);
  return kp.sign(contents);
}

auto create_simple_json()
{
  nlohmann::json j;
  j[jsonrpc::JSON_RPC] = jsonrpc::RPC_VERSION;
  j[jsonrpc::ID] = 1;
  j[jsonrpc::METHOD] = "empty_function";
  j[jsonrpc::PARAMS] = nlohmann::json::object();
  return j;
}

auto create_signed_json()
{
  auto j = create_simple_json();
  nlohmann::json sj;
  sj["req"] = j;
  auto sig = sign_json(j);
  sj["sig"] = sig;
  return sj;
}

// caller used throughout
auto ca = kp.self_sign("CN=name");
tls::Verifier verifier(ca);
auto user_caller = verifier.raw_cert_data();

auto ca_mem = kp.self_sign("CN=name_member");
tls::Verifier verifier_mem(ca_mem);
auto member_caller = verifier_mem.raw_cert_data();

auto ca_nos = kp.self_sign("CN=nostore_user");
tls::Verifier verifier_nos(ca_nos);
auto nos_caller = verifier_nos.raw_cert_data();

tls::KeyPair kp_other;
auto ca_inv = kp_other.self_sign("CN=name");
tls::Verifier verifier_inv(ca_inv);
auto invalid_caller = verifier_inv.raw_cert_data();

enclave::RPCContext rpc_ctx(0, user_caller);
enclave::RPCContext invalid_rpc_ctx(0, invalid_caller);
enclave::RPCContext member_rpc_ctx(0, member_caller);

void prepare_callers()
{
  Store::Tx txs;

  ccf::Certs* user_certs = &network.user_certs;
  ccf::Certs* member_certs = &network.member_certs;
  auto user_certs_view = txs.get_view(*user_certs);
  user_certs_view->put(user_caller, 0);
  user_certs_view->put(invalid_caller, 1);
  user_certs_view->put(nos_caller, 2);
  auto member_certs_view = txs.get_view(*member_certs);
  member_certs_view->put(member_caller, 0);
  member_certs_view->put(invalid_caller, 0);
  REQUIRE(txs.commit() == kv::CommitSuccess::OK);
}

TEST_CASE("SignedReq to and from json")
{
  ccf::SignedReq sr;
  REQUIRE(sr.sig.empty());
  REQUIRE(sr.req.empty());

  nlohmann::json j = sr;

  sr = j;
  REQUIRE(sr.sig.empty());
  REQUIRE(sr.req.empty());
}

TEST_CASE("Verify signature on Member Frontend")
{
  prepare_callers();
  TestMemberFrontend frontend(*network.tables, network, node);
  CallerId caller_id(0);
  CallerId inval_caller_id(1);
  Store::Tx txs;

  SUBCASE("with signature")
  {
    auto signed_call = create_signed_json();
    CHECK(frontend.verify_client_signature(
      txs, member_caller, caller_id, signed_call, false));
  }

  SUBCASE("signature not verified")
  {
    auto signed_call = create_signed_json();
    CHECK(!frontend.verify_client_signature(
      txs, invalid_caller, inval_caller_id, signed_call, false));
  }
}

TEST_CASE("Verify signature")
{
  prepare_callers();
  TestUserFrontend frontend(*network.tables);
  CallerId caller_id(0);
  CallerId inval_caller_id(1);
  Store::Tx txs;

  SUBCASE("with signature")
  {
    auto signed_call = create_signed_json();
    CHECK(frontend.verify_client_signature(
      txs, user_caller, caller_id, signed_call, false));
  }

  SUBCASE("signature not verified")
  {
    auto signed_call = create_signed_json();
    CHECK(!frontend.verify_client_signature(
      txs, invalid_caller, inval_caller_id, signed_call, false));
  }
}

TEST_CASE("get_signed_req")
{
  prepare_callers();
  TestUserFrontend frontend(*network.tables);
  auto simple_call = create_simple_json();
  CallerId caller_id(0);
  CallerId inval_caller_id(1);
  CallerId nos_caller_id(2);
  Store::Tx txs;

  SUBCASE("request with no signature")
  {
    frontend.process_json(rpc_ctx, txs, caller_id, simple_call);
    auto signed_resp = frontend.get_signed_req(caller_id);
    CHECK(!signed_resp.has_value());
  }
  SUBCASE("request with signature")
  {
    auto signed_call = create_signed_json();
    frontend.process_json(rpc_ctx, txs, caller_id, signed_call);
    auto signed_resp = frontend.get_signed_req(caller_id);
    CHECK(signed_resp.has_value());
    auto value = signed_resp.value();
    ccf::SignedReq signed_req(signed_call);
    CHECK(value.req == signed_req.req);
    CHECK(value.sig == signed_req.sig);
  }
  SUBCASE("request with signature but do not store")
  {
    TestReqNotStoredFrontend frontend_nostore(*network.tables);
    auto signed_call = create_signed_json();
    frontend_nostore.process_json(rpc_ctx, txs, nos_caller_id, signed_call);
    auto signed_resp = frontend_nostore.get_signed_req(nos_caller_id);
    CHECK(signed_resp.has_value());
    auto value = signed_resp.value();
    CHECK(value.req.empty());
    CHECK(value.sig == signed_call[jsonrpc::SIG]);
  }
  SUBCASE("signature not verified")
  {
    auto signed_call = create_signed_json();
    frontend.process_json(rpc_ctx, txs, caller_id, signed_call);
    auto signed_resp = frontend.get_signed_req(inval_caller_id);
    CHECK(!signed_resp.has_value());
  }
}

TEST_CASE("MinimalHandleFuction")
{
  prepare_callers();
  TestMinimalHandleFunction frontend(*network.tables);
  auto echo_call = create_simple_json();
  echo_call[jsonrpc::METHOD] = "echo_function";
  echo_call[jsonrpc::PARAMS] = {{"data", {"nested", "Some string"}},
                                {"other", "Another string"}};
  CallerId caller_id(0);
  Store::Tx txs;

  auto response =
    frontend.process_json(rpc_ctx, txs, caller_id, echo_call).value();
  CHECK(response[jsonrpc::RESULT] == echo_call[jsonrpc::PARAMS]);
}

TEST_CASE("process_json")
{
  prepare_callers();
  TestUserFrontend frontend(*network.tables);
  auto simple_call = create_simple_json();
  CallerId caller_id(0);
  CallerId inval_caller_id(1);

  Store::Tx txs;

  SUBCASE("with out")
  {
    auto response =
      frontend.process_json(rpc_ctx, txs, caller_id, simple_call).value();
    CHECK(response[jsonrpc::RESULT] == true);
  }
  SUBCASE("with signature")
  {
    auto signed_call = create_signed_json();
    auto response =
      frontend.process_json(rpc_ctx, txs, caller_id, signed_call).value();
    CHECK(response[jsonrpc::RESULT] == true);
  }
  SUBCASE("signature not verified")
  {
    auto signed_call = create_signed_json();
    auto response =
      frontend.process_json(invalid_rpc_ctx, txs, inval_caller_id, signed_call)
        .value();
    CHECK(
      response[jsonrpc::ERR][jsonrpc::CODE] ==
      static_cast<int16_t>(jsonrpc::ErrorCodes::INVALID_CLIENT_SIGNATURE));
  }
}

TEST_CASE("process")
{
  prepare_callers();
  TestUserFrontend frontend(*network.tables);
  auto simple_call = create_simple_json();

  SUBCASE("without signature")
  {
    std::vector<uint8_t> serialized_call =
      jsonrpc::pack(simple_call, jsonrpc::Pack::MsgPack);
    std::vector<uint8_t> serialized_response =
      frontend.process(rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(response[jsonrpc::RESULT] == true);
  }
  SUBCASE("with signature")
  {
    auto signed_call = create_signed_json();

    std::vector<uint8_t> serialized_call =
      jsonrpc::pack(signed_call, jsonrpc::Pack::MsgPack);
    std::vector<uint8_t> serialized_response =
      frontend.process(rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(response[jsonrpc::RESULT] == true);
  }
  SUBCASE("signature not verified")
  {
    auto signed_call = create_signed_json();

    std::vector<uint8_t> serialized_call =
      jsonrpc::pack(signed_call, jsonrpc::Pack::MsgPack);
    std::vector<uint8_t> serialized_response =
      frontend.process(invalid_rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(
      response[jsonrpc::ERR][jsonrpc::CODE] ==
      static_cast<int16_t>(jsonrpc::ErrorCodes::INVALID_CLIENT_SIGNATURE));
  }
}

// callers

TEST_CASE("User caller")
{
  prepare_callers();
  auto simple_call = create_simple_json();
  std::vector<uint8_t> serialized_call =
    jsonrpc::pack(simple_call, jsonrpc::Pack::MsgPack);
  TestUserFrontend frontend(*network.tables);

  SUBCASE("valid caller")
  {
    std::vector<uint8_t> serialized_response =
      frontend.process(rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(response[jsonrpc::RESULT] == true);
  }
  SUBCASE("invalid caller")
  {
    std::vector<uint8_t> serialized_response =
      frontend.process(member_rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(
      response[jsonrpc::ERR][jsonrpc::CODE] ==
      static_cast<int16_t>(jsonrpc::ErrorCodes::INVALID_CALLER_ID));
  }
}

TEST_CASE("Member caller")
{
  prepare_callers();
  auto simple_call = create_simple_json();
  std::vector<uint8_t> serialized_call =
    jsonrpc::pack(simple_call, jsonrpc::Pack::MsgPack);
  TestMemberFrontend frontend(*network.tables, network, node);

  SUBCASE("valid caller")
  {
    std::vector<uint8_t> serialized_response =
      frontend.process(member_rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(response[jsonrpc::RESULT] == true);
  }
  SUBCASE("invalid caller")
  {
    std::vector<uint8_t> serialized_response =
      frontend.process(rpc_ctx, serialized_call);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(
      response[jsonrpc::ERR][jsonrpc::CODE] ==
      static_cast<int16_t>(jsonrpc::ErrorCodes::INVALID_CALLER_ID));
  }
}

TEST_CASE("No certs table")
{
  prepare_callers();

  auto simple_call = create_simple_json();
  std::vector<uint8_t> serialized_call =
    jsonrpc::pack(simple_call, jsonrpc::Pack::MsgPack);
  TestNoCertsFrontend frontend(*network.tables);

  std::vector<uint8_t> serialized_response =
    frontend.process(rpc_ctx, serialized_call);
  auto response = jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
  CHECK(response[jsonrpc::RESULT] == true);
}

// We need an explicit main to initialize kremlib and EverCrypt
int main(int argc, char** argv)
{
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  ::EverCrypt_AutoConfig2_init();
  int res = context.run();
  if (context.shouldExit())
    return res;
  return res;
}

class StubForwarder : public AbstractForwarder
{
public:
  std::vector<std::vector<uint8_t>> forwarded_cmds;

  StubForwarder() {}

  bool forward_command(
    enclave::RPCContext& ctx,
    NodeId from,
    NodeId to,
    CallerId caller_id,
    const std::vector<uint8_t>& data) override
  {
    forwarded_cmds.push_back(data);
    return true;
  }

  void clear()
  {
    forwarded_cmds.clear();
  }
};

TEST_CASE("Forwarding")
{
  prepare_callers();

  TestForwardingFrontEnd frontend_follower(*network.tables);
  TestForwardingFrontEnd frontend_leader(*network2.tables);

  auto follower_forwarder = std::make_shared<StubForwarder>();
  auto follower_replicator = std::make_shared<kv::FollowerStubReplicator>();
  network.tables->set_replicator(follower_replicator);

  auto leader_replicator = std::make_shared<kv::LeaderStubReplicator>();
  network2.tables->set_replicator(leader_replicator);

  auto write_req = create_simple_json();
  std::vector<uint8_t> serialized_call =
    jsonrpc::pack(write_req, jsonrpc::Pack::MsgPack);

  INFO("Frontend without forwarder does not forward");
  {
    enclave::RPCContext ctx(0, nullb);
    REQUIRE(ctx.is_pending == false);
    REQUIRE(follower_forwarder->forwarded_cmds.empty());
    auto serialized_response = frontend_follower.process(ctx, serialized_call);
    REQUIRE(ctx.is_pending == false);
    REQUIRE(follower_forwarder->forwarded_cmds.size() == 0);

    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(
      response[jsonrpc::ERR][jsonrpc::CODE] ==
      static_cast<int16_t>(jsonrpc::ErrorCodes::TX_NOT_LEADER));
  }

  frontend_follower.set_cmd_forwarder(follower_forwarder);

  INFO("Write command on follower is forwarded to leader");
  {
    enclave::RPCContext ctx(0, nullb);
    REQUIRE(ctx.is_pending == false);
    REQUIRE(follower_forwarder->forwarded_cmds.empty());
    frontend_follower.process(ctx, serialized_call);
    REQUIRE(ctx.is_pending == true);
    REQUIRE(follower_forwarder->forwarded_cmds.size() == 1);

    auto forwarded_cmd = follower_forwarder->forwarded_cmds.back();
    follower_forwarder->forwarded_cmds.pop_back();
    enclave::RPCContext fwd_ctx(0, 0, 0);
    auto serialized_response =
      frontend_leader.process_forwarded(fwd_ctx, forwarded_cmd);

    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);
    CHECK(response[jsonrpc::RESULT] == true);
  }

  INFO("Forwarding write command to a follower return TX_NOT_LEADER");
  {
    enclave::RPCContext ctx(0, nullb);
    REQUIRE(ctx.is_pending == false);
    REQUIRE(follower_forwarder->forwarded_cmds.empty());
    frontend_follower.process(ctx, serialized_call);
    REQUIRE(ctx.is_pending == true);
    REQUIRE(follower_forwarder->forwarded_cmds.size() == 1);

    auto forwarded_cmd = follower_forwarder->forwarded_cmds.back();
    follower_forwarder->forwarded_cmds.pop_back();
    enclave::RPCContext fwd_ctx(0, 0, 0);

    // Processing forwarded response by a follower frontend (here, the same
    // frontend that the command was originally issued to)
    auto serialized_response =
      frontend_follower.process_forwarded(fwd_ctx, forwarded_cmd);
    auto response =
      jsonrpc::unpack(serialized_response, jsonrpc::Pack::MsgPack);

    CHECK(
      response[jsonrpc::ERR][jsonrpc::CODE] ==
      static_cast<int16_t>(jsonrpc::ErrorCodes::TX_NOT_LEADER));
  }

  INFO("Write command should not be forwarded if marked as non-forwardable");
  {
    TestNoForwardingFrontEnd frontend_follower_no_forwarding(*network.tables);

    auto follower2_forwarder = std::make_shared<StubForwarder>();
    auto follower2_replicator = std::make_shared<kv::FollowerStubReplicator>();
    network.tables->set_replicator(follower_replicator);
    frontend_follower_no_forwarding.set_cmd_forwarder(follower2_forwarder);

    enclave::RPCContext ctx(0, nullb);
    REQUIRE(ctx.is_pending == false);
    REQUIRE(follower2_forwarder->forwarded_cmds.empty());
    frontend_follower_no_forwarding.process(ctx, serialized_call);
    REQUIRE(ctx.is_pending == false);
    REQUIRE(follower2_forwarder->forwarded_cmds.size() == 0);
  }
}
