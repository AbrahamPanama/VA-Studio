// SPDX-License-Identifier: GPL-2.0-or-later
// QA ONLY. Not referenced by stage and never shipped. Writes fake dumps only below st-e-qa.
#include <windows.h>
#include <cstdlib>
#include <string>
int main() {
    char value[64]{};
    GetEnvironmentVariableA("ST_E_EXIT_CODE", value, sizeof(value));
    DWORD code = static_cast<DWORD>(std::strtoull(value, nullptr, 0));
    value[0] = 0;
    GetEnvironmentVariableA("ST_E_DUMP_BYTES", value, sizeof(value));
    auto bytes = std::strtoull(value, nullptr, 0);
    if (bytes) {
        wchar_t root[32768];
        auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", root, 32768);
        std::wstring dir = n && n < 32768 ? std::wstring(root, n) : L"";
        // Refuse writes outside this task's disposable directory.
        if (dir != L"C:/vacards/st-e-qa/local" && dir != L"C:\\vacards\\st-e-qa\\local") ExitProcess(91);
        dir += L"/CrashDumps";
        CreateDirectoryW(dir.c_str(), nullptr);
        auto name = dir + L"/inkscape.exe." + std::to_wstring(GetCurrentProcessId()) + L".dmp";
        HANDLE file = CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) ExitProcess(92);
        LARGE_INTEGER size{}; size.QuadPart = bytes;
        if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) ExitProcess(93);
        value[0] = 0;
        GetEnvironmentVariableA("ST_E_STALE_DUMP", value, sizeof(value));
        if (value[0] == '1') {
            FILETIME old{1, 0};
            if (!SetFileTime(file, &old, nullptr, &old)) ExitProcess(94);
        }
        CloseHandle(file);
    }
    ExitProcess(code);
}
