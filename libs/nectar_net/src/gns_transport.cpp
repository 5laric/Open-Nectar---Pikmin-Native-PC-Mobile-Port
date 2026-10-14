#include "nectar_net/gns_transport.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace nectar::net {

struct GnsTransport::Impl {
    static constexpr std::size_t kMaxMessageBytes = 4U * 1024U * 1024U;

    static std::mutex registryMutex;
    static std::unordered_map<HSteamNetConnection, Impl*> connections;
    static std::unordered_map<HSteamListenSocket, Impl*> listeners;
    static unsigned users;
    static bool libraryReady;

    ISteamNetworkingSockets* sockets = nullptr;
    HSteamListenSocket listener = k_HSteamListenSocket_Invalid;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    TransportState currentState = TransportState::Closed;
    std::deque<TransportEvent> events;
    bool acquired = false;

    Impl()
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (users == 0) {
            SteamDatagramErrMsg error{};
            libraryReady = GameNetworkingSockets_Init(nullptr, error);
            if (!libraryReady) {
                currentState = TransportState::Failed;
                queue(TransportEventType::Error, error);
                return;
            }
        }
        ++users;
        acquired = true;
        sockets = SteamNetworkingSockets();
        if (sockets == nullptr) {
            currentState = TransportState::Failed;
            queue(TransportEventType::Error, "GameNetworkingSockets returned no interface");
        }
    }

    ~Impl()
    {
        close();
        std::lock_guard<std::mutex> lock(registryMutex);
        if (acquired && users != 0 && --users == 0) {
            GameNetworkingSockets_Kill();
            libraryReady = false;
        }
    }

    void queue(TransportEventType type, const std::string& detail = {})
    {
        TransportEvent event;
        event.type = type;
        event.detail = detail;
        events.push_back(std::move(event));
    }

    static bool address(const Endpoint& endpoint, bool listening, SteamNetworkingIPAddr& output)
    {
        output.Clear();
        if (!endpoint.address.empty() && endpoint.address != "0.0.0.0" && endpoint.address != "::") {
            if (!output.ParseString(endpoint.address.c_str())) return false;
        } else if (!listening) {
            return false;
        }
        output.m_port = endpoint.port;
        return endpoint.port != 0;
    }

    static void statusChanged(SteamNetConnectionStatusChangedCallback_t* info)
    {
        Impl* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            const auto connectionIt = connections.find(info->m_hConn);
            if (connectionIt != connections.end()) target = connectionIt->second;
            if (target == nullptr && info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
                const auto listenerIt = listeners.find(info->m_info.m_hListenSocket);
                if (listenerIt != listeners.end()) target = listenerIt->second;
            }
        }
        // GnsTransport is deliberately single-thread-owned. RunCallbacks()
        // invokes this synchronously from poll(), so target cannot disappear
        // unless the caller destroys a transport concurrently (unsupported).
        if (target != nullptr) target->onStatus(*info);
    }

    void onStatus(const SteamNetConnectionStatusChangedCallback_t& info)
    {
        switch (info.m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (info.m_info.m_hListenSocket == k_HSteamListenSocket_Invalid) break;
            if (connection != k_HSteamNetConnection_Invalid
                || sockets->AcceptConnection(info.m_hConn) != k_EResultOK) {
                sockets->CloseConnection(info.m_hConn, 0, "Only one guest is allowed", false);
                break;
            }
            connection = info.m_hConn;
            currentState = TransportState::Connecting;
            {
                std::lock_guard<std::mutex> lock(registryMutex);
                connections[connection] = this;
            }
            break;
        case k_ESteamNetworkingConnectionState_Connected:
            currentState = TransportState::Connected;
            queue(TransportEventType::Connected);
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            // A refused second guest may still produce a terminal callback
            // through the listener. It must not tear down the accepted peer.
            if (info.m_hConn != connection) {
                sockets->CloseConnection(info.m_hConn, 0, nullptr, false);
                break;
            }
            const bool localProblem =
                info.m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally;
            const std::string detail = info.m_info.m_szEndDebug;
            {
                std::lock_guard<std::mutex> lock(registryMutex);
                connections.erase(info.m_hConn);
            }
            sockets->CloseConnection(info.m_hConn, 0, nullptr, false);
            if (connection == info.m_hConn) connection = k_HSteamNetConnection_Invalid;
            // A host whose guest left keeps its room open for a reconnection.
            currentState = localProblem ? TransportState::Failed
                : listener != k_HSteamListenSocket_Invalid ? TransportState::Listening
                                                           : TransportState::Closed;
            queue(localProblem ? TransportEventType::Error : TransportEventType::Disconnected, detail);
            break;
        }
        default:
            break;
        }
    }

    void disconnectPeer(bool flushReliable)
    {
        if (sockets == nullptr || connection == k_HSteamNetConnection_Invalid) return;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            connections.erase(connection);
        }
        sockets->CloseConnection(connection, 0, "Nectar peer dropped", flushReliable);
        connection = k_HSteamNetConnection_Invalid;
        currentState = listener != k_HSteamListenSocket_Invalid ? TransportState::Listening : TransportState::Closed;
    }

    void close()
    {
        if (sockets != nullptr && connection != k_HSteamNetConnection_Invalid) {
            {
                std::lock_guard<std::mutex> lock(registryMutex);
                connections.erase(connection);
            }
            sockets->CloseConnection(connection, 0, "Nectar transport closed", false);
            connection = k_HSteamNetConnection_Invalid;
        }
        if (sockets != nullptr && listener != k_HSteamListenSocket_Invalid) {
            {
                std::lock_guard<std::mutex> lock(registryMutex);
                listeners.erase(listener);
            }
            sockets->CloseListenSocket(listener);
            listener = k_HSteamListenSocket_Invalid;
        }
        events.clear();
        currentState = TransportState::Closed;
    }
};

