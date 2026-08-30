#include "CrashHandler.h"

#include <Windows.h>
// DbgHelp must come after Windows.h
#include <DbgHelp.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#pragma comment(lib, "Dbghelp.lib")

namespace CrashHandler {

static char g_log_path[MAX_PATH] = {0};
static bool g_installed = false;
static LONG g_in_handler = 0;

static void timestamp(char* buf, size_t n) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    if (std::strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm) == 0 && n) buf[0] = '\0';
}

static void module_of(void* addr, char* name, size_t name_sz, DWORD64* base) {
    *base = 0;
    if (name_sz) name[0] = '\0';
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod) {
        *base = (DWORD64)mod;
        char full[MAX_PATH];
        if (GetModuleFileNameA(mod, full, MAX_PATH)) {
            const char* bn = strrchr(full, '\\');
            bn = bn ? bn + 1 : full;
            strncpy_s(name, name_sz, bn, _TRUNCATE);
        }
    }
}

// Walk the current call stack and write each frame to `f`. Symbolizes via
// dbghelp when a PDB is available, else logs module!+0xRVA.
static void write_stack(FILE* f) {
    void* frames[62];
    USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    HANDLE proc = GetCurrentProcess();

    alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (USHORT i = 0; i < n; i++) {
        DWORD64 addr = (DWORD64)frames[i];
        char modname[MAX_PATH];
        DWORD64 base = 0;
        module_of(frames[i], modname, sizeof(modname), &base);
        DWORD64 rva = base ? addr - base : 0;

        DWORD64 disp = 0;
        bool have_sym = SymFromAddr(proc, addr, &disp, sym) != FALSE;
        DWORD ldisp = 0;
        bool have_line =
            SymGetLineFromAddr64(proc, addr, &ldisp, &line) != FALSE;

        if (have_sym && have_line) {
            const char* fn = strrchr(line.FileName, '\\');
            fn = fn ? fn + 1 : line.FileName;
            fprintf(f, "  [%02u] %s!%s+0x%llx (%s:%lu) [rva 0x%llx]\n", i,
                    modname[0] ? modname : "?", sym->Name,
                    (unsigned long long)disp, fn, line.LineNumber,
                    (unsigned long long)rva);
        } else if (have_sym) {
            fprintf(f, "  [%02u] %s!%s+0x%llx [rva 0x%llx]\n", i,
                    modname[0] ? modname : "?", sym->Name,
                    (unsigned long long)disp, (unsigned long long)rva);
        } else {
            fprintf(f, "  [%02u] %s+0x%llx\n", i,
                    modname[0] ? modname : "?", (unsigned long long)rva);
        }
    }
}

static void dump(const char* reason, unsigned long code, void* fault_addr) {
    // Re-entrancy guard: if logging the crash itself faults, don't loop.
    if (InterlockedExchange(&g_in_handler, 1)) return;

    FILE* f = nullptr;
    if (fopen_s(&f, g_log_path, "a") == 0 && f) {
        char tb[32];
        timestamp(tb, sizeof(tb));
        fprintf(f, "\n==== d3d9_uploader crash report %s ====\n", tb);
        fprintf(f, "reason : %s\n", reason);
        if (code) {
            fprintf(f, "code   : 0x%08lx\n", code);
        }
        if (fault_addr) {
            char modname[MAX_PATH];
            DWORD64 base = 0;
            module_of(fault_addr, modname, sizeof(modname), &base);
            fprintf(f, "address: %p (%s+0x%llx)\n", fault_addr,
                    modname[0] ? modname : "?",
                    base ? (unsigned long long)((DWORD64)fault_addr - base)
                         : 0ull);
        }
        fprintf(f, "stack  :\n");
        write_stack(f);
        fprintf(f, "==== end crash report ====\n");
        fflush(f);
        fclose(f);
    }

    g_in_handler = 0;
}

static bool is_fatal(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN / __fastfail
        case 0xC0000374:  // STATUS_HEAP_CORRUPTION
            return true;
        default:
            return false;
    }
}

static LONG WINAPI veh(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && is_fatal(ep->ExceptionRecord->ExceptionCode)) {
        dump("vectored exception", ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    }
    // Let normal handling proceed (this is a diagnostic, not a recovery).
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        dump("unhandled exception", ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void invalid_parameter(const wchar_t* expr, const wchar_t* func,
                              const wchar_t* file, unsigned int line,
                              uintptr_t) {
    // This runs INSTEAD of the CRT's default handler (which fast-fails with
    // 0xc0000409). It is the usual source of that code here (a bad printf-style
    // argument, e.g. %s given a std::string or a null pointer).
    (void)expr;
    (void)func;
    (void)file;
    (void)line;
    dump("CRT invalid parameter (0xc0000409)", 0xC0000409, nullptr);
    // Fall through to the default behavior (process terminates).
}

static void purecall() { dump("pure virtual call", 0, nullptr); }

static void terminate_fn() {
    dump("std::terminate (unhandled C++ exception)", 0, nullptr);
    abort();
}

void install(const char* crash_log_path) {
    if (g_installed || !crash_log_path) return;
    g_installed = true;
    strncpy_s(g_log_path, crash_log_path, _TRUNCATE);

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    AddVectoredExceptionHandler(1 /*call first*/, veh);
    SetUnhandledExceptionFilter(unhandled);
    _set_invalid_parameter_handler(invalid_parameter);
    _set_purecall_handler(purecall);
    std::set_terminate(terminate_fn);
}

}  // namespace CrashHandler
