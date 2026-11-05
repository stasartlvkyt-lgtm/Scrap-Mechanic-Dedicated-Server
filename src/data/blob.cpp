#include "blob.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace scrapds {
namespace {
std::uint32_t be32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Truncated uint32");
    return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at+1]) << 16) | (std::uint32_t(bytes[at+2]) << 8) | bytes[at+3];
}
float befloat(std::span<const std::uint8_t> bytes, std::size_t at) {
    const auto value = std::bit_cast<float>(be32(bytes, at));
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite float");
    return value;
}
template <std::size_t N> std::string array_json(const std::array<float, N>& values) {
    std::ostringstream out; out << '[' << std::setprecision(9);
    for (std::size_t i = 0; i < N; ++i) { if (i) out << ','; out << values[i]; }
    return out.str() + ']';
}
}

RigidBodyPrefix decode_rigid_body_prefix(std::span<const std::uint8_t> prefix, std::size_t stored_size) {
    if (prefix.size() < 55 || prefix.size() > stored_size) throw std::runtime_error("Truncated RigidBody prefix");
    if (prefix[0] != 0 || prefix[2] != 1 || !((prefix[1] == 1 && stored_size == 60) || (prefix[1] == 2 && stored_size == 80))) throw std::runtime_error("Unknown RigidBody variant");
    RigidBodyPrefix body{};
    body.id = be32(prefix, 3);
    body.world_id = (std::uint16_t(prefix[7]) << 8) | prefix[8];
    body.variant = prefix[1];
    body.bounds_min_x_max_x_min_y_max_y = {befloat(prefix,21),befloat(prefix,17),befloat(prefix,13),befloat(prefix,9)};
    if (body.bounds_min_x_max_x_min_y_max_y[0] > body.bounds_min_x_max_x_min_y_max_y[1] || body.bounds_min_x_max_x_min_y_max_y[2] > body.bounds_min_x_max_x_min_y_max_y[3]) throw std::runtime_error("Inverted RigidBody bounds");
    for (std::size_t i = 0; i < 4; ++i) body.candidate_quaternion[i] = befloat(prefix,27+4*i);
    for (std::size_t i = 0; i < 3; ++i) body.candidate_position[i] = befloat(prefix,43+4*i);
    return body;
}

ChildShapePrefix decode_child_shape_prefix(std::span<const std::uint8_t> prefix, std::size_t stored_size) {
    if (prefix.size() < 31 || prefix.size() > stored_size) throw std::runtime_error("Truncated ChildShape prefix");
    if (prefix[0] != 1 || prefix[2] != 1 || !((prefix[1] == 31 && stored_size == 48) || (prefix[1] == 32 && (stored_size == 42 || stored_size == 43)) || (prefix[1] == 40 && stored_size == 49))) throw std::runtime_error("Unknown ChildShape variant");
    ChildShapePrefix shape{};
    shape.id = be32(prefix,3); shape.body_id = be32(prefix,7); shape.repeated_body_id = be32(prefix,27); shape.variant = prefix[1];
    if (shape.body_id != shape.repeated_body_id) throw std::runtime_error("ChildShape body IDs disagree");
    std::ostringstream uuid;
    uuid << std::hex << std::setfill('0');
    for (int i=0; i<16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) uuid << '-';
        uuid << std::setw(2) << unsigned(prefix[26-i]);
    }
    shape.candidate_uuid = uuid.str();
    return shape;
}

std::string rigid_body_prefix_json(const RigidBodyPrefix& body) {
    return "{\"status\":\"partial\",\"evidence\":\"eight-save-row-and-rtree-comparison\",\"id\":" + std::to_string(body.id) + ",\"world_id\":" + std::to_string(body.world_id) + ",\"variant\":" + std::to_string(body.variant) + ",\"verified_bounds_min_x_max_x_min_y_max_y\":" + array_json(body.bounds_min_x_max_x_min_y_max_y) + ",\"candidate_pose\":{\"confidence\":\"inferred-not-runtime-verified\",\"quaternion_component_order\":\"unknown\",\"quaternion\":" + array_json(body.candidate_quaternion) + ",\"position\":" + array_json(body.candidate_position) + "},\"collision_geometry\":\"not-decoded\"}";
}

std::string child_shape_prefix_json(const ChildShapePrefix& shape) {
    return "{\"status\":\"partial\",\"id\":" + std::to_string(shape.id) + ",\"body_id\":" + std::to_string(shape.body_id) + ",\"variant\":" + std::to_string(shape.variant) + ",\"candidate_uuid\":\"" + shape.candidate_uuid + "\",\"uuid_confidence\":\"two-asset-definition-cross-checks\",\"placement_and_geometry\":\"not-decoded\"}";
}

std::string inspect_blob_prefix_json(const std::string& table, const std::string& hex_prefix, std::size_t stored_size) {
    if (table != "RigidBody" && table != "ChildShape") return {};
    try {
        if (hex_prefix.size() % 2) throw std::runtime_error("Odd hex length");
        std::vector<std::uint8_t> bytes;
        auto nibble = [](char c) -> unsigned {
            if (c >= '0' && c <= '9') return unsigned(c-'0');
            if (c >= 'a' && c <= 'f') return unsigned(c-'a'+10);
            if (c >= 'A' && c <= 'F') return unsigned(c-'A'+10);
            throw std::runtime_error("Invalid hex");
        };
        for (std::size_t i = 0; i < hex_prefix.size(); i += 2) bytes.push_back(std::uint8_t((nibble(hex_prefix[i])<<4)|nibble(hex_prefix[i+1])));
        if (table == "RigidBody") return rigid_body_prefix_json(decode_rigid_body_prefix(bytes, stored_size));
        return child_shape_prefix_json(decode_child_shape_prefix(bytes, stored_size));
    } catch (const std::runtime_error&) {
        return "{\"status\":\"unknown-or-invalid-variant\"}";
    }
}
}
