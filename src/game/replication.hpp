#pragma once
#include "commands.hpp"
#include "net/world_sync.hpp"
#include <cstdint>

namespace scrapds {
// Builds the live ID22 update for creations, removals and changed states. The origin
// client is included in the recipient set by the transport.
net::NetObjectPacket make_object_update(const WorldState& world, const CommandResult& result,
                                        std::uint32_t tick, const net::InitialAssetCatalog& catalog);
struct ReplicatedCommandResult {
    CommandResult command;
    std::vector<std::uint8_t> response;
};
// Commits the world only after every required client update has been encoded.
ReplicatedCommandResult apply_replicated_command(WorldState& world, const AssetDefinitions& assets,
    const net::GameCommand& command, std::uint32_t tick, const net::InitialAssetCatalog& catalog,
    std::uint16_t world_id = 0, std::optional<std::uint32_t> player_container_id = std::nullopt);
}
