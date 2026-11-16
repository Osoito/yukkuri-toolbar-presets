#include "slots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HSP3 PVal, 32-bit build. Confirmed against the running game: records sit 48
 * bytes apart in the variable table. */
#pragma pack(push, 1)
typedef struct {
    unsigned short flag;      /* 4 = int */
    unsigned short mode;      /* 1 = allocated */
    int len[5];
    int size;                 /* 4 for a scalar int */
    DWORD pt;                 /* address of the value */
    DWORD master;             /* 0 for a plain variable */
    int support;              /* 9 for these */
    int arraycnt;
    int offset;
} PVal;
#pragma pack(pop)

/* flag, mode, len[5] and size are identical for every scalar int variable, so
 * their 28 leading bytes make a needle that finds the whole variable table. */
#define PVAL_PREFIX_LEN 28
#define PVAL_SIZE 48

static const unsigned char PVAL_PREFIX[PVAL_PREFIX_LEN] = {
    0x04, 0x00,             /* flag = 4   */
    0x01, 0x00,             /* mode = 1   */
    0x01, 0x00, 0x00, 0x00, /* len[0] = 1 */
    0x01, 0x00, 0x00, 0x00, /* len[1] = 1 */
    0x00, 0x00, 0x00, 0x00, /* len[2] = 0 */
    0x00, 0x00, 0x00, 0x00, /* len[3] = 0 */
    0x00, 0x00, 0x00, 0x00, /* len[4] = 0 */
    0x04, 0x00, 0x00, 0x00  /* size   = 4 */
};

int slots_debug = 0;

#define SCAN_CHUNK (4 * 1024 * 1024)
#define WRITABLE (PAGE_READWRITE | PAGE_WRITECOPY | \
                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

typedef struct {
    DWORD pval;
    DWORD pt;
} Entry;

typedef struct {
    Entry *items;
    size_t count;
    size_t capacity;
} EntryList;

/* ReadProcessMemory on our own process is deliberate: it probes in kernel mode
 * and returns FALSE instead of raising an access violation, so scanning cannot
 * crash the game on a page that went away mid-scan. */
static int read_mem(HANDLE process, DWORD addr, void *dst, SIZE_T size)
{
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, (LPCVOID)(UINT_PTR)addr, dst, size, &got))
        return 0;
    return got == size;
}

static int read_int(HANDLE process, DWORD addr, int *out)
{
    return read_mem(process, addr, out, sizeof(*out));
}

static int entries_push(EntryList *list, DWORD pval, DWORD pt)
{
    if (list->count == list->capacity) {
        size_t want = list->capacity ? list->capacity * 2 : 1024;
        Entry *grown = (Entry *)realloc(list->items, want * sizeof(Entry));
        if (!grown)
            return 0;
        list->items = grown;
        list->capacity = want;
    }
    list->items[list->count].pval = pval;
    list->items[list->count].pt = pt;
    list->count++;
    return 1;
}

