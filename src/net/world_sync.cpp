#include "world_sync.hpp"
#include "data/state.hpp"
#include "net/character.hpp"
#include <algorithm>
#include <limits>
#include <iterator>
#include <set>
#include <stdexcept>

namespace scrapds::net {
namespace {
constexpr std::size_t record_limit = 1000000;
std::uint32_t read32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("truncated big-endian word");
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < 4; ++i) result = (result << 8) | bytes[offset + i];
    return result;
}
void put32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(std::uint8_t(value >> shift));
}
void append(std::vector<std::uint8_t>& output, std::span<const std::uint8_t> input) {
    if (input.size() > world_sync_limit || output.size() > world_sync_limit - input.size()) throw std::runtime_error("world synchronization packet exceeds 64 MiB");
    output.insert(output.end(), input.begin(), input.end());
}
void valid_id(std::uint8_t id) {
    if (id != 21 && id != 22) throw std::runtime_error("expected NetObj packet ID21 or ID22");
}
void valid_operation(std::uint8_t op) {
    if (op != 1 && op != 2 && op != 3 && op != 5) throw std::runtime_error("unsupported NetObj record operation");
}
void valid_blob_id(std::uint8_t id) {
    if (id != 11 && id != 13 && id != 25 && id != 27) throw std::runtime_error("expected BlobData packet ID11/13/25/27");
}
std::span<const std::uint8_t> take(std::span<const std::uint8_t> bytes, std::size_t& offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::runtime_error("truncated BlobData entry");
    auto field = bytes.subspan(offset, size);
    offset += size;
    return field;
}
std::uint16_t take16(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    const auto field = take(bytes, offset, 2);
    return std::uint16_t((field[0] << 8) | field[1]);
}
BlobDataEntry blob_entry(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    BlobDataEntry result;
    const auto uid = take(bytes, offset, 16);
    std::copy(uid.begin(), uid.end(), result.uid.begin());
    const auto key = take(bytes, offset, take16(bytes, offset));
    result.key.assign(key.begin(), key.end());
    result.world_id = take16(bytes, offset);
    result.flags = take(bytes, offset, 1)[0];
    const auto size = read32(bytes, offset);
    offset += 4;
    const auto payload = take(bytes, offset, size);
    result.payload.assign(payload.begin(), payload.end());
    return result;
}
NetObjectRecord parse_record(std::span<const std::uint8_t> data) {
    if (data.size() < 5) throw std::runtime_error("truncated NetObj record header");
    NetObjectRecord record;
    record.operation = data[0] >> 5;
    record.type = data[0] & 31;
    valid_operation(record.operation);
    const std::size_t id_offset = record.operation == 1 ? 2 : 1;
    if (record.operation == 1) record.subtype = data[1];
    record.id = read32(data, id_offset);
    record.payload.assign(data.begin() + id_offset + 4, data.end());
    return record;
}
std::vector<std::uint8_t> record_data(const NetObjectRecord& record) {
    valid_operation(record.operation);
    if (record.type > 31 || record.subtype.has_value() != (record.operation == 1)) throw std::runtime_error("invalid NetObj type/subtype header");
    if (record.payload.size() > (record.operation == 1 ? 32759u : 32760u)) throw std::runtime_error("NetObj record exceeds 32767-byte frame limit");
    std::vector<std::uint8_t> data{std::uint8_t((record.operation << 5) | record.type)};
    if (record.subtype) data.push_back(*record.subtype);
    put32(data, record.id);
    append(data, record.payload);
    if (data.size() + 2 > 32767) throw std::runtime_error("NetObj record exceeds 32767-byte frame limit");
    return data;
}
class BitWriter {
public:
    void bit(bool value) {
        if((position_&7u)==0)bytes_.push_back(0);
        if(value)bytes_.back()|=std::uint8_t(0x80u>>(position_&7u));
        ++position_;
    }
    void bits(std::uint32_t value,unsigned count) {
        if(count>32)throw std::invalid_argument("invalid-container-bit-width");
        for(unsigned i=count;i>0;--i)bit(((value>>(i-1))&1u)!=0);
    }
    void little_endian_bits(std::uint64_t value,unsigned count) {
        if(count>64)throw std::invalid_argument("invalid-container-scalar-width");
        for(unsigned byte=0;byte<(count+7)/8;++byte) {
            const auto raw=std::uint8_t(value>>(byte*8));
            const auto in_byte=std::min(8u,count-byte*8);
            for(unsigned bit_index=0;bit_index<in_byte;++bit_index)
                bit(((raw>>(7-bit_index))&1u)!=0);
        }
    }
    void raw(std::span<const std::uint8_t> bytes) {
        for(const auto value:bytes)for(int bit_index=7;bit_index>=0;--bit_index)bit(((value>>bit_index)&1u)!=0);
    }
    std::vector<std::uint8_t> finish() && { return std::move(bytes_); }
private:
    std::vector<std::uint8_t> bytes_;
    std::size_t position_{};
};
bool container_slot_empty(const std::array<std::uint8_t,22>& entry) {
    return std::all_of(entry.begin()+2,entry.begin()+18,[](std::uint8_t value){return value==0;});
}
void write_container_slot_uuid(BitWriter& bits,const std::array<std::uint8_t,22>& entry) {
    if(container_slot_empty(entry)){bits.bits(0,2);return;}
    bits.bits(1,2);bits.raw(std::span<const std::uint8_t>(entry).subspan(2,16));
}
std::uint32_t container_slot_quantity(const std::array<std::uint8_t,22>& entry) {
    return (std::uint32_t(entry[18])<<24)|(std::uint32_t(entry[19])<<16)|(std::uint32_t(entry[20])<<8)|entry[21];
}
std::uint16_t container_slot_type(const std::array<std::uint8_t,22>& entry) {
    return std::uint16_t((std::uint16_t(entry[0])<<8)|entry[1]);
}
}
BlobDataEntry parse_saved_blob_data(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > world_sync_limit) throw std::runtime_error("BlobData entry exceeds 64 MiB");
    std::size_t offset = 0;
    auto result = blob_entry(bytes, offset);
    if (offset != bytes.size()) throw std::runtime_error("trailing bytes after saved BlobData entry");
    return result;
}
std::vector<std::uint8_t> make_saved_blob_data(const BlobDataEntry& entry) {
    if (entry.key.size() > 65535 || entry.payload.size() > world_sync_limit) throw std::runtime_error("invalid BlobData field size");
    std::vector<std::uint8_t> bytes(entry.uid.begin(), entry.uid.end());
    bytes.push_back(std::uint8_t(entry.key.size() >> 8));
    bytes.push_back(std::uint8_t(entry.key.size()));
    append(bytes, entry.key);
    bytes.push_back(std::uint8_t(entry.world_id >> 8));
    bytes.push_back(std::uint8_t(entry.world_id));
    bytes.push_back(entry.flags);
    put32(bytes, std::uint32_t(entry.payload.size()));
    append(bytes, entry.payload);
    return bytes;
}
BlobDataPacket parse_blob_data_packet(std::span<const std::uint8_t> packet) {
    if (packet.size() < 5 || packet.size() > world_sync_limit) throw std::runtime_error("invalid BlobData packet size");
    valid_blob_id(packet[0]);
    BlobDataPacket result{packet[0], read32(packet, 1), {}};
    std::size_t offset = 5;
    while (offset < packet.size()) {
        if (result.entries.size() >= record_limit) throw std::runtime_error("too many BlobData entries");
        result.entries.push_back(blob_entry(packet, offset));
    }
    return result;
}
std::vector<std::uint8_t> make_blob_data_packet(const BlobDataPacket& packet) {
    valid_blob_id(packet.message_id);
    if (packet.entries.size() > record_limit) throw std::runtime_error("too many BlobData entries");
    std::vector<std::uint8_t> bytes{packet.message_id};
    put32(bytes, packet.tick);
    for (const auto& entry : packet.entries) append(bytes, make_saved_blob_data(entry));
    return bytes;
}
std::vector<std::vector<std::uint8_t>> split_world_aggregate(std::span<const std::uint8_t> packet) {
    if (packet.empty() || packet[0] != 29 || packet.size() > world_sync_limit) throw std::runtime_error("invalid world aggregate");
    std::vector<std::vector<std::uint8_t>> children;
    std::size_t offset = 1;
    while (offset < packet.size()) {
        if (children.size() >= record_limit) throw std::runtime_error("too many aggregate children");
        const auto size = read32(packet, offset);
        offset += 4;
        if (size == 0 || size > packet.size() - offset) throw std::runtime_error("invalid aggregate child size");
        children.emplace_back(packet.begin() + offset, packet.begin() + offset + size);
        offset += size;
    }
    return children;
}
std::vector<std::uint8_t> make_world_aggregate(std::span<const std::vector<std::uint8_t>> packets) {
    if (packets.size() > record_limit) throw std::runtime_error("too many aggregate children");
    std::vector<std::uint8_t> output{29};
    for (const auto& packet : packets) {
        if (packet.empty() || packet.size() > world_sync_limit || output.size() > world_sync_limit - 4) throw std::runtime_error("invalid aggregate child size");
        put32(output, std::uint32_t(packet.size()));
        append(output, packet);
    }
    return output;
}
NetObjectPacket parse_net_object_packet(std::span<const std::uint8_t> packet) {
    if (packet.size() < 5 || packet.size() > world_sync_limit) throw std::runtime_error("invalid NetObj packet size");
    valid_id(packet[0]);
    NetObjectPacket result{packet[0], read32(packet, 1), {}};
    std::vector<std::uint8_t> previous;
    std::size_t offset = 5;
    while (offset < packet.size()) {
        if (result.records.size() >= record_limit) throw std::runtime_error("too many NetObj records");
        const auto start = offset;
        const bool delta = (packet[offset] & 128) != 0;
        std::vector<std::uint8_t> data;
        if (delta) {
            if (previous.empty() || previous.size() > 63) throw std::runtime_error("NetObj delta has no compatible previous record");
            const std::size_t mask_size = (previous.size() + 8) / 8;
            if (mask_size > packet.size() - offset) throw std::runtime_error("truncated NetObj delta mask");
            const auto mask = packet.subspan(offset, mask_size);
            offset += mask_size;
            data = previous;
            for (std::size_t i = 0; i < data.size(); ++i) {
                if (!(mask[mask_size - 1 - i / 8] & (1u << (i % 8)))) {
                    if (offset >= packet.size()) throw std::runtime_error("truncated NetObj delta data");
                    data[i] = packet[offset++];
                }
            }
        } else {
            if (packet.size() - offset < 2) throw std::runtime_error("truncated NetObj record size");
            const std::size_t size = (std::size_t(packet[offset]) << 8) | packet[offset + 1];
            if (size < 7 || size > 32767 || size > packet.size() - offset) throw std::runtime_error("invalid NetObj record size");
            offset += 2;
            data.assign(packet.begin() + offset, packet.begin() + start + size);
            offset = start + size;
        }
        auto record = parse_record(data);
        record.wire_offset = start;
        record.wire_size = offset - start;
        record.used_byte_delta = delta;
        result.records.push_back(std::move(record));
        previous = data.size() <= 63 ? std::move(data) : std::vector<std::uint8_t>{};
    }
    return result;
}
std::vector<std::uint8_t> make_net_object_packet(const NetObjectPacket& packet, bool byte_delta) {
    valid_id(packet.message_id);
    if (packet.records.size() > record_limit) throw std::runtime_error("too many NetObj records");
    std::vector<std::uint8_t> output{packet.message_id};
    put32(output, packet.tick);
    std::vector<std::uint8_t> previous;
    for (const auto& record : packet.records) {
        auto data = record_data(record);
        std::vector<std::uint8_t> encoded;
        if (byte_delta && data.size() == previous.size() && data.size() <= 63) {
            const std::size_t mask_size = (data.size() + 8) / 8;
            encoded.resize(mask_size, 0);
            encoded[0] = 128;
            for (std::size_t i = 0; i < data.size(); ++i) {
                if (data[i] == previous[i]) encoded[mask_size - 1 - i / 8] |= std::uint8_t(1u << (i % 8));
                else encoded.push_back(data[i]);
            }
        }
        if (encoded.empty() || encoded.size() >= data.size() + 2) {
            encoded.clear();
            const auto size = data.size() + 2;
            encoded.push_back(std::uint8_t(size >> 8));
            encoded.push_back(std::uint8_t(size));
            encoded.insert(encoded.end(), data.begin(), data.end());
        }
        append(output, encoded);
        previous = data.size() <= 63 ? std::move(data) : std::vector<std::uint8_t>{};
    }
    return output;
}
namespace {
void check_blob(std::span<const std::uint8_t> blob, std::uint8_t type, std::size_t minimum) {
    if (blob.size() < minimum || blob[0] != type || blob[2] != 1) throw std::runtime_error("unsupported or truncated saved NetObj BLOB");
}
using AssetUuid = std::array<std::uint8_t, 16>;
AssetUuid saved_uuid(std::span<const std::uint8_t> blob, std::size_t offset) {
    if (offset > blob.size() || blob.size() - offset < 16) throw std::runtime_error("truncated saved asset UUID");
    AssetUuid key{};
    std::copy_n(blob.begin() + offset, 16, key.begin());
    return key;
}
NetObjectRecord state_record(std::span<const std::uint8_t> blob, std::uint8_t type, std::uint16_t index, std::size_t state_offset) {
    check_blob(blob, type, state_offset);
    NetObjectRecord record;
    record.operation = 3;
    record.type = type;
    record.id = read32(blob, 3);
    record.payload = {std::uint8_t(index >> 8), std::uint8_t(index)};
    record.payload.insert(record.payload.end(), blob.begin() + state_offset, blob.end());
    return record;
}
}
NetObjectRecord make_body_creation(std::span<const std::uint8_t> blob) {
    check_blob(blob, 0, 60);
    if (!((blob[1] == 1 && blob.size() == 60) || (blob[1] == 2 && blob.size() == 80))) throw std::runtime_error("unsupported RigidBody save variant");
    NetObjectRecord record;
    record.operation = 1;
    record.type = 0;
    record.subtype = blob[1];
    record.id = read32(blob, 3);
    record.payload.insert(record.payload.end(), blob.begin() + 7, blob.begin() + 9);
    record.payload.insert(record.payload.end(), blob.begin() + 27, blob.begin() + (blob[1] == 1 ? 55 : 79));
    return record;
}
NetObjectRecord make_child_shape_state(std::span<const std::uint8_t> blob, std::uint16_t index) {
    check_blob(blob, 1, 31);
    if (blob[1] != 31 && blob[1] != 32 && blob[1] != 40) throw std::runtime_error("unsupported ChildShape save variant");
    return state_record(blob, 1, index, 27);
}
NetObjectRecord make_joint_state(std::span<const std::uint8_t> blob, std::uint16_t index) {
    check_blob(blob, 2, 39);
    if (blob[1] != 3 && blob[1] != 4 && blob[1] != 28 && blob[1] != 41) throw std::runtime_error("unsupported Joint save variant");
    return state_record(blob, 2, index, 31);
}
NetObjectRecord make_character_creation(std::uint32_t id,const CharacterCreation& character) {
    if(!id||id==UINT32_MAX)throw std::invalid_argument("invalid-character-id");
    NetObjectRecord result;result.operation=1;result.type=6;result.subtype=0;result.id=id;
    result.payload=serialize_character_creation(character);return result;
}
NetObjectRecord make_character_state(std::uint32_t id,const CharacterInitialState& character) {
    if(!id||id==UINT32_MAX)throw std::invalid_argument("invalid-character-id");
    NetObjectRecord result;result.operation=3;result.type=6;result.id=id;
    result.payload=serialize_character_initial_state(character);return result;
}
NetObjectRecord make_container_creation(const scrapds::StoredContainer& container) {
    if(!container.id||container.id==UINT32_MAX||!container.supported_layout||
       container.serialized_entries.size()!=container.entry_count||container.entry_count>1023)
        throw std::invalid_argument("invalid-container-create-state");
    BitWriter bits;
    bits.little_endian_bits(container.entry_count,10);
    bits.bit(false); // No slot filter (the PlayerManager default is 0xffff).
    for(const auto& entry:container.serialized_entries)write_container_slot_uuid(bits,entry);
    for(const auto& entry:container.serialized_entries)
        if(!container_slot_empty(entry))bits.little_endian_bits(container_slot_quantity(entry),32);
    for(const auto& entry:container.serialized_entries)
        if(!container_slot_empty(entry))bits.little_endian_bits(container_slot_type(entry),16);
    bits.bit(false); // No auxiliary UUID list.
    NetObjectRecord result;result.operation=1;result.type=4;result.subtype=0;result.id=container.id;
    result.payload=std::move(bits).finish();return result;
}
NetObjectRecord make_container_slot_state(const scrapds::StoredContainer& container,std::uint32_t slot) {
    if(!container.id||container.id==UINT32_MAX||!container.supported_layout||
       container.serialized_entries.size()!=container.entry_count||slot>=container.entry_count)
        throw std::invalid_argument("invalid-container-slot-state");
    const auto& entry=container.serialized_entries[slot];BitWriter bits;
    bits.bit(false);bits.bit(true);bits.bit(false);
    bits.little_endian_bits(1,10);
    write_container_slot_uuid(bits,entry);
    if(!container_slot_empty(entry)) {
        bits.little_endian_bits(container_slot_quantity(entry),32);
        bits.little_endian_bits(container_slot_type(entry),16);
    }
    bits.little_endian_bits(slot,10);
    bits.bit(false);
    NetObjectRecord result;result.operation=3;result.type=4;result.id=container.id;
    result.payload=std::move(bits).finish();return result;
}
void merge_asset_catalog(InitialAssetCatalog& target,const InitialAssetCatalog& incoming) {
    auto merged=target;
    auto merge=[](auto& destination,const auto& source) {
        std::map<std::uint16_t,InitialAssetCatalog::SavedUuid> by_index;
        for(const auto& [uuid,index]:destination)if(!by_index.emplace(index,uuid).second)throw std::runtime_error("duplicate asset catalog index");
        for(const auto& [uuid,index]:source) {
            const auto current=destination.find(uuid);
            if(current!=destination.end()&&current->second!=index)throw std::runtime_error("conflicting asset catalog UUID");
            const auto existing=by_index.find(index);
            if(existing!=by_index.end()&&existing->second!=uuid)throw std::runtime_error("conflicting asset catalog index");
            destination.emplace(uuid,index);by_index.emplace(index,uuid);
        }
    };
    merge(merged.shapes,incoming.shapes);merge(merged.joints,incoming.joints);
    target=std::move(merged);
}
static void validate_catalog_hash(std::string_view hash) {
    if(hash.size()!=64||!std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))
        throw std::runtime_error("invalid bootstrap SHA-256");
}
std::vector<std::uint8_t> serialize_asset_catalog(const InitialAssetCatalog& catalog,std::string_view hash) {
    validate_catalog_hash(hash);
    InitialAssetCatalog checked;merge_asset_catalog(checked,catalog);
    std::vector<std::uint8_t> result{'S','D','S','C','A','T','1',0};
    result.insert(result.end(),hash.begin(),hash.end());
    for(const auto* entries:{&checked.shapes,&checked.joints}) {
        if(entries->size()>65536)throw std::runtime_error("asset catalog too large");
        put32(result,static_cast<std::uint32_t>(entries->size()));
        for(const auto& [uuid,index]:*entries) {append(result,uuid);result.push_back(std::uint8_t(index>>8));result.push_back(std::uint8_t(index));}
    }
    return result;
}
InitialAssetCatalog parse_asset_catalog(std::span<const std::uint8_t> bytes,std::string_view hash) {
    validate_catalog_hash(hash);std::size_t offset=0;
    const auto magic=take(bytes,offset,8);
    constexpr std::array<std::uint8_t,8> expected{'S','D','S','C','A','T','1',0};
    if(!std::equal(magic.begin(),magic.end(),expected.begin()))throw std::runtime_error("invalid asset catalog header");
    const auto stored_hash=take(bytes,offset,64);
    if(!std::equal(stored_hash.begin(),stored_hash.end(),hash.begin()))throw std::runtime_error("asset catalog belongs to another bootstrap");
    InitialAssetCatalog result;
    for(auto* entries:{&result.shapes,&result.joints}) {
        const auto count=read32(bytes,offset);offset+=4;
        if(count>65536||count>(bytes.size()-offset)/18)throw std::runtime_error("invalid asset catalog entry count");
        for(std::uint32_t i=0;i<count;++i) {
            InitialAssetCatalog::SavedUuid uuid{};const auto raw=take(bytes,offset,16);std::copy(raw.begin(),raw.end(),uuid.begin());
            const auto index=take16(bytes,offset);
            if(!entries->emplace(uuid,index).second)throw std::runtime_error("duplicate asset catalog UUID");
        }
    }
    if(offset!=bytes.size())throw std::runtime_error("trailing asset catalog data");
    InitialAssetCatalog checked;merge_asset_catalog(checked,result);return checked;
}
InitialAssetCatalog infer_initial_asset_catalog(const NetObjectPacket& captured, const scrapds::WorldState& world) {
    if (captured.message_id != 21) throw std::runtime_error("native initial-world bootstrap requires NetObj ID21");
    InitialAssetCatalog result;
    for (const auto& record : captured.records) {
        if (record.operation != 3 || (record.type != 1 && record.type != 2)) continue;
        if (record.payload.size() < 2) throw std::runtime_error("initial state has no asset catalog index");
        const auto index = std::uint16_t((record.payload[0] << 8) | record.payload[1]);
        auto& catalog = record.type == 1 ? result.shapes : result.joints;
        const Blob* blob=nullptr;
        if(record.type==1) { const auto it=world.shapes().find(record.id); if(it!=world.shapes().end())blob=&it->second.data; }
        else { const auto it=world.joints().find(record.id); if(it!=world.joints().end())blob=&it->second.data; }
        // A checkpoint may no longer contain entities from the replay
        // template. Surviving entities still establish their catalog entries.
        if(!blob)continue;
        const auto key = saved_uuid(*blob, record.type == 1 ? 11 : 15);
        const auto [position, inserted] = catalog.emplace(key, index);
        if (!inserted && position->second != index) throw std::runtime_error("conflicting asset catalog indices");
    }
    return result;
}
NativeInitialWorld rebuild_initial_world(const NetObjectPacket& captured, const scrapds::WorldState& world, std::uint32_t tick) {
    return rebuild_initial_world(captured, world, tick, infer_initial_asset_catalog(captured, world));
}
NativeInitialWorld rebuild_initial_world(const NetObjectPacket& captured, const scrapds::WorldState& world, std::uint32_t tick, const InitialAssetCatalog& catalog) {
    if (captured.message_id != 21) throw std::runtime_error("native initial-world bootstrap requires NetObj ID21");
    const auto& shape_catalog = catalog.shapes;
    const auto& joint_catalog = catalog.joints;
    std::set<std::uint32_t> body_ids, shape_ids, joint_ids;
    for (const auto& record : captured.records) {
        if (record.operation == 1) {
            if (record.type == 0) body_ids.insert(record.id);
            else if (record.type == 1) shape_ids.insert(record.id);
            else if (record.type == 2) joint_ids.insert(record.id);
        }
    }
    auto equal_ids = [](const auto& ids, const auto& entities) {
        if (ids.size() != entities.size()) return false;
        for (const auto& [id, entity] : entities) if (!ids.contains(id)) return false;
        return true;
    };
    if (!equal_ids(body_ids, world.bodies()) || !equal_ids(shape_ids, world.shapes()) || !equal_ids(joint_ids, world.joints())) throw std::runtime_error("bootstrap entity identities differ from the loaded save");
    NativeInitialWorld result;
    result.packet = captured;
    result.packet.tick = tick;
    for (auto& record : result.packet.records) {
        if (record.type == 0 && record.operation == 1) {
            record = make_body_creation(world.bodies().at(record.id).data);
            ++result.native_body_creations;
        } else if (record.type == 1 && record.operation == 3) {
            const auto& blob = world.shapes().at(record.id).data;
            record = make_child_shape_state(blob, shape_catalog.at(saved_uuid(blob, 11)));
            ++result.native_shape_states;
        } else if (record.type == 2 && record.operation == 3) {
            const auto& blob = world.joints().at(record.id).data;
            record = make_joint_state(blob, joint_catalog.at(saved_uuid(blob, 15)));
            ++result.native_joint_states;
        } else if (record.type == 0 && record.operation == 3) {
            const auto& blob = world.bodies().at(record.id).data;
            if (blob.size() == 60 && blob[1] == 1) {
                record.payload.assign(blob.begin() + 55, blob.end());
                ++result.native_static_body_states;
            } else ++result.preserved_dynamic_body_states;
        } else ++result.preserved_records;
    }
    return result;
}
NativeInitialWorld rebuild_live_initial_world(const NetObjectPacket& captured, const scrapds::WorldState& world, std::uint32_t tick, const InitialAssetCatalog& catalog) {
    if (captured.message_id != 21) throw std::runtime_error("live initial-world bootstrap requires NetObj ID21");
    std::set<std::uint32_t> bodies, shapes, joints, containers;
    for (const auto& record : captured.records) {
        if (record.operation != 1) continue;
        if (record.type == 0) bodies.insert(record.id);
        else if (record.type == 1) shapes.insert(record.id);
        else if (record.type == 2) joints.insert(record.id);
        else if (record.type == 4) containers.insert(record.id);
    }
    std::vector<NetObjectRecord> body_creates, shape_creates, shape_states, body_states, joint_creates, joint_states, container_creates;
    NetObjectPacket expanded = captured;
    // The replay packet is a layout/catalog template. Drop stale world objects
    // from it so deletes and replacement saves don't resurrect captured data.
    std::erase_if(expanded.records,[&](const NetObjectRecord& record) {
        if(record.type==4&&record.operation==3)return true;
        if(record.operation!=1&&record.operation!=3)return false;
        if(record.type==0)return !world.bodies().contains(record.id);
        if(record.type==1)return !world.shapes().contains(record.id);
        if(record.type==2)return !world.joints().contains(record.id);
        if(record.type==4)return !world.container_ids().contains(record.id);
        return false;
    });
    for(auto& record:expanded.records)if(record.operation==1&&record.type==4) {
        const auto container=world.containers().find(record.id);
        if(container==world.containers().end())throw std::runtime_error("captured Container has no save row");
        if(container->second.supported_layout)record=make_container_creation(container->second);
    }
    for(const auto& [id,container]:world.containers())if(!containers.contains(id)&&container.supported_layout)
        container_creates.push_back(make_container_creation(container));
    for (const auto& [id, body] : world.bodies()) {
        if (bodies.contains(id)) continue;
        if (body.data.size() != 60 || body.data[0] != 0 || body.data[1] != 1 || body.data[2] != 1) throw std::runtime_error("new dynamic or unknown body lacks native runtime state");
        body_creates.push_back(make_body_creation(body.data));
        NetObjectRecord state;
        state.operation = 3; state.type = 0; state.id = id;
        state.payload.assign(body.data.begin() + 55, body.data.end());
        body_states.push_back(std::move(state));
    }
    for (const auto& [id, shape] : world.shapes()) {
        if (shapes.contains(id)) continue;
        if (shape.data.size() != 48 || shape.data[0] != 1 || shape.data[1] != 31 || shape.data[2] != 1) throw std::runtime_error("live initial synchronization only supports new native blocks");
        if (!world.bodies().contains(shape.body_id)) throw std::runtime_error("new shape has no native body");
        NetObjectRecord creation;
        creation.operation = 1; creation.type = 1; creation.subtype = shape.data[1]; creation.id = id;
        shape_creates.push_back(std::move(creation));
        const auto key = saved_uuid(shape.data, 11);
        const auto index = catalog.shapes.find(key);
        if (index == catalog.shapes.end()) throw std::runtime_error("new block UUID has no established asset catalog index");
        shape_states.push_back(make_child_shape_state(shape.data, index->second));
    }
    for(const auto& [id,joint]:world.joints()) {
        if(joints.contains(id))continue;
        if(joint.data.size()!=69||joint.data[0]!=2||joint.data[2]!=1||
           (joint.data[1]!=3&&joint.data[1]!=4&&joint.data[1]!=28&&joint.data[1]!=41))
            throw std::runtime_error("live initial synchronization only supports known 69-byte joints");
        if((joint.shape_a!=UINT32_MAX&&!world.shapes().contains(joint.shape_a))||
           (joint.shape_b!=UINT32_MAX&&!world.shapes().contains(joint.shape_b)))
            throw std::runtime_error("new joint references a missing shape");
        const auto index=catalog.joints.find(saved_uuid(joint.data,15));
        if(index==catalog.joints.end())throw std::runtime_error("new joint UUID has no established asset catalog index");
        NetObjectRecord creation;creation.operation=1;creation.type=2;creation.subtype=joint.data[1];creation.id=id;
        joint_creates.push_back(std::move(creation));
        joint_states.push_back(make_joint_state(joint.data,index->second));
    }
    // Preserve the original object's dependency ordering: all body creates,
    // all shape creates, all shape states, all body states, then joints.
    auto insert_group = [&](std::uint8_t operation, std::uint8_t type, std::vector<NetObjectRecord>& added) {
        if (added.empty()) return;
        auto position = expanded.records.end();
        bool found = false;
        for (auto it = expanded.records.begin(); it != expanded.records.end(); ++it) {
            if (it->operation == operation && it->type == type) { position = it + 1; found = true; }
        }
        if (!found) {
            // A captured empty group has no end marker. Select the first
            // subsequent supported group, retaining unrelated bootstrap data.
            const auto rank = [](const NetObjectRecord& record) {
                if (record.operation == 1 && record.type == 0) return 0;
                if (record.operation == 1 && record.type == 1) return 1;
                if (record.operation == 3 && record.type == 1) return 2;
                if (record.operation == 3 && record.type == 0) return 3;
                if (record.type == 2) return 4;
                return -1;
            };
            const int target = operation == 1 ? (type == 0 ? 0 : 1) : (type == 1 ? 2 : 3);
            position = std::find_if(expanded.records.begin(), expanded.records.end(), [&](const auto& record) { return rank(record) > target; });
        }
        expanded.records.insert(position, std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    };
    insert_group(1, 0, body_creates);
    insert_group(1, 1, shape_creates);
    insert_group(3, 1, shape_states);
    insert_group(3, 0, body_states);
    insert_group(1, 2, joint_creates);
    insert_group(3, 2, joint_states);
    insert_group(1, 4, container_creates);
    return rebuild_initial_world(expanded, world, tick, catalog);
}
}
