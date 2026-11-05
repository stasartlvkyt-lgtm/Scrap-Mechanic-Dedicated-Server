#include "assets.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace scrapds {
namespace {
std::string text(sqlite3_stmt* s, int c) { const auto* p = sqlite3_column_text(s,c); return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s,c)) : std::string{}; }
std::optional<std::uint32_t> color(const std::string& value) {
    if (value.size() != 6 && value.size() != 8) return {};
    auto digit = [](char c) -> int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
    std::uint32_t result = value.size() == 6 ? 0xff000000 : 0;
    for (std::size_t i=0;i<value.size()/2;++i) { const int a=digit(value[2*i]),b=digit(value[2*i+1]); if(a<0||b<0)return {}; result |= std::uint32_t(a*16+b) << (8*i); }
    return result;
}
std::optional<std::vector<std::array<double,3>>> hull_vertices(const std::filesystem::path& root,const std::string& asset_path,bool allow_rounded_degenerate,bool& degenerate) {
    constexpr std::string_view game="$GAME_DATA/",survival="$SURVIVAL_DATA/";
    std::filesystem::path base;std::string_view tail;
    if(asset_path.starts_with(game)){base=root/"Data";tail=std::string_view(asset_path).substr(game.size());}
    else if(asset_path.starts_with(survival)){base=root/"Survival";tail=std::string_view(asset_path).substr(survival.size());}
    else return {};
    const auto relative=std::filesystem::u8path(tail);if(relative.empty()||relative.is_absolute()||relative.has_root_name())return {};
    for(const auto& part:relative)if(part=="..")return {};
    std::error_code ec;const auto resolved_base=std::filesystem::weakly_canonical(base,ec);if(ec)return {};
    const auto file_path=std::filesystem::weakly_canonical(base/relative,ec);if(ec)return {};
    const auto within=file_path.lexically_relative(resolved_base);if(within.empty()||within.is_absolute())return {};
    for(const auto& part:within)if(part=="..")return {};
    if(file_path.extension()!=".obj"||!std::filesystem::is_regular_file(file_path,ec)||ec||std::filesystem::file_size(file_path,ec)>4*1024*1024||ec)return {};
    std::ifstream file(file_path,std::ios::binary);if(!file)return {};std::vector<std::array<double,3>> out;std::string line;
    while(std::getline(file,line)) {
        if(line.size()>65536)return {};std::istringstream in(line);in.imbue(std::locale::classic());std::string tag;in>>tag;if(tag!="v")continue;
        std::array<double,3> v{};if(!(in>>v[0]>>v[1]>>v[2]))return {};
        for(auto n:v)if(!std::isfinite(n)||std::abs(n)>32767)return {};
        out.push_back(v);if(out.size()>2048)return {};
    }
    if(!file.eof()||out.size()<2)return {};
    // Lines and planes are valid only when the definition explicitly gives a
    // positive rounding margin; this is how the game's small pipes are encoded.
    const auto sub=[](const auto& a,const auto& b){return std::array<double,3>{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
    const auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    std::array<double,3> v{},normal{};bool line_found=false,plane_found=false,volume_found=false;
    for(std::size_t i=1;i<out.size();++i){const auto x=sub(out[i],out[0]);if(!line_found&&dot(x,x)>1e-12){v=x;line_found=true;continue;}
        if(line_found&&!plane_found){normal={v[1]*x[2]-v[2]*x[1],v[2]*x[0]-v[0]*x[2],v[0]*x[1]-v[1]*x[0]};if(dot(normal,normal)>1e-12){plane_found=true;continue;}}
        if(plane_found&&std::abs(dot(normal,x))>1e-9){volume_found=true;break;}}
    if(!line_found||(!volume_found&&!allow_rounded_degenerate))return {};degenerate=!volume_found;return out;
}
}
AssetDefinitions AssetDefinitions::load(const std::filesystem::path& root) {
    AssetDefinitions result;
    sqlite3* raw = nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK) { if(raw)sqlite3_close(raw); throw std::runtime_error("Cannot initialize asset JSON reader"); }
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    for (const auto* branch : {"Data", "Survival"}) {
        const auto directory = root / branch / "Objects" / "Database" / "ShapeSets";
        if (!std::filesystem::is_directory(directory)) continue;
        std::vector<std::filesystem::path> paths;
        for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) if(item.is_regular_file() && item.path().extension()==".shapeset")paths.push_back(item.path());
        std::sort(paths.begin(),paths.end());
        for (const auto& path : paths) {
            if(std::filesystem::file_size(path)>16*1024*1024)throw std::runtime_error("Shape definition file exceeds 16 MiB");
            std::ifstream file(path,std::ios::binary);
            if(!file)throw std::runtime_error("Cannot read shape definitions");
            std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
            for (const auto* list : {"$.blockList", "$.partList"}) {
                sqlite3_stmt* statement=nullptr;
                const char* query="SELECT json_extract(value,'$.uuid'),json_extract(value,'$.name'),json_extract(value,'$.color'),json_extract(value,'$.physicsMaterial'),json_extract(value,'$.box.x'),json_extract(value,'$.box.y'),json_extract(value,'$.box.z'),json_type(value,'$.hull'),json_extract(value,'$.cylinder.axis'),json_extract(value,'$.cylinder.diameter'),json_extract(value,'$.cylinder.depth'),json_extract(value,'$.hull.x'),json_extract(value,'$.hull.y'),json_extract(value,'$.hull.z'),json_extract(value,'$.hull.col'),json_extract(value,'$.hull.margin'),json_type(value,'$.box'),json_type(value,'$.cylinder'),json_extract(value,'$.survivalSpring.length'),json_type(value,'$.survivalSpring.stiffnessLevels') FROM json_each(?1,?2)";
                const auto prepared=sqlite3_prepare_v2(db.get(),query,-1,&statement,nullptr);
                std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> s(statement,sqlite3_finalize);
                if(prepared!=SQLITE_OK)throw std::runtime_error("SQLite JSON reader is unavailable");
                sqlite3_bind_text64(s.get(),1,source.data(),source.size(),SQLITE_TRANSIENT,SQLITE_UTF8);
                sqlite3_bind_text(s.get(),2,list,-1,SQLITE_STATIC);
                int status;
                while((status=sqlite3_step(s.get()))==SQLITE_ROW) {
                    AssetDefinition asset{text(s.get(),0),text(s.get(),1),text(s.get(),3),std::string_view(list)=="$.blockList",color(text(s.get(),2))};
                    std::array<double,3> dimensions{};bool valid_box=true;
                    for(int axis=0;axis<3;++axis){const auto type=sqlite3_column_type(s.get(),4+axis);if(type!=SQLITE_INTEGER&&type!=SQLITE_FLOAT){valid_box=false;break;}dimensions[axis]=sqlite3_column_double(s.get(),4+axis);if(!std::isfinite(dimensions[axis])||dimensions[axis]<=0||dimensions[axis]>32767){valid_box=false;break;}}
                    const bool hull=sqlite3_column_type(s.get(),7)!=SQLITE_NULL;
                    if(valid_box&&!hull)asset.box_dimensions=dimensions;
                    const auto axis=text(s.get(),8);const auto diameter_type=sqlite3_column_type(s.get(),9),depth_type=sqlite3_column_type(s.get(),10);
                    if(!hull&&(axis=="X"||axis=="Y"||axis=="Z")&&(diameter_type==SQLITE_INTEGER||diameter_type==SQLITE_FLOAT)&&(depth_type==SQLITE_INTEGER||depth_type==SQLITE_FLOAT)) {
                        const auto diameter=sqlite3_column_double(s.get(),9),depth=sqlite3_column_double(s.get(),10);
                        if(std::isfinite(diameter)&&std::isfinite(depth)&&diameter>0&&depth>0&&diameter<=32767&&depth<=32767)
                            asset.cylinder_dimensions=CylinderDimensions{diameter,depth,std::uint8_t(axis=="X"?0:axis=="Y"?1:2)};
                    }
                    if(hull&&sqlite3_column_type(s.get(),16)==SQLITE_NULL&&sqlite3_column_type(s.get(),17)==SQLITE_NULL) {
                        HullGeometry geometry;bool valid=true;
                        for(int i=0;i<3;++i){const auto type=sqlite3_column_type(s.get(),11+i);geometry.dimensions[i]=sqlite3_column_double(s.get(),11+i);if((type!=SQLITE_INTEGER&&type!=SQLITE_FLOAT)||!std::isfinite(geometry.dimensions[i])||geometry.dimensions[i]<=0||geometry.dimensions[i]>32767)valid=false;}
                        // Explicit asset margins use the same quarter-metre grid
                        // units as OBJ vertices; omitted margin uses Bullet's default.
                        const auto type=sqlite3_column_type(s.get(),15);if(type!=SQLITE_NULL){if(type!=SQLITE_INTEGER&&type!=SQLITE_FLOAT)valid=false;else geometry.margin=sqlite3_column_double(s.get(),15)*0.25;}
                        if(!std::isfinite(geometry.margin)||geometry.margin<0||geometry.margin>1)valid=false;
                        if(valid)if(auto points=hull_vertices(root,text(s.get(),14),type!=SQLITE_NULL&&geometry.margin>0,geometry.degenerate)){geometry.vertices=std::move(*points);asset.hull_geometry=std::move(geometry);}
                    }
                    if(sqlite3_column_type(s.get(),18)==SQLITE_INTEGER&&sqlite3_column_type(s.get(),19)==SQLITE_NULL) {
                        const auto length=sqlite3_column_int64(s.get(),18);if(length>=1&&length<=32)asset.survival_spring_length=std::uint32_t(length);
                    }
                    if(asset.uuid.size()!=36)throw std::runtime_error("Invalid shape UUID in asset definitions");
                    result.entries_.insert_or_assign(asset.uuid,std::move(asset));
                }
                if(status!=SQLITE_DONE)throw std::runtime_error("Invalid shape definition JSON: "+std::string(sqlite3_errmsg(db.get())));
            }
        }
    }
    if(result.entries_.empty())throw std::runtime_error("No Data/Survival shape definitions found");
    return result;
}
const AssetDefinition* AssetDefinitions::find(const std::string& uuid) const { const auto it=entries_.find(uuid); return it==entries_.end()?nullptr:&it->second; }
}
