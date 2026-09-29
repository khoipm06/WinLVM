@echo off
setlocal
echo === RUNNING BTRFS TEST === > C:\winlvm\test_btrfs_batch.log
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\winlvm\test_btrfs.ps1 %* >> C:\winlvm\test_btrfs_batch.log 2>&1
set EXIT_CODE=%ERRORLEVEL%
echo Exit Code: %EXIT_CODE% >> C:\winlvm\test_btrfs_batch.log
type C:\winlvm\test_btrfs_batch.log
exit /b %EXIT_CODE%
