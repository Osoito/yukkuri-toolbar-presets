/* Locating and editing the nine top-launcher slots of Yukkuri Hakkei.
 *
 * The slots are the HSP variables act_select_key_confing_1..9, followed in the
 * variable table by act_select_key_confing_a..z.  All 35 are scalar ints whose
 * data blocks sit SLOTS_STRIDE bytes apart, descending.  The base address moves
 * every run, so it has to be found at runtime.
 *
 * Every entry point takes a process handle so the same code can run injected
 * (GetCurrentProcess) or from an external test harness.
 */
#ifndef SLOTS_H
#define SLOTS_H

#include <windows.h>

#define SLOTS_COUNT 9
#define SLOTS_LETTER_COUNT 26
#define SLOTS_BLOCK_COUNT (SLOTS_COUNT + SLOTS_LETTER_COUNT)
#define SLOTS_STRIDE 88

typedef struct {
    DWORD pval;   /* address of slot 1's PVal record, used for revalidation */
    DWORD data;   /* address of slot 1's four value bytes */
} SlotsLocation;

/* Set to 1 to trace the scan on stderr. */
extern int slots_debug;

/* Read act_select_key_confing_a..z out of key_save. Returns 1 on success. */
int slots_letters_from_key_save(const wchar_t *key_save_path,
                                int out[SLOTS_LETTER_COUNT]);

/* How distinctive the letter values are, as a count of distinct non-zero
 * entries. The block is found by matching these against memory, so a player who
 * has never assigned any letter shortcut leaves nothing to match on and
 * slots_locate cannot tell the candidates apart. */
int slots_letters_strength(const int letters[SLOTS_LETTER_COUNT]);

/* Find slot 1. Returns 1 on a single unambiguous match, 0 otherwise.
 * Fails while the game is still on the title screen: HSP has not allocated the
 * variables yet, so callers should retry. */
int slots_locate(HANDLE process, const int letters[SLOTS_LETTER_COUNT],
                 SlotsLocation *out);

/* Structural recheck of a previously found location. Deliberately ignores the
 * stored values, which change whenever the player edits the toolbar. */
int slots_valid(HANDLE process, const SlotsLocation *loc);

int slots_read(HANDLE process, const SlotsLocation *loc, int out[SLOTS_COUNT]);
int slots_write(HANDLE process, const SlotsLocation *loc,
                const int values[SLOTS_COUNT]);

#endif /* SLOTS_H */
