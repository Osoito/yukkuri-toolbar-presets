/* Yukkuri toolbar preset mod.
 *
 * F1..F8         switch the top launcher to preset 1..8
 * Shift+F1..F8   capture the current launcher into that preset
 * F9             show or hide the preset panel
 *
 * Interaction is keyboard-only on purpose. HSP reads input with
 * GetAsyncKeyState, which sees physical key and button state regardless of
 * message routing, so a clickable panel would feed every click straight through
 * to the game as well. Function keys are the one range the game never polls.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "names.h"
#include "overlay.h"
#include "presets.h"
#include "slots.h"

#define TOAST_MS 2500
#define LINE_HEIGHT 26
#define OVERLAY_PAD 12
#define OVERLAY_BORDER 2
#define PANEL_LINES (PRESETS_COUNT + 4)

/* Sampled from the game's own launcher artwork (pict/system/menu/sc.png) so the
 * overlay sits in the same palette as the UI it belongs to. */
#define ACCENT RGB(244, 255, 103)
#define ACCENT_DIM RGB(191, 218, 63)
#define TEXT_NORMAL RGB(228, 228, 214)
#define TEXT_EMPTY RGB(118, 122, 96)
#define TEXT_WARN RGB(255, 170, 90)

/* How wide a preset row may get, in columns where a CJK glyph counts as two. */
#define ROW_COLUMNS 66

/* Distinct letter-shortcut values needed before the fingerprint can pick the
 * toolbar block out of the variable table. */
#define MIN_FINGERPRINT 4

static wchar_t game_dir[MAX_PATH];
static wchar_t presets_path[MAX_PATH];
static wchar_t names_path[MAX_PATH];
static wchar_t key_save_path[MAX_PATH];
static wchar_t log_path[MAX_PATH];

static PresetFile presets;
static SlotsLocation location;
static volatile LONG located;
static volatile LONG relocate_wanted = 1;
static volatile LONG locator_running = 1;
static volatile LONG weak_fingerprint;

static HFONT overlay_font;
static wchar_t toast[160];
static DWORD toast_until;
static int panel_open;
static int hotkey_was_down[9];

static void mod_log(const char *format, ...)
{
    HANDLE file;
    char line[512];
    DWORD written;
    va_list args;
    int len;

    va_start(args, format);
    len = vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (len < 0)
        return;
    line[len++] = '\n';

    file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    WriteFile(file, line, (DWORD)len, &written, NULL);
    CloseHandle(file);
}

/* vsnprintf renders %ls through the C locale, which cannot encode the game
 * directory, so wide strings get their own converting helper. */
static void mod_log_wide(const char *label, const wchar_t *text)
{
    char utf8[MAX_PATH * 3];

    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL,
                            NULL))
        mod_log("%s: %s", label, utf8);
}

static void trace_present(const char *which, int x, int y, int w, int h,
                          int sx, int sy, int sw, int sh, DWORD rop)
{
    mod_log("present %s dst=%d,%d %dx%d src=%d,%d %dx%d rop=%08lX", which, x, y,
            w, h, sx, sy, sw, sh, rop);
}

static void set_toast(const wchar_t *format, ...)
{
    va_list args;
    va_start(args, format);
    _vsnwprintf(toast, sizeof(toast) / sizeof(toast[0]) - 1, format, args);
    va_end(args);
    toast[sizeof(toast) / sizeof(toast[0]) - 1] = 0;
    toast_until = GetTickCount() + TOAST_MS;
}

static void build_paths(void)
{
    wchar_t *slash;

    if (!GetModuleFileNameW(NULL, game_dir, MAX_PATH))
        return;
    slash = wcsrchr(game_dir, L'\\');
    if (slash)
        slash[1] = 0;

    _snwprintf(presets_path, MAX_PATH, L"%syk_presets.ini", game_dir);
    _snwprintf(names_path, MAX_PATH, L"%syk_action_names.ini", game_dir);
    _snwprintf(key_save_path, MAX_PATH, L"%skey_save", game_dir);
    _snwprintf(log_path, MAX_PATH, L"%syk_toolbar_mod.log", game_dir);
    presets_path[MAX_PATH - 1] = 0;
    names_path[MAX_PATH - 1] = 0;
    key_save_path[MAX_PATH - 1] = 0;
    log_path[MAX_PATH - 1] = 0;
}

/* ---- slot location ------------------------------------------------------ */

/* Scanning costs a full pass over the game's memory, so it only runs when the
 * location is unknown or has failed revalidation. */
