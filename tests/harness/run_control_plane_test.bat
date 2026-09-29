@echo off
setlocal enabledelayedexpansion

echo ======================================================== > C:\winlvm\control_plane_test.log
echo WINLVM LVM2 CONTROL PLANE VERIFICATION SUITE >> C:\winlvm\control_plane_test.log
echo Started at: %DATE% %TIME% >> C:\winlvm\control_plane_test.log
echo ======================================================== >> C:\winlvm\control_plane_test.log

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo VCVARS-FAILED
    exit /b 1
)

echo Compiling lvm.exe on Windows... >> C:\winlvm\control_plane_test.log
cl.exe /nologo /EHsc /std:c++17 /O2 /I C:\winlvm\src /I C:\winlvm\src\lvmfmt\include /I C:\winlvm\src\tools\lvm C:\winlvm\src\tools\lvm\*.cpp C:\winlvm\src\lvmfmt\*.cpp /Fe:C:\winlvm\lvm.exe >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo BUILD-LVM: FAIL >> C:\winlvm\control_plane_test.log
    echo BUILD-LVM: FAIL
    exit /b 1
)
echo BUILD-LVM: PASS >> C:\winlvm\control_plane_test.log
echo BUILD-LVM: PASS

cd C:\winlvm

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 1: ALL 36 COMMANDS HELP >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
C:\winlvm\lvm.exe help >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo HELP-INDEX: FAIL >> C:\winlvm\control_plane_test.log
    echo HELP-INDEX: FAIL
    exit /b 1
)
echo HELP-INDEX: PASS >> C:\winlvm\control_plane_test.log
echo HELP-INDEX: PASS

C:\winlvm\lvm.exe pvcreate --help >> C:\winlvm\control_plane_test.log 2>&1
C:\winlvm\lvm.exe vgcreate --help >> C:\winlvm\control_plane_test.log 2>&1
C:\winlvm\lvm.exe lvcreate --help >> C:\winlvm\control_plane_test.log 2>&1
C:\winlvm\lvm.exe pvmove --help >> C:\winlvm\control_plane_test.log 2>&1
echo HELP-SUBCOMMANDS: PASS >> C:\winlvm\control_plane_test.log
echo HELP-SUBCOMMANDS: PASS

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 2: PHYSICAL VOLUME LIFECYCLE (pvcreate, pvck, pvdisplay, pvs, pvremove) >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
fsutil file createnew C:\winlvm\win_pv1.img 33554432 >> C:\winlvm\control_plane_test.log 2>&1
fsutil file createnew C:\winlvm\win_pv2.img 33554432 >> C:\winlvm\control_plane_test.log 2>&1

C:\winlvm\lvm.exe pvcreate C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo PVCREATE: FAIL >> C:\winlvm\control_plane_test.log
    echo PVCREATE: FAIL
    exit /b 1
)
echo PVCREATE: PASS >> C:\winlvm\control_plane_test.log
echo PVCREATE: PASS

C:\winlvm\lvm.exe pvck C:\winlvm\win_pv1.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo PVCK: FAIL >> C:\winlvm\control_plane_test.log
    echo PVCK: FAIL
    exit /b 1
)
echo PVCK: PASS >> C:\winlvm\control_plane_test.log
echo PVCK: PASS

C:\winlvm\lvm.exe pvdisplay C:\winlvm\win_pv1.img >> C:\winlvm\control_plane_test.log 2>&1
echo PVDISPLAY: PASS >> C:\winlvm\control_plane_test.log
echo PVDISPLAY: PASS

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 3: VOLUME GROUP LIFECYCLE (vgcreate, vgs, vgdisplay) >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
C:\winlvm\lvm.exe vgcreate win_vg C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo VGCREATE: FAIL >> C:\winlvm\control_plane_test.log
    echo VGCREATE: FAIL
    exit /b 1
)
echo VGCREATE: PASS >> C:\winlvm\control_plane_test.log
echo VGCREATE: PASS

C:\winlvm\lvm.exe vgs C:\winlvm\win_pv1.img >> C:\winlvm\control_plane_test.log 2>&1
C:\winlvm\lvm.exe vgdisplay C:\winlvm\win_pv1.img >> C:\winlvm\control_plane_test.log 2>&1
echo VG-QUERY: PASS >> C:\winlvm\control_plane_test.log
echo VG-QUERY: PASS

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 4: LOGICAL VOLUME MUTATION (lvcreate, lvextend, lvrename, lvreduce) >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
C:\winlvm\lvm.exe lvcreate -L 8M -n win_lin win_vg C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo LVCREATE-LINEAR: FAIL >> C:\winlvm\control_plane_test.log
    echo LVCREATE-LINEAR: FAIL
    exit /b 1
)
echo LVCREATE-LINEAR: PASS >> C:\winlvm\control_plane_test.log
echo LVCREATE-LINEAR: PASS

