@echo off
rem Builds both plugins into out\  (run from this folder).  Usage: build.bat
rem   out\SR3Population.asi   <- population\*.cpp
rem   out\SR3PopCounter.asi   <- counter\*.cpp
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if not exist out mkdir out
if not exist out\obj\pop mkdir out\obj\pop
if not exist out\obj\cnt mkdir out\obj\cnt
cl /nologo /O2 /LD /EHsc /Fo:out\obj\pop\ /Fe:out\SR3Population.asi population\*.cpp user32.lib kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /LD /EHsc /Fo:out\obj\cnt\ /Fe:out\SR3PopCounter.asi counter\*.cpp user32.lib gdi32.lib kernel32.lib
if errorlevel 1 exit /b 1
echo built: out\SR3Population.asi  out\SR3PopCounter.asi
