#include <gccore.h>
#include <fat.h>
#include <malloc.h>
#include <math.h>
#include <sdcard/gcsd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <ogc/lwp_watchdog.h>

#include "boot.h"
#include "covers.h"
#include "disc.h"
#include "movie.h"
#include "music.h"
#include "sound.h"
#include "xpui.h"
#include "gfx.h"
#include "gx.h"
#include "library.h"
#include "scene.h"
#include "video.h"
#include "pageview.h"

// Theme - matches design/mockup.html
#define APP_NAME   "FML Launcher"
#define C_BG_TOP   RGB(26, 24, 48)
#define C_FLOOR    RGB(7, 7, 12)
#define C_HL       RGB(0, 195, 227)
#define C_TEXT     RGB(255, 255, 255)
#define C_DIM      RGB(154, 154, 176)
#define C_ERROR    RGB(255, 79, 94)
#define C_DEMO     RGB(139, 108, 255)
#define C_LOGO1    RGB(139, 108, 255)
#define C_LOGO2    RGB(62, 224, 197)
#define C_DIALOG   RGB(42, 42, 54)
#define C_INK      RGB(17, 17, 17)

#define MARGIN 32
// Screen edges in layout units (wider than 0..640 in 16:9)
#define SL ((int)gx_left)
#define SR ((int)gx_right)
#define LOGO_SIZE 40

typedef struct {
	const char *name;
	const char *swiss_prefix; // how Swiss names this device
	const char *vba_prefix;   // how Visual Boy Advance GX names it
	const DISC_INTERFACE *iface;
} Device;

static const Device devices[] = {
	{ "SD2SP2", "sdc:/", "port2:/", &__io_gcsd2 },
	{ "SD Gecko B", "sdb:/", "cardb:/", &__io_gcsdb },
	{ "SD Gecko A", "sda:/", "carda:/", &__io_gcsda },
};
#define DEVICE_COUNT (int)(sizeof(devices) / sizeof(devices[0]))

enum { SCREEN_HOME, SCREEN_NO_CARD, SCREEN_LAUNCH, SCREEN_ERROR, SCREEN_SEARCHING, SCREEN_SETTINGS, SCREEN_SEARCH, SCREEN_PAGE, SCREEN_VIDEO_CONFIRM,
       SCREEN_MUSIC, SCREEN_MOVIES, SCREEN_DISC_MENU, SCREEN_RIP };

static int device = -1;
static bool demo;
static int screen = SCREEN_HOME;
static int selected;
static int load_percent;
static const char *loading_name;
static bool fly_case; // launching a game (the case flies forward), not Swiss from Settings
static char error_title[64], error_text[160];

// Web page viewer
static float page_scroll, page_hint;

// The disc in the drive: its menu, and copying it to the card
static DiscInfo disc;
enum { DISC_PLAY, DISC_PLAY_SD, DISC_COPY, DISC_CANCEL };
static int disc_options[4], disc_option_count, disc_option;
static char rip_target[300], rip_title[64];
static int rip_stop_frames; // "Press B again to stop" window

// Tabs and search
static int tab = TAB_ALL;
static char query[24];
static int key_sel;
#define KEY_COLS 10
static const char *const keys[] = {
	"A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
	"K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
	"U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3",
	"4", "5", "6", "7", "8", "9", "Space", "Del", "Clear", "OK",
};
#define KEY_COUNT (int)(sizeof(keys) / sizeof(keys[0]))

// Settings panel
enum { SETTING_VIDEO, SETTING_SCREEN, SETTING_GAME_WIDE, SETTING_SWISS, SETTING_DEVICE, SETTING_RESTART, SETTING_COUNT };
static int setting;

// Display settings, saved in sd:/fml/settings.ini
#define SETTINGS_PATH "sd:/fml/settings.ini"
static int video_setting = VMODE_AUTO;
static bool wide_setting, game_wide;
static bool force_480i;                // B held at boot: safe mode for TVs without 480p
static int video_undo, confirm_frames; // "Keep this video mode?" countdown
#define CONFIRM_SECONDS 15

static Tex background_tex, logo_tex, gear_tex, star_tex, splash_tex;
static int splash_alpha; // 0..255, splash drawn over everything while fading out

extern const int splash_w, splash_h;
extern const u8 splash_tiles[];
#define GEAR_SIZE 28
#define STAR_SIZE 16

// Updates: a newer launcher copied to the card runs instead of the one in the
// Pico. Hold R while starting to skip it (if a bad copy ever gets there).
#define SD_LAUNCHER "sd:/fml/launcher.dol"
#define CHAIN_ARG "fml-chained"
static const char build_id[] = "FMLBUILD:" __DATE__ " " __TIME__;

// ---------------------------------------------------------------- storage

static void unmount(void)
{
	if (device >= 0) {
		fatUnmount("sd:/");
		devices[device].iface->shutdown();
		device = -1;
	}
}

// Try devices in order, starting at `start`, and keep the first that mounts.
static bool mount_from(int start)
{
	unmount();
	for (int i = 0; i < DEVICE_COUNT; i++) {
		int d = (start + i) % DEVICE_COUNT;
		const DISC_INTERFACE *iface = devices[d].iface;
		if (iface->startup() && iface->isInserted() && fatMountSimple("sd", iface)) {
			device = d;
			return true;
		}
		iface->shutdown();
	}
	return false;
}

static void render(void);

// Shows "Looking for SD card..." first, so a slow or stuck card is visible
// instead of a black screen.
static void search_card(int start)
{
	screen = SCREEN_SEARCHING;
	game_count = 0;
	render();
	render();
	mount_from(start);
}

static void load_library(void)
{
	music_stop();
	if (demo)
		library_demo();
	else if (device >= 0)
		library_scan();
	else
		game_count = 0;
	tab = TAB_ALL;
	query[0] = 0;
	covers_reset(demo);
	selected = 0;
	// Continue where you left off, else start on the first disc image
	const Game *last = library_last_played();
	for (int i = 0; i < game_count; i++)
		if (last ? !strcmp(games[i].path, last->path) : games[i].is_disc) {
			selected = i;
			break;
		}
	scene_snap(selected);
	screen = library_total() > 0 ? SCREEN_HOME : SCREEN_NO_CARD;
}

// Re-applies the tab and search, keeping the same game selected if it's still shown
static void apply_filter(void)
{
	char keep[256] = "";
	if (game_count > 0)
		snprintf(keep, sizeof(keep), "%s", games[selected].path);
	library_filter(tab, query);
	selected = 0;
	for (int i = 0; i < game_count; i++)
		if (!strcmp(games[i].path, keep)) {
			selected = i;
			break;
		}
	covers_reset(demo);
	scene_snap(selected);
}

// ---------------------------------------------------------------- display settings

static const char *const video_names[VMODE_COUNT] = { "auto", "480p", "480i" };

static void load_settings(void)
{
	FILE *f = device >= 0 ? fopen(SETTINGS_PATH, "r") : NULL;
	char line[64];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *v = strchr(line, '=');
		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(line, "video")) {
			for (int i = 0; i < VMODE_COUNT; i++)
				if (!strcmp(v, video_names[i]))
					video_setting = i;
		} else if (!strcmp(line, "screen")) {
			wide_setting = !strcmp(v, "16:9");
		} else if (!strcmp(line, "game_widescreen")) {
			game_wide = !strcmp(v, "on");
		}
	}
	if (f)
		fclose(f);
	gx_set_video(force_480i ? VMODE_480I : video_setting);
	gx_set_wide(wide_setting);
}

static void save_settings(void)
{
	if (demo || device < 0)
		return;
	mkdir("sd:/fml", 0777);
	FILE *f = fopen(SETTINGS_PATH, "w");
	if (!f)
		return;
	fprintf(f, "video=%s\nscreen=%s\ngame_widescreen=%s\n", video_names[video_setting], wide_setting ? "16:9" : "4:3",
		game_wide ? "on" : "off");
	fclose(f);
}

// Changes a display setting from the Settings panel (dir -1 / +1)
static void change_setting(int dir)
{
	if (setting == SETTING_VIDEO) {
		bool was_prog = gx_progressive();
		video_undo = video_setting;
		video_setting = (video_setting + dir + VMODE_COUNT) % VMODE_COUNT;
		force_480i = false;
		gx_set_video(video_setting);
		if (gx_progressive() != was_prog) {
			// The TV may not show the new mode: switch back unless A is pressed
			confirm_frames = CONFIRM_SECONDS * 60;
			screen = SCREEN_VIDEO_CONFIRM;
			return;
		}
	} else if (setting == SETTING_SCREEN) {
		wide_setting = !wide_setting;
		gx_set_wide(wide_setting);
	} else if (setting == SETTING_GAME_WIDE) {
		game_wide = !game_wide;
	}
	save_settings();
}

