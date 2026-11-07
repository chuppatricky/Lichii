// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/testbuild_release.h"

TEST_CASE("TestbuildRelease: version from build names and tags", "[common]") {
    using TestbuildRelease::Version;
    REQUIRE(Version("chuppatricky-amd-testbuild v14") == 14u);
    REQUIRE(Version("chuppatricky-amd-testbuild-v13") == 13u);
    REQUIRE(Version("chuppatricky-amd-testbuild-v105") == 105u);
    REQUIRE_FALSE(Version("chuppatricky-amd-testbuild v14 — Ocarina of Time on AMD"));
    REQUIRE_FALSE(Version("chuppatricky-amd-testbuild-v"));
    REQUIRE_FALSE(Version("chuppatricky-amd-testbuild-v0"));
    REQUIRE_FALSE(Version("chuppatricky-amd-testbuild-14"));
    REQUIRE_FALSE(Version("oot-amd-v10"));
    REQUIRE_FALSE(Version("build-75"));
    REQUIRE_FALSE(Version(""));
}

TEST_CASE("TestbuildRelease: platform assets", "[common]") {
    using TestbuildRelease::IsPlatformAsset;
    REQUIRE(IsPlatformAsset("chuppatricky-amd-testbuild-v13-windows-x86_64.zip", true));
    REQUIRE_FALSE(IsPlatformAsset("chuppatricky-amd-testbuild-v13-x86_64.AppImage", true));
    REQUIRE(IsPlatformAsset("chuppatricky-amd-testbuild-v13-x86_64.AppImage", false));
    REQUIRE_FALSE(IsPlatformAsset("chuppatricky-amd-testbuild-v13-windows-x86_64.zip", false));
}
