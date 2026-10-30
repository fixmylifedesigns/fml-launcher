#include <gccore.h>
#include <asndlib.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

#include "gx.h"
#include "library.h"
#include "movie.h"
#include "music.h"
#include "pl_mpeg.h"
#include "sound.h"
#include "xpui.h"

#define WHITE 0xFFFFFF
#define C_TEXT 0xDDE8FF
#define C_DIM 0x8EA3C8
#define C_ACCENT 0x7CC4FF

static const MediaFile *files;
static int file_count, sel, scroll;
static char message[120];
static int message_frames;

// Playback
static plm_t *plm;
static bool playing, paused, ended;
static Tex frame_tex;
static u16 *tiles;
static int tex_w, tex_h, show_w, show_h;
static bool have_frame;
static double duration;
static u64 last_tick;
static int controls_frames; // the control bar shows for a while after any button
static int volume = 220;
static char title[96];

// ---------------------------------------------------------------- video frames

static int ytab[256], rv[256], gu[256], gv[256], bu[256];
static u8 clamp_tab[1024];
static bool tables_ready;

static void make_tables(void)
{
	for (int i = 0; i < 256; i++) {
		ytab[i] = (int)(1.164f * (i - 16) * 65536);
		rv[i] = (int)(1.596f * (i - 128) * 65536);
		gu[i] = (int)(0.391f * (i - 128) * 65536);
		gv[i] = (int)(0.813f * (i - 128) * 65536);
		bu[i] = (int)(2.018f * (i - 128) * 65536);
	}
	for (int i = 0; i < 1024; i++) {
		int v = i - 384;
		clamp_tab[i] = v < 0 ? 0 : v > 255 ? 255 : v;
	}
	tables_ready = true;
}

#define CLAMP(v) clamp_tab[((v) >> 16) + 384]

// YCbCr 4:2:0 planes to RGB565 in the GPU's 4x4 tile order
static void on_video(plm_t *p, plm_frame_t *f, void *user)
{
	(void)p;
	(void)user;
	int w = f->y.width, h = f->y.height, cw = f->cb.width;
	if (!tiles || w != tex_w || h != tex_h) {
		free(tiles);
		tiles = memalign(32, w * h * 2);
		tex_w = w;
		tex_h = h;
	}
	if (!tiles)
		return;
	show_w = f->width;
	show_h = f->height;
	u16 *out = tiles;
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < w; tx += 4)
			for (int y = ty; y < ty + 4; y++) {
				const u8 *yr = f->y.data + y * w;
				const u8 *ur = f->cb.data + (y >> 1) * cw, *vr = f->cr.data + (y >> 1) * cw;
				for (int x = tx; x < tx + 4; x++) {
					int Y = ytab[yr[x]], u = ur[x >> 1], v = vr[x >> 1];
					int r = CLAMP(Y + rv[v]), g = CLAMP(Y - gu[u] - gv[v]), b = CLAMP(Y + bu[u]);
					*out++ = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
				}
			}
	tex_from_tiles(&frame_tex, tiles, w, h, GX_TF_RGB565);
	have_frame = true;
}

// ---------------------------------------------------------------- sound

// Decoded samples wait in a ring until the mixer wants the next chunk. Both
// happen on the main thread: the mixer's buffers are refilled every frame.
#define RING (64 * 1024)         // stereo frames
#define CHUNK 2048               // stereo frames per mixer buffer
#define BUFFERS 4
static s16 *ring;
static u32 ring_read, ring_write; // in frames, wrap at RING
static s16 *chunks[BUFFERS];
static int next_chunk;
static int voice = -1;
static int samplerate;

static void on_audio(plm_t *p, plm_samples_t *s, void *user)
{
	(void)p;
	(void)user;
	for (unsigned int i = 0; i < s->count; i++) {
		if (ring_write - ring_read >= RING) // full: drop the oldest
			ring_read++;
		s16 *o = ring + (ring_write % RING) * 2;
		for (int c = 0; c < 2; c++) {
			float v = s->interleaved[i * 2 + c] * 32767.0f;
			o[c] = v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
		}
		ring_write++;
	}
}

