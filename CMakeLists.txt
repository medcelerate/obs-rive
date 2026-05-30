# obs-rive: an OBS Studio plugin that renders Rive (.riv) files as a video
# source. Mirrors the structure of TDRive - shared shell + per-platform Rive
# backends (Metal / D3D11 / GL).
#
# Build flow:
#   1. ./scripts/build_rive.sh release        (macOS / Linux)
#      scripts\build_rive.bat                 (Windows)
#   2. cmake -B build -DCMAKE_BUILD_TYPE=Release \
#        -DLIBOBS_PATH=<obs install or build dir>
#      cmake --build build --config Release
#
# Outputs:
#   macOS:   build/obs-rive.plugin (CFBundle)
#   Windows: build/Release/obs-rive.dll
#   Linux:   build/obs-rive.so

cmake_minimum_required(VERSION 3.20)
project(obs-rive LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

# -----------------------------------------------------------------------------
# Find libobs
# -----------------------------------------------------------------------------
# Three discovery paths in order of preference:
#   1. CONFIG mode via OBS's own LibObsConfig.cmake (if installed system-wide
#      or via vcpkg / Homebrew).
#   2. LIBOBS_PATH cmake var pointing at an obs-studio build dir (the path
#      that contains libobs/libobs.dylib / .lib / .so).
#   3. pkg-config (typical on Linux distros).

set(LIBOBS_PATH "" CACHE PATH "Path to an OBS Studio install or build tree")
# Backslash-to-forward-slash conversion: -DLIBOBS_PATH="D:\..." on Windows
# arrives with backslashes that confuse find_path on some CMake versions.
if(LIBOBS_PATH)
    file(TO_CMAKE_PATH "${LIBOBS_PATH}" LIBOBS_PATH)
endif()
message(STATUS "LIBOBS_PATH = ${LIBOBS_PATH}")

find_package(libobs CONFIG QUIET)
if(NOT libobs_FOUND)
    set(LIBOBS_INCLUDE_DIR "")
    set(LIBOBS_LIBRARY     "")

    if(APPLE AND LIBOBS_PATH)
        # On macOS libobs ships as a .framework inside OBS.app. Accept either
        # LIBOBS_PATH=<dir containing libobs.framework> or
        # LIBOBS_PATH=<.../libobs.framework> directly.
        if(IS_DIRECTORY "${LIBOBS_PATH}/libobs.framework")
            set(_OBS_FRAMEWORK "${LIBOBS_PATH}/libobs.framework")
        elseif(EXISTS "${LIBOBS_PATH}/Headers/obs-module.h")
            set(_OBS_FRAMEWORK "${LIBOBS_PATH}")
        endif()
        if(_OBS_FRAMEWORK)
            set(LIBOBS_INCLUDE_DIR "${_OBS_FRAMEWORK}/Headers")
            set(LIBOBS_LIBRARY     "${_OBS_FRAMEWORK}")
        endif()
    endif()

    if(NOT LIBOBS_INCLUDE_DIR AND LIBOBS_PATH)
        # Explicit checks instead of find_path: find_path with
        # PATH_SUFFIXES has been seen to skip the bare directory on
        # Windows. We know the install layouts our CI produces.
        foreach(_sub "include" "include/obs" "libobs" "libobs/include")
            set(_candidate "${LIBOBS_PATH}/${_sub}/obs-module.h")
            message(STATUS "libobs probe: ${_candidate}")
            if(EXISTS "${_candidate}")
                message(STATUS "  -> found, LIBOBS_INCLUDE_DIR set")
                set(LIBOBS_INCLUDE_DIR "${LIBOBS_PATH}/${_sub}")
                break()
            endif()
        endforeach()

        foreach(_libdir "bin/64bit" "lib" "lib64" "libobs")
            foreach(_libname "obs.lib" "libobs.lib"
                              "libobs.so" "libobs.so.0"
                              "libobs.dylib")
                set(_candidate "${LIBOBS_PATH}/${_libdir}/${_libname}")
                message(STATUS "libobs probe: ${_candidate}")
                if(EXISTS "${_candidate}")
                    message(STATUS "  -> found, LIBOBS_LIBRARY set")
                    set(LIBOBS_LIBRARY "${_candidate}")
                    break()
                endif()
            endforeach()
            if(LIBOBS_LIBRARY)
                break()
            endif()
        endforeach()
    endif()

    if((NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIBRARY) AND UNIX AND NOT APPLE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(LIBOBS_PKG libobs)
            if(LIBOBS_PKG_FOUND)
                set(LIBOBS_INCLUDE_DIR ${LIBOBS_PKG_INCLUDE_DIRS})
                set(LIBOBS_LIBRARY    ${LIBOBS_PKG_LIBRARIES})
            endif()
        endif()
    endif()

    if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIBRARY)
        message(STATUS "Diagnostic: LIBOBS_INCLUDE_DIR = ${LIBOBS_INCLUDE_DIR}")
        message(STATUS "Diagnostic: LIBOBS_LIBRARY     = ${LIBOBS_LIBRARY}")
        if(LIBOBS_PATH)
            message(STATUS "Diagnostic: LIBOBS_PATH contents:")
            file(GLOB _children RELATIVE "${LIBOBS_PATH}" "${LIBOBS_PATH}/*")
            foreach(_c ${_children})
                message(STATUS "  ${_c}")
            endforeach()
            if(EXISTS "${LIBOBS_PATH}/include")
                file(GLOB _inc RELATIVE "${LIBOBS_PATH}/include" "${LIBOBS_PATH}/include/*")
                message(STATUS "Diagnostic: LIBOBS_PATH/include contents:")
                foreach(_c ${_inc})
                    message(STATUS "  ${_c}")
                endforeach()
            endif()
            if(EXISTS "${LIBOBS_PATH}/bin/64bit")
                file(GLOB _bin RELATIVE "${LIBOBS_PATH}/bin/64bit" "${LIBOBS_PATH}/bin/64bit/*")
                message(STATUS "Diagnostic: LIBOBS_PATH/bin/64bit contents:")
                foreach(_c ${_bin})
                    message(STATUS "  ${_c}")
                endforeach()
            endif()
        endif()
        message(FATAL_ERROR
            "Could not find libobs. Pass -DLIBOBS_PATH=<obs install or build dir, "
            "or libobs.framework on macOS>, or install libobs system-wide.")
    endif()

    if(APPLE AND _OBS_FRAMEWORK)
        # Link as a framework: clang accepts the .framework dir as a -framework
        # input via the IMPORTED_LOCATION + FRAMEWORK property combo.
        add_library(OBS::libobs INTERFACE IMPORTED)
        set_target_properties(OBS::libobs PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "-F${LIBOBS_PATH} -framework libobs")
    else()
        add_library(OBS::libobs UNKNOWN IMPORTED)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION             "${LIBOBS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}")
    endif()
