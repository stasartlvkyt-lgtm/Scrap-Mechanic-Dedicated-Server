#include "net/character.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>
#include <cmath>

namespace scrapds::net {
namespace {
class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> data) : data_(data) {}
    std::size_t position() const noexcept { return bit_; }
    std::size_t remaining() const noexcept { return data_.size()*8-bit_; }
    std::uint32_t read(unsigned count) {
        if (count > 32 || bit_ + count > data_.size() * 8) throw std::runtime_error("truncated character update at bit " + std::to_string(bit_) + " requesting " + std::to_string(count));
        std::uint32_t value{};
        for (unsigned i=0;i<count;++i,++bit_)
            value=(value<<1)|((data_[bit_/8]>>(7-(bit_%8)))&1u);
        return value;
    }
    std::uint32_t var(unsigned width, unsigned payload_bits, unsigned max_groups) {
        std::uint32_t value{}; unsigned shift{};
        for(unsigned group=0;group<max_groups;++group) {
            const auto chunk=read(width);
            if((std::uint64_t(chunk>>1)<<shift)>UINT32_MAX)throw std::runtime_error("oversized character update varint");
            value |= (chunk>>1)<<shift;
            if(!(chunk&1)) return value;
            shift+=payload_bits;
        }
        throw std::runtime_error("oversized character update varint");
    }
    std::uint32_t little(unsigned count) {
        std::uint32_t value{};
        for(unsigned shift=0;count;shift+=8) {
            const auto width=std::min(count,8u);
            value|=read(width)<<shift;
            count-=width;
        }
        return value;
    }
    // The game's 21-bit reader stores each extracted eight-bit chunk in
    // little-endian byte order, unlike its ordinary fixed-width bit fields.
    std::uint32_t var21() {
        std::uint32_t value{}; unsigned shift{};
        for(unsigned group=0;group<2;++group) {
            const auto low=read(8);
            const auto middle=read(8);
            const auto high=read(5);
            const auto raw=low | (middle<<8) | (high<<16);
            if((std::uint64_t(raw>>1)<<shift)>UINT32_MAX)throw std::runtime_error("oversized character tick");
            value|=(raw>>1)<<shift;
            if(!(raw&1u)) return value;
            shift+=20;
        }
        throw std::runtime_error("oversized character tick");
    }
    // The 15-bit Character ID reader likewise places the first eight bits
    // in the low byte and the remaining seven in the next byte.
    std::uint32_t var15() {
        std::uint32_t value{};
        // D9E040 rejects a third group before accepting its continuation bit.
        for(unsigned group=0;group<2;++group) {
            const auto low=read(8);
            const auto high=read(7);
            const auto raw=low|(high<<8);
            value|=(raw>>1)<<(14*group);
            if(!(raw&1u)) {
                if(!value)throw std::runtime_error("invalid zero character id");
                return value;
            }
        }
        throw std::runtime_error("oversized character id");
    }
    std::uint32_t var9() {
        std::uint32_t value{};
        for(unsigned group=0;group<4;++group) {
            const auto raw=little(9);value|=(raw>>1)<<(8*group);
            if(!(raw&1u))return value;
        }
        throw std::runtime_error("oversized character tick delta");
    }
private:
    std::span<const std::uint8_t> data_; std::size_t bit_{};
};
CharacterUpdate read_character_state(Bits& bits,std::uint8_t type) {
    CharacterUpdate out;out.state_type=type;
    if(out.state_type==0) return out;
    if(out.state_type==3) {
        bits.read(8); bits.read(32); bits.read(3); bits.read(1);
        for(int i=0;i<3;++i) {
            const auto negative=bits.read(1)!=0;
            const auto magnitude=bits.little(27);
            out.position[i]=static_cast<float>(magnitude)*(8192.0f/134217727.0f)*(negative?-1.0f:1.0f);
        }
        // CharacterMovement stores each velocity component as sign-magnitude
        // with a 13-bit magnitude in units of 10 / 8192.
        for(int i=0;i<3;++i) {
            const auto negative=bits.read(1)!=0;
            const auto magnitude=bits.little(13);
            out.velocity[i]=static_cast<float>(magnitude)*(10.0f/8191.0f)*(negative?-1.0f:1.0f);
        }
        if(bits.read(1)) for(int i=0;i<4;++i) { bits.read(1); bits.read(23); }
        out.character_id=bits.var15();
        out.has_character_state=true;
        return out;
    }
    if(out.state_type==4) {
        const auto omitted=bits.read(2);
        float square_sum{};
        for(std::uint32_t component=0;component<4;++component) {
            if(component==omitted) continue;
            const auto negative=bits.read(1)!=0;
            const auto magnitude=bits.little(13);
            const float value=static_cast<float>(magnitude)*0.00008632728227138694f*(negative?-1.0f:1.0f);
            out.rotation[component]=value;
            square_sum+=value*value;
        }
        if(square_sum>1.0001f) throw std::runtime_error("invalid tumble quaternion");
        out.rotation[omitted]=std::sqrt(std::max(0.0f,1.0f-square_sum));
        for(int i=0;i<3;++i) {
            const auto negative=bits.read(1)!=0;
            const auto magnitude=bits.little(27);
            out.position[i]=static_cast<float>(magnitude)*(8192.0f/134217727.0f)*(negative?-1.0f:1.0f);
        }
        for(int i=0;i<6;++i) { bits.read(1); bits.read(23); }
        out.character_id=bits.var15();
        out.has_character_state=true;
        return out;
    }
    throw std::runtime_error("unknown character state type");
}
class BitWriter {
public:
    explicit BitWriter(std::uint8_t id):bytes_{id}{}
    void write(std::uint32_t value,unsigned count) {
        for(unsigned i=count;i>0;--i,++bit_) {
            if(bit_/8==bytes_.size())bytes_.push_back(0);
            if((value>>(i-1))&1u)bytes_[bit_/8]|=std::uint8_t(0x80u>>(bit_%8));
        }
    }
    void little(std::uint32_t value,unsigned count) {
        for(unsigned shift=0;count;shift+=8) {const auto width=std::min(count,8u);write((value>>shift)&((1u<<width)-1),width);count-=width;}
    }
    void variable(std::uint32_t value,unsigned payload,unsigned width) {
        const auto mask=(1u<<payload)-1u;
        do {const auto chunk=value&mask;value>>=payload;little((chunk<<1)|(value?1u:0u),width);}while(value);
    }
    void copy(std::span<const std::uint8_t> data,std::size_t begin,std::size_t end) {
        if(end<begin||end>data.size()*8)throw std::invalid_argument("invalid-character-state-bits");
        for(auto bit=begin;bit<end;++bit)write((data[bit/8]>>(7-bit%8))&1u,1);
    }
    std::vector<std::uint8_t> finish(){return std::move(bytes_);}
private:
    std::vector<std::uint8_t> bytes_;std::size_t bit_{8};
};
CharacterUpdate parse_client(std::span<const std::uint8_t> packet,std::size_t* begin,std::size_t* end) {
    if(packet.size()<2 || packet[0]!=0x1e)throw std::runtime_error("not a client character update");
    Bits bits(packet.subspan(1));const auto tick=bits.var21();const auto ack=bits.var(7,6,6);
    if(ack>4096)throw std::runtime_error("too many character acknowledgements");
    for(std::uint32_t i=0;i<ack;++i){bits.var21();bits.read(6);}
    if(begin)*begin=bits.position();
    auto out=read_character_state(bits,static_cast<std::uint8_t>(bits.read(4)));out.tick=tick;out.acknowledgements=ack;
    if(end)*end=bits.position();return out;
}
}
CharacterUpdate parse_character_update(std::span<const std::uint8_t> packet) {return parse_client(packet,nullptr,nullptr);}
ServerCharacterUpdate parse_server_character_update(std::span<const std::uint8_t> packet) {
    if(packet.size()<2||packet[0]!=0x18)throw std::runtime_error("not a server character update");
    Bits bits(packet.subspan(1));ServerCharacterUpdate out;out.server_tick=bits.var21();const auto zigzag=bits.var9();
    const auto delta=(zigzag>>1)^(0u-(zigzag&1u));out.client_tick=out.server_tick+delta;
    out.sequence=static_cast<std::uint8_t>(bits.read(6));out.character=read_character_state(bits,static_cast<std::uint8_t>(bits.read(4)));out.character.tick=out.server_tick;
    if(!out.character.has_character_state||bits.read(4)!=0)throw std::runtime_error("unsupported server character record list");
    return out;
}
std::vector<std::uint8_t> make_server_character_update(std::span<const std::uint8_t> packet,std::uint32_t server_tick,std::uint32_t acknowledged_client_tick,std::uint8_t sequence) {
    return make_server_character_update(packet,server_tick,acknowledged_client_tick,sequence,std::nullopt);
}
std::vector<std::uint8_t> make_server_character_update(std::span<const std::uint8_t> packet,std::uint32_t server_tick,std::uint32_t acknowledged_client_tick,std::uint8_t sequence,std::optional<std::array<float,3>> authoritative_position) {
    std::size_t begin{},end{};const auto update=parse_client(packet,&begin,&end);
    if(!update.has_character_state||sequence>=64)throw std::invalid_argument("invalid-server-character-update");
    auto source=std::vector<std::uint8_t>(packet.begin()+1,packet.end());
    if(authoritative_position) {
        for(const auto value:*authoritative_position)if(!std::isfinite(value))throw std::invalid_argument("nonfinite-authoritative-character-position");
        // Type 3 movement and type 4 tumble both put their three 1+27-bit
        // position components 48 bits after the state-type field.
        const auto position_begin=begin+48;
        if(position_begin+84>end)throw std::invalid_argument("truncated-authoritative-character-position");
        for(unsigned axis=0;axis<3;++axis) {
            const auto magnitude=static_cast<std::uint32_t>(std::min(134217727.0,std::round(std::abs(double((*authoritative_position)[axis]))*(134217727.0/8192.0))));
            auto bit=position_begin+axis*28;
            const auto set=[&](std::size_t at,bool value){const auto mask=std::uint8_t(0x80u>>(at%8));if(value)source[at/8]|=mask;else source[at/8]&=std::uint8_t(~mask);};
            set(bit++,std::signbit((*authoritative_position)[axis]));
            for(unsigned shift=0;shift<27;) {const auto width=std::min(8u,27u-shift);for(unsigned j=0;j<width;++j)set(bit++,((magnitude>>(shift+width-1-j))&1u)!=0);shift+=width;}
        }
    }
    BitWriter writer(0x18);writer.variable(server_tick,20,21);
    const auto delta=acknowledged_client_tick-server_tick;const auto zigzag=(delta<<1)^(0u-(delta>>31));writer.variable(zigzag,8,9);
    writer.write(sequence,6);writer.copy(source,begin,end);writer.write(0,4);return writer.finish();
}
namespace {
void read_body_state(Bits& bits,ServerBodyUpdate& out,std::uint32_t type) {
    if(type!=1&&type!=2)throw std::runtime_error("not a body state record");out.has_velocity=type==1;
    const auto omitted=bits.read(2);float sum=0;
    for(unsigned i=0;i<4;++i)if(i!=omitted){const bool negative=bits.read(1)!=0;const auto magnitude=bits.little(13);const auto value=float(magnitude)*0.00008632728227138694f*(negative?-1.0f:1.0f);out.rotation[i]=value;sum+=value*value;}
    if(sum>1.0001f)throw std::runtime_error("invalid-body-quaternion");out.rotation[omitted]=std::sqrt(std::max(0.0f,1.0f-sum));
    auto read_fixed=[&](unsigned width,float factor){const bool negative=bits.read(1)!=0;const auto magnitude=bits.little(width);return float(magnitude)*factor*(negative?-1.0f:1.0f);};
    for(auto& value:out.position)value=read_fixed(27,8192.0f/134217727.0f);
    if(out.has_velocity){for(auto& value:out.velocity)value=read_fixed(23,1000.0f/8388607.0f);for(auto& value:out.angular_velocity)value=read_fixed(23,1000.0f/8388607.0f);}
    out.flags=std::uint8_t(bits.read(3));out.body_id=bits.var15();
}
ServerBodyUpdate body_header(Bits& bits) {
    ServerBodyUpdate out;out.server_tick=bits.var21();const auto z=bits.var9();
    out.acknowledged_client_tick=out.server_tick+((z>>1)^(0u-(z&1u)));out.sequence=std::uint8_t(bits.read(6));return out;
}
}
ServerBodyUpdate parse_server_body_update(std::span<const std::uint8_t> packet) {
    if(packet.size()<2||packet[0]!=0x18)throw std::runtime_error("not a server body update");
    Bits bits(packet.subspan(1));auto out=body_header(bits);read_body_state(bits,out,bits.read(4));return out;
}
std::vector<ServerBodyUpdate> extract_server_body_updates(std::span<const std::uint8_t> packet) {
    if(packet.size()<2||packet[0]!=0x18)throw std::runtime_error("not a server body update");
    Bits bits(packet.subspan(1));const auto header=body_header(bits);std::vector<ServerBodyUpdate> result;
    for(unsigned i=0;i<10000&&bits.remaining()>=4;++i) {
        const auto type=bits.read(4);if(!type)break;
        try {
            if(type==1||type==2){auto body=header;read_body_state(bits,body,type);result.push_back(body);}
            else if(type==3||type==4)(void)read_character_state(bits,std::uint8_t(type));
            else break; // Other NetObjUnreliable types need their own schemas.
        } catch(const std::runtime_error&) {break;}
    }
    return result;
}
std::vector<std::uint8_t> make_server_body_update(const ServerBodyUpdate& update) {
    if(!update.body_id||update.body_id>=0x10000000u||update.sequence>=64||update.flags>=8)throw std::invalid_argument("invalid-server-body-update");
    double norm=0;for(const auto q:update.rotation){if(!std::isfinite(q))throw std::invalid_argument("invalid-body-quaternion");norm+=double(q)*q;}
    if(norm<1e-12||!std::isfinite(norm))throw std::invalid_argument("invalid-body-quaternion");
    std::array<double,4> q{};for(int i=0;i<4;++i)q[i]=update.rotation[i]/std::sqrt(norm);
    unsigned omitted=0;for(unsigned i=1;i<4;++i)if(std::abs(q[i])>std::abs(q[omitted]))omitted=i;
    if(q[omitted]<0)for(auto& value:q)value=-value;
    BitWriter writer(0x18);writer.variable(update.server_tick,20,21);
    const auto delta=update.acknowledged_client_tick-update.server_tick;writer.variable((delta<<1)^(0u-(delta>>31)),8,9);
    writer.write(update.sequence,6);writer.write(update.has_velocity?1:2,4);writer.write(omitted,2);
    auto fixed=[&](double value,double scale,std::uint32_t maximum,unsigned width){
        if(!std::isfinite(value))throw std::invalid_argument("nonfinite-body-state");
        writer.write(std::signbit(value)?1:0,1);const auto fraction=std::min(1.0,std::abs(value)/scale);
        writer.little(static_cast<std::uint32_t>(std::round(fraction*maximum)),width);
    };
    for(unsigned i=0;i<4;++i)if(i!=omitted)fixed(q[i],0.7071067811865476,8191,13);
    for(const auto value:update.position)fixed(value,8192.0,134217727,27);
    if(update.has_velocity){for(const auto value:update.velocity)fixed(value,1000.0,8388607,23);for(const auto value:update.angular_velocity)fixed(value,1000.0,8388607,23);}
    writer.write(update.flags,3);writer.variable(update.body_id,14,15);writer.write(0,4);return writer.finish();
}

