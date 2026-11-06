#pragma once

#include <cstdint>
#include <array>
#include <span>
#include <vector>
#include <map>
#include <optional>
#include <string>

namespace scrapds::net {

struct CharacterUpdate {
    std::uint32_t tick{};
    std::uint32_t acknowledgements{};
    std::uint8_t state_type{};
    std::uint32_t character_id{};
    bool has_character_state{};
    std::array<float,3> position{};
    std::array<float,3> velocity{};
    std::array<float,4> rotation{};
};
// Returns an explicit violation when a client displacement exceeds bounded
// horizontal, upward-jump, or downward-fall speeds plus a small quantization margin.
std::string character_displacement_violation(const std::array<float,3>& previous,
    const std::array<float,3>& current,double elapsed_seconds);
std::string format_character_asset_uuid(const std::array<std::uint8_t,16>& memory);

// Parses the observed client-to-server 0x1e stream, including type 3/4
// transforms, acknowledgement lists, and character IDs.
CharacterUpdate parse_character_update(std::span<const std::uint8_t> packet);
struct ServerCharacterUpdate {
    std::uint32_t server_tick{}, client_tick{};
    std::uint8_t sequence{};
    CharacterUpdate character;
};
// One type-3/4 Character record in ID24 server-to-client framing.
ServerCharacterUpdate parse_server_character_update(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> make_server_character_update(std::span<const std::uint8_t> client_packet,std::uint32_t server_tick,std::uint32_t acknowledged_client_tick,std::uint8_t sequence=0,std::optional<std::array<float,3>> authoritative_position=std::nullopt,std::optional<std::array<float,3>> authoritative_velocity=std::nullopt);
struct ServerBodyUpdate {
    std::uint32_t server_tick{}, acknowledged_client_tick{}, body_id{};
    std::uint8_t sequence{},flags{};
    bool has_velocity{};
    std::array<float,4> rotation{};
    std::array<float,3> position{},velocity{},angular_velocity{};
};
// One DynamicBody record in an ID24 stream, preserving native fixed-point
// transform, velocity and the three runtime flag bits.
ServerBodyUpdate parse_server_body_update(std::span<const std::uint8_t> packet);
std::vector<ServerBodyUpdate> extract_server_body_updates(std::span<const std::uint8_t> packet);
std::vector<std::uint8_t> make_server_body_update(const ServerBodyUpdate& update);

// Character virtual creation writer (RVA 0x72FDB0). UUID bytes are in
// native 128-bit memory order; the two orientation words are kept opaque.
struct CharacterCreation {
    std::uint64_t owner_steam_id{};
    std::array<float,3> position{};
    std::uint16_t world_id{};
    std::array<std::uint32_t,2> orientation_words{};
    std::array<std::uint8_t,16> asset_uuid_memory{};
    bool flag{};
};
std::vector<std::uint8_t> serialize_character_creation(const CharacterCreation& character);
CharacterCreation parse_character_creation(std::span<const std::uint8_t> payload);
// Full Character state as written by virtual serializer RVA 0x7300B0.
struct CharacterInitialState {
    std::array<bool,8> flags{};
    std::optional<std::uint32_t> extra_id;
    std::uint32_t color_rgba{UINT32_MAX};
    std::array<std::uint8_t,16> asset_uuid_memory{};
    std::uint32_t state_word{UINT32_MAX};
    bool active{};
    std::uint32_t player_id{UINT32_MAX};
};
std::vector<std::uint8_t> serialize_character_initial_state(const CharacterInitialState& state);

class CharacterOwnership {
public:
    bool claim(std::uint64_t steam_id, std::uint32_t character_id);
    void release(std::uint64_t steam_id) noexcept;
    std::size_t size() const noexcept { return by_peer_.size(); }
private:
    std::map<std::uint64_t,std::uint32_t> by_peer_;
    std::map<std::uint32_t,std::uint64_t> by_character_;
};

class CharacterStateTracker {
public:
    // Returns an empty string when the update becomes the peer's latest state.
    // Ticks are compared as wrapping uint32 serial numbers.
    std::string observe(std::uint64_t steam_id, const CharacterUpdate& update);
    void release(std::uint64_t steam_id) noexcept;
private:
    CharacterOwnership ownership_;
    std::map<std::uint64_t,CharacterUpdate> latest_;
};

// Server-created runtime Characters. The game's UniqueIdProvider starts
// index 3 at 1 on startup; this counter is not part of Game.uniqueIds.
// Creation must be replicated to clients before this registry enforces input.
class ServerCharacters {
public:
    std::uint32_t spawn(std::uint64_t steam_id);
    void despawn(std::uint64_t steam_id) noexcept;
    std::string observe(std::uint64_t steam_id, const CharacterUpdate& update);
    std::optional<CharacterUpdate> latest(std::uint64_t steam_id) const;
    std::optional<std::array<float,3>> tick_start_position(std::uint64_t steam_id) const;
    std::size_t size() const noexcept { return characters_.size(); }
private:
    struct Character {
        std::uint32_t id;
        std::optional<CharacterUpdate> state;
        std::array<float,3> tick_start_position{};
    };
    std::uint32_t next_id_{1};
    std::map<std::uint64_t,Character> characters_;
};

} // namespace scrapds::net
