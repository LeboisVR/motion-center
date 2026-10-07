@echo off
setlocal EnableExtensions

REM ============================================================================
REM  GenerateExe.bat - Build Motion Center V1.2 (onefile .exe)
REM  Embarque toutes les datas via le .spec : icons, images (photos des box),
REM  firmware, Simhub profile, avrdude. Pillow est requis pour les photos JPG.
REM ============================================================================

cd /d "%~dp0"

set "SPEC=Motion Center V1.2 onefile.spec"
set "APPNAME=Motion Center V1.2"

REM --- Choisir l'interpreteur Python ------------------------------------------
set "PY=python"
where python >nul 2>&1
if errorlevel 1 (
    where py >nul 2>&1
    if errorlevel 1 (
        echo [ERREUR] Python introuvable dans le PATH.
        goto :fail
    )
    set "PY=py"
)

echo === Interpreteur : %PY%
%PY% --version

REM --- Verifier / installer les dependances de build --------------------------
echo === Verification des dependances (pyinstaller, pillow, pyserial)...
%PY% -m pip install --disable-pip-version-check --quiet pyinstaller pillow pyserial
if errorlevel 1 (
    echo [ERREUR] Echec de l'installation des dependances.
    goto :fail
)

REM --- Garde-fou : Pillow doit etre importable (sinon photos des box absentes) -
%PY% -c "import PIL, PIL.ImageTk" 2>nul
if errorlevel 1 (
    echo [ERREUR] Pillow/PIL indisponible : les photos des box ne seraient pas incluses.
    goto :fail
)

REM --- Liberer l'ancien exe (instance ouverte / verrou) -----------------------
tasklist /FI "IMAGENAME eq %APPNAME%.exe" 2>nul | find /I "%APPNAME%.exe" >nul
if not errorlevel 1 (
    echo === Fermeture de l'instance en cours de %APPNAME%.exe ...
    taskkill /IM "%APPNAME%.exe" /F >nul 2>&1
    timeout /t 2 /nobreak >nul 2>&1
)
if exist "dist\%APPNAME%.exe" (
    del /F /Q "dist\%APPNAME%.exe" >nul 2>&1
    if exist "dist\%APPNAME%.exe" (
        echo [ERREUR] Impossible de supprimer dist\%APPNAME%.exe.
        echo          Ferme Motion Center s'il est ouvert, ou mets en pause la synchro Google Drive, puis reessaie.
        goto :fail
    )
)

REM --- Build ------------------------------------------------------------------
echo === Compilation via "%SPEC%" ...
%PY% -m PyInstaller --noconfirm --clean "%SPEC%"
if errorlevel 1 (
    echo [ERREUR] La compilation PyInstaller a echoue.
    goto :fail
)

if not exist "dist\%APPNAME%.exe" (
    echo [ERREUR] Executable introuvable : dist\%APPNAME%.exe
    goto :fail
)

echo.
echo === OK : dist\%APPNAME%.exe genere avec succes.
echo.
pause
exit /b 0

:fail
echo.
echo === Build interrompu.
echo.
pause
exit /b 1
