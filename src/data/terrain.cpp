#include "terrain.hpp"
#include <lz4.h>
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
namespace scrapds {
namespace {
struct LuaTerrainValue {
    enum class Kind { other,number,string,uuid,table } kind{Kind::other};
    double number{};std::string string;std::array<std::uint8_t,16> uuid{};
    std::vector<std::pair<LuaTerrainValue,LuaTerrainValue>> entries;
};
class LuaTerrainReader {
    std::span<const std::uint8_t> bytes_;std::size_t bit_{56},nodes_{};
    std::uint64_t bits(unsigned count) {
        if(count>64||bit_>bytes_.size()*8||count>bytes_.size()*8-bit_)throw std::runtime_error("truncated terrain LUA data");
        std::uint64_t v=0;for(unsigned i=0;i<count;++i,++bit_)v=(v<<1)|((bytes_[bit_/8]>>(7-bit_%8))&1);return v;
    }
    std::string string(std::uint32_t size) {
        bit_=(bit_+7)&~std::size_t(7);const auto at=bit_/8;
        if(size>1024*1024||at>bytes_.size()||size>bytes_.size()-at)throw std::runtime_error("invalid terrain LUA string");
        bit_+=std::size_t(size)*8;return std::string(reinterpret_cast<const char*>(bytes_.data()+at),size);
    }
    static LuaTerrainValue num(double n){LuaTerrainValue v;v.kind=LuaTerrainValue::Kind::number;v.number=n;return v;}
public:
    explicit LuaTerrainReader(std::span<const std::uint8_t> b):bytes_(b){constexpr std::array<std::uint8_t,7> magic{'L','U','A',0,0,0,1};if(b.size()<magic.size()||!std::equal(magic.begin(),magic.end(),b.begin()))throw std::runtime_error("unsupported terrain LUA version");}
    LuaTerrainValue value(unsigned depth=0) {
        if(depth>16||++nodes_>1000000)throw std::runtime_error("terrain LUA limits exceeded");
        const auto type=bits(8);LuaTerrainValue v;
        switch(type) {
        case 1:return v;
        case 2:return num(double(bits(1)));
        case 3:return num(std::bit_cast<float>(std::uint32_t(bits(32))));
        case 4:v.kind=LuaTerrainValue::Kind::string;v.string=string(std::uint32_t(bits(32)));return v;
        case 5: {
            v.kind=LuaTerrainValue::Kind::table;const auto n=bits(32);const auto array=bits(1);
            if(n>200000||nodes_+n>1000000)throw std::runtime_error("terrain LUA table too large");
            v.entries.reserve(std::size_t(n));
            if(array){const auto start=std::bit_cast<std::int32_t>(std::uint32_t(bits(32)));for(std::uint64_t i=0;i<n;++i)v.entries.emplace_back(num(double(std::int64_t(start)+std::int64_t(i))),value(depth+1));}
            else for(std::uint64_t i=0;i<n;++i){auto k=value(depth+1);auto x=value(depth+1);v.entries.emplace_back(std::move(k),std::move(x));}
            return v;
        }
        case 6:return num(std::bit_cast<std::int32_t>(std::uint32_t(bits(32))));
        case 7:return num(std::bit_cast<std::int16_t>(std::uint16_t(bits(16))));
        case 8:return num(std::bit_cast<std::int8_t>(std::uint8_t(bits(8))));
        case 11:return num(std::bit_cast<double>(bits(64)));
        case 100:
            if(bits(32)!=10001)throw std::runtime_error("unsupported terrain LUA native class");
            v.kind=LuaTerrainValue::Kind::uuid;for(auto& byte:v.uuid)byte=std::uint8_t(bits(8));return v;
        default:throw std::runtime_error("unsupported terrain LUA value");
        }
    }
    bool finished() const {return bit_<=bytes_.size()*8&&bytes_.size()*8-bit_<8;}
};
std::uint32_t be32(std::span<const std::uint8_t> b,std::size_t p) { return (std::uint32_t(b[p])<<24)|(std::uint32_t(b[p+1])<<16)|(std::uint32_t(b[p+2])<<8)|b[p+3]; }
std::int32_t signed32(std::span<const std::uint8_t> b,std::size_t p) { return std::bit_cast<std::int32_t>(be32(b,p)); }
std::uint16_t be16(std::span<const std::uint8_t> b,std::size_t p) { return std::uint16_t((std::uint16_t(b[p])<<8)|b[p+1]); }
std::uint32_t le32(std::span<const std::uint8_t> b,std::size_t p) { return std::uint32_t(b[p])|(std::uint32_t(b[p+1])<<8)|(std::uint32_t(b[p+2])<<16)|(std::uint32_t(b[p+3])<<24); }
}
OverworldTerrain decode_overworld_terrain(std::span<const std::uint8_t> decoded) {
    LuaTerrainReader reader(decoded);const auto root=reader.value();if(!reader.finished()||root.kind!=LuaTerrainValue::Kind::table)throw std::runtime_error("invalid overworld terrain table");
    const auto field=[&](std::string_view name)->const LuaTerrainValue& {
        const LuaTerrainValue* found=nullptr;for(const auto& [key,value]:root.entries)if(key.kind==LuaTerrainValue::Kind::string&&key.string==name){if(found)throw std::runtime_error("duplicate terrain field");found=&value;}
        if(!found||found->kind!=LuaTerrainValue::Kind::table)throw std::runtime_error("missing overworld terrain field");return *found;
    };
    const auto integer=[](const LuaTerrainValue& v)->std::int32_t {
        if(v.kind!=LuaTerrainValue::Kind::number||!std::isfinite(v.number)||v.number<INT32_MIN||v.number>INT32_MAX||std::trunc(v.number)!=v.number)throw std::runtime_error("invalid terrain grid integer");return std::int32_t(v.number);
    };
    using Grid=std::map<std::pair<std::int32_t,std::int32_t>,const LuaTerrainValue*>;
    const auto grid=[&](const LuaTerrainValue& rows) {
        Grid out;for(const auto& [y,row]:rows.entries){const auto iy=integer(y);if(row.kind!=LuaTerrainValue::Kind::table)throw std::runtime_error("invalid terrain grid row");
            for(const auto& [x,value]:row.entries)if(!out.emplace(std::pair{integer(x),iy},&value).second)throw std::runtime_error("duplicate terrain grid cell");}
        return out;
    };
    const auto uuids=grid(field("uid")),xs=grid(field("xOffset")),ys=grid(field("yOffset")),rotations=grid(field("rotation"));
    if(uuids.size()!=xs.size()||uuids.size()!=ys.size()||uuids.size()!=rotations.size())throw std::runtime_error("inconsistent terrain grid sizes");
    OverworldTerrain result;auto& cells=result.cells;cells.reserve(uuids.size());
    for(const auto& [position,uuid]:uuids){const auto x=xs.find(position),y=ys.find(position),rotation=rotations.find(position);
        if(uuid->kind!=LuaTerrainValue::Kind::uuid||x==xs.end()||y==ys.end()||rotation==rotations.end())throw std::runtime_error("inconsistent terrain grid cell");
        cells.push_back({position.first,position.second,integer(*x->second),integer(*y->second),integer(*rotation->second),uuid->uuid});}
    for(const auto& [position,value]:grid(field("elevation"))){if(value->kind!=LuaTerrainValue::Kind::number||!std::isfinite(value->number))throw std::runtime_error("invalid terrain elevation");result.elevation.emplace(position,value->number);}
    for(const auto& [position,value]:grid(field("cliffLevel"))){if(value->kind!=LuaTerrainValue::Kind::number||!std::isfinite(value->number))throw std::runtime_error("invalid terrain cliff");result.cliff.emplace(position,value->number);}
    for(const auto& [position,value]:grid(field("flags"))){const auto n=integer(*value);if(n<0)throw std::runtime_error("invalid terrain flags");result.flags.emplace(position,std::uint32_t(n));}
    bool have_seed=false;for(const auto& [key,value]:root.entries)if(key.kind==LuaTerrainValue::Kind::string&&key.string=="seed"){if(have_seed)throw std::runtime_error("duplicate overworld seed");result.seed=integer(value);have_seed=true;}
    if(!have_seed||result.flags.size()!=cells.size()||result.elevation.empty()||result.cliff.empty())throw std::runtime_error("incomplete overworld terrain data");
    return result;
}
std::vector<OverworldCell> decode_overworld_cells(std::span<const std::uint8_t> decoded) {return decode_overworld_terrain(decoded).cells;}
std::int32_t terrain_int_noise_2d(std::int32_t x,std::int32_t y,std::int32_t seed) {
    const auto mix=[](std::uint32_t n){
        const auto shift=[](std::uint32_t v,unsigned k){return std::bit_cast<std::uint32_t>(std::bit_cast<std::int32_t>(v)>>k);};
        n=~n+(n<<15);n=(n^shift(n,12))*5u;n=(n^shift(n,4))*2057u;return n^shift(n,16);
    };
    const auto value=mix(std::uint32_t(x)+mix(std::uint32_t(y)));return std::bit_cast<std::int32_t>(mix(value+std::uint32_t(seed)));
}
std::vector<TerrainFlattenArea> overworld_cell_flatten_areas(const OverworldTerrain& terrain,std::int32_t x,std::int32_t y,std::span<const TerrainFlattenArea> nodes) {
    std::vector<TerrainFlattenArea> areas(nodes.begin(),nodes.end());const auto found=terrain.flags.find({x,y});
    const auto seed=std::bit_cast<std::int32_t>(std::uint32_t(terrain.seed)+358u);
    if(areas.empty()&&found!=terrain.flags.end()&&found->second==0&&terrain_int_noise_2d(x,y,seed)%7==0)areas.push_back({(double(x)+0.5)*64,(double(y)+0.5)*64,16});
    return areas;
}
double unflattened_overworld_height(const OverworldTerrain& t,double x,double y) {
    if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1e9||std::abs(y)>1e9)throw std::runtime_error("invalid overworld coordinates");
    const auto cx=std::int32_t(std::floor(x/64)),cy=std::int32_t(std::floor(y/64));
    const auto corner=[&](const auto& grid,int a,int b){const auto it=grid.find({a,b});return it==grid.end()?0.0:it->second;};
    const auto inside=[&](int a,int b){return t.flags.contains({a,b});};
    const auto mid=[&](int a,int b){if(!inside(a,b))return 0.0;return (corner(t.elevation,a,b)+corner(t.elevation,a+1,b)+corner(t.elevation,a,b+1)+corner(t.elevation,a+1,b+1))*0.25;};
    const auto west=[&](int a,int b){return inside(a,b)?(corner(t.elevation,a,b)+corner(t.elevation,a,b+1))*0.5:0.0;};
    const auto south=[&](int a,int b){return inside(a,b)?(corner(t.elevation,a,b)+corner(t.elevation,a+1,b))*0.5:0.0;};
    const auto sw=[&](int a,int b){return inside(a,b)?corner(t.elevation,a,b):0.0;};
    const auto flat=[&](int a,int b,std::uint32_t mask){const auto it=t.flags.find({a,b});return it!=t.flags.end()&&(it->second&mask)!=0?1.0:0.0;};
    const auto bezier=[](double a,double b,double c,double d,double p){const auto q=1-p;return q*q*q*a+3*q*q*p*b+3*q*p*p*c+p*p*p*d;};
    const auto lerp=[](double a,double b,double p){return a+(b-a)*p;};
    const auto midx=[&](int a,int b,double p){return bezier(mid(a,b),flat(a,b,0xa00)?mid(a,b):west(a+1,b),flat(a+1,b,0xa00)?mid(a+1,b):west(a+1,b),mid(a+1,b),p);};
    const auto southx=[&](int a,int b,double p){const auto fe=(flat(a,b-1,0xa00)+flat(a,b,0xa00))*0.5,fw=(flat(a+1,b-1,0xa00)+flat(a+1,b,0xa00))*0.5;return bezier(south(a,b),lerp(sw(a+1,b),south(a,b),fe),lerp(sw(a+1,b),south(a+1,b),fw),south(a+1,b),p);};
    const double fx=x/64-cx,fy=y/64-cy;const int a=fx<0.5?cx-1:cx,b=fy<0.5?cy-1:cy;const double px=fx<0.5?fx+0.5:fx-0.5,py=fy<0.5?fy+0.5:fy-0.5;
    const auto fn=lerp(flat(a,b,0x500),flat(a+1,b,0x500),px),fs=lerp(flat(a,b+1,0x500),flat(a+1,b+1,0x500),px);
    const auto base=bezier(midx(a,b,px),lerp(southx(a,b+1,px),midx(a,b,px),fn),lerp(southx(a,b+1,px),midx(a,b+1,px),fs),midx(a,b+1,px),py);
    const auto cliffs=std::min({corner(t.cliff,cx,cy),corner(t.cliff,cx+1,cy),corner(t.cliff,cx,cy+1),corner(t.cliff,cx+1,cy+1)});
    return base+8*cliffs;
}
double overworld_height_with_flatten_areas(const OverworldTerrain& terrain,double x,double y,std::span<const TerrainFlattenArea> areas) {
    const auto original=unflattened_overworld_height(terrain,x,y);
    const auto cliff=[&](double px,double py){const auto cx=std::int32_t(std::floor(px/64)),cy=std::int32_t(std::floor(py/64));
        const auto at=[&](int a,int b){const auto it=terrain.cliff.find({a,b});return it==terrain.cliff.end()?0.0:it->second;};
        return 8*std::min({at(cx,cy),at(cx+1,cy),at(cx,cy+1),at(cx+1,cy+1)});};
    double max_weight=0;const TerrainFlattenArea* selected=nullptr;
    for(const auto& area:areas) {
        if(!std::isfinite(area.x)||!std::isfinite(area.y)||!std::isfinite(area.radius)||area.radius<=0||std::abs(area.x)>1e9||std::abs(area.y)>1e9)throw std::runtime_error("invalid terrain flatten area");
        const auto distance=std::hypot(x-area.x,y-area.y)/area.radius;
        if(distance<2){const auto p=std::clamp(2-distance,0.0,1.0);const auto weight=p*p*(3-2*p);if(!selected||weight>max_weight){max_weight=weight;selected=&area;}}
    }
    if(!selected)return original;
    const auto current_cliff=cliff(x,y);const auto target_base=unflattened_overworld_height(terrain,selected->x,selected->y)-cliff(selected->x,selected->y);
    const auto target=std::floor(target_base*4+0.5)*0.25;
    return original+(target-(original-current_cliff))*max_weight;
}
std::map<std::array<std::uint8_t,16>,std::filesystem::path> index_game_terrain_tiles(const std::filesystem::path& game) {
    std::map<std::array<std::uint8_t,16>,std::filesystem::path> result;
    for(const auto* branch:{"Data","Survival"}) {
        const auto root=game/branch/"Terrain";std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root,std::filesystem::directory_options::skip_permission_denied,ec),end;
        if(ec)continue;
        for(;it!=end;it.increment(ec)){
            if(ec){ec.clear();continue;}if(!it->is_regular_file(ec)||ec||it->path().extension()!=".tile"){ec.clear();continue;}
            std::ifstream file(it->path(),std::ios::binary);std::array<std::uint8_t,24> header{};
            if(!file.read(reinterpret_cast<char*>(header.data()),header.size())||header[0]!='T'||header[1]!='I'||header[2]!='L'||header[3]!='E')continue;
            const auto version=le32(header,4);if(version<11||version>15)continue;
            std::array<std::uint8_t,16> uuid{};std::reverse_copy(header.begin()+8,header.begin()+24,uuid.begin());
            const auto [entry,inserted]=result.emplace(uuid,it->path());
            if(!inserted&&entry->second!=it->path())throw std::runtime_error("duplicate terrain tile UUID");
        }
    }
    return result;
}
TerrainDelta decode_terrain_delta(std::span<const std::uint8_t> compressed) {
    if(compressed.empty()||compressed.size()>131072)throw std::runtime_error("invalid terrain compressed size");
    std::vector<std::uint8_t> decoded(34+65535);
    const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(compressed.data()),reinterpret_cast<char*>(decoded.data()),int(compressed.size()),int(decoded.size()));
    if(n<34)throw std::runtime_error("invalid compressed terrain record");
    decoded.resize(static_cast<std::size_t>(n));const std::span<const std::uint8_t> b(decoded);
    if(b[0]!=12||b[1]!=0||b[2]!=1)throw std::runtime_error("unknown terrain record version");
    const auto size=be16(b,32);if(b[31]<4||b[31]>9||decoded.size()!=34u+size)throw std::runtime_error("invalid terrain delta envelope");
    TerrainDelta value;value.id=be32(b,3);value.region_y=signed32(b,7);value.region_x=signed32(b,11);
    value.world_id=be16(b,15);if(value.world_id!=be16(b,17))throw std::runtime_error("terrain world IDs disagree");
    value.chunk_position={signed32(b,27),signed32(b,23),signed32(b,19)};value.kind=b[31];
    value.payload.assign(b.begin()+34,b.end());return value;
}
std::vector<TerrainChange> decode_terrain_density_changes(const TerrainDelta& delta) {
    if(delta.kind<4||delta.kind>9||delta.payload.empty())throw std::runtime_error("unsupported terrain density delta");
    const auto& p=delta.payload;const bool sparse=delta.kind>=8,with_material=(delta.kind%2)==0;
    std::size_t changes=0,header_bits=0;
    if(sparse){changes=p[0];header_bits=8+changes*12;}
    else {
        unsigned octants=0;for(unsigned i=0;i<8;++i)octants+=(p[0]>>i)&1;
        const std::size_t header=1+octants*64;if(p.size()<header)throw std::runtime_error("truncated terrain octant mask");
        for(std::size_t i=1;i<header;++i)changes+=std::popcount(unsigned(p[i]));header_bits=header*8;
    }
    const auto material_start=header_bits+changes*5;
    if(p.size()!=(material_start+(with_material?changes*3:0)+7)/8)throw std::runtime_error("invalid terrain density data length");
    constexpr std::array<std::array<std::uint8_t,3>,8> bases{{{0,0,0},{8,0,0},{8,8,0},{8,0,8},{0,8,0},{0,0,8},{0,8,8},{8,8,8}}};
    std::vector<TerrainChange> output;output.reserve(changes);std::size_t bit=header_bits,material_bit=material_start;
    const auto read_bits=[&](std::size_t& offset,unsigned n){unsigned value=0;for(unsigned i=0;i<n;++i,++offset)value=(value<<1)|((p[offset/8]>>(7-offset%8))&1);return value;};
    const auto append=[&](unsigned octant,unsigned index) {
            TerrainChange change;for(unsigned axis=0;axis<3;++axis) {
                change.position[axis]=bases[octant][axis];
                for(unsigned plane=0;plane<3;++plane)change.position[axis]+=std::uint8_t(((index>>(axis+3*plane))&1u)<<plane);
            }
            change.density=std::uint8_t(read_bits(bit,5));if(with_material)change.material=std::uint8_t(read_bits(material_bit,3));
            output.push_back(change);
    };
    if(sparse) {
        std::size_t position_bit=8;
        for(std::size_t i=0;i<changes;++i) {
            const auto low=read_bits(position_bit,8),high=read_bits(position_bit,4);
            const auto address=low|(high<<8);append(address>>9,address&511);
        }
    } else {
        std::size_t mask_offset=1;
        for(unsigned octant=0;octant<8;++octant)if(p[0]&(1u<<octant)) {
            for(unsigned index=0;index<512;++index)if(p[mask_offset+index/8]&(128u>>(index%8)))append(octant,index);
            mask_offset+=64;
        }
    }
    return output;
}
void apply_terrain_density_changes(std::span<std::uint8_t> generated_chunk,const TerrainDelta& delta) {
    if(generated_chunk.size()!=17u*17u*17u)throw std::runtime_error("terrain base chunk must contain 4913 voxels");
    // Decode fully before touching the caller's base chunk.
    const auto changes=decode_terrain_density_changes(delta);
    for(const auto& change:changes) {
        const auto [x,y,z]=change.position;auto& voxel=generated_chunk[17u*(y+17u*x)+z];
        const auto high=change.material?std::uint8_t(*change.material<<5):std::uint8_t(voxel&0xe0);
        voxel=std::uint8_t(high|change.density);
    }
}
namespace {
void apply_terrain_neighbour_changes_impl(std::span<std::uint8_t> generated_chunk,std::uint16_t world_id,std::array<std::int32_t,3> position,std::span<const TerrainDelta> deltas,bool all_neighbours) {
    if(generated_chunk.size()!=4913)throw std::runtime_error("terrain base chunk must contain 4913 voxels");
    std::array<std::uint8_t,4913> updated{};std::copy(generated_chunk.begin(),generated_chunk.end(),updated.begin());
    std::array<bool,27> seen{};
    for(const auto& delta:deltas) {
        if(delta.world_id!=world_id)continue;
        std::array<int,3> direction{};bool relevant=true;
        for(unsigned axis=0;axis<3;++axis){const auto distance=std::int64_t(delta.chunk_position[axis])-position[axis];if(distance< -1||distance>1||(!all_neighbours&&distance<0)){relevant=false;break;}direction[axis]=static_cast<int>(distance);}
        if(!relevant)continue;
        const auto slot=std::size_t(direction[0]+1)*9+std::size_t(direction[1]+1)*3+std::size_t(direction[2]+1);
        if(seen[slot])throw std::runtime_error("duplicate terrain delta for chunk");seen[slot]=true;
        for(const auto& change:decode_terrain_density_changes(delta)) {
            auto p=change.position;bool boundary=true;
            for(unsigned axis=0;axis<3;++axis)if(direction[axis]){
                const auto source=direction[axis]>0?0:16;if(p[axis]!=source){boundary=false;break;}p[axis]=direction[axis]>0?16:0;
            }
            if(!boundary)continue;
            auto& voxel=updated[17u*(p[1]+17u*p[0])+p[2]];
            voxel=std::uint8_t((change.material?*change.material<<5:voxel&0xe0)|change.density);
        }
    }
    std::copy(updated.begin(),updated.end(),generated_chunk.begin());
}
}
void apply_terrain_neighbour_changes(std::span<std::uint8_t> generated_chunk,std::uint16_t world_id,std::array<std::int32_t,3> position,std::span<const TerrainDelta> deltas) {
    apply_terrain_neighbour_changes_impl(generated_chunk,world_id,position,deltas,false);
}
void apply_terrain_all_neighbour_changes(std::span<std::uint8_t> generated_chunk,std::uint16_t world_id,std::array<std::int32_t,3> position,std::span<const TerrainDelta> deltas) {
    apply_terrain_neighbour_changes_impl(generated_chunk,world_id,position,deltas,true);
}
std::vector<TerrainTileChunk> read_terrain_tile_cell(const std::filesystem::path& path,std::uint32_t cell_x,std::uint32_t cell_y) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("cannot open terrain tile");
    const auto end=file.tellg();if(end<60)throw std::runtime_error("truncated terrain tile header");
    const auto file_size=static_cast<std::uint64_t>(end);
    const auto read=[&](std::uint64_t offset,std::size_t size) {
        if(offset>file_size||size>file_size-offset||size>64u*1024u*1024u)throw std::runtime_error("terrain tile range outside file");
        std::vector<std::uint8_t> bytes(size);file.seekg(static_cast<std::streamoff>(offset));
        if(size&&!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))throw std::runtime_error("cannot read terrain tile data");return bytes;
    };
    const auto header=read(0,60);const std::span<const std::uint8_t> h(header);
    if(h[0]!='T'||h[1]!='I'||h[2]!='L'||h[3]!='E')throw std::runtime_error("invalid terrain tile magic");
    const auto version=le32(h,4),width=le32(h,32),height=le32(h,36),offset=le32(h,40),cell_size=le32(h,44);
    if(version<11||version>15)throw std::runtime_error("unsupported terrain tile version");
    if(cell_x>=width||cell_y>=height||cell_size<388||cell_size>65536)throw std::runtime_error("invalid terrain tile cell");
    const auto cell=read(std::uint64_t(offset)+(std::uint64_t(cell_y)*width+cell_x)*cell_size,cell_size);
    const std::span<const std::uint8_t> c(cell);const auto count=le32(c,372),data_offset=le32(c,376),compressed_size=le32(c,380),plain_size=le32(c,384);
    if(!count){if(compressed_size||plain_size)throw std::runtime_error("invalid empty terrain tile cell");return {};}
    if(std::uint64_t(count)*4108!=plain_size||plain_size>64u*1024u*1024u||!compressed_size||compressed_size>64u*1024u*1024u)throw std::runtime_error("invalid terrain tile voxel size");
    const auto compressed=read(data_offset,compressed_size);std::vector<std::uint8_t> plain(plain_size);
    const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(compressed.data()),reinterpret_cast<char*>(plain.data()),int(compressed.size()),int(plain.size()));
    if(n!=int(plain.size()))throw std::runtime_error("invalid compressed terrain tile voxels");
    const std::span<const std::uint8_t> p(plain);std::vector<TerrainTileChunk> chunks;chunks.reserve(count);
    for(std::size_t i=0;i<count;++i){const auto base=i*4108;TerrainTileChunk chunk;
        for(std::size_t axis=0;axis<3;++axis)chunk.position[axis]=std::bit_cast<std::int32_t>(le32(p,base+axis*4));
        std::copy_n(p.begin()+base+12,4096,chunk.voxels.begin());chunks.push_back(std::move(chunk));}
    return chunks;
}
TerrainTileHeights read_terrain_tile_heights(const std::filesystem::path& path,std::uint32_t cell_x,std::uint32_t cell_y,std::uint32_t mip) {
    if(mip>5)throw std::runtime_error("invalid terrain height mip");
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("cannot open terrain tile");
    const auto end=file.tellg();if(end<60)throw std::runtime_error("truncated terrain tile header");const auto size=std::uint64_t(end);
    const auto read=[&](std::uint64_t at,std::size_t count){if(at>size||count>size-at||count>64u*1024u*1024u)throw std::runtime_error("terrain tile range outside file");std::vector<std::uint8_t> b(count);file.seekg(std::streamoff(at));if(count&&!file.read(reinterpret_cast<char*>(b.data()),std::streamsize(count)))throw std::runtime_error("cannot read terrain tile data");return b;};
    const auto h=read(0,60);if(h[0]!='T'||h[1]!='I'||h[2]!='L'||h[3]!='E'||le32(h,4)<11||le32(h,4)>15)throw std::runtime_error("unsupported terrain tile");
    const auto width=le32(h,32),height=le32(h,36),offset=le32(h,40),cell_size=le32(h,44);
    if(cell_x>=width||cell_y>=height||cell_size<72||cell_size>65536)throw std::runtime_error("invalid terrain height cell");
    const auto c=read(std::uint64_t(offset)+(std::uint64_t(cell_y)*width+cell_x)*cell_size,cell_size);
    const auto data_offset=le32(c,mip*4),compressed_size=le32(c,24+mip*4),plain_size=le32(c,48+mip*4);
    const auto scale=1u<<mip,side=32u/scale+1,fine_side=64u/scale+1;
    const auto expected=8u*(side*side+fine_side*fine_side);
    if(!compressed_size||compressed_size>64u*1024u*1024u||plain_size!=expected)throw std::runtime_error("invalid terrain height payload size");
    const auto packed=read(data_offset,compressed_size);std::vector<std::uint8_t> plain(plain_size);
    const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()),reinterpret_cast<char*>(plain.data()),int(packed.size()),int(plain.size()));
    if(n!=int(plain.size()))throw std::runtime_error("invalid compressed terrain heights");
    TerrainTileHeights result{2u*scale,side,{}};result.samples.reserve(side*side);
    for(std::size_t i=0;i<std::size_t(side)*side;++i){const auto value=std::bit_cast<float>(le32(plain,i*8));if(!std::isfinite(value))throw std::runtime_error("nonfinite terrain height");result.samples.push_back(value);}
    return result;
}
std::vector<TerrainFlattenArea> read_terrain_tile_flatten_nodes(const std::filesystem::path& path,std::uint32_t cell_x,std::uint32_t cell_y) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("cannot open terrain tile");
    const auto end=file.tellg();if(end<60)throw std::runtime_error("truncated terrain tile header");const auto size=std::uint64_t(end);
    const auto read=[&](std::uint64_t at,std::size_t count){if(at>size||count>size-at||count>64u*1024u*1024u)throw std::runtime_error("terrain tile range outside file");std::vector<std::uint8_t> b(count);file.seekg(std::streamoff(at));if(count&&!file.read(reinterpret_cast<char*>(b.data()),std::streamsize(count)))throw std::runtime_error("cannot read terrain tile data");return b;};
    const auto h=read(0,60);if(h[0]!='T'||h[1]!='I'||h[2]!='L'||h[3]!='E'||le32(h,4)<11||le32(h,4)>15)throw std::runtime_error("unsupported terrain tile");
    const auto width=le32(h,32),height=le32(h,36),offset=le32(h,40),cell_size=le32(h,44);
    if(cell_x>=width||cell_y>=height||cell_size<180||cell_size>65536)throw std::runtime_error("invalid terrain node cell");
    const auto c=read(std::uint64_t(offset)+(std::uint64_t(cell_y)*width+cell_x)*cell_size,cell_size);
    const auto count=le32(c,164),data_offset=le32(c,168),compressed_size=le32(c,172),plain_size=le32(c,176);
    if(!count)return {};if(count>1000000||!compressed_size||!plain_size||plain_size>64u*1024u*1024u)throw std::runtime_error("invalid terrain node payload size");
    const auto packed=read(data_offset,compressed_size);std::vector<std::uint8_t> plain(plain_size);
    const auto n=LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()),reinterpret_cast<char*>(plain.data()),int(packed.size()),int(plain.size()));
    if(n!=int(plain.size()))throw std::runtime_error("invalid compressed terrain nodes");
    std::size_t at=0;const auto need=[&](std::size_t len){if(at>plain.size()||len>plain.size()-at)throw std::runtime_error("truncated terrain nodes");};
    need(1);const auto tag_count=plain[at++];std::vector<std::string> tags;tags.reserve(tag_count);
    for(unsigned i=0;i<tag_count;++i){need(1);const auto len=plain[at++];need(len);tags.emplace_back(reinterpret_cast<const char*>(plain.data()+at),len);at+=len;}
    std::vector<TerrainFlattenArea> result;
    for(std::uint32_t i=0;i<count;++i){need(41);const auto x=std::bit_cast<float>(le32(plain,at)),y=std::bit_cast<float>(le32(plain,at+4)),radius=std::bit_cast<float>(le32(plain,at+28))*0.5f;at+=40;
        const auto used=plain[at++];need(used);bool flatten=false;for(unsigned j=0;j<used;++j){const auto index=plain[at++];if(index>=tags.size())throw std::runtime_error("invalid terrain node tag");if(tags[index]=="FLATTEN")flatten=true;}
        need(4);const auto user_size=le32(plain,at);at+=4;need(user_size);at+=user_size;
        if(flatten){if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(radius)||radius<=0)throw std::runtime_error("invalid terrain flatten node");result.push_back({x,y,radius});}}
    if(at!=plain.size())throw std::runtime_error("trailing terrain node data");return result;
}
std::vector<TerrainFlattenArea> collect_overworld_flatten_nodes(const OverworldTerrain& terrain,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog,std::int32_t cell_x,std::int32_t cell_y) {
    if(cell_x==INT32_MIN||cell_x==INT32_MAX||cell_y==INT32_MIN||cell_y==INT32_MAX)throw std::runtime_error("terrain cell coordinate overflow");
    std::map<std::pair<std::int32_t,std::int32_t>,const OverworldCell*> cells;
    for(const auto& cell:terrain.cells)if(!cells.emplace(std::pair{cell.x,cell.y},&cell).second)throw std::runtime_error("duplicate overworld cell");
    std::vector<TerrainFlattenArea> result;
    for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto x=cell_x+dx,y=cell_y+dy;const auto it=cells.find({x,y});
        if(it==cells.end()){if(terrain.flags.contains({x,y}))throw std::runtime_error("overworld cell data missing");continue;}
        const auto& cell=*it->second;const auto path=catalog.find(cell.tile_uuid);if(path==catalog.end()||cell.tile_x<0||cell.tile_y<0||cell.rotation<0||cell.rotation>3)throw std::runtime_error("invalid overworld tile placement");
        auto nodes=read_terrain_tile_flatten_nodes(path->second,std::uint32_t(cell.tile_x),std::uint32_t(cell.tile_y));
        for(auto& node:nodes){const auto a=std::floor(node.x+0.5),b=std::floor(node.y+0.5);switch(cell.rotation){case 1:node.x=64-b;node.y=a;break;case 2:node.x=64-a;node.y=64-b;break;case 3:node.x=b;node.y=64-a;break;default:node.x=a;node.y=b;break;}node.x+=double(x)*64;node.y+=double(y)*64;result.push_back(node);}}
    return result;
}
std::vector<TerrainTileChunk> read_overworld_tile_voxels(const OverworldCell& cell,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog) {
    const auto tile=catalog.find(cell.tile_uuid);if(tile==catalog.end())throw std::runtime_error("overworld terrain tile is missing");
    if(cell.tile_x<0||cell.tile_y<0||cell.rotation<0||cell.rotation>3)throw std::runtime_error("invalid overworld terrain tile placement");
    return read_terrain_tile_cell(tile->second,std::uint32_t(cell.tile_x),std::uint32_t(cell.tile_y));
}
std::optional<std::array<std::uint8_t,4913>> assemble_terrain_tile_chunk(std::span<const TerrainTileChunk> chunks,std::array<std::int32_t,3> position) {
    std::map<std::array<std::int32_t,3>,const TerrainTileChunk*> by_position;
    for(const auto& chunk:chunks)if(!by_position.emplace(chunk.position,&chunk).second)throw std::runtime_error("duplicate terrain tile chunk coordinates");
    std::array<const TerrainTileChunk*,8> neighbours{};
    for(unsigned mask=0;mask<8;++mask) {
        auto pos=position;for(unsigned axis=0;axis<3;++axis)if(mask&(1u<<axis)) {
            if(pos[axis]==std::numeric_limits<std::int32_t>::max())throw std::runtime_error("terrain chunk coordinates overflow");++pos[axis];
        }
        const auto found=by_position.find(pos);if(found==by_position.end())return std::nullopt;neighbours[mask]=found->second;
    }
    std::array<std::uint8_t,4913> result{};
    for(unsigned x=0;x<17;++x)for(unsigned y=0;y<17;++y)for(unsigned z=0;z<17;++z) {
        const unsigned mask=(x==16?1:0)|(y==16?2:0)|(z==16?4:0);
        result[17*(y+17*x)+z]=neighbours[mask]->voxels[256*(x%16)+16*(y%16)+(z%16)];
    }
    return result;
}
std::vector<TerrainVertex> mesh_overworld_tile_voxels(const OverworldCell& cell,std::span<const TerrainTileChunk> chunks,const TerrainMeshTables& tables) {
    return mesh_overworld_tile_voxels(cell,chunks,0,{},tables);
}
std::vector<TerrainVertex> mesh_overworld_tile_voxels(const OverworldCell& cell,std::span<const TerrainTileChunk> chunks,std::uint16_t world_id,std::span<const TerrainDelta> deltas,const TerrainMeshTables& tables) {
    if(cell.rotation<0||cell.rotation>3)throw std::runtime_error("invalid overworld terrain rotation");
    std::vector<TerrainVertex> out;
    for(const auto& chunk:chunks) {
        if(chunk.position[0]<0||chunk.position[0]>=4||chunk.position[1]<0||chunk.position[1]>=4||chunk.position[2]<0||chunk.position[2]>=16)continue;
        const auto assembled=assemble_terrain_tile_chunk(chunks,chunk.position);if(!assembled)continue;
        auto voxels=*assembled;
        if(!deltas.empty()) {
            std::int32_t local_x=chunk.position[0],local_y=chunk.position[1];
            switch(cell.rotation){case 1:{const auto x=local_x;local_x=3-local_y;local_y=x;break;}case 2:local_x=3-local_x;local_y=3-local_y;break;case 3:{const auto x=local_x;local_x=local_y;local_y=3-x;break;}default:break;}
            const std::array<std::int32_t,3> world_position{cell.x*4+local_x,cell.y*4+local_y,chunk.position[2]};
            apply_terrain_all_neighbour_changes(voxels,world_id,world_position,deltas);
        }
        auto vertices=mesh_terrain_chunk(voxels,chunk.position,tables);
        if(vertices.size()>6000000-out.size())throw std::runtime_error("overworld terrain mesh too large");
        for(auto& v:vertices) {
            const auto x=v.x,y=v.y;
            switch(cell.rotation) {case 1:v.x=64-y;v.y=x;break;case 2:v.x=64-x;v.y=64-y;break;case 3:v.x=y;v.y=64-x;break;default:break;}
            v.x+=float(double(cell.x)*64);v.y+=float(double(cell.y)*64);
        }
        out.insert(out.end(),vertices.begin(),vertices.end());
    }
    return out;
}
std::vector<TerrainVertex> mesh_overworld_heightfield(const OverworldCell& cell,const TerrainTileHeights& heights,std::span<const float> world_offsets) {
    if(cell.rotation<0||cell.rotation>3||heights.side<2||heights.side>33||!heights.spacing||std::uint64_t(heights.side-1)*heights.spacing!=64)throw std::runtime_error("invalid overworld height grid");
    const auto count=std::size_t(heights.side)*heights.side;
    if(heights.samples.size()!=count||world_offsets.size()!=count)throw std::runtime_error("incomplete overworld height offsets");
    std::vector<TerrainVertex> points;points.reserve(count);
    for(unsigned y=0;y<heights.side;++y)for(unsigned x=0;x<heights.side;++x){
        const auto index=std::size_t(y)*heights.side+x;const float h=heights.samples[index]+world_offsets[index];
        if(!std::isfinite(heights.samples[index])||!std::isfinite(world_offsets[index])||!std::isfinite(h))throw std::runtime_error("nonfinite overworld height");
        float px=float(x*heights.spacing),py=float(y*heights.spacing);const auto ox=px,oy=py;
        switch(cell.rotation){case 1:px=64-oy;py=ox;break;case 2:px=64-ox;py=64-oy;break;case 3:px=oy;py=64-ox;break;default:break;}
        points.push_back({px+float(double(cell.x)*64),py+float(double(cell.y)*64),h});
    }
    std::vector<TerrainVertex> out;out.reserve(std::size_t(heights.side-1)*(heights.side-1)*6);
    for(unsigned y=0;y+1<heights.side;++y)for(unsigned x=0;x+1<heights.side;++x){const auto i=std::size_t(y)*heights.side+x;const auto s=heights.side;
        out.insert(out.end(),{points[i],points[i+1],points[i+s+1],points[i],points[i+s+1],points[i+s]});}
    return out;
}
std::vector<TerrainVertex> mesh_generated_overworld_heightfield(const OverworldTerrain& terrain,const OverworldCell& cell,const TerrainTileHeights& heights,const std::function<std::vector<TerrainFlattenArea>(std::int32_t,std::int32_t)>& flatten_nodes) {
    if(cell.rotation<0||cell.rotation>3||std::abs(double(cell.x))>15624998||std::abs(double(cell.y))>15624998||heights.side<2||heights.side>33||std::uint64_t(heights.side-1)*heights.spacing!=64||heights.samples.size()!=std::size_t(heights.side)*heights.side||!terrain.flags.contains({cell.x,cell.y})||!flatten_nodes)throw std::runtime_error("invalid generated overworld cell");
    std::map<std::pair<std::int32_t,std::int32_t>,std::vector<TerrainFlattenArea>> cache;std::vector<float> offsets;offsets.reserve(std::size_t(heights.side)*heights.side);
    for(unsigned y=0;y<heights.side;++y)for(unsigned x=0;x<heights.side;++x){
        double px=double(x)*heights.spacing,py=double(y)*heights.spacing;const auto ox=px,oy=py;
        switch(cell.rotation){case 1:px=64-oy;py=ox;break;case 2:px=64-ox;py=64-oy;break;case 3:px=oy;py=64-ox;break;default:break;}
        px+=double(cell.x)*64;py+=double(cell.y)*64;
        const auto wx=std::int32_t(std::floor(px/64)),wy=std::int32_t(std::floor(py/64));
        if(!terrain.flags.contains({wx,wy})){offsets.push_back(-16-heights.samples[std::size_t(y)*heights.side+x]);continue;}
        const auto key=std::pair{wx,wy};auto it=cache.find(key);
        if(it==cache.end()){const auto nodes=flatten_nodes(wx,wy);it=cache.emplace(key,overworld_cell_flatten_areas(terrain,wx,wy,nodes)).first;}
        offsets.push_back(float(overworld_height_with_flatten_areas(terrain,px,py,it->second)));
    }
    return mesh_overworld_heightfield(cell,heights,offsets);
}
std::vector<TerrainVertex> mesh_saved_overworld_cell(const OverworldTerrain& terrain,const std::map<std::array<std::uint8_t,16>,std::filesystem::path>& catalog,const OverworldCell& cell) {
    if(cell.rotation<0||cell.rotation>3||std::abs(double(cell.x))>15624998||std::abs(double(cell.y))>15624998)throw std::runtime_error("invalid saved overworld cell");
    std::map<std::pair<std::int32_t,std::int32_t>,const OverworldCell*> cells;for(const auto& part:terrain.cells)if(!cells.emplace(std::pair{part.x,part.y},&part).second)throw std::runtime_error("duplicate overworld cell");
    const auto original=cells.find({cell.x,cell.y});if(original==cells.end()||original->second->tile_uuid!=cell.tile_uuid||original->second->tile_x!=cell.tile_x||original->second->tile_y!=cell.tile_y||original->second->rotation!=cell.rotation)throw std::runtime_error("saved overworld cell mismatch");
    std::map<std::pair<std::int32_t,std::int32_t>,TerrainTileHeights> cache;
    const auto get_heights=[&](const OverworldCell& part)->const TerrainTileHeights& {
        const auto key=std::pair{part.x,part.y};auto it=cache.find(key);if(it!=cache.end())return it->second;
        const auto tile=catalog.find(part.tile_uuid);if(tile==catalog.end()||part.tile_x<0||part.tile_y<0||part.rotation<0||part.rotation>3)throw std::runtime_error("invalid overworld tile placement");
        return cache.emplace(key,read_terrain_tile_heights(tile->second,std::uint32_t(part.tile_x),std::uint32_t(part.tile_y))).first->second;
    };
    TerrainTileHeights heights=get_heights(cell);
    for(unsigned y=0;y<heights.side;++y)for(unsigned x=0;x<heights.side;++x){
        const int lx=int(x*heights.spacing),ly=int(y*heights.spacing);int rx=lx,ry=ly;
        switch(cell.rotation){case 1:rx=64-ly;ry=lx;break;case 2:rx=64-lx;ry=64-ly;break;case 3:rx=ly;ry=64-lx;break;default:break;}
        const auto wx=std::int64_t(cell.x)*64+rx,wy=std::int64_t(cell.y)*64+ry;
        const auto tx=std::int32_t(std::floor(double(wx)/64)),ty=std::int32_t(std::floor(double(wy)/64));
        const auto target=cells.find({tx,ty});if(target==cells.end()){if(terrain.flags.contains({tx,ty}))throw std::runtime_error("adjacent overworld cell missing");continue;}
        const auto& neighbour=*target->second;int px=int(wx-std::int64_t(tx)*64),py=int(wy-std::int64_t(ty)*64);const auto ox=px,oy=py;
        switch(neighbour.rotation){case 1:px=oy;py=64-ox;break;case 2:px=64-ox;py=64-oy;break;case 3:px=64-oy;py=ox;break;default:break;}
        const auto& samples=get_heights(neighbour);if(px<0||py<0||px>64||py>64||px%int(samples.spacing)||py%int(samples.spacing))throw std::runtime_error("unaligned adjacent terrain sample");
        heights.samples[std::size_t(y)*heights.side+x]=samples.samples[std::size_t(py/samples.spacing)*samples.side+px/samples.spacing];
    }
    return mesh_generated_overworld_heightfield(terrain,cell,heights,[&](std::int32_t x,std::int32_t y){return collect_overworld_flatten_nodes(terrain,catalog,x,y);});
}
TerrainMeshTables TerrainMeshTables::from_game_executable(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("cannot open game executable");
    const auto end=file.tellg();if(end<64)throw std::runtime_error("truncated game executable");const auto size=std::uint64_t(end);
    const auto read=[&](std::uint64_t offset,std::size_t length){if(offset>size||length>size-offset)throw std::runtime_error("invalid executable range");std::vector<std::uint8_t> b(length);file.seekg(std::streamoff(offset));if(length&&!file.read(reinterpret_cast<char*>(b.data()),std::streamsize(length)))throw std::runtime_error("cannot read game executable");return b;};
    const auto dos=read(0,64);if(dos[0]!='M'||dos[1]!='Z')throw std::runtime_error("invalid game executable magic");
    const auto pe_offset=le32(dos,60);const auto header=read(pe_offset,24);
    if(header[0]!='P'||header[1]!='E'||header[2]||header[3])throw std::runtime_error("invalid game executable header");
    const auto count=std::uint16_t(header[6]|(header[7]<<8)),optional_size=std::uint16_t(header[20]|(header[21]<<8));
    if(!count||count>96)throw std::runtime_error("invalid game executable section count");
    const auto sections=read(std::uint64_t(pe_offset)+24+optional_size,std::size_t(count)*40);
    const auto rva_read=[&](std::uint32_t address,std::size_t length) {
        for(unsigned i=0;i<count;++i){const auto at=std::size_t(i)*40;const auto va=le32(sections,at+12),raw_size=le32(sections,at+16),raw=le32(sections,at+20);
            if(address>=va&&std::uint64_t(address-va)+length<=raw_size)return read(std::uint64_t(raw)+address-va,length);}
        throw std::runtime_error("terrain lookup tables not found in game executable");
    };
    // RVAs identified in the 889 client. Validate the lookup structure before
    // use; another executable revision must be researched independently.
    const auto edges=rva_read(0x14711c0,256*24),patterns=rva_read(0x14763e0,256),triangles=rva_read(0x14764e0,256);
    if(patterns[0]!=0||patterns[1]!=1||patterns[255]!=0||edges[24]!=0x01||edges[25]!=0x62||triangles[16]!=0x31)throw std::runtime_error("unsupported game terrain table revision");
    TerrainMeshTables result;
    for(unsigned i=0;i<16;++i)std::copy_n(triangles.begin()+16*i,16,result.triangles[i].begin());
    for(unsigned i=0;i<256;++i) {
        result.patterns[i]=patterns[i];if(patterns[i]>=16)throw std::runtime_error("invalid game terrain pattern");
        const auto& pattern=result.triangles[patterns[i]];const auto vertex_count=unsigned(pattern[0]>>4),triangle_count=unsigned(pattern[0]&15);
        if(vertex_count>12||triangle_count>5||3*triangle_count>15)throw std::runtime_error("invalid game terrain triangulation");
        for(unsigned j=0;j<12;++j){const auto e=std::uint16_t(edges[24*i+2*j]|(std::uint16_t(edges[24*i+2*j+1])<<8));result.edges[i][j]=e;
            if(j<vertex_count){const auto a=e&7,b=(e>>4)&7;if(a==b||(((i>>a)^(i>>b))&1)==0)throw std::runtime_error("invalid game terrain edge");}}
        for(unsigned j=0;j<3*triangle_count;++j)if(pattern[1+j]>=vertex_count)throw std::runtime_error("invalid game terrain triangle");
    }
    return result;
}
std::vector<TerrainVertex> mesh_terrain_chunk(std::span<const std::uint8_t> voxels,std::array<std::int32_t,3> position,const TerrainMeshTables& tables) {
    if(voxels.size()!=4913)throw std::runtime_error("terrain base chunk must contain 4913 voxels");
    for(const auto coordinate:position)if(coordinate<-1000000||coordinate>1000000)throw std::runtime_error("terrain chunk coordinates outside mesh range");
    std::vector<TerrainVertex> output;
    for(int x=0;x<16;++x)for(int y=0;y<16;++y)for(int z=0;z<16;++z) {
        std::array<int,8> density{};unsigned code=0;
        for(unsigned corner=0;corner<8;++corner){const auto cx=x+int(corner&1),cy=y+int((corner>>1)&1),cz=z+int((corner>>2)&1);density[corner]=int(voxels[17*(cy+17*cx)+cz]&31)-16;if(density[corner]<0)code|=1u<<corner;}
        const auto& pattern=tables.triangles[tables.patterns[code]];const auto vertex_count=unsigned(pattern[0]>>4),triangle_count=unsigned(pattern[0]&15);std::array<TerrainVertex,12> vertices{};
        for(unsigned i=0;i<vertex_count;++i){const auto edge=tables.edges[code][i];const unsigned a=edge&7,b=(edge>>4)&7;const float t=float(density[a])/float(density[a]-density[b]);
            const auto component=[&](unsigned axis,int cell){const auto ca=(a>>axis)&1,cb=(b>>axis)&1;return float(position[axis]*16)+0.5f+float(cell)+float(ca)+(float(int(cb)-int(ca))*t);};
            vertices[i]={component(0,x),component(1,y),component(2,z)};}
        for(unsigned i=0;i<triangle_count;++i){output.push_back(vertices[pattern[3*i+3]]);output.push_back(vertices[pattern[3*i+2]]);output.push_back(vertices[pattern[3*i+1]]);}
    }
    return output;
}
std::string inspect_terrain_json(const std::filesystem::path& path) {
    sqlite3* raw=nullptr;const auto name=path.u8string();
    const auto code=sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()),&raw,SQLITE_OPEN_READONLY,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    if(code!=SQLITE_OK)throw std::runtime_error("cannot open terrain database");
    sqlite3_stmt* statement=nullptr;
    if(sqlite3_prepare_v2(db.get(),"SELECT id,worldId,x,y,data FROM VoxelTerrain ORDER BY id",-1,&statement,nullptr)!=SQLITE_OK)throw std::runtime_error("cannot read VoxelTerrain table");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(statement,sqlite3_finalize);
    std::uint64_t count=0,bytes=0,changes=0;std::map<unsigned,std::uint64_t> kinds;int step;
    while((step=sqlite3_step(stmt.get()))==SQLITE_ROW) {
        for(int i=0;i<4;++i)if(sqlite3_column_type(stmt.get(),i)!=SQLITE_INTEGER)throw std::runtime_error("invalid terrain row coordinates");
        if(sqlite3_column_type(stmt.get(),4)!=SQLITE_BLOB)throw std::runtime_error("invalid terrain row blob");
        const auto size=sqlite3_column_bytes(stmt.get(),4);const auto* data=static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt.get(),4));
        if(size<=0||!data)throw std::runtime_error("empty terrain row blob");
        const auto chunk=decode_terrain_delta({data,static_cast<std::size_t>(size)});
        if(sqlite3_column_int64(stmt.get(),0)!=chunk.id||sqlite3_column_int64(stmt.get(),1)!=chunk.world_id||sqlite3_column_int64(stmt.get(),2)!=chunk.region_x||sqlite3_column_int64(stmt.get(),3)!=chunk.region_y)throw std::runtime_error("terrain row does not match record header");
        ++count;bytes+=chunk.payload.size();++kinds[chunk.kind];changes+=decode_terrain_density_changes(chunk).size();
    }
    if(step!=SQLITE_DONE)throw std::runtime_error("cannot scan terrain records");
    auto result=std::string("{\"terrain_collision_available\":false,\"requires_generated_base\":true,\"delta_chunks\":")+std::to_string(count)+",\"delta_payload_bytes\":"+std::to_string(bytes)+",\"density_changes\":"+std::to_string(changes)+",\"kinds\":{";
    bool first=true;for(const auto& [kind,total]:kinds){if(!first)result+=',';first=false;result+='"'+std::to_string(kind)+"\":"+std::to_string(total);}return result+"}}";
}
}
