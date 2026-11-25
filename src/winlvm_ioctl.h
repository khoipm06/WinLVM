/*
 * winlvm_ioctl.h — Canonical user/kernel control-plane ABI for WinLVM.
 *
 * Shared single source of truth included by winlvm.sys (kernel mode),
 * user-mode Win32 tools, and Linux host builds/tests.
 */
#ifndef WINLVM_IOCTL_H
#define WINLVM_IOCTL_H

#if defined(_WIN32) && !defined(_KERNEL_MODE) && !defined(HOST_WDM_TYPES_H)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <stddef.h>
#include <stdint.h>
typedef WCHAR WINLVM_WCHAR;
#elif defined(_KERNEL_MODE)
/* Kernel callers must include <ntifs.h> / <ntddk.h> / <wdm.h> before this header. */
#include <stddef.h>
#if defined(_MSC_VER)
#ifndef WINLVM_KM_STDINT_DEFINED
#define WINLVM_KM_STDINT_DEFINED
typedef signed __int8    int8_t;
typedef signed __int16   int16_t;
typedef signed __int32   int32_t;
typedef signed __int64   int64_t;
typedef unsigned __int8  uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
#ifndef _UINTPTR_T_DEFINED
#define _UINTPTR_T_DEFINED
#if defined(_WIN64)
typedef unsigned __int64 uintptr_t;
#else
typedef unsigned __int32 uintptr_t;
#endif
#endif
#ifndef _INTPTR_T_DEFINED
#define _INTPTR_T_DEFINED
#if defined(_WIN64)
typedef signed __int64   intptr_t;
#else
typedef signed __int32   intptr_t;
#endif
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffu
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffui64
#endif
#ifndef SIZE_MAX
#if defined(_WIN64)
#define SIZE_MAX   0xffffffffffffffffui64
#else
#define SIZE_MAX   0xffffffffu
#endif
#endif
#endif /* WINLVM_KM_STDINT_DEFINED */
#if !defined(__cplusplus) && !defined(__bool_true_false_are_defined) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define __bool_true_false_are_defined 1
#if _MSC_VER >= 1800
#define bool _Bool
#else
typedef unsigned char bool;
#endif
#define true  1
#define false 0
#endif
#else
#include <stdint.h>
#endif
typedef WCHAR WINLVM_WCHAR;
#else
#include <stddef.h>
#include <stdint.h>

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif
#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022u
#endif
#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0u
#endif
#ifndef FILE_READ_DATA
#define FILE_READ_DATA 0x0001u
#endif
#ifndef FILE_WRITE_DATA
#define FILE_WRITE_DATA 0x0002u
#endif

#ifndef HOST_WDM_TYPES_H
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef char     CHAR;
#endif
/* Fixed 16-bit UTF-16 code unit so wire layouts match 64-bit Windows on LP64 hosts. */
typedef uint16_t WINLVM_WCHAR;
#endif

#define WINLVM_CONTROL_DEVICE_NAME L"\\Device\\WinLvmControl"
#define WINLVM_CONTROL_DOS_NAME    L"\\DosDevices\\WinLvmControl"
#define WINLVM_USER_DEVICE_NAME    L"\\\\.\\WinLvmControl"

#define WINLVM_ABI_VERSION      1u
#define WINLVM_LV_UUID_WIRE_LEN 40
#define WINLVM_NAME_WIRE_LEN    64
#define WINLVM_PATH_WIRE_LEN    256

#define IOCTL_WINLVM_INVENTORY      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_REFRESH        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_QUIESCE_VG     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_RESUME_VG      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_ACTIVATE_LV    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_DEACTIVATE_LV  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_UPDATE_LV      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_WINLVM_GET_LOG        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_WINLVM_PROBE_DEVICE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define WINLVM_INV_LV_ACTIVE   0x1u
#define WINLVM_INV_LV_READONLY 0x2u
#define WINLVM_INV_LV_STRIPED  0x4u

#pragma pack(push, 8)

typedef struct _WINLVM_CONTROL_REQUEST {
    ULONG version;
    ULONG size;
} WINLVM_CONTROL_REQUEST, *PWINLVM_CONTROL_REQUEST;

typedef struct _WINLVM_LV_REQUEST {
    WINLVM_CONTROL_REQUEST header;
    CHAR lv_uuid[WINLVM_LV_UUID_WIRE_LEN];
} WINLVM_LV_REQUEST, *PWINLVM_LV_REQUEST;

typedef struct _WINLVM_VG_REQUEST {
    WINLVM_CONTROL_REQUEST header;
    CHAR vg_name[WINLVM_NAME_WIRE_LEN];
} WINLVM_VG_REQUEST, *PWINLVM_VG_REQUEST;

