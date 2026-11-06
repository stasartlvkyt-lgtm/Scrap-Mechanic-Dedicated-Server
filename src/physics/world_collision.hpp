#pragma once
#include "simulation.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scrapds {
class WorldState;
struct TerrainVertex;
struct CollisionBlock {
    std::uint32_t shape_id{}, body_id{};
    std::uint16_t world_id{};
    std::array<std::int32_t, 3> grid_position{};
    std::array<std::int32_t, 3> grid_dimensions{};
    Vec3 body_position;
    std::array<double, 4> body_rotation_xyzw{0, 0, 0, 1};
};
struct CollisionCoverage {
    std::uint64_t source_shapes{}, block_boxes{}, terrain_triangles{}, unsupported_parts{},
                  unsupported_shapes{}, degenerate_blocks{}, unsupported_bodies{}, unsupported_joints{}, worlds{};
};
struct CollisionBounds { Vec3 minimum, maximum; };
struct CollisionRayHit {
    std::uint32_t shape_id{}, body_id{};
    std::uint16_t world_id{};
    Vec3 point, normal;
    double fraction{};
};
struct CapsuleMotion {
    Vec3 position;
    std::vector<CollisionRayHit> contacts;
};

// Snapshot of decoded block collision geometry and resident static terrain. No part meshes,
// dynamics/mass, constraints, character controllers, or gameplay simulation.
class WorldCollision {
public:
    static WorldCollision from_world(const WorldState& state);
    static WorldCollision from_blocks(const std::vector<CollisionBlock>& blocks);
    WorldCollision(WorldCollision&&) noexcept;
    WorldCollision& operator=(WorldCollision&&) noexcept;
    ~WorldCollision();
    WorldCollision(const WorldCollision&) = delete;
    WorldCollision& operator=(const WorldCollision&) = delete;

    const CollisionCoverage& coverage() const;
    std::optional<CollisionRayHit> raycast(std::uint16_t world_id, Vec3 start, Vec3 end) const;
    // Sweep a vertical capsule by its centre through decoded block geometry.
    // cylinder_height excludes the hemispherical caps; this query does not
    // cover terrain, parts, or moving bodies omitted from this snapshot.
    std::optional<CollisionRayHit> sweep_capsule(std::uint16_t world_id, Vec3 start, Vec3 end,double radius,double cylinder_height) const;
    // Sweep and slide through this collision snapshot; initial overlaps and
    // unsupported world geometry require a separate recovery/controller path.
    CapsuleMotion move_capsule(std::uint16_t world_id, Vec3 start, Vec3 displacement,double radius,double cylinder_height,double skin=0.001,unsigned max_iterations=4) const;
    void add_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y,std::span<const TerrainVertex> vertices);
    void remove_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y);
    std::optional<CollisionBounds> body_bounds(std::uint16_t world_id, std::uint32_t body_id) const;
    std::string summary_json() const;
private:
    struct Impl;
    explicit WorldCollision(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
