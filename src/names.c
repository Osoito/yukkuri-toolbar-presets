#include "names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    wchar_t label[NAMES_LABEL_MAX];
} NameEntry;

static NameEntry entries[NAMES_MAX];
static int entry_count;

static int find(int id)
{
    int i;
    for (i = 0; i < entry_count; i++) {
        if (entries[i].id == id)
            return i;
    }
    return -1;
}

const wchar_t *names_lookup(int id)
{
    int at = find(id);
    if (at < 0 || !entries[at].label[0])
        return NULL;
    return entries[at].label;
}

static char *read_all(const wchar_t *path, DWORD *size_out)
{
    HANDLE file;
    DWORD size, got = 0;
    char *data;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return NULL;

    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > 512 * 1024) {
        CloseHandle(file);
        return NULL;
    }
    data = (char *)malloc(size + 1);
    if (!data) {
        CloseHandle(file);
        return NULL;
    }
    if (!ReadFile(file, data, size, &got, NULL) || got != size) {
        free(data);
        CloseHandle(file);
        return NULL;
    }
    CloseHandle(file);
    data[size] = 0;
    *size_out = size;
    return data;
}

static void trim(char *s)
{
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n' ||
                   s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = 0;
}

void names_load(const wchar_t *path)
{
    DWORD size = 0;
    char *data = read_all(path, &size);
    char *line, *next;

    entry_count = 0;
    if (!data)
        return;

    line = data;
    if (size >= 3 && (unsigned char)line[0] == 0xEF &&
        (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line += 3;

    for (; line && *line && entry_count < NAMES_MAX; line = next) {
        char *eq;
        int id;

        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        trim(line);
        if (line[0] == ';' || line[0] == '#' || !line[0])
            continue;

        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        id = atoi(line);
        if (find(id) >= 0)
            continue;

        entries[entry_count].id = id;
        entries[entry_count].label[0] = 0;
        MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, entries[entry_count].label,
                            NAMES_LABEL_MAX);
        entries[entry_count].label[NAMES_LABEL_MAX - 1] = 0;
        entry_count++;
    }

    free(data);
}

static int compare_entries(const void *a, const void *b)
{
    return ((const NameEntry *)a)->id - ((const NameEntry *)b)->id;
}

static void write_file(const wchar_t *path)
{
    HANDLE file;
    char *buffer;
    int len = 0;
    int i;
    DWORD written = 0;
    size_t capacity = 4096 + (size_t)entry_count * 128;

    buffer = (char *)malloc(capacity);
    if (!buffer)
        return;

    len += sprintf(buffer + len, "\xEF\xBB\xBF");
    len += sprintf(buffer + len,
        "; Action names for the toolbar preset panel.\n"
        "; The game keeps its own ID-to-name table inside encrypted bytecode,\n"
        "; so these cannot be filled in automatically. Every ID you actually\n"
        "; use is added here with a blank label; write the name after the =\n"
        "; sign and it will show up in the F9 panel instead of the number.\n"
        "; Keep labels short - the panel truncates long rows.\n\n");

    for (i = 0; i < entry_count; i++) {
        char label[NAMES_LABEL_MAX * 4] = "";
        if (entries[i].label[0])
            WideCharToMultiByte(CP_UTF8, 0, entries[i].label, -1, label,
                                sizeof(label), NULL, NULL);
        len += sprintf(buffer + len, "%d=%s\n", entries[i].id, label);
    }

    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, buffer, (DWORD)len, &written, NULL);
        CloseHandle(file);
    }
    free(buffer);
}

void names_note(const wchar_t *path, const int *ids, int count)
{
    int added = 0;
    int i;

    /* Labels are opt-in: the panel shows plain action IDs unless the player has
     * created this file, and a default install should not grow a stray one. */
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return;

    for (i = 0; i < count; i++) {
        if (ids[i] == 0)          /* an empty slot needs no label */
            continue;
        if (find(ids[i]) >= 0 || entry_count >= NAMES_MAX)
            continue;
        entries[entry_count].id = ids[i];
        entries[entry_count].label[0] = 0;
        entry_count++;
        added = 1;
    }

    if (!added)
        return;
    qsort(entries, entry_count, sizeof(entries[0]), compare_entries);
    write_file(path);
}