// ---------------------------------------------------------------- static art

static void make_background(void)
{
	Surface s = { memalign(32, 640 * 480 * 4), 640, 480 };
	gfx_target(&s);
	gfx_vgradient(300, 340, C_BG_TOP, C_FLOOR);
	gfx_radial(320, 120, 420, 260, C_TEXT, 0.08f);
	gfx_radial(320, 330, 260, 170, C_HL, 0.22f);
	tex_from_surface(&background_tex, &s, GX_TF_RGBA8);
	free(s.px);
}

// Gradient circle with a diamond outline
static void make_logo(void)
{
	static u32 px[LOGO_SIZE * LOGO_SIZE];
	Surface s = { px, LOGO_SIZE, LOGO_SIZE };
	gfx_target(&s);
	gfx_clear(0);
	const int d = 38, x = 1, y = 1;
	float r = d / 2.0f, cx = x + r, cy = y + r;
	for (int j = 0; j < LOGO_SIZE; j++) {
		for (int i = 0; i < LOGO_SIZE; i++) {
			float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
			float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
			if (cov <= 0)
				continue;
			int t = ((i - x) + (j - y)) * 256 / (2 * d);
			gfx_blend(i, j, gfx_mix(C_LOGO1, C_LOGO2, t), (int)(255 * (cov > 1 ? 1 : cov)));
			int hits = 0;
			for (int k = 0; k < 4; k++) {
				float m = fabsf(i + (k & 1 ? 0.75f : 0.25f) - cx) + fabsf(j + (k & 2 ? 0.75f : 0.25f) - cy);
				if (m <= d * 0.24f && m >= d * 0.13f)
					hits++;
			}
			if (hits)
				gfx_blend(i, j, C_TEXT, hits * 255 / 4);
		}
	}
	tex_from_surface(&logo_tex, &s, GX_TF_RGBA8);
}

// Settings gear: 8 teeth around a ring, 4x4 supersampled
static void make_gear(void)
{
	static u32 px[GEAR_SIZE * GEAR_SIZE];
	Surface s = { px, GEAR_SIZE, GEAR_SIZE };
	gfx_target(&s);
	gfx_clear(0);
	const float c = GEAR_SIZE / 2.0f;
	for (int j = 0; j < GEAR_SIZE; j++) {
		for (int i = 0; i < GEAR_SIZE; i++) {
			int hits = 0;
			for (int k = 0; k < 16; k++) {
				float x = i + (k % 4 + 0.5f) / 4 - c, y = j + (k / 4 + 0.5f) / 4 - c;
				float r = sqrtf(x * x + y * y), a = atan2f(y, x);
				float outer = cosf(8 * a) > 0.2f ? 9.5f : 7.5f;
				if (r <= outer && r >= 3.2f)
					hits++;
			}
			if (hits)
				gfx_blend(i, j, C_TEXT, hits * 255 / 16);
		}
	}
	tex_from_surface(&gear_tex, &s, GX_TF_RGBA8);
}

// Five-pointed star for favourites, 4x4 supersampled
static void make_star(void)
{
	static u32 px[STAR_SIZE * STAR_SIZE];
	Surface s = { px, STAR_SIZE, STAR_SIZE };
	gfx_target(&s);
	gfx_clear(0);
	float vx[10], vy[10];
	for (int k = 0; k < 10; k++) {
		float a = -1.5707963f + k * 0.6283185f, r = k & 1 ? 3.3f : 7.6f;
		vx[k] = STAR_SIZE / 2.0f + r * cosf(a);
		vy[k] = STAR_SIZE / 2.0f + 0.6f + r * sinf(a);
	}
	for (int j = 0; j < STAR_SIZE; j++)
		for (int i = 0; i < STAR_SIZE; i++) {
			int hits = 0;
			for (int q = 0; q < 16; q++) {
				float x = i + (q % 4 + 0.5f) / 4, y = j + (q / 4 + 0.5f) / 4;
				bool in = false; // even-odd point in polygon
				for (int a = 0, b = 9; a < 10; b = a++)
					if ((vy[a] > y) != (vy[b] > y) && x < (vx[b] - vx[a]) * (y - vy[a]) / (vy[b] - vy[a]) + vx[a])
						in = !in;
				hits += in;
			}
			if (hits)
				gfx_blend(i, j, C_TEXT, hits * 255 / 16);
		}
	tex_from_surface(&star_tex, &s, GX_TF_RGBA8);
}

static void draw_star(float x, float y, u32 color)
{
	gx_blend(BLEND_PREMULT);
	gx_texture(&star_tex);
	gx_quad(x, y, STAR_SIZE, STAR_SIZE, color);
	gx_blend(BLEND_ALPHA);
}

// "just now", "5 min ago", "yesterday", "3 days ago"...
static void time_ago(char *out, size_t n, u32 then)
{
	u32 now = (u32)time(NULL);
	u32 d = now > then ? now - then : 0;
	if (d < 60) snprintf(out, n, "just now");
	else if (d < 3600) snprintf(out, n, "%lu min ago", (unsigned long)(d / 60));
	else if (d < 86400) snprintf(out, n, "%lu hour%s ago", (unsigned long)(d / 3600), d < 7200 ? "" : "s");
	else if (d < 2 * 86400) snprintf(out, n, "yesterday");
	else if (d < 60 * 86400) snprintf(out, n, "%lu days ago", (unsigned long)(d / 86400));
	else snprintf(out, n, "%lu months ago", (unsigned long)(d / (30 * 86400)));
}

// ---------------------------------------------------------------- UI (drawn on the GPU every frame)

static void format_size(char *out, size_t n, u32 bytes)
{
	if (bytes >= 1024 * 1024 * 1024)
		snprintf(out, n, "%lu.%02lu GB", (unsigned long)(bytes >> 30), (unsigned long)((bytes >> 20) % 1024 * 100 / 1024));
	else if (bytes >= 1024 * 1024)
		snprintf(out, n, "%lu.%lu MB", (unsigned long)(bytes >> 20), (unsigned long)((bytes >> 10) % 1024 * 10 / 1024));
	else
		snprintf(out, n, "%lu KB", (unsigned long)((bytes + 1023) >> 10));
}

static int glyph_width(const char *glyph)
{
	return strlen(glyph) > 1 ? gfx_text_width(FONT_11B, glyph) + 10 : 20;
}

static int button_glyph(int x, int cy, const char *glyph)
{
	int w = glyph_width(glyph);
	gx_round_rect(x, cy - 10, w, 20, 10, rgba(C_TEXT, 255));
	gx_text_centered(FONT_11B, x + w / 2, gfx_center_baseline(FONT_11B, cy), glyph, rgba(C_INK, 255));
	return w;
}

static int hint_width(const char *glyph, const char *label)
{
	return glyph_width(glyph) + 6 + gfx_text_width(FONT_13M, label);
}

static int hint(int x, int cy, const char *glyph, const char *label)
{
	x += button_glyph(x, cy, glyph) + 6;
	gx_text(FONT_13M, x, gfx_center_baseline(FONT_13M, cy), label, rgba(C_DIM, 255));
	return x + gfx_text_width(FONT_13M, label) + 18;
}