static void fill_chunk(s16 *dst)
{
	for (int i = 0; i < CHUNK; i++) {
		if (ring_read < ring_write) {
			const s16 *in = ring + (ring_read % RING) * 2;
			dst[i * 2] = in[0];
			dst[i * 2 + 1] = in[1];
			ring_read++;
		} else {
			dst[i * 2] = dst[i * 2 + 1] = 0;
		}
	}
	DCFlushRange(dst, CHUNK * 4);
}

static void voice_callback(s32 v)
{
	(void)v; // buffers are added from the main loop
}

static void feed_audio(void)
{
	if (voice < 0)
		return;
	if (ASND_StatusVoice(voice) == SND_UNUSED) {
		fill_chunk(chunks[next_chunk]);
		ASND_SetVoice(voice, VOICE_STEREO_16BIT, samplerate, 0, chunks[next_chunk], CHUNK * 4, volume, volume, voice_callback);
		next_chunk = (next_chunk + 1) % BUFFERS;
	}
	for (int guard = 0; guard < 2 && ASND_TestVoiceBufferReady(voice) == 1; guard++) {
		fill_chunk(chunks[next_chunk]);
		if (ASND_AddVoice(voice, chunks[next_chunk], CHUNK * 4) != SND_OK)
			break;
		next_chunk = (next_chunk + 1) % BUFFERS;
	}
}

static void start_audio(void)
{
	if (!plm_get_num_audio_streams(plm))
		return;
	if (!ring)
		ring = malloc(RING * 4);
	for (int i = 0; i < BUFFERS; i++)
		if (!chunks[i])
			chunks[i] = memalign(32, CHUNK * 4);
	if (!ring || !chunks[BUFFERS - 1])
		return;
	sound_start();
	ring_read = ring_write = 0;
	samplerate = plm_get_samplerate(plm);
	voice = ASND_GetFirstUnusedVoice();
	next_chunk = 0;
	plm_set_audio_decode_callback(plm, on_audio, NULL);
	plm_set_audio_lead_time(plm, 0.15);
}

static void stop_audio(void)
{
	if (voice >= 0)
		ASND_StopVoice(voice);
	voice = -1;
}

// ---------------------------------------------------------------- playing

static void close_video(void)
{
	stop_audio();
	if (plm)
		plm_destroy(plm);
	plm = NULL;
	playing = paused = ended = have_frame = false;
	tex_free(&frame_tex);
	free(tiles);
	tiles = NULL;
	tex_w = tex_h = 0;
}

static void show_message(const char *text)
{
	snprintf(message, sizeof(message), "%s", text);
	message_frames = 240;
}

static void open_video(const MediaFile *m)
{
	if (!m->playable) {
		show_message("This video needs converting to MPEG-1 first: run tools/make_movie.py on your PC.");
		return;
	}
	music_stop(); // one thing at a time
	if (!tables_ready)
		make_tables();
	const u8 *data;
	u32 size;
	if (library_demo_file(m->path, &data, &size))
		plm = plm_create_with_memory((uint8_t *)data, size, 0);
	else
		plm = plm_create_with_filename(m->path);
	if (!plm || !plm_probe(plm, 512 * 1024) || !plm_get_num_video_streams(plm)) {
		close_video();
		show_message("This file isn't an MPEG-1 video. Convert it with tools/make_movie.py.");
		return;
	}
	if (plm_get_width(plm) > 1024 || plm_get_height(plm) > 1024) {
		close_video();
		show_message("This video is too big. Convert it with tools/make_movie.py.");
		return;
	}
	plm_set_video_decode_callback(plm, on_video, NULL);
	start_audio();
	duration = plm_get_duration(plm);
	const char *name = strrchr(m->path, '/') + 1;
	snprintf(title, sizeof(title), "%.*s", (int)(strrchr(name, '.') - name), name);
	playing = true;
	paused = ended = false;
	controls_frames = 180;
	last_tick = gettime();
}

// Advance by the real time since last frame (capped, so a slow frame
// slows the video down instead of piling up work)
static void advance(void)
{
	u64 now = gettime();
	double dt = ticks_to_microsecs(diff_ticks(last_tick, now)) / 1e6;
	last_tick = now;
	if (!playing || paused || ended)
		return;
	if (dt > 0.1)
		dt = 0.1;
	plm_decode(plm, dt);
	feed_audio();
	if (plm_has_ended(plm)) {
		ended = true;
		controls_frames = 1 << 30;
	}
}

