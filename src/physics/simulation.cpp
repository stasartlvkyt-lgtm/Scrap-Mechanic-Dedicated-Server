#include "simulation.hpp"
#include "core/json.hpp"
#include <btBulletDynamicsCommon.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

namespace scrapds {
namespace {
btVector3 bt(Vec3 value) { return {btScalar(value.x), btScalar(value.y), btScalar(value.z)}; }
Vec3 vec(const btVector3& v) { return {v.x(), v.y(), v.z()}; }
bool scalar_finite(double value) {
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<btScalar>::max();
}
bool finite(Vec3 v) { return scalar_finite(v.x) && scalar_finite(v.y) && scalar_finite(v.z); }
void validate_state(const btRigidBody& body) {
    const auto& q = body.getWorldTransform().getRotation();
    if (!finite(vec(body.getWorldTransform().getOrigin())) || !finite(vec(body.getLinearVelocity())) ||
        !scalar_finite(q.x()) || !scalar_finite(q.y()) || !scalar_finite(q.z()) || !scalar_finite(q.w()))
        throw std::runtime_error("simulation produced a non-finite body state");
}
void validate(const Scenario& s) {
    if (!std::isfinite(s.dt) || s.dt < 0.0001 || s.dt > 0.1) throw std::runtime_error("dt must be between 0.0001 and 0.1 seconds");
    if (!finite(s.gravity) || !scalar_finite(s.ground_z)) throw std::runtime_error("gravity and ground_z must be finite Bullet scalars");
    if (s.solver_iterations < 1 || s.solver_iterations > 1000) throw std::runtime_error("solver_iterations must be between 1 and 1000");
    if (s.bodies.size() > 10000) throw std::runtime_error("scenario exceeds 10000 bodies");
    std::set<std::uint32_t> ids;
    for (const auto& b : s.bodies) {
        if (!ids.insert(b.id).second) throw std::runtime_error("duplicate body id");
        if (!std::isfinite(b.mass) || b.mass < 0 || b.mass > 1e9) throw std::runtime_error("invalid body mass");
        if (b.mass > 0 && btScalar(b.mass) == 0) throw std::runtime_error("positive body mass underflows Bullet scalar");
        if (!finite(b.half_extents) || b.half_extents.x <= 0 || b.half_extents.y <= 0 || b.half_extents.z <= 0) throw std::runtime_error("box half_extents must be positive and finite");
        if (btScalar(b.half_extents.x) == 0 || btScalar(b.half_extents.y) == 0 || btScalar(b.half_extents.z) == 0) throw std::runtime_error("box half_extents underflow Bullet scalar");
        if (!finite(b.position) || !finite(b.velocity)) throw std::runtime_error("body position/velocity must be finite");
        if (!std::isfinite(b.friction) || b.friction < 0 || b.friction > 10 || !std::isfinite(b.restitution) || b.restitution < 0 || b.restitution > 1) throw std::runtime_error("invalid body material");
    }
}
}
Scenario default_scenario() { Scenario s; s.bodies.push_back(BodySpec{1}); return s; }
SimulationResult simulate(const Scenario& scenario, std::uint64_t ticks, const std::filesystem::path& trace, bool realtime) {
    validate(scenario);
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher(&config);
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world(&dispatcher, &broadphase, &solver, &config);
    world.setGravity(bt(scenario.gravity));
    world.getSolverInfo().m_numIterations = scenario.solver_iterations;
    // Bodies must be detached before they are destroyed, including on exceptions.
    std::vector<std::unique_ptr<btCollisionShape>> shapes;
    std::vector<std::unique_ptr<btRigidBody>> bodies;
    struct Detach { btDiscreteDynamicsWorld& world; std::vector<std::unique_ptr<btRigidBody>>& bodies; ~Detach() { for (auto& b : bodies) world.removeRigidBody(b.get()); } } detach{world, bodies};
    auto add = [&](std::unique_ptr<btCollisionShape> shape, const BodySpec& spec) {
        btVector3 inertia(0, 0, 0);
        if (spec.mass > 0) shape->calculateLocalInertia(btScalar(spec.mass), inertia);
        if (!finite(vec(inertia))) throw std::runtime_error("body inertia exceeds Bullet scalar range");
        btRigidBody::btRigidBodyConstructionInfo info(btScalar(spec.mass), nullptr, shape.get(), inertia);
        info.m_friction = btScalar(spec.friction);
        info.m_restitution = btScalar(spec.restitution);
        auto body = std::make_unique<btRigidBody>(info);
        btTransform transform; transform.setIdentity(); transform.setOrigin(bt(spec.position));
        body->setWorldTransform(transform);
        body->setLinearVelocity(bt(spec.velocity));
        shapes.push_back(std::move(shape));
        bodies.push_back(std::move(body));
        world.addRigidBody(bodies.back().get());
    };
    BodySpec ground; ground.mass = 0; ground.position = {0, 0, scenario.ground_z};
    add(std::make_unique<btStaticPlaneShape>(btVector3(0, 0, 1), 0), ground);
    for (const auto& body : scenario.bodies) add(std::make_unique<btBoxShape>(bt(body.half_extents)), body);
    std::ofstream output;
    if (!trace.empty()) {
        if (std::filesystem::exists(trace)) throw std::runtime_error("trace output already exists; choose a new path");
        output.open(trace, std::ios::out);
        if (!output) throw std::runtime_error("cannot create trace output");
        output << "tick,id,x,y,z,qx,qy,qz,qw,vx,vy,vz\n" << std::setprecision(9);
    }
    auto write_tick = [&](std::uint64_t tick) {
        if (!output.is_open()) return;
        for (std::size_t i = 0; i < scenario.bodies.size(); ++i) {
            const auto& body = *bodies[i + 1];
            validate_state(body);
            const auto& p = body.getWorldTransform().getOrigin();
            const auto& q = body.getWorldTransform().getRotation();
            const auto& v = body.getLinearVelocity();
            output << tick << ',' << scenario.bodies[i].id << ',' << p.x() << ',' << p.y() << ',' << p.z() << ',' << q.x() << ',' << q.y() << ',' << q.z() << ',' << q.w() << ',' << v.x() << ',' << v.y() << ',' << v.z() << '\n';
        }
        if (!output) throw std::runtime_error("failed to write trace output");
    };
    const auto start = std::chrono::steady_clock::now();
    write_tick(0);
    for (std::uint64_t index = 0; index < ticks; ++index) {
        const auto tick = index + 1;
        world.stepSimulation(btScalar(scenario.dt), 0, btScalar(scenario.dt));
        write_tick(tick);
        if (realtime) std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(tick * scenario.dt)));
    }
    if (output.is_open()) { output.flush(); if (!output) throw std::runtime_error("failed to flush trace output"); }
    SimulationResult result{ticks, scenario.dt, {}};
    for (std::size_t i = 0; i < scenario.bodies.size(); ++i) {
        const auto& body = *bodies[i + 1];
        validate_state(body);
        const auto& q = body.getWorldTransform().getRotation();
        result.bodies.push_back({scenario.bodies[i].id, vec(body.getWorldTransform().getOrigin()), vec(body.getLinearVelocity()), q.x(), q.y(), q.z(), q.w()});
    }
    return result;
}
std::string result_json(const Scenario& scenario, const SimulationResult& result) {
    std::ostringstream out;
    out << std::setprecision(10) << "{\"scenario\":" << json_quote(scenario.name)
        << ",\"physics\":\"upstream Bullet 3.25; game parity unverified\",\"ticks\":" << result.ticks
        << ",\"dt\":" << result.dt << ",\"simulated_seconds\":" << result.ticks * result.dt << ",\"bodies\":[";
    bool first = true;
    for (const auto& b : result.bodies) {
        if (!first) out << ','; first = false;
        out << "{\"id\":" << b.id << ",\"position\":[" << b.position.x << ',' << b.position.y << ',' << b.position.z
            << "],\"velocity\":[" << b.velocity.x << ',' << b.velocity.y << ',' << b.velocity.z
            << "],\"rotation\":[" << b.qx << ',' << b.qy << ',' << b.qz << ',' << b.qw << "]}";
    }
    return out.str() + "]}";
}
}
