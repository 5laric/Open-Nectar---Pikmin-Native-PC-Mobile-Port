#include "nectar_net/gated_transport.h"

#include "nectar_net/secure_memory.h"

#include <chrono>
#include <utility>

namespace nectar::net {

namespace {

std::uint64_t steadyMilliseconds()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void wipe(std::vector<std::uint8_t>& bytes)
{
    secureWipe(bytes.data(), bytes.size());
    bytes.clear();
}

} // namespace

GatedTransport::GatedTransport(std::unique_ptr<ITransport> inner, Role role, const JoinCredentials& credentials,
                               Clock clock, std::uint64_t timeoutMs)
    : mInner(std::move(inner)),
      mRole(role),
      mClock(clock ? std::move(clock) : Clock(steadyMilliseconds)),
      mHost(credentials, timeoutMs),
      mGuest(credentials, timeoutMs)
{
}

GatedTransport::~GatedTransport()
{
    if (mInner) mInner->close();
}

bool GatedTransport::listen(const Endpoint& endpoint)
{
    if (mRole != Role::Host || !mInner) return false;
    mReady.clear();
    mHost.onDisconnected();
    return mInner->listen(endpoint);
}

bool GatedTransport::connect(const Endpoint& endpoint)
{
    if (mRole != Role::Guest || !mInner) return false;
    mReady.clear();
    mGuest.onDisconnected();
    return mInner->connect(endpoint);
}

JoinState GatedTransport::joinState() const
{
    return mRole == Role::Host ? mHost.state() : mGuest.state();
}

bool GatedTransport::send(Delivery delivery, const std::uint8_t* data, std::size_t size)
{
    if (!mInner || joinState() != JoinState::Authenticated) return false;
    return mInner->send(delivery, data, size);
}

TransportState GatedTransport::state() const
{
    if (!mInner) return TransportState::Closed;
    const TransportState inner = mInner->state();
    if (inner == TransportState::Connected && joinState() != JoinState::Authenticated)
        return TransportState::Connecting;
    return inner;
}

void GatedTransport::surface(TransportEventType type, const char* detail)
{
    TransportEvent event;
    event.type = type;
    if (detail != nullptr) event.detail = detail;
    mReady.push_back(std::move(event));
}

void GatedTransport::dropPeer(JoinAction action, JoinError error)
{
    mLastError = error;
    mInner->disconnectPeer(action == JoinAction::SendAndClose);
    if (mRole == Role::Host) {
        // Invisible to the session layer: the room simply keeps waiting.
        ++mRejectedPeers;
        mHost.onDisconnected();
    } else {
        surface(TransportEventType::Error, joinErrorString(error));
    }
}

void GatedTransport::apply(JoinAction action, const std::vector<std::uint8_t>& output)
{
    const JoinError error = mRole == Role::Host ? mHost.error() : mGuest.error();
    switch (action) {
    case JoinAction::None:
    case JoinAction::Forward:
        return;
    case JoinAction::Send:
        if (!mInner->send(Delivery::ReliableOrdered, output.data(), output.size())) {
            dropPeer(JoinAction::Close, JoinError::UnexpectedMessage);
        }
        return;
    case JoinAction::SendAndClose:
        mInner->send(Delivery::ReliableOrdered, output.data(), output.size());
        dropPeer(action, error);
        return;
    case JoinAction::Close:
        dropPeer(action, error);
        return;
    }
}

void GatedTransport::handle(TransportEvent& event)
{
    const std::uint64_t now = mClock();
    std::vector<std::uint8_t> output;

    switch (event.type) {
    case TransportEventType::Connected:
        if (mRole == Role::Host) {
            mHost.onConnected(now);
        } else {
            apply(mGuest.onConnected(now, output), output);
        }
        break;

    case TransportEventType::Message: {
        const bool wasAuthenticated = joinState() == JoinState::Authenticated;
        const JoinAction action = mRole == Role::Host
            ? mHost.onMessage(event.delivery, event.payload.data(), event.payload.size(), output)
            : mGuest.onMessage(event.delivery, event.payload.data(), event.payload.size());
        if (action == JoinAction::Forward) {
            mReady.push_back(std::move(event));
            break;
        }
        wipe(event.payload);
        apply(action, output);
        if (!wasAuthenticated && joinState() == JoinState::Authenticated) {
            surface(TransportEventType::Connected, nullptr);
        }
        break;
    }

    case TransportEventType::Disconnected:
    case TransportEventType::Error: {
        const bool wasAuthenticated = joinState() == JoinState::Authenticated;
        if (mRole == Role::Host) {
            mHost.onDisconnected();
            // An unauthenticated peer leaving is not a session event.
            if (wasAuthenticated || event.type == TransportEventType::Error) mReady.push_back(std::move(event));
        } else {
            if (mGuest.state() != JoinState::Rejected) mGuest.onDisconnected();
            mReady.push_back(std::move(event));
        }
        break;
    }
    }
    wipe(output);
}

bool GatedTransport::poll(TransportEvent& event)
{
    if (!mInner) return false;
    TransportEvent incoming;
    while (mReady.empty() && mInner->poll(incoming)) handle(incoming);

    if (mReady.empty()) {
        const std::uint64_t now = mClock();
        const JoinAction action = mRole == Role::Host ? mHost.tick(now) : mGuest.tick(now);
        if (action == JoinAction::Close) dropPeer(action, JoinError::Timeout);
    }

    if (mReady.empty()) return false;
    event = std::move(mReady.front());
    mReady.pop_front();
    return true;
}

void GatedTransport::disconnectPeer(bool flushReliable)
{
    if (!mInner) return;
    mInner->disconnectPeer(flushReliable);
    mHost.onDisconnected();
    mGuest.onDisconnected();
}

void GatedTransport::close()
{
    if (mInner) mInner->close();
    mReady.clear();
    mHost.onDisconnected();
    mGuest.onDisconnected();
}

} // namespace nectar::net
