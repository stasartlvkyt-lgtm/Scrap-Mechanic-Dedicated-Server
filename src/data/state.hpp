#pragma once
#include "blob.hpp"
#include "terrain.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace scrapds {
using Blob = std::vector<std::uint8_t>;
struct GameMetadata {
    std::int64_t save_version{}, flags{}, seed{}, tick{};
    std::optional<std::string> game_version;
    std::optional<std::int64_t> build_number;
};
struct StoredBody {
    std::uint32_t id{};
    std::uint16_t world_id{};
    Blob data;
    std::optional<RigidBodyPrefix> prefix;
};
struct StoredShape {
    std::uint32_t id{}, body_id{};
    Blob data;
    std::optional<ChildShapePrefix> prefix;
};
struct StoredJoint {
    // A saved -1 world/ground anchor is retained as UINT32_MAX in the view.
    std::uint32_t id{}, shape_a{}, shape_b{};
    Blob data;
};
struct BlockCreation {
    // Missing body_id creates a StaticBody at the native 16-cell origin.
    std::optional<std::uint32_t> body_id;
    std::uint16_t world_id{};
    std::array<std::uint8_t,16> uuid{};
    std::array<std::int32_t,3> cell_position{}, dimensions{};
    std::uint32_t color_abgr{};
};
struct CreatedBlock {
    std::uint32_t body_id{}, shape_id{};
    bool created_body{};
};
struct RemovedShapes {
    std::vector<std::uint32_t> shape_ids, empty_body_ids;
};
struct SplitShape {
    std::uint32_t selected_shape_id{};
    std::vector<std::uint32_t> fragment_shape_ids;
};
struct StoredController {
    std::uint32_t id{}, host_id{}, host_type{};
    Blob data;
};
struct StoredContainer {
    std::uint32_t id{};
    // Build-889 entries are 22 bytes: two retained opaque bytes, a serialized
    // 16-byte item UUID, and a BE32 quantity (0xffffffff marks an empty slot).
    std::uint16_t entry_count{};
    std::vector<std::array<std::uint8_t,22>> serialized_entries;
    Blob opaque_suffix, data;
    bool supported_layout{};
};
struct StoredPlayerSnapshot {
    std::uint32_t player_id{};
    Blob unlinked_packet;
    bool inventory_verified{};
};
struct StoredPlayerLocation {
    std::uint16_t world_id{};
    std::array<double,3> position{};
};
struct StoredBlobDataEntry {
    std::array<std::uint8_t,16> uid{};
    Blob key,payload;
    std::uint16_t world_id{};
    std::uint8_t flags{};
};

// Authoritative in-memory save state. Unknown tables/BLOB fields are retained
// verbatim in the private SQLite database; the owner's source is never written.
// Physics and network serialization are separate consumers of these records.
class WorldState {
public:
    static WorldState load(const std::filesystem::path& source);
    WorldState(WorldState&&) noexcept;
    WorldState& operator=(WorldState&&) noexcept;
    ~WorldState();
    WorldState(const WorldState&) = delete;
    WorldState& operator=(const WorldState&) = delete;

