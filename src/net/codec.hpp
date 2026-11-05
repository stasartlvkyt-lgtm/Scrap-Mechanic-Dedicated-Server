#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace scrapds::net {

inline constexpr std::size_t max_packet_bytes = 64u * 1024u * 1024u;

// Observed wrapper framing: byte 0 unchanged, remaining bytes a raw LZ4 block.
// These functions do not implement message schemas, split packets or handshake.
// A packet containing only its identifier is returned unchanged; empty input is
// invalid. Errors throw std::invalid_argument or std::runtime_error. The hard
// bound applies to input/output sizes including the preserved identifier.
std::vector<std::uint8_t> compress_packet(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> decompress_packet(
    std::span<const std::uint8_t> packet,
    std::size_t max_output = 8u * 1024u * 1024u);

} // namespace scrapds::net
