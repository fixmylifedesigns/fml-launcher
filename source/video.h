#ifndef VIDEO_H
#define VIDEO_H

#include <gctypes.h>

// Plays an .fmv boot video (see tools/make_video.py) full screen, streaming
// each CMPR frame straight to the GPU. Give either a file path or a memory
// copy. Any button skips it. Returns false if there was nothing to play.
bool video_play(const char *path, const u8 *mem, u32 mem_size);

#endif
