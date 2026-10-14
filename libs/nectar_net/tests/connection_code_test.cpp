#include "nectar_net/connection_code.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

nectar::net::ConnectionCode sample()
{
    nectar::net::ConnectionCode code;
    code.protocolMajor = 2;
    code.flags = 3;
    code.expiresUnixSeconds = 0x0102030405060708ULL;
    for (std::size_t i = 0; i < code.sessionNonce.size(); ++i) code.sessionNonce[i] = static_cast<std::uint8_t>(i);
    for (std::size_t i = 0; i < code.joinSecret.size(); ++i) code.joinSecret[i] = static_cast<std::uint8_t>(0x20U + i);
    for (std::size_t i = 0; i < code.hostFingerprint.size(); ++i)
        code.hostFingerprint[i] = static_cast<std::uint8_t>(0x40U + i);
    nectar::net::DirectEndpoint ipv4;
    ipv4.family = nectar::net::DirectEndpoint::Family::Ipv4;
    ipv4.address[0] = 192;
    ipv4.address[1] = 0;
    ipv4.address[2] = 2;
    ipv4.address[3] = 10;
    ipv4.port = 48810;
    ipv4.priority = 100;
    code.endpoints.push_back(ipv4);
    nectar::net::DirectEndpoint ipv6;
    ipv6.family = nectar::net::DirectEndpoint::Family::Ipv6;
    ipv6.address[0] = 0x20;
    ipv6.address[1] = 0x01;
    ipv6.address[15] = 1;
    ipv6.port = 48810;
    ipv6.priority = 80;
    code.endpoints.push_back(ipv6);
    return code;
}

} // namespace

int main()
{
    using namespace nectar::net;
    const ConnectionCode source = sample();
    const std::string encoded = encodeConnectionCode(source);
    check(encoded.rfind("NECTAR1-", 0) == 0, "code has the versioned human prefix");
    check(encoded.size() < 384, "code stays inside the parser cap");

    ConnectionCode decoded;
    check(decodeConnectionCode(encoded, decoded) == ConnectionCodeError::None, "code roundtrips");
    check(decoded.protocolMajor == source.protocolMajor && decoded.flags == source.flags, "metadata roundtrips");
    check(decoded.sessionNonce == source.sessionNonce && decoded.joinSecret == source.joinSecret,
          "session material roundtrips");
    check(decoded.hostFingerprint == source.hostFingerprint, "host fingerprint roundtrips");
    check(decoded.endpoints.size() == 2 && decoded.endpoints[0].port == 48810
              && decoded.endpoints[1].family == DirectEndpoint::Family::Ipv6,
          "IPv4 and IPv6 endpoints roundtrip");
    check(validateConnectionCode(decoded, 2, 0x0102030405060707ULL) == ConnectionCodeError::None,
          "fresh code validates for the expected protocol");
    check(validateConnectionCode(decoded, 3, 0x0102030405060707ULL) == ConnectionCodeError::InvalidProtocol,
          "different protocol is rejected");
    check(validateConnectionCode(decoded, 2, 0x0102030405060709ULL) == ConnectionCodeError::Expired,
          "expired code is rejected");

    std::string compact;
    for (char c : encoded)
        if (c != '-') compact.push_back(c);
    check(decodeConnectionCode(compact, decoded) == ConnectionCodeError::BadPrefix,
          "the visible version prefix is mandatory");

    std::string corrupt = encoded;
    corrupt.back() = corrupt.back() == '0' ? '1' : '0';
    check(decodeConnectionCode(corrupt, decoded) == ConnectionCodeError::ChecksumMismatch,
          "single-character corruption is rejected");

    corrupt = encoded + "0";
    check(decodeConnectionCode(corrupt, decoded) != ConnectionCodeError::None, "trailing Base32 data is rejected");
    check(decodeConnectionCode("NPIX2-old", decoded) == ConnectionCodeError::BadPrefix,
          "legacy two-way ICE offers cannot be mistaken for direct codes");
    check(decodeConnectionCode(std::string(385, 'A'), decoded) == ConnectionCodeError::TooLong,
          "oversized input is rejected before decoding");

    ConnectionCode invalid = source;
    invalid.endpoints[0].port = 0;
    check(encodeConnectionCode(invalid).empty(), "port zero cannot be encoded");
    invalid = source;
    invalid.endpoints.clear();
    check(encodeConnectionCode(invalid).empty(), "an invitation requires a reachable endpoint");
    invalid = source;
    invalid.joinSecret.fill(0);
    check(encodeConnectionCode(invalid).empty(), "an all-zero join secret cannot be encoded");

    ConnectionCode generated;
    const auto fingerprint = source.hostFingerprint;
    check(createConnectionCode(2, 1, 1000, 300, fingerprint, source.endpoints, generated)
              == ConnectionCodeError::None,
          "host invitation is created from the operating system CSPRNG");
    check(generated.expiresUnixSeconds == 1300 && generated.protocolMajor == 2,
          "generated invitation carries protocol and expiration");
    check(generated.sessionNonce != ConnectionCode{}.sessionNonce
              && generated.joinSecret != ConnectionCode{}.joinSecret,
          "generated invitation has non-zero security material");
    ConnectionCode generatedAgain;
    check(createConnectionCode(2, 1, 1000, 300, fingerprint, source.endpoints, generatedAgain)
              == ConnectionCodeError::None
              && generatedAgain.sessionNonce != generated.sessionNonce
              && generatedAgain.joinSecret != generated.joinSecret,
          "separate invitations receive separate security material");

    std::printf("nectar_net_connection_code_test: %s\n", failures == 0 ? "all tests passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
