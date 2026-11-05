#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace scrapds {
struct RigidBodyPrefix {
    std::uint32_t id;
    std::uint16_t world_id;
    std::uint8_t variant;
    // Bounds match the SQLite RTree in all eight inspected saves.
    std::array<float, 4> bounds_min_x_max_x_min_y_max_y;
    // These interpretations require live-pose or serializer validation.
    std::array<float, 4> candidate_quaternion;
    std::array<float, 3> candidate_position;
};
struct ChildShapePrefix {
    std::uint32_t id;
    std::uint32_t body_id;
    std::uint32_t repeated_body_id;
    std::uint8_t variant;
    std::string candidate_uuid;
};
// Bounded prefixes permit reading a tiny SQL substr instead of the full BLOB.
// stored_size is the original SQL length(data); invalid/truncated input throws.
RigidBodyPrefix decode_rigid_body_prefix(std::span<const std::uint8_t> prefix, std::size_t stored_size);
ChildShapePrefix decode_child_shape_prefix(std::span<const std::uint8_t> prefix, std::size_t stored_size);
std::string rigid_body_prefix_json(const RigidBodyPrefix& body);
std::string child_shape_prefix_json(const ChildShapePrefix& shape);
// Utility for bounded inspection; failures become an explicit status, never a
// fabricated decoded object. Unknown table names yield an empty string.
std::string inspect_blob_prefix_json(const std::string& table, const std::string& hex_prefix, std::size_t stored_size);
}
