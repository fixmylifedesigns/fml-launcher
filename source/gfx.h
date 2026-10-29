#ifndef GFX_H
#define GFX_H

#include <gctypes.h>

#include "font_data.h"

// Software 2D drawing into ARGB surfaces (premultiplied alpha). Surfaces are
// turned into GPU textures by gx.c: the background, the UI overlay and the
// generated covers are all drawn with these functions.

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

typedef struct {
	u32 *px; // w * h pixels, 0xAARRGGBB premultiplied
	int w, h;
} Surface;

// All drawing goes to the current target. Colours are RGB; alpha is 0..255.
void gfx_target(Surface *s);
Surface *gfx_current(void);
void gfx_clear(u32 argb);

void gfx_blend(int x, int y, u32 color, int alpha);
void gfx_rect(int x, int y, int w, int h, u32 color, int alpha);
void gfx_round_rect(int x, int y, int w, int h, int r, u32 color, int alpha);
void gfx_round_outline(int x, int y, int w, int h, int r, u32 color, int alpha);
void gfx_circle(float cx, float cy, float r, u32 color, int alpha);
// Opaque gradients over the whole target
void gfx_vgradient(int y0, int y1, u32 top, u32 bottom);
void gfx_linear_gradient(float degrees, u32 from, u32 to);
// Elliptical glow, like CSS radial-gradient(rx ry at cx cy, color strength, transparent 70%)
void gfx_radial(float cx, float cy, float rx, float ry, u32 color, float strength);

// Text is positioned by its baseline.
void gfx_text(int font, int x, int baseline, const char *s, u32 color, int alpha);
void gfx_text_fit(int font, int x, int baseline, const char *s, u32 color, int max_w);
void gfx_text_centered(int font, int cx, int baseline, const char *s, u32 color);
int gfx_text_width(int font, const char *s);
// Baseline that vertically centres capital letters on cy.
int gfx_center_baseline(int font, int cy);

u32 gfx_mix(u32 a, u32 b, int t); // RGB mix, t = 0..256
u32 gfx_hsl(float h, float s, float l);

#endif
