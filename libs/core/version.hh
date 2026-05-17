#pragma once

#include <string_view>

#include <semver.hpp>

namespace mm::core {

constexpr std::string_view kVersion = "6.0.0";
static_assert(semver::valid(kVersion));

}  // namespace mm::core
