#include "net/character.hpp"
#include "net/world_sync.hpp"
#include "net/player_info.hpp"
#include "net/codec.hpp"
#include "steam/inventory.hpp"
#include "game/player_spawn.hpp"

#include <algorithm>
#include <bit>
#include <iostream>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
struct FakeInventory {int state{22},destroyed{};std::uint64_t owner{};};
bool inventory_deserialize(void*,std::int32_t* handle,const void*,std::uint32_t size,bool reserved){*handle=7;return size==3&&!reserved;}
int inventory_status(void* instance,std::int32_t){return static_cast<FakeInventory*>(instance)->state;}
bool inventory_owner(void* instance,std::int32_t,std::uint64_t owner){return static_cast<FakeInventory*>(instance)->owner==owner;}
void inventory_destroy(void* instance,std::int32_t){++static_cast<FakeInventory*>(instance)->destroyed;}
void append_bits(std::vector<std::uint8_t>& packet,std::size_t& bit,std::uint32_t value,unsigned count) {
    for(unsigned i=0;i<count;++i,++bit) {
        if(packet.size()*8<=bit)packet.push_back(0);
        const auto one=(value>>(count-i-1))&1u;
        packet[bit/8]|=static_cast<std::uint8_t>(one<<(7-(bit%8)));
    }
}
std::vector<std::uint8_t> tumble_fixture(std::uint32_t character_id=1,std::uint32_t rotation_magnitude=0) {
    std::vector<std::uint8_t> packet{0x1e};std::size_t bit{8};
    const std::uint32_t raw_tick=558439u<<1;
    append_bits(packet,bit,raw_tick&0xff,8);
    append_bits(packet,bit,(raw_tick>>8)&0xff,8);
    append_bits(packet,bit,(raw_tick>>16)&0x1f,5);
    append_bits(packet,bit,0,7); // no acknowledgements
    append_bits(packet,bit,4,4); // CharacterTumble
    append_bits(packet,bit,0,2); // omitted quaternion component
    for(int i=0;i<3;++i) {
        append_bits(packet,bit,0,1);
        const auto value=i==0?rotation_magnitude:0u;
        append_bits(packet,bit,value&0xff,8);append_bits(packet,bit,(value>>8)&0x1f,5);
    }
    for(int i=0;i<3;++i)append_bits(packet,bit,0,28);
    for(int i=0;i<6;++i)append_bits(packet,bit,0,24);
    do {
        const auto part=character_id&0x3fff;character_id>>=14;
        const auto raw_id=(part<<1)|(character_id?1u:0u);
        append_bits(packet,bit,raw_id&0xff,8);append_bits(packet,bit,(raw_id>>8)&0x7f,7);
    } while(character_id);
    return packet;
}
}

