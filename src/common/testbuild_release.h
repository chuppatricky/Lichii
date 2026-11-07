// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <optional>
#include <string_view>

#include "common/common_types.h"

/// chuppatricky-amd-testbuild releases on GitHub (chuppatricky/Lichii): tags look like
/// "chuppatricky-amd-testbuild-v14" and the build's own version like "chuppatricky-amd-testbuild v14".
namespace TestbuildRelease {

constexpr std::string_view Repository = "chuppatricky/Lichii";
constexpr std::string_view Prefix = "chuppatricky-amd-testbuild";

/// The N of "chuppatricky-amd-testbuild vN" or "chuppatricky-amd-testbuild-vN"; nothing for
/// anything else (other releases, extra text, a missing or zero number).
[[nodiscard]] inline std::optional<u32> Version(std::string_view name) {
    if (!name.starts_with(Prefix)) {
        return std::nullopt;
    }
    name.remove_prefix(Prefix.size());
    if (name.size() < 3 || (name[0] != ' ' && name[0] != '-') || name[1] != 'v') {
        return std::nullopt;
    }
    name.remove_prefix(2);
    u32 number{};
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), number);
    if (error != std::errc{} || end != name.data() + name.size() || number == 0) {
        return std::nullopt;
    }
    return number;
}

/// Whether a release asset is the download for this platform: the Windows zip or the AppImage.
[[nodiscard]] inline bool IsPlatformAsset(std::string_view asset_name, bool windows) {
    return windows ? asset_name.ends_with("-windows-x86_64.zip")
                   : asset_name.ends_with("-x86_64.AppImage");
}

} // namespace TestbuildRelease
