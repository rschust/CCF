# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.
import socket
import ssl
import msgpack
import struct
import select
import contextlib
import json
import logging
import time
import os
from enum import IntEnum
from cryptography import x509
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import asymmetric

from loguru import logger as LOG

# Values defined in node/rpc/jsonrpc.h
class ErrorCode(IntEnum):
    PARSE_ERROR = -32700
    INVALID_REQUEST = -32600
    METHOD_NOT_FOUND = -32601
    INVALID_PARAMS = -32602
    INTERNAL_ERROR = -32603
    NODE_NOT_FOUND = -32604
    INVALID_CLIENT_SIGNATURE = -32605
    INVALID_CALLER_ID = -32606
    CODE_ID_NOT_FOUND = -32607
    CODE_ID_RETIRED = -32608
    RPC_NOT_FORWARDED = -32609
    SERVER_ERROR_START = -32000
    TX_NOT_LEADER = -32001
    TX_REPLICATED = -32002
    TX_ROLLED_BACK = -32003
    TX_FAILED_TO_COMMIT = -32004
    TX_FAILED_TO_REPLICATE = -32005
    SCRIPT_ERROR = -32006
    INSUFFICIENT_RIGHTS = -32007
    DENIED = -32008
    TX_LEADER_UNKNOWN = -32009
    RPC_NOT_SIGNED = -32010
    SERVER_ERROR_END = -32099


def truncate(string, max_len=256):
    if len(string) > 256:
        return string[: 256 - 3] + "..."
    else:
        return string


class Request:
    def __init__(self, id, method, params, jsonrpc="2.0"):
        self.id = id
        self.method = method
        self.params = params
        self.jsonrpc = jsonrpc

    def to_dict(self):
        return {
            "id": self.id,
            "method": self.method,
            "jsonrpc": self.jsonrpc,
            "params": self.params,
        }

    def to_msgpack(self):
        return msgpack.packb(self.to_dict(), use_bin_type=True)

    def to_json(self):
        return json.dumps(self.to_dict()).encode()


class Response:
    def __init__(
        self,
        id,
        result=None,
        error=None,
        commit=None,
        term=None,
        global_commit=None,
        jsonrpc="2.0",
    ):
        self.id = id
        self.result = result
        self.error = error
        self.jsonrpc = jsonrpc
        self.commit = commit
        self.term = term
        self.global_commit = global_commit
        self._attrs = set(locals()) - {"self"}

    def to_dict(self):
        d = {"id": self.id, "jsonrpc": self.jsonrpc}
        if self.result is not None:
            d["result"] = self.result
        else:
            d["error"] = self.error
        return d

    def _from_parsed(self, parsed):
        def decode(sl, is_key=False):
            if is_key and hasattr(sl, "decode"):
                return sl.decode()
            if hasattr(sl, "items"):
                return {decode(k, is_key=True): decode(v) for k, v in sl.items()}
            elif isinstance(sl, list):
                return [decode(e) for e in sl]
            else:
                return sl

        parsed_s = {
            decode(attr, is_key=True): decode(value) for attr, value in parsed.items()
        }
        unexpected = parsed_s.keys() - self._attrs
        if unexpected:
            raise ValueError("Unexpected keys in response: {}".format(unexpected))
        for attr, value in parsed_s.items():
            setattr(self, attr, value)

    def from_msgpack(self, data):
        parsed = msgpack.unpackb(data)
        self._from_parsed(parsed)

    def from_json(self, data):
        parsed = json.loads(data.decode())
        self._from_parsed(parsed)


