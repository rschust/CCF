// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#ifdef PBFT
#  include "pbft/pbft.h"
#endif

#include "calltypes.h"
#include "consensus.h"
#include "ds/logger.h"
#include "enclave/rpcclient.h"
#include "enclave/rpcsessions.h"
#include "encryptor.h"
#include "entities.h"
#include "history.h"
#include "kv/replicator.h"
#include "networkstate.h"
#include "nodetonode.h"
#include "rpc/consts.h"
#include "rpc/frontend.h"
#include "rpc/serialization.h"
#include "seal.h"
#include "tls/client.h"
#include "tls/entropy.h"

#include <atomic>
#include <chrono>
#include <fmt/format_header_only.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>
#include <vector>

extern "C"
{
#include <evercrypt/EverCrypt_AutoConfig2.h>
}

namespace ccf
{
  template <typename T>
  class StateMachine
  {
    std::atomic<T> s;

  public:
    StateMachine(T s) : s(s) {}
    void expect(T s) const
    {
      if (s != this->s.load())
        throw std::logic_error("Unexpected state");
    }

    bool check(T s) const
    {
      return s == this->s.load();
    }

    void advance(T s)
    {
      this->s.store(s);
    }
  };

  class NodeState : public ccf::AbstractNodeState
  {
  private:
    template <typename T>
    using Result = std::pair<T, bool>;

    template <typename T>
    static Result<T> Success(T&& v)
    {
      return {std::forward<T>(v), true};
    }

    template <typename T>
    static Result<T> Fail()
    {
      return {{}, false};
    }

    template <typename T>
    static Result<T> Fail(const char* s)
    {
      LOG_DEBUG_FMT(s);
      return {{}, false};
    }

    enum class State
    {
      uninitialized,
      initialized,
      started,
      partOfPublicNetwork,
      partOfNetwork,
      readingPublicLedger,
      readingPrivateLedger,
      awaitingRecoveryTx
    };

    //
    // this node's core state
    //
    StateMachine<State> sm;
    SpinLock lock;

    NodeId self;
    tls::KeyPair node_kp;
    std::vector<uint8_t> node_cert;
    CodeDigest node_code_id;

    //
    // kv store, replication, and I/O
    //
    ringbuffer::AbstractWriterFactory& writer_factory;
    std::unique_ptr<ringbuffer::AbstractWriter> to_host;
    raft::Config raft_config;

    NetworkState& network;
    std::shared_ptr<ConsensusRaft> raft;
#ifdef PBFT
    using ConsensusPbft = pbft::Pbft<NodeToNode>;
    std::shared_ptr<ConsensusPbft> pbft;
#endif
    std::shared_ptr<NodeToNode> n2n_channels;
    enclave::RPCSessions& rpcsessions;
    std::shared_ptr<kv::TxHistory> history;
    std::shared_ptr<kv::AbstractTxEncryptor> encryptor;

    //
    // join protocol
    //
    std::vector<uint8_t> raw_fresh_key;
    std::map<NodeId, std::vector<uint8_t>> joiners_fresh_keys;

    //
    // recovery
    //
    std::shared_ptr<Store> recovery_store;
    std::shared_ptr<kv::TxHistory> recovery_history;
    std::shared_ptr<kv::AbstractTxEncryptor> recovery_encryptor;
    kv::Version recovery_v;
    crypto::Sha256Hash recovery_root;
    std::vector<kv::Version> term_history;
    kv::Version last_recovered_commit_idx = 1;

    raft::Index ledger_idx = 0;

  public:
    NodeState(
      ringbuffer::AbstractWriterFactory& writer_factory,
      NetworkState& network,
      enclave::RPCSessions& rpcsessions) :
      sm(State::uninitialized),
      self(INVALID_ID),
      writer_factory(writer_factory),
      to_host(writer_factory.create_writer_to_outside()),
      network(network),
      rpcsessions(rpcsessions)
    {
      ::EverCrypt_AutoConfig2_init();
    }

    //
    // funcs in state "uninitialized"
    //
    void initialize(
      raft::Config& raft_config_, std::shared_ptr<NodeToNode> n2n_channels_)
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::uninitialized);

      raft_config = raft_config_;
      n2n_channels = n2n_channels_;
      sm.advance(State::initialized);
    }

    //
    // funcs in state "initialized"
    //
    auto create_new(const CreateNew::In& args)
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::initialized);

      // Generate node key pair
      std::stringstream name;
      name << "CN=" << Actors::MANAGEMENT;
      node_cert = node_kp.self_sign(name.str());

      // We present our self-signed certificate to the management frontend
      rpcsessions.add_cert(
        Actors::MANAGEMENT, nullb, node_cert, node_kp.private_key());

      // Generate node quote
      std::vector<uint8_t> quote(args.quote_max_size);