static void top_bar(void)
{
	const int cy = 41;
	gx_blend(BLEND_PREMULT);
	gx_texture(&logo_tex);
	gx_quad(SL + MARGIN - 1, cy - 20, LOGO_SIZE, LOGO_SIZE, 0xFFFFFFFF);
	gx_blend(BLEND_ALPHA);
	gx_text(FONT_19B, SL + 82, gfx_center_baseline(FONT_19B, cy), APP_NAME, rgba(C_TEXT, 255));

	// The song playing in the background
	const char *song = music_now_playing();
	if (song && screen != SCREEN_SEARCHING) {
		int x = SL + 82 + gfx_text_width(FONT_19B, APP_NAME) + 16;
		int w = gfx_text_width(FONT_12, song);
		if (w > 190)
			w = 190;
		gx_round_rect(x, cy - 12, 30 + w + 12, 24, 12, rgba(C_TEXT, 18));
		xp_glyph(XG_NOTE, x + 15, cy, 14, rgba(C_HL, 255));
		gx_text_fit(FONT_12, x + 28, gfx_center_baseline(FONT_12, cy), song, rgba(C_DIM, 255), 190);
	}

	char clock[8];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	snprintf(clock, sizeof(clock), "%d:%02d", tm->tm_hour, tm->tm_min);
	int clock_x = SR - MARGIN - gfx_text_width(FONT_15S, clock);
	gx_text(FONT_15S, clock_x, gfx_center_baseline(FONT_15S, cy), clock, rgba(C_TEXT, 255));

	const char *dev = demo ? "Demo" : device >= 0 ? devices[device].name : "No card";
	u32 dot = demo ? C_DEMO : device >= 0 ? C_HL : C_ERROR;
	int chip_w = 11 + 7 + 7 + gfx_text_width(FONT_13M, dev) + 11;
	int chip_x = clock_x - 14 - chip_w;
	gx_round_rect(chip_x - 1, cy - 14, chip_w + 2, 28, 14, rgba(C_TEXT, 28));
	gx_round_rect(chip_x, cy - 13, chip_w, 26, 13, rgba(C_BG_TOP, 200));
	gx_round_rect(chip_x, cy - 13, chip_w, 26, 13, rgba(C_TEXT, 18));
	gx_circle(chip_x + 14.5f, cy, 7, rgba(dot, 60));
	gx_circle(chip_x + 14.5f, cy, 3.5f, rgba(dot, 255));
	gx_text(FONT_13M, chip_x + 25, gfx_center_baseline(FONT_13M, cy), dev, rgba(C_TEXT, 255));

	// Settings gear, highlighted while the panel is open
	if (screen == SCREEN_HOME || screen == SCREEN_SETTINGS) {
		float gx = chip_x - 12 - 14, open = screen == SCREEN_SETTINGS;
		if (open)
			gx_circle(gx, cy, 17, rgba(C_HL, 255));
		gx_circle(gx, cy, 14, rgba(open ? C_BG_TOP : C_TEXT, open ? 255 : 18));
		gx_blend(BLEND_PREMULT);
		gx_texture(&gear_tex);
		gx_quad(gx - GEAR_SIZE / 2, cy - GEAR_SIZE / 2, GEAR_SIZE, GEAR_SIZE, 0xFFFFFFFF);
		gx_blend(BLEND_ALPHA);
	}
}

static void settings_panel(void)
{
	const char *swiss = library_swiss_path();
	static const char *const titles[SETTING_COUNT] = {
		"Video output", "Screen shape", "Widescreen in games", "Open Swiss", "Switch SD card", "Restart",
	};
	char device_line[64], video_value[24];
	snprintf(device_line, sizeof(device_line), "Now using: %s", demo ? "Demo" : device >= 0 ? devices[device].name : "No card");
	if (force_480i)
		snprintf(video_value, sizeof(video_value), "480i (B held)");
	else if (video_setting == VMODE_AUTO)
		snprintf(video_value, sizeof(video_value), "Auto (%s)", gx_progressive() ? "480p" : "480i");
	else
		snprintf(video_value, sizeof(video_value), "%s", video_names[video_setting]);
	const char *values[SETTING_COUNT] = { video_value, wide_setting ? "16:9" : "4:3", game_wide ? "On" : "Off" };
	const char *subs[SETTING_COUNT] = {
		"Auto uses 480p when the cable supports it",
		"16:9 for widescreen TVs",
		"Swiss widens 3D games (some may glitch)",
		swiss ? "Swiss's own menu: settings, files, cheats" : "No swiss .dol on the SD card",
		device_line,
		"Reset the GameCube",
	};

	gx_texture(NULL);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 170));
	const int row_h = 46, w = 400, h = 60 + SETTING_COUNT * row_h + 40, x = 320 - w / 2, y = 240 - h / 2;
	gx_round_rect(x - 1, y - 1, w + 2, h + 2, 15, rgba(C_TEXT, 28));
	gx_round_rect(x, y, w, h, 14, rgba(C_DIALOG, 255));
	gx_text(FONT_19B, x + 22, y + 36, "Settings", rgba(C_TEXT, 255));

	for (int i = 0; i < SETTING_COUNT; i++) {
		int ry = y + 52 + i * row_h;
		bool on = i == setting, disabled = i == SETTING_SWISS && !swiss;
		if (i == SETTING_SWISS) { // divider between display options and actions
			gx_texture(NULL);
			gx_quad(x + 22, ry - 2, w - 44, 1, rgba(C_TEXT, 28));
		}
		if (on) {
			gx_round_rect(x + 12, ry, w - 24, row_h - 4, 10, rgba(C_HL, 255));
			gx_round_rect(x + 14, ry + 2, w - 28, row_h - 8, 8, rgba(C_DIALOG, 255));
			gx_round_rect(x + 14, ry + 2, w - 28, row_h - 8, 8, rgba(C_HL, 40));
		}
		gx_text(FONT_15S, x + 28, ry + 19, titles[i], rgba(disabled ? C_DIM : C_TEXT, 255));
		gx_text(FONT_12, x + 28, ry + 35, subs[i], rgba(C_DIM, 255));
		if (i < SETTING_SWISS) {
			char v[40];
			snprintf(v, sizeof(v), on ? "< %s >" : "%s", values[i]);
			int vw = gfx_text_width(FONT_13S, v);
			gx_text(FONT_13S, x + w - 28 - vw, ry + 19, v, rgba(on ? C_HL : C_TEXT, 255));
		}
	}

	int hx = hint(x + 22, y + h - 22, "A", "Select");
	hint(hx, y + h - 22, "B", "Close");
}

// After switching between 480p and 480i: keep it, or it switches back by itself
static void video_confirm_panel(void)
{
	gx_texture(NULL);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 200));
	const int w = 420, h = 150, x = 320 - w / 2, y = 165;
	gx_round_rect(x - 1, y - 1, w + 2, h + 2, 15, rgba(C_TEXT, 28));
	gx_round_rect(x, y, w, h, 14, rgba(C_DIALOG, 255));
	char title[48], text[80];
	snprintf(title, sizeof(title), "Keep %s?", gx_progressive() ? "480p progressive" : "480i interlaced");
	snprintf(text, sizeof(text), "Switching back in %d seconds if nothing is pressed.", (confirm_frames + 59) / 60);
	gx_text_centered(FONT_19B, 320, y + 44, title, rgba(C_TEXT, 255));
	gx_text_centered(FONT_13M, 320, y + 70, text, rgba(C_DIM, 255));
	int bw = hint_width("A", "Keep") + 18 + hint_width("B", "Switch back");
	int hx = hint(320 - bw / 2, y + 112, "A", "Keep");
	hint(hx, y + 112, "B", "Switch back");
}

// ---------------------------------------------------------------- disc menu and copying

static void panel_box(int x, int y, int w, int h)
{
	gx_texture(NULL);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 170));
	gx_round_rect(x - 1, y - 1, w + 2, h + 2, 15, rgba(C_TEXT, 28));
	gx_round_rect(x, y, w, h, 14, rgba(C_DIALOG, 255));
}

static const char *disc_option_name(int o)
{
	switch (o) {
	case DISC_PLAY: return "Play disc";
	case DISC_PLAY_SD: return "Play the SD card copy";
	case DISC_COPY: return "Copy to SD card";
	default: return "Cancel";
	}
}

static const char *disc_option_sub(int o)
{
	switch (o) {
	case DISC_PLAY: return "Starts the game from the drive";
	case DISC_PLAY_SD: return "Already on the card: loads faster and quieter";
	case DISC_COPY: return "Saves a 1.36 GB backup to games/ (about 15 min)";
	default: return "";
	}
}

static void disc_menu_panel(void)
{
	const int row_h = 46, w = 420, h = 96 + disc_option_count * row_h + 40, x = 320 - w / 2, y = 240 - h / 2;
	panel_box(x, y, w, h);
	gx_text_fit(FONT_19B, x + 22, y + 36, disc.title, rgba(C_TEXT, 255), w - 44);
	char sub[96];
	snprintf(sub, sizeof(sub), "%s \x03 %s \x03 Disc %d \x03 Version 1.%02d", disc.id, disc.region[0] ? disc.region : "Unknown region",
		disc.disc_no + 1, disc.version);
	gx_text(FONT_12, x + 22, y + 58, sub, rgba(C_DIM, 255));
	for (int i = 0; i < disc_option_count; i++) {
		int ry = y + 76 + i * row_h, o = disc_options[i];
		bool on = i == disc_option;
		if (on) {
			gx_round_rect(x + 12, ry, w - 24, row_h - 4, 10, rgba(C_HL, 255));
			gx_round_rect(x + 14, ry + 2, w - 28, row_h - 8, 8, rgba(C_DIALOG, 255));
			gx_round_rect(x + 14, ry + 2, w - 28, row_h - 8, 8, rgba(C_HL, 40));
		}
		const char *s = disc_option_sub(o);
		gx_text(FONT_15S, x + 28, s[0] ? ry + 19 : gfx_center_baseline(FONT_15S, ry + row_h / 2 - 2), disc_option_name(o), rgba(C_TEXT, 255));
		if (s[0])
			gx_text(FONT_12, x + 28, ry + 35, s, rgba(C_DIM, 255));
	}
	int hx = hint(x + 22, y + h - 22, "A", "Select");
	hint(hx, y + h - 22, "B", "Close");
}

