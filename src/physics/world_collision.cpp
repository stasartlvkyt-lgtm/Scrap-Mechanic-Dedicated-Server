#include "world_collision.hpp"
#include "data/state.hpp"
#include "data/shape_geometry.hpp"
#include <btBulletCollisionCommon.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace scrapds {
namespace {
btVector3 bullet(Vec3 v) { return {btScalar(v.x), btScalar(v.y), btScalar(v.z)}; }
Vec3 native(const btVector3& v) { return {v.x(), v.y(), v.z()}; }
bool finite(Vec3 v) {
    const auto good=[](double x){return std::isfinite(x) && std::abs(x)<=std::numeric_limits<btScalar>::max();};
    return good(v.x)&&good(v.y)&&good(v.z);
}
struct Identity { std::uint32_t shape_id, body_id; std::uint16_t world_id; };
struct Scene {
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btCollisionWorld world{&dispatcher, &broadphase, &config};
    std::deque<Identity> identities;
    std::vector<std::unique_ptr<btBoxShape>> shapes;
    std::vector<std::unique_ptr<btCollisionObject>> objects;
    std::map<std::uint32_t, CollisionBounds> body_bounds;
    ~Scene() { for (auto& object : objects) world.removeCollisionObject(object.get()); }
    void add(const CollisionBlock& block) {
        if (!finite(block.body_position)) throw std::runtime_error("invalid block body position");
        double norm=0;
        for (const auto q : block.body_rotation_xyzw) {
            if(!std::isfinite(q))throw std::runtime_error("invalid block body quaternion");
            norm+=q*q;
        }
        if(std::abs(norm-1)>0.001)throw std::runtime_error("block body quaternion is not normalized");
        for(auto size:block.grid_dimensions)if(size<=0)throw std::runtime_error("block collision dimensions must be positive");
        const auto& p=block.grid_position;const auto& d=block.grid_dimensions;
        const Vec3 center{(double(p[0])+double(d[0])*0.5)*0.25,(double(p[1])+double(d[1])*0.5)*0.25,(double(p[2])+double(d[2])*0.5)*0.25};
        const btVector3 half(btScalar(double(d[0])*0.125),btScalar(double(d[1])*0.125),btScalar(double(d[2])*0.125));
        btQuaternion q(btScalar(block.body_rotation_xyzw[0]),btScalar(block.body_rotation_xyzw[1]),btScalar(block.body_rotation_xyzw[2]),btScalar(block.body_rotation_xyzw[3]));
        q.normalize();
        btTransform parent(q,bullet(block.body_position));
        btTransform local;local.setIdentity();local.setOrigin(bullet(center));
        auto shape=std::make_unique<btBoxShape>(half);
        shape->setMargin(btScalar(0));
        auto object=std::make_unique<btCollisionObject>();
        object->setCollisionShape(shape.get());object->setWorldTransform(parent*local);
        identities.push_back({block.shape_id,block.body_id,block.world_id});object->setUserPointer(&identities.back());
        btVector3 lo,hi;shape->getAabb(object->getWorldTransform(),lo,hi);
        auto [at,inserted]=body_bounds.emplace(block.body_id,CollisionBounds{native(lo),native(hi)});
        if(!inserted){auto& b=at->second;b.minimum={std::min(b.minimum.x,double(lo.x())),std::min(b.minimum.y,double(lo.y())),std::min(b.minimum.z,double(lo.z()))};b.maximum={std::max(b.maximum.x,double(hi.x())),std::max(b.maximum.y,double(hi.y())),std::max(b.maximum.z,double(hi.z()))};}
        shapes.push_back(std::move(shape));objects.push_back(std::move(object));world.addCollisionObject(objects.back().get());
    }
};
}
struct WorldCollision::Impl {
    CollisionCoverage coverage;
    std::map<std::uint16_t,std::unique_ptr<Scene>> scenes;
    void add(const CollisionBlock& block){auto& scene=scenes[block.world_id];if(!scene)scene=std::make_unique<Scene>();scene->add(block);++coverage.block_boxes;coverage.worlds=scenes.size();}
};
WorldCollision::WorldCollision(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
WorldCollision::WorldCollision(WorldCollision&&) noexcept=default;
WorldCollision& WorldCollision::operator=(WorldCollision&&) noexcept=default;
WorldCollision::~WorldCollision()=default;
WorldCollision WorldCollision::from_blocks(const std::vector<CollisionBlock>& blocks){auto p=std::make_unique<Impl>();std::set<std::pair<std::uint16_t,std::uint32_t>> ids;for(const auto& block:blocks){if(!ids.emplace(block.world_id,block.shape_id).second)throw std::runtime_error("duplicate collision shape ID");p->add(block);}p->coverage.source_shapes=blocks.size();return WorldCollision(std::move(p));}
WorldCollision WorldCollision::from_world(const WorldState& state){
    auto p=std::make_unique<Impl>();p->coverage.source_shapes=state.shapes().size();p->coverage.unsupported_joints=state.joints().size();
    std::map<std::uint32_t,std::optional<RigidBodyPose>> poses;
    for(const auto& [id,body]:state.bodies()){
        try{poses.emplace(id,decode_rigid_body_pose(body.data));}catch(const std::runtime_error&){poses.emplace(id,std::nullopt);}
    }
    for(const auto& [id,stored]:state.shapes()){
        std::optional<ShapeGeometry> shape;
        try{shape=decode_child_geometry(stored.data);}catch(const std::runtime_error&){++p->coverage.unsupported_shapes;continue;}
        if(!shape){++p->coverage.unsupported_shapes;continue;}
        if(shape->kind!=SavedShapeKind::block){++p->coverage.unsupported_parts;continue;}
        const auto pose=poses.find(stored.body_id);
        if(pose==poses.end()||!pose->second){++p->coverage.unsupported_bodies;continue;}
        const auto& dimensions=*shape->grid_dimensions;
        if(dimensions[0]<=0||dimensions[1]<=0||dimensions[2]<=0){++p->coverage.degenerate_blocks;continue;}
        const auto& body=*pose->second;
        CollisionBlock block;block.shape_id=id;block.body_id=stored.body_id;block.world_id=body.world_id;block.grid_position=shape->grid_position;block.grid_dimensions=dimensions;
        block.body_position={body.position[0],body.position[1],body.position[2]};
        for(std::size_t i=0;i<4;++i)block.body_rotation_xyzw[i]=body.rotation_xyzw[i];
        p->add(block);
    }
    return WorldCollision(std::move(p));
}
const CollisionCoverage& WorldCollision::coverage()const{return impl_->coverage;}
std::optional<CollisionRayHit> WorldCollision::raycast(std::uint16_t world_id,Vec3 start,Vec3 end)const{
    if(!finite(start)||!finite(end))throw std::runtime_error("ray endpoints must be finite Bullet scalars");
    if(start.x==end.x&&start.y==end.y&&start.z==end.z)throw std::runtime_error("ray must have nonzero length");
    const auto scene=impl_->scenes.find(world_id);if(scene==impl_->scenes.end())return std::nullopt;
    btCollisionWorld::ClosestRayResultCallback callback(bullet(start),bullet(end));scene->second->world.rayTest(bullet(start),bullet(end),callback);
    if(!callback.hasHit())return std::nullopt;
    const auto* id=static_cast<const Identity*>(callback.m_collisionObject->getUserPointer());
    return CollisionRayHit{id->shape_id,id->body_id,id->world_id,native(callback.m_hitPointWorld),native(callback.m_hitNormalWorld),callback.m_closestHitFraction};
}
std::optional<CollisionRayHit> WorldCollision::sweep_capsule(std::uint16_t world_id,Vec3 start,Vec3 end,double radius,double cylinder_height)const{
    if(!finite(start)||!finite(end))throw std::runtime_error("capsule endpoints must be finite Bullet scalars");
    if(!std::isfinite(radius)||!std::isfinite(cylinder_height)||radius<=0||cylinder_height<0||radius>std::numeric_limits<btScalar>::max()||cylinder_height>std::numeric_limits<btScalar>::max())
        throw std::runtime_error("invalid capsule dimensions");
    if(start.x==end.x&&start.y==end.y&&start.z==end.z)throw std::runtime_error("capsule sweep must have nonzero length");
    const auto scene=impl_->scenes.find(world_id);if(scene==impl_->scenes.end())return std::nullopt;
    const auto r=btScalar(radius),h=btScalar(cylinder_height);
    if(r<=0||!std::isfinite(double(r))||!std::isfinite(double(h)))throw std::runtime_error("invalid Bullet capsule dimensions");
    btCapsuleShapeZ capsule(r,h);btTransform from,to;from.setIdentity();to.setIdentity();from.setOrigin(bullet(start));to.setOrigin(bullet(end));
    btCollisionWorld::ClosestConvexResultCallback callback(from.getOrigin(),to.getOrigin());
    scene->second->world.convexSweepTest(&capsule,from,to,callback);
    if(!callback.hasHit())return std::nullopt;
    const auto* identity=static_cast<const Identity*>(callback.m_hitCollisionObject->getUserPointer());
    return CollisionRayHit{identity->shape_id,identity->body_id,identity->world_id,native(callback.m_hitPointWorld),native(callback.m_hitNormalWorld),callback.m_closestHitFraction};
}
CapsuleMotion WorldCollision::move_capsule(std::uint16_t world_id,Vec3 start,Vec3 displacement,double radius,double cylinder_height,double skin,unsigned max_iterations)const{
    if(!finite(start)||!finite(displacement)||!std::isfinite(skin)||skin<0||max_iterations==0||max_iterations>16)
        throw std::runtime_error("invalid capsule motion");
    if(!std::isfinite(radius)||!std::isfinite(cylinder_height)||radius<=0||cylinder_height<0||radius>std::numeric_limits<btScalar>::max()||cylinder_height>std::numeric_limits<btScalar>::max()||btScalar(radius)<=0)
        throw std::runtime_error("invalid capsule dimensions");
    CapsuleMotion result{start,{}};auto remaining=displacement;
    for(unsigned i=0;i<max_iterations;++i) {
        const auto length=std::sqrt(remaining.x*remaining.x+remaining.y*remaining.y+remaining.z*remaining.z);
        if(!std::isfinite(length))throw std::runtime_error("capsule motion too large");
        if(length<1e-7)break;
        const Vec3 end{result.position.x+remaining.x,result.position.y+remaining.y,result.position.z+remaining.z};
        const auto hit=sweep_capsule(world_id,result.position,end,radius,cylinder_height);
        if(!hit){result.position=end;break;}
        result.contacts.push_back(*hit);
        const auto fraction=std::clamp(hit->fraction,0.0,1.0);
        const auto advance=std::max(0.0,fraction-skin/length);
        result.position={result.position.x+remaining.x*advance,result.position.y+remaining.y*advance,result.position.z+remaining.z*advance};
        remaining={remaining.x*(1-advance),remaining.y*(1-advance),remaining.z*(1-advance)};
        const auto into=remaining.x*hit->normal.x+remaining.y*hit->normal.y+remaining.z*hit->normal.z;
        if(into>=-1e-9)break;
        remaining={remaining.x-into*hit->normal.x,remaining.y-into*hit->normal.y,remaining.z-into*hit->normal.z};
    }
    return result;
}
std::optional<CollisionBounds> WorldCollision::body_bounds(std::uint16_t world_id,std::uint32_t body_id)const{const auto scene=impl_->scenes.find(world_id);if(scene==impl_->scenes.end())return std::nullopt;const auto at=scene->second->body_bounds.find(body_id);if(at==scene->second->body_bounds.end())return std::nullopt;return at->second;}
std::string WorldCollision::summary_json()const{const auto& c=coverage();return "{\"mode\":\"decoded-block-collision-only\",\"game_physics_parity\":false,\"source_shapes\":"+std::to_string(c.source_shapes)+",\"block_boxes\":"+std::to_string(c.block_boxes)+",\"unsupported_parts\":"+std::to_string(c.unsupported_parts)+",\"unsupported_shapes\":"+std::to_string(c.unsupported_shapes)+",\"degenerate_blocks\":"+std::to_string(c.degenerate_blocks)+",\"unsupported_body_shapes\":"+std::to_string(c.unsupported_bodies)+",\"unsupported_joints\":"+std::to_string(c.unsupported_joints)+",\"worlds\":"+std::to_string(c.worlds)+"}";}
}
