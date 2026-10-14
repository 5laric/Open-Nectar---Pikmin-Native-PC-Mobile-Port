#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nectar::net {

// Self-contained invitation for a direct UDP session. The checksum only
// catches transcription errors; joinSecret and hostFingerprint are consumed
// by the authenticated transport handshake implemented above this codec.
struct DirectEndpoint {
    enum class Family : std::uint8_t { Ipv4 = 4, Ipv6 = 6 };

    Family family = Family::Ipv4;
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port = 0;
    std::uint8_t priority = 0;
};

struct ConnectionCode {
    static constexpr std::uint8_t kFormatVersion = 1;
    static constexpr std::size_t kMaxEndpoints = 4;

    std::uint16_t protocolMajor = 0;
    std::uint16_t flags = 0;
    std::uint64_t expiresUnixSeconds = 0;
    std::array<std::uint8_t, 16> sessionNonce{};
    std::array<std::uint8_t, 16> joinSecret{};
    std::array<std::uint8_t, 32> hostFingerprint{};
    std::vector<DirectEndpoint> endpoints;
};

enum class ConnectionCodeError {
    None,
    Empty,
    TooLong,
    BadPrefix,
    BadCharacter,
    BadPadding,
    Truncated,
    TrailingData,
    BadMagic,
    UnsupportedVersion,
    InvalidEndpointCount,
    InvalidAddressFamily,
    InvalidPort,
    InvalidProtocol,
    MissingSessionMaterial,
    Expired,
    RandomSourceFailure,
    ChecksumMismatch,
};

// Produces grouped Crockford Base32 beginning with "NECTAR1-". Returns an
// empty string when the object is invalid (for example no endpoint or port 0).
std::string encodeConnectionCode(const ConnectionCode& code);

// Strict, allocation-bounded parser. ASCII spaces and hyphens are accepted as
// visual separators; ambiguous Crockford characters O/I/L are accepted.
ConnectionCodeError decodeConnectionCode(const std::string& text, ConnectionCode& code);

// Creates the security-sensitive fields from the operating system CSPRNG.
// nowUnixSeconds is explicit so callers and tests do not depend on a clock
// hidden inside the codec. The fingerprint identifies the host build/key.
ConnectionCodeError createConnectionCode(std::uint16_t protocolMajor, std::uint16_t flags,
                                         std::uint64_t nowUnixSeconds, std::uint64_t lifetimeSeconds,
                                         const std::array<std::uint8_t, 32>& hostFingerprint,
                                         const std::vector<DirectEndpoint>& endpoints, ConnectionCode& code);

// Checks the fields whose meaning depends on the receiving session. Decoding
// alone remains useful for diagnostics and never silently consults wall time.
ConnectionCodeError validateConnectionCode(const ConnectionCode& code, std::uint16_t expectedProtocolMajor,
                                           std::uint64_t nowUnixSeconds);

const char* connectionCodeErrorString(ConnectionCodeError error);

} // namespace nectar::net