std::vector<std::uint8_t> serialize_character_creation(const CharacterCreation& character) {
    std::vector<std::uint8_t> out;out.reserve(47);
    auto word=[&](std::uint64_t value,unsigned bytes) {for(unsigned i=bytes;i>0;--i)out.push_back(std::uint8_t(value>>(8*(i-1))));};
    word(character.owner_steam_id,8);
    // The game reverses the complete 96-bit vector, not each float separately.
    for(int i=2;i>=0;--i) {
        if(!std::isfinite(character.position[std::size_t(i)]))throw std::invalid_argument("nonfinite-character-position");
        word(std::bit_cast<std::uint32_t>(character.position[std::size_t(i)]),4);
    }
    word(character.world_id,2);
    for(const auto value:character.orientation_words)word(value,4);
    out.insert(out.end(),character.asset_uuid_memory.rbegin(),character.asset_uuid_memory.rend());
    out.push_back(character.flag?0x80:0);
    return out;
}
CharacterCreation parse_character_creation(std::span<const std::uint8_t> payload) {
    if(payload.size()!=47||(payload[46]&0x7f))throw std::invalid_argument("invalid-character-creation");
    auto word=[&](std::size_t at,unsigned size){std::uint64_t value=0;for(unsigned i=0;i<size;++i)value=(value<<8)|payload[at+i];return value;};
    CharacterCreation result;result.owner_steam_id=word(0,8);
    for(int i=0;i<3;++i) {result.position[2-i]=std::bit_cast<float>(std::uint32_t(word(8+i*4,4)));if(!std::isfinite(result.position[2-i]))throw std::invalid_argument("nonfinite-character-position");}
    result.world_id=std::uint16_t(word(20,2));result.orientation_words={std::uint32_t(word(22,4)),std::uint32_t(word(26,4))};
    std::reverse_copy(payload.begin()+30,payload.begin()+46,result.asset_uuid_memory.begin());result.flag=(payload[46]&0x80)!=0;
    return result;
}
std::vector<std::uint8_t> serialize_character_initial_state(const CharacterInitialState& state) {
    std::vector<std::uint8_t> out;std::size_t offset=0;
    auto bits=[&](std::uint32_t value,unsigned count) {
        for(unsigned i=count;i>0;--i,++offset) {
            if(offset/8==out.size())out.push_back(0);
            if((value>>(i-1))&1u)out[offset/8]|=std::uint8_t(0x80u>>(offset%8));
        }
    };
    bits(15,4);
    for(const auto flag:state.flags)bits(flag?1:0,1);
    bits(state.extra_id.has_value()?1:0,1);if(state.extra_id)bits(*state.extra_id,32);
    bits(state.color_rgba,32);
    for(auto it=state.asset_uuid_memory.rbegin();it!=state.asset_uuid_memory.rend();++it)bits(*it,8);
    bits(state.state_word,32);bits(state.active?1:0,1);bits(state.player_id,32);
    return out;
}

