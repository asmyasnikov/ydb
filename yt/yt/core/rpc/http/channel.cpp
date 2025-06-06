#include "channel.h"
#include "config.h"
#include "helpers.h"

#include <yt/yt/core/rpc/dispatcher.h>

#include <yt/yt/core/http/client.h>
#include <yt/yt/core/http/http.h>
#include <yt/yt/core/http/helpers.h>
#include <yt/yt/core/http/private.h>

#include <yt/yt/core/https/config.h>
#include <yt/yt/core/https/client.h>

#include <yt/yt/core/rpc/grpc/helpers.h>

namespace NYT::NRpc::NHttp {

namespace {

using namespace NRpc;
using namespace NYTree;
using namespace NYT::NHttp;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(THttpChannel)

class THttpChannel
    : public IChannel
{
public:
    THttpChannel(
        const std::string& address,
        const NConcurrency::IPollerPtr& poller,
        bool isHttps,
        NHttps::TClientCredentialsConfigPtr credentials)
        : EndpointAddress_(address)
        , EndpointAttributes_(ConvertToAttributes(BuildYsonStringFluently()
            .BeginMap()
                .Item("address").Value(EndpointAddress_)
            .EndMap()))
        , Poller_(poller)
        , IsHttps_(isHttps)
        , Credentials_(credentials)
    {
        RecreateClient();
    }

    // IChannel implementation.
    const std::string& GetEndpointDescription() const override
    {
        return EndpointAddress_;
    }

    const IAttributeDictionary& GetEndpointAttributes() const override
    {
        return *EndpointAttributes_;
    }

    IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        const TSendOptions& options) override
    {
        auto guard = ReaderGuard(SpinLock_);
        if (!TerminationError_.IsOK()) {
            auto error = TerminationError_;
            guard.Release();
            responseHandler->HandleError(error);
            return nullptr;
        }

        // Assume that the user will rarely change the timeout setting.
        // Recreating the client is harmless, only the connection cache is lost.
        auto client = Client_;
        if (ClientTimeout_ != options.Timeout) {
            guard.Release();
            auto writerGuard = WriterGuard(SpinLock_);
            ClientTimeout_ = options.Timeout;
            RecreateClient();
            client = Client_;
        }

        return New<TCallHandler>(
            this,
            client,
            std::move(request),
            std::move(responseHandler));
    }

    void Terminate(const TError& error) override
    {
        {
            auto guard = WriterGuard(SpinLock_);

            if (!TerminationError_.IsOK()) {
                return;
            }

            TerminationError_ = error;
        }
        Terminated_.Fire(TerminationError_);
    }

    void SubscribeTerminated(const TCallback<void(const TError&)>& callback) override
    {
        Terminated_.Subscribe(callback);
    }

    void UnsubscribeTerminated(const TCallback<void(const TError&)>& callback) override
    {
        Terminated_.Unsubscribe(callback);
    }

    // Custom methods.
    const std::string& GetEndpointAddress() const
    {
        return EndpointAddress_;
    }

    int GetInflightRequestCount() override
    {
        YT_UNIMPLEMENTED();
    }

    const IMemoryUsageTrackerPtr& GetChannelMemoryTracker() override
    {
        return MemoryUsageTracker_;
    }

private:
    IClientPtr Client_;
    std::optional<TDuration> ClientTimeout_;

    const std::string EndpointAddress_;
    const IAttributeDictionaryPtr EndpointAttributes_;
    const NConcurrency::IPollerPtr Poller_;
    const IMemoryUsageTrackerPtr MemoryUsageTracker_ = GetNullMemoryUsageTracker();

    bool IsHttps_;
    NHttps::TClientCredentialsConfigPtr Credentials_;

    TSingleShotCallbackList<void(const TError&)> Terminated_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, SpinLock_);
    TError TerminationError_;

    void RecreateClient()
    {
        auto clientConfig = New<NHttps::TClientConfig>();
        clientConfig->MaxIdleConnections = 10;
        if (ClientTimeout_) {
            clientConfig->ConnectionIdleTimeout = *ClientTimeout_;
            clientConfig->HeaderReadTimeout = *ClientTimeout_;
            clientConfig->BodyReadIdleTimeout = *ClientTimeout_;
        }

        if (IsHttps_) {
            clientConfig->Credentials = Credentials_;
            Client_ = NHttps::CreateClient(clientConfig, Poller_);
        } else {
            Client_ = NHttp::CreateClient(clientConfig, Poller_);
        }
    }

