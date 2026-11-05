#include "net/control.hpp"

#include <stdexcept>

namespace scrapds::net {

std::array<std::uint8_t, 1> make_control_challenge() noexcept { return {3}; }
std::array<std::uint8_t, 1> make_control_acceptance() noexcept { return {5}; }

std::vector<std::uint8_t> make_control_authentication(std::string_view password, std::uint64_t secret) {
    if (password.size() > 255) throw std::invalid_argument("control password exceeds 255 bytes");
    std::vector<std::uint8_t> packet;
    packet.reserve(password.size() + 10);
    packet.push_back(4);
    packet.push_back(static_cast<std::uint8_t>(password.size()));
    for (const unsigned char byte : password) packet.push_back(byte);
    for (unsigned int i = 0; i < 8; ++i) packet.push_back(static_cast<std::uint8_t>(secret >> (8 * i)));
    return packet;
}

ControlMessage parse_control(std::span<const std::uint8_t> packet) {
    if (packet.empty()) throw std::invalid_argument("empty control packet");
    ControlMessage message;
    switch (packet[0]) {
    case 3:
    case 5:
        if (packet.size() != 1) throw std::invalid_argument("control singleton has trailing data");
        message.kind = static_cast<ControlKind>(packet[0]);
        return message;
    case 4: {
        if (packet.size() < 10) throw std::invalid_argument("truncated control authentication packet");
        const std::size_t length = packet[1];
        if (packet.size() != length + 10) throw std::invalid_argument("control password length differs from packet size");
        message.kind = ControlKind::authentication;
        message.password.assign(reinterpret_cast<const char*>(packet.data() + 2), length);
        for (unsigned int i = 0; i < 8; ++i)
            message.secret |= static_cast<std::uint64_t>(packet[2 + length + i]) << (8 * i);
        return message;
    }
    default:
        throw std::invalid_argument("unknown pre-game control identifier");
    }
}

} // namespace scrapds::net