bool CharacterOwnership::claim(std::uint64_t steam_id, std::uint32_t character_id) {
    const auto peer_it=by_peer_.find(steam_id);
    if(peer_it!=by_peer_.end()) return peer_it->second==character_id;
    const auto character_it=by_character_.find(character_id);
    if(character_it!=by_character_.end()&&character_it->second!=steam_id) return false;
    by_peer_[steam_id]=character_id;
    by_character_[character_id]=steam_id;
    return true;
}

void CharacterOwnership::release(std::uint64_t steam_id) noexcept {
    const auto it=by_peer_.find(steam_id);
    if(it==by_peer_.end()) return;
    by_character_.erase(it->second);
    by_peer_.erase(it);
}

std::string CharacterStateTracker::observe(std::uint64_t steam_id,const CharacterUpdate& update) {
    if(!update.has_character_state)return "packet-has-no-character-state";
    const auto current=latest_.find(steam_id);
    if(current!=latest_.end()) {
        if(current->second.character_id!=update.character_id)return "peer-changed-character-id";
        const auto delta=std::bit_cast<std::int32_t>(update.tick-current->second.tick);
        if(delta<0)return "stale-character-tick";
    }
    if(!ownership_.claim(steam_id,update.character_id))
        return current!=latest_.end()?"peer-changed-character-id":"character-id-owned-by-another-peer";
    latest_.insert_or_assign(steam_id,update);
    return {};
}

