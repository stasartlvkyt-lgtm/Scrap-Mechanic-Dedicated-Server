#include "net/game_commands.hpp"

#include <bit>
#include <stdexcept>
#include <type_traits>

namespace scrapds::net {
namespace {
class Reader {
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_{};
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint8_t byte() {
        if (cursor_ >= bytes_.size()) throw std::invalid_argument("truncated game command");
        return bytes_[cursor_++];
    }
    std::uint32_t word() {
        std::uint32_t value = 0;
        for (int i=0; i<4; ++i) value = (value << 8) | byte();
        return value;
    }
    std::int32_t signed_word() { return std::bit_cast<std::int32_t>(word()); }
    void finish() const {
        if (cursor_ != bytes_.size()) throw std::invalid_argument("trailing bytes after game command");
    }
};
void word(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift=24; shift>=0; shift-=8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::array<std::int32_t,3> vector(Reader& input) {
    std::array<std::int32_t,3> value{};
    for (int i=2; i>=0; --i) value[i]=input.signed_word();
    return value;
}
void vector(std::vector<std::uint8_t>& bytes,const std::array<std::int32_t,3>& value) {
    for (int i=2; i>=0; --i) word(bytes,std::bit_cast<std::uint32_t>(value[i]));
}
std::array<std::uint8_t,16> uuid(Reader& input) {
    std::array<std::uint8_t,16> value{};
    for (int i=15;i>=0;--i)value[i]=input.byte();
    return value;
}
void uuid(std::vector<std::uint8_t>& bytes,const std::array<std::uint8_t,16>& value) {
    for (int i=15;i>=0;--i)bytes.push_back(value[i]);
}
ShapeSelection selection(Reader& input) {
    // The game reverses the entire packed33-byte object. Therefore vector and
    // field ORDER is reversed on the wire, in addition to each integer's bytes.
    ShapeSelection value;
    for (int i=2; i>=0; --i) value.corner_b[i] = input.signed_word();
    for (int i=2; i>=0; --i) value.corner_a[i] = input.signed_word();
    value.kind = input.byte();
    value.child_shape_id = input.word();
    value.body_id = input.word();
    return value;
}
void selection(std::vector<std::uint8_t>& bytes, const ShapeSelection& value) {
    for (int i=2; i>=0; --i) word(bytes, std::bit_cast<std::uint32_t>(value.corner_b[i]));
    for (int i=2; i>=0; --i) word(bytes, std::bit_cast<std::uint32_t>(value.corner_a[i]));
    bytes.push_back(value.kind);
    word(bytes, value.child_shape_id);
    word(bytes, value.body_id);
}
}

GameCommand parse_game_command(std::span<const std::uint8_t> plaintext) {
    Reader input(plaintext);
    const auto id = input.byte();
    GameCommand command;
    switch (id) {
    case 0x22: {
        BuildShapeCommand value;
        value.inventory_slot=input.signed_word();
        value.quantity=input.word();
        value.block_dimensions=vector(input);
        value.auxiliary_position=vector(input);
        value.axis_b=vector(input);
        value.axis_a=vector(input);
        value.cell_position=vector(input);
        value.inventory_uuid=uuid(input);
        value.shape_uuid=uuid(input);
        value.kind=input.byte();
        value.child_shape_id=input.word();
        value.body_id=input.word();
        command=value;
        break;
    }
    case 0x23: command = RemoveShapeCommand{selection(input)}; break;
    case 0x27: command = RemoveJointCommand{input.word()}; break;
    case 0x24: {
        const auto color = input.word();
        command = PaintShapeCommand{selection(input), color};
        break;
    }
    case 0x25: {
        const auto color = input.word();
        command = PaintJointCommand{input.word(), color};
        break;
    }
    case 0x26: {
        const auto color = input.word();
        command = PaintCharacterCommand{input.word(), color};
        break;
    }
    default: throw std::invalid_argument("unsupported game command ID");
    }
    input.finish();
    return command;
}

std::vector<std::uint8_t> serialize_game_command(const GameCommand& command) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(38);
    std::visit([&](const auto& value) {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, RemoveShapeCommand>) {
            bytes.push_back(0x23);
            selection(bytes, value.selection);
        } else if constexpr (std::is_same_v<Type, RemoveJointCommand>) {
            bytes.push_back(0x27);
            word(bytes,value.joint_id);
        } else if constexpr (std::is_same_v<Type, PaintShapeCommand>) {
            bytes.push_back(0x24);
            word(bytes, value.color_rgba);
            selection(bytes, value.selection);
        } else if constexpr (std::is_same_v<Type, PaintJointCommand>) {
            bytes.push_back(0x25);
            word(bytes, value.color_rgba);
            word(bytes, value.joint_id);
        } else if constexpr (std::is_same_v<Type, PaintCharacterCommand>) {
            bytes.push_back(0x26);
            word(bytes, value.color_rgba);
            word(bytes, value.character_id);
        } else {
            bytes.push_back(0x22);
            word(bytes,std::bit_cast<std::uint32_t>(value.inventory_slot));
            word(bytes,value.quantity);
            vector(bytes,value.block_dimensions);
            vector(bytes,value.auxiliary_position);
            vector(bytes,value.axis_b);
            vector(bytes,value.axis_a);
            vector(bytes,value.cell_position);
            uuid(bytes,value.inventory_uuid);
            uuid(bytes,value.shape_uuid);
            bytes.push_back(value.kind);
            word(bytes,value.child_shape_id);
            word(bytes,value.body_id);
        }
    }, command);
    return bytes;
}
}
