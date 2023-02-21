/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/merge_chunk_request_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ShardSvrMergeAllChunksOnShardCommand final
    : public TypedCommand<ShardSvrMergeAllChunksOnShardCommand> {
public:
    using Request = ShardSvrMergeAllChunksOnShard;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command invoked either by the config server or by the mongos to merge all "
               "contiguous chunks on a shard";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            uassert(ErrorCodes::InvalidNamespace,
                    "invalid namespace specified for request",
                    ns().isValid());

            ConfigSvrCommitMergeAllChunksOnShard configSvrCommitMergeAllChunksOnShard(ns());
            configSvrCommitMergeAllChunksOnShard.setDbName(DatabaseName::kAdmin);
            configSvrCommitMergeAllChunksOnShard.setShard(request().getShard());

            auto config = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto swCommandResponse = config->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                "admin",
                configSvrCommitMergeAllChunksOnShard.toBSON(
                    BSON(WriteConcernOptions::kWriteConcernField
                         << ShardingCatalogClient::kMajorityWriteConcern.toBSON())),
                Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(swCommandResponse));
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };
} shardSvrMergeAllChunksOnShard;

}  // namespace
}  // namespace mongo
