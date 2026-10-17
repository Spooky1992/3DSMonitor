@echo off
rem Converts 3DSMonitor files on Windows:
rem   shot_*.raw -> .png (raw2png.py)   rec_*.raw -> .mp4 (convert.py)
rem Double-click: converts every .raw in this folder and its subfolders.
rem Drag and drop .raw files on it: converts those files.
rem raw2png.py, convert.py and rawfile.py must be in the same folder as this file.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

for %%s in (raw2png.py convert.py rawfile.py) do (
  if not exist "%~dp0%%s" (
    echo ERREUR : %%s doit etre dans le meme dossier que ce fichier : %~dp0
    pause
    exit /b 1
  )
)

set "PY="
where py >nul 2>nul && set "PY=py"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
  echo ERREUR : Python n'est pas installe sur ce PC.
  echo Installe-le depuis https://www.python.org/downloads/
  echo en cochant "Add python.exe to PATH", puis relance ce fichier.
  pause
  exit /b 1
)

set COUNT=0
if "%~1"=="" (
  for /r %%f in (*.raw) do call :convert "%%f"
) else (
  for %%f in (%*) do call :convert "%%~f"
)

echo.
if "!COUNT!"=="0" (
  echo Aucun fichier .raw trouve dans %~dp0 ni dans ses sous-dossiers.
) else (
  echo Termine : !COUNT! fichier^(s^) traite^(s^). Les .png / .mp4 sont a cote des .raw.
)
pause
exit /b 0

:convert
set /a COUNT+=1
set "NAME=%~n1"
if /i "!NAME:~0,4!"=="rec_" (
  call :need_ffmpeg
  %PY% "%~dp0convert.py" "%~1"
) else (
  %PY% "%~dp0raw2png.py" "%~1"
)
exit /b 0

:need_ffmpeg
if defined FFMPEG_OK exit /b 0
where ffmpeg >nul 2>nul && set "FFMPEG_OK=1"
if not defined FFMPEG_OK %PY% -c "import imageio_ffmpeg" >nul 2>nul && set "FFMPEG_OK=1"
if not defined FFMPEG_OK (
  echo Installation de ffmpeg pour Python ^(une seule fois^)...
  %PY% -m pip install --user imageio-ffmpeg
  set "FFMPEG_OK=1"
)
exit /b 0
