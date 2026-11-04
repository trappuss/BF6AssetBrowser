# BF6AssetBrowser — Battlefield 6 (Frostbite) on the core. Self-contained: it carries its own
# superbundle/cas reader (Bf6Toc), reverse-engineered from the reshax toolkit and verified against
# the retail install (docs/BF6_STORAGE_FORMAT.md). Oodle (oo2core_9_win64.dll) is loaded dynamically
# at runtime from the game folder, so there is no build-time Oodle dependency.

# zstd decodes 0x0F cas blocks (same robust find as games/dr). Required.
find_package(zstd CONFIG QUIET)
if(NOT TARGET zstd::libzstd AND NOT TARGET zstd::libzstd_shared AND NOT TARGET zstd::libzstd_static)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
        if(TARGET PkgConfig::ZSTD)
            add_library(zstd::libzstd ALIAS PkgConfig::ZSTD)
        endif()
    endif()
endif()
set(_bf6_zstd)
foreach(_t zstd::libzstd zstd::libzstd_shared zstd::libzstd_static)
    if(TARGET ${_t} AND NOT _bf6_zstd)
        set(_bf6_zstd ${_t})
    endif()
endforeach()
if(NOT _bf6_zstd)
    message(FATAL_ERROR "games/bf6: zstd not found — needed for 0x0F cas blocks")
endif()

# The Qt-free engine library: the TOC/cas reader + block decompressor, linkable by the conformance
# tool without the GUI (mirrors games/dr's dr_engine).
add_library(bf6_engine STATIC Bf6Toc.h Bf6Toc.cpp)
target_include_directories(bf6_engine PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(bf6_engine PUBLIC ${_bf6_zstd})
if(MSVC)
    target_compile_options(bf6_engine PRIVATE /EHsc /MP)
    target_compile_definitions(bf6_engine PRIVATE NOMINMAX)
endif()
message(STATUS "games/bf6: zstd via ${_bf6_zstd}; Oodle loaded at runtime from the game folder")

# bcdec (header-only BC1-7 decoder, MIT) for in-app texture previews — shared with POE2/DR. Prefer a
# vendored copy under third_party/, else POE2's sibling copy. Overridable.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/bcdec/bcdec.h")
    set(_bf6_bcdec_default "${CMAKE_SOURCE_DIR}/third_party/bcdec")
else()
    set(_bf6_bcdec_default "${CMAKE_SOURCE_DIR}/../POE2AssetBrowser/third_party/bcdec")
endif()
set(BF6_BCDEC_DIR "${_bf6_bcdec_default}" CACHE PATH "bcdec.h (header-only BC decoder)")
if(NOT EXISTS "${BF6_BCDEC_DIR}/bcdec.h")
    message(FATAL_ERROR "games/bf6: bcdec.h not found at BF6_BCDEC_DIR=${BF6_BCDEC_DIR} — "
                        "set -DBF6_BCDEC_DIR=<folder with bcdec.h>")
endif()

qt_add_executable(BF6AssetBrowser WIN32
    main.cpp
    Bf6Store.h   Bf6Store.cpp
    Bf6Plugin.h  Bf6Plugin.cpp
    Bf6Names.h   Bf6Names.cpp
    BcdecImpl.cpp
    bf6.qrc
)
assetbrowser_configure_target(BF6AssetBrowser)
target_link_libraries(BF6AssetBrowser PRIVATE bf6_engine)
target_include_directories(BF6AssetBrowser PRIVATE "${BF6_BCDEC_DIR}")

# Storage conformance (no Qt): parse a real install and decompress a sample (raw/zstd/Oodle). Guarded
# on the source being present so a BF6-only checkout still configures.
if(ASSETBROWSER_BUILD_TESTS AND EXISTS "${CMAKE_SOURCE_DIR}/conformance/tools/bf6_toc_check.cpp")
    add_executable(bf6_toc_check "${CMAKE_SOURCE_DIR}/conformance/tools/bf6_toc_check.cpp")
    target_link_libraries(bf6_toc_check PRIVATE bf6_engine)
    target_include_directories(bf6_toc_check PRIVATE "${CMAKE_SOURCE_DIR}/games/bf6")
    set_target_properties(bf6_toc_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/conformance")
endif()