static void seek_by(double seconds)
{
	if (!plm)
		return;
	double to = plm_get_time(plm) + seconds;
	if (to < 0)
		to = 0;
	if (duration > 0 && to > duration - 1)
		to = duration - 1;
	ring_read = ring_write;
	plm_seek(plm, to, FALSE);
	ended = false;
	controls_frames = 180;
}

static void set_paused(bool p)
{
	paused = p;
	if (voice >= 0)
		ASND_PauseVoice(voice, p);
	controls_frames = p ? 1 << 30 : 180;
}

// ---------------------------------------------------------------- the app

void movie_open(void)
{
	file_count = library_media(MEDIA_VIDEO, &files);
	if (sel >= file_count)
		sel = 0;
	message_frames = 0;
}

void movie_close(void)
{
	close_video();
}

bool movie_input(u32 down, u32 held)
{
	if (playing) {
		if (down & PAD_BUTTON_B) {
			close_video();
			return true;
		}
		if (down)
			controls_frames = paused || ended ? 1 << 30 : 180;
		if (down & (PAD_BUTTON_A | PAD_BUTTON_START)) {
			if (ended) {
				plm_rewind(plm);
				ended = false;
				ring_read = ring_write;
				set_paused(false);
			} else {
				set_paused(!paused);
			}
		}
		if (down & PAD_TRIGGER_Z)
			close_video();
		if (down & PAD_TRIGGER_L)
			seek_by(-30);
		if (down & PAD_TRIGGER_R)
			seek_by(30);
		static int seek_hold;
		s8 cx = PAD_SubStickX(0), cy = PAD_SubStickY(0), sx = PAD_StickX(0);
		if (cx > 60 || cx < -60 || (held & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) || sx > 60 || sx < -60) {
			bool fwd = cx > 60 || sx > 60 || (held & PAD_BUTTON_RIGHT);
			if (seek_hold % 15 == 0)
				seek_by(fwd ? 10 : -10);
			seek_hold++;
		} else {
			seek_hold = 0;
		}
		if (cy > 60 || cy < -60 || (held & (PAD_BUTTON_UP | PAD_BUTTON_DOWN))) {
			volume += cy > 60 || (held & PAD_BUTTON_UP) ? 3 : -3;
			if (volume < 0) volume = 0;
			if (volume > 255) volume = 255;
			if (voice >= 0)
				ASND_ChangeVolumeVoice(voice, volume, volume);
			controls_frames = 180;
		}
		return true;
	}

	if (down & PAD_BUTTON_B)
		return false;
	static int hold;
	s8 sy = PAD_StickY(0);
	int dir = (held & PAD_BUTTON_UP) || sy > 60 ? -1 : (held & PAD_BUTTON_DOWN) || sy < -60 ? 1 : 0;
	if (dir) {
		if (hold == 0 || (hold > 18 && hold % 4 == 0))
			sel += dir;
		hold++;
	} else {
		hold = 0;
	}
	if (sel < 0) sel = 0;
	if (sel >= file_count) sel = file_count - 1;
	if ((down & (PAD_BUTTON_A | PAD_BUTTON_START)) && file_count > 0)
		open_video(&files[sel]);
	return true;
}

// ---------------------------------------------------------------- drawing

static void format_time(char *out, size_t n, double s)
{
	u32 t = s < 0 ? 0 : (u32)s;
	if (t >= 3600)
		snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60));
	else
		snprintf(out, n, "%lu:%02lu", (unsigned long)(t / 60), (unsigned long)(t % 60));
}