static void rip_panel(void)
{
	RipStatus st;
	disc_rip_status(&st);
	const int w = 460, h = 200, x = 320 - w / 2, y = 140;
	panel_box(x, y, w, h);
	const char *head = st.state == RIP_DONE ? "Copied to the SD card" : st.state == RIP_FAILED ? "Copy failed"
		: st.state == RIP_CANCELLED ? "Copy stopped" : "Copying to the SD card...";
	gx_text(FONT_19B, x + 22, y + 36, head, rgba(C_TEXT, 255));
	gx_text_fit(FONT_13M, x + 22, y + 60, rip_title, rgba(C_DIM, 255), w - 44);

	float frac = st.total ? (float)st.done / st.total : 0;
	gx_round_rect(x + 22, y + 82, w - 44, 8, 4, rgba(C_TEXT, 32));
	if (frac > 0)
		gx_round_rect(x + 22, y + 82, (w - 44) * frac, 8, 4, rgba(st.state == RIP_FAILED ? C_ERROR : C_HL, 255));

	char line[160], done[24], total[24];
	format_size(done, sizeof(done), (u32)st.done);
	format_size(total, sizeof(total), (u32)st.total);
	if (st.state == RIP_RUNNING) {
		float secs = ticks_to_millisecs(gettick() - st.started) / 1000.0f;
		float speed = secs > 1 ? st.done / secs : 0;
		if (speed > 0) {
			u32 left = (u32)((st.total - st.done) / speed);
			snprintf(line, sizeof(line), "%s of %s \x03 %.1f MB/s \x03 about %lu min left", done, total, speed / (1024 * 1024),
				(unsigned long)(left / 60 + 1));
		} else {
			snprintf(line, sizeof(line), "%s of %s \x03 starting...", done, total);
		}
		gx_text(FONT_12, x + 22, y + 112, line, rgba(C_DIM, 255));
		gx_text(FONT_12, x + 22, y + 132, "You can keep the console on its own: it doesn't need you.", rgba(C_DIM, 255));
		hint(x + 22, y + h - 24, "B", rip_stop_frames > 0 ? "Press again to stop" : "Stop");
	} else if (st.state == RIP_DONE) {
		snprintf(line, sizeof(line), "Saved as %s", rip_target + 4);
		gx_text_fit(FONT_12, x + 22, y + 112, line, rgba(C_DIM, 255), w - 44);
		snprintf(line, sizeof(line), "CRC32 %08lX \x03 compare it on redump.org", (unsigned long)st.crc);
		gx_text(FONT_12, x + 22, y + 132, line, rgba(C_DIM, 255));
		int hx = hint(x + 22, y + h - 24, "A", "Play it");
		hint(hx, y + h - 24, "B", "Close");
	} else {
		gx_text_fit(FONT_12, x + 22, y + 112, st.state == RIP_FAILED ? st.error : "The unfinished copy was removed.", rgba(C_DIM, 255), w - 44);
		hint(x + 22, y + h - 24, "A", "OK");
	}
}

static void game_info(void)
{
	const Game *g = &games[selected];
	int tw = gfx_text_width(FONT_20B, g->title);
	if (tw > 540) {
		gx_text_fit(FONT_20B, 50, 382, g->title, rgba(C_TEXT, 255), 540);
		tw = 540;
	} else {
		gx_text_centered(FONT_20B, 320, 382, g->title, rgba(C_TEXT, 255));
	}
	if (g->fav)
		draw_star(320 - tw / 2 - STAR_SIZE - 8, 382 - 14, rgba(0xFFD54A, 255));

	// "Continue" on the last thing played
	const Game *last = library_last_played();
	if (last && !strcmp(last->path, g->path) && tab != TAB_RECENT) {
		int pw = gfx_text_width(FONT_11B, "CONTINUE") + 20;
		gx_round_rect(320 - pw / 2, 344, pw, 18, 9, rgba(C_HL, 255));
		gx_text_centered(FONT_11B, 320, gfx_center_baseline(FONT_11B, 353), "CONTINUE", rgba(C_INK, 255));
	}

	char size[32], sub[160];
	format_size(size, sizeof(size), g->size);
	static const char *const kind_names[] = { "", "App", "Game Boy", "Game Boy Color", "Game Boy Advance", "Web page" };
	if (g->kind == KIND_APP) {
		const MediaFile *files;
		bool music = !strcmp(g->path, "fml:music");
		int n = library_media(music ? MEDIA_MUSIC : MEDIA_VIDEO, &files);
		snprintf(sub, sizeof(sub), "Built-in app \x03 %d %s%s", n, music ? "song" : "video", n == 1 ? "" : "s");
	} else if (g->kind == KIND_DRIVE) {
		if (g->id[0])
			snprintf(sub, sizeof(sub), "In the disc drive \x03 %s%s%s \x03 Disc %d", g->id, g->region[0] ? " \x03 " : "", g->region, g->disc_no + 1);
		else
			snprintf(sub, sizeof(sub), "In the disc drive");
	} else if (g->is_disc && g->region[0])
		snprintf(sub, sizeof(sub), "%s \x03 %s \x03 %s \x03 %s", g->id, g->region, g->folder[0] ? g->folder : "/", size);
	else if (g->is_disc)
		snprintf(sub, sizeof(sub), "%s \x03 %s \x03 %s", g->id, g->folder[0] ? g->folder : "/", size);
	else
	{
		char kind[48];
		if (g->region[0])
			snprintf(kind, sizeof(kind), "%s \x03 %s", kind_names[g->kind], g->region);
		else
			snprintf(kind, sizeof(kind), "%s", kind_names[g->kind]);
		snprintf(sub, sizeof(sub), "%s \x03 %s \x03 %s", kind, g->folder[0] ? g->folder : "/", size);
	}
	if (g->plays) {
		char ago[24];
		time_ago(ago, sizeof(ago), g->last);
		size_t len = strlen(sub);
		snprintf(sub + len, sizeof(sub) - len, " \x03 Played %s", ago);
	}
	gx_text_centered(FONT_12, 320, 401, sub, rgba(C_DIM, 255));

	// Position: dots for short lists, a counter for long ones
	if (game_count <= 24) {
		int w = (game_count - 1) * 10 + 16, x = 320 - w / 2;
		for (int i = 0; i < game_count; i++) {
			if (i == selected) {
				gx_round_rect(x, 412, 16, 5, 2.5f, rgba(C_HL, 255));
				x += 21;
			} else {
				gx_round_rect(x, 412, 5, 5, 2.5f, rgba(C_TEXT, 48));
				x += 10;
			}
		}
	} else {
		char pos[32];
		snprintf(pos, sizeof(pos), "%d / %d", selected + 1, game_count);
		gx_text_centered(FONT_12, 320, 419, pos, rgba(C_DIM, 255));
	}
}

