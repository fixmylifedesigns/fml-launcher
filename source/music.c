#include <gccore.h>
#include <asndlib.h>
#include <mp3player.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gx.h"
#include "library.h"
#include "music.h"
#include "sound.h"
#include "xpui.h"

#define WHITE 0xFFFFFF
#define C_TEXT 0xDDE8FF
#define C_DIM 0x8EA3C8
#define C_ACCENT 0x7CC4FF

// ---------------------------------------------------------------- reading files

// A song is read from the card, or (demo card) from memory
typedef struct {
	FILE *f;
	const u8 *mem;
	u32 size, pos;
} Src;

static bool src_open(Src *s, const MediaFile *m)
{
	memset(s, 0, sizeof(*s));
	const u8 *data;
	u32 size;
	if (library_demo_file(m->path, &data, &size)) {
		s->mem = data;
		s->size = size;
		return true;
	}
	s->f = fopen(m->path, "rb");
	s->size = m->size;
	return s->f != NULL;
}

static int src_read(Src *s, void *buf, int len)
{
	if (s->f)
		return fread(buf, 1, len, s->f);
	if (!s->mem)
		return 0;
	if (s->pos >= s->size)
		return 0;
	if ((u32)len > s->size - s->pos)
		len = s->size - s->pos;
	memcpy(buf, s->mem + s->pos, len);
	s->pos += len;
	return len;
}

static void src_seek(Src *s, u32 pos)
{
	if (s->f)
		fseek(s->f, pos, SEEK_SET);
	else
		s->pos = pos < s->size ? pos : s->size;
}

static void src_close(Src *s)
{
	if (s->f)
		fclose(s->f);
	memset(s, 0, sizeof(*s));
}

// ---------------------------------------------------------------- tags

typedef struct {
	const MediaFile *file;
	char title[64], artist[48], album[48];
	u32 seconds;              // 0 = unknown
	u32 data_start, data_end; // the audio, without tags
	bool parsed, bad;
} Track;

static Track *tracks;
static int track_count;

// Accented Latin-1 letters (0xC0-0xFF) without their accents, for the ASCII font
static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";

static void put_char(char *out, int *o, int n, u32 cp)
{
	if (*o >= n - 1)
		return;
	if (cp < 0x20)
		return;
	if (cp < 0x7F)
		out[(*o)++] = cp;
	else if (cp >= 0xC0 && cp <= 0xFF)
		out[(*o)++] = latin1[cp - 0xC0];
	else if (cp == 0x2019 || cp == 0x2018)
		out[(*o)++] = '\'';
	else if (cp == 0x2013 || cp == 0x2014)
		out[(*o)++] = '-';
	else if (cp >= 0xA0)
		out[(*o)++] = cp == 0xA0 ? ' ' : '?';
}

// ID3 text frame body (encoding byte first) to ASCII
static void decode_text(const u8 *p, int n, char *out, int out_n)
{
	int o = 0;
	if (n >= 1) {
		int enc = *p++;
		n--;
		if (enc == 0) {
			for (int i = 0; i < n && p[i]; i++)
				put_char(out, &o, out_n, p[i]);
		} else if (enc == 1 || enc == 2) {
			bool le = false;
			if (enc == 1 && n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
				le = p[0] == 0xFF;
				p += 2;
				n -= 2;
			}
			for (int i = 0; i + 1 < n; i += 2) {
				u32 cp = le ? p[i] | (p[i + 1] << 8) : (p[i] << 8) | p[i + 1];
				if (!cp)
					break;
				put_char(out, &o, out_n, cp);
			}
		} else {
			for (int i = 0; i < n && p[i];) {
				u32 c = p[i++];
				if (c >= 0xC0) {
					int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
					c &= 0x3F >> more;
					while (more-- && i < n)
						c = (c << 6) | (p[i++] & 0x3F);
				}
				put_char(out, &o, out_n, c);
			}
		}
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = 0;
}

static u32 syncsafe(const u8 *p)
{
	return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3];
}

