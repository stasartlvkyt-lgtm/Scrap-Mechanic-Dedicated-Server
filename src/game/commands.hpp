#pragma once
#include "data/state.hpp"
#include "data/assets.hpp"
#include "net/game_commands.hpp"
#include <optional>
#include <string>
#include <vector>
namespace scrapds {
struct CommandResult {
    bool applied{};
    std::string reason;
    std::vector<std::uint32_t> changed_shapes;
    std::vector<std::uint32_t> created_bodies, created_shapes;
    std::vector<std::uint32_t> removed_bodies, removed_shapes;
    std::vector<std::uint32_t> changed_joints;
    std::vector<std::uint32_t> removed_joints;
    struct ContainerSlot { std::uint32_t container_id{},slot{}; };
    std::vector<ContainerSlot> changed_container_slots;
    // Appended to preserve the layout of the existing result fields.
    std::vector<std::uint32_t> removed_harvestables;
};
// Bounded Creative edits and Survival build/removal, LootHarvestable pickup,
// and joint transactions. Authentication is checked by transport; gameplay
// handlers remain limited to the explicitly supported object variants.
CommandResult apply_game_command(WorldState& world, const AssetDefinitions& assets, const net::GameCommand& command,
    std::uint16_t world_id = 0,std::optional<std::uint32_t> player_container_id = std::nullopt);
}
