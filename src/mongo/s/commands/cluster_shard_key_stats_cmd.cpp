/**
 * Shard key extraction performance statistics command (mongos only)
 *
 * Usage:
 *   db.runCommand({getShardKeyStats: 1})    // Get stats
 *   db.runCommand({resetShardKeyStats: 1})  // Reset stats
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
namespace {

class GetShardKeyStatsCommand : public Command {
public:
    GetShardKeyStatsCommand() : Command("getShardKeyStats") {}

    virtual bool slaveOk() const override { return true; }
    virtual bool adminOnly() const override { return false; }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override { return false; }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) override {}

    virtual void help(std::stringstream& help) const override {
        help << "Get shard key extraction performance statistics. "
             << "Shows fast path vs fallback (CanonicalQuery) timing.";
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {

        result.append("shardKeyExtraction", getShardKeyExtractionStats());
        return true;
    }
} getShardKeyStatsCmd;

class ResetShardKeyStatsCommand : public Command {
public:
    ResetShardKeyStatsCommand() : Command("resetShardKeyStats") {}

    virtual bool slaveOk() const override { return true; }
    virtual bool adminOnly() const override { return false; }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override { return false; }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) override {}

    virtual void help(std::stringstream& help) const override {
        help << "Reset shard key extraction performance statistics.";
    }

    bool run(OperationContext* txn,
             const std::string& dbname,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {

        resetShardKeyExtractionStats();
        result.append("reset", true);
        return true;
    }
} resetShardKeyStatsCmd;

}  // namespace
}  // namespace mongo