static void parse_id3v2(Track *t, const u8 *tag, u32 n, int ver)
{
	u32 pos = 0;
	int hdr = ver == 2 ? 6 : 10;
	while (pos + hdr <= n && tag[pos]) {
		const u8 *f = tag + pos;
		u32 size = ver == 2 ? (f[3] << 16) | (f[4] << 8) | f[5] : ver == 4 ? syncsafe(f + 4) : (f[4] << 24) | (f[5] << 16) | (f[6] << 8) | f[7];
		if (size > n - pos - hdr)
			break;
		const u8 *body = f + hdr;
		if (!memcmp(f, ver == 2 ? "TT2" : "TIT2", ver == 2 ? 3 : 4))
			decode_text(body, size, t->title, sizeof(t->title));
		else if (!memcmp(f, ver == 2 ? "TP1" : "TPE1", ver == 2 ? 3 : 4))
			decode_text(body, size, t->artist, sizeof(t->artist));
		else if (!memcmp(f, ver == 2 ? "TAL" : "TALB", ver == 2 ? 3 : 4))
			decode_text(body, size, t->album, sizeof(t->album));
		pos += hdr + size;
	}
}

// Length from the first MPEG frame: the Xing/Info or VBRI frame count for
// variable bitrate files, else the bitrate.
static void read_length(Track *t, Src *s)
{
	static const u16 rates1[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
	static const u16 rates2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
	static const u16 freqs[] = { 44100, 48000, 32000 };
	static u8 buf[4096];
	src_seek(s, t->data_start);
	int n = src_read(s, buf, sizeof(buf));
	for (int i = 0; i + 4 <= n; i++) {
		if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0)
			continue;
		int ver = (buf[i + 1] >> 3) & 3, layer = (buf[i + 1] >> 1) & 3;
		int bri = buf[i + 2] >> 4, sri = (buf[i + 2] >> 2) & 3;
		if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3)
			continue;
		bool mpeg1 = ver == 3, mono = (buf[i + 3] >> 6) == 3;
		u32 rate = freqs[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
		u32 kbps = mpeg1 ? rates1[bri] : rates2[bri];
		u32 spf = mpeg1 ? 1152 : 576;
		int side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
		const u8 *x = buf + i + 4 + side;
		const u8 *v = buf + i + 4 + 32;
		if (x + 12 <= buf + n && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) && (x[7] & 1)) {
			u32 frames = (x[8] << 24) | (x[9] << 16) | (x[10] << 8) | x[11];
			t->seconds = (u32)((u64)frames * spf / rate);
		} else if (v + 18 <= buf + n && !memcmp(v, "VBRI", 4)) {
			u32 frames = (v[14] << 24) | (v[15] << 16) | (v[16] << 8) | v[17];
			t->seconds = (u32)((u64)frames * spf / rate);
		} else {
			u32 audio = t->data_end > t->data_start + i ? t->data_end - t->data_start - i : 0;
			t->seconds = audio / (kbps * 125);
		}
		return;
	}
}

// "Artist - Title.mp3" when the file has no tags
static void title_from_name(Track *t)
{
	const char *name = strrchr(t->file->path, '/') + 1;
	char base[128];
	snprintf(base, sizeof(base), "%s", name);
	char *dot = strrchr(base, '.');
	if (dot)
		*dot = 0;
	char *dash = strstr(base, " - ");
	if (!t->title[0]) {
		if (dash && !t->artist[0]) {
			*dash = 0;
			snprintf(t->artist, sizeof(t->artist), "%s", base);
			snprintf(t->title, sizeof(t->title), "%s", dash + 3);
		} else {
			snprintf(t->title, sizeof(t->title), "%s", base);
		}
	}
}

