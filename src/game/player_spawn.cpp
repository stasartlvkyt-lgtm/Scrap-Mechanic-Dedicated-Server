#include "player_spawn.hpp"
#include "net/world_sync.hpp"
#include "net/codec.hpp"
#include <algorithm>
#include <stdexcept>

namespace scrapds {
std::vector<std::uint8_t> strip_game_info_players(std::span<const std::uint8_t> packet) {
    if(packet.size()<23||packet[0]!=2)throw std::invalid_argument("invalid-game-info");
    std::size_t offset=17;
    auto read=[&](unsigned n){if(offset>packet.size()||n>packet.size()-offset)throw std::invalid_argument("truncated-game-info");std::uint32_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|packet[offset++];return v;};
    auto skip=[&](std::size_t n){if(offset>packet.size()||n>packet.size()-offset)throw std::invalid_argument("truncated-game-info");offset+=n;};
    const auto ugc=read(4);if(ugc>(packet.size()-offset)/24)throw std::invalid_argument("invalid-game-info-ugc");skip(std::size_t(ugc)*24);
    const auto name_size=read(2);skip(name_size);
    std::vector<std::uint8_t> out(packet.begin(),packet.begin()+offset);
    for(int list=0;list<2;++list) {
        const auto count=read(4);if(count>(packet.size()-offset)/18)throw std::invalid_argument("invalid-game-info-key-count");
        const auto count_at=out.size();out.insert(out.end(),4,0);std::uint32_t retained=0;
        for(std::uint32_t i=0;i<count;++i) {
            const auto start=offset;skip(16);const auto key_size=read(2);skip(key_size);
            if(std::equal(net::player_blob_uid.begin(),net::player_blob_uid.end(),packet.begin()+start))continue;
            out.insert(out.end(),packet.begin()+start,packet.begin()+offset);++retained;
        }
        for(int i=0;i<4;++i)out[count_at+i]=std::uint8_t(retained>>(24-8*i));
    }
    out.insert(out.end(),packet.begin()+offset,packet.end());return out;
}
std::optional<net::CharacterCreation> find_replay_spawn(std::span<const std::uint8_t> packet) {
    if(packet.empty())throw std::invalid_argument("empty-replay-packet");
    if(packet[0]==29) {for(const auto& child:net::split_world_aggregate(packet))if(auto found=find_replay_spawn(child))return found;}
    if(packet[0]==21||packet[0]==22)for(const auto& record:net::parse_net_object_packet(packet).records)
        if(record.operation==1&&record.type==6)return net::parse_character_creation(record.payload);
    return std::nullopt;
}
std::optional<std::vector<std::uint8_t>> strip_replay_players(std::span<const std::uint8_t> packet) {
    if(packet.empty())throw std::invalid_argument("empty-replay-packet");
    auto original=[&]{return std::vector<std::uint8_t>(packet.begin(),packet.end());};
    if(packet[0]==29) {
        auto children=net::split_world_aggregate(packet);std::vector<std::vector<std::uint8_t>> retained;bool changed=false;
        for(const auto& child:children) {auto filtered=strip_replay_players(child);if(!filtered){changed=true;continue;}if(*filtered!=child)changed=true;retained.push_back(std::move(*filtered));}
        if(!changed)return original();if(retained.empty())return std::nullopt;return net::make_world_aggregate(retained);
    }
    if(packet[0]==11||packet[0]==13||packet[0]==25||packet[0]==27) {
        auto parsed=net::parse_blob_data_packet(packet);const auto removed=std::erase_if(parsed.entries,[](const auto& entry){return entry.uid==net::player_blob_uid;});
        if(!removed)return original();if(parsed.entries.empty())return std::nullopt;return net::make_blob_data_packet(parsed);
    }
    if(packet[0]==21||packet[0]==22) {
        auto parsed=net::parse_net_object_packet(packet);const auto removed=std::erase_if(parsed.records,[](const auto& record){return record.type==6;});
        if(!removed)return original();if(parsed.records.empty())return std::nullopt;return net::make_net_object_packet(parsed);
    }
    return original();
}
PlayerSpawnPackets prepare_player_spawn(WorldState& world,net::ServerCharacters& characters,
                                        const PlayerSpawnSpec& spec,std::uint32_t tick) {
    if(!spec.steam_id)throw std::invalid_argument("invalid-player-owner");
    if(spec.existing_player_id&&*spec.existing_player_id==UINT32_MAX)throw std::invalid_argument("invalid-player-id");
    PlayerSpawnPackets result;bool spawned=false;
    try {
        world.atomic([&] {
            result.player_id=spec.existing_player_id?*spec.existing_player_id:world.reserve_player_id();result.owner_steam_id=spec.steam_id;
            result.spawn_server_tick=tick;result.spawn_position=spec.character.position;
            result.character_id=characters.spawn(spec.steam_id);spawned=true;
            result.player_state_words=spec.player_state_words;result.player_flags=spec.player_flags;result.info=spec.info;
            result.inventory_container_id=world.ensure_player_inventory_container(spec.steam_id,spec.player_state_words[1]);
            result.player_state_words[1]=result.inventory_container_id;
            result.container_creation=net::make_net_object_packet({22,tick,{
                net::make_container_creation(world.containers().at(result.inventory_container_id))}});
            net::PlayerSnapshot player;player.player_id=result.player_id;player.owner_steam_id=spec.steam_id;
            player.state_words=result.player_state_words;player.flags=spec.player_flags;player.info=spec.info;
            result.player_before=net::make_blob_data_packet({13,tick,{net::make_player_blob_entry(player)}});
            player.character_id=result.character_id;
            result.player_after=net::make_blob_data_packet({13,tick,{net::make_player_blob_entry(player)}});
            auto creation=spec.character;creation.owner_steam_id=spec.steam_id;
            result.character_state=spec.character_state;result.character_state.player_id=result.player_id;
            net::NetObjectPacket packet{22,tick,{net::make_character_creation(result.character_id,creation),net::make_character_state(result.character_id,result.character_state)}};
            std::vector<std::vector<std::uint8_t>> children{net::make_net_object_packet(packet)};
            result.character_update=net::make_world_aggregate(children);
            world.store_player_snapshot(spec.steam_id,result.player_id,result.player_before,spec.info.inventory_verified);
            return true;
        });
    } catch(...) {if(spawned)characters.despawn(spec.steam_id);throw;}
    return result;
}
PlayerSpawnPackets restore_player_snapshot(std::uint64_t steam_id,const StoredPlayerSnapshot& stored) {
    if(!steam_id||stored.player_id==UINT32_MAX)throw std::invalid_argument("invalid-stored-player");
    auto packet=net::parse_blob_data_packet(stored.unlinked_packet);
    if(packet.message_id!=13||packet.entries.size()!=1||packet.entries.front().uid!=net::player_blob_uid)
        throw std::invalid_argument("invalid-stored-player-packet");
    auto player=net::parse_player_blob_entry(packet.entries.front());
    if(player.player_id!=stored.player_id||player.owner_steam_id!=steam_id||player.character_id!=UINT32_MAX)
        throw std::invalid_argument("stored-player-identity-mismatch");
    player.info.inventory_verified=stored.inventory_verified;
    PlayerSpawnPackets result;result.owner_steam_id=steam_id;result.player_id=stored.player_id;
    result.player_before=stored.unlinked_packet;result.info=player.info;result.player_state_words=player.state_words;result.player_flags=player.flags;
    if(!stored.inventory_verified&&!player.info.inventory_result.empty()) {
        player.info.inventory_result.clear();
        packet.entries.front()=net::make_player_blob_entry(player);
        result.player_before=net::make_blob_data_packet(packet);result.info=player.info;
    }
    return result;
}
PlayerSpawnPackets restore_player_snapshot(WorldState& world,std::uint64_t steam_id,
                                           const StoredPlayerSnapshot& stored,std::uint32_t tick) {
    auto result=restore_player_snapshot(steam_id,stored);
    world.atomic([&] {
        result.inventory_container_id=world.ensure_player_inventory_container(steam_id,result.player_state_words[1]);
        result.player_state_words[1]=result.inventory_container_id;
        net::PlayerSnapshot player;player.player_id=result.player_id;player.owner_steam_id=steam_id;
        player.state_words=result.player_state_words;player.flags=result.player_flags;player.info=result.info;
        result.player_before=net::make_blob_data_packet({13,tick,{net::make_player_blob_entry(player)}});
        result.container_creation=net::make_net_object_packet({22,tick,{
            net::make_container_creation(world.containers().at(result.inventory_container_id))}});
        world.store_player_snapshot(steam_id,result.player_id,result.player_before,stored.inventory_verified);
        return true;
    });
    return result;
}
std::array<std::vector<std::uint8_t>,2> prepare_player_disconnect(const PlayerSpawnPackets& player,std::uint32_t tick) {
    if(!player.character_id||player.player_id==UINT32_MAX)throw std::invalid_argument("invalid-player-disconnect");
    auto unlinked=net::parse_blob_data_packet(player.player_before);
    if(unlinked.message_id!=13||unlinked.entries.size()!=1||unlinked.entries.front().uid!=net::player_blob_uid)
        throw std::invalid_argument("invalid-player-snapshot");
    const auto& key=unlinked.entries.front().key;
    if(key.size()!=4||key[0]!=std::uint8_t(player.player_id)||key[1]!=std::uint8_t(player.player_id>>8)||key[2]!=std::uint8_t(player.player_id>>16)||key[3]!=std::uint8_t(player.player_id>>24))
        throw std::invalid_argument("invalid-player-snapshot-key");
    unlinked.tick=tick;
    net::NetObjectPacket removal{22,tick,{{2,6,{},player.character_id,{}}}};
    const std::vector<std::vector<std::uint8_t>> children{net::make_net_object_packet(removal)};
    return {net::make_blob_data_packet(unlinked),net::make_world_aggregate(children)};
}
std::vector<std::uint8_t> character_spawn_at_latest_position(const PlayerSpawnPackets& spawn,const net::CharacterUpdate& latest) {
    if(!latest.has_character_state||latest.character_id!=spawn.character_id)throw std::invalid_argument("character-state-owner-mismatch");
    auto children=net::split_world_aggregate(spawn.character_update);bool found=false;
    for(auto& child:children) {
        if(child.empty()||(child[0]!=21&&child[0]!=22))continue;
        auto packet=net::parse_net_object_packet(child);
        for(auto& record:packet.records)if(record.operation==1&&record.type==6&&record.id==spawn.character_id) {
            if(found)throw std::invalid_argument("duplicate-character-creation");
            auto creation=net::parse_character_creation(record.payload);creation.position=latest.position;
            record=net::make_character_creation(spawn.character_id,creation);found=true;
        }
        child=net::make_net_object_packet(packet);
    }
    if(!found)throw std::invalid_argument("missing-character-creation");
    return net::make_world_aggregate(children);
}
std::vector<std::uint8_t> paint_player_character(PlayerSpawnPackets& spawn,std::uint32_t color_rgba,std::uint32_t tick) {
    if(!spawn.character_id||spawn.character_id==UINT32_MAX)throw std::invalid_argument("invalid-character-paint-target");
    if(!color_rgba)throw std::invalid_argument("character-default-color-unavailable");
    auto state=spawn.character_state;state.color_rgba=color_rgba;
    const auto update=net::make_net_object_packet({22,tick,{net::make_character_state(spawn.character_id,state)}});
    auto children=net::split_world_aggregate(spawn.character_update);bool found=false;
    for(auto& child:children)if(!child.empty()&&(child[0]==21||child[0]==22)) {
        auto packet=net::parse_net_object_packet(child);packet.tick=tick;
        for(auto& record:packet.records)if(record.operation==3&&record.type==6&&record.id==spawn.character_id) {
            if(found)throw std::invalid_argument("duplicate-character-state");
            record=net::make_character_state(spawn.character_id,state);found=true;
        }
        child=net::make_net_object_packet(packet);
    }
    if(!found)throw std::invalid_argument("missing-character-state");
    auto snapshot=net::make_world_aggregate(children);
    spawn.character_state=state;spawn.character_update=std::move(snapshot);
    return update;
}
std::vector<std::uint8_t> apply_verified_player_inventory(WorldState& world,PlayerSpawnPackets& spawn,
                                                          std::span<const std::uint8_t> inventory_result,std::uint32_t tick) {
    if(!spawn.owner_steam_id||spawn.player_id==UINT32_MAX||!spawn.character_id||inventory_result.empty()||inventory_result.size()>1024*1024)
        throw std::invalid_argument("invalid-verified-player-inventory");
    auto info=spawn.info;info.inventory_result.assign(inventory_result.begin(),inventory_result.end());info.inventory_verified=true;
    net::PlayerSnapshot player;player.player_id=spawn.player_id;player.owner_steam_id=spawn.owner_steam_id;
    player.state_words=spawn.player_state_words;player.flags=spawn.player_flags;player.info=info;
    const auto unlinked=net::make_blob_data_packet({13,tick,{net::make_player_blob_entry(player)}});
    player.character_id=spawn.character_id;
    const auto linked=net::make_blob_data_packet({13,tick,{net::make_player_blob_entry(player)}});
    world.store_player_snapshot(spawn.owner_steam_id,spawn.player_id,unlinked,true);
    spawn.info=std::move(info);spawn.player_before=unlinked;spawn.player_after=linked;
    return linked;
}
}
