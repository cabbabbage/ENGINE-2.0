#VIBBLE 2D GAME ENGINE

cmake_minimum_required(VERSION 3.20)
project(VIBBLEEngine)

# Required interpreter for lint/check scripts.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ------------------------------------------
# Build settings focused on compile speed
# ------------------------------------------
add_compile_definitions(SDL_DISABLE_OLD_NAMES)
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
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/*.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/*.c
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/*.hpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/*.h
    ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools/*.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools/*.c
    ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools/*.hpp
    ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools/*.h
)

list(REMOVE_DUPLICATES ENGINE_SRC)
# Exclude standalone tool entry points from the main engine binary
list(REMOVE_ITEM ENGINE_SRC
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/cli/asset_tool_cli.cpp"
    "${CMAKE_SOURCE_DIR}/ENGINE/tools/cli/image_cache_generator.cpp"
)

add_executable(engine ${ENGINE_SRC})
target_sources(engine PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/tools/cli/image_cache_generator.cpp
)

add_executable(debug_flag_sync_regression_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/debug_flag_sync_regression_test.cpp
)

add_executable(room_legacy_migration_regression_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/room_legacy_migration_regression_test.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/gameplay/map_generation/room_legacy_migration.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils/utils/weighted_range.cpp
)
target_include_directories(room_legacy_migration_regression_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
)
target_link_libraries(room_legacy_migration_regression_test PRIVATE
    nlohmann_json::nlohmann_json
)

add_executable(map_template_data_cleanup_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/map_template_data_cleanup_test.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/core/manifest/map_manifest_normalizer.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/core/manifest/manifest_loader.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/gameplay/map_generation/map_layers_geometry.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils/utils/map_grid_settings.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils/utils/weighted_range.cpp
)
target_include_directories(map_template_data_cleanup_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
)
target_link_libraries(map_template_data_cleanup_test PRIVATE
    nlohmann_json::nlohmann_json
)

add_executable(spawn_trail_classification_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/spawn_trail_classification_test.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/gameplay/spawn/trail_classification.cpp
)
target_include_directories(spawn_trail_classification_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
)

add_executable(sprite_floor_clip_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/sprite_floor_clip_test.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/rendering/render/sprite_floor_clip.cpp
)
target_include_directories(sprite_floor_clip_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
)

add_executable(dof_blur_strength_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/dof_blur_strength_test.cpp
)
target_include_directories(dof_blur_strength_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
)

add_executable(movement_paths_frame_edit_state_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/movement_paths_frame_edit_state_test.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools/frame_editors/shared/FrameEditState.cpp
)
target_include_directories(movement_paths_frame_edit_state_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/editor
    ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools
)
target_link_libraries(movement_paths_frame_edit_state_test PRIVATE
    nlohmann_json::nlohmann_json
)

add_executable(enemy_behavior_phase_regression_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/enemy_behavior_phase_regression_test.cpp
)
target_include_directories(enemy_behavior_phase_regression_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
)

add_executable(enemy_combat_cooldown_regression_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/enemy_combat_cooldown_regression_test.cpp
)
target_include_directories(enemy_combat_cooldown_regression_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
)

add_executable(enemy_attack_cycle_regression_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/enemy_attack_cycle_regression_test.cpp
)
target_include_directories(enemy_attack_cycle_regression_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
)

add_executable(enemy_ai_layers_regression_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/enemy_ai_layers_regression_test.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/animation/controllers/shared/internal/enemy_movement_goal.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/animation/controllers/shared/internal/enemy_positioning_system.cpp
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/animation/navigation/enemy_navigation_system.cpp
)
target_include_directories(enemy_ai_layers_regression_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
)


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

# Prefer the cached vcpkg_installed folder so find_package can locate SDL3, even
# if the toolchain integration has not injected the prefix path yet.
set(_VCPKG_CUSTOM_PREFIX_DIRS
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/share"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/share"
    "${CMAKE_SOURCE_DIR}/vcpkg/installed/x64-windows/share"
)
foreach(_prefix IN LISTS _VCPKG_CUSTOM_PREFIX_DIRS)
    if(IS_DIRECTORY "${_prefix}" AND NOT "${_prefix}" STREQUAL "")
        list(APPEND CMAKE_PREFIX_PATH "${_prefix}")
    endif()
endforeach()
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
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(Clipper2 CONFIG QUIET)
target_link_libraries(enemy_behavior_phase_regression_test PRIVATE
    SDL3::SDL3
)
target_link_libraries(enemy_combat_cooldown_regression_test PRIVATE
    SDL3::SDL3
)
target_link_libraries(enemy_attack_cycle_regression_test PRIVATE
    SDL3::SDL3
)

add_executable(throw_physics_runtime_test
    ${CMAKE_SOURCE_DIR}/ENGINE/tests/throw_physics_runtime_test.cpp
)
target_include_directories(throw_physics_runtime_test PRIVATE
    ${CMAKE_SOURCE_DIR}/ENGINE/runtime
)
target_link_libraries(sprite_floor_clip_test PRIVATE
    SDL3::SDL3
)

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
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/app
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/assets
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/ui
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/core
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/audio
          ${CMAKE_SOURCE_DIR}/ENGINE/runtime/gameplay/spawn
          ${CMAKE_SOURCE_DIR}/ENGINE/editor
          ${CMAKE_SOURCE_DIR}/ENGINE/editor/devtools
          ${CMAKE_SOURCE_DIR}/ENGINE/tools/cli
  )

target_compile_definitions(engine PRIVATE PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

# ------------------------------------------
# Link libraries
# ------------------------------------------
target_link_libraries(engine
    PRIVATE
        nlohmann_json::nlohmann_json
        SDL3::SDL3
        SDL3_image::SDL3_image
        SDL3_ttf::SDL3_ttf
        ${GLAD_TARGET}
        OpenGL::GL
)
if (Clipper2_FOUND)
    target_link_libraries(engine PRIVATE Clipper2::Clipper2)
    target_compile_definitions(engine PRIVATE VIBBLE_HAS_CLIPPER2=1)
endif()
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
    target_link_libraries(engine PRIVATE d3d11 d3dcompiler)
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

# ------------------------------------------
set(RUNTIME_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/ENGINE/runtime/rendering/shaders")
set(RUNTIME_SHADER_PACKAGE_DIR "${RUNTIME_SHADER_SOURCE_DIR}")
add_custom_target(opengl_runtime_preflight
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/ENGINE/tools/rendering/opengl_runtime_preflight.py"
            --workspace "${CMAKE_SOURCE_DIR}"
    COMMENT "Running OpenGL runtime preflight checks"
    VERBATIM)
add_custom_target(opengl_runtime_preflight_strict
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/ENGINE/tools/rendering/opengl_runtime_preflight.py"
            --workspace "${CMAKE_SOURCE_DIR}"
            --strict-tools
    COMMENT "Running strict OpenGL runtime preflight checks (requires shader compilers)"
    VERBATIM)
add_custom_target(runtime_shader_packages ALL
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/ENGINE/tools/shaders/build_runtime_shader_packages.py"
            --input "${RUNTIME_SHADER_SOURCE_DIR}"
            --output "${RUNTIME_SHADER_PACKAGE_DIR}"
    BYPRODUCTS "${RUNTIME_SHADER_PACKAGE_DIR}/runtime_shaders.json"
    COMMENT "Packaging deterministic runtime shader bundles (SPIR-V)"
    VERBATIM)
add_dependencies(opengl_runtime_preflight runtime_shader_packages)
add_dependencies(opengl_runtime_preflight_strict runtime_shader_packages)
add_dependencies(engine opengl_runtime_preflight)

# ------------------------------------------
# C++ Cache Tools (ENGINE/tools)
# ------------------------------------------

# Helper for CLI tools that share includes, definitions, and libs.
function(add_cli_tool target_name entry_point)
    add_executable(${target_name}
        ENGINE/tools/cli/${entry_point}
        ENGINE/tools/cli/image_cache_generator.cpp
        ENGINE/runtime/utils/utils/stb_image_impl.cpp
    )
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/ENGINE/tools/cli
        ${CMAKE_SOURCE_DIR}/ENGINE/runtime/utils
        ${CMAKE_SOURCE_DIR}/external
    )
    target_link_libraries(${target_name} PRIVATE
        nlohmann_json::nlohmann_json
    )
    target_compile_definitions(${target_name} PRIVATE
        PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )
endfunction()

# Asset Tool CLI - main cache generator
add_cli_tool(asset_tool_cli asset_tool_cli.cpp)

# Output all tools to ENGINE/tools directory
foreach(tool_target asset_tool_cli)
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
