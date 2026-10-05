cmake_minimum_required(VERSION 3.16)
project(GoofTroopRecomp C)
set(CMAKE_C_STANDARD 11)

# Umbrella build: ONE goof_core shared by every target that advances the
# guest -- the headless gates, the headless comparison binary and the
# interactive player.
#
#                         goof_core  (STATIC, compiled once)
#                              |
#          +-------------------+-------------------+
#          |                   |                   |
#   goof_phase4_boot    goof_core_render      (future consumers)
#                              |
#          +-------------------+-------------------+
#          |                   |                   |
#  goof_headless_frames  goof_boot_e1000     goof_app_core
#                                                  |
#                                        +---------+---------+
#                                        |                   |
#                                  goof_recomp        goof_app_headless
#
# The generated C is NOT part of the source tree.  It is produced locally from
# the user's ROM by tools/generate_aot.py (or tools/goof_build.py, which also
# configures and builds):
#
#   python3 tools/goof_build.py --rom /path/to/rom.sfc
#
# or by hand:
#
#   python3 tools/generate_aot.py --rom /path/to/rom.sfc --out build/generated
#   cmake -S . -B build/cmake -G Ninja \
#         -DGOOF_AOT_GENERATED_DIR=build/generated/aot -DGOOF_BUILD_FRONTEND=ON
#   cmake --build build/cmake

set(GOOF_AOT_GENERATED_DIR "" CACHE PATH
  "Generated AOT C directory produced by tools/generate_aot.py")
if(GOOF_AOT_GENERATED_DIR AND NOT IS_ABSOLUTE "${GOOF_AOT_GENERATED_DIR}")
  get_filename_component(GOOF_AOT_GENERATED_DIR "${GOOF_AOT_GENERATED_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(GOOF_AOT_GENERATED_DIR "${GOOF_AOT_GENERATED_DIR}" CACHE PATH
    "Generated AOT C directory produced by tools/generate_aot.py" FORCE)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/goof_core.cmake)

add_subdirectory(gates)
add_subdirectory(frontend)
