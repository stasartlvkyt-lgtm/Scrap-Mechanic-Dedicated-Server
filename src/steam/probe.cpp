#include "probe.hpp"
#include "inventory.hpp"
#include "core/json.hpp"
#include "net/codec.hpp"
#include "net/character.hpp"
#include "net/player_info.hpp"
#include "net/control.hpp"
#include "net/delivery.hpp"
#include "net/world_sync.hpp"
#include "data/state.hpp"
#include "data/assets.hpp"
#include "data/shape_geometry.hpp"
#include "game/commands.hpp"
#include "game/replication.hpp"
#include "game/player_spawn.hpp"
#include "physics/world_physics.hpp"
#include "physics/world_collision.hpp"
#include "physics/overworld_streamer.hpp"
#include <lz4.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <io.h>

namespace scrapds {
namespace {
using Clock = std::chrono::steady_clock;
constexpr const char* expected_exe = "cf403cf9d3e465e05298c8e6082f4d664c32140ebcc4696a64200e367abb530d";
constexpr const char* expected_dll = "4df999c0c8cb12589f0864d52be5d4c775577aeb27fee28b49b188f9ba083eea";
// Saved Container UUID byte order is the reverse of the canonical asset UUID.
constexpr std::array<std::uint8_t,16> paint_tool_item_saved_uuid{
    0x5c,0x98,0xf9,0x76,0x80,0x12,0x20,0xa6,0x9d,0x43,0xe7,0x7a,0x84,0x6a,0x1c,0x73};
using Handle = std::uint32_t;

struct CharacterMotionState {
    std::uint16_t world_id{};
    Vec3 position{},velocity{};
    std::uint32_t client_tick{};
    bool initialized{},has_client_tick{},grounded{};
};
struct CharacterPhysicsProfile {
    double capsule_radius{0.3};
    double capsule_height{1.4};
    double crouch_height{0.8};
    double move_speed{1.4575};
    double sprint_speed{4.0};
    double crouch_speed{3.0};
    double slow_down_speed{2.5};
    double jump_impulse{7.0};
    double maximum_horizontal_speed{6.0};
    bool swim_supported{};
};
struct CharacterCapsuleResult {
    Vec3 position{};
    std::uint32_t contacts{};
};

// The SDK writes initialization diagnostics to stdout through both CRT and
// native handles. Keep public-command stdout parseable as a single JSON value.
// This guard must outlive Session so shutdown diagnostics are redirected too.
struct SdkOutputRedirect {
    int saved_descriptor=-1;
    HANDLE saved_handle=INVALID_HANDLE_VALUE;
    HANDLE original_crt_handle=INVALID_HANDLE_VALUE;
    bool descriptor_redirected{},handle_redirected{};
    SdkOutputRedirect(){
        std::cout.flush();std::fflush(stdout);
        saved_handle=GetStdHandle(STD_OUTPUT_HANDLE);
        original_crt_handle=reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
        saved_descriptor=_dup(_fileno(stdout));
        if(saved_descriptor<0)throw std::runtime_error("Cannot preserve stdout for Steam SDK diagnostics");
        if(_dup2(_fileno(stderr),_fileno(stdout))!=0){restore();throw std::runtime_error("Cannot redirect Steam SDK CRT diagnostics to stderr");}
        descriptor_redirected=true;
        handle_redirected=true;
        if(!SetStdHandle(STD_OUTPUT_HANDLE,GetStdHandle(STD_ERROR_HANDLE))){restore();throw std::runtime_error("Cannot redirect Steam SDK native diagnostics to stderr");}
    }
    void restore()noexcept{
        std::fflush(stdout);
        if(descriptor_redirected){_dup2(saved_descriptor,_fileno(stdout));descriptor_redirected=false;}
        if(saved_descriptor>=0){_close(saved_descriptor);saved_descriptor=-1;}
        // _dup2 closes the old CRT HANDLE and creates a fresh duplicate when
        // restoring. If the OS standard handle pointed at that CRT handle,
        // restore the new valid equivalent rather than a closed numeric value.
        if(handle_redirected){const auto target=saved_handle==original_crt_handle?reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))):saved_handle;SetStdHandle(STD_OUTPUT_HANDLE,target);handle_redirected=false;}
    }
    ~SdkOutputRedirect(){restore();}
};

std::string path_utf8(const std::filesystem::path& path) {
    const auto str = path.u8string();
    return {reinterpret_cast<const char*>(str.data()), str.size()};
}
std::string array_json(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i=0;i<values.size();++i) { if(i)out+=',';out+=values[i]; }
    return out+']';
}
std::string elapsed(Clock::time_point start) {
    std::ostringstream out;out<<std::fixed<<std::setprecision(3)<<std::chrono::duration<double>(Clock::now()-start).count();return out.str();
}
std::string sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input)throw std::runtime_error("Cannot open game file for SHA256: "+path_utf8(path));
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider unavailable");
    struct CloseAlgorithm { BCRYPT_ALG_HANDLE p;~CloseAlgorithm(){BCryptCloseAlgorithmProvider(p,0);} } close_algorithm{algorithm};
    if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)throw std::runtime_error("Cannot create SHA256 hash");
    struct CloseHash { BCRYPT_HASH_HANDLE p;~CloseHash(){BCryptDestroyHash(p);} } close_hash{hash};
    std::array<unsigned char,65536> buffer{};
    while(input){input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());const auto count=input.gcount();if(count&&BCryptHashData(hash,buffer.data(),static_cast<ULONG>(count),0)<0)throw std::runtime_error("SHA256 update failed");}
    if(!input.eof())throw std::runtime_error("Game file read failed");
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)throw std::runtime_error("SHA256 finish failed");
    std::ostringstream out;out<<std::hex<<std::setfill('0');for(auto byte:digest)out<<std::setw(2)<<unsigned(byte);return out.str();
}
std::filesystem::path game_dll(std::filesystem::path game) {
    game=std::filesystem::canonical(game);
    if(std::filesystem::is_regular_file(game))game=game.parent_path();
    if(!std::filesystem::exists(game/L"steam_api64.dll"))game/=L"Release";
    const auto exe=game/L"ScrapMechanic.exe",dll=game/L"steam_api64.dll";
    if(sha256(exe)!=expected_exe)throw std::runtime_error("Unsupported ScrapMechanic.exe SHA256; native bindings are locked to the documented build");
    if(sha256(dll)!=expected_dll)throw std::runtime_error("Unsupported steam_api64.dll SHA256; refusing unverified native ABI");
    return dll;
}
struct Environment {
    std::vector<std::pair<std::wstring,std::optional<std::wstring>>> old;
    Environment(){
        try {for(const wchar_t* name:{L"SteamAppId",L"SteamGameId"}){
            SetLastError(ERROR_SUCCESS);const DWORD size=GetEnvironmentVariableW(name,nullptr,0);
            std::optional<std::wstring> value;
            if(size){std::wstring buffer(size,L'\0');const DWORD got=GetEnvironmentVariableW(name,buffer.data(),size);if(!got)throw std::runtime_error("Cannot read process environment");buffer.resize(got);value=std::move(buffer);}
            else if(GetLastError()!=ERROR_ENVVAR_NOT_FOUND)value=L"";
            old.emplace_back(name,std::move(value));
            if(!SetEnvironmentVariableW(name,L"387990"))throw std::runtime_error("Cannot set process AppID environment");
        }}catch(...){restore();throw;}
    }
    void restore()noexcept{for(const auto& [name,value]:old)SetEnvironmentVariableW(name.c_str(),value?value->c_str():nullptr);old.clear();}
    ~Environment(){restore();}
};
struct Library {
    HMODULE module{};
    explicit Library(const std::filesystem::path& path){module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)throw std::runtime_error("Cannot load installed Steam DLL: Windows error "+std::to_string(GetLastError()));}
    ~Library(){if(module)FreeLibrary(module);}
    template<typename T>T get(const char* name){const auto address=GetProcAddress(module,name);if(!address)throw std::runtime_error(std::string("Missing verified Steam export: ")+name);return reinterpret_cast<T>(address);}
};

// Own flat C ABI bindings; layouts cross-checked against Valve SDK1.52
// (GameServer014) and SDK1.54 (SteamNetworkingSockets012). No SDK redistribution.
struct Callback { std::int32_t user,id;void* data;std::int32_t size; };
#pragma pack(push,1)
struct Identity {std::int32_t type,size;std::uint8_t data[128];std::uint64_t steam_id()const{std::uint64_t value{};if(type==16&&size==8)std::memcpy(&value,data,8);return value;}};
struct IPAddress {std::uint8_t ipv6[16];std::uint16_t port;};
#pragma pack(pop)
struct Info {Identity remote;std::int64_t user_data;Handle listen;IPAddress address;std::uint16_t pad;std::uint32_t remote_pop,relay_pop;std::int32_t state,end_reason;char end_debug[128],description[128];std::int32_t flags;std::uint32_t reserved[63];};
struct Status {Handle connection;Info info;std::int32_t old_state;};
struct Authentication {std::int32_t availability;char debug[256];};
struct Message {void* data;std::int32_t size;Handle connection;Identity peer;std::int64_t connection_data,time_received,number;void* free_data;void* release;std::int32_t channel,flags;std::int64_t user_data;std::uint16_t lane,pad;};
static_assert(sizeof(void*)==8,"Steam probe is Windows x64 only");
static_assert(sizeof(Callback)==24&&offsetof(Callback,data)==8&&offsetof(Callback,size)==16);
static_assert(sizeof(Identity)==136&&sizeof(IPAddress)==18);
static_assert(sizeof(Info)==696&&offsetof(Info,state)==176&&offsetof(Info,flags)==440);
static_assert(sizeof(Status)==712&&offsetof(Status,info)==8&&offsetof(Status,old_state)==704);
static_assert(sizeof(Message)==216&&offsetof(Message,release)==184&&offsetof(Message,flags)==196);
static_assert(sizeof(Authentication)==260);
template<std::size_t N>std::string bounded(const char(&value)[N]){const auto end=std::find(value,value+N,'\0');return {value,end};}

