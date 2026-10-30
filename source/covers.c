#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "covers.h"
#include "demo_covers.h"
#include "library.h"
#include "shapes.h"

#define MAX_W 192
#define MAX_H 228
#define SPINE_W 24

#define LOAD_RANGE (gx_wide ? 9 : 7)  // load covers this far from the selection
#define KEEP_RANGE (LOAD_RANGE + 4) // free covers further away than this

static Cover cache[MAX_GAMES];
static bool demo_mode;

static u32 scratch[MAX_W * MAX_H];
static u32 scratch2[MAX_W * MAX_H];

static u32 hash(const char *s)
{
	u32 h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (u8)*s) * 16777619u;
	return h;
}

void covers_reset(bool demo)
{
	for (int i = 0; i < MAX_GAMES; i++) {
		if (cache[i].loaded) {
			tex_free(&cache[i].front);
			tex_free(&cache[i].spine);
			tex_free(&cache[i].back);
		}
		memset(&cache[i], 0, sizeof(Cover));
	}
	demo_mode = demo;
}

Cover *covers_get(int index)
{
	return &cache[index];
}

// ---------------------------------------------------------------- .cov files

static u16 be16(const u8 *p) { return (p[0] << 8) | p[1]; }

// .cov: "CCV1", then front, spine and back as (u16 w, u16 h, RGB565 tiles)
static void parse_cov(Cover *c, const u8 *data, u32 size)
{
	if (size < 16 || memcmp(data, "CCV1", 4) != 0)
		return;
	u32 off = 4;
	Tex *parts[3] = { &c->front, &c->spine, &c->back };
	for (int i = 0; i < 3 && off + 4 <= size; i++) {
		int w = be16(data + off), h = be16(data + off + 2);
		off += 4;
		u32 bytes = (u32)w * h * 2;
		if (w == 0 || h == 0)
			continue;
		if (w % 4 || h % 4 || w > 512 || h > 512 || off + bytes > size)
			return;
		tex_from_tiles(parts[i], data + off, w, h, GX_TF_RGB565);
		off += bytes;
	}
}

static void load_cov(Cover *c, const char *key)
{
	if (demo_mode) {
		for (int i = 0; i < demo_cover_count; i++)
			if (demo_covers[i].key && strcmp(demo_covers[i].key, key) == 0)
				parse_cov(c, demo_covers[i].data, demo_covers[i].size);
		return;
	}
	char path[128];
	snprintf(path, sizeof(path), "sd:/covers/%s.cov", key);
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *data = size > 0 && size < 2 * 1024 * 1024 ? malloc(size) : NULL;
	if (data && fread(data, 1, size, f) == (size_t)size)
		parse_cov(c, data, size);
	free(data);
	fclose(f);
}

// ---------------------------------------------------------------- disc banner

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// Reads opening.bnr's 96x32 image into ARGB (premultiplied). Returns false if none.
static bool read_banner(const char *path, u32 *out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	bool ok = false;
	u8 hdr[12];
	u8 *fst = NULL;
	if (fseek(f, 0x424, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8)
		goto done;
	u32 fst_off = be32(hdr), fst_size = be32(hdr + 4);
	if (fst_size < 12 || fst_size > 512 * 1024)
		goto done;
	fst = malloc(fst_size);
	if (!fst || fseek(f, fst_off, SEEK_SET) != 0 || fread(fst, 1, fst_size, f) != fst_size)
		goto done;

	u32 count = be32(fst + 8);
	if (count * 12 > fst_size)
		goto done;
	const char *names = (const char *)fst + count * 12;
	u32 bnr_off = 0;
	for (u32 i = 1; i < count; i++) {
		const u8 *e = fst + i * 12;
		u32 name_off = be32(e) & 0xFFFFFF;
		if (e[0] == 0 && count * 12 + name_off < fst_size && strcasecmp(names + name_off, "opening.bnr") == 0) {
			bnr_off = be32(e + 4);
			break;
		}
	}
	static u8 raw[96 * 32 * 2];
	if (!bnr_off || fseek(f, bnr_off, SEEK_SET) != 0 || fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "BNR", 3) != 0)
		goto done;
	if (fseek(f, bnr_off + 0x20, SEEK_SET) != 0 || fread(raw, 1, sizeof(raw), f) != sizeof(raw))
		goto done;

	// RGB5A3, 4x4 tiles
	const u8 *p = raw;
	for (int ty = 0; ty < 32; ty += 4)
		for (int tx = 0; tx < 96; tx += 4)
			for (int y = ty; y < ty + 4; y++)
				for (int x = tx; x < tx + 4; x++, p += 2) {
					u16 v = be16(p);
					int a, r, g, b;
					if (v & 0x8000) {
						a = 255;
						r = ((v >> 10) & 31) * 255 / 31;
						g = ((v >> 5) & 31) * 255 / 31;
						b = (v & 31) * 255 / 31;
					} else {
						a = ((v >> 12) & 7) * 255 / 7;
						r = ((v >> 8) & 15) * 17;
						g = ((v >> 4) & 15) * 17;
						b = (v & 15) * 17;
					}
					out[y * 96 + x] = ((u32)a << 24) | ((u32)(r * a / 255) << 16) | ((u32)(g * a / 255) << 8) | (u32)(b * a / 255);
				}
	ok = true;
done:
	free(fst);
	fclose(f);
	return ok;
}

