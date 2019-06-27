// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once
#include "node/whitelists.h"

#include <map>

namespace ccf
{
  // TODO: read from json/lua file during genesis creation
  static const std::map<WlIds, Whitelist> default_whitelists = {
    {MEMBER_CAN_READ,
     {Tables::MEMBERS,
      Tables::MEMBER_CERTS,
      Tables::MEMBER_ACKS,
      Tables::USER_CERTS,
      Tables::NODES,
      Tables::NODE_CERTS,
      Tables::VALUES,
      Tables::SIGNATURES,
      Tables::USER_CLIENT_SIGNATURES,
      Tables::MEMBER_CLIENT_SIGNATURES,
      Tables::WHITELISTS,
      Tables::PROPOSALS,
      Tables::GOV_SCRIPTS,
      Tables::APP_SCRIPTS,
      Tables::APP_PUBLIC}},

    {MEMBER_CAN_PROPOSE,
     {Tables::MEMBERS,
      Tables::MEMBER_CERTS,
      Tables::USER_CERTS,
      Tables::NODES,
      Tables::NODE_CERTS,
      Tables::VALUES,
      Tables::WHITELISTS,
      Tables::GOV_SCRIPTS,
      Tables::APP_SCRIPTS}},

    {USER_APP_CAN_READ_ONLY,
     {Tables::MEMBERS,
      Tables::MEMBER_CERTS,
      Tables::MEMBER_ACKS,
      Tables::WHITELISTS,
      Tables::GOV_SCRIPTS,
      Tables::APP_SCRIPTS,
      Tables::VOTING_HISTORY}},

    {USER_APP_CAN_WRITE, {Tables::APP_PUBLIC, Tables::APP}}};
}