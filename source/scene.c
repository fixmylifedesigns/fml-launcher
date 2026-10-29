#include <math.h>
#include <string.h>

#include "covers.h"
#include "gx.h"
#include "library.h"
#include "scene.h"
#include "shapes.h"

// Layout in design pixels (see design/mockup.html)
#define EYE_X 320
#define EYE_Y 190
#define EYE_DIST 720
#define BASE_Y 324 // screen y where cases stand on the floor
#define SIDE_ANGLE 58
#define SIDE_GAP 46
#define FLOOR_Y (-BASE_Y - 2)

typedef struct {
	float x, y, z, ry, bright, alpha, spin;
} Pose;

static Pose pose[MAX_GAMES];
static Tex halo_tex;
static u32 frame;

void scene_init(void)
{
	tex_radial(&halo_tex, 64);
}

static Pose target_pose(int i, int selected, bool launching)
{
	Pose p = { 0, 0, 0, 0, 1, 1, 0 };
	int off = i - selected, d = off < 0 ? -off : off, s = off < 0 ? -1 : 1;
	if (off == 0) {
		p.z = 70;
		if (launching) {
			p.z = 260;
			p.y = 10;
			p.spin = -180;
		}
	} else {
		p.x = s * (112 + (d - 1) * SIDE_GAP);
		p.z = -40 - d * 18;
		p.ry = -s * SIDE_ANGLE;
		p.bright = 0.75f - d * 0.08f;
		if (p.bright < 0.35f)
			p.bright = 0.35f;
		p.alpha = d > (gx_wide ? 8 : 5) || launching ? 0 : 1; // 16:9 has room for more
	}
	return p;
}

void scene_snap(int selected)
{
	for (int i = 0; i < game_count; i++)
		pose[i] = target_pose(i, selected, false);
}

static float settle;

void scene_step(int selected, bool launching)
{
	frame++;
	float moved = 0;
	for (int i = 0; i < game_count; i++) {
		Pose t = target_pose(i, selected, launching), *p = &pose[i];
		const float k = 0.16f;
		moved += fabsf(t.x - p->x) + fabsf(t.z - p->z) + fabsf(t.ry - p->ry);
		p->x += (t.x - p->x) * k;
		p->y += (t.y - p->y) * k;
		p->z += (t.z - p->z) * k;
		p->ry += (t.ry - p->ry) * k;
		p->bright += (t.bright - p->bright) * k;
		p->alpha += (t.alpha - p->alpha) * k;
		p->spin += (t.spin - p->spin) * k;
	}
	settle = moved < 1 ? 1 : 0;
}

float scene_settled(void) { return settle; }

static u32 shade(u32 rgb, float b, float a)
{
	int r = ((rgb >> 16) & 0xFF) * b, g = ((rgb >> 8) & 0xFF) * b, bl = (rgb & 0xFF) * b;
	return ((u32)r << 24) | ((u32)g << 16) | ((u32)bl << 8) | (u32)(a * 255);
}

static void vtx(float x, float y, float z, u32 c, float u, float v)
{
	GX_Position3f32(x, y, z);
	GX_Color1u32(c);
	GX_TexCoord2f32(u, v);
}

// Quad from 4 corners: top-left, top-right, bottom-right, bottom-left
static void face(Tex *t, const float q[4][3], u32 c, bool flip_u)
{
	gx_texture(t);
	float u0 = flip_u ? 1 : 0, u1 = flip_u ? 0 : 1;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	vtx(q[0][0], q[0][1], q[0][2], c, u0, 0);
	vtx(q[1][0], q[1][1], q[1][2], c, u1, 0);
	vtx(q[2][0], q[2][1], q[2][2], c, u1, 1);
	vtx(q[3][0], q[3][1], q[3][2], c, u0, 1);
	GX_End();
}