// Bilinear scale of a premultiplied ARGB image onto the current target
static void draw_scaled(const u32 *src, int sw, int sh, int x, int y, int w, int h)
{
	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			float fx = (i + 0.5f) * sw / w - 0.5f, fy = (j + 0.5f) * sh / h - 0.5f;
			int x0 = fx < 0 ? 0 : (int)fx, y0 = fy < 0 ? 0 : (int)fy;
			int x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
			float ax = fx - x0, ay = fy - y0;
			if (ax < 0) ax = 0;
			if (ay < 0) ay = 0;
			u32 out = 0;
			for (int shift = 0; shift < 32; shift += 8) {
				float c00 = (src[y0 * sw + x0] >> shift) & 0xFF, c10 = (src[y0 * sw + x1] >> shift) & 0xFF;
				float c01 = (src[y1 * sw + x0] >> shift) & 0xFF, c11 = (src[y1 * sw + x1] >> shift) & 0xFF;
				float c = (c00 * (1 - ax) + c10 * ax) * (1 - ay) + (c01 * (1 - ax) + c11 * ax) * ay;
				out |= (u32)(c + 0.5f) << shift;
			}
			// premultiplied over
			Surface *t = gfx_current();
			int px = x + i, py = y + j;
			if (px < 0 || py < 0 || px >= t->w || py >= t->h)
				continue;
			u32 d = t->px[py * t->w + px], a = out >> 24, res = 0;
			for (int shift = 0; shift < 32; shift += 8) {
				u32 sc = (out >> shift) & 0xFF, dc = (d >> shift) & 0xFF;
				res |= (sc + dc * (255 - a) / 255) << shift;
			}
			t->px[py * t->w + px] = res;
		}
	}
}

// ---------------------------------------------------------------- generated art

static void initials(char *out, const char *title)
{
	// First letters of the first two words, else the first two letters
	const char *second = NULL;
	for (const char *p = title; *p; p++)
		if (*p == ' ' && p[1] && p[1] != ' ') {
			second = p + 1;
			break;
		}
	// Also split CamelCase ("MemCardPro" -> "MC")
	if (!second)
		for (const char *p = title + 1; *p; p++)
			if (isupper((u8)*p) && islower((u8)p[-1])) {
				second = p;
				break;
			}
	out[0] = toupper((u8)title[0]);
	out[1] = second ? toupper((u8)*second) : (title[1] ? title[1] : 0);
	out[2] = 0;
}

