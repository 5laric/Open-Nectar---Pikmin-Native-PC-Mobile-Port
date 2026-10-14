#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nectar::net {

enum class Delivery {
    ReliableOrdered,
    Unreliable,
};

enum class TransportState {
    Closed,
    Listening,
    Connecting,
    Connected,
    Failed,
};

enum class TransportEventType {
    Connected,
    Disconnected,
    Message,
    Error,
};

struct Endpoint {
    std::string address;
    std::uint16_t port = 0;
};

struct TransportEvent {
    TransportEventType type = TransportEventType::Error;
    Delivery delivery       = Delivery::ReliableOrdered;
    std::vector<std::uint8_t> payload;
    std::string detail;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool listen(const Endpoint& endpoint) = 0;
    virtual bool connect(const Endpoint& endpoint) = 0;
    virtual bool send(Delivery delivery, const std::uint8_t* data, std::size_t size) = 0;
    virtual bool poll(TransportEvent& event) = 0;
    virtual void close() = 0;
    virtual TransportState state() const = 0;
};

} // namespace nectar::net
