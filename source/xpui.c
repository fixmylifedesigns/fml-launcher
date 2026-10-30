#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "gx.h"
#include "xpui.h"

#define TASKBAR_H 30
#define WHITE 0xFFFFFF

static Tex wallpaper;

// Rolling green hill under a blue sky, drawn once in software
static void make_wallpaper(void)
{
	const int W = 320, H = 240;
	Surface s = { memalign(32, W * H * 4), W, H };
	if (!s.px)
		return;
	gfx_target(&s);
	gfx_vgradient(0, 165, RGB(36, 98, 204), RGB(160, 204, 244));
	gfx_radial(75, 45, 85, 25, WHITE, 0.45f);
	gfx_radial(215, 70, 105, 28, WHITE, 0.35f);
	gfx_radial(280, 30, 60, 18, WHITE, 0.30f);
	for (int x = 0; x < W; x++) {
		float a = (x - 125) / 125.0f, b = (x - 295) / 85.0f;
		float top = 159 - 46 * expf(-a * a) - 17 * expf(-b * b);
		for (int y = (int)top; y < H; y++) {
			float t = (y - top) / (H - top + 1);
			u32 c = gfx_mix(RGB(126, 196, 64), RGB(34, 112, 28), (int)(t * 256));
			int edge = y == (int)top ? (int)((top - (int)top) * 255) : 0;
			gfx_blend(x, y, c, 255 - edge);
		}
	}
	gfx_radial(125, 125, 130, 30, RGB(220, 255, 160), 0.18f);
	tex_from_surface(&wallpaper, &s, GX_TF_RGB565);
	free(s.px);
}

void xp_free(void)
{
	tex_free(&wallpaper);
}

void xp_layout(XpWindow *win)
{
	win->x = gx_left + 22;
	win->y = 12;
	win->w = gx_right - gx_left - 44;
	win->h = 480 - TASKBAR_H - 22;
	win->cx = win->x + 4;
	win->cy = win->y + 30;
	win->cw = win->w - 8;
	win->ch = win->h - 34;
}

// ---------------------------------------------------------------- small parts

static int pill_w(const char *button)
{
	return strlen(button) > 1 ? gfx_text_width(FONT_11B, button) + 10 : 18;
}

static int pill(int x, int cy, const char *button)
{
	int w = pill_w(button);
	gx_round_rect(x, cy - 9, w, 18, 9, rgba(WHITE, 255));
	gx_text_centered(FONT_11B, x + w / 2, gfx_center_baseline(FONT_11B, cy), button, rgba(0x10204A, 255));
	return w;
}

int xp_hint_width(const char *button, const char *label)
{
	return pill_w(button) + 5 + gfx_text_width(FONT_12, label) + 12;
}

int xp_hint(int x, int cy, const char *button, const char *label)
{
	x += pill(x, cy, button) + 5;
	gx_text(FONT_12, x + 1, gfx_center_baseline(FONT_12, cy) + 1, label, rgba(0x0A1E5C, 160));
	gx_text(FONT_12, x, gfx_center_baseline(FONT_12, cy), label, rgba(WHITE, 255));
	return x + gfx_text_width(FONT_12, label) + 12;
}

int xp_chip_width(const char *button, const char *label)
{
	return 6 + pill_w(button) + 6 + gfx_text_width(FONT_12, label) + 10;
}

int xp_chip(int x, int cy, const char *button, const char *label, bool on)
{
	int w = xp_chip_width(button, label);
	gx_round_rect(x, cy - 12, w, 24, 12, rgba(on ? 0x3C8DFF : 0xFFFFFF, on ? 120 : 22));
	pill(x + 6, cy, button);
	gx_text(FONT_12, x + 6 + pill_w(button) + 6, gfx_center_baseline(FONT_12, cy), label, rgba(on ? WHITE : 0xB8C8E8, 255));
	return x + w + 8;
}

void xp_glyph(int glyph, float cx, float cy, float s, u32 c)
{
	gx_texture(NULL);
	switch (glyph) {
	case XG_PLAY:
		gx_tri(cx - s * 0.32f, cy - s * 0.42f, cx + s * 0.46f, cy, cx - s * 0.32f, cy + s * 0.42f, c);
		break;
	case XG_PAUSE:
		gx_quad(cx - s * 0.34f, cy - s * 0.38f, s * 0.24f, s * 0.76f, c);
		gx_quad(cx + s * 0.10f, cy - s * 0.38f, s * 0.24f, s * 0.76f, c);
		break;
	case XG_STOP:
		gx_quad(cx - s * 0.32f, cy - s * 0.32f, s * 0.64f, s * 0.64f, c);
		break;
	case XG_PREV:
		gx_quad(cx - s * 0.42f, cy - s * 0.36f, s * 0.14f, s * 0.72f, c);
		gx_tri(cx + s * 0.38f, cy - s * 0.36f, cx + s * 0.38f, cy + s * 0.36f, cx - s * 0.26f, cy, c);
		break;
	case XG_NEXT:
		gx_quad(cx + s * 0.28f, cy - s * 0.36f, s * 0.14f, s * 0.72f, c);
		gx_tri(cx - s * 0.38f, cy - s * 0.36f, cx + s * 0.26f, cy, cx - s * 0.38f, cy + s * 0.36f, c);
		break;
	case XG_VOLUME:
		gx_quad(cx - s * 0.45f, cy - s * 0.14f, s * 0.2f, s * 0.28f, c);
		gx_tri(cx - s * 0.3f, cy - s * 0.14f, cx + s * 0.02f, cy - s * 0.42f, cx + s * 0.02f, cy + s * 0.42f, c);
		gx_tri(cx - s * 0.3f, cy - s * 0.14f, cx + s * 0.02f, cy + s * 0.42f, cx - s * 0.3f, cy + s * 0.14f, c);
		gx_line(cx + s * 0.18f, cy - s * 0.18f, cx + s * 0.18f, cy + s * 0.18f, s * 0.08f, c);
		gx_line(cx + s * 0.34f, cy - s * 0.32f, cx + s * 0.34f, cy + s * 0.32f, s * 0.08f, c);
		break;
	case XG_NOTE:
		gx_quad(cx + s * 0.06f, cy - s * 0.46f, s * 0.1f, s * 0.7f, c);
		gx_line(cx + s * 0.11f, cy - s * 0.42f, cx + s * 0.42f, cy - s * 0.22f, s * 0.12f, c);
		gx_circle(cx - s * 0.06f, cy + s * 0.26f, s * 0.2f, c);
		break;
	case XG_FILM:
		gx_round_rect(cx - s * 0.46f, cy - s * 0.34f, s * 0.92f, s * 0.68f, s * 0.08f, c);
		gx_texture(NULL);
		for (int i = 0; i < 4; i++) {
			float x = cx - s * 0.38f + i * s * 0.22f;
			gx_quad(x, cy - s * 0.3f, s * 0.1f, s * 0.08f, rgba(0x10204A, 255));
			gx_quad(x, cy + s * 0.22f, s * 0.1f, s * 0.08f, rgba(0x10204A, 255));
		}
		gx_quad(cx - s * 0.34f, cy - s * 0.16f, s * 0.68f, s * 0.32f, rgba(0x10204A, 160));
		break;
	}
}

// ---------------------------------------------------------------- desktop

void xp_desktop(int *hint_x, int *hint_cy, int *hint_end)
{
	if (!wallpaper.ready)
		make_wallpaper();
	gx_blend(BLEND_ALPHA);
	gx_texture(&wallpaper);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, 0xFFFFFFFF);

	// Taskbar
	const float ty = 480 - TASKBAR_H;
	gx_texture(NULL);
	gx_quad_grad(gx_left, ty, gx_right - gx_left, 4, rgba(0x3D8DF5, 255), rgba(0x245EDC, 255));
	gx_quad_grad(gx_left, ty + 4, gx_right - gx_left, TASKBAR_H - 4, rgba(0x245EDC, 255), rgba(0x1941A5, 255));
	gx_quad(gx_left, ty, gx_right - gx_left, 1, rgba(0x6FA8FF, 255));

	// Start button: green, rounded on the right
	const float sx = gx_left + 22;
	float sw = 18 + 6 + gfx_text_width(FONT_15S, "Home") + 16;
	gx_round_rect(sx - 14, ty + 1, sw + 14, TASKBAR_H - 2, 12, rgba(0x2F8A2F, 255));
	gx_texture(NULL);
	gx_quad_grad(sx - 2, ty + 3, sw - 12, (TASKBAR_H - 6) / 2, rgba(0x6CC76C, 255), rgba(0x3DA33D, 255));
	gx_quad_grad(sx - 2, ty + 3 + (TASKBAR_H - 6) / 2, sw - 12, (TASKBAR_H - 6) / 2, rgba(0x3DA33D, 255), rgba(0x2F8A2F, 255));
	int x = sx + pill(sx, ty + TASKBAR_H / 2, "B") + 6;
	gx_text(FONT_15S, x + 1, gfx_center_baseline(FONT_15S, ty + TASKBAR_H / 2) + 1, "Home", rgba(0x123D12, 200));
	gx_text(FONT_15S, x, gfx_center_baseline(FONT_15S, ty + TASKBAR_H / 2), "Home", rgba(WHITE, 255));

	// Tray with the clock
	char clock[8];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	snprintf(clock, sizeof(clock), "%d:%02d", tm->tm_hour, tm->tm_min);
	int cw = gfx_text_width(FONT_12, clock) + 28;
	float tx = gx_right - 22 - cw;
	gx_texture(NULL);
	gx_quad_grad(tx, ty + 1, cw + 22, TASKBAR_H - 1, rgba(0x14A0F0, 255), rgba(0x0C78D8, 255));
	gx_quad(tx, ty + 1, 1, TASKBAR_H - 1, rgba(0x0A4FB8, 255));
	gx_text(FONT_12, tx + 14, gfx_center_baseline(FONT_12, ty + TASKBAR_H / 2), clock, rgba(WHITE, 255));

	*hint_x = sx + sw + 12;
	*hint_cy = ty + TASKBAR_H / 2;
	*hint_end = tx - 8;
}

// ---------------------------------------------------------------- window

void xp_window(const XpWindow *win, const char *title, int icon)
{
	const float x = win->x, y = win->y, w = win->w, h = win->h;
	gx_round_rect(x - 2, y - 1, w + 5, h + 5, 10, rgba(0, 70)); // shadow
	gx_round_rect(x, y, w, h, 8, rgba(0x0842C8, 255));
	gx_round_rect(x, y, w, 30, 8, rgba(0x3A8CF8, 255));
	gx_texture(NULL);
	gx_quad_grad(x, y + 4, w, 12, rgba(0x2E84F6, 255), rgba(0x0D5FE6, 255));
	gx_quad_grad(x, y + 16, w, 14, rgba(0x0D5FE6, 255), rgba(0x0447CF, 255));
	gx_quad(x + 7, y + 1, w - 14, 1, rgba(0x9CCBFF, 200));
	gx_quad(x + 3, y + 30, 1, h - 33, rgba(0x2C6BE0, 255));
	gx_quad(x + w - 4, y + 30, 1, h - 33, rgba(0x2C6BE0, 255));

	// Icon and title
	gx_round_rect(x + 8, y + 7, 17, 17, 4, rgba(0xF39A1E, 255));
	gx_round_rect(x + 9, y + 8, 15, 8, 4, rgba(0xFFD27A, 140));
	xp_glyph(icon, x + 16.5f, y + 15.5f, 13, rgba(WHITE, 255));
	gx_text(FONT_15S, x + 32, gfx_center_baseline(FONT_15S, y + 15) + 1, title, rgba(0x0A1E5C, 200));
	gx_text(FONT_15S, x + 31, gfx_center_baseline(FONT_15S, y + 15), title, rgba(WHITE, 255));

	// Minimise, maximise, close
	for (int i = 0; i < 3; i++) {
		float bx = x + w - 8 - 21 - (2 - i) * 23, by = y + 5;
		bool close = i == 2;
		gx_round_rect(bx, by, 21, 21, 4, rgba(WHITE, 255));
		gx_round_rect(bx + 1, by + 1, 19, 19, 3, rgba(close ? 0xD8492B : 0x2563E8, 255));
		gx_round_rect(bx + 2, by + 2, 17, 8, 3, rgba(WHITE, close ? 60 : 50));
		gx_texture(NULL);
		u32 c = rgba(WHITE, 255);
		if (i == 0)
			gx_quad(bx + 6, by + 13, 8, 3, c);
		else if (i == 1) {
			gx_quad(bx + 5, by + 5, 11, 3, c);
			gx_quad(bx + 5, by + 5, 2, 11, c);
			gx_quad(bx + 14, by + 5, 2, 11, c);
			gx_quad(bx + 5, by + 14, 11, 2, c);
		} else {
			gx_line(bx + 6, by + 6, bx + 15, by + 15, 2.4f, c);
			gx_line(bx + 15, by + 6, bx + 6, by + 15, 2.4f, c);
		}
	}

	// Client area: the media player's dark blue body
	gx_texture(NULL);
	gx_quad_grad(win->cx, win->cy, win->cw, win->ch, rgba(0x22375E, 255), rgba(0x0B1730, 255));
}

void xp_panel(float x, float y, float w, float h)
{
	gx_round_rect(x - 1, y - 1, w + 2, h + 2, 5, rgba(0x5674A8, 160));
	gx_round_rect(x, y, w, h, 4, rgba(0x03070F, 255));
}

void xp_transport(float x, float y, float w, float h)
{
	gx_round_rect(x, y, w, h, 8, rgba(0x08101F, 255));
	gx_round_rect(x + 1, y + 1, w - 2, h - 2, 7, rgba(0x203660, 255));
	gx_texture(NULL);
	gx_quad_grad(x + 3, y + 2, w - 6, h / 2 - 2, rgba(0x3B5888, 255), rgba(0x22385F, 255));
	gx_quad_grad(x + 3, y + h / 2, w - 6, h / 2 - 3, rgba(0x172A4C, 255), rgba(0x0F1D38, 255));
	gx_quad(x + 8, y + 2, w - 16, 1, rgba(0x8FB2EA, 140));
}

void xp_round_button(float cx, float cy, float r, int glyph, bool big, bool lit)
{
	gx_circle(cx, cy + 1, r + 2, rgba(0, 110));
	gx_circle(cx, cy, r + 1.5f, rgba(big ? 0xB7D3FF : 0x7F98C4, 255));
	u32 base = big ? (lit ? 0x3A88FF : 0x1C5CD8) : (lit ? 0x4A6CA8 : 0x253C66);
	gx_circle(cx, cy, r, rgba(base, 255));
	gx_circle(cx, cy + r * 0.38f, r * 0.62f, rgba(big ? 0x6FB4FF : 0x46659C, 110));
	gx_round_rect(cx - r * 0.66f, cy - r * 0.9f, r * 1.32f, r * 0.82f, r * 0.41f, rgba(WHITE, big ? 80 : 46));
	xp_glyph(glyph, cx, cy, r * (big ? 1.05f : 1.1f), rgba(WHITE, 255));
}

void xp_slider(float x, float cy, float w, float frac, bool thumb)
{
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	gx_round_rect(x - 1, cy - 4, w + 2, 8, 4, rgba(0x5674A8, 200));
	gx_round_rect(x, cy - 3, w, 6, 3, rgba(0x050B18, 255));
	if (frac * w >= 6) {
		gx_round_rect(x, cy - 3, w * frac, 6, 3, rgba(0x2F8BFF, 255));
		gx_round_rect(x + 1, cy - 2, w * frac - 2, 2, 1, rgba(0xA6D2FF, 160));
	}
	if (thumb) {
		float tx = x + w * frac;
		gx_circle(tx, cy + 1, 8, rgba(0, 100));
		gx_circle(tx, cy, 7, rgba(0xDDEBFF, 255));
		gx_circle(tx, cy, 5, rgba(0x3F8EF5, 255));
		gx_circle(tx - 1.5f, cy - 2, 2.2f, rgba(WHITE, 170));
	}
}
