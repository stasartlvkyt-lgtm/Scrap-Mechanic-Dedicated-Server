#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace scrapds {
// Native Steam listener and experimental game server. Public listing/admission
// is opt-in; private operation requires an explicit SteamID allowlist.
// Uses only the user's installed, hash-verified game DLL on Windows x64.
std::string probe_steam_identity(const std::filesystem::path& game, int seconds = 30);
std::string run_steam_listener(const std::filesystem::path& game, const std::filesystem::path& output,
                               std::uint64_t allow_peer, std::uint64_t allow_peer2 = 0, int seconds = 120,
                               bool handshake = false, bool game_ready = false,
                               const std::filesystem::path& replay_dir = {},
                               const std::filesystem::path& world_path = {},
                               bool public_server = false, bool native_players = false, bool native_physics = false);
}
