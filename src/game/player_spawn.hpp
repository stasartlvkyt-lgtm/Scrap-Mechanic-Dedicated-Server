#pragma once
#include "data/state.hpp"
#include "net/character.hpp"
#include "net/player_info.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace scrapds {
struct PlayerSpawnSpec {
    std::uint64_t steam_id{};
    net::PlayerInfo info;
    net::CharacterCreation character;
    net::CharacterInitialState character_state;
    // Present only for a previously allocated Player owned by this SteamID.
    std::optional<std::uint32_t> existing_player_id;
    std::array<std::uint32_t,3> player_state_words{UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::uint8_t player_flags{0xff};
};
struct PlayerSpawnPackets {
    std::uint32_t player_id{},character_id{};
    std::uint64_t owner_steam_id{};
    std::array<std::uint32_t,3> player_state_words{UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::uint8_t player_flags{0xff};
    net::PlayerInfo info;
    net::CharacterInitialState character_state;
    std::vector<std::uint8_t> player_before,player_after,character_update;
};
// Builds the observed Player-before, Player-linked and Character create/state
// sequence. All packet bytes are prepared before the Player counter commits.
PlayerSpawnPackets prepare_player_spawn(WorldState& world,net::ServerCharacters& characters,
                                        const PlayerSpawnSpec& spec,std::uint32_t tick);
// Unlink the Player, then remove its runtime Character. The Player record
// remains available for later reconnects within the current listener session.
std::array<std::vector<std::uint8_t>,2> prepare_player_disconnect(const PlayerSpawnPackets& player,std::uint32_t tick);
PlayerSpawnPackets restore_player_snapshot(std::uint64_t steam_id,const StoredPlayerSnapshot& stored);
// Replace the Character's spawn position with its latest accepted client state
// for a later-joining observer. This is not authoritative physics.
std::vector<std::uint8_t> character_spawn_at_latest_position(const PlayerSpawnPackets& spawn,const net::CharacterUpdate& latest);
// Updates the active Character color in the saved spawn snapshot and returns
// the live NetObj state packet for broadcast.
std::vector<std::uint8_t> paint_player_character(PlayerSpawnPackets& spawn,std::uint32_t color_rgba,std::uint32_t tick);
// Adds a Steam Inventory result only after the server API verified its owner,
// then persists a fresh unlinked Player snapshot and returns the linked update.
std::vector<std::uint8_t> apply_verified_player_inventory(WorldState& world,PlayerSpawnPackets& spawn,
                                                          std::span<const std::uint8_t> inventory_result,std::uint32_t tick);
// Removes captured Player and Character records from a replay packet. A packet
// containing only removed records becomes nullopt; unrelated data is retained.
std::optional<std::vector<std::uint8_t>> strip_replay_players(std::span<const std::uint8_t> packet);
std::optional<net::CharacterCreation> find_replay_spawn(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> strip_game_info_players(std::span<const std::uint8_t> packet);
}