endif()

# -----------------------------------------------------------------------------
# Rive runtime - prebuilt static libs from scripts/build_rive.{sh,bat}.
# -----------------------------------------------------------------------------
set(RIVE_DIR "${CMAKE_SOURCE_DIR}/third_party/rive-runtime")

if(CMAKE_CONFIGURATION_TYPES)
    set(RIVE_OUT_DIR
        "${RIVE_DIR}/renderer/out/$<IF:$<CONFIG:Debug>,debug,release>")
else()
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _rive_cfg)
    if(_rive_cfg STREQUAL "")
        set(_rive_cfg "release")
    endif()
    set(RIVE_OUT_DIR "${RIVE_DIR}/renderer/out/${_rive_cfg}")
endif()

if(WIN32)
    file(GLOB RIVE_LIBS "${RIVE_DIR}/renderer/out/release/*.lib")
else()
    file(GLOB RIVE_LIBS "${RIVE_OUT_DIR}/*.a")
endif()

if(NOT RIVE_LIBS AND NOT CMAKE_CONFIGURATION_TYPES)
    message(WARNING "No Rive static libs found under ${RIVE_OUT_DIR}.\n"
                    "Run scripts/build_rive.sh (or build_rive.bat) first.")
endif()

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------
set(OBS_RIVE_SOURCES
    src/plugin-main.cpp
    src/rive-source.cpp
    src/rive-core.cpp
)
if(APPLE)
    list(APPEND OBS_RIVE_SOURCES src/backend_metal.mm)
