@echo off
rem Re-patch + rebuild XenonRecomp, regenerate the C++ into build\ppc_gen, then copy
rem only changed files into ppc\ so ninja rebuilds just those.
setlocal
cd /d "%~dp0.."
python -I scripts\patch_recompiler.py tools\XenonRecomp\XenonRecomp\recompiler.cpp || exit /b 1
call scripts\env.cmd ninja -C tools\xexdump\build XenonRecomp || exit /b 1
if exist build\ppc_gen rmdir /s /q build\ppc_gen
mkdir build\ppc_gen
rem XenonRecomp writes to out_directory_path from the TOML; point a temp copy at ppc_gen.
powershell -NoProfile -Command "(Get-Content config\DAO.toml) -replace '^out_directory_path = .*', 'out_directory_path = \"../build/ppc_gen\"' | Set-Content config\DAO.gen.toml"
tools\xexdump\build\XenonRecomp\XenonRecomp\XenonRecomp.exe config\DAO.gen.toml tools\XenonRecomp\XenonUtils\ppc_context.h > analysis\recomp_log.txt 2>&1
del config\DAO.gen.toml
findstr /v /c:"Recompiling functions" analysis\recomp_log.txt
python -I scripts\sync_dir.py build\ppc_gen ppc
exit /b 0
