@echo off
rem Re-patch + rebuild XenonRecomp, regenerate the C++ into build\ppc_gen, then copy
rem only changed files into ppc\ so ninja rebuilds just those.
rem "recomp.cmd tu6" does the same for Title Update 6 (config\DAO_tu6.toml, needs
rem title_update\default.xexp): build\ppc_tu6_gen, names prefixed with tu6_, into ppc_tu6\.
setlocal
cd /d "%~dp0.."
set CONFIG=DAO
set OUT=ppc
set LOG=recomp_log
if /i "%~1"=="tu6" (
    set CONFIG=DAO_tu6
    set OUT=ppc_tu6
    set LOG=recomp_log_tu6
)
python -I scripts\patch_recompiler.py tools\XenonRecomp\XenonRecomp\recompiler.cpp || exit /b 1
call scripts\env.cmd ninja -C tools\xexdump\build XenonRecomp || exit /b 1
if exist build\%OUT%_gen rmdir /s /q build\%OUT%_gen
mkdir build\%OUT%_gen
rem XenonRecomp writes to out_directory_path from the TOML; point a temp copy at the _gen folder.
powershell -NoProfile -Command "(Get-Content config\%CONFIG%.toml) -replace '^out_directory_path = .*', 'out_directory_path = \"../build/%OUT%_gen\"' | Set-Content config\%CONFIG%.gen.toml"
tools\xexdump\build\XenonRecomp\XenonRecomp\XenonRecomp.exe config\%CONFIG%.gen.toml tools\XenonRecomp\XenonUtils\ppc_context.h > analysis\%LOG%.txt 2>&1
del config\%CONFIG%.gen.toml
findstr /v /c:"Recompiling functions" analysis\%LOG%.txt
if /i "%~1"=="tu6" python -I scripts\prefix_symbols.py build\%OUT%_gen tu6 || exit /b 1
python -I scripts\sync_dir.py build\%OUT%_gen %OUT%
exit /b 0
