@echo off
rem Goof Troop Recomp -- start the game built by setup_windows.bat
setlocal EnableExtensions DisableDelayedExpansion
set "REPO=%~dp0"
if not exist "%REPO%dist\run_game.bat" goto not_built
call "%REPO%dist\run_game.bat" %*
endlocal & exit /b %ERRORLEVEL%
:not_built
echo The game has not been built yet. Run:
echo     setup_windows.bat C:\path\to\rom.sfc
if not defined GOOF_NO_PAUSE pause
endlocal & exit /b 1
