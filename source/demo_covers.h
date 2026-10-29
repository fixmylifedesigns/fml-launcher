#ifndef DEMO_COVERS_H
#define DEMO_COVERS_H

#include <gctypes.h>

// Covers baked into the build by `tools/covers.py embed`, used by demo mode.
typedef struct {
	const char *key; // game ID or .dol name
	const u8 *data;  // .cov file contents
	u32 size;
} DemoCover;

extern const DemoCover demo_covers[];
extern const int demo_cover_count;

#endif
