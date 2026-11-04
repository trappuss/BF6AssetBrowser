@echo off
setlocal enabledelayedexpansion
title AssetBrowser - Build and Verify
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  BUILD THE MONOREPO AND RUN THE USER-SIDE HALF OF THE PHASE-4/5 GATES.
::
::  1. verify-src.py (source checks, non-blocking)
::  2. cmake configure + build (preset windows-msvc-release: core, POE2AssetBrowser,
::     FOXAssetBrowser, the conformance tests). The first run builds Qt6 + zlib + qtsvg
::     through vcpkg - 30-90 min and several GB; later runs take seconds to minutes.
::  3. deploy the Qt runtime beside each exe (as run.bat did)
::  4. seed each new exe's data\ INI from the OLD project's INI (same keys), so the
::     first run already knows your game folders - your old installs are untouched
::  5. run each exe:  --check (self-tests)  and  --quit-when-ready (index + tabs),
::     plus the core test exes; every result is PASS / FAIL with the needle it looked for
::
::  Everything is written to _deliveries\verify\ (the full logs per step). Send me that
::  folder if anything fails. Reads your game data; writes only under build\ and
::  _deliveries\.
::
::  CRLF line endings, on purpose: cmd resumes at the wrong line after a call/goto in an
::  LF-only file. Check names carry no ( ) or : - parens break cmd's if-blocks and a
::  colon turns the per-check log into an NTFS stream.
:: ---------------------------------------------------------------------------

:: --nopause: run unattended (Run Everything.bat calls it this way) - no pause on error or at the end.
set "PAUSE=pause"
:: (type nul, not rem: rem would swallow the "& exit /b 1" that follows it on the same line)
if /i "%~1"=="--nopause" set "PAUSE=type nul"
set "OUT=_deliveries\verify"
if not exist "_deliveries" mkdir "_deliveries"
if exist "%OUT%" rd /s /q "%OUT%"
mkdir "%OUT%"
set "LOG=%OUT%\_results.txt"
set /a PASS=0, FAIL=0
> "%LOG%" echo AssetBrowser build+verify  %DATE% %TIME%

set "POE2_OLD=..\POE2AssetBrowser"
set "FOX_OLD=..\FOXAssetBrowser"
set "D4_OLD=..\Diablo4AssetBrowser Native"
set "BUILD=build\release"
set "POE2_EXE=%BUILD%\games\poe2\POE2AssetBrowser.exe"
set "FOX_EXE=%BUILD%\games\fox\FOXAssetBrowser.exe"
set "D4_EXE=%BUILD%\games\d4\D4AssetBrowser.exe"

goto :main

:: ── helpers ─────────────────────────────────────────────────────────────────

:: check <name> <needle> <exe> <args...>   -- run, look for the needle in stdout+stderr
:check
set "NAME=%~1" & set "NEED=%~2" & set "RUN=%~3" & shift & shift & shift
set "ARGS="
:cargs
if "%~1"=="" goto :crun
set ARGS=!ARGS! "%~1"
shift
goto :cargs
:crun
"%RUN%" %ARGS% > "%OUT%\_step.txt" 2>&1
set "RC=!errorlevel!"
type "%OUT%\_step.txt" >> "%OUT%\_%NAME%.log"
findstr /c:"%NEED%" "%OUT%\_step.txt" >nul 2>&1
if errorlevel 1 (
    set /a FAIL+=1
    echo   FAIL  %NAME%   ^(no "%NEED%", rc=!RC!^)
    >> "%LOG%" echo FAIL  %NAME%  needle "%NEED%" not found  rc=!RC!
) else (
    set /a PASS+=1
    echo   PASS  %NAME%
    >> "%LOG%" echo PASS  %NAME%
)
exit /b 0

:: checkany <name> <needleA> <needleB> <exe> <args...>  -- either needle passes; says which
:checkany
set "NAME=%~1" & set "NEEDA=%~2" & set "NEEDB=%~3" & set "RUN=%~4" & shift & shift & shift & shift
set "ARGS="
:aargs
if "%~1"=="" goto :arun
set ARGS=!ARGS! "%~1"
shift
goto :aargs
:arun
"%RUN%" %ARGS% > "%OUT%\_step.txt" 2>&1
set "RC=!errorlevel!"
type "%OUT%\_step.txt" >> "%OUT%\_%NAME%.log"
findstr /c:"%NEEDA%" "%OUT%\_step.txt" >nul 2>&1
if not errorlevel 1 (
    set /a PASS+=1
    echo   PASS  %NAME%   ^(real index: "%NEEDA%"^)
    >> "%LOG%" echo PASS  %NAME%  "%NEEDA%"
    exit /b 0
)
findstr /c:"%NEEDB%" "%OUT%\_step.txt" >nul 2>&1
if not errorlevel 1 (
    set /a PASS+=1
    echo   PASS  %NAME%   ^(no game folder configured yet: "%NEEDB%" - set it in the app once, re-run^)
    >> "%LOG%" echo PASS  %NAME%  "%NEEDB%"  ^(unconfigured^)
    exit /b 0
)
set /a FAIL+=1
echo   FAIL  %NAME%   ^(neither needle, rc=!RC!^)
>> "%LOG%" echo FAIL  %NAME%  neither "%NEEDA%" nor "%NEEDB%"  rc=!RC!
exit /b 0

