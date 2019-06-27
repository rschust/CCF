# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the Apache 2.0 License.
import argparse
import os
import logging
import infra.runner
import e2e_args
from infra.perf import PERF_COLUMNS

from loguru import logger as LOG


def cli_args(add=lambda x: None, accept_unknown=False):
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--client", help="Client binary", required=True)
    parser.add_argument(
        "-n",
        "--nodes",
        help="List of hostnames[,pub_hostnames:ports]. If empty, two nodes are spawned locally",
        action="append",
    )
    parser.add_argument(
        "-cn",
        "--client-nodes",
        help="List of hostnames for spawning client(s). If empty, one client is spawned locally",
        action="append",
    )
    parser.add_argument(
        "--send-tx-to",
        choices=["primary", "followers", "all"],
        default="all",
        help="Send client requests only to primary, only to followers, or to all nodes",
    )
    parser.add_argument(
        "--metrics-file",
        default="metrics.json",
        help="Path to json file where the transaction rate metrics will be saved to",
    )
    parser.add_argument("-t", "--threads", help="Number of client threads", default=1)
    parser.add_argument(
        "-f",
        "--fixed-seed",
        help="Set a fixed seed for port and IP generation.",
        action="store_true",
    )

    # Client binary args are parsed from a config file
    # Default is in the same directory as this script
    default_config_path = os.path.join(
        os.path.dirname(os.path.realpath(__file__)), "common_config.ini"
    )
    parser.add_argument(
        "--config", help="Path to config for client binary", default=default_config_path
    )

    parser.add_argument(
        "-i", "--iterations", help="Number of transactions", required=True, type=int
    )
    parser.add_argument(
        "--sign", help="Sign all client transactions", action="store_true"
    )

    return e2e_args.cli_args(add=add, parser=parser, accept_unknown=accept_unknown)


def run(*args, **kwargs):
    with infra.path.working_dir(args[0]):
        infra.path.mk_new("perf_summary.csv", PERF_COLUMNS)

    infra.runner.run(*args, **kwargs)


if __name__ == "__main__":

    def add(parser):
        parser.add_argument(
            "-p",
            "--package",
            help="The enclave package to load (e.g., libloggingenc)",
            required=True,
        )

    def get_command(host_arg, port_arg):
        return ["host:", host_arg, "port:", port_arg]

    args = cli_args(add)

    infra.runner.run(args.build_dir, args.package, get_command, args)
