@echo off
setlocal enabledelayedexpansion

echo === RUNNING Stage 2 MVP SWEEP === > C:\winlvm\stage2_exec.log

echo --- STEP 1: EXTRACT SOURCE --- >> C:\winlvm\stage2_exec.log
cd /d C:\winlvm
tar -xzf C:\winlvm\winlvm_src.tgz >> C:\winlvm\stage2_exec.log 2>&1
if errorlevel 1 (
    echo EXTRACT-FAIL >> C:\winlvm\stage2_exec.log
    goto end
)
echo EXTRACT-PASS >> C:\winlvm\stage2_exec.log

echo --- STEP 2: BUILD DRIVER --- >> C:\winlvm\stage2_exec.log
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" C:\winlvm\driver\winlvm.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /v:m /nologo >> C:\winlvm\stage2_exec.log 2>&1
if errorlevel 1 (
    echo BUILD-FAIL >> C:\winlvm\stage2_exec.log
    goto end
)
echo BUILD-PASS >> C:\winlvm\stage2_exec.log

echo --- STEP 3: RUN VERIFICATION SCRIPT --- >> C:\winlvm\stage2_exec.log
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\winlvm\test_linear_mvp.ps1 >> C:\winlvm\stage2_exec.log 2>&1
if errorlevel 1 (
    echo TEST-SCRIPT-RC-FAIL >> C:\winlvm\stage2_exec.log
) else (
    echo TEST-SCRIPT-RC-PASS >> C:\winlvm\stage2_exec.log
)

:end
echo === FINISHED Stage 2 MVP SWEEP === >> C:\winlvm\stage2_exec.log
type C:\winlvm\stage2_exec.log
if exist C:\winlvm\linear_mvp.log type C:\winlvm\linear_mvp.log
