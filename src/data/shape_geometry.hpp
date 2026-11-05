#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace scrapds {
enum class SavedShapeKind : std::uint8_t { block = 31, part = 32, scaled_part = 40 };
struct ShapeGeometry {
    std::uint32_t id{};
    std::uint32_t body_id{};
    std::string uuid;
    SavedShapeKind kind{};
    // Integer quarter-metre construction-grid coordinates, in the parent body.
    std::array<std::int32_t, 3> grid_position{};
    // Numeric 0xAABBGGRR: low byte red, then green, blue, alpha.
    std::uint32_t color_abgr{};
    // Blocks and scaled parts contain dimensions; ordinary parts use assets.
    std::optional<std::array<std::int32_t, 3>> grid_dimensions;
    std::array<std::int32_t, 3> x_axis{1, 0, 0};
    std::array<std::int32_t, 3> z_axis{0, 0, 1};
    // Raw final serialized state byte; semantics not yet claimed. Old parts
    // omit this byte and are represented with nullopt.
    std::optional<std::uint8_t> state_flags;
};

// Returns nullopt for unsupported type/version. Malformed known records throw.
// Recovers construction placement and block dimensions, not part mesh geometry.
std::optional<ShapeGeometry> decode_child_geometry(std::span<const std::uint8_t> bytes);

struct JointGeometry {
    std::uint32_t id{},shape_a{},shape_b{};
    std::uint8_t kind{};
    std::string uuid;
    std::array<std::int32_t,3> grid_a{},grid_b{},axis_a{},axis_b{},reference_a{},reference_b{};
};
// Anchor grid points are body-local; their centre is grid*0.25+0.125.
std::optional<JointGeometry> decode_joint_geometry(std::span<const std::uint8_t> bytes);
// Returns the persisted stiffness index for an unlinked SurvivalSpringController.
std::optional<std::uint8_t> decode_survival_spring_setting(std::span<const std::uint8_t> bytes,std::uint32_t controller_id,std::uint32_t joint_id);
struct PistonSettings { std::uint8_t length_index{},speed_index{}; };
// Decode the compact, unlinked survival piston controller without altering it.
std::optional<PistonSettings> decode_survival_piston_settings(std::span<const std::uint8_t> bytes,std::uint32_t controller_id,std::uint32_t joint_id);

struct RigidBodyPose {
    std::uint32_t id{};
    std::uint16_t world_id{};
    bool is_static{};
    std::uint16_t repeated_world_id{};
    std::array<float, 4> rotation_xyzw{};
    std::array<float, 3> position{};
    // Native DynamicBody serialization order, confirmed against the Lua
    // velocity/angularVelocity getter table and BodyPhysicsProxy fields.
    std::optional<std::array<float, 3>> dynamic_vector0;
    std::optional<std::array<float, 3>> dynamic_vector1;
    const std::optional<std::array<float, 3>>& linear_velocity() const noexcept { return dynamic_vector0; }
    const std::optional<std::array<float, 3>>& angular_velocity() const noexcept { return dynamic_vector1; }
};
std::optional<RigidBodyPose> decode_rigid_body_pose(std::span<const std::uint8_t> bytes);
}
