#include "net/codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void require_failure(Function&& operation, const char* message) {
    bool failed = false;
    try { operation(); }
    catch (const std::invalid_argument&) { failed = true; }
    catch (const std::runtime_error&) { failed = true; }
    require(failed, message);
}

std::uint32_t read_u32(std::span<const std::uint8_t> source, std::size_t offset) {
    require(offset + 4 <= source.size(), "truncated fixture size");
    return static_cast<std::uint32_t>(source[offset]) |
        (static_cast<std::uint32_t>(source[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(source[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(source[offset + 3]) << 24);
}

bool golden_test(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot open game-generated fixture");
    const std::vector<std::uint8_t> fixture(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::array<std::uint8_t, 8> magic{'S', 'D', 'S', 'C', 'O', 'D', '0', '1'};
    require(fixture.size() >= 16, "fixture header truncated");
    require(std::equal(magic.begin(), magic.end(), fixture.begin()), "fixture magic differs");
    const auto source_size = read_u32(fixture, 8);
    const auto compressed_size = read_u32(fixture, 12);
    require(source_size == 4097 && compressed_size == 2074, "fixture sizes differ from native evidence");
    require(fixture.size() == 16u + source_size + compressed_size, "fixture length differs");
    const auto source = std::span<const std::uint8_t>(fixture).subspan(16, source_size);
    const auto compressed = std::span<const std::uint8_t>(fixture).subspan(16 + source_size, compressed_size);
    require(source[0] == 0x7f && compressed[0] == source[0], "game fixture identifier was not preserved");
    for (std::size_t i = 1; i < source.size(); ++i)
        require(source[i] == static_cast<std::uint8_t>((i * 17) ^ (i >> 3)), "synthetic fixture source differs");
    const auto decoded = scrapds::net::decompress_packet(compressed, source.size());
    require(std::equal(decoded.begin(), decoded.end(), source.begin(), source.end()), "native fixture decode differs");
    require_failure([&] { scrapds::net::decompress_packet(compressed, source.size() - 1); }, "native fixture ignored output cap");
    const auto encoded = scrapds::net::compress_packet(source);
    require(scrapds::net::decompress_packet(encoded, source.size()) == decoded, "re-encoded fixture does not decode");
    return std::equal(encoded.begin(), encoded.end(), compressed.begin(), compressed.end());
}

void roundtrip_tests() {
    std::mt19937 random(0x534453);
    const std::array<std::size_t, 13> lengths{1, 2, 3, 15, 16, 255, 256, 4096, 4097, 65535, 65536, 262144, 1048576};
    for (const auto length : lengths) {
        for (int pattern = 0; pattern < 3; ++pattern) {
            std::vector<std::uint8_t> source(length, 0);
            source[0] = 0xd3;
            for (std::size_t i = 1; i < length; ++i)
                source[i] = pattern == 0 ? 0 : pattern == 1
                    ? static_cast<std::uint8_t>((i * 17) ^ (i >> 3))
                    : static_cast<std::uint8_t>(random());
            const auto encoded = scrapds::net::compress_packet(source);
            require(encoded.front() == source.front(), "identifier changed");
            require(scrapds::net::decompress_packet(encoded, source.size()) == source, "round trip differs");
        }
    }
    for (const auto identifier : {0x00, 0x14, 0x7f, 0xff}) {
        const std::array<std::uint8_t, 1> source{static_cast<std::uint8_t>(identifier)};
        require(scrapds::net::compress_packet(source) == std::vector<std::uint8_t>(source.begin(), source.end()), "single byte compressed");
        require(scrapds::net::decompress_packet(source, 1) == std::vector<std::uint8_t>(source.begin(), source.end()), "single byte decompressed");
    }
}

void invalid_tests() {
    const std::vector<std::uint8_t> empty;
    require_failure([&] { scrapds::net::compress_packet(empty); }, "empty compress accepted");
    require_failure([&] { scrapds::net::decompress_packet(empty); }, "empty decompress accepted");
    const std::array<std::uint8_t, 1> single{1};
    require_failure([&] { scrapds::net::decompress_packet(single, 0); }, "zero output cap accepted");
    require_failure([&] { scrapds::net::decompress_packet(single, scrapds::net::max_packet_bytes + 1); }, "oversized output cap accepted");
    const std::vector<std::uint8_t> oversized(scrapds::net::max_packet_bytes + 1, 0);
    require_failure([&] { scrapds::net::compress_packet(oversized); }, "oversized source accepted");
    require_failure([&] { scrapds::net::decompress_packet(oversized); }, "oversized compressed input accepted");
    const std::array<std::uint8_t, 2> truncated_literals{0x7f, 0xf0};
    const std::array<std::uint8_t, 5> invalid_match{0x7f, 0x00, 0xff, 0xff, 0x00};
    require_failure([&] { scrapds::net::decompress_packet(truncated_literals, 4097); }, "truncated literals accepted");
    require_failure([&] { scrapds::net::decompress_packet(invalid_match, 4097); }, "invalid match accepted");
    require_failure([&] { scrapds::net::decompress_packet(truncated_literals, 1); }, "body accepted with one-byte output cap");
    std::vector<std::uint8_t> expansion(16384, 0x33);
    const auto compressed = scrapds::net::compress_packet(expansion);
    require_failure([&] { scrapds::net::decompress_packet(compressed, 32); }, "expansion ignored output cap");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: codec_tests <codec-game.bin>");
        const auto exact = golden_test(argv[1]);
        roundtrip_tests();
        invalid_tests();
        std::cout << "codec tests passed; upstream encode matches native fixture: "
                  << (exact ? "yes" : "no (valid alternative LZ4 encoding)") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "codec test failed: " << error.what() << '\n';
        return 1;
    }
}
