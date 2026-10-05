# Cross toolchain: Linux host -> Windows x86-64, MinGW-w64 GCC.
#
# Used by the windows-mingw-cross preset (CMakePresets.json).  Ubuntu/Debian
# package gcc-mingw-w64-x86-64 provides these compiler names and the target
# sysroot below.  Only C is used by the project.
# GOOF_WINDOWS_PORTABILITY_IMPLEMENTATION.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Search headers/libraries/packages only in the target sysroot, never in the
# Linux host's /usr.  SDL2 for Windows is located through SDL2_DIR, which
# find_package honours directly.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
