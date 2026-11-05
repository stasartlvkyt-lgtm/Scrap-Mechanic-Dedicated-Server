#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scrapds {
struct Vec3 { double x{}, y{}, z{}; };
struct BodySpec {
    std::uint32_t id{};
    double mass{1.0};
    Vec3 half_extents{0.5, 0.5, 0.5};
    Vec3 position{0.0, 0.0, 5.0};
    Vec3 velocity{};
    double friction{0.5};
    double restitution{0.0};
};
struct Scenario {
    std::string name{"falling-box"};
    Vec3 gravity{0, 0, -10};
    double ground_z{0};
    double dt{1.0 / 40.0};
    int solver_iterations{10};
    std::vector<BodySpec> bodies;
};
struct BodyState {
    std::uint32_t id{};
    Vec3 position;
    Vec3 velocity;
    double qx{}, qy{}, qz{}, qw{1};
};
struct SimulationResult { std::uint64_t ticks{}; double dt{}; std::vector<BodyState> bodies; };
Scenario default_scenario();
SimulationResult simulate(const Scenario& scenario, std::uint64_t ticks, const std::filesystem::path& trace = {}, bool realtime = false);
std::string result_json(const Scenario& scenario, const SimulationResult& result);
}
