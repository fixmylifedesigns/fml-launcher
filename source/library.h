#ifndef LIBRARY_H
#define LIBRARY_H

#include <gctypes.h>

// Everything launchable on the card: .dol homebrew, .iso/.gcm disc images and
// Game Boy ROMs, found in any folder. games[] is the list currently shown,
// after the tab and search filters.

// KIND_APP: built into the launcher (music and movie players).
// KIND_DRIVE: the disc in the GameCube's own drive.
enum { KIND_DISC, KIND_DOL, KIND_GB, KIND_GBC, KIND_GBA, KIND_WEB, KIND_APP, KIND_DRIVE };

typedef struct {
	char path[256];  // "sd:/games/Melee.iso"
	char title[64];  // shown under the carousel
	char folder[64]; // "games/"
	char key[96];    // cover name: game ID for discs, file name for .dol and ROMs
	char id[7];      // game ID, discs only
	char region[16]; // Game Boy ROMs: "Japan", "USA", "Europe"...
	u32 size;
	u8 kind;
	bool is_disc;
	u8 disc_no;      // discs: 0 = disc 1, 1 = disc 2
	// Play history, from sd:/fml/history.txt
	bool fav;
	u16 plays;
	u32 last;        // when it was last started (seconds, GameCube clock), 0 = never
} Game;

#define MAX_GAMES 512

extern Game games[MAX_GAMES];
extern int game_count;

enum { TAB_ALL, TAB_RECENT, TAB_FAVS, TAB_GAMECUBE, TAB_GAMEBOY, TAB_APPS, TAB_COUNT };
extern const char *const tab_names[TAB_COUNT];

// Reads sd:/. Files already in sd:/fml/library.cache aren't opened again,
// so only new games cost any time.
void library_scan(void);
void library_demo(void); // fills in a fake card
// Rebuilds games[] from everything found, keeping only what matches the tab
// and contains `query` (case-insensitive) in its title or region.
void library_filter(int tab, const char *query);
int library_total(void);

static inline bool is_gameboy(const Game *g) { return g->kind >= KIND_GB && g->kind <= KIND_GBA; }

// The Swiss .dol on the card, or NULL. Swiss isn't in the list: it's opened
// from Settings and used in the background to start disc images.
const char *library_swiss_path(void);
// Visual Boy Advance GX, used to start Game Boy ROMs, or NULL
const char *library_vba_path(void);

// Play history and favourites (saved to the card, except on the demo card)
void library_played(const char *path, u32 now);
bool library_toggle_fav(const char *path); // returns the new state
const Game *library_last_played(void);     // NULL if nothing was played yet

// "Japan", "USA"... from the 4th letter of a GameCube game ID ("" if unknown)
const char *region_from_id(char c);

// The disc in the drive, shown first in the All and GameCube tabs (NULL = none).
// Call library_filter again afterwards.
void library_set_drive(const Game *g);
// The disc image on the card with this ID and disc number, or NULL
const Game *library_find_disc(const char *id, int disc_no);

// Music and video files on the card. They aren't shown on the shelf: the
// built-in players list them.
enum { MEDIA_MUSIC, MEDIA_VIDEO, MEDIA_TYPES };
typedef struct {
	char path[256];
	u32 size;
	bool playable; // videos: MPEG-1 (.mpg); other formats need converting first
} MediaFile;
int library_media(int type, const MediaFile **list);
// The demo card's music and video live in the program itself
bool library_demo_file(const char *path, const u8 **data, u32 *size);

#endif
