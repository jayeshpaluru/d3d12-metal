#pragma once
#include "dxc/Support/WinIncludes.h"
#ifndef interface
#define interface struct
#endif
typedef float FLOAT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT;
#ifndef _In_
#define _In_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(x)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif
#ifndef _In_reads_bytes_opt_
#define _In_reads_bytes_opt_(x)
#endif
#ifndef _In_range_
#define _In_range_(x, y)
#endif
#ifndef _In_bytecount_
#define _In_bytecount_(x)
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_opt_result_z_
#define _Outptr_opt_result_z_
#endif
#ifndef _Outptr_opt_result_bytebuffer_
#define _Outptr_opt_result_bytebuffer_(x)
#endif
#ifndef _COM_Outptr_
#define _COM_Outptr_
#endif
#ifndef _COM_Outptr_result_maybenull_
#define _COM_Outptr_result_maybenull_
#endif
#ifndef _COM_Outptr_opt_
#define _COM_Outptr_opt_
#endif
#ifndef _COM_Outptr_opt_result_maybenull_
#define _COM_Outptr_opt_result_maybenull_
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _Out_writes_z_
#define _Out_writes_z_(x)
#endif
#ifndef _Out_writes_opt_
#define _Out_writes_opt_(x)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(x)
#endif
#ifndef _Out_writes_all_opt_
#define _Out_writes_all_opt_(x)
#endif
#ifndef _Out_writes_to_opt_
#define _Out_writes_to_opt_(x, y)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(x)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(x)
#endif
#ifndef _Out_writes_bytes_all_opt_
#define _Out_writes_bytes_all_opt_(x)
#endif
#ifndef _Out_writes_bytes_opt_
#define _Out_writes_bytes_opt_(x)
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(x)
#endif
#ifndef _Inout_updates_bytes_
#define _Inout_updates_bytes_(x)
#endif
#ifndef _Field_size_
#define _Field_size_(x)
#endif
#ifndef _Field_size_opt_
#define _Field_size_opt_(x)
#endif
#ifndef _Field_size_bytes_
#define _Field_size_bytes_(x)
#endif
#ifndef _Field_size_full_
#define _Field_size_full_(x)
#endif
#ifndef _Field_size_full_opt_
#define _Field_size_full_opt_(x)
#endif
#ifndef _Field_size_bytes_full_
#define _Field_size_bytes_full_(x)
#endif
#ifndef _Field_size_bytes_full_opt_
#define _Field_size_bytes_full_opt_(x)
#endif
#ifndef _Field_size_bytes_part_
#define _Field_size_bytes_part_(x, y)
#endif
#ifndef _Field_range_
#define _Field_range_(x, y)
#endif
#ifndef _Field_z_
#define _Field_z_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _IRQL_requires_
#define _IRQL_requires_(x)
#endif
#ifndef _IRQL_requires_min_
#define _IRQL_requires_min_(x)
#endif
#ifndef _IRQL_requires_max_
#define _IRQL_requires_max_(x)
#endif
#ifndef _At_
#define _At_(x, y)
#endif
#ifndef _Always_
#define _Always_(x)
#endif
#ifndef _Return_type_success_
#define _Return_type_success_(x)
#endif
#ifndef _Translates_Win32_to_HRESULT_
#define _Translates_Win32_to_HRESULT_(x)
#endif
#ifndef _Maybenull_
#define _Maybenull_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Outptr_result_nullonfailure_
#define _Outptr_result_nullonfailure_
#endif
#ifndef _Analysis_assume_
#define _Analysis_assume_(x)
#endif
#ifndef _Success_
#define _Success_(x)
#endif
#ifndef _In_count_
#define _In_count_(x)
#endif
#ifndef _In_opt_count_
#define _In_opt_count_(x)
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#ifndef WINAPI
#define WINAPI
#endif
typedef uint16_t UINT16;
typedef uint64_t UINT64;
typedef int64_t INT64;
typedef int16_t INT16;
typedef int8_t INT8;
typedef struct _LUID { uint32_t LowPart; int32_t HighPart; } LUID;
typedef struct _SECURITY_ATTRIBUTES { uint32_t nLength; void *lpSecurityDescriptor; int bInheritHandle; } SECURITY_ATTRIBUTES;
typedef void *HWND;
typedef GUID UUID;
#include <stdlib.h>
inline void *GetProcessHeap() { return nullptr; }
inline void *HeapAlloc(void *, uint32_t, size_t n) { return malloc(n); }
inline int HeapFree(void *, uint32_t, void *p) { free(p); return 1; }
#define __analysis_assume(x)
#define __out_ecount(x)
#define __in_range(a, b)
#define __in_ecount(x)
#define __inout_ecount(x)
#define __in
#define __out
#define __inout
#define __out_bcount(x)
#define __in_bcount(x)
#define __assume(x)
#include <string.h>
inline long StringCchCopyA(char *dst, size_t n, const char *src) { strlcpy(dst, src, n); return 0; }
#define __field_ecount_part(a, b)
#define __field_ecount(a)
#define __field_bcount(a)
#define __out_ecount_part(a, b)
#define __in_ecount_opt(a)
#define __out_opt
#define __in_opt
#define __inout_opt
#ifndef CONST
#define CONST const
#endif
#ifndef _UI8_MAX
#define _UI8_MAX 0xff
#endif
#include <algorithm>
using std::min;
using std::max;
#define _Outptr_result_bytebuffer_maybenull_(x)
#define _Outptr_result_maybenull_z_
#define D3D11_COMMONSHADER_TEMP_REGISTER_COUNT 4096
#define D3D10_REQ_CONSTANT_BUFFER_ELEMENT_COUNT 4096
#ifndef __override
#define __override
#endif
#define D3D11_SHADER_MAX_INTERFACES 253
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT 14
#define D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT 16
#define D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT 128
inline int QueryPerformanceCounter(LARGE_INTEGER *value) { value->QuadPart = 0; return 1; }