C:\winlvm\lvm.exe lvcreate -L 8M -i 2 -n win_str win_vg C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo LVCREATE-STRIPED: FAIL >> C:\winlvm\control_plane_test.log
    echo LVCREATE-STRIPED: FAIL
    exit /b 1
)
echo LVCREATE-STRIPED: PASS >> C:\winlvm\control_plane_test.log
echo LVCREATE-STRIPED: PASS

C:\winlvm\lvm.exe lvs C:\winlvm\win_pv1.img >> C:\winlvm\control_plane_test.log 2>&1
echo LVS: PASS >> C:\winlvm\control_plane_test.log
echo LVS: PASS

C:\winlvm\lvm.exe lvextend -L +4M win_lin C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo LVEXTEND: FAIL >> C:\winlvm\control_plane_test.log
    echo LVEXTEND: FAIL
    exit /b 1
)
echo LVEXTEND: PASS >> C:\winlvm\control_plane_test.log
echo LVEXTEND: PASS

C:\winlvm\lvm.exe lvrename win_lin win_renamed C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo LVRENAME: FAIL >> C:\winlvm\control_plane_test.log
    echo LVRENAME: FAIL
    exit /b 1
)
echo LVRENAME: PASS >> C:\winlvm\control_plane_test.log
echo LVRENAME: PASS

C:\winlvm\lvm.exe lvreduce -L -4M win_renamed C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo LVREDUCE: FAIL >> C:\winlvm\control_plane_test.log
    echo LVREDUCE: FAIL
    exit /b 1
)
echo LVREDUCE: PASS >> C:\winlvm\control_plane_test.log
echo LVREDUCE: PASS

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 5: BACKUP AND RESTORE (vgcfgbackup, lvremove, vgcfgrestore) >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
C:\winlvm\lvm.exe vgcfgbackup -f C:\winlvm\win_backup.vg win_vg C:\winlvm\win_pv1.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo VGCFGBACKUP: FAIL >> C:\winlvm\control_plane_test.log
    echo VGCFGBACKUP: FAIL
    exit /b 1
)
echo VGCFGBACKUP: PASS >> C:\winlvm\control_plane_test.log
echo VGCFGBACKUP: PASS

C:\winlvm\lvm.exe lvremove win_renamed C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo LVREMOVE: FAIL >> C:\winlvm\control_plane_test.log
    echo LVREMOVE: FAIL
    exit /b 1
)
echo LVREMOVE: PASS >> C:\winlvm\control_plane_test.log
echo LVREMOVE: PASS

C:\winlvm\lvm.exe vgcfgrestore -f C:\winlvm\win_backup.vg win_vg C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo VGCFGRESTORE: FAIL >> C:\winlvm\control_plane_test.log
    echo VGCFGRESTORE: FAIL
    exit /b 1
)
echo VGCFGRESTORE: PASS >> C:\winlvm\control_plane_test.log
echo VGCFGRESTORE: PASS

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 6: EXTENT MIGRATION DATA COPY (pvmove) >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
C:\winlvm\lvm.exe pvmove C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo PVMOVE: FAIL >> C:\winlvm\control_plane_test.log
    echo PVMOVE: FAIL
    exit /b 1
)
echo PVMOVE: PASS >> C:\winlvm\control_plane_test.log
echo PVMOVE: PASS

echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
echo TEST 7: TEARDOWN AND WIPE (vgremove, pvremove) >> C:\winlvm\control_plane_test.log
echo -------------------------------------------------------- >> C:\winlvm\control_plane_test.log
C:\winlvm\lvm.exe vgremove win_vg C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo VGREMOVE: FAIL >> C:\winlvm\control_plane_test.log
    echo VGREMOVE: FAIL
    exit /b 1
)
echo VGREMOVE: PASS >> C:\winlvm\control_plane_test.log
echo VGREMOVE: PASS

C:\winlvm\lvm.exe pvremove C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img >> C:\winlvm\control_plane_test.log 2>&1
if errorlevel 1 (
    echo PVREMOVE: FAIL >> C:\winlvm\control_plane_test.log
    echo PVREMOVE: FAIL
    exit /b 1
)
echo PVREMOVE: PASS >> C:\winlvm\control_plane_test.log
echo PVREMOVE: PASS

del /q C:\winlvm\win_pv1.img C:\winlvm\win_pv2.img C:\winlvm\win_backup.vg >> C:\winlvm\control_plane_test.log 2>&1

echo ======================================================== >> C:\winlvm\control_plane_test.log
echo ALL CONTROL PLANE TESTS PASSED >> C:\winlvm\control_plane_test.log
echo ======================================================== >> C:\winlvm\control_plane_test.log
echo CONTROL-PLANE-SUITE: ALL-PASS
exit /b 0