static void format_size(char *out, size_t n, u32 bytes)
{
	if (bytes >= 1024 * 1024)
		snprintf(out, n, "%lu.%lu MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes >> 10) % 1024 * 10 / 1024));
	else
		snprintf(out, n, "%lu KB", (unsigned long)((bytes + 1023) >> 10));
}

// Full screen video with the control bar over it
static void draw_playing(void)
{
	advance();
	gx_2d();
	gx_blend(BLEND_ALPHA);
	gx_texture(NULL);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 255));
	if (have_frame && frame_tex.ready && show_w && show_h) {
		float par = plm_get_pixel_aspect_ratio(plm);
		if (par <= 0)
			par = 1;
		float aspect = show_w * par / show_h, sw = gx_right - gx_left, sh = 480;
		float w = sw, h = sw / aspect;
		if (h > sh) {
			h = sh;
			w = sh * aspect;
		}
		gx_texture(&frame_tex);
		gx_quad_uv(320 - w / 2, 240 - h / 2, w, h, 0, 0, (float)show_w / tex_w, (float)show_h / tex_h, 0xFFFFFFFF);
	}

	if (controls_frames > 0) {
		controls_frames--;
		int a = controls_frames > 20 ? 255 : controls_frames * 255 / 20;
		float x = gx_left + 40, w = gx_right - gx_left - 80, y = 392, h = 70;
		gx_round_rect(x, y, w, h, 12, rgba(0x0A1428, a * 215 / 255));
		gx_round_rect(x + 1, y + 1, w - 2, 30, 11, rgba(0x3B5888, a * 120 / 255));
		gx_text_fit(FONT_13S, x + 16, y + 20, title, rgba(WHITE, a), w - 200);
		char t[40], cur[16], len[16];
		format_time(cur, sizeof(cur), plm_get_time(plm));
		format_time(len, sizeof(len), duration);
		snprintf(t, sizeof(t), "%s / %s", cur, len);
		gx_text(FONT_12, x + w - 16 - gfx_text_width(FONT_12, t), y + 20, t, rgba(C_TEXT, a));
		if (a > 200) {
			xp_slider(x + 16, y + 34, w - 32, duration > 0 ? plm_get_time(plm) / duration : 0, true);
			float bcy = y + 54;
			xp_round_button(x + 30, bcy, 12, paused || ended ? XG_PLAY : XG_PAUSE, true, !paused);
			struct { const char *b, *l; } hints[] = {
				{ "A", ended ? "Play again" : paused ? "Play" : "Pause" },
				{ "B", "Back" }, { "C", "Seek / Volume" }, { "L", "-30s" }, { "R", "+30s" },
			};
			int hx = x + 52;
			for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
				if (hx + xp_hint_width(hints[i].b, hints[i].l) <= x + w - 104)
					hx = xp_hint(hx, bcy, hints[i].b, hints[i].l);
			xp_glyph(XG_VOLUME, x + w - 96, bcy, 16, rgba(C_TEXT, 255));
			xp_slider(x + w - 84, bcy, 68, volume / 255.0f, false);
		}
	}
	if (ended) {
		gx_round_rect(220, 200, 200, 44, 10, rgba(0x0A1428, 220));
		gx_text_centered(FONT_15S, 320, gfx_center_baseline(FONT_15S, 222), "The end", rgba(WHITE, 255));
	}
}

