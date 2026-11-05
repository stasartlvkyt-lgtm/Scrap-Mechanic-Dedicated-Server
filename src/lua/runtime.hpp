#pragma once
#include "physics/simulation.hpp"
#include <filesystem>
namespace scrapds { Scenario load_scenario(const std::filesystem::path& path); }