/* Collect every scalar int PVal in the process, in ascending address order. */
static int collect_pvals(HANDLE process, EntryList *out)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *buf = (unsigned char *)malloc(SCAN_CHUNK);
    DWORD addr = 0;

    if (!buf)
        return 0;

    while (addr < 0x7FFF0000u &&
           VirtualQueryEx(process, (LPCVOID)(UINT_PTR)addr, &mbi, sizeof(mbi))) {
        DWORD base = (DWORD)(UINT_PTR)mbi.BaseAddress;
        DWORD size = (DWORD)mbi.RegionSize;
        int scannable = mbi.State == MEM_COMMIT &&
                        (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) &&
                        (mbi.Protect & WRITABLE) != 0 &&
                        (mbi.Protect & PAGE_GUARD) == 0;

        if (size == 0)
            break;

        if (scannable) {
            DWORD done = 0;
            while (done < size) {
                /* Overlap by one record so a PVal straddling a chunk boundary
                 * is still seen whole. */
                DWORD want = size - done;
                DWORD i;
                if (want > SCAN_CHUNK)
                    want = SCAN_CHUNK;
                if (!read_mem(process, base + done, buf, want))
                    break;

                for (i = 0; i + PVAL_SIZE <= want; i += 4) {
                    const PVal *p;
                    if (memcmp(buf + i, PVAL_PREFIX, PVAL_PREFIX_LEN) != 0)
                        continue;
                    p = (const PVal *)(buf + i);
                    if (p->master != 0 || p->support != 9 || p->pt == 0)
                        continue;
                    if (out->count &&
                        out->items[out->count - 1].pval >= base + done + i)
                        continue;   /* overlapping view of the same record */
                    if (!entries_push(out, base + done + i, p->pt)) {
                        free(buf);
                        return 0;
                    }
                }

                if (want < SCAN_CHUNK)
                    break;
                done += SCAN_CHUNK - PVAL_SIZE;
            }
        }

        addr = base + size;
    }

    free(buf);
    if (slots_debug)
        fprintf(stderr, "[slots] scalar int PVals: %u\n",
                (unsigned)out->count);
    return 1;
}

int slots_letters_strength(const int letters[SLOTS_LETTER_COUNT])
{
    int distinct = 0;
    int i, j;

    for (i = 0; i < SLOTS_LETTER_COUNT; i++) {
        if (letters[i] == 0)
            continue;
        for (j = 0; j < i; j++) {
            if (letters[j] == letters[i])
                break;
        }
        if (j == i)
            distinct++;
    }
    return distinct;
}

int slots_locate(HANDLE process, const int letters[SLOTS_LETTER_COUNT],
                 SlotsLocation *out)
{
    EntryList entries = {NULL, 0, 0};
    size_t i;
    int found = 0;

    if (!collect_pvals(process, &entries)) {
        free(entries.items);
        return 0;
    }

    /* Walk runs whose data pointers step down by exactly SLOTS_STRIDE; the 35
     * shortcut variables form one such run. */
    i = 0;
    while (i < entries.count) {
        size_t end = i;
        size_t off;

        while (end + 1 < entries.count &&
               entries.items[end + 1].pt == entries.items[end].pt - SLOTS_STRIDE)
            end++;

        if (end - i + 1 >= SLOTS_BLOCK_COUNT) {
            if (slots_debug)
                fprintf(stderr, "[slots] run of %u at pt=0x%08lX\n",
                        (unsigned)(end - i + 1), entries.items[i].pt);
            for (off = i; off + SLOTS_BLOCK_COUNT - 1 <= end; off++) {
                DWORD slot1 = entries.items[off].pt;
                int k, match = 1;
                for (k = 0; k < SLOTS_LETTER_COUNT; k++) {
                    int value = 0;
                    DWORD at = slot1 - SLOTS_STRIDE * (SLOTS_COUNT + k);
                    if (!read_int(process, at, &value) || value != letters[k]) {
                        match = 0;
                        break;
                    }
                }
                if (!match)
                    continue;
                if (found) {
                    /* The same block seen twice is fine; two different blocks
                     * mean the fingerprint is not unique, so refuse to guess. */
                    if (out->data == slot1)
                        continue;
                    if (slots_debug)
                        fprintf(stderr,
                                "[slots] ambiguous: 0x%08lX and 0x%08lX\n",
                                out->data, slot1);
                    free(entries.items);
                    return 0;
                }
                out->pval = entries.items[off].pval;
                out->data = slot1;
                found = 1;
            }
        }
        i = end + 1;
    }

    free(entries.items);
    return found;
}

