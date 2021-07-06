/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/pipeline/document_source_change_stream.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/change_stream_helpers_legacy.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_close_cursor.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_lookup_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_lookup_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transactions.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/vector_clock.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

// The $changeStream stage is an alias for many stages, but we need to be able to serialize
// and re-parse the pipeline. To make this work, the 'transformation' stage will serialize itself
// with the original specification, and all other stages that are created during the alias expansion
// will not serialize themselves.
REGISTER_DOCUMENT_SOURCE(changeStream,
                         DocumentSourceChangeStream::LiteParsed::parse,
                         DocumentSourceChangeStream::createFromBson,
                         AllowedWithApiStrict::kAlways);

constexpr StringData DocumentSourceChangeStream::kDocumentKeyField;
constexpr StringData DocumentSourceChangeStream::kFullDocumentBeforeChangeField;
constexpr StringData DocumentSourceChangeStream::kFullDocumentField;
constexpr StringData DocumentSourceChangeStream::kIdField;
constexpr StringData DocumentSourceChangeStream::kNamespaceField;
constexpr StringData DocumentSourceChangeStream::kUuidField;
constexpr StringData DocumentSourceChangeStream::kUpdateDescriptionField;
constexpr StringData DocumentSourceChangeStream::kOperationTypeField;
constexpr StringData DocumentSourceChangeStream::kStageName;
constexpr StringData DocumentSourceChangeStream::kClusterTimeField;
constexpr StringData DocumentSourceChangeStream::kTxnNumberField;
constexpr StringData DocumentSourceChangeStream::kLsidField;
constexpr StringData DocumentSourceChangeStream::kRenameTargetNssField;
constexpr StringData DocumentSourceChangeStream::kUpdateOpType;
constexpr StringData DocumentSourceChangeStream::kDeleteOpType;
constexpr StringData DocumentSourceChangeStream::kReplaceOpType;
constexpr StringData DocumentSourceChangeStream::kInsertOpType;
constexpr StringData DocumentSourceChangeStream::kDropCollectionOpType;
constexpr StringData DocumentSourceChangeStream::kRenameCollectionOpType;
constexpr StringData DocumentSourceChangeStream::kDropDatabaseOpType;
constexpr StringData DocumentSourceChangeStream::kInvalidateOpType;
constexpr StringData DocumentSourceChangeStream::kNewShardDetectedOpType;

constexpr StringData DocumentSourceChangeStream::kRegexAllCollections;
constexpr StringData DocumentSourceChangeStream::kRegexAllDBs;
constexpr StringData DocumentSourceChangeStream::kRegexCmdColl;

void DocumentSourceChangeStream::checkValueType(const Value v,
                                                const StringData filedName,
                                                BSONType expectedType) {
    uassert(40532,
            str::stream() << "Entry field \"" << filedName << "\" should be "
                          << typeName(expectedType) << ", found: " << typeName(v.getType()),
            (v.getType() == expectedType));
}

DocumentSourceChangeStream::ChangeStreamType DocumentSourceChangeStream::getChangeStreamType(
    const NamespaceString& nss) {

    // If we have been permitted to run on admin, 'allChangesForCluster' must be true.
    return (nss.isAdminDB()
                ? ChangeStreamType::kAllChangesForCluster
                : (nss.isCollectionlessAggregateNS() ? ChangeStreamType::kSingleDatabase
                                                     : ChangeStreamType::kSingleCollection));
}

std::string DocumentSourceChangeStream::getNsRegexForChangeStream(const NamespaceString& nss) {
    auto regexEscape = [](const std::string& source) {
        std::string result = "";
        std::string escapes = "*+|()^?[]./\\$";
        for (const char& c : source) {
            if (escapes.find(c) != std::string::npos) {
                result.append("\\");
            }
            result += c;
        }
        return result;
    };

    auto type = getChangeStreamType(nss);
    switch (type) {
        case ChangeStreamType::kSingleCollection:
            // Match the target namespace exactly.
            return "^" + regexEscape(nss.ns()) + "$";
        case ChangeStreamType::kSingleDatabase:
            // Match all namespaces that start with db name, followed by ".", then NOT followed by
            // '$' or 'system.'
            return "^" + regexEscape(nss.db().toString()) + "\\." + kRegexAllCollections;
        case ChangeStreamType::kAllChangesForCluster:
            // Match all namespaces that start with any db name other than admin, config, or local,
            // followed by ".", then NOT followed by '$' or 'system.'.
            return kRegexAllDBs + "\\." + kRegexAllCollections;
        default:
            MONGO_UNREACHABLE;
    }
}