static void parse_track(Track *t)
{
	t->parsed = true;
	Src s;
	if (!src_open(&s, t->file)) {
		t->bad = true;
		title_from_name(t);
		return;
	}
	t->data_end = s.size;
	u8 h[10];
	if (src_read(&s, h, 10) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
		u32 size = syncsafe(h + 6);
		t->data_start = 10 + size + (h[5] & 0x10 ? 10 : 0);
		u32 n = size < 128 * 1024 ? size : 128 * 1024;
		u8 *tag = malloc(n);
		if (tag && (u32)src_read(&s, tag, n) == n)
			parse_id3v2(t, tag, n, h[3]);
		free(tag);
	}
	// ID3v1 at the end
	if (s.size > 128 + t->data_start) {
		u8 v1[128];
		src_seek(&s, s.size - 128);
		if (src_read(&s, v1, 128) == 128 && !memcmp(v1, "TAG", 3)) {
			t->data_end = s.size - 128;
			u8 field[31];
			field[0] = 0; // Latin-1
			if (!t->title[0]) {
				memcpy(field + 1, v1 + 3, 30);
				decode_text(field, 31, t->title, sizeof(t->title));
			}
			if (!t->artist[0]) {
				memcpy(field + 1, v1 + 33, 30);
				decode_text(field, 31, t->artist, sizeof(t->artist));
			}
			if (!t->album[0]) {
				memcpy(field + 1, v1 + 63, 30);
				decode_text(field, 31, t->album, sizeof(t->album));
			}
		}
	}
	read_length(t, &s);
	src_close(&s);
	title_from_name(t);
}

// ---------------------------------------------------------------- playing

static Src play_src;
static volatile s64 seek_to = -1;
static volatile u32 samples, rate = 44100;
static volatile float levels[32];
static int cur = -1, sel, scroll;
static bool playing, paused;
static int volume = 200;
static bool shuffle;
static int repeat; // 0 off, 1 all, 2 one
static int since_start;
static char now_playing[120];
static char message[96];
static int message_frames;

static s32 reader(void *cb, void *buf, s32 len)
{
	(void)cb;
	s64 to = seek_to;
	if (to >= 0) {
		src_seek(&play_src, (u32)to);
		seek_to = -1;
	}
	return src_read(&play_src, buf, len);
}

// Called by the MP3 player for every decoded frame: counts the time played
// and measures how loud each of the 32 frequency bands is, for the bars.
static void filter(struct mad_stream *stream, struct mad_frame *frame)
{
	(void)stream;
	int ns = MAD_NSBSAMPLES(&frame->header), nch = MAD_NCHANNELS(&frame->header);
	samples += 32 * ns;
	rate = frame->header.samplerate ? frame->header.samplerate : 44100;
	for (int sb = 0; sb < 32; sb++) {
		u32 acc = 0;
		for (int ch = 0; ch < nch; ch++)
			for (int s = 0; s < ns; s++) {
				s32 v = frame->sbsample[ch][s][sb];
				acc += (u32)(v < 0 ? -v : v) >> 12;
			}
		levels[sb] = (float)acc / (ns * nch) / 65536.0f;
	}
}

static void show_message(const char *text)
{
	snprintf(message, sizeof(message), "%s", text);
	message_frames = 180;
}

static void stop_playing(void)
{
	if (playing)
		MP3Player_Stop();
	src_close(&play_src);
	playing = paused = false;
	ASND_Pause(0);
	now_playing[0] = 0;
	for (int i = 0; i < 32; i++)
		levels[i] = 0;
}

static bool play(int i)
{
	if (i < 0 || i >= track_count)
		return false;
	stop_playing();
	Track *t = &tracks[i];
	if (!t->parsed)
		parse_track(t);
	if (t->bad || !src_open(&play_src, t->file)) {
		show_message("This song couldn't be opened.");
		return false;
	}
	src_seek(&play_src, t->data_start);
	sound_start();
	samples = 0;
	seek_to = -1;
	cur = i;
	paused = false;
	since_start = 0;
	MP3Player_Volume(volume);
	MP3Player_PlayFile(NULL, reader, filter);
	playing = true;
	if (t->artist[0])
		snprintf(now_playing, sizeof(now_playing), "%s - %s", t->artist, t->title);
	else
		snprintf(now_playing, sizeof(now_playing), "%s", t->title);
	return true;
}

static float elapsed(void)
{
	return playing ? (float)samples / rate : 0;
}

static int next_track(int dir)
{
	if (track_count == 0)
		return -1;
	if (shuffle && track_count > 1) {
		int n;
		do
			n = rand() % track_count;
		while (n == cur);
		return n;
	}
	int n = cur + dir;
	if (n < 0 || n >= track_count)
		return repeat == 1 || dir < 0 ? (n + track_count) % track_count : -1;
	return n;
}

