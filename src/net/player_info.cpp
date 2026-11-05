#include "player_info.hpp"
#include "net/codec.hpp"
#include <algorithm>
#include <stdexcept>

namespace scrapds::net {
PlayerInfo parse_player_info(std::span<const std::uint8_t> bytes) {
    std::size_t offset=0;
    auto take=[&](std::size_t n) {if(offset>bytes.size()||n>bytes.size()-offset)throw std::invalid_argument("truncated-player-info");auto part=bytes.subspan(offset,n);offset+=n;return part;};
    auto word=[&](unsigned size) {std::uint32_t value=0;for(auto b:take(size))value=(value<<8)|b;return value;};
    if(word(1)!=9)throw std::invalid_argument("not-player-info");
    const auto name_bytes=take(word(2));
    if(std::find(name_bytes.begin(),name_bytes.end(),0)!=name_bytes.end())throw std::invalid_argument("invalid-player-name");
    PlayerInfo result;result.name.assign(name_bytes.begin(),name_bytes.end());
    if(word(4)!=2)throw std::invalid_argument("unsupported-appearance-version");
    result.appearance_type=std::uint8_t(word(1));const auto count=word(1);
    if(count>9)throw std::invalid_argument("too-many-appearance-parts");
    result.appearance.resize(count);
    for(auto& part:result.appearance) {const auto uuid=take(16);std::copy(uuid.begin(),uuid.end(),part.uuid_memory.begin());}
    for(auto& part:result.appearance)part.color=word(4);
    result.inventory_kind=word(4);const auto inventory_length=word(4);
    if(inventory_length>1024*1024)throw std::invalid_argument("oversized-inventory-result");
    const auto inventory=take(inventory_length);result.inventory_result.assign(inventory.begin(),inventory.end());
    if(offset!=bytes.size())throw std::invalid_argument("trailing-player-info");
    return result;
}
BlobDataEntry make_player_blob_entry(const PlayerSnapshot& player) {
    if(!player.player_id||player.player_id==UINT32_MAX||!player.owner_steam_id)throw std::invalid_argument("invalid-player-identity");
    if(player.info.name.size()>255||player.info.name.find('\0')!=std::string::npos)throw std::invalid_argument("invalid-player-name");
    if(player.info.appearance.size()>9||player.info.inventory_result.size()>1024*1024)throw std::invalid_argument("invalid-player-snapshot");
    std::vector<std::uint8_t> raw{0}; // compress_packet preserves this sentinel.
    auto word=[&](std::uint64_t value,unsigned size){for(unsigned i=size;i>0;--i)raw.push_back(std::uint8_t(value>>(8*(i-1))));};
    word(player.character_id,4);word(player.owner_steam_id,8);
    for(auto value:player.state_words)word(value,4);
    raw.push_back(player.flags);raw.push_back(static_cast<std::uint8_t>(player.info.name.size()));
    raw.insert(raw.end(),player.info.name.begin(),player.info.name.end());
    word(2,4);raw.push_back(player.info.appearance_type);raw.push_back(9);
    for(std::size_t i=0;i<9;++i) {
        if(i<player.info.appearance.size())raw.insert(raw.end(),player.info.appearance[i].uuid_memory.begin(),player.info.appearance[i].uuid_memory.end());
        else raw.insert(raw.end(),16,0);
    }
    for(std::size_t i=0;i<9;++i)word(i<player.info.appearance.size()?player.info.appearance[i].color:0,4);
    word(player.info.inventory_kind,4);word(player.info.inventory_result.size(),4);
    raw.insert(raw.end(),player.info.inventory_result.begin(),player.info.inventory_result.end());
    auto compressed=compress_packet(raw);compressed.erase(compressed.begin());
    BlobDataEntry entry;entry.uid=player_blob_uid;
    entry.key={std::uint8_t(player.player_id),std::uint8_t(player.player_id>>8),std::uint8_t(player.player_id>>16),std::uint8_t(player.player_id>>24)};
    entry.world_id=0xfffe;entry.flags=13;entry.payload=std::move(compressed);return entry;
}
PlayerSnapshot parse_player_blob_entry(const BlobDataEntry& entry) {
    if(entry.uid!=player_blob_uid||entry.key.size()!=4||entry.world_id!=0xfffe||entry.flags!=13||entry.payload.empty())
        throw std::invalid_argument("invalid-player-blob-entry");
    const auto player_id=std::uint32_t(entry.key[0])|(std::uint32_t(entry.key[1])<<8)|(std::uint32_t(entry.key[2])<<16)|(std::uint32_t(entry.key[3])<<24);
    std::vector<std::uint8_t> encoded{0};encoded.insert(encoded.end(),entry.payload.begin(),entry.payload.end());
    const auto raw=decompress_packet(encoded,2*1024*1024);if(raw.empty()||raw[0]!=0)throw std::invalid_argument("invalid-player-blob-compression");
    std::size_t offset=1;
    auto take=[&](std::size_t n) {if(offset>raw.size()||n>raw.size()-offset)throw std::invalid_argument("truncated-player-blob");const auto part=std::span<const std::uint8_t>(raw).subspan(offset,n);offset+=n;return part;};
    auto word=[&](unsigned size) {std::uint32_t value=0;for(auto byte:take(size))value=(value<<8)|byte;return value;};
    PlayerSnapshot result;result.player_id=player_id;result.character_id=word(4);
    for(unsigned i=0;i<8;++i)result.owner_steam_id=(result.owner_steam_id<<8)|take(1)[0];
    for(auto& value:result.state_words)value=word(4);result.flags=std::uint8_t(word(1));
    const auto name_size=std::size_t(word(1));const auto name=take(name_size);
    if(std::find(name.begin(),name.end(),0)!=name.end())throw std::invalid_argument("invalid-player-blob-name");
    result.info.name.assign(name.begin(),name.end());if(word(4)!=2)throw std::invalid_argument("unsupported-player-blob-appearance-version");
    result.info.appearance_type=std::uint8_t(word(1));result.info.appearance.resize(9);
    for(auto& part:result.info.appearance){const auto uuid=take(16);std::copy(uuid.begin(),uuid.end(),part.uuid_memory.begin());}
    for(auto& part:result.info.appearance)part.color=word(4);
    result.info.inventory_kind=word(4);const auto inventory_size=std::size_t(word(4));
    if(inventory_size>1024*1024)throw std::invalid_argument("oversized-player-blob-inventory");
    const auto inventory=take(inventory_size);result.info.inventory_result.assign(inventory.begin(),inventory.end());
    if(offset!=raw.size()||!player_id||player_id==UINT32_MAX||!result.owner_steam_id)
        throw std::invalid_argument("invalid-player-blob-tail-or-identity");
    return result;
}
}