// Tab pills under the top bar, and the search query on the right
static void tab_bar(void)
{
	const int cy = 86;
	int x = SL + MARGIN;
	// Tighter pills when the row would run into the search chip (4:3)
	int names_w = 0;
	for (int i = 0; i < TAB_COUNT; i++)
		names_w += gfx_text_width(FONT_13S, tab_names[i]);
	int pad = names_w + TAB_COUNT * 30 + 60 > SR - SL - 2 * MARGIN - 110 ? 16 : 24;
	x += button_glyph(x, cy, "L") + 8;
	for (int i = 0; i < TAB_COUNT; i++) {
		int w = gfx_text_width(FONT_13S, tab_names[i]) + pad;
		if (i == tab) {
			gx_round_rect(x, cy - 13, w, 26, 13, rgba(C_HL, 255));
			gx_text(FONT_13S, x + pad / 2, gfx_center_baseline(FONT_13S, cy), tab_names[i], rgba(C_INK, 255));
		} else {
			gx_round_rect(x, cy - 13, w, 26, 13, rgba(C_TEXT, 14));
			gx_text(FONT_13S, x + pad / 2, gfx_center_baseline(FONT_13S, cy), tab_names[i], rgba(C_DIM, 255));
		}
		x += w + (pad < 24 ? 4 : 6);
	}
	button_glyph(x + 2, cy, "R");

	// Search chip
	char label[40];
	if (query[0])
		snprintf(label, sizeof(label), "\"%s\"", query);
	else
		snprintf(label, sizeof(label), "Search");
	int w = 20 + 6 + gfx_text_width(FONT_13S, label) + 14;
	int sx = SR - MARGIN - w;
	gx_round_rect(sx, cy - 13, w, 26, 13, rgba(query[0] ? C_HL : C_TEXT, query[0] ? 60 : 14));
	button_glyph(sx + 3, cy, "Z");
	gx_text(FONT_13S, sx + 29, gfx_center_baseline(FONT_13S, cy), label, rgba(query[0] ? C_TEXT : C_DIM, 255));
}

// On-screen keyboard for searching; the carousel filters as you type
static void search_panel(void)
{
	const int x = 32, y = 262, w = 576, h = 196;
	gx_round_rect(x - 1, y - 1, w + 2, h + 2, 15, rgba(C_TEXT, 28));
	gx_round_rect(x, y, w, h, 14, rgba(C_DIALOG, 245));

	// Query field with a blinking cursor
	gx_round_rect(x + 14, y + 12, w - 28, 32, 8, rgba(0, 120));
	static int blink;
	blink++;
	bool cursor = (blink / 30) % 2 == 0;
	char field[48];
	if (query[0])
		snprintf(field, sizeof(field), "%s%s", query, cursor ? "_" : "");
	else
		snprintf(field, sizeof(field), "%s", cursor ? "_" : "Type to search");
	gx_text(FONT_15S, x + 26, gfx_center_baseline(FONT_15S, y + 28), field, rgba(query[0] ? C_TEXT : C_DIM, 255));
	char count[32];
	snprintf(count, sizeof(count), "%d match%s", game_count, game_count == 1 ? "" : "es");
	gx_text(FONT_12, x + w - 26 - gfx_text_width(FONT_12, count), gfx_center_baseline(FONT_12, y + 28), count, rgba(C_DIM, 255));

	const int kw = 52, kh = 30, gap = 4, kx = x + (w - (KEY_COLS * kw + (KEY_COLS - 1) * gap)) / 2, ky = y + 54;
	for (int i = 0; i < KEY_COUNT; i++) {
		int cx = kx + (i % KEY_COLS) * (kw + gap), cy = ky + (i / KEY_COLS) * (kh + gap);
		bool on = i == key_sel;
		if (on)
			gx_round_rect(cx - 2, cy - 2, kw + 4, kh + 4, 8, rgba(C_HL, 255));
		gx_round_rect(cx, cy, kw, kh, 6, rgba(on ? C_DIALOG : C_TEXT, on ? 255 : 16));
		int font = strlen(keys[i]) > 1 ? FONT_12 : FONT_15S;
		gx_text_centered(font, cx + kw / 2, gfx_center_baseline(font, cy + kh / 2), keys[i], rgba(on ? C_HL : C_TEXT, 255));
	}
}

static void draw_ui(void)
{
	gx_2d();
	gx_blend(BLEND_ALPHA);
	top_bar();
	const int hint_y = 448;

	if (screen == SCREEN_SEARCHING) {
		gx_text_centered(FONT_20B, 320, 250, "Looking for SD card...", rgba(C_TEXT, 255));
		gx_text_centered(FONT_13M, 320, 274, "SD2SP2, SD Gecko B, SD Gecko A", rgba(C_DIM, 255));
	} else if (screen == SCREEN_NO_CARD) {
		gx_round_rect(279, 167, 82, 106, 7, rgba(C_TEXT, 64));
		gx_round_rect(280, 168, 80, 104, 6, rgba(C_BG_TOP, 255));
		gx_text_centered(FONT_20B, 320, 318, "No SD card found", rgba(C_TEXT, 255));
		gx_text_centered(FONT_13M, 320, 340, "Insert a card into SD2SP2 or an SD Gecko.", rgba(C_DIM, 255));
		int x = hint(SL + MARGIN, hint_y, "X", "Try again");
		hint(x, hint_y, "Y", "Demo mode");
	} else if (screen == SCREEN_LAUNCH) {
		char text[96];
		snprintf(text, sizeof(text), "Loading %s...", loading_name);
		gx_text_centered(FONT_13S, 320, 404, text, rgba(C_DIM, 255));
		gx_round_rect(220, 420, 200, 4, 2, rgba(C_TEXT, 32));
		if (load_percent > 0)
			gx_round_rect(220, 420, 2 * load_percent, 4, 2, rgba(C_HL, 255));
	} else {
		tab_bar();
		if (game_count > 0) {
			game_info();
		} else {
			int ny = screen == SCREEN_SEARCH ? 180 : 240; // stay above the keyboard
			gx_text_centered(FONT_20B, 320, ny, "Nothing here", rgba(C_TEXT, 255));
			const char *why = query[0] ? "No games match your search."
				: tab == TAB_RECENT ? "Games you play show up here."
				: tab == TAB_FAVS ? "Press X on a game to add it here."
				: "No games of this kind on the card.";
			gx_text_centered(FONT_13M, 320, ny + 24, why, rgba(C_DIM, 255));
		}
		if (screen != SCREEN_SEARCH) {
			bool play = game_count > 0 && (games[selected].is_disc || is_gameboy(&games[selected]));
			bool web = game_count > 0 && (games[selected].kind == KIND_WEB || games[selected].kind == KIND_APP);
			bool drive = game_count > 0 && games[selected].kind == KIND_DRIVE;
			int x = game_count > 0 ? hint(SL + MARGIN, hint_y, "A", drive ? "Options" : web ? "Open" : play ? "Play" : "Start") : SL + MARGIN;
			x += button_glyph(x, hint_y, "L") + 4;
			x = hint(x, hint_y, "R", "Tabs");
			x = hint(x, hint_y, "Z", "Search");
			if (game_count > 0)
				hint(x, hint_y, "X", games[selected].fav ? "Unstar" : "Star");
			hint(SR - MARGIN - hint_width("Y", "Settings"), hint_y, "Y", "Settings");
		}
	}

	if (screen == SCREEN_SEARCH)
		search_panel();

	if (screen == SCREEN_SETTINGS)
		settings_panel();

	if (screen == SCREEN_VIDEO_CONFIRM)
		video_confirm_panel();

	if (screen == SCREEN_DISC_MENU)
		disc_menu_panel();

	if (screen == SCREEN_RIP)
		rip_panel();

	if (screen == SCREEN_ERROR) {
		gx_texture(NULL);
		gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 170));
		gx_round_rect(119, 149, 402, 142, 15, rgba(C_TEXT, 28));
		gx_round_rect(120, 150, 400, 140, 14, rgba(C_DIALOG, 255));
		gx_text_centered(FONT_19B, 320, 196, error_title, rgba(C_TEXT, 255));
		gx_text_centered(FONT_13M, 320, 222, error_text, rgba(C_DIM, 255));
		gx_texture(NULL);
		gx_quad(121, 244, 398, 1, rgba(C_TEXT, 28));
		gx_text_centered(FONT_15S, 320, gfx_center_baseline(FONT_15S, 267), "OK", rgba(C_HL, 255));
	}
}

// ---------------------------------------------------------------- frames

static void render_page(void)
{
	gx_2d();
	gx_blend(BLEND_ALPHA);
	gx_depth(false, false);
	gx_texture(NULL);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 255));
	page_draw(page_scroll);

	// Scrollbar and a hint that fades out after a few seconds
	int h = page_height();
	if (h > 480) {
		float bar = 480.0f * 480 / h, y = (480 - bar) * page_scroll / (h - 480);
		gx_round_rect(632, 4, 4, 472, 2, rgba(0, 80));
		gx_round_rect(632, 4 + y * 472 / 480, 4, bar * 472 / 480, 2, rgba(C_HL, 220));
	}
	if (page_hint > 0) {
		int a = page_hint > 1 ? 255 : (int)(page_hint * 255);
		gx_round_rect(MARGIN - 8, 430, 330, 32, 16, rgba(0, a * 3 / 4));
		int x = hint(MARGIN, 446, "B", "Back");
		x += button_glyph(x, 446, "L") + 4;
		hint(x, 446, "R", "Page up / down");
		page_hint -= 1 / 60.0f;
	}
	gx_end_frame();
}

