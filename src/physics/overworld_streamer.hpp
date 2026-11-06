#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace scrapds {
class WorldPhysics;
class WorldCollision;
class WorldState;

struct TerrainAnchor {
    std::uint16_t world_id{};
    double x{},y{};
    std::uint8_t priority{};
};

// Experimental on-demand collision streaming for saved Survival overworlds.
// Only cells in the active area are resident; unsupported/missing tiles are counted.
class OverworldTerrainStreamer {
public:
    OverworldTerrainStreamer(const WorldState& state,const std::filesystem::path& game,std::size_t max_resident_cells=512);
    OverworldTerrainStreamer(OverworldTerrainStreamer&&) noexcept;
    OverworldTerrainStreamer& operator=(OverworldTerrainStreamer&&) noexcept;
    ~OverworldTerrainStreamer();
    OverworldTerrainStreamer(const OverworldTerrainStreamer&)=delete;
    OverworldTerrainStreamer& operator=(const OverworldTerrainStreamer&)=delete;

    // Keeps cells near supplied positions and loads at most `budget` new meshes;
    // the return value counts cells attached or detached from the scene.
    std::size_t stream(WorldPhysics& physics,std::span<const TerrainAnchor> anchors,std::size_t budget=4);
    std::size_t stream(WorldCollision& collision,std::span<const TerrainAnchor> anchors,std::size_t budget=4);
    // Reattaches all resident meshes after a physics scene rebuild.
    void attach(WorldPhysics& physics) const;
    void attach(WorldCollision& collision) const;
    std::string summary_json() const;
private:
    struct Impl;
    template<class Scene> std::size_t stream_impl(Scene& scene,std::span<const TerrainAnchor> anchors,std::size_t budget);
    template<class Scene> void attach_impl(Scene& scene) const;
    std::unique_ptr<Impl> impl_;
};
}