static DWORD WINAPI locator_thread(LPVOID unused)
{
    (void)unused;
    while (locator_running) {
        if (relocate_wanted) {
            int letters[SLOTS_LETTER_COUNT];
            SlotsLocation found;
            int strength;

            if (!slots_letters_from_key_save(key_save_path, letters)) {
                mod_log("key_save unreadable, retrying");
                InterlockedExchange(&located, 0);
                Sleep(2000);
                continue;
            }

            strength = slots_letters_strength(letters);
            if (strength < MIN_FINGERPRINT) {
                if (!weak_fingerprint)
                    mod_log("letter shortcuts give only %d distinct values, "
                            "too few to identify the toolbar block", strength);
                InterlockedExchange(&weak_fingerprint, 1);
                InterlockedExchange(&located, 0);
                Sleep(2000);
                continue;
            }
            InterlockedExchange(&weak_fingerprint, 0);

            if (slots_locate(GetCurrentProcess(), letters, &found)) {
                location = found;
                InterlockedExchange(&located, 1);
                InterlockedExchange(&relocate_wanted, 0);
                mod_log("located slot 1 at 0x%08lX (pval 0x%08lX)", found.data,
                        found.pval);
            } else {
                /* Normal before a save is loaded: HSP has not created the
                 * variables yet. */
                InterlockedExchange(&located, 0);
            }
        }
        Sleep(2000);
    }
    return 0;
}

/* True when the cached location still describes the variable table. */
static int slots_ready(void)
{
    if (!located)
        return 0;
    if (slots_valid(GetCurrentProcess(), &location))
        return 1;
    mod_log("location went stale, rescanning");
    InterlockedExchange(&located, 0);
    InterlockedExchange(&relocate_wanted, 1);
    return 0;
}

/* ---- actions ------------------------------------------------------------ */

static void apply_preset(int index)
{
    Preset *preset = &presets.items[index];

    if (!preset->used) {
        set_toast(L"预设 %d 是空的  (Shift+F%d 保存当前工具栏)", index + 1,
                  index + 1);
        return;
    }
    if (!slots_ready()) {
        set_toast(L"还没定位到工具栏变量，请先进入游戏");
        return;
    }
    if (!slots_write(GetCurrentProcess(), &location, preset->slots)) {
        set_toast(L"写入失败");
        mod_log("slots_write failed for preset %d", index + 1);
        return;
    }
    presets.last = index + 1;
    presets_save(presets_path, &presets);
    set_toast(L"已切换：%s", preset->name);
    mod_log("applied preset %d", index + 1);
}

static void capture_preset(int index)
{
    Preset *preset = &presets.items[index];

    if (!slots_ready()) {
        set_toast(L"还没定位到工具栏变量，请先进入游戏");
        return;
    }
    if (!slots_read(GetCurrentProcess(), &location, preset->slots)) {
        set_toast(L"读取失败");
        return;
    }
    if (!preset->name[0])
        _snwprintf(preset->name, PRESETS_NAME_MAX, L"预设 %d", index + 1);
    preset->used = 1;
    presets.last = index + 1;

    /* Give the player blank lines to fill in for any action they have not
     * labelled yet. */
    names_note(names_path, preset->slots, SLOTS_COUNT);

    if (presets_save(presets_path, &presets))
        set_toast(L"已保存到 F%d：%s", index + 1, preset->name);
    else
        set_toast(L"保存失败，检查 yk_presets.ini 是否只读");
    mod_log("captured preset %d", index + 1);
}

static void poll_hotkeys(void)
{
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int i;

    /* Only react while the game actually has focus, so these keys stay free in
     * every other application. */
    if (GetForegroundWindow() != overlay_window()) {
        memset(hotkey_was_down, 0, sizeof(hotkey_was_down));
        return;
    }

    for (i = 0; i < 9; i++) {
        int down = (GetAsyncKeyState(VK_F1 + i) & 0x8000) != 0;
        int pressed = down && !hotkey_was_down[i];
        hotkey_was_down[i] = down;

        if (!pressed)
            continue;
        if (i == 8)
            panel_open = !panel_open;
        else if (shift)
            capture_preset(i);
        else
            apply_preset(i);
    }
}

/* ---- drawing ------------------------------------------------------------ */

typedef struct {
    wchar_t text[256];
    COLORREF colour;
} OverlayLine;

static HDC buffer_dc;
static HBITMAP buffer_bitmap;
static HBITMAP buffer_previous;
static int buffer_width;
static int buffer_height;

