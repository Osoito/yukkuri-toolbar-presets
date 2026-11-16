/* Draws on top of the game by intercepting the blit that presents each frame.
 *
 * The game statically imports GDI32!BitBlt and GDI32!StretchBlt, so patching
 * its import address table is enough - no inline hooks, no trampolines, and no
 * interference with the translation hook already loaded in the process.
 *
 * Drawing happens on the *source* surface just before it is presented, then the
 * source is put back. Painting the window after the present instead leaves the
 * overlay missing for the gap between the game's blit and ours, which reads as
 * a flicker once per frame.
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include <windows.h>

/* Called on the game thread with the surface to draw on.
 *
 * origin_x/origin_y convert window coordinates to target coordinates: a point
 * at window (wx, wy) belongs at (wx + origin_x, wy + origin_y).
 * width/height are the window's client size, for centring.
 * save_under is 1 when the target is the game's own surface, meaning whatever
 * is drawn must be recorded so overlay_restore can undo it. */
typedef void (*OverlayPaint)(HDC target, int width, int height, int origin_x,
                             int origin_y, int save_under);

/* Called right after the present to undo a save_under paint. */
typedef void (*OverlayRestore)(HDC target);

/* Optional diagnostics for the first few presents. */
typedef void (*OverlayTrace)(const char *which, int x, int y, int w, int h,
                             int sx, int sy, int sw, int sh, DWORD rop);
extern OverlayTrace overlay_trace;

/* Returns 1 if at least one of the two blit functions was hooked. */
int overlay_install(OverlayPaint on_paint, OverlayRestore on_restore);

/* The game window, resolved when the hook is installed. */
HWND overlay_window(void);

#endif /* OVERLAY_H */
