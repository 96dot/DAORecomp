@echo off
rem Sets up MSVC headers/libs + portable LLVM, then runs the given command.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%~dp0..\tools\llvm\bin;%PATH%"
%*