ResumeTokenData DocumentSourceChangeStream::resolveResumeTokenFromSpec(
    const DocumentSourceChangeStreamSpec& spec) {
    if (spec.getStartAfter()) {
        return spec.getStartAfter()->getData();
    } else if (spec.getResumeAfter()) {
        return spec.getResumeAfter()->getData();
    } else if (spec.getStartAtOperationTime()) {
        return ResumeToken::makeHighWaterMarkToken(*spec.getStartAtOperationTime()).getData();
    }
    tasserted(5666901,
              "Expected one of 'startAfter', 'resumeAfter' or 'startAtOperationTime' to be "
              "populated in $changeStream spec");
}

Timestamp DocumentSourceChangeStream::getStartTimeForNewStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // If we do not have an explicit starting point, we should start from the latest majority
    // committed operation. If we are on mongoS and do not have a starting point, set it to the
    // current clusterTime so that all shards start in sync.
    auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
    const auto currentTime =
        !expCtx->inMongos ? LogicalTime{replCoord->getMyLastAppliedOpTime().getTimestamp()} : [&] {
            const auto currentTime = VectorClock::get(expCtx->opCtx)->getTime();
            return currentTime.clusterTime();
        }();

    // We always start one tick beyond the most recent operation, to ensure that the stream does not
    // return it.
    return currentTime.addTicks(1).asTimestamp();
}

list<intrusive_ptr<DocumentSource>> DocumentSourceChangeStream::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(50808,
            "$changeStream stage expects a document as argument",
            elem.type() == BSONType::Object);

    auto spec = DocumentSourceChangeStreamSpec::parse(IDLParserErrorContext("$changeStream"),
                                                      elem.embeddedObject());

    // Make sure that it is legal to run this $changeStream before proceeding.
    DocumentSourceChangeStream::assertIsLegalSpecification(expCtx, spec);

    // If we see this stage on a shard, it means that the raw $changeStream stage was dispatched to
    // us from an old mongoS. Build a legacy shard pipeline.
    if (expCtx->needsMerge ||
        !feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()) {
        return change_stream_legacy::buildPipeline(expCtx, spec);
    }
    return _buildPipeline(expCtx, spec);
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceChangeStream::_buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceChangeStreamSpec spec) {
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    // If the user did not specify an explicit starting point, set it to the current time.
    if (!spec.getResumeAfter() && !spec.getStartAfter() && !spec.getStartAtOperationTime()) {
        // Make sure we update the 'startAtOperationTime' in the 'spec' so that we serialize the
        // correct start point when sending it to the shards.
        spec.setStartAtOperationTime(DocumentSourceChangeStream::getStartTimeForNewStream(expCtx));
    }

    // Obtain the resume token from the spec. This will be used when building the pipeline.
    auto resumeToken = DocumentSourceChangeStream::resolveResumeTokenFromSpec(spec);

    // Unfold the $changeStream into its constituent stages and add them to the pipeline.
    stages.push_back(DocumentSourceChangeStreamOplogMatch::create(expCtx, spec));
    stages.push_back(DocumentSourceChangeStreamUnwindTransaction::create(expCtx));
    stages.push_back(DocumentSourceChangeStreamTransform::create(expCtx, spec));
    tassert(5666900,
            "'DocumentSourceChangeStreamTransform' stage should populate "
            "'initialPostBatchResumeToken' field",
            !expCtx->initialPostBatchResumeToken.isEmpty());

    // The resume stage must come after the check invalidate stage so that the former can determine
    // whether the event that matches the resume token should be followed by an "invalidate" event.
    stages.push_back(DocumentSourceChangeStreamCheckInvalidate::create(expCtx, spec));

    // If the starting point is a high water mark, or if we will be splitting the pipeline for
    // dispatch to the shards in a cluster, we must include a DSCSCheckResumability stage.
    if (expCtx->inMongos || ResumeToken::isHighWaterMarkToken(resumeToken)) {
        stages.push_back(DocumentSourceChangeStreamCheckResumability::create(expCtx, spec));
    }

    // If the pipeline is built on MongoS, we check for topology change events here. If a topology
    // change event is detected, this stage forwards the event directly to the executor via an
    // exception (bypassing the rest of the pipeline). MongoS must see all topology change events,
    // so it's important that this stage occurs before any filtering is performed.
    if (expCtx->inMongos) {
        stages.push_back(DocumentSourceChangeStreamCheckTopologyChange::create(expCtx));
    }

    // If 'fullDocument' is set to "updateLookup", add the DSCSAddPostImage stage here.
    if (spec.getFullDocument() == FullDocumentModeEnum::kUpdateLookup) {
        stages.push_back(DocumentSourceChangeStreamAddPostImage::create(expCtx));
    }

    // If the pipeline is built on MongoS, then the DSCSHandleTopologyChange stage acts as the
    // split point for the pipline. All stages before this stages will run on shards and all
    // stages after and inclusive of this stage will run on the MongoS.
    if (expCtx->inMongos) {
        stages.push_back(DocumentSourceChangeStreamHandleTopologyChange::create(expCtx));
    }

    // If the resume point is an event, we must include a DSCSEnsureResumeTokenPresent stage.
    if (!ResumeToken::isHighWaterMarkToken(resumeToken)) {
        stages.push_back(DocumentSourceChangeStreamEnsureResumeTokenPresent::create(expCtx, spec));
    }

    // We only create a pre-image lookup stage on a non-merging mongoD. We place this stage here
    // so that any $match stages which follow the $changeStream pipeline prefix may be able to
    // skip ahead of the DSCSAddPreImage stage. This allows a whole-db or whole-cluster stream to
    // run on an instance where only some collections have pre-images enabled, so long as the
    // user filters for only those namespaces.
    // TODO SERVER-36941: figure out how to get this to work in a sharded cluster.
    if (spec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff) {
        invariant(!expCtx->inMongos);
        stages.push_back(DocumentSourceChangeStreamAddPreImage::create(expCtx, spec));
    }

    return stages;
}