/* Regions of the game's own surface that the current frame overwrote, kept so
 * they can be put back the moment the frame has been presented. */
#define MAX_SAVED 2
typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HBITMAP previous;
    int capacity_w, capacity_h;
    int x, y, w, h;
} SavedRegion;

static SavedRegion saved[MAX_SAVED];
static int saved_count;

static void ensure_font(void);

/* The same list, in the same order, that YukkuriHook.dll uses when it swaps the
 * game's Japanese fonts for a CJK-capable one. Following it means the overlay
 * ends up with whichever face the translated text is already using. */
static const wchar_t *const FONT_CANDIDATES[] = {
    L"SimHei", L"Microsoft YaHei", L"Microsoft JhengHei", L"PMingLiU",
    L"SimSun", L"MS UI Gothic", L"Microsoft Sans Serif"
};

static int CALLBACK count_font(const LOGFONTW *logfont,
                               const TEXTMETRICW *metrics, DWORD type,
                               LPARAM param)
{
    (void)logfont;
    (void)metrics;
    (void)type;
    *(int *)param = 1;
    return 0;
}

/* GDI silently substitutes a default face for a name that is not installed, so
 * each candidate has to be checked before it is used. */
static int font_installed(const wchar_t *face)
{
    LOGFONTW query;
    HDC screen;
    int found = 0;

    ZeroMemory(&query, sizeof(query));
    query.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(query.lfFaceName, face, LF_FACESIZE);

    screen = GetDC(NULL);
    if (!screen)
        return 0;
    EnumFontFamiliesExW(screen, &query, count_font, (LPARAM)&found, 0);
    ReleaseDC(NULL, screen);
    return found;
}

static void ensure_font(void)
{
    const wchar_t *face = L"Microsoft Sans Serif";
    size_t i;

    if (overlay_font)
        return;

    for (i = 0; i < sizeof(FONT_CANDIDATES) / sizeof(FONT_CANDIDATES[0]); i++) {
        if (font_installed(FONT_CANDIDATES[i])) {
            face = FONT_CANDIDATES[i];
            break;
        }
    }
    mod_log_wide("overlay font", face);

    overlay_font = CreateFontW(19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, face);
}

static int ensure_buffer(HDC reference, int width, int height)
{
    if (buffer_dc && buffer_width >= width && buffer_height >= height)
        return 1;

    if (buffer_dc) {
        SelectObject(buffer_dc, buffer_previous);
        DeleteObject(buffer_bitmap);
        DeleteDC(buffer_dc);
        buffer_dc = NULL;
    }

    buffer_dc = CreateCompatibleDC(reference);
    if (!buffer_dc)
        return 0;
    buffer_bitmap = CreateCompatibleBitmap(reference, width, height);
    if (!buffer_bitmap) {
        DeleteDC(buffer_dc);
        buffer_dc = NULL;
        return 0;
    }
    buffer_previous = (HBITMAP)SelectObject(buffer_dc, buffer_bitmap);
    buffer_width = width;
    buffer_height = height;
    return 1;
}

static int save_region(HDC target, int x, int y, int w, int h)
{
    SavedRegion *slot;

    if (saved_count >= MAX_SAVED)
        return 0;
    slot = &saved[saved_count];

    if (slot->dc && (slot->capacity_w < w || slot->capacity_h < h)) {
        SelectObject(slot->dc, slot->previous);
        DeleteObject(slot->bitmap);
        DeleteDC(slot->dc);
        slot->dc = NULL;
    }
    if (!slot->dc) {
        slot->dc = CreateCompatibleDC(target);
        if (!slot->dc)
            return 0;
        slot->bitmap = CreateCompatibleBitmap(target, w, h);
        if (!slot->bitmap) {
            DeleteDC(slot->dc);
            slot->dc = NULL;
            return 0;
        }
        slot->previous = (HBITMAP)SelectObject(slot->dc, slot->bitmap);
        slot->capacity_w = w;
        slot->capacity_h = h;
    }

    if (!BitBlt(slot->dc, 0, 0, w, h, target, x, y, SRCCOPY))
        return 0;
    slot->x = x;
    slot->y = y;
    slot->w = w;
    slot->h = h;
    saved_count++;
    return 1;
}

static void on_restore(HDC target)
{
    int i;
    for (i = 0; i < saved_count; i++)
        BitBlt(target, saved[i].x, saved[i].y, saved[i].w, saved[i].h,
               saved[i].dc, 0, 0, SRCCOPY);
    saved_count = 0;
}

