#include "physics/overworld_streamer.hpp"
#include "data/state.hpp"
#include "data/terrain.hpp"
#include "physics/world_collision.hpp"
#include "physics/world_physics.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace scrapds {
namespace {
using CellKey=std::tuple<std::uint16_t,std::int32_t,std::int32_t>;
struct SavedWorld {
    OverworldTerrain terrain;
    std::map<std::pair<std::int32_t,std::int32_t>,OverworldCell> cells;
};
struct Candidate {std::uint8_t priority{},distance{};CellKey key;};
}
struct OverworldTerrainStreamer::Impl {
    std::map<std::uint16_t,SavedWorld> worlds;
    std::map<std::array<std::uint8_t,16>,std::filesystem::path> catalog;
    std::optional<TerrainMeshTables> mesh_tables;
    std::vector<TerrainDelta> deltas;
    std::map<CellKey,std::vector<TerrainVertex>> resident;
    std::set<CellKey> voxel_resident;
    std::set<CellKey> failed;
    std::size_t limit{},unsupported_worlds{},dropped_targets{},unmapped_deltas{};
};

OverworldTerrainStreamer::OverworldTerrainStreamer(const WorldState& state,const std::filesystem::path& game,std::size_t max_resident_cells):impl_(std::make_unique<Impl>()) {
    if(!max_resident_cells||max_resident_cells>4096)throw std::invalid_argument("invalid-overworld-terrain-resident-limit");
    impl_->limit=max_resident_cells;impl_->catalog=index_game_terrain_tiles(game);
    for(const auto id:state.terrain_generator_worlds())try {
        const auto blob=state.terrain_generator_data(id);if(!blob)continue;
        SavedWorld saved;saved.terrain=decode_overworld_terrain(*blob);
        for(const auto& cell:saved.terrain.cells)if(!saved.cells.emplace(std::pair{cell.x,cell.y},cell).second)throw std::runtime_error("duplicate-overworld-cell");
        if(!saved.cells.empty())impl_->worlds.emplace(id,std::move(saved));
    } catch(const std::runtime_error&) {
        // Keep the remaining supported worlds available; skipped terrain is
        // visible in summary_json and never replaced with fabricated geometry.
        ++impl_->unsupported_worlds;
    }
    if(!impl_->worlds.empty()) {
        impl_->deltas=state.terrain_deltas();
        const auto region=[](std::int32_t chunk){auto value=std::int64_t(chunk)/4;if(chunk<0&&chunk%4)--value;return value;};
        std::erase_if(impl_->deltas,[&](const auto& delta){
            const bool mapped=region(delta.chunk_position[0])==delta.region_x&&region(delta.chunk_position[1])==delta.region_y;
            if(!mapped)++impl_->unmapped_deltas;
            return !mapped;
        });
        auto executable=game;
        if(std::filesystem::is_directory(game)) {
            const auto root_exe=game/L"ScrapMechanic.exe",release_exe=game/L"Release"/L"ScrapMechanic.exe";
            if(std::filesystem::is_regular_file(root_exe))executable=root_exe;
            else executable=release_exe;
        }
        if(std::filesystem::is_regular_file(executable))impl_->mesh_tables=TerrainMeshTables::from_game_executable(executable);
    }
}
OverworldTerrainStreamer::OverworldTerrainStreamer(OverworldTerrainStreamer&&) noexcept=default;
OverworldTerrainStreamer& OverworldTerrainStreamer::operator=(OverworldTerrainStreamer&&) noexcept=default;
OverworldTerrainStreamer::~OverworldTerrainStreamer()=default;

