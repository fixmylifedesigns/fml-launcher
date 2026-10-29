#ifndef SCENE_H
#define SCENE_H

#include <gctypes.h>

// The 3D carousel: one game case per library entry, drawn by the GPU.

void scene_init(void);
void scene_snap(int selected);                    // jump straight to the layout
void scene_step(int selected, bool launching);    // ease towards the layout, once per frame
void scene_draw(int selected, u32 highlight, bool halo);
float scene_settled(void);                        // 0 = still moving, 1 = at rest

#endif
