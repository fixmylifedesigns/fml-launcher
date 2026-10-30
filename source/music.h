#ifndef MUSIC_H
#define MUSIC_H
#include <gctypes.h>

// The music player app: plays the .mp3 files on the card. Music keeps
// playing after the app is closed, until it's stopped or a game starts.

void music_open(void);
// Handles one frame of input; returns false when the app should close.
bool music_input(u32 down, u32 held);
void music_draw(void);
// Every frame, open or not: moves on to the next song when one ends.
void music_tick(void);
// "Artist - Title" of the song playing, or NULL
const char *music_now_playing(void);
void music_stop(void);

#endif
