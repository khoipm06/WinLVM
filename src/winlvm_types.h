/*
 * winlvm_types.h — shared freestanding types for the winlvm C layer.
 * Freestanding definition of winlvm_byte_source for disk and file I/O.
 */
#ifndef WINLVM_TYPES_H
#define WINLVM_TYPES_H

#include <stddef.h>
#if defined(_KERNEL_MODE) && defined(_MSC_VER)
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

/* A read-only, randomly-addressable byte source. The driver backs this
 * with IRP_MJ_READ down to the physical disk; user-mode tests back it
 * with fopen/fread. read() returns bytes read, or 0 on short read/fail. */
typedef struct winlvm_byte_source {
    void *impl;
    uint64_t size;
    size_t (*read)(void *impl, uint64_t offset, void *buf, size_t len);
} winlvm_byte_source;

#endif
