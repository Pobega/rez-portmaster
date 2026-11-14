cmake_minimum_required(VERSION 3.24)
project(rez_recomp VERSION 1.0.1 LANGUAGES C CXX)

# Rez (Dreamcast) as a native program. engine/ translates the game's SH-4 code to C++ at build time
# and provides the runtime it runs on; src/framework/ is the port layer (settings, HUD);
# src/ replaces Sega's libraries with native code; game/rez.toml describes the game.

set(DREAM_WERROR OFF CACHE BOOL "Warnings as errors in engine targets")

cmake_host_system_information(RESULT _dc_threads QUERY NUMBER_OF_LOGICAL_CORES)
if(_dc_threads LESS 1)
  set(_dc_threads 1)
elseif(_dc_threads GREATER 64)
  set(_dc_threads 64)
endif()
set(DREAM_GEN_SHARDS ${_dc_threads} CACHE STRING "Parts the generated game code is split into")

option(DREAM_STRICT_FP "Enforce strict cross-host floating-point rounding and disable FMA" ON)
set(DREAMCOMP_FP_OPTIONS
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:$<IF:$<BOOL:${DREAM_STRICT_FP}>,-ffp-contract=off,-ffp-contract=fast>>
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:$<IF:$<BOOL:${DREAM_STRICT_FP}>,-ffp-contract=off,-ffp-contract=fast>>)
add_compile_options(${DREAMCOMP_FP_OPTIONS})
add_subdirectory(engine)
include(cmake/Port.cmake)

set(DREAMCOMP_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

add_library(rcheevos STATIC
    third_party/rcheevos/src/rc_client.c
    third_party/rcheevos/src/rc_compat.c
    third_party/rcheevos/src/rc_util.c
    third_party/rcheevos/src/rc_version.c
    third_party/rcheevos/src/rapi/rc_api_common.c
    third_party/rcheevos/src/rapi/rc_api_info.c
    third_party/rcheevos/src/rapi/rc_api_runtime.c
    third_party/rcheevos/src/rapi/rc_api_user.c
    third_party/rcheevos/src/rcheevos/alloc.c
    third_party/rcheevos/src/rcheevos/condition.c
    third_party/rcheevos/src/rcheevos/condset.c
    third_party/rcheevos/src/rcheevos/consoleinfo.c
    third_party/rcheevos/src/rcheevos/format.c
    third_party/rcheevos/src/rcheevos/lboard.c
    third_party/rcheevos/src/rcheevos/memref.c
    third_party/rcheevos/src/rcheevos/operand.c
    third_party/rcheevos/src/rcheevos/rc_validate.c
    third_party/rcheevos/src/rcheevos/richpresence.c
    third_party/rcheevos/src/rcheevos/runtime.c
    third_party/rcheevos/src/rcheevos/runtime_progress.c
    third_party/rcheevos/src/rcheevos/trigger.c
    third_party/rcheevos/src/rcheevos/value.c
    third_party/rcheevos/src/rhash/cdreader.c
    third_party/rcheevos/src/rhash/hash.c
    third_party/rcheevos/src/rhash/hash_disc.c
    third_party/rcheevos/src/rhash/hash_rom.c
    third_party/rcheevos/src/rhash/hash_zip.c
    third_party/rcheevos/src/rhash/md5.c)
target_include_directories(rcheevos PUBLIC ${PROJECT_SOURCE_DIR}/third_party/rcheevos/include
                                    PRIVATE ${PROJECT_SOURCE_DIR}/third_party/rcheevos/src)
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH RC_HASH_NO_ENCRYPTED)
target_compile_features(rcheevos PRIVATE c_std_99)

add_library(dreamcomp_core OBJECT src/framework/core.cpp src/framework/settings.cpp
                                  src/framework/hud.cpp src/framework/sharp2d.cpp
                                  src/framework/disc_folder.cpp
                                  src/framework/ra_creds.cpp
                                  src/framework/ra_http.cpp
                                  src/framework/ra_client.cpp)
target_include_directories(dreamcomp_core PUBLIC ${PROJECT_SOURCE_DIR}/include
                                                 ${PROJECT_SOURCE_DIR}/src/framework)
target_link_libraries(dreamcomp_core PUBLIC dream::runtime dream::render dream::game_config rcheevos ${CMAKE_DL_LIBS})
target_compile_features(dreamcomp_core PUBLIC cxx_std_20)
target_compile_options(dreamcomp_core PRIVATE ${DREAMCOMP_WARNINGS})
if(TARGET dream_render_vk)
  target_link_libraries(dreamcomp_core PUBLIC dream::render_vk)
  target_compile_definitions(dreamcomp_core PRIVATE DREAM_WITH_RENDERER)
endif()

add_subdirectory(game)

enable_testing()
add_executable(test_ra_e2e tests/test_ra_e2e.cpp)
target_include_directories(test_ra_e2e PRIVATE src engine/core/include engine/runtime/include engine/render/include
                                               third_party/rcheevos/include third_party/rcheevos/src)
target_link_libraries(test_ra_e2e PRIVATE dreamcomp_core dream::runtime dream::render rcheevos ${CMAKE_DL_LIBS})
target_compile_features(test_ra_e2e PRIVATE cxx_std_20)
target_compile_options(test_ra_e2e PRIVATE ${DREAMCOMP_WARNINGS})
add_test(NAME test_ra_e2e COMMAND test_ra_e2e WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
