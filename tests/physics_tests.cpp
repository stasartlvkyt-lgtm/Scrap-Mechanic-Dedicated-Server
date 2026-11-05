#include "physics/simulation.hpp"
#include "lua/runtime.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        auto s = scrapds::default_scenario();
        auto initial = scrapds::simulate(s, 0);
        require(initial.bodies.at(0).position.z == 5, "zero-tick state changed");
        auto falling = scrapds::simulate(s, 10);
        require(falling.bodies.at(0).position.z < 5 && falling.bodies.at(0).position.z > 4, "gravity not applied");
        auto settled = scrapds::simulate(s, 400);
        require(std::abs(settled.bodies.at(0).position.z - 0.5) < 0.02, "box did not settle on plane");
        require(std::abs(settled.bodies.at(0).velocity.z) < 0.01, "settled body still moving");
        auto again = scrapds::simulate(s, 400);
        require(std::abs(again.bodies.at(0).position.z - settled.bodies.at(0).position.z) < 1e-6, "repeat scenario differs");
        s.ground_z = 7;
        s.bodies[0].position.z = 12;
        require(std::abs(scrapds::simulate(s, 400).bodies.at(0).position.z - 7.5) < 0.02, "ground offset ignored");
        s.bodies[0].half_extents.z = -1;
        bool rejected = false;
        try { (void)scrapds::simulate(s, 0); } catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid shape accepted");
        s = scrapds::default_scenario();
        s.bodies[0].position.z = 1e300;
        rejected = false;
        try { (void)scrapds::simulate(s, 0); } catch (const std::exception&) { rejected = true; }
        require(rejected, "finite double overflowing Bullet scalar accepted");
        s = scrapds::default_scenario();
        s.bodies[0].half_extents.z = 1e-300;
        rejected = false;
        try { (void)scrapds::simulate(s, 0); } catch (const std::exception&) { rejected = true; }
        require(rejected, "positive extent underflowing Bullet scalar accepted");
        std::cout << "physics checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
