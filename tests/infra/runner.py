# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.
import getpass
import time
import logging
import multiprocessing
import json
from random import seed
import infra.ccf
import infra.proc
import infra.remote_client
import infra.jsonrpc
import infra.rates
import os
import re

from loguru import logger as LOG

logging.getLogger("matplotlib").setLevel(logging.WARNING)
logging.getLogger("paramiko").setLevel(logging.WARNING)


def number_of_local_nodes():
    """
    On 2-core VMs, we start only one node, but on 4 core, we want to start 2.
    Not 3, because the client is typically running two threads.
    """
    if multiprocessing.cpu_count() > 2:
        return 2
    else:
        return 1


def get_command_args(args, get_command):
    command_args = []
    if args.label:
        command_args.append("--label={}".format(args.label))
    if args.sign:
        command_args.append("--sign")
    return get_command(*command_args)


def filter_nodes(primary, followers, filter_type):
    if filter_type == "primary":
        return [primary]
    elif filter_type == "followers":
        return followers
    else:
        return [primary] + followers


def configure_remote_client(args, client_id, client_host, node, command_args):
    if client_host == "localhost":
        client_host = infra.net.expand_localhost()
        remote_impl = infra.remote.LocalRemote
    else:
        remote_impl = infra.remote.SSHRemote
    try:
        remote_client = infra.remote_client.CCFRemoteClient(
            "client_" + str(client_id),
            client_host,
            args.client,
            node.host,
            node.tls_port,
            args.workspace,
            args.label,
            args.iterations,
            args.config,
            command_args,
            remote_impl,
        )
        remote_client.setup()
        return remote_client
    except Exception:
        LOG.exception("Failed to start client {}".format(client_host))
        raise


def run_client(args, primary, command_args):
    command = [
        args.client,
        "--host={}".format(primary.host),
        "--port={}".format(primary.tls_port),
        "--transactions={}".format(args.iterations),
        "--config={}".format(args.config),
    ]
    command += command_args

    LOG.info("Client can be run with {}".format(" ".join(command)))
    while True:
        time.sleep(60)


def run(build_directory, get_command, args):
    if args.fixed_seed:
        seed(getpass.getuser())

    hosts = args.nodes
    if not hosts:
        hosts = ["localhost"] * number_of_local_nodes()

    LOG.info("Starting nodes on {}".format(hosts))

    with infra.ccf.network(
        hosts, args.build_dir, args.debug_nodes, args.perf_nodes, pdb=args.pdb
    ) as network:
        primary, followers = network.start_and_join(args)

        command_args = get_command_args(args, get_command)

        if args.network_only:
            run_client(args, primary, command_args)
        else:
            nodes = filter_nodes(primary, followers, args.send_tx_to)
            clients = []
            client_hosts = args.client_nodes or ["localhost"]
            for client_id, client_host in enumerate(client_hosts):
                node = nodes[client_id % len(nodes)]
                remote_client = configure_remote_client(
                    args, client_id, client_host, node, command_args
                )
                clients.append(remote_client)

            for remote_client in clients:
                remote_client.start()

            try:
                tx_rates = infra.rates.TxRates(primary)
                while True:
                    if not tx_rates.process_next():
                        for i, remote_client in enumerate(clients):
                            remote_client.wait()
                            remote_client.print_result()
                            remote_client.stop()
                        break
                    time.sleep(1)

                LOG.info(f"Rates: {tx_rates}")
                tx_rates.save_results(args.metrics_file)

            except KeyboardInterrupt:
                for remote_client in clients:
                    remote_client.stop()
