#pragma once
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace scrapds {
struct CylinderDimensions {
    double diameter{},depth{};
    std::uint8_t axis{}; // X=0, Y=1, Z=2
};
struct HullGeometry {
    std::array<double,3> dimensions{};
    std::vector<std::array<double,3>> vertices; // OBJ coordinates, in grid units.
    double margin{0.04};
    bool degenerate{}; // A rounded line/plane requires an explicit positive margin.
};
struct AssetDefinition {
    std::string uuid, name, physics_material;
    bool block{};
    std::optional<std::uint32_t> default_color_abgr;
    // Explicit box collision size in construction-grid units, if provided.
    std::optional<std::array<double,3>> box_dimensions;
    std::optional<CylinderDimensions> cylinder_dimensions;
    std::optional<HullGeometry> hull_geometry;
    // Vanilla survival suspension length in construction-grid units.
    std::optional<std::uint32_t> survival_spring_length;
    // Explicit maximum stack size from the game's shape definition.
    std::optional<std::uint32_t> stack_size;
};
class AssetDefinitions {
public:
    static AssetDefinitions load(const std::filesystem::path& installation_or_pack);
    const AssetDefinition* find(const std::string& uuid) const;
    std::size_t size() const { return entries_.size(); }
private:
    std::map<std::string, AssetDefinition> entries_;
};
}
