#include <malloc.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "gx.h"
#include "font_data.h"

#define FIFO_SIZE (256 * 1024)

static GXRModeObj *rmode;
static void *xfb[2];
static int cur_fb;

bool gx_wide;
float gx_left = 0, gx_right = 640;

// Progressive when asked, or on Auto when a component/digital cable is in
// (GCVideo, Carby, GCHD and component cables all report one).
static GXRModeObj *pick_mode(int vmode)
{
	bool prog = vmode == VMODE_480P || (vmode == VMODE_AUTO && VIDEO_HaveComponentCable());
	switch (VIDEO_GetCurrentTvMode()) {
	case VI_PAL:
		// PAL consoles do progressive at 60 Hz, like PAL games' own 480p option
		return prog ? &TVEurgb60Hz480Prog : &TVPal576IntDfScale;
	case VI_EURGB60:
		return prog ? &TVEurgb60Hz480Prog : &TVEurgb60Hz480IntDf;
	case VI_MPAL:
		return prog ? &TVMpal480Prog : &TVMpal480IntDf;
	default:
		return prog ? &TVNtsc480Prog : &TVNtsc480IntDf;
	}
}

static void apply_mode(GXRModeObj *m)
{
	rmode = m;
	VIDEO_Configure(rmode);
	VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer(xfb[cur_fb]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
}

static void setup_copy(void)
{
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	u32 xfb_height = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfb_height);
	// Interlaced output needs the flicker filter; progressive stays sharp without it
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, gx_progressive() ? GX_FALSE : GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering, rmode->viHeight == 2 * rmode->xfbHeight ? GX_ENABLE : GX_DISABLE);
	GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
}

void gx_init(int vmode)
{
	VIDEO_Init();
	// Room for the biggest mode, so switching modes later needs no new buffers
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(&TVPal576IntDfScale));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(&TVPal576IntDfScale));
	apply_mode(pick_mode(vmode));

	void *fifo = memalign(32, FIFO_SIZE);
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);

	GX_SetCopyClear((GXColor){ 0, 0, 0, 255 }, GX_MAX_Z24);
	setup_copy();
	GX_SetDispCopyGamma(GX_GM_1_0);
	GX_SetCullMode(GX_CULL_NONE);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	gx_texture(NULL);
	gx_blend(BLEND_ALPHA);
	gx_depth(false, false);

	GX_CopyDisp(xfb[0], GX_TRUE);
}

