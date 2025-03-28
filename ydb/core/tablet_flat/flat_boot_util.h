#pragma once

#include "util_fmt_abort.h"

#include <util/system/yassert.h>
#include <util/generic/ylimits.h>

namespace NKikimr {
namespace NTabletFlatExecutor {
namespace NBoot {

    class TSpawned {
    public:
        explicit TSpawned(bool spawned)
            : Spawned(spawned)
        { }

        explicit operator bool() const noexcept
        {
            return Spawned;
        }

    private:
        const bool Spawned;
    };

    class TLeft {
    public:
        TLeft() = default;

        explicit operator bool() const noexcept
        {
            return Value > 0;
        }

        ui64 Get() const noexcept
        {
            return Value;
        }

        TLeft& operator +=(const TSpawned& spawned)
        {
            if (spawned) {
                *this += size_t(1);
            }

            return *this;
        }

        TLeft& operator +=(size_t inc)
        {
            if (Value > Max<decltype(Value)>() - inc) {
                Y_TABLET_ERROR("TLeft counter is overflowed");
            }

            Value += inc;

            return *this;
        }

        TLeft& operator -=(size_t dec)
        {
            Y_ENSURE(Value >= dec, "TLeft counter is underflowed");

            Value -= dec;

            return *this;
        }

    private:
        ui64 Value = 0;
    };
}
}
}
