@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
cl /nologo /O1 /EHsc harness.cpp /Fe:harness_DX11.exe /link /BASE:0x10000 /FIXED /DYNAMICBASE:NO
