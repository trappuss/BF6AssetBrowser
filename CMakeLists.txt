cmake_minimum_required(VERSION 3.21)
project(AssetBrowser VERSION 0.1.0 LANGUAGES CXX)

# The AssetBrowser family monorepo: one shared core, one plugin per game.
# Phases 1-4 build `abcore` (types, math, store, export, viewport, shell, tabs), the conformance
# tests and games/poe2 (POE2AssetBrowser on the core); phase 5 added games/fox, phase 6 games/d4.
# See docs/CORE_EXTRACTION_PLAN.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# AUTORCC is not on by default: without it a game's .qrc is accepted silently and never compiled,
# and the window icon stays the default — D4's real-data log said so ("app icon: :/app_256.png
# missing from the Qt resource", 2026-09-24). FOX's own CMakeLists carries the same scar.
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)

add_subdirectory(core)

# One place for the per-target flags all three projects copy-paste today, with the drift they have
# already accumulated (FOX adds /Z7, POE2 has no PCH and no static-Qt branch).
function(assetbrowser_configure_target tgt)
    target_link_libraries(${tgt} PRIVATE abcore)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /EHa /MP /bigobj)
        target_compile_definitions(${tgt} PRIVATE NOMINMAX)
    endif()
endfunction()

# Declared before the games so a game's own conformance tool (guarded on this option in its
# CMakeLists) builds on the first configure, not only after the cache is seeded. enable_testing()
# and the conformance subdirectory stay below, after the games.
option(ASSETBROWSER_BUILD_TESTS "Build the conformance tests" ON)

# Phase 4: the first game on the core. Skipped (with a message) when the POE2 project folder is not
# where POE2_SRC points.
# The games are each gated on BOTH their option AND the folder being present, so a checkout that
# carries only a subset (e.g. the DRAssetBrowser repo = core + games/dr) still configures and builds.
option(ASSETBROWSER_BUILD_POE2 "Build POE2AssetBrowser on the core" ON)
if(ASSETBROWSER_BUILD_POE2 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/poe2/CMakeLists.txt")
    add_subdirectory(games/poe2)
endif()

option(ASSETBROWSER_BUILD_FOX "Build FOXAssetBrowser on the core" ON)
if(ASSETBROWSER_BUILD_FOX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/fox/CMakeLists.txt")
    add_subdirectory(games/fox)
endif()
option(ASSETBROWSER_BUILD_D4 "Build D4AssetBrowser on the core" ON)
if(ASSETBROWSER_BUILD_D4 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/d4/CMakeLists.txt")
    add_subdirectory(games/d4)
endif()

# DR phase 1: Destiny Rising storage + index. Self-contained (carries its own MPK codec in
# games/dr), so it builds wherever the core does.
option(ASSETBROWSER_BUILD_DR "Build DRAssetBrowser on the core" ON)
if(ASSETBROWSER_BUILD_DR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/dr/CMakeLists.txt")
    add_subdirectory(games/dr)
endif()

# BF6 phase 1: Battlefield 6 (Frostbite) storage + index. Self-contained (carries its own TOC/cas
# reader in games/bf6), so it builds wherever the core does.
option(ASSETBROWSER_BUILD_BF6 "Build BF6AssetBrowser on the core" ON)
if(ASSETBROWSER_BUILD_BF6 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/bf6/CMakeLists.txt")
    add_subdirectory(games/bf6)
endif()

# The core conformance suite references every game's adapter, so it only builds when that folder is
# present (the full monorepo). DR's own codec conformance (dr_mpk_check) lives in games/dr's
# CMakeLists and builds from conformance/tools/dr_mpk_check.cpp without the rest of the suite.
option(ASSETBROWSER_BUILD_TESTS "Build the conformance tests" ON)
if(ASSETBROWSER_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/conformance/CMakeLists.txt")
    enable_testing()
    add_subdirectory(conformance)
endif()