void gx_set_video(int vmode)
{
	GXRModeObj *m = pick_mode(vmode);
	if (m == rmode)
		return;
	GX_DrawDone();
	VIDEO_SetBlack(TRUE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	apply_mode(m);
	setup_copy();
}

bool gx_progressive(void)
{
	return (rmode->viTVMode & 3) == VI_PROGRESSIVE;
}

// 16:9 TVs stretch the 640-wide picture; drawing 4/3 more layout units across
// it keeps everything the right shape and shows more of the scene at the sides.
void gx_set_wide(bool wide)
{
	gx_wide = wide;
	float half = wide ? 320.0f * 4 / 3 : 320;
	gx_left = 320 - half;
	gx_right = 320 + half;
}

GXRModeObj *gx_rmode(void) { return rmode; }

void gx_end_frame(void)
{
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	cur_fb ^= 1;
	GX_CopyDisp(xfb[cur_fb], GX_TRUE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(xfb[cur_fb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

// ---------------------------------------------------------------- textures

static void tex_prepare(Tex *t, int w, int h, u8 fmt)
{
	u32 size = GX_GetTexBufferSize(w, h, fmt, GX_FALSE, 0);
	if (!t->data || t->w != w || t->h != h || t->fmt != fmt) {
		free(t->data);
		t->data = memalign(32, size);
		t->w = w;
		t->h = h;
		t->fmt = fmt;
	}
}

static void tex_finish(Tex *t, bool smooth)
{
	DCFlushRange(t->data, GX_GetTexBufferSize(t->w, t->h, t->fmt, GX_FALSE, 0));
	GX_InitTexObj(&t->obj, t->data, t->w, t->h, t->fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
	u8 filter = smooth ? GX_LINEAR : GX_NEAR;
	GX_InitTexObjFilterMode(&t->obj, filter, filter);
	GX_InvalidateTexAll();
	t->ready = true;
}

void tex_from_surface(Tex *t, const Surface *s, u8 fmt)
{
	tex_prepare(t, s->w, s->h, fmt);
	if (!t->data)
		return;
	if (fmt == GX_TF_RGBA8) {
		// 4x4 tiles: 16 AR pairs, then 16 GB pairs
		u8 *out = t->data;
		for (int ty = 0; ty < s->h; ty += 4) {
			for (int tx = 0; tx < s->w; tx += 4) {
				for (int y = ty; y < ty + 4; y++) {
					for (int x = tx; x < tx + 4; x++) {
						u32 p = s->px[y * s->w + x];
						int i = ((y - ty) * 4 + (x - tx)) * 2;
						out[i] = p >> 24;
						out[i + 1] = p >> 16;
						out[32 + i] = p >> 8;
						out[32 + i + 1] = p;
					}
				}
				out += 64;
			}
		}
	} else {
		u16 *out = t->data;
		for (int ty = 0; ty < s->h; ty += 4)
			for (int tx = 0; tx < s->w; tx += 4)
				for (int y = ty; y < ty + 4; y++)
					for (int x = tx; x < tx + 4; x++) {
						u32 p = s->px[y * s->w + x];
						*out++ = ((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F);
					}
	}
	tex_finish(t, fmt != GX_TF_RGBA8 || s->w != 640);
}

void tex_from_tiles(Tex *t, const void *tiles, int w, int h, u8 fmt)
{
	tex_prepare(t, w, h, fmt);
	if (!t->data)
		return;
	memcpy(t->data, tiles, GX_GetTexBufferSize(w, h, fmt, GX_FALSE, 0));
	tex_finish(t, true);
}

void tex_radial(Tex *t, int size)
{
	tex_prepare(t, size, size, GX_TF_I8);
	u8 *out = t->data;
	// 8x4 tiles
	for (int ty = 0; ty < size; ty += 4)
		for (int tx = 0; tx < size; tx += 8)
			for (int y = ty; y < ty + 4; y++)
				for (int x = tx; x < tx + 8; x++) {
					float dx = (x + 0.5f) / size * 2 - 1, dy = (y + 0.5f) / size * 2 - 1;
					float d = 1 - sqrtf(dx * dx + dy * dy);
					*out++ = d <= 0 ? 0 : (u8)(d * d * 255);
				}
	tex_finish(t, true);
}

void tex_free(Tex *t)
{
	free(t->data);
	memset(t, 0, sizeof(*t));
}

// ---------------------------------------------------------------- state

void gx_2d(void)
{
	Mtx44 proj;
	Mtx view;
	guOrtho(proj, 0, 480, gx_left, gx_right, 0, 1000);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	guMtxIdentity(view);
	guMtxTransApply(view, view, 0, 0, -100);
	GX_LoadPosMtxImm(view, GX_PNMTX0);
}

void gx_3d(float eye_x, float eye_y, float distance, Mtx view_out)
{
	// Off-centre frustum so the z = 0 plane maps 1:1 to screen pixels and the
	// vanishing point sits at (eye_x, eye_y), like CSS perspective-origin.
	const float n = 10, f = 5000;
	float k = n / distance;
	Mtx44 proj;
	guFrustum(proj, eye_y * k, -(480 - eye_y) * k, (gx_left - eye_x) * k, (gx_right - eye_x) * k, n, f);
	GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
	// World: x right, y up (screen y negated), z towards the viewer
	guMtxIdentity(view_out);
	guMtxTransApply(view_out, view_out, -eye_x, eye_y, -distance);
}

void gx_blend(int mode)
{
	if (mode == BLEND_PREMULT)
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	else if (mode == BLEND_ADD)
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
	else
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}

void gx_depth(bool test, bool write)
{
	GX_SetZMode(test ? GX_TRUE : GX_FALSE, GX_LEQUAL, write ? GX_TRUE : GX_FALSE);
}

void gx_texture(Tex *t)
{
	if (t && t->ready) {
		GX_LoadTexObj(&t->obj, GX_TEXMAP0);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	} else {
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
}

void gx_quad(float x, float y, float w, float h, u32 c)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position3f32(x, y, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(0, 0);
	GX_Position3f32(x + w, y, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(1, 0);
	GX_Position3f32(x + w, y + h, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(1, 1);
	GX_Position3f32(x, y + h, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(0, 1);
	GX_End();
}

void gx_quad_uv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 c)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position3f32(x, y, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(u0, v0);
	GX_Position3f32(x + w, y, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(u1, v0);
	GX_Position3f32(x + w, y + h, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(u1, v1);
	GX_Position3f32(x, y + h, 0);
	GX_Color1u32(c);
	GX_TexCoord2f32(u0, v1);
	GX_End();
}

void gx_mask(Tex *t)
{
	GX_LoadTexObj(&t->obj, GX_TEXMAP0);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

// ---------------------------------------------------------------- GPU text

// Fonts the UI draws on the GPU, packed into one I8 atlas at startup
static const int ui_fonts[] = { FONT_11B, FONT_12, FONT_13M, FONT_13S, FONT_15S, FONT_19B, FONT_20B };
#define UI_FONT_COUNT (int)(sizeof(ui_fonts) / sizeof(ui_fonts[0]))
#define GLYPHS 98
#define ATLAS_W 512
#define ATLAS_H 512

typedef struct {
	u16 x, y;
} AtlasPos;

static Tex atlas, circle;
static AtlasPos atlas_pos[FONT_COUNT][GLYPHS];
static bool in_atlas[FONT_COUNT];

static void i8_to_tiles(const u8 *src, u8 *dst, int w, int h)
{
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < w; tx += 8)
			for (int y = ty; y < ty + 4; y++)
				for (int x = tx; x < tx + 8; x++)
					*dst++ = src[y * w + x];
}

void gx_ui_init(void)
{
	// Shelf-pack every glyph of the UI fonts
	u8 *pixels = calloc(ATLAS_W * ATLAS_H, 1);
	int x = 1, y = 1, shelf = 0;
	for (int f = 0; f < UI_FONT_COUNT; f++) {
		const Font *font = &fonts[ui_fonts[f]];
		in_atlas[ui_fonts[f]] = true;
		for (int g = 0; g < GLYPHS; g++) {
			const Glyph *gl = &font->glyphs[g];
			if (x + gl->w + 1 > ATLAS_W) {
				x = 1;
				y += shelf + 1;
				shelf = 0;
			}
			if (y + gl->h + 1 > ATLAS_H)
				break;
			atlas_pos[ui_fonts[f]][g] = (AtlasPos){ x, y };
			for (int j = 0; j < gl->h; j++)
				memcpy(&pixels[(y + j) * ATLAS_W + x], font->bits + gl->off + j * gl->w, gl->w);
			x += gl->w + 1;
			if (gl->h > shelf)
				shelf = gl->h;
		}
	}
	tex_prepare(&atlas, ATLAS_W, ATLAS_H, GX_TF_I8);
	i8_to_tiles(pixels, atlas.data, ATLAS_W, ATLAS_H);
	tex_finish(&atlas, false);

	// Anti-aliased disc for round shapes
	const int cs = 64;
	for (int j = 0; j < cs; j++)
		for (int i = 0; i < cs; i++) {
			float dx = i + 0.5f - cs / 2.0f, dy = j + 0.5f - cs / 2.0f;
			float cov = cs / 2.0f - sqrtf(dx * dx + dy * dy) + 0.5f;
			pixels[j * cs + i] = cov <= 0 ? 0 : cov >= 1 ? 255 : (u8)(cov * 255);
		}
	tex_prepare(&circle, cs, cs, GX_TF_I8);
	i8_to_tiles(pixels, circle.data, cs, cs);
	tex_finish(&circle, true);
	free(pixels);
}

static int glyph_index(unsigned char c)
{
	if (c >= 32 && c <= 126)
		return c - 32;
	if (c >= 1 && c <= 3)
		return 95 + c - 1;
	return '?' - 32;
}

void gx_text(int font, float x, float baseline, const char *s, u32 rgba)
{
	if (!in_atlas[font])
		return;
	const Font *f = &fonts[font];
	gx_mask(&atlas);
	int pen = (int)(x * 16);
	for (; *s; s++) {
		int gi = glyph_index((unsigned char)*s);
		const Glyph *g = &f->glyphs[gi];
		if (g->w && g->h) {
			AtlasPos p = atlas_pos[font][gi];
			float gx = (pen + 8) / 16 + g->x, gy = baseline + g->y;
			gx_quad_uv(gx, gy, g->w, g->h, (float)p.x / ATLAS_W, (float)p.y / ATLAS_H,
			           (float)(p.x + g->w) / ATLAS_W, (float)(p.y + g->h) / ATLAS_H, rgba);
		}
		pen += g->adv;
	}
}

void gx_text_centered(int font, float cx, float baseline, const char *s, u32 rgba)
{
	gx_text(font, cx - gfx_text_width(font, s) / 2, baseline, s, rgba);
}

void gx_text_fit(int font, float x, float baseline, const char *s, u32 rgba, int max_w)
{
	if (gfx_text_width(font, s) <= max_w) {
		gx_text(font, x, baseline, s, rgba);
		return;
	}
	char buf[260];
	int len = strlen(s);
	if (len > 255)
		len = 255;
	while (len > 0) {
		memcpy(buf, s, len);
		strcpy(buf + len, "...");
		if (gfx_text_width(font, buf) <= max_w)
			break;
		len--;
	}
	gx_text(font, x, baseline, len ? buf : "...", rgba);
}

// Rounded rectangle: four quarter-discs and three plain rectangles
void gx_round_rect(float x, float y, float w, float h, float r, u32 rgba)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	gx_mask(&circle);
	gx_quad_uv(x, y, r, r, 0, 0, 0.5f, 0.5f, rgba);
	gx_quad_uv(x + w - r, y, r, r, 0.5f, 0, 1, 0.5f, rgba);
	gx_quad_uv(x, y + h - r, r, r, 0, 0.5f, 0.5f, 1, rgba);
	gx_quad_uv(x + w - r, y + h - r, r, r, 0.5f, 0.5f, 1, 1, rgba);
	// Middle parts sample the disc's solid centre
	gx_quad_uv(x + r, y, w - 2 * r, h, 0.5f, 0.5f, 0.5f, 0.5f, rgba);
	gx_quad_uv(x, y + r, r, h - 2 * r, 0.5f, 0.5f, 0.5f, 0.5f, rgba);
	gx_quad_uv(x + w - r, y + r, r, h - 2 * r, 0.5f, 0.5f, 0.5f, 0.5f, rgba);
}

void gx_circle(float cx, float cy, float r, u32 rgba)
{
	gx_round_rect(cx - r, cy - r, 2 * r, 2 * r, r, rgba);
}
