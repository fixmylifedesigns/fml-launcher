#include <gccore.h>
#include <stdio.h>
#include <string.h>

#include "gx.h"
#include "pageview.h"

#define HEADER_SIZE 64
#define SLOTS 4

typedef struct {
	char magic[4]; // "FPG1"
	u16 w, h, strip_h, strips;
	char title[48];
} FpgHeader;

typedef struct {
	int strip; // -1 = empty
	Tex tex;
} Slot;

static FILE *file;
static const u8 *mem;
static u32 mem_size;
static FpgHeader hdr;
static u32 strip_bytes;
static Slot slots[SLOTS];
static bool open_ok;

static bool read_at(u32 offset, void *dst, u32 n)
{
	if (file)
		return fseek(file, offset, SEEK_SET) == 0 && fread(dst, 1, n, file) == n;
	if (offset + n > mem_size)
		return false;
	memcpy(dst, mem + offset, n);
	return true;
}

bool page_open(const char *path, const u8 *data, u32 size)
{
	page_close();
	mem = data;
	mem_size = size;
	if (path && !(file = fopen(path, "rb")))
		return false;
	if (!read_at(0, &hdr, sizeof(hdr)) || memcmp(hdr.magic, "FPG1", 4) != 0 ||
	    hdr.w % 4 || hdr.strip_h % 4 || hdr.w > 1024 || hdr.strip_h > 1024 || hdr.strips == 0) {
		page_close();
		return false;
	}
	strip_bytes = (u32)hdr.w * hdr.strip_h * 2;
	for (int i = 0; i < SLOTS; i++)
		slots[i].strip = -1;
	open_ok = true;
	return true;
}

void page_close(void)
{
	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].tex.data)
			tex_free(&slots[i].tex);
		slots[i].strip = -1;
	}
	if (file)
		fclose(file);
	file = NULL;
	mem = NULL;
	open_ok = false;
}

int page_height(void)
{
	return open_ok ? hdr.h : 0;
}

// Returns the slot holding `strip`, loading it into the slot furthest from `near` if needed.
static Slot *get_strip(int strip, int near)
{
	for (int i = 0; i < SLOTS; i++)
		if (slots[i].strip == strip)
			return &slots[i];
	Slot *victim = &slots[0];
	int worst = -1;
	for (int i = 0; i < SLOTS; i++) {
		int d = slots[i].strip < 0 ? 1 << 20 : (slots[i].strip > near ? slots[i].strip - near : near - slots[i].strip);
		if (d > worst) {
			worst = d;
			victim = &slots[i];
		}
	}
	static u8 buf[640 * 256 * 2] __attribute__((aligned(32)));
	if (strip_bytes > sizeof(buf) || !read_at(HEADER_SIZE + (u32)strip * strip_bytes, buf, strip_bytes))
		return NULL;
	tex_from_tiles(&victim->tex, buf, hdr.w, hdr.strip_h, GX_TF_RGB565);
	victim->strip = strip;
	return victim;
}

void page_draw(float scroll)
{
	if (!open_ok)
		return;
	float scale = 640.0f / hdr.w;
	int first = (int)(scroll / (hdr.strip_h * scale));
	for (int s = first; s < hdr.strips && s * hdr.strip_h * scale < scroll + 480; s++) {
		Slot *slot = get_strip(s, first);
		if (!slot)
			continue;
		gx_texture(&slot->tex);
		gx_quad(0, s * hdr.strip_h * scale - scroll, 640, hdr.strip_h * scale, 0xFFFFFFFF);
	}
}
