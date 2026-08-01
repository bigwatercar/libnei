@echo off
REM win_full_build.cmd — Complete Windows 4-quadrant build & test
set SRC=c:\Personal\Projects\LibNei\libnei-src
set LOG=%SRC%\build\win_full_result.txt
echo === Windows 4-Quadrant Build ^& Test === > %LOG%
echo Started: %DATE% %TIME% >> %LOG%
echo. >> %LOG%

REM === Q1: Debug Shared ===
echo [1/8] Configuring win-debug-shared... >> %LOG%
cmake -S %SRC% -B %SRC%\build\win-debug-shared -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=ON -DNEI_BUILD_TESTS=ON >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-debug-shared CONFIGURE >> %LOG% & goto :q2)
echo [2/8] Building win-debug-shared... >> %LOG%
cmake --build %SRC%\build\win-debug-shared --config Debug -j %NUMBER_OF_PROCESSORS% >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-debug-shared BUILD >> %LOG% & goto :q2)
echo [3/8] Testing win-debug-shared... >> %LOG%
cd /d %SRC%\build\win-debug-shared
ctest --output-on-failure -C Debug >> %LOG% 2>&1
if %ERRORLEVEL% EQU 0 (echo PASS: win-debug-shared >> %LOG%) else (echo FAIL: win-debug-shared TESTS >> %LOG%)
cd /d %SRC%

:q2
REM === Q2: Debug Static ===
echo [4/8] Configuring win-debug-static... >> %LOG%
cmake -S %SRC% -B %SRC%\build\win-debug-static -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF -DNEI_BUILD_TESTS=ON >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-debug-static CONFIGURE >> %LOG% & goto :q3)
echo [5/8] Building win-debug-static... >> %LOG%
cmake --build %SRC%\build\win-debug-static --config Debug -j %NUMBER_OF_PROCESSORS% >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-debug-static BUILD >> %LOG% & goto :q3)
echo [6/8] Testing win-debug-static... >> %LOG%
cd /d %SRC%\build\win-debug-static
ctest --output-on-failure -C Debug >> %LOG% 2>&1
if %ERRORLEVEL% EQU 0 (echo PASS: win-debug-static >> %LOG%) else (echo FAIL: win-debug-static TESTS >> %LOG%)
cd /d %SRC%

:q3
REM === Q3: Release Shared ===
echo [7/8] Configuring win-rel-shared... >> %LOG%
cmake -S %SRC% -B %SRC%\build\win-rel-shared -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DNEI_BUILD_TESTS=ON >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-rel-shared CONFIGURE >> %LOG% & goto :q4)
echo [8/8] Building win-rel-shared... >> %LOG%
cmake --build %SRC%\build\win-rel-shared --config Release -j %NUMBER_OF_PROCESSORS% >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-rel-shared BUILD >> %LOG% & goto :q4)
echo [9/8] Testing win-rel-shared... >> %LOG%
cd /d %SRC%\build\win-rel-shared
ctest --output-on-failure -C Release >> %LOG% 2>&1
if %ERRORLEVEL% EQU 0 (echo PASS: win-rel-shared >> %LOG%) else (echo FAIL: win-rel-shared TESTS >> %LOG%)
cd /d %SRC%

:q4
REM === Q4: Release Static ===
echo [10/8] Configuring win-rel-static... >> %LOG%
cmake -S %SRC% -B %SRC%\build\win-rel-static -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DNEI_BUILD_TESTS=ON >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-rel-static CONFIGURE >> %LOG% & goto :done)
echo [11/8] Building win-rel-static... >> %LOG%
cmake --build %SRC%\build\win-rel-static --config Release -j %NUMBER_OF_PROCESSORS% >> %LOG% 2>&1
if %ERRORLEVEL% NEQ 0 (echo FAIL: win-rel-static BUILD >> %LOG% & goto :done)
echo [12/8] Testing win-rel-static... >> %LOG%
cd /d %SRC%\build\win-rel-static
ctest --output-on-failure -C Release >> %LOG% 2>&1
if %ERRORLEVEL% EQU 0 (echo PASS: win-rel-static >> %LOG%) else (echo FAIL: win-rel-static TESTS >> %LOG%)
cd /d %SRC%

:done
echo. >> %LOG%
echo === Done: %DATE% %TIME% === >> %LOG%
echo === Final Summary === >> %LOG%
findstr /C:"PASS:" /C:"FAIL:" %LOG% >> %LOG%
echo. >> %LOG%
type %LOG%
