#pragma once

#include <filesystem>
#include <string>

namespace scrapds {
// Opens an existing Scrap Mechanic SQLite save read-only. Samples are bounded;
// BLOB payloads are described, never interpreted as decoded game objects.
// Throws std::runtime_error on I/O, SQLite, or save-schema errors.
std::string inspect_world_json(const std::filesystem::path& path, int sample_limit = 3);
}