static void render(void)
{
	if (screen == SCREEN_PAGE) {
		render_page();
		return;
	}
	if (screen == SCREEN_MUSIC || screen == SCREEN_MOVIES) {
		gx_depth(false, false);
		gx_blend(BLEND_ALPHA);
		if (screen == SCREEN_MUSIC)
			music_draw();
		else
			movie_draw();
		gx_end_frame();
		return;
	}
	scene_step(selected, screen == SCREEN_LAUNCH && fly_case);
	covers_update(selected);

	gx_2d();
	gx_blend(BLEND_ALPHA);
	gx_depth(false, false);
	gx_texture(&background_tex);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, 0xFFFFFFFF);

	if (game_count > 0 && screen != SCREEN_NO_CARD)
		scene_draw(selected, C_HL, screen == SCREEN_HOME);

	draw_ui();
	if (splash_alpha > 0) {
		gx_2d();
		gx_blend(BLEND_ALPHA);
		gx_texture(NULL); // black bars beside the 4:3 splash in 16:9
		gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, splash_alpha));
		gx_texture(&splash_tex);
		gx_quad(0, 0, 640, 480, rgba(0xFFFFFF, splash_alpha));
	}
	gx_end_frame();
}

// Boot splash on black, with an optional status line under it
static void splash_frame(int alpha, const char *status)
{
	gx_2d();
	gx_blend(BLEND_ALPHA);
	gx_depth(false, false);
	gx_texture(NULL);
	gx_quad(gx_left, 0, gx_right - gx_left, 480, rgba(0, 255));
	gx_texture(&splash_tex);
	gx_quad(0, 0, 640, 480, rgba(0xFFFFFF, alpha));
	if (status)
		gx_text_centered(FONT_12, 320, 462, status, rgba(C_DIM, alpha * 3 / 4));
	gx_end_frame();
}

static void show_error(const char *title, const char *text)
{
	snprintf(error_title, sizeof(error_title), "%s", title);
	snprintf(error_text, sizeof(error_text), "%s", text);
	screen = SCREEN_ERROR;
}

// ---------------------------------------------------------------- launching

static void loading_progress(int percent)
{
	load_percent = percent;
	render();
}

