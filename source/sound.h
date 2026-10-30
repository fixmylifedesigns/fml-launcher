#ifndef SOUND_H
#define SOUND_H

// Audio for the music and movie players (ASND mixer + libogc's MP3 player).
// Started the first time something plays, and shut down before another
// program runs.
void sound_start(void);
void sound_stop(void);

#endif
