/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <delayimp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "audit.h"

typedef HANDLE (WINAPI *CreateFileWFn)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *CreateFileAFn)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
typedef HANDLE (WINAPI *CreateFile2Fn)(LPCWSTR,DWORD,DWORD,DWORD,LPCREATEFILE2_EXTENDED_PARAMETERS);
typedef FARPROC (WINAPI *GetProcAddressFn)(HMODULE,LPCSTR);
typedef NTSTATUS (NTAPI *NtCreateFileFn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
typedef NTSTATUS (NTAPI *NtOpenFileFn)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,PIO_STATUS_BLOCK,ULONG,ULONG);
typedef NTSTATUS (NTAPI *LdrGetProcedureAddressFn)(PVOID,PANSI_STRING,ULONG,PVOID*);
typedef NTSTATUS (NTAPI *LdrGetProcedureAddressForCallerFn)(PVOID,PANSI_STRING,ULONG,PVOID*,ULONG,PVOID*);
typedef BOOL (WINAPI *CreateProcessWFn)(LPCWSTR,LPWSTR,LPSECURITY_ATTRIBUTES,LPSECURITY_ATTRIBUTES,BOOL,DWORD,LPVOID,LPCWSTR,LPSTARTUPINFOW,LPPROCESS_INFORMATION);
typedef NTSTATUS (NTAPI *LdrRegisterDllNotificationFn)(ULONG, PVOID, PVOID, PVOID*);

static HMODULE g_self;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static volatile LONG g_events, g_patched, g_patch_failures;
static WCHAR g_case[96];
static CreateFileWFn g_create_w;
static CreateFileAFn g_create_a;
static CreateFile2Fn g_create_2;
static GetProcAddressFn g_get_proc;
static NtCreateFileFn g_nt_create;
static NtOpenFileFn g_nt_open;
static LdrGetProcedureAddressFn g_ldr_get_proc;
static LdrGetProcedureAddressForCallerFn g_ldr_get_proc_caller;
static CreateProcessWFn g_create_process;
static LdrRegisterDllNotificationFn g_register_notify;
static PVOID g_notify_cookie;

