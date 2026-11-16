/* Exercises slots.c against the running game from the outside.
 *
 * Same code path the injected DLL uses, only with a remote process handle, so a
 * pass here means the locator itself is sound before any injection happens.
 *
 *   slots_test.exe                 locate and print
 *   slots_test.exe 501 450 ... x9  locate and write nine values
 */
#include <windows.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>

#include "slots.h"

/* Escaped so the source stays pure ASCII: "ゆっくり八景.exe". */
static const wchar_t GAME_EXE[] = L"\u3086\u3063\u304F\u308A\u516B\u666F.exe";

static DWORD find_game(void)
{
    PROCESSENTRY32W entry;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD pid = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            HANDLE probe;
            if (lstrcmpiW(entry.szExeFile, GAME_EXE) != 0)
                continue;
            /* A crashed instance can linger and refuse to open; skip it. */
            probe = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, entry.th32ProcessID);
            if (probe) {
                CloseHandle(probe);
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }

    CloseHandle(snap);
    return pid;
}

int main(int argc, char **argv)
{
    int letters[SLOTS_LETTER_COUNT];
    slots_debug = 1;
    int values[SLOTS_COUNT];
    SlotsLocation loc;
    HANDLE process;
    DWORD pid;
    int k;

    if (!slots_letters_from_key_save(L"key_save", letters)) {
        printf("could not read key_save (run from the game directory)\n");
        return 1;
    }
    printf("key_save a..z:");
    for (k = 0; k < SLOTS_LETTER_COUNT; k++)
        printf(" %d", letters[k]);
    printf("\n");

    pid = find_game();
    if (!pid) {
        printf("game is not running\n");
        return 1;
    }
    printf("pid: %lu\n", pid);

    process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                          PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!process) {
        printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }

    if (!slots_locate(process, letters, &loc)) {
        printf("locate failed (title screen, or key_save is out of sync)\n");
        CloseHandle(process);
        return 2;
    }
    printf("slot1 pval=0x%08lX data=0x%08lX\n", loc.pval, loc.data);
    printf("valid: %d\n", slots_valid(process, &loc));

    if (!slots_read(process, &loc, values)) {
        printf("read failed\n");
        CloseHandle(process);
        return 3;
    }
    printf("slots 1..9:");
    for (k = 0; k < SLOTS_COUNT; k++)
        printf(" %d", values[k]);
    printf("\n");

    if (argc == 1 + SLOTS_COUNT) {
        for (k = 0; k < SLOTS_COUNT; k++)
            values[k] = atoi(argv[1 + k]);
        if (!slots_write(process, &loc, values)) {
            printf("write failed\n");
            CloseHandle(process);
            return 4;
        }
        slots_read(process, &loc, values);
        printf("after write:");
        for (k = 0; k < SLOTS_COUNT; k++)
            printf(" %d", values[k]);
        printf("\n");
    }

    CloseHandle(process);
    return 0;
}
