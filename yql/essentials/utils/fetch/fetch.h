#pragma once

#include <library/cpp/uri/http_url.h>
#include <library/cpp/http/io/headers.h>
#include <library/cpp/http/io/stream.h>
#include <library/cpp/retry/retry_policy.h>

#include <util/datetime/base.h>
#include <util/generic/string.h>
#include <util/generic/ptr.h>

namespace NYql {

struct IFetchResult: public TThrRefBase {
    virtual THttpInput& GetStream() = 0;
    virtual unsigned GetRetCode() = 0;
    virtual THttpURL GetRedirectURL(const THttpURL& baseUrl) = 0;
};

using TFetchResultPtr = TIntrusivePtr<IFetchResult>;

THttpURL ParseURL(const TStringBuf addr);
IRetryPolicy<unsigned>::TPtr GetDefaultPolicy();
ERetryErrorClass DefaultClassifyHttpCode(unsigned code);
TFetchResultPtr Fetch(const THttpURL& url, const THttpHeaders& additionalHeaders = {}, const TDuration& timeout = TDuration::Max(), size_t redirects = 10, const IRetryPolicy<unsigned>::TPtr& policy = nullptr);

} // NYql
