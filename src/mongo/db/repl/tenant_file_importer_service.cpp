/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/repl/tenant_file_importer_service.h"

#include <boost/filesystem.hpp>
#include <fmt/format.h>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo::repl {

using namespace fmt::literals;
using namespace shard_merge_utils;
using namespace tenant_migration_access_blocker;

namespace {
const auto _TenantFileImporterService =
    ServiceContext::declareDecoration<TenantFileImporterService>();

const ReplicaSetAwareServiceRegistry::Registerer<TenantFileImporterService>
    _TenantFileImporterServiceRegisterer("TenantFileImporterService");

void importCopiedFiles(OperationContext* opCtx,
                       UUID& migrationId,
                       const StringData& donorConnectionString) {
    auto tempWTDirectory = fileClonerTempDir(migrationId);
    uassert(6113315,
            str::stream() << "Missing file cloner's temporary dbpath directory: "
                          << tempWTDirectory.string(),
            boost::filesystem::exists(tempWTDirectory));

    // TODO SERVER-63204: Evaluate correct place to remove the temporary WT dbpath.
    ON_BLOCK_EXIT([&tempWTDirectory, &migrationId] {
        LOGV2_INFO(6113324,
                   "Done importing files, removing the temporary WT dbpath",
                   "migrationId"_attr = migrationId,
                   "tempDbPath"_attr = tempWTDirectory.string());
        boost::system::error_code ec;
        boost::filesystem::remove_all(tempWTDirectory, ec);
    });

    auto metadatas = wiredTigerRollbackToStableAndGetMetadata(opCtx, tempWTDirectory.string());
    for (auto&& m : metadatas) {
        auto tenantId = parseTenantIdFromDB(m.ns.db());
        if (tenantId == boost::none) {
            continue;
        }

        LOGV2_DEBUG(6114100, 1, "Create recipient access blocker", "tenantId"_attr = tenantId);
        addTenantMigrationRecipientAccessBlocker(opCtx->getServiceContext(),
                                                 *tenantId,
                                                 migrationId,
                                                 MigrationProtocolEnum::kShardMerge,
                                                 donorConnectionString);
    }

    wiredTigerImportFromBackupCursor(opCtx, metadatas, tempWTDirectory.string());

    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& m : metadatas) {
        AutoGetDb dbLock(opCtx, m.ns.db(), MODE_IX);
        Lock::CollectionLock systemViewsLock(
            opCtx,
            NamespaceString(m.ns.dbName(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);
        uassertStatusOK(catalog->reloadViews(opCtx, m.ns.dbName()));
    }
}
}  // namespace

TenantFileImporterService* TenantFileImporterService::get(ServiceContext* serviceContext) {
    return &_TenantFileImporterService(serviceContext);
}

void TenantFileImporterService::startMigration(const UUID& migrationId,
                                               const StringData& donorConnectionString) {
    stdx::lock_guard lk(_mutex);
    if (migrationId == _migrationId && _state >= State::kStarted && _state < State::kInterrupted) {
        return;
    }

    _reset(lk);
    _migrationId = migrationId;
    _donorConnectionString = donorConnectionString.toString();
    _eventQueue = std::make_shared<Queue>();
    _state = State::kStarted;

    _thread = std::make_unique<stdx::thread>([this, migrationId] {
        Client::initThread("TenantFileImporterService");
        LOGV2_INFO(6378904,
                   "TenantFileImporterService starting worker thread",
                   "migrationId"_attr = migrationId.toString());
        auto opCtx = cc().makeOperationContext();
        _handleEvents(opCtx.get());
    });
}

void TenantFileImporterService::learnedFilename(const UUID& migrationId,
                                                const BSONObj& metadataDoc) {
    stdx::lock_guard lk(_mutex);
    if (migrationId == _migrationId && _state >= State::kLearnedAllFilenames) {
        return;
    }

    tassert(8423347,
            "Called learnedFilename with migrationId {}, but {} is active"_format(
                migrationId.toString(), _migrationId ? _migrationId->toString() : "no migration"),
            migrationId == _migrationId);

    _state = State::kLearnedFilename;
    ImporterEvent event{ImporterEvent::Type::kLearnedFileName, migrationId};
    event.metadataDoc = metadataDoc.getOwned();
    invariant(_eventQueue);
    auto success = _eventQueue->tryPush(std::move(event));

    uassert(6378903,
            "TenantFileImporterService failed to push '{}' event without blocking"_format(
                stateToString(_state)),
            success);
}

void TenantFileImporterService::learnedAllFilenames(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    if (migrationId == _migrationId && _state >= State::kLearnedAllFilenames) {
        return;
    }

    tassert(8423345,
            "Called learnedAllFilenames with migrationId {}, but {} is active"_format(
                migrationId.toString(), _migrationId ? _migrationId->toString() : "no migration"),
            migrationId == _migrationId);

    _state = State::kLearnedAllFilenames;
    invariant(_eventQueue);
    auto success = _eventQueue->tryPush({ImporterEvent::Type::kLearnedAllFilenames, migrationId});
    uassert(6378902,
            "TenantFileImporterService failed to push '{}' event without blocking"_format(
                stateToString(_state)),
            success);
}

void TenantFileImporterService::interrupt(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    if (migrationId != _migrationId) {
        LOGV2_WARNING(
            6378901,
            "Called interrupt with migrationId {migrationId}, but {activeMigrationId} is active",
            "migrationId"_attr = migrationId.toString(),
            "activeMigrationId"_attr = _migrationId ? _migrationId->toString() : "no migration");
        return;
    }
    _interrupt(lk);
}

void TenantFileImporterService::interruptAll() {
    stdx::lock_guard lk(_mutex);
    if (!_migrationId) {
        return;
    }
    _interrupt(lk);
}

void TenantFileImporterService::_handleEvents(OperationContext* opCtx) {
    using eventType = ImporterEvent::Type;

    std::string donorConnectionString;
    boost::optional<UUID> migrationId;

    std::shared_ptr<Queue> eventQueueRef;
    {
        stdx::lock_guard lk(_mutex);
        invariant(_eventQueue);
        eventQueueRef = _eventQueue;
        donorConnectionString = _donorConnectionString;
        migrationId = _migrationId;
    }

    ImporterEvent event{eventType::kNone, UUID::gen()};
    while (true) {
        opCtx->checkForInterrupt();

        try {
            event = eventQueueRef->pop(opCtx);
        } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>& err) {
            LOGV2_WARNING(6378900, "Event queue was interrupted", "error"_attr = err);
            break;
        }

        // Out-of-order events for a different migration are not permitted.
        invariant(event.migrationId == migrationId);

        switch (event.type) {
            case eventType::kNone:
                continue;
            case eventType::kLearnedFileName:
                cloneFile(opCtx, event.metadataDoc);
                continue;
            case eventType::kLearnedAllFilenames:
                importCopiedFiles(opCtx, event.migrationId, donorConnectionString);
                _voteImportedFiles(opCtx);
                break;
        }
        break;
    }
}

