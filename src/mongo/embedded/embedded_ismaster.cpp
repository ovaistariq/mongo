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

#include "mongo/platform/basic.h"

#include "mongo/db/audit.h"
#include "mongo/db/commands.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/wire_version.h"
#include "mongo/rpc/metadata/client_metadata.h"

namespace mongo {
namespace {

class CmdIsMaster : public BasicCommand {
public:
    bool requiresAuth() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ isMaster : 1 }";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required

    CmdIsMaster() : BasicCommand("isMaster", "ismaster") {}

    virtual bool run(OperationContext* opCtx,
                     const std::string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {

        auto wireSpec = WireSpec::instance().get();

        ClientMetadata::tryFinalize(opCtx->getClient());
        audit::logClientMetadata(opCtx->getClient());

        result.appendBool("ismaster", true);

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", static_cast<long long>(MaxMessageSizeBytes));
        result.appendNumber("maxWriteBatchSize",
                            static_cast<long long>(write_ops::kMaxWriteBatchSize));
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);
        result.appendNumber("connectionId", opCtx->getClient()->getConnectionId());

        result.append("minWireVersion", wireSpec->incomingExternalClient.minWireVersion);
        result.append("maxWireVersion", wireSpec->incomingExternalClient.maxWireVersion);

        result.append("readOnly", storageGlobalParams.readOnly);

        return true;
    }
} CmdIsMaster;

}  // namespace
}  // namespace mongo
