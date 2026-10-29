#define _GNU_SOURCE // strcasestr
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "library.h"

Game games[MAX_GAMES];
int game_count;
const char *const tab_names[TAB_COUNT] = { "All", "Recent", "Favourites", "GameCube", "Game Boy", "Apps" };

// Everything found on the card; games[] is the filtered view of this
static Game all_games[MAX_GAMES];
static int all_count;
static char swiss_path[256], vba_path[256];
static bool demo_card; // history stays in memory

#define CACHE_PATH "sd:/fml/library.cache"
#define CACHE_MAGIC 0x464D4C32 // "FML2" (FML1 had no disc regions)
#define HISTORY_PATH "sd:/fml/history.txt"

// Last scan's results, so known files aren't opened again
static Game *cache;
static int cache_count, cache_hits;
static bool cache_dirty;

static bool has_ext(const char *name, const char *ext)
{
	size_t len = strlen(name), elen = strlen(ext);
	return len > elen && strcasecmp(name + len - elen, ext) == 0;
}

// "Snes9x_GX-v4.dol" -> "Snes9x GX v4"
static void title_from_file(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s", name);
	char *dot = strrchr(out, '.');
	if (dot)
		*dot = 0;
	for (char *p = out; *p; p++)
		if (*p == '_' || *p == '-')
			*p = ' ';
}

// "Pokemon - Emerald Version (USA, Europe).gba" -> "Pokemon - Emerald Version"
static void title_from_rom(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s", name);
	char *cut = strrchr(out, '.');
	if (cut)
		*cut = 0;
	cut = strstr(out, " (");
	if (cut && cut != out)
		*cut = 0;
	cut = strstr(out, " [");
	if (cut && cut != out)
		*cut = 0;
}

static void base_name(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s", name);
	char *dot = strrchr(out, '.');
	if (dot)
		*dot = 0;
}

// Region of a disc: the 4th letter of its game ID
static void disc_region(Game *g)
{
	switch (g->id[3]) {
	case 'J': strcpy(g->region, "Japan"); break;
	case 'E': strcpy(g->region, "USA"); break;
	case 'P': strcpy(g->region, "Europe"); break;
	case 'D': strcpy(g->region, "Germany"); break;
	case 'F': strcpy(g->region, "France"); break;
	case 'S': strcpy(g->region, "Spain"); break;
	case 'I': strcpy(g->region, "Italy"); break;
	case 'U': strcpy(g->region, "Australia"); break;
	case 'K': strcpy(g->region, "Korea"); break;
	}
}

// Reads the game ID and internal title from a disc image header.
static bool read_disc(Game *g)
{
	FILE *f = fopen(g->path, "rb");
	if (!f)
		return false;
	u8 head[0x60];
	bool ok = fread(head, 1, sizeof(head), f) == sizeof(head);
	fclose(f);
	if (!ok || head[0x1C] != 0xC2 || head[0x1D] != 0x33 || head[0x1E] != 0x9F || head[0x1F] != 0x3D)
		return false;
	for (int i = 0; i < 6; i++)
		if (!isalnum(head[i]))
			return false;
	memcpy(g->id, head, 6);
	g->id[6] = 0;
	strcpy(g->key, g->id);

	// Use the disc's own title when it is plain ASCII (Japanese discs often aren't)
	const char *t = (const char *)&head[0x20];
	bool ascii = t[0] != 0;
	for (int i = 0; i < 0x40 && t[i]; i++)
		if ((u8)t[i] < 0x20 || (u8)t[i] > 0x7E)
			ascii = false;
	if (ascii)
		snprintf(g->title, sizeof(g->title), "%.63s", t);
	return true;
}

static bool is_swiss(const char *name)
{
	return strncasecmp(name, "swiss", 5) == 0;
}

static bool is_vba(const char *name)
{
	return strcasestr(name, "vbagx") || strcasestr(name, "visualboyadvance");
}

static int kind_of(const char *name)
{
	if (has_ext(name, ".dol")) return KIND_DOL;
	if (has_ext(name, ".iso") || has_ext(name, ".gcm")) return KIND_DISC;
	if (has_ext(name, ".gba")) return KIND_GBA;
	if (has_ext(name, ".gbc")) return KIND_GBC;
	if (has_ext(name, ".gb")) return KIND_GB;
	if (has_ext(name, ".fpg")) return KIND_WEB;
	return -1;
}

