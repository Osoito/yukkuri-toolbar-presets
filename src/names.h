/* Optional action-ID to label table.
 *
 * The game keeps its ID-to-name mapping inside the encrypted start.ax
 * bytecode, so there is no way to derive these automatically. Instead the mod
 * seeds a plain text file with every ID the player actually uses and lets them
 * fill in the labels once.
 */
#ifndef NAMES_H
#define NAMES_H

#include <windows.h>

#define NAMES_MAX 256
#define NAMES_LABEL_MAX 24

void names_load(const wchar_t *path);

/* The label for an ID, or NULL when it has not been filled in. */
const wchar_t *names_lookup(int id);

/* Adds any of the given IDs that the file does not mention yet, as blank
 * entries ready to be filled in. Rewrites the file only when something was
 * added. */
void names_note(const wchar_t *path, const int *ids, int count);

#endif /* NAMES_H */
