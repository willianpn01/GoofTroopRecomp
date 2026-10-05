@echo off
rem Goof Troop Recomp -- remove everything setup_windows.bat generated.
rem Deletes only the build\ and dist\ folders next to this file.
setlocal EnableExtensions DisableDelayedExpansion
set "REPO=%~dp0"
if not exist "%REPO%CMakeLists.txt" goto wrong_place
if not exist "%REPO%tools\goof_setup.py" goto wrong_place
if exist "%REPO%build\" rmdir /s /q "%REPO%build"
if exist "%REPO%dist\" rmdir /s /q "%REPO%dist"
if exist "%REPO%build\" goto failed
if exist "%REPO%dist\" goto failed
echo CLEAN: done ^(build\ and dist\ removed^)
endlocal & exit /b 0
:wrong_place
echo CLEAN: refused - this file is not inside the Goof Troop Recomp folder.
endlocal & exit /b 32
:failed
echo CLEAN: could not remove build\ or dist\ ^(is the game still running?^)
endlocal & exit /b 32
