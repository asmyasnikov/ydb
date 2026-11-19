#pragma once

#include "catalog.h"

namespace NKikimr::NKqp::NParsedQueryBench {

void BenchHostText(const TString& bytes, const TOfflineCatalog& catalog, const TVector<TIntrusivePtr<TOpRoot>>& expected,
    NYql::TExprContext& expectedContext, const TVector<TVector<TString>>& expectedTypes, size_t warmup, size_t repeats);

} // namespace NKikimr::NKqp::NParsedQueryBench
