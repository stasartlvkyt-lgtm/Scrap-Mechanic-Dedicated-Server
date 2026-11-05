#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "net/world_sync.hpp"

namespace scrapds::net {
inline constexpr std::array<std::uint8_t,16> player_blob_uid{0x51,0x86,0x88,0x83,0xd2,0xd2,0x49,0x53,0x91,0x35,0x1a,0xb0,0xbd,0xc2,0xa4,0x7e};
struct PlayerAppearancePart {
    std::array<std::uint8_t,16> uuid_memory{};
    std::uint32_t color{};
};
struct PlayerInfo {
    std::string name;
    std::uint8_t appearance_type{};
    std::vector<PlayerAppearancePart> appearance;
    std::uint32_t inventory_kind{};
    std::vector<std::uint8_t> inventory_result;
    bool inventory_verified{}, inventory_expired{};
};
// Decodes message ID 9; inventory signature and ownership verification are separate.
PlayerInfo parse_player_info(std::span<const std::uint8_t> plaintext);
struct PlayerSnapshot {
    std::uint32_t player_id{},character_id{UINT32_MAX};
    std::uint64_t owner_steam_id{};
    std::array<std::uint32_t,3> state_words{UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::uint8_t flags{0xff};
    PlayerInfo info;
};
// PlayerManager writes the compressed Player snapshot under this BlobData key.
BlobDataEntry make_player_blob_entry(const PlayerSnapshot& player);
// Parses the server's persisted Player blob format and validates its identity.
PlayerSnapshot parse_player_blob_entry(const BlobDataEntry& entry);
}
