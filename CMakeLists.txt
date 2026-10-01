cmake_minimum_required(VERSION 3.24)

project(ClaudePets LANGUAGES CXX RC)

set(CLAUDE_PETS_VERSION "" CACHE STRING "Version embedded into claude-pets.exe as major.minor.patch; empty takes the latest v* git tag")
set(PETS_VERSION "${CLAUDE_PETS_VERSION}")
if(PETS_VERSION STREQUAL "")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE PETS_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        string(REGEX REPLACE "^v" "" PETS_VERSION "${PETS_VERSION}")
    endif()
endif()
if(NOT PETS_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(PETS_VERSION "0.0.0")
endif()
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" PETS_VERSION_MATCH "${PETS_VERSION}")
set(PETS_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(PETS_VERSION_MINOR "${CMAKE_MATCH_2}")
set(PETS_VERSION_PATCH "${CMAKE_MATCH_3}")
configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)
message(STATUS "claude-pets version ${PETS_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(claude-pets WIN32
    src/animation.cpp
    src/app.cpp
    src/claude_app.cpp
    src/desktop_sessions.cpp
    src/diagnostics.cpp
    src/frame_clock.cpp
    src/hooks.cpp
    src/i18n.cpp
    src/json.cpp
    src/log.cpp
    src/main.cpp
    src/monitor.cpp
    src/notices.cpp
    src/overlay.cpp
    src/overlay_input.cpp
    src/overlay_layout.cpp
    src/overlay_motion.cpp
    src/pets.cpp
    src/renderer.cpp
    src/settings.cpp
    src/system_integration.cpp
    src/transcript.cpp
    src/util.cpp
    res/app.rc
)

target_include_directories(claude-pets PRIVATE src res "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(claude-pets PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A00000B
)

if(MSVC)
    target_compile_options(claude-pets PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:inline /GS /guard:cf
        $<$<CONFIG:Release>:/O2 /Gy /Gw>)
    target_link_options(claude-pets PRIVATE /MANIFEST:NO /GUARD:CF /DYNAMICBASE /NXCOMPAT
        /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:windowscodecs.dll /DELAYLOAD:shcore.dll /DELAYLOAD:dxgi.dll
        /DELAYLOAD:shell32.dll /DELAYLOAD:ole32.dll
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
endif()

target_link_libraries(claude-pets PRIVATE d2d1 dwrite dxgi windowscodecs shcore shell32 ole32 user32 gdi32 advapi32 delayimp)

set_target_properties(claude-pets PROPERTIES OUTPUT_NAME "claude-pets")
