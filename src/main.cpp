#include "core/json.hpp"
#include "data/world.hpp"
#include "data/state.hpp"
#include "data/assets.hpp"
#include "data/terrain.hpp"
#include "lua/runtime.hpp"
#include "physics/simulation.hpp"
#include "physics/world_physics.hpp"
#include "steam/probe.hpp"
#include <charconv>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::uint64_t integer(std::string_view text, std::uint64_t maximum) {
    std::uint64_t value{};
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > maximum) throw std::runtime_error("invalid integer: " + std::string(text));
    return value;
}
void usage() {
    std::cout << "scrapds 0.1.0 - native dedicated server research build\n"
        "  scrapds status\n"
        "  scrapds inspect <world.db> [--samples 0..100]\n"
        "  scrapds inspect-terrain <world.db>\n"
        "  scrapds inspect-terrain-tile <tile> <cell-x> <cell-y>\n"
        "  scrapds load-world <world.db> [--advance-ticks N] [--save-as NEW_WORLD.db]\n"
        "  scrapds simulate [--scenario file.lua] [--ticks N] [--trace new.csv] [--realtime]\n"
        "  scrapds simulate-world <world.db> [--game INSTALL_DIR] [--ticks N] [--density KG_PER_M3] [--save-as NEW_WORLD.db]\n"
        "  scrapds steam-identity --game INSTALL_DIR [--seconds N]\n"
        "  scrapds listen-probe --game INSTALL_DIR --output NEW_DIR --allow-peer STEAMID [--seconds N] [--handshake] [--game-ready] [--replay DIR] [--world WORLD.db] [--allow-peer2 STEAMID]\n"
        "  scrapds serve --game INSTALL_DIR --output NEW_DIR --replay DIR --world WORLD.db [--allow-peer STEAMID] [--allow-peer2 STEAMID] [--public] [--native-players] [--native-physics] [--seconds N|0]\n"
        "    serve --seconds 0 runs until <output>/stop.request is created; clean stop writes the final world checkpoint.\n"
        "    --public advertises the server and admits authenticated public-universe Steam peers.\n"
        "    --native-players enables experimental server-created Player/Character records.\n"
        "    --native-physics enables incomplete block-body simulation; requires --native-players.\n"
        "Simulation uses upstream Bullet and reference Lua 5.1.5. Vanilla join is experimental; authoritative game physics and full gameplay are incomplete.\n";
}
}
int main_impl(int argc, char** argv) {
    try {
        if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") { usage(); return 0; }
        const std::string command = argv[1];
        if (command == "status") {
            if (argc != 2) throw std::runtime_error("status takes no arguments");
            std::cout << "{\"version\":\"0.1.0\",\"world_inspection\":true,\"native_world_load\":true,\"native_block_build_and_paint\":true,\"creative_joint_removal\":true,\"survival_joint_item_refund\":false,\"native_joint_paint_explicit_color\":true,\"joint_paint_default_color\":true,\"character_paint_explicit_color_requires_native_players\":true,\"character_paint_saved_color\":true,\"character_paint_captured_default_reset\":true,\"character_paint_live_verified\":false,\"native_survival_voxel_collision_streaming\":true,\"native_survival_id25_script_refresh_partial\":true,\"native_bootstrap_rebuild\":true,\"bootstrap_live_deletes\":true,\"bootstrap_new_known_joints\":true,\"headless_physics_scenarios\":true,\"game_world_simulation\":false,\"block_collision_query\":true,\"steam_transport\":true,\"steam_inventory_owner_validation\":true,\"verified_player_inventory_persistence\":true,\"client_control_handshake\":true,\"client_game_info\":true,\"client_join_state\":true,\"vanilla_client_join\":true,\"vanilla_client_join_observed\":true,\"vanilla_gameplay_command_verified\":false,\"game_lua_api\":false,\"survival_inventory\":false,\"steam_public_listing_opt_in\":true,\"dedicated_server_complete\":false}\n";
        } else if (command == "inspect") {
            if (argc != 3 && argc != 5) throw std::runtime_error("inspect expects world.db [--samples N]");
            int samples = 3;
            if (argc == 5) {
                if (std::string_view(argv[3]) != "--samples") throw std::runtime_error("unknown inspect option");
                samples = int(integer(argv[4], 100));
            }
            std::cout << scrapds::inspect_world_json(std::filesystem::u8path(argv[2]), samples) << '\n';
        } else if (command == "inspect-terrain") {
            if(argc!=3)throw std::runtime_error("inspect-terrain expects world.db");
            std::cout<<scrapds::inspect_terrain_json(std::filesystem::u8path(argv[2]))<<'\n';
        } else if (command == "inspect-terrain-tile") {
            if(argc!=5)throw std::runtime_error("inspect-terrain-tile expects tile cell-x cell-y");
            const auto chunks=scrapds::read_terrain_tile_cell(std::filesystem::u8path(argv[2]),std::uint32_t(integer(argv[3],UINT32_MAX)),std::uint32_t(integer(argv[4],UINT32_MAX)));
            std::size_t complete=0;for(const auto& chunk:chunks)if(scrapds::assemble_terrain_tile_chunk(chunks,chunk.position))++complete;
            std::cout<<"{\"voxel_chunks\":"<<chunks.size()<<",\"complete_with_boundaries\":"<<complete<<",\"voxels_per_chunk\":4096,\"chunks\":[";
            bool first=true;for(const auto& chunk:chunks){if(!first)std::cout<<',';first=false;std::cout<<'['<<chunk.position[0]<<','<<chunk.position[1]<<','<<chunk.position[2]<<']';}std::cout<<"]}\n";
        } else if (command == "load-world") {
            if (argc < 3) throw std::runtime_error("load-world expects world.db");
            std::uint64_t advance = 0;
            std::filesystem::path save;
            for (int i = 3; i < argc; ++i) {
                const std::string_view flag = argv[i];
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + std::string(flag));
                const auto value = argv[++i];
                if (flag == "--advance-ticks") advance = integer(value, INT64_MAX);
                else if (flag == "--save-as") save = std::filesystem::u8path(value);
                else throw std::runtime_error("unknown world option: " + std::string(flag));
            }
            auto state = scrapds::WorldState::load(std::filesystem::u8path(argv[2]));
            if (advance) state.advance_ticks(advance);
            if (!save.empty()) state.save_as(save);
            std::cout << state.summary_json() << '\n';
        } else if (command == "simulate-world") {
            if(argc<3)throw std::runtime_error("simulate-world expects world.db");
            std::uint64_t ticks=400;double density=1000.0;std::filesystem::path save,game;
            for(int i=3;i<argc;++i) {
                const std::string_view flag=argv[i];if(i+1>=argc)throw std::runtime_error("missing value for "+std::string(flag));
                const std::string value=argv[++i];
                if(flag=="--ticks")ticks=integer(value,10000000);
                else if(flag=="--density"){std::size_t used=0;density=std::stod(value,&used);if(used!=value.size())throw std::runtime_error("invalid density");}
                else if(flag=="--save-as")save=std::filesystem::u8path(value);
                else if(flag=="--game")game=std::filesystem::u8path(value);
                else throw std::runtime_error("unknown simulate-world option: "+std::string(flag));
            }
            auto world=scrapds::WorldState::load(std::filesystem::u8path(argv[2]));
            std::optional<scrapds::AssetDefinitions> assets;if(!game.empty())assets=scrapds::AssetDefinitions::load(game);
            auto physics=scrapds::WorldPhysics::from_world(world,density,nullptr,assets?&*assets:nullptr);physics.step(ticks);physics.writeback(world);world.advance_ticks(ticks);
            if(!save.empty())world.save_as(save);
            std::ostringstream out;out<<std::setprecision(9)<<"{\"physics\":"<<physics.summary_json()<<",\"ticks\":"<<ticks<<",\"bodies\":[";
            bool first=true;for(const auto& body:physics.bodies()){if(!first)out<<',';first=false;out<<"{\"id\":"<<body.id<<",\"world\":"<<body.world_id<<",\"position\":["<<body.position.x<<','<<body.position.y<<','<<body.position.z<<"],\"velocity\":["<<body.velocity.x<<','<<body.velocity.y<<','<<body.velocity.z<<"]}";}
            std::cout<<out.str()<<"]}\n";
        } else if (command == "simulate") {
            std::uint64_t ticks = 400;
            bool realtime = false;
            std::filesystem::path scenario_file, trace;
            for (int i = 2; i < argc; ++i) {
                std::string_view flag = argv[i];
                if (flag == "--realtime") { realtime = true; continue; }
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + std::string(flag));
                const auto value = argv[++i];
                if (flag == "--ticks") ticks = integer(value, 10000000);
                else if (flag == "--scenario") scenario_file = std::filesystem::u8path(value);
                else if (flag == "--trace") trace = std::filesystem::u8path(value);
                else throw std::runtime_error("unknown simulation option: " + std::string(flag));
            }
            const auto scenario = scenario_file.empty() ? scrapds::default_scenario() : scrapds::load_scenario(scenario_file);
            std::cout << scrapds::result_json(scenario, scrapds::simulate(scenario, ticks, trace, realtime)) << '\n';
        } else if (command == "steam-identity" || command == "listen-probe" || command == "serve") {
            std::filesystem::path game, output, replay, world;
            std::uint64_t allowed = 0, allowed2 = 0;
            int seconds = command == "steam-identity" ? 30 : command == "serve" ? 0 : 120;
            bool handshake = command == "serve";
            bool game_ready = command == "serve";
            bool public_server = false;
            bool native_players = false;
            bool native_physics = false;
            for (int i = 2; i < argc; ++i) {
                const std::string_view flag = argv[i];
                if (flag == "--handshake" && command != "steam-identity") { handshake = true; continue; }
                if (flag == "--game-ready" && command != "steam-identity") { game_ready = true; continue; }
                if (flag == "--public" && command == "serve") { public_server = true; continue; }
                if (flag == "--native-players" && command == "serve") { native_players = true; continue; }
                if (flag == "--native-physics" && command == "serve") { native_physics = true; continue; }
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + std::string(flag));
                const auto value = argv[++i];
                if (flag == "--game") game = std::filesystem::u8path(value);
                else if (flag == "--output" && command != "steam-identity") output = std::filesystem::u8path(value);
                else if (flag == "--allow-peer" && command != "steam-identity") allowed = integer(value, UINT64_MAX);
                else if (flag == "--allow-peer2" && command != "steam-identity") allowed2 = integer(value, UINT64_MAX);
                else if (flag == "--replay" && command != "steam-identity") replay = std::filesystem::u8path(value);
                else if (flag == "--world" && command != "steam-identity") world = std::filesystem::u8path(value);
                else if (flag == "--seconds") seconds = int(integer(value, 86400));
                else throw std::runtime_error("unknown Steam probe option: " + std::string(flag));
            }
            if (game.empty() || (seconds < 1 && command != "serve")) throw std::runtime_error("--game and a positive --seconds are required outside serve --seconds 0");
            if (command == "steam-identity") std::cout << scrapds::probe_steam_identity(game, seconds) << '\n';
            else {
                if (output.empty() || (!public_server && allowed == 0)) throw std::runtime_error("private server requires --output and --allow-peer; open servers require serve --public");
                if (command == "serve" && (replay.empty() || world.empty())) throw std::runtime_error("serve requires --replay and --world; this native listener remains experimental");
                std::cout << scrapds::run_steam_listener(game, output, allowed, allowed2, seconds, handshake, game_ready, replay, world, public_server, native_players, native_physics) << '\n';
            }
        } else throw std::runtime_error("unknown command: " + command);
        return 0;
    } catch (const std::exception& e) { std::cerr << "scrapds: " << e.what() << '\n'; return 1; }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** wide_argv) {
    try {
        std::vector<std::string> strings;
        strings.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (!size) throw std::runtime_error("invalid Unicode argument");
            std::string utf8(size, '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, utf8.data(), size, nullptr, nullptr)) throw std::runtime_error("cannot convert Unicode argument");
            utf8.pop_back();
            strings.push_back(std::move(utf8));
        }
        std::vector<char*> pointers;
        for (auto& s : strings) pointers.push_back(s.data());
        return main_impl(argc, pointers.data());
    } catch (const std::exception& e) { std::cerr << "scrapds: " << e.what() << '\n'; return 1; }
}
#else
int main(int argc, char** argv) { return main_impl(argc, argv); }
#endif
