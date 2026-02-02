#VIBBLE 2D GAME ENGINE

cmake_minimum_required(VERSION 3.20)
project(VIBBLEEngine)

# ------------------------------------------
# Build settings focused on compile speed
# ------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# Default to Release if not set (for single-config generators)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compiler cache (ccache/sccache) if present
find_program(CCACHE_PROGRAM ccache)
find_program(SCCACHE_PROGRAM sccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
elseif(SCCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
endif()

# Speed-oriented flags
if(MSVC)
    add_compile_options(/MP /Zc:inline /Zc:__cplusplus /permissive /FS /bigobj)
else()
    add_compile_options(-pipe)
endif()

# Output to ENGINE/ for all configs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/ENGINE)
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UP)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG_UP} ${CMAKE_SOURCE_DIR}/ENGINE)
endforeach()

# ------------------------------------------
# Sources
# ------------------------------------------
file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/ENGINE/*.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/*.c
    ${CMAKE_SOURCE_DIR}/ENGINE/*.hpp
    ${CMAKE_SOURCE_DIR}/ENGINE/*.h
)

list(REMOVE_DUPLICATES ENGINE_SRC)
list(REMOVE_ITEM ENGINE_SRC "${CMAKE_SOURCE_DIR}/ENGINE/dev_mode/frame_editor_session_child_timelines.cpp")
# Exclude standalone tool entry points from the main engine binary
list(REMOVE_ITEM ENGINE_SRC
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/asset_tool_cli.cpp"
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/set_rebuild_cli.cpp"
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/cache_validator_cli.cpp"
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/apply_effects_cli.cpp"
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/image_cache_generator.cpp"
)

add_executable(engine ${ENGINE_SRC})

# Unity build (jumbo) for faster compiles
option(ENABLE_UNITY_BUILD "Enable unity builds for faster compilation" OFF)
if(ENABLE_UNITY_BUILD)
    set_target_properties(engine PROPERTIES UNITY_BUILD ON)
    set_target_properties(engine PROPERTIES UNITY_BUILD_BATCH_SIZE 16)
endif()

# Precompiled header
option(ENABLE_PCH "Enable precompiled headers" ON)
if(ENABLE_PCH AND EXISTS "${CMAKE_SOURCE_DIR}/ENGINE/pch.hpp")
    target_precompile_headers(engine PRIVATE ${CMAKE_SOURCE_DIR}/ENGINE/pch.hpp)
endif()

# ------------------------------------------
# Dependencies (prefer vcpkg; with glad fallback)
# ------------------------------------------
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    set(_NLOHMANN_JSON_DIR "${CMAKE_SOURCE_DIR}/external/nlohmann_json")
    if(EXISTS "${_NLOHMANN_JSON_DIR}/include/nlohmann/json.hpp")
        add_library(nlohmann_json INTERFACE)
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
        target_include_directories(nlohmann_json INTERFACE "${_NLOHMANN_JSON_DIR}/include")
        message(STATUS "Using bundled nlohmann_json from ${_NLOHMANN_JSON_DIR}")
    else()
        message(FATAL_ERROR "nlohmann_json not found. Install the package or add it to ${_NLOHMANN_JSON_DIR}")
    endif()
endif()
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
find_package(SDL2_mixer CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

# Try glad via package config first (vcpkg provides this)
find_package(glad CONFIG QUIET)
if(glad_FOUND)
    set(GLAD_TARGET glad::glad)
else()
    message(WARNING "glad CONFIG package not found. Trying local source fallback at third_party/glad ...")
    set(GLAD_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/glad")
    if(EXISTS "${GLAD_SRC_DIR}/src/glad.c")
        add_library(glad STATIC "${GLAD_SRC_DIR}/src/glad.c")
        target_include_directories(glad PUBLIC "${GLAD_SRC_DIR}/include")
        set(GLAD_TARGET glad)
        message(STATUS "Using local glad from ${GLAD_SRC_DIR}")
    elseif(EXISTS "${GLAD_SRC_DIR}/CMakeLists.txt")
        add_subdirectory("${GLAD_SRC_DIR}" EXCLUDE_FROM_ALL)
        if(TARGET glad::glad)
            set(GLAD_TARGET glad::glad)
        elseif(TARGET glad)
            set(GLAD_TARGET glad)
        else()
            message(FATAL_ERROR "Unable to determine glad target from third_party/glad")
        endif()
        message(STATUS "Using third_party glad subdirectory")
    else()
        message(WARNING
            "glad not found; continuing without glad.\n"
            "Fix options (optional):\n"
            "  1) Install via vcpkg and pass the vcpkg toolchain to CMake (recommended):\n"
            "       vcpkg install glad:x64-windows\n"
            "       -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake\n"
            "  2) Add glad sources to third_party/glad (with include/ and src/glad.c).")
        set(GLAD_TARGET "")
    endif()
endif()

# OpenGL (needed by glad-backed GL apps)
find_package(OpenGL REQUIRED)
if (WIN32)
    # Ensure opengl32.lib is found even when Windows SDK lib paths are not on the default LIB search list.
    set(_WINSDK_KITS_ROOT "C:/Program Files (x86)/Windows Kits/10/Lib")
    if (EXISTS "${_WINSDK_KITS_ROOT}")
        file(GLOB _winsdk_versions LIST_DIRECTORIES true "${_WINSDK_KITS_ROOT}/*")
        list(SORT _winsdk_versions COMPARE NATURAL ORDER DESCENDING)
        list(GET _winsdk_versions 0 _winsdk_latest)
        set(_winsdk_um_x64 "${_winsdk_latest}/um/x64")
        set(_winsdk_ucrt_x64 "${_winsdk_latest}/ucrt/x64")
    endif()
    find_library(OPENGL32_ABSOLUTE
        NAMES opengl32 OpenGL32
        PATHS "${_winsdk_um_x64}" "${_WINSDK_KITS_ROOT}"
        PATH_SUFFIXES "um/x64" "x64"
    )
    if (OPENGL32_ABSOLUTE AND TARGET OpenGL::GL)
        set_property(TARGET OpenGL::GL PROPERTY IMPORTED_IMPLIB "${OPENGL32_ABSOLUTE}")
        set_property(TARGET OpenGL::GL PROPERTY IMPORTED_LOCATION "${OPENGL32_ABSOLUTE}")
    endif()
    if (EXISTS "${_winsdk_um_x64}")
        set(WINSDK_UM_X64_LIBDIR "${_winsdk_um_x64}")
    endif()
    if (EXISTS "${_winsdk_ucrt_x64}")
        set(WINSDK_UCRT_X64_LIBDIR "${_winsdk_ucrt_x64}")
    endif()
endif()
if (MSVC)
    set(_VCTOOLS_ROOT "C:/VS2022/BuildTools/VC/Tools/MSVC")
    if (EXISTS "${_VCTOOLS_ROOT}")
        file(GLOB _vctools_versions LIST_DIRECTORIES true "${_VCTOOLS_ROOT}/*")
        list(SORT _vctools_versions COMPARE NATURAL ORDER DESCENDING)
        list(GET _vctools_versions 0 _vctools_latest)
        set(MSVC_LIB_DIR "${_vctools_latest}/lib/x64")
    endif()
endif()

# ------------------------------------------
# Include dirs
# ------------------------------------------
target_include_directories(engine
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ENGINE
        ${CMAKE_SOURCE_DIR}/ENGINE/asset
        ${CMAKE_SOURCE_DIR}/ENGINE/core
        ${CMAKE_SOURCE_DIR}/ENGINE/dev_mode
        ${CMAKE_SOURCE_DIR}/ENGINE/render
        ${CMAKE_SOURCE_DIR}/ENGINE/render_pipeline
        ${CMAKE_SOURCE_DIR}/ENGINE/map_generation
        ${CMAKE_SOURCE_DIR}/ENGINE/spawn
        ${CMAKE_SOURCE_DIR}/ENGINE/ui
        ${CMAKE_SOURCE_DIR}/ENGINE/utils
)

target_compile_definitions(engine PRIVATE PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

# ------------------------------------------
# Link libraries
# ------------------------------------------
# SDL2main is not present on all platforms; link it only if the target exists.
set(_SDL2MAIN_TARGET "")
if(TARGET SDL2::SDL2main)
    set(_SDL2MAIN_TARGET SDL2::SDL2main)
endif()

target_link_libraries(engine
    PRIVATE
        nlohmann_json::nlohmann_json
        SDL2::SDL2 ${_SDL2MAIN_TARGET}
        SDL2_image::SDL2_image
        SDL2_mixer::SDL2_mixer
        SDL2_ttf::SDL2_ttf
        ${GLAD_TARGET}
        OpenGL::GL
)
if (WINSDK_UM_X64_LIBDIR)
    target_link_directories(engine PRIVATE "${WINSDK_UM_X64_LIBDIR}")
endif()
if (WINSDK_UCRT_X64_LIBDIR)
    target_link_directories(engine PRIVATE "${WINSDK_UCRT_X64_LIBDIR}")
endif()
if (MSVC_LIB_DIR)
    target_link_directories(engine PRIVATE "${MSVC_LIB_DIR}")
endif()
if (OPENGL32_ABSOLUTE)
    target_link_libraries(engine PRIVATE ${OPENGL32_ABSOLUTE})
endif()

# Link Windows COM/Shell libs for modern file dialogs
if (WIN32)
    target_link_libraries(engine PRIVATE Ole32 Shell32)
endif()

# Faster relinks on MSVC Debug
if(MSVC)
    target_link_options(engine
        PRIVATE
            "$<$<CONFIG:Debug>:/INCREMENTAL>"
            "$<$<CONFIG:RelWithDebInfo>:/DEBUG:FASTLINK>"
            "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>"
    )
endif()

# ------------------------------------------
# Unit tests (doctest-based)
# ------------------------------------------
set(TEST_ROOT "${CMAKE_SOURCE_DIR}/TEST_TMP")
add_executable(engine_tests
    TESTS/test_main.cpp
    TESTS/test_dev_json_store.cpp
    TESTS/test_manifest_loader.cpp
    TESTS/test_scene_renderer_prereqs.cpp
    TESTS/test_animation_child_visibility.cpp
    TESTS/test_animation_child_runtime.cpp
    TESTS/test_animation_child_clone.cpp
    TESTS/test_frame_editor_async_payload.cpp
    TESTS/test_attack_validation.cpp
    TESTS/frame_editor_session_test_helpers.cpp
    ENGINE/asset/animation.cpp
    ENGINE/asset/surface_utils.cpp
    ENGINE/asset/animation_cloner.cpp
    ENGINE/animation_update/child_attachment_controller.cpp
    ENGINE/animation_update/attack_validation.cpp
    ENGINE/dev_mode/asset_sections/animation_editor_window/AnimationDocument.cpp
    ENGINE/dev_mode/dm_styles.cpp
    ENGINE/dev_mode/core/dev_json_store.cpp
    ENGINE/dev_mode/core/manifest_store.cpp
    ENGINE/dev_mode/manifest_asset_utils.cpp
    ENGINE/dev_mode/tag_utils.cpp
    ENGINE/dev_mode/draw_utils.cpp
    ENGINE/dev_mode/font_cache.cpp
    ENGINE/dev_mode/widgets.cpp
    ENGINE/dev_mode/frame_editors/shared/Point3DEditor.cpp
    ENGINE/core/manifest/manifest_loader.cpp
    ENGINE/utils/log.cpp
    ENGINE/utils/cache_manager.cpp
    ENGINE/utils/loading_status_notifier.cpp
  )

target_include_directories(engine_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ENGINE
        ${CMAKE_SOURCE_DIR}/ENGINE/asset
        ${CMAKE_SOURCE_DIR}/ENGINE/core
        ${CMAKE_SOURCE_DIR}/ENGINE/dev_mode
        ${CMAKE_SOURCE_DIR}/ENGINE/render
        ${CMAKE_SOURCE_DIR}/ENGINE/render_pipeline
        ${CMAKE_SOURCE_DIR}/ENGINE/map_generation
        ${CMAKE_SOURCE_DIR}/ENGINE/spawn
        ${CMAKE_SOURCE_DIR}/ENGINE/ui
        ${CMAKE_SOURCE_DIR}/ENGINE/utils
        ${CMAKE_SOURCE_DIR}/external
)

target_compile_definitions(engine_tests PRIVATE
    DEV_MODE_DISABLE_JSON_DEBOUNCE=1
    PROJECT_ROOT="${TEST_ROOT}"
    FRAME_EDITOR_TEST_PUBLIC_ACCESS=1
    FRAME_EDITOR_ACCESS=public
)

target_link_libraries(engine_tests
    PRIVATE
        nlohmann_json::nlohmann_json
        SDL2::SDL2 ${_SDL2MAIN_TARGET}
        SDL2_image::SDL2_image
        SDL2_mixer::SDL2_mixer
        SDL2_ttf::SDL2_ttf
        ${GLAD_TARGET}
        OpenGL::GL
)
if (WINSDK_UM_X64_LIBDIR)
    target_link_directories(engine_tests PRIVATE "${WINSDK_UM_X64_LIBDIR}")
endif()
if (WINSDK_UCRT_X64_LIBDIR)
    target_link_directories(engine_tests PRIVATE "${WINSDK_UCRT_X64_LIBDIR}")
endif()
if (MSVC_LIB_DIR)
    target_link_directories(engine_tests PRIVATE "${MSVC_LIB_DIR}")
endif()
if (OPENGL32_ABSOLUTE)
    target_link_libraries(engine_tests PRIVATE ${OPENGL32_ABSOLUTE})
endif()

if (WIN32)
    target_link_libraries(engine_tests PRIVATE Ole32 Shell32)
endif()

# ------------------------------------------
# Isolated WorldGrid tests (lightweight target)
# ------------------------------------------
add_executable(engine_world_tests
    TESTS/test_main.cpp
    TESTS/test_world_grid_same_chunk_move.cpp
    ENGINE/world/world_grid.cpp
    ENGINE/world/grid_point.cpp
    ENGINE/world/chunk_manager.cpp
    ENGINE/utils/grid_math.cpp
    ENGINE/utils/log.cpp
    TESTS/stubs/chunk_stubs.cpp
)

target_include_directories(engine_world_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/TESTS/stubs
        ${CMAKE_SOURCE_DIR}/ENGINE
    ${CMAKE_SOURCE_DIR}/external
)

target_compile_definitions(engine_world_tests PRIVATE
    PROJECT_ROOT="${TEST_ROOT}"
)

# No heavy libs needed; these sources don't call into SDL functions
# or OpenGL, they only use SDL types.
target_link_libraries(engine_world_tests
    PRIVATE
        SDL2::SDL2 ${_SDL2MAIN_TARGET}
)
if (WINSDK_UM_X64_LIBDIR)
    target_link_directories(engine_world_tests PRIVATE "${WINSDK_UM_X64_LIBDIR}")
endif()
if (WINSDK_UCRT_X64_LIBDIR)
    target_link_directories(engine_world_tests PRIVATE "${WINSDK_UCRT_X64_LIBDIR}")
endif()
if (MSVC_LIB_DIR)
    target_link_directories(engine_world_tests PRIVATE "${MSVC_LIB_DIR}")
endif()

# ------------------------------------------
# C++ Cache Tools (ENGINE/tools)
# ------------------------------------------

# Asset Tool CLI - main cache generator
add_executable(asset_tool_cli
    ENGINE/tools/asset_tool_cli.cpp
    ENGINE/tools/image_cache_generator.cpp
)
target_include_directories(asset_tool_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/tools
    ${CMAKE_SOURCE_DIR}/external
)
target_compile_definitions(asset_tool_cli PRIVATE
    IMGCACHE_STB_IMAGE_IMPL=1
    IMGCACHE_STB_IMAGE_WRITE_IMPL=1
)
target_link_libraries(asset_tool_cli PRIVATE
    nlohmann_json::nlohmann_json
)

# Set Rebuild Values CLI - manifest manipulation
add_executable(set_rebuild_cli
    ENGINE/tools/set_rebuild_cli.cpp
)
target_include_directories(set_rebuild_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/tools
)
target_link_libraries(set_rebuild_cli PRIVATE
    nlohmann_json::nlohmann_json
)

# Cache Validator CLI - cache integrity validation
add_executable(cache_validator_cli
    ENGINE/tools/cache_validator_cli.cpp
    ENGINE/tools/image_cache_generator.cpp
)
target_include_directories(cache_validator_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/tools
    ${CMAKE_SOURCE_DIR}/external
)
target_compile_definitions(cache_validator_cli PRIVATE
    IMGCACHE_STB_IMAGE_IMPL=1
    IMGCACHE_STB_IMAGE_WRITE_IMPL=1
)
target_link_libraries(cache_validator_cli PRIVATE
    nlohmann_json::nlohmann_json
)

# Apply Effects CLI - effect preview generation
add_executable(apply_effects_cli
    ENGINE/tools/apply_effects_cli.cpp
    ENGINE/tools/image_cache_generator.cpp
)
target_include_directories(apply_effects_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/tools
    ${CMAKE_SOURCE_DIR}/external
)
target_compile_definitions(apply_effects_cli PRIVATE
    IMGCACHE_STB_IMAGE_IMPL=1
    IMGCACHE_STB_IMAGE_WRITE_IMPL=1
)
target_link_libraries(apply_effects_cli PRIVATE
    nlohmann_json::nlohmann_json
)

# Output all tools to ENGINE/tools directory
foreach(tool_target asset_tool_cli set_rebuild_cli cache_validator_cli apply_effects_cli)
    set_target_properties(${tool_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/ENGINE/tools"
    )
    # Set output directory for all configurations
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${cfg}" CFG_UP)
        set_target_properties(${tool_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${CFG_UP} "${CMAKE_SOURCE_DIR}/ENGINE/tools"
        )
    endforeach()
endforeach()
