#include "shape_geometry.hpp"
#include "blob.hpp"
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace scrapds {
namespace {
std::int32_t signed_be16(std::span<const std::uint8_t> b, std::size_t offset) {
    const std::uint32_t raw = (std::uint32_t(b[offset]) << 8) | b[offset + 1];
    return raw < 0x8000u ? std::int32_t(raw) : std::int32_t(raw) - 0x10000;
}
std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at+1]) << 16) |
           (std::uint32_t(b[at+2]) << 8) | b[at+3];
}
float befloat(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const auto value = std::bit_cast<float>(be32(bytes, offset));
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite saved body field");
    return value;
}
std::array<std::int32_t, 3> axis(int code) {
    std::array<std::int32_t, 3> value{};
    if (code == 0 || code < -3 || code > 3) throw std::runtime_error("invalid saved shape axis code");
    value[std::size_t(code > 0 ? code - 1 : -code - 1)] = code > 0 ? 1 : -1;
    return value;
}
}

std::optional<RigidBodyPose> decode_rigid_body_pose(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 3) throw std::runtime_error("truncated saved body header");
    if (bytes[0] != 0 || bytes[2] != 1 || (bytes[1] != 1 && bytes[1] != 2)) return std::nullopt;
    const auto prefix = decode_rigid_body_prefix(bytes, bytes.size());
    RigidBodyPose value;
    value.id = prefix.id;
    value.world_id = prefix.world_id;
    value.is_static = bytes[1] == 1;
    value.repeated_world_id = std::uint16_t((std::uint16_t(bytes[25]) << 8) | bytes[26]);
    if (value.repeated_world_id != value.world_id) throw std::runtime_error("saved body world IDs disagree");
    value.position = prefix.candidate_position;
    // StaticBody writes one 128-bit quaternion; BitStream reverses the whole
    // scalar into big endian. DynamicBody writes four separate float32 values.
    // Both matrix->quaternion helpers produce XYZW before serialization.
    for (std::size_t i=0; i<4; ++i) value.rotation_xyzw[i] = prefix.candidate_quaternion[value.is_static ? 3-i : i];
    double norm = 0;
    for (const auto q : value.rotation_xyzw) norm += double(q) * q;
    if (std::abs(norm - 1.0) > 0.001) throw std::runtime_error("saved body quaternion is not normalized");
    if (!value.is_static) {
        std::array<float, 3> a{}, b{};
        for (std::size_t i=0; i<3; ++i) {
            a[i] = befloat(bytes, 55+4*i);
            b[i] = befloat(bytes, 67+4*i);
        }
        value.dynamic_vector0 = a;
        value.dynamic_vector1 = b;
    }
    return value;
}