std::mutex GnsTransport::Impl::registryMutex;
std::unordered_map<HSteamNetConnection, GnsTransport::Impl*> GnsTransport::Impl::connections;
std::unordered_map<HSteamListenSocket, GnsTransport::Impl*> GnsTransport::Impl::listeners;
unsigned GnsTransport::Impl::users = 0;
bool GnsTransport::Impl::libraryReady = false;

GnsTransport::GnsTransport() : mImpl(std::make_unique<Impl>()) {}
GnsTransport::~GnsTransport() = default;

bool GnsTransport::listen(const Endpoint& endpoint)
{
    if (mImpl->sockets == nullptr) return false;
    mImpl->close();
    SteamNetworkingIPAddr bindAddress;
    if (!Impl::address(endpoint, true, bindAddress)) {
        mImpl->currentState = TransportState::Failed;
        mImpl->queue(TransportEventType::Error, "Invalid listen endpoint");
        return false;
    }
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(+Impl::statusChanged));
    mImpl->listener = mImpl->sockets->CreateListenSocketIP(bindAddress, 1, &option);
    if (mImpl->listener == k_HSteamListenSocket_Invalid) {
        mImpl->currentState = TransportState::Failed;
        mImpl->queue(TransportEventType::Error, "Could not create the encrypted UDP listener");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(Impl::registryMutex);
        Impl::listeners[mImpl->listener] = mImpl.get();
    }
    mImpl->currentState = TransportState::Listening;
    return true;
}

bool GnsTransport::connect(const Endpoint& endpoint)
{
    if (mImpl->sockets == nullptr) return false;
    mImpl->close();
    SteamNetworkingIPAddr remoteAddress;
    if (!Impl::address(endpoint, false, remoteAddress)) {
        mImpl->currentState = TransportState::Failed;
        mImpl->queue(TransportEventType::Error, "Invalid remote endpoint");
        return false;
    }
    SteamNetworkingConfigValue_t option;
    option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                  reinterpret_cast<void*>(+Impl::statusChanged));
    mImpl->connection = mImpl->sockets->ConnectByIPAddress(remoteAddress, 1, &option);
    if (mImpl->connection == k_HSteamNetConnection_Invalid) {
        mImpl->currentState = TransportState::Failed;
        mImpl->queue(TransportEventType::Error, "Could not start the encrypted UDP connection");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(Impl::registryMutex);
        Impl::connections[mImpl->connection] = mImpl.get();
    }
    mImpl->currentState = TransportState::Connecting;
    return true;
}

bool GnsTransport::send(Delivery delivery, const std::uint8_t* data, std::size_t size)
{
    if (mImpl->currentState != TransportState::Connected || (data == nullptr && size != 0)
        || size > Impl::kMaxMessageBytes || size > std::numeric_limits<std::uint32_t>::max())
        return false;
    const int flags = delivery == Delivery::ReliableOrdered ? k_nSteamNetworkingSend_Reliable
                                                             : k_nSteamNetworkingSend_UnreliableNoDelay;
    return mImpl->sockets->SendMessageToConnection(mImpl->connection, data, static_cast<std::uint32_t>(size),
                                                    flags, nullptr)
        == k_EResultOK;
}

bool GnsTransport::poll(TransportEvent& event)
{
    if (mImpl->sockets != nullptr) mImpl->sockets->RunCallbacks();
    if (!mImpl->events.empty()) {
        event = std::move(mImpl->events.front());
        mImpl->events.pop_front();
        return true;
    }
    if (mImpl->currentState != TransportState::Connected) return false;

    ISteamNetworkingMessage* message = nullptr;
    const int received = mImpl->sockets->ReceiveMessagesOnConnection(mImpl->connection, &message, 1);
    if (received <= 0) {
        if (received < 0) {
            event = TransportEvent{};
            event.type = TransportEventType::Error;
            event.detail = "GameNetworkingSockets receive failed";
            mImpl->currentState = TransportState::Failed;
            return true;
        }
        return false;
    }
    event = TransportEvent{};
    event.type = TransportEventType::Message;
    event.delivery = (message->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0
        ? Delivery::ReliableOrdered
        : Delivery::Unreliable;
    if (message->m_cbSize >= 0 && static_cast<std::size_t>(message->m_cbSize) <= Impl::kMaxMessageBytes) {
        const auto* begin = static_cast<const std::uint8_t*>(message->m_pData);
        event.payload.assign(begin, begin + message->m_cbSize);
    } else {
        event.type = TransportEventType::Error;
        event.detail = "Received an oversized encrypted transport message";
        mImpl->currentState = TransportState::Failed;
    }
    message->Release();
    return true;
}

void GnsTransport::disconnectPeer(bool flushReliable) { mImpl->disconnectPeer(flushReliable); }
void GnsTransport::close() { mImpl->close(); }
TransportState GnsTransport::state() const { return mImpl->currentState; }

} // namespace nectar::net