elseif(WIN32)
    list(APPEND OBS_RIVE_SOURCES src/backend_d3d11.cpp)
elseif(UNIX)
    list(APPEND OBS_RIVE_SOURCES src/backend_gl.cpp)
else()
    message(FATAL_ERROR "Unsupported platform.")
endif()

# -----------------------------------------------------------------------------
# Plugin target
# -----------------------------------------------------------------------------
if(APPLE)
    add_library(obs-rive MODULE ${OBS_RIVE_SOURCES})
    set_target_properties(obs-rive PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        SUFFIX ""
    )
elseif(WIN32)
    add_library(obs-rive SHARED ${OBS_RIVE_SOURCES})
    set_target_properties(obs-rive PROPERTIES PREFIX "")
else()
    add_library(obs-rive SHARED ${OBS_RIVE_SOURCES})
    # Drop the default 'lib' prefix CMake adds on Linux - OBS plugins on
    # Linux are conventionally named '<name>.so', not 'lib<name>.so'.
    set_target_properties(obs-rive PROPERTIES PREFIX "")
endif()

target_include_directories(obs-rive PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${RIVE_DIR}/include"
    "${RIVE_DIR}/renderer/include"
)

if(TARGET OBS::libobs)
    target_link_libraries(obs-rive PRIVATE OBS::libobs ${RIVE_LIBS})
else()
    target_link_libraries(obs-rive PRIVATE ${RIVE_LIBS})
endif()

if(APPLE)
    target_compile_options(obs-rive PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(obs-rive PRIVATE
        "-framework Foundation"
        "-framework CoreFoundation"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework CoreGraphics"
        "-framework CoreText"
        "-framework ImageIO"
    )
elseif(WIN32)
    target_compile_definitions(obs-rive PRIVATE
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
    # OBS's util_uint64.h calls _udiv128 / _umul128 under a feature check
    # that clang-cl satisfies, but the intrinsics aren't actually exposed
    # by clang-cl. Force-include a small compat header that supplies
    # __int128-backed fallbacks via #define, so the OBS macros expand to
    # something clang can codegen.
    target_compile_options(obs-rive PRIVATE
        "/FI${CMAKE_SOURCE_DIR}/src/clang_cl_obs_compat.h")
    target_link_libraries(obs-rive PRIVATE
        d3d11 dxgi dxguid d3dcompiler
    )
    set_property(TARGET obs-rive PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # Linux: Rive's GL backend uses desktop GL; we drive it via EGL.
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    find_package(OpenGL REQUIRED)
    find_library(EGL_LIBRARY NAMES EGL)
    if(NOT EGL_LIBRARY)
        message(FATAL_ERROR "EGL not found. Install libegl1-mesa-dev or equivalent.")
    endif()
    # RIVE_DESKTOP_GL switches rive/renderer/gl/gles3.hpp into the desktop-GL
    # branch (includes glad_custom.h with GLuint/GLenum/etc). Must match how
    # the Rive runtime itself was compiled.
    target_compile_definitions(obs-rive PRIVATE RIVE_DESKTOP_GL)
    # glad_custom.h + the generated glad headers live alongside the Rive
    # renderer sources, not under renderer/include.
    target_include_directories(obs-rive PRIVATE
        "${RIVE_DIR}/renderer/glad"
        "${RIVE_DIR}/renderer/glad/include"
    )
    target_link_libraries(obs-rive PRIVATE
        ${EGL_LIBRARY}
        OpenGL::GL
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
endif()

# Stage the locale data next to the binary for development installs.
install(DIRECTORY data/ DESTINATION data)