std::optional<ShapeGeometry> decode_child_geometry(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 3) throw std::runtime_error("truncated saved shape header");
    if (bytes[0] != 1 || bytes[2] != 1 || (bytes[1] != 31 && bytes[1] != 32 && bytes[1] != 40)) return std::nullopt;
    const auto prefix = decode_child_shape_prefix(bytes, bytes.size());
    ShapeGeometry value;
    value.id = prefix.id;
    value.body_id = prefix.body_id;
    value.uuid = prefix.candidate_uuid;
    value.kind = SavedShapeKind(prefix.variant);
    for (std::size_t i=0; i<3; ++i) value.grid_position[i] = signed_be16(bytes, 31+2*i);
    value.color_abgr = be32(bytes, 37);
    if (value.kind == SavedShapeKind::block || value.kind == SavedShapeKind::scaled_part) {
        std::array<std::int32_t, 3> dimensions{};
        for (std::size_t i=0; i<3; ++i) {
            dimensions[i] = signed_be16(bytes, 41+2*i);
            if (dimensions[i] < 0) throw std::runtime_error("saved shape dimensions must be nonnegative");
        }
        value.grid_dimensions = dimensions;
    }
    if (value.kind != SavedShapeKind::block) {
        const auto packed = bytes[value.kind == SavedShapeKind::part ? 41 : 47];
        // ChildShape+60 serializes in the high nibble (z axis), +72 in low
        // nibble (x axis), with signed-axis codes biased by four.
        value.z_axis = axis(int(packed >> 4) - 4);
        value.x_axis = axis(int(packed & 15) - 4);
        int dot = 0;
        for (std::size_t i=0; i<3; ++i) dot += value.x_axis[i] * value.z_axis[i];
        if (dot != 0) throw std::runtime_error("saved shape axes must be perpendicular");
    }
    if (value.kind == SavedShapeKind::block) value.state_flags = bytes[47];
    else if (value.kind == SavedShapeKind::scaled_part) value.state_flags = bytes[48];
    else if (bytes.size() == 43) value.state_flags = bytes[42];
    return value;
}
std::optional<JointGeometry> decode_joint_geometry(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<3)throw std::runtime_error("truncated saved joint header");
    if(bytes[0]!=2||bytes[2]!=1||(bytes[1]!=3&&bytes[1]!=4&&bytes[1]!=28&&bytes[1]!=41))return std::nullopt;
    if(bytes.size()!=69)throw std::runtime_error("invalid saved joint length");
    JointGeometry j;j.kind=bytes[1];j.id=be32(bytes,3);j.shape_a=be32(bytes,7);j.shape_b=be32(bytes,11);
    if(j.shape_a!=be32(bytes,31)||j.shape_b!=be32(bytes,35))throw std::runtime_error("saved joint shape IDs disagree");
    std::ostringstream uuid;uuid<<std::hex<<std::setfill('0');for(int i=0;i<16;++i){if(i==4||i==6||i==8||i==10)uuid<<'-';uuid<<std::setw(2)<<unsigned(bytes[30-i]);}j.uuid=uuid.str();
    for(std::size_t i=0;i<3;++i){j.grid_a[i]=std::bit_cast<std::int32_t>(be32(bytes,47-4*i));j.grid_b[i]=std::bit_cast<std::int32_t>(be32(bytes,59-4*i));}
    j.axis_a=axis(int(bytes[63]&15)-4);j.axis_b=axis(int(bytes[63]>>4)-4);
    j.reference_a=axis(int(bytes[64]&15)-4);j.reference_b=axis(int(bytes[64]>>4)-4);
    int dot_a=0,dot_b=0;for(int i=0;i<3;++i){dot_a+=j.axis_a[i]*j.reference_a[i];dot_b+=j.axis_b[i]*j.reference_b[i];}
    if(dot_a||dot_b)throw std::runtime_error("saved joint axes must be perpendicular");return j;
}
std::optional<std::uint8_t> decode_survival_spring_setting(std::span<const std::uint8_t> bytes,std::uint32_t controller_id,std::uint32_t joint_id) {
    if(bytes.size()<3)throw std::runtime_error("truncated spring controller header");
    if(bytes[0]!=3||bytes[1]!=36||bytes[2]!=1)return std::nullopt;
    // ControllerBase list lengths are zero in this compact unlinked form.
    if(bytes.size()!=20||be32(bytes,3)!=controller_id||be32(bytes,7)!=joint_id||bytes[11]!=2||be32(bytes,12)!=joint_id||bytes[16]!=2||bytes[17]!=0||bytes[18]!=0||bytes[19]>18)throw std::runtime_error("unsupported spring controller state");
    return bytes[19];
}
std::optional<PistonSettings> decode_survival_piston_settings(std::span<const std::uint8_t> bytes,std::uint32_t controller_id,std::uint32_t joint_id) {
    if(bytes.size()<3)throw std::runtime_error("truncated piston controller header");
    if(bytes[0]!=3||bytes[1]!=35||bytes[2]!=1)return std::nullopt;
    // PistonController::write saves two bytes after ControllerBase. 0xff is
    // a special unchanged value in the game reader, not a usable setting.
    if(bytes.size()!=21||be32(bytes,3)!=controller_id||be32(bytes,7)!=joint_id||bytes[11]!=2||be32(bytes,12)!=joint_id||bytes[16]!=2||bytes[17]!=0||bytes[18]!=0||bytes[19]==255||bytes[20]==255)throw std::runtime_error("unsupported piston controller state");
    return PistonSettings{bytes[19],bytes[20]};
}
}