struct API {
    bool(*init)(std::uint32_t,std::uint16_t,std::uint16_t,std::uint16_t,int,const char*);
    void*(*server)();void(*shutdown)();
    void(*product)(void*,const char*);void(*description)(void*,const char*);void(*moddir)(void*,const char*);
    void(*dedicated)(void*,bool);void(*advertise)(void*,bool);void(*anonymous)(void*);void(*logoff)(void*);
    bool(*logged)(void*);std::uint64_t(*steam_id)(void*);
    void(*manual_init)();std::int32_t(*get_pipe)();std::int32_t(*get_user)();
    void(*frame)(std::int32_t);bool(*next)(std::int32_t,Callback*);void(*free_callback)(std::int32_t);
    void*(*interface_for_server)(std::int32_t,const char*);
    void*(*server_inventory)();
    bool(*inventory_deserialize)(void*,std::int32_t*,const void*,std::uint32_t,bool);
    int(*inventory_status)(void*,std::int32_t);
    bool(*inventory_check_owner)(void*,std::int32_t,std::uint64_t);
    void(*inventory_destroy)(void*,std::int32_t);
    explicit API(Library& lib){
#define BIND(member,name) member=lib.get<decltype(member)>(name)
        BIND(init,"SteamInternal_GameServer_Init");BIND(server,"SteamAPI_SteamGameServer_v014");BIND(shutdown,"SteamGameServer_Shutdown");
        BIND(product,"SteamAPI_ISteamGameServer_SetProduct");BIND(description,"SteamAPI_ISteamGameServer_SetGameDescription");BIND(moddir,"SteamAPI_ISteamGameServer_SetModDir");
        BIND(dedicated,"SteamAPI_ISteamGameServer_SetDedicatedServer");BIND(advertise,"SteamAPI_ISteamGameServer_SetAdvertiseServerActive");BIND(anonymous,"SteamAPI_ISteamGameServer_LogOnAnonymous");BIND(logoff,"SteamAPI_ISteamGameServer_LogOff");
        BIND(logged,"SteamAPI_ISteamGameServer_BLoggedOn");BIND(steam_id,"SteamAPI_ISteamGameServer_GetSteamID");
        BIND(manual_init,"SteamAPI_ManualDispatch_Init");BIND(get_pipe,"SteamGameServer_GetHSteamPipe");BIND(get_user,"SteamGameServer_GetHSteamUser");
        BIND(frame,"SteamAPI_ManualDispatch_RunFrame");BIND(next,"SteamAPI_ManualDispatch_GetNextCallback");BIND(free_callback,"SteamAPI_ManualDispatch_FreeLastCallback");BIND(interface_for_server,"SteamInternal_FindOrCreateGameServerInterface");
        BIND(server_inventory,"SteamAPI_SteamGameServerInventory_v003");BIND(inventory_deserialize,"SteamAPI_ISteamInventory_DeserializeResult");BIND(inventory_status,"SteamAPI_ISteamInventory_GetResultStatus");BIND(inventory_check_owner,"SteamAPI_ISteamInventory_CheckResultSteamID");BIND(inventory_destroy,"SteamAPI_ISteamInventory_DestroyResult");
#undef BIND
    }
};
struct Session {
    std::filesystem::path dll;Environment environment;Library library;API api;
    void* server{};std::int32_t pipe{};bool initialized{},logged{},advertise_requested{},advertising{};std::uint64_t id{};
    Clock::time_point start=Clock::now();std::vector<std::string> callbacks;
    explicit Session(const std::filesystem::path& game,bool publish=false):dll(game_dll(game)),library(dll),api(library),advertise_requested(publish){}
    ~Session(){if(server){if(advertising)api.advertise(server,false);api.logoff(server);}if(initialized)api.shutdown();}
    void login(int seconds){
        initialized=api.init(0x7f000001,0,27665,27666,2,"1.0.7");
        if(!initialized)return;
        server=api.server();if(!server)throw std::runtime_error("Null GameServer014 interface");
        api.product(server,"387990");api.description(server,"Scrap Mechanic");api.moddir(server,"Scrap Mechanic");api.dedicated(server,true);api.advertise(server,false);
        api.manual_init();pipe=api.get_pipe();if(!pipe)throw std::runtime_error("Null GameServer callback pipe");api.anonymous(server);
        const auto deadline=Clock::now()+std::chrono::seconds(seconds);
        while(Clock::now()<deadline){
            api.frame(pipe);Callback callback{};
            for(int i=0;i<1000&&api.next(pipe,&callback);++i){
                std::string value="{\"id\":"+std::to_string(callback.id)+",\"bytes\":"+std::to_string(callback.size)+",\"elapsed_seconds\":"+elapsed(start);
                if((callback.id==102||callback.id==103)&&callback.data&&callback.size>=4&&callback.size<=64){std::int32_t result{};std::memcpy(&result,callback.data,4);value+=",\"eresult\":"+std::to_string(result);}
                value+='}';if(callbacks.size()<100)callbacks.push_back(std::move(value));api.free_callback(pipe);
            }
            logged=api.logged(server);id=api.steam_id(server);if(logged&&id){if(advertise_requested){api.advertise(server,true);advertising=true;}return;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::string json()const{return "{\"format\":\"scrapds.native-steam-identity.v1\",\"status\":"+json_quote(logged?"logged_on":initialized?"login_timeout":"initialization_failed")+",\"app_id\":387990,\"dll\":"+json_quote(path_utf8(dll))+",\"dll_sha256\":"+json_quote(expected_dll)+",\"exe_sha256\":"+json_quote(expected_exe)+",\"logged_on\":"+(logged?"true":"false")+",\"steam_id\":"+json_quote(std::to_string(id))+",\"steam_account_type\":"+std::to_string((id>>52)&15)+",\"universe\":"+std::to_string(id>>56)+",\"game_port\":27665,\"query_port\":27666,\"advertising\":"+(advertising?"true":"false")+",\"callbacks\":"+array_json(callbacks)+",\"elapsed_seconds\":"+elapsed(start)+'}';}
};

struct Networking {
    void* sockets{};
    Handle(*listen)(void*,int,int,const void*);bool(*close_listener)(void*,Handle);
    Handle(*group)(void*);bool(*destroy_group)(void*,Handle);bool(*set_group)(void*,Handle,Handle);
    int(*accept)(void*,Handle);bool(*close_connection)(void*,Handle,int,const char*,bool);
    bool(*info)(void*,Handle,Info*);bool(*identity)(void*,Identity*);
    int(*init_auth)(void*);int(*auth)(void*,Authentication*);
    int(*receive)(void*,Handle,Message**,int);void(*release)(Message*);
    int(*send)(void*,Handle,const void*,std::uint32_t,int,std::int64_t*);int(*flush)(void*,Handle);
    explicit Networking(Session& session){
        auto& lib=session.library;
#define BIND(member,name) member=lib.get<decltype(member)>(name)
        BIND(listen,"SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");BIND(close_listener,"SteamAPI_ISteamNetworkingSockets_CloseListenSocket");BIND(group,"SteamAPI_ISteamNetworkingSockets_CreatePollGroup");BIND(destroy_group,"SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");BIND(set_group,"SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
        BIND(accept,"SteamAPI_ISteamNetworkingSockets_AcceptConnection");BIND(close_connection,"SteamAPI_ISteamNetworkingSockets_CloseConnection");BIND(info,"SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");BIND(identity,"SteamAPI_ISteamNetworkingSockets_GetIdentity");BIND(init_auth,"SteamAPI_ISteamNetworkingSockets_InitAuthentication");BIND(auth,"SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");BIND(receive,"SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");BIND(release,"SteamAPI_SteamNetworkingMessage_t_Release");
        BIND(send,"SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");BIND(flush,"SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection");
#undef BIND
        sockets=session.api.interface_for_server(session.api.get_user(),"SteamNetworkingSockets012");if(!sockets)throw std::runtime_error("No GameServer SNS012 interface");
        Identity own{};if(!identity(sockets,&own)||own.steam_id()!=session.id)throw std::runtime_error("SNS identity differs from anonymous server");
    }
};
void write_file(const std::filesystem::path& path,const std::string& text){std::ofstream output(path,std::ios::binary|std::ios::trunc);if(!output)throw std::runtime_error("Cannot write probe output: "+path_utf8(path));output<<text<<'\n';output.close();if(!output)throw std::runtime_error("Cannot finish probe output");}
void write_binary(const std::filesystem::path& path,const std::vector<std::uint8_t>& bytes){std::ofstream output(path,std::ios::binary|std::ios::trunc);if(!output)throw std::runtime_error("Cannot write asset catalog: "+path_utf8(path));output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));output.close();if(!output)throw std::runtime_error("Cannot finish asset catalog");}
// Minimum GameInfo payload accepted by the observed client parser: build 889,
// no UGC, no name, empty payload lists, final flag clear. Multi-byte fields use
// network byte order; the client byte-swaps them on little-endian hosts. The
// transport wrapper compresses everything after message ID 2.
std::vector<std::uint8_t> make_minimal_game_info(){
    std::vector<std::uint8_t> packet;packet.reserve(40);
    auto u32=[&](std::uint32_t v){for(int i=3;i>=0;--i)packet.push_back(static_cast<std::uint8_t>(v>>(8*i)));};
    auto u16=[&](std::uint16_t v){for(int i=1;i>=0;--i)packet.push_back(static_cast<std::uint8_t>(v>>(8*i)));};
    packet.push_back(2);
    u32(889);u32(0);u32(0);u32(0);
    u32(0);
    u16(0);
    u32(0);
    u32(0);
    packet.push_back(0);
    return packet;
}
struct ReplayPackets {
    std::vector<std::uint8_t> game_info, init11, init13, world29;
    std::vector<std::vector<std::uint8_t>> post;
};
std::vector<std::uint8_t> read_binary(const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot open replay packet: "+path_utf8(path));
    const auto size=input.tellg();
    if(size<0||size>64*1024*1024)throw std::runtime_error("Invalid replay packet size: "+path_utf8(path));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.seekg(0);
    if(!data.empty()&&!input.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size())))throw std::runtime_error("Cannot read replay packet: "+path_utf8(path));
    return data;
}
ReplayPackets load_replay(const std::filesystem::path& directory){
    ReplayPackets replay;
    replay.game_info=read_binary(directory/L"gameinfo.bin");
    replay.init11=read_binary(directory/L"init11.bin");
    replay.init13=read_binary(directory/L"init13.bin");
    replay.world29=read_binary(directory/L"world29.bin");
    if(replay.game_info.empty()||replay.game_info[0]!=2||replay.init11.empty()||replay.init11[0]!=11||
       replay.init13.empty()||replay.init13[0]!=13||replay.world29.empty()||replay.world29[0]!=29)
        throw std::runtime_error("Replay packet identifiers do not match GameInfo/11/13/29");
    const auto post_directory=directory/L"post";
    if(std::filesystem::is_directory(post_directory)){
        std::vector<std::filesystem::path> paths;
        for(const auto& entry:std::filesystem::directory_iterator(post_directory))
            if(entry.is_regular_file()&&entry.path().extension()==L".bin")paths.push_back(entry.path());
        std::sort(paths.begin(),paths.end());
        for(std::size_t i=0;i<paths.size()&&i<64;++i)replay.post.push_back(read_binary(paths[i]));
    }
    return replay;
}
using BlobUid=std::array<std::uint8_t,16>;
constexpr BlobUid terrain_data_uid{0x61,0xaa,0x13,0xd7,0xe7,0x15,0x41,0x53,0xa2,0x69,0x4d,0x33,0x8c,0x0c,0x5b,0xd4};
constexpr BlobUid generic_world_runtime_uid{0x3f,0xf3,0x6c,0x8b,0x93,0xf7,0x44,0x28,0xae,0x4d,0x42,0x89,0xa6,0xf0,0xcf,0x77};
constexpr BlobUid generic_world_saved_uid{0x52,0x97,0x76,0x9d,0xf4,0x51,0x4e,0x5e,0x9a,0x38,0x8b,0x0f,0xb9,0x5e,0x2e,0xda};
constexpr BlobUid generic_flat_uid{0x44,0xac,0x02,0x0c,0xae,0xc7,0x4f,0x8b,0xb2,0x30,0x34,0xd2,0xe3,0xbd,0x23,0xeb};
constexpr BlobUid script_runtime_uid_a{0xea,0x4a,0x35,0x01,0xc8,0xd5,0x5d,0x13,0xb0,0x74,0x49,0x8f,0x39,0x2d,0x5d,0x7e};
constexpr BlobUid script_runtime_uid_b{0xc5,0xb8,0x04,0xf5,0x68,0x30,0x5f,0xf7,0x94,0x73,0x67,0x63,0x0e,0x1c,0x59,0x84};
constexpr BlobUid script_saved_uid{0x51,0x30,0xc4,0x7b,0x0a,0x19,0x53,0xfc,0xa5,0xa1,0x20,0x5c,0x19,0x26,0x45,0x74};
constexpr std::uint32_t captured_character_default_color=UINT32_MAX;
class ScriptTableKeyReader {
public:
    explicit ScriptTableKeyReader(std::span<const std::uint8_t> bytes):bytes_(bytes) {
        constexpr std::array<std::uint8_t,7> magic{'L','U','A',0,0,0,1};
        if(bytes.size()<magic.size()||!std::equal(magic.begin(),magic.end(),bytes.begin()))throw std::runtime_error("unsupported ScriptData LUA payload");
    }
    std::set<std::string> root_keys() {
        if(bits(8)!=5)throw std::runtime_error("ScriptData root is not a table");
        const auto count=bits(32),array=bits(1);
        if(count>100000)throw std::runtime_error("ScriptData root table is too large");
        std::set<std::string> result;
        if(array) { bits(32);for(std::uint64_t i=0;i<count;++i)skip_value(1);return result; }
        for(std::uint64_t i=0;i<count;++i) {
            const auto type=static_cast<std::uint8_t>(bits(8));
            if(type==4)result.insert(read_string());else skip_body(type,1);
            skip_value(1);
        }
        return result;
    }
private:
    std::uint64_t bits(unsigned count) {
        if(count>64||bit_>bytes_.size()*8||count>bytes_.size()*8-bit_)throw std::runtime_error("truncated ScriptData LUA payload");
        std::uint64_t value=0;for(unsigned i=0;i<count;++i,++bit_)value=(value<<1)|((bytes_[bit_/8]>>(7-(bit_%8)))&1u);return value;
    }
    std::string read_string() {
        const auto size=bits(32);if(size>1024*1024)throw std::runtime_error("oversized ScriptData LUA string");
        bit_=(bit_+7)&~std::size_t(7);const auto offset=bit_/8;
        if(offset>bytes_.size()||size>bytes_.size()-offset)throw std::runtime_error("truncated ScriptData LUA string");
        bit_+=std::size_t(size)*8;return std::string(reinterpret_cast<const char*>(bytes_.data()+offset),std::size_t(size));
    }
    void skip_value(unsigned depth) {if(++nodes_>1000000)throw std::runtime_error("ScriptData LUA node limit exceeded");skip_body(static_cast<std::uint8_t>(bits(8)),depth);}
    void skip_body(std::uint8_t type,unsigned depth) {
        if(depth>16)throw std::runtime_error("ScriptData LUA nesting limit exceeded");
        switch(type) {
        case 1: return;
        case 2: bits(1);return;
        case 3: bits(32);return;
        case 4: (void)read_string();return;
        case 5: {
            const auto count=bits(32),array=bits(1);if(count>100000)throw std::runtime_error("ScriptData LUA table is too large");
            if(array) {bits(32);for(std::uint64_t i=0;i<count;++i)skip_value(depth+1);}
            else for(std::uint64_t i=0;i<count;++i){skip_value(depth+1);skip_value(depth+1);}
            return;
        }
        case 6: bits(32);return;
        case 7: bits(16);return;
        case 8: bits(8);return;
        case 11: bits(64);return;
        case 100: if(bits(32)!=10001)throw std::runtime_error("unsupported ScriptData LUA native class");bits(128);return;
        default: throw std::runtime_error("unknown ScriptData LUA value tag");
        }
    }
    std::span<const std::uint8_t> bytes_;std::size_t bit_{56},nodes_{};
};
std::set<std::string> script_root_keys(std::span<const std::uint8_t> compressed) {
    if(compressed.empty()||compressed.size()>1024*1024)return {};
    std::vector<std::uint8_t> decoded(1024*1024);
    const auto size=LZ4_decompress_safe(reinterpret_cast<const char*>(compressed.data()),reinterpret_cast<char*>(decoded.data()),int(compressed.size()),int(decoded.size()));
    if(size<0)return {};decoded.resize(std::size_t(size));
    try{return ScriptTableKeyReader(decoded).root_keys();}catch(const std::runtime_error&){return {};}
}
struct BlobUidMapping { BlobUid runtime_uid,saved_uid; std::set<std::string> saved_root_keys; };
std::vector<std::uint8_t> rebuild_mapped_blob_packet(const std::vector<std::uint8_t>& captured,
                                                     const scrapds::WorldState& world,
                                                     std::uint32_t tick,
                                                     std::span<const BlobUidMapping> mappings) {
    auto packet=net::parse_blob_data_packet(captured);
    std::vector<std::optional<std::uint8_t>> runtime_flags(mappings.size());
    std::vector<std::set<std::vector<std::uint8_t>>> runtime_keys(mappings.size());
    for(std::size_t i=0;i<mappings.size();++i)for(const auto& entry:packet.entries)if(entry.uid==mappings[i].runtime_uid){
        if(runtime_flags[i]&&*runtime_flags[i]!=entry.flags)throw std::runtime_error("conflicting runtime BlobData flags for mapped UUID");
        runtime_flags[i]=entry.flags;
        runtime_keys[i].insert(entry.key);
    }
    if(std::none_of(runtime_flags.begin(),runtime_flags.end(),[](const auto& flags){return flags.has_value();})) {
        packet.tick=tick;return net::make_blob_data_packet(packet);
    }
    std::vector<scrapds::StoredBlobDataEntry> saved;
    if(packet.message_id==11)saved=world.terrain_generator_entries();
    else if(packet.message_id==25) {
        std::set<BlobUid> saved_uids;for(const auto& mapping:mappings)saved_uids.insert(mapping.saved_uid);
        for(const auto& uid:saved_uids) {
            auto rows=world.script_data_entries(uid);
            saved.insert(saved.end(),std::make_move_iterator(rows.begin()),std::make_move_iterator(rows.end()));
        }
    } else saved=world.generic_data_entries();
    std::vector<std::vector<const scrapds::StoredBlobDataEntry*>> replacements(mappings.size());
    for(const auto& item:saved) {
        if(packet.message_id!=25) {
            for(std::size_t i=0;i<mappings.size();++i)if(runtime_flags[i]&&item.uid==mappings[i].saved_uid)replacements[i].push_back(&item);
            continue;
        }
        std::vector<std::size_t> key_owners;
        for(std::size_t i=0;i<mappings.size();++i)if(runtime_flags[i]&&item.uid==mappings[i].saved_uid&&runtime_keys[i].contains(item.key))key_owners.push_back(i);
        if(key_owners.size()==1) {replacements[key_owners.front()].push_back(&item);continue;}
        if(key_owners.size()>1)continue;
        const auto fields=script_root_keys(item.payload);std::vector<std::size_t> field_owners;
        for(std::size_t i=0;i<mappings.size();++i)if(runtime_flags[i]&&item.uid==mappings[i].saved_uid&&!mappings[i].saved_root_keys.empty()&&
            std::any_of(mappings[i].saved_root_keys.begin(),mappings[i].saved_root_keys.end(),[&](const auto& key){return fields.contains(key);}))field_owners.push_back(i);
        if(field_owners.size()==1)replacements[field_owners.front()].push_back(&item);
    }
    if(packet.message_id==25)for(auto& items:replacements) {
        std::set<std::vector<std::uint8_t>> matched_keys;
        for(const auto* item:items)matched_keys.insert(item->key);
        if(matched_keys.size()<3)items.clear();
    }
    std::vector<net::BlobDataEntry> entries;entries.reserve(packet.entries.size()+saved.size());
    for(const auto& entry:packet.entries){
        bool replaced=false;
        for(std::size_t i=0;i<mappings.size();++i)if(runtime_flags[i]&&!replacements[i].empty()&&entry.uid==mappings[i].runtime_uid){replaced=true;break;}
        if(!replaced)entries.push_back(entry);
    }
    for(std::size_t i=0;i<mappings.size();++i)if(runtime_flags[i])for(const auto* item:replacements[i])
        entries.push_back({mappings[i].runtime_uid,item->key,item->world_id,*runtime_flags[i],item->payload});
    packet.tick=tick;packet.entries=std::move(entries);
    return net::make_blob_data_packet(packet);
}
} // namespace

