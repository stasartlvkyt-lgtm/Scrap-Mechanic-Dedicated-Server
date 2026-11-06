#pragma once
#include "physics/simulation.hpp"
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace scrapds {
class WorldState;
class AssetDefinitions;
struct TerrainVertex;
struct OverworldCell;
struct OverworldTerrain;
struct WorldPhysicsCoverage {
    std::uint64_t source_bodies{}, static_bodies{}, dynamic_bodies{}, box_parts{}, cylinder_parts{}, hull_parts{}, bearing_joints{}, spring_joints{}, passive_controllers{}, skipped_geometry{}, skipped_joint{}, skipped_controller{}, skipped_unmapped{}, skipped_invalid{}, terrain_chunks{}, terrain_triangles{}, flat_surface_worlds{};
};
struct WorldPhysicsBody {
    std::uint32_t id{};
    std::uint16_t world_id{};
    Vec3 position, velocity, angular_velocity;
    double qx{},qy{},qz{},qw{1};
};
struct WorldPhysicsBounds { Vec3 minimum,maximum; };
struct WorldPhysicsCapsuleMotion { Vec3 position; std::uint32_t contacts{}; };

// Experimental Bullet simulation of complete supported bodies, free bearings
// and supported suspension groups. Material parameters are approximate;
// non-flat world terrain, motors and unsupported joints remain excluded.
class WorldPhysics {
public:
    static WorldPhysics from_world(const WorldState& state,double density=1000.0,const std::set<std::uint32_t>* mapped_dynamic_ids=nullptr,const AssetDefinitions* assets=nullptr);
    WorldPhysics(WorldPhysics&&) noexcept;
    WorldPhysics& operator=(WorldPhysics&&) noexcept;
    ~WorldPhysics();
    WorldPhysics(const WorldPhysics&)=delete;
    WorldPhysics& operator=(const WorldPhysics&)=delete;
    const WorldPhysicsCoverage& coverage() const;
    void step(std::uint64_t ticks);
    // Values are world-space; relative_position is measured from Bullet's
    // centre of mass. Forces act on the next step, impulses act immediately.
    void apply_impulse(std::uint32_t body_id,Vec3 impulse,Vec3 relative_position={});
    void apply_force(std::uint32_t body_id,Vec3 force,Vec3 relative_position={});
    // Sweeps and slides a vertical capsule against the current simulated
    // bodies and resident terrain in its world.
    WorldPhysicsCapsuleMotion move_character_capsule(std::uint16_t world_id,Vec3 start,Vec3 displacement,double radius,double cylinder_height,double skin=0.001,unsigned max_iterations=4) const;
    // Explicitly supplied terrain geometry; tile-to-world mapping remains caller-owned.
    void add_terrain_mesh(std::uint16_t world_id,std::span<const TerrainVertex> triangle_vertices);
    void add_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y,std::span<const TerrainVertex> triangle_vertices);
    void remove_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y);
    // Builds a single saved Survival cell before adding its collision mesh.
    void add_saved_overworld_cell(std::uint16_t world_id,const OverworldTerrain& terrain,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog,const OverworldCell& cell);
    std::vector<WorldPhysicsBody> bodies() const;
    std::optional<WorldPhysicsBounds> bounds(std::uint32_t body_id) const;
    // Writes only simulated dynamic bodies; applies the batch atomically.
    void writeback(WorldState& state) const;
    std::string summary_json() const;
private:
    struct Impl;
    explicit WorldPhysics(std::unique_ptr<Impl> impl);
    void add_terrain_mesh_impl(std::uint16_t world_id,std::optional<std::pair<std::int32_t,std::int32_t>> cell,std::span<const TerrainVertex> vertices);
    std::unique_ptr<Impl> impl_;
};
}
