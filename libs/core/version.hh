#pragma once

#include <semver.hpp>
#include <string_view>

namespace mm::core {

constexpr std::string_view kVersion = "6.0.0";
static_assert(semver::valid(kVersion));

}  // namespace mm::core
