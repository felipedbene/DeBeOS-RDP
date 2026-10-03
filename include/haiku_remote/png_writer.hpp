#pragma once

#include "haiku_remote/surface.hpp"

#include <string>

namespace haiku_remote {

bool write_png(const Surface& surface, const std::string& path, std::string& error);

} // namespace haiku_remote