typedef struct _WINLVM_UPDATE_LV_REQUEST {
    WINLVM_CONTROL_REQUEST header;
    CHAR lv_uuid[WINLVM_LV_UUID_WIRE_LEN];
    ULONGLONG byte_size;
    ULONG logical_sector;
    ULONG physical_sector;
} WINLVM_UPDATE_LV_REQUEST, *PWINLVM_UPDATE_LV_REQUEST;

typedef struct _WINLVM_PROBE_DEVICE_REQUEST {
    WINLVM_CONTROL_REQUEST header;
    WINLVM_WCHAR device_path[WINLVM_PATH_WIRE_LEN];
} WINLVM_PROBE_DEVICE_REQUEST, *PWINLVM_PROBE_DEVICE_REQUEST;

typedef struct _WINLVM_INVENTORY_LV_ENTRY {
    CHAR vg_name[WINLVM_NAME_WIRE_LEN];
    CHAR lv_name[WINLVM_NAME_WIRE_LEN];
    CHAR lv_uuid[WINLVM_LV_UUID_WIRE_LEN];
    ULONGLONG byte_size;
    ULONG logical_sector;
    ULONG flags;
    WINLVM_WCHAR drive_letter;
    WINLVM_WCHAR reserved[3];
} WINLVM_INVENTORY_LV_ENTRY, *PWINLVM_INVENTORY_LV_ENTRY;

typedef struct _WINLVM_INVENTORY {
    ULONG version;
    ULONG size;
    ULONG record_count;
    ULONG lv_entry_count;
} WINLVM_INVENTORY, *PWINLVM_INVENTORY;

#pragma pack(pop)

#if defined(__cplusplus)
#define WINLVM_ABI_ASSERT(cond, name) static_assert((cond), #name)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define WINLVM_ABI_ASSERT(cond, name) _Static_assert((cond), #name)
#else
#define WINLVM_ABI_ASSERT(cond, name) typedef char winlvm_abi_assert_##name[(cond) ? 1 : -1]
#endif

WINLVM_ABI_ASSERT(sizeof(WINLVM_CONTROL_REQUEST) == 8, control_request_size);
WINLVM_ABI_ASSERT(sizeof(WINLVM_LV_REQUEST) == 48, lv_request_size);
WINLVM_ABI_ASSERT(sizeof(WINLVM_VG_REQUEST) == 72, vg_request_size);
WINLVM_ABI_ASSERT(sizeof(WINLVM_UPDATE_LV_REQUEST) == 64, update_lv_request_size);
WINLVM_ABI_ASSERT(sizeof(WINLVM_PROBE_DEVICE_REQUEST) == 520, probe_device_request_size);
WINLVM_ABI_ASSERT(sizeof(WINLVM_INVENTORY_LV_ENTRY) == 192, inventory_lv_entry_size);
WINLVM_ABI_ASSERT(sizeof(WINLVM_INVENTORY) == 16, inventory_size);

#if defined(_MSC_VER) && !defined(__cplusplus)
#define WINLVM_INLINE static __inline
#else
#define WINLVM_INLINE static inline
#endif

/*
 * Normalize an LVM2 UUID into its canonical 32-character alphanumeric form.
 * Accepts both raw 32-char UUIDs and hyphenated 6-4-4-4-4-4-6 wire strings.
 * Returns 1 on success (out[32] == '\0'), 0 on malformed or unterminated input.
 */
WINLVM_INLINE int winlvm_normalize_uuid(const char *in, size_t in_cap, char out[33]) {
    size_t n = 0;
    size_t i = 0;
    int saw_nul = 0;
    if (!in || !out || in_cap == 0) {
        return 0;
    }
    for (i = 0; i < in_cap; i++) {
        char c = in[i];
        if (c == '\0') {
            saw_nul = 1;
            break;
        }
        if (c == '-') {
            continue;
        }
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z'))) {
            return 0;
        }
        if (n >= 32) {
            return 0;
        }
        out[n++] = c;
    }
    if (!saw_nul && in_cap != 32) {
        return 0;
    }
    if (n != 32) {
        return 0;
    }
    out[32] = '\0';
    return 1;
}

/*
 * Compare two LVM2 UUIDs ignoring optional '-' separators.
 * Returns 1 iff both normalize to identical 32-character alphanumeric UUIDs.
 */
WINLVM_INLINE int winlvm_uuid_equal_wire(const char *a, size_t a_cap,
                                         const char *b, size_t b_cap) {
    char na[33];
    char nb[33];
    size_t i;
    if (!winlvm_normalize_uuid(a, a_cap, na) ||
        !winlvm_normalize_uuid(b, b_cap, nb)) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        if (na[i] != nb[i]) {
            return 0;
        }
    }
    return 1;
}

#endif /* WINLVM_IOCTL_H */
