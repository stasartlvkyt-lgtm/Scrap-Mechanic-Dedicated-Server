#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace scrapds::net {
// Input is a complete DECOMPRESSED application packet, including its ID byte.
// These layouts are build-bound static findings, not authorization decisions.
struct ShapeSelection {
    std::uint32_t body_id{};
    std::uint32_t child_shape_id{}; // 0 selects a block region; >0 a child shape.
    std::uint8_t kind{4};
    std::array<std::int32_t, 3> corner_a{};
    std::array<std::int32_t, 3> corner_b{};
    bool operator==(const ShapeSelection&) const = default;
};
struct RemoveShapeCommand {
    ShapeSelection selection;
    bool operator==(const RemoveShapeCommand&) const = default;
};
struct RemoveJointCommand {
    std::uint32_t joint_id{};
    bool operator==(const RemoveJointCommand&) const = default;
};
struct PaintShapeCommand {
    ShapeSelection selection;
    // Host uint32 packing: R in bits0..7, G8..15, B16..23, A24..31.
    // 0 requests the asset's default color in the original handler.
    std::uint32_t color_rgba{};
    bool operator==(const PaintShapeCommand&) const = default;
};
struct PaintJointCommand {
    std::uint32_t joint_id{}, color_rgba{};
    bool operator==(const PaintJointCommand&) const = default;
};
struct PaintCharacterCommand {
    std::uint32_t character_id{}, color_rgba{};
    bool operator==(const PaintCharacterCommand&) const = default;
};
struct BuildShapeCommand {
    std::uint32_t body_id{}, child_shape_id{};
    std::uint8_t kind{}; // Observed: 2=new placement, 4=body/shape target, 8=joint target.
    // Canonical UUID bytes, in the order of the asset's hexadecimal text.
    std::array<std::uint8_t,16> shape_uuid{}, inventory_uuid{};
    std::array<std::int32_t,3> cell_position{};
    // Integer orientation vectors: A=x axis, B=z axis (constructor and save
    // serializer corroborate the object offsets +72/+60 respectively).
    std::array<std::int32_t,3> axis_a{}, axis_b{};
    std::array<std::int32_t,3> auxiliary_position{}; // Semantics still unknown.
    std::array<std::int32_t,3> block_dimensions{};
    std::uint32_t quantity{};
    std::int32_t inventory_slot{};
    bool operator==(const BuildShapeCommand&) const = default;
};
using GameCommand = std::variant<RemoveShapeCommand, RemoveJointCommand, PaintShapeCommand,
                                 PaintJointCommand, PaintCharacterCommand, BuildShapeCommand>;

// Throws invalid_argument for unknown IDs or any truncated/trailing payload.
// The codec currently covers application IDs 22-27.
// Semantic checks (world, ownership, geometry, inventory cost) belong to game/.
GameCommand parse_game_command(std::span<const std::uint8_t> plaintext);
std::vector<std::uint8_t> serialize_game_command(const GameCommand& command);
}
