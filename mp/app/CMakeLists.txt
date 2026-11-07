# bioshock2mp - ReXGlue recomp of Bioshock2MP_Xenon.xex. SDK boilerplate lives in generated/rexglue.cmake.
cmake_minimum_required(VERSION 3.25)
project(bioshock2mp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(generated/rexglue.cmake)

# MP_SPAWNLOG=OFF builds a clean boot (no VM hooks) - use it if the hooked build crashes before the menu.
option(MP_SPAWNLOG "VM hooks: vocabulary + spawn logging" ON)
set(BIOSHOCK2MP_SOURCES src/main.cpp src/crash_trace.cpp)
if(MP_SPAWNLOG)
    list(APPEND BIOSHOCK2MP_SOURCES src/mp_spawnlog.cpp)
endif()

if(WIN32)
    add_executable(bioshock2mp WIN32 ${BIOSHOCK2MP_SOURCES})
else()
    add_executable(bioshock2mp ${BIOSHOCK2MP_SOURCES})
endif()
rexglue_setup_target(bioshock2mp GPU_PLUGINS xenos)
if(WIN32)
    target_link_libraries(bioshock2mp PRIVATE dbghelp)
endif()
