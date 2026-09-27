@echo off
rem Builds build/port/repair/repair_gr2.exe (x86, MSVC Build Tools 2022) from this folder.
rem The MSVC x86 environment: M2W_VCVARS32 (set by tools/repair_gr2.py from
rem [tools] vcvars32 in webclient.toml), else the Build Tools 2022 default.
if "%M2W_VCVARS32%"=="" set "M2W_VCVARS32=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
call "%M2W_VCVARS32%" >nul
if not exist "%~dp0..\..\build\port\repair" mkdir "%~dp0..\..\build\port\repair"
cd /d "%~dp0..\..\build\port\repair"
cl /nologo /EHsc /O2 "%~dp0repair_gr2.cpp" /link /OUT:"%~dp0..\..\build\port\repair\repair_gr2.exe"