static void seek_by(float seconds)
{
	if (!playing || cur < 0)
		return;
	Track *t = &tracks[cur];
	if (!t->seconds)
		return;
	float to = elapsed() + seconds;
	if (to < 0)
		to = 0;
	if (to > t->seconds - 1)
		to = t->seconds - 1;
	u32 span = t->data_end - t->data_start;
	seek_to = t->data_start + (u32)((double)span * to / t->seconds);
	samples = (u32)(to * rate);
}

static void toggle_pause(void)
{
	if (!playing) {
		play(sel);
		return;
	}
	paused = !paused;
	ASND_Pause(paused);
}

void music_tick(void)
{
	if (!playing)
		return;
	since_start++;
	if (!paused && since_start > 30 && !MP3Player_IsPlaying()) {
		int n = repeat == 2 ? cur : next_track(1);
		if (n < 0)
			stop_playing();
		else
			play(n);
	}
}

const char *music_now_playing(void)
{
	return playing ? now_playing : NULL;
}

void music_stop(void)
{
	stop_playing();
}

// ---------------------------------------------------------------- the app

void music_open(void)
{
	const MediaFile *files;
	int n = library_media(MEDIA_MUSIC, &files);
	// Keep the list (and what's playing) if the songs on the card are the same
	if (!(tracks && n == track_count && (n == 0 || tracks[0].file == &files[0]))) {
		stop_playing();
		free(tracks);
		tracks = n ? calloc(n, sizeof(Track)) : NULL;
		track_count = tracks ? n : 0;
		for (int i = 0; i < track_count; i++)
			tracks[i].file = &files[i];
		cur = -1;
		sel = scroll = 0;
	}
	if (cur >= 0)
		sel = cur;
	srand(time(NULL));
	message_frames = 0;
}

bool music_input(u32 down, u32 held)
{
	if (down & PAD_BUTTON_B)
		return false;

	// List cursor, repeating while held
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
	if (sel >= track_count) sel = track_count - 1;

	if (down & PAD_BUTTON_A) {
		if (playing && sel == cur)
			toggle_pause();
		else
			play(sel);
	}
	if (down & PAD_BUTTON_START)
		toggle_pause();
	if (down & PAD_TRIGGER_Z)
		stop_playing();
	if ((down & PAD_TRIGGER_L) && track_count) {
		if (playing && elapsed() > 3) {
			play(cur);
		} else {
			int n = next_track(-1);
			if (n >= 0 && play(n))
				sel = n;
		}
	}
	if ((down & PAD_TRIGGER_R) && track_count) {
		int n = playing ? next_track(1) : sel;
		if (n < 0)
			n = 0;
		if (play(n))
			sel = n;
	}
	if (down & PAD_BUTTON_X)
		shuffle = !shuffle;
	if (down & PAD_BUTTON_Y)
		repeat = (repeat + 1) % 3;

	// C-stick: left/right seeks, up/down sets the volume
	static int seek_hold;
	s8 cx = PAD_SubStickX(0), cy = PAD_SubStickY(0);
	if (cx > 60 || cx < -60) {
		if (seek_hold % 12 == 0)
			seek_by(cx > 0 ? 5 : -5);
		seek_hold++;
	} else {
		seek_hold = 0;
	}
	if (cy > 60 || cy < -60) {
		volume += cy > 0 ? 3 : -3;
		if (volume < 0) volume = 0;
		if (volume > 255) volume = 255;
		if (playing)
			MP3Player_Volume(volume);
	}
	return true;
}

// ---------------------------------------------------------------- drawing

static void format_time(char *out, size_t n, u32 s)
{
	snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static u32 hash(const char *s)
{
	u32 h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (u8)*s) * 16777619u;
	return h;
}

#define BARS 20
static float bar_h[BARS], peak_h[BARS];
static int peak_wait[BARS];

