cmake_minimum_required(VERSION 3.16)
project(GoofTroopGates C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(GOOF_AOT_GENERATED_DIR "" CACHE PATH
  "Generated AOT C directory produced by tools/generate_aot.py")

# The shared compiled guest execution core (gate V10).  These gates link
# goof_core; they never compile a core source themselves.  Include directories,
# compile definitions and compile options arrive with the target, so no
# consumer restates them.  goof_core is SDL-free and this include never looks
# for SDL.
include(${ROOT}/cmake/goof_core.cmake)

# Windows builds (MinGW) omit three Linux-only test targets: goof_phase4_boot
# and goof_ppu_window_hdma_test resolve further dead host symbols by ELF
# --gc-sections, and goof_pbn_refhost needs dlopen.  The E1000, PBN and
# headless-frames gates build on every platform.
if(NOT WIN32)
add_executable(goof_phase4_boot boot_harness.c)
target_link_libraries(goof_phase4_boot PRIVATE goof_core)
target_link_options(goof_phase4_boot PRIVATE -Wl,--gc-sections)
endif()

# Headless frame pipeline gate: certified epochs + isolated PPU render.
# No SDL/OpenGL/host presentation.  The MMIO wraps only count accesses made
# while a render is in progress.
add_executable(goof_headless_frames headless_frame_gate.c)
target_link_libraries(goof_headless_frames PRIVATE goof_core_render)

# Permanent boot gate for the proven deterministic domain [E1, E1000]:
# twelve semantic checkpoints, six framebuffer anchors, the E154 IPL upload
# contract, the E1000 APU endpoint and the campaign-wide invariants.
# Headless: no SDL/OpenGL/host presentation and no image output by default.
add_executable(goof_boot_e1000 goof_boot_e1000_gate.c)
target_link_libraries(goof_boot_e1000 PRIVATE goof_core_render)

# Independent acceptance oracles for the physical-frame boundary milestone:
# PBN1 (frozen reference VRAM block), PBN3 (queue-drain causal chain) and
# PBN10 (visual reference).  None of them reads a workspace hash.
# P5: goof_pbn1_reference.h is a captured runtime VRAM block (publication
# review pending); the gate can be left out with -DGOOF_BUILD_PBN_GATE=OFF.
option(GOOF_BUILD_PBN_GATE "Build the PBN acceptance gate" ON)
if(GOOF_BUILD_PBN_GATE)
add_executable(goof_pbn goof_pbn_gate.c)
target_link_libraries(goof_pbn PRIVATE goof_core_render)
endif()

# PPU window logic + render-time HDMA replay unit tests (no ROM, no guest
# execution): GOOF_PPU_HDMA_WINDOW_SPOTLIGHT_IMPLEMENTATION.
if(NOT WIN32)
add_executable(goof_ppu_window_hdma_test goof_ppu_window_hdma_test.c)
target_link_libraries(goof_ppu_window_hdma_test PRIVATE goof_core_render)
target_link_options(goof_ppu_window_hdma_test PRIVATE -Wl,--gc-sections)
endif()

# P5: the libretro PBN reference host (pbn_refhost.c) is not part of the
# clean tree; it only regenerates goof_pbn1_reference.h.

# Test-only link instrumentation.  It lives here, on the gate targets, and
# never in goof_core or in any product target.
foreach(target goof_headless_frames goof_boot_e1000)
  target_link_options(${target} PRIVATE -Wl,--gc-sections
    -Wl,--wrap=ReadReg,--wrap=ReadRegWord,--wrap=WriteReg,--wrap=WriteRegWord
    -Wl,--wrap=apu_cpuWrite,--wrap=audio_trace_on_spc_port_write)
endforeach()

# The PBN gate observes guest stores and DMA triggers instead of MMIO reads.
if(GOOF_BUILD_PBN_GATE)
target_link_options(goof_pbn PRIVATE -Wl,--gc-sections
  -Wl,--wrap=cpu_write8,--wrap=cpu_write16,--wrap=ppudma_record_dma
  -Wl,--wrap=interp_cursor_step,--wrap=interp_cursor_dispatch_aot)
endif()
