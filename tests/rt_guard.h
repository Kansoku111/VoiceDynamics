#pragma once
// Test-only guard (pattern shared with the other plug-ins in this repository).
// Test-only real-time guard for a loaded plugin DLL. Rewrites the module's
// import address table so that heap allocation, locking, waiting, file and
// window-message APIs called by the plugin binary are counted while armed.
// The plugin statically links the C runtime, so malloc/new reach HeapAlloc
// through this table. Counting is limited to the arming thread.
#include <windows.h>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace rtguard {
inline thread_local bool armed = false;
inline std::atomic<int> heap{0}, locks{0}, waits{0}, files{0}, windows{0};
struct Original {
    decltype(&HeapAlloc) heapAlloc = nullptr;
    decltype(&HeapReAlloc) heapReAlloc = nullptr;
    decltype(&HeapFree) heapFree = nullptr;
    decltype(&VirtualAlloc) virtualAlloc = nullptr;
    decltype(&EnterCriticalSection) enterCriticalSection = nullptr;
    decltype(&AcquireSRWLockExclusive) acquireExclusive = nullptr;
    decltype(&AcquireSRWLockShared) acquireShared = nullptr;
    decltype(&WaitForSingleObject) waitSingle = nullptr;
    decltype(&WaitForSingleObjectEx) waitSingleEx = nullptr;
    decltype(&WaitForMultipleObjects) waitMultiple = nullptr;
    decltype(&Sleep) sleep = nullptr;
    decltype(&SleepEx) sleepEx = nullptr;
    decltype(&CreateFileW) createFile = nullptr;
    decltype(&WriteFile) writeFile = nullptr;
    decltype(&ReadFile) readFile = nullptr;
    decltype(&SendMessageW) sendMessage = nullptr;
    decltype(&PostMessageW) postMessage = nullptr;
    decltype(&InvalidateRect) invalidateRect = nullptr;
};
inline Original original;
inline LPVOID WINAPI hHeapAlloc(HANDLE h, DWORD f, SIZE_T n) { if (armed) ++heap; return original.heapAlloc(h, f, n); }
inline LPVOID WINAPI hHeapReAlloc(HANDLE h, DWORD f, LPVOID p, SIZE_T n) { if (armed) ++heap; return original.heapReAlloc(h, f, p, n); }
inline BOOL WINAPI hHeapFree(HANDLE h, DWORD f, LPVOID p) { if (armed && p) ++heap; return original.heapFree(h, f, p); }
inline LPVOID WINAPI hVirtualAlloc(LPVOID a, SIZE_T n, DWORD t, DWORD p) { if (armed) ++heap; return original.virtualAlloc(a, n, t, p); }
inline void WINAPI hEnterCriticalSection(LPCRITICAL_SECTION c) { if (armed) ++locks; original.enterCriticalSection(c); }
inline void WINAPI hAcquireExclusive(PSRWLOCK l) { if (armed) ++locks; original.acquireExclusive(l); }
inline void WINAPI hAcquireShared(PSRWLOCK l) { if (armed) ++locks; original.acquireShared(l); }
inline DWORD WINAPI hWaitSingle(HANDLE h, DWORD t) { if (armed) ++waits; return original.waitSingle(h, t); }
inline DWORD WINAPI hWaitSingleEx(HANDLE h, DWORD t, BOOL a) { if (armed) ++waits; return original.waitSingleEx(h, t, a); }
inline DWORD WINAPI hWaitMultiple(DWORD n, const HANDLE* h, BOOL all, DWORD t) { if (armed) ++waits; return original.waitMultiple(n, h, all, t); }
inline void WINAPI hSleep(DWORD t) { if (armed) ++waits; original.sleep(t); }
inline DWORD WINAPI hSleepEx(DWORD t, BOOL a) { if (armed) ++waits; return original.sleepEx(t, a); }
inline HANDLE WINAPI hCreateFile(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) { if (armed) ++files; return original.createFile(n, a, s, sa, c, f, t); }
inline BOOL WINAPI hWriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD w, LPOVERLAPPED o) { if (armed) ++files; return original.writeFile(h, b, n, w, o); }
inline BOOL WINAPI hReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD r, LPOVERLAPPED o) { if (armed) ++files; return original.readFile(h, b, n, r, o); }
inline LRESULT WINAPI hSendMessage(HWND h, UINT m, WPARAM w, LPARAM l) { if (armed) ++windows; return original.sendMessage(h, m, w, l); }
inline BOOL WINAPI hPostMessage(HWND h, UINT m, WPARAM w, LPARAM l) { if (armed) ++windows; return original.postMessage(h, m, w, l); }
inline BOOL WINAPI hInvalidateRect(HWND h, const RECT* r, BOOL e) { if (armed) ++windows; return original.invalidateRect(h, r, e); }