void DocumentSourceChangeStream::assertIsLegalSpecification(
    const intrusive_ptr<ExpressionContext>& expCtx, const DocumentSourceChangeStreamSpec& spec) {
    // We can only run on a replica set, or through mongoS. Confirm that this is the case.
    auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
    uassert(
        40573,
        "The $changeStream stage is only supported on replica sets",
        expCtx->inMongos ||
            (replCoord &&
             replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet));

    // If 'allChangesForCluster' is true, the stream must be opened on the 'admin' database with
    // {aggregate: 1}.
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "A $changeStream with 'allChangesForCluster:true' may only be opened "
                             "on the 'admin' database, and with no collection name; found "
                          << expCtx->ns.ns(),
            !spec.getAllChangesForCluster() ||
                (expCtx->ns.isAdminDB() && expCtx->ns.isCollectionlessAggregateNS()));

    // Prevent $changeStream from running on internal databases. A stream may run against the
    // 'admin' database iff 'allChangesForCluster' is true. A stream may run against the 'config'
    // database iff 'allowToRunOnConfigDB' is true.
    const bool isNotBannedInternalDB =
        !expCtx->ns.isLocal() && (!expCtx->ns.isConfigDB() || spec.getAllowToRunOnConfigDB());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.db()
                          << " database",
            expCtx->ns.isAdminDB() ? static_cast<bool>(spec.getAllChangesForCluster())
                                   : isNotBannedInternalDB);

    // Prevent $changeStream from running on internal collections in any database. A stream may run
    // against the internal collections iff 'allowToRunOnSystemNS' is true and the stream is not
    // opened through a mongos process.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "$changeStream may not be opened on the internal " << expCtx->ns.ns()
                          << " collection"
                          << (spec.getAllowToRunOnSystemNS() ? " through mongos" : ""),
            !expCtx->ns.isSystem() || (spec.getAllowToRunOnSystemNS() && !expCtx->inMongos));

    // TODO SERVER-36941: We do not currently support sharded pre-image lookup.
    const bool shouldAddPreImage =
        (spec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff);
    uassert(51771,
            "the 'fullDocumentBeforeChange' option is not supported in a sharded cluster",
            !(shouldAddPreImage && (expCtx->inMongos || expCtx->needsMerge)));

    uassert(31123,
            "Change streams from mongos may not show migration events",
            !(expCtx->inMongos && spec.getShowMigrationEvents()));

    uassert(50865,
            "Do not specify both 'resumeAfter' and 'startAfter' in a $changeStream stage",
            !spec.getResumeAfter() || !spec.getStartAfter());

    auto resumeToken = (spec.getResumeAfter() || spec.getStartAfter())
        ? resolveResumeTokenFromSpec(spec)
        : boost::optional<ResumeTokenData>();

    uassert(40674,
            "Only one type of resume option is allowed, but multiple were found",
            !(spec.getStartAtOperationTime() && resumeToken));

    uassert(ErrorCodes::InvalidResumeToken,
            "Attempting to resume a change stream using 'resumeAfter' is not allowed from an "
            "invalidate notification",
            !(spec.getResumeAfter() && resumeToken->fromInvalidate));

    // If we are resuming a single-collection stream, the resume token should always contain a
    // UUID unless the token is a high water mark.
    uassert(ErrorCodes::InvalidResumeToken,
            "Attempted to resume a single-collection stream, but the resume token does not "
            "include a UUID",
            !resumeToken || resumeToken->uuid || !expCtx->isSingleNamespaceAggregation() ||
                ResumeToken::isHighWaterMarkToken(*resumeToken));
}

}  // namespace mongo
