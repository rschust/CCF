// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#include "enclave/appinterface.h"
#include "luainterp/luaargs.h"
#include "luainterp/luainterp.h"
#include "luainterp/luakv.h"
#include "luainterp/txscriptrunner.h"
#include "node/entities.h"
#include "node/rpc/nodeinterface.h"
#include "node/rpc/userfrontend.h"

#include <memory>
#include <vector>

namespace ccfapp
{
  using namespace std;
  using namespace kv;
  using namespace ccf;
  using namespace lua;

  using GenericTable = Store::Map<nlohmann::json, nlohmann::json>;

  class AppTsr : public TxScriptRunner
  {
  private:
    // TODO: once the kv store allows for dynamic table creation, we can derive
    // this list dynamically based on an apps requirements.
    const std::vector<GenericTable*> app_tables;
    void add_custom_tables(
      lua::Interpreter& li,
      Store::Tx& tx,
      int& n_registered_tables) const override
    {
      n_registered_tables++;
      TableCreator<false>::create(li, tx, app_tables);
    }

  public:
    AppTsr(NetworkTables& network, std::vector<GenericTable*> app_tables) :
      TxScriptRunner(network),
      app_tables(app_tables)
    {}
  };

  class Lua : public ccf::UserRpcFrontend
  {
  private:
    NetworkTables& network;
    std::unique_ptr<AppTsr> tsr;

  public:
    Lua(NetworkTables& network, const uint16_t n_tables = 8) :
      UserRpcFrontend(*network.tables),
      network(network)
    {
      // create public and private app tables (2x n_tables in total)
      std::vector<GenericTable*> app_tables(n_tables * 2);
      for (uint16_t i = 0; i < n_tables; i++)
      {
        const auto suffix = std::to_string(i);
        app_tables[i] = &tables.create<GenericTable>("priv" + suffix);
        app_tables[i + n_tables] = &tables.create<GenericTable>("pub" + suffix);
      }
      tsr = std::make_unique<AppTsr>(network, app_tables);

      auto default_handler = [this](RequestArgs& args) {
        if (args.method == UserScriptIds::ENV_HANDLER)
          return jsonrpc::error(
            jsonrpc::ErrorCodes::METHOD_NOT_FOUND,
            "Cannot call environment script ('" + args.method + "')");

        const auto scripts = args.tx.get_view(this->network.app_scripts);

        // try find script for method
        auto handler_script = scripts->get(args.method);
        if (!handler_script)
          return jsonrpc::error(
            jsonrpc::ErrorCodes::METHOD_NOT_FOUND,
            "No handler script found for method '" + args.method + "'");

        const auto response = tsr->run<nlohmann::json>(
          args.tx,
          {*handler_script,
           {},
           WlIds::USER_APP_CAN_READ_ONLY,
           scripts->get(UserScriptIds::ENV_HANDLER)},
          // vvv arguments to the script vvv
          args);

        const auto err_it = response.find(jsonrpc::ERR);
        if (err_it == response.end())
        {
          const auto result_it = response.find(jsonrpc::RESULT);
          if (result_it == response.end())
          {
            // Response contains neither RESULT nor ERR. It may not even be an
            // object. We assume the entire response is a successful result.
            return make_pair(true, response);
          }
          else
          {
            return make_pair(true, *result_it);
          }
        }
        else
        {
          return make_pair(false, *err_it);
        }
      };
      set_default(default_handler, MayWrite);
    }
  };

  std::shared_ptr<enclave::RpcHandler> get_rpc_handler(
    NetworkTables& network, AbstractNotifier& notifier)
  {
    return make_shared<Lua>(network);
  }
} // namespace ccfapp
