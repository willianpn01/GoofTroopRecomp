# goof_core.cmake -- the single source of truth AND the single compiled artefact
# for the Goof guest execution core.
#
# Included by the top-level CMakeLists.txt (the umbrella build), by
# gates/CMakeLists.txt (the permanent headless gates) and by
# frontend/CMakeLists.txt (the interactive video frontend).  Target creation is
# idempotent, so a standalone configure of either subproject and a combined
# configure of both produce exactly one goof_core.
#
# WHAT THIS FILE OWNS  (gate V10 -- SAME COMPILED GUEST EXECUTION CORE)
#
#   goof_core         init + step of the guest, compiled ONCE:
#                     goof_frame_driver, goof_run_frame_adapter,
#                     goof_gate_common (deterministic contract: logical hash,
#                     FNV-1a, OAM hash, M/X suffix, pinned ROM SHA-256),
#                     the snesrecomp runtime and the generated AOT banks.
#   goof_core_render  the isolated boundary renderer, compiled ONCE.  Separate
#                     from goof_core because goof_phase4_boot does not link it,
#                     and because it is the only core object that carries a
#                     definition (g_new_ppu) the other gates resolve by
#                     garbage collection rather than by linking.
#
# Compile definitions, options and include directories are attached to
# goof_core as PUBLIC, so every consumer -- gate main, goof_app, frontend main
# -- compiles against exactly the same ones.  Consumers must not restate them.
#
# WHAT THIS FILE MUST NEVER OWN
#
#   gate instrumentation (GATE_CHECK / GATE_INVARIANT, checkpoint tables,
#   campaign hashing, live-block diff, PPM dumping, the six -Wl,--wrap hooks,
#   the gate's own bus/hardware-event observers) and anything SDL.  The SDL
#   half is a configure-time failure, not a convention (gate V11).
#
# Inputs:
#   GOOF_AOT_GENERATED_DIR  generated AOT C directory (tools/generate_aot.py)
#   GOOF_ENGINE_DIR         snesrecomp checkout (default: external/snesrecomp)

if(NOT GOOF_AOT_GENERATED_DIR)
  message(FATAL_ERROR
    "Set -DGOOF_AOT_GENERATED_DIR=<dir written by tools/generate_aot.py>")
endif()

