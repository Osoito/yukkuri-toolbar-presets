/* Toolbar presets, stored next to the game as a hand-editable UTF-8 ini.
 *
 * Slot values are only ever captured from the running game, never typed in, so
 * an invalid action ID can never reach the game through a preset.
 */
#ifndef PRESETS_H
#define PRESETS_H

#include <windows.h>

#include "slots.h"

#define PRESETS_COUNT 8
#define PRESETS_NAME_MAX 48

typedef struct {
    int used;
    wchar_t name[PRESETS_NAME_MAX];
    int slots[SLOTS_COUNT];
} Preset;

typedef struct {
    Preset items[PRESETS_COUNT];
    int last;                  /* 1-based index of the preset last applied */
} PresetFile;

void presets_init(PresetFile *out);

/* Two ready-made presets, so a fresh install is useful before the player has
 * captured anything. Only used when no preset file exists yet. */
void presets_defaults(PresetFile *out);

int presets_load(const wchar_t *path, PresetFile *out);
int presets_save(const wchar_t *path, const PresetFile *in);

#endif /* PRESETS_H */