static void write_all(const char *s) {
    DWORD n = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, s, (DWORD)strlen(s), &n, NULL);
}
static void utf8(const WCHAR *w, char *dst, int cap) {
    if (!w) { strncpy(dst, "", cap); return; }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, cap, NULL, NULL);
    if (!n && cap) dst[0] = 0;
    for (int i = 0; dst[i]; ++i) if (dst[i] == '\t' || dst[i] == '\r' || dst[i] == '\n') dst[i] = ' ';
}
static void escaped(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 4 < cap; ++in) {
        if (*in == '%' || *in == '\t' || *in == '\r' || *in == '\n') {
            static const char hex[] = "0123456789ABCDEF";
            unsigned char c = (unsigned char)*in;
            out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
        } else out[n++] = *in;
    }
    out[n] = 0;
}
static void caller_name(void *ret, char *out, int cap) {
    HMODULE m = NULL; WCHAR path[2048] = L"<unknown>";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ret, &m)) GetModuleFileNameW(m, path, 2048);
    utf8(path, out, cap);
}
static void final_path(HANDLE h, WCHAR *path, DWORD count) {
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD n = GetFinalPathNameByHandleW(h, path, count, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n && n < count) return;
    }
    path[0] = 0;
}
static void record_open(const char *kind, const char *api, const WCHAR *requested, HANDLE handle, LONG result, DWORD access, void *ret) {
    if (g_tls == TLS_OUT_OF_INDEXES || TlsGetValue(g_tls)) return;
    TlsSetValue(g_tls, (void *)1);
    WCHAR resolved[4096]; char p[8192], epath[16384], mod[4096], emod[8192], case_id[256], ecase[512], line[27000];
    final_path(handle, resolved, 4096);
    utf8(resolved[0] ? resolved : requested, p, sizeof(p)); escaped(p, epath, sizeof(epath));
    caller_name(ret, mod, sizeof(mod)); escaped(mod, emod, sizeof(emod));
    utf8(g_case, case_id, sizeof(case_id)); escaped(case_id, ecase, sizeof(ecase));
    _snprintf(line, sizeof(line)-1, "OPEN\t%s\t%s\t%s\t%s\t0x%08lx\t%ld\t%s\n", ecase, kind, api,
              epath, (unsigned long)access, (long)result, emod);
    line[sizeof(line)-1] = 0; write_all(line); InterlockedIncrement(&g_events);
    TlsSetValue(g_tls, NULL);
}
static void record_native(const char *api, PHANDLE h, POBJECT_ATTRIBUTES oa, ACCESS_MASK access, NTSTATUS status, void *ret) {
    WCHAR requested[4096] = L"";
    if (oa && oa->ObjectName && oa->ObjectName->Buffer) {
        USHORT n = oa->ObjectName->Length / sizeof(WCHAR); if (n >= 4096) n = 4095;
        memcpy(requested, oa->ObjectName->Buffer, n * sizeof(WCHAR)); requested[n] = 0;
    }
    record_open("open", api, requested, status >= 0 && h ? *h : INVALID_HANDLE_VALUE, status, access, ret);
}
#define RETADDR() __builtin_return_address(0)
static HANDLE WINAPI hook_create_w(LPCWSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD f,HANDLE t) {
    if (TlsGetValue(g_tls)) return g_create_w(p,a,s,sa,d,f,t);
    TlsSetValue(g_tls,(void*)1); HANDLE h=g_create_w(p,a,s,sa,d,f,t); DWORD err=GetLastError(); TlsSetValue(g_tls,NULL);
    record_open("open","CreateFileW",p,h,h==INVALID_HANDLE_VALUE?-(LONG)err:0,a,RETADDR()); SetLastError(err); return h;
}
static HANDLE WINAPI hook_create_a(LPCSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD f,HANDLE t) {
    if (TlsGetValue(g_tls)) return g_create_a(p,a,s,sa,d,f,t);
    TlsSetValue(g_tls,(void*)1); HANDLE h=g_create_a(p,a,s,sa,d,f,t); DWORD err=GetLastError(); TlsSetValue(g_tls,NULL);
    WCHAR w[4096]=L""; if(p) MultiByteToWideChar(CP_ACP,0,p,-1,w,4096);
    record_open("open","CreateFileA",w,h,h==INVALID_HANDLE_VALUE?-(LONG)err:0,a,RETADDR()); SetLastError(err); return h;
}
static HANDLE WINAPI hook_create_2(LPCWSTR p,DWORD a,DWORD s,DWORD d,LPCREATEFILE2_EXTENDED_PARAMETERS x) {
    if (TlsGetValue(g_tls)) return g_create_2(p,a,s,d,x);
    TlsSetValue(g_tls,(void*)1); HANDLE h=g_create_2(p,a,s,d,x); DWORD err=GetLastError(); TlsSetValue(g_tls,NULL);
    record_open("open","CreateFile2",p,h,h==INVALID_HANDLE_VALUE?-(LONG)err:0,a,RETADDR()); SetLastError(err); return h;
}
static NTSTATUS NTAPI hook_nt_create(PHANDLE h,ACCESS_MASK a,POBJECT_ATTRIBUTES o,PIO_STATUS_BLOCK i,PLARGE_INTEGER z,ULONG at,ULONG sh,ULONG d,ULONG op,PVOID ea,ULONG el) {
    if (TlsGetValue(g_tls)) return g_nt_create(h,a,o,i,z,at,sh,d,op,ea,el);
    DWORD err=GetLastError(); TlsSetValue(g_tls,(void*)1); NTSTATUS s=g_nt_create(h,a,o,i,z,at,sh,d,op,ea,el); TlsSetValue(g_tls,NULL);
    record_native("NtCreateFile",h,o,a,s,RETADDR()); SetLastError(err); return s;
}
static NTSTATUS NTAPI hook_nt_open(PHANDLE h,ACCESS_MASK a,POBJECT_ATTRIBUTES o,PIO_STATUS_BLOCK i,ULONG sh,ULONG op) {
    if (TlsGetValue(g_tls)) return g_nt_open(h,a,o,i,sh,op);
    DWORD err=GetLastError(); TlsSetValue(g_tls,(void*)1); NTSTATUS s=g_nt_open(h,a,o,i,sh,op); TlsSetValue(g_tls,NULL);
    record_native("NtOpenFile",h,o,a,s,RETADDR()); SetLastError(err); return s;
}
static FARPROC WINAPI hook_get_proc(HMODULE m,LPCSTR name) {
    if (name && ((uintptr_t)name >> 16)) {
        if (!strcmp(name,"CreateFileW")) return (FARPROC)hook_create_w;
        if (!strcmp(name,"CreateFileA")) return (FARPROC)hook_create_a;
        if (!strcmp(name,"CreateFile2")) return (FARPROC)hook_create_2;
        if (!strcmp(name,"NtCreateFile")) return (FARPROC)hook_nt_create;
        if (!strcmp(name,"NtOpenFile")) return (FARPROC)hook_nt_open;
    }
    return g_get_proc(m,name);
}
static NTSTATUS NTAPI hook_ldr_get_proc(PVOID m,PANSI_STRING n,ULONG ordinal,PVOID *out) {
    if (n && n->Buffer && out && ordinal == 0) {
        if (n->Length==11 && !memcmp(n->Buffer,"CreateFileW",11)) {*out=hook_create_w;return 0;}
        if (n->Length==11 && !memcmp(n->Buffer,"CreateFileA",11)) {*out=hook_create_a;return 0;}
        if (n->Length==11 && !memcmp(n->Buffer,"CreateFile2",11)) {*out=hook_create_2;return 0;}
        if (n->Length==12 && !memcmp(n->Buffer,"NtCreateFile",12)) {*out=hook_nt_create;return 0;}
        if (n->Length==10 && !memcmp(n->Buffer,"NtOpenFile",10)) {*out=hook_nt_open;return 0;}
    }
    return g_ldr_get_proc(m,n,ordinal,out);
}
static NTSTATUS NTAPI hook_ldr_get_proc_caller(PVOID m,PANSI_STRING n,ULONG ord,PVOID *out,ULONG flags,PVOID *callback) {
    if (n && n->Buffer && out && ord==0) {
        if (n->Length==11 && !memcmp(n->Buffer,"CreateFileW",11)) {*out=hook_create_w;return 0;}
        if (n->Length==11 && !memcmp(n->Buffer,"CreateFileA",11)) {*out=hook_create_a;return 0;}
        if (n->Length==11 && !memcmp(n->Buffer,"CreateFile2",11)) {*out=hook_create_2;return 0;}
        if (n->Length==12 && !memcmp(n->Buffer,"NtCreateFile",12)) {*out=hook_nt_create;return 0;}
        if (n->Length==10 && !memcmp(n->Buffer,"NtOpenFile",10)) {*out=hook_nt_open;return 0;}
    }
    return g_ldr_get_proc_caller(m,n,ord,out,flags,callback);
}
static BOOL WINAPI hook_create_process_w(LPCWSTR app,LPWSTR cmd,LPSECURITY_ATTRIBUTES p,LPSECURITY_ATTRIBUTES t,BOOL inherit,DWORD flags,LPVOID env,LPCWSTR cwd,LPSTARTUPINFOW si,LPPROCESS_INFORMATION pi) {
    BOOL ok=g_create_process(app,cmd,p,t,inherit,flags,env,cwd,si,pi); DWORD err=GetLastError();
    if (!TlsGetValue(g_tls)) {
        WCHAR target[4096]=L"<command>"; if(app) lstrcpynW(target,app,4096);
        record_open("spawn","CreateProcessW",target,INVALID_HANDLE_VALUE,ok?0:-(LONG)err,flags,RETADDR());
    }
    SetLastError(err); return ok;
}

