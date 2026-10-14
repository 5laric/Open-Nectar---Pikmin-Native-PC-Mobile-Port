#pragma once

#include "nectar_net/join_gate.h"
#include "nectar_net/transport.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

namespace nectar::net {

// Wraps any ITransport with the join quarantine of ADR-0002. Callers see
// Connected only after the join secret has been validated; until then no
// message is surfaced and send() refuses gameplay. A host drops a guest that
// fails, times out or speaks out of turn, and keeps listening for another.
class GatedTransport final : public ITransport {
public:
    enum class Role { Host, Guest };
    using Clock = std::function<std::uint64_t()>; // monotonic milliseconds

    GatedTransport(std::unique_ptr<ITransport> inner, Role role, const JoinCredentials& credentials,
                   Clock clock = {}, std::uint64_t timeoutMs = kJoinTimeoutMs);
    ~GatedTransport() override;

    GatedTransport(const GatedTransport&) = delete;
    GatedTransport& operator=(const GatedTransport&) = delete;

    bool listen(const Endpoint& endpoint) override;
    bool connect(const Endpoint& endpoint) override;
    bool send(Delivery delivery, const std::uint8_t* data, std::size_t size) override;
    bool poll(TransportEvent& event) override;
    void disconnectPeer(bool flushReliable) override;
    void close() override;
    TransportState state() const override;

    JoinState joinState() const;
    // Last quarantine failure, for diagnostics; never contains secrets.
    JoinError lastJoinError() const { return mLastError; }
    // Guests the host dropped before authentication (scan/flood diagnostics).
    std::uint32_t rejectedPeers() const { return mRejectedPeers; }

private:
    void handle(TransportEvent& event);
    void apply(JoinAction action, const std::vector<std::uint8_t>& output);
    void dropPeer(JoinAction action, JoinError error);
    void surface(TransportEventType type, const char* detail);

    std::unique_ptr<ITransport> mInner;
    Role mRole;
    Clock mClock;
    HostJoinGate mHost;
    GuestJoinGate mGuest;
    std::deque<TransportEvent> mReady;
    JoinError mLastError = JoinError::None;
    std::uint32_t mRejectedPeers = 0;
};

} // namespace nectar::net
