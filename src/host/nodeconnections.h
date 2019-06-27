// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "ledger.h"
#include "node/nodetypes.h"
#include "raft/rafttypes.h"
#include "tcp.h"

#include <unordered_map>

namespace asynchost
{
  class NodeConnections
  {
  private:
    class ConnectionBehaviour : public TCPBehaviour
    {
    public:
      NodeConnections& parent;
      ccf::NodeId node;
      uint32_t msg_size = (uint32_t)-1;
      std::vector<uint8_t> pending;

      ConnectionBehaviour(NodeConnections& parent, ccf::NodeId node) :
        parent(parent),
        node(node)
      {}

      void on_read(size_t len, uint8_t*& incoming)
      {
        LOG_DEBUG_FMT("node {} received {}", node, len);

        pending.insert(pending.end(), incoming, incoming + len);

        const uint8_t* data = pending.data();
        size_t size = pending.size();
        size_t used = 0;

        while (true)
        {
          if (msg_size == (uint32_t)-1)
          {
            if (size < sizeof(uint32_t))
              break;

            msg_size = serialized::read<uint32_t>(data, size);
            used += sizeof(uint32_t);
          }

          if (size < msg_size)
          {
            LOG_DEBUG_FMT("node {} has {}/{}", node, size, msg_size);
            break;
          }

          auto p = data;
          auto psize = size;
          auto msg_type = serialized::read<ccf::NodeMsgType>(p, psize);
          auto header = serialized::read<ccf::Header>(p, psize);

          if (node == ccf::NoNode)
            associate(header.from_node);

          LOG_DEBUG_FMT(
            "node in: node {}, size {}, type {}", node, msg_size, msg_type);

          RINGBUFFER_WRITE_MESSAGE(
            ccf::node_inbound,
            parent.to_enclave,
            serializer::ByteRange{data, msg_size});

          data += msg_size;
          used += msg_size;
          size -= msg_size;
          msg_size = (uint32_t)-1;
        }

        if (used > 0)
          pending.erase(pending.begin(), pending.begin() + used);
      }

      virtual void associate(ccf::NodeId) {}
    };

    class IncomingBehaviour : public ConnectionBehaviour
    {
    public:
      size_t id;

      IncomingBehaviour(NodeConnections& parent, size_t id) :
        ConnectionBehaviour(parent, ccf::NoNode),
        id(id)
      {}

      void on_disconnect()
      {
        LOG_DEBUG_FMT("node incoming disconnect {}, {}", id, node);

        parent.incoming.erase(id);

        if (node != ccf::NoNode)
          parent.associated.erase(node);
      }

      virtual void associate(ccf::NodeId n)
      {
        node = n;
        parent.associated.emplace(node, parent.incoming.at(id));
        LOG_DEBUG_FMT("node incoming {} associated with {}", id, node);
      }
    };

    class OutgoingBehaviour : public ConnectionBehaviour
    {
    public:
      OutgoingBehaviour(NodeConnections& parent, ccf::NodeId node) :
        ConnectionBehaviour(parent, node)
      {}

      void on_resolve_failed()
      {
        LOG_DEBUG_FMT("node resolve failed {}", node);
        reconnect();
      }

      void on_connect_failed()
      {
        LOG_DEBUG_FMT("node connect failed {}", node);
        reconnect();
      }

      void on_disconnect()
      {
        LOG_DEBUG_FMT("node disconnect failed {}", node);
        reconnect();
      }

      void reconnect()
      {
        auto s = parent.find(node);

        if (s)
          s.value()->reconnect();
      }
    };

    class ServerBehaviour : public TCPBehaviour
    {
    public:
      NodeConnections& parent;

      ServerBehaviour(NodeConnections& parent) : parent(parent) {}

      void on_accept(TCP& peer)
      {
        auto id = parent.get_next_id();
        peer->set_behaviour(std::make_unique<IncomingBehaviour>(parent, id));
        parent.incoming.emplace(id, peer);

        LOG_DEBUG_FMT("node accept {}", id);
      }
    };

    Ledger& ledger;
    TCP listener;
    std::unordered_map<ccf::NodeId, TCP> outgoing;
    std::unordered_map<size_t, TCP> incoming;
    std::unordered_map<ccf::NodeId, TCP> associated;
    size_t next_id = 1;
    std::unique_ptr<ringbuffer::AbstractWriter> to_enclave;

