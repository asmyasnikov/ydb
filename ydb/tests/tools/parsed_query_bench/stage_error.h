#pragma once

#include <util/generic/yexception.h>

namespace NKikimr::NKqp::NParsedQueryBench {

class TStageError : public yexception {};

inline void RequireStage(bool condition, const char* message) {
    Y_ENSURE_EX(condition, TStageError() << message);
}

} // namespace NKikimr::NKqp::NParsedQueryBench
