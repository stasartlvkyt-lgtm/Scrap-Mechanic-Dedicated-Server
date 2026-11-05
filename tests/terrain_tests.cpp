#include "data/terrain.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
int main(int argc,char** argv){try {
    bool lua_rejected=false;try{(void)scrapds::decode_overworld_cells(std::vector<std::uint8_t>{'L','U','A',0,0,0,1,5});}catch(const std::runtime_error&){lua_rejected=true;}
    if(!lua_rejected)throw std::runtime_error("truncated terrain LUA table accepted");
    // Independent raw LZ4 record from a saved VoxelTerrain row.
    const std::vector<std::uint8_t> row{0x82,0x0c,0x00,0x01,0x00,0x00,0x00,0x05,0x00,0x01,0x00,0x10,0x01,0x05,0x00,0x43,0xff,0xff,0xff,0xff,0x0c,0x00,0x50,0x04,0x05,0x00,0x01,0x00};
    const auto d=scrapds::decode_terrain_delta(row);
    if(d.id!=5||d.world_id!=0||d.region_x!=1||d.region_y!=0||d.chunk_position!=std::array<std::int32_t,3>{4,1,-1}||d.kind!=5||d.payload!=std::vector<std::uint8_t>{0})throw std::runtime_error("saved terrain envelope decoded incorrectly");
    if(!scrapds::decode_terrain_density_changes(d).empty())throw std::runtime_error("empty delta changed voxels");
    const std::vector<std::uint8_t> edited{0x8e,0x0c,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x01,0x00,0x10,0x01,0x04,0x00,0x42,0x05,0x00,0x4d,0x01,0x0f,0x00,0x15,0x11,0x19,0x00,0x8f,0x34,0x08,0x82,0x00,0x0c,0x82,0x7c,0x00,0x01,0x00,0x05,0x1b,0x20,0x10,0x00,0xc0,0x08,0x44,0x33,0x04,0x26,0x6b,0x98,0x77,0x8c,0x2c,0x20,0x80};
    const auto changes=scrapds::decode_terrain_density_changes(scrapds::decode_terrain_delta(edited));
    if(changes.size()!=18||changes[0].position!=std::array<std::uint8_t,3>{1,3,2}||changes[0].density!=1||changes[3].position!=std::array<std::uint8_t,3>{1,5,0}||changes[3].density!=3)throw std::runtime_error("saved terrain voxel changes decoded incorrectly");
    auto with_material=d;with_material.kind=4;with_material.payload.assign(66,0);with_material.payload[0]=1;with_material.payload[1]=0x80;with_material.payload[65]=0x3d;
    const auto material_changes=scrapds::decode_terrain_density_changes(with_material);
    if(material_changes.size()!=1||material_changes[0].position!=std::array<std::uint8_t,3>{0,0,0}||material_changes[0].density!=7||material_changes[0].material!=5)throw std::runtime_error("terrain density and material bits decoded incorrectly");
    auto sparse=d;sparse.kind=8;sparse.payload={1,1,0xe8,0xb0};
    const auto sparse_changes=scrapds::decode_terrain_density_changes(sparse);
    if(sparse_changes.size()!=1||sparse_changes[0].position!=std::array<std::uint8_t,3>{9,8,8}||sparse_changes[0].density!=17||sparse_changes[0].material!=3)throw std::runtime_error("sparse terrain delta decoded incorrectly");
    sparse.kind=9;sparse.payload={1,1,0xe8,0x80};const auto sparse_density=scrapds::decode_terrain_density_changes(sparse);
    if(sparse_density.size()!=1||sparse_density[0].density!=17||sparse_density[0].material)throw std::runtime_error("sparse density-only delta decoded incorrectly");
    auto mask_density=d;mask_density.kind=7;mask_density.payload=with_material.payload;mask_density.payload.back()&=0xf8;
    if(scrapds::decode_terrain_density_changes(mask_density).at(0).density!=7)throw std::runtime_error("network bitmap density decoded incorrectly");
    std::vector<std::uint8_t> base(4913,0xa0);scrapds::apply_terrain_density_changes(base,scrapds::decode_terrain_delta(edited));
    if(base[17*(3+17*1)+2]!=0xa1||base[17*(5+17*1)]!=0xa3||base[0]!=0xa0)throw std::runtime_error("terrain delta did not preserve base material and unchanged cells");
    base[0]=0x40;scrapds::apply_terrain_density_changes(base,with_material);if(base[0]!=0xa7)throw std::runtime_error("terrain material update was not applied");
    auto truncated=d;truncated.payload={1};bool truncated_rejected=false;try{(void)scrapds::decode_terrain_density_changes(truncated);}catch(const std::runtime_error&){truncated_rejected=true;}
    if(!truncated_rejected)throw std::runtime_error("truncated terrain mask accepted");
    const auto original=base;truncated_rejected=false;try{scrapds::apply_terrain_density_changes(base,truncated);}catch(const std::runtime_error&){truncated_rejected=true;}
    if(!truncated_rejected||base!=original)throw std::runtime_error("invalid terrain delta partially changed base chunk");
    auto corner=with_material;corner.chunk_position={5,2,0};
    std::vector<std::uint8_t> shared(4913,0x40);
    scrapds::apply_terrain_neighbour_changes(shared,d.world_id,{4,1,-1},std::vector<scrapds::TerrainDelta>{corner});
    if(shared[17*(16+17*16)+16]!=0xa7||shared[0]!=0x40)throw std::runtime_error("adjacent terrain corner edit missing");
    const auto before_shared=shared;truncated.chunk_position={4,1,-1};truncated_rejected=false;
    try{scrapds::apply_terrain_neighbour_changes(shared,d.world_id,{4,1,-1},std::vector<scrapds::TerrainDelta>{corner,truncated});}catch(const std::runtime_error&){truncated_rejected=true;}
    if(!truncated_rejected||shared!=before_shared)throw std::runtime_error("invalid neighbour edit partially changed terrain");
    const auto tile_path=std::filesystem::temp_directory_path()/("scrapds-tile-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tile");
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}} cleanup{tile_path};
    std::vector<std::uint8_t> tile(60+388,0),voxels(4108,0);const auto put=[](auto& b,std::size_t offset,std::uint32_t v){for(int i=0;i<4;++i)b[offset+i]=std::uint8_t(v>>(8*i));};
    tile[0]='T';tile[1]='I';tile[2]='L';tile[3]='E';put(tile,4,15);put(tile,32,1);put(tile,36,1);put(tile,40,60);put(tile,44,388);
    put(voxels,0,2);put(voxels,4,3);put(voxels,8,4);voxels[12+42]=0x91;
    std::vector<std::uint8_t> compressed{0xf0};std::size_t extra=voxels.size()-15;while(extra>=255){compressed.push_back(255);extra-=255;}compressed.push_back(std::uint8_t(extra));compressed.insert(compressed.end(),voxels.begin(),voxels.end());
    put(tile,60+372,1);put(tile,60+376,std::uint32_t(tile.size()));put(tile,60+380,std::uint32_t(compressed.size()));put(tile,60+384,std::uint32_t(voxels.size()));tile.insert(tile.end(),compressed.begin(),compressed.end());
    std::vector<std::uint8_t> heights(8*(33*33+65*65),0);for(std::size_t i=0;i<33*33;++i)put(heights,i*8,0x41200000);
    std::vector<std::uint8_t> packed_heights{0xf0};extra=heights.size()-15;while(extra>=255){packed_heights.push_back(255);extra-=255;}packed_heights.push_back(std::uint8_t(extra));packed_heights.insert(packed_heights.end(),heights.begin(),heights.end());
    put(tile,60,std::uint32_t(tile.size()));put(tile,60+24,std::uint32_t(packed_heights.size()));put(tile,60+48,std::uint32_t(heights.size()));tile.insert(tile.end(),packed_heights.begin(),packed_heights.end());
    std::vector<std::uint8_t> nodes{1,7,'F','L','A','T','T','E','N'};const auto node_start=nodes.size();nodes.resize(node_start+40,0);put(nodes,node_start,0x3fc00000);put(nodes,node_start+4,0x40200000);put(nodes,node_start+28,0x41000000);nodes.insert(nodes.end(),{1,0,0,0,0,0});
    std::vector<std::uint8_t> packed_nodes{0xf0,std::uint8_t(nodes.size()-15)};packed_nodes.insert(packed_nodes.end(),nodes.begin(),nodes.end());
    put(tile,60+164,1);put(tile,60+168,std::uint32_t(tile.size()));put(tile,60+172,std::uint32_t(packed_nodes.size()));put(tile,60+176,std::uint32_t(nodes.size()));tile.insert(tile.end(),packed_nodes.begin(),packed_nodes.end());
    {std::ofstream out(tile_path,std::ios::binary);out.write(reinterpret_cast<const char*>(tile.data()),std::streamsize(tile.size()));}
    const auto tile_chunks=scrapds::read_terrain_tile_cell(tile_path,0,0);
    if(tile_chunks.size()!=1||tile_chunks[0].position!=std::array<std::int32_t,3>{2,3,4}||tile_chunks[0].voxels[42]!=0x91)throw std::runtime_error("terrain tile voxel chunks decoded incorrectly");
    const auto height_samples=scrapds::read_terrain_tile_heights(tile_path,0,0);
    if(height_samples.side!=33||height_samples.spacing!=2||height_samples.samples.size()!=1089||height_samples.samples[0]!=10||height_samples.samples.back()!=10)throw std::runtime_error("terrain tile heights decoded incorrectly");
    const auto flatten_nodes=scrapds::read_terrain_tile_flatten_nodes(tile_path,0,0);
    if(flatten_nodes.size()!=1||flatten_nodes[0].x!=1.5||flatten_nodes[0].y!=2.5||flatten_nodes[0].radius!=4)throw std::runtime_error("terrain flatten nodes decoded incorrectly");
    scrapds::OverworldTerrain node_world;scrapds::OverworldCell node_cell;node_cell.x=-1;node_cell.y=1;node_cell.rotation=1;node_world.cells.push_back(node_cell);node_world.flags[{-1,1}]=1;
    const auto placed_nodes=scrapds::collect_overworld_flatten_nodes(node_world,{{node_cell.tile_uuid,tile_path}},-1,1);
    if(placed_nodes.size()!=1||placed_nodes[0].x!=-3||placed_nodes[0].y!=66||placed_nodes[0].radius!=4)throw std::runtime_error("terrain flatten node placement incorrect");
    const auto saved_surface=scrapds::mesh_saved_overworld_cell(node_world,{{node_cell.tile_uuid,tile_path}},node_cell);
    if(saved_surface.size()!=32*32*6||saved_surface[0].x!=0||saved_surface[0].y!=64||saved_surface[0].z!=-16)throw std::runtime_error("saved overworld heightfield assembly incorrect");
    scrapds::OverworldCell height_cell;height_cell.x=-2;height_cell.y=3;height_cell.rotation=1;
    const auto height_mesh=scrapds::mesh_overworld_heightfield(height_cell,{64,2,{1,2,3,4}},std::vector<float>{10,20,30,40});
    if(height_mesh.size()!=6||height_mesh[0].x!=-64||height_mesh[0].y!=192||height_mesh[0].z!=11||height_mesh[1].x!=-64||height_mesh[1].y!=256||height_mesh[1].z!=22)throw std::runtime_error("world heightfield placement incorrect");
    bool missing_offsets=false;try{(void)scrapds::mesh_overworld_heightfield(height_cell,{64,2,{1,2,3,4}},{});}catch(const std::runtime_error&){missing_offsets=true;}
    if(!missing_offsets)throw std::runtime_error("heightfield missing world offsets accepted");
    scrapds::OverworldTerrain generated;for(int y=-2;y<=3;++y)for(int x=-2;x<=3;++x){generated.elevation[{x,y}]=2*x+3*y;generated.cliff[{x,y}]=2;if(x<3&&y<3)generated.flags[{x,y}]=0;}
    const auto base_height=scrapds::unflattened_overworld_height(generated,16,48);
    if(std::abs(base_height-18.796875)>0.0001)throw std::runtime_error("overworld elevation interpolation incorrect");
    const std::vector<scrapds::TerrainFlattenArea> flattened{{8,8,16}};
    if(std::abs(scrapds::overworld_height_with_flatten_areas(generated,8,8,flattened)-16.5)>0.0001||std::abs(scrapds::overworld_height_with_flatten_areas(generated,48,48,flattened)-scrapds::unflattened_overworld_height(generated,48,48))>0.0001)throw std::runtime_error("terrain flatten area interpolation incorrect");
    if(scrapds::terrain_int_noise_2d(0,0,358)!=1720988898||scrapds::terrain_int_noise_2d(4,-6,12345)!=361046841)throw std::runtime_error("terrain noise hash mismatch");
    const auto random_area=scrapds::overworld_cell_flatten_areas(generated,1,1,{});
    if(random_area.size()!=1||random_area[0].x!=96||random_area[0].y!=96||random_area[0].radius!=16||!scrapds::overworld_cell_flatten_areas(generated,0,0,{}).empty())throw std::runtime_error("random terrain flatten area mismatch");
    const scrapds::OverworldCell origin_cell{};const auto world_surface=scrapds::mesh_generated_overworld_heightfield(generated,origin_cell,{64,2,{0,0,0,0}},[](int,int){return std::vector<scrapds::TerrainFlattenArea>{{10000,10000,1}};});
    if(world_surface.size()!=6||std::abs(world_surface[0].z-16)>0.0001||std::abs(world_surface[2].z-21)>0.0001)throw std::runtime_error("generated world height surface mismatch");
    if(scrapds::assemble_terrain_tile_chunk(tile_chunks,{2,3,4}))throw std::runtime_error("missing terrain boundaries were fabricated");
    std::vector<scrapds::TerrainTileChunk> neighbours(8);for(unsigned mask=0;mask<8;++mask){auto& n=neighbours[mask];n.position={int(2+(mask&1)),int(3+((mask>>1)&1)),int(4+((mask>>2)&1))};n.voxels.fill(std::uint8_t(mask));}neighbours[0].voxels[256*2+16*3+4]=0x79;
    auto assembled=scrapds::assemble_terrain_tile_chunk(neighbours,{2,3,4});
    if(!assembled||(*assembled)[17*(3+17*2)+4]!=0x79||(*assembled)[17*(0+17*16)]!=1||(*assembled)[17*(16+17*16)+16]!=7)throw std::runtime_error("terrain tile shared boundaries assembled incorrectly");
    if(argc>=2){
        const auto tables=scrapds::TerrainMeshTables::from_game_executable(argv[1]);std::array<std::uint8_t,4913> plane{};
        if(!scrapds::mesh_terrain_chunk(plane,{0,0,0},tables).empty())throw std::runtime_error("uniform voxels generated a terrain surface");
        for(int x=8;x<17;++x)for(int y=0;y<17;++y)for(int z=0;z<17;++z)plane[17*(y+17*x)+z]=31;
        const auto mesh=scrapds::mesh_terrain_chunk(plane,{0,0,0},tables);
        if(mesh.size()!=16*16*6)throw std::runtime_error("planar terrain has incorrect triangle count");
        for(const auto& v:mesh)if(std::abs(v.x-(7.5f+16.0f/31.0f))>0.0001f)throw std::runtime_error("terrain surface interpolation incorrect");
        std::vector<scrapds::TerrainTileChunk> source(8);
        for(unsigned mask=0;mask<8;++mask){auto& part=source[mask];part.position={int(mask&1),int((mask>>1)&1),int((mask>>2)&1)};for(int x=0;x<16;++x)for(int y=0;y<16;++y)for(int z=0;z<16;++z)part.voxels[256*x+16*y+z]=(int(mask&1)*16+x>=8)?31:0;}
        scrapds::OverworldCell world_cell;world_cell.x=-2;world_cell.y=3;world_cell.rotation=1;
        const auto placed=scrapds::mesh_overworld_tile_voxels(world_cell,source,tables);
        if(placed.size()!=mesh.size())throw std::runtime_error("world tile mesh size mismatch");
        for(std::size_t i=0;i<placed.size();++i)if(std::abs(placed[i].x-(64-mesh[i].y-128))>0.0001f||std::abs(placed[i].y-(mesh[i].x+192))>0.0001f||placed[i].z!=mesh[i].z)throw std::runtime_error("world tile mesh rotation or placement incorrect");
        if(argc>=3) {
            const auto actual=scrapds::read_terrain_tile_cell(argv[2],0,0);std::size_t complete=0,triangles=0;
            for(const auto& part:actual)if(const auto chunk=scrapds::assemble_terrain_tile_chunk(actual,part.position)){++complete;triangles+=scrapds::mesh_terrain_chunk(*chunk,part.position,tables).size()/3;}
            if(!complete||!triangles)throw std::runtime_error("real terrain tile produced no mesh");
            std::cout<<complete<<" real tile chunks, "<<triangles<<" terrain triangles\n";
        }
        if(argc>=4){const auto actual_nodes=scrapds::read_terrain_tile_flatten_nodes(argv[3],0,0);if(actual_nodes.size()!=1||std::abs(actual_nodes[0].x-32.75)>0.001||std::abs(actual_nodes[0].radius-32.636093)>0.001)throw std::runtime_error("real terrain flatten node mismatch");}
    }
    bool rejected=false;try{(void)scrapds::decode_terrain_delta(std::span(row).first(10));}catch(const std::runtime_error&){rejected=true;}
    if(!rejected)throw std::runtime_error("truncated terrain accepted");
    std::cout<<"terrain envelope checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
