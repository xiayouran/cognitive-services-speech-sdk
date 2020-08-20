//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"

#include <chrono>
#include <thread>
#include <random>
#include <string>

#include "thread_service.h"
#include "test_utils.h"
#include "site_helpers.h"
#include "usp.h"
#include "guid_utils.h"
#include "usp_binary_message.h"

using namespace std;
using namespace Microsoft::CognitiveServices::Speech;
using namespace Microsoft::CognitiveServices::Speech::Impl;

class UspClient : public USP::Callbacks, public std::enable_shared_from_this<UspClient>
{
    USP::EndpointType m_endpointType;
    USP::RecognitionMode m_mode;

public:
    UspClient(USP::EndpointType endpoint = USP::EndpointType::Speech,
        USP::RecognitionMode mode = USP::RecognitionMode::Interactive)
        : m_endpointType(endpoint), m_mode(mode)
    {
    }

    void Init()
    {
        auto region = SubscriptionsRegionsMap[UNIFIED_SPEECH_SUBSCRIPTION].Region.empty() ? "westus" : SubscriptionsRegionsMap[UNIFIED_SPEECH_SUBSCRIPTION].Region;
        std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
        authData[static_cast<size_t>(USP::AuthenticationType::SubscriptionKey)] = SubscriptionsRegionsMap[UNIFIED_SPEECH_SUBSCRIPTION].Key;

        m_threadService = std::make_shared<CSpxThreadService>();
        m_threadService->Init();
        auto client = USP::Client(shared_from_this(), m_endpointType, PAL::CreateGuidWithoutDashes(), m_threadService)
            .SetRecognitionMode(m_mode)
            .SetRegion(region)
            .SetAuthentication(authData)
            .SetQueryParameter(USP::endpoint::langQueryParam, "en-us");
        if (!DefaultSettingsMap[ENDPOINT].empty())
        {
            client.SetEndpointType(USP::EndpointType::Speech).SetEndpointUrl(DefaultSettingsMap[ENDPOINT]);
        }

        m_connection = client.Connect();
    }

    void Term()
    {
        m_threadService->Term();
    }

    virtual void OnError(const std::shared_ptr<ISpxErrorInformation>& error) override
    {
        FAIL(error->GetDetails());
    }

    template <class T>
    void WriteAudio(T* buffer, uint32_t size)
    {
        std::shared_ptr<uint8_t> data(new uint8_t[size], [](uint8_t* p) { delete[] p; });
        memcpy(data.get(), buffer, size);
        auto audioChunk = std::make_shared<Microsoft::CognitiveServices::Speech::Impl::DataChunk>(data, size);

        m_connection->WriteAudio(audioChunk);
    }

    virtual ~UspClient() = default;
private:
    USP::ConnectionPtr m_connection;
    std::shared_ptr<CSpxThreadService> m_threadService;
};

using UspClientPtr = std::shared_ptr<UspClient>;

