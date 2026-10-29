#ifndef SHAPES_H
#define SHAPES_H

#include "library.h"

// Size of each kind of case: GameCube games and homebrew use a GameCube case,
// Game Boy games a cardboard Game Boy box (a bit wider and shorter).
typedef struct {
	int front_w, front_h; // generated cover texture size (multiples of 4)
	float hw, hh, hd;     // half width, height, depth in design pixels
} CaseShape;

static const CaseShape SHAPE_CASE = { 160, 228, 75, 104, 11 };
static const CaseShape SHAPE_GB_BOX = { 192, 220, 86, 98, 14 };

static inline const CaseShape *case_shape(const Game *g)
{
	return is_gameboy(g) ? &SHAPE_GB_BOX : &SHAPE_CASE;
}

#endif