// Region of a Game Boy ROM: the No-Intro tag in the file name if there is one
// ("(Japan)", "(USA, Europe)"), else the ROM header.
static void rom_region(Game *g, const char *name)
{
	const char *open = strstr(name, " (");
	if (open) {
		const char *close = strchr(open, ')');
		static const char *const known[] = { "Japan", "USA", "Europe", "World", "Korea", "Germany", "France", "Spain", "Italy", "Australia" };
		for (size_t i = 0; close && i < sizeof(known) / sizeof(known[0]); i++) {
			const char *hit = strstr(open, known[i]);
			if (hit && hit < close) {
				snprintf(g->region, sizeof(g->region), "%.*s", (int)(close - open - 2), open + 2);
				return;
			}
		}
	}

	FILE *f = fopen(g->path, "rb");
	if (!f)
		return;
	u8 head[0x150];
	bool ok = fread(head, 1, sizeof(head), f) == sizeof(head);
	fclose(f);
	if (!ok)
		return;
	if (g->kind == KIND_GBA) {
		// Game code at 0xAC; its last letter is the region
		switch (head[0xAF]) {
		case 'J': strcpy(g->region, "Japan"); break;
		case 'E': strcpy(g->region, "USA"); break;
		case 'P': strcpy(g->region, "Europe"); break;
		case 'D': strcpy(g->region, "Germany"); break;
		case 'F': strcpy(g->region, "France"); break;
		case 'S': strcpy(g->region, "Spain"); break;
		case 'I': strcpy(g->region, "Italy"); break;
		case 'K': strcpy(g->region, "Korea"); break;
		}
	} else {
		// Destination code: 0 = Japan, 1 = everywhere else
		strcpy(g->region, head[0x14A] == 0 ? "Japan" : "International");
	}
}

static const Game *cache_find(const char *path, int kind)
{
	for (int i = 0; i < cache_count; i++)
		if (cache[i].kind == kind && !strcmp(cache[i].path, path))
			return &cache[i];
	return NULL;
}

// Adds one file to all_games. `id` is only used by the demo card. A file in
// the cache is taken from there; `size` < 0 means look it up only when needed.
static void add_entry(const char *path, int kind, s64 size, const char *id, bool root)
{
	const char *name = strrchr(path, '/') + 1;
	if (kind == KIND_DOL && is_swiss(name)) {
		if (!swiss_path[0] || root) // prefer one in the card's root
			snprintf(swiss_path, sizeof(swiss_path), "%s", path);
		return;
	}
	if (kind == KIND_DOL && is_vba(name) && !vba_path[0])
		snprintf(vba_path, sizeof(vba_path), "%s", path);
	if (all_count >= MAX_GAMES || strlen(path) >= sizeof(games[0].path))
		return;

	const Game *known = id ? NULL : cache_find(path, kind);
	if (known) {
		all_games[all_count++] = *known;
		cache_hits++;
		return;
	}
	if (size < 0) {
		struct stat st;
		if (stat(path, &st) != 0)
			return;
		size = st.st_size;
	}
	cache_dirty = true;

	Game *g = &all_games[all_count];
	memset(g, 0, sizeof(*g));
	strcpy(g->path, path);
	snprintf(g->folder, sizeof(g->folder), "%.*s", (int)(name - path - 4), path + 4); // drop "sd:/"
	g->size = size;
	g->kind = kind;
	g->is_disc = kind == KIND_DISC;
	if (kind == KIND_DISC) {
		title_from_file(g->title, sizeof(g->title), name);
		if (id) {
			strcpy(g->id, id);
			strcpy(g->key, id);
			snprintf(g->title, sizeof(g->title), "%.*s", (int)(strrchr(name, '.') - name), name);
		} else if (!read_disc(g)) {
			return;
		}
		disc_region(g);
	} else if (kind == KIND_DOL || kind == KIND_WEB) {
		if (kind == KIND_WEB)
			base_name(g->title, sizeof(g->title), name);
		else
			title_from_file(g->title, sizeof(g->title), name);
		base_name(g->key, sizeof(g->key), name);
	} else {
		title_from_rom(g->title, sizeof(g->title), name);
		base_name(g->key, sizeof(g->key), name);
		rom_region(g, name);
	}
	all_count++;
}