int main() {
    try {
        const std::vector<std::uint8_t> heartbeat{0x1e,0xce,0x0a,0x88,0x00};
        scrapds::net::CharacterUpdate empty;
        try { empty=scrapds::net::parse_character_update(heartbeat); }
        catch(const std::exception& e) { throw std::runtime_error(std::string("heartbeat: ")+e.what()); }
        if(empty.tick!=558439 || empty.acknowledgements!=0 || empty.state_type!=0 || empty.has_character_state)
            throw std::runtime_error("captured heartbeat decode mismatch");
        { std::vector<std::uint8_t> overflow{0x1e};std::size_t offset=8;
          for(const auto raw:{1u,8192u}) {append_bits(overflow,offset,raw&255u,8);append_bits(overflow,offset,(raw>>8)&255u,8);append_bits(overflow,offset,(raw>>16)&31u,5);}
          append_bits(overflow,offset,0,11);bool rejected=false;
          try{(void)scrapds::net::parse_character_update(overflow);}catch(const std::runtime_error&){rejected=true;}
          if(!rejected)throw std::runtime_error("overflowing character tick was accepted"); }
        const std::vector<std::uint8_t> movement{0x1e,0xd6,0x0a,0x88,0x03,0,0,0,0,0x40,0,0,0,0x20,0,0,0x02,0x00,0xa9,0x70,0x00,0x7f,0xfd,0xff,0xf2,0x70,0x80,0x40,0x00};
        scrapds::net::CharacterUpdate state;
        try { state=scrapds::net::parse_character_update(movement); }
        catch(const std::exception& e) { throw std::runtime_error(std::string("movement: ")+e.what()); }
        if(!state.has_character_state) throw std::runtime_error("captured movement had no character state");
        if(state.character_id!=1) throw std::runtime_error("captured movement character ID mismatch");
        const std::array<float,3> origin{};
        const std::array<std::uint8_t,16> known_character_uuid{0x04,0x76,0x1b,0x4a,0xa8,0x3e,0x47,0x36,0xb5,0x65,0x12,0x0b,0xc7,0x76,0xed,0xb2};
        if(scrapds::net::format_character_asset_uuid(known_character_uuid)!="04761b4a-a83e-4736-b565-120bc776edb2")throw std::runtime_error("Character asset UUID text conversion mismatch");
        if(!scrapds::net::character_displacement_violation(origin,{0.45f,0,0},0.025).empty())throw std::runtime_error("allowed horizontal motion was rejected");
        if(scrapds::net::character_displacement_violation(origin,{0.451f,0,0},0.025)!="character-horizontal-speed-limit")throw std::runtime_error("excess horizontal motion was accepted");
        if(!scrapds::net::character_displacement_violation(origin,{0,0,0.5f},0.025).empty())throw std::runtime_error("jump impulse displacement was rejected");
        if(scrapds::net::character_displacement_violation(origin,{0,0,0.501f},0.025)!="character-upward-speed-limit")throw std::runtime_error("upward teleport was accepted");
        if(scrapds::net::character_displacement_violation(origin,{0,0,-1.551f},0.025)!="character-fall-speed-limit")throw std::runtime_error("downward teleport was accepted");
        const auto server_wire=scrapds::net::make_server_character_update(movement,558600,state.tick,7);
        const auto server_decoded=scrapds::net::parse_server_character_update(server_wire);
        if(server_decoded.server_tick!=558600||server_decoded.client_tick!=state.tick||server_decoded.sequence!=7||server_decoded.character.character_id!=state.character_id||server_decoded.character.position!=state.position)
            throw std::runtime_error("server Character framing roundtrip mismatch");
        const std::array<float,3> corrected{state.position[0]+0.75f,state.position[1]-0.5f,state.position[2]+0.25f};
        const auto corrected_wire=scrapds::net::make_server_character_update(movement,558600,state.tick,7,corrected);
        const auto corrected_state=scrapds::net::parse_server_character_update(corrected_wire).character;
        for(std::size_t i=0;i<3;++i)if(std::abs(corrected_state.position[i]-corrected[i])>0.0001f)
            throw std::runtime_error("server Character authoritative position encoding mismatch");
        const std::array<float,3> corrected_velocity{1.25f,-2.5f,3.75f};
        const auto corrected_motion=scrapds::net::parse_server_character_update(
            scrapds::net::make_server_character_update(movement,558600,state.tick,7,corrected,corrected_velocity)).character;
        for(std::size_t i=0;i<3;++i)if(std::abs(corrected_motion.velocity[i]-corrected_velocity[i])>0.002f)
            throw std::runtime_error("server Character authoritative velocity encoding mismatch");
        const auto observer=scrapds::net::parse_server_character_update(scrapds::net::make_server_character_update(movement,558600,558510,2));
        if(observer.client_tick!=558510||observer.character.position!=state.position)throw std::runtime_error("server Character frame did not preserve the observer's acknowledged tick");
        {
            // First DynamicBody record from a real host's ID24 packet.
            const std::vector<std::uint8_t> captured{0x18,0x00,0x0c,0x8f,0xfc,0x5c,0x44,0x08,0x61,0x05,0x0c,0x03,0x58,0xae,0x36,0x2c,0x48,0x05,0x9e,0x41,0x80,0xa6,0x8a,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x00,0x2f,0xf0,0x21};
            const auto body=scrapds::net::parse_server_body_update(captured);
            if(body.server_tick!=558592||body.acknowledged_client_tick!=0||body.sequence!=2||!body.has_velocity||body.flags!=5||body.body_id!=383)
                throw std::runtime_error("captured DynamicBody header mismatch");
            const auto extracted=scrapds::net::extract_server_body_updates(captured);
            if(extracted.empty()||extracted.front().body_id!=383||extracted.front().flags!=5)throw std::runtime_error("captured DynamicBody records not extracted");
            const auto encoded=scrapds::net::make_server_body_update(body);
            if(encoded.size()!=captured.size())throw std::runtime_error("DynamicBody frame size mismatch");
            for(std::size_t bit=8;bit<8+348;++bit)if(((captured[bit/8]^encoded[bit/8])&(0x80u>>(bit%8)))!=0)throw std::runtime_error("DynamicBody frame differs from host record");
            auto bad=body;bad.rotation={0,0,0,0};bool rejected=false;try{(void)scrapds::net::make_server_body_update(bad);}catch(const std::invalid_argument&){rejected=true;}
            if(!rejected)throw std::runtime_error("zero body quaternion accepted");
        }
        {
            // Captured sleeping-body update (type 2, without velocity).
            const std::vector<std::uint8_t> captured{0x18,0x00,0x0c,0x8f,0xfc,0x5c,0x44,0x04,0x89,0x33,0x92,0xb7,0x80,0xe3,0x52,0x40,0x5a,0x0e,0xd7,0x61,0x00,0xbe,0x24,0x00,0x2c,0x50,0x62};
            const auto body=scrapds::net::parse_server_body_update(captured);
            if(body.server_tick!=558592||body.acknowledged_client_tick!=0||body.sequence!=1||body.has_velocity||body.flags!=5||body.body_id!=837)throw std::runtime_error("captured sleeping body mismatch");
            const auto encoded=scrapds::net::make_server_body_update(body);
            for(std::size_t bit=8;bit<8+204;++bit)if(((captured[bit/8]^encoded[bit/8])&(0x80u>>(bit%8)))!=0)throw std::runtime_error("sleeping body frame differs from host record");
        }
        {
            const std::vector<std::uint8_t> bytes{0x18,0x02,0x0c,0x88,0x18,0x64,0x44,0x00,0xc0,0x00,0x00,0x00,0x10,0x08,0x00,0x00,0x08,0x00,0x00,0x00,0x80,0x2a,0x5c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00};
            const auto actual=scrapds::net::parse_server_character_update(bytes);
            if(actual.server_tick!=558593||actual.client_tick!=0||actual.sequence!=0||actual.character.character_id!=1||std::fabs(actual.character.position[0]-16.0f)>1e-4f||std::fabs(actual.character.position[2]-0.72003174f)>1e-4f)
                throw std::runtime_error("captured host Character update mismatch");
            // Reframe the recorded 190-bit state as a client message, then
            // rebuild the server packet and compare every captured byte.
            std::vector<std::uint8_t> source{0x1e};std::size_t offset=8;
            append_bits(source,offset,0,21);append_bits(source,offset,0,7);
            for(std::size_t i=54;i<244;++i)append_bits(source,offset,(bytes[1+i/8]>>(7-i%8))&1u,1);
            if(scrapds::net::make_server_character_update(source,558593,0)!=bytes)
                throw std::runtime_error("server Character encoder differs from captured host packet");
        }
        {
            const std::vector<std::uint8_t> bytes{0x18,0x16,0x0c,0x8c,0x58,0x08,0x01,0x80,0x00,0x00,0x00,0x20,0x10,0x00,0x00,0x10,0x00,0x00,0x01,0x00,0x54,0xb8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00};
            const auto actual=scrapds::net::parse_server_character_update(bytes);
            if(actual.server_tick!=558603||actual.client_tick!=558440||actual.sequence!=0||actual.character.character_id!=1)throw std::runtime_error("second host Character header mismatch");
            std::vector<std::uint8_t> source{0x1e};std::size_t offset=8;const auto raw=558440u<<1;
            append_bits(source,offset,raw&255,8);append_bits(source,offset,(raw>>8)&255,8);append_bits(source,offset,(raw>>16)&31,5);append_bits(source,offset,0,7);
            for(std::size_t i=45;i<235;++i)append_bits(source,offset,(bytes[1+i/8]>>(7-i%8))&1u,1);
            if(scrapds::net::make_server_character_update(source,558603,558440)!=bytes)throw std::runtime_error("server Character encoder differs from second host packet");
        }
        if(std::fabs(state.position[0]-16.0f)>1e-5f||std::fabs(state.position[1]-16.0f)>1e-5f||std::fabs(state.position[2]-0.72003174f)>1e-5f)
            throw std::runtime_error("captured movement position mismatch");
        if(std::fabs(state.velocity[0]-10.0f)>0.002f||std::fabs(state.velocity[1]-10.0f)>0.002f||std::fabs(state.velocity[2]-0.7203f)>0.002f)
            throw std::runtime_error("captured movement velocity mismatch");
        const auto tumble=scrapds::net::parse_character_update(tumble_fixture());
        if(!tumble.has_character_state||tumble.state_type!=4||tumble.character_id!=1||std::fabs(tumble.rotation[0]-1.0f)>1e-6f)
            throw std::runtime_error("tumble quaternion fixture decode mismatch");
        const std::array<float,3> corrected_tumble{tumble.position[0]+0.5f,tumble.position[1]-0.25f,tumble.position[2]+1.0f};
        const auto tumble_authoritative=scrapds::net::parse_server_character_update(
            scrapds::net::make_server_character_update(tumble_fixture(),558600,tumble.tick,3,corrected_tumble)).character;
        if(tumble_authoritative.state_type!=4||tumble_authoritative.rotation!=tumble.rotation)
            throw std::runtime_error("authoritative tumble update changed its rotation");
        for(std::size_t i=0;i<3;++i)if(std::abs(tumble_authoritative.position[i]-corrected_tumble[i])>0.0001f)
            throw std::runtime_error("authoritative tumble position encoding mismatch");
        const auto rotated=scrapds::net::parse_character_update(tumble_fixture(1,4096));
        if(std::fabs(rotated.rotation[1]-0.3535965f)>1e-5f||std::fabs(rotated.rotation[0]-std::sqrt(1.0f-rotated.rotation[1]*rotated.rotation[1]))>1e-5f)
            throw std::runtime_error("tumble quaternion scale mismatch");
        if(scrapds::net::parse_character_update(tumble_fixture(0x1234)).character_id!=0x1234)throw std::runtime_error("multi-byte Character ID byte order mismatch");
        if(scrapds::net::parse_character_update(tumble_fixture(0x123456)).character_id!=0x123456)throw std::runtime_error("two-group Character ID mismatch");
        for(const auto id:{0u,0x10000000u}) {
            bool rejected=false;try{(void)scrapds::net::parse_character_update(tumble_fixture(id));}catch(const std::runtime_error&){rejected=true;}
            if(!rejected)throw std::runtime_error("invalid Character ID accepted");
        }
        scrapds::net::CharacterOwnership ownership;
        constexpr std::uint64_t steam_a=1ull,steam_b=2ull;
        FakeInventory fake;fake.owner=steam_a;
        {scrapds::steam::InventoryValidation inventory({&fake,inventory_deserialize,inventory_status,inventory_owner,inventory_destroy});
         const std::vector<std::uint8_t> serialized{1,2,3};
         if(!inventory.begin(11,steam_a,serialized)||inventory.begin(11,steam_a,serialized)||!inventory.poll().empty())throw std::runtime_error("inventory validation pending state failed");
         fake.state=1;const auto outcome=inventory.poll();
         if(outcome.size()!=1||outcome[0].connection!=11||!outcome[0].owner_valid||fake.destroyed!=1)throw std::runtime_error("inventory owner validation failed");
         fake.state=27;if(!inventory.begin(12,steam_b,serialized))throw std::runtime_error("expired inventory request failed");
         const auto expired=inventory.poll();if(expired.size()!=1||expired[0].status!=27||expired[0].owner_valid||fake.destroyed!=2)throw std::runtime_error("foreign inventory was accepted");
         fake.state=22;if(!inventory.begin(13,steam_a,serialized))throw std::runtime_error("pending inventory request failed");inventory.cancel(13);
         if(fake.destroyed!=3)throw std::runtime_error("inventory handle not released on disconnect");}
        if(!ownership.claim(steam_a,state.character_id)||ownership.claim(steam_a,state.character_id+1)||ownership.claim(steam_b,state.character_id))
            throw std::runtime_error("character ownership binding failed");
        if(ownership.claim(steam_a+(1ull<<32),state.character_id))
            throw std::runtime_error("SteamID ownership was truncated to a 32-bit handle");
        ownership.release(steam_a);
        if(!ownership.claim(steam_b,state.character_id)||ownership.size()!=1)
            throw std::runtime_error("character ownership release failed");
        scrapds::net::CharacterStateTracker tracker;
        if(!tracker.observe(steam_a,state).empty())throw std::runtime_error("first character state was rejected");
        auto newer=state;newer.tick=state.tick+2;
        if(!tracker.observe(steam_a,newer).empty())throw std::runtime_error("newer character tick was rejected");
        if(tracker.observe(steam_a,state)!="stale-character-tick")throw std::runtime_error("stale character state was accepted");
        auto hijack=state;hijack.tick=newer.tick+2;hijack.character_id++;
        if(tracker.observe(steam_a,hijack)!="peer-changed-character-id")throw std::runtime_error("peer changed bound character ID");
        if(tracker.observe(steam_b,newer)!="character-id-owned-by-another-peer")throw std::runtime_error("duplicate character owner was accepted");
        tracker.release(steam_a);
        if(!tracker.observe(steam_b,newer).empty())throw std::runtime_error("released character ID remained owned");
        auto wrap=newer;wrap.character_id=777;wrap.tick=0xfffffffeu;
        if(!tracker.observe(22,wrap).empty())throw std::runtime_error("initial wrap fixture rejected");
        wrap.tick=1;
        if(!tracker.observe(22,wrap).empty())throw std::runtime_error("forward uint32 tick wrap was rejected");
        wrap.tick=0xfffffffdu;
        if(tracker.observe(22,wrap)!="stale-character-tick")throw std::runtime_error("stale tick across uint32 wrap was accepted");
        scrapds::net::ServerCharacters server_characters;
        std::vector<std::uint8_t> player_info{9,0,3,'B','o','b',0,0,0,2,1,2};
        for(int i=0;i<32;++i)player_info.push_back(std::uint8_t(i));
        player_info.insert(player_info.end(),{0x11,0x22,0x33,0x44,0xaa,0xbb,0xcc,0xdd,0,0,0,1,0,0,0,3,0xde,0xad,0xbe});
        const auto player=scrapds::net::parse_player_info(player_info);
        if(player.name!="Bob"||player.appearance_type!=1||player.appearance.size()!=2||player.appearance[0].uuid_memory[15]!=15||player.appearance[1].uuid_memory[0]!=16||player.appearance[0].color!=0x11223344||player.appearance[1].color!=0xaabbccdd||player.inventory_kind!=1||player.inventory_result!=std::vector<std::uint8_t>({0xde,0xad,0xbe}))
            throw std::runtime_error("player info fields decoded incorrectly");
        for(std::size_t size=0;size<player_info.size();++size) {
            bool truncated=false;try {scrapds::net::parse_player_info(std::span(player_info).first(size));}catch(const std::invalid_argument&){truncated=true;}
            if(!truncated)throw std::runtime_error("truncated player info accepted");
        }
        auto extra_player_info=player_info;extra_player_info.push_back(0);
        bool trailing=false;try {scrapds::net::parse_player_info(extra_player_info);}catch(const std::invalid_argument&){trailing=true;}
        if(!trailing)throw std::runtime_error("trailing player info accepted");
        scrapds::net::PlayerSnapshot snapshot;snapshot.player_id=0x01020304;snapshot.character_id=9;snapshot.owner_steam_id=0x0102030405060708ull;
        snapshot.state_words={1,2,3};snapshot.flags=2;snapshot.info=player;
        const auto player_entry=scrapds::net::make_player_blob_entry(snapshot);
        if(player_entry.uid!=std::array<std::uint8_t,16>{0x51,0x86,0x88,0x83,0xd2,0xd2,0x49,0x53,0x91,0x35,0x1a,0xb0,0xbd,0xc2,0xa4,0x7e}||player_entry.key!=std::vector<std::uint8_t>({4,3,2,1})||player_entry.world_id!=0xfffe||player_entry.flags!=13)throw std::runtime_error("player BlobData key mismatch");
        std::vector<std::uint8_t> wrapped{0};wrapped.insert(wrapped.end(),player_entry.payload.begin(),player_entry.payload.end());
        auto actual_player=scrapds::net::decompress_packet(wrapped);actual_player.erase(actual_player.begin());
        std::vector<std::uint8_t> player_oracle{0,0,0,9,1,2,3,4,5,6,7,8,0,0,0,1,0,0,0,2,0,0,0,3,2,3,'B','o','b',0,0,0,2,1,9};
        for(int i=0;i<32;++i)player_oracle.push_back(std::uint8_t(i));player_oracle.insert(player_oracle.end(),7*16,0);
        player_oracle.insert(player_oracle.end(),{0x11,0x22,0x33,0x44,0xaa,0xbb,0xcc,0xdd});player_oracle.insert(player_oracle.end(),7*4,0);
        player_oracle.insert(player_oracle.end(),{0,0,0,1,0,0,0,3,0xde,0xad,0xbe});
        if(actual_player!=player_oracle)throw std::runtime_error("player BlobData payload mismatch");
        const auto player_packet=scrapds::net::parse_blob_data_packet(scrapds::net::make_blob_data_packet({13,17,{player_entry}}));
        if(player_packet.entries.size()!=1||player_packet.entries[0].payload!=player_entry.payload||player_packet.entries[0].key!=player_entry.key)throw std::runtime_error("player BlobData framing mismatch");
        const auto capture=std::filesystem::path(__FILE__).parent_path().parent_path()/"artifacts/world-sync/replay/post/post-002-id13-67184.bin";
        const auto captured_info=std::filesystem::path(__FILE__).parent_path().parent_path()/"artifacts/world-sync/replay/gameinfo.bin";
        if(std::filesystem::exists(captured_info)) {
            std::ifstream stream(captured_info,std::ios::binary);std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(stream)),{});
            const auto filtered=scrapds::strip_game_info_players(original);
            if(original.size()!=120||filtered.size()!=98||original[52]!=3||filtered[52]!=2)throw std::runtime_error("GameInfo retained stale player key or wrong count");
            auto truncated=original;truncated.resize(30);bool rejected=false;try{(void)scrapds::strip_game_info_players(truncated);}catch(const std::invalid_argument&){rejected=true;}
            if(!rejected)throw std::runtime_error("truncated GameInfo accepted");
        }
        if(std::filesystem::exists(capture)) {
            std::ifstream file(capture,std::ios::binary);std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),{});
            const auto packet=scrapds::net::parse_blob_data_packet(data);
            const auto it=std::find_if(packet.entries.begin(),packet.entries.end(),[&](const auto& entry){return entry.uid==player_entry.uid;});
            if(it==packet.entries.end()||it->key.size()!=4)throw std::runtime_error("captured Player BlobData is missing");
            std::vector<std::uint8_t> body{0};body.insert(body.end(),it->payload.begin(),it->payload.end());body=scrapds::net::decompress_packet(body);body.erase(body.begin());
            if(body.size()<26||body.size()<26u+body[25])throw std::runtime_error("captured Player payload is truncated");
            auto be=[&](std::size_t at,unsigned size){std::uint64_t value=0;for(unsigned i=0;i<size;++i)value=(value<<8)|body.at(at+i);return value;};
            std::vector<std::uint8_t> info_wire{9,0,body[25]};info_wire.insert(info_wire.end(),body.begin()+26,body.end());
            scrapds::net::PlayerSnapshot observed;observed.player_id=std::uint32_t(it->key[0])|(std::uint32_t(it->key[1])<<8)|(std::uint32_t(it->key[2])<<16)|(std::uint32_t(it->key[3])<<24);
            observed.character_id=std::uint32_t(be(0,4));observed.owner_steam_id=be(4,8);for(int i=0;i<3;++i)observed.state_words[i]=std::uint32_t(be(12+4*i,4));observed.flags=body[24];observed.info=scrapds::net::parse_player_info(info_wire);
            const auto encoded=scrapds::net::make_player_blob_entry(observed);std::vector<std::uint8_t> encoded_body{0};encoded_body.insert(encoded_body.end(),encoded.payload.begin(),encoded.payload.end());encoded_body=scrapds::net::decompress_packet(encoded_body);encoded_body.erase(encoded_body.begin());
            if(encoded_body!=body||encoded.key!=it->key||encoded.world_id!=it->world_id||encoded.flags!=it->flags)throw std::runtime_error("captured Player snapshot reconstructed incorrectly");
            const auto filtered_player=scrapds::strip_replay_players(data);
            if(filtered_player)for(const auto& entry:scrapds::net::parse_blob_data_packet(*filtered_player).entries)if(entry.uid==scrapds::net::player_blob_uid)throw std::runtime_error("captured Player was not removed from replay");
            std::cout<<"captured player snapshot verified\n";
        }
        scrapds::net::CharacterCreation creation;
        creation.owner_steam_id=0x0102030405060708ull;creation.position={1.0f,2.0f,3.0f};creation.world_id=0x1234;
        creation.orientation_words={0x11223344,0xaabbccdd};creation.flag=true;
        for(std::size_t i=0;i<creation.asset_uuid_memory.size();++i)creation.asset_uuid_memory[i]=std::uint8_t(i);
        const std::vector<std::uint8_t> creation_oracle{
            1,2,3,4,5,6,7,8, 0x40,0x40,0,0, 0x40,0,0,0, 0x3f,0x80,0,0, 0x12,0x34,
            0x11,0x22,0x33,0x44,0xaa,0xbb,0xcc,0xdd,
            15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,0x80};
        if(scrapds::net::serialize_character_creation(creation)!=creation_oracle)throw std::runtime_error("character creation byte order mismatch");
        const auto create_record=scrapds::net::make_character_creation(37,creation);
        const auto create_wire=scrapds::net::make_net_object_packet({22,19,{create_record}});
        const auto create_parsed=scrapds::net::parse_net_object_packet(create_wire);
        if(create_parsed.records.size()!=1||create_parsed.records[0].operation!=1||create_parsed.records[0].type!=6||create_parsed.records[0].subtype!=0||create_parsed.records[0].id!=37||create_parsed.records[0].payload!=creation_oracle)
            throw std::runtime_error("character creation NetObj framing mismatch");
        scrapds::net::CharacterInitialState initial;initial.active=true;initial.player_id=3;
        const std::vector<std::uint8_t> state_oracle{0xf0,0x07,0xff,0xff,0xff,0xf8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x07,0xff,0xff,0xff,0xfc,0,0,0,0x0c};
        const auto initial_record=scrapds::net::make_character_state(1,initial);
        if(initial_record.operation!=3||initial_record.type!=6||initial_record.payload!=state_oracle)throw std::runtime_error("captured initial Character state differs");
        const auto character_capture=std::filesystem::path(__FILE__).parent_path().parent_path()/"artifacts/world-sync/replay/post/post-003-id29-67185.bin";
        if(std::filesystem::exists(character_capture)) {
            std::ifstream file(character_capture,std::ios::binary);std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),{});
            bool found_create=false,found_state=false;
            for(const auto& child:scrapds::net::split_world_aggregate(data)) {
                if(child.empty()||(child[0]!=21&&child[0]!=22))continue;
                for(const auto& record:scrapds::net::parse_net_object_packet(child).records) {
                    if(record.type!=6||record.id!=1)continue;
                    if(record.operation==3) {if(record.payload!=state_oracle)throw std::runtime_error("captured Character state changed");found_state=true;}
                    if(record.operation!=1)continue;
                    if(record.payload.size()!=47)throw std::runtime_error("captured Character creation length mismatch");
                    const auto& p=record.payload;auto be=[&](std::size_t at,unsigned n){std::uint64_t value=0;for(unsigned i=0;i<n;++i)value=(value<<8)|p.at(at+i);return value;};
                    scrapds::net::CharacterCreation actual;actual.owner_steam_id=be(0,8);
                    for(int i=0;i<3;++i)actual.position[2-i]=std::bit_cast<float>(std::uint32_t(be(8+i*4,4)));
                    actual.world_id=std::uint16_t(be(20,2));actual.orientation_words={std::uint32_t(be(22,4)),std::uint32_t(be(26,4))};
                    std::reverse_copy(p.begin()+30,p.begin()+46,actual.asset_uuid_memory.begin());actual.flag=(p[46]&0x80)!=0;
                    if(scrapds::net::make_character_creation(record.id,actual).payload!=p)throw std::runtime_error("captured Character creation reconstructed incorrectly");
                    found_create=true;
                }
            }
            if(!found_create||!found_state)throw std::runtime_error("captured Character records are missing");
            const auto template_spawn=scrapds::find_replay_spawn(data);
            if(!template_spawn||template_spawn->world_id!=0||template_spawn->position[0]!=16.0f||template_spawn->position[1]!=16.0f||!template_spawn->flag)throw std::runtime_error("captured spawn template not recovered");
            const auto filtered=scrapds::strip_replay_players(data);
            if(filtered)for(const auto& child:scrapds::net::split_world_aggregate(*filtered))if(!child.empty()&&(child[0]==21||child[0]==22))for(const auto& record:scrapds::net::parse_net_object_packet(child).records)if(record.type==6)throw std::runtime_error("captured Character was not removed from replay");
            std::cout<<"captured character spawn verified\n";
        }
        const auto first_id=server_characters.spawn(steam_a),second_id=server_characters.spawn(steam_b);
        if(first_id!=1||second_id!=2||server_characters.size()!=2)throw std::runtime_error("server character allocator sequence mismatch");
        auto owned=state;owned.character_id=first_id;
        if(server_characters.observe(steam_b,owned)!="character-not-owned-by-peer"||!server_characters.observe(steam_a,owned).empty())throw std::runtime_error("server character owner check failed");
        auto old=owned;old.tick--;
        if(server_characters.observe(steam_a,old)!="stale-character-tick")throw std::runtime_error("server accepted stale character state");
        auto tick_start=server_characters.tick_start_position(steam_a);
        if(!tick_start||*tick_start!=owned.position)throw std::runtime_error("server did not retain the character tick start position");
        auto same_tick=owned;same_tick.position[0]+=0.2f;
        if(!server_characters.observe(steam_a,same_tick).empty())throw std::runtime_error("server rejected a same-tick character sample");
        tick_start=server_characters.tick_start_position(steam_a);
        if(!tick_start||*tick_start!=owned.position)throw std::runtime_error("same-tick sample moved the anti-drift baseline");
        auto drifted=same_tick;drifted.position[0]+=0.2f;
        if(scrapds::net::character_displacement_violation(*tick_start,drifted.position,0.0)!="character-horizontal-speed-limit")throw std::runtime_error("repeated same-tick samples accumulated movement tolerance");
        auto next_tick=same_tick;++next_tick.tick;
        if(!server_characters.observe(steam_a,next_tick).empty())throw std::runtime_error("server rejected the next character tick");
        tick_start=server_characters.tick_start_position(steam_a);
        if(!tick_start||*tick_start!=next_tick.position)throw std::runtime_error("new character tick did not advance the movement baseline");
        server_characters.despawn(steam_a);
        if(server_characters.observe(steam_a,owned)!="character-not-spawned"||server_characters.spawn(steam_a)!=3)throw std::runtime_error("despawn reused a character ID");
        bool duplicate_spawn=false;try {server_characters.spawn(steam_a);}catch(const std::runtime_error&){duplicate_spawn=true;}
        if(!duplicate_spawn)throw std::runtime_error("duplicate character owner spawned");
        bool rejected=false;
        try { scrapds::net::parse_character_update(std::vector<std::uint8_t>{0x1e,0}); }
        catch(const std::exception&) { rejected=true; }
        if(!rejected) throw std::runtime_error("truncated movement accepted");
        std::cout << "captured character update decoded; character id=" << state.character_id << " position=" << state.position[0] << ',' << state.position[1] << ',' << state.position[2] << '\n';
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