#ifdef GET_QUOTE
      crypto::Sha256Hash h{node_cert};
      size_t quote_len = args.quote_max_size;

      // TODO(#important,#TR): The "alpha" parameters, including the unique
      // service identifier, should also be included in the quote.
      oe_result_t res = oe_get_report(
        OE_REPORT_FLAGS_REMOTE_ATTESTATION,
        h.h,
        h.SIZE,
        nullptr,
        0,
        quote.data(),
        &quote_len);

      if (res != OE_OK)
      {
        LOG_FAIL_FMT("Failed to get quote: {}", oe_result_str(res));
        return Fail<CreateNew::Out>("oe_get_report failed");
      }
      quote.resize(quote_len);

      // Set own code version
      oe_report_t parsed_quote = {0};
      res = oe_parse_report(quote.data(), quote.size(), &parsed_quote);
      if (res != OE_OK)
      {
        LOG_FAIL_FMT("Failed to parse quote: {}", oe_result_str(res));
        return Fail<CreateNew::Out>("oe_parse_report failed");
      }

      std::copy(
        std::begin(parsed_quote.identity.unique_id),
        std::end(parsed_quote.identity.unique_id),
        std::begin(node_code_id));
#endif

      if (args.recover)
      {
        init_public_ledger_recovery();
        sm.advance(State::readingPublicLedger);
      }
      else
      {
        sm.advance(State::started);
      }
      return Success<CreateNew::Out>({node_cert, quote});
    }

    //
    // funcs in state "started"
    //
    auto start_network(Store::Tx& tx, const StartNetwork::In& args)
    {
      // TODO(#important,#TR): Service start-up protocol should be updated in
      // line with the paper (section IV-B) to provide more flexibility and
      // record in the ledger the state of the service (booting -> opening ->
      // open -> closed).
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::started);

      // Create fresh network secrets and seal
      network.secrets = std::make_unique<NetworkSecrets>(
        "CN=The CA", std::make_unique<Seal>(writer_factory));

      accept_all_connections();
      self = args.id;
      setup_raft();

      setup_store();

      // Before loading the initial transaction, its integrity needs to be
      // protected since deserialise only accepts protected entries.
      StoreSerialiser s(encryptor, 1);
      auto protected_tx0 = s.serialise_domains(args.tx0);

      // Load initial transaction (may affect CCF and app tables).
      if (
        network.tables->deserialise(protected_tx0) ==
        kv::DeserialiseSuccess::FAILED)
        return Fail<StartNetwork::Out>("Deserialisation of tx0 failed");

      // Become the leader and force replication.
      raft->force_become_leader();
      raft->replicate({{1, protected_tx0, true}});

      // Network signs tx0.
      tls::KeyPair keys(network.secrets->get_current().priv_key);
      auto tx0Sig = keys.sign(args.tx0);

      // Sets itself as trusted.
      auto nodes_view = tx.get_view(network.nodes);
      auto leader_info = nodes_view->get(self).value();
      leader_info.status = NodeStatus::TRUSTED;
      nodes_view->put(self, leader_info);

#ifdef GET_QUOTE
      if (!trust_own_code_id(tx, self))
        return Fail<StartNetwork::Out>("Parsing quote of starting node failed");
