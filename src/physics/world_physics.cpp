#include "physics/world_physics.hpp"
#include "data/state.hpp"
#include "data/assets.hpp"
#include "data/shape_geometry.hpp"
#include "data/terrain.hpp"
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace scrapds {
namespace {
// Bullet's cached hull bounds already include its collision margin. Avoid
// adding it a second time when composing a saved object's broadphase bounds.
class HullShape final : public btConvexHullShape {
public:
    void getAabb(const btTransform& transform,btVector3& minimum,btVector3& maximum) const override {
        getNonvirtualAabb(transform,minimum,maximum,btScalar(0));
    }
};
struct Body {
    std::uint32_t id{};
    std::uint16_t world_id{};
    bool dynamic{};
    std::vector<std::unique_ptr<btCollisionShape>> children;
    std::unique_ptr<btCompoundShape> shape;
    std::unique_ptr<btRigidBody> rigid;
    // Body save pose describes the construction origin; Bullet integrates the
    // compound's centre of mass. This transform maps COM into body-local space.
    btTransform body_to_com{btTransform::getIdentity()};
};
struct Scene {
    struct Terrain {
        bool overworld_cell{};
        std::int32_t cell_x{},cell_y{};
        std::unique_ptr<btTriangleMesh> mesh;
        std::unique_ptr<btBvhTriangleMeshShape> shape;
        std::unique_ptr<btRigidBody> rigid;
    };
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btSequentialImpulseConstraintSolver solver;
    btDiscreteDynamicsWorld world{&dispatcher,&broadphase,&solver,&config};
    std::vector<std::unique_ptr<Body>> bodies;
    std::vector<std::unique_ptr<Terrain>> terrain;
    std::vector<std::unique_ptr<btTypedConstraint>> constraints;
    Scene(){world.setGravity(btVector3(0,0,-10));world.getSolverInfo().m_numIterations=30;}
    ~Scene(){for(const auto& constraint:constraints)world.removeConstraint(constraint.get());for(const auto& body:bodies)world.removeRigidBody(body->rigid.get());for(const auto& item:terrain)world.removeRigidBody(item->rigid.get());}
};
void put_float(Blob& bytes,std::size_t offset,double value) {
    if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())throw std::runtime_error("nonfinite-physics-state");
    const auto word=std::bit_cast<std::uint32_t>(static_cast<float>(value));
    for(int i=0;i<4;++i)bytes.at(offset+i)=std::uint8_t(word>>(24-8*i));
}
bool finite(Vec3 v) {
    const auto good=[](double x){return std::isfinite(x)&&std::abs(x)<=std::numeric_limits<btScalar>::max();};
    return good(v.x)&&good(v.y)&&good(v.z);
}
}
struct WorldPhysics::Impl {
    WorldPhysicsCoverage coverage;
    bool stepped{};
    std::map<std::uint16_t,std::unique_ptr<Scene>> scenes;
    std::map<std::uint32_t,Body*> body_index;
    std::pair<std::uint64_t,std::uint64_t> world_version;
};
WorldPhysics::WorldPhysics(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
WorldPhysics::WorldPhysics(WorldPhysics&&) noexcept=default;
WorldPhysics& WorldPhysics::operator=(WorldPhysics&&) noexcept=default;
WorldPhysics::~WorldPhysics()=default;
WorldPhysics WorldPhysics::from_world(const WorldState& state,double density,const std::set<std::uint32_t>* mapped_dynamic_ids,const AssetDefinitions* assets) {
    if(!std::isfinite(density)||density<=0||density>1e7)throw std::invalid_argument("invalid-block-density");
    auto impl=std::make_unique<Impl>();impl->coverage.source_bodies=state.bodies().size();
    if(const auto terrain=state.table_counts().find("VoxelTerrain");terrain!=state.table_counts().end())impl->coverage.terrain_chunks=terrain->second;
    impl->world_version=state.physics_version();
    std::map<std::uint32_t,std::vector<const StoredShape*>> shapes;
    for(const auto& [id,shape]:state.shapes()){(void)id;shapes[shape.body_id].push_back(&shape);}
    std::set<std::uint32_t> joint_bodies;
    for(const auto& [id,joint]:state.joints()) {
        (void)id;
        for(const auto shape_id:{joint.shape_a,joint.shape_b}) {
            const auto it=state.shapes().find(shape_id);if(it!=state.shapes().end())joint_bodies.insert(it->second.body_id);
        }
    }
    std::set<std::uint32_t> controller_bodies;
    for(const auto& [id,controller]:state.controllers()) {
        if(controller.host_type==1) {
            const auto shape=state.shapes().find(controller.host_id);
            if(shape!=state.shapes().end()) {
                bool passive=false;const auto& d=controller.data;
                if(d.size()==24&&d[0]==3&&d[1]==25&&d[2]==1&&d[11]==1&&d[16]==1&&d[17]==0&&d[18]==0) {
                    const auto word=[&](std::size_t at){return (std::uint32_t(d[at])<<24)|(std::uint32_t(d[at+1])<<16)|(std::uint32_t(d[at+2])<<8)|d[at+3];};
                    if(word(3)==id&&word(7)==controller.host_id&&word(12)==controller.host_id)try {
                        const auto decoded=decode_child_geometry(shape->second.data);
                        passive=decoded&&decoded->uuid=="ed27f5e2-cac5-4a32-a5d9-49f116acc6af";
                    }catch(const std::runtime_error&) {}
                }
                if(passive)++impl->coverage.passive_controllers;else controller_bodies.insert(shape->second.body_id);
            }
        } else if(controller.host_type!=2) {
            // Unknown host kinds remain conservative; joint hosts are handled
            // separately and must not be confused with coincident body IDs.
            if(state.bodies().contains(controller.host_id))controller_bodies.insert(controller.host_id);
            const auto shape=state.shapes().find(controller.host_id);if(shape!=state.shapes().end())controller_bodies.insert(shape->second.body_id);
        }
    }
    struct Spring {std::uint32_t length{};std::uint8_t stiffness{};};
    struct Bearing { JointGeometry joint;std::uint32_t body_a{},body_b{};std::optional<Spring> spring; };
    std::vector<Bearing> bearings;std::map<std::uint32_t,std::set<std::uint32_t>> graph;std::set<std::uint32_t> invalid_joint_bodies;
    std::map<std::uint32_t,std::vector<const StoredController*>> joint_controllers;for(const auto& [id,controller]:state.controllers()){(void)id;if(controller.host_type==2)joint_controllers[controller.host_id].push_back(&controller);}
    for(const auto& [id,joint]:state.joints()) {
        const auto sa=state.shapes().find(joint.shape_a),sb=state.shapes().find(joint.shape_b);
        const bool a=sa!=state.shapes().end(),b=sb!=state.shapes().end();
        if(a)graph[sa->second.body_id];if(b)graph[sb->second.body_id];
        if(!a||!b){if(a)invalid_joint_bodies.insert(sa->second.body_id);if(b)invalid_joint_bodies.insert(sb->second.body_id);continue;}
        const auto ba=sa->second.body_id,bb=sb->second.body_id;graph[ba].insert(bb);graph[bb].insert(ba);
        try {
            const auto decoded=decode_joint_geometry(joint.data);
            if(!decoded||decoded->id!=id||decoded->shape_a!=joint.shape_a||decoded->shape_b!=joint.shape_b||ba==bb)throw std::runtime_error("unsupported-joint");
            std::optional<Spring> spring;
            if(decoded->kind==3&&decoded->uuid=="4a1b886b-913e-4aad-b5b6-6e41b0db23a6"&&!joint_controllers.contains(id)) {}
            else if(decoded->kind==4&&assets) {
                const auto* asset=assets->find(decoded->uuid);const auto controllers=joint_controllers.find(id);
                if(!asset||!asset->survival_spring_length||controllers==joint_controllers.end()||controllers->second.size()!=1)throw std::runtime_error("unsupported-spring-asset");
                const auto* controller=controllers->second.front();const auto setting=decode_survival_spring_setting(controller->data,controller->id,id);
                if(!setting)throw std::runtime_error("unsupported-spring-controller");spring=Spring{*asset->survival_spring_length,*setting};
            }else throw std::runtime_error("unsupported-joint-kind");
            bearings.push_back({*decoded,ba,bb,spring});
        }catch(const std::runtime_error&){invalid_joint_bodies.insert(ba);invalid_joint_bodies.insert(bb);}
    }
    const auto body_supported=[&](std::uint32_t id) {
        if(invalid_joint_bodies.contains(id)||controller_bodies.contains(id))return false;
        const auto body=state.bodies().find(id);const auto group=shapes.find(id);if(body==state.bodies().end()||group==shapes.end()||group->second.empty())return false;
        try {
            const auto pose=decode_rigid_body_pose(body->second.data);if(!pose||(!pose->is_static&&mapped_dynamic_ids&&!mapped_dynamic_ids->contains(id)))return false;
            double volume=0;
            for(const auto* item:group->second) {
                const auto shape=decode_child_geometry(item->data);if(!shape)return false;std::array<double,3> d{};bool cyl=false;
                if(shape->kind==SavedShapeKind::block&&shape->grid_dimensions)for(int i=0;i<3;++i)d[i]=(*shape->grid_dimensions)[i];
                else if(shape->kind==SavedShapeKind::part&&assets) {const auto* asset=assets->find(shape->uuid);if(!asset)return false;
                    if(asset->box_dimensions)d=*asset->box_dimensions;
                    else if(asset->cylinder_dimensions){const auto& c=*asset->cylinder_dimensions;d.fill(c.diameter);d[c.axis]=c.depth;cyl=true;}
                    else if(asset->hull_geometry)d=asset->hull_geometry->dimensions;
                    else return false;
                }else return false;
                if(d[0]<=0||d[1]<=0||d[2]<=0)return false;volume+=d[0]*d[1]*d[2]/64*(cyl?0.7853981633974483:1.0);
            }
            return std::isfinite(volume)&&volume>0&&(pose->is_static||volume*density<=1e9);
        }catch(const std::runtime_error&){return false;}
    };
    const auto saved_frame=[&](std::uint32_t id,const std::array<std::int32_t,3>& grid,const std::array<std::int32_t,3>& axis) {
        const auto pose=*decode_rigid_body_pose(state.bodies().at(id).data);btQuaternion q(pose.rotation_xyzw[0],pose.rotation_xyzw[1],pose.rotation_xyzw[2],pose.rotation_xyzw[3]);q.normalize();
        const btTransform transform(q,btVector3(pose.position[0],pose.position[1],pose.position[2]));
        return std::pair{transform*btVector3(btScalar(grid[0]*0.25+0.125),btScalar(grid[1]*0.25+0.125),btScalar(grid[2]*0.25+0.125)),transform.getBasis()*btVector3(btScalar(axis[0]),btScalar(axis[1]),btScalar(axis[2]))};
    };
    for(const auto& bearing:bearings)try {
        const auto pa=decode_rigid_body_pose(state.bodies().at(bearing.body_a).data),pb=decode_rigid_body_pose(state.bodies().at(bearing.body_b).data);
        if(!pa||!pb||pa->world_id!=pb->world_id)throw std::runtime_error("bearing-world");
        const auto a=saved_frame(bearing.body_a,bearing.joint.grid_a,bearing.joint.axis_a),b=saved_frame(bearing.body_b,bearing.joint.grid_b,bearing.joint.axis_b);
        if(bearing.spring) {
            const auto delta=b.first-a.first;const auto along=delta.dot(a.second);const auto lateral=delta-a.second*along;
            const auto ra=saved_frame(bearing.body_a,bearing.joint.grid_a,bearing.joint.reference_a).second,rb=saved_frame(bearing.body_b,bearing.joint.grid_b,bearing.joint.reference_b).second;
            if(a.second.dot(b.second)<0.98||ra.dot(rb)<0.98||lateral.length()>0.15||along<0||along>btScalar(bearing.spring->length*0.25+0.25))throw std::runtime_error("spring-frames");
        } else if((a.first-b.first).length()>0.25||std::abs(double(a.second.dot(b.second)))<0.98)throw std::runtime_error("bearing-frames");
    }catch(const std::exception&){invalid_joint_bodies.insert(bearing.body_a);invalid_joint_bodies.insert(bearing.body_b);}
    std::set<std::uint32_t> seen,enabled_joint_bodies;
    for(const auto& [root,adjacent]:graph)if(!seen.contains(root)) {
        (void)adjacent;std::vector<std::uint32_t> stack{root},component;bool valid=true;
        while(!stack.empty()){const auto id=stack.back();stack.pop_back();if(!seen.insert(id).second)continue;component.push_back(id);if(!body_supported(id))valid=false;for(const auto neighbour:graph.at(id))stack.push_back(neighbour);}
        if(valid)enabled_joint_bodies.insert(component.begin(),component.end());
    }
    for(const auto& [id,saved]:state.bodies()) {
        std::optional<RigidBodyPose> pose;
        try{pose=decode_rigid_body_pose(saved.data);}catch(const std::runtime_error&){++impl->coverage.skipped_invalid;continue;}
        if(!pose){++impl->coverage.skipped_invalid;continue;}
        // A static anchor remains a valid collider even if unsupported joints
        // or controllers attach to it. Only the dynamic side must be excluded.
        if(!pose->is_static&&joint_bodies.contains(id)&&!enabled_joint_bodies.contains(id)){++impl->coverage.skipped_joint;continue;}
        if(!pose->is_static&&controller_bodies.contains(id)){++impl->coverage.skipped_controller;continue;}
        if(!pose->is_static&&mapped_dynamic_ids&&!mapped_dynamic_ids->contains(id)){++impl->coverage.skipped_unmapped;continue;}
        const auto group=shapes.find(id);
        if(group==shapes.end()||group->second.empty()){++impl->coverage.skipped_geometry;continue;}
        auto body=std::make_unique<Body>();body->id=id;body->world_id=pose->world_id;body->dynamic=!pose->is_static;
        body->shape=std::make_unique<btCompoundShape>();body->shape->setMargin(0);
        double volume=0;bool valid=true;std::uint64_t part_boxes=0,part_cylinders=0,part_hulls=0;std::vector<btScalar> child_masses;
        for(const auto* item:group->second) {
            std::optional<ShapeGeometry> geometry;
            try{geometry=decode_child_geometry(item->data);}catch(const std::runtime_error&){valid=false;break;}
            if(!geometry){valid=false;break;}
            std::array<double,3> d{};std::optional<CylinderDimensions> cylinder;const HullGeometry* hull=nullptr;
            if(geometry->kind==SavedShapeKind::block&&geometry->grid_dimensions)for(int i=0;i<3;++i)d[i]=(*geometry->grid_dimensions)[i];
            else if(geometry->kind==SavedShapeKind::part&&assets) {
                const auto* asset=assets->find(geometry->uuid);if(!asset){valid=false;break;}
                if(asset->box_dimensions){d=*asset->box_dimensions;++part_boxes;}
                else if(asset->cylinder_dimensions){cylinder=asset->cylinder_dimensions;d.fill(cylinder->diameter);d[cylinder->axis]=cylinder->depth;++part_cylinders;}
                else if(asset->hull_geometry){hull=&*asset->hull_geometry;d=hull->dimensions;++part_hulls;}
                else {valid=false;break;}
            } else {valid=false;break;}
            const auto& p=geometry->grid_position;
            if(d[0]<=0||d[1]<=0||d[2]<=0){valid=false;break;}
            const auto child_volume=cylinder?3.14159265358979323846*std::pow(cylinder->diameter*0.125,2)*(cylinder->depth*0.25):double(d[0])*d[1]*d[2]/64.0;volume+=child_volume;
            child_masses.push_back(btScalar(child_volume*density));
            const btVector3 half{btScalar(d[0]*0.125),btScalar(d[1]*0.125),btScalar(d[2]*0.125)};
            std::unique_ptr<btCollisionShape> child;
            if(hull) {
                std::unique_ptr<btConvexHullShape> shape;if(hull->degenerate)shape=std::make_unique<btConvexHullShape>();else shape=std::make_unique<HullShape>();
                for(const auto& p:hull->vertices)shape->addPoint(btVector3(btScalar(p[0]*0.25),btScalar(p[1]*0.25),btScalar(p[2]*0.25)),false);
                shape->setMargin(btScalar(hull->margin));shape->recalcLocalAabb();child=std::move(shape);
            }
            else if(!cylinder)child=std::make_unique<btBoxShape>(half);
            else if(cylinder->axis==0)child=std::make_unique<btCylinderShapeX>(half);
            else if(cylinder->axis==1)child=std::make_unique<btCylinderShape>(half);
            else child=std::make_unique<btCylinderShapeZ>(half);
            if(!hull)child->setMargin(0);btTransform local;local.setIdentity();
            const auto& xa=geometry->x_axis;const auto& za=geometry->z_axis;
            const btVector3 x{btScalar(xa[0]),btScalar(xa[1]),btScalar(xa[2])};
            const btVector3 z{btScalar(za[0]),btScalar(za[1]),btScalar(za[2])};const auto y=z.cross(x);
            const btMatrix3x3 basis(x.x(),y.x(),z.x(),x.y(),y.y(),z.y(),x.z(),y.z(),z.z());
            local.setBasis(basis);
            local.setOrigin(btVector3(btScalar(p[0]*0.25),btScalar(p[1]*0.25),btScalar(p[2]*0.25))+basis*btVector3(btScalar(d[0]*0.125),btScalar(d[1]*0.125),btScalar(d[2]*0.125)));
            body->shape->addChildShape(local,child.get());body->children.push_back(std::move(child));
        }
        const double mass=body->dynamic?volume*density:0;
        if(!valid||!std::isfinite(mass)||mass>1e9){++impl->coverage.skipped_geometry;continue;}
        if(body->dynamic&&(btScalar(mass)<=0||std::any_of(child_masses.begin(),child_masses.end(),[](btScalar m){return m<=0||!std::isfinite(double(m));}))) {++impl->coverage.skipped_invalid;continue;}
        btVector3 inertia(0,0,0);
        if(mass>0) {
            body->shape->calculatePrincipalAxisTransform(child_masses.data(),body->body_to_com,inertia);
            const auto inverse=body->body_to_com.inverse();
            for(int i=0;i<body->shape->getNumChildShapes();++i)
                body->shape->updateChildTransform(i,inverse*body->shape->getChildTransform(i),false);
            body->shape->recalculateLocalAabb();
            if(!std::isfinite(double(inertia.x()))||!std::isfinite(double(inertia.y()))||!std::isfinite(double(inertia.z()))) {++impl->coverage.skipped_invalid;continue;}
        }
        btRigidBody::btRigidBodyConstructionInfo info(btScalar(mass),nullptr,body->shape.get(),inertia);
        body->rigid=std::make_unique<btRigidBody>(info);
        const auto& q=pose->rotation_xyzw;const auto& p=pose->position;
        btQuaternion rotation(q[0],q[1],q[2],q[3]);rotation.normalize();
        body->rigid->setWorldTransform(btTransform(rotation,btVector3(p[0],p[1],p[2]))*body->body_to_com);
        if(body->dynamic) {
            const auto& v=*pose->linear_velocity();const auto& a=*pose->angular_velocity();
            body->rigid->setLinearVelocity(btVector3(v[0],v[1],v[2]));body->rigid->setAngularVelocity(btVector3(a[0],a[1],a[2]));
        }
        auto& scene=impl->scenes[body->world_id];if(!scene)scene=std::make_unique<Scene>();
        if(body->dynamic)++impl->coverage.dynamic_bodies;else ++impl->coverage.static_bodies;
        impl->coverage.box_parts+=part_boxes;
        impl->coverage.cylinder_parts+=part_cylinders;
        impl->coverage.hull_parts+=part_hulls;
        scene->bodies.push_back(std::move(body));
        scene->world.addRigidBody(scene->bodies.back()->rigid.get());
        impl->body_index.emplace(id,scene->bodies.back().get());
    }
    for(const auto& bearing:bearings)if(enabled_joint_bodies.contains(bearing.body_a)&&enabled_joint_bodies.contains(bearing.body_b)) {
        const auto ia=impl->body_index.find(bearing.body_a),ib=impl->body_index.find(bearing.body_b);
        if(ia==impl->body_index.end()||ib==impl->body_index.end())throw std::runtime_error("supported-bearing-body-was-not-created");
        auto& a=*ia->second;auto& b=*ib->second;const auto inverse_a=a.body_to_com.inverse(),inverse_b=b.body_to_com.inverse();
        const auto point=[](const auto& g){return btVector3(btScalar(g[0]*0.25+0.125),btScalar(g[1]*0.25+0.125),btScalar(g[2]*0.25+0.125));};
        const auto direction=[](const auto& v){return btVector3(btScalar(v[0]),btScalar(v[1]),btScalar(v[2]));};
        const auto pivot_a=inverse_a*point(bearing.joint.grid_a),pivot_b=inverse_b*point(bearing.joint.grid_b);
        const auto axis_a=inverse_a.getBasis()*direction(bearing.joint.axis_a);auto axis_b=inverse_b.getBasis()*direction(bearing.joint.axis_b);
        if((a.rigid->getWorldTransform().getBasis()*axis_a).dot(b.rigid->getWorldTransform().getBasis()*axis_b)<0)axis_b=-axis_b;
        std::unique_ptr<btTypedConstraint> constraint;
        if(bearing.spring) {
            const auto frame=[&](const btTransform& inverse,const auto& grid,const auto& primary,const auto& reference){
                const auto z=direction(primary),x=direction(reference),y=z.cross(x);const btMatrix3x3 basis(x.x(),y.x(),z.x(),x.y(),y.y(),z.y(),x.z(),y.z(),z.z());
                return inverse*btTransform(basis,point(grid));};
            auto spring=std::make_unique<btGeneric6DofSpring2Constraint>(*a.rigid,*b.rigid,frame(inverse_a,bearing.joint.grid_a,bearing.joint.axis_a,bearing.joint.reference_a),frame(inverse_b,bearing.joint.grid_b,bearing.joint.axis_b,bearing.joint.reference_b));
            constexpr std::array<float,19> stiffness{0,7500,20000,37500,87500,120000,157500,200000,247500,300000,357000,420000,487500,560000,637500,720000,807000,900000,997000};
            spring->setLinearLowerLimit(btVector3(0,0,0.25));spring->setLinearUpperLimit(btVector3(0,0,btScalar(bearing.spring->length*0.25)));
            spring->setAngularLowerLimit(btVector3(0,0,0));spring->setAngularUpperLimit(btVector3(0,0,0));spring->enableSpring(2,true);
            spring->setStiffness(2,stiffness[bearing.spring->stiffness],false);spring->setDamping(2,2500,true);spring->setEquilibriumPoint(2,btScalar((bearing.spring->length+1)*0.25));
            constraint=std::move(spring);++impl->coverage.spring_joints;
        } else {constraint=std::make_unique<btHingeConstraint>(*a.rigid,*b.rigid,pivot_a,pivot_b,axis_a,axis_b,true);++impl->coverage.bearing_joints;}
        auto& scene=*impl->scenes.at(a.world_id);scene.constraints.push_back(std::move(constraint));scene.world.addConstraint(scene.constraints.back().get(),true);
    }
    WorldPhysics physics(std::move(impl));
    // CreativeFlatWorld's terrain_flat.lua returns height zero; its cell bounds
    // are -16..15 and the game's terrain cell size is 64 world units.
    const std::array<TerrainVertex,6> flat{{{-1024,-1024,0},{1024,-1024,0},{1024,1024,0},{-1024,-1024,0},{1024,1024,0},{-1024,1024,0}}};
    for(const auto world_id:state.flat_surface_worlds()){physics.add_terrain_mesh(world_id,flat);++physics.impl_->coverage.flat_surface_worlds;}
    return physics;
}
const WorldPhysicsCoverage& WorldPhysics::coverage() const{return impl_->coverage;}
void WorldPhysics::step(std::uint64_t ticks) {
    if(ticks>10000000)throw std::invalid_argument("too-many-physics-ticks");
    // Keep the network tick at 25 ms while resolving thin geometry more often.
    for(std::uint64_t i=0;i<ticks;++i)for(auto& [id,scene]:impl_->scenes){(void)id;scene->world.stepSimulation(btScalar(0.025),4,btScalar(0.00625));}
    if(ticks)impl_->stepped=true;
}
namespace {
btVector3 checked_vector(Vec3 value) {
    for(const auto component:{value.x,value.y,value.z})if(!std::isfinite(component)||std::abs(component)>std::numeric_limits<btScalar>::max())throw std::invalid_argument("invalid-physics-vector");
    return btVector3(btScalar(value.x),btScalar(value.y),btScalar(value.z));
}
}
void WorldPhysics::apply_impulse(std::uint32_t id,Vec3 impulse,Vec3 relative_position) {
    const auto force=checked_vector(impulse),position=checked_vector(relative_position);const auto it=impl_->body_index.find(id);
    if(it==impl_->body_index.end()||!it->second->dynamic)throw std::invalid_argument("unknown-or-static-physics-body");
    it->second->rigid->activate(true);it->second->rigid->applyImpulse(force,position);impl_->stepped=true;
}
void WorldPhysics::apply_force(std::uint32_t id,Vec3 force,Vec3 relative_position) {
    const auto value=checked_vector(force),position=checked_vector(relative_position);const auto it=impl_->body_index.find(id);
    if(it==impl_->body_index.end()||!it->second->dynamic)throw std::invalid_argument("unknown-or-static-physics-body");
    it->second->rigid->activate(true);it->second->rigid->applyForce(value,position);
}
WorldPhysicsCapsuleMotion WorldPhysics::move_character_capsule(std::uint16_t world_id,Vec3 start,Vec3 displacement,double radius,double cylinder_height,double skin,unsigned max_iterations) const {
    if(!finite(start)||!finite(displacement)||!std::isfinite(radius)||!std::isfinite(cylinder_height)||
       !std::isfinite(skin)||radius<=0||cylinder_height<0||skin<0||max_iterations==0||max_iterations>16||
       radius>std::numeric_limits<btScalar>::max()||cylinder_height>std::numeric_limits<btScalar>::max())
        throw std::invalid_argument("invalid-character-capsule-motion");
    const auto scene=impl_->scenes.find(world_id);
    if(scene==impl_->scenes.end())return {{start.x+displacement.x,start.y+displacement.y,start.z+displacement.z},0};
    const btScalar bullet_radius=btScalar(radius),bullet_height=btScalar(cylinder_height);
    if(bullet_radius<=0||!std::isfinite(double(bullet_radius))||!std::isfinite(double(bullet_height)))
        throw std::invalid_argument("invalid-character-capsule-dimensions");
    btCapsuleShapeZ capsule(bullet_radius,bullet_height);Vec3 position=start,remaining=displacement;std::uint32_t contacts=0;
    for(unsigned i=0;i<max_iterations;++i) {
        const auto length=std::sqrt(remaining.x*remaining.x+remaining.y*remaining.y+remaining.z*remaining.z);
        if(!std::isfinite(length))throw std::invalid_argument("character-capsule-motion-too-large");
        if(length<1e-7)break;
        btTransform from,to;from.setIdentity();to.setIdentity();from.setOrigin(checked_vector(position));
        const Vec3 end{position.x+remaining.x,position.y+remaining.y,position.z+remaining.z};
        to.setOrigin(checked_vector(end));
        btCollisionWorld::ClosestConvexResultCallback callback(from.getOrigin(),to.getOrigin());
        scene->second->world.convexSweepTest(&capsule,from,to,callback);
        if(!callback.hasHit()){position=end;break;}
        ++contacts;
        const auto normal=callback.m_hitNormalWorld;
        const auto fraction=std::clamp(double(callback.m_closestHitFraction),0.0,1.0);
        const auto advance=std::max(0.0,fraction-skin/length);
        position={position.x+remaining.x*advance,position.y+remaining.y*advance,position.z+remaining.z*advance};
        remaining={remaining.x*(1-advance),remaining.y*(1-advance),remaining.z*(1-advance)};
        const auto into=remaining.x*normal.x()+remaining.y*normal.y()+remaining.z*normal.z();
        if(into>=-1e-9)break;
        remaining={remaining.x-into*normal.x(),remaining.y-into*normal.y(),remaining.z-into*normal.z()};
    }
    return {position,contacts};
}
void WorldPhysics::add_saved_overworld_cell(std::uint16_t world_id,const OverworldTerrain& terrain,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog,const OverworldCell& cell) {
    const auto vertices=mesh_saved_overworld_cell(terrain,catalog,cell);add_overworld_cell_mesh(world_id,cell.x,cell.y,vertices);
}
void WorldPhysics::add_terrain_mesh(std::uint16_t world_id,std::span<const TerrainVertex> vertices) {
    add_terrain_mesh_impl(world_id,std::nullopt,vertices);
}
void WorldPhysics::add_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y,std::span<const TerrainVertex> vertices) {
    add_terrain_mesh_impl(world_id,std::pair{cell_x,cell_y},vertices);
}
void WorldPhysics::add_terrain_mesh_impl(std::uint16_t world_id,std::optional<std::pair<std::int32_t,std::int32_t>> cell,std::span<const TerrainVertex> vertices) {
    if(vertices.empty())return;
    if(vertices.size()%3||vertices.size()>3000000)throw std::invalid_argument("invalid terrain triangle count");
    for(const auto& v:vertices)for(const auto n:{v.x,v.y,v.z})if(!std::isfinite(n)||std::abs(double(n))>std::numeric_limits<btScalar>::max())throw std::invalid_argument("invalid terrain vertex");
    auto terrain=std::make_unique<Scene::Terrain>();if(cell){terrain->overworld_cell=true;terrain->cell_x=cell->first;terrain->cell_y=cell->second;}terrain->mesh=std::make_unique<btTriangleMesh>();
    for(std::size_t i=0;i<vertices.size();i+=3){const auto& a=vertices[i];const auto& b=vertices[i+1];const auto& c=vertices[i+2];terrain->mesh->addTriangle(btVector3(a.x,a.y,a.z),btVector3(b.x,b.y,b.z),btVector3(c.x,c.y,c.z));}
    terrain->shape=std::make_unique<btBvhTriangleMeshShape>(terrain->mesh.get(),true);terrain->shape->setMargin(0);
    btRigidBody::btRigidBodyConstructionInfo info(0,nullptr,terrain->shape.get(),btVector3(0,0,0));terrain->rigid=std::make_unique<btRigidBody>(info);terrain->rigid->setWorldTransform(btTransform::getIdentity());
    auto& scene=impl_->scenes[world_id];if(!scene)scene=std::make_unique<Scene>();scene->terrain.push_back(std::move(terrain));scene->world.addRigidBody(scene->terrain.back()->rigid.get());impl_->coverage.terrain_triangles+=vertices.size()/3;
}
void WorldPhysics::remove_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y) {
    const auto found=impl_->scenes.find(world_id);if(found==impl_->scenes.end())return;auto& scene=*found->second;
    for(auto it=scene.terrain.begin();it!=scene.terrain.end();) {
        const auto& terrain=**it;
        if(terrain.overworld_cell&&terrain.cell_x==cell_x&&terrain.cell_y==cell_y) {
            scene.world.removeRigidBody(terrain.rigid.get());
            const auto triangles=terrain.mesh->getNumTriangles();impl_->coverage.terrain_triangles-=static_cast<std::uint64_t>(triangles);it=scene.terrain.erase(it);
        } else ++it;
    }
}
std::vector<WorldPhysicsBody> WorldPhysics::bodies() const {
    std::vector<WorldPhysicsBody> result;
    for(const auto& [world,scene]:impl_->scenes)for(const auto& body:scene->bodies)if(body->dynamic) {
        const auto t=body->rigid->getWorldTransform()*body->body_to_com.inverse();const auto& p=t.getOrigin();const auto q=t.getRotation();const auto v=body->rigid->getLinearVelocity();const auto a=body->rigid->getAngularVelocity();
        result.push_back({body->id,world,{p.x(),p.y(),p.z()},{v.x(),v.y(),v.z()},{a.x(),a.y(),a.z()},q.x(),q.y(),q.z(),q.w()});
    }
    return result;
}
std::optional<WorldPhysicsBounds> WorldPhysics::bounds(std::uint32_t id) const {
    const auto it=impl_->body_index.find(id);if(it==impl_->body_index.end())return std::nullopt;
    btVector3 low,high;it->second->shape->getAabb(it->second->rigid->getWorldTransform(),low,high);
    return WorldPhysicsBounds{{low.x(),low.y(),low.z()},{high.x(),high.y(),high.z()}};
}
void WorldPhysics::writeback(WorldState& state) const {
    if(state.physics_version()!=impl_->world_version)throw std::runtime_error("physics-world-changed-since-load");
    if(!impl_->stepped)return;
    std::map<std::uint32_t,Blob> updates;
        for(const auto& [world,scene]:impl_->scenes)for(const auto& body:scene->bodies)if(body->dynamic) {
            const auto found=state.bodies().find(body->id);
            if(found==state.bodies().end()||found->second.world_id!=world||found->second.data.size()!=80||found->second.data[1]!=2)
                throw std::runtime_error("physics-body-changed-since-load");
            auto bytes=found->second.data;const auto& com=body->rigid->getWorldTransform();const auto t=com*body->body_to_com.inverse();const auto p=t.getOrigin();const auto q=t.getRotation();const auto v=body->rigid->getLinearVelocity();const auto a=body->rigid->getAngularVelocity();
            btVector3 low,high;body->shape->getAabb(com,low,high);
            put_float(bytes,9,high.y());put_float(bytes,13,low.y());put_float(bytes,17,high.x());put_float(bytes,21,low.x());
            for(int i=0;i<4;++i)put_float(bytes,27+4*i,q[i]);
            for(int i=0;i<3;++i){put_float(bytes,43+4*i,p[i]);put_float(bytes,55+4*i,v[i]);put_float(bytes,67+4*i,a[i]);}
            updates.emplace(body->id,std::move(bytes));
        }
    state.replace_body_blobs(std::move(updates));
    impl_->world_version=state.physics_version();
    impl_->stepped=false;
}
std::string WorldPhysics::summary_json() const {
    const auto& c=coverage();return "{\"mode\":\"experimental-block-bodies\",\"authoritative_game_parity\":false,\"source_bodies\":"+std::to_string(c.source_bodies)+",\"static_bodies\":"+std::to_string(c.static_bodies)+",\"dynamic_bodies\":"+std::to_string(c.dynamic_bodies)+",\"box_parts\":"+std::to_string(c.box_parts)+",\"cylinder_parts\":"+std::to_string(c.cylinder_parts)+",\"hull_parts\":"+std::to_string(c.hull_parts)+",\"bearing_joints\":"+std::to_string(c.bearing_joints)+",\"spring_joints\":"+std::to_string(c.spring_joints)+",\"passive_controllers\":"+std::to_string(c.passive_controllers)+",\"skipped_geometry\":"+std::to_string(c.skipped_geometry)+",\"skipped_joint\":"+std::to_string(c.skipped_joint)+",\"skipped_controller\":"+std::to_string(c.skipped_controller)+",\"skipped_unmapped\":"+std::to_string(c.skipped_unmapped)+",\"skipped_invalid\":"+std::to_string(c.skipped_invalid)+",\"unsupported_terrain_chunks\":"+std::to_string(c.terrain_chunks)+",\"terrain_triangles\":"+std::to_string(c.terrain_triangles)+",\"flat_surface_worlds\":"+std::to_string(c.flat_surface_worlds)+"}";
}
}