// Word-wraps the title into at most two lines that fit max_w.
static int wrap_title(const char *title, char lines[2][132], int font, int max_w)
{
	char words[64];
	snprintf(words, sizeof(words), "%s", title);
	lines[0][0] = lines[1][0] = 0;
	int line = 0;
	for (char *w = strtok(words, " "); w; w = strtok(NULL, " ")) {
		char test[264];
		snprintf(test, sizeof(test), "%s%s%s", lines[line], lines[line][0] ? " " : "", w);
		if (gfx_text_width(font, test) <= max_w || !lines[line][0]) {
			snprintf(lines[line], 132, "%.131s", test);
		} else if (line == 0) {
			line = 1;
			snprintf(lines[1], 132, "%s", w);
		} else {
			break;
		}
	}
	return line + 1;
}

static const char *band_label(const Game *g)
{
	switch (g->kind) {
	case KIND_DISC: return "GAME";
	case KIND_GBA: return "GAME BOY ADVANCE";
	case KIND_GBC: return "GAME BOY COLOR";
	case KIND_GB: return "GAME BOY";
	case KIND_WEB: return "WEB PAGE";
	case KIND_APP: return "FML APP";
	case KIND_DRIVE: return "DISC IN DRIVE";
	default: return "HOMEBREW";
	}
}

// Music player: a note on a glossy round badge. Movie player: a film frame.
static void draw_app_icon(const Game *g, int w)
{
	float cx = w / 2.0f, cy = 96;
	gfx_circle(cx, cy + 3, 52, RGB(0, 0, 0), 70);
	gfx_circle(cx, cy, 52, RGB(255, 255, 255), 235);
	if (!strcmp(g->key, "fml-music")) {
		gfx_circle(cx, cy, 47, RGB(32, 104, 230), 255);
		gfx_radial(cx, cy - 22, 40, 20, RGB(255, 255, 255), 0.35f);
		gfx_rect(cx + 2, cy - 30, 7, 44, RGB(255, 255, 255), 255);
		for (int i = 0; i < 12; i++)
			gfx_rect(cx + 2 + i * 2, cy - 30 + i, 3, 10, RGB(255, 255, 255), 255);
		gfx_circle(cx - 7, cy + 16, 12, RGB(255, 255, 255), 255);
	} else {
		gfx_circle(cx, cy, 47, RGB(230, 120, 26), 255);
		gfx_radial(cx, cy - 22, 40, 20, RGB(255, 255, 255), 0.35f);
		gfx_round_rect(cx - 30, cy - 21, 60, 42, 5, RGB(255, 255, 255), 255);
		for (int i = 0; i < 5; i++) {
			gfx_rect(cx - 26 + i * 12, cy - 18, 6, 5, RGB(150, 70, 10), 255);
			gfx_rect(cx - 26 + i * 12, cy + 13, 6, 5, RGB(150, 70, 10), 255);
		}
		gfx_rect(cx - 24, cy - 9, 48, 18, RGB(150, 70, 10), 200);
	}
}

// The disc in the drive: a mini DVD
static void draw_disc_icon(int w)
{
	float cx = w / 2.0f, cy = 98;
	gfx_circle(cx, cy + 3, 56, RGB(0, 0, 0), 80);
	gfx_circle(cx, cy, 56, RGB(214, 220, 232), 255);
	gfx_radial(cx - 18, cy - 20, 50, 30, RGB(255, 255, 255), 0.55f);
	gfx_radial(cx + 22, cy + 18, 40, 26, RGB(170, 140, 255), 0.35f);
	gfx_circle(cx, cy, 20, RGB(150, 156, 170), 255);
	gfx_circle(cx, cy, 8, RGB(40, 40, 52), 255);
}