static void draw_list(void)
{
	int hx, hcy, hend;
	gx_2d();
	xp_desktop(&hx, &hcy, &hend);
	XpWindow win;
	xp_layout(&win);
	xp_window(&win, "FML Media Player - Videos", XG_FILM);

	float ty = win.cy, tx = win.cx;
	gx_texture(NULL);
	gx_quad_grad(tx, ty, win.cw, 34, rgba(0x2B4677, 255), rgba(0x15264A, 255));
	gx_quad(tx, ty + 34, win.cw, 1, rgba(0x060D1C, 255));
	gx_round_rect(tx + 8, ty + 5, 116, 24, 12, rgba(0x3C8DFF, 150));
	xp_glyph(XG_FILM, tx + 24, ty + 17, 16, rgba(WHITE, 255));
	gx_text(FONT_13S, tx + 36, gfx_center_baseline(FONT_13S, ty + 17), "Video Library", rgba(WHITE, 255));
	char count[32];
	snprintf(count, sizeof(count), "%d video%s", file_count, file_count == 1 ? "" : "s");
	gx_text(FONT_12, tx + win.cw - 12 - gfx_text_width(FONT_12, count), gfx_center_baseline(FONT_12, ty + 17), count, rgba(C_DIM, 255));

	float top = ty + 42, bottom = win.cy + win.ch - 10;
	float info_w = gx_wide ? 300 : 220;
	float list_w = win.cw - 24 - info_w, lx = win.cx + 8;
	xp_panel(lx, top, list_w, bottom - top);
	if (!file_count) {
		gx_text_centered(FONT_19B, lx + list_w / 2, top + 120, "No videos found", rgba(C_TEXT, 255));
		gx_text_centered(FONT_12, lx + list_w / 2, top + 144, "Copy .mpg videos anywhere on the SD card.", rgba(C_DIM, 255));
		gx_text_centered(FONT_12, lx + list_w / 2, top + 162, "Make them with tools/make_movie.py.", rgba(C_DIM, 255));
	}
	const int row_h = 34;
	int rows = (int)((bottom - top - 8) / row_h);
	if (sel < scroll) scroll = sel;
	if (sel >= scroll + rows) scroll = sel - rows + 1;
	if (scroll < 0) scroll = 0;
	for (int r = 0; r < rows && scroll + r < file_count; r++) {
		int i = scroll + r;
		const MediaFile *m = &files[i];
		float ry = top + 4 + r * row_h;
		if (i == sel)
			gx_round_rect(lx + 4, ry, list_w - 8, row_h - 3, 5, rgba(0x2F62B8, 255));
		gx_round_rect(lx + 10, ry + 4, 34, 24, 4, rgba(m->playable ? 0x3C8DFF : 0x56627A, 255));
		xp_glyph(XG_FILM, lx + 27, ry + 16, 22, rgba(WHITE, 255));
		const char *name = strrchr(m->path, '/') + 1;
		char base[128], size[24], sub[160];
		snprintf(base, sizeof(base), "%.*s", (int)(strrchr(name, '.') - name), name);
		format_size(size, sizeof(size), m->size);
		const char *ext = strrchr(name, '.') + 1;
		if (m->playable)
			snprintf(sub, sizeof(sub), "%s \x03 %s", size, ext);
		else
			snprintf(sub, sizeof(sub), "%s \x03 %s \x03 needs converting", size, ext);
		gx_text_fit(FONT_13S, lx + 54, ry + 15, base, rgba(m->playable ? WHITE : C_DIM, 255), list_w - 70);
		gx_text_fit(FONT_12, lx + 54, ry + 29, sub, rgba(m->playable ? C_DIM : 0xFFB070, 255), list_w - 70);
	}

	// Details of the selected video
	float ix = lx + list_w + 8;
	xp_panel(ix, top, info_w, bottom - top);
	if (file_count) {
		const MediaFile *m = &files[sel];
		gx_round_rect(ix + 16, top + 16, info_w - 32, (info_w - 32) * 9 / 16, 6, rgba(m->playable ? 0x1C4FA8 : 0x3A4458, 255));
		xp_glyph(XG_FILM, ix + info_w / 2, top + 16 + (info_w - 32) * 9 / 32, 54, rgba(WHITE, 220));
		float y = top + 30 + (info_w - 32) * 9 / 16;
		const char *name = strrchr(m->path, '/') + 1;
		char folder[96], size[24];
		snprintf(folder, sizeof(folder), "%.*s", (int)(name - m->path - 4), m->path + 4);
		format_size(size, sizeof(size), m->size);
		gx_text_fit(FONT_15S, ix + 16, y + 6, name, rgba(WHITE, 255), info_w - 32);
		gx_text_fit(FONT_12, ix + 16, y + 26, folder[0] ? folder : "/", rgba(C_DIM, 255), info_w - 32);
		gx_text(FONT_12, ix + 16, y + 44, size, rgba(C_DIM, 255));
		if (m->playable)
			gx_text(FONT_12, ix + 16, y + 66, "Press A to play", rgba(C_ACCENT, 255));
		else {
			gx_text_fit(FONT_12, ix + 16, y + 66, "Can't play this format yet.", rgba(0xFFB070, 255), info_w - 32);
			gx_text_fit(FONT_12, ix + 16, y + 84, "Convert it on your PC with", rgba(C_DIM, 255), info_w - 32);
			gx_text_fit(FONT_12, ix + 16, y + 100, "tools/make_movie.py", rgba(C_TEXT, 255), info_w - 32);
		}
	}
	if (message_frames > 0) {
		message_frames--;
		gx_round_rect(lx + 8, bottom - 34, list_w - 16, 26, 6, rgba(0x4A1F14, 230));
		gx_text_fit(FONT_12, lx + 16, bottom - 17, message, rgba(0xFFC2B0, 255), list_w - 32);
	}

	if (file_count && hx + xp_hint_width("A", "Play") <= hend)
		xp_hint(hx, hcy, "A", "Play");
}

void movie_draw(void)
{
	if (playing)
		draw_playing();
	else
		draw_list();
}