static void draw_bars(float x, float y, float w, float h)
{
	static const u8 edges[BARS + 1] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 21, 24, 28, 32 };
	float gap = 4, bw = (w - gap * (BARS - 1)) / BARS;
	const int seg_h = 6;
	int segs = (int)(h / seg_h);
	for (int i = 0; i < BARS; i++) {
		float v = 0;
		if (playing && !paused) {
			for (int b = edges[i]; b < edges[i + 1]; b++)
				v += levels[b];
			v /= edges[i + 1] - edges[i];
			v = sqrtf(v * (1 + i * 0.7f) * 16);
			if (v > 1)
				v = 1;
		}
		bar_h[i] = v > bar_h[i] ? v : bar_h[i] * 0.88f;
		if (bar_h[i] >= peak_h[i]) {
			peak_h[i] = bar_h[i];
			peak_wait[i] = 24;
		} else if (peak_wait[i] > 0) {
			peak_wait[i]--;
		} else {
			peak_h[i] -= 0.012f;
			if (peak_h[i] < 0)
				peak_h[i] = 0;
		}
		float bx = x + i * (bw + gap);
		int lit = (int)(bar_h[i] * segs + 0.5f);
		gx_texture(NULL);
		for (int s = 0; s < segs; s++) {
			float sy = y + h - (s + 1) * seg_h;
			int t = s * 256 / segs;
			u32 c = s < lit ? gfx_mix(0x1E6BFF, 0x8FF0FF, t) : 0x0E1A33;
			gx_quad(bx, sy + 1, bw, seg_h - 2, rgba(c, s < lit ? 255 : 160));
			// Reflection under the bars
			if (s < lit && s < 6)
				gx_quad(bx, y + h + 3 + s * 3, bw, 2, rgba(c, 50 - s * 8));
		}
		int ps = (int)(peak_h[i] * segs);
		if (ps > 0)
			gx_quad(bx, y + h - ps * seg_h - 3, bw, 2, rgba(0xE6F6FF, 230));
	}
}

static void draw_now_playing(float x, float y, float w, float h)
{
	xp_panel(x, y, w, h);
	const Track *t = cur >= 0 ? &tracks[cur] : track_count && sel < track_count ? &tracks[sel] : NULL;
	if (!track_count) {
		gx_text_centered(FONT_19B, x + w / 2, y + h / 2 - 8, "No music found", rgba(C_TEXT, 255));
		gx_text_centered(FONT_12, x + w / 2, y + h / 2 + 16, "Copy .mp3 files anywhere on the SD card.", rgba(C_DIM, 255));
		return;
	}
	// Album tile in a colour from the album (or title), with a note on it
	if (t) {
		u32 hue = hash(t->album[0] ? t->album : t->title) % 360;
		gx_round_rect(x + 12, y + 12, 64, 64, 6, rgba(gfx_hsl(hue, 0.6f, 0.45f), 255));
		gx_round_rect(x + 12, y + 44, 64, 32, 6, rgba(gfx_hsl(hue + 40, 0.65f, 0.28f), 200));
		gx_round_rect(x + 14, y + 14, 60, 24, 5, rgba(WHITE, 40));
		xp_glyph(XG_NOTE, x + 44, y + 44, 40, rgba(WHITE, 230));
		float tx = x + 90;
		int tw = w - 104;
		gx_text_fit(FONT_19B, tx, y + 34, t->title, rgba(WHITE, 255), tw);
		gx_text_fit(FONT_13M, tx, y + 55, t->artist[0] ? t->artist : "Unknown artist", rgba(C_TEXT, 255), tw);
		gx_text_fit(FONT_12, tx, y + 73, t->album[0] ? t->album : "", rgba(C_DIM, 255), tw);
		if (cur < 0)
			gx_text(FONT_12, tx, y + 73, "Press A to play", rgba(C_ACCENT, 255));
		else if (paused)
			gx_text(FONT_11B, x + w - 64, y + 22, "PAUSED", rgba(0xFFD54A, 255));
	}
	draw_bars(x + 14, y + 92, w - 28, h - 92 - 34);
	if (message_frames > 0) {
		message_frames--;
		gx_text_centered(FONT_12, x + w / 2, y + h - 10, message, rgba(0xFF8F7A, 255));
	}
}