void CharacterStateTracker::release(std::uint64_t steam_id) noexcept {
    latest_.erase(steam_id);
    ownership_.release(steam_id);
}

std::uint32_t ServerCharacters::spawn(std::uint64_t steam_id) {
    if(!steam_id)throw std::invalid_argument("invalid-character-owner");
    if(characters_.contains(steam_id))throw std::runtime_error("character-owner-already-spawned");
    if(next_id_==UINT32_MAX)throw std::runtime_error("character-id-exhausted");
    const auto id=next_id_;
    characters_.emplace(steam_id,Character{id});
    ++next_id_;
    return id;
}
void ServerCharacters::despawn(std::uint64_t steam_id) noexcept { characters_.erase(steam_id); }
std::string ServerCharacters::observe(std::uint64_t steam_id,const CharacterUpdate& update) {
    if(!update.has_character_state)return "packet-has-no-character-state";
    const auto it=characters_.find(steam_id);
    if(it==characters_.end())return "character-not-spawned";
    auto& character=it->second;
    if(character.id!=update.character_id)return "character-not-owned-by-peer";
    if(character.state&&std::bit_cast<std::int32_t>(update.tick-character.state->tick)<0)return "stale-character-tick";
    character.state=update;return {};
}
std::optional<CharacterUpdate> ServerCharacters::latest(std::uint64_t steam_id) const {
    const auto it=characters_.find(steam_id);
    return it==characters_.end()?std::nullopt:it->second.state;
}

} // namespace scrapds::net
