#ifndef PAGEVIEW_H
#define PAGEVIEW_H

#include <gctypes.h>

// Scrollable web page snapshots (.fpg, made by tools/make_page.py). Only the
// strips on screen are kept in memory, so long pages stream from the card.

bool page_open(const char *path, const u8 *mem, u32 mem_size); // file, or a copy in memory
void page_close(void);
int page_height(void);
// Draws the page scrolled to `scroll` pixels (2D mode), loading strips as needed.
void page_draw(float scroll);

#endif
