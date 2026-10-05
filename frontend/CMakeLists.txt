cmake_minimum_required(VERSION 3.16)
project(GoofTroopFrontend C)
set(CMAKE_C_STANDARD 11)

# Goof Troop Recomp -- permanent application area.
#
# Target layers, and the split is the point:
#
#   goof_core          the shared compiled guest execution core (gate V10).
#                      Defined in cmake/goof_core.cmake, compiled once, linked
#                      by these targets AND by the phase4_boot gates.
#   goof_app_core      STATIC, SDL-FREE.  ONLY the application coordinator:
#                      goof_app.c + goof_compare.c + goof_input.c.  No core
#                      source is
#                      compiled here.
#   goof_app_headless  SDL-FREE executable over goof_app_core.  Always built.
#                      Makes "the coordinator is linkable without SDL" a
#                      link-level fact (gate V11).
#   goof_recomp        The interactive player.  The ONLY target that links
#                      SDL.  Built only with -DGOOF_BUILD_FRONTEND=ON.
#
# Both executables link the SAME goof_app_core archive, so the comparison path
# and the player run the same compiled goof_app (gate V10b).

option(GOOF_BUILD_FRONTEND "Build the SDL2 interactive frontend" OFF)

set(GOOF_AOT_GENERATED_DIR "" CACHE PATH
  "Clean-room generated AOT directory produced by reproduce_aot_artifacts.py")

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/goof_core.cmake)

