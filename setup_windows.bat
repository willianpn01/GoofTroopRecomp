@echo off
rem Goof Troop Recomp -- one-command setup for Windows.
rem
rem   setup_windows.bat C:\path\to\rom.sfc   validate ROM, generate, build, fill dist\
rem   setup_windows.bat --install-deps       install the build tools into MSYS2 (asks first)
rem   setup_windows.bat --clean              remove build\ and dist\
rem
rem You can also drag your ROM file onto this file.
rem Works from Command Prompt, PowerShell or a double-click.  Nothing outside
rem this folder is changed; PATH is only adjusted for this script's own run.
setlocal EnableExtensions DisableDelayedExpansion
set "REPO=%~dp0"
set "PKGS=mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-python"
rem Not named RC: child processes inherit it, and CMake reads RC as the resource compiler.
set "SETUP_RC=0"

if /i "%~1"=="--clean" goto clean

rem --- locate MSYS2 (build tools) ------------------------------------------
set "MSYS="
if defined MSYS2_ROOT if exist "%MSYS2_ROOT%\usr\bin\bash.exe" set "MSYS=%MSYS2_ROOT%"
if not defined MSYS if exist "C:\msys64\usr\bin\bash.exe" set "MSYS=C:\msys64"
if not defined MSYS if exist "%SystemDrive%\msys64\usr\bin\bash.exe" set "MSYS=%SystemDrive%\msys64"
if not defined MSYS goto no_msys
set "UCRT=%MSYS%\ucrt64\bin"

if /i "%~1"=="--install-deps" goto install_deps

rem --- check the build tools -----------------------------------------------
set "MISSING="
if not exist "%UCRT%\gcc.exe" set "MISSING=1" & echo Missing: C compiler ^(gcc^) - compiles the game
if not exist "%UCRT%\cmake.exe" set "MISSING=1" & echo Missing: CMake - configures the build
if not exist "%UCRT%\ninja.exe" set "MISSING=1" & echo Missing: Ninja - runs the build
if not exist "%UCRT%\python.exe" set "MISSING=1" & echo Missing: Python - validates the ROM and generates the game code
if not exist "%UCRT%\SDL2.dll" set "MISSING=1" & echo Missing: SDL2 - window, audio and controller support
if defined MISSING goto missing_tools

rem --- run the setup (PATH change is local to this script) ------------------
set "PATH=%UCRT%;%PATH%"
set "CC=gcc"
set "PYTHONDONTWRITEBYTECODE=1"
"%UCRT%\python.exe" "%REPO%tools\goof_setup.py" %*
set "SETUP_RC=%ERRORLEVEL%"
goto done

:no_msys
echo.
echo SETUP: MSYS2 was not found.
echo.
echo   MSYS2 provides the compiler and build tools this project uses.
echo   1. Download and install it from https://www.msys2.org
echo      ^(keep the default folder C:\msys64^)
echo   2. Run:   setup_windows.bat --install-deps
echo   3. Run:   setup_windows.bat C:\path\to\rom.sfc
echo.
echo   Installed somewhere else? Set MSYS2_ROOT to that folder and run again.
set "SETUP_RC=30"
goto done

:missing_tools
echo.
echo SETUP: some build tools are not installed in MSYS2 ^("%MSYS%"^).
echo.
echo   Install them with:
echo       setup_windows.bat --install-deps
echo   then run setup again:
echo       setup_windows.bat C:\path\to\rom.sfc
set "SETUP_RC=30"
goto done

:install_deps
echo This will install the following MSYS2 packages into "%MSYS%":
echo.
echo   %PKGS%
echo.
echo Nothing else on this computer is changed.
set "ANSWER="
set /p "ANSWER=Install now? [y/N] "
if /i not "%ANSWER%"=="y" goto install_cancelled
"%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm %PKGS%"
set "SETUP_RC=%ERRORLEVEL%"
if not "%SETUP_RC%"=="0" goto install_failed
echo.
echo Build tools installed. Now run:
echo     setup_windows.bat C:\path\to\rom.sfc
goto done

:install_cancelled
echo Nothing was installed.
set "SETUP_RC=1"
goto done

:install_failed
echo.
echo SETUP: the package installation failed ^(see the messages above^).
echo   Open "MSYS2 MSYS" from the Start menu once, run:  pacman -Syu
echo   then run again:  setup_windows.bat --install-deps
goto done

:clean
call "%REPO%clean_windows.bat"
set "SETUP_RC=%ERRORLEVEL%"
goto end

:done
echo.
if not defined GOOF_NO_PAUSE pause
:end
endlocal & exit /b %SETUP_RC%