static void case_matrix(const Pose *p, float hh, bool selected, Mtx view, Mtx out)
{
	Mtx m;
	float ry = p->ry + p->spin, bob = 0;
	if (selected && p->spin > -1) {
		// Idle float: slow turn and bob, 4 second loop
		float t = (frame % 240) / 240.0f * 6.2831853f;
		ry += -9 * cosf(t);
		bob = 3 - 3 * cosf(t);
	}
	// Rotate about y, then move into place (view is a pure translation)
	float rad = ry * 3.14159265f / 180, c = cosf(rad), s = sinf(rad);
	memset(m, 0, sizeof(Mtx));
	m[0][0] = c;
	m[0][2] = s;
	m[1][1] = 1;
	m[2][0] = -s;
	m[2][2] = c;
	m[0][3] = EYE_X + p->x + view[0][3];
	m[1][3] = -BASE_Y + hh + p->y + bob + view[1][3];
	m[2][3] = p->z + view[2][3];
	memcpy(out, m, sizeof(Mtx));
}

// Game Boy boxes take their height from the box art's proportions, so
// Japanese and US boxes keep their real shapes. Everything else is fixed.
static void box_size(int i, float *hw, float *hh, float *hd)
{
	const CaseShape *sh = case_shape(&games[i]);
	*hw = sh->hw;
	*hh = sh->hh;
	*hd = sh->hd;
	Cover *c = covers_get(i);
	if (is_gameboy(&games[i]) && c->loaded && c->front.ready && c->front.w > 0) {
		// Keep about the same face area whatever the shape: landscape Japanese
		// GBA boxes get wider, tall Japanese GBC boxes taller.
		float aspect = (float)c->front.h / c->front.w, area = sh->hw * sh->hh;
		float w = sqrtf(area / aspect);
		w = w < 64 ? 64 : w > 118 ? 118 : w;
		float h = w * aspect;
		*hw = w;
		*hh = h > 112 ? 112 : h;
	}
}

