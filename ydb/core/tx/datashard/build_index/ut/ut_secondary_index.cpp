#include "defs.h"
#include "ut_helpers.h"
#include <ydb/core/tx/datashard/ut_common/datashard_ut_common.h>
#include "datashard_ut_common_kqp.h"

#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/tx_proxy/upload_rows.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/protos/index_builder.pb.h>

namespace NKikimr {

using namespace NKikimr::NDataShard::NKqpHelpers;
using namespace NSchemeShard;
using namespace Tests;

static const TString kMainTable = "/Root/table-1";
static const TString kIndexTable = "/Root/table-2";

static void DoBadRequest(Tests::TServer::TPtr server, TActorId sender,
    std::function<void(NKikimrTxDataShard::TEvBuildIndexCreateRequest&)> setupRequest,
    TString expectedError, bool expectedErrorSubstring = false)
{
    auto snapshot = CreateVolatileSnapshot(server, {kMainTable});
    TVector<ui64> datashards = GetTableShards(server, sender, kMainTable);
    TTableId tableId = ResolveTableId(server, sender, kMainTable);

    TStringBuilder data;
    TString err;
    UNIT_ASSERT(datashards.size() == 1);

    auto ev = std::make_unique<TEvDataShard::TEvBuildIndexCreateRequest>();
    NKikimrTxDataShard::TEvBuildIndexCreateRequest& rec = ev->Record;
    rec.SetId(1);

    rec.SetTabletId(datashards[0]);
    rec.SetOwnerId(tableId.PathId.OwnerId);
    rec.SetPathId(tableId.PathId.LocalPathId);

    rec.SetTargetName(kIndexTable);
    rec.AddIndexColumns("value");
    rec.AddIndexColumns("key");

    rec.SetSnapshotTxId(snapshot.TxId);
    rec.SetSnapshotStep(snapshot.Step);

    setupRequest(rec);

    DoBadRequest<TEvDataShard::TEvBuildIndexProgressResponse>(server, sender, std::move(ev), datashards[0], expectedError, expectedErrorSubstring);
}

Y_UNIT_TEST_SUITE(TTxDataShardBuildIndexScan) {
    static void DoBuildIndex(Tests::TServer::TPtr server, TActorId sender,
                             const TString& tableFrom, const TString& tableTo,
                             const TRowVersion& snapshot,
                             const NKikimrIndexBuilder::EBuildStatus& expected) {
        auto &runtime = *server->GetRuntime();
        TVector<ui64> datashards = GetTableShards(server, sender, tableFrom);
        TTableId tableId = ResolveTableId(server, sender, tableFrom);

        for (auto tid: datashards) {
            auto ev = new TEvDataShard::TEvBuildIndexCreateRequest;
            NKikimrTxDataShard::TEvBuildIndexCreateRequest& rec = ev->Record;
            rec.SetId(1);

            rec.SetTabletId(tid);
            rec.SetOwnerId(tableId.PathId.OwnerId);
            rec.SetPathId(tableId.PathId.LocalPathId);

            rec.SetTargetName(tableTo);
            rec.AddIndexColumns("value");
            rec.AddIndexColumns("key");

            rec.SetSnapshotTxId(snapshot.TxId);
            rec.SetSnapshotStep(snapshot.Step);

            runtime.SendToPipe(tid, sender, ev, 0, GetPipeConfigWithRetries());

            while (true) {
                TAutoPtr<IEventHandle> handle;
                auto reply = runtime.GrabEdgeEventRethrow<TEvDataShard::TEvBuildIndexProgressResponse>(handle);

                if (expected == NKikimrIndexBuilder::EBuildStatus::DONE
                    && reply->Record.GetStatus() == NKikimrIndexBuilder::EBuildStatus::ACCEPTED) {
                    Cerr << "skip ACCEPTED" << Endl;
                    continue;
                }

                if (expected != NKikimrIndexBuilder::EBuildStatus::IN_PROGRESS
                    && reply->Record.GetStatus() == NKikimrIndexBuilder::EBuildStatus::IN_PROGRESS) {
                    Cerr << "skip INPROGRESS" << Endl;
                    continue;
                }

                NYql::TIssues issues;
                NYql::IssuesFromMessage(reply->Record.GetIssues(), issues);
                UNIT_ASSERT_VALUES_EQUAL_C(reply->Record.GetStatus(), expected, issues.ToString());
                break;
            }
        }
    }

    static void CreateShardedTableForIndex(
        Tests::TServer::TPtr server, TActorId sender,
        const TString &root, const TString &name,
        ui64 shards, bool enableOutOfOrder)
    {
        NLocalDb::TCompactionPolicyPtr policy = NLocalDb::CreateDefaultUserTablePolicy();
        policy->KeepEraseMarkers = true;

        auto opts = TShardedTableOptions()
                        .Shards(shards)
                        .EnableOutOfOrder(enableOutOfOrder)
                        .Policy(policy.Get())
                        .ShadowData(EShadowDataMode::Enabled)
                        .Columns({{"value", "Uint32", true, false}, {"key", "Uint32", true, false}});

        CreateShardedTable(server, sender, root, name, opts);
    }

    Y_UNIT_TEST(BadRequest) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        // Allow manipulating shadow data using normal schemeshard operations
        runtime.GetAppData().AllowShadowDataInSchemeShardForTests = true;

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1, false);

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.SetTabletId(0);
        }, TStringBuilder() << "{ <main>: Error: Wrong shard 0 this is " << GetTableShards(server, sender, kMainTable)[0] << " }");
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.SetPathId(0);
        }, "{ <main>: Error: Unknown table id: 0 }");

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.SetSnapshotStep(request.GetSnapshotStep() + 1);
        }, "Error: Unknown snapshot", true);
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.SetSnapshotTxId(request.GetSnapshotTxId() + 1);
        }, "Error: Unknown snapshot", true);

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.ClearTargetName();
        }, "{ <main>: Error: Empty target table name }");
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.SetTargetName("");
        }, "{ <main>: Error: Empty target table name }");

        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.AddIndexColumns("some");
        }, "{ <main>: Error: Unknown index column: some }");
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.AddDataColumns("some");
        }, "{ <main>: Error: Unknown data column: some }");

        // test multiple issues:
        DoBadRequest(server, sender, [](NKikimrTxDataShard::TEvBuildIndexCreateRequest& request) {
            request.AddIndexColumns("some1");
            request.AddDataColumns("some2");
        }, "[ { <main>: Error: Unknown index column: some1 } { <main>: Error: Unknown data column: some2 } ]");
    }

    Y_UNIT_TEST(RunScan) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetUseRealThreads(false);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        // Allow manipulating shadow data using normal schemeshard operations
        runtime.GetAppData().AllowShadowDataInSchemeShardForTests = true;

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1, false);

        // Upsert some initial values
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 100), (3, 300), (5, 500);");

        CreateShardedTableForIndex(server, sender, "/Root", "table-2", 1, false);

        auto snapshot = CreateVolatileSnapshot(server, { kMainTable });

        DoBuildIndex(server, sender, kMainTable, kIndexTable, snapshot, NKikimrIndexBuilder::EBuildStatus::DONE);

        // Writes to shadow data should not be visible yet
        auto data = ReadShardedTable(server, kIndexTable);
        UNIT_ASSERT_VALUES_EQUAL(data, "");

        // Alter table: disable shadow data and change compaction policy
        auto policy = NLocalDb::CreateDefaultUserTablePolicy();
        policy->KeepEraseMarkers = false;
        WaitTxNotification(server, AsyncAlterAndDisableShadow(server, "/Root", "table-2", policy.Get()));

        // Shadow data must be visible now
        auto data2 = ReadShardedTable(server, kIndexTable);
        UNIT_ASSERT_VALUES_EQUAL(data2,
                                 "value = 100, key = 1\n"
                                 "value = 300, key = 3\n"
                                 "value = 500, key = 5\n");
    }

    Y_UNIT_TEST(ShadowBorrowCompaction) {
        TPortManager pm;
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings
            .SetDomainName("Root")
            .SetUseRealThreads(false);

        Tests::TServer::TPtr server = new TServer(serverSettings);
        auto &runtime = *server->GetRuntime();
        auto sender = runtime.AllocateEdgeActor();

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::TABLET_EXECUTOR, NLog::PRI_DEBUG);

        // Allow manipulating shadow data using normal schemeshard operations
        runtime.GetAppData().AllowShadowDataInSchemeShardForTests = true;

        InitRoot(server, sender);

        CreateShardedTable(server, sender, "/Root", "table-1", 1, false);

        // Upsert some initial values
        ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 100), (2, 200), (3, 300), (4, 400), (5, 500);");

        CreateShardedTableForIndex(server, sender, "/Root", "table-2", 1, false);

        TBlockEvents<TEvDataShard::TEvCompactBorrowed> block(runtime, [&](const TEvDataShard::TEvCompactBorrowed::TPtr& event) {
            return runtime.FindActorName(event->Sender) == "FLAT_SCHEMESHARD_ACTOR";
        });

        auto snapshot = CreateVolatileSnapshot(server, { kMainTable });

        DoBuildIndex(server, sender, kMainTable, kIndexTable, snapshot, NKikimrIndexBuilder::EBuildStatus::DONE);

        // Writes to shadow data should not be visible yet
        auto data = ReadShardedTable(server, kIndexTable);
        UNIT_ASSERT_VALUES_EQUAL(data, "");

        // Split index
        auto shards1 = GetTableShards(server, sender, kIndexTable);
        UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);

        // Split would fail otherwise :(
        SetSplitMergePartCountLimit(server->GetRuntime(), -1);

        auto senderSplit = runtime.AllocateEdgeActor();
        ui64 txId = AsyncSplitTable(server, senderSplit, kIndexTable, shards1.at(0), 300);
        WaitTxNotification(server, senderSplit, txId);

        auto shards2 = GetTableShards(server, sender, kIndexTable);
        UNIT_ASSERT_VALUES_EQUAL(shards2.size(), 2u);

        for (auto shardIndex : xrange(2u)) {
            auto stats = WaitTableStats(runtime, shards2.at(shardIndex));
            // Cerr << "Received shard stats:" << Endl << stats.DebugString() << Endl;

            UNIT_ASSERT_VALUES_EQUAL(stats.GetTableStats().GetRowCount(), shardIndex == 0 ? 2 : 3);

            THashSet<ui64> owners(stats.GetUserTablePartOwners().begin(), stats.GetUserTablePartOwners().end());
            // Note: datashard always adds current shard to part owners, even if there are no parts
            UNIT_ASSERT_VALUES_EQUAL(owners, (THashSet<ui64>{shards1.at(0), shards2.at(shardIndex)}));

            auto tableId = ResolveTableId(server, sender, kIndexTable);
            auto result = CompactBorrowed(runtime, shards2.at(shardIndex), tableId);
            // Cerr << "Compact result " << result.DebugString() << Endl;
            UNIT_ASSERT_VALUES_EQUAL(result.GetTabletId(), shards2.at(shardIndex));
            UNIT_ASSERT_VALUES_EQUAL(result.GetPathId().GetOwnerId(), tableId.PathId.OwnerId);
            UNIT_ASSERT_VALUES_EQUAL(result.GetPathId().GetLocalId(), tableId.PathId.LocalPathId);

            for (int i = 0; i < 5 && (owners.size() > 1 || owners.contains(shards1.at(0))); ++i) {
                auto stats = WaitTableStats(runtime, shards2.at(shardIndex));
                owners = THashSet<ui64>(stats.GetUserTablePartOwners().begin(), stats.GetUserTablePartOwners().end());
            }

            UNIT_ASSERT_VALUES_EQUAL(owners, (THashSet<ui64>{shards2.at(shardIndex)}));
        }

        // Alter table: disable shadow data and change compaction policy
        auto policy = NLocalDb::CreateDefaultUserTablePolicy();
        policy->KeepEraseMarkers = false;
        WaitTxNotification(server, AsyncAlterAndDisableShadow(server, "/Root", "table-2", policy.Get()));

        // Shadow data must be visible now
        auto data2 = ReadShardedTable(server, kIndexTable);
        UNIT_ASSERT_VALUES_EQUAL(data2,
                                 "value = 100, key = 1\n"
                                 "value = 200, key = 2\n"
                                 "value = 300, key = 3\n"
                                 "value = 400, key = 4\n"
                                 "value = 500, key = 5\n");
    }
}

} // namespace NKikimr
