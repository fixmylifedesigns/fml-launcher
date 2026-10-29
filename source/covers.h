#ifndef COVERS_H
#define COVERS_H

#include "gx.h"

// Cover textures for the carousel, loaded lazily around the selection.
// Each game gets the best art available: a .cov from sd:/covers/ (front,
// spine and back), else a generated cover (using the disc banner if any).

typedef struct {
	Tex front, spine, back; // back may be missing (not ready)
	u32 base;               // generated spine/edge colour
	bool loaded;
} Cover;

void covers_reset(bool demo); // forget everything (new card or demo)
void covers_update(int selected); // load at most one cover per call; frees far ones
Cover *covers_get(int index);

#endif