/* Renders a bordered box of text lines, centred horizontally in window_width,
 * and puts it on the target at the given window-space y. hdc must already have
 * the overlay font selected, because the box is sized from measured text. */
static void draw_box(HDC target, int window_width, int y,
                     const OverlayLine *lines, int count, int origin_x,
                     int origin_y, int save_under)
{
    RECT rect;
    HBRUSH back, edge;
    HFONT previous_font;
    int text_width = 0;
    int width, height, i, x;

    for (i = 0; i < count; i++) {
        SIZE size;
        if (GetTextExtentPoint32W(target, lines[i].text,
                                  (int)wcslen(lines[i].text), &size) &&
            size.cx > text_width)
            text_width = size.cx;
    }

    width = text_width + OVERLAY_PAD * 2 + OVERLAY_BORDER * 2;
    height = count * LINE_HEIGHT + OVERLAY_PAD * 2 + OVERLAY_BORDER * 2;
    if (!ensure_buffer(target, width, height))
        return;

    edge = CreateSolidBrush(ACCENT);
    back = CreateSolidBrush(RGB(20, 22, 14));
    SetRect(&rect, 0, 0, width, height);
    FillRect(buffer_dc, &rect, edge);
    SetRect(&rect, OVERLAY_BORDER, OVERLAY_BORDER, width - OVERLAY_BORDER,
            height - OVERLAY_BORDER);
    FillRect(buffer_dc, &rect, back);
    DeleteObject(edge);
    DeleteObject(back);

    previous_font = (HFONT)SelectObject(buffer_dc, overlay_font);
    SetBkMode(buffer_dc, TRANSPARENT);
    for (i = 0; i < count; i++) {
        SetTextColor(buffer_dc, lines[i].colour);
        TextOutW(buffer_dc, OVERLAY_BORDER + OVERLAY_PAD,
                 OVERLAY_BORDER + OVERLAY_PAD + i * LINE_HEIGHT, lines[i].text,
                 (int)wcslen(lines[i].text));
    }
    SelectObject(buffer_dc, previous_font);

    x = (window_width - width) / 2;
    if (x < 0)
        x = 0;
    x += origin_x;
    y += origin_y;

    if (save_under && !save_region(target, x, y, width, height))
        return;
    BitBlt(target, x, y, width, height, buffer_dc, 0, 0, SRCCOPY);
}

static void set_line(OverlayLine *line, COLORREF colour, const wchar_t *format,
                     ...)
{
    va_list args;
    va_start(args, format);
    _vsnwprintf(line->text, 255, format, args);
    va_end(args);
    line->text[255] = 0;
    line->colour = colour;
}

/* Columns a string occupies, counting CJK glyphs as double width. */
static int columns_of(const wchar_t *text)
{
    int total = 0;
    for (; *text; text++)
        total += *text >= 0x1100 ? 2 : 1;
    return total;
}

/* Joins the nine slots into one readable string. Labels come from
 * yk_action_names.ini when the player has filled them in, otherwise the raw
 * action ID is shown. When the row will not fit, the ends are kept and the
 * middle is elided, so the budget shrinks as the preset name grows. */
static void describe_slots(wchar_t *out, size_t capacity,
                           const int slots[SLOTS_COUNT], int budget)
{
    wchar_t token[SLOTS_COUNT][NAMES_LABEL_MAX + 8];
    int width[SLOTS_COUNT];
    int total = 0;
    int head = 0, tail = 0, used = 2;
    int i;

    for (i = 0; i < SLOTS_COUNT; i++) {
        const wchar_t *label = slots[i] ? names_lookup(slots[i]) : NULL;
        if (slots[i] == 0)
            lstrcpynW(token[i], L"-", NAMES_LABEL_MAX + 8);
        else if (label)
            lstrcpynW(token[i], label, NAMES_LABEL_MAX + 8);
        else
            _snwprintf(token[i], NAMES_LABEL_MAX + 7, L"%d", slots[i]);
        token[i][NAMES_LABEL_MAX + 7] = 0;
        width[i] = columns_of(token[i]) + 1;
        total += width[i];
    }

    if (budget < 8)
        budget = 8;

    out[0] = 0;
    if (total <= budget) {
        for (i = 0; i < SLOTS_COUNT; i++) {
            if (i)
                wcsncat(out, L" ", capacity - wcslen(out) - 1);
            wcsncat(out, token[i], capacity - wcslen(out) - 1);
        }
        return;
    }

    /* Grow from both ends alternately until the budget runs out. */
    while (head + tail < SLOTS_COUNT) {
        int from_head = head <= tail;
        int index = from_head ? head : SLOTS_COUNT - 1 - tail;
        if (used + width[index] > budget)
            break;
        used += width[index];
        if (from_head)
            head++;
        else
            tail++;
    }
    if (head == 0)
        head = 1;

    for (i = 0; i < head; i++) {
        if (i)
            wcsncat(out, L" ", capacity - wcslen(out) - 1);
        wcsncat(out, token[i], capacity - wcslen(out) - 1);
    }
    wcsncat(out, L" … ", capacity - wcslen(out) - 1);
    for (i = SLOTS_COUNT - tail; i < SLOTS_COUNT; i++) {
        if (i > SLOTS_COUNT - tail)
            wcsncat(out, L" ", capacity - wcslen(out) - 1);
        wcsncat(out, token[i], capacity - wcslen(out) - 1);
    }
}