class FramedTLSClient:
    def __init__(self, host, port, server_hostname, cert=None, key=None, cafile=None):
        self.host = host
        self.port = port
        self.server_hostname = server_hostname
        self.cert = cert
        self.key = key
        self.cafile = cafile
        self.context = None
        self.sock = None
        self.conn = None

    def connect(self):
        if self.cafile:
            self.context = ssl.create_default_context(cafile=self.cafile)

            # Auto detect EC curve to use based on server CA
            ca_bytes = open(self.cafile, "rb").read()
            ca_curve = (
                x509.load_pem_x509_certificate(ca_bytes, default_backend())
                .public_key()
                .curve
            )
            if isinstance(ca_curve, asymmetric.ec.SECP256K1):
                self.context.set_ecdh_curve("secp256k1")
        else:
            self.context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        if self.cert and self.key:
            self.context.load_cert_chain(certfile=self.cert, keyfile=self.key)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.conn = self.context.wrap_socket(
            self.sock, server_side=False, server_hostname=self.server_hostname
        )
        self.conn.connect((self.host, self.port))

    def send(self, msg):
        frame = struct.pack("<I", len(msg)) + msg
        self.conn.sendall(frame)

    def _read(self):
        size, = struct.unpack("<I", self.conn.recv(4))
        data = self.conn.recv(size)
        while len(data) < size:
            data += self.conn.recv(size - len(data))
        return data

    def read(self):
        for _ in range(5000):
            r, _, _ = select.select([self.conn], [], [], 0)
            if r:
                return self._read()
            else:
                time.sleep(0.01)

    def disconnect(self):
        self.conn.close()


class Stream:
    def __init__(self, jsonrpc="2.0", format="msgpack"):
        self.jsonrpc = jsonrpc
        self.seqno = 0
        self.pending = {}
        self.format = format

    def request(self, method, params):
        r = Request(self.seqno, method, params, self.jsonrpc)
        self.seqno += 1
        return r

    def response(self, id):
        return self.pending.pop(id, None)

    def update(self, msg):
        r = Response(0)
        getattr(r, "from_{}".format(self.format))(msg)
        self.pending[r.id] = r


class RPCLogger:
    def log_request(self, request, name, description):
        LOG.info(
            truncate(
                "{} #{} {} {}{}".format(
                    name, request.id, request.method, request.params, description
                )
            )
        )

    def log_response(self, response):
        LOG.debug(
            truncate(
                "#{} {}".format(
                    response.id,
                    {
                        k: v
                        for k, v in (response.__dict__ or {}).items()
                        if not k.startswith("_")
                    },
                )
            )
        )


class RPCFileLogger(RPCLogger):
    def __init__(self, path):
        self.path = path

    def log_request(self, request, name, description):
        with open(self.path, "a") as f:
            f.write(">> Request:" + os.linesep)
            json.dump(request.to_dict(), f, indent=2)
            f.write(os.linesep)

    def log_response(self, response):
        with open(self.path, "a") as f:
            f.write("<< Response:" + os.linesep)
            json.dump(response.to_dict(), f, indent=2)
            f.write(os.linesep)


class FramedTLSJSONRPCClient:
    def __init__(
        self,
        host,
        port,
        server_hostname,
        cert=None,
        key=None,
        cafile=None,
        version="2.0",
        format="msgpack",
        description=None,
    ):
        self.client = FramedTLSClient(host, port, server_hostname, cert, key, cafile)
        self.stream = Stream(version, format=format)
        self.format = format
        self.name = "[{}:{}]".format(host, port)
        self.description = description
        self.rpc_loggers = (RPCLogger(),)

    def connect(self):
        return self.client.connect()

    def disconnect(self):
        return self.client.disconnect()

    def request(self, method, params):
        r = self.stream.request(method, params)
        self.client.send(getattr(r, "to_{}".format(self.format))())
        description = ""
        if self.description:
            description = " ({})".format(self.description)
        for logger in self.rpc_loggers:
            logger.log_request(r, self.name, description)
        return r.id

    def tick(self):
        msg = self.client.read()
        self.stream.update(msg)

    def response(self, id):
        self.tick()
        r = self.stream.response(id)
        for logger in self.rpc_loggers:
            logger.log_response(r)
        return r

    def do(self, method, params, expected_result=None, expected_error_code=None):
        id = self.request(method, params)
        r = self.response(id)

        if expected_result is not None:
            assert expected_result == r.result

        if expected_error_code is not None:
            assert expected_error_code.value == r.error["code"]
        return r

    def rpc(self, method, params):
        id = self.request(method, params)
        return self.response(id)


@contextlib.contextmanager
def client(
    host,
    port,
    server_hostname="users",
    cert=None,
    key=None,
    cafile=None,
    version="2.0",
    format="msgpack",
    description=None,
    log_file=None,
):
    c = FramedTLSJSONRPCClient(
        host, port, server_hostname, cert, key, cafile, version, format, description
    )

    if log_file is not None:
        c.rpc_loggers += (RPCFileLogger(log_file),)

    c.connect()
    try:
        yield c
    finally:
        c.disconnect()