static void before_jump(void)
{
	sound_stop();
	disc_stop();
	unmount();
	GX_DrawDone();
	GX_AbortFrame();
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

static void open_page(const Game *g)
{
	bool ok;
#ifdef START_DEMO
	extern const u8 demo_page_fpg[], demo_page_fpg_end[];
	ok = demo ? page_open(NULL, demo_page_fpg, demo_page_fpg_end - demo_page_fpg) : page_open(g->path, NULL, 0);
#else
	ok = page_open(g->path, NULL, 0);
#endif
	if (!ok) {
		show_error("Couldn't open the page", "The page file is missing or damaged.");
		return;
	}
	page_scroll = 0;
	page_hint = 4;
	screen = SCREEN_PAGE;
}

static void open_disc_menu(void);

// Starts a game through Swiss, told to autoload `autoload` ("Autoload=...").
// Returns NULL if Swiss is missing (an error is shown), else why it failed.
static const char *start_with_swiss(const char *autoload)
{
	const char *swiss = library_swiss_path();
	if (!swiss) {
		screen = SCREEN_HOME;
		show_error("Swiss not found", "Put a swiss .dol on the SD card to play games.");
		return NULL;
	}
	// CUBEBOOT=1 is the mode Swiss uses when another launcher starts it:
	// no Swiss menus or info screen, straight into the game.
	// Games follow the launcher's video: 480p when it's progressive, and
	// Swiss's widescreen hack when it's switched on in Settings.
	bool prog = gx_progressive();
	const char *args[] = {
		autoload, "AutoBoot=Yes",
		prog ? "Force NTSC Video Mode=480p" : "Force NTSC Video Mode=480i",
		prog ? "Force PAL Video Mode=576p" : "Force PAL Video Mode=576i",
		game_wide ? "Force Widescreen=3D" : "Force Widescreen=No",
		NULL,
	};
	const char *env[] = { "CUBEBOOT=1", NULL };
	return boot_dol(swiss, args, env, loading_progress, before_jump);
}

static void launch(void)
{
	const Game *g = &games[selected];
	if (g->kind == KIND_APP) {
		library_played(g->path, (u32)time(NULL));
		covers_reset(demo); // room for the player
		if (!strcmp(g->path, "fml:music")) {
			music_open();
			screen = SCREEN_MUSIC;
		} else {
			movie_open();
			screen = SCREEN_MOVIES;
		}
		return;
	}
	if (g->kind == KIND_DRIVE) {
		if (disc.state == DISC_GAME)
			open_disc_menu();
		else if (disc.state == DISC_WII)
			show_error("This is a Wii disc", "Wii games can't run on a GameCube.");
		else if (disc.state == DISC_UNKNOWN)
			show_error("This disc can't be read", "The drive only reads original GameCube discs.");
		return;
	}
	if (g->kind == KIND_WEB) {
		library_played(g->path, (u32)time(NULL));
		open_page(g);
		return;
	}
	library_played(g->path, (u32)time(NULL));
	screen = SCREEN_LAUNCH;
	loading_name = g->title;
	fly_case = true;
	load_percent = 0;
	for (int i = 0; i < 25; i++) // let the case fly forward
		render();

	if (demo) {
		for (int p = 0; p <= 100; p += 2)
			loading_progress(p);
		for (int i = 0; i < 20; i++)
			render();
		screen = SCREEN_HOME;
		return;
	}

	const char *err;
	if (g->is_disc) {
		char autoload[300];
		snprintf(autoload, sizeof(autoload), "Autoload=%s%s", devices[device].swiss_prefix, g->path + 4);
		err = start_with_swiss(autoload);
		if (!err)
			return;
	} else if (is_gameboy(g)) {
		// Game Boy ROMs are started by Visual Boy Advance GX: argv[1] is the
		// folder in its own device naming, argv[2] the file name.
		const char *vba = library_vba_path();
		if (!vba) {
			screen = SCREEN_HOME;
			show_error("Visual Boy Advance GX not found", "Put vbagx.dol on the SD card to play Game Boy games.");
			return;
		}
		char folder[300];
		const char *name = strrchr(g->path, '/') + 1;
		snprintf(folder, sizeof(folder), "%s%.*s", devices[device].vba_prefix, (int)(name - g->path - 4), g->path + 4);
		const char *args[] = { folder, name, NULL };
		err = boot_dol(vba, args, NULL, loading_progress, before_jump);
	} else {
		err = boot_dol(g->path, NULL, NULL, loading_progress, before_jump);
	}
	screen = SCREEN_HOME;
	show_error("Couldn't start the software", err);
}

static void chain_progress(int percent)
{
	(void)percent;
}

static void before_chain(void)
{
	// Keep the splash on screen while the other copy starts
	sound_stop();
	unmount();
	GX_DrawDone();
	GX_AbortFrame();
}

// True if the file contains this exact build (then there's nothing newer to run)
static bool same_build(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return true;
	static char buf[16384 + sizeof(build_id)];
	size_t keep = 0, n, idlen = strlen(build_id);
	bool found = false;
	while (!found && (n = fread(buf + keep, 1, 16384, f)) > 0) {
		size_t total = keep + n;
		for (size_t i = 0; i + idlen <= total; i++)
			if (buf[i] == 'F' && !memcmp(buf + i, build_id, idlen)) {
				found = true;
				break;
			}
		keep = total < idlen ? total : idlen - 1;
		memmove(buf, buf + total - keep, keep);
	}
	fclose(f);
	return found;
}

// Runs sd:/fml/launcher.dol if it's a different build. Only returns if it isn't.
static void run_sd_launcher(void)
{
	struct stat st;
	if (stat(SD_LAUNCHER, &st) != 0 || same_build(SD_LAUNCHER))
		return;
	const char *args[] = { CHAIN_ARG, NULL };
	boot_dol(SD_LAUNCHER, args, NULL, chain_progress, before_chain);
	// Couldn't start it: carry on with this copy
	mount_from(0);
}

// Starts Swiss with no arguments, so its normal menu opens.
static void open_swiss(void)
{
	const char *swiss = library_swiss_path();
	if (!swiss)
		return;
	screen = SCREEN_LAUNCH;
	loading_name = "Swiss";
	fly_case = false;
	load_percent = 0;
	if (demo) {
		for (int p = 0; p <= 100; p += 2)
			loading_progress(p);
		screen = SCREEN_HOME;
		show_error("Demo mode", "Swiss would open here on a real SD card.");
		return;
	}
	const char *err = boot_dol(swiss, NULL, NULL, loading_progress, before_jump);
	screen = SCREEN_HOME;
	show_error("Couldn't start Swiss", err);
}

// ---------------------------------------------------------------- the disc drive

// Shows the disc in the drive (or takes it off the shelf) when it changes
static void update_drive(void)
{
	static u32 seen;
	DiscInfo d;
	u32 changed = disc_get(&d);
	if (changed == seen)
		return;
	seen = changed;
	bool was = disc.state != DISC_NONE;
	disc = d;
	if (d.state == DISC_NONE) {
		library_set_drive(NULL);
	} else {
		Game g;
		memset(&g, 0, sizeof(g));
		strcpy(g.path, "dvd:");
		g.kind = KIND_DRIVE;
		if (d.state == DISC_GAME) {
			snprintf(g.title, sizeof(g.title), "%s", d.title);
			strcpy(g.id, d.id);
			strcpy(g.key, d.id);
			snprintf(g.region, sizeof(g.region), "%s", d.region);
			g.disc_no = d.disc_no;
			g.size = (u32)DISC_SIZE;
		} else {
			strcpy(g.title, d.state == DISC_READING ? "Reading disc..." : d.state == DISC_WII ? "Wii disc" : "Unreadable disc");
		}
		library_set_drive(&g);
	}
	if (screen == SCREEN_DISC_MENU && d.state != DISC_GAME)
		screen = SCREEN_HOME;
	if (screen == SCREEN_NO_CARD || screen == SCREEN_SEARCHING || screen == SCREEN_LAUNCH)
		return;
	apply_filter();
	// A disc that was just put in slides into view
	if (!was && d.state != DISC_NONE && screen == SCREEN_HOME && game_count > 0 && games[0].kind == KIND_DRIVE)
		selected = 0;
}

static void open_disc_menu(void)
{
	disc_option_count = 0;
	disc_options[disc_option_count++] = DISC_PLAY;
	disc_options[disc_option_count++] = library_find_disc(disc.id, disc.disc_no) ? DISC_PLAY_SD : DISC_COPY;
	disc_options[disc_option_count++] = DISC_CANCEL;
	disc_option = 0;
	screen = SCREEN_DISC_MENU;
}

// "Super Smash Bros. Melee [GALE01].iso", without characters FAT can't store
static void rip_file_name(char *out, size_t n)
{
	char safe[64];
	int o = 0;
	for (const char *p = disc.title; *p && o < 56; p++)
		safe[o++] = strchr("\\/:*?\"<>|", *p) ? ' ' : *p;
	while (o > 0 && (safe[o - 1] == ' ' || safe[o - 1] == '.'))
		o--;
	safe[o] = 0;
	if (disc.disc_no)
		snprintf(out, n, "sd:/games/%s [%s] (Disc %d).iso", safe[0] ? safe : disc.id, disc.id, disc.disc_no + 1);
	else
		snprintf(out, n, "sd:/games/%s [%s].iso", safe[0] ? safe : disc.id, disc.id);
}

static void start_copy(void)
{
	if (demo || device < 0) {
		show_error("No SD card", "Copying needs an SD card (this is the demo card).");
		return;
	}
	rip_file_name(rip_target, sizeof(rip_target));
	snprintf(rip_title, sizeof(rip_title), "%s", disc.title);
	struct stat st;
	if (stat(rip_target, &st) == 0) {
		show_error("Already on the card", "A file with this game's name is in games/.");
		return;
	}
	struct statvfs fs;
	if (statvfs("sd:/", &fs) == 0 && (u64)fs.f_bfree * fs.f_bsize < DISC_SIZE + 4 * 1024 * 1024) {
		show_error("Not enough space", "Copying a disc needs 1.4 GB free on the SD card.");
		return;
	}
	mkdir("sd:/games", 0777);
	if (!disc_rip_start(rip_target)) {
		show_error("Couldn't start copying", "Take the disc out and put it back in.");
		return;
	}
	music_stop(); // the card is busy enough
	rip_stop_frames = 0;
	screen = SCREEN_RIP;
}

static void play_drive_disc(void)
{
	if (demo) {
		show_error("Demo mode", "Swiss would start the disc here on a real SD card.");
		return;
	}
	screen = SCREEN_LAUNCH;
	loading_name = disc.title;
	fly_case = true;
	load_percent = 0;
	for (int i = 0; i < 25; i++)
		render();
	// Swiss lists the disc as "dvd:/<name> [GAMEID].gcm"
	char autoload[64];
	snprintf(autoload, sizeof(autoload), "Autoload=dvd:/*?%s?.gcm", disc.id);
	const char *err = start_with_swiss(autoload);
	if (err) {
		screen = SCREEN_HOME;
		show_error("Couldn't start Swiss", err);
	}
}

// Reads the card again after a copy, keeping the tab and search
static void refresh_library(void)
{
	library_scan();
	apply_filter();
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
	// Started by the launcher in the Pico (the copy on the card is running)
	bool chained = argc > 1 && argv[1] && !strcmp(argv[1], CHAIN_ARG);

	gx_init(VMODE_AUTO);
	PAD_Init();
	gx_ui_init();
	scene_init();
	make_background();
	make_logo();
	make_gear();
	make_star();

	// Debug: hold Z while starting to open the demo card even with an SD card in
	PAD_ScanPads();
	bool force_demo = PAD_ButtonsHeld(0) & PAD_TRIGGER_Z;
	// Hold B while starting for 480i, in case the TV shows nothing in 480p
	force_480i = PAD_ButtonsHeld(0) & PAD_BUTTON_B;
	if (force_480i)
		gx_set_video(VMODE_480I);

	// Splash: fade in, look for the card while it's showing, then fade into the carousel
	tex_from_tiles(&splash_tex, splash_tiles, splash_w, splash_h, GX_TF_RGB565);
	for (int i = chained ? 30 : 1; i <= 30; i++)
		splash_frame(i * 255 / 30, NULL);
	splash_frame(255, "Looking for SD card...");
	splash_frame(255, "Looking for SD card...");
	mount_from(0);
	PAD_ScanPads();
	if (!chained && device >= 0 && !(PAD_ButtonsHeld(0) & PAD_TRIGGER_R))
		run_sd_launcher();
#ifdef START_DEMO
	force_demo |= device < 0; // `make DEMO=1`: straight to the demo card in Dolphin
#endif
	demo = force_demo;
	disc_init();
	PAD_ScanPads();
	force_480i |= (PAD_ButtonsHeld(0) & PAD_BUTTON_B) != 0;
	load_settings();

	// Boot video from the card (it starts and ends on the splash image)
	bool played = false;
	if (device >= 0)
		played = video_play("sd:/fml/splash.fmv", NULL, 0);
#ifdef START_DEMO
	if (!played) {
		extern const u8 splash_fmv[], splash_fmv_end[];
		played = video_play(NULL, splash_fmv, splash_fmv_end - splash_fmv);
	}
#endif

	load_library();
	for (int i = 0; i < (played ? 20 : 60); i++) // hold the splash a moment
		splash_frame(255, NULL);
	for (int i = 24; i >= 0; i--) {
		splash_alpha = i * 255 / 24;
		render();
	}
	tex_free(&splash_tex);

	int repeat = 0;
	for (;;) {
		update_drive();
		music_tick();
		render();

		PAD_ScanPads();
		u32 down = PAD_ButtonsDown(0), held = PAD_ButtonsHeld(0);
		s8 stick = PAD_StickX(0);

		if (screen == SCREEN_MUSIC) {
			if (!music_input(down, held)) {
				xp_free();
				screen = SCREEN_HOME;
			}
			continue;
		}

		if (screen == SCREEN_MOVIES) {
			if (!movie_input(down, held)) {
				movie_close();
				xp_free();
				screen = SCREEN_HOME;
			}
			continue;
		}

		if (screen == SCREEN_DISC_MENU) {
			s8 sy = PAD_StickY(0);
			static bool moved;
			int move = 0;
			if (down & PAD_BUTTON_UP) move = -1;
			if (down & PAD_BUTTON_DOWN) move = 1;
			if (!moved && (sy > 60 || sy < -60))
				move = sy > 0 ? -1 : 1;
			moved = sy > 60 || sy < -60;
			disc_option = (disc_option + move + disc_option_count) % disc_option_count;
			if (down & PAD_BUTTON_B) {
				screen = SCREEN_HOME;
			} else if (down & PAD_BUTTON_A) {
				int o = disc_options[disc_option];
				screen = SCREEN_HOME;
				if (o == DISC_PLAY) {
					play_drive_disc();
				} else if (o == DISC_COPY) {
					start_copy();
				} else if (o == DISC_PLAY_SD) {
					const Game *copy = library_find_disc(disc.id, disc.disc_no);
					for (int i = 0; copy && i < game_count; i++)
						if (!strcmp(games[i].path, copy->path)) {
							selected = i;
							launch();
							break;
						}
				}
			}
			continue;
		}

		if (screen == SCREEN_RIP) {
			RipStatus st;
			disc_rip_status(&st);
			if (rip_stop_frames > 0)
				rip_stop_frames--;
			if (st.state == RIP_RUNNING) {
				if (down & PAD_BUTTON_B) {
					if (rip_stop_frames > 0)
						disc_rip_cancel();
					else
						rip_stop_frames = 180;
				}
			} else if (st.state == RIP_DONE) {
				if (down & (PAD_BUTTON_A | PAD_BUTTON_B)) {
					disc_rip_clear();
					screen = SCREEN_HOME;
					refresh_library();
					if (down & PAD_BUTTON_A) {
						const Game *copy = library_find_disc(disc.id, disc.disc_no);
						for (int i = 0; copy && i < game_count; i++)
							if (!strcmp(games[i].path, copy->path)) {
								selected = i;
								scene_snap(i);
								launch();
								break;
							}
					}
				}
			} else if (down & (PAD_BUTTON_A | PAD_BUTTON_B)) {
				disc_rip_clear();
				screen = SCREEN_HOME;
			}
			continue;
		}

		if (screen == SCREEN_ERROR) {
			if (down & (PAD_BUTTON_A | PAD_BUTTON_B))
				screen = library_total() > 0 ? SCREEN_HOME : SCREEN_NO_CARD;
			continue;
		}

		if (screen == SCREEN_PAGE) {
			s8 sy = PAD_StickY(0), cy = PAD_SubStickY(0);
			float max = page_height() - 480;
			if (sy > 20 || sy < -20) page_scroll -= sy * 0.25f;
			if (cy > 20 || cy < -20) page_scroll -= cy * 0.6f;
			if (held & PAD_BUTTON_UP) page_scroll -= 14;
			if (held & PAD_BUTTON_DOWN) page_scroll += 14;
			if (down & PAD_TRIGGER_L) page_scroll -= 440;
			if (down & PAD_TRIGGER_R) page_scroll += 440;
			if (page_scroll > max) page_scroll = max;
			if (page_scroll < 0) page_scroll = 0;
			if (down & (PAD_BUTTON_B | PAD_BUTTON_START)) {
				page_close();
				screen = SCREEN_HOME;
			}
			continue;
		}

		if (screen == SCREEN_SEARCH) {
			s8 sy = PAD_StickY(0);
			static bool moved;
			int dx = 0, dy = 0;
			if (down & PAD_BUTTON_LEFT) dx = -1;
			if (down & PAD_BUTTON_RIGHT) dx = 1;
			if (down & PAD_BUTTON_UP) dy = -1;
			if (down & PAD_BUTTON_DOWN) dy = 1;
			bool tilted = stick < -60 || stick > 60 || sy < -60 || sy > 60;
			if (tilted && !moved) {
				if (stick < -60) dx = -1;
				if (stick > 60) dx = 1;
				if (sy > 60) dy = -1;
				if (sy < -60) dy = 1;
			}
			moved = tilted;
			int col = key_sel % KEY_COLS, row = key_sel / KEY_COLS, rows = KEY_COUNT / KEY_COLS;
			col = (col + dx + KEY_COLS) % KEY_COLS;
			row = (row + dy + rows) % rows;
			key_sel = row * KEY_COLS + col;

			size_t len = strlen(query);
			bool changed = false;
			if (down & PAD_BUTTON_A) {
				const char *k = keys[key_sel];
				if (!strcmp(k, "OK")) {
					screen = SCREEN_HOME;
				} else if (!strcmp(k, "Del")) {
					if (len) {
						query[len - 1] = 0;
						changed = true;
					}
				} else if (!strcmp(k, "Clear")) {
					query[0] = 0;
					changed = true;
				} else if (len + 1 < sizeof(query)) {
					query[len] = !strcmp(k, "Space") ? ' ' : k[0];
					query[len + 1] = 0;
					changed = true;
				}
			}
			if (down & PAD_BUTTON_B) {
				if (len) {
					query[len - 1] = 0;
					changed = true;
				} else {
					screen = SCREEN_HOME;
				}
			}
			if (down & (PAD_BUTTON_START | PAD_TRIGGER_Z))
				screen = SCREEN_HOME;
			if (changed)
				apply_filter();
			continue;
		}

		if (screen == SCREEN_VIDEO_CONFIRM) {
			if (down & PAD_BUTTON_A) {
				save_settings();
				screen = SCREEN_SETTINGS;
			} else if ((down & PAD_BUTTON_B) || --confirm_frames <= 0) {
				video_setting = video_undo;
				gx_set_video(video_setting);
				screen = SCREEN_SETTINGS;
			}
			continue;
		}

		if (screen == SCREEN_SETTINGS) {
			s8 sy = PAD_StickY(0);
			static bool stick_moved;
			int move = 0;
			if (down & PAD_BUTTON_UP) move = -1;
			if (down & PAD_BUTTON_DOWN) move = 1;
			if (!stick_moved && (sy > 60 || sy < -60))
				move = sy > 0 ? -1 : 1;
			stick_moved = sy > 60 || sy < -60;
			setting = (setting + move + SETTING_COUNT) % SETTING_COUNT;
			int side = down & PAD_BUTTON_LEFT ? -1 : down & PAD_BUTTON_RIGHT ? 1 : 0;

			if (down & (PAD_BUTTON_B | PAD_BUTTON_Y)) {
				screen = SCREEN_HOME;
			} else if (setting < SETTING_SWISS && (side || (down & PAD_BUTTON_A))) {
				change_setting(side ? side : 1);
			} else if (down & PAD_BUTTON_A) {
				if (setting == SETTING_SWISS) {
					open_swiss();
				} else if (setting == SETTING_DEVICE) {
					demo = false;
					search_card(device < 0 ? 0 : device + 1);
					load_library();
				} else {
					sound_stop();
					disc_stop();
					unmount();
					SYS_ResetSystem(SYS_HOTRESET, 0, 0);
				}
			}
			continue;
		}

		if (screen == SCREEN_NO_CARD) {
			if (down & PAD_BUTTON_Y) {
				demo = true;
				load_library();
			}
			if (down & PAD_BUTTON_X) {
				search_card(0);
				load_library();
			}
			continue;
		}

		// Home: slide left/right, repeating while held
		int dir = 0;
		if ((held & PAD_BUTTON_LEFT) || stick < -60) dir = -1;
		if ((held & PAD_BUTTON_RIGHT) || stick > 60) dir = 1;
		if (dir) {
			if (repeat == 0 || (repeat > 16 && repeat % 6 == 0)) {
				int next = selected + dir;
				if (next >= 0 && next < game_count)
					selected = next;
			}
			repeat++;
		} else {
			repeat = 0;
		}
		if (down & (PAD_TRIGGER_L | PAD_TRIGGER_R)) {
			tab = (tab + (down & PAD_TRIGGER_R ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
			apply_filter();
		}
		if (down & PAD_TRIGGER_Z) {
			key_sel = 0;
			screen = SCREEN_SEARCH;
		}

		if ((down & PAD_BUTTON_A) && game_count > 0)
			launch();
		if (down & PAD_BUTTON_Y) {
			setting = SETTING_VIDEO;
			screen = SCREEN_SETTINGS;
		}
		if ((down & PAD_BUTTON_X) && game_count > 0) {
			library_toggle_fav(games[selected].path);
			if (tab == TAB_FAVS) // unstarred: it leaves this tab
				apply_filter();
		}
		if (down & PAD_BUTTON_START) {
			sound_stop();
			disc_stop();
			unmount();
			SYS_ResetSystem(SYS_HOTRESET, 0, 0);
		}
	}
	return 0;
}
