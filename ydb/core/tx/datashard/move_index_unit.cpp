#include "datashard_impl.h"
#include "datashard_pipeline.h"
#include "execution_unit_ctors.h"

namespace NKikimr {
namespace NDataShard {

class TMoveIndexUnit : public TExecutionUnit {
public:
    TMoveIndexUnit(TDataShard& dataShard, TPipeline& pipeline)
        : TExecutionUnit(EExecutionUnitKind::MoveIndex, false, dataShard, pipeline)
    { }

    bool IsReadyToExecute(TOperation::TPtr) const override {
        return true;
    }

    void MoveChangeRecords(NIceDb::TNiceDb& db, const NKikimrTxDataShard::TMoveIndex& move, TVector<IDataShardChangeCollector::TChange>& changeRecords) {
        const auto remapPrevId = TPathId::FromProto(move.GetReMapIndex().GetSrcPathId());
        const auto remapNewId = TPathId::FromProto(move.GetReMapIndex().GetDstPathId());

        for (auto& record: changeRecords) {
            if (record.PathId == remapPrevId) {
                record.PathId = remapNewId;
                if (record.LockId) {
                    DataShard.MoveChangeRecord(db, record.LockId, record.LockOffset, record.PathId);
                } else {
                    DataShard.MoveChangeRecord(db, record.Order, record.PathId);
                }
            }
        }

        for (auto& pr : DataShard.GetLockChangeRecords()) {
            for (auto& record : pr.second.Changes) {
                if (record.PathId == remapPrevId) {
                    record.PathId = remapNewId;
                    DataShard.MoveChangeRecord(db, record.LockId, record.LockOffset, record.PathId);
                }
            }
        }
    }

    EExecutionStatus Execute(TOperation::TPtr op, TTransactionContext& txc, const TActorContext& ctx) override {
        Y_ENSURE(op->IsSchemeTx());

        TActiveTransaction* tx = dynamic_cast<TActiveTransaction*>(op.Get());
        Y_ENSURE(tx, "cannot cast operation of kind " << op->GetKind());

        if (tx->GetSchemeTxType() != TSchemaOperation::ETypeMoveIndex) {
            return EExecutionStatus::Executed;
        }

        const auto& schemeTx = tx->GetSchemeTx();
        if (!schemeTx.HasMoveIndex()) {
            return EExecutionStatus::Executed;
        }

        NIceDb::TNiceDb db(txc.DB);

        op->ChangeRecords().clear();

        auto changesQueue = DataShard.TakeChangesQueue();
        auto lockChangeRecords = DataShard.TakeLockChangeRecords();
        auto committedLockChangeRecords = DataShard.TakeCommittedLockChangeRecords();

        if (!DataShard.LoadChangeRecords(db, op->ChangeRecords())) {
            DataShard.SetChangesQueue(std::move(changesQueue));
            DataShard.SetLockChangeRecords(std::move(lockChangeRecords));
            DataShard.SetCommittedLockChangeRecords(std::move(committedLockChangeRecords));
            return EExecutionStatus::Restart;
        }

        if (!DataShard.LoadLockChangeRecords(db)) {
            DataShard.SetChangesQueue(std::move(changesQueue));
            DataShard.SetLockChangeRecords(std::move(lockChangeRecords));
            DataShard.SetCommittedLockChangeRecords(std::move(committedLockChangeRecords));
            return EExecutionStatus::Restart;
        }

        if (!DataShard.LoadChangeRecordCommits(db, op->ChangeRecords())) {
            DataShard.SetChangesQueue(std::move(changesQueue));
            DataShard.SetLockChangeRecords(std::move(lockChangeRecords));
            DataShard.SetCommittedLockChangeRecords(std::move(committedLockChangeRecords));
            return EExecutionStatus::Restart;
        }

        LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::TX_DATASHARD, "TMoveIndexUnit Execute"
            << ": schemeTx# " << schemeTx.DebugString()
            << ": changeRecords size# " << op->ChangeRecords().size()
            << ", at tablet# " << DataShard.TabletID());

        DataShard.SuspendChangeSender(ctx);

        const auto& params = schemeTx.GetMoveIndex();
        DataShard.MoveUserIndex(op, params, ctx, txc);
        MoveChangeRecords(db, params, op->ChangeRecords());

        BuildResult(op, NKikimrTxDataShard::TEvProposeTransactionResult::COMPLETE);
        op->Result()->SetStepOrderId(op->GetStepOrder().ToPair());

        return EExecutionStatus::DelayCompleteNoMoreRestarts;
    }

    void Complete(TOperation::TPtr op, const TActorContext& ctx) override {
        DataShard.CreateChangeSender(ctx);
        DataShard.MaybeActivateChangeSender(ctx);
        DataShard.EnqueueChangeRecords(std::move(op->ChangeRecords()), 0, true);
    }
};

THolder<TExecutionUnit> CreateMoveIndexUnit(TDataShard& dataShard, TPipeline& pipeline) {
    return THolder(new TMoveIndexUnit(dataShard, pipeline));
}

} // namespace NDataShard
} // namespace NKikimr