static void generate_front(Cover *c, const Game *g, u32 hue)
{
	const int FRONT_W = case_shape(g)->front_w, FRONT_H = case_shape(g)->front_h;
	Surface s = { scratch, FRONT_W, FRONT_H };
	gfx_target(&s);
	gfx_linear_gradient(150, gfx_hsl(hue, 0.65f, 0.55f), gfx_hsl(hue + 45, 0.70f, 0.26f));

	gfx_rect(0, 0, FRONT_W, 24, RGB(0, 0, 0), 168);
	gfx_text(FONT_9K, 9, gfx_center_baseline(FONT_9K, 12), band_label(g), RGB(255, 255, 255), 210);

	static u32 banner[96 * 32];
	if (g->kind == KIND_APP) {
		draw_app_icon(g, FRONT_W);
	} else if (g->kind == KIND_DRIVE) {
		draw_disc_icon(FRONT_W);
	} else if (g->is_disc && !demo_mode && read_banner(g->path, banner)) {
		gfx_round_rect(6, 42, 148, 52, 4, RGB(0, 0, 0), 90);
		draw_scaled(banner, 96, 32, 8, 44, 144, 48);
	} else {
		char mono[3];
		initials(mono, g->title);
		gfx_text(FONT_62K, 12, 38 + fonts[FONT_62K].cap + 4, mono, RGB(255, 255, 255), 235);
	}

	char lines[2][132];
	int n = wrap_title(g->title, lines, FONT_13B, FRONT_W - 26);
	for (int i = 0; i < n; i++)
		gfx_text_fit(FONT_13B, 13, FRONT_H - 14 - (n - 1 - i) * 16, lines[i], RGB(255, 255, 255), FRONT_W - 26);

	// Hinge shadow and a diagonal shine
	for (int x = 0; x < 6; x++)
		gfx_rect(x, 0, 1, FRONT_H, RGB(0, 0, 0), 96 * (6 - x) / 6);
	for (int y = 0; y < FRONT_H; y++) {
		for (int x = 0; x < FRONT_W; x++) {
			float t = (x * 0.906f + y * 0.423f) / (FRONT_W * 0.906f + FRONT_H * 0.423f);
			int a = t < 0.35f ? (int)(56 * (1 - t / 0.35f)) : t > 0.7f ? (int)(18 * (t - 0.7f) / 0.3f) : 0;
			gfx_blend(x, y, RGB(255, 255, 255), a);
		}
	}
	tex_from_surface(&c->front, &s, GX_TF_RGB565);
}

static void generate_spine(Cover *c, const Game *g)
{
	const int FRONT_H = case_shape(g)->front_h;
	// Draw the title sideways on a 228x24 strip, then turn it to read top to bottom.
	Surface strip = { scratch2, FRONT_H, SPINE_W };
	gfx_target(&strip);
	gfx_clear(0xFF000000 | c->base);
	char upper[64];
	snprintf(upper, sizeof(upper), "%s", g->title);
	for (char *p = upper; *p; p++)
		*p = toupper((u8)*p);
	gfx_text_fit(FONT_9K, 10, gfx_center_baseline(FONT_9K, SPINE_W / 2), upper, RGB(255, 255, 255), FRONT_H - 20);

	Surface s = { scratch, SPINE_W, FRONT_H };
	for (int y = 0; y < SPINE_W; y++)
		for (int x = 0; x < FRONT_H; x++)
			scratch[x * SPINE_W + (SPINE_W - 1 - y)] = scratch2[y * FRONT_H + x];
	tex_from_surface(&c->spine, &s, GX_TF_RGB565);
}

static void load_cover(int index)
{
	Cover *c = &cache[index];
	const Game *g = &games[index];
	u32 hue = hash(g->title) % 360;
	if (g->kind == KIND_APP)
		hue = !strcmp(g->key, "fml-music") ? 214 : 24;
	else if (g->kind == KIND_DRIVE && !g->id[0])
		hue = 230;
	c->base = gfx_hsl(hue + 45, 0.60f, 0.22f);
	load_cov(c, g->key);
	if (!c->front.ready)
		generate_front(c, g, hue);
	if (!c->spine.ready)
		generate_spine(c, g);
	c->loaded = true;
}

void covers_update(int selected)
{
	for (int i = 0; i < game_count; i++) {
		int d = i > selected ? i - selected : selected - i;
		if (cache[i].loaded && d > KEEP_RANGE) {
			tex_free(&cache[i].front);
			tex_free(&cache[i].spine);
			tex_free(&cache[i].back);
			cache[i].loaded = false;
		}
	}
	for (int d = 0; d <= LOAD_RANGE; d++) {
		for (int sign = 1; sign >= -1; sign -= 2) {
			int i = selected + d * sign;
			if (i >= 0 && i < game_count && !cache[i].loaded) {
				load_cover(i);
				return;
			}
			if (d == 0)
				break;
		}
	}
}
