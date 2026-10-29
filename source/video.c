#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"
#include "video.h"

typedef struct {
	char magic[4]; // "FMV1"
	u16 w, h, fps, frames;
	u32 frame_bytes;
	u8 pad[16];
} FmvHeader;

typedef struct {
	FILE *file;
	const u8 *mem;
	u32 size, pos;
} Source;

static bool source_read(Source *s, void *dst, u32 n)
{
	if (s->file)
		return fread(dst, 1, n, s->file) == n;
	if (s->pos + n > s->size)
		return false;
	memcpy(dst, s->mem + s->pos, n);
	s->pos += n;
	return true;
}

static bool source_skip(Source *s, u32 n)
{
	if (s->file)
		return fseek(s->file, n, SEEK_CUR) == 0;
	if (s->pos + n > s->size)
		return false;
	s->pos += n;
	return true;
}

bool video_play(const char *path, const u8 *mem, u32 mem_size)
{
	Source src = { NULL, mem, mem_size, 0 };
	if (path) {
		src.file = fopen(path, "rb");
		if (!src.file)
			return false;
	} else if (!mem) {
		return false;
	}

	bool played = false;
	u8 *buf[2] = { NULL, NULL };
	FmvHeader hdr;
	if (!source_read(&src, &hdr, sizeof(hdr)) || memcmp(hdr.magic, "FMV1", 4) != 0 ||
	    hdr.w % 8 || hdr.h % 8 || hdr.w > 1024 || hdr.h > 1024 || hdr.fps == 0 ||
	    hdr.frame_bytes != (u32)hdr.w * hdr.h / 2)
		goto done;

	buf[0] = memalign(32, hdr.frame_bytes);
	buf[1] = memalign(32, hdr.frame_bytes);
	if (!buf[0] || !buf[1])
		goto done;

	// Frames are timed by the clock, so any frame rate plays at the right
	// speed. The next frame is read while the current one is on screen; if
	// the card falls behind, frames are dropped rather than slowing down.
	u64 start = gettime();
	for (int i = 0; i < hdr.frames; i++) {
		u64 due = (u64)i * 1000000 / hdr.fps, next = (u64)(i + 1) * 1000000 / hdr.fps;
		if (i > 0 && i + 1 < hdr.frames && ticks_to_microsecs(diff_ticks(start, gettime())) >= next) {
			if (!source_skip(&src, hdr.frame_bytes))
				break;
			continue;
		}
		u8 *frame = buf[i & 1];
		if (!source_read(&src, frame, hdr.frame_bytes))
			break;
		DCFlushRange(frame, hdr.frame_bytes);
		while (ticks_to_microsecs(diff_ticks(start, gettime())) + 8000 < due)
			VIDEO_WaitVSync();

		Tex t = { .data = frame, .w = hdr.w, .h = hdr.h, .fmt = GX_TF_CMPR, .ready = true };
		GX_InitTexObj(&t.obj, frame, hdr.w, hdr.h, GX_TF_CMPR, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjFilterMode(&t.obj, GX_LINEAR, GX_LINEAR);
		GX_InvalidateTexAll();

		gx_2d();
		gx_blend(BLEND_ALPHA);
		gx_depth(false, false);
		gx_texture(&t);
		gx_quad(0, 0, 640, 480, 0xFFFFFFFF);
		gx_end_frame();
		played = true;

		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_START))
			break;
	}

done:
	// The GPU may still be reading the last frame
	GX_DrawDone();
	free(buf[0]);
	free(buf[1]);
	if (src.file)
		fclose(src.file);
	return played;
}
