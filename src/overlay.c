#include "overlay.h"

#include <string.h>

typedef BOOL (WINAPI *PfnBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *PfnStretchBlt)(HDC, int, int, int, int, HDC, int, int,
                                     int, int, DWORD);

OverlayTrace overlay_trace;

static PfnBitBlt real_bit_blt;
static PfnStretchBlt real_stretch_blt;
static OverlayPaint paint_callback;
static OverlayRestore restore_callback;
static HWND game_window;
static LONG painting;

HWND overlay_window(void)
{
    return game_window;
}

static void trace_present(const char *which, int x, int y, int w, int h,
                          int sx, int sy, int sw, int sh, DWORD rop)
{
    static int remaining = 8;

    if (!overlay_trace || remaining <= 0)
        return;
    remaining--;
    overlay_trace(which, x, y, w, h, sx, sy, sw, sh, rop);
}

/* True for a blit that presents the whole frame unscaled, which is the only
 * shape we can draw into the source for. */
static int is_plain_present(HDC source, int w, int h, int sw, int sh, DWORD rop)
{
    RECT client;

    if (!source || rop != SRCCOPY || w != sw || h != sh)
        return 0;
    if (!GetClientRect(game_window, &client))
        return 0;
    return w >= client.right && h >= client.bottom;
}

/* Enter the overlay, returning 0 if it is already running on this thread. Our
 * own drawing must never re-enter the hook. */
static int begin_paint(void)
{
    return InterlockedCompareExchange(&painting, 1, 0) == 0;
}

static void end_paint(void)
{
    InterlockedExchange(&painting, 0);
}

static void paint_on_window(HDC dest)
{
    RECT client;

    if (!paint_callback || !game_window)
        return;
    /* HSP gives every offscreen buffer a hidden window of its own, so matching
     * the one real window keeps the overlay out of the game's sprite buffers. */
    if (WindowFromDC(dest) != game_window)
        return;
    if (!begin_paint())
        return;
    if (GetClientRect(game_window, &client) &&
        client.right > 0 && client.bottom > 0)
        paint_callback(dest, client.right, client.bottom, 0, 0, 0);
    end_paint();
}

static BOOL WINAPI hook_bit_blt(HDC dest, int x, int y, int w, int h, HDC src,
                                int sx, int sy, DWORD rop)
{
    BOOL result;
    int drew = 0;

    if (paint_callback && game_window && WindowFromDC(dest) == game_window) {
        trace_present("BitBlt", x, y, w, h, sx, sy, w, h, rop);
        if (is_plain_present(src, w, h, w, h, rop) && begin_paint()) {
            RECT client;
            if (GetClientRect(game_window, &client)) {
                paint_callback(src, client.right, client.bottom, sx - x, sy - y,
                               1);
                drew = 1;
            } else {
                end_paint();
            }
        }
    }

    result = real_bit_blt(dest, x, y, w, h, src, sx, sy, rop);

    if (drew) {
        if (restore_callback)
            restore_callback(src);
        end_paint();
    } else {
        paint_on_window(dest);
    }
    return result;
}

static BOOL WINAPI hook_stretch_blt(HDC dest, int x, int y, int w, int h,
                                    HDC src, int sx, int sy, int sw, int sh,
                                    DWORD rop)
{
    BOOL result;
    int drew = 0;

    if (paint_callback && game_window && WindowFromDC(dest) == game_window) {
        trace_present("StretchBlt", x, y, w, h, sx, sy, sw, sh, rop);
        if (is_plain_present(src, w, h, sw, sh, rop) && begin_paint()) {
            RECT client;
            if (GetClientRect(game_window, &client)) {
                paint_callback(src, client.right, client.bottom, sx - x, sy - y,
                               1);
                drew = 1;
            } else {
                end_paint();
            }
        }
    }

    result = real_stretch_blt(dest, x, y, w, h, src, sx, sy, sw, sh, rop);

    if (drew) {
        if (restore_callback)
            restore_callback(src);
        end_paint();
    } else {
        paint_on_window(dest);
    }
    return result;
}

/* Replace one import address table entry. Returns the previous target. */
static void *patch_iat(HMODULE module, const char *dll, const char *function,
                       void *replacement)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *import;
    DWORD rva;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;

    rva = nt->OptionalHeader
              .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva)
        return NULL;

    for (import = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); import->Name;
         import++) {
        IMAGE_THUNK_DATA *names, *addresses;

        if (lstrcmpiA((const char *)(base + import->Name), dll) != 0)
            continue;

        /* Without an import lookup table there is no safe way to match by
         * name: by the time we run, FirstThunk holds resolved addresses, not
         * name RVAs, and walking it as names would dereference garbage. The
         * game does emit one for every import, so bail out instead. */
        if (!import->OriginalFirstThunk)
            continue;
        addresses = (IMAGE_THUNK_DATA *)(base + import->FirstThunk);
        names = (IMAGE_THUNK_DATA *)(base + import->OriginalFirstThunk);

        for (; names->u1.AddressOfData; names++, addresses++) {
            IMAGE_IMPORT_BY_NAME *entry;
            void *previous;
            DWORD protection;

            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            entry = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((const char *)entry->Name, function) != 0)
                continue;

            previous = (void *)(UINT_PTR)addresses->u1.Function;
            if (!VirtualProtect(&addresses->u1.Function,
                                sizeof(addresses->u1.Function),
                                PAGE_READWRITE, &protection))
                return NULL;
            addresses->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&addresses->u1.Function,
                           sizeof(addresses->u1.Function), protection,
                           &protection);
            return previous;
        }
    }
    return NULL;
}

static BOOL CALLBACK pick_main_window(HWND window, LPARAM param)
{
    DWORD pid = 0;
    RECT rect;
    LONG *best_area = (LONG *)param;
    LONG area;

    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId())
        return TRUE;
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL)
        return TRUE;
    if (!GetClientRect(window, &rect))
        return TRUE;

    /* The game window is by far the largest visible one in the process. */
    area = rect.right * rect.bottom;
    if (area > *best_area) {
        *best_area = area;
        game_window = window;
    }
    return TRUE;
}

int overlay_install(OverlayPaint on_paint, OverlayRestore on_restore)
{
    HMODULE game = GetModuleHandleW(NULL);
    LONG best_area = 0;
    int hooked = 0;

    game_window = NULL;
    EnumWindows(pick_main_window, (LPARAM)&best_area);
    if (!game_window)
        return 0;

    paint_callback = on_paint;
    restore_callback = on_restore;

    real_bit_blt = (PfnBitBlt)patch_iat(game, "GDI32.dll", "BitBlt",
                                        (void *)hook_bit_blt);
    if (real_bit_blt)
        hooked++;

    real_stretch_blt = (PfnStretchBlt)patch_iat(game, "GDI32.dll", "StretchBlt",
                                                 (void *)hook_stretch_blt);
    if (real_stretch_blt)
        hooked++;

    return hooked > 0;
}