static void *hook_for(const char *dll,const char *sym) {
    if (!_stricmp(dll,"ntdll.dll")) {
        if(!strcmp(sym,"NtCreateFile")) return hook_nt_create;
        if(!strcmp(sym,"NtOpenFile")) return hook_nt_open;
        if(!strcmp(sym,"LdrGetProcedureAddress")) return hook_ldr_get_proc;
        if(!strcmp(sym,"LdrGetProcedureAddressForCaller")) return hook_ldr_get_proc_caller;
    }
    if (!_stricmp(dll,"kernel32.dll") || !_stricmp(dll,"kernelbase.dll") || !_strnicmp(dll,"api-ms-win-core-file-",21)) {
        if(!strcmp(sym,"CreateFileW")) return hook_create_w;
        if(!strcmp(sym,"CreateFileA")) return hook_create_a;
        if(!strcmp(sym,"CreateFile2")) return hook_create_2;
    }
    if (!_stricmp(dll,"kernel32.dll") || !_stricmp(dll,"kernelbase.dll") || !_strnicmp(dll,"api-ms-win-core-libraryloader-",30))
        if(!strcmp(sym,"GetProcAddress")) return hook_get_proc;
    if (!_stricmp(dll,"kernel32.dll") || !_stricmp(dll,"kernelbase.dll") || !_strnicmp(dll,"api-ms-win-core-processthreads-",31))
        if(!strcmp(sym,"CreateProcessW")) return hook_create_process_w;
    return NULL;
}
static void *hook_for_bound(const char *dll,void *current) {
    if (!_stricmp(dll,"ntdll.dll")) {
        if(current==(void*)g_nt_create)return hook_nt_create;
        if(current==(void*)g_nt_open)return hook_nt_open;
        if(current==(void*)g_ldr_get_proc)return hook_ldr_get_proc;
        if(current==(void*)g_ldr_get_proc_caller)return hook_ldr_get_proc_caller;
    }
    if (!_stricmp(dll,"kernel32.dll") || !_stricmp(dll,"kernelbase.dll") || !_strnicmp(dll,"api-ms-win-core-file-",21)) {
        if(current==(void*)g_create_w)return hook_create_w;
        if(current==(void*)g_create_a)return hook_create_a;
        if(current==(void*)g_create_2)return hook_create_2;
    }
    if (!_stricmp(dll,"kernel32.dll") || !_stricmp(dll,"kernelbase.dll") || !_strnicmp(dll,"api-ms-win-core-libraryloader-",30))
        if(current==(void*)g_get_proc)return hook_get_proc;
    if (!_stricmp(dll,"kernel32.dll") || !_stricmp(dll,"kernelbase.dll") || !_strnicmp(dll,"api-ms-win-core-processthreads-",31))
        if(current==(void*)g_create_process)return hook_create_process_w;
    return NULL;
}
static BOOL patch_slot(IMAGE_THUNK_DATA *slot,void *to) {
    DWORD old=0; if(!VirtualProtect(&slot->u1.Function,sizeof(void*),PAGE_READWRITE,&old)){InterlockedIncrement(&g_patch_failures);return FALSE;}
    InterlockedExchangePointer((PVOID volatile *)&slot->u1.Function,to); DWORD ignored; VirtualProtect(&slot->u1.Function,sizeof(void*),old,&ignored); InterlockedIncrement(&g_patched); return TRUE;
}
static unsigned patch_table(HMODULE module,DWORD rva,DWORD size,BOOL delay) {
    if(!rva || !size) return 0;
    BYTE *base=(BYTE*)module; IMAGE_DOS_HEADER *dos=(void*)base; if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
    IMAGE_NT_HEADERS *nt=(void*)(base+dos->e_lfanew); unsigned n=0;
    if(delay) {
        ImgDelayDescr *d=(void*)(base+rva); for(;d->rvaDLLName;d++) {
            BOOL rva=(d->grAttrs & dlattrRva)!=0;
            const char *dll=rva?(const char*)(base+d->rvaDLLName):(const char*)(uintptr_t)d->rvaDLLName;
            IMAGE_THUNK_DATA *names=rva?(void*)(base+d->rvaINT):(void*)(uintptr_t)d->rvaINT;
            IMAGE_THUNK_DATA *slots=rva?(void*)(base+d->rvaIAT):(void*)(uintptr_t)d->rvaIAT;
            for(;names->u1.AddressOfData;names++,slots++) { if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue; IMAGE_IMPORT_BY_NAME *ib=rva?(void*)(base+names->u1.AddressOfData):(void*)(uintptr_t)names->u1.AddressOfData; void *to=hook_for(dll,(char*)ib->Name); if(to && (void*)slots->u1.Function!=to) n+=patch_slot(slots,to); }
        }
    } else {
        IMAGE_IMPORT_DESCRIPTOR *d=(void*)(base+rva); for(;d->Name;d++) {
            const char *dll=(const char*)(base+d->Name); if(!d->OriginalFirstThunk) continue;
            IMAGE_THUNK_DATA *slots=(void*)(base+d->FirstThunk);
            if(!d->OriginalFirstThunk) { for(;slots->u1.Function;slots++){void *to=hook_for_bound(dll,(void*)slots->u1.Function);if(to && (void*)slots->u1.Function!=to)n+=patch_slot(slots,to);} continue; }
            IMAGE_THUNK_DATA *names=(void*)(base+d->OriginalFirstThunk);
            for(;names->u1.AddressOfData;names++,slots++) { if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue; IMAGE_IMPORT_BY_NAME *ib=(void*)(base+names->u1.AddressOfData); void *to=hook_for(dll,(char*)ib->Name); if(to && (void*)slots->u1.Function!=to)n+=patch_slot(slots,to); }
        }
    }
    (void)nt; return n;
}
static unsigned patch_module(HMODULE module) {
    if(!module || module==g_self)return 0; BYTE *b=(BYTE*)module; IMAGE_DOS_HEADER *dos=(void*)b; if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return 0;
    IMAGE_NT_HEADERS *nt=(void*)(b+dos->e_lfanew); if(nt->Signature!=IMAGE_NT_SIGNATURE)return 0;
    unsigned n=patch_table(module,nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size,FALSE);
#ifdef IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
    n+=patch_table(module,nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress,nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size,TRUE);
#endif
    return n;
}
typedef struct { ULONG Flags; const UNICODE_STRING *FullDllName,*BaseDllName; PVOID DllBase; ULONG SizeOfImage; } DllLoaded;
typedef union { DllLoaded Loaded; DllLoaded Unloaded; } DllNotificationData;
static VOID CALLBACK dll_notification(ULONG reason,const DllNotificationData *data,PVOID context) {
    (void)context; if(reason==1 && data && data->Loaded.DllBase) patch_module((HMODULE)data->Loaded.DllBase);
}
static BOOL WINAPI patch_all(void) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId()); if(snap==INVALID_HANDLE_VALUE)return FALSE;
    MODULEENTRY32W me; memset(&me,0,sizeof(me)); me.dwSize=sizeof(me); BOOL ok=TRUE;
    if(Module32FirstW(snap,&me)) do { patch_module(me.hModule); } while(Module32NextW(snap,&me)); else ok=FALSE;
    CloseHandle(snap); return ok;
}
BOOL WINAPI VacardsOpenAuditStartW(LPCWSTR event_file,LPCWSTR case_id) {
    if(g_log!=INVALID_HANDLE_VALUE || !event_file || !case_id)return FALSE;
    g_log=CreateFileW(event_file,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(g_log==INVALID_HANDLE_VALUE)return FALSE;
    lstrcpynW(g_case,case_id,96); g_tls=TlsAlloc(); if(g_tls==TLS_OUT_OF_INDEXES)return FALSE;
    HMODULE kb=GetModuleHandleW(L"kernelbase.dll"),nt=GetModuleHandleW(L"ntdll.dll");
    if(!kb||!nt)return FALSE;
    HMODULE exe=GetModuleHandleW(NULL); IMAGE_DOS_HEADER *dos=(void*)exe; IMAGE_NT_HEADERS *headers=(void*)((BYTE*)exe+dos->e_lfanew);
    if(headers->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF) return FALSE;
    /* KernelBase originals avoid the Kernel32 forwarder recursion seen in probe round 1. */
    g_create_w=(CreateFileWFn)GetProcAddress(kb,"CreateFileW"); g_create_a=(CreateFileAFn)GetProcAddress(kb,"CreateFileA");
    g_create_2=(CreateFile2Fn)GetProcAddress(kb,"CreateFile2"); g_get_proc=(GetProcAddressFn)GetProcAddress(kb,"GetProcAddress");
    g_nt_create=(NtCreateFileFn)GetProcAddress(nt,"NtCreateFile"); g_nt_open=(NtOpenFileFn)GetProcAddress(nt,"NtOpenFile");
    g_ldr_get_proc=(LdrGetProcedureAddressFn)GetProcAddress(nt,"LdrGetProcedureAddress");
    g_ldr_get_proc_caller=(LdrGetProcedureAddressForCallerFn)GetProcAddress(nt,"LdrGetProcedureAddressForCaller");
    g_create_process=(CreateProcessWFn)GetProcAddress(kb,"CreateProcessW");
    if(!g_create_w||!g_create_a||!g_create_2||!g_get_proc||!g_nt_create||!g_nt_open||!g_ldr_get_proc||!g_ldr_get_proc_caller||!g_create_process)return FALSE;
    g_register_notify=(LdrRegisterDllNotificationFn)GetProcAddress(nt,"LdrRegisterDllNotification");
    if(!g_register_notify || g_register_notify(0,dll_notification,NULL,&g_notify_cookie)<0)return FALSE;
    patch_all();
    char line[256]; _snprintf(line,sizeof(line)-1,"START\t%s\twindows-iat\n","p9-parser-opens/2"); line[sizeof(line)-1]=0; write_all(line);
    return InterlockedCompareExchange(&g_patch_failures,0,0)==0;
}
BOOL WINAPI VacardsOpenAuditCheckCoverage(void) {
    if(!patch_all())return FALSE;
    return InterlockedCompareExchange(&g_patch_failures,0,0)==0;
}
void WINAPI VacardsOpenAuditStop(void) {
    if(g_log!=INVALID_HANDLE_VALUE){char line[128];_snprintf(line,sizeof(line)-1,"END\t%ld\t%ld\n",(long)g_events,(long)g_patched);line[sizeof(line)-1]=0;write_all(line);FlushFileBuffers(g_log);CloseHandle(g_log);g_log=INVALID_HANDLE_VALUE;}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID reserved) {
    (void)reserved; if(reason==DLL_PROCESS_ATTACH){g_self=instance;DisableThreadLibraryCalls(instance);} return TRUE;
}
