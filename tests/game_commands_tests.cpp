#include "net/game_commands.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <limits>
#include <stdexcept>

using namespace scrapds::net;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char* message) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
}
int main() {
    try {
        // Independent byte-order oracle: entire packed struct reversed, as the
        // game's dispatcher does. Hand-written values exercise every field.
        const std::vector<std::uint8_t> remove{
            0x23,
            0,0,0,6, 0,0,0,5, 0,0,0,4, // b.z,b.y,b.x
            0xff,0xff,0xff,0xfd, 0xff,0xff,0xff,0xfe, 0xff,0xff,0xff,0xff,
            4, 0x55,0x66,0x77,0x88, 0x11,0x22,0x33,0x44};
        const auto parsed = std::get<RemoveShapeCommand>(parse_game_command(remove));
        require(parsed.selection.body_id==0x11223344 && parsed.selection.child_shape_id==0x55667788, "entity IDs/order wrong");
        require(parsed.selection.corner_a==std::array<std::int32_t,3>{-1,-2,-3}, "signed corner_a/order wrong");
        require(parsed.selection.corner_b==std::array<std::int32_t,3>{4,5,6}, "corner_b/order wrong");
        require(serialize_game_command(parsed)==remove, "remove encoding differs from packed byte oracle");

        auto paint = remove;
        paint[0]=0x24;
        paint.insert(paint.begin()+1,{0x44,0x33,0x22,0x11});
        const auto colored = std::get<PaintShapeCommand>(parse_game_command(paint));
        require(colored.color_rgba==0x44332211 && colored.selection==parsed.selection, "shape paint color/order wrong");
        require(serialize_game_command(colored)==paint, "paint encoding differs");

        const std::vector<std::uint8_t> joint{0x25,0xaa,0xbb,0xcc,0xdd,0x10,0x20,0x30,0x40};
        const auto j = std::get<PaintJointCommand>(parse_game_command(joint));
        require(j.joint_id==0x10203040 && j.color_rgba==0xaabbccdd, "joint schema wrong");
        require(serialize_game_command(j)==joint, "joint encoding wrong");
        auto character=joint;character[0]=0x26;
        const auto c = std::get<PaintCharacterCommand>(parse_game_command(character));
        require(c.character_id==j.joint_id && c.color_rgba==j.color_rgba, "character schema wrong");
        require(serialize_game_command(c)==character, "character encoding wrong");

        const std::vector<std::uint8_t> harvestable{0x28,0x10,0x20,0x30,0x40};
        const auto harvest=std::get<InteractHarvestableCommand>(parse_game_command(harvestable));
        require(harvest.harvestable_id==0x10203040&&serialize_game_command(harvest)==harvestable,"harvestable interaction layout wrong");
        const std::vector<std::uint8_t> character_interaction{0x29,0x50,0x60,0x70,0x80};
        const auto interaction=std::get<InteractCharacterCommand>(parse_game_command(character_interaction));
        require(interaction.character_id==0x50607080&&serialize_game_command(interaction)==character_interaction,"character interaction layout wrong");
        for (const auto& bytes : {remove,paint,joint,character,harvestable,character_interaction}) {
            for (std::size_t n=0;n<bytes.size();++n)
                rejects([&]{parse_game_command(std::span(bytes).first(n));}, "truncated command accepted");
            auto trailing=bytes;trailing.push_back(0);
            rejects([&]{parse_game_command(trailing);}, "trailing bytes accepted");
        }
        rejects([]{parse_game_command(std::vector<std::uint8_t>{0x21});}, "unknown command accepted");
        auto edge=parsed;
        edge.selection.corner_a={std::numeric_limits<std::int32_t>::min(),0,std::numeric_limits<std::int32_t>::max()};
        edge.selection.child_shape_id=0;
        require(std::get<RemoveShapeCommand>(parse_game_command(serialize_game_command(edge)))==edge, "boundary coordinates lost");
        const auto fixture=std::filesystem::path(__FILE__).parent_path()/"fixtures"/"game-build-command.bin";
        std::ifstream source(fixture,std::ios::binary);
        require(source.good(),"missing real build-command fixture");
        const std::vector<std::uint8_t> build_bytes((std::istreambuf_iterator<char>(source)),std::istreambuf_iterator<char>());
        const auto build=std::get<BuildShapeCommand>(parse_game_command(build_bytes));
        require(build.kind==2 && build.body_id==0 && build.child_shape_id==0,"captured build selector wrong");
        require(build.cell_position==std::array<std::int32_t,3>{54,63,0},"captured build position wrong");
        require(build.axis_a==std::array<std::int32_t,3>{1,0,0} && build.axis_b==std::array<std::int32_t,3>{0,0,1},"captured build axes wrong");
        require(build.block_dimensions==std::array<std::int32_t,3>{1,2,5},"captured build dimensions wrong");
        require(build.quantity==1 && build.inventory_slot==2,"captured inventory fields wrong");
        require(build.shape_uuid==std::array<std::uint8_t,16>{0xa6,0xc6,0xce,0x30,0xdd,0x47,0x45,0x87,0xb4,0x75,0x08,0x5d,0x55,0xc6,0xa3,0xb4},"captured asset UUID order wrong");
        require(build.inventory_uuid==build.shape_uuid,"captured item UUID wrong");
        require(serialize_game_command(build)==build_bytes,"build byte-for-byte roundtrip differs from real client");
        for(std::size_t n=0;n<build_bytes.size();++n)
            rejects([&]{parse_game_command(std::span(build_bytes).first(n));},"truncated build accepted");
        auto extra_build=build_bytes;extra_build.push_back(0);
        rejects([&]{parse_game_command(extra_build);},"trailing build bytes accepted");
        std::cout << "game command checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
