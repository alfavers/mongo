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

#include "mongo/s/catalog/type_config_version.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

const NamespaceString VersionType::ConfigNS(NamespaceString::kConfigVersionNamespace);

const BSONField<int> VersionType::minCompatibleVersion("minCompatibleVersion");
const BSONField<int> VersionType::currentVersion("currentVersion");
const BSONField<OID> VersionType::clusterId("clusterId");

void VersionType::clear() {
    _minCompatibleVersion.reset();
    _currentVersion.reset();
    _clusterId = OID{};
}

void VersionType::cloneTo(VersionType* other) const {
    other->clear();

    other->_minCompatibleVersion = _minCompatibleVersion;
    other->_currentVersion = _currentVersion;
    other->_clusterId = _clusterId;
}

Status VersionType::validate() const {
    return Status::OK();
}

BSONObj VersionType::toBSON() const {
    BSONObjBuilder builder;

    builder.append("_id", 1);
    builder.append(clusterId.name(), getClusterId());
    if (_minCompatibleVersion)
        builder.append(minCompatibleVersion.name(), _minCompatibleVersion.get());
    if (_currentVersion)
        builder.append(currentVersion.name(), _currentVersion.get());

    return builder.obj();
}

StatusWith<VersionType> VersionType::fromBSON(const BSONObj& source) {
    VersionType version;

    {
        long long vMinCompatibleVersion;
        Status status =
            bsonExtractIntegerField(source, minCompatibleVersion.name(), &vMinCompatibleVersion);
        if (status == ErrorCodes::NoSuchKey) {
            // skip optional field
        } else if (!status.isOK()) {
            return status;
        } else {
            version._minCompatibleVersion = vMinCompatibleVersion;
        }
    }

    {
        long long vCurrentVersion;
        Status status = bsonExtractIntegerField(source, currentVersion.name(), &vCurrentVersion);
        if (status == ErrorCodes::NoSuchKey) {
            // skip optional field
        } else if (!status.isOK()) {
            return status;
        } else {
            version._currentVersion = vCurrentVersion;
        }
    }

    {
        BSONElement vClusterIdElem;
        Status status =
            bsonExtractTypedField(source, clusterId.name(), BSONType::jstOID, &vClusterIdElem);
        if (!status.isOK())
            return status;
        version._clusterId = vClusterIdElem.OID();
    }

    return version;
}

void VersionType::setMinCompatibleVersion(boost::optional<int> minCompatibleVersion) {
    _minCompatibleVersion = minCompatibleVersion;
}

void VersionType::setCurrentVersion(boost::optional<int> currentVersion) {
    _currentVersion = currentVersion;
}

void VersionType::setClusterId(const OID& clusterId) {
    _clusterId = clusterId;
}

std::string VersionType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
