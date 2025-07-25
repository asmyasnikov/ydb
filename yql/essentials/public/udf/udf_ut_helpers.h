#pragma once
#include "udf_value.h"

#include <yql/essentials/utils/method_index.h>

#include <util/generic/ymath.h>
#include <util/system/platform.h>
#include <util/generic/yexception.h>
#include <util/system/yassert.h>
#include <util/string/hex.h>

namespace NYql {
namespace NUdf {

template<bool HasLength = true>
class TLazyList: public NUdf::TBoxedValue {
    struct TIterator: public NUdf::TBoxedValue {
        TIterator(i32 from, i32 to)
            : From_(from), To_(to), Curr_(Max<i32>())
        {
            if (To_ >= From_) {
                To_--; // exclude last
            } else {
                From_--; // exclude first
            }
        }
    private:
        bool Skip() override {
            if (Curr_ == Max<i32>()) {
                Curr_ = From_;
                return true;
            }
            if (To_ >= From_) {
                if (Curr_ < To_) {
                    ++Curr_;
                    return true;
                }
            } else {
                if (Curr_ > To_) {
                    --Curr_;
                    return true;
                }
            }
            return false;
        }

        bool Next(NUdf::TUnboxedValue& value) override {
            if (!Skip())
                return false;
            value = NUdf::TUnboxedValuePod(Curr_);
            return true;
        }
        i32 From_, To_, Curr_;
    };
public:
    TLazyList(i32 from, i32 to)
        : From_(from), To_(to)
    {
    }

private:
    bool HasFastListLength() const override {
        return HasLength;
    }

    ui64 GetListLength() const override {
        if (HasLength)
            return Abs(To_ - From_);

        Y_ABORT("No length!");
    }

    bool HasListItems() const override {
        return To_ != From_;
    }

    NUdf::TUnboxedValue GetListIterator() const override {
        return NUdf::TUnboxedValuePod(new TIterator(From_, To_));
    }

    NUdf::IBoxedValuePtr ReverseListImpl(const NUdf::IValueBuilder& builder) const override {
        Y_UNUSED(builder);
        return new TLazyList(To_, From_);
    }

    NUdf::IBoxedValuePtr SkipListImpl(const NUdf::IValueBuilder& builder, ui64 count) const override {
        Y_UNUSED(builder);
        count = std::min<ui64>(count, Abs(To_ - From_));
        if (To_ >= From_) {
            return new TLazyList(From_ + count, To_);
        }
        return new TLazyList(From_ - count, To_);
    }

    NUdf::IBoxedValuePtr TakeListImpl(const NUdf::IValueBuilder& builder, ui64 count) const override {
        Y_UNUSED(builder);
        count = std::min<ui64>(count, Abs(To_ - From_));
        if (To_ >= From_) {
            return new TLazyList(From_, From_ + count);
        }
        return new TLazyList(From_, From_ - count);
    }

    NUdf::IBoxedValuePtr ToIndexDictImpl(const NUdf::IValueBuilder& builder) const override {
        Y_UNUSED(builder);
        return nullptr;
    }

    i32 From_, To_;
};

}
}