add_library(goof_app_core STATIC goof_app.c goof_compare.c goof_input.c)
target_include_directories(goof_app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(goof_app_core PUBLIC goof_core_render)
# No SDL include directory, no SDL library, no SDL definition reaches this
# target -- not even transitively.  This is gate V11's build-system half.

# GOOF_ENHANCEMENTS_E1: host-side input policy, SDL-FREE and guest-free --
# default key/pad bindings (host/goof_host_input.c) and player-slot ownership
# (host/goof_controller.c).  Only goof_recomp and the host tests link it;
# goof_app_headless and every phase4 gate do not, so no oracle executable can
# depend on a physical device.
#
# GOOF_ENHANCEMENTS_E2 adds, still SDL-FREE and guest-free: configurable
# bindings (host/goof_bindings.c), the config.ini reader/writer
# (host/goof_config.c) and the Settings overlay state machine, pages and
# bitmap font (host/ui/).  Same library, same rule: never linked into an
# oracle executable, so no user config can reach a gate (invariant O12).
#
# GOOF_ENHANCEMENTS_E3 adds, same library, same rule: host video / audio
# settings (host/goof_present_settings.c, host/goof_audio_settings.c) and the
# Settings > Video / Audio pages (host/ui/goof_video_menu.c,
# host/ui/goof_audio_menu.c).
#
# GOOF_ENHANCEMENTS_E4 adds, same library, same rule: the host simulation
# clock / pacing policy (host/goof_frame_pacer.c), clock-free so it runs
# against a fake clock in tests/goof_frame_pacer_test.c.  It needs libm.
add_library(goof_host_input STATIC host/goof_host_input.c host/goof_controller.c
            host/goof_bindings.c host/goof_config.c
            host/goof_present_settings.c host/goof_audio_settings.c
            host/goof_frame_pacer.c
            host/ui/goof_bitmap_font.c host/ui/goof_menu.c
            host/ui/goof_input_menu.c host/ui/goof_video_menu.c
            host/ui/goof_audio_menu.c)
target_include_directories(goof_host_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
  target_link_libraries(goof_host_input PUBLIC m)
endif()

option(GOOF_BUILD_HOST_TESTS "Build the E1/E2 host input, config and menu tests" OFF)

add_executable(goof_app_headless main_headless.c)
target_link_libraries(goof_app_headless PRIVATE goof_app_core)
# Same --gc-sections discipline the permanent gates use: the engine runner
# keeps host hooks (save states, frame dump, SPC player) that no Goof target
# provides, and that no Goof target reaches.
target_link_options(goof_app_headless PRIVATE -Wl,--gc-sections)

if(GOOF_BUILD_FRONTEND)
  # SDL is looked for ONLY inside this branch.  With the option OFF (the
  # default) no find_package and no pkg-config call for SDL2 is executed at
  # all, so a machine with no SDL development package configures and builds
  # the headless targets normally (gates V3 and V4).
  message(STATUS "GOOF_BUILD_FRONTEND=ON: locating SDL2 for goof_recomp")
  if(WIN32)
    # The official SDL2 MinGW package puts -mwindows (GUI subsystem) on
    # SDL2::SDL2 unless told not to.  The first Windows player stays a
    # console application so diagnostics remain visible.
    set(SDL2_NO_MWINDOWS ON)
  endif()
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    message(STATUS "goof_recomp: SDL2 via find_package")
    set(GOOF_SDL2_LIBRARIES SDL2::SDL2)
    set(GOOF_SDL2_INCLUDE_DIRS "")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GOOF_SDL2 REQUIRED sdl2)
    message(STATUS "goof_recomp: SDL2 via pkg-config ${GOOF_SDL2_VERSION}")
    set(GOOF_SDL2_LIBRARIES ${GOOF_SDL2_LIBRARIES})
  endif()

  # The SDL2 GameController adapter (GOOF_ENHANCEMENTS_E1).  A library, not
  # a second source of goof_recomp, so goof_recomp keeps exactly one object of
  # its own on its link line (gate V10-B).
  # GOOF_ENHANCEMENTS_E2: sdl_settings.c (Settings overlay event
  # translation + rendering) joins it for the same reason.
  add_library(goof_host_sdl STATIC sdl_pads.c sdl_settings.c sdl_filter.c)
  target_link_libraries(goof_host_sdl PUBLIC goof_host_input ${GOOF_SDL2_LIBRARIES})
  if(GOOF_SDL2_INCLUDE_DIRS)
    target_include_directories(goof_host_sdl PUBLIC ${GOOF_SDL2_INCLUDE_DIRS})
  endif()

  add_executable(goof_recomp main_sdl.c)
  if(WIN32)
    # Windows host adaptation (GOOF_WINDOWS_PORTABILITY_IMPLEMENTATION).
    # SDL.h renames main() to SDL_main() on Windows; SDL2main supplies the
    # real entry point and must precede SDL2 on the link line.
    if(NOT TARGET SDL2::SDL2main)
      message(FATAL_ERROR "goof_recomp: Windows build needs SDL2::SDL2main "
        "from the SDL2 CMake package (set SDL2_DIR or CMAKE_PREFIX_PATH)")
    endif()
    list(PREPEND GOOF_SDL2_LIBRARIES SDL2::SDL2main)
  endif()
  target_link_libraries(goof_recomp PRIVATE goof_app_core goof_host_sdl
                        ${GOOF_SDL2_LIBRARIES})
  if(GOOF_SDL2_INCLUDE_DIRS)
    target_include_directories(goof_recomp PRIVATE ${GOOF_SDL2_INCLUDE_DIRS})
  endif()
  target_link_options(goof_recomp PRIVATE -Wl,--gc-sections)
  if(WIN32)
    # -mconsole pins the console subsystem.  The executable is named
    # GoofTroopRecomp.exe and SDL2.dll is copied beside it, so the build
    # directory runs without any PATH lookup.
    target_link_options(goof_recomp PRIVATE -mconsole)
    set_target_properties(goof_recomp PROPERTIES OUTPUT_NAME GoofTroopRecomp)
    add_custom_command(TARGET goof_recomp POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:goof_recomp>
      VERBATIM)
  endif()
else()
  message(STATUS "GOOF_BUILD_FRONTEND=OFF: SDL2 is not searched for")
endif()

# E1 host tests.  Off by default so the product and gate builds are unchanged;
# prototypes/e1/run_e1_tests.sh turns them on.
if(GOOF_BUILD_HOST_TESTS)
  add_executable(goof_controller_test tests/goof_controller_test.c)
  target_link_libraries(goof_controller_test PRIVATE goof_host_input)
  add_executable(goof_host_input_test tests/goof_host_input_test.c)
  target_link_libraries(goof_host_input_test PRIVATE goof_host_input)
  # E2 (SDL-free): bindings / mapping, config file, Settings menu state.
  # E3 (SDL-free): video / audio settings, config compatibility, the
  # expanded Settings menu.  E4 (SDL-free): simulation clock / pacing on a
  # fake clock (T1-T18).
  foreach(t goof_bindings_test goof_config_test goof_menu_test
            goof_present_settings_test goof_audio_settings_test
            goof_config_e3_test goof_menu_e3_test
            goof_frame_pacer_test goof_vsync_settings_test goof_filter_settings_test)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE goof_host_input)
  endforeach()
  if(GOOF_BUILD_FRONTEND)
    # E2 (SDL): overlay event translation + offscreen render snapshots.
    add_executable(goof_settings_sdl_test tests/goof_settings_sdl_test.c)
    add_executable(goof_filter_sdl_test tests/goof_filter_sdl_test.c)
    target_link_libraries(goof_filter_sdl_test PRIVATE goof_host_sdl goof_app_core)
    target_link_options(goof_filter_sdl_test PRIVATE -Wl,--gc-sections)
    target_link_libraries(goof_settings_sdl_test PRIVATE goof_host_sdl)
    # E3 (SDL): Video / Audio page snapshots through the real overlay renderer.
    add_executable(goof_settings_e3_sdl_test tests/goof_settings_e3_sdl_test.c)
    target_link_libraries(goof_settings_e3_sdl_test PRIVATE goof_host_sdl)
    add_executable(goof_mapping_regression_test tests/goof_mapping_regression_test.c)
    target_link_libraries(goof_mapping_regression_test PRIVATE goof_host_sdl)
    add_executable(goof_sdl_pads_virtual_test tests/goof_sdl_pads_virtual_test.c)
    target_link_libraries(goof_sdl_pads_virtual_test PRIVATE goof_host_sdl)
    if(WIN32)
      # Both include SDL.h, which renames main(); same entry point as goof_recomp.
      foreach(t goof_mapping_regression_test goof_sdl_pads_virtual_test
                goof_settings_sdl_test goof_settings_e3_sdl_test goof_filter_sdl_test)
        target_link_libraries(${t} PRIVATE SDL2::SDL2main)
        target_link_options(${t} PRIVATE -mconsole)
      endforeach()
    endif()
  endif()
endif()
