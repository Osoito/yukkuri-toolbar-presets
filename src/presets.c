#include "presets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void presets_init(PresetFile *out)
{
    memset(out, 0, sizeof(*out));
}

/* Captured from a configured game, so every action ID here is one the game
 * itself produced. */
static const struct {
    const wchar_t *name;
    int slots[SLOTS_COUNT];
} SHIPPED_PRESETS[] = {
    {L"优良培育",            {1, 500, 5, 200, 1101, 1020, 1001, 400, 450}},
    {L"对待下种就该这样做！", {2, 503, 1100, 110, 150, 511, 1001, 80, 502}},
};

void presets_defaults(PresetFile *out)
{
    size_t i;

    presets_init(out);
    for (i = 0; i < sizeof(SHIPPED_PRESETS) / sizeof(SHIPPED_PRESETS[0]); i++) {
        Preset *preset = &out->items[i];
        lstrcpynW(preset->name, SHIPPED_PRESETS[i].name, PRESETS_NAME_MAX);
        memcpy(preset->slots, SHIPPED_PRESETS[i].slots, sizeof(preset->slots));
        preset->used = 1;
    }
    /* Nothing has been applied yet, so no row is marked active. */
    out->last = 0;
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
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) {
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

/* Parses "1,2,3,4,5,6,7,8,9" and only accepts a complete set. */
static int parse_slots(const char *text, int out[SLOTS_COUNT])
{
    int n = 0;
    const char *p = text;

    while (*p && n < SLOTS_COUNT) {
        char *end;
        long value = strtol(p, &end, 10);
        if (end == p)
            return 0;
        out[n++] = (int)value;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    return n == SLOTS_COUNT;
}

int presets_load(const wchar_t *path, PresetFile *out)
{
    DWORD size = 0;
    char *data = read_all(path, &size);
    char *line, *next;
    int section = -1;          /* 0-based preset index, -1 = none/meta */

    presets_init(out);
    if (!data)
        return 0;

    line = data;
    /* Skip a UTF-8 BOM if the file was round-tripped through an editor. */
    if (size >= 3 && (unsigned char)line[0] == 0xEF &&
        (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        line += 3;

    for (; line && *line; line = next) {
        char *eq;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        trim(line);

        if (line[0] == '[') {
            section = -1;
            if (strncmp(line, "[preset", 7) == 0) {
                int n = atoi(line + 7);
                if (n >= 1 && n <= PRESETS_COUNT)
                    section = n - 1;
            }
            continue;
        }

        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        trim(line);

        if (section < 0) {
            if (strcmp(line, "last") == 0)
                out->last = atoi(eq + 1);
            continue;
        }

        if (strcmp(line, "name") == 0) {
            MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1,
                                out->items[section].name, PRESETS_NAME_MAX);
            out->items[section].name[PRESETS_NAME_MAX - 1] = 0;
        } else if (strcmp(line, "slots") == 0) {
            if (parse_slots(eq + 1, out->items[section].slots))
                out->items[section].used = 1;
        }
    }

    free(data);
    if (out->last < 0 || out->last > PRESETS_COUNT)
        out->last = 0;
    return 1;
}

int presets_save(const wchar_t *path, const PresetFile *in)
{
    HANDLE file;
    char buffer[8192];
    int len = 0;
    int i;
    DWORD written = 0;
    BOOL ok;

    /* A BOM keeps Notepad from guessing the encoding of the preset names. */
    len += sprintf(buffer + len, "\xEF\xBB\xBF");
    len += sprintf(buffer + len,
                   "; Yukkuri toolbar presets. Rename a preset by editing its\n"
                   "; name= line; slot values are captured from the game.\n\n");
    len += sprintf(buffer + len, "[meta]\nlast=%d\n", in->last);

    for (i = 0; i < PRESETS_COUNT; i++) {
        char name[PRESETS_NAME_MAX * 4];
        int k;

        if (!in->items[i].used)
            continue;

        WideCharToMultiByte(CP_UTF8, 0, in->items[i].name, -1, name,
                            sizeof(name), NULL, NULL);
        len += sprintf(buffer + len, "\n[preset%d]\nname=%s\nslots=", i + 1,
                       name);
        for (k = 0; k < SLOTS_COUNT; k++)
            len += sprintf(buffer + len, "%s%d", k ? "," : "",
                           in->items[i].slots[k]);
        len += sprintf(buffer + len, "\n");
    }

    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(file, buffer, (DWORD)len, &written, NULL);
    CloseHandle(file);
    return ok && written == (DWORD)len;
}