// Returns the number of patched import slots.
inline int install(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    struct Entry { const char* name; void* hook; void** saved; };
    const Entry entries[]{
        {"HeapAlloc", reinterpret_cast<void*>(&hHeapAlloc), reinterpret_cast<void**>(&original.heapAlloc)},
        {"HeapReAlloc", reinterpret_cast<void*>(&hHeapReAlloc), reinterpret_cast<void**>(&original.heapReAlloc)},
        {"HeapFree", reinterpret_cast<void*>(&hHeapFree), reinterpret_cast<void**>(&original.heapFree)},
        {"VirtualAlloc", reinterpret_cast<void*>(&hVirtualAlloc), reinterpret_cast<void**>(&original.virtualAlloc)},
        {"EnterCriticalSection", reinterpret_cast<void*>(&hEnterCriticalSection), reinterpret_cast<void**>(&original.enterCriticalSection)},
        {"AcquireSRWLockExclusive", reinterpret_cast<void*>(&hAcquireExclusive), reinterpret_cast<void**>(&original.acquireExclusive)},
        {"AcquireSRWLockShared", reinterpret_cast<void*>(&hAcquireShared), reinterpret_cast<void**>(&original.acquireShared)},
        {"WaitForSingleObject", reinterpret_cast<void*>(&hWaitSingle), reinterpret_cast<void**>(&original.waitSingle)},
        {"WaitForSingleObjectEx", reinterpret_cast<void*>(&hWaitSingleEx), reinterpret_cast<void**>(&original.waitSingleEx)},
        {"WaitForMultipleObjects", reinterpret_cast<void*>(&hWaitMultiple), reinterpret_cast<void**>(&original.waitMultiple)},
        {"Sleep", reinterpret_cast<void*>(&hSleep), reinterpret_cast<void**>(&original.sleep)},
        {"SleepEx", reinterpret_cast<void*>(&hSleepEx), reinterpret_cast<void**>(&original.sleepEx)},
        {"CreateFileW", reinterpret_cast<void*>(&hCreateFile), reinterpret_cast<void**>(&original.createFile)},
        {"WriteFile", reinterpret_cast<void*>(&hWriteFile), reinterpret_cast<void**>(&original.writeFile)},
        {"ReadFile", reinterpret_cast<void*>(&hReadFile), reinterpret_cast<void**>(&original.readFile)},
        {"SendMessageW", reinterpret_cast<void*>(&hSendMessage), reinterpret_cast<void**>(&original.sendMessage)},
        {"PostMessageW", reinterpret_cast<void*>(&hPostMessage), reinterpret_cast<void**>(&original.postMessage)},
        {"InvalidateRect", reinterpret_cast<void*>(&hInvalidateRect), reinterpret_cast<void**>(&original.invalidateRect)},
    };
    int patched = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            for (const auto& e : entries) {
                if (std::strcmp(name, e.name)) continue;
                DWORD old = 0;
                VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                if (!*e.saved) *e.saved = reinterpret_cast<void*>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<ULONG_PTR>(e.hook);
                VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
                ++patched;
            }
        }
    }
    return patched;
}
inline void reset() { heap = locks = waits = files = windows = 0; }
inline int total() { return heap + locks + waits + files + windows; }
inline std::string report() {
    return "heap=" + std::to_string(heap.load()) + " locks=" + std::to_string(locks.load()) + " waits=" + std::to_string(waits.load()) +
           " files=" + std::to_string(files.load()) + " windows=" + std::to_string(windows.load());
}
struct Armed {
    Armed() { armed = true; }
    ~Armed() { armed = false; }
};
}  // namespace rtguard