#endif

      sm.advance(State::partOfNetwork);

      return Success<StartNetwork::Out>(
        {std::string(
           network.secrets->get_current().cert.data(),
           network.secrets->get_current().cert.data() +
             network.secrets->get_current().cert.size()),
         tx0Sig});
    }

    void join_network(enclave::RPCContext& rpc_ctx, const JoinNetwork::In& args)
    {
      std::lock_guard<SpinLock> guard(lock);

      if (!sm.check(State::started) && !sm.check(State::awaitingRecoveryTx))
        throw std::logic_error("Unexpected state before joining network");

      // Peer certificate needs to be signed by network certificate
      auto tls_ca = std::make_shared<tls::CA>(args.network_cert);
      auto join_client_cert = std::make_unique<tls::Cert>(
        Actors::NODES, tls_ca, node_cert, node_kp.private_key(), nullb);

      // Create and connect to endpoint
      auto join_client =
        rpcsessions.create_client(rpc_ctx, std::move(join_client_cert));

      // Only reply to the client when the leader has responded
      rpc_ctx.is_pending = true;

      join_client->connect(
        args.hostname,
        args.service,
        [this, rpc_ctx](const std::vector<uint8_t>& data) {
          auto j = jsonrpc::unpack(data, jsonrpc::Pack::MsgPack);

          // Check that the response is valid.
          try
          {
            auto resp = jsonrpc::Response<JoinNetworkNodeToNode::Out>(j);
          }
          catch (const std::exception& e)
          {
            return std::make_pair(
              rpc_ctx.is_pending,
              jsonrpc::pack(
                jsonrpc::error_response(
                  rpc_ctx.req.seq_no,
                  jsonrpc::ErrorCodes::INTERNAL_ERROR,
                  "An error occured while joining the network"),
                rpc_ctx.pack.value()));
          }

          // Set network secrets, node id and become part of network.
          auto res = j.at(jsonrpc::RESULT).get<JoinNetworkNodeToNode::Out>();

          // If the current network secrets do not apply since the genesis, the
          // joining node can only join the public network
          bool public_only = (res.version != 0);

          // In a private network, seal secrets immediately. Note that in
          // recovery, temporary secrets are overwritten
          network.secrets = std::make_unique<NetworkSecrets>(
            res.version,
            res.network_secrets,
            std::make_unique<Seal>(writer_factory),
            !public_only);

          self = res.id;
          if (res.version != 0 && sm.check(State::awaitingRecoveryTx))
          {
            // If the joining node was started in recovery, truncate the ledger
            // and reset the store as we will receive the entirety of the ledger
            // from the leader
            LOG_INFO_FMT("Truncating entire ledger");
            log_truncate(0);
            network.tables->clear();
          }
          else
          {
            // If the node was started normally, accept all connections
            accept_all_connections();
          }
          setup_raft(public_only);
          setup_store();

          if (public_only)
            sm.advance(State::partOfPublicNetwork);
          else
            sm.advance(State::partOfNetwork);

          LOG_INFO_FMT(
            "Node has now joined the network as node {}: {}",
            self,
            (public_only ? "public only" : "all domains"));

          jsonrpc::Response<JoinNetwork::Out> join_rpc_resp;
          join_rpc_resp.id = rpc_ctx.req.seq_no;
          join_rpc_resp.result.id = self;

          return std::make_pair(
            rpc_ctx.is_pending,
            jsonrpc::pack(join_rpc_resp, rpc_ctx.pack.value()));
        });

      // Generate fresh key to encrypt/decrypt historical network secrets sent
      // by the leader via the kv store
      raw_fresh_key = tls::Entropy().random(crypto::GCM_SIZE_KEY);

      // Send RPC request to remote node to join the network.
      jsonrpc::ProcedureCall<JoinNetworkNodeToNode::In> join_rpc;
      join_rpc.id = 1;
      join_rpc.method = ccf::NodeProcs::JOIN;
      join_rpc.params.raw_fresh_key = raw_fresh_key;

      auto join_req =
        jsonrpc::pack(nlohmann::json(join_rpc), jsonrpc::Pack::MsgPack);

      join_client->send(join_req);
    }

    // TODO(#important,#TR): Once start-up protocol is updated, the recovery
    // protocol could be refactored to be in line with start-up protocol
    // (sections IV-B, IV-G).
    void init_public_ledger_recovery()
    {
      // Create temporary network secrets but do not seal yet
      network.secrets = std::make_unique<NetworkSecrets>(
        "CN=The CA", std::make_unique<Seal>(writer_factory), false);
      accept_member_connections();
      setup_store();
    }

    //
    // funcs in state "readingPublicLedger"
    //
    void start_ledger_recovery()
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::readingPublicLedger);
      LOG_INFO_FMT("Start public recovery");
      read_ledger_idx(++ledger_idx);
    }

    void recover_public_ledger_entry(std::vector<uint8_t> ledger_entry)
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::readingPublicLedger);

      LOG_INFO_FMT(
        "Deserialising public ledger entry ({})", ledger_entry.size());

      // When reading the public ledger, deserialise in the real store
      auto result = network.tables->deserialise(ledger_entry, true);
      if (result == kv::DeserialiseSuccess::FAILED)
      {
        LOG_FAIL_FMT("Failed to deserialise entry in public ledger");
        network.tables->rollback(ledger_idx - 1);
        recover_public_ledger_end_unsafe();
        return;
      }

      // If the ledger entry is a signature, it is safe to compact the store
      if (result == kv::DeserialiseSuccess::PASS_SIGNATURE)
      {
        network.tables->compact(ledger_idx);
        Store::Tx tx;
        auto sig_view = tx.get_view(network.signatures);
        auto sig = sig_view->get(0);
        if (sig.has_value())
        {
          auto sig_value = sig.value();
          LOG_DEBUG_FMT(
            "Read signature at {} for term {}", ledger_idx, sig_value.term);
          for (auto i = term_history.size(); i <= sig_value.term; ++i)
          {
            term_history.push_back(last_recovered_commit_idx + 1);
          }
          last_recovered_commit_idx = ledger_idx;
        }
        else
        {
          throw std::logic_error("Invalid signature");
        }
      }

      read_ledger_idx(++ledger_idx);
    }

    void recover_public_ledger_end_unsafe()
    {
      Store::Tx tx;
      sm.expect(State::readingPublicLedger);

      // When reaching the end of the public ledger, truncate to last signed
      // index and promote network secrets to this index
      auto ls_idx = last_signed_index(tx);
      network.tables->rollback(ls_idx);
      log_truncate(ls_idx);
      LOG_INFO_FMT("Truncating ledger to last signed index: {}", ls_idx);

      network.secrets->promote_secrets(0, ls_idx + 1);
      sm.advance(State::awaitingRecoveryTx);
    }

    kv::Version last_signed_index(Store::Tx& tx)
    {
      auto sig_tv = tx.get_view(network.signatures);
      auto sig = sig_tv->get(0);
      if (!sig.has_value())
      {
        return 0;
      }
      auto sig_value = sig.value();
      return sig_value.index;
    }

    //
    // funcs in state "readingPrivateLedger"
    //
    void recover_private_ledger_entry(std::vector<uint8_t> ledger_entry)
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::readingPrivateLedger);

      LOG_INFO_FMT(
        "Deserialising private ledger entry ({})", ledger_entry.size());

      // When reading the private ledger, deserialise in the recovery store
      auto result = recovery_store->deserialise(ledger_entry);
      if (result == kv::DeserialiseSuccess::FAILED)
      {
        LOG_FAIL_FMT("Failed to deserialise entry in private ledger");
        recovery_store->rollback(ledger_idx - 1);
        recover_private_ledger_end_unsafe();
        return;
      }

      // If the final recovery version has been reached, end recovery
      if (result == kv::DeserialiseSuccess::PASS_SIGNATURE)
        network.tables->compact(ledger_idx);

      if (recovery_store->current_version() == recovery_v)
      {
        LOG_INFO_FMT("Reached recovery final version at {}", recovery_v);
        recover_private_ledger_end_unsafe();
      }
      else
      {
        read_ledger_idx(++ledger_idx);
      }
    }

    void recover_private_ledger_end_unsafe()
    {
      sm.expect(State::readingPrivateLedger);

      // When reaching the end of the private ledger, make sure the same
      // ledger has been read and swap in private state
      auto h = dynamic_cast<MerkleTxHistory*>(recovery_history.get());
      if (h->get_root() != recovery_root)
      {
        LOG_FATAL_FMT(
          "Root of public store does not match root of private store");
      }

      network.tables->swap_private_maps(*recovery_store.get());
      recovery_store.reset();

      // Raft should deserialise all security domains when network is opened
      raft->enable_all_domains();

      // On followers, resume replication
      if (!raft->is_leader())
        raft->resume_replication();

      // Seal all known network secrets
      network.secrets->seal_all();

      accept_user_connections();
      if (raft->is_follower())
        accept_node_connections();

      LOG_INFO_FMT("Now part of network");
      sm.advance(State::partOfNetwork);
    }

    //
    // funcs in state "readingPublicLedger" or "readingPrivateLedger"
    //
    void recover_ledger_end()
    {
      std::lock_guard<SpinLock> guard(lock);

      if (is_reading_public_ledger())
      {
        recover_public_ledger_end_unsafe();
      }
      else if (is_reading_private_ledger())
      {
        recover_private_ledger_end_unsafe();
      }
      else
      {
        throw std::logic_error(
          "Cannot end ledger recovery if not reading public or private "
          "ledger");
      }
    }

    void node_quotes(Store::Tx& tx, GetQuotes::Out& result)
    {
      auto nodes_view = tx.get_view(network.nodes);

      nodes_view->foreach([&result](const NodeId& nid, const NodeInfo& ni) {
        if (ni.status == ccf::NodeStatus::TRUSTED)
        {
          GetQuotes::Quote quote;
          quote.node_id = nid;
          quote.raw = std::string(ni.quote.begin(), ni.quote.end());

#ifdef GET_QUOTE
          oe_report_t parsed_quote = {0};
          auto res =
            oe_parse_report(ni.quote.data(), ni.quote.size(), &parsed_quote);
          if (res != OE_OK)
          {
            quote.error =
              fmt::format("Failed to parse quote: {}", oe_result_str(res));
          }
          else
          {
            quote.mrenclave = fmt::format(
              "{:02x}", fmt::join(parsed_quote.identity.unique_id, ""));
          }
#endif
          result.quotes.push_back(quote);
        }
      });
    };

    //
    // funcs in state "awaitingRecoveryTx"
    //
    std::string replace_nodes(
      Store::Tx& tx, const std::vector<ccf::NodeInfo>& new_nodes)
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::awaitingRecoveryTx);

      auto [nodes_view, values_view, certs_view, sigs_view] = tx.get_view(
        network.nodes, network.values, network.node_certs, network.signatures);
      std::map<NodeId, NodeInfo> nodes_to_delete;
      nodes_view->foreach(
        [&nodes_to_delete](const NodeId& nid, const NodeInfo& ni) {
          // Only retire nodes that have not already been retired
          if (ni.status != ccf::NodeStatus::RETIRED)
            nodes_to_delete[nid] = ni;
        });
      for (auto [nid, ni] : nodes_to_delete)
      {
        ni.status = ccf::NodeStatus::RETIRED;
        nodes_view->put(nid, ni);
      }

      std::vector<Cert> certs_to_delete;
      certs_view->foreach(
        [&certs_to_delete](const Cert& cstr, const NodeId& _) {
          certs_to_delete.push_back(cstr);
        });
      for (Cert& cstr : certs_to_delete)
      {
        certs_view->remove(cstr);
      }

      for (const auto& ni : new_nodes)
      {
        auto nid = get_next_id(values_view, ccf::ValueIds::NEXT_NODE_ID);
        LOG_INFO_FMT("Adding node {}", nid);
        nodes_view->put(nid, ni);
        if (node_cert == ni.cert)
        {
          self = nid;
          LOG_INFO_FMT("Setting self to {}", self);
        }
        tls::Verifier verifier(ni.cert);
        certs_view->put(
          {verifier.raw()->raw.p,
           verifier.raw()->raw.p + verifier.raw()->raw.len},
          nid);
      }

      LOG_INFO_FMT("Replaced nodes");

      kv::Version index = 0;
      kv::Term term = 0;
      kv::Version global_commit = 0;
      auto ls = sigs_view->get(0);
      if (ls.has_value())
      {
        auto s = ls.value();
        index = s.index;
        term = s.term;
        global_commit = s.commit;
      }

      auto h = dynamic_cast<MerkleTxHistory*>(history.get());
      if (h)
        h->set_node_id(self);
      setup_raft(true);
      LOG_DEBUG_FMT(
        "Restarting Raft at index: {} term: {} commit_idx {}",
        index,
        term,
        global_commit);
      raft->force_become_leader(index, term, term_history, index);

      // Sets itself as trusted
      auto leader_info = nodes_view->get(self).value();
      leader_info.status = NodeStatus::TRUSTED;
      nodes_view->put(self, leader_info);

