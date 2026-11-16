/* Prints the paths inject.c builds, without starting anything. */
#include <windows.h>

#include <stdio.h>

static const wchar_t GAME_EXE[] = L"\u3086\u3063\u304F\u308A\u516B\u666F.exe";
static const wchar_t LAUNCHER_EXE[] = L"1111\u5F00\u59CB\u6E38\u620F_CN.exe";
static const wchar_t MOD_DLL[] = L"YukkuriToolbarMod.dll";

static void show(const char *label, const wchar_t *value)
{
    char utf8[MAX_PATH * 3];
    int i;

    WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), NULL, NULL);
    printf("%-14s %s\n", label, utf8);
    printf("%-14s ", "  codepoints");
    for (i = 0; value[i] && i < 60; i++)
        printf("%04X ", (unsigned)value[i]);
    printf("\n%-14s %s\n", "  exists",
           GetFileAttributesW(value) == INVALID_FILE_ATTRIBUTES ? "NO" : "yes");
}

int main(void)
{
    wchar_t own_path[MAX_PATH];
    wchar_t built[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, own_path, MAX_PATH);
    show("own exe", own_path);

    slash = wcsrchr(own_path, L'\\');
    if (slash)
        slash[1] = 0;
    show("own dir", own_path);

    _snwprintf(built, MAX_PATH, L"%s%s", own_path, MOD_DLL);
    built[MAX_PATH - 1] = 0;
    show("dll", built);

    _snwprintf(built, MAX_PATH, L"%s%s", own_path, LAUNCHER_EXE);
    built[MAX_PATH - 1] = 0;
    show("launcher", built);

    _snwprintf(built, MAX_PATH, L"%s%s", own_path, GAME_EXE);
    built[MAX_PATH - 1] = 0;
    show("game", built);

    /* Try the exact call inject.c makes, but suspended so nothing actually
     * runs, then kill it again. */
    _snwprintf(built, MAX_PATH, L"%s%s", own_path, LAUNCHER_EXE);
    built[MAX_PATH - 1] = 0;
    {
        STARTUPINFOW startup;
        PROCESS_INFORMATION info;

        ZeroMemory(&startup, sizeof(startup));
        startup.cb = sizeof(startup);
        if (CreateProcessW(built, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                           NULL, own_path, &startup, &info)) {
            printf("\nCreateProcessW: OK (pid %lu), terminating\n",
                   info.dwProcessId);
            TerminateProcess(info.hProcess, 0);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        } else {
            printf("\nCreateProcessW failed: %lu\n", GetLastError());
        }
    }

    return 0;
}
