#include "physics/world_collision.hpp"
#include "data/state.hpp"
#include "data/shape_geometry.hpp"
#include "data/terrain.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
bool close(double a,double b,double tolerance=0.0001){return std::abs(a-b)<=tolerance;}
}
int main(int argc,char** argv){
    try{
        scrapds::CollisionBlock block;block.shape_id=7;block.body_id=3;block.world_id=4;block.grid_position={4,0,0};block.grid_dimensions={4,8,4};block.body_position={10,20,30};
        const auto half=std::sqrt(0.5);block.body_rotation_xyzw={0,0,half,half};
        auto second=block;second.shape_id=8;second.world_id=5;second.body_position={10,20,40};
        auto scene=scrapds::WorldCollision::from_blocks({block,second});
        require(scene.coverage().block_boxes==2&&scene.coverage().worlds==2,"collision coverage");
        const auto hit=scene.raycast(4,{9,21.5,35},{9,21.5,25});
        require(hit&&hit->shape_id==7&&hit->body_id==3&&hit->world_id==4,"transformed block ray hit");
        require(close(hit->point.z,31)&&close(hit->normal.z,1),"rotated block hit surface");
        require(!scene.raycast(5,{9,21.5,35},{9,21.5,25}),"cross-world ray contamination");
        require(!scene.raycast(99,{9,21.5,35},{9,21.5,25}),"unknown world ray contamination");
        const auto capsule_hit=scene.sweep_capsule(4,{9,21.5,35},{9,21.5,25},0.5,1.0);
        require(capsule_hit&&capsule_hit->shape_id==7&&capsule_hit->body_id==3&&close(capsule_hit->fraction,0.3,0.005)&&capsule_hit->normal.z>0.99,"capsule sweep failed on rotated block");
        require(!scene.sweep_capsule(5,{9,21.5,35},{9,21.5,25},0.5,1.0),"capsule sweep crossed worlds");
        const auto blocked=scene.move_capsule(4,{9,19,30.5},{0,4,0},0.5,1.0);
        require(!blocked.contacts.empty()&&close(blocked.position.y,20.499,0.01)&&close(blocked.position.x,9),"capsule motion crossed a block wall");
        const auto sliding=scene.move_capsule(4,{9.5,19,30.5},{-1,4,0},0.5,1.0);
        require(!sliding.contacts.empty()&&close(sliding.position.y,20.499,0.02)&&close(sliding.position.x,8.5,0.02),"capsule did not slide along the block wall");
        const std::vector<scrapds::TerrainVertex> ground{{-5,-5,0},{5,-5,0},{5,5,0},{-5,-5,0},{5,5,0},{-5,5,0}};
        scene.add_overworld_cell_mesh(4,0,0,ground);
        require(scene.coverage().terrain_triangles==2,"resident terrain coverage mismatch");
        const auto terrain_player=scene.move_capsule(4,{0,0,3},{0,0,-4},0.3,0.8);
        require(!terrain_player.contacts.empty()&&close(terrain_player.position.z,0.701,0.02),"capsule passed through resident overworld terrain");
        scene.remove_overworld_cell_mesh(4,0,0);
        require(scene.coverage().terrain_triangles==0&&scene.move_capsule(4,{0,0,3},{0,0,-4},0.3,0.8).contacts.empty(),"removed terrain remained in collision scene");
        const auto clear=scene.move_capsule(5,{9,19,30.5},{0,4,0},0.5,1.0);
        require(clear.contacts.empty()&&close(clear.position.y,23),"capsule motion changed in an empty world");
        const auto bound=scene.body_bounds(4,3);
        require(bound&&close(bound->minimum.x,8)&&close(bound->maximum.x,10)&&close(bound->minimum.y,21)&&close(bound->maximum.y,22),"rotated block bounds");
        bool rejected=false;try{scene.raycast(4,{0,0,0},{0,0,0});}catch(const std::runtime_error&){rejected=true;}require(rejected,"zero-length ray accepted");
        rejected=false;try{scene.raycast(4,{0,0,0},{1e300,0,0});}catch(const std::runtime_error&){rejected=true;}require(rejected,"out-of-range ray accepted");
        rejected=false;try{scene.sweep_capsule(4,{0,0,0},{0,0,1},0,1);}catch(const std::runtime_error&){rejected=true;}require(rejected,"zero-radius capsule accepted");
        auto degenerate=block;degenerate.grid_dimensions[0]=0;rejected=false;try{(void)scrapds::WorldCollision::from_blocks({degenerate});}catch(const std::runtime_error&){rejected=true;}require(rejected,"degenerate collision box accepted");
        for(int i=1;i<argc;++i){
            const auto state=scrapds::WorldState::load(argv[i]);auto real=scrapds::WorldCollision::from_world(state);std::cout<<real.summary_json()<<'\n';
            // All-block Creative save provides independent stored RTree bounds.
            // Only static bodies are compared here: dynamic RTree records may
            // contain a wider creation/joint aggregate, not a per-body AABB.
            std::size_t checked=0,mismatched=0,joint_containment=0;
            for(const auto&[id,body]:state.bodies()){
                const auto pose=scrapds::decode_rigid_body_pose(body.data);if(!pose||!pose->is_static)continue;
                bool all_blocks=true;std::size_t shape_count=0;
                for(const auto&[sid,shape]:state.shapes())if(shape.body_id==id){++shape_count;const auto decoded=scrapds::decode_child_geometry(shape.data);if(!decoded||decoded->kind!=scrapds::SavedShapeKind::block)all_blocks=false;}
                if(!all_blocks||!shape_count||!body.prefix)continue;
                const auto actual=real.body_bounds(body.world_id,id);if(!actual)continue;
                const auto& expected=body.prefix->bounds_min_x_max_x_min_y_max_y;
                bool has_joints=false;
                for(const auto&[joint_id,joint]:state.joints()) {
                    (void)joint_id;
                    for(const auto child:{joint.shape_a,joint.shape_b}) {
                        const auto shape=state.shapes().find(child);
                        if(shape!=state.shapes().end()&&shape->second.body_id==id)has_joints=true;
                    }
                }
                if(has_joints) {
                    // Joint proxies can enlarge a body's saved bounds. Their
                    // collision geometry is explicitly outside this importer.
                    require(actual->minimum.x>=expected[0]-0.011&&actual->maximum.x<=expected[1]+0.011&&actual->minimum.y>=expected[2]-0.011&&actual->maximum.y<=expected[3]+0.011,"block geometry escapes joint aggregate bounds");
                    ++joint_containment;continue;
                }
                const double delta=std::max({std::abs(actual->minimum.x-expected[0]),std::abs(actual->maximum.x-expected[1]),std::abs(actual->minimum.y-expected[2]),std::abs(actual->maximum.y-expected[3])});
                ++checked;if(delta>0.011){++mismatched;std::cout<<"bounds mismatch body "<<id<<" delta "<<delta<<'\n';}
            }
            require(checked>0,"no static all-block bodies for bounds verification");
            require(mismatched==0,"static all-block bounds differ from saved RTree");
            std::cout<<checked<<" static body bounds match SQLite within 0.011 world units; "<<joint_containment<<" joint aggregates contain block geometry\n";
        }
        std::cout<<"world_collision_tests: passed\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