TEST_CASE("USP is properly functioning", "[usp]")
{
    SECTION("usp can be initialized, connected and closed")
    {
        auto client = std::make_shared<UspClient>();
        REQUIRE_NOTHROW(client->Init());
        REQUIRE_NOTHROW(client->Term());
    }

    REQUIRE(exists(ROOT_RELATIVE_PATH(SINGLE_UTTERANCE_ENGLISH)));

    SECTION("usp can be used to upload binary data")
    {
        string dummy = "RIFF1234567890";
        auto client = std::make_shared<UspClient>();
        REQUIRE_NOTHROW(client->Init());
        client->WriteAudio(dummy.data(), (uint32_t)dummy.length());
        REQUIRE_NOTHROW(client->Term());
    }

    random_engine rnd(12345);
    size_t buffer_size_8k = 1 << 13;
    vector<char> buffer(buffer_size_8k);

    SECTION("usp can be used to upload audio from file")
    {
        auto client = std::make_shared<UspClient>();
        client->Init();
        auto is = get_stream(ROOT_RELATIVE_PATH(SINGLE_UTTERANCE_ENGLISH));

        while (is) {
            auto size_to_read = max(size_t(1 << 10), rnd() % buffer_size_8k);
            is.read(buffer.data(), size_to_read);
            auto bytesRead = (uint32_t)is.gcount();
            client->WriteAudio(buffer.data(), bytesRead);
            std::this_thread::sleep_for(std::chrono::milliseconds(rnd() % 100));
        }
        std::this_thread::sleep_for(std::chrono::seconds(10));
        REQUIRE_NOTHROW(client->Term());
    }

    SECTION("usp can toggled on/off multiple times in a row")
    {
        for (unsigned int i = 10; i > 0; i--)
        {
            auto client = std::make_shared<UspClient>();
            client->Init();
            auto is = get_stream(ROOT_RELATIVE_PATH(SINGLE_UTTERANCE_ENGLISH));
            while (is && (rnd()%i < i>>1)) {
                is.read(buffer.data(), buffer_size_8k);
                auto bytesRead = (uint32_t)is.gcount();
                client->WriteAudio(buffer.data(), bytesRead);
                std::this_thread::sleep_for(std::chrono::milliseconds(rnd() % 100));
            }
            std::this_thread::sleep_for(std::chrono::seconds(10));
            REQUIRE_NOTHROW(client->Term());
        }
    }

    SECTION("several usp clients can coexist peacefully")
    {
        int num_handles = 10;
        vector<UspClientPtr> clients(num_handles);
        for (int i = 0; i < num_handles; ++i)
        {
            clients[i] = std::make_shared<UspClient>();
            clients[i]->Init();
        }

        auto is = get_stream(ROOT_RELATIVE_PATH(SINGLE_UTTERANCE_ENGLISH));
        is.read(buffer.data(), buffer_size_8k);
        REQUIRE(is.good());

        for (int i = 0; i < num_handles; i++)
        {
            auto bytesRead = (uint32_t)is.gcount();
            clients[i]->WriteAudio(buffer.data(), bytesRead);
        }

        while (is)
        {
            auto size_to_read = max(size_t(1 << 10), rnd() % buffer_size_8k);
            is.read(buffer.data(), size_to_read);
            auto bytesRead = (uint32_t)is.gcount();
            clients[rnd() % num_handles]->WriteAudio(buffer.data(), bytesRead);
            std::this_thread::sleep_for(std::chrono::milliseconds(rnd() % 100));
        }

        std::this_thread::sleep_for(std::chrono::seconds(10));
        for (int i = 0; i < num_handles; i++)
        {
            REQUIRE_NOTHROW(clients[i]->Term());
        }
    }
}

class TlsCheck : public USP::Callbacks
{
    void OnError(const std::shared_ptr<ISpxErrorInformation>& error) override
    {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 6237)
        // Disable: (<zero> && <expression>) is always zero.  <expression> is never evaluated and might have side effects.
#endif
        REQUIRE(error->GetCancellationCode() == CancellationErrorCode::ServiceRedirectPermanent);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }

public:
    virtual ~TlsCheck() {};
};

TEST_CASE("USP uses TLS12", "[usp]")
{
    // GitHub doesn't allow TLSv1 and TLSv1.1 since February 2018 (https://githubengineering.com/crypto-removal-notice/).
    auto service = std::make_shared<CSpxThreadService>();
    service->Init();
    auto callbacks = std::make_shared<TlsCheck>();
    std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
    authData[static_cast<size_t>(USP::AuthenticationType::SubscriptionKey)] = "test";
    auto client = USP::Client(callbacks, USP::EndpointType::Speech, PAL::CreateGuidWithoutDashes(), service)
        .SetRegion("westus")
        .SetEndpointUrl("wss://www.github.com/")
        .SetAuthentication(authData);

    auto connection = client.Connect();
    constexpr unsigned int dataSize = 7;
    auto data = new uint8_t[dataSize]{ 1, 2, 3, 4, 5, 6, 7 };
    std::shared_ptr<uint8_t> buffer(data, [](uint8_t* p) { delete[] p; });
    connection->WriteAudio(std::make_shared<DataChunk>(buffer, dataSize));
    this_thread::sleep_for(5s);
}

class PortCheck : public USP::Callbacks
{
public:
    PortCheck() : m_promise()
    {
    }

    virtual ~PortCheck() = default;

    void OnError(const std::shared_ptr<ISpxErrorInformation>& error) override
    {
        std::exception_ptr exPtr;

        try
        {
            REQUIRE(error->GetCancellationCode() == CancellationErrorCode::ConnectionFailure);
            SPXTEST_REQUIRE_THAT(error->GetDetails(), Catch::Contains("Connection failed", Catch::CaseSensitive::No));
            m_promise.set_value();
        }
        catch (std::exception&)
        {
            m_promise.set_exception(std::current_exception());
        }
        catch (...)
        {
            // Trying to rethrow something that isn't an exception can cause asserts.
            // Let's create an arbitrary exception to rethrow instead
            m_promise.set_exception(make_exception_ptr(std::runtime_error("Unknown failure on error callback")));
        }
    }

