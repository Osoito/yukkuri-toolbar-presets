/* Launcher and injector for the toolbar preset mod.
 *
 * Runs the Chinese-translation launcher exactly as before, waits for the game
 * window to come up, then loads the mod DLL into it. Attaching to an already
 * running game works too.
 *
 * Both this and the DLL are 32-bit, matching the game, which keeps the
 * injection a plain LoadLibraryW in a remote thread.
 */
#include <windows.h>
#include <tlhelp32.h>

#include <stdarg.h>
#include <stdio.h>

/* Escaped so the source stays pure ASCII. */
static const wchar_t GAME_EXE[] = L"\u3086\u3063\u304F\u308A\u516B\u666F.exe";
static const wchar_t LAUNCHER_EXE[] = L"1111\u5F00\u59CB\u6E38\u620F_CN.exe";
static const wchar_t MOD_DLL[] = L"YukkuriToolbarMod.dll";

#define WAIT_SECONDS 120

/* wprintf renders %ls through the C locale, which drops the Chinese and
 * Japanese parts of these paths and makes diagnostics lie about what was
 * actually tried. Format wide, then emit UTF-8 to a UTF-8 console. */
static void say(const wchar_t *format, ...)
{
    wchar_t wide[1024];
    char utf8[3072];
    va_list args;

    va_start(args, format);
    _vsnwprintf(wide, 1023, format, args);
    va_end(args);
    wide[1023] = 0;

    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), NULL,
                            NULL))
        fputs(utf8, stdout);
}

/* Strips the last component from a path that ends in a backslash. */
static int go_up(wchar_t *dir)
{
    int len = lstrlenW(dir);
    wchar_t *slash;

    if (len < 2)
        return 0;
    dir[len - 1] = 0;
    slash = wcsrchr(dir, L'\\');
    if (!slash)
        return 0;
    slash[1] = 0;
    return 1;
}

static int has_file(const wchar_t *dir, const wchar_t *name)
{
    wchar_t probe[MAX_PATH];

    _snwprintf(probe, MAX_PATH, L"%ls%ls", dir, name);
    probe[MAX_PATH - 1] = 0;
    return GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES;
}

/* This program is easy to run from the build directory by mistake, so look for
 * the game upwards from wherever it happens to live instead of assuming it is
 * right next to us. The game executable counts as well as the launcher, so a
 * renamed launcher still leaves the folder identifiable.
 * Returns 1 and fills out with a trailing backslash. */
static int locate_game_dir(const wchar_t *start, wchar_t *out)
{
    wchar_t dir[MAX_PATH];
    int level;

    lstrcpynW(dir, start, MAX_PATH);
    for (level = 0; level < 5; level++) {
        if (has_file(dir, LAUNCHER_EXE) || has_file(dir, GAME_EXE)) {
            lstrcpynW(out, dir, MAX_PATH);
            return 1;
        }
        if (!go_up(dir))
            break;
    }
    return 0;
}

typedef struct {
    DWORD pid;
    HWND window;
} WindowSearch;

