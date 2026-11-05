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
static CommandResult apply_game_command_impl(WorldState& world,const AssetDefinitions& assets,const net::GameCommand& command, std::uint16_t world_id) {
    if(const auto* remove_joint=std::get_if<net::RemoveJointCommand>(&command)) {
        // The vanilla handler returns the joint item through an inventory
        // transaction. Until that transaction is modeled, only expose the
        // no-refund Creative path.
        if(world.metadata().flags!=1)return {false,"joint-removal-requires-supported-creative-mode",{}};
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
            world.remove_joint(remove_joint->joint_id);
            CommandResult result{true,"applied"};
            result.removed_joints.push_back(remove_joint->joint_id);
            return result;
        } catch(const std::runtime_error& error) {return {false,error.what(),{}};}
    }
    if(const auto* paint_joint=std::get_if<net::PaintJointCommand>(&command)) {
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
        try {
            const auto created=world.create_block(request);
            CommandResult result{true,"applied",{}, {}, {created.shape_id}};
            if(created.created_body)result.created_bodies.push_back(created.body_id);
            return result;
        } catch(const std::runtime_error& error) {return {false,error.what(),{}};}
    }
    if(const auto* remove=std::get_if<net::RemoveShapeCommand>(&command)) {
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
CommandResult apply_game_command(WorldState& world,const AssetDefinitions& assets,const net::GameCommand& command,std::uint16_t world_id) {
    CommandResult result;
    world.atomic([&] { result=apply_game_command_impl(world,assets,command,world_id); return result.applied; });
    return result;
}
}