    const GameMetadata& metadata() const;
    const std::map<std::uint32_t, StoredBody>& bodies() const;
    const std::map<std::uint32_t, StoredShape>& shapes() const;
    const std::map<std::uint32_t, StoredJoint>& joints() const;
    const std::map<std::uint32_t, StoredController>& controllers() const;
    const std::map<std::uint32_t, StoredContainer>& containers() const;
    // Returns the saved quantity only when container, slot and serialized UUID
    // match. Empty slots and unknown layouts return nullopt.
    std::optional<std::uint32_t> container_slot_quantity(std::uint32_t container_id,
        std::uint32_t slot,const std::array<std::uint8_t,16>& saved_item_uuid) const;
    // Atomically spends from an exact saved slot, preserving its opaque bytes,
    // other slots, suffix, row ID and all unrelated container data.
    void spend_container_slot(std::uint32_t container_id,std::uint32_t slot,
        const std::array<std::uint8_t,16>& saved_item_uuid,std::uint32_t quantity);
    const std::set<std::uint32_t>& container_ids() const;
    const std::map<std::string, std::uint64_t>& table_counts() const;
    // Worlds whose saved game metadata explicitly names CreativeFlatWorld.
    std::set<std::uint16_t> flat_surface_worlds() const;
    // World IDs that contain the saved terrain-generator ScriptData record.
    std::set<std::uint16_t> terrain_generator_worlds() const;
    // Opaque, decompressed LUA terrain-generator state for an existing world.
    // The contained table encoding is not yet interpreted.
    std::optional<Blob> terrain_generator_data(std::uint16_t world_id) const;
    // Saved terrain-generator records; unlike a raw ScriptData dump, this only
    // returns the supported terrain manager's bounded per-world entries.
    std::vector<StoredBlobDataEntry> terrain_generator_entries() const;
    // Saved ScriptData entries for managers with an independently verified
    // runtime UUID mapping (most ScriptData managers remain opaque).
    std::vector<StoredBlobDataEntry> script_data_entries(const std::array<std::uint8_t,16>& uid) const;
    std::vector<StoredBlobDataEntry> generic_data_entries() const;
    // Decoded VoxelTerrain deltas with SQLite/header identity cross-checks.
    std::vector<TerrainDelta> terrain_deltas() const;
    void advance_ticks(std::uint64_t count);
    // Runs a compound game command atomically across SQLite and cached state.
    // A false return or exception rolls back every nested mutation.
    bool atomic(const std::function<bool()>& operation);
    // PlayerManager uses provider index 10, stored in uniqueIds slot 8.
    // Returns the old value and atomically advances the saved counter.
    std::uint32_t reserve_player_id();
    // Container uses provider index 6, stored in uniqueIds slot 4.
    std::uint32_t reserve_container_id();
    // Returns the persistent per-SteamID inventory Container, creating the
    // native 30-slot default when no saved Player link is available.
    std::uint32_t ensure_player_inventory_container(std::uint64_t steam_id,
        std::uint32_t preferred_container_id = UINT32_MAX);
    // Server-owned extension table, included atomically in SQLite checkpoints.
    std::map<std::uint64_t,StoredPlayerSnapshot> player_snapshots() const;
    void store_player_snapshot(std::uint64_t steam_id,std::uint32_t player_id,const Blob& unlinked_packet);
    void store_player_snapshot(std::uint64_t steam_id,std::uint32_t player_id,const Blob& unlinked_packet,bool inventory_verified);
    std::map<std::uint64_t,std::uint32_t> player_character_colors() const;
    void store_player_character_color(std::uint64_t steam_id,std::uint32_t color_rgba);
    std::map<std::uint64_t,StoredPlayerLocation> player_locations() const;
    void store_player_location(std::uint64_t steam_id,const StoredPlayerLocation& location);
    // Replaces known records only, retaining identity and ownership. Invalid
    // prefixes and orphan references are rejected before any mutation.
    void replace_body_blob(std::uint32_t id, Blob data);
    void replace_shape_blob(std::uint32_t id, Blob data);
    void replace_shape_blobs(std::map<std::uint32_t, Blob> data);
    // Joint BLOB identity/shape references are retained when applying state edits.
    void replace_joint_blob(std::uint32_t id, Blob data);
    // Replaces a structurally recognized Container BLOB while retaining its
    // SQLite row identity. Slot semantics remain the caller's responsibility.
    void replace_container_blob(std::uint32_t id, Blob data);
    // Bounded creative block insertion; reserves saved counters and persists
    // body/shape/RTree changes in one SQLite transaction. Throws on invalid
    // geometry, overlap, unsupported collision geometry or exhausted IDs.
    CreatedBlock create_block(const BlockCreation& block);
    // Removes complete shapes only. Referenced shapes are rejected so related
    // joint/controller state cannot be silently orphaned.
    RemovedShapes remove_shapes(const std::vector<std::uint32_t>& ids);
    // Keeps the requested half-open block intersection under the original ID
    // and creates up to six non-overlapping remainder cuboids atomically.
    SplitShape split_block_shape(std::uint32_t id,
        const std::array<std::int32_t,3>& intersection_position,
        const std::array<std::int32_t,3>& intersection_dimensions,
        std::uint32_t selected_color_abgr);
    // Deletes one saved Joint row transactionally. Higher-level game rules
    // decide whether the caller is allowed to reclaim its inventory item.
    void remove_joint(std::uint32_t id);
    void save_as(const std::filesystem::path& output) const;
    // Atomically replaces a server-owned checkpoint and refuses to replace
    // the original source save, even when both paths name the same file.
    void save_checkpoint(const std::filesystem::path& output) const;
    std::string summary_json() const;
private:
    struct Impl;
    explicit WorldState(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
