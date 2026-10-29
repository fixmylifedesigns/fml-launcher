#ifndef GX_H
#define GX_H

#include <gccore.h>

#include "gfx.h"

// Thin layer over the GameCube GPU: video setup, textures and quads.

typedef struct {
	GXTexObj obj;
	void *data;
	int w, h;
	u8 fmt;
	bool ready;
} Tex;

enum { VMODE_AUTO, VMODE_480P, VMODE_480I, VMODE_COUNT };
void gx_init(int vmode);
void gx_set_video(int vmode); // switch output mode while running
bool gx_progressive(void);
void gx_set_wide(bool wide);  // 16:9 layout
extern bool gx_wide;
extern float gx_left, gx_right; // visible x range, in 640x480 layout units
GXRModeObj *gx_rmode(void);
void gx_end_frame(void); // copy to the TV and wait for vsync

// Textures. Sizes must be multiples of 4 (8 wide for I8).
void tex_from_surface(Tex *t, const Surface *s, u8 fmt); // GX_TF_RGBA8 or GX_TF_RGB565
void tex_from_tiles(Tex *t, const void *tiles, int w, int h, u8 fmt);
void tex_radial(Tex *t, int size); // soft round I8 blob
void tex_free(Tex *t);

// Projections. 2D uses screen pixels; 3D uses screen pixels on the z = 0 plane,
// with the camera `distance` in front of (eye_x, eye_y), y pointing down.
void gx_2d(void);
void gx_3d(float eye_x, float eye_y, float distance, Mtx view_out);

enum { BLEND_ALPHA, BLEND_PREMULT, BLEND_ADD };
void gx_blend(int mode);
void gx_depth(bool test, bool write);
void gx_texture(Tex *t); // NULL = vertex colour only

// Screen-space textured quad (2D mode)
void gx_quad(float x, float y, float w, float h, u32 rgba);

// Same, with explicit texture coordinates
void gx_quad_uv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 rgba);

// I8 textures used as alpha masks: output colour is the vertex colour and
// alpha is texture * vertex alpha. Used for text, round shapes and glows.
void gx_mask(Tex *t);

// GPU text and shapes (2D mode), so nothing has to be re-uploaded when the UI changes
void gx_ui_init(void);
void gx_text(int font, float x, float baseline, const char *s, u32 rgba);
void gx_text_centered(int font, float cx, float baseline, const char *s, u32 rgba);
void gx_text_fit(int font, float x, float baseline, const char *s, u32 rgba, int max_w);
void gx_round_rect(float x, float y, float w, float h, float r, u32 rgba);
void gx_circle(float cx, float cy, float r, u32 rgba);

static inline u32 rgba(u32 rgb, int a) { return (rgb << 8) | (u8)a; }

#endif
