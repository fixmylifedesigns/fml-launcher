#ifndef XPUI_H
#define XPUI_H
#include <gctypes.h>

// The look of the music and movie players: a Windows XP style desktop with a
// taskbar, and a media player window with glossy round buttons. Everything
// is drawn on the GPU in 2D mode, in 640x480 layout units.

enum { XG_PLAY, XG_PAUSE, XG_STOP, XG_PREV, XG_NEXT, XG_VOLUME, XG_NOTE, XG_FILM };

// Window placement inside the desktop (depends on 4:3 / 16:9)
typedef struct {
	float x, y, w, h;     // whole window
	float cx, cy, cw, ch; // client area under the title bar
} XpWindow;

void xp_layout(XpWindow *win);
// Wallpaper and taskbar. The taskbar's start button reads "B Home".
// Returns the x where the taskbar has room for hints, and its centre line.
void xp_desktop(int *hint_x, int *hint_cy, int *hint_end);
void xp_window(const XpWindow *win, const char *title, int icon);
// Dark recessed panel inside the window
void xp_panel(float x, float y, float w, float h);
// Transport bar background (glossy dark blue)
void xp_transport(float x, float y, float w, float h);
void xp_round_button(float cx, float cy, float r, int glyph, bool big, bool lit);
void xp_slider(float x, float cy, float w, float frac, bool thumb);
void xp_glyph(int glyph, float cx, float cy, float size, u32 rgba);
// Button hint on the taskbar ("A Play"); returns the x after it
int xp_hint(int x, int cy, const char *button, const char *label);
int xp_hint_width(const char *button, const char *label);
// Toggle chip on the toolbar ("X Shuffle: On")
int xp_chip(int x, int cy, const char *button, const char *label, bool on);
int xp_chip_width(const char *button, const char *label);
void xp_free(void);

#endif
