#include "replication.hpp"
#include <algorithm>
#include <stdexcept>

namespace scrapds {
// Keep this translation unit tied to the complete GameCommand variant used by
// the listener, including explicitly rejected interaction request types.
net::NetObjectPacket make_object_update(const WorldState& world, const CommandResult& result,
                                        std::uint32_t tick, const net::InitialAssetCatalog& catalog) {
    net::NetObjectPacket update;
    update.message_id = 22;
    update.tick = tick;
    for (const auto id : result.created_bodies) {
        const auto it = world.bodies().find(id);
        if (it == world.bodies().end()) throw std::runtime_error("created body is absent from world state");
        update.records.push_back(net::make_body_creation(it->second.data));
    }
    for (const auto id : result.removed_shapes) update.records.push_back({2, 1, {}, id, {}});
    for (const auto id : result.removed_bodies) update.records.push_back({2, 0, {}, id, {}});
    for (const auto id : result.removed_joints) update.records.push_back({2, 2, {}, id, {}});
    for (const auto id : result.created_shapes) {
        const auto it = world.shapes().find(id);
        if (it == world.shapes().end()) throw std::runtime_error("created shape is absent from world state");
        if (it->second.data.size() < 3 || it->second.data[0] != 1) throw std::runtime_error("created shape has an unsupported BLOB");
        net::NetObjectRecord creation;
        creation.operation = 1;
        creation.type = 1;
        creation.subtype = it->second.data[1];
        creation.id = id;
        update.records.push_back(std::move(creation));
    }
    std::vector<std::uint32_t> states = result.changed_shapes;
    states.insert(states.end(), result.created_shapes.begin(), result.created_shapes.end());
    for (const auto id : states) {
        const auto it = world.shapes().find(id);
        if (it == world.shapes().end()) throw std::runtime_error("replicated shape is absent from world state");
        const auto& blob = it->second.data;
        if (blob.size() < 27 || blob[0] != 1) throw std::runtime_error("replicated shape has an unsupported BLOB");
        net::InitialAssetCatalog::SavedUuid uuid{};
        std::copy_n(blob.begin() + 11, 16, uuid.begin());
        const auto index = catalog.shapes.find(uuid);
        if (index == catalog.shapes.end()) throw std::runtime_error("replicated shape UUID has no asset catalog index");
        update.records.push_back(net::make_child_shape_state(blob, index->second));
    }
    for (const auto id : result.created_bodies) {
        const auto& blob = world.bodies().at(id).data;
        if (blob.size() < 60) throw std::runtime_error("created body has an unsupported BLOB");
        net::NetObjectRecord state;
        state.operation = 3;
        state.type = 0;
        state.id = id;
        state.payload.assign(blob.begin() + 55, blob.end());
        update.records.push_back(std::move(state));
    }
    for(const auto id:result.changed_joints) {
        const auto it=world.joints().find(id);
        if(it==world.joints().end())throw std::runtime_error("replicated joint is absent from world");
        const auto& blob=it->second.data;
        if(blob.size()<31)throw std::runtime_error("replicated joint has an unsupported BLOB");
        net::InitialAssetCatalog::SavedUuid uuid{};
        std::copy_n(blob.begin()+15,16,uuid.begin());
        const auto index=catalog.joints.find(uuid);
        if(index==catalog.joints.end())throw std::runtime_error("replicated joint UUID has no asset catalog index");
        update.records.push_back(net::make_joint_state(blob,index->second));
    }
    for(const auto& changed:result.changed_container_slots) {
        const auto it=world.containers().find(changed.container_id);
        if(it==world.containers().end())throw std::runtime_error("changed container is absent from world");
        update.records.push_back(net::make_container_slot_state(it->second,changed.slot));
    }
    return update;
}
ReplicatedCommandResult apply_replicated_command(WorldState& world,const AssetDefinitions& assets,
    const net::GameCommand& command,std::uint32_t tick,const net::InitialAssetCatalog& catalog,std::uint16_t world_id,std::optional<std::uint32_t> player_container_id) {
    ReplicatedCommandResult result;
    world.atomic([&] {
        if(const auto* build=std::get_if<net::BuildShapeCommand>(&command)) {
            net::InitialAssetCatalog::SavedUuid uuid{};
            std::reverse_copy(build->shape_uuid.begin(),build->shape_uuid.end(),uuid.begin());
            if(!catalog.shapes.contains(uuid)) { result.command={false,"asset-not-in-bootstrap-client-catalog",{}}; return false; }
        }
        result.command=apply_game_command(world,assets,command,world_id,player_container_id);
        if(!result.command.applied)return false;
        auto packet=make_object_update(world,result.command,tick,catalog);
        if(!packet.records.empty())result.response=net::make_net_object_packet(packet);
        return true;
    });
    return result;
}
}
