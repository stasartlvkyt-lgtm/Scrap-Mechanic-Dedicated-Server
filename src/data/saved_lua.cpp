#include "saved_lua.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace scrapds {
namespace {
class Reader {
    std::span<const std::uint8_t> bytes_;
    std::size_t bit_{56}, nodes_{};
    std::uint64_t bits(unsigned count) {
        if(count>64||bit_>bytes_.size()*8||count>bytes_.size()*8-bit_)throw std::runtime_error("truncated saved LUA value");
        std::uint64_t value=0;
        for(unsigned i=0;i<count;++i,++bit_)value=(value<<1)|((bytes_[bit_/8]>>(7-bit_%8))&1);
        return value;
    }
    std::string string(std::uint32_t size) {
        bit_=(bit_+7)&~std::size_t(7);
        const auto at=bit_/8;
        if(size>16u*1024u*1024u||at>bytes_.size()||size>bytes_.size()-at)throw std::runtime_error("invalid saved LUA string");
        bit_+=std::size_t(size)*8;
        return {reinterpret_cast<const char*>(bytes_.data()+at),size};
    }
public:
    explicit Reader(std::span<const std::uint8_t> bytes):bytes_(bytes) {
        constexpr std::array<std::uint8_t,7> magic{'L','U','A',0,0,0,1};
        if(bytes.size()<magic.size()||!std::equal(magic.begin(),magic.end(),bytes.begin()))throw std::runtime_error("unsupported saved LUA version");
    }
    SavedLuaValue value(unsigned depth=0) {
        if(depth>32||++nodes_>1000000)throw std::runtime_error("saved LUA value limits exceeded");
        SavedLuaValue result;
        const auto type=bits(8);
        switch(type) {
        case 1: return result;
        case 2: result.kind=SavedLuaValue::Kind::boolean;result.boolean=bits(1)!=0;return result;
        case 3: result.kind=SavedLuaValue::Kind::number;result.number=std::bit_cast<float>(std::uint32_t(bits(32)));break;
        case 4: result.kind=SavedLuaValue::Kind::string;result.string=string(std::uint32_t(bits(32)));return result;
        case 5: {
            result.kind=SavedLuaValue::Kind::table;
            const auto count=bits(32);const auto array=bits(1);
            if(count>200000||nodes_+count>1000000)throw std::runtime_error("saved LUA table too large");
            result.entries.reserve(std::size_t(count));
            if(array) {
                auto index=std::bit_cast<std::int32_t>(std::uint32_t(bits(32)));
                for(std::uint64_t i=0;i<count;++i) {
                    SavedLuaValue key;key.kind=SavedLuaValue::Kind::number;key.number=double(std::int64_t(index)+std::int64_t(i));
                    result.entries.emplace_back(std::move(key),value(depth+1));
                }
            } else {
                for(std::uint64_t i=0;i<count;++i) {
                    auto key=value(depth+1);
                    auto entry=value(depth+1);
                    result.entries.emplace_back(std::move(key),std::move(entry));
                }
            }
            return result;
        }
        case 6: result.kind=SavedLuaValue::Kind::number;result.number=std::bit_cast<std::int32_t>(std::uint32_t(bits(32)));break;
        case 7: result.kind=SavedLuaValue::Kind::number;result.number=std::bit_cast<std::int16_t>(std::uint16_t(bits(16)));break;
        case 8: result.kind=SavedLuaValue::Kind::number;result.number=std::bit_cast<std::int8_t>(std::uint8_t(bits(8)));break;
        case 11: result.kind=SavedLuaValue::Kind::number;result.number=std::bit_cast<double>(bits(64));break;
        case 100:
            if(bits(32)!=10001)throw std::runtime_error("unsupported saved LUA native value");
            result.kind=SavedLuaValue::Kind::uuid;
            for(auto& byte:result.uuid)byte=std::uint8_t(bits(8));
            return result;
        default: throw std::runtime_error("unsupported saved LUA value type");
        }
        if(!std::isfinite(result.number))throw std::runtime_error("nonfinite saved LUA number");
        return result;
    }
    bool finished() const {return bit_<=bytes_.size()*8&&bytes_.size()*8-bit_<8;}
};
}
SavedLuaValue decode_saved_lua_value(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);auto result=reader.value();
    if(!reader.finished())throw std::runtime_error("trailing saved LUA value data");
    return result;
}
const SavedLuaValue* saved_lua_field(const SavedLuaValue& table,std::string_view name) {
    if(table.kind!=SavedLuaValue::Kind::table)return nullptr;
    const SavedLuaValue* found=nullptr;
    for(const auto& [key,value]:table.entries)if(key.kind==SavedLuaValue::Kind::string&&key.string==name) {
        if(found)throw std::runtime_error("duplicate saved LUA field");
        found=&value;
    }
    return found;
}
}
