#pragma once
#include <ydb/core/formats/arrow/common/adapter.h>
#include <ydb/core/formats/arrow/reader/position.h>

#include <ydb/library/actors/core/log.h>
#include <ydb/library/formats/arrow/hash/xx_hash.h>
#include <ydb/library/formats/arrow/validation/validation.h>
#include <ydb/library/services/services.pb.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/array/array_base.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/record_batch.h>
#include <util/generic/string.h>
#include <util/string/join.h>
#include <util/system/types.h>

#include <optional>
#include <vector>

namespace NKikimr::NArrow::NHash {

class TXX64 {
public:
    enum class ENoColumnPolicy {
        Ignore,
        Verify,
        ReturnEmpty
    };

private:
    ui64 Seed = 0;
    const std::vector<TString> ColumnNames;
    const ENoColumnPolicy NoColumnPolicy;

    template <class TDataContainer>
    std::vector<std::shared_ptr<typename NAdapter::TDataBuilderPolicy<TDataContainer>::TColumn>> GetColumns(
        const std::shared_ptr<TDataContainer>& batch) const {
        std::vector<std::shared_ptr<typename NAdapter::TDataBuilderPolicy<TDataContainer>::TColumn>> columns;
        columns.reserve(ColumnNames.size());
        for (auto& colName : ColumnNames) {
            auto array = batch->GetColumnByName(colName);
            if (!array) {
                switch (NoColumnPolicy) {
                    case ENoColumnPolicy::Ignore:
                        break;
                    case ENoColumnPolicy::Verify:
                        AFL_VERIFY(false)("reason", "no_column")("column_name", colName);
                    case ENoColumnPolicy::ReturnEmpty:
                        return {};
                }
            } else {
                columns.emplace_back(array);
            }
        }
        if (columns.empty()) {
            AFL_WARN(NKikimrServices::ARROW_HELPER)("event", "cannot_read_all_columns")("reason", "fields_not_found")(
                "field_names", JoinSeq(",", ColumnNames))("batch_fields", JoinSeq(",", batch->schema()->field_names()));
        }
        return columns;
    }

public:
    TXX64(const std::vector<TString>& columnNames, const ENoColumnPolicy noColumnPolicy, const ui64 seed = 0);
    TXX64(const std::vector<std::string>& columnNames, const ENoColumnPolicy noColumnPolicy, const ui64 seed = 0);

    const std::vector<TString>& GetColumnNames() const {
        return ColumnNames;
    }

    template <class TAction>
    static void CalcForAll(const std::shared_ptr<arrow::Array>& array, const ui64 seed, const TAction& action) {
        AFL_VERIFY(array);
        NArrow::SwitchType(array->type_id(), [&](const auto& type) {
            using TWrap = std::decay_t<decltype(type)>;
            using T = typename TWrap::T;
            using TArray = typename arrow::TypeTraits<T>::ArrayType;
            for (ui32 idx = 0; idx < array->length(); ++idx) {
                if (array->IsNull(idx)) {
                    action(CalcSimple(Default<TString>().data(), 0, seed), idx);
                    continue;
                }
                auto& typedArray = static_cast<const TArray&>(*array);
                auto value = typedArray.GetView(idx);
                if constexpr (arrow::has_string_view<T>()) {
                    action(CalcSimple(value.data(), value.size(), seed), idx);
                } else if constexpr (arrow::has_c_type<T>()) {
                    action(CalcSimple(&value, sizeof(value), seed), idx);
                } else {
                    static_assert(arrow::is_decimal_type<T>());
                    AFL_VERIFY(false);
                }
            }
            return true;
        });
    }

    static ui64 CalcSimple(const std::string_view data, const ui64 seed);
    static ui64 CalcSimple(const void* data, const ui32 dataSize, const ui64 seed);
    static ui64 CalcForScalar(const std::shared_ptr<arrow::Scalar>& scalar, const ui64 seed);
    static void AppendField(const std::shared_ptr<arrow::Array>& array, const int row, NXX64::TStreamStringHashCalcer& hashCalcer);
    static void AppendField(const std::shared_ptr<arrow::Scalar>& scalar, NXX64::TStreamStringHashCalcer& hashCalcer);
    static void AppendField(const std::shared_ptr<arrow::Array>& array, const int row, NXX64::TStreamStringHashCalcer_H3& hashCalcer);
    static void AppendField(const std::shared_ptr<arrow::Scalar>& scalar, NXX64::TStreamStringHashCalcer_H3& hashCalcer);
    static ui64 CalcHash(const std::shared_ptr<arrow::Scalar>& scalar);
    std::optional<std::vector<ui64>> Execute(const std::shared_ptr<arrow::RecordBatch>& batch) const;

    template <class TDataContainer, class TAcceptor>
    [[nodiscard]] bool ExecuteToArrayImpl(const std::shared_ptr<TDataContainer>& batch, const TAcceptor& acceptor) const {
        std::vector<std::shared_ptr<typename NAdapter::TDataBuilderPolicy<TDataContainer>::TColumn>> columns = GetColumns(batch);
        if (columns.empty()) {
            return false;
        }

        std::vector<NAccessor::IChunkedArray::TReader> columnScanners;
        for (auto&& i : columns) {
            columnScanners.emplace_back(
                NAccessor::IChunkedArray::TReader(std::make_shared<typename NAdapter::TDataBuilderPolicy<TDataContainer>::TAccessor>(i)));
        }

        {
            NXX64::TStreamStringHashCalcer hashCalcer(Seed);
            for (int row = 0; row < batch->num_rows(); ++row) {
                hashCalcer.Start();
                for (auto& column : columnScanners) {
                    auto address = column.GetReadChunk(row);
                    AppendField(address.GetArray(), address.GetPosition(), hashCalcer);
                }
                acceptor(hashCalcer.Finish());
            }
        }
        return true;
    }

    template <class TDataContainer>
    std::shared_ptr<arrow::Array> ExecuteToArray(const std::shared_ptr<TDataContainer>& batch) const {
        auto builder = NArrow::MakeBuilder(arrow::TypeTraits<arrow::UInt64Type>::type_singleton());
        auto& intBuilder = static_cast<arrow::UInt64Builder&>(*builder);
        TStatusValidator::Validate(intBuilder.Reserve(batch->num_rows()));

        const auto acceptor = [&](const ui64 hash) {
            intBuilder.UnsafeAppend(hash);
        };

        if (!ExecuteToArrayImpl(batch, acceptor)) {
            return nullptr;
        }

        return NArrow::TStatusValidator::GetValid(builder->Finish());
    }

    template <class TDataContainer>
    std::vector<ui64> ExecuteToVector(const std::shared_ptr<TDataContainer>& batch) const {
        std::vector<ui64> result;
        result.reserve(batch->num_rows());

        const auto acceptor = [&](const ui64 hash) {
            result.emplace_back(hash);
        };

        AFL_VERIFY(ExecuteToArrayImpl(batch, acceptor));
        return result;
    }
};

}   // namespace NKikimr::NArrow::NHash