:: launch <logname> <exe> <args...>  -- run ONCE into _<logname>.log; several :expect checks then read it.
:: QUOTE any argument holding a comma or ';' - call splits on them (run 15: --show-tab a,b,c reached the
:: app as --show-tab a, and b and c were dropped; the app now logs "args: ... ignored" for such leftovers).
:: (2026-09-25: FOX's boot, models, textures, files and catalogue checks were five launches of ~18 s
:: each looking for lines ONE boot writes; POE2's four more. One launch each now - the same lines.)
:launch
set "LNAME=%~1" & set "RUN=%~2" & shift & shift
set "ARGS="
:largs
if "%~1"=="" goto :lrun
set ARGS=!ARGS! "%~1"
shift
goto :largs
:lrun
"%RUN%" %ARGS% > "%OUT%\_%LNAME%.log" 2>&1
set "RC_%LNAME%=!errorlevel!"
exit /b 0

:: expect <name> <logname> <needle> [<unconfigured-needle>]  -- PASS when the launch's log has the needle
:: (or, with no game folder set yet, the second one). A needle starting "re:" is a findstr regex, so a
:: check can require a NON-ZERO count ("re:models: [1-9][0-9]* row") - "models: " matched "0 row(s)".
:expect
set "NAME=%~1" & set "LNAME=%~2" & set "NEED=%~3" & set "NEEDB=%~4"
set "FS=/c:"
if /i "!NEED:~0,3!"=="re:" ( set "FS=/r /c:" & set "NEED=!NEED:~3!" )
findstr %FS%"!NEED!" "%OUT%\_%LNAME%.log" >nul 2>&1
if not errorlevel 1 goto :exppass
if "!NEEDB!"=="" goto :expfail
findstr /c:"!NEEDB!" "%OUT%\_%LNAME%.log" >nul 2>&1
if errorlevel 1 goto :expfail
set /a PASS+=1
echo   PASS  %NAME%   ^(no game folder configured yet: "!NEEDB!" - set it in the app once, re-run^)
>> "%LOG%" echo PASS  %NAME%  "!NEEDB!"  ^(unconfigured^)  [_%LNAME%.log]
exit /b 0
:exppass
set /a PASS+=1
echo   PASS  %NAME%
>> "%LOG%" echo PASS  %NAME%  "!NEED!"  [_%LNAME%.log]
exit /b 0
:expfail
set /a FAIL+=1
echo   FAIL  %NAME%   ^(no "!NEED!" in _%LNAME%.log, rc=!RC_%LNAME%!^)
>> "%LOG%" echo FAIL  %NAME%  needle "!NEED!" not in _%LNAME%.log  rc=!RC_%LNAME%!
exit /b 0

:: deploy <exe-folder>  -- Qt runtime + plugins beside an exe (vcpkg puts them under vcpkg_installed)
:deploy
set "DEST=%~1"
set "VINST=%BUILD%\vcpkg_installed\x64-windows"
copy /y "%VINST%\bin\*.dll" "%DEST%\" >nul 2>&1
for %%P in (platforms imageformats styles iconengines) do (
    if not exist "%DEST%\%%P" mkdir "%DEST%\%%P"
    copy /y "%VINST%\Qt6\plugins\%%P\*.dll" "%DEST%\%%P\" >nul 2>&1
)
exit /b 0

:: foxdict  -- FOX's name dictionaries beside the FOX exe. FOX reads them from <exe folder>\dict
:: (AppPaths::dictDir) when no dictionary folder is set, and the port never had one there: every run
:: logged "0 named" top-level entries, and the colour swatches, FMTT presets and shader pack could not
:: be found by name (2026-09-24). Copied from the folder FOX's own exe reads (build\release\dict, which
:: carries the recovered name lists); only newer files are copied, backups (*.bak*) are skipped.
:foxdict
if not exist "%FOX_OLD%\build\release\dict" ( echo   ^(no %FOX_OLD%\build\release\dict - FOX runs without its name dictionaries^) & exit /b 0 )
robocopy "%FOX_OLD%\build\release\dict" "%BUILD%\games\fox\dict" *.txt /XO /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 ( echo   copying FOX's dictionaries FAILED ^(robocopy %errorlevel%^) & exit /b 0 )
echo   FOX name dictionaries beside the FOX exe
exit /b 0

:: seedini <old-ini> <new-ini>  -- copy the old project's settings once, never overwrite
:seedini
if exist "%~2" exit /b 0
if not exist "%~1" ( echo   ^(no old INI at %~1 - the exe starts on its first-run page^) & exit /b 0 )
for %%D in ("%~2") do if not exist "%%~dpD" mkdir "%%~dpD"
copy /y "%~1" "%~2" >nul
echo   seeded %~2 from the old project's settings
exit /b 0

:: ── main ────────────────────────────────────────────────────────────────────
:main
echo ============================================================
echo  AssetBrowser - build and verify
echo ============================================================
echo.

:: 0. source checks (non-blocking)
set "PYEXE="
py -3 -c "import sys" >nul 2>&1 && set "PYEXE=py -3"
if not defined PYEXE python -c "import sys" >nul 2>&1 && set "PYEXE=python"
if defined PYEXE (
    echo [0] verify-src.py
    %PYEXE% "%~dp0verify-src.py" --quiet > "%OUT%\_verify-src.log" 2>&1
    if errorlevel 1 ( echo   verify-src found problems ^(see %OUT%\_verify-src.log^). Building anyway. ) else ( echo   clean )
) else ( echo [0] verify-src.py skipped - no python on PATH )

:: 1. MSVC + vcpkg
where cl >nul 2>&1
if errorlevel 1 (
    echo [1] Initializing Visual Studio 2022 build tools...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" ( echo   ERROR: Visual Studio 2022 not found. & %PAUSE% & exit /b 1 )
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo   ERROR: MSVC C++ tools not found. & %PAUSE% & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 ( echo   ERROR: vcvars64 failed. & %PAUSE% & exit /b 1 )
) else ( echo [1] MSVC already on PATH. )
if not defined VCPKG_ROOT ( echo   ERROR: VCPKG_ROOT is not set ^(the same one the three projects build with^). & %PAUSE% & exit /b 1 )
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ( echo   ERROR: VCPKG_ROOT="%VCPKG_ROOT%" is not a vcpkg checkout. & %PAUSE% & exit /b 1 )
if not exist "%POE2_OLD%\src\store\AssetStore.h" echo   NOTE: %POE2_OLD% not found - POE2AssetBrowser will not be built.
if not exist "%FOX_OLD%\src\index\ArchiveIndex.h" echo   NOTE: %FOX_OLD% not found - FOXAssetBrowser will not be built.
if not exist "%D4_OLD%\src\casc\CascReader.h" echo   NOTE: %D4_OLD% not found - D4AssetBrowser will not be built.

taskkill /im POE2AssetBrowser.exe /f >nul 2>&1
taskkill /im FOXAssetBrowser.exe /f >nul 2>&1
taskkill /im D4AssetBrowser.exe /f >nul 2>&1

:: 2. configure + build (a moved folder's stale cache is set aside first - see :fixmoved)
call :fixmoved "%~dp0build\release" "%~dp0."
echo [2] Configuring ^(first time: vcpkg builds Qt6 - long^)...
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --preset windows-msvc-release 2>&1 | Tee-Object -FilePath '%~dp0%OUT%\configure_log.txt'; exit $LASTEXITCODE"
if errorlevel 1 (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path '%~dp0%OUT%\configure_log.txt' -Pattern 'CMake Error','error','FAILED' | ForEach-Object { $_.Line } | Select-Object -First 40 | Out-File -FilePath '%~dp0%OUT%\build_errors.txt' -Encoding utf8"
    echo   CONFIGURE FAILED - see %OUT%\build_errors.txt & %PAUSE% & exit /b 1
)
echo [3] Building...
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0%OUT%\build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path '%~dp0%OUT%\build_log.txt' -Pattern 'error C',': error','error LNK','fatal error','FAILED','ninja: build stopped' | ForEach-Object { $_.Line } | Select-Object -First 60 | Out-File -FilePath '%~dp0%OUT%\build_errors.txt' -Encoding utf8"
if not "%RC%"=="0" ( echo   BUILD FAILED - see %OUT%\build_errors.txt & %PAUSE% & exit /b 1 )
echo   build OK

:: 4. runtime beside each exe + settings seeded from the old projects
echo [4] Deploying the Qt runtime and seeding settings...
call :deploy "%BUILD%"
call :deploy "%BUILD%\conformance"
if exist "%POE2_EXE%" (
    call :deploy "%BUILD%\games\poe2"
    call :seedini "%POE2_OLD%\build\release\data\POE2AssetBrowser\POE2AssetBrowser.ini" "%BUILD%\games\poe2\data\POE2AssetBrowser\POE2AssetBrowser.ini"
)
if exist "%FOX_EXE%" (
    call :deploy "%BUILD%\games\fox"
    call :seedini "%FOX_OLD%\build\release\data\FOXAssetBrowser\FOXAssetBrowser.ini" "%BUILD%\games\fox\data\FOXAssetBrowser\FOXAssetBrowser.ini"
    call :foxdict
)
if exist "%D4_EXE%" (
    call :deploy "%BUILD%\games\d4"
    call :seedini "%D4_OLD%\build\release\data\D4AssetBrowser\D4AssetBrowser.ini" "%BUILD%\games\d4\data\D4AssetBrowser\D4AssetBrowser.ini"
)

:: 5. checks
echo.
echo [5] Checks
echo.
echo -- core tests
if exist "%BUILD%\conformance\test_core_math.exe"   call :check core_math        "core::rm::selfTest" "%BUILD%\conformance\test_core_math.exe"
if exist "%BUILD%\conformance\test_glb.exe"         call :check glb_exporter     "all pass"  "%BUILD%\conformance\test_glb.exe"
if exist "%BUILD%\conformance\test_export_util.exe" call :check export_helpers   "all pass"  "%BUILD%\conformance\test_export_util.exe"
if exist "%BUILD%\conformance\test_bgindex.exe"     call :check background_index "all pass"  "%BUILD%\conformance\test_bgindex.exe"
if exist "%BUILD%\conformance\viewport_check.exe"   call :check viewport         "all pass"  "%BUILD%\conformance\viewport_check.exe"
if exist "%BUILD%\conformance\thumbnail_check.exe"  call :check thumbnails       "all pass"  "%BUILD%\conformance\thumbnail_check.exe"
if exist "%BUILD%\conformance\shell_check.exe"      call :check shell_synthetic  "shell_check: 0 failure" "%BUILD%\conformance\shell_check.exe"
if exist "%BUILD%\conformance\d4_animindex_check.exe" call :check d4_animindex   "d4_animindex_check: 0 failure" "%BUILD%\conformance\d4_animindex_check.exe"

:: crash_note_*: the exe crashes ON PURPOSE (--crash-test) and the crash note it writes - to a scratch file,
:: never yours - must say what happened: abort() / qFatal (SIGABRT, which left NO note on Windows before
:: 2026-09-25) and an access violation (0xc0000005). The exit code of those two runs is 3 by design.
:: poe2_build*: Customize runs on the CORE tab since 2026-09-23 (docs/customize/CUSTOMIZE_CORE_DESIGN.md). _build
:: assembles the Warrior ("customize: assembled Warrior"); _build_rand fills every slot from seed 7 and exports
:: the whole build ("customize: exported") - the same pieces every run on the same install.
:: poe2_socket: the Warrior's hair hangs from "Head_Attachment", a SOCKET character.ao declares (parent
:: aux_Head_attachment), not a bone - resolved from the .ao chain since 2026-09-23. poe2_dump writes the two
:: .ao files whole (plus UTF-8 copies) so every line a socket carries can be read, not just socket/parent.
if exist "%POE2_EXE%" (
    echo.
    echo -- POE2AssetBrowser on the core
    call :check    poe2_selftests  "self-test summary: 0 failure" "%POE2_EXE%" --check
    call :launch   poe2_shell "%POE2_EXE%" --quit-when-ready --show-tab "models,textures,customize,bulk"
    call :expect   poe2_boot       poe2_shell "shell: index ready" "unconfigured: first-run page shown"
    call :expect   poe2_models     poe2_shell "re:models: [1-9][0-9]* row" "unconfigured: first-run page shown"
    call :expect   poe2_textures   poe2_shell "re:textures: [1-9][0-9]* row" "unconfigured: first-run page shown"
    call :expect   poe2_customize  poe2_shell "customize: catalogue" "unconfigured: first-run page shown"
    call :expect   poe2_bulk       poe2_shell "shell: shown bulk" "unconfigured: first-run page shown"
    call :launch   poe2_build "%POE2_EXE%" --quit-when-ready --build "subject=Warrior" --shot "%OUT%\poe2_build.png" --settle 1500
    call :expect   poe2_build      poe2_build "customize: assembled Warrior" "unconfigured: first-run page shown"
    call :expect   poe2_socket     poe2_build "socket Head_Attachment" "unconfigured: first-run page shown"
    call :checkany poe2_dump       "dump: wrote metadata/characters/character.ao" "unconfigured: first-run page shown" "%POE2_EXE%" --quit-when-ready --dump-paths "metadata/characters/character.ao;metadata/characters/str/strfourb.ao" --dump-to "%OUT%\dump"
    call :checkany poe2_build_rand "customize: exported" "unconfigured: first-run page shown" "%POE2_EXE%" --quit-when-ready --build "random:7" --build-export "%OUT%\poe2_build_random.glb" --shot "%OUT%\poe2_build_random.png" --settle 1500
    call :launch   crash_abort_run "%POE2_EXE%" --crash-test abort --crash-note "%OUT%\_crash_abort.log"
    call :expect   crash_note_abort crash_abort "SIGABRT"
    call :launch   crash_segv_run "%POE2_EXE%" --crash-test segv --crash-note "%OUT%\_crash_segv.log"
    call :expect   crash_note_segv  crash_segv "0xc0000005"
) else ( echo. & echo -- POE2AssetBrowser not built )

:: fox_customize (2026-09-25): its old needle "customize: built" matched "built 0 part(s)", which FOX logs at
:: startup before any index - it proved nothing. It now needs a character built with at least one part.
:: fox_files likewise matched "files: columns", also logged at startup; it now needs the tab brought up
:: ("shell: shown files"), and fox_shot_files checks the preview itself.
:: fox_shot_*: the re-hosted harness -- --games/--model/--character through FoxPlugin, --tab/--shot/--settle
:: through the core. Needles are FOX's own log lines ("models: loaded" = core Models tab, "look applied" = FOX Customize).
:: (Comments stay OUTSIDE the block: a "::" label inside "( ... )" aborts cmd's parser mid-block, silently.)
:: fox_slotcolor (2026-09-24): Survive's per-slot gear colour was the one choice the build text never wrote,
:: so presets, outfits, .foxbuild files and undo all lost it. --build-roundtrip writes the build text, applies
:: it back and writes it again; the needle wants the two equal AND the colour in it.
:: fox_export_menu: the Customize host now hands the core its export rows (Ctrl+E / Ctrl+Shift+A found only
:: disabled core rows before). --export-menu logs every row as "<role> on|off <text>".
:: fox_shot_files searches the .fmdl by its full name: with FOX's dictionaries in place (2026-09-24) the
:: bare stem matches 27 files and the first is a texture, which has no model preview.
:: fox_slotcolor dyes the INNER slot (the one ssd_f wears by default) with the install's first solid
:: swatch ("=first"; the log lists them all). cm_scol4_c07 was "NOT matched" because it is not in this
:: install: run 12 logged scol3_c00..c21 plus only scol4_c02/c17/c19. The name was mine, not the game's.
:: fox_core_customize (2026-09-25, stage 2e-2): the CORE Customize tab on FOX's characters, beside FOX's own
:: (--core-customize adds it; harness only until 2e is done). Same character as fox_character, so the two
:: logs and shots compare: FOX logs "customize: built N part(s)", the core "customize: assembled ssd_f - ..."
:: with its piece count, and the notes say how each part was placed.
:: fox_core_weapon (2026-09-25, stage 2e-3): the ar00 receiver family on the core tab, on its own parts; each
:: seated part logs "<slot> - seated at CNP_...". Run 17 asked for a NAMED weapon, but on this install FOX's
:: EquipCatalog reads 0 weapon builds ("equip: 0 game weapon build(s)" in every FOX log), so there are none;
:: the families (37) are what this install has - the log lists them as "weapon ids, e.g. ...".
:: Run 18: ar00 has no parts of its own family here, so it built the bare receiver (0 pieces). The check now
:: fits the FIRST barrel, magazine and muzzle each list offers ("@1", the harness's positional pick), so it
:: names no part this install may lack; the muzzle then seats on the barrel if the barrel carries the point.
:: fox_core_vehicle / fox_core_other (2026-09-26, stage 2e-4): FOX's "Vehicle and buddy" and "All other characters"
:: pages on the core tab. Run 20 opened "subject=@1" of each (the harness resolves it): vehicle:amb (the
:: ambulance, 6 pieces, "Glass - seated at CNP_WIND0") and other:avm (2 pieces). The checks now want those:
:: the ambulance by name with its glass seated by the plan, and @1 of the other page with a piece fitted.
:: fox_vehicle / fox_other: the SAME two subjects on FOX's own tab (--vehicle, --character other:), so the
:: shots compare side by side, as fox_character does for fox_core_character. Run 20's core ambulance shows
:: two dark fragments above it; FOX's shot says whether FOX draws them too.
:: fox_core_wav: the wheeled armoured vehicle. Run 21: it owns a turret only (its gun is part of the turret
:: model), seated at CNP_TURRET by the plan; the check wants that seat.
:: Run 21 compared the ambulance shots: FOX's own tab draws detached pieces too (grey glass shards, a
:: hanging cord), but the two cameras differ, so the shots cannot say whether they sit in the SAME place.
:: fox_core_veh_export / fox_vehicle_export write both ambulances as .glb (the core build, FOX's scene),
:: to be compared mesh by mesh.
:: fox_core_looks (2026-09-26, stage 2e-5a): the build's Camo / Variation and Gear Color rows on the core tab,
:: with the FIRST variation and the FIRST gear colour each list offers ("@1"). The store says per piece what
:: it did: "<file>.fv2: N texture substitution(s)" and "gear colour painted N of M paintable material(s)".
:: Run 22 put them on the ambulance: mfv_camo_c00 substituted 0 and nothing on it is paintable - it is
:: the path running, not a look. The check now uses the armoured vehicle (wav); the counts are read.
:: fox_vehicle_camo: FOX's own tab on the same vehicle with camo_c00 (--camo), for its per-part word.
:: Run 22 compared the two ambulance .glb files mesh by mesh: all 24 match FOX's bounds exactly but one,
:: the unskinned broken windscreen (amb0_wb00_bre), which the core wrote collapsed onto the origin (fixed).
:: fox_vehicle_census (2026-09-26): the user chose to seat vehicle parts BEYOND FOX's plan (run 22: the
:: ambulance's glass lands 1.8 m above the roof; Wb, Hmic, Reci sit at the origin while the hull names
:: CNP_HMIC, CNP_RECI and CNP_amb0_wbNN_bre). --vehicle-census writes every vehicle's own parts (where
:: each is authored, skinned or not, FOX's plan) and every connect point, so the rule is read from all
:: 42 vehicles, not guessed from one. The needle wants it written; the rows are read.
:: Run 23 (2026-10-05): the census read the rule (design doc 2e-4). A part whose bounds lie 0.25 m or more from
:: its own origin is modelled in place and is not seated; one at its origin with no plan takes the hull's
:: CNP_<ROW>. On the ambulance: Glass not seated, Hmic on CNP_HMIC, Reci on CNP_RECI - the checks want
:: those three. The armoured vehicle's turret is modelled in place too (its CNP_TURRET is at the origin,
:: so nothing moves); fox_core_wav now wants that word.
:: fox_catalogues (2026-09-24): the colour swatches, FMTT material presets and shader table FOX's MainWindow
:: builds when the index is ready; the port had left all three out (no "layer:" line in any run before).
:: They then loaded EMPTY (0 swatches, no .fmtt, no shader pack): the exe had no name dictionaries (see
:: :foxdict). The needle is now the line FoxPlugin writes only when all three found their data.
if exist "%FOX_EXE%" (
    echo.
    echo -- FOXAssetBrowser on the core
    call :check    fox_selftests   "self-test summary: 0 failure" "%FOX_EXE%" --check
    call :launch   fox_shell "%FOX_EXE%" --quit-when-ready --show-tab "models,textures,customize,files"
    call :expect   fox_boot        fox_shell "shell: index ready" "unconfigured: first-run page shown"
    call :expect   fox_models      fox_shell "re:models: [1-9][0-9]* row" "unconfigured: first-run page shown"
    call :expect   fox_textures    fox_shell "re:textures: [1-9][0-9]* row" "unconfigured: first-run page shown"
    call :expect   fox_files       fox_shell "shell: shown files" "unconfigured: first-run page shown"
    call :expect   fox_catalogues  fox_shell "catalogues: all three loaded" "unconfigured: first-run page shown"
    call :checkany fox_shot_model     "models: decoded clip" "unconfigured: first-run page shown" "%FOX_EXE%" --quit-when-ready --games all --model bdf0_main0_def --clip 0 --tab Models --shot "%OUT%\fox_model.png" --settle 2000
    call :checkany fox_shot_files     "files: preview" "unconfigured: first-run page shown" "%FOX_EXE%" --quit-when-ready --games all --search bdf0_main0_def.fmdl --tab Files --shot "%OUT%\fox_files.png" --settle 2000
    call :launch   fox_character "%FOX_EXE%" --quit-when-ready --games all --character ssd_f --shot "%OUT%\fox_ssd_f.png" --settle 2500
    call :expect   fox_shot_character fox_character "look applied" "unconfigured: first-run page shown"
    call :expect   fox_customize      fox_character "re:customize: built [1-9][0-9]* part" "unconfigured: first-run page shown"
    call :checkany fox_slotcolor      "build roundtrip same; slotcolor fields 1" "unconfigured: first-run page shown" "%FOX_EXE%" --quit-when-ready --games all --character "ssd_f,slotcolor:inner=first" --build-roundtrip
    call :checkany fox_export_menu    "exportmenu: export.selection on" "unconfigured: first-run page shown" "%FOX_EXE%" --quit-when-ready --games all --character ssd_f --export-menu
    call :launch   fox_core_character "%FOX_EXE%" --quit-when-ready --games all --core-customize "category=character;subject=ssd_f" --shot "%OUT%\fox_core_ssd_f.png" --settle 2500
    call :expect   fox_core_customize fox_core_character "re:customize: assembled ssd_f .* [1-9][0-9]* pieces" "unconfigured: first-run page shown"
    call :launch   fox_core_weapon "%FOX_EXE%" --quit-when-ready --games all --core-customize "category=weapon;subject=family:ar00;slot.barrel=@1;slot.magazine=@1;slot.muzzle=@1" --shot "%OUT%\fox_core_weapon.png" --settle 2500
    call :expect   fox_core_weapon    fox_core_weapon "re:customize: assembled family:ar00 .* [1-9][0-9]* pieces" "unconfigured: first-run page shown"
    call :launch   fox_core_vehicle "%FOX_EXE%" --quit-when-ready --games all --core-customize "category=vehicle;subject=vehicle:amb" --core-customize-export "%OUT%\fox_core_vehicle.glb" --shot "%OUT%\fox_core_vehicle.png" --settle 2500
    call :expect   fox_core_vehicle   fox_core_vehicle "re:customize: assembled vehicle:amb .* [1-9][0-9]* pieces" "unconfigured: first-run page shown"
    call :expect   fox_core_veh_glass fox_core_vehicle "re:customize: Glass .* modelled in place" "unconfigured: first-run page shown"
    call :expect   fox_core_veh_hmic  fox_core_vehicle "re:customize: Hmic .* seated at CNP_HMIC" "unconfigured: first-run page shown"
    call :expect   fox_core_veh_reci  fox_core_vehicle "re:customize: Reci .* seated at CNP_RECI" "unconfigured: first-run page shown"
    call :expect   fox_core_veh_export fox_core_vehicle "customize: exported fox_core_vehicle.glb" "unconfigured: first-run page shown"
    call :launch   fox_core_other "%FOX_EXE%" --quit-when-ready --games all --core-customize "category=other;subject=@1" --shot "%OUT%\fox_core_other.png" --settle 2500
    call :expect   fox_core_other     fox_core_other "re:customize: assembled other:.* [1-9][0-9]* pieces" "unconfigured: first-run page shown"
    call :launch   fox_vehicle "%FOX_EXE%" --quit-when-ready --games all --vehicle amb --export "%OUT%\fox_vehicle_amb.glb" --shot "%OUT%\fox_vehicle_amb.png" --settle 2500
    call :expect   fox_vehicle        fox_vehicle "re:customize: built [1-9][0-9]* part" "unconfigured: first-run page shown"
    call :expect   fox_vehicle_export fox_vehicle "re:devshot: export scene .* ok" "unconfigured: first-run page shown"
    call :launch   fox_other "%FOX_EXE%" --quit-when-ready --games all --character other:avm --shot "%OUT%\fox_other_avm.png" --settle 2500
    call :expect   fox_other          fox_other "re:customize: built [1-9][0-9]* part" "unconfigured: first-run page shown"
    call :launch   fox_core_wav "%FOX_EXE%" --quit-when-ready --games all --core-customize "category=vehicle;subject=vehicle:wav" --shot "%OUT%\fox_core_wav.png" --settle 2500
    call :expect   fox_core_wav       fox_core_wav "re:customize: Turret .* modelled in place" "unconfigured: first-run page shown"
    call :launch   fox_core_looks "%FOX_EXE%" --quit-when-ready --games all --core-customize "category=vehicle;subject=vehicle:wav;slot.look:camo=@1;slot.look:gearcolor=@1" --shot "%OUT%\fox_core_looks.png" --settle 2500
    call :expect   fox_core_look_camo fox_core_looks "re:look: .*texture substitution" "unconfigured: first-run page shown"
    call :expect   fox_core_look_gear fox_core_looks "re:look: .*gear colour painted" "unconfigured: first-run page shown"
    call :launch   fox_vehicle_camo "%FOX_EXE%" --quit-when-ready --games all --vehicle wav --camo camo_c00 --shot "%OUT%\fox_vehicle_camo.png" --settle 2500
    call :expect   fox_vehicle_camo   fox_vehicle_camo "re:customize: built [1-9][0-9]* part" "unconfigured: first-run page shown"
    call :launch   fox_vehicle_census "%FOX_EXE%" --quit-when-ready --games all --vehicle-census "%OUT%\fox_vehicle_census.tsv"
    call :expect   fox_vehicle_census fox_vehicle_census "re:census: vehicles .* own part" "unconfigured: first-run page shown"
) else ( echo. & echo -- FOXAssetBrowser not built )

:: d4_*: phase 6a. The repro models are d4/CLAUDE.md's (canonical rig, cape, fur). Needles: "models: loaded" =
:: the core Models tab parsed and uploaded it; "image(s)" = the .glb carried decoded textures (materials resolved).
:: d4_shot_rig waits for the secondary indexes (--wait-secondary: clips come from D4's animation index) and
:: plays clip 0 — "models: decoded clip" = listed through the index and decoded against barF_base00's own rig.
:: d4_attach: demon_butcher_miniboss (BSK_original_butcher) holds a sickle and a cleaver on spawn, the cleaver on
:: HP_rightWeapon (d4data Actor JSON, msgKey 1000) - "devshot: attached" = listed from the entity index and seated
:: by D4's own seat(). The list is quoted: cmd splits an unquoted a,b into two arguments.
:: d4_attach_anim: the same with clip 0 playing. Both weapon bones logged a zero rest AND bind translation
:: (2026-09-23), so at rest the weapons sit at the origin; if the bones are animation-only prop bones
:: a clip moves them into the hands. The two shots side by side say which.
:: Answered 2026-09-23: in the hands under a clip. Since then a piece with no rest placement makes the
:: Models tab open on the game's idle (AnimKey_Neutral) at frame 0, paused - d4_attach's needle.
:: d4_multiseg: sub-objects with more than one segment were silently dropped by D4's parser (Korlic loaded 256
:: tris of 32,944). Loaded by default since 2026-09-23; the needle is Korlic's measured full count. _off sets
:: D4_PARSE_MULTISEG=0 (the diagnostic opt-out) and expects the loss to be logged; the variable is cleared after.
if exist "%D4_EXE%" (
    echo.
    echo -- D4AssetBrowser on the core
    call :check    d4_selftests   "self-test summary: 0 failure" "%D4_EXE%" --check
    call :launch   d4_rig "%D4_EXE%" --quit-when-ready --wait-secondary --model barF_base00 --clip 0 --tab Models --shot "%OUT%\d4_barF_base00.png" --settle 2000
    call :expect   d4_boot        d4_rig "shell: index ready" "unconfigured: first-run page shown"
    call :expect   d4_shot_rig    d4_rig "models: decoded clip" "unconfigured: first-run page shown"
    call :checkany d4_attach      "models: posed on the idle clip" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --wait-secondary --model demon_butcher_miniboss --attach "sickle,cleaver" --tab Models --shot "%OUT%\d4_attach.png" --settle 2000
    call :checkany d4_attach_anim "devshot: clip" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --wait-secondary --model demon_butcher_miniboss --clip 0 --attach "sickle,cleaver" --tab Models --shot "%OUT%\d4_attach_anim.png" --settle 2000
    call :checkany d4_multiseg     "32944 tris" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --model Barbarian_Ancient_Korlic --tab Models --shot "%OUT%\d4_multiseg.png" --settle 2000
    set "D4_PARSE_MULTISEG=0"
    call :checkany d4_multiseg_off "multi-segment sub-object" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --model Barbarian_Ancient_Korlic --tab Models
    set "D4_PARSE_MULTISEG="
    call :checkany d4_shot_fur    "models: loaded" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --model barF_stor151_TRS --tab Models --shot "%OUT%\d4_barF_stor151_TRS.png" --settle 2000
    call :checkany d4_export      "image(s)" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --model barF_base03_TRS --export "%OUT%\d4_barF_base03_TRS.glb"
    call :checkany d4_shot_tex    "textures: loaded" "unconfigured: first-run page shown" "%D4_EXE%" --quit-when-ready --texture barM_P00_BOD_normal --tab Textures --shot "%OUT%\d4_texture.png" --settle 1500
) else ( echo. & echo -- D4AssetBrowser not built )

echo.
echo ============================================================
echo  %PASS% passed, %FAIL% failed.   Results: %OUT%\_results.txt
echo  Launch:  %POE2_EXE%
echo           %FOX_EXE%
echo           %D4_EXE%
echo  One thing to look at by eye: POE2 Customize should open on the
echo  character's FACE. If it shows the back, tell me - one sign flips it.
echo ============================================================
>> "%LOG%" echo %PASS% passed, %FAIL% failed
%PAUSE%
exit /b %FAIL%

:: fixmoved <build dir> <source dir> - a project folder that was MOVED (2026-10-05: ...\Claude Current to
:: G:\G Claude Projects\AssetBrowser) keeps a CMakeCache.txt naming the old place, and CMake refuses to
:: configure there. The stale CMakeCache.txt and CMakeFiles are RENAMED aside (*.moved_<time>; nothing is
:: deleted) so the next configure starts fresh; vcpkg_installed stays. Silent when nothing moved.
:fixmoved
powershell -NoProfile -ExecutionPolicy Bypass -Command "$b='%~1'; $c=Join-Path $b 'CMakeCache.txt'; if (Test-Path -LiteralPath $c) { $m=Select-String -LiteralPath $c -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$'; $h=if ($m) { $m.Matches[0].Groups[1].Value } else { '' }; $s=(Resolve-Path -LiteralPath '%~2').Path; if ((($h -replace '\\','/').TrimEnd('/')) -ne (($s -replace '\\','/').TrimEnd('/'))) { $t=Get-Date -Format 'yyyyMMdd_HHmmss'; Rename-Item -LiteralPath $c -NewName ('CMakeCache.txt.moved_'+$t); $f=Join-Path $b 'CMakeFiles'; if (Test-Path -LiteralPath $f) { Rename-Item -LiteralPath $f -NewName ('CMakeFiles.moved_'+$t) }; Write-Output ('  build folder was configured for ' + $h + ' - stale cache renamed aside, configuring fresh') } }"
exit /b 0
