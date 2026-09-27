@echo off
setlocal enabledelayedexpansion

echo === RUNNING MULTI-PV TEST HARNESS === > C:\winlvm\multi_pv_exec.log
echo Start Time: %DATE% %TIME% >> C:\winlvm\multi_pv_exec.log
echo Command line: %0 %* >> C:\winlvm\multi_pv_exec.log

cd /d C:\winlvm

REM Check if a newly deployed source archive is present to rebuild the driver
if exist C:\winlvm\winlvm_src.tgz (
    echo --- STEP 1: EXTRACT SOURCE --- >> C:\winlvm\multi_pv_exec.log
    tar -xzf C:\winlvm\winlvm_src.tgz >> C:\winlvm\multi_pv_exec.log 2>&1
    if errorlevel 1 (
        echo EXTRACT-FAIL >> C:\winlvm\multi_pv_exec.log
        set TEST_RC=1
        goto finish
    )
    echo EXTRACT-PASS >> C:\winlvm\multi_pv_exec.log
    del /f /q C:\winlvm\winlvm_src.tgz >> C:\winlvm\multi_pv_exec.log 2>&1

    echo --- STEP 2: BUILD DRIVER --- >> C:\winlvm\multi_pv_exec.log
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" C:\winlvm\driver\winlvm.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:m /nologo >> C:\winlvm\multi_pv_exec.log 2>&1
    if errorlevel 1 (
        echo BUILD-FAIL >> C:\winlvm\multi_pv_exec.log
        set TEST_RC=2
        goto finish
    )
    echo BUILD-PASS >> C:\winlvm\multi_pv_exec.log
)

echo --- STEP: EXECUTE MULTI-PV TEST SCRIPT --- >> C:\winlvm\multi_pv_exec.log
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\winlvm\test_multi_pv.ps1 %* >> C:\winlvm\multi_pv_exec.log 2>&1
set TEST_RC=%ERRORLEVEL%

if %TEST_RC% equ 0 (
    echo TEST-SCRIPT-RC-PASS >> C:\winlvm\multi_pv_exec.log
) else (
    echo TEST-SCRIPT-RC-FAIL (RC=%TEST_RC%) >> C:\winlvm\multi_pv_exec.log
)

:finish
echo Finish Time: %DATE% %TIME% >> C:\winlvm\multi_pv_exec.log
echo === FINISHED MULTI-PV TEST HARNESS === >> C:\winlvm\multi_pv_exec.log
type C:\winlvm\multi_pv_exec.log
if exist C:\winlvm\multi_pv_test.log (
    echo.
    echo --- MULTI_PV_TEST.LOG DUMP ---
    type C:\winlvm\multi_pv_test.log
)
exit /b %TEST_RC%