static void draw_panel(HDC target, int width, int origin_x, int origin_y,
                       int save_under)
{
    OverlayLine lines[PANEL_LINES];
    wchar_t description[256];
    int count = 0;
    int i;

    set_line(&lines[count++], ACCENT, L"工具栏预设   (F9 关闭)");

    for (i = 0; i < PRESETS_COUNT; i++) {
        const Preset *preset = &presets.items[i];
        int active = presets.last == i + 1;

        if (!preset->used) {
            set_line(&lines[count++], TEXT_EMPTY, L"   F%d   —  空", i + 1);
            continue;
        }
        /* The 10 columns cover the marker, "F1", the gaps and the brackets. */
        describe_slots(description, 256, preset->slots,
                       ROW_COLUMNS - columns_of(preset->name) - 10);
        set_line(&lines[count++], active ? ACCENT : TEXT_NORMAL,
                 L"%s F%d   %s   [%s]", active ? L"▶" : L"  ", i + 1,
                 preset->name, description);
    }

    set_line(&lines[count++], ACCENT_DIM, L"Shift+F1~F8 把当前工具栏存成预设");
    /* Only worth a line when something is wrong; once located it is noise. */
    if (!located && weak_fingerprint)
        set_line(&lines[count++], TEXT_WARN,
                 L"请到 设置→快捷键设置 给至少 4 个字母键分配不同动作（仅需一次）");
    else if (!located)
        set_line(&lines[count++], TEXT_WARN,
                 L"尚未定位到工具栏变量，请先读取或新建存档");

    draw_box(target, width, 150, lines, count, origin_x, origin_y, save_under);
}

static void on_paint(HDC target, int width, int height, int origin_x,
                     int origin_y, int save_under)
{
    HFONT previous_font;
    int show_toast;

    (void)height;
    poll_hotkeys();

    show_toast = (int)(GetTickCount() - toast_until) < 0;
    if (!panel_open && !show_toast)
        return;

    ensure_font();
    previous_font = (HFONT)SelectObject(target, overlay_font);

    if (panel_open)
        draw_panel(target, width, origin_x, origin_y, save_under);
    if (show_toast) {
        OverlayLine line;
        set_line(&line, RGB(235, 240, 255), L"%s", toast);
        draw_box(target, width, 116, &line, 1, origin_x, origin_y, save_under);
    }

    SelectObject(target, previous_font);
}

/* ---- entry point -------------------------------------------------------- */

static DWORD WINAPI init_thread(LPVOID unused)
{
    int i;

    (void)unused;
    build_paths();
    if (!presets_load(presets_path, &presets)) {
        /* First run on this install: hand the player something to start from
         * rather than eight empty rows. */
        presets_defaults(&presets);
        presets_save(presets_path, &presets);
        mod_log("no preset file, wrote the shipped defaults");
    }
    names_load(names_path);
    /* Seed the label file with every action already in use, so creating an
     * empty one is enough to get the full list to fill in. */
    for (i = 0; i < PRESETS_COUNT; i++) {
        if (presets.items[i].used)
            names_note(names_path, presets.items[i].slots, SLOTS_COUNT);
    }
    mod_log("mod starting");
    mod_log_wide("game folder", game_dir);

    overlay_trace = trace_present;
    if (!overlay_install(on_paint, on_restore)) {
        mod_log("failed to hook BitBlt/StretchBlt - overlay disabled");
        return 1;
    }
    mod_log("overlay hook installed");

    CreateThread(NULL, 0, locator_thread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        /* Loader lock rules out doing any of this inline. */
        CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    }
    return TRUE;
}