get_filename_component(GOOF_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(GOOF_ENGINE_DIR ${GOOF_SOURCE_DIR}/external/snesrecomp CACHE PATH
  "snesrecomp engine checkout (pinned revision, see docs)")
set(GOOF_PHASE4_DIR ${GOOF_SOURCE_DIR}/runtime)
set(GOOF_RUNNER_DIR ${GOOF_ENGINE_DIR}/runner/src)

foreach(goof_required ${GOOF_RUNNER_DIR}/snes/snes.c
        ${GOOF_AOT_GENERATED_DIR}/dispatch_v2.c)
  if(NOT EXISTS ${goof_required})
    message(FATAL_ERROR "goof_core.cmake: missing ${goof_required}")
  endif()
endforeach()
unset(goof_required)

set(GOOF_CORE_SNES_SOURCES
  ${GOOF_RUNNER_DIR}/snes/apu.c
  ${GOOF_RUNNER_DIR}/snes/cart.c
  ${GOOF_RUNNER_DIR}/snes/cpu.c
  ${GOOF_RUNNER_DIR}/snes/dma.c
  ${GOOF_RUNNER_DIR}/snes/dsp.c
  ${GOOF_RUNNER_DIR}/snes/audio_shadow.c
  ${GOOF_RUNNER_DIR}/snes/dsp_shadow.c
  ${GOOF_RUNNER_DIR}/snes/msu1.c
  ${GOOF_RUNNER_DIR}/snes/color_lut.c
  ${GOOF_RUNNER_DIR}/snes/ppu.c
  ${GOOF_RUNNER_DIR}/snes/ppu_old.c
  ${GOOF_RUNNER_DIR}/snes/snes.c
  ${GOOF_RUNNER_DIR}/snes/snes_other.c
  ${GOOF_RUNNER_DIR}/snes/spc.c
  ${GOOF_RUNNER_DIR}/snes/interp816.c
  ${GOOF_RUNNER_DIR}/snes/interp_bridge.c)

set(GOOF_CORE_SOURCES
  ${GOOF_PHASE4_DIR}/goof_frame_driver.c
  ${GOOF_PHASE4_DIR}/goof_gate_common.c
  ${GOOF_PHASE4_DIR}/goof_run_frame_adapter.c
  ${GOOF_RUNNER_DIR}/common_cpu_infra.c
  ${GOOF_RUNNER_DIR}/common_rtl.c
  ${GOOF_RUNNER_DIR}/recomp_hw.c
  ${GOOF_RUNNER_DIR}/cpu_state.c
  ${GOOF_RUNNER_DIR}/cpu_trace.c
  ${GOOF_RUNNER_DIR}/sha256.c
  ${GOOF_RUNNER_DIR}/audio_trace.c
  ${GOOF_RUNNER_DIR}/ppu_dma_trace.c
  ${GOOF_CORE_SNES_SOURCES}
  ${GOOF_AOT_GENERATED_DIR}/bank00_v2.c
  ${GOOF_AOT_GENERATED_DIR}/bank01_v2.c
  ${GOOF_AOT_GENERATED_DIR}/bank02_v2.c
  ${GOOF_AOT_GENERATED_DIR}/dispatch_v2.c
  ${GOOF_AOT_GENERATED_DIR}/aot_entries_v2.c
  ${GOOF_AOT_GENERATED_DIR}/unresolved_stubs_v2.c)

set(GOOF_CORE_RENDER_SOURCES ${GOOF_PHASE4_DIR}/goof_headless_renderer.c)

set(GOOF_CORE_INCLUDE_DIRS
  ${GOOF_PHASE4_DIR}
  ${GOOF_PHASE4_DIR}/include
  ${GOOF_RUNNER_DIR}
  ${GOOF_RUNNER_DIR}/snes)

# SNESRECOMP_BLOCK_MOVE_GATE keeps the MVN/MVP hook that generated code
# ALREADY calls alive at SNESRECOMP_TRACE=0, exactly as SNESRECOMP_MX_ENTRY_GATE
# does for cpu_trace_func_entry.  It certifies declared timing residual R2 (the
# emitter charges a generated block move as ONE byte), which is only safe while
# no generated block move executes.  `et` is a compile-time constant at every
# call site, so every other trace event still folds away to nothing.
set(GOOF_CORE_COMPILE_DEFINITIONS
  SNESRECOMP_TRACE=0
  SNESRECOMP_REVERSE_DEBUG=0
  SNESRECOMP_MX_ENTRY_GATE=1
  SNESRECOMP_BLOCK_MOVE_GATE=1)

set(GOOF_CORE_COMPILE_OPTIONS
  -ffunction-sections -fdata-sections -w -Wno-implicit-function-declaration)

# --- gate V11 guard: no SDL may reach the shared core -------------------
foreach(goof_core_path
        ${GOOF_CORE_SOURCES} ${GOOF_CORE_RENDER_SOURCES}
        ${GOOF_CORE_INCLUDE_DIRS})
  if(goof_core_path MATCHES "[Ss][Dd][Ll]")
    message(FATAL_ERROR
      "goof_core.cmake: SDL reached the shared Goof core via ${goof_core_path}")
  endif()
endforeach()
unset(goof_core_path)

# --- the single compiled guest execution core (gate V10) ----------------
if(NOT TARGET goof_core)
  add_library(goof_core STATIC ${GOOF_CORE_SOURCES})
  target_include_directories(goof_core PUBLIC ${GOOF_CORE_INCLUDE_DIRS})
  target_compile_definitions(goof_core PUBLIC ${GOOF_CORE_COMPILE_DEFINITIONS})
  target_compile_options(goof_core PUBLIC ${GOOF_CORE_COMPILE_OPTIONS})
  target_link_libraries(goof_core PUBLIC m)
  # Windows host adaptation only; the Linux configuration is untouched.
  #   goof_host_hooks.c  inert definitions of the three dead engine host hooks
  #                      that COFF links need and ELF --gc-sections hides.
  #   __USE_MINGW_ANSI_STDIO=1  C99 printf (%zu, %llx) on every MinGW CRT,
  #                      including the msvcrt the Ubuntu cross compiler
  #                      targets.  PUBLIC, so every Windows TU formats alike.
  if(WIN32)
    target_sources(goof_core PRIVATE ${GOOF_PHASE4_DIR}/goof_host_hooks.c)
  endif()
  if(MINGW)
    target_compile_definitions(goof_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
  endif()
endif()

if(NOT TARGET goof_core_render)
  add_library(goof_core_render STATIC ${GOOF_CORE_RENDER_SOURCES})
  target_link_libraries(goof_core_render PUBLIC goof_core)
endif()