#ifdef GET_QUOTE
      if (!trust_own_code_id(tx, self))
        throw std::logic_error("Parsing node of first recovery node failed");
#endif

      accept_node_connections();

      LOG_INFO_FMT("Restarted network");

      sm.advance(State::partOfPublicNetwork);

      return std::string(
        network.secrets->get_current().cert.data(),
        network.secrets->get_current().cert.data() +
          network.secrets->get_current().cert.size());
    }

    //
    // funcs in state "partOfPublicNetwork"
    //
    void setup_private_recovery_store()
    {
      // Setup recovery store by cloning tables of store
      recovery_store = std::make_shared<Store>();
      recovery_store->clone_schema(*network.tables);
      Signatures* recovery_signature_map =
        recovery_store->get<Signatures>("signatures");
      Nodes* recovery_nodes_map = recovery_store->get<Nodes>("nodes");

      recovery_history = std::make_shared<MerkleTxHistory>(
        *recovery_store.get(),
        self,
        node_kp,
        *recovery_signature_map,
        *recovery_nodes_map);

      recovery_encryptor =
#ifdef USE_NULL_ENCRYPTOR
        std::make_shared<NullTxEncryptor>();
#else
        std::make_shared<TxEncryptor>(self, *network.secrets);
#endif

      recovery_store->set_history(recovery_history);
      recovery_store->set_encryptor(recovery_encryptor);

      // Record real store version and root
      recovery_v = network.tables->current_version();
      auto h = dynamic_cast<MerkleTxHistory*>(history.get());
      recovery_root = h->get_root();

      LOG_DEBUG_FMT("Recovery store successfully setup: {}", recovery_v);
    }

    bool finish_recovery(
      Store::Tx& tx, const nlohmann::json& sealed_secrets) override
    {
      std::lock_guard<SpinLock> guard(lock);
      sm.expect(State::partOfPublicNetwork);

      LOG_INFO_FMT("Initiating end of recovery (leader)");

      // Unseal past network secrets
      auto past_secrets_idx = network.secrets->restore(sealed_secrets);

      // Emit signature to certify transactions that happened on public
      // network
      history->emit_signature();

      // Write past network secrets to followers via secrets table
      auto [nodes_view, secrets_view] =
        tx.get_view(network.nodes, network.secrets_table);
      std::map<NodeId, NodeInfo> new_followers;
      nodes_view->foreach(
        [&new_followers, this](const NodeId& nid, const NodeInfo& ni) {
          if (ni.status != ccf::NodeStatus::RETIRED && nid != self)
            new_followers[nid] = ni;
        });

      // For all nodes in the new network, write all past network secrets to the
      // secrets table, encrypted with the respective ephemeral keys
      for (auto const& ns_idx : past_secrets_idx)
      {
        ccf::PastNetworkSecrets past_secrets;
        for (auto [nid, ni] : new_followers)
        {
          ccf::SerialisedNetworkSecrets ns;
          ns.node_id = nid;

          auto serial = network.secrets->get_serialised_secret(ns_idx);
          if (serial.has_value())
          {
            LOG_DEBUG_FMT(
              "Writing network secret {} of size {} to follower {} in secrets "
              "table",
              ns_idx,
              serial.value().size(),
              nid);

            // Encrypt network secrets with joiner's fresh key
            auto search = joiners_fresh_keys.find(nid);
            if (search == joiners_fresh_keys.end())
            {
              LOG_FAIL_FMT("No fresh key for joiner {}", nid);
              continue;
            }

            crypto::KeyAesGcm joiner_key(
              CBuffer(search->second.data(), search->second.size()));
            crypto::GcmCipher gcmcipher(serial.value().size());

            // Get random IV
            auto iv = tls::Entropy().random(gcmcipher.hdr.getIv().n);
            std::copy(iv.begin(), iv.end(), gcmcipher.hdr.iv);

            joiner_key.encrypt(
              iv,
              CBuffer(serial.value().data(), serial.value().size()),
              CBuffer(),
              gcmcipher.cipher.data(),
              gcmcipher.hdr.tag);

            ns.serial_ns = gcmcipher.serialise();
            past_secrets.secrets.emplace_back(std::move(ns));
          }
          else
          {
            LOG_FAIL_FMT("Network secrets have not been restored: {}", ns_idx);
            return false;
          }
        }
        secrets_view->put(ns_idx, past_secrets);
      }

      // Setup new temporary store and record current version/root
      setup_private_recovery_store();

      // Start reading private security domain of ledger
      ledger_idx = 0;
      read_ledger_idx(++ledger_idx);

      sm.advance(State::readingPrivateLedger);
      return true;
    }

    //
    // funcs in state "partOfNetwork" or "partOfPublicNetwork"
    //
    void tick(std::chrono::milliseconds elapsed)
    {
      if (
        !sm.check(State::partOfNetwork) &&
        !sm.check(State::partOfPublicNetwork))
        return;

      raft->periodic(elapsed);

#ifdef PBFT
      ITimer::handle_timeouts(elapsed);
#endif
    }

    void node_msg(const std::vector<uint8_t>& data)
    {
      // Only process messages once part of network
      if (
        !sm.check(State::partOfNetwork) &&
        !sm.check(State::partOfPublicNetwork))
      {
        return;
      }

      auto p = data.data();
      auto psize = data.size();
      Header header;
      NodeMsgType msg_type = serialized::overlay<NodeMsgType>(p, psize);

      switch (msg_type)
      {
        case channel_msg:
          n2n_channels->recv_message(p, psize);
          break;

        case consensus_msg_pbft:
#ifdef PBFT
          pbft->recv_message(p, psize);
#endif
          break;
        case consensus_msg_raft:
          raft->recv_message(p, psize);
          break;

        default:
        {}
      }
    }

    //
    // always available
    //
    bool is_leader() const override
    {
      return (
        (sm.check(State::partOfNetwork) ||
         sm.check(State::partOfPublicNetwork)) &&
        raft->is_leader());
    }

    bool is_part_of_network() const
    {
      return sm.check(State::partOfNetwork);
    }

    bool is_reading_public_ledger() const
    {
      return sm.check(State::readingPublicLedger);
    }

    bool is_reading_private_ledger() const
    {
      return sm.check(State::readingPrivateLedger);
    }

    bool is_awaiting_recovery() const
    {
      return sm.check(State::awaitingRecoveryTx);
    }

    bool is_part_of_public_network() const override
    {
      return sm.check(State::partOfPublicNetwork);
    }

    // Used from nodefrontend.h to set the joiner's fresh key to encrypt past
    // network secrets
    void set_joiner_key(
      NodeId joiner_id, const std::vector<uint8_t>& raw_key) override
    {
      LOG_DEBUG_FMT("Setting fresh key for joiner {}", joiner_id);
      joiners_fresh_keys.emplace(joiner_id, raw_key);
    }

  private:
    void accept_member_connections()
    {
      tls::KeyPair nw(network.secrets->get_current().priv_key);
      tls::KeyPair members_keypair;

      auto members_privkey = members_keypair.private_key();
      auto members_cert =
        nw.sign_csr(members_keypair.create_csr("CN=members"), "CN=The CA");

      // Accept member connections.
      rpcsessions.add_cert(
        ccf::Actors::MEMBERS, nullb, members_cert, members_privkey);
    }

    void accept_node_connections()
    {
      tls::KeyPair nw(network.secrets->get_current().priv_key);
      tls::KeyPair nodes_keypair;

      auto nodes_privkey = nodes_keypair.private_key();
      auto nodes_cert =
        nw.sign_csr(nodes_keypair.create_csr("CN=nodes"), "CN=The CA");

      // Accept node connections.
      rpcsessions.add_cert(
        ccf::Actors::NODES, nullb, nodes_cert, nodes_privkey);
    }

    void accept_user_connections()
    {
      tls::KeyPair nw(network.secrets->get_current().priv_key);
      tls::KeyPair users_keypair;

      auto users_privkey = users_keypair.private_key();
      auto users_cert =
        nw.sign_csr(users_keypair.create_csr("CN=users"), "CN=The CA");

      // Accept user connections.
      rpcsessions.add_cert(
        ccf::Actors::USERS, nullb, users_cert, users_privkey);
    }

    void accept_all_connections()
    {
      accept_member_connections();
      accept_node_connections();
      accept_user_connections();
    }

    bool trust_own_code_id(Store::Tx& tx, NodeId self)
    {
#ifdef GET_QUOTE
      // Setting own code version as trusted
      auto codeid_view = tx.get_view(network.code_id);
      codeid_view->put(node_code_id, CodeStatus::ACCEPTED);
#else
      throw std::logic_error("Code version check is not implemented");
#endif
      return true;
    }

    void follower_finish_recovery()
    {
      if (!raft->is_follower())
        return;

      sm.expect(State::partOfPublicNetwork);

      LOG_INFO_FMT("Initiating end of recovery (follower)");

      // Setup new temporary store and record current version/root
      setup_private_recovery_store();

      // Suspend raft replication at recovery_v + 1 since this is called from
      // commit hook
      raft->suspend_replication(recovery_v + 1);

      // Start reading private security domain of ledger
      ledger_idx = 0;
      read_ledger_idx(++ledger_idx);

      sm.advance(State::readingPrivateLedger);
    }

    void setup_raft(bool public_only = false)
    {
      // setup node-to-node channels, raft, pbft and store hooks
      n2n_channels->initialize(self, network.secrets->get_current().priv_key);

      raft = std::make_shared<ConsensusRaft>(
        std::make_unique<raft::Adaptor<Store, kv::DeserialiseSuccess>>(
          network.tables),
        std::make_unique<raft::LedgerEnclave>(writer_factory),
        n2n_channels,
        self,
        raft_config.requestTimeout,
        raft_config.electionTimeout,
        public_only);

      network.tables->set_replicator(raft);

#ifdef PBFT
      setup_pbft();
#endif

      // When a node is added, even locally, inform the host so that it can
      // map the node id to a hostname and service and inform raft so that it
      // can add a new active configuration.
      network.nodes.set_local_hook([this](
                                     kv::Version version,
                                     const Nodes::State& s,
                                     const Nodes::Write& w) {
        auto configure = false;
        std::unordered_set<NodeId> configuration;

        // TODO(#important,#TR): Only TRUSTED nodes should be sent append
        // entries, counted in election votes and allowed to establish
        // node-to-node channels (section III-F).
        for (auto& [node_id, ni] : w)
        {
#ifdef PBFT
          pbft->add_configuration({node_id, ni.value.host, ni.value.raftport});
#endif
          switch (ni.value.status)
          {
            case NodeStatus::PENDING:
            {
              add_node(node_id, ni.value.host, ni.value.raftport);
              configure = true;
              break;
            }
            case NodeStatus::RETIRED:
            {
              configure = true;
              break;
            }
            default:
            {}
          }
        }

        if (configure)
        {
          s.foreach([&](NodeId node_id, const Nodes::VersionV& v) {
            if (v.value.status != NodeStatus::RETIRED)
              configuration.insert(node_id);
          });
          raft->add_configuration(version, move(configuration));
        }
      });

      // When a transaction that changes the configuration commits globally,
      // inform the host of any nodes that no longer need to be tracked.
      network.nodes.set_global_hook(
        [this](
          kv::Version version, const Nodes::State& s, const Nodes::Write& w) {
          for (auto& [node_id, ni] : w)
          {
            if (ni.value.status == NodeStatus::RETIRED)
              remove_node(node_id);
          }
        });

      network.secrets_table.set_local_hook([this](
                                             kv::Version version,
                                             const Secrets::State& s,
                                             const Secrets::Write& w) {
        // If in follower mode in a public network, if new secrets are written
        // to the secrets table for our entry, decrypt these secrets and
        // initiate end of recovery protocol
        if (!(raft->is_follower() && is_part_of_public_network()))
          return;

        bool has_secrets = false;

        for (auto& [v, past_secrets] : w)
        {
          for (auto& nw_secrets_at_v : past_secrets.value.secrets)
          {
            if (nw_secrets_at_v.node_id == self)
            {
              crypto::GcmCipher gcmcipher;
              gcmcipher.deserialise(nw_secrets_at_v.serial_ns);
              std::vector<uint8_t> plain_nw_secret_at_v(
                gcmcipher.cipher.size());

              crypto::KeyAesGcm fresh_key(raw_fresh_key);

              if (!fresh_key.decrypt(
                    gcmcipher.hdr.getIv(),
                    gcmcipher.hdr.tag,
                    gcmcipher.cipher,
                    CBuffer(),
                    plain_nw_secret_at_v.data()))
              {
                throw std::logic_error(
                  "Decryption of past network secrets failed");
              }

              has_secrets = true;
              if (!network.secrets->set_secret(v, plain_nw_secret_at_v))
              {
                throw std::logic_error(
                  "Cannot set secrets because they already exist!");
              }
            }
          }
        }
        if (has_secrets)
        {
          raw_fresh_key.clear();
          follower_finish_recovery();
        }
      });
    }

    void setup_store()
    {
      history = std::make_shared<MerkleTxHistory>(
        *network.tables.get(),
        self,
        node_kp,
        network.signatures,
        network.nodes);

      encryptor =
#ifdef USE_NULL_ENCRYPTOR
        std::make_shared<NullTxEncryptor>();
#else
        std::make_shared<TxEncryptor>(self, *network.secrets);
#endif

      network.tables->set_history(history);
      network.tables->set_encryptor(encryptor);
    }

    void add_node(
      NodeId node, const std::string& hostname, const std::string& service)
    {
      if (node != self)
      {
        RINGBUFFER_WRITE_MESSAGE(
          ccf::add_node, to_host, node, hostname, service);
      }
    }

    void remove_node(NodeId node)
    {
      if (node != self)
      {
        RINGBUFFER_WRITE_MESSAGE(ccf::remove_node, to_host, node);
      }
    }

    void read_ledger_idx(raft::Index idx)
    {
      RINGBUFFER_WRITE_MESSAGE(raft::log_get, to_host, idx);
    }

    void log_truncate(raft::Index idx)
    {
      RINGBUFFER_WRITE_MESSAGE(raft::log_truncate, to_host, idx);
    }

#ifdef PBFT
    void setup_pbft()
    {
      pbft = std::make_shared<ConsensusPbft>(n2n_channels, self);
    }
#endif
  };
}