int slots_valid(HANDLE process, const SlotsLocation *loc)
{
    PVal p;
    int k;

    if (!loc->pval || !loc->data)
        return 0;
    if (!read_mem(process, loc->pval, &p, sizeof(p)))
        return 0;
    if (p.flag != 4 || p.mode != 1 || p.size != 4 || p.support != 9 ||
        p.master != 0 || p.pt != loc->data)
        return 0;

    /* The eight slots after slot 1 must still be laid out at the same stride. */
    for (k = 1; k < SLOTS_COUNT; k++) {
        PVal q;
        if (!read_mem(process, loc->pval + (DWORD)PVAL_SIZE * k, &q, sizeof(q)))
            return 0;
        if (q.flag != 4 || q.size != 4 ||
            q.pt != loc->data - (DWORD)SLOTS_STRIDE * k)
            return 0;
    }
    return 1;
}

int slots_read(HANDLE process, const SlotsLocation *loc, int out[SLOTS_COUNT])
{
    int k;
    for (k = 0; k < SLOTS_COUNT; k++) {
        if (!read_int(process, loc->data - (DWORD)SLOTS_STRIDE * k, &out[k]))
            return 0;
    }
    return 1;
}

int slots_write(HANDLE process, const SlotsLocation *loc,
                const int values[SLOTS_COUNT])
{
    int k;
    for (k = 0; k < SLOTS_COUNT; k++) {
        DWORD at = loc->data - (DWORD)SLOTS_STRIDE * k;
        SIZE_T put = 0;
        if (!WriteProcessMemory(process, (LPVOID)(UINT_PTR)at, &values[k],
                                sizeof(values[k]), &put) ||
            put != sizeof(values[k]))
            return 0;
    }
    return 1;
}

/* ---- key_save parsing -------------------------------------------------- */

/* Container layout is documented in tools/hspv.py. */
#pragma pack(push, 1)
typedef struct {
    char magic[4];            /* "hspv" */
    unsigned short version;
    unsigned short opt;
    unsigned int count;
    unsigned int header_size;  /* offset of the data area */
} HspvHeader;

typedef struct {
    int name_offset;
    int data_offset;
    int reserved[2];
    PVal pval;
} HspvRecord;
#pragma pack(pop)

int slots_letters_from_key_save(const wchar_t *path,
                                int out[SLOTS_LETTER_COUNT])
{
    HANDLE file;
    DWORD size, got = 0;
    unsigned char *data = NULL;
    HspvHeader *head;
    unsigned int i;
    int seen = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    size = GetFileSize(file, NULL);
    if (size != INVALID_FILE_SIZE && size >= sizeof(HspvHeader) &&
        size < 16u * 1024 * 1024) {
        data = (unsigned char *)malloc(size);
        if (data && !ReadFile(file, data, size, &got, NULL))
            got = 0;
    }
    CloseHandle(file);

    if (!data || got != size) {
        free(data);
        return 0;
    }

    head = (HspvHeader *)data;
    if (memcmp(head->magic, "hspv", 4) != 0 ||
        head->header_size > size ||
        (unsigned long long)head->count * sizeof(HspvRecord) + 16 > size) {
        free(data);
        return 0;
    }

    for (i = 0; i < head->count; i++) {
        HspvRecord *rec = (HspvRecord *)(data + 16 + i * sizeof(HspvRecord));
        const char *name = (const char *)(data + head->header_size +
                                          rec->name_offset);
        const char *suffix = "act_select_key_confing_";
        size_t prefix_len = strlen(suffix);
        DWORD value_at = head->header_size + rec->data_offset;
        char letter;
        int index;

        if ((DWORD)rec->name_offset >= size - head->header_size)
            continue;
        if (strncmp(name, suffix, prefix_len) != 0)
            continue;
        letter = name[prefix_len];
        if (letter < 'a' || letter > 'z' || name[prefix_len + 1] != '\0')
            continue;
        if (rec->pval.flag != 4 || rec->pval.size != 4 ||
            value_at + 4 > size)
            continue;

        index = letter - 'a';
        memcpy(&out[index], data + value_at, sizeof(int));
        seen++;
    }

    free(data);
    return seen == SLOTS_LETTER_COUNT;
}
