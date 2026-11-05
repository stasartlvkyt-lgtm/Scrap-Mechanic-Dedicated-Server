#include "net/control.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void invalid(Function operation, const char* message) {
    try { operation(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
}

int main() {
    try {
        using namespace scrapds::net;
        // Hand-derived vector from the verified stores in 0x466788..0x4667B7:
        // ID4, password-byte-count, literal bytes, little-endian secret.
        const std::vector<std::uint8_t> golden{4,3,'a','b','c',0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11};
        const auto auth = parse_control(golden);
        require(auth.kind == ControlKind::authentication && auth.password == "abc" && auth.secret == 0x1122334455667788ull,
                "static authentication vector decoded incorrectly");
        require(make_control_authentication("abc", 0x1122334455667788ull) == golden, "static authentication vector encoded incorrectly");
        const std::vector<std::uint8_t> empty_password{4,0,0,0,0,0,0,0,0,0};
        require(make_control_authentication("", 0) == empty_password, "empty password framing differs");
        require(parse_control(empty_password).password.empty(), "empty password rejected");
        require(make_control_challenge() == std::array<std::uint8_t,1>{3}, "challenge must be exactly byte03");
        require(make_control_acceptance() == std::array<std::uint8_t,1>{5}, "acceptance must be exactly byte05");
        require(parse_control(make_control_challenge()).kind == ControlKind::challenge, "challenge parse failed");
        require(parse_control(make_control_acceptance()).kind == ControlKind::accepted, "acceptance parse failed");
        std::string maximum(255, '\0');
        for (std::size_t i=0; i<maximum.size(); ++i) maximum[i] = static_cast<char>(i);
        const auto maximum_packet = make_control_authentication(maximum, UINT64_MAX);
        require(maximum_packet.size() == 265 && maximum_packet[1] == 255, "maximum password framing differs");
        const auto maximum_auth = parse_control(maximum_packet);
        require(maximum_auth.password == maximum && maximum_auth.secret == UINT64_MAX, "binary password bytes changed");
        invalid([&] { make_control_authentication(std::string(256, 'x'), 0); }, "oversized password accepted");
        for (std::size_t i=0; i<golden.size(); ++i)
            invalid([&] { parse_control(std::span<const std::uint8_t>(golden).first(i)); }, "truncated authentication accepted");
        auto trailing = golden; trailing.push_back(0);
        invalid([&] { parse_control(trailing); }, "trailing authentication byte accepted");
        for (const std::uint8_t id : {0,1,2,6,255})
            invalid([&] { const std::array packet{id}; parse_control(packet); }, "unknown control ID accepted");
        for (const std::uint8_t id : {3,5})
            invalid([&] { const std::array<std::uint8_t,2> packet{id,0}; parse_control(packet); }, "trailing singleton byte accepted");
        auto false_length = golden; false_length[1] = 4;
        invalid([&] { parse_control(false_length); }, "inconsistent password length accepted");
        std::cout << "control_tests: static vectors and malformed bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "control_tests: " << error.what() << '\n';
        return 1;
    }
}
