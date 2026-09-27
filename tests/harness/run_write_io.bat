@echo off
setlocal enabledelayedexpansion

echo === RUNNING WRITE I/O TEST HARNESS === > C:\winlvm\write_exec.log
echo Start Time: %DATE% %TIME% >> C:\winlvm\write_exec.log
echo Command line: %0 %* >> C:\winlvm\write_exec.log

cd /d C:\winlvm

REM Check if a newly deployed source archive is present to rebuild the driver
if exist C:\winlvm\winlvm_src.tgz (
    echo --- STEP 1: EXTRACT SOURCE --- >> C:\winlvm\write_exec.log
    tar -xzf C:\winlvm\winlvm_src.tgz >> C:\winlvm\write_exec.log 2>&1
    if errorlevel 1 (
        echo EXTRACT-FAIL >> C:\winlvm\write_exec.log
        set TEST_RC=1
        goto finish
    )
    echo EXTRACT-PASS >> C:\winlvm\write_exec.log
    del /f /q C:\winlvm\winlvm_src.tgz >> C:\winlvm\write_exec.log 2>&1

    echo --- STEP 2: BUILD DRIVER --- >> C:\winlvm\write_exec.log
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" C:\winlvm\driver\winlvm.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:m /nologo >> C:\winlvm\write_exec.log 2>&1
    if errorlevel 1 (
        echo BUILD-FAIL >> C:\winlvm\write_exec.log
        set TEST_RC=2
        goto finish
    )
    echo BUILD-PASS >> C:\winlvm\write_exec.log
)

echo --- STEP: EXECUTE WRITE I/O TEST SCRIPT --- >> C:\winlvm\write_exec.log
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\winlvm\test_write_io.ps1 %* >> C:\winlvm\write_exec.log 2>&1
set TEST_RC=%ERRORLEVEL%

if %TEST_RC% equ 0 (
    echo TEST-SCRIPT-RC-PASS >> C:\winlvm\write_exec.log
) else (
    echo TEST-SCRIPT-RC-FAIL (RC=%TEST_RC%) >> C:\winlvm\write_exec.log
)

:finish
echo Finish Time: %DATE% %TIME% >> C:\winlvm\write_exec.log
echo === FINISHED WRITE I/O TEST HARNESS === >> C:\winlvm\write_exec.log
type C:\winlvm\write_exec.log
if exist C:\winlvm\write_test.log (
    echo.
    echo --- WRITE_TEST.LOG DUMP ---
    type C:\winlvm\write_test.log
)
exit /b %TEST_RC%
