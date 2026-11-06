#include "world_collision.hpp"
#include "data/state.hpp"
#include "data/shape_geometry.hpp"
#include "data/assets.hpp"
#include "data/terrain.hpp"
#include <btBulletCollisionCommon.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace scrapds {
namespace {
constexpr std::size_t max_unbounded_collision_shapes=100000;
btVector3 bullet(Vec3 v) { return {btScalar(v.x), btScalar(v.y), btScalar(v.z)}; }
Vec3 native(const btVector3& v) { return {v.x(), v.y(), v.z()}; }
bool finite(Vec3 v) {
    const auto good=[](double x){return std::isfinite(x) && std::abs(x)<=std::numeric_limits<btScalar>::max();};
    return good(v.x)&&good(v.y)&&good(v.z);
}
struct Identity { std::uint32_t shape_id, body_id; std::uint16_t world_id; };
struct Scene {
    struct Terrain {
        std::unique_ptr<btTriangleMesh> mesh;
        std::unique_ptr<btBvhTriangleMeshShape> shape;
        std::unique_ptr<btCollisionObject> object;
        std::size_t triangles{};
    };
    btDefaultCollisionConfiguration config;
    btCollisionDispatcher dispatcher{&config};
    btDbvtBroadphase broadphase;
    btCollisionWorld world{&dispatcher, &broadphase, &config};
    std::deque<Identity> identities;
    std::vector<std::unique_ptr<btCollisionShape>> shapes;
    std::vector<std::unique_ptr<btCollisionObject>> objects;
    std::map<std::uint32_t, CollisionBounds> body_bounds;
    std::map<std::tuple<bool,std::int32_t,std::int32_t>,std::unique_ptr<Terrain>> terrain;
    ~Scene() { for(auto& [key,item]:terrain){(void)key;world.removeCollisionObject(item->object.get());}for (auto& object : objects) world.removeCollisionObject(object.get()); }
    void add_object(std::uint32_t shape_id,std::uint32_t body_id,std::uint16_t world_id,
                    std::unique_ptr<btCollisionShape> shape,const btTransform& transform) {
        auto object=std::make_unique<btCollisionObject>();
        object->setCollisionShape(shape.get());object->setWorldTransform(transform);
        identities.push_back({shape_id,body_id,world_id});object->setUserPointer(&identities.back());
        btVector3 lo,hi;shape->getAabb(transform,lo,hi);
        auto [at,inserted]=body_bounds.emplace(body_id,CollisionBounds{native(lo),native(hi)});
        if(!inserted){auto& b=at->second;b.minimum={std::min(b.minimum.x,double(lo.x())),std::min(b.minimum.y,double(lo.y())),std::min(b.minimum.z,double(lo.z()))};b.maximum={std::max(b.maximum.x,double(hi.x())),std::max(b.maximum.y,double(hi.y())),std::max(b.maximum.z,double(hi.z()))};}
        shapes.push_back(std::move(shape));objects.push_back(std::move(object));world.addCollisionObject(objects.back().get());
    }
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
        add_object(block.shape_id,block.body_id,block.world_id,std::move(shape),parent*local);
    }
    void add_part(std::uint32_t shape_id,std::uint32_t body_id,std::uint16_t world_id,
                  const ShapeGeometry& geometry,const RigidBodyPose& body,const AssetDefinition& asset) {
        std::array<double,3> dimensions{};std::optional<CylinderDimensions> cylinder;const HullGeometry* hull=nullptr;
        if(asset.box_dimensions)dimensions=*asset.box_dimensions;
        else if(asset.cylinder_dimensions){cylinder=asset.cylinder_dimensions;dimensions.fill(cylinder->diameter);dimensions[cylinder->axis]=cylinder->depth;}
        else if(asset.hull_geometry){hull=&*asset.hull_geometry;dimensions=hull->dimensions;}
        else throw std::runtime_error("unsupported-part-collision-asset");
        for(const auto n:dimensions)if(!std::isfinite(n)||n<=0||n>32767)throw std::runtime_error("invalid-part-collision-dimensions");
        const btVector3 half{btScalar(dimensions[0]*0.125),btScalar(dimensions[1]*0.125),btScalar(dimensions[2]*0.125)};
        std::unique_ptr<btCollisionShape> child;
        if(hull) {
            auto shape=std::make_unique<btConvexHullShape>();
            for(const auto& vertex:hull->vertices)shape->addPoint({btScalar(vertex[0]*0.25),btScalar(vertex[1]*0.25),btScalar(vertex[2]*0.25)},false);
            shape->setMargin(btScalar(hull->margin));shape->recalcLocalAabb();child=std::move(shape);
        } else if(!cylinder) {
            auto shape=std::make_unique<btBoxShape>(half);shape->setMargin(0);child=std::move(shape);
        } else if(cylinder->axis==0) {
            auto shape=std::make_unique<btCylinderShapeX>(half);shape->setMargin(0);child=std::move(shape);
        } else if(cylinder->axis==1) {
            auto shape=std::make_unique<btCylinderShape>(half);shape->setMargin(0);child=std::move(shape);
        } else {
            auto shape=std::make_unique<btCylinderShapeZ>(half);shape->setMargin(0);child=std::move(shape);
        }
        const auto& xa=geometry.x_axis;const auto& za=geometry.z_axis;const auto y=std::array<double,3>{
            double(za[1])*xa[2]-double(za[2])*xa[1],double(za[2])*xa[0]-double(za[0])*xa[2],double(za[0])*xa[1]-double(za[1])*xa[0]};
        btMatrix3x3 basis{btScalar(xa[0]),btScalar(y[0]),btScalar(za[0]),btScalar(xa[1]),btScalar(y[1]),btScalar(za[1]),btScalar(xa[2]),btScalar(y[2]),btScalar(za[2])};
        btTransform local;local.setIdentity();local.setBasis(basis);
        local.setOrigin(btVector3(btScalar(geometry.grid_position[0]*0.25),btScalar(geometry.grid_position[1]*0.25),btScalar(geometry.grid_position[2]*0.25))+basis*half);
        btQuaternion rotation(btScalar(body.rotation_xyzw[0]),btScalar(body.rotation_xyzw[1]),btScalar(body.rotation_xyzw[2]),btScalar(body.rotation_xyzw[3]));
        rotation.normalize();const btTransform parent(rotation,{body.position[0],body.position[1],body.position[2]});
        add_object(shape_id,body_id,world_id,std::move(child),parent*local);
    }
    void add_terrain(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y,std::span<const TerrainVertex> vertices,bool overworld_cell=true) {
        if(vertices.empty()||vertices.size()%3||vertices.size()>3000000)throw std::runtime_error("invalid-overworld-collision-mesh");
        const auto key=std::tuple{overworld_cell,cell_x,cell_y};if(terrain.contains(key))throw std::runtime_error("duplicate-overworld-collision-cell");
        auto value=std::make_unique<Terrain>();value->triangles=vertices.size()/3;value->mesh=std::make_unique<btTriangleMesh>();
        for(std::size_t i=0;i<vertices.size();i+=3) {
            const auto& a=vertices[i];const auto& b=vertices[i+1];const auto& c=vertices[i+2];
            for(const auto& v:{a,b,c})for(const auto n:{v.x,v.y,v.z})if(!std::isfinite(n)||std::abs(double(n))>std::numeric_limits<btScalar>::max())throw std::runtime_error("invalid-overworld-collision-vertex");
            value->mesh->addTriangle({a.x,a.y,a.z},{b.x,b.y,b.z},{c.x,c.y,c.z},true);
        }
        value->shape=std::make_unique<btBvhTriangleMeshShape>(value->mesh.get(),true,true);
        value->object=std::make_unique<btCollisionObject>();value->object->setCollisionShape(value->shape.get());
        value->object->setWorldTransform(btTransform::getIdentity());
        identities.push_back({UINT32_MAX,UINT32_MAX,world_id});
        value->object->setUserPointer(&identities.back());
        world.addCollisionObject(value->object.get());terrain.emplace(key,std::move(value));
    }
    void remove_terrain(std::int32_t cell_x,std::int32_t cell_y) {
        const auto it=terrain.find({true,cell_x,cell_y});if(it==terrain.end())return;
        world.removeCollisionObject(it->second->object.get());terrain.erase(it);
    }
};
}
struct WorldCollision::Impl {
    CollisionCoverage coverage;
    std::map<std::uint16_t,std::unique_ptr<Scene>> scenes;
    void add(const CollisionBlock& block){auto& scene=scenes[block.world_id];if(!scene)scene=std::make_unique<Scene>();scene->add(block);++coverage.block_boxes;coverage.worlds=scenes.size();}
    void add_part(std::uint32_t shape_id,std::uint32_t body_id,std::uint16_t world_id,const ShapeGeometry& geometry,const RigidBodyPose& body,const AssetDefinition& asset) {
        auto& scene=scenes[world_id];if(!scene)scene=std::make_unique<Scene>();scene->add_part(shape_id,body_id,world_id,geometry,body,asset);
        if(asset.box_dimensions)++coverage.part_boxes;else if(asset.cylinder_dimensions)++coverage.part_cylinders;else if(asset.hull_geometry)++coverage.part_hulls;
        coverage.worlds=scenes.size();
    }
    void add_flat(std::uint16_t world_id,std::span<const TerrainVertex> vertices){auto& scene=scenes[world_id];if(!scene)scene=std::make_unique<Scene>();scene->add_terrain(world_id,0,0,vertices,false);coverage.terrain_triangles+=vertices.size()/3;coverage.worlds=scenes.size();}
};
WorldCollision::WorldCollision(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
WorldCollision::WorldCollision(WorldCollision&&) noexcept=default;
WorldCollision& WorldCollision::operator=(WorldCollision&&) noexcept=default;
WorldCollision::~WorldCollision()=default;
WorldCollision WorldCollision::from_blocks(const std::vector<CollisionBlock>& blocks){auto p=std::make_unique<Impl>();std::set<std::pair<std::uint16_t,std::uint32_t>> ids;for(const auto& block:blocks){if(!ids.emplace(block.world_id,block.shape_id).second)throw std::runtime_error("duplicate collision shape ID");p->add(block);}p->coverage.source_shapes=blocks.size();return WorldCollision(std::move(p));}
WorldCollision WorldCollision::from_world(const WorldState& state,const AssetDefinitions* assets,
    std::span<const CollisionAnchor> anchors,double region_radius){
    const bool regional=!anchors.empty();
    if(!regional&&state.shapes().size()>max_unbounded_collision_shapes)throw std::runtime_error("large-world-collision-requires-regional-anchors");
    if(regional&&(!std::isfinite(region_radius)||region_radius<=0||region_radius>10000000))throw std::invalid_argument("invalid-saved-collision-region-radius");
    for(const auto& anchor:anchors)if(!std::isfinite(anchor.x)||!std::isfinite(anchor.y)||std::abs(anchor.x)>10000000||std::abs(anchor.y)>10000000)
        throw std::invalid_argument("invalid-saved-collision-anchor");
    auto p=std::make_unique<Impl>();p->coverage.source_shapes=state.shapes().size();p->coverage.unsupported_joints=state.joints().size();
    std::map<std::uint32_t,std::optional<RigidBodyPose>> poses;
    for(const auto& [id,body]:state.bodies()){
        std::optional<RigidBodyPose> pose;
        try{pose=decode_rigid_body_pose(body.data);}catch(const std::runtime_error&){}
        bool selected=!regional;
        if(regional)for(const auto& anchor:anchors)if(anchor.world_id==body.world_id) {
            if(body.prefix) {
                const auto& bounds=body.prefix->bounds_min_x_max_x_min_y_max_y;
                if(bounds[1]>=anchor.x-region_radius&&bounds[0]<=anchor.x+region_radius&&
                   bounds[3]>=anchor.y-region_radius&&bounds[2]<=anchor.y+region_radius){selected=true;break;}
            } else if(pose) {
                if(std::abs(double(pose->position[0])-anchor.x)<=region_radius&&
                   std::abs(double(pose->position[1])-anchor.y)<=region_radius){selected=true;break;}
            } else {selected=true;break;}
        }
        if(!selected){++p->coverage.culled_bodies;continue;}
        poses.emplace(id,std::move(pose));
    }
    for(const auto& [id,stored]:state.shapes()){
        if(!poses.contains(stored.body_id)){++p->coverage.culled_shapes;continue;}
        std::optional<ShapeGeometry> shape;
        try{shape=decode_child_geometry(stored.data);}catch(const std::runtime_error&){++p->coverage.unsupported_shapes;continue;}
        if(!shape){++p->coverage.unsupported_shapes;continue;}
        const auto pose=poses.find(stored.body_id);
        if(pose==poses.end()||!pose->second){++p->coverage.unsupported_bodies;continue;}
        if(shape->kind==SavedShapeKind::part&&assets) {
            const auto* asset=assets->find(shape->uuid);
            if(!asset){++p->coverage.unsupported_parts;continue;}
            try{p->add_part(id,stored.body_id,pose->second->world_id,*shape,*pose->second,*asset);}
            catch(const std::runtime_error&){++p->coverage.unsupported_parts;}
            continue;
        }
        if(shape->kind!=SavedShapeKind::block){++p->coverage.unsupported_parts;continue;}
        const auto& dimensions=*shape->grid_dimensions;
        if(dimensions[0]<=0||dimensions[1]<=0||dimensions[2]<=0){++p->coverage.degenerate_blocks;continue;}
        const auto& body=*pose->second;
        CollisionBlock block;block.shape_id=id;block.body_id=stored.body_id;block.world_id=body.world_id;block.grid_position=shape->grid_position;block.grid_dimensions=dimensions;
        block.body_position={body.position[0],body.position[1],body.position[2]};
        for(std::size_t i=0;i<4;++i)block.body_rotation_xyzw[i]=body.rotation_xyzw[i];
        p->add(block);
    }
    const std::array<TerrainVertex,6> flat{{{-1024,-1024,0},{1024,-1024,0},{1024,1024,0},{-1024,-1024,0},{1024,1024,0},{-1024,1024,0}}};
    for(const auto world_id:state.flat_surface_worlds())p->add_flat(world_id,flat);
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
void WorldCollision::add_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y,std::span<const TerrainVertex> vertices) {
    auto& scene=impl_->scenes[world_id];if(!scene)scene=std::make_unique<Scene>();scene->add_terrain(world_id,cell_x,cell_y,vertices,true);
    impl_->coverage.terrain_triangles+=vertices.size()/3;impl_->coverage.worlds=impl_->scenes.size();
}
void WorldCollision::remove_overworld_cell_mesh(std::uint16_t world_id,std::int32_t cell_x,std::int32_t cell_y) {
    const auto scene=impl_->scenes.find(world_id);if(scene==impl_->scenes.end())return;const auto it=scene->second->terrain.find({true,cell_x,cell_y});if(it==scene->second->terrain.end())return;
    impl_->coverage.terrain_triangles-=it->second->triangles;scene->second->remove_terrain(cell_x,cell_y);
}
std::string WorldCollision::summary_json()const{const auto& c=coverage();return "{\"mode\":\"decoded-block-part-and-resident-terrain-collision\",\"game_physics_parity\":false,\"source_shapes\":"+std::to_string(c.source_shapes)+",\"block_boxes\":"+std::to_string(c.block_boxes)+",\"part_boxes\":"+std::to_string(c.part_boxes)+",\"part_cylinders\":"+std::to_string(c.part_cylinders)+",\"part_hulls\":"+std::to_string(c.part_hulls)+",\"terrain_triangles\":"+std::to_string(c.terrain_triangles)+",\"unsupported_parts\":"+std::to_string(c.unsupported_parts)+",\"unsupported_shapes\":"+std::to_string(c.unsupported_shapes)+",\"degenerate_blocks\":"+std::to_string(c.degenerate_blocks)+",\"unsupported_body_shapes\":"+std::to_string(c.unsupported_bodies)+",\"unsupported_joints\":"+std::to_string(c.unsupported_joints)+",\"culled_shapes\":"+std::to_string(c.culled_shapes)+",\"culled_bodies\":"+std::to_string(c.culled_bodies)+",\"worlds\":"+std::to_string(c.worlds)+"}";}
}
