#include "commands.hpp"
#include "data/shape_geometry.hpp"
#include <algorithm>
#include <map>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace scrapds {
namespace {
void put32(Blob& blob,std::size_t offset,std::uint32_t value) { for(int i=0;i<4;++i)blob.at(offset+i)=std::uint8_t(value>>(24-8*i)); }
std::string joint_uuid(const Blob& data) {
    if(data.size()<31) return {};
    std::ostringstream uuid;uuid<<std::hex<<std::setfill('0');
    for(std::size_t i=0;i<16;++i) {if(i==4||i==6||i==8||i==10)uuid<<'-';uuid<<std::setw(2)<<unsigned(data[30-i]);}
    return uuid.str();
}
}
static CommandResult apply_game_command_impl(WorldState& world,const AssetDefinitions& assets,const net::GameCommand& command, std::uint16_t world_id,std::optional<std::uint32_t> player_container_id) {
    if(std::holds_alternative<net::InteractHarvestableCommand>(command))return {false,"harvestable-interaction-not-implemented",{}};
    if(std::holds_alternative<net::InteractCharacterCommand>(command))return {false,"character-interaction-not-implemented",{}};
    if(const auto* remove_joint=std::get_if<net::RemoveJointCommand>(&command)) {
        const auto mode=world.metadata().flags;
        if(mode!=1&&mode!=14)return {false,"joint-removal-requires-supported-game-mode",{}};
        if(mode==14&&!player_container_id)return {false,"player-inventory-unavailable",{}};
        const auto joint=world.joints().find(remove_joint->joint_id);
        if(joint==world.joints().end())return {false,"unknown-joint",{}};
        const auto& data=joint->second.data;
        if(data.size()!=69||data[0]!=2||data[2]!=1||
           (data[1]!=3&&data[1]!=4&&data[1]!=28&&data[1]!=41))
            return {false,"unsupported-joint-removal-layout",{}};
        bool has_world_shape=false;
        for(const auto shape_id:{joint->second.shape_a,joint->second.shape_b}) {
            if(shape_id==UINT32_MAX)continue;
            has_world_shape=true;
            const auto shape=world.shapes().find(shape_id);
            if(shape==world.shapes().end())return {false,"joint-references-unknown-shape",{}};
            const auto body=world.bodies().find(shape->second.body_id);
            if(body==world.bodies().end()||body->second.world_id!=world_id)
                return {false,"joint-in-another-world",{}};
        }
        if(!has_world_shape)return {false,"joint-has-no-world-shape",{}};
        try {
            CommandResult result{true,"applied"};
            if(mode==14) {
                const auto* definition=assets.find(joint_uuid(data));
                if(!definition||!definition->stack_size)return {false,"joint-stack-size-unavailable",{}};
                std::array<std::uint8_t,16> item{};std::copy_n(data.begin()+15,16,item.begin());
                for(const auto slot:world.credit_container(*player_container_id,item,1,*definition->stack_size))
                    result.changed_container_slots.push_back({*player_container_id,slot});
            }
            world.remove_joint(remove_joint->joint_id);
            result.removed_joints.push_back(remove_joint->joint_id);
            return result;
        } catch(const std::runtime_error& error) {return {false,error.what(),{}};}
    }
    if(const auto* paint_joint=std::get_if<net::PaintJointCommand>(&command)) {
        if(world.metadata().flags!=1)return {false,"joint-paint-requires-supported-creative-mode",{}};
        const auto joint=world.joints().find(paint_joint->joint_id);
        if(joint==world.joints().end())return {false,"unknown-joint",{}};
        if(joint->second.data.size()!=69||joint->second.data[0]!=2||joint->second.data[2]!=1||
           (joint->second.data[1]!=3&&joint->second.data[1]!=4&&joint->second.data[1]!=28&&joint->second.data[1]!=41))
            return {false,"unsupported-joint-color-layout",{}};
        auto color=paint_joint->color_rgba;
        if(!color) {
            const auto* definition=assets.find(joint_uuid(joint->second.data));
            if(!definition||!definition->default_color_abgr)return {false,"joint-default-color-unavailable",{}};
            color=*definition->default_color_abgr;
        }
        auto data=joint->second.data;
        put32(data,65,color);
        try { world.replace_joint_blob(paint_joint->joint_id,std::move(data)); }
        catch(const std::runtime_error& error) { return {false,error.what(),{}}; }
        CommandResult result{true,"applied"};
        result.changed_joints.push_back(paint_joint->joint_id);
        return result;
    }
    if(const auto* build=std::get_if<net::BuildShapeCommand>(&command)) {
        const auto mode=world.metadata().flags;
        if(mode!=1&&mode!=14)return {false,"unsupported-game-mode-for-building",{}};
        if(build->kind!=2&&build->kind!=4)return {false,"unsupported-build-kind",{}};
        if(build->axis_a!=std::array<std::int32_t,3>{1,0,0}||build->axis_b!=std::array<std::int32_t,3>{0,0,1})return {false,"unsupported-build-orientation",{}};
        if(build->kind==2&&(build->body_id||build->child_shape_id))return {false,"new-body-build-has-target",{}};
        if(build->kind==4&&build->child_shape_id) {
            const auto it=world.shapes().find(build->child_shape_id);
            if(it==world.shapes().end()||it->second.body_id!=build->body_id)return {false,"shape-does-not-belong-to-body",{}};
        }
        std::ostringstream uuid;uuid<<std::hex<<std::setfill('0');
        for(std::size_t i=0;i<16;++i) {if(i==4||i==6||i==8||i==10)uuid<<'-';uuid<<std::setw(2)<<unsigned(build->shape_uuid[i]);}
        const auto* definition=assets.find(uuid.str());
        if(!definition||!definition->block)return {false,"build-requires-known-block-asset",{}};
        if(!definition->default_color_abgr)return {false,"default-asset-color-unavailable",{}};
        BlockCreation request;
        if(build->kind==4)request.body_id=build->body_id;
        request.world_id=world_id;request.uuid=build->shape_uuid;request.cell_position=build->cell_position;
        request.dimensions=build->block_dimensions;request.color_abgr=*definition->default_color_abgr;
        std::uint32_t item_cost=0;
        if(mode==14) {
            if(!player_container_id)return {false,"player-inventory-unavailable",{}};
            if(build->inventory_slot<0)return {false,"invalid-inventory-slot",{}};
            if(build->inventory_uuid!=build->shape_uuid)return {false,"inventory-item-does-not-match-built-block",{}};
            std::uint64_t volume=1;
            for(const auto dimension:build->block_dimensions) {
                if(dimension<=0||dimension>256)return {false,"invalid-block-dimensions",{}};
                volume*=static_cast<std::uint32_t>(dimension);
            }
            // The captured client oracle sends quantity=1 for a 1x2x5 block;
            // it is not the material cost. Charge the server-computed volume.
            if(volume>UINT32_MAX||!build->quantity)return {false,"invalid-build-quantity",{}};
            item_cost=static_cast<std::uint32_t>(volume);
        }
        try {
            const auto created=world.create_block(request);
            CommandResult result{true,"applied",{}, {}, {created.shape_id}};
            if(created.created_body)result.created_bodies.push_back(created.body_id);
            if(mode==14) {
                std::array<std::uint8_t,16> item_uuid{};
                std::reverse_copy(build->inventory_uuid.begin(),build->inventory_uuid.end(),item_uuid.begin());
                world.spend_container_slot(*player_container_id,static_cast<std::uint32_t>(build->inventory_slot),item_uuid,item_cost);
                result.changed_container_slots.push_back({*player_container_id,static_cast<std::uint32_t>(build->inventory_slot)});
            }
            return result;
        } catch(const std::runtime_error& error) {return {false,error.what(),{}};}
    }
    if(const auto* remove=std::get_if<net::RemoveShapeCommand>(&command)) {
        const auto mode=world.metadata().flags;
        if(mode!=1&&mode!=14)return {false,"shape-removal-requires-supported-game-mode",{}};
        if(mode==14&&!player_container_id)return {false,"player-inventory-unavailable",{}};
        const auto& selection=remove->selection;
        if(selection.kind!=4)return {false,"unsupported-selection-kind",{}};
        if(!world.bodies().contains(selection.body_id))return {false,"unknown-body",{}};
        if(world.bodies().at(selection.body_id).world_id!=world_id)return {false,"body-in-another-world",{}};
        std::vector<std::uint32_t> selected;
        struct Cut { std::array<std::int32_t,3> position{},dimensions{}; };
        std::map<std::uint32_t,Cut> partial;
        if(selection.child_shape_id) {
            const auto it=world.shapes().find(selection.child_shape_id);
            if(it==world.shapes().end()||it->second.body_id!=selection.body_id)return {false,"shape-does-not-belong-to-body",{}};
            selected.push_back(it->first);
        } else {
            std::array<std::int32_t,3> lo{},hi{};
            for(std::size_t i=0;i<3;++i) {
                lo[i]=selection.corner_a[i];hi[i]=selection.corner_b[i];
                if(lo[i]>=hi[i])return {false,"empty-or-inverted-block-region",{}};
            }
            for(const auto& [id,shape]:world.shapes()) {
                if(shape.body_id!=selection.body_id)continue;
                const auto geometry=decode_child_geometry(shape.data);
                if(!geometry||geometry->kind!=SavedShapeKind::block||!geometry->grid_dimensions)continue;
                bool intersects=true,contains=true;
                Cut cut;
                for(std::size_t i=0;i<3;++i) {
                    const auto a=geometry->grid_position[i],extent=geometry->grid_dimensions->at(i);
                    if(extent<=0){intersects=false;break;}
                    const auto b=std::int64_t(a)+extent;
                    if(hi[i]<=a||lo[i]>=b)intersects=false;
                    if(lo[i]>a||hi[i]<b)contains=false;
                    cut.position[i]=std::max(lo[i],a);
                    cut.dimensions[i]=std::int32_t(std::min<std::int64_t>(hi[i],b)-cut.position[i]);
                }
                if(intersects){selected.push_back(id);if(!contains)partial.emplace(id,cut);}
            }
        }
        if(selected.empty())return {false,"empty-selection",{}};
        try {
            for(const auto shape_id:selected) {
                for(const auto& [joint_id,joint]:world.joints()) {
                    (void)joint_id;
                    if(joint.shape_a==shape_id||joint.shape_b==shape_id)return {false,"shape-has-joint-reference",{}};
                }
                for(const auto& [controller_id,controller]:world.controllers()) {
                    (void)controller_id;
                    if(controller.host_id==shape_id)return {false,"shape-has-controller-reference",{}};
                }
            }
            CommandResult result{true,"applied"};
            if(mode==14) {
                struct Refund {std::array<std::uint8_t,16> item{};std::uint32_t amount{},stack_size{};};
                std::map<std::string,Refund> refunds;
                for(const auto shape_id:selected) {
                    const auto geometry=decode_child_geometry(world.shapes().at(shape_id).data);
                    if(!geometry||geometry->kind!=SavedShapeKind::block||!geometry->grid_dimensions)return {false,"survival-removal-requires-block",{}};
                    const auto* definition=assets.find(geometry->uuid);
                    if(!definition||!definition->block||!definition->stack_size)return {false,"block-stack-size-unavailable",{}};
                    const auto& dimensions=partial.contains(shape_id)?partial.at(shape_id).dimensions:*geometry->grid_dimensions;
                    std::uint64_t volume=1;for(const auto dimension:dimensions) {if(dimension<=0)return {false,"invalid-block-dimensions",{}};volume*=static_cast<std::uint32_t>(dimension);}
                    auto& refund=refunds[geometry->uuid];
                    if(volume>UINT32_MAX-1u-refund.amount)return {false,"block-refund-too-large",{}};
                    refund.amount+=static_cast<std::uint32_t>(volume);refund.stack_size=*definition->stack_size;
                    std::string hex;for(const auto c:geometry->uuid)if(c!='-')hex.push_back(c);
                    if(hex.size()!=32)return {false,"invalid-block-uuid",{}};
                    const auto digit=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
                    for(std::size_t i=0;i<16;++i) {const auto high=digit(hex[2*i]),low=digit(hex[2*i+1]);if(high<0||low<0)return {false,"invalid-block-uuid",{}};refund.item[15-i]=std::uint8_t(high*16+low);}
                }
                for(const auto& [uuid,refund]:refunds) {
                    (void)uuid;
                    for(const auto slot:world.credit_container(*player_container_id,refund.item,refund.amount,refund.stack_size))
                        result.changed_container_slots.push_back({*player_container_id,slot});
                }
            }
            for(const auto& [shape_id,cut]:partial) {
                const auto geometry=decode_child_geometry(world.shapes().at(shape_id).data);
                const auto split=world.split_block_shape(shape_id,cut.position,cut.dimensions,geometry->color_abgr);
                result.created_shapes.insert(result.created_shapes.end(),split.fragment_shape_ids.begin(),split.fragment_shape_ids.end());
            }
            const auto removed=world.remove_shapes(selected);
            result.removed_shapes=removed.shape_ids;result.removed_bodies=removed.empty_body_ids;return result;
        } catch(const std::runtime_error& error) {return {false,error.what(),{}};}
    }
    const auto* paint=std::get_if<net::PaintShapeCommand>(&command);
    if(!paint)return {false,"command-not-implemented",{}};
    if(world.metadata().flags!=1)return {false,"shape-paint-requires-supported-creative-mode",{}};
    const auto& selection=paint->selection;
    if(selection.kind!=4)return {false,"unsupported-selection-kind",{}};
    if(!world.bodies().contains(selection.body_id))return {false,"unknown-body",{}};
    if(world.bodies().at(selection.body_id).world_id!=world_id)return {false,"body-in-another-world",{}};
    std::vector<std::uint32_t> selected;
    struct Cut { std::array<std::int32_t,3> position{},dimensions{}; };
    std::map<std::uint32_t,Cut> partial;
    if(selection.child_shape_id) {
        const auto it=world.shapes().find(selection.child_shape_id);
        if(it==world.shapes().end()||it->second.body_id!=selection.body_id)return {false,"shape-does-not-belong-to-body",{}};
        selected.push_back(it->first);
    } else {
        std::array<std::int32_t,3> lo{},hi{};
        for(std::size_t i=0;i<3;++i) {
            lo[i]=selection.corner_a[i];hi[i]=selection.corner_b[i];
            if(lo[i]>=hi[i])return {false,"empty-or-inverted-block-region",{}};
        }
        for(const auto& [id,shape]:world.shapes()) {
            if(shape.body_id!=selection.body_id)continue;
            const auto geometry=decode_child_geometry(shape.data);
            if(!geometry||geometry->kind!=SavedShapeKind::block)continue;
            bool intersects=true,contains=true;
            Cut cut;
            for(std::size_t i=0;i<3;++i) {
                const auto a=geometry->grid_position[i];
                const auto extent=geometry->grid_dimensions->at(i);
                if(extent<=0){intersects=false;break;}
                const auto b=std::int64_t(a)+extent;
                if(hi[i]<=a||lo[i]>=b)intersects=false;
                if(lo[i]>a||hi[i]<b)contains=false;
                cut.position[i]=std::max(lo[i],a);
                cut.dimensions[i]=std::int32_t(std::min<std::int64_t>(hi[i],b)-cut.position[i]);
            }
            if(intersects){selected.push_back(id);if(!contains)partial.emplace(id,cut);}
        }
    }
    if(selected.empty())return {false,"empty-selection",{}};
    std::map<std::uint32_t,Blob> mutations;
    CommandResult result{true,"applied",{}};
    for(const auto id:selected) {
        const auto& shape=world.shapes().at(id);
        const auto geometry=decode_child_geometry(shape.data);
        if(!geometry)return {false,"unknown-shape-format",{}};
        std::uint32_t color=paint->color_rgba;
        if(!color) {
            const auto* definition=assets.find(geometry->uuid);
            if(!definition||!definition->default_color_abgr)return {false,"default-asset-color-unavailable",{}};
            color=*definition->default_color_abgr;
        }
        if(geometry->color_abgr==color)continue;
        if(const auto cut=partial.find(id);cut!=partial.end()) {
            const auto split=world.split_block_shape(id,cut->second.position,cut->second.dimensions,color);
            result.changed_shapes.push_back(id);
            result.created_shapes.insert(result.created_shapes.end(),split.fragment_shape_ids.begin(),split.fragment_shape_ids.end());
            continue;
        }
        auto data=shape.data; put32(data,37,color); mutations.emplace(id,std::move(data));
    }
    for(const auto& [id,data]:mutations) { (void)data; result.changed_shapes.push_back(id); }
    world.replace_shape_blobs(std::move(mutations));
    return result;
}
CommandResult apply_game_command(WorldState& world,const AssetDefinitions& assets,const net::GameCommand& command,std::uint16_t world_id,std::optional<std::uint32_t> player_container_id) {
    CommandResult result;
    world.atomic([&] { result=apply_game_command_impl(world,assets,command,world_id,player_container_id); return result.applied; });
    return result;
}
}