    class TCallHandler
        : public IClientRequestControl
    {
    public:
        TCallHandler(
            THttpChannel* channel,
            const IClientPtr& client,
            const IClientRequestPtr& request,
            IClientResponseHandlerPtr responseHandler)
        {
            auto httpRequestHeaders = TranslateRequest(request);

            auto protocol = channel->IsHttps_ ? "https" : "http";
            // See TServer::DoRegisterService().
            auto url = Format("%v://%v/%v/%v", protocol, channel->EndpointAddress_, request->GetService(), request->GetMethod());

            TSharedRef httpRequestBody;
            try {
                THROW_ERROR_EXCEPTION_IF(
                    request->IsAttachmentCompressionEnabled(),
                    "Compression codecs are not supported in HTTP");
                auto requestBody = request->Serialize();
                THROW_ERROR_EXCEPTION_UNLESS(requestBody.Size() == 2, "Attachments are not supported in HTTP");
                if (request->IsLegacyRpcCodecsEnabled()) {
                    httpRequestBody = NGrpc::ExtractMessageFromEnvelopedMessage(requestBody[1]);
                } else {
                    httpRequestBody = requestBody[1];
                }
            } catch (const std::exception& ex) {
                responseHandler->HandleError(TError(NRpc::EErrorCode::TransportError, "Request serialization failed")
                    << ex);
                return;
            }

            Response_ = client->Post(url, httpRequestBody, httpRequestHeaders);
            Response_.Subscribe(
                BIND(&TCallHandler::OnResponse,
                    channel->EndpointAddress_,
                    request->GetRequestId(),
                    request->GetService(),
                    request->GetMethod(),
                    std::move(responseHandler))
                    .Via(NRpc::TDispatcher::Get()->GetHeavyInvoker()));
        }

        // IClientRequestControl overrides
        void Cancel() override
        {
            Response_.Cancel(TError(NYT::EErrorCode::Canceled, "HTTP RPC request canceled"));
        }

        TFuture<void> SendStreamingPayload(const TStreamingPayload& /*payload*/) override
        {
            YT_UNIMPLEMENTED();
        }

        TFuture<void> SendStreamingFeedback(const TStreamingFeedback& /*feedback*/) override
        {
            YT_UNIMPLEMENTED();
        }

    private:
        TFuture<IResponsePtr> Response_;

        static void OnResponse(
            const std::string& address,
            TRequestId requestId,
            const std::string& service,
            const std::string& method,
            const IClientResponseHandlerPtr& responseHandler,
            const TErrorOr<IResponsePtr>& responseOrError)
        {
            try {
                if (!responseOrError.IsOK()) {
                    responseHandler->HandleError(TError(NRpc::EErrorCode::TransportError, "HTTP client request failed")
                        << responseOrError);
                    return;
                }

                const auto& response = responseOrError.Value();
                if (response->GetStatusCode() == EStatusCode::NotFound) {
                    responseHandler->HandleError(TError(NRpc::EErrorCode::NoSuchService, "URL was not resolved to a service"));
                    return;
                }

                if (response->GetStatusCode() == EStatusCode::BadRequest) {
                    responseHandler->HandleError(ParseYTError(response));
                    return;
                }

                if (response->GetStatusCode() != EStatusCode::OK) {
                    responseHandler->HandleError(TError(NRpc::EErrorCode::TransportError, "Unexpected HTTP status code")
                        << TErrorAttribute("status", response->GetStatusCode()));
                    return;
                }

                auto responseBody = PushEnvelope(response->ReadAll(), NCompression::ECodec::None);

                NRpc::NProto::TResponseHeader responseHeader;
                ToProto(responseHeader.mutable_request_id(), requestId);
                ToProto(responseHeader.mutable_service(), service);
                ToProto(responseHeader.mutable_method(), method);

                auto responseMessage = CreateResponseMessage(
                    responseHeader,
                    responseBody,
                    /*attachments*/ {});
                responseHandler->HandleResponse(responseMessage, address);
            } catch (const std::exception& ex) {
                responseHandler->HandleError(TError(NRpc::EErrorCode::TransportError, "Response deserialization failed")
                    << ex);
            }
        }

        // This function does the backwards transformation of NRpc::NHttp::THttpHandler::TranslateRequest().
        THeadersPtr TranslateRequest(const IClientRequestPtr& request)
        {
            using namespace NHeaders;
            using NYT::FromProto;

            NProto::TRequestHeader& rpcHeader = request->Header();
            THeadersPtr httpHeaders = New<THeaders>();

            if (rpcHeader.has_request_format()) {
                auto format = FromProto<EMessageFormat>(rpcHeader.request_format());
                httpHeaders->Add(ContentTypeHeaderName, ToHttpContentType(format));
            }

            if (rpcHeader.has_request_format_options()) {
                httpHeaders->Add(RequestFormatOptionsHeaderName, rpcHeader.request_format_options());
            }

            if (rpcHeader.has_response_format()) {
                auto format = FromProto<EMessageFormat>(rpcHeader.response_format());
                httpHeaders->Add(AcceptHeaderName, ToHttpContentType(format));
            }

            if (rpcHeader.has_response_format_options()) {
                httpHeaders->Add(ResponseFormatOptionsHeaderName, rpcHeader.response_format_options());
            }

            if (rpcHeader.has_request_id()) {
                auto requestId = FromProto<TRequestId>(rpcHeader.request_id());
                httpHeaders->Add(RequestIdHeaderName, ToString(requestId));
            }

            if (rpcHeader.HasExtension(NRpc::NProto::TCredentialsExt::credentials_ext)) {
                const auto& credentialsExt = rpcHeader.GetExtension(NRpc::NProto::TCredentialsExt::credentials_ext);

                if (credentialsExt.has_token()) {
                    httpHeaders->Add(AuthorizationHeaderName, "OAuth " + credentialsExt.token());
                }

                if (credentialsExt.has_user_ticket()) {
                    httpHeaders->Add(UserTicketHeaderName, credentialsExt.user_ticket());
                }

                if (credentialsExt.has_session_id() || credentialsExt.has_ssl_session_id()) {
                    std::string cookieString;

                    if (credentialsExt.has_session_id()) {
                        static const std::string SessionIdCookieName("Session_id");
                        cookieString += SessionIdCookieName + "=" + credentialsExt.session_id();
                    }

                    if (credentialsExt.has_ssl_session_id()) {
                        if (!cookieString.empty()) {
                            cookieString += "; ";
                        }
                        static const std::string SessionId2CookieName("sessionid2");
                        cookieString += SessionId2CookieName + "=" + credentialsExt.ssl_session_id();
                    }

                    httpHeaders->Add(CookieHeaderName, cookieString);
                }
            }

            if (rpcHeader.has_user_agent()) {
                httpHeaders->Add(UserAgentHeaderName, rpcHeader.user_agent());
            }

            if (const auto& user = request->GetUser(); !user.empty()) {
                // TODO(babenko): switch to std:::string
                httpHeaders->Add(UserNameHeaderName, std::string(user));
            }

            if (const auto& userTag = request->GetUserTag(); !userTag.empty()) {
                // TODO(babenko): switch to std:::string
                httpHeaders->Add(UserTagHeaderName, std::string(userTag));
            }

            if (rpcHeader.has_timeout()) {
                httpHeaders->Add(XRequestTimeoutHeaderName, ToString<i64>(FromProto<i64>(rpcHeader.timeout())));
            }

            if (rpcHeader.has_protocol_version_major()) {
                httpHeaders->Add(ProtocolVersionMajor, ToString<i64>(FromProto<i64>(rpcHeader.protocol_version_major())));
            }

            if (rpcHeader.has_protocol_version_minor()) {
                httpHeaders->Add(ProtocolVersionMinor, ToString<i64>(FromProto<i64>(rpcHeader.protocol_version_minor())));
            }

            if (rpcHeader.HasExtension(NRpc::NProto::TCustomMetadataExt::custom_metadata_ext)) {
                const auto& customMetadataExt = rpcHeader.GetExtension(NRpc::NProto::TCustomMetadataExt::custom_metadata_ext);
                for (const auto& [key, value] : customMetadataExt.entries()) {
                    httpHeaders->Add("X-" + key, value);
                }
            }

            return httpHeaders;
        }
    };
};

DEFINE_REFCOUNTED_TYPE(THttpChannel)

////////////////////////////////////////////////////////////////////////////////

} // namespace

IChannelPtr CreateHttpChannel(
    const std::string& address,
    const NConcurrency::IPollerPtr& poller,
    bool isHttps,
    NHttps::TClientCredentialsConfigPtr credentials)
{
    return New<THttpChannel>(address, poller, isHttps, credentials);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc::NHttp