static BOOL CALLBACK find_window_proc(HWND window, LPARAM param)
{
    WindowSearch *search = (WindowSearch *)param;
    DWORD pid = 0;

    GetWindowThreadProcessId(window, &pid);
    if (pid == search->pid && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == NULL) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

static HWND main_window_of(DWORD pid)
{
    WindowSearch search;
    search.pid = pid;
    search.window = NULL;
    EnumWindows(find_window_proc, (LPARAM)&search);
    return search.window;
}

/* Returns a pid we can actually open. A crashed instance of the game can linger
 * at a higher integrity level and must be skipped rather than reported. */
static DWORD find_game(void)
{
    PROCESSENTRY32W entry;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;

    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            HANDLE probe;
            if (lstrcmpiW(entry.szExeFile, GAME_EXE) != 0)
                continue;
            probe = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE,
                                entry.th32ProcessID);
            if (probe) {
                CloseHandle(probe);
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

static int already_injected(DWORD pid)
{
    MODULEENTRY32W entry;
    HANDLE snapshot;
    int found = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (lstrcmpiW(entry.szModule, MOD_DLL) == 0) {
                found = 1;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

/* CreateProcessW is picky about which of its two ways of naming a program it
 * accepts, and which one works has turned out to depend on the environment, so
 * try the application-name form first and fall back to a quoted command line.
 */
static int start_launcher(const wchar_t *launcher_path, const wchar_t *work_dir)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION info;
    wchar_t command[MAX_PATH + 4];
    DWORD error;

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);

    say(L"starting %ls\n", launcher_path);
    if (GetFileAttributesW(launcher_path) == INVALID_FILE_ATTRIBUTES) {
        say(L"that file does not exist (error %lu)\n", GetLastError());
        return 0;
    }

    if (CreateProcessW(launcher_path, NULL, NULL, NULL, FALSE, 0, NULL,
                       work_dir, &startup, &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return 1;
    }
    error = GetLastError();
    say(L"CreateProcessW by name failed: %lu, retrying as a command line\n",
            error);

    _snwprintf(command, MAX_PATH + 4, L"\"%ls\"", launcher_path);
    command[MAX_PATH + 3] = 0;
    if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, work_dir,
                       &startup, &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return 1;
    }

    say(L"could not start the launcher: %lu\n", GetLastError());
    say(L"start it yourself, then run this program again to attach\n");
    return 0;
}

static int inject(DWORD pid, const wchar_t *dll_path)
{
    HANDLE process, thread;
    SIZE_T bytes = (lstrlenW(dll_path) + 1) * sizeof(wchar_t);
    void *remote;
    FARPROC load_library;
    DWORD exit_code = 0;
    int ok = 0;

    process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                          PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                          PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        say(L"OpenProcess failed: %lu\n", GetLastError());
        return 0;
    }

    remote = VirtualAllocEx(process, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (!remote) {
        say(L"VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 0;
    }

    if (WriteProcessMemory(process, remote, dll_path, bytes, NULL)) {
        /* Same bitness and the same kernel32 base, so this address is valid in
         * the target as well. */
        load_library = GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                      "LoadLibraryW");
        thread = CreateRemoteThread(process, NULL, 0,
                                    (LPTHREAD_START_ROUTINE)load_library,
                                    remote, 0, NULL);
        if (thread) {
            WaitForSingleObject(thread, 10000);
            GetExitCodeThread(thread, &exit_code);
            CloseHandle(thread);
            ok = exit_code != 0;
            if (!ok)
                say(L"LoadLibraryW returned NULL in the game process\n");
        } else {
            say(L"CreateRemoteThread failed: %lu\n", GetLastError());
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

int main(void)
{
    wchar_t own_path[MAX_PATH];
    wchar_t game_dir[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    wchar_t launcher_path[MAX_PATH];
    wchar_t *slash;
    DWORD pid;
    int waited;

    SetConsoleOutputCP(CP_UTF8);

    if (!GetModuleFileNameW(NULL, own_path, MAX_PATH))
        return 1;
    slash = wcsrchr(own_path, L'\\');
    if (slash)
        slash[1] = 0;

    if (!locate_game_dir(own_path, game_dir)) {
        say(L"could not find the game in %ls or any folder above it\n"
            L"put this program in the folder that contains %ls\n",
            own_path, GAME_EXE);
        system("pause");
        return 1;
    }

    _snwprintf(launcher_path, MAX_PATH, L"%ls%ls", game_dir, LAUNCHER_EXE);
    launcher_path[MAX_PATH - 1] = 0;

    /* Prefer the DLL sitting next to this program, so a freshly built copy in
     * the build directory is used when run from there. */
    _snwprintf(dll_path, MAX_PATH, L"%ls%ls", own_path, MOD_DLL);
    dll_path[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) {
        _snwprintf(dll_path, MAX_PATH, L"%ls%ls", game_dir, MOD_DLL);
        dll_path[MAX_PATH - 1] = 0;
    }
    if (GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) {
        say(L"%ls not found next to this program or in %ls\n", MOD_DLL,
            game_dir);
        system("pause");
        return 1;
    }
    say(L"game folder: %ls\n", game_dir);

    pid = find_game();
    if (!pid) {
        if (!start_launcher(launcher_path, game_dir)) {
            system("pause");
            return 1;
        }
    } else {
        say(L"attaching to the running game\n");
    }

    /* Wait for the game to exist and put a window up before injecting, so the
     * loader is done and the translation hook is already in place. */
    for (waited = 0; waited < WAIT_SECONDS * 4; waited++) {
        pid = find_game();
        if (pid && main_window_of(pid))
            break;
        Sleep(250);
    }
    if (!pid || !main_window_of(pid)) {
        say(L"gave up waiting for the game window\n");
        system("pause");
        return 1;
    }

    if (already_injected(pid)) {
        say(L"mod already loaded in pid %lu\n", pid);
        return 0;
    }

    /* Give HSP a moment to finish its own start-up before adding a thread. */
    Sleep(1500);

    if (!inject(pid, dll_path)) {
        say(L"injection failed\n");
        system("pause");
        return 1;
    }

    say(L"mod loaded into pid %lu\n", pid);
    return 0;
}
