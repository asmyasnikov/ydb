#include "client.h"
#include "config.h"

#include <yt/yt/core/http/client.h>
#include <yt/yt/core/http/private.h>

#include <yt/yt/core/net/config.h>
#include <yt/yt/core/net/address.h>

#include <yt/yt/core/crypto/tls.h>

#include <yt/yt/core/concurrency/poller.h>

#include <library/cpp/openssl/io/stream.h>

namespace NYT::NHttps {

using namespace NNet;
using namespace NHttp;
using namespace NCrypto;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TClient
    : public IClient
{
public:
    explicit TClient(IClientPtr underlying)
        : Underlying_(std::move(underlying))
    { }

    TFuture<IResponsePtr> Get(
        const TString& url,
        const THeadersPtr& headers) override
    {
        return Underlying_->Get(url, headers);
    }

    TFuture<IResponsePtr> Post(
        const TString& url,
        const TSharedRef& body,
        const THeadersPtr& headers) override
    {
        return Underlying_->Post(url, body, headers);
    }

    TFuture<IResponsePtr> Patch(
        const TString& url,
        const TSharedRef& body,
        const THeadersPtr& headers) override
    {
        return Underlying_->Patch(url, body, headers);
    }

    TFuture<IResponsePtr> Put(
        const TString& url,
        const TSharedRef& body,
        const THeadersPtr& headers) override
    {
        return Underlying_->Put(url, body, headers);
    }

    TFuture<IResponsePtr> Delete(
        const TString& url,
        const THeadersPtr& headers) override
    {
        return Underlying_->Delete(url, headers);
    }

    TFuture<IActiveRequestPtr> StartPost(
        const TString& url,
        const THeadersPtr& headers) override
    {
        return Underlying_->StartPost(url, headers);
    }

    TFuture<IActiveRequestPtr> StartPatch(
        const TString& url,
        const THeadersPtr& headers) override
    {
        return Underlying_->StartPatch(url, headers);
    }

    TFuture<IActiveRequestPtr> StartPut(
        const TString& url,
        const THeadersPtr& headers) override
    {
        return Underlying_->StartPut(url, headers);
    }

    TFuture<IResponsePtr> Request(
        EMethod method,
        const TString& url,
        const std::optional<TSharedRef>& body,
        const THeadersPtr& headers) override
    {
        return Underlying_->Request(method, url, body, headers);
    }

private:
    const IClientPtr Underlying_;
};

IClientPtr CreateClient(
    const TClientConfigPtr& config,
    const IPollerPtr& poller)
{
    auto sslContext =  New<TSslContext>();
    if (config->Credentials) {
        sslContext->ApplyConfig(config->Credentials);
    } else {
        sslContext->UseBuiltinOpenSslX509Store();
    }
    sslContext->Commit();

    auto tlsDialer = sslContext->CreateDialer(
        New<TDialerConfig>(),
        poller,
        HttpLogger());

    auto httpClient = NHttp::CreateClient(
        config,
        tlsDialer,
        poller->GetInvoker());

    return New<TClient>(std::move(httpClient));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttps
