#include <math.h>
#include <string.h>

#include "gfx.h"

static Surface *target;

void gfx_target(Surface *s) { target = s; }
Surface *gfx_current(void) { return target; }

void gfx_clear(u32 argb)
{
	for (int i = 0; i < target->w * target->h; i++)
		target->px[i] = argb;
}

u32 gfx_mix(u32 a, u32 b, int t)
{
	int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
	int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
	return RGB(ar + (((br - ar) * t) >> 8), ag + (((bg - ag) * t) >> 8), ab + (((bb - ab) * t) >> 8));
}

static float hue_to_rgb(float p, float q, float t)
{
	if (t < 0) t += 1;
	if (t > 1) t -= 1;
	if (t < 1 / 6.0f) return p + (q - p) * 6 * t;
	if (t < 1 / 2.0f) return q;
	if (t < 2 / 3.0f) return p + (q - p) * (2 / 3.0f - t) * 6;
	return p;
}

u32 gfx_hsl(float h, float s, float l)
{
	h = fmodf(h, 360) / 360;
	float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
	return RGB((int)(hue_to_rgb(p, q, h + 1 / 3.0f) * 255), (int)(hue_to_rgb(p, q, h) * 255),
	           (int)(hue_to_rgb(p, q, h - 1 / 3.0f) * 255));
}

// Premultiplied "over" of an opaque colour with coverage alpha.
void gfx_blend(int x, int y, u32 color, int alpha)
{
	if (alpha <= 0 || x < 0 || y < 0 || x >= target->w || y >= target->h)
		return;
	u32 *p = &target->px[y * target->w + x];
	u32 src = 0xFF000000 | color;
	if (alpha >= 255) {
		*p = src;
		return;
	}
	u32 d = *p, out = 0;
	int t = alpha + 1;
	for (int shift = 0; shift < 32; shift += 8) {
		int dc = (d >> shift) & 0xFF, sc = (src >> shift) & 0xFF;
		out |= (u32)(dc + (((sc - dc) * t) >> 8)) << shift;
	}
	*p = out;
}

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Signed distance from a pixel centre to a rounded rectangle's edge (negative inside).
static inline float rr_dist(int px, int py, int x, int y, int w, int h, float r)
{
	float hw = w * 0.5f, hh = h * 0.5f;
	float qx = fabsf(px + 0.5f - (x + hw)) - (hw - r);
	float qy = fabsf(py + 0.5f - (y + hh)) - (hh - r);
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float inside = qx > qy ? qx : qy;
	return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

void gfx_rect(int x, int y, int w, int h, u32 color, int alpha)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			gfx_blend(i, j, color, alpha);
}

void gfx_round_rect(int x, int y, int w, int h, int r, u32 color, int alpha)
{
	for (int j = y; j < y + h; j++) {
		for (int i = x; i < x + w; i++) {
			bool corner = (i < x + r || i >= x + w - r) && (j < y + r || j >= y + h - r);
			float cov = corner ? clampf(0.5f - rr_dist(i, j, x, y, w, h, r), 0, 1) : 1;
			gfx_blend(i, j, color, (int)(alpha * cov));
		}
	}
}

void gfx_round_outline(int x, int y, int w, int h, int r, u32 color, int alpha)
{
	for (int j = y; j < y + h; j++) {
		for (int i = x; i < x + w; i++) {
			if (i > x + r && i < x + w - r - 1 && j > y && j < y + h - 1)
				continue; // middle of the shape
			if (j > y + r && j < y + h - r - 1 && i > x && i < x + w - 1)
				continue;
			float d = rr_dist(i, j, x, y, w, h, r);
			float cov = clampf(0.5f - d, 0, 1) - clampf(-0.5f - d, 0, 1);
			gfx_blend(i, j, color, (int)(alpha * cov));
		}
	}
}

void gfx_circle(float cx, float cy, float r, u32 color, int alpha)
{
	for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++) {
		for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++) {
			float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
			float cov = clampf(r + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
			gfx_blend(i, j, color, (int)(alpha * cov));
		}
	}
}

void gfx_vgradient(int y0, int y1, u32 top, u32 bottom)
{
	for (int y = 0; y < target->h; y++) {
		int t = y <= y0 ? 0 : y >= y1 ? 256 : (y - y0) * 256 / (y1 - y0);
		u32 c = 0xFF000000 | gfx_mix(top, bottom, t);
		for (int x = 0; x < target->w; x++)
			target->px[y * target->w + x] = c;
	}
}

void gfx_linear_gradient(float degrees, u32 from, u32 to)
{
	// CSS angle: 0deg points up, 90deg points right.
	float a = degrees * 3.14159265f / 180;
	float dx = sinf(a), dy = -cosf(a);
	float len = fabsf(target->w * dx) + fabsf(target->h * dy);
	for (int y = 0; y < target->h; y++) {
		for (int x = 0; x < target->w; x++) {
			float t = ((x + 0.5f - target->w / 2.0f) * dx + (y + 0.5f - target->h / 2.0f) * dy) / len + 0.5f;
			target->px[y * target->w + x] = 0xFF000000 | gfx_mix(from, to, (int)(clampf(t, 0, 1) * 256));
		}
	}
}

void gfx_radial(float cx, float cy, float rx, float ry, u32 color, float strength)
{
	int x0 = (int)(cx - rx), x1 = (int)(cx + rx), y0 = (int)(cy - ry), y1 = (int)(cy + ry);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float dx = (x - cx) / rx, dy = (y - cy) / ry;
			float t = sqrtf(dx * dx + dy * dy) / 0.7f;
			if (t < 1)
				gfx_blend(x, y, color, (int)(strength * (1 - t) * 255));
		}
	}
}

// ---------------------------------------------------------------- text

static const Glyph *glyph_for(const Font *f, unsigned char c)
{
	if (c >= 32 && c <= 126)
		return &f->glyphs[c - 32];
	if (c >= 1 && c <= 3)
		return &f->glyphs[95 + c - 1];
	return &f->glyphs['?' - 32];
}

void gfx_text(int font, int x, int baseline, const char *s, u32 color, int alpha)
{
	const Font *f = &fonts[font];
	int pen = x * 16;
	for (; *s; s++) {
		const Glyph *g = glyph_for(f, (unsigned char)*s);
		int gx = (pen + 8) / 16 + g->x, gy = baseline + g->y;
		const u8 *bits = f->bits + g->off;
		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				gfx_blend(gx + i, gy + j, color, bits[j * g->w + i] * alpha / 255);
		pen += g->adv;
	}
}

int gfx_text_width(int font, const char *s)
{
	const Font *f = &fonts[font];
	int w = 0;
	for (; *s; s++)
		w += glyph_for(f, (unsigned char)*s)->adv;
	return (w + 8) / 16;
}

void gfx_text_fit(int font, int x, int baseline, const char *s, u32 color, int max_w)
{
	if (gfx_text_width(font, s) <= max_w) {
		gfx_text(font, x, baseline, s, color, 255);
		return;
	}
	char buf[260];
	int len = strlen(s);
	if (len > 255)
		len = 255;
	while (len > 0) {
		memcpy(buf, s, len);
		strcpy(buf + len, "...");
		if (gfx_text_width(font, buf) <= max_w)
			break;
		len--;
	}
	if (len == 0)
		strcpy(buf, "...");
	gfx_text(font, x, baseline, buf, color, 255);
}

void gfx_text_centered(int font, int cx, int baseline, const char *s, u32 color)
{
	gfx_text(font, cx - gfx_text_width(font, s) / 2, baseline, s, color, 255);
}

int gfx_center_baseline(int font, int cy)
{
	return cy + fonts[font].cap / 2;
}