    void WaitForErrorCallback(std::chrono::seconds maxWait = 10s, std::chrono::seconds additionalWaitTime = 2s)
    {
        auto future = m_promise.get_future();
        auto res = future.wait_for(maxWait);

        if (additionalWaitTime.count() > 0)
        {
            std::this_thread::sleep_for(additionalWaitTime);
        }

        SPXTEST_REQUIRE(res == std::future_status::ready);
    }

private:
    std::promise<void> m_promise;
};

TEST_CASE("Port specification", "[usp]")
{
    SECTION("Valid port specification")
    {
        auto service = std::make_shared<CSpxThreadService>();
        service->Init();
        auto callbacks = std::make_shared<PortCheck>();
        std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
        authData[static_cast<size_t>(USP::AuthenticationType::SubscriptionKey)] = "test";
        auto client = USP::Client(callbacks, USP::EndpointType::Speech, PAL::CreateGuidWithoutDashes(), service)
            .SetRegion("westus")
            .SetEndpointUrl("ws://127.0.0.1:12345/mytest")
            .SetAuthentication(authData);

        auto connection = client.Connect();
        constexpr unsigned int dataSize = 7;
        auto data = new uint8_t[dataSize]{ 1, 2, 3, 4, 5, 6, 7 };
        std::shared_ptr<uint8_t> buffer(data, [](uint8_t* p) { delete[] p; });
        connection->WriteAudio(std::make_shared<DataChunk>(buffer, dataSize));
        callbacks->WaitForErrorCallback();
    }

    SECTION("Valid port specification 2")
    {
        auto service = std::make_shared<CSpxThreadService>();
        service->Init();
        auto callbacks = std::make_shared<PortCheck>();
        std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
        authData[static_cast<size_t>(USP::AuthenticationType::SubscriptionKey)] = "test";
        auto client = USP::Client(callbacks, USP::EndpointType::Speech, PAL::CreateGuidWithoutDashes(), service)
            .SetRegion("westus")
            .SetEndpointUrl("wss://myserver:50/mydir/myapi?foo=bar")
            .SetAuthentication(authData);

        auto connection = client.Connect();
        constexpr unsigned int dataSize = 7;
        auto data = new uint8_t[dataSize]{ 1, 2, 3, 4, 5, 6, 7 };
        std::shared_ptr<uint8_t> buffer(data, [](uint8_t* p) { delete[] p; });
        connection->WriteAudio(std::make_shared<DataChunk>(buffer, dataSize));
        callbacks->WaitForErrorCallback();
    }

    SECTION("Invalid port specification")
    {
        auto service = std::make_shared<CSpxThreadService>();
        service->Init();
        auto callbacks = std::make_shared<PortCheck>();
        std::array<std::string, static_cast<size_t>(USP::AuthenticationType::SIZE_AUTHENTICATION_TYPE)> authData;
        authData[static_cast<size_t>(USP::AuthenticationType::SubscriptionKey)] = "test";
        auto client = USP::Client(callbacks, USP::EndpointType::Speech, PAL::CreateGuidWithoutDashes(), service)
            .SetRegion("westus")
            .SetEndpointUrl("ws://127.0.0.1:abc/mytest")  // Invalid port specification, should fail on connect.
            .SetAuthentication(authData);

        REQUIRE_THROWS_WITH(client.Connect(), "Port is not valid");
    }
}

TEST_CASE("USP binary message serialization optimisation", "[usp][binary_message]")
{
    std::string original("This is a short test");
    USP::BinaryMessage msg(original.length() + 1, "ralph.test", USP::MessageType::Config, PAL::ToString(PAL::CreateGuidWithoutDashes()));
    memcpy(msg.Data(), original.c_str(), original.length() + 1);

    uint8_t* ptrData = msg.Data();
    REQUIRE(ptrData != nullptr);
    REQUIRE(ptrData[0] == 'T');

    std::shared_ptr<uint8_t> serialized;
    size_t bytes = msg.Serialize(serialized);
    (void)bytes;

    ptrData = msg.Data();

    std::string afterSerialization(reinterpret_cast<char*>(ptrData), original.length());
    REQUIRE(original == afterSerialization);
}