template<class Scene>
std::size_t OverworldTerrainStreamer::stream_impl(Scene& physics,std::span<const TerrainAnchor> anchors,std::size_t budget) {
    if(!budget)return 0;
    std::map<CellKey,std::pair<std::uint8_t,std::uint8_t>> targets;
    for(const auto& anchor:anchors) {
        if(!std::isfinite(anchor.x)||!std::isfinite(anchor.y)||std::abs(anchor.x)>double(std::numeric_limits<std::int32_t>::max())*64||std::abs(anchor.y)>double(std::numeric_limits<std::int32_t>::max())*64)continue;
        if(!impl_->worlds.contains(anchor.world_id))continue;
        const auto cx=static_cast<std::int32_t>(std::floor(anchor.x/64.0)),cy=static_cast<std::int32_t>(std::floor(anchor.y/64.0));
        const auto& world=impl_->worlds.at(anchor.world_id);
        if(!world.cells.contains({cx,cy}))continue;
        for(std::int32_t dx=-1;dx<=1;++dx)for(std::int32_t dy=-1;dy<=1;++dy) {
            if((dx<0&&cx==INT32_MIN)||(dx>0&&cx==INT32_MAX)||(dy<0&&cy==INT32_MIN)||(dy>0&&cy==INT32_MAX))continue;
            const CellKey key{anchor.world_id,cx+dx,cy+dy};const auto candidate=std::pair{anchor.priority,static_cast<std::uint8_t>(dx*dx+dy*dy)};
            if(!world.cells.contains({cx+dx,cy+dy}))continue;
            const auto [it,inserted]=targets.emplace(key,candidate);if(!inserted)it->second=std::min(it->second,candidate);
        }
    }
    std::vector<Candidate> ordered;ordered.reserve(targets.size());for(const auto& [key,ranking]:targets)ordered.push_back({ranking.first,ranking.second,key});
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return std::tie(a.priority,a.distance,a.key)<std::tie(b.priority,b.distance,b.key);});
    impl_->dropped_targets=ordered.size()>impl_->limit?ordered.size()-impl_->limit:0;
    if(ordered.size()>impl_->limit)ordered.resize(impl_->limit);
    targets.clear();for(const auto& item:ordered)targets.emplace(item.key,std::pair{item.priority,item.distance});
    std::size_t removed{};
    for(auto it=impl_->resident.begin();it!=impl_->resident.end();) {
        if(!targets.contains(it->first)) {
            const auto [world_id,x,y]=it->first;physics.remove_overworld_cell_mesh(world_id,x,y);impl_->voxel_resident.erase(it->first);it=impl_->resident.erase(it);++removed;
        } else ++it;
    }
    std::size_t added{};
    for(const auto& target:ordered) {
        if(added>=budget||impl_->resident.size()>=impl_->limit)break;
        if(impl_->resident.contains(target.key)||impl_->failed.contains(target.key))continue;
        const auto [world_id,x,y]=target.key;const auto world=impl_->worlds.find(world_id);if(world==impl_->worlds.end())continue;
        const auto cell=world->second.cells.find({x,y});if(cell==world->second.cells.end()){impl_->failed.insert(target.key);continue;}
        try {
            auto mesh=mesh_saved_overworld_cell(world->second.terrain,impl_->catalog,cell->second);
            if(mesh.empty())throw std::runtime_error("empty-overworld-collision-mesh");
            bool has_voxel_mesh=false;
            const auto chunks=read_overworld_tile_voxels(cell->second,impl_->catalog);
            if(!chunks.empty()) {
                if(!impl_->mesh_tables)throw std::runtime_error("game terrain mesh tables unavailable");
                auto voxel_mesh=mesh_overworld_tile_voxels(cell->second,chunks,world_id,impl_->deltas,*impl_->mesh_tables);
                if(voxel_mesh.size()>3000000-mesh.size())throw std::runtime_error("overworld voxel collision exceeds per-cell mesh limit");
                has_voxel_mesh=!voxel_mesh.empty();
                mesh.insert(mesh.end(),voxel_mesh.begin(),voxel_mesh.end());
            }
            physics.add_overworld_cell_mesh(world_id,x,y,mesh);impl_->resident.emplace(target.key,std::move(mesh));if(has_voxel_mesh)impl_->voxel_resident.insert(target.key);++added;
        } catch(const std::runtime_error&) {impl_->failed.insert(target.key);}
    }
    return added+removed;
}
std::size_t OverworldTerrainStreamer::stream(WorldPhysics& physics,std::span<const TerrainAnchor> anchors,std::size_t budget){return stream_impl(physics,anchors,budget);}
std::size_t OverworldTerrainStreamer::stream(WorldCollision& collision,std::span<const TerrainAnchor> anchors,std::size_t budget){return stream_impl(collision,anchors,budget);}
template<class Scene>
void OverworldTerrainStreamer::attach_impl(Scene& physics) const {
    for(const auto& [key,mesh]:impl_->resident)physics.add_overworld_cell_mesh(std::get<0>(key),std::get<1>(key),std::get<2>(key),mesh);
}
void OverworldTerrainStreamer::attach(WorldPhysics& physics) const {attach_impl(physics);}
void OverworldTerrainStreamer::attach(WorldCollision& collision) const {attach_impl(collision);}
std::string OverworldTerrainStreamer::summary_json() const {
    std::size_t source_cells{};for(const auto& [id,world]:impl_->worlds){(void)id;source_cells+=world.cells.size();}
    return "{\"worlds\":"+std::to_string(impl_->worlds.size())+",\"unsupported_worlds\":"+std::to_string(impl_->unsupported_worlds)+",\"source_cells\":"+std::to_string(source_cells)+",\"terrain_delta_chunks\":"+std::to_string(impl_->deltas.size())+",\"unmapped_terrain_deltas\":"+std::to_string(impl_->unmapped_deltas)+",\"resident_cells\":"+std::to_string(impl_->resident.size())+",\"resident_voxel_cells\":"+std::to_string(impl_->voxel_resident.size())+",\"cell_limit\":"+std::to_string(impl_->limit)+",\"deferred_target_cells\":"+std::to_string(impl_->dropped_targets)+",\"unavailable_cells\":"+std::to_string(impl_->failed.size())+"}";
}
}
