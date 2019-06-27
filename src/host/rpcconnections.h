// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "../tls/msg_types.h"
#include "tcp.h"

#include <unordered_map>

namespace asynchost
{
  class RPCConnections
  {
  private:
    class ClientBehaviour : public TCPBehaviour
    {
    public:
      RPCConnections& parent;
      int64_t id;

      ClientBehaviour(RPCConnections& parent, int64_t id) :
        parent(parent),
        id(id)
      {}

      void on_resolve_failed()
      {
        LOG_DEBUG_FMT("rpc resolve failed {}", id);
        cleanup();
      }

      void on_connect_failed()
      {
        LOG_DEBUG_FMT("rpc connect failed {}", id);
        cleanup();
      }

      void on_read(size_t len, uint8_t*& data)
      {
        LOG_DEBUG_FMT("rpc read {}: {}", id, len);

        RINGBUFFER_WRITE_MESSAGE(
          tls::tls_inbound,
          parent.to_enclave,
          (size_t)id,
          serializer::DynamicByteRange{data, len});
      }

      void on_disconnect()
      {
        LOG_DEBUG_FMT("rpc disconnect {}", id);
        cleanup();
      }

      void cleanup()
      {
        parent.sockets.erase(id);
        RINGBUFFER_WRITE_MESSAGE(tls::tls_stop, parent.to_enclave, (size_t)id);
      }
    };

    class ServerBehaviour : public TCPBehaviour
    {
    public:
      RPCConnections& parent;
      int64_t id;

      ServerBehaviour(RPCConnections& parent, int64_t id) :
        parent(parent),
        id(id)
      {}

      void on_resolve_failed()
      {
        LOG_DEBUG_FMT("rpc resolve failed {}", id);
        cleanup();
      }

      void on_listen_failed()
      {
        LOG_DEBUG_FMT("rpc connect failed {}", id);
        cleanup();
      }

      void on_accept(TCP& peer)
      {
        auto client_id = parent.get_next_id();
        peer->set_behaviour(
          std::make_unique<ClientBehaviour>(parent, client_id));

        parent.sockets.emplace(client_id, peer);

        LOG_DEBUG_FMT("rpc accept {}", client_id);

        RINGBUFFER_WRITE_MESSAGE(
          tls::tls_start, parent.to_enclave, (size_t)client_id);
      }

      void cleanup()
      {
        parent.sockets.erase(id);
      }
    };

    std::unordered_map<int64_t, TCP> sockets;
    int64_t next_id = 1;

    std::unique_ptr<ringbuffer::AbstractWriter> to_enclave;

  public:
    RPCConnections(ringbuffer::AbstractWriterFactory& writer_factory) :
      to_enclave(writer_factory.create_writer_to_inside())
    {}

    bool listen(int64_t id, const std::string& host, const std::string& service)
    {
      if (id == 0)
        id = get_next_id();

      if (sockets.find(id) != sockets.end())
      {
        LOG_FAIL_FMT("Cannot listen on id {}: already in use", id);
        return false;
      }

      TCP s;
      s->set_behaviour(std::make_unique<ServerBehaviour>(*this, id));

      if (!s->listen(host, service))
        return false;

      sockets.emplace(id, s);
      return true;
    }

    bool connect(
      int64_t id, const std::string& host, const std::string& service)
    {
      if (id == 0)
        id = get_next_id();

      if (sockets.find(id) != sockets.end())
      {
        LOG_FAIL_FMT("Cannot connect on id {}: already in use", id);
        return false;
      }

      TCP s;
      s->set_behaviour(std::make_unique<ClientBehaviour>(*this, id));

      if (!s->connect(host, service))
        return false;

      sockets.emplace(id, s);
      return true;
    }

    bool write(int64_t id, size_t len, const uint8_t* data)
    {
      auto s = sockets.find(id);

      if (s == sockets.end())
      {
        LOG_FAIL_FMT(
          "Received an outbound message for id {} which is not a known "
          "connection. Ignoring message of {} bytes",
          id,
          len);
        return false;
      }

      return s->second->write(len, data);
    }

    bool close(int64_t id)
    {
      if (sockets.erase(id) < 1)
      {
        LOG_FAIL_FMT("Cannot close id {}: does not exist", id);
        return false;
      }

      return true;
    }

    void register_message_handlers(
      messaging::Dispatcher<ringbuffer::Message>& disp)
    {
      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, tls::tls_outbound, [this](const uint8_t* data, size_t size) {
          auto [id, body] =
            ringbuffer::read_message<tls::tls_outbound>(data, size);

          int64_t connect_id = (int64_t)id;
          LOG_DEBUG_FMT("rpc write from enclave {}: {}", connect_id, body.size);

          write(connect_id, body.size, body.data);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, tls::tls_connect, [this](const uint8_t* data, size_t size) {
          auto [id, host, service] =
            ringbuffer::read_message<tls::tls_connect>(data, size);

          int64_t connect_id = (int64_t)id;
          LOG_DEBUG_FMT("rpc connect request from enclave {}", connect_id);

          if (check_enclave_side_id(connect_id))
            connect(connect_id, host, service);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, tls::tls_closed, [this](const uint8_t* data, size_t size) {
          auto [id] = ringbuffer::read_message<tls::tls_closed>(data, size);

          LOG_DEBUG_FMT("rpc closed from enclave {}", id);
          close(id);
        });

      DISPATCHER_SET_MESSAGE_HANDLER(
        disp, tls::tls_error, [this](const uint8_t* data, size_t size) {
          auto [id] = ringbuffer::read_message<tls::tls_error>(data, size);

          LOG_DEBUG_FMT("rpc error from enclave {}", id);
          close(id);
        });
    }

  private:
    int64_t get_next_id()
    {
      auto id = next_id++;

      if (next_id < 0)
        next_id = 1;

      while (sockets.find(id) != sockets.end())
      {
        id++;

        if (id < 0)
          id = 1;
      }

      return id;
    }

    bool check_enclave_side_id(int64_t id)
    {
      bool ok = id < 0;

      if (!ok)
      {
        LOG_FAIL_FMT("rpc id is not in dedicated range ({})", id);
      }

      return ok;
    }
  };
}