static void draw_playlist(float x, float y, float w, float h)
{
	xp_panel(x, y, w, h);
	gx_texture(NULL);
	gx_quad_grad(x, y, w, 26, rgba(0x2A4778, 255), rgba(0x15274A, 255));
	char head[48];
	snprintf(head, sizeof(head), "%d song%s", track_count, track_count == 1 ? "" : "s");
	gx_text(FONT_13S, x + 10, gfx_center_baseline(FONT_13S, y + 13), "Playlist", rgba(WHITE, 255));
	gx_text(FONT_12, x + w - 10 - gfx_text_width(FONT_12, head), gfx_center_baseline(FONT_12, y + 13), head, rgba(C_DIM, 255));

	const int row_h = 20;
	int rows = (int)((h - 32) / row_h);
	if (sel < scroll) scroll = sel;
	if (sel >= scroll + rows) scroll = sel - rows + 1;
	if (scroll < 0) scroll = 0;
	// Read the tags of the songs on screen, one per frame
	for (int i = scroll; i < scroll + rows && i < track_count; i++)
		if (!tracks[i].parsed) {
			parse_track(&tracks[i]);
			break;
		}
	for (int r = 0; r < rows && scroll + r < track_count; r++) {
		int i = scroll + r;
		const Track *t = &tracks[i];
		float ry = y + 30 + r * row_h;
		if (i == sel)
			gx_round_rect(x + 4, ry, w - 8, row_h - 2, 4, rgba(0x2F62B8, 255));
		bool now = i == cur && playing;
		u32 c = now ? C_ACCENT : i == sel ? WHITE : C_TEXT;
		char num[8], len[12] = "";
		snprintf(num, sizeof(num), "%d", i + 1);
		if (now)
			xp_glyph(paused ? XG_PAUSE : XG_PLAY, x + 18, ry + row_h / 2 - 1, 11, rgba(C_ACCENT, 255));
		else
			gx_text(FONT_12, x + 26 - gfx_text_width(FONT_12, num), ry + 14, num, rgba(C_DIM, 255));
		if (t->seconds)
			format_time(len, sizeof(len), t->seconds);
		int lw = gfx_text_width(FONT_12, len);
		char label[120];
		const char *title = t->parsed ? t->title : strrchr(t->file->path, '/') + 1;
		if (t->parsed && t->artist[0])
			snprintf(label, sizeof(label), "%s - %s", t->title, t->artist);
		else
			snprintf(label, sizeof(label), "%s", title);
		gx_text_fit(FONT_12, x + 34, ry + 14, label, rgba(c, 255), w - 34 - lw - 16);
		gx_text(FONT_12, x + w - 10 - lw, ry + 14, len, rgba(C_DIM, 255));
	}
	// Scroll bar
	if (track_count > rows) {
		float track_h = h - 36, bar = track_h * rows / track_count;
		float by = y + 32 + (track_h - bar) * scroll / (track_count - rows);
		gx_round_rect(x + w - 4, by, 3, bar, 1.5f, rgba(0x6F93D0, 200));
	}
}

