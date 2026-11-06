#pragma once
#include <cstddef>
#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace scrapds { class WorldState; struct StoredContainer; }

namespace scrapds::net {
struct CharacterCreation;
struct CharacterInitialState;
inline constexpr std::size_t world_sync_limit = 64u * 1024u * 1024u;
struct NetObjectRecord {
    std::uint8_t operation{}; // 1=create, 2=remove, 3=state, 5=reliable update.
    std::uint8_t type{};
    std::optional<std::uint8_t> subtype; // Present only for create.
    std::uint32_t id{};
    std::vector<std::uint8_t> payload;
    std::size_t wire_offset{}, wire_size{};
    bool used_byte_delta{};
};
struct NetObjectPacket {
    std::uint8_t message_id{21};
    std::uint32_t tick{};
    std::vector<NetObjectRecord> records;
};
struct BlobDataEntry {
    std::array<std::uint8_t, 16> uid{};
    std::vector<std::uint8_t> key;
    std::uint16_t world_id{};
    std::uint8_t flags{};
    std::vector<std::uint8_t> payload;
};
struct BlobDataPacket {
    std::uint8_t message_id{11};
    std::uint32_t tick{};
    std::vector<BlobDataEntry> entries;
};
// IDs11/13/25/27: tick + repeated UUID/key/world/flags/length/payload entries.
// Entry payload may contain its own codec/serialization and remains opaque.
BlobDataPacket parse_blob_data_packet(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> make_blob_data_packet(const BlobDataPacket& packet);
BlobDataEntry parse_saved_blob_data(std::span<const std::uint8_t> entry);
std::vector<std::uint8_t> make_saved_blob_data(const BlobDataEntry& entry);
// ID29 is a packet aggregate: id, repeated [big-endian u32 size, packet].
std::vector<std::vector<std::uint8_t>> split_world_aggregate(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> make_world_aggregate(std::span<const std::vector<std::uint8_t>> packets);
// IDs21/22 carry a tick then byte-aligned NetObj records. These functions
// recover/build framing and byte deltas; entity payload semantics are separate.
NetObjectPacket parse_net_object_packet(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> make_net_object_packet(const NetObjectPacket& packet, bool byte_delta = true);
NetObjectRecord make_body_creation(std::span<const std::uint8_t> saved_blob);
NetObjectRecord make_child_shape_state(std::span<const std::uint8_t> saved_blob, std::uint16_t asset_index);
NetObjectRecord make_joint_state(std::span<const std::uint8_t> saved_blob, std::uint16_t asset_index);
NetObjectRecord make_character_creation(std::uint32_t id, const CharacterCreation& character);
NetObjectRecord make_character_state(std::uint32_t id, const CharacterInitialState& character);
NetObjectRecord make_container_creation(const scrapds::StoredContainer& container);
NetObjectRecord make_container_slot_state(const scrapds::StoredContainer& container,std::uint32_t slot);
struct NativeInitialWorld {
    NetObjectPacket packet;
    std::size_t native_body_creations{}, native_shape_states{}, native_joint_states{};
    std::size_t native_static_body_states{}, preserved_dynamic_body_states{}, preserved_records{};
};
struct InitialAssetCatalog {
    using SavedUuid = std::array<std::uint8_t, 16>;
    std::map<SavedUuid, std::uint16_t> shapes, joints;
};
// Call once against the unchanged source save, before any UUID mutations.
InitialAssetCatalog infer_initial_asset_catalog(const NetObjectPacket& captured, const scrapds::WorldState& initial_world);
// Catalog snapshots are bound to the SHA-256 of the captured world29 packet.
std::vector<std::uint8_t> serialize_asset_catalog(const InitialAssetCatalog& catalog, std::string_view bootstrap_sha256);
InitialAssetCatalog parse_asset_catalog(std::span<const std::uint8_t> bytes, std::string_view bootstrap_sha256);
void merge_asset_catalog(InitialAssetCatalog& target, const InitialAssetCatalog& incoming);
// Hybrid bootstrap: regenerate verified entity payloads from the loaded save,
// retaining unsupported runtime metadata/Lua/terrain records explicitly. The
// source save must have exactly the captured body/shape/joint identity sets.
// Asset UUID->index mapping is inferred from matching captured initial states.
// This is not a complete standalone initial-world serializer.
NativeInitialWorld rebuild_initial_world(const NetObjectPacket& captured, const scrapds::WorldState& world, std::uint32_t tick);
NativeInitialWorld rebuild_initial_world(const NetObjectPacket& captured, const scrapds::WorldState& world, std::uint32_t tick, const InitialAssetCatalog& catalog);
// Rebuilds supported live entities using an established asset catalog.
NativeInitialWorld rebuild_live_initial_world(const NetObjectPacket& captured, const scrapds::WorldState& world, std::uint32_t tick, const InitialAssetCatalog& catalog);
}
