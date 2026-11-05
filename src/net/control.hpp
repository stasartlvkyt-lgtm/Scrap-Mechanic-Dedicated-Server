#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scrapds::net {

// Observed pre-game control messages. These are sent directly through SNS,
// outside the byte-0-preserving LZ4 packet wrapper. This is only the control
// preamble; version/mod checks and game initialization follow separately.
enum class ControlKind : std::uint8_t { challenge = 3, authentication = 4, accepted = 5 };

struct ControlMessage {
    ControlKind kind{};
    // Meaningful only for authentication. Password length is bytes, not UTF-8
    // code points; the wire representation is not NUL terminated.
    std::string password;
    std::uint64_t secret{};
};

std::array<std::uint8_t, 1> make_control_challenge() noexcept;
std::array<std::uint8_t, 1> make_control_acceptance() noexcept;
std::vector<std::uint8_t> make_control_authentication(std::string_view password, std::uint64_t secret);

// Strict size/identifier validation; throws std::invalid_argument on malformed
// input or an unknown control ID. No password/identity authentication policy is
// applied by parsing or serializing a message.
ControlMessage parse_control(std::span<const std::uint8_t> packet);

} // namespace scrapds::net