void TenantFileImporterService::_voteImportedFiles(OperationContext* opCtx) {
    boost::optional<UUID> migrationId;
    {
        stdx::lock_guard lk(_mutex);
        migrationId = _migrationId;
    }
    invariant(migrationId);

    auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());

    RecipientVoteImportedFiles cmd(*migrationId, replCoord->getMyHostAndPort(), true /* success */);

    auto voteResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
        opCtx,
        NamespaceString::kAdminDb.toString(),
        cmd.toBSON({}),
        [](executor::TaskExecutor::CallbackHandle handle) {},
        [](executor::TaskExecutor::CallbackHandle handle) {});

    auto voteStatus = getStatusFromCommandResult(voteResponse);
    if (!voteStatus.isOK()) {
        LOGV2_WARNING(6113403,
                      "Failed to run recipientVoteImportedFiles command on primary",
                      "status"_attr = voteStatus);
        // TODO SERVER-64192: handle this case, retry, and/or throw error, etc.
    }
}

void TenantFileImporterService::_interrupt(WithLock) {
    if (_state == State::kInterrupted) {
        return;
    }

    // TODO SERVER-66150: interrupt the tenant file cloner by closing the dbClientConnnection via
    // shutdownAndDisallowReconnect() and shutting down the writer pool.
    if (_eventQueue) {
        _eventQueue->closeConsumerEnd();
    }

    {
        // TODO SERVER-66907: Uncomment op ctx interrupt logic.
        // OperationContext* ptr = _opCtx.get();
        // stdx::lock_guard<Client> lk(*ptr->getClient());
        // _opCtx->markKilled(ErrorCodes::Interrupted);
    }

    _state = State::kInterrupted;
}

void TenantFileImporterService::_reset(WithLock) {
    if (_migrationId) {
        LOGV2_INFO(6378905,
                   "TenantFileImporterService resetting migration",
                   "migrationId"_attr = _migrationId->toString());
        _migrationId.reset();
    }

    if (_thread && _thread->joinable()) {
        _thread->join();
        _thread.reset();
    }

    if (_eventQueue) {
        _eventQueue.reset();
    }

    // TODO SERVER-66907: how should we be resetting _opCtx?
    _state = State::kUninitialized;
}
}  // namespace mongo::repl