static void scan_dir(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *de;
	char path[512];
	while ((de = readdir(d)) && all_count < MAX_GAMES) {
		const char *name = de->d_name;
		if (name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s%s", dir, name);
		// The directory entry says whether it's a folder, so files aren't
		// looked up one by one (slow on FAT with many files)
		bool is_dir;
		if (de->d_type == DT_DIR || de->d_type == DT_REG) {
			is_dir = de->d_type == DT_DIR;
		} else {
			struct stat st;
			if (stat(path, &st) != 0)
				continue;
			is_dir = S_ISDIR(st.st_mode);
		}
		if (is_dir) {
			// Folders that never hold games
			if (depth < 4 && strcasecmp(name, "covers") != 0 && strcasecmp(name, "fml") != 0 &&
			    strcasecmp(name, "System Volume Information") != 0) {
				strcat(path, "/");
				scan_dir(path, depth + 1);
			}
			continue;
		}
		int kind = kind_of(name);
		if (kind >= 0)
			add_entry(path, kind, -1, NULL, depth == 0);
	}
	closedir(d);
}

// ---------------------------------------------------------------- cache

static void load_cache(void)
{
	free(cache);
	cache = NULL;
	cache_count = 0;
	FILE *f = fopen(CACHE_PATH, "rb");
	if (!f)
		return;
	u32 head[3]; // magic, sizeof(Game), count
	if (fread(head, 1, sizeof(head), f) == sizeof(head) && head[0] == CACHE_MAGIC && head[1] == sizeof(Game) &&
	    head[2] <= MAX_GAMES && (cache = malloc(head[2] * sizeof(Game) + 1))) {
		if (fread(cache, sizeof(Game), head[2], f) == head[2])
			cache_count = head[2];
	}
	fclose(f);
}

static void save_cache(void)
{
	mkdir("sd:/fml", 0777);
	FILE *f = fopen(CACHE_PATH, "wb");
	if (!f)
		return;
	u32 head[3] = { CACHE_MAGIC, sizeof(Game), all_count };
	fwrite(head, 1, sizeof(head), f);
	fwrite(all_games, sizeof(Game), all_count, f);
	fclose(f);
}

// ---------------------------------------------------------------- history

static Game *find_game(const char *path)
{
	for (int i = 0; i < all_count; i++)
		if (!strcmp(all_games[i].path, path))
			return &all_games[i];
	return NULL;
}

// One line per game: plays, last played, favourite, path
static void load_history(void)
{
	for (int i = 0; i < all_count; i++) {
		all_games[i].plays = 0;
		all_games[i].last = 0;
		all_games[i].fav = false;
	}
	FILE *f = fopen(HISTORY_PATH, "r");
	if (!f)
		return;
	char line[320];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		unsigned plays, fav;
		unsigned long last;
		int n = 0;
		if (sscanf(line, "%u\t%lu\t%u\t%n", &plays, &last, &fav, &n) != 3 || !n)
			continue;
		Game *g = find_game(line + n);
		if (g) {
			g->plays = plays;
			g->last = last;
			g->fav = fav != 0;
		}
	}
	fclose(f);
}

static void save_history(void)
{
	if (demo_card)
		return;
	mkdir("sd:/fml", 0777);
	FILE *f = fopen(HISTORY_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < all_count; i++) {
		const Game *g = &all_games[i];
		if (g->plays || g->fav)
			fprintf(f, "%u\t%lu\t%u\t%s\n", g->plays, (unsigned long)g->last, g->fav, g->path);
	}
	fclose(f);
}

// games[] holds copies: keep them in step with all_games
static void sync_shown(const Game *g)
{
	for (int i = 0; i < game_count; i++)
		if (!strcmp(games[i].path, g->path)) {
			games[i].plays = g->plays;
			games[i].last = g->last;
			games[i].fav = g->fav;
		}
}

void library_played(const char *path, u32 now)
{
	Game *g = find_game(path);
	if (!g)
		return;
	if (g->plays < 65535)
		g->plays++;
	g->last = now ? now : 1;
	sync_shown(g);
	save_history();
}

bool library_toggle_fav(const char *path)
{
	Game *g = find_game(path);
	if (!g)
		return false;
	g->fav = !g->fav;
	sync_shown(g);
	save_history();
	return g->fav;
}

const Game *library_last_played(void)
{
	const Game *best = NULL;
	for (int i = 0; i < all_count; i++)
		if (all_games[i].last && (!best || all_games[i].last > best->last))
			best = &all_games[i];
	return best;
}