std::string probe_steam_identity(const std::filesystem::path& game,int seconds){
    if(seconds<1||seconds>30)throw std::runtime_error("Identity timeout must be 1..30 seconds");
    SdkOutputRedirect sdk_output;
    Session session(game);session.login(seconds);return session.json();
}

std::string run_steam_listener(const std::filesystem::path& game,const std::filesystem::path& output,std::uint64_t allow_peer,std::uint64_t allow_peer2,int seconds,bool handshake,bool game_ready,const std::filesystem::path& replay_dir,const std::filesystem::path& world_path,bool public_server,bool native_players,bool native_physics){
    if(seconds<0||seconds>86400)throw std::runtime_error("Listener duration must be 0..86400 seconds");
    auto valid_peer_id=[](std::uint64_t id){return id&&((id>>52)&15)==1&&(id>>56)==1;};
    if(!valid_peer_id(allow_peer)&&!(public_server&&allow_peer==0))throw std::runtime_error("A private server requires an explicit public-universe individual Steam peer ID");
    if(allow_peer&&!valid_peer_id(allow_peer))throw std::runtime_error("The allowed peer must be a public-universe individual Steam ID");
    if(allow_peer2&&(((allow_peer2>>52)&15)!=1||(allow_peer2>>56)!=1))throw std::runtime_error("The second allowed peer must be a public-universe individual Steam ID");
    auto allowed_peer=[&](std::uint64_t id){return valid_peer_id(id)&&(public_server||id==allow_peer||(allow_peer2&&id==allow_peer2));};
    if(!world_path.empty()&&replay_dir.empty())throw std::runtime_error("Native world currently requires a bootstrap directory for Lua/terrain metadata");
    if(native_players&&(world_path.empty()||replay_dir.empty()||!game_ready))throw std::runtime_error("Native players require a world, replay and game-ready handshake");
    if(native_physics&&!native_players)throw std::runtime_error("Experimental native physics requires --native-players");
    std::optional<WorldState> native_world;
    std::optional<AssetDefinitions> native_assets;
    if(!world_path.empty()) { native_world=WorldState::load(world_path);native_assets=AssetDefinitions::load(game); }
    SdkOutputRedirect sdk_output;
    const auto directory=std::filesystem::absolute(output);
    if(std::filesystem::exists(directory))throw std::runtime_error("Listener output directory must be new");
    std::filesystem::create_directories(directory.parent_path());
    if(!std::filesystem::create_directory(directory))throw std::runtime_error("Cannot exclusively create listener output directory");
    Session session(game,public_server);session.login(30);
    if(!session.logged){const auto result=session.json();write_file(directory/L"session.json",result);return result;}
    steam::InventoryValidation inventory({session.api.server_inventory(),session.api.inventory_deserialize,session.api.inventory_status,session.api.inventory_check_owner,session.api.inventory_destroy});
    Networking net(session);
    const auto auth_initial=net.init_auth(net.sockets);
    const Handle group=net.group(net.sockets);if(!group)throw std::runtime_error("Cannot create SNS poll group");
    struct Owned {
        Networking& net;Handle group,listener{};std::set<Handle> connections;
        ~Owned(){for(auto value:connections)net.close_connection(net.sockets,value,1000,"scrapds probe complete",false);if(listener)net.close_listener(net.sockets,listener);net.destroy_group(net.sockets,group);}
    } owned{net,group};
    owned.listener=net.listen(net.sockets,0,0,nullptr);if(!owned.listener)throw std::runtime_error("Cannot create P2P listener");
    const auto started=Clock::now();
    const auto deadline=seconds==0?Clock::time_point::max():started+std::chrono::seconds(seconds);
    std::vector<std::string> events,messages,control_events,game_events;std::set<Handle> accepted,challenged,authenticated,gameinfo_sent,join_accepted,game_started;std::map<Handle,std::uint64_t> connection_steam_ids;std::map<std::uint64_t,std::size_t> active_steam_connections;std::map<Handle,net::PlayerInfo> player_infos;std::map<Handle,std::vector<std::uint8_t>> pending_inventory_results;net::CharacterStateTracker character_states;net::ServerCharacters server_characters;std::map<Handle,PlayerSpawnPackets> live_players;std::map<Handle,CharacterMotionState> character_motions;
    std::map<std::uint64_t,PlayerSpawnPackets> offline_players;
    if(native_players)for(const auto& [owner,snapshot]:native_world->player_snapshots()) {
        auto restored=restore_player_snapshot(*native_world,owner,snapshot,static_cast<std::uint32_t>(native_world->metadata().tick));
        offline_players.emplace(owner,std::move(restored));
    }
    std::map<std::uint64_t,std::uint32_t> player_colors;
    if(native_players) {
        player_colors=native_world->player_character_colors();
        for(const auto& [owner,color]:player_colors) {
            const auto player=offline_players.find(owner);
            if(player==offline_players.end())throw std::runtime_error("Stored Player color has no Player snapshot");
            player->second.character_state.color_rgba=color;
        }
    }
    std::map<std::uint64_t,StoredPlayerLocation> player_locations;
    std::map<Handle,std::uint16_t> player_worlds;
    std::map<Handle,StoredPlayerLocation> active_player_locations;
    std::map<Handle,std::uint32_t> latest_client_ticks;
    std::set<std::uint64_t> dirty_locations;
    if(native_players) {
        player_locations=native_world->player_locations();
        for(const auto& [owner,location]:player_locations) { (void)location;if(!offline_players.contains(owner))throw std::runtime_error("Stored Player location has no Player snapshot"); }
    }
    // A removal is valid only for peers that received the complete spawn.
    std::map<Handle,std::set<Handle>> visible_players;
    std::map<Handle,std::set<std::uint32_t>> visible_containers;
    std::uint64_t total_bytes{},total_messages{},captured_bytes{},applied_commands{},rejected_commands{};bool finished{};Authentication authentication{};
    std::optional<ReplayPackets> replay;
    if(!replay_dir.empty())replay=load_replay(replay_dir);
    std::map<std::uint32_t,std::uint8_t> physics_flags;
    std::set<std::uint32_t> mapped_physics_bodies;
    std::optional<WorldPhysics> physics;
    std::optional<WorldCollision> character_collision;
    std::optional<OverworldTerrainStreamer> overworld_terrain;
    if(native_players)overworld_terrain.emplace(*native_world,game,512);
    if(native_physics) {
        for(const auto& packet:replay->post)if(!packet.empty()&&packet[0]==24) {
            try {for(const auto& body:net::extract_server_body_updates(packet))physics_flags.insert_or_assign(body.body_id,body.flags);}
            catch(const std::runtime_error&) {} // A malformed replay frame supplies no runtime flags.
        }
        for(const auto& [id,flags]:physics_flags){(void)flags;mapped_physics_bodies.insert(id);}
    }
    std::optional<net::CharacterCreation> spawn_template;
    if(native_players) {
        for(const auto& packet:replay->post)if((spawn_template=find_replay_spawn(packet)))break;
        if(!spawn_template)throw std::runtime_error("Native players require a Character spawn in replay post packets");
    }
    CharacterPhysicsProfile character_profile;
    if(native_players&&native_assets) {
        if(const auto* definition=native_assets->find_character(net::format_character_asset_uuid(spawn_template->asset_uuid_memory))) {
            const auto radius=definition->capsule_radius.value_or(character_profile.capsule_radius);
            const auto height=definition->capsule_height.value_or(character_profile.capsule_height);
            if(std::isfinite(radius)&&std::isfinite(height)&&radius>0&&height>=2*radius) {
                character_profile.capsule_radius=radius;
                character_profile.capsule_height=height;
                if(definition->crouch_height&&*definition->crouch_height>=2*radius&&*definition->crouch_height<=height)
                    character_profile.crouch_height=*definition->crouch_height;
            }
            if(definition->move_speed)character_profile.move_speed=*definition->move_speed;
            if(definition->sprint_speed)character_profile.sprint_speed=*definition->sprint_speed;
            if(definition->crouch_speed)character_profile.crouch_speed=*definition->crouch_speed;
            if(definition->slow_down_speed)character_profile.slow_down_speed=*definition->slow_down_speed;
            if(definition->jump_impulse)character_profile.jump_impulse=*definition->jump_impulse;
            character_profile.swim_supported=definition->can_swim.value_or(false);
            character_profile.maximum_horizontal_speed=std::max({character_profile.move_speed,character_profile.sprint_speed,character_profile.crouch_speed});
        }
        if(!std::isfinite(character_profile.maximum_horizontal_speed)||character_profile.maximum_horizontal_speed<=0||
           !std::isfinite(character_profile.capsule_radius)||!std::isfinite(character_profile.capsule_height)||
           character_profile.capsule_radius<=0||character_profile.capsule_height<2*character_profile.capsule_radius)
            throw std::runtime_error("invalid-player-character-physics-profile");
    }
    using CollisionRegionKey=std::tuple<std::uint16_t,std::int64_t,std::int64_t>;
    std::set<CollisionRegionKey> active_collision_regions;
    auto collision_anchors=[&] {
        constexpr double sector=128.0;
        std::set<CollisionRegionKey> keys;
        auto add=[&](std::uint16_t world,double x,double y) {
            if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>10000000||std::abs(y)>10000000)
                throw std::runtime_error("invalid-character-collision-anchor");
            keys.emplace(world,static_cast<std::int64_t>(std::floor(x/sector)),static_cast<std::int64_t>(std::floor(y/sector)));
        };
        if(spawn_template)add(spawn_template->world_id,spawn_template->position[0],spawn_template->position[1]);
        for(const auto& [peer,location]:active_player_locations){(void)peer;add(location.world_id,location.position[0],location.position[1]);}
        if(native_physics)for(const auto id:mapped_physics_bodies) {
            const auto body=native_world->bodies().find(id);if(body==native_world->bodies().end())continue;
            try{const auto pose=decode_rigid_body_pose(body->second.data);if(pose&&!pose->is_static)add(pose->world_id,pose->position[0],pose->position[1]);}
            catch(const std::runtime_error&) {}
        }
        std::vector<CollisionAnchor> anchors;anchors.reserve(keys.size());
        for(const auto& [world,x,y]:keys)anchors.push_back({world,double(x)*sector+sector*0.5,double(y)*sector+sector*0.5});
        return std::pair{std::move(keys),std::move(anchors)};
    };
    auto sync_character_collision=[&](bool force) {
        if(!native_players||native_physics)return false;
        auto [keys,anchors]=collision_anchors();
        if(!force&&character_collision&&keys==active_collision_regions)return false;
        auto rebuilt=WorldCollision::from_world(*native_world,native_assets?&*native_assets:nullptr,anchors,256.0);
        if(overworld_terrain)overworld_terrain->attach(rebuilt);
        character_collision=std::move(rebuilt);active_collision_regions=std::move(keys);
        write_file(directory/L"character-collision.json",character_collision->summary_json());
        return true;
    };
    auto sync_native_physics=[&](bool force) {
        if(!native_physics)return false;
        auto [keys,anchors]=collision_anchors();
        if(!force&&physics&&keys==active_collision_regions)return false;
        std::vector<PhysicsRegionAnchor> regions;regions.reserve(anchors.size());
        for(const auto& anchor:anchors)regions.push_back({anchor.world_id,anchor.x,anchor.y});
        auto rebuilt=WorldPhysics::from_world(*native_world,1000.0,&mapped_physics_bodies,&*native_assets,regions,256.0);
        if(!rebuilt.coverage().dynamic_bodies)throw std::runtime_error("Native physics found no supported dynamic bodies with captured runtime flags");
        if(overworld_terrain)overworld_terrain->attach(rebuilt);
        physics=std::move(rebuilt);active_collision_regions=std::move(keys);
        write_file(directory/L"physics.json",physics->summary_json());
        return true;
    };
    auto move_character_capsule=[&](std::uint16_t world,Vec3 start,Vec3 displacement) {
        const auto cylinder_height=character_profile.capsule_height-2*character_profile.capsule_radius;
        if(physics) {
            const auto moved=physics->move_character_capsule(world,start,displacement,character_profile.capsule_radius,cylinder_height);
            return CharacterCapsuleResult{moved.position,moved.contacts};
        }
        if(character_collision) {
            const auto moved=character_collision->move_capsule(world,start,displacement,character_profile.capsule_radius,cylinder_height);
            return CharacterCapsuleResult{moved.position,static_cast<std::uint32_t>(moved.contacts.size())};
        }
        return CharacterCapsuleResult{{start.x+displacement.x,start.y+displacement.y,start.z+displacement.z},0};
    };
    auto character_is_grounded=[&](std::uint16_t world,Vec3 position) {
        constexpr double probe_distance=0.06;
        const auto probe=move_character_capsule(world,position,{0,0,-probe_distance});
        return probe.contacts>0&&position.z-probe.position.z<probe_distance-0.002;
    };
    if(native_players&&!native_physics)sync_character_collision(true);
    if(native_physics)sync_native_physics(true);
    if(overworld_terrain) {
        std::vector<TerrainAnchor> anchors;anchors.reserve(active_player_locations.size()+(physics?physics->bodies().size():0)+1);
        if(physics)for(const auto& body:physics->bodies())anchors.push_back({body.world_id,body.position.x,body.position.y,1});
        if(spawn_template)anchors.push_back({spawn_template->world_id,spawn_template->position[0],spawn_template->position[1],0});
        if(physics)overworld_terrain->stream(*physics,anchors,32);
        else if(character_collision)overworld_terrain->stream(*character_collision,anchors,32);
        write_file(directory/L"overworld-terrain.json",overworld_terrain->summary_json());
    }
    std::optional<net::InitialAssetCatalog> native_catalog;
    std::optional<net::NetObjectPacket> native_initial_template;
    std::size_t native_initial_body_creations{};
    if(native_world) {
        const auto bootstrap_hash=sha256(replay_dir/L"world29.bin");
        const auto saved_catalog_path=std::filesystem::absolute(world_path).parent_path()/L"asset-catalog.bin";
        net::InitialAssetCatalog restored_catalog;
        if(std::filesystem::exists(saved_catalog_path))restored_catalog=net::parse_asset_catalog(read_binary(saved_catalog_path),bootstrap_hash);
        const auto packets=net::split_world_aggregate(replay->world29);
        for(const auto& packet:packets)if(!packet.empty()&&packet.front()==21) {
            native_initial_template=net::parse_net_object_packet(packet);
            native_initial_body_creations=static_cast<std::size_t>(std::count_if(
                native_initial_template->records.begin(),native_initial_template->records.end(),
                [](const auto& record){return record.type==0&&record.operation==1;}));
            auto inferred=net::infer_initial_asset_catalog(*native_initial_template,*native_world);
            net::merge_asset_catalog(restored_catalog,inferred);
            native_catalog=std::move(restored_catalog);
            break;
        }
        if(!native_catalog||!native_initial_template||!native_initial_body_creations)
            throw std::runtime_error("Bootstrap has no native initial NetObj packet");
        write_binary(directory/L"asset-catalog.bin",net::serialize_asset_catalog(*native_catalog,bootstrap_hash));
    }
    auto send_control=[&](Handle connection,const std::uint8_t* data,std::size_t size,const char* kind){
        std::int64_t number=-1;
        const int result=net.send(net.sockets,connection,data,static_cast<std::uint32_t>(size),8,&number);
        const int flushed=net.flush(net.sockets,connection);
        if(control_events.size()<10000)control_events.push_back("{\"connection\":"+std::to_string(connection)+",\"kind\":"+json_quote(kind)+",\"bytes\":"+std::to_string(size)+",\"eresult\":"+std::to_string(result)+",\"message_number\":"+std::to_string(number)+",\"flush_eresult\":"+std::to_string(flushed)+",\"elapsed_seconds\":"+elapsed(started)+'}');
        return result==1;
    };
    auto send_game=[&](Handle connection,const std::vector<std::uint8_t>& plaintext,const char* kind){
        const auto wire=net::compress_packet(plaintext);
        std::int64_t number=-1;
        const int result=net.send(net.sockets,connection,wire.data(),static_cast<std::uint32_t>(wire.size()),8,&number);
        const int flushed=net.flush(net.sockets,connection);
        if(game_events.size()<10000)game_events.push_back("{\"connection\":"+std::to_string(connection)+",\"kind\":"+json_quote(kind)+",\"plain_bytes\":"+std::to_string(plaintext.size())+",\"wire_bytes\":"+std::to_string(wire.size())+",\"eresult\":"+std::to_string(result)+",\"message_number\":"+std::to_string(number)+",\"flush_eresult\":"+std::to_string(flushed)+",\"elapsed_seconds\":"+elapsed(started)+'}');
        return result==1;
    };
    auto report=[&](){return "{\"format\":\"scrapds.native-sns-listener.v1\",\"status\":"+json_quote(finished?"capture_finished":"listening")+",\"steam_id\":"+json_quote(std::to_string(session.id))+",\"allowed_peer\":"+json_quote(std::to_string(allow_peer)) + ",\"allowed_peer2\":"+json_quote(std::to_string(allow_peer2))+",\"interface\":\"SteamNetworkingSockets012\",\"virtual_port\":0,\"sends_game_messages\":"+(handshake?"true":"false")+",\"sends_control_messages\":"+(handshake?"true":"false")+",\"replay\":"+(replay?"true":"false")+",\"native_world\":"+(native_world?native_world->summary_json():"null")+",\"applied_commands\":"+std::to_string(applied_commands)+",\"rejected_commands\":"+std::to_string(rejected_commands)+",\"advertising\":"+(session.advertising?"true":"false")+",\"public_server\":"+(public_server?"true":"false")+",\"authentication_initial\":"+std::to_string(auth_initial)+",\"authentication_current\":{\"availability\":"+std::to_string(authentication.availability)+",\"debug\":"+json_quote(bounded(authentication.debug))+"},\"message_count\":"+std::to_string(total_messages)+",\"captured_message_count\":"+std::to_string(messages.size())+",\"total_message_bytes\":"+std::to_string(total_bytes)+",\"challenge_count\":"+std::to_string(challenged.size())+",\"authenticated_count\":"+std::to_string(authenticated.size())+",\"game_info_sent_count\":"+std::to_string(gameinfo_sent.size())+",\"join_accepted_count\":"+std::to_string(join_accepted.size())+",\"game_started_count\":"+std::to_string(game_started.size())+",\"events\":"+array_json(events)+",\"control_events\":"+array_json(control_events)+",\"game_events\":"+array_json(game_events)+",\"messages\":"+array_json(messages)+",\"elapsed_seconds\":"+elapsed(started)+'}';};
    auto compact_report=[&](){return "{\"format\":\"scrapds.native-sns-listener.v1\",\"status\":"+json_quote(finished?"capture_finished":"listening")+",\"steam_id\":"+json_quote(std::to_string(session.id))+",\"allowed_peer\":"+json_quote(std::to_string(allow_peer))+",\"allowed_peer2\":"+json_quote(std::to_string(allow_peer2))+",\"public_server\":"+(public_server?"true":"false")+",\"advertising\":"+(session.advertising?"true":"false")+",\"native_world\":"+(native_world?native_world->summary_json():"null")+",\"applied_commands\":"+std::to_string(applied_commands)+",\"rejected_commands\":"+std::to_string(rejected_commands)+",\"message_count\":"+std::to_string(total_messages)+",\"captured_message_count\":"+std::to_string(messages.size())+",\"total_message_bytes\":"+std::to_string(total_bytes)+",\"challenge_count\":"+std::to_string(challenged.size())+",\"authenticated_count\":"+std::to_string(authenticated.size())+",\"game_info_sent_count\":"+std::to_string(gameinfo_sent.size())+",\"join_accepted_count\":"+std::to_string(join_accepted.size())+",\"game_started_count\":"+std::to_string(game_started.size())+",\"event_count\":"+std::to_string(events.size())+",\"control_event_count\":"+std::to_string(control_events.size())+",\"game_event_count\":"+std::to_string(game_events.size())+",\"authentication_current\":{\"availability\":"+std::to_string(authentication.availability)+",\"debug\":"+json_quote(bounded(authentication.debug))+"},\"elapsed_seconds\":"+elapsed(started)+'}';};
    auto save=[&](bool full){const auto temporary=directory/L".listener.tmp";write_file(temporary,full?report():compact_report());if(!MoveFileExW(temporary.c_str(),(directory/L"listener.json").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){const auto error=GetLastError();if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED)throw std::runtime_error("Cannot publish listener progress");}};
    if(native_world)native_world->save_as(directory/L"world.db");
    auto next_save=Clock::now()+std::chrono::seconds(30);
    auto next_tick=Clock::now();
    std::uint32_t server_tick=native_world?static_cast<std::uint32_t>(native_world->metadata().tick):0x00800500u;
    std::optional<std::vector<std::uint8_t>> native_script_init,native_generic_init;
    if(native_world) {
        const std::array terrain_mapping{BlobUidMapping{terrain_data_uid,terrain_data_uid}};
        const std::array generic_mappings{
            BlobUidMapping{generic_flat_uid,generic_flat_uid},
            BlobUidMapping{generic_world_runtime_uid,generic_world_saved_uid}
        };
        native_script_init=rebuild_mapped_blob_packet(replay->init11,*native_world,server_tick,terrain_mapping);
        native_generic_init=rebuild_mapped_blob_packet(replay->init13,*native_world,server_tick,generic_mappings);
        if(native_initial_template&&native_catalog)
            (void)net::rebuild_live_initial_world(*native_initial_template,*native_world,server_tick,*native_catalog);
    }
    net.auth(net.sockets,&authentication);save(false);
    write_file(directory/L"ready.json","{\"steam_id\":"+json_quote(std::to_string(session.id))+",\"virtual_port\":0,\"seconds\":"+std::to_string(seconds)+",\"run_mode\":"+json_quote(seconds==0?"until-stop-request":"timed")+",\"stop_file\":"+json_quote(path_utf8(directory/L"stop.request"))+",\"allowed_peer\":"+json_quote(std::to_string(allow_peer))+",\"allowed_peer2\":"+json_quote(std::to_string(allow_peer2))+",\"public_server\":"+(public_server?"true":"false")+",\"advertising\":"+(session.advertising?"true":"false")+",\"native_players\":"+(native_players?"true":"false")+",\"native_physics\":"+(physics?"true":"false")+",\"character_capsule_radius\":"+std::to_string(character_profile.capsule_radius)+",\"character_capsule_height\":"+std::to_string(character_profile.capsule_height)+",\"character_crouch_height\":"+std::to_string(character_profile.crouch_height)+",\"character_max_horizontal_speed\":"+std::to_string(character_profile.maximum_horizontal_speed)+",\"character_slow_down_speed\":"+std::to_string(character_profile.slow_down_speed)+",\"character_jump_impulse\":"+std::to_string(character_profile.jump_impulse)+",\"character_can_swim\":"+(character_profile.swim_supported?"true":"false")+",\"handshake\":"+(handshake?"true":"false")+",\"game_ready\":"+(game_ready?"true":"false")+",\"replay\":"+(replay?"true":"false")+'}');
    auto native_packet=[&](const std::vector<std::uint8_t>& packet,auto&& recurse)->std::vector<std::uint8_t> {
        if(!native_world||packet.empty())return packet;
        if(packet.front()==29) {
            auto components=net::split_world_aggregate(packet);
            for(auto& component:components)component=recurse(component,recurse);
            return net::make_world_aggregate(components);
        }
        if(packet.front()==21) {
            const auto source=net::parse_net_object_packet(packet);
            const auto body_count=std::count_if(source.records.begin(),source.records.end(),[](const auto& record){return record.type==0&&record.operation==1;});
            if(static_cast<std::size_t>(body_count)==native_initial_body_creations)
                return net::make_net_object_packet(net::rebuild_live_initial_world(*native_initial_template,*native_world,server_tick,*native_catalog).packet);
        }
        if(packet.front()==21||packet.front()==22) {
            auto source=net::parse_net_object_packet(packet);source.tick=server_tick;
            for(auto& record:source.records)if(record.operation==3&&record.type==1&&native_world->shapes().contains(record.id)) {
                const auto& blob=native_world->shapes().at(record.id).data;
                net::InitialAssetCatalog::SavedUuid uuid{};std::copy_n(blob.begin()+11,16,uuid.begin());
                record=net::make_child_shape_state(blob,native_catalog->shapes.at(uuid));
            }
            return net::make_net_object_packet(source);
        }
        if(packet.front()==11||packet.front()==13||packet.front()==25||packet.front()==27) {
            if(packet.front()==25) {
                const std::array script_state_mappings{
                    BlobUidMapping{script_runtime_uid_a,script_saved_uid,{"responseSlider"}},
                    BlobUidMapping{script_runtime_uid_b,script_saved_uid,{"gearIdx","batteryPoints","fuelPoints","casingMaterialAmounts","batteryPower","settingsLevel","chemicalPoints","currentSettingsLevel","sliderPos"}}
                };
                return rebuild_mapped_blob_packet(packet,*native_world,server_tick,script_state_mappings);
            }
            auto source=net::parse_blob_data_packet(packet);source.tick=server_tick;return net::make_blob_data_packet(source);
        }
        return packet;
    };
    auto remember_container_creates=[&](Handle peer,const std::vector<std::uint8_t>& packet,auto&& recurse)->void {
        if(packet.empty())return;
        if(packet[0]==29){for(const auto& child:net::split_world_aggregate(packet))recurse(peer,child,recurse);return;}
        if(packet[0]!=21&&packet[0]!=22)return;
        for(const auto& record:net::parse_net_object_packet(packet).records)
            if(record.operation==1&&record.type==4)visible_containers[peer].insert(record.id);
    };
    auto send_replay=[&](Handle peer,const std::vector<std::uint8_t>& packet,const char* kind) {
        if(native_physics&&!packet.empty()&&packet[0]==24)return true;
        auto rebuilt=native_packet(packet,native_packet);
        if(native_players) {auto filtered=strip_replay_players(rebuilt);if(!filtered)return true;rebuilt=std::move(*filtered);}
        const auto sent=send_game(peer,rebuilt,kind);
        if(sent&&native_players)remember_container_creates(peer,rebuilt,remember_container_creates);
        return sent;
    };
    auto flush_locations=[&] {
        if(!native_players||dirty_locations.empty())return;
        native_world->atomic([&] {for(const auto owner:dirty_locations)native_world->store_player_location(owner,player_locations.at(owner));return true;});
        dirty_locations.clear();
    };
    std::set<Handle> deferred_close;
    // Steam's game protocol sends periodic client traffic after joining. Keep
    // accepted peers from occupying slots forever when that traffic stops.
    std::map<Handle,Clock::time_point> last_peer_activity;
    unsigned movement_sequence=0;
    std::uint64_t physics_ticks=0;
    std::size_t physics_cursor=0;
    while(Clock::now()<deadline&&!std::filesystem::exists(directory/L"stop.request")){
        session.api.frame(session.pipe);Callback callback{};std::set<Handle> pending_close=std::move(deferred_close),desynchronized=pending_close;deferred_close.clear();
        for(int i=0;i<1000&&session.api.next(session.pipe,&callback);++i){
            struct Free {API& api;std::int32_t pipe;~Free(){api.free_callback(pipe);}} free{session.api,session.pipe};
            if(callback.id==1221){
                if(!callback.data||callback.size!=sizeof(Status))throw std::runtime_error("SNS status callback ABI mismatch");
                Status status{};std::memcpy(&status,callback.data,sizeof(status));owned.connections.insert(status.connection);
                std::string event="{\"connection\":"+std::to_string(status.connection)+",\"state\":"+std::to_string(status.info.state)+",\"old_state\":"+std::to_string(status.old_state)+",\"steam_id\":"+json_quote(std::to_string(status.info.remote.steam_id()))+",\"end_reason\":"+std::to_string(status.info.end_reason)+",\"end_debug\":"+json_quote(bounded(status.info.end_debug))+",\"elapsed_seconds\":"+elapsed(started);
                if(status.info.state==1){Info current{};if(!net.info(net.sockets,status.connection,&current)||!allowed_peer(current.remote.steam_id())||current.listen!=owned.listener){event+=",\"action\":\"rejected-unexpected-peer\"";pending_close.insert(status.connection);}else{const bool grouped=net.set_group(net.sockets,status.connection,group);const int result=grouped?net.accept(net.sockets,status.connection):0;event+=",\"action\":\"accept\",\"accept_eresult\":"+std::to_string(result);if(result==1){accepted.insert(status.connection);const auto steam_id=current.remote.steam_id();connection_steam_ids[status.connection]=steam_id;++active_steam_connections[steam_id];if(handshake&&!challenged.contains(status.connection)){const auto challenge=net::make_control_challenge();if(send_control(status.connection,challenge.data(),challenge.size(),"challenge"))challenged.insert(status.connection);}}else pending_close.insert(status.connection);}}
                if(status.info.state==4||status.info.state==5)pending_close.insert(status.connection);
                event+='}';if(events.size()<10000)events.push_back(std::move(event));
            }
        }
        for(const auto peer:accepted)last_peer_activity.try_emplace(peer,Clock::now());
        for(const auto& outcome:inventory.poll()) {
            const auto player=player_infos.find(outcome.connection);
            const auto pending=pending_inventory_results.find(outcome.connection);
            if(player!=player_infos.end()&&outcome.owner_valid){player->second.inventory_verified=true;player->second.inventory_expired=outcome.status==27;}
            else if(player!=player_infos.end()&&pending==pending_inventory_results.end()){player->second.inventory_verified=false;player->second.inventory_expired=outcome.status==27;}
            if(outcome.owner_valid&&native_players&&!pending_close.contains(outcome.connection)&&pending!=pending_inventory_results.end()) {
                try {
                    const auto live=live_players.find(outcome.connection);
                    if(live==live_players.end())throw std::runtime_error("inventory-owner-no-longer-active");
                    const auto update=apply_verified_player_inventory(*native_world,live->second,pending->second,server_tick);
                    if(player!=player_infos.end())player->second.inventory_result=pending->second;
                    std::set<Handle> recipients;
                    if(game_started.contains(outcome.connection))recipients.insert(outcome.connection);
                    for(const auto& [peer,subjects]:visible_players)if(game_started.contains(peer)&&subjects.contains(outcome.connection))recipients.insert(peer);
                    for(const auto peer:recipients)if(!send_game(peer,update,"verified-player-inventory")) {pending_close.insert(peer);desynchronized.insert(peer);}
                    if(game_events.size()<10000)game_events.push_back("{\"kind\":\"inventory-state-applied\",\"connection\":"+std::to_string(outcome.connection)+",\"bytes\":"+std::to_string(pending->second.size())+'}');
                } catch(const std::exception& error) {
                    if(game_events.size()<10000)game_events.push_back("{\"kind\":\"inventory-state-rejected\",\"connection\":"+std::to_string(outcome.connection)+",\"error\":"+json_quote(error.what())+'}');
                }
            }
            if(pending!=pending_inventory_results.end())pending_inventory_results.erase(pending);
            if(game_events.size()<10000)game_events.push_back("{\"kind\":\"inventory-validation\",\"connection\":"+std::to_string(outcome.connection)+",\"status\":"+std::to_string(outcome.status)+",\"owner_valid\":"+(outcome.owner_valid?"true":"false")+'}');
        }
        // Drain before closing ended connections so queued final messages survive.
        for(int batch=0;batch<32;++batch){
            std::array<Message*,32> batch_messages{};
            const int count=net.receive(net.sockets,group,batch_messages.data(),static_cast<int>(batch_messages.size()));
            if(count<0||count>static_cast<int>(batch_messages.size()))throw std::runtime_error("SNS receive failed");
            struct Release {Networking& net;std::array<Message*,32>& messages;int count;~Release(){for(int i=0;i<count;++i)if(messages[i])net.release(messages[i]);}} release{net,batch_messages,count};
            for(int i=0;i<count;++i){
                const auto* message=batch_messages[i];
                if(!message||!accepted.contains(message->connection)||!allowed_peer(message->peer.steam_id()))throw std::runtime_error("Received unapproved SNS peer message");
                last_peer_activity.insert_or_assign(message->connection,Clock::now());
                if(desynchronized.contains(message->connection))continue;
                if(message->size<0||message->size>16*1024*1024||(message->size&&!message->data))throw std::runtime_error("Invalid SNS message size");
                bool control_handled=false;
                if(handshake&&!authenticated.contains(message->connection)&&message->size>=1&&message->size<=300){
                    try {
                        const auto control=net::parse_control(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(message->data),static_cast<std::size_t>(message->size)));
                        if(control.kind==net::ControlKind::authentication){
                            if(control_events.size()<10000)control_events.push_back("{\"connection\":"+std::to_string(message->connection)+",\"kind\":\"authentication\",\"password_bytes\":"+std::to_string(control.password.size())+",\"secret\":"+json_quote(std::to_string(control.secret))+",\"elapsed_seconds\":"+elapsed(started)+'}');
                            const auto acceptance=net::make_control_acceptance();
                            if(send_control(message->connection,acceptance.data(),acceptance.size(),"acceptance"))authenticated.insert(message->connection);
                            control_handled=true;
                        } else if(control.kind==net::ControlKind::accepted){
                            if(control_events.size()<10000)control_events.push_back("{\"connection\":"+std::to_string(message->connection)+",\"kind\":\"accepted\",\"elapsed_seconds\":"+elapsed(started)+'}');
                            authenticated.insert(message->connection);control_handled=true;
                        } else {
                            if(control_events.size()<10000)control_events.push_back("{\"connection\":"+std::to_string(message->connection)+",\"kind\":\"challenge\",\"elapsed_seconds\":"+elapsed(started)+'}');
                            control_handled=true;
                        }
                    } catch(const std::invalid_argument&) {}
                }
                // The observed client sends a one-byte 0x01 reliably after the
                // 05 control acceptance. Answer once with the minimum GameInfo
                // payload so the client can advance past its connect state.
                if(handshake&&authenticated.contains(message->connection)&&!gameinfo_sent.contains(message->connection)&&message->size==1&&static_cast<const std::uint8_t*>(message->data)[0]==1){
                    bool sent=false;
                    if(replay){
                        auto info=replay->game_info;
                        if(native_world) {
                            if(info.size()<17)throw std::runtime_error("Truncated bootstrap GameInfo");
                            const auto game_mode=native_world->metadata().flags;
                            if(game_mode<0||game_mode>UINT32_MAX)throw std::runtime_error("Game mode outside uint32 range");
                            for(int byte=0;byte<4;++byte) {
                                info[5+byte]=std::uint8_t(static_cast<std::uint32_t>(game_mode)>>(24-8*byte));
                                info[9+byte]=std::uint8_t(static_cast<std::uint32_t>(native_world->metadata().seed)>>(24-8*byte));
                                info[13+byte]=std::uint8_t(server_tick>>(24-8*byte));
                            }
                        }
                        if(native_players)info=strip_game_info_players(info);
                        sent=send_game(message->connection,info,native_world?"game-info-native":"game-info-replay");
                        sent=send_replay(message->connection,native_script_init?*native_script_init:replay->init11,"init-script")&&sent;
                        sent=send_replay(message->connection,native_generic_init?*native_generic_init:replay->init13,"init-generic")&&sent;
                        sent=send_replay(message->connection,replay->world29,native_world?"world-data-native":"world-data")&&sent;
                    } else {
                        sent=send_game(message->connection,make_minimal_game_info(),"game-info");
                    }
                    if(sent)gameinfo_sent.insert(message->connection);
                }
                // Experimental next-state probe: the real client sends its large
                // join packet (id 6) after GameInfo. Reply with the observed
                // server->client state transition id 7. This does not send world
                // data; it only tests whether the client advances to state 4.
                if(game_ready&&gameinfo_sent.contains(message->connection)&&!join_accepted.contains(message->connection)&&message->size>1&&static_cast<const std::uint8_t*>(message->data)[0]==6){
                    if(send_game(message->connection,std::vector<std::uint8_t>{7},"join-accepted"))join_accepted.insert(message->connection);
                }
                // After id 7 the client sends its player-info packet (256
                // decoded bytes in the captured vanilla session).
                // (id 9). Reply with the state-6 transition id 10. No world or
                // NetObj data is sent yet; this only tests the client state.
                if(game_ready&&join_accepted.contains(message->connection)&&!game_started.contains(message->connection)&&message->size>1&&static_cast<const std::uint8_t*>(message->data)[0]==9){
                    std::optional<PlayerSpawnPackets> prepared;
                    std::optional<StoredPlayerLocation> joining_location;
                    try {
                        const auto plain=net::decompress_packet(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(message->data),message->size));
                        auto info=net::parse_player_info(plain);auto inventory_result=std::move(info.inventory_result);info.inventory_result.clear();
                        if(native_players) {
                            PlayerSpawnSpec spec;spec.steam_id=message->peer.steam_id();spec.info=info;spec.character=*spawn_template;spec.character_state.active=true;
                            if(const auto prior=offline_players.find(spec.steam_id);prior!=offline_players.end()){
                                spec.existing_player_id=prior->second.player_id;spec.character_state=prior->second.character_state;spec.character_state.active=true;
                                spec.player_state_words=prior->second.player_state_words;spec.player_flags=prior->second.player_flags;
                                if(prior->second.info.inventory_verified) {
                                    spec.info.inventory_kind=prior->second.info.inventory_kind;
                                    spec.info.inventory_result=prior->second.info.inventory_result;
                                    spec.info.inventory_verified=true;
                                }
                            }
                            info=spec.info;
                            if(const auto color=player_colors.find(spec.steam_id);color!=player_colors.end())spec.character_state.color_rgba=color->second;
                            if(const auto location=player_locations.find(spec.steam_id);location!=player_locations.end()) {
                                spec.character.world_id=location->second.world_id;
                                for(int axis=0;axis<3;++axis)spec.character.position[axis]=static_cast<float>(location->second.position[axis]);
                            }
                            joining_location=StoredPlayerLocation{spec.character.world_id,{spec.character.position[0],spec.character.position[1],spec.character.position[2]}};
                            prepared=prepare_player_spawn(*native_world,server_characters,spec,server_tick);
                            player_worlds.insert_or_assign(message->connection,spec.character.world_id);
                        }
                        if(send_game(message->connection,std::vector<std::uint8_t>{10},"game-start")){
                            if(joining_location){
                                active_player_locations.insert_or_assign(message->connection,*joining_location);
                                try{if(character_collision)sync_character_collision(false);else if(physics)sync_native_physics(false);}
                                catch(const std::exception& error){std::cerr<<"scrapds: character collision region update failed: "<<error.what()<<'\n';}
                                CharacterMotionState motion;motion.world_id=joining_location->world_id;
                                motion.position={joining_location->position[0],joining_location->position[1],joining_location->position[2]};
                                motion.grounded=character_is_grounded(motion.world_id,motion.position);motion.initialized=true;
                                character_motions.insert_or_assign(message->connection,motion);
                            }
                            if(prepared){live_players.emplace(message->connection,std::move(*prepared));prepared.reset();offline_players.erase(message->peer.steam_id());}
                            if(info.inventory_kind&&!inventory_result.empty()) {
                                const bool inventory_started=inventory.begin(message->connection,message->peer.steam_id(),inventory_result);
                                if(inventory_started&&native_players)pending_inventory_results.insert_or_assign(message->connection,std::move(inventory_result));
                                if(game_events.size()<10000)game_events.push_back("{\"kind\":\"inventory-validation-start\",\"connection\":"+std::to_string(message->connection)+",\"started\":"+(inventory_started?"true":"false")+'}');
                            }
                            player_infos.insert_or_assign(message->connection,std::move(info));
                            game_started.insert(message->connection);
                            if(replay){
                                const auto post_count=std::min<std::size_t>(replay->post.size(),24);
                                for(std::size_t post_index=0;post_index<post_count;++post_index)if(!send_replay(message->connection,replay->post[post_index],"post-bootstrap")&&native_players){pending_close.insert(message->connection);desynchronized.insert(message->connection);break;}
                            }
                            if(native_players&&!pending_close.contains(message->connection)) {
                                auto send_spawn=[&](Handle peer,Handle subject,const PlayerSpawnPackets& spawn) {
                                    auto character_update=spawn.character_update;
                                    const auto owner=connection_steam_ids.find(subject);
                                    if(owner!=connection_steam_ids.end())if(const auto latest=server_characters.latest(owner->second))character_update=character_spawn_at_latest_position(spawn,*latest);
                                    if(spawn.inventory_container_id!=UINT32_MAX&&!visible_containers[peer].contains(spawn.inventory_container_id)) {
                                        if(spawn.container_creation.empty()||!send_game(peer,native_packet(spawn.container_creation,native_packet),"native-player-container")) {
                                            pending_close.insert(peer);desynchronized.insert(peer);return false;
                                        }
                                        visible_containers[peer].insert(spawn.inventory_container_id);
                                    }
                                    for(const auto* packet:std::array<const std::vector<std::uint8_t>*,3>{&spawn.player_before,&spawn.player_after,&character_update}) {
                                        if(!send_game(peer,native_packet(*packet,native_packet),"native-player-spawn")){pending_close.insert(peer);desynchronized.insert(peer);return false;}
                                    }
                                    visible_players[peer].insert(subject);
                                    return true;
                                };
                                for(const auto& [peer,spawn]:live_players)if(peer!=message->connection&&!pending_close.contains(peer)&&
                                    player_worlds.contains(peer)&&player_worlds.at(peer)==player_worlds.at(message->connection))
                                    if(!send_spawn(message->connection,peer,spawn))break;
                                for(const auto& [owner,spawn]:offline_players) {
                                    (void)owner;
                                    if(pending_close.contains(message->connection))break;
                                    if(spawn.inventory_container_id!=UINT32_MAX&&!visible_containers[message->connection].contains(spawn.inventory_container_id)) {
                                        if(spawn.container_creation.empty()||!send_game(message->connection,native_packet(spawn.container_creation,native_packet),"native-offline-player-container")) {
                                            pending_close.insert(message->connection);desynchronized.insert(message->connection);break;
                                        }
                                        visible_containers[message->connection].insert(spawn.inventory_container_id);
                                    }
                                    if(!send_game(message->connection,native_packet(spawn.player_before,native_packet),"native-offline-player")){pending_close.insert(message->connection);desynchronized.insert(message->connection);}
                                }
                                if(!pending_close.contains(message->connection)) {
                                    const auto& spawn=live_players.at(message->connection);
                                    for(const auto peer:game_started)if(!pending_close.contains(peer)&&
                                        player_worlds.contains(peer)&&player_worlds.at(peer)==player_worlds.at(message->connection))
                                        send_spawn(peer,message->connection,spawn);
                                }
                            }
                        } else {if(prepared){server_characters.despawn(message->peer.steam_id());prepared.reset();}pending_close.insert(message->connection);}
                    } catch(const std::exception& error) {
                        if(prepared)server_characters.despawn(message->peer.steam_id());
                        pending_close.insert(message->connection);
                        if(game_events.size()<10000)game_events.push_back("{\"kind\":\"player-info-rejected\",\"error\":"+json_quote(error.what())+'}');
                    }
                }
                bool native_command=false;
                const auto message_id=message->size?static_cast<const std::uint8_t*>(message->data)[0]:0;
                if(native_world&&game_started.contains(message->connection)&&message->size>1&&(message_id==29||(message_id>=0x22&&message_id<=0x2b))) {
                    try {
                        const auto plain=net::decompress_packet(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(message->data),message->size));
                        auto handle=[&](const std::vector<std::uint8_t>& p,auto&& recurse)->void {
                            if(p.empty())return;
                            if(p.front()==29) { for(const auto& part:net::split_world_aggregate(p))recurse(part,recurse);return; }
                            if(p.front()<0x22||p.front()>0x2b)return;
                            native_command=true;
                            const auto command=net::parse_game_command(p);
                            CommandResult result;std::vector<std::uint8_t> response;bool world_mutated=false;std::set<Handle> recipients;
                            if(const auto* paint=std::get_if<net::PaintCharacterCommand>(&command)) {
                                std::string failure;
                                if(!native_players)failure="character-paint-requires-native-players";
                                else if(native_world->metadata().flags!=1&&native_world->metadata().flags!=14)
                                    failure="character-paint-requires-supported-game-mode";
                                else if(native_world->metadata().flags==14) {
                                    bool owns_paint_tool=false;
                                    const auto painter=live_players.find(message->connection);
                                    if(painter!=live_players.end()) {
                                        const auto inventory=native_world->containers().find(painter->second.inventory_container_id);
                                        if(inventory!=native_world->containers().end()&&inventory->second.supported_layout)
                                            for(std::uint32_t slot=0;slot<inventory->second.entry_count;++slot) {
                                                const auto quantity=native_world->container_slot_quantity(
                                                    painter->second.inventory_container_id,slot,paint_tool_item_saved_uuid);
                                                if(quantity&&*quantity>0){owns_paint_tool=true;break;}
                                            }
                                    }
                                    if(!owns_paint_tool)failure="character-paint-requires-owned-paint-tool";
                                }
                                if(!failure.empty())result={false,std::move(failure),{}};
                                else {
                                    auto target=std::find_if(live_players.begin(),live_players.end(),[&](const auto& item){return item.second.character_id==paint->character_id;});
                                    if(target==live_players.end())result={false,"character-not-active",{}};
                                    else if(player_worlds.at(target->first)!=player_worlds.at(message->connection))result={false,"character-in-another-world",{}};
                                    else try {
                                        const auto asset_uuid=net::format_character_asset_uuid(target->second.character_state.asset_uuid_memory);
                                        const auto* definition=native_assets?native_assets->find_character(asset_uuid):nullptr;
                                        if(asset_uuid!="00000000-0000-0000-0000-000000000000"&&(!definition||definition->paint_enabled!=true))
                                            throw std::runtime_error(definition?"character-paint-disabled-by-asset":"character-asset-unavailable");
                                        const auto color=paint->color_rgba?paint->color_rgba:
                                            definition&&definition->default_paint_color_rgba?*definition->default_paint_color_rgba:
                                            asset_uuid=="00000000-0000-0000-0000-000000000000"?captured_character_default_color:0u;
                                        if(!color)throw std::runtime_error("character-default-color-unavailable");
                                        const auto owner=connection_steam_ids.find(target->first);
                                        if(owner==connection_steam_ids.end())throw std::runtime_error("character-owner-unavailable");
                                        auto updated=target->second;
                                        response=paint_player_character(updated,color,server_tick);
                                        native_world->store_player_character_color(owner->second,color);
                                        player_colors.insert_or_assign(owner->second,color);
                                        target->second=std::move(updated);result={true,"applied"};
                                        for(const auto peer:game_started)if(player_worlds.contains(peer)&&player_worlds.at(peer)==player_worlds.at(target->first))recipients.insert(peer);
                                    } catch(const std::exception& error) {result={false,error.what(),{}};}
                                }
                            } else {
                                std::optional<std::uint32_t> player_container;
                                if(native_players)if(const auto player=live_players.find(message->connection);player!=live_players.end())
                                    player_container=player->second.inventory_container_id;
                                const auto command_world=player_worlds.contains(message->connection)?player_worlds.at(message->connection):std::uint16_t(0);
                                auto processed=apply_replicated_command(*native_world,*native_assets,command,server_tick,*native_catalog,command_world,player_container);
                                result=processed.command;response=std::move(processed.response);world_mutated=result.applied;
                                if(result.applied)for(const auto peer:game_started)
                                    if(!native_players||(player_worlds.contains(peer)&&player_worlds.at(peer)==command_world))recipients.insert(peer);
                            }
                            if(result.applied)++applied_commands;else ++rejected_commands;
                            if(world_mutated&&character_collision)try {
                                sync_character_collision(true);
                            } catch(const std::exception& error) {
                                character_collision.reset();active_collision_regions.clear();
                                try{write_file(directory/L"character-collision.json","{\"active\":false,\"error\":"+json_quote(error.what())+'}');}catch(const std::exception&) {}
                                std::cerr<<"scrapds: character collision disabled after world change: "<<error.what()<<'\n';
                            }
                            if(world_mutated&&physics)try{sync_native_physics(true);}catch(const std::exception& e){physics.reset();active_collision_regions.clear();try{write_file(directory/L"physics.json","{\"active\":false,\"error\":"+json_quote(e.what())+'}');}catch(const std::exception&) {}std::cerr<<"scrapds: physics region rebuild failed after world change: "<<e.what()<<'\n';}
                            if(!response.empty()) {
                                if(recipients.empty()&&!native_players)recipients=game_started;
                                net::deliver_world_update(recipients,pending_close,desynchronized,[&](Handle peer){return send_game(peer,response,result.reason=="applied"&&std::holds_alternative<net::PaintCharacterCommand>(command)?"native-character-paint":"native-object-update");});
                            }
                            if(game_events.size()<10000)game_events.push_back("{\"kind\":\"native-command\",\"applied\":"+std::string(result.applied?"true":"false")+",\"reason\":"+json_quote(result.reason)+",\"elapsed_seconds\":"+elapsed(started)+'}');
                        };
                        handle(plain,handle);
                    } catch(const std::exception& error) { native_command=true;++rejected_commands;if(game_events.size()<10000)game_events.push_back("{\"kind\":\"native-command-error\",\"error\":"+json_quote(error.what())+'}'); }
                }
                if(game_started.contains(message->connection)&&message_id==0x1e) {
                    native_command=true;
                    try {
                        const auto plain=net::decompress_packet(std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(message->data),message->size));
                        auto update=net::parse_character_update(plain);
                        std::optional<std::array<float,3>> corrected_position;
                        std::optional<std::array<float,3>> corrected_velocity;
                        std::optional<CharacterMotionState> accepted_motion;
                        if(update.has_character_state) {
                            if(native_players) {
                                std::array<float,3> baseline{};double elapsed_ticks{};bool have_baseline=false;
                                if(const auto previous=server_characters.latest(message->peer.steam_id())) {
                                    const auto tick_delta=std::bit_cast<std::int32_t>(update.tick-previous->tick);
                                    if(tick_delta<0)throw std::runtime_error("stale-character-tick");
                                    elapsed_ticks=std::clamp(double(tick_delta),0.0,40.0);
                                    if(tick_delta==0) {
                                        const auto tick_start=server_characters.tick_start_position(message->peer.steam_id());
                                        if(!tick_start)throw std::runtime_error("missing-character-tick-baseline");
                                        baseline=*tick_start;
                                    } else baseline=previous->position;
                                    have_baseline=true;
                                } else if(const auto spawn=live_players.find(message->connection);spawn!=live_players.end()) {
                                    const auto server_age=std::bit_cast<std::int32_t>(server_tick-spawn->second.spawn_server_tick);
                                    elapsed_ticks=std::clamp(double(server_age),0.0,40.0);baseline=spawn->second.spawn_position;have_baseline=true;
                                }
                                if(have_baseline&&player_worlds.contains(message->connection)&&(character_collision||physics)) {
                                    const auto requested_position=update.position;
                                    const auto requested_velocity=update.velocity;
                                    const auto world_id=player_worlds.at(message->connection);
                                    const std::array<float,3> horizontal_request{requested_position[0],requested_position[1],baseline[2]};
                                    const auto violation=net::character_displacement_violation(baseline,horizontal_request,elapsed_ticks*0.025);
                                    CharacterMotionState motion;
                                    if(const auto current=character_motions.find(message->connection);current!=character_motions.end())motion=current->second;
                                    if(!motion.initialized||motion.world_id!=world_id) {
                                        motion={};motion.initialized=true;motion.world_id=world_id;
                                        motion.position={baseline[0],baseline[1],baseline[2]};
                                        if(const auto previous=server_characters.latest(message->peer.steam_id()))
                                            motion.velocity={previous->velocity[0],previous->velocity[1],previous->velocity[2]};
                                        motion.grounded=character_is_grounded(world_id,motion.position);
                                    }
                                    const bool duplicate_tick=motion.has_client_tick&&update.tick==motion.client_tick;
                                    if(duplicate_tick) {
                                        update.position={static_cast<float>(motion.position.x),static_cast<float>(motion.position.y),static_cast<float>(motion.position.z)};
                                        if(update.state_type==3)update.velocity={static_cast<float>(motion.velocity.x),static_cast<float>(motion.velocity.y),static_cast<float>(motion.velocity.z)};
                                        if(std::abs(motion.position.x-requested_position[0])>0.002||
                                           std::abs(motion.position.y-requested_position[1])>0.002||
                                           std::abs(motion.position.z-requested_position[2])>0.002)
                                            corrected_position=update.position;
                                        if(update.state_type==3&&(
                                            std::abs(motion.velocity.x-requested_velocity[0])>0.05||
                                            std::abs(motion.velocity.y-requested_velocity[1])>0.05||
                                            std::abs(motion.velocity.z-requested_velocity[2])>0.05))
                                            corrected_velocity=update.velocity;
                                        accepted_motion=motion;
                                    } else {
                                        const double dt=std::clamp(elapsed_ticks*0.025,0.0,1.0);
                                        const Vec3 start=motion.position;
                                        double dx=double(requested_position[0])-start.x;
                                        double dy=double(requested_position[1])-start.y;
                                        if(!violation.empty())dx=dy=0;
                                        if(dt>0) {
                                            const double distance=std::hypot(dx,dy),maximum=character_profile.maximum_horizontal_speed*dt;
                                            if(distance>maximum&&distance>0){dx*=maximum/distance;dy*=maximum/distance;}
                                        } else dx=dy=0;
                                        const bool grounded=motion.grounded||character_is_grounded(world_id,start);
                                        const bool jumping=grounded&&update.state_type==3&&requested_velocity[2]>4.0f&&dt>0;
                                        const double initial_vz=grounded?(jumping?character_profile.jump_impulse:0.0):std::clamp(motion.velocity.z,-50.0,50.0);
                                        constexpr double gravity=9.81;
                                        const double dz=initial_vz*dt-0.5*gravity*dt*dt;
                                        double next_vz=std::clamp(initial_vz-gravity*dt,-50.0,50.0);
                                        auto moved=move_character_capsule(world_id,start,{dx,dy,dz});
                                        const double target_z=start.z+dz;
                                        if(moved.contacts&&dz>0&&moved.position.z<target_z-0.002)next_vz=0;
                                        bool next_grounded=false;
                                        const auto ground_probe=move_character_capsule(world_id,moved.position,{0,0,-0.06});
                                        if(ground_probe.contacts&&moved.position.z-ground_probe.position.z<0.058&&next_vz<=0) {
                                            moved.position.z=ground_probe.position.z;next_vz=0;next_grounded=true;
                                        } else if(moved.contacts&&dz<0&&moved.position.z>target_z+0.002) {
                                            next_vz=0;next_grounded=true;
                                        }
                                        motion.position=moved.position;
                                        motion.velocity={dt>0?(moved.position.x-start.x)/dt:0.0,
                                                         dt>0?(moved.position.y-start.y)/dt:0.0,next_vz};
                                        motion.grounded=next_grounded;motion.world_id=world_id;motion.initialized=true;
                                        motion.client_tick=update.tick;motion.has_client_tick=true;
                                        update.position={static_cast<float>(moved.position.x),static_cast<float>(moved.position.y),static_cast<float>(moved.position.z)};
                                        if(update.state_type==3)update.velocity={static_cast<float>(motion.velocity.x),static_cast<float>(motion.velocity.y),static_cast<float>(motion.velocity.z)};
                                        const bool position_changed=std::abs(moved.position.x-requested_position[0])>0.002||
                                            std::abs(moved.position.y-requested_position[1])>0.002||
                                            std::abs(moved.position.z-requested_position[2])>0.002;
                                        const bool velocity_changed=update.state_type==3&&(
                                            std::abs(motion.velocity.x-requested_velocity[0])>0.05||
                                            std::abs(motion.velocity.y-requested_velocity[1])>0.05||
                                            std::abs(motion.velocity.z-requested_velocity[2])>0.05);
                                        if(position_changed||!violation.empty())corrected_position=update.position;
                                        if(update.state_type==3&&(position_changed||velocity_changed||!violation.empty()))corrected_velocity=update.velocity;
                                        if(!violation.empty()&&game_events.size()<10000)
                                            game_events.push_back("{\"kind\":\"character-motion-corrected\",\"reason\":"+json_quote(violation)+",\"elapsed_seconds\":"+elapsed(started)+'}');
                                        accepted_motion=motion;
                                    }
                                }
                            }
                            const auto rejection=native_players?server_characters.observe(message->peer.steam_id(),update):character_states.observe(message->peer.steam_id(),update);if(!rejection.empty())throw std::runtime_error(rejection);
                            if(accepted_motion)character_motions.insert_or_assign(message->connection,*accepted_motion);
                            if(native_players) {
                                StoredPlayerLocation location;location.world_id=player_worlds.at(message->connection);
                                for(int axis=0;axis<3;++axis)location.position[axis]=update.position[axis];
                                player_locations.insert_or_assign(message->peer.steam_id(),location);active_player_locations.insert_or_assign(message->connection,location);dirty_locations.insert(message->peer.steam_id());
                            }
                        }
                        const auto previous_tick=latest_client_ticks.find(message->connection);
                        if(previous_tick==latest_client_ticks.end()||std::bit_cast<std::int32_t>(update.tick-previous_tick->second)>=0)
                            latest_client_ticks.insert_or_assign(message->connection,update.tick);
                        if(game_events.size()<10000)game_events.push_back("{\"kind\":\"character-update\",\"tick\":"+std::to_string(update.tick)+",\"acknowledgements\":"+std::to_string(update.acknowledgements)+",\"state_type\":"+std::to_string(update.state_type)+",\"character_id\":"+std::to_string(update.character_id)+",\"position\":["+std::to_string(update.position[0])+','+std::to_string(update.position[1])+','+std::to_string(update.position[2])+"],\"velocity\":["+std::to_string(update.velocity[0])+','+std::to_string(update.velocity[1])+','+std::to_string(update.velocity[2])+"] ,\"elapsed_seconds\":"+elapsed(started)+'}');
                        if(native_players&&update.has_character_state&&movement_sequence<64) {
                            const auto sequence=static_cast<std::uint8_t>(movement_sequence++);
                            for(const auto peer:game_started)if(peer!=message->connection&&!pending_close.contains(peer)&&visible_players[peer].contains(message->connection)) {
                                std::uint32_t acknowledged_tick=0;
                                if(const auto latest=latest_client_ticks.find(peer);latest!=latest_client_ticks.end())acknowledged_tick=latest->second;
                                const auto forwarded=net::compress_packet(net::make_server_character_update(plain,server_tick,acknowledged_tick,sequence,corrected_position,corrected_velocity));
                                std::int64_t number=-1;
                                net.send(net.sockets,peer,forwarded.data(),static_cast<std::uint32_t>(forwarded.size()),0,&number);
                            }
                            if((corrected_position||corrected_velocity)&&!pending_close.contains(message->connection)) {
                                const auto authoritative=net::compress_packet(net::make_server_character_update(plain,server_tick,update.tick,sequence,corrected_position,corrected_velocity));
                                std::int64_t number=-1;
                                if(net.send(net.sockets,message->connection,authoritative.data(),static_cast<std::uint32_t>(authoritative.size()),0,&number)!=1)
                                    pending_close.insert(message->connection);
                            }
                        }
                    } catch(const std::exception& error) {
                        ++rejected_commands;
                        if(game_events.size()<10000)game_events.push_back("{\"kind\":\"character-update-rejected\",\"error\":"+json_quote(error.what())+'}');
                    }
                }
                if(!native_command&&game_started.contains(message->connection)&&message->size>1){
                    const auto first=static_cast<const std::uint8_t*>(message->data)[0];
                    if(first!=1&&first!=6&&first!=9){
                        for(const auto other:game_started)if(other!=message->connection){
                            std::int64_t number=-1;
                            net.send(net.sockets,other,message->data,static_cast<std::uint32_t>(message->size),(message->flags&8)?8:0,&number);
                        }
                    }
                }
                ++total_messages;total_bytes+=message->size;
                if(messages.size()<10000&&captured_bytes+message->size<=64*1024*1024) {
                    std::ostringstream name;name<<std::setw(6)<<std::setfill('0')<<total_messages-1<<"-rx.bin";const auto file=directory/name.str();std::ofstream stream(file,std::ios::binary);if(!stream)throw std::runtime_error("Cannot create capture payload");if(message->size)stream.write(static_cast<const char*>(message->data),message->size);stream.close();if(!stream)throw std::runtime_error("Cannot finish capture payload");
                    captured_bytes+=message->size;messages.push_back("{\"file\":"+json_quote(name.str())+",\"bytes\":"+std::to_string(message->size)+",\"sha256\":"+json_quote(sha256(file))+",\"connection\":"+std::to_string(message->connection)+",\"number\":"+std::to_string(message->number)+",\"flags\":"+std::to_string(message->flags)+",\"reliable\":"+((message->flags&8)?"true":"false")+",\"lane\":"+std::to_string(message->lane)+",\"control\":"+(control_handled?"true":"false")+",\"elapsed_seconds\":"+elapsed(started)+'}');
                }
            }
            if(count<static_cast<int>(batch_messages.size()))break;
        }
        // Drain queued packets before evaluating inactivity: a delayed server
        // tick must not disconnect a peer whose heartbeat was already waiting.
        const auto activity_now=Clock::now();
        for(const auto& [peer,last]:last_peer_activity) {
            const auto limit=game_started.contains(peer)?std::chrono::seconds(45):std::chrono::seconds(120);
            if(activity_now-last>limit) {
                pending_close.insert(peer);
                if(game_events.size()<10000)game_events.push_back("{\"kind\":\"peer-inactivity-timeout\",\"connection\":"+std::to_string(peer)+'}');
            }
        }
        while(!pending_close.empty()){
            const auto connection=*pending_close.begin();pending_close.erase(pending_close.begin());
            if(native_players&&live_players.contains(connection)) {
                const auto identity=connection_steam_ids.find(connection);
                if(identity!=connection_steam_ids.end()) {
                    const auto& player=live_players.at(connection);
                    const auto packets=prepare_player_disconnect(player,server_tick);
                    for(const auto peer:game_started)if(peer!=connection&&!pending_close.contains(peer)&&visible_players[peer].contains(connection)) {
                        for(const auto& packet:packets)if(!send_game(peer,packet,"native-player-disconnect")) {
                            pending_close.insert(peer);desynchronized.insert(peer);break;
                        }
                    }
                    offline_players.insert_or_assign(identity->second,player);
                }
            }
            inventory.cancel(connection);
            pending_inventory_results.erase(connection);
            const auto identity=connection_steam_ids.find(connection);
            if(identity!=connection_steam_ids.end()) {const auto owner=identity->second;if(live_players.contains(connection))server_characters.despawn(owner);connection_steam_ids.erase(identity);const auto active=active_steam_connections.find(owner);if(active!=active_steam_connections.end()&&--active->second==0){active_steam_connections.erase(active);character_states.release(owner);}}
            live_players.erase(connection);
            player_worlds.erase(connection);
            active_player_locations.erase(connection);
            character_motions.erase(connection);
            latest_client_ticks.erase(connection);
            last_peer_activity.erase(connection);
            visible_players.erase(connection);visible_containers.erase(connection);for(auto& [peer,subjects]:visible_players){(void)peer;subjects.erase(connection);}
            net.close_connection(net.sockets,connection,1000,desynchronized.contains(connection)?"World update failed; reconnect to resynchronize":"scrapds probe complete",false);owned.connections.erase(connection);accepted.erase(connection);challenged.erase(connection);authenticated.erase(connection);gameinfo_sent.erase(connection);join_accepted.erase(connection);game_started.erase(connection);player_infos.erase(connection);
        }
        if(Clock::now()>=next_save){
            net.auth(net.sockets,&authentication);
            if(native_world)try{flush_locations();native_world->save_checkpoint(directory/L"world.db");}catch(const std::exception& error){std::cerr<<"scrapds: world checkpoint failed: "<<error.what()<<'\n';}
            try{save(false);}catch(const std::exception& error){std::cerr<<"scrapds: progress save failed: "<<error.what()<<'\n';}
            next_save=Clock::now()+std::chrono::seconds(30);
        }
        if(Clock::now()>=next_tick){
            if(!physics&&native_players)try{sync_character_collision(false);}catch(const std::exception& error){std::cerr<<"scrapds: character collision region update failed: "<<error.what()<<'\n';}
            if(physics)try{sync_native_physics(false);}catch(const std::exception& error){std::cerr<<"scrapds: physics region update failed: "<<error.what()<<'\n';}
            if(!physics&&overworld_terrain&&character_collision)try {
                std::vector<TerrainAnchor> anchors;anchors.reserve(active_player_locations.size());
                for(const auto& [peer,location]:active_player_locations){(void)peer;anchors.push_back({location.world_id,location.position[0],location.position[1],0});}
                if(overworld_terrain->stream(*character_collision,anchors,4))write_file(directory/L"overworld-terrain.json",overworld_terrain->summary_json());
            }catch(const std::exception& error){std::cerr<<"scrapds: character terrain stream failed: "<<error.what()<<'\n';}
            if(physics)try {
                if(overworld_terrain) {
                    std::vector<TerrainAnchor> anchors;anchors.reserve(active_player_locations.size()+physics->bodies().size()+1);
                    for(const auto& [peer,location]:active_player_locations){(void)peer;anchors.push_back({location.world_id,location.position[0],location.position[1],0});}
                    for(const auto& body:physics->bodies())anchors.push_back({body.world_id,body.position.x,body.position.y,1});
                    if(spawn_template)anchors.push_back({spawn_template->world_id,spawn_template->position[0],spawn_template->position[1],2});
                    if(overworld_terrain->stream(*physics,anchors,4))write_file(directory/L"overworld-terrain.json",overworld_terrain->summary_json());
                }
                physics->step(1);physics->writeback(*native_world);++physics_ticks;
                const auto bodies=physics->bodies();
                if(!bodies.empty()&&!game_started.empty()) {
                    const auto count=std::min<std::size_t>(bodies.size(),64u-movement_sequence);
                    for(std::size_t index=0;index<count;++index) {
                        const auto& body=bodies[(physics_cursor+index)%bodies.size()];
                        net::ServerBodyUpdate update;update.server_tick=server_tick;update.sequence=static_cast<std::uint8_t>(movement_sequence++);
                        update.body_id=body.id;update.flags=physics_flags.at(body.id);update.has_velocity=true;
                        update.rotation={float(body.qx),float(body.qy),float(body.qz),float(body.qw)};
                        update.position={float(body.position.x),float(body.position.y),float(body.position.z)};
                        update.velocity={float(body.velocity.x),float(body.velocity.y),float(body.velocity.z)};
                        update.angular_velocity={float(body.angular_velocity.x),float(body.angular_velocity.y),float(body.angular_velocity.z)};
                        for(const auto peer:game_started) {
                            const auto world=player_worlds.find(peer);if(world==player_worlds.end()||world->second!=body.world_id)continue;
                            if(const auto tick=latest_client_ticks.find(peer);tick!=latest_client_ticks.end())update.acknowledged_client_tick=tick->second;
                            else update.acknowledged_client_tick=0;
                            const auto encoded=net::compress_packet(net::make_server_body_update(update));std::int64_t number=-1;
                            net.send(net.sockets,peer,encoded.data(),static_cast<std::uint32_t>(encoded.size()),0,&number);
                        }
                    }
                    physics_cursor=(physics_cursor+count)%bodies.size();
                }
            }catch(const std::exception& e){physics.reset();std::cerr<<"scrapds: physics disabled after simulation error: "<<e.what()<<'\n';try{write_file(directory/L"physics.json","{\"active\":false,\"error\":"+json_quote(e.what())+'}');}catch(const std::exception&) {}}
            // Client dispatcher handles ID22 as a ticked NetObj packet. An
            // empty record list is accepted once ID21 has initialized the
            // receiver. ID30 is client-to-server input, not a client tick.
            if(replay) {
                const auto tick_wire=net::compress_packet(net::make_net_object_packet({22,server_tick,{}}));
                for(const auto connection:game_started){
                    std::int64_t number=-1;
                    const auto sent=net.send(net.sockets,connection,tick_wire.data(),static_cast<std::uint32_t>(tick_wire.size()),8,&number);
                    net.flush(net.sockets,connection);
                    if(sent!=1)deferred_close.insert(connection);
                }
            }
            // Client runtime advances its game tick once per 0.025 seconds
            // (47BCC0); keep the advertised server clock at the same rate.
            if(native_world)native_world->advance_ticks(1);
            server_tick+=1u;
            movement_sequence=0;
            next_tick+=std::chrono::milliseconds(25);
            // Preserve the fixed clock across brief delays while bounding catch-up
            // after a long stall. Network callbacks are serviced between ticks.
            const auto now=Clock::now();
            if(next_tick<now-std::chrono::milliseconds(250))
                next_tick=now-std::chrono::milliseconds(250);
        }
        const auto now=Clock::now();
        if(next_tick>now)
            std::this_thread::sleep_until(std::min(next_tick,now+std::chrono::milliseconds(10)));
    }
    if(native_world){flush_locations();native_world->save_checkpoint(directory/L"world.db");}
    if(native_physics)write_file(directory/L"physics-final.json","{\"ticks\":"+std::to_string(physics_ticks)+",\"active\":"+(physics?"true":"false")+",\"coverage\":"+(physics?physics->summary_json():"null")+'}');
    finished=true;net.auth(net.sockets,&authentication);save(true);
    const auto result="{\"identity\":"+session.json()+",\"listener\":"+report()+'}';write_file(directory/L"session.json",result);return result;
}
} // namespace scrapds
#else
namespace scrapds {
std::string probe_steam_identity(const std::filesystem::path&,int){throw std::runtime_error("Steam identity probe currently requires Windows x64 and the verified installed DLL; Linux Steam ABI is not validated");}
std::string run_steam_listener(const std::filesystem::path&,const std::filesystem::path&,std::uint64_t,std::uint64_t,int,bool,bool,const std::filesystem::path&,const std::filesystem::path&,bool,bool,bool){throw std::runtime_error("Steam listener probe currently requires Windows x64 and the verified installed DLL; Linux Steam ABI is not validated");}
}
#endif