  public:
    NodeConnections(
      Ledger& ledger,
      ringbuffer::AbstractWriterFactory& writer_factory,
      const std::string& host,
      const std::string& service) :
      ledger(ledger),
      to_enclave(writer_factory.create_writer_to_inside())
    {
      listener->set_behaviour(std::make_unique<ServerBehaviour>(*this));
      listener->listen(host, service);
    }

    void register_message_handlers(
      messaging::Dispatcher<ringbuffer::Message>& disp)
    {
      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, ccf::add_node, [this](const uint8_t* data, size_t size) {
          auto [id, hostname, service] =
            ringbuffer::read_message<ccf::add_node>(data, size);
          add_node(id, hostname, service);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, ccf::remove_node, [this](const uint8_t* data, size_t size) {
          auto [id] = ringbuffer::read_message<ccf::remove_node>(data, size);
          remove_node(id);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, ccf::node_outbound, [this](const uint8_t* data, size_t size) {
          auto to = serialized::read<ccf::NodeId>(data, size);
          auto node = find(to, true);

          if (!node)
            return;

          auto data_to_send = data;
          auto size_to_send = size;

          // If the message is a raft append entries message, affix the
          // corresponding ledger entries
          if (
            serialized::read<ccf::NodeMsgType>(data, size) ==
              ccf::NodeMsgType::consensus_msg_raft &&
            serialized::peek<raft::RaftMsgType>(data, size) ==
              raft::raft_append_entries)
          {
            // Parse the indices to be sent to the recipient.
            auto p = data;
            auto psize = size;
            const auto& ae = serialized::overlay<raft::AppendEntries>(p, psize);

            // Find the total frame size, and write it along with the header.
            auto count = ae.idx - ae.prev_idx;
            uint32_t frame = (uint32_t)(
              size_to_send +
              ledger.framed_entries_size(ae.prev_idx + 1, ae.idx));

            LOG_DEBUG_FMT(
              "raft send AE to {} [{}]: {}, {}",
              to,
              frame,
              ae.idx,
              ae.prev_idx);

            // TODO(#performance): writev
            node.value()->write(sizeof(uint32_t), (uint8_t*)&frame);
            node.value()->write(size_to_send, data_to_send);

            auto framed_entries =
              ledger.read_framed_entries(ae.prev_idx + 1, ae.idx);
            frame = (uint32_t)framed_entries.size();
            node.value()->write(frame, framed_entries.data());
          }
          else
          {
            // Write as framed data to the recipient.
            uint32_t frame = (uint32_t)size_to_send;

            LOG_DEBUG_FMT("node send to {} [{}]", to, frame);

            node.value()->write(sizeof(uint32_t), (uint8_t*)&frame);
            node.value()->write(size_to_send, data_to_send);
          }
        });
    }

  private:
    bool add_node(
      ccf::NodeId node, const std::string& host, const std::string& service)
    {
      if (outgoing.find(node) != outgoing.end())
      {
        LOG_FAIL_FMT("Cannot add node {}: already in use", node);
        return false;
      }

      LOG_DEBUG_FMT("Adding node {} {}:{}", node, host, service);

      TCP s;
      s->set_behaviour(std::make_unique<OutgoingBehaviour>(*this, node));

      if (!s->connect(host, service))
      {
        LOG_DEBUG_FMT("Node failed initial connect {}", node);
        return false;
      }

      outgoing.emplace(node, s);
      return true;
    }

    std::optional<TCP> find(ccf::NodeId node, bool use_incoming = false)
    {
      auto s = outgoing.find(node);

      if (s != outgoing.end())
        return s->second;

      if (use_incoming)
      {
        auto s = associated.find(node);

        if (s != associated.end())
          return s->second;
      }

      LOG_FAIL_FMT("Unknown node {}", node);
      return {};
    }

    bool remove_node(ccf::NodeId node)
    {
      LOG_DEBUG_FMT("removing node {}", node);

      if (outgoing.erase(node) < 1)
      {
        LOG_FAIL_FMT("Cannot remove node {}: does not exist", node);
        return false;
      }

      return true;
    }

    size_t get_next_id()
    {
      auto id = next_id++;

      while (incoming.find(id) != incoming.end())
        id = next_id++;

      return id;
    }
  };
}
