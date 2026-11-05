#pragma once
#include "data/state.hpp"
#include "data/assets.hpp"
#include "net/game_commands.hpp"
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
};
// Bounded Creative block paint/build/remove, joint removal and paint.
// Authentication is checked by transport. Survival inventory transactions
// and character paint rules remain separate.
CommandResult apply_game_command(WorldState& world, const AssetDefinitions& assets, const net::GameCommand& command, std::uint16_t world_id = 0);
}
