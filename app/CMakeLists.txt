# bioshock2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bioshock2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BIOSHOCK2_SOURCES
    src/main.cpp
    src/bridge_probe.cpp
    src/bridge_cheats.cpp
    src/bridge_give.cpp
    src/bridge_verify.cpp
    src/bridge_profile.cpp
    src/bridge_loader.cpp
    src/crash_trace.cpp
)

if(WIN32)
    add_executable(bioshock2 WIN32 ${BIOSHOCK2_SOURCES})
else()
    add_executable(bioshock2 ${BIOSHOCK2_SOURCES})
endif()

rexglue_setup_target(bioshock2 GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(bioshock2 PRIVATE dbghelp)  # bridge_profile.cpp: stack sampling
endif()