static int compare_games(const void *a, const void *b)
{
	return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

static void finish_scan(void)
{
	qsort(all_games, all_count, sizeof(Game), compare_games);
	library_filter(TAB_ALL, "");
}

void library_scan(void)
{
	all_count = 0;
	swiss_path[0] = vba_path[0] = 0;
	demo_card = false;
	load_cache();
	cache_hits = 0;
	cache_dirty = false;
	scan_dir("sd:/", 0);
	// Rewrite the cache when anything was added or removed
	if (cache_dirty || cache_hits != cache_count)
		save_cache();
	free(cache);
	cache = NULL;
	cache_count = 0;
	load_history();
	finish_scan();
}

static int compare_recent(const void *a, const void *b)
{
	u32 la = ((const Game *)a)->last, lb = ((const Game *)b)->last;
	return la < lb ? 1 : la > lb ? -1 : 0;
}

static bool in_tab(const Game *g, int tab)
{
	switch (tab) {
	case TAB_RECENT: return g->last != 0;
	case TAB_FAVS: return g->fav;
	case TAB_GAMECUBE: return g->kind == KIND_DISC;
	case TAB_GAMEBOY: return is_gameboy(g);
	case TAB_APPS: return g->kind == KIND_DOL || g->kind == KIND_WEB;
	default: return true;
	}
}

void library_filter(int tab, const char *query)
{
	game_count = 0;
	for (int i = 0; i < all_count; i++) {
		const Game *g = &all_games[i];
		if (in_tab(g, tab) && (!query[0] || strcasestr(g->title, query) || (g->region[0] && strcasestr(g->region, query))))
			games[game_count++] = *g;
	}
	if (tab == TAB_RECENT) // newest first
		qsort(games, game_count, sizeof(Game), compare_recent);
}

int library_total(void)
{
	return all_count;
}

// ---------------------------------------------------------------- demo card

typedef struct {
	const char *path;
	const char *id;
	u32 size;
} DemoGame;

static const DemoGame demo_games[] = {
	{ "sd:/swiss_r2119.dol", NULL, 2938880 },
	{ "sd:/apps/GCMM.dol", NULL, 655360 },
	{ "sd:/apps/gc-forever.dol", NULL, 1003520 },
	{ "sd:/games/Super Smash Bros. Melee.iso", "GALJ01", 1459978240 },
	{ "sd:/apps/Mario64Port.dol", NULL, 3502080 },
	{ "sd:/apps/MemCardPro_Manager.dol", NULL, 419840 },
	{ "sd:/apps/Neogeo-CD-Redux.dol", NULL, 1689600 },
	{ "sd:/apps/PicoDrive.dol", NULL, 1239040 },
	{ "sd:/apps/ScummVM.dol", NULL, 4106240 },
	{ "sd:/apps/SNES9xGX.dol", NULL, 2293760 },
	{ "sd:/apps/vbagx.dol", NULL, 1935360 },
	{ "sd:/apps/emulators/fceugx.dol", NULL, 1464320 },
	{ "sd:/homebrew/tetris.dol", NULL, 122880 },
	{ "sd:/web/PicoLoader Guide.fpg", NULL, 20653120 },
	{ "sd:/roms/gba/Pocket Monsters - Ruby (Japan).gba", NULL, 8388608 },
	{ "sd:/roms/gba/Chinito Sushi (Japan) (GBA).gba", NULL, 616012 },
	{ "sd:/roms/gbc/Chinito Sushi (Japan) (GBC).gbc", NULL, 524288 },
	{ "sd:/MuiThai3D/muithai3d.dol", NULL, 989824 },
	{ "sd:/roms/gbc/Pocket Monsters Kin (Japan) (SGB Enhanced) (GB Compatible).gbc", NULL, 2097152 },
	{ "sd:/roms/gba/Pokemon - Emerald Version (USA, Europe).gba", NULL, 16777216 },
	{ "sd:/roms/gba/Mario Kart - Super Circuit (USA).gba", NULL, 4194304 },
	{ "sd:/roms/gbc/Pokemon - Crystal Version (USA, Europe) (Rev 1).gbc", NULL, 2097152 },
	{ "sd:/roms/gb/Tetris (World) (Rev 1).gb", NULL, 32768 },
};

void library_demo(void)
{
	all_count = 0;
	swiss_path[0] = vba_path[0] = 0;
	demo_card = true;
	for (size_t i = 0; i < sizeof(demo_games) / sizeof(demo_games[0]); i++) {
		const DemoGame *d = &demo_games[i];
		add_entry(d->path, kind_of(d->path), d->size, d->id, strchr(d->path + 4, '/') == NULL);
	}
	finish_scan();
}

const char *library_swiss_path(void)
{
	return swiss_path[0] ? swiss_path : NULL;
}

const char *library_vba_path(void)
{
	return vba_path[0] ? vba_path : NULL;
}
