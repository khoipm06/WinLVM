#ifndef WINLVM_GUI_RESOURCE_H
#define WINLVM_GUI_RESOURCE_H

#define IDI_WINLVM_APP 101

// Child control IDs
#define IDC_TOOLBAR    1001
#define IDC_TREEVIEW   1002
#define IDC_LISTVIEW   1003
#define IDC_STATUSBAR  1004

// Commands
#define IDM_FILE_RESCAN        2001
#define IDM_FILE_ELEVATE       2002
#define IDM_FILE_HIDE_TRAY     2003
#define IDM_FILE_EXIT          2004

#define IDM_VOL_ACTIVATE       2101
#define IDM_VOL_DEACTIVATE     2102
#define IDM_VOL_DRIVE_LETTER   2103
#define IDM_VOL_TOGGLE_RO      2104
#define IDM_VOL_OPEN_EXPLORER  2105
#define IDM_VOL_CREATE_PV      2106
#define IDM_VOL_CREATE_VG      2107
#define IDM_VOL_CREATE_LV      2108
#define IDM_VOL_EXTEND_LV      2109
#define IDM_VOL_RENAME_LV      2110
#define IDM_VOL_DELETE_LV      2111

#define IDM_VIEW_REFRESH       2201
#define IDM_VIEW_SHOW_INTERNAL 2202

// Tray menu commands
#define IDM_TRAY_OPEN_MANAGER  3001
#define IDM_TRAY_RESCAN        3002
#define IDM_TRAY_EXIT          3003
#define IDM_TRAY_LV_OPEN_BASE  3100
#define IDM_TRAY_LV_ACT_BASE   3200
#define IDM_TRAY_LV_DEACT_BASE 3300

// Custom window messages & timers
#define WM_APP_TRAY_ICON       (WM_APP + 1)
#define WM_APP_FORCE_EXIT      (WM_APP + 2)
#define IDT_POLL_REFRESH       101
#define IDT_DEBOUNCE_RESCAN    102

#endif
