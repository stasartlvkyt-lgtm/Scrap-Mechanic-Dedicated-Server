#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace scrapds {
// Delta payload requires generated base terrain before it can provide geometry.
struct TerrainDelta {
    std::uint32_t id{};
    std::uint16_t world_id{};
    std::int32_t region_x{},region_y{};
    std::array<std::int32_t,3> chunk_position{};
    std::uint8_t kind{};
    std::vector<std::uint8_t> payload;
};
struct TerrainChange { std::array<std::uint8_t,3> position{};std::uint8_t density{};std::optional<std::uint8_t> material; };
struct TerrainTileChunk { std::array<std::int32_t,3> position{};std::array<std::uint8_t,4096> voxels{}; };
struct TerrainTileHeights { std::uint32_t spacing{},side{};std::vector<float> samples; };
struct OverworldCell {
    std::int32_t x{},y{},tile_x{},tile_y{},rotation{};
    // 128-bit UUID in the saved bitstream's byte order.
    std::array<std::uint8_t,16> tile_uuid{};
};
struct OverworldTerrain {
    std::int32_t seed{};
    std::vector<OverworldCell> cells;
    std::map<std::pair<std::int32_t,std::int32_t>,double> elevation,cliff;
    std::map<std::pair<std::int32_t,std::int32_t>,std::uint32_t> flags;
};
struct TerrainFlattenArea { double x{},y{},radius{}; };
std::int32_t terrain_int_noise_2d(std::int32_t x,std::int32_t y,std::int32_t seed);
// `nodes` must contain all FLATTEN nodes in the game's 3x3 search area.
std::vector<TerrainFlattenArea> overworld_cell_flatten_areas(const OverworldTerrain& terrain,std::int32_t cell_x,std::int32_t cell_y,std::span<const TerrainFlattenArea> nodes);
// Reads the supported subset of the game's LUA terrain table. Unknown value
// types and inconsistent grids are rejected, never silently fabricated.
std::vector<OverworldCell> decode_overworld_cells(std::span<const std::uint8_t> decoded);
OverworldTerrain decode_overworld_terrain(std::span<const std::uint8_t> decoded);
// Base elevation and cliff contribution, before FLATTEN nodes and random
// flattening are applied by the game's generator.
double unflattened_overworld_height(const OverworldTerrain& terrain,double x,double y);
double overworld_height_with_flatten_areas(const OverworldTerrain& terrain,double x,double y,std::span<const TerrainFlattenArea> areas);
// Indexes installed TILE headers by the UUID byte order stored in LUA tables.
std::map<std::array<std::uint8_t,16>,std::filesystem::path> index_game_terrain_tiles(const std::filesystem::path& game);
struct TerrainMeshTables {
    std::array<std::array<std::uint16_t,12>,256> edges{};
    std::array<std::uint8_t,256> patterns{};
    std::array<std::array<std::uint8_t,16>,16> triangles{};
    static TerrainMeshTables from_game_executable(const std::filesystem::path& path);
};
struct TerrainVertex {float x{},y{},z{};};
TerrainDelta decode_terrain_delta(std::span<const std::uint8_t> compressed);
// Even kinds carry low and high voxel bits; odd kinds carry only low bits.
// Kinds 4–7 use occupancy masks; 8–9 use sparse voxel addresses.
std::vector<TerrainChange> decode_terrain_density_changes(const TerrainDelta& delta);
// Applies a decoded delta to an existing 17x17x17 generated voxel chunk.
void apply_terrain_density_changes(std::span<std::uint8_t> generated_chunk,const TerrainDelta& delta);
// Applies edits from this chunk and its positive neighbours, including shared
// boundary samples. Deltas from other worlds or non-neighbouring chunks are ignored.
void apply_terrain_neighbour_changes(std::span<std::uint8_t> generated_chunk,std::uint16_t world_id,std::array<std::int32_t,3> position,std::span<const TerrainDelta> deltas);
// Applies shared boundary edits symmetrically from all 26 neighbours.
void apply_terrain_all_neighbour_changes(std::span<std::uint8_t> generated_chunk,std::uint16_t world_id,std::array<std::int32_t,3> position,std::span<const TerrainDelta> deltas);
// Reads voxel chunks for one cell of a version 11–15 TILE asset.
std::vector<TerrainTileChunk> read_terrain_tile_cell(const std::filesystem::path& path,std::uint32_t cell_x,std::uint32_t cell_y);
// Reads the game's height sample grid at mip 0–5. Samples are row-major Y,X.
TerrainTileHeights read_terrain_tile_heights(const std::filesystem::path& path,std::uint32_t cell_x,std::uint32_t cell_y,std::uint32_t mip=0);
// Reads local FLATTEN nodes; x/y are local coordinates and radius is scale.x/2.
std::vector<TerrainFlattenArea> read_terrain_tile_flatten_nodes(const std::filesystem::path& path,std::uint32_t cell_x,std::uint32_t cell_y);
std::vector<TerrainFlattenArea> collect_overworld_flatten_nodes(const OverworldTerrain& terrain,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog,std::int32_t cell_x,std::int32_t cell_y);
// Load a mapped overworld cell in tile-local coordinates; rotation and world
// placement are intentionally left to the caller.
std::vector<TerrainTileChunk> read_overworld_tile_voxels(const OverworldCell& cell,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog);
// Returns no chunk unless all eight neighbours needed for shared boundaries
// are present. Voxel layout matches the game's 17x17x17 terrain buffer.
std::optional<std::array<std::uint8_t,4913>> assemble_terrain_tile_chunk(std::span<const TerrainTileChunk> chunks,std::array<std::int32_t,3> position);
std::vector<TerrainVertex> mesh_terrain_chunk(std::span<const std::uint8_t> voxels,std::array<std::int32_t,3> position,const TerrainMeshTables& tables);
// Triangles for complete tile-local chunks, rotated and translated into the
// saved world's coordinates. Missing boundary neighbours remain excluded.
std::vector<TerrainVertex> mesh_overworld_tile_voxels(const OverworldCell& cell,std::span<const TerrainTileChunk> chunks,const TerrainMeshTables& tables);
// As above, applying persisted world-space terrain deltas before meshing.
std::vector<TerrainVertex> mesh_overworld_tile_voxels(const OverworldCell& cell,std::span<const TerrainTileChunk> chunks,std::uint16_t world_id,std::span<const TerrainDelta> deltas,const TerrainMeshTables& tables);
// Elevation/cliff/flattening contributions must be supplied for every sample
// in tile-local order. This does not synthesize missing world-generator data.
std::vector<TerrainVertex> mesh_overworld_heightfield(const OverworldCell& cell,const TerrainTileHeights& heights,std::span<const float> world_offsets);
// The callback supplies FLATTEN nodes in the exact 3x3 area for a queried cell.
std::vector<TerrainVertex> mesh_generated_overworld_heightfield(const OverworldTerrain& terrain,const OverworldCell& cell,const TerrainTileHeights& heights,const std::function<std::vector<TerrainFlattenArea>(std::int32_t,std::int32_t)>& flatten_nodes);
std::vector<TerrainVertex> mesh_saved_overworld_cell(const OverworldTerrain& terrain,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog,const OverworldCell& cell);
std::string inspect_terrain_json(const std::filesystem::path& path);
}
