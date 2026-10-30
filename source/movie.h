#ifndef MOVIE_H
#define MOVIE_H
#include <gctypes.h>

// The movie player app: plays MPEG-1 videos (.mpg) from the card, with sound.
// Other formats are listed but need converting first (tools/make_movie.py).

void movie_open(void);
bool movie_input(u32 down, u32 held); // false = close the app
void movie_draw(void);
void movie_close(void);

#endif
