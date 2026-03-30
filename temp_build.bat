@echo off
setlocal
set  CL=/DENGINE_WORLD_TESTS=1
call C:\\VS2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat -host_arch=x64 -arch=x64
cmake --build build --config RelWithDebInfo --target engine_tests

