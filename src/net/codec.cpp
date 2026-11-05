#include "net/codec.hpp"

#include <lz4.h>

#include <stdexcept>

namespace scrapds::net {
namespace {
void validate_packet(std::span<const std::uint8_t> packet) {
    if (packet.empty()) throw std::invalid_argument("packet is empty");
    if (packet.size() > max_packet_bytes)
        throw std::invalid_argument("packet exceeds the 64 MiB hard limit");
}
} // namespace

std::vector<std::uint8_t> compress_packet(std::span<const std::uint8_t> packet) {
    validate_packet(packet);
    if (packet.size() == 1) return {packet[0]};
    const auto input_size = static_cast<int>(packet.size() - 1);
    const auto capacity = LZ4_compressBound(input_size);
    if (capacity <= 0) throw std::runtime_error("LZ4 rejected the input size");
    // Restrict the destination even for a maximum-size incompressible input.
    // LZ4 reports failure instead of creating an oversized transport packet.
    const auto bounded_capacity = static_cast<int>(
        static_cast<std::size_t>(capacity) < max_packet_bytes - 1
        ? static_cast<std::size_t>(capacity) : max_packet_bytes - 1);
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(bounded_capacity) + 1);
    encoded[0] = packet[0];
    const auto size = LZ4_compress_default(
        reinterpret_cast<const char*>(packet.data() + 1),
        reinterpret_cast<char*>(encoded.data() + 1), input_size, bounded_capacity);
    if (size <= 0) throw std::runtime_error("compressed packet exceeds the hard limit or LZ4 failed");
    encoded.resize(static_cast<std::size_t>(size) + 1);
    return encoded;
}

std::vector<std::uint8_t> decompress_packet(
    std::span<const std::uint8_t> packet, std::size_t max_output) {
    validate_packet(packet);
    if (max_output == 0 || max_output > max_packet_bytes)
        throw std::invalid_argument("output limit must be between 1 byte and 64 MiB");
    if (packet.size() == 1) return {packet[0]};
    if (max_output == 1)
        throw std::runtime_error("compressed body cannot fit the output limit");
    std::vector<std::uint8_t> decoded(max_output);
    decoded[0] = packet[0];
    const auto size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(packet.data() + 1),
        reinterpret_cast<char*>(decoded.data() + 1),
        static_cast<int>(packet.size() - 1), static_cast<int>(max_output - 1));
    if (size < 0) throw std::runtime_error("invalid LZ4 block or decompressed packet exceeds output limit");
    decoded.resize(static_cast<std::size_t>(size) + 1);
    return decoded;
}

} // namespace scrapds::net