static void draw_case(int i, bool selected, Mtx view)
{
	Pose *p = &pose[i];
	if (p->alpha < 0.01f)
		return;
	Cover *c = covers_get(i);
	float HW, HH, HD;
	box_size(i, &HW, &HH, &HD);
	Mtx mv;
	case_matrix(p, HH, selected, view, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	float b = p->bright, a = p->alpha;
	u32 base = c->loaded ? c->base : RGB(40, 40, 52);
	const float front[4][3] = { { -HW, HH, HD }, { HW, HH, HD }, { HW, -HH, HD }, { -HW, -HH, HD } };
	const float back[4][3] = { { HW, HH, -HD }, { -HW, HH, -HD }, { -HW, -HH, -HD }, { HW, -HH, -HD } };
	const float spine[4][3] = { { -HW, HH, -HD }, { -HW, HH, HD }, { -HW, -HH, HD }, { -HW, -HH, -HD } };
	const float edge[4][3] = { { HW, HH, HD }, { HW, HH, -HD }, { HW, -HH, -HD }, { HW, -HH, HD } };
	const float top[4][3] = { { -HW, HH, -HD }, { HW, HH, -HD }, { HW, HH, HD }, { -HW, HH, HD } };
	const float bottom[4][3] = { { -HW, -HH, HD }, { HW, -HH, HD }, { HW, -HH, -HD }, { -HW, -HH, -HD } };

	if (c->loaded && c->front.ready)
		face(&c->front, front, shade(0xFFFFFF, b, a), false);
	else
		face(NULL, front, shade(base, b, a), false);
	if (c->loaded && c->back.ready)
		face(&c->back, back, shade(0xFFFFFF, b * 0.8f, a), false);
	else
		face(NULL, back, shade(RGB(32, 32, 42), b, a), false);
	if (c->loaded && c->spine.ready)
		face(&c->spine, spine, shade(0xFFFFFF, b * 0.8f, a), false);
	else
		face(NULL, spine, shade(base, b * 0.8f, a), false);
	// GameCube cases are black plastic; Game Boy boxes are printed cardboard
	bool box = is_gameboy(&games[i]);
	face(NULL, edge, shade(box ? base : RGB(21, 21, 28), box ? b * 0.6f : b, a), false);
	face(NULL, top, shade(box ? base : RGB(42, 42, 52), box ? b * 0.9f : b, a), false);
	face(NULL, bottom, shade(box ? base : RGB(21, 21, 28), box ? b * 0.5f : b, a), false);
}

// Faded upside-down copy of the front face, just below the case
static void draw_reflection(int i, bool selected, Mtx view)
{
	Pose *p = &pose[i];
	Cover *c = covers_get(i);
	if (p->alpha < 0.01f || !c->loaded || !c->front.ready)
		return;
	float HW, HH, HD;
	box_size(i, &HW, &HH, &HD);
	Mtx mv;
	case_matrix(p, HH, selected, view, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	gx_texture(&c->front);
	float top = -HH - 2, mid = top - 2 * HH * 0.45f;
	u32 near = shade(0xFFFFFF, p->bright, 0.28f * p->alpha), far = shade(0xFFFFFF, p->bright, 0);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	vtx(-HW, top, HD, near, 0, 1);
	vtx(HW, top, HD, near, 1, 1);
	vtx(HW, mid, HD, far, 1, 0.55f);
	vtx(-HW, mid, HD, far, 0, 0.55f);
	GX_End();
}

// Floor grid, strongest just behind the cases and fading towards the viewer
// and into the distance (like the mockup's masked floor).
static int floor_alpha(float z)
{
	float near = (250 - z) / 300, far = (z + 320) / 200;
	near = near < 0 ? 0 : near > 1 ? 1 : near;
	far = far < 0 ? 0 : far > 1 ? 1 : far;
	return (int)(46 * near * far);
}

static void draw_floor(Mtx view, u32 hl)
{
	GX_LoadPosMtxImm(view, GX_PNMTX0);
	gx_texture(NULL);
	GX_SetLineWidth(9, GX_TO_ZERO);
	const int z_near = 264, z_far = -312;
	// Lines running into the distance, in segments so the fade follows depth
	for (int x = -1408; x <= 2048; x += 48) {
		GX_Begin(GX_LINESTRIP, GX_VTXFMT0, (z_near - z_far) / 48 + 1);
		for (int z = z_near; z >= z_far; z -= 48)
			vtx(x, FLOOR_Y, z, rgba(hl, floor_alpha(z)), 0, 0);
		GX_End();
	}
	for (int z = z_near; z >= z_far; z -= 48) {
		GX_Begin(GX_LINES, GX_VTXFMT0, 2);
		vtx(-1408, FLOOR_Y, z, rgba(hl, floor_alpha(z)), 0, 0);
		vtx(2048, FLOOR_Y, z, rgba(hl, floor_alpha(z)), 0, 0);
		GX_End();
	}
}

void scene_draw(int selected, u32 hl, bool halo)
{
	Mtx view;

	// Floor grid
	gx_3d(EYE_X, EYE_Y, EYE_DIST, view);
	gx_blend(BLEND_ALPHA);
	gx_depth(false, false);
	draw_floor(view, hl);

	// Pulsing glow under the selected case
	if (halo) {
		gx_2d();
		gx_texture(&halo_tex);
		float pulse = 0.775f + 0.225f * cosf((frame % 120) / 120.0f * 6.2831853f);
		gx_quad(230, 300, 180, 34, rgba(hl, (int)(180 * pulse)));
		gx_3d(EYE_X, EYE_Y, EYE_DIST, view);
	}

	// Reflections, then the cases from back to front
	int order[MAX_GAMES], n = 0;
	for (int i = 0; i < game_count; i++)
		if (pose[i].alpha > 0.01f && i - selected >= -9 && i - selected <= 9)
			order[n++] = i;
	for (int k = 0; k < n; k++)
		draw_reflection(order[k], order[k] == selected, view);

	// Sort by depth, farthest first
	for (int a = 1; a < n; a++)
		for (int b = a; b > 0 && pose[order[b]].z < pose[order[b - 1]].z; b--) {
			int t = order[b];
			order[b] = order[b - 1];
			order[b - 1] = t;
		}
	gx_depth(true, true);
	for (int k = 0; k < n; k++)
		draw_case(order[k], order[k] == selected, view);
	gx_depth(false, false);
}

