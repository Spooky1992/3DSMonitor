@echo off
rem Converts 3DSMonitor .raw files to .png (Windows).
rem Double-click: converts every .raw in this folder and its subfolders.
rem Drag and drop .raw files on it: converts those files.
rem raw2png.py and rawfile.py must be in the same folder as this file.
cd /d "%~dp0"

if not exist "%~dp0raw2png.py" (
  echo ERREUR : raw2png.py doit etre dans le meme dossier que ce fichier : %~dp0
  pause
  exit /b 1
)

set "PY="
where py >/dev/null 2>/dev/null && set "PY=py"
if not defined PY where python >/dev/null 2>/dev/null && set "PY=python"
if not defined PY (
  echo ERREUR : Python n'est pas installe.
  echo Installe-le depuis https://www.python.org/downloads/
  echo en cochant "Add python.exe to PATH", puis relance ce fichier.
  pause
  exit /b 1
)

set COUNT=0
if "%~1"=="" (
  for /r %%f in (*.raw) do (
    %PY% "%~dp0raw2png.py" "%%f"
    set /a COUNT+=1
  )
) else (
  for %%f in (%*) do (
    %PY% "%~dp0raw2png.py" "%%~f"
    set /a COUNT+=1
  )
)

echo.
if "%COUNT%"=="0" (
  echo Aucun fichier .raw trouve dans %~dp0 ni dans ses sous-dossiers.
) else (
  echo Termine : %COUNT% fichier^(s^) traite^(s^). Les .png sont a cote des .raw.
)
pause