void music_draw(void)
{
	int hx, hcy, hend;
	gx_2d();
	xp_desktop(&hx, &hcy, &hend);
	XpWindow win;
	xp_layout(&win);
	xp_window(&win, "FML Media Player - Music", XG_NOTE);

	// Toolbar
	float ty = win.cy, tx = win.cx;
	gx_texture(NULL);
	gx_quad_grad(tx, ty, win.cw, 34, rgba(0x2B4677, 255), rgba(0x15264A, 255));
	gx_quad(tx, ty + 34, win.cw, 1, rgba(0x060D1C, 255));
	gx_round_rect(tx + 8, ty + 5, 124, 24, 12, rgba(0x3C8DFF, 150));
	xp_glyph(XG_NOTE, tx + 24, ty + 17, 16, rgba(WHITE, 255));
	gx_text(FONT_13S, tx + 36, gfx_center_baseline(FONT_13S, ty + 17), "Now Playing", rgba(WHITE, 255));
	static const char *const repeat_names[] = { "Repeat: Off", "Repeat: All", "Repeat: One" };
	const char *shuf = shuffle ? "Shuffle: On" : "Shuffle: Off";
	int cx = win.cx + win.cw - 8 - xp_chip_width("X", shuf) - 8 - xp_chip_width("Y", repeat_names[repeat]);
	cx = xp_chip(cx, ty + 17, "X", shuf, shuffle);
	xp_chip(cx, ty + 17, "Y", repeat_names[repeat], repeat != 0);

	// Now playing and the playlist
	float top = ty + 42, bottom = win.cy + win.ch - 92;
	float pl_w = gx_wide ? 320 : 232;
	float vis_w = win.cw - 24 - pl_w;
	draw_now_playing(win.cx + 8, top, vis_w, bottom - top);
	draw_playlist(win.cx + 16 + vis_w, top, pl_w, bottom - top);

	// Seek bar with the times
	const Track *t = cur >= 0 ? &tracks[cur] : NULL;
	char a[16] = "0:00", b[16] = "0:00";
	float pos = elapsed();
	if (t && t->seconds && pos > t->seconds)
		pos = t->seconds;
	format_time(a, sizeof(a), (u32)pos);
	if (t && t->seconds)
		format_time(b, sizeof(b), t->seconds);
	float sy = bottom + 14;
	gx_text(FONT_12, win.cx + 10, gfx_center_baseline(FONT_12, sy), a, rgba(C_TEXT, 255));
	gx_text(FONT_12, win.cx + win.cw - 10 - gfx_text_width(FONT_12, b), gfx_center_baseline(FONT_12, sy), b, rgba(C_TEXT, 255));
	xp_slider(win.cx + 50, sy, win.cw - 100, t && t->seconds ? pos / t->seconds : 0, t != NULL);

	// Transport: stop, previous, play/pause, next, then the volume
	float by = bottom + 30, bh = 56, mid = win.cx + win.cw / 2, bcy = by + bh / 2;
	xp_transport(win.cx + 8, by, win.cw - 16, bh);
	xp_round_button(mid - 104, bcy, 13, XG_STOP, false, false);
	xp_round_button(mid - 56, bcy, 15, XG_PREV, false, false);
	xp_round_button(mid, bcy, 22, playing && !paused ? XG_PAUSE : XG_PLAY, true, playing && !paused);
	xp_round_button(mid + 56, bcy, 15, XG_NEXT, false, false);
	// Button names under the buttons
	gx_text_centered(FONT_11B, mid - 104, by + bh - 3, "Z", rgba(C_DIM, 255));
	gx_text_centered(FONT_11B, mid - 56, by + bh - 3, "L", rgba(C_DIM, 255));
	gx_text_centered(FONT_11B, mid + 56, by + bh - 3, "R", rgba(C_DIM, 255));
	float vx = win.cx + win.cw - 16 - 120;
	xp_glyph(XG_VOLUME, vx - 16, bcy, 18, rgba(C_TEXT, 255));
	xp_slider(vx, bcy, 100, volume / 255.0f, true);
	// Song on the left of the transport
	if (t) {
		gx_text_fit(FONT_13S, win.cx + 22, bcy - 3, t->title, rgba(WHITE, 255), mid - 130 - win.cx - 30);
		gx_text_fit(FONT_12, win.cx + 22, bcy + 14, t->artist[0] ? t->artist : "", rgba(C_DIM, 255), mid - 130 - win.cx - 30);
	}

	// Controller hints on the taskbar
	struct { const char *b, *l; } hints[] = {
		{ "A", playing && sel == cur && !paused ? "Pause" : "Play" },
		{ "L", "" }, { "R", "Song" }, { "C", "Seek / Volume" }, { "Z", "Stop" },
	};
	int x = hx;
	for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
		if (!hints[i].l[0]) {
			gx_round_rect(x, hcy - 9, 18, 18, 9, rgba(WHITE, 255));
			gx_text_centered(FONT_11B, x + 9, gfx_center_baseline(FONT_11B, hcy), hints[i].b, rgba(0x10204A, 255));
			x += 22;
			continue;
		}
		if (x + xp_hint_width(hints[i].b, hints[i].l) > hend)
			break;
		x = xp_hint(x, hcy, hints[i].b, hints[i].l);
	}
}
