#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <psppower.h>
#include "pspmath.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "macros.h"
#ifndef PSP_DEVBENCH
#define PSP_TRI_CHAIN 0 /* patch 107 was tested on the real PSP: no measurable improvement (+0.1..0.3 ms), so it stays off */
#define PSP_DEVBENCH 0 /* DIAGNOSTIC PATCH: measures the CPU on the real PSP ~7 s after boot and closes the game (see psp_devbench). 0 = normal game */
#endif

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define INFO_MSG(x) printf("%s %s\n", __FILE__ ":" TOSTRING(__LINE__), x)
#define _UNUSED(x) (void)(x)

#define SUPPORT_CHECK(x) assert(x)

// align value to N-byte boundary
#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED (1024)
#define MAX_LIGHTS 2
#define MAX_VERTICES 64

/* Pixel Formats */
#define GU_PSM_5650		(0) /* Display, Texture, Palette */
#define GU_PSM_5551		(1) /* Display, Texture, Palette */
#define GU_PSM_4444		(2) /* Display, Texture, Palette */
#define GU_PSM_8888		(3) /* Display, Texture, Palette */
#define GU_PSM_T4		(4) /* Texture */
#define GU_PSM_T8		(5) /* Texture */
#define GU_PSM_T16		(6) /* Texture */
#define GU_PSM_T32		(7) /* Texture */
extern void* getStaticVramTexBuffer(unsigned int width, unsigned int height, unsigned int psm);
extern void gfx_scegu_draw_triangles_2d(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
extern void gfx_scegu_draw_quad_2d(float buf_vbo[]);
extern float identity_matrix[4][4];

struct RGBA {
    uint8_t r, g, b, a;
} __attribute__((packed, aligned(4)));

struct XYWidthHeight {
    uint16_t x, y, width, height;
} __attribute__((packed, aligned(4)));

struct LoadedVertex {
    float x, y, z, w;
    float _x, _y, _z, _w;
    float u, v;
    struct RGBA color;
    uint32_t clip_rej;
} __attribute__((packed, aligned(16)));

#if defined(TARGET_PSP)
typedef char LoadedVertex_vfpu_size_check[(sizeof(struct LoadedVertex) == 48) ? 1 : -1];
#ifdef GBI_FLOATS
typedef char Vtx_vfpu_size_check[(sizeof(Vtx) == 24) ? 1 : -1];
#else
typedef char Vtx_vfpu_size_check[(sizeof(Vtx) == 16) ? 1 : -1];
#endif
extern uint32_t gfx_clip_to_hyperplane_vfpu(struct LoadedVertex *dest,
                                            const struct LoadedVertex *source,
                                            const float plane[4], uint32_t in_count);
extern void gfx_transform_vertices_vfpu(struct LoadedVertex *dest, const Vtx *source,
                                        uint32_t count, const float matrix[4][4]);
extern void gfx_transform_vertices_eye_vfpu(struct LoadedVertex *dest, const Vtx *source,
                                            uint32_t count, const float matrices[32]);
#if defined(GBI_FLOATS) && defined(F3DEX_GBI_2E)
#define PSP_EYE2_VFPU 1
extern void gfx_transform_vertices_eye2_vfpu(struct LoadedVertex *dest, const Vtx *source,
                                             uint32_t count, const float matrices[32]);
#else
#define PSP_EYE2_VFPU 0
#endif
#endif

typedef struct VertexColor {
	unsigned short u, v;
	struct RGBA color;
	unsigned short x, y, z;
} VertexColor __attribute__((aligned(16)));

struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    
    uint32_t texture_id;
    uint8_t cms, cmt;
#if defined(TARGET_PSP)
    uint8_t mirror_s, mirror_t;
#endif
    bool linear_filter;
} __attribute__((packed, aligned(4)));
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t used_textures[2];
    uint8_t vertex_color_source[2];
    uint8_t env_alpha; /* the alpha channel comes from the environment color (SHADEFADEA / BLENDRGBFADEA) */
    uint8_t enva_tex;  /* BLENDRGBFADEA with texture */
} __attribute__((packed, aligned(4)));

static struct ColorCombiner color_combiner_pool[64];
static uint8_t color_combiner_pool_size;

struct TriPipelineState {
    struct ColorCombiner *comb;
    bool use_alpha;
    bool used_textures[2];
    bool use_texture;
    float tex_u_scale, tex_v_scale;
    float tex_u_bias, tex_v_bias;
    uint32_t shader_id; /* id of the active shader and its fog mode (scegu_fog_mode is a pure function of the id): computed once when preparing the state */
    int fog_mode;
} __attribute__((packed, aligned(4)));

struct RSP {
    float modelview_matrix_stack[11][4][4]__attribute__((aligned(16)));

    float MP_matrix[4][4] __attribute__((aligned(16)));
    float P_matrix[4][4] __attribute__((aligned(16)));
    uint8_t modelview_matrix_stack_size;
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    bool lookat_stale; // current_lookat_coeffs must be recomputed (only needed with texgen)
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    
    struct VertexColor loaded_vertices_2D[4];
    struct LoadedVertex loaded_vertices[MAX_VERTICES];
} __attribute__((aligned(16)));
static struct RSP rsp_bss __attribute__((aligned(16), unused));

/* PSP_SCRATCH: the "RSP" state (matrices, lights and above all the 64 loaded vertices, ~4.3 KB) and the temporary clip vertices live in
 * the PSP scratchpad (16 KB at 0x00010000, fast memory outside the 16 KB data cache). This way they do not compete with the game's command list
 * nor with the vertices being read from the ROM, which are what dirties the cache the most. Compile with -DPSP_SCRATCH=0 to
 * go back to normal memory. */
#if defined(TARGET_PSP) && !defined(PSP_SCRATCH)
#define PSP_SCRATCH 1
#endif
#if defined(TARGET_PSP) && PSP_SCRATCH
#define PSP_SCRATCH_BASE 0x00010000u
#define PSP_SCRATCH_CLIP_OFF ((sizeof(struct RSP) + 63u) & ~63u)
typedef char psp_scratch_size_check[(PSP_SCRATCH_CLIP_OFF + 54u * 48u) <= 0x4000u ? 1 : -1];
#define rsp (*(struct RSP *) PSP_SCRATCH_BASE)
#else
#define rsp rsp_bss
#endif

static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
    } loaded_texture[2];
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    int32_t sc_ulx, sc_uly, sc_lrx, sc_lry; /* scissor N64 crudo (U10.2) */
    bool sc_valid;
    float vp_l, vp_t, vp_w, vp_h; /* viewport in N64 pixels (from the top) */
    void *z_buf_address;
    void *color_image_address;
} rdp  __attribute__((aligned(4)));

static struct RenderingState {
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct ColorCombiner *color_combiner;
    uint32_t color_combiner_id;
    bool color_combiner_valid;
    struct TextureHashmapNode *textures[2];
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    bool tri_pipeline_dirty;
    bool backend_state_dirty;
    struct TriPipelineState tri_pipeline;
} rendering_state __attribute__((aligned(16)));

struct GfxDimensions gfx_current_dimensions __attribute__((aligned(4)));

static bool dropped_frame;
static const struct RGBA white_color = {0xff, 0xff, 0xff, 0xff};

#if defined(TARGET_PSP)
typedef struct psp_fast_t {
  float u,v;
  struct RGBA color;
  float x,y,z;
} psp_fast_t;
/* EXACT N64 fog factor (0..255) per vertex of the buffer; used by gfx_scegu.c for the 2nd pass. */
unsigned char scegu_fog_alpha[MAX_BUFFERED * 3];
/* PSP_EYE_SPACE: vertices are transformed on the CPU to EYE SPACE (model*view) and the GE only applies the projection; the GE's
 * model and view matrices stay at identity. Before, each model G_MTX flushed the batch (one draw call per
 * object/part) because the GE needed its model matrix; now different objects with the same state (coins, identical enemies,
 * parts of the same model) share a call. The result is the same matrix product. Only with PSP_F2_REPLAY 0 (list
 * replay patches the GE's matrices). PSP_EYE_SPACE 0 goes back to the previous path. */
#ifndef PSP_EYE_SPACE
#define PSP_EYE_SPACE (!PSP_F2_REPLAY)
#endif
#if PSP_EYE_SPACE
float scegu_mtx_proj[16] __attribute__((aligned(16))),
      scegu_mtx_view[16] __attribute__((aligned(16))) = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 },
      scegu_mtx_model[16] __attribute__((aligned(16))) = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
#else
float scegu_mtx_proj[16] __attribute__((aligned(16))), scegu_mtx_view[16] __attribute__((aligned(16))), scegu_mtx_model[16] __attribute__((aligned(16)));
#endif
uint32_t scegu_mt_tex[2]; /* texture_id of the tile 0 and 1 textures of the current draw */
extern int scegu_fog_mode(uint32_t shader_id); /* gfx_scegu.c */
static psp_fast_t buf_vbo[MAX_BUFFERED  * 3] __attribute__ ((aligned (32))); // 3 vertices in a triangle and 26 floats per vtx
#else
static float buf_vbo[MAX_BUFFERED * (26 * 3)] // 3 vertices in a triangle and 26 floats per vtx
#endif
static size_t buf_vbo_len;
static size_t buf_num_vert;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#if defined(TARGET_PSP)
static uint8_t psp_texture_stage_buf[256 * 256 * 4] __attribute__((aligned(16)));

static const uint8_t *gfx_prepare_psp_texture_for_upload(const uint8_t *src, uint32_t width, uint32_t height, unsigned int type, bool mirror_s, bool mirror_t, uint32_t *upload_width, uint32_t *upload_height, bool *applied_mirror_s, bool *applied_mirror_t) {
    const size_t bytes_per_pixel = type == GU_PSM_5551 ? 2 : 4;
    const uint32_t source_x_offset = mirror_s ? width : 0;
    const uint32_t source_y_offset = mirror_t ? height : 0;

    *applied_mirror_s = mirror_s;
    *applied_mirror_t = mirror_t;
    *upload_width = width * (mirror_s ? 2 : 1);
    *upload_height = height * (mirror_t ? 2 : 1);

    if (!mirror_s && !mirror_t) {
        return src;
    }

    if ((size_t)(*upload_width) * (*upload_height) * bytes_per_pixel > sizeof(psp_texture_stage_buf)) {
        *upload_width = width;
        *upload_height = height;
        *applied_mirror_s = false;
        *applied_mirror_t = false;
        return src;
    }

    const size_t src_row_bytes = (size_t)width * bytes_per_pixel;
    const size_t dst_row_bytes = (size_t)(*upload_width) * bytes_per_pixel;

    memset(psp_texture_stage_buf, 0, (size_t)(*upload_width) * (*upload_height) * bytes_per_pixel);

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src_row = src + (size_t)y * src_row_bytes;
        uint8_t *dst_row = psp_texture_stage_buf + (size_t)(source_y_offset + y) * dst_row_bytes + (size_t)source_x_offset * bytes_per_pixel;

        memcpy(dst_row, src_row, src_row_bytes);

        if (mirror_s) {
            uint8_t *dst_mirror_row = psp_texture_stage_buf + (size_t)(source_y_offset + y) * dst_row_bytes;
            for (uint32_t x = 0; x < width; x++) {
                memcpy(dst_mirror_row + (size_t)x * bytes_per_pixel,
                       src_row + (size_t)(width - 1 - x) * bytes_per_pixel,
                       bytes_per_pixel);
            }
        }
    }

    if (mirror_t) {
        const uint32_t mirror_y_offset = source_y_offset ? 0 : height;
        for (uint32_t y = 0; y < height; y++) {
            memcpy(psp_texture_stage_buf + (size_t)(mirror_y_offset + y) * dst_row_bytes,
                   psp_texture_stage_buf + (size_t)(source_y_offset + height - 1 - y) * dst_row_bytes,
                   dst_row_bytes);
        }
    }

    return psp_texture_stage_buf;
}

static void gfx_upload_texture(int tile, const uint8_t *buf, uint32_t width, uint32_t height, unsigned int type) {
    const bool mirror_s = (rdp.texture_tile.cms & G_TX_MIRROR) != 0;
    const bool mirror_t = (rdp.texture_tile.cmt & G_TX_MIRROR) != 0;
    uint32_t upload_width = width;
    uint32_t upload_height = height;
    bool applied_mirror_s = false;
    bool applied_mirror_t = false;
    const uint8_t *upload_buf = gfx_prepare_psp_texture_for_upload(buf, width, height, type, mirror_s, mirror_t, &upload_width, &upload_height, &applied_mirror_s, &applied_mirror_t);

    rendering_state.textures[tile]->mirror_s = applied_mirror_s;
    rendering_state.textures[tile]->mirror_t = applied_mirror_t;

    gfx_rapi->upload_texture(upload_buf, upload_width, upload_height, type);
}
#else
static void gfx_upload_texture(int tile, const uint8_t *buf, uint32_t width, uint32_t height, unsigned int type) {
    _UNUSED(tile);
    gfx_rapi->upload_texture(buf, width, height, type);
}
#endif

#if defined(TARGET_PSP)
#include <pspthreadman.h>
static unsigned long get_time(void) {
    return sceKernelGetSystemTimeWide();
}
#else
#include <time.h>
static unsigned long get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif


//******************* Clipping things

// Bits for clipping
// +-+-+-
// xxyyzz
#define Z_NEG  (0x01)
#define Z_POS  (0x02)
#define Y_NEG  (0x04)
#define Y_POS  (0x08)
#define X_NEG  (0x10)
#define X_POS  (0x20)

// Test all but Z_NEG (for No Near Plane microcodes)
#define CLIP_TEST_FLAGS ( X_POS | X_NEG | Y_POS | Y_NEG | Z_POS | Z_NEG)
#define SC_IN (0x100) /* extra bit of clip_rej: the vertex is SURELY inside all the scissor planes (see gfx_vtx_post); 0 = not known */
//#define CLIP_TEST_FLAGS ( Z_POS | Z_NEG ) /* Faster but worse */

/* X/Y clipping by the GE: the PSP does not clip sides, but its "guard band" accepts primitives with vertices up to ~2048 px from the
 * viewport center and the scissor already clips the raster. A triangle that only crosses a side/top/bottom edge of the
 * screen, with all its vertices in front of the camera (w > 0) and within PSP_GE_XY_GUARD times w of the center (NDC), is
 * sent as is (clipping in 3D does not change how attributes are interpolated). Whatever touches near/far or leaves the guard
 * is still clipped on the CPU. PSP_GE_XY_CLIP 0 goes back to clipping everything on the CPU. (Idea from ghurone/Sonic-R-PSP.) */
#ifndef PSP_GE_XY_CLIP
#define PSP_GE_XY_CLIP 1
#endif
#define PSP_GE_XY_GUARD 3.0f

/* PSP_F2_REPLAY_MTX: while recording a frame 1 the trivial reject and the side/vertical clip let through up to this
 * multiple of w (1.0 = exact screen). The 2nd frame moves the objects half a tick without clipping again: what was just outside the
 * screen in the 1st may enter in the 2nd, and the GE clips it (guard band + scissor). */
#define PSP_REC_XY_MARGIN 1.25f
static float sRejXY = 1.0f;
/* When recording, a triangle that is NOT discarded is discarded anyway if it is clearly back-facing; edge-on ones (sine of the angle between their two
 * edges on screen smaller than this, ~7 degrees) are let through, because with the 2nd frame's camera they may become front-facing. */
#define PSP_REC_CULL_SIN2 0.0150f
/* PSP_F2_REPLAY_MTX: object (gMtxTbl entry) whose matrices are being interpreted and band near the near plane (see below) */
static int sRecCur = -1;
static float sRecNearBand;
static unsigned char sRecNear[1024];

/* PSP_F2_SKY: the sky carries its orthographic matrix in its own list (G_MTX_PROJECTION | G_MTX_MUL), which the GE receives as a
 * GU_VIEW block. When recording (frame 1) it is noted where that block ended up; in frame 2 it is rewritten with the orthographic of the real state. */
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX && PSP_F2_SKY && defined(GBI_FLOATS)
#define PSP_SKY_PATCH 1
extern const float *psp_sky_interp_ortho(void);
extern const float *psp_sky_real_ortho(void);
extern int gfx_scegu_view_block_ok(unsigned int off, const float *m);
extern void gfx_scegu_write_view(unsigned int off, const float *m);
extern unsigned int gfx_scegu_list_pos(void);
static int sRecMtxOn;                                       /* 1 while recording (defined below) */
static const float *sSkyInterpPtr;                          /* interpolated sky orthographic (the one frame 1 carries) */
static unsigned int sRecSkyOff;                             /* offset of the sky's GU_VIEW block in list A */
static int sRecSkyOk;                                       /* that block is noted and verified */
static float sRecSkyPre[16] __attribute__((aligned(16)));   /* GU_VIEW accumulated BEFORE multiplying the sky orthographic */
#else
#define PSP_SKY_PATCH 0
#endif

static inline float vec3_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2]);
}

static inline float vec4_dot(const float *lhs, const float *rhs){
    return (lhs[0]*rhs[0]) + (lhs[1]*rhs[1]) + (lhs[2]*rhs[2])+ (lhs[3]*rhs[3]);
}

static inline void vec4_sub(float *out, const float* lhs, const float*rhs){
    out[0] = lhs[0]-rhs[0];
    out[1] = lhs[1]-rhs[1];
    out[2] = lhs[2]-rhs[2];
    out[3] = lhs[3]-rhs[3];
}

void gfx_clip_interpolate_vert(struct LoadedVertex* out, const struct  LoadedVertex* lhs, const struct LoadedVertex* rhs, const float factor )
{
    // projected pos
    out->x = lhs->x + (rhs->x - lhs->x) * factor;
    out->y = lhs->y + (rhs->y - lhs->y) * factor;
    out->z = lhs->z + (rhs->z - lhs->z) * factor;
    //out->w = lhs->w + (rhs->w - lhs->w) * factor;
    // transfomed pos
    out->_x = lhs->_x + (rhs->_x - lhs->_x) * factor;
    out->_y = lhs->_y + (rhs->_y - lhs->_y) * factor;
    out->_z = lhs->_z + (rhs->_z - lhs->_z) * factor;
    out->_w = lhs->_w + (rhs->_w - lhs->_w) * factor;
    // color
    out->color.r = lhs->color.r + (rhs->color.r - lhs->color.r) * factor;
    out->color.g = lhs->color.g + (rhs->color.g - lhs->color.g) * factor;
    out->color.b = lhs->color.b + (rhs->color.b - lhs->color.b) * factor;
    out->color.a = lhs->color.a + (rhs->color.a - lhs->color.a) * factor;
    // texture
    out->u = lhs->u + (rhs->u - lhs->u) * factor;
    out->v = lhs->v + (rhs->v - lhs->v) * factor;
}

//*****************************************************************************
//
//	The following clipping code was taken from The Irrlicht Engine.
//	See http://irrlicht.sourceforge.net/ for more information.
//	Copyright (C) 2002-2006 Nikolaus Gebhardt/Alten Thomas
//
//*****************************************************************************
static const float NDCPlane[6][4] __attribute__((aligned(16))) =
{
	{  0.f,  0.f,  1.f, -1.f },	// near
	{  1.f,  0.f,  0.f, -1.f },	// left
	{ -1.f,  0.f,  0.f, -1.f },	// right
	{  0.f,  1.f,  0.f, -1.f },	// bottom
	{  0.f, -1.f,  0.f, -1.f },	// top
	{  0.f,  0.f, -1.f, -1.f }	// far
};

static uint32_t clipToHyperPlane( struct LoadedVertex *dest, const struct LoadedVertex *source, uint32_t inCount, const float plane[4] )
{
#if defined(TARGET_PSP)
	return gfx_clip_to_hyperplane_vfpu(dest, source, plane, inCount);
#else
	uint32_t outCount;
	struct LoadedVertex *out;

	const struct LoadedVertex *a;
	const struct LoadedVertex *b;

	float aDotPlane;
	float bDotPlane;
    float temp_vec[4];

	out = dest;
	outCount = 0;
	b = source;
	bDotPlane = vec4_dot(&b->_x, plane);
    size_t i;

#define EPSILON 0.00000001
	for(i = 1; i < inCount + 1; ++i)
	{
		a = &source[i%inCount];
		aDotPlane = vec4_dot(&a->_x, plane);

		// current point inside
		if ( aDotPlane <= EPSILON )
		{
			// last point outside
			if ( bDotPlane > EPSILON )
			{
				// intersect line segment with plane
                // Next 2 lines are "(b->ProjectedPos - a->ProjectedPos).Dot( plane )"
                vec4_sub(temp_vec, &b->_x, &a->_x);
                const float dot_projected = vec4_dot(temp_vec, plane);
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );
				out += 1;
				outCount += 1;
			}
			// copy current to out
			*out = *a;
			b = out;

			out += 1;
			outCount += 1;
		}
		else
		{
			// current point outside

			if ( bDotPlane <= EPSILON )
			{
				// previous was inside
				// intersect line segment with plane
                // Next 2 lines are "(b->ProjectedPos - a->ProjectedPos).Dot( plane )"
                vec4_sub(temp_vec, &b->_x, &a->_x);
                const float dot_projected = vec4_dot(temp_vec, plane);
				gfx_clip_interpolate_vert(out, b, a, bDotPlane / dot_projected );

				out += 1;
				outCount += 1;
			}
			b = a;
		}

        bDotPlane = vec4_dot(&b->_x, plane);
	}

	return outCount;
#endif
}

/* Same as NDCPlane but with the sides at PSP_REC_XY_MARGIN (only while recording a frame 1, see sRejXY) */
static const float NDCPlaneWide[6][4] __attribute__((aligned(16))) =
{
	{  0.f,  0.f,  1.f, -1.f },	// near
	{  1.f,  0.f,  0.f, -PSP_REC_XY_MARGIN },	// left
	{ -1.f,  0.f,  0.f, -PSP_REC_XY_MARGIN },	// right
	{  0.f,  1.f,  0.f, -PSP_REC_XY_MARGIN },	// bottom
	{  0.f, -1.f,  0.f, -PSP_REC_XY_MARGIN },	// top
	{  0.f,  0.f, -1.f, -1.f }	// far
};

/* The usual version (the 6 passes, in this order; each one leaves the polygon rotated one position). Only used to check the new one (PSP_CLIPCHECK). */
#ifdef PSP_CLIPCHECK
uint32_t clip_to_frustum_ref( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn )
{
	uint32_t vOut;
	const float (*pl)[4] = (sRejXY > 1.0f) ? NDCPlaneWide : NDCPlane;
	vOut = vIn;
	vOut = clipToHyperPlane( v1, v0, vOut, pl[2] );		// right
	vOut = clipToHyperPlane( v0, v1, vOut, pl[1] );		// left
	vOut = clipToHyperPlane( v1, v0, vOut, pl[4] );		// top
	vOut = clipToHyperPlane( v0, v1, vOut, pl[3] );		// bottom
	vOut = clipToHyperPlane( v1, v0, vOut, pl[0] );		// near
	vOut = clipToHyperPlane( v0, v1, vOut, pl[5] );		// far
	return vOut;
}
#endif

/* Clipping against the frustum. Before: always the 6 passes (each one walks all the vertices with the VFPU), although almost all triangles only
 * cross 1 plane (the near). Now: only the passes of the planes for which some vertex is outside (clip_rej bits in "need"). A pass
 * over a polygon entirely inside returns the same vertices, UNMODIFIED, starting from the 2nd (= rotated one position), so
 * skipping it is equivalent to rotating the polygon one position: the count (rot) is kept and applied on the next real pass or at the end.
 * The result is the same, vertex by vertex and in the same order, as with the 6 passes. *res = buffer where it ends up (v0 or v1). */
uint32_t clip_to_frustum( struct LoadedVertex * v0, struct LoadedVertex * v1, uint32_t vIn, uint32_t need, struct LoadedVertex **res )
{
	static const unsigned char step_pl[6]  = { 2, 1, 4, 3, 0, 5 };
	static const unsigned char step_bit[6] = { X_POS, X_NEG, Y_POS, Y_NEG, Z_NEG, Z_POS };
	const float (*pl)[4] = (sRejXY > 1.0f) ? NDCPlaneWide : NDCPlane;
	struct LoadedVertex *buf[2] = { v0, v1 };
	int ci = 0;
	uint32_t n = vIn;
	uint32_t rot = 0;

	for (int s = 0; s < 6; s++) {
		if (!(need & step_bit[s])) {
			rot++;
			continue;
		}
		const uint32_t r = n ? (rot % n) : 0;
		if (r) {
			/* polygon rotated r positions in the other buffer, and the pass writes to this one again */
			struct LoadedVertex *src = buf[ci];
			struct LoadedVertex *tmp = buf[ci ^ 1];
			for (uint32_t j = 0; j < n; j++) {
				const uint32_t k = j + r;
				tmp[j] = src[k >= n ? k - n : k];
			}
			n = clipToHyperPlane( src, tmp, n, pl[step_pl[s]] );
		} else {
			n = clipToHyperPlane( buf[ci ^ 1], buf[ci], n, pl[step_pl[s]] );
			ci ^= 1;
		}
		rot = 0;
	}
	{
		const uint32_t r = n ? (rot % n) : 0;
		if (r) {
			struct LoadedVertex *src = buf[ci];
			struct LoadedVertex *tmp = buf[ci ^ 1];
			for (uint32_t j = 0; j < n; j++) {
				const uint32_t k = j + r;
				tmp[j] = src[k >= n ? k - n : k];
			}
			ci ^= 1;
		}
	}
	*res = buf[ci];
	return n;
}

static struct LoadedVertex temp_a[20];
static struct LoadedVertex temp_b[20];

/* RDP scissor clip planes (adjusted clip space, same as _x/_y/_z/_w).
 * Each plane: a*x + b*y + c*z + d*w <= 0 is "inside". Only the edges that are NOT
 * against the screen edge (4 px margin) are generated, so the game's normal scissor changes nothing. */
static float gfx_adjust_x_for_aspect_ratio(float x);
static float sc_planes[4][4] __attribute__((aligned(16)));
static unsigned char sc_ptype[4]; /* 0 = plano en x (a*x + d*w), 1 = plano en y (b*y + d*w) */

/* (4/3)/aspect factor: it only changes when the dimensions change, so it is computed once (before it was 2 divisions
 * per vertex and per triangle). */
static float sAspectK = 1.0f;
static int sAspectW = -1, sAspectH = -1;
static inline float gfx_aspect_k(void) {
    const int w = (int)gfx_current_dimensions.width, h = (int)gfx_current_dimensions.height;
    if (w != sAspectW || h != sAspectH) {
        sAspectW = w;
        sAspectH = h;
        sAspectK = (4.0f / 3.0f) / ((float)w / (float)h);
        if (sAspectK > 0.995f && sAspectK < 1.005f) sAspectK = 1.0f; /* 4:3 view (362x272): this is the original N64 projection, with no adjustment */
    }
    return sAspectK;
}

static int sProjCompensate = 0; /* 1 if the GE draws with the matrix compensated to 16:9 (x * k) */

static float sScGX = 1.0e30f, sScGY = 1.0e30f; /* bounds of the per-vertex scissor pre-test (see below) */
static int gfx_scissor_planes_compute(void) {
    if (!rdp.sc_valid || rdp.vp_w <= 1.0f || rdp.vp_h <= 1.0f) {
        sScGX = sScGY = 1.0e30f;
        return 0;
    }
    /* The RDP scissor is in N64 pixels of the screen; clipping is done in the CPU's clip space
     * (_x = k * projected_x always; the GE draws with x*k only if the projection is compensated).
     * - Scissor edge almost against the viewport edge (credits windows, 1-2 px margin):
     * the edge is a fraction of the viewport; with a compensated projection the CPU frustum already clips it.
     * - Interior edge (dialog boxes): exact N64 coordinates, compressed by k like the rest of the 2D. */
    const float k = gfx_aspect_k();
    const float sl = rdp.sc_ulx / 4.0f, sr = rdp.sc_lrx / 4.0f;
    const float st = rdp.sc_uly / 4.0f, sb = rdp.sc_lry / 4.0f;
    const float il = sl - rdp.vp_l;
    const float ir = (rdp.vp_l + rdp.vp_w) - sr;
    const float it = st - rdp.vp_t;
    const float ib = (rdp.vp_t + rdp.vp_h) - sb;
    const float near_edge = 2.5f;
    /* While a frame 1 is being recorded (sRejXY > 1) the scissor edges that are a few pixels from the screen edge are not
     * clipped on the CPU: the GE's scissor does it (the list carries it) and so the 2nd frame, with the camera moved, has geometry
     * outside the edge to show (otherwise a strip with the background appeared on the sides). */
    const bool rec = (sRejXY > 1.0f);
    const float rec_edge = 12.0f;
    int n = 0;

    if (il > -0.25f && !(rec && il <= rec_edge)) {
        const float g = -1.0f + 2.0f * il / rdp.vp_w;
        const bool near = il <= near_edge;
        const float mul = (near && sProjCompensate) ? 1.0f : k;
        if (!(near && sProjCompensate && il <= 0.25f)) {
            sc_planes[n][0] = -1.f; sc_planes[n][1] = 0.f; sc_planes[n][2] = 0.f; sc_planes[n][3] = g * mul; sc_ptype[n] = 0; n++;
        }
    }
    if (ir > -0.25f && !(rec && ir <= rec_edge)) {
        const float g = 1.0f - 2.0f * ir / rdp.vp_w;
        const bool near = ir <= near_edge;
        const float mul = (near && sProjCompensate) ? 1.0f : k;
        if (!(near && sProjCompensate && ir <= 0.25f)) {
            sc_planes[n][0] = 1.f; sc_planes[n][1] = 0.f; sc_planes[n][2] = 0.f; sc_planes[n][3] = -(g * mul); sc_ptype[n] = 0; n++;
        }
    }
    if (it > 0.25f && !(rec && it <= rec_edge)) {
        const float g = 1.0f - 2.0f * it / rdp.vp_h;
        sc_planes[n][0] = 0.f; sc_planes[n][1] = 1.f; sc_planes[n][2] = 0.f; sc_planes[n][3] = -g; sc_ptype[n] = 1; n++;
    }
    if (ib > 0.25f && !(rec && ib <= rec_edge)) {
        const float g = -1.0f + 2.0f * ib / rdp.vp_h;
        sc_planes[n][0] = 0.f; sc_planes[n][1] = -1.f; sc_planes[n][2] = 0.f; sc_planes[n][3] = g; sc_ptype[n] = 1; n++;
    }
    {
        float gx = 1.0e30f, gy = 1.0e30f;
        for (int p = 0; p < n; p++) {
            float m = -sc_planes[p][3] * 0.999f;
            if (m < 0.0f) m = 0.0f;
            if (sc_ptype[p] == 0) { if (m < gx) gx = m; } else { if (m < gy) gy = m; }
        }
        sScGX = gx; sScGY = gy;
    }
    return n;
}

/* Called once per triangle, but the result only depends on the scissor, the viewport, the aspect factor and the
 * projection compensation: it is cached and only recomputed if any of them changes (identical result, without the ~6 divisions). */
static int sScDirty = 1, sScNp = 0;
/* Bounds for the per-vertex scissor pre-test (gfx_vtx_post): |x| <= sScGX * w and |y| <= sScGY * w => the vertex is not outside any scissor
 * plane (the planes are axis-aligned and at distance m = -dw from the center; m is used with a 0.1 % margin that amply covers rounding).
 * Without planes on an axis, the bound is huge (always satisfied). */
/* Scissor / viewport / projection / margin change: the planes must be recomputed and the already loaded vertices lose their SC_IN mark. */
static inline void gfx_sc_touch(void) {
    sScDirty = 1;
    for (int i = 0; i < MAX_VERTICES; i++) {
        rsp.loaded_vertices[i].clip_rej &= ~(uint32_t)SC_IN;
    }
}
static int gfx_scissor_planes_slow(void) {
    static int c_ok = 0, c_n = 0, c_valid = 0, c_comp = 0, c_rec = 0;
    static int c_sc[4];
    static float c_vp[4], c_k;
    const float k = gfx_aspect_k();
    const int rec_now = (sRejXY > 1.0f);
    if (c_ok && c_valid == (int)rdp.sc_valid && c_comp == sProjCompensate && c_k == k && c_rec == rec_now &&
        c_sc[0] == rdp.sc_ulx && c_sc[1] == rdp.sc_uly && c_sc[2] == rdp.sc_lrx && c_sc[3] == rdp.sc_lry &&
        c_vp[0] == rdp.vp_l && c_vp[1] == rdp.vp_t && c_vp[2] == rdp.vp_w && c_vp[3] == rdp.vp_h) {
        return c_n;
    }
    c_n = gfx_scissor_planes_compute();
    c_ok = 1;
    c_valid = (int)rdp.sc_valid;
    c_comp = sProjCompensate;
    c_rec = rec_now;
    c_k = k;
    c_sc[0] = rdp.sc_ulx; c_sc[1] = rdp.sc_uly; c_sc[2] = rdp.sc_lrx; c_sc[3] = rdp.sc_lry;
    c_vp[0] = rdp.vp_l; c_vp[1] = rdp.vp_t; c_vp[2] = rdp.vp_w; c_vp[3] = rdp.vp_h;
    return c_n;
}

/* Each triangle only looks at one flag: whoever changes scissor/viewport/projection/margin/dimensions sets sScDirty. */
static inline int gfx_scissor_planes(void) {
    if (__builtin_expect(sScDirty, 0)) {
        sScNp = gfx_scissor_planes_slow();
        sScDirty = 0;
    }
    return sScNp;
}

void gfx_clip_single_vert( struct LoadedVertex *p_p_vertices, size_t *p_num_vertices, struct LoadedVertex *v_arr[3], bool do_frustum, int sc_np, uint32_t need)
{
	//
	//	At this point all vertices are lit/projected and have both transformed and projected
	//	vertex positions. For the best results we clip against the projected vertex positions,
	//	but use the resulting intersections to interpolate the transformed positions. 
	//
    size_t clipped_vertices_num = 0;

    temp_a[ 0 ] = *v_arr[ 0 ];
    temp_a[ 1 ] = *v_arr[ 1 ];
    temp_a[ 2 ] = *v_arr[ 2 ];

    uint32_t out = 3;
    struct LoadedVertex *cur = temp_a; /* buffer where the polygon is */
    if( do_frustum ) {
#ifdef PSP_CLIPCHECK
        static struct LoadedVertex chk_a[20], chk_b[20];
        static unsigned int chk_n, chk_bad, chk_skip;
        chk_a[0] = temp_a[0]; chk_a[1] = temp_a[1]; chk_a[2] = temp_a[2];
        const uint32_t ref_out = clip_to_frustum_ref( chk_a, chk_b, 3 );
#endif
        out = clip_to_frustum( temp_a, temp_b, 3, need, &cur );
#ifdef PSP_CLIPCHECK
        chk_n++;
        if (need != CLIP_TEST_FLAGS) chk_skip++;
        if (ref_out != out || memcmp(chk_a, cur, out * sizeof(struct LoadedVertex)) != 0) chk_bad++;
        if ((chk_n % 500) == 0) printf("CLIPCHECK n=%u noall=%u bad=%u\n", chk_n, chk_skip, chk_bad);
#endif
        if( out < 3 ){
            *p_num_vertices = 0;
            return;
        }
    }

    /* Clipping against the RDP scissor (scissor planes) */
    if( sc_np > 0 ) {
        struct LoadedVertex *src = cur;
        struct LoadedVertex *dst = (cur == temp_a) ? temp_b : temp_a;
        for( int i = 0; i < sc_np; ++i ) {
            out = clipToHyperPlane( dst, src, out, sc_planes[i] );
            struct LoadedVertex *t = src; src = dst; dst = t;
            if( out < 3 ) {
                *p_num_vertices = 0;
                return;
            }
        }
        if( src != temp_a ) {
            memcpy( temp_a, src, out * sizeof(struct LoadedVertex) );
        }
    } else if( cur != temp_a ) {
        memcpy( temp_a, cur, out * sizeof(struct LoadedVertex) );
    }

    // Retesselate
    for( uint32_t j = 0; j <= out - 3; ++j )
    {            
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ 0 ] );
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ j + 1 ] );
        p_p_vertices[clipped_vertices_num++] = ( temp_a[ j + 2 ] );
    }

	*p_num_vertices = clipped_vertices_num;
}

//******************* End Clipping things


/* Profile Sweep (options menu, only for measuring): every 6 log windows (~6 s) one more drawing stage is turned off, with the camera still,
 * and the log notes the mode in OSWP. The EUS difference between two consecutive modes is what that stage costs on the real PSP.
 * 0 normal | 1 the GE draws but with a 1-pixel scissor (no screen fill) | 2 + nothing sent to the GE | 3 + no triangle copying
 * 4 + no state preparation or clipping | 5 + no triangle culling | 6 + vertices only transformed (no color / clip)
 * 7 + no vertices | 8 + no walking the game's list */
int gPspSweep = 0;
static int sSweepMode = 0;
static int sSweepWin = 0;

/* Profile counters (printed to ms0:/sm64_profile.txt). Counting is free; the timers only run while
 * the log is active (Triangle + R), so they cost nothing in the normal game. */
extern int sProfLogOn; /* defined below */
extern int gPspGpuProf; /* GPU profile, defined below */
extern float gPcAreaAcc;
extern unsigned int gPspArenaMax; /* gfx_scegu.c */
static unsigned int sPcTri, sPcVtx, sPcDraw, sPcMtx, sPcClip, sPcTexMiss, sPcTexClr, sPcDrawUs, sPcTexUs;
static unsigned int sPcTexEv, sPcTexSame; /* texture changes requested by the game (tile 0) / of those, the ones that were already the bound texture (without breaking the batch) */
static unsigned int sPcClipSkip;
extern short gMenuMode;           /* ingame_menu.c: -1 = no pause menu */
static unsigned int sPcMtxUs, sPcPrepUs, sPcPrepN; /* time in gfx_sp_matrix, time (without drawing or textures) and number of times the triangle pipeline state was rebuilt */
static unsigned int sPcVtxUs, sPcTriSub, sPcTriUs, sPcTriSmp, sTriSampleCtr; /* time in gfx_sp_vertex, triangles received, sampling of gfx_sp_tri1 */
/* time breakdown per triangle (only with the log active): class 0 = discarded early (back face / off screen),
 * 1 = drawn without CPU clipping, 2 = goes through CPU clipping. Time and number of samples (1 in 8) per class. */
static unsigned int sTriClass, sTriClassUs[3], sTriClassN[3], sPcClipUs;
unsigned int gPcFogAll, gPcFogKeep; /* triangles with fog (all / the ones that need a 2nd pass), incremented by gfx_scegu.c */

#if PSP_DEVBENCH == 5
/* Diagnostic 5: a real frame is replayed with an exact cycle counter (CP0 Count, kernel mode only) and each stage is broken down. */
static int sB5On, sB5Cull, sB5Abl, sB5Cap, sB5Hash, sB5PrepOn;
static unsigned int sB5HashV, sB5PAcc[6], sB5PN, sB5CapSeen[16];
static void b5_micro_capture(struct LoadedVertex *t1, struct LoadedVertex *t2, struct LoadedVertex *t3);
static unsigned int sB5H[12], sB5NCmd;
#define B5ABL(b_) ((sB5Abl >> (b_)) & 1)
#define B5H(i_, c_) do { sB5H[i_] += (c_) ? 1u : 0u; } while (0)
static unsigned int sB5Acc[16], sB5Cnt[16], sB5Mark[4];
#ifdef PSP_B5_TIMER
#define B5C() (sceKernelGetSystemTimeLow() * 333u) /* only to test the logic in the emulator (there is no user CP0) */
#else
#define B5C() ({ unsigned int c_; __asm__ volatile("mfc0 %0, $9" : "=r"(c_)); c_; })
#endif
#define B5MARK(i_) do { if (sB5On) sB5Mark[i_] = B5C(); } while (0)
#else
#define B5MARK(i_) do { } while (0)
#define B5ABL(b_) 0
#define B5H(i_, c_) do { } while (0)
#endif
/* Matrix variant (bits): 1 = the model*projection product (rsp.MP_matrix) is computed when loading vertices and not on every G_MTX / G_POPMTX,
 * 2 = gfx_update_mv multiplies directly on its matrices, without intermediate copies. */
#ifndef PSP_MTX_VAR
#define PSP_MTX_VAR 0
#endif
#if PSP_DEVBENCH == 5
static int sMtxVar, sB5OpOn;
static unsigned int sB5OpAcc[256], sB5OpN[256], sB5Sec[8], sB5SecN[8];
#define MTXVAR sMtxVar
#define B5S0(n_) const unsigned int n_ = sB5OpOn ? sceKernelGetSystemTimeLow() : 0U
#define B5S1(n_, i_) do { if (sB5OpOn) { sB5Sec[i_] += sceKernelGetSystemTimeLow() - n_; sB5SecN[i_]++; } } while (0)
#else
#define MTXVAR PSP_MTX_VAR
#define B5S0(n_) do { } while (0)
#define B5S1(n_, i_) do { } while (0)
#endif
#if PSP_DEVBENCH == 5
#define B5T(i_, call_) do { if (sB5On) { const unsigned int b5t_ = B5C(); call_; sB5Acc[i_] += B5C() - b5t_; sB5Cnt[i_]++; } else { call_; } } while (0)
#else
#define B5T(i_, call_) call_
#endif

static void gfx_flush(void) {
#if PSP_DEVBENCH == 5
    const unsigned int b5f0 = sB5On ? B5C() : 0;
#endif
    if (buf_vbo_len > 0) {
#if PSP_DEVBENCH == 5
        if (sB5Hash) {
            const uint32_t *hw = (const uint32_t *)(const void *)buf_vbo;
            unsigned int hh = sB5HashV;
            for (size_t q = 0; q < buf_vbo_len / 4; q++) hh = (hh ^ hw[q]) * 16777619u;
            for (size_t q = 0; q < buf_num_vert; q++) hh = (hh ^ scegu_fog_alpha[q]) * 16777619u;
            hh = (hh ^ (unsigned int)buf_vbo_num_tris) * 16777619u;
            sB5HashV = hh;
        }
#endif
        //int num = buf_vbo_num_tris;
        //unsigned long t0 = get_time();
        sPcDraw++;
        if (__builtin_expect(sSweepMode >= 2, 0)) {
            /* Profile Sweep: nothing is sent to the GE */
        } else if (__builtin_expect(sSweepMode == 1, 0)) {
            /* Profile Sweep: the GE transforms and prepares everything but barely fills pixels (1x1 scissor) */
            gfx_rapi->set_scissor(0, 0, 1, 1);
            gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
            gfx_rapi->set_scissor(rendering_state.scissor.x, rendering_state.scissor.y, rendering_state.scissor.width, rendering_state.scissor.height);
        } else if (sProfLogOn) {
            const unsigned int tq0 = sceKernelGetSystemTimeLow();
            gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
            sPcDrawUs += sceKernelGetSystemTimeLow() - tq0;
        } else {
            gfx_rapi->draw_triangles((float *)buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        }
        buf_vbo_len = 0;
        buf_num_vert = 0;
        buf_vbo_num_tris = 0;
        //unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
#if PSP_DEVBENCH == 5
    if (sB5On) { sB5Acc[10] += B5C() - b5f0; sB5Cnt[10]++; }
#endif
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = (cc_id >> 24) << 24;
    uint8_t shader_input_mapping[2][4] = {{0}};
    struct CCFeatures cc_features;
    for (int i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (int i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (int j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
    }
    gfx_cc_get_features(shader_id, &cc_features);
    /* BLENDRGBFADEA (alpha = ENV: gorra/Mario/Boo desvaneciendose) y BLENDRGBA (alpha = SHADE: explosiones, humo)
     * they share shader id 0x38D but need different texture modes on the GE; they are separated with a bit of their own. */
    if ((shader_id & 0xFFFu) == 0x38Du && cc_features.num_inputs != 0 &&
        shader_input_mapping[1][cc_features.num_inputs - 1] == CC_ENV) {
        shader_id |= (1u << 28); /* SHADER_OPT_ENVA */
    }
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id);
    comb->used_textures[0] = cc_features.used_textures[0];
    comb->used_textures[1] = cc_features.used_textures[1];
    comb->vertex_color_source[0] = cc_features.num_inputs == 0 ? CC_0 : shader_input_mapping[0][cc_features.num_inputs - 1];
    comb->vertex_color_source[1] = cc_features.num_inputs == 0 ? CC_0 : shader_input_mapping[1][cc_features.num_inputs - 1];
    comb->env_alpha = (comb->vertex_color_source[1] == CC_ENV);
    comb->enva_tex = ((shader_id & (1u << 28)) && cc_features.used_textures[0]) ? 1 : 0;
}

static inline struct RGBA gfx_vertex_color_from(uint8_t source, const struct RGBA *shade_color, float lod_w, bool allow_lod);

/* BLENDRGBFADEA textured patches (Boo's eyes/mouth): the model draws them BEFORE with SHADEFADEA (same vertex array).
 * With DECAL (which gives lerp(shade, texture, alpha_tex) with alpha = ENV over the WHOLE patch) the face accumulated as a 3rd translucent layer
 * and looked more opaque. If the patch was already drawn in the color pass, the texture pass is done with MODULATE and white
 * vertex color: only the texels with alpha are drawn, with the texture's color. Patches that have NO previous pass (top of the
 * loose hat, Mario) still use DECAL. */
int scegu_enva_modulate = 0; /* read by gfx_scegu.c when drawing */
static const void *sLastVtxSrc = NULL;
static const void *sEnvaVtxRing[8];
static int sEnvaVtxRingPos = 0;
static bool sEnvaWhite = false;

/* The GE only has ONE color per vertex. Before, with alpha (fading objects: toads, Bowser, blue stars)
 * the whole color of the ALPHA source was returned (e.g. env = 255,255,255,a), losing the lighting (shade)
 * of the RGB and leaving the object flat and very light. Now the RGB comes from the color channel source and only the
 * alpha comes from the alpha channel source, as the N64 combiner does. */
static inline struct RGBA gfx_get_vertex_color(const struct ColorCombiner *comb, bool use_alpha, const struct RGBA *shade_color, float lod_w, bool allow_lod) {
    struct RGBA c = gfx_vertex_color_from(comb->vertex_color_source[0], shade_color, lod_w, allow_lod);
    if (sEnvaWhite) {
        c.r = c.g = c.b = 255;
    }
    if (use_alpha) {
        const struct RGBA a = gfx_vertex_color_from(comb->vertex_color_source[1], shade_color, lod_w, allow_lod);
        c.a = a.a;
    }
    /* 2-texture blend by LOD (Peach/Bowser frame): the combiner's alpha channel is just TEXEL0A, so
     * without this the alpha came out 255 and the LOD factor the backend uses to blend the 2nd pass was lost. */
    if (allow_lod && comb->vertex_color_source[0] == CC_LOD) {
        c.a = c.r;
    }
    return c;
}

static inline struct RGBA gfx_vertex_color_from(uint8_t source, const struct RGBA *shade_color, float lod_w, bool allow_lod) {
    switch (source) {
        case CC_PRIM:
            return rdp.prim_color;
        case CC_SHADE:
            return *shade_color;
        case CC_ENV:
            return rdp.env_color;
        case CC_LOD:
            if (allow_lod) {
                /* lod_w = texels/pixel ratio (N64) of this vertex. With 2 levels (max level 1):
                 * <=1 -> level 0, frac 0 ; 1..2 -> frac = ratio-1 ; >=2 -> saturates at level 1 (frac 1) */
                float distance_frac = lod_w - 1.0f;
                if (distance_frac < 0.0f) distance_frac = 0.0f;
                if (distance_frac > 1.0f) distance_frac = 1.0f;
                const uint8_t lod = distance_frac * 255.0f;
                return (struct RGBA){lod, lod, lod, lod};
            }
            break;
    }
    return white_color;
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }
    
    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

extern int gfx_vram_space_available(void);
extern void texman_clear(void);
extern unsigned int texman_get_bound(void);
extern void gfx_scegu_sync_all(void); /* gfx_scegu.c: waits for the GE to finish everything sent */

#if defined(TARGET_PSP)
/* Same as the gfx_texture_cache_lookup search but WITHOUT side effects (it neither binds nor changes the GE sampler): returns the node or NULL. */
static struct TextureHashmapNode *gfx_texture_cache_peek(const uint8_t *orig_addr, uint32_t fmt, uint32_t siz) {
    size_t hash = (uintptr_t)orig_addr;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode *node = gfx_texture_cache.hashmap[hash];
    const uint8_t mirror_s = (rdp.texture_tile.cms & G_TX_MIRROR) != 0;
    const uint8_t mirror_t = (rdp.texture_tile.cmt & G_TX_MIRROR) != 0;
    while (node != NULL && node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if (node->texture_addr == orig_addr && node->fmt == fmt && node->siz == siz &&
            node->mirror_s == mirror_s && node->mirror_t == mirror_t) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}
#endif

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz) {
    size_t hash = (uintptr_t)orig_addr;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
#if defined(TARGET_PSP)
    const uint8_t mirror_s = (rdp.texture_tile.cms & G_TX_MIRROR) != 0;
    const uint8_t mirror_t = (rdp.texture_tile.cmt & G_TX_MIRROR) != 0;
#endif
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz
#if defined(TARGET_PSP)
            && (*node)->mirror_s == mirror_s && (*node)->mirror_t == mirror_t
#endif
        ) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            gfx_rapi->set_sampler_parameters(tile, (*node)->linear_filter, (*node)->cms, (*node)->cmt);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    if(!gfx_vram_space_available()) {
        gfx_scegu_sync_all(); /* the GE might still be reading textures that the new ones are going to overwrite */
        texman_clear();
        sPcTexClr++;

        // Pool is full. We just invalidate everything and start over.
        gfx_texture_cache.pool_pos = 0;
        memset(gfx_texture_cache.pool, 0, sizeof(gfx_texture_cache.pool));
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        // Pool is full. We just invalidate everything and start over.
#if defined(TARGET_PSP)
        /* The cache nodes are all dropped, so the texture store must restart together with them: otherwise every node created from now on
         * asks for a new texture slot while the old ones stay allocated, and the slot counter of the texture manager (512 entries)
         * runs past its table. */
        gfx_scegu_sync_all();
        texman_clear();
        sPcTexClr++;
#endif
        gfx_texture_cache.pool_pos = 0;
        memset(gfx_texture_cache.pool, 0, sizeof(gfx_texture_cache.pool));
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    sPcTexMiss++;
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    /*@Note: unneeded due to sequential GE flow */
    //gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
#if defined(TARGET_PSP)
    (*node)->mirror_s = mirror_s;
    (*node)->mirror_t = mirror_t;
#endif
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    *n = *node;
    return false;
}

static void import_texture_rgba16(int tile) {
    uint16_t rgba16_buf[4096] __attribute__ ((aligned(4)));    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint16_t col16 = (rdp.loaded_texture[tile].addr[2 * i] << 8) | rdp.loaded_texture[tile].addr[2 * i + 1];
        const uint8_t a = col16 & 1;
        const uint8_t r = (col16 >> 11) & 0x1f;
        const uint8_t g = (col16 >> 6) & 0x1f;
        const uint8_t b = (col16 >> 1) & 0x1f;
        rgba16_buf[i] = (a << 15)  | (b << 10)  | (g << 5) | (r);
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_upload_texture(tile, (const uint8_t *)rgba16_buf, width, height, GU_PSM_5551);
}

static void import_texture_rgba32(int tile) {
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = (rdp.loaded_texture[tile].size_bytes / 2) / rdp.texture_tile.line_size_bytes;
    gfx_upload_texture(tile, rdp.loaded_texture[tile].addr, width, height, GU_PSM_8888);
}

static void import_texture_ia4(int tile) {
    uint8_t rgba32_buf[32768] __attribute__ ((aligned(4)));
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part >> 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_3_8(r);
        rgba32_buf[4*i + 1] = SCALE_3_8(g);
        rgba32_buf[4*i + 2] = SCALE_3_8(b);
        rgba32_buf[4*i + 3] = alpha ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_ia8(int tile) {
    uint8_t rgba32_buf[16384]__attribute__ ((aligned(4)));
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[i] >> 4;
        uint8_t alpha = rdp.loaded_texture[tile].addr[i] & 0xf;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = SCALE_4_8(alpha);
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_ia16(int tile) {
    uint8_t rgba32_buf[8192];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[2 * i];
        uint8_t alpha = rdp.loaded_texture[tile].addr[2 * i + 1];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = alpha;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_i4(int tile) {
    uint8_t rgba32_buf[32768];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = 255;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_i8(int tile) {
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[i];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = 255;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}


static void import_texture_ci4(int tile) {
    uint8_t rgba32_buf[32768];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint16_t col16 = (rdp.palette[idx * 2] << 8) | rdp.palette[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture_ci8(int tile) {
    uint8_t rgba32_buf[16384];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t idx = rdp.loaded_texture[tile].addr[i];
        uint16_t col16 = (rdp.palette[idx * 2] << 8) | rdp.palette[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_upload_texture(tile, rgba32_buf, width, height, GU_PSM_8888);
}

static void import_texture(int tile) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
    
    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], rdp.loaded_texture[tile].addr, fmt, siz)) {
        return;
    }
    
    //int t0 = get_time();
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    //int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);
}

static inline void gfx_mark_tri_pipeline_dirty(void) {
    rendering_state.tri_pipeline_dirty = true;
}


#if defined(TARGET_PSP) && PSP_DECAL_ADAPTIVE
/* ---- EXACT N64 decal mode (ZMODE_DEC) ----
 * Formula taken from an exact RDP emulator (ParaLLEl-RDP, depth_test.h / rdp_renderer.cpp): the decal passes if |z - z_memory| <= (combined_dz << 3)
 * in 18-bit z (z18 = 8 * z15, z15 = 0..0x7FFF), where
 * dzpix       = |dz/dx| + |dz/dy| of the triangle in N64 pixels (z15 per pixel), rounded UP to a power of 2 (normalize_dzpix)
 * memory_dz   = that of the already drawn pixel (here: the same plane, the floor); if z < 0.875 of the range (compressed z exponent < 3):
 * max(memory_dz << 1, 16 >> exponent)
 * combined_dz = largest power of 2 <= (dzpix | memory_dz)
 * The GE can only shift the depth (16-bit z = 2 * z15) towards the camera, so the N64 tolerance is applied as
 * offset = 2 * combined_dz (in z15 = combined_dz) + a small margin for the GE's interpolation error (PSP_DECAL_OFFSET).
 * It comes out in power-of-2 steps: triangles of the same plane fall into the same one and almost never break the draw batch.
 * Slopes are measured in N64 pixels (240 high, square), same as the LOD calculation. */
extern void gfx_scegu_set_decal_bias(int bias);
static int sDecalBias = PSP_DECAL_OFFSET; /* the one the GE has set (only valid inside decal mode) */
static inline int gfx_msb_u32(unsigned int v) { return 31 - __builtin_clz(v); }
static int gfx_decal_bias(const struct LoadedVertex *a, const struct LoadedVertex *b, const struct LoadedVertex *c) {
    const float wa = a->_w, wb = b->_w, wc = c->_w;
    if (!(wa > 0.01f && wb > 0.01f && wc > 0.01f)) {
        return PSP_DECAL_FALLBACK;
    }
    {
        const float aspect = (float) gfx_current_dimensions.width / (float) gfx_current_dimensions.height;
        const float px = 90.0f * aspect * aspect, py = 120.0f, kz = 16383.5f; /* z15 = (ndc_z + 1) * 16383.5 */
        const float ia = 1.0f / wa, ib = 1.0f / wb, ic = 1.0f / wc;
        const float za_n = a->_z * ia, zb_n = b->_z * ib, zc_n = c->_z * ic;
        const float xa = a->_x * ia * px, ya = a->_y * ia * py, za = za_n * kz;
        const float x1 = b->_x * ib * px - xa, y1 = b->_y * ib * py - ya, z1 = zb_n * kz - za;
        const float x2 = c->_x * ic * px - xa, y2 = c->_y * ic * py - ya, z2 = zc_n * kz - za;
        const float det = x1 * y2 - x2 * y1;
        float dzdx, dzdy, zm;
        int ix, iy, s, dz, mem, comb, e, z18, inv, tol;
        if (fabsf(det) < 1e-4f) {
            return sDecalBias; /* edge-on: not visible, the current offset is left as is */
        }
        dzdx = fabsf((z1 * y2 - z2 * y1) / det);
        dzdy = fabsf((x1 * z2 - x2 * z1) / det);
        ix = dzdx < 32767.0f ? (int) dzdx : 32767;
        iy = dzdy < 32767.0f ? (int) dzdy : 32767;
        s = ix + iy;
        if (s >= 0x8000) dz = 0x8000;
        else if (s == 0) dz = 1;
        else dz = 1 << (gfx_msb_u32((unsigned int) s) + 1); /* normalize_dzpix */
        zm = (za_n + zb_n + zc_n) * (1.0f / 3.0f);
        z18 = (int) ((zm + 1.0f) * (16383.5f * 8.0f));
        if (z18 < 0) z18 = 0;
        if (z18 > 0x3ffff) z18 = 0x3ffff;
        inv = 0x3ffff - z18;
        if (inv < 1) inv = 1;
        e = 17 - gfx_msb_u32((unsigned int) inv); /* exponente de z comprimido (z_compress) */
        if (e < 0) e = 0;
        if (e > 7) e = 7;
        mem = dz;
        if (e < 3) {
            const int m2 = 16 >> e;
            mem = dz << 1;
            if (m2 > mem) mem = m2;
        }
        comb = dz | mem;
        comb = 1 << gfx_msb_u32((unsigned int) comb); /* combine_dz */
        tol = (int) ((float) (comb * 2) * PSP_DECAL_SCALE) + PSP_DECAL_OFFSET; /* z15 -> GE z16 (x2), times the scale, plus the margin */
        if (tol > PSP_DECAL_MAX) tol = PSP_DECAL_MAX;
        return tol;
    }
}
#endif

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    int mix;
    bool texture_used[2];
    int texture_ord[2];
    int num_inputs;
};

static void gfx_prepare_tri_pipeline_state(void) {
    bool backend_state_dirty;

    if (!rendering_state.tri_pipeline_dirty) {
        return;
    }

    backend_state_dirty = rendering_state.backend_state_dirty;
#if PSP_DEVBENCH == 5
    unsigned int b5pt = 0;
#define B5P(i_) do { if (sB5PrepOn) { const unsigned int b5pn = sceKernelGetSystemTimeLow(); if (i_ == 0) { sB5PN++; } else { sB5PAcc[i_] += b5pn - b5pt; } b5pt = b5pn; } } while (0)
    B5P(0);
#else
#define B5P(i_) do { } while (0)
#endif

    const unsigned int pp0 = sProfLogOn ? sceKernelGetSystemTimeLow() : 0U; /* profile timer: see PREPMS */
    const unsigned int pd0 = sPcDrawUs + sPcTexUs;

    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (backend_state_dirty || depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (backend_state_dirty || z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }

    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (backend_state_dirty || zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
#if defined(TARGET_PSP) && PSP_DECAL_ADAPTIVE
        sDecalBias = PSP_DECAL_OFFSET; /* the backend's set_zmode_decal(true) leaves it at PSP_DECAL_OFFSET */
#endif
    }

    if (backend_state_dirty || rdp.viewport_or_scissor_changed) {
        bool vp_changed = false;
        if (backend_state_dirty ||
            memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
            vp_changed = true; /* on PSP set_viewport overwrites the scissor: it must be reapplied */
        }
        if (backend_state_dirty || vp_changed ||
            memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    B5P(1);
    uint32_t cc_id = rdp.combine_mode;

    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;

    if (texture_edge) {
        use_alpha = true;
    }
    if (use_noise) {
        use_alpha = true; /* noise fade-out always needs alpha */
    }

    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;

    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }

    if (!rendering_state.color_combiner_valid || rendering_state.color_combiner_id != cc_id) {
        rendering_state.color_combiner = gfx_lookup_or_create_color_combiner(cc_id);
        rendering_state.color_combiner_id = cc_id;
        rendering_state.color_combiner_valid = true;
    }

    struct ColorCombiner *comb = rendering_state.color_combiner;
    struct ShaderProgram *prg = comb->prg;
    if (backend_state_dirty || prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (backend_state_dirty || use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }

    B5P(2);
    const bool used_textures[2] = {comb->used_textures[0], comb->used_textures[1]};
    const bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;

    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
#if defined(TARGET_PSP)
            if (i == 0 && rdp.textures_changed[0]) {
                /* The game repeats loading the SAME texture (every instance of an object, every piece of a level with the same material) and before
                 * that broke the batch even though the GE had nothing to change: a draw of ~6 triangles at a time. If the requested texture is
                 * exactly the one already bound (same cache node, same mirrors) there is nothing to send: the same batch continues. The
                 * sampler parameters are compared further below, as usual, and if they differ it does break. */
                sPcTexEv++;
                struct TextureHashmapNode *pk = gfx_texture_cache_peek(rdp.loaded_texture[0].addr, rdp.texture_tile.fmt, rdp.texture_tile.siz);
                if (!backend_state_dirty && pk != NULL && pk == rendering_state.textures[0] && texman_get_bound() == pk->texture_id) {
                    rdp.textures_changed[0] = false;
                    sPcTexSame++;
                }
            }
#endif
            if (rdp.textures_changed[i]) {
                gfx_flush();
                if (sProfLogOn) {
                    const unsigned int tq0 = sceKernelGetSystemTimeLow();
                    import_texture(i);
                    sPcTexUs += sceKernelGetSystemTimeLow() - tq0;
                } else {
                    import_texture(i);
                }
                rdp.textures_changed[i] = false;
            }
            scegu_mt_tex[i] = rendering_state.textures[i]->texture_id; /* ids for the 2-texture blend (Peach/Bowser frame) */
            if (backend_state_dirty ||
                linear_filter != rendering_state.textures[i]->linear_filter ||
                rdp.texture_tile.cms != rendering_state.textures[i]->cms ||
                rdp.texture_tile.cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile.cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile.cmt;
            }
        }
    }

    B5P(3);
    struct TriPipelineState *state = &rendering_state.tri_pipeline;
    state->comb = comb;
    state->use_alpha = use_alpha;
    state->used_textures[0] = used_textures[0];
    state->used_textures[1] = used_textures[1];
    state->use_texture = used_textures[0] || used_textures[1];
    state->tex_u_scale = 0.0f;
    state->tex_v_scale = 0.0f;
    state->tex_u_bias = 0.0f;
    state->tex_v_bias = 0.0f;
    if (state->use_texture) {
        const float filter_bias = linear_filter ? 16.0f : 0.0f;
        state->tex_u_scale = 1.0f / (8.0f * (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4));
        state->tex_v_scale = 1.0f / (8.0f * (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4));
        state->tex_u_bias = (filter_bias - rdp.texture_tile.uls * 8.0f) * state->tex_u_scale;
        state->tex_v_bias = (filter_bias - rdp.texture_tile.ult * 8.0f) * state->tex_v_scale;
#if defined(TARGET_PSP)
        {
            const struct TextureHashmapNode *texture_node = used_textures[0] ? rendering_state.textures[0] : rendering_state.textures[1];
            const uint32_t tile_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
            const uint32_t tile_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

            /* The mirror texture is uploaded duplicated ([reflection | original]) and sampled with REPEAT.
             * The exact N64 mirror over that texture is u' = u / (2 * width) + 0.5, that is
             * scale * 0.5 and bias * 0.5 + 0.5. Before this was only done for 32x64 tiles; the others
             * (e.g. 16x16 shadows) came out at double frequency and with no offset. It is applied per axis. */
            (void)tile_width;
            (void)tile_height;
            if (texture_node != NULL) {
                if (texture_node->mirror_s) {
                    state->tex_u_scale *= 0.5f;
                    state->tex_u_bias = state->tex_u_bias * 0.5f + 0.5f;
                }

                if (texture_node->mirror_t) {
                    state->tex_v_scale *= 0.5f;
                    state->tex_v_bias = state->tex_v_bias * 0.5f + 0.5f;
                }
            }
        }
#endif
    }

    B5P(4);
    state->shader_id = prg->shader_id;
#if defined(TARGET_PSP)
    state->fog_mode = (prg->shader_id & SHADER_OPT_FOG) ? scegu_fog_mode(prg->shader_id) : 0;
#else
    state->fog_mode = 0;
#endif
    B5P(5);
    rendering_state.tri_pipeline_dirty = false;
    rendering_state.backend_state_dirty = false;
    if (sProfLogOn) {
        const unsigned int dt = sceKernelGetSystemTimeLow() - pp0;
        const unsigned int ex = (sPcDrawUs + sPcTexUs) - pd0;
        sPcPrepUs += dt > ex ? dt - ex : 0U;
        sPcPrepN++;
    }
}

static inline float dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

static void gfx_normalize_vector(float v[3]) {
    float dot = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if(dot > 0.00001f){
        const float scale = 1.0f / sqrtf(dot);
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
    }
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

/* dir / 127.0f for each possible value of a signed char (same result, bit for bit, as dividing: 3 fewer divisions for each light that is recomputed). */
static float sDir127[256];
static int sDir127Init = 0;
static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    if (__builtin_expect(!sDir127Init, 0)) {
        for (int i = 0; i < 256; i++) {
            sDir127[i] = (float)(signed char)i / 127.0f;
        }
        sDir127Init = 1;
    }
    float light_dir[3] = {
        sDir127[(uint8_t)light->dir[0]],
        sDir127[(uint8_t)light->dir[1]],
        sDir127[(uint8_t)light->dir[2]]
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

#if !defined(TARGET_PSP)
static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}
#else 
static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
  	__asm__ volatile (
        ".set			push\n"					// save assember option
        ".set			noreorder\n"			// suppress reordering
		"lv.q   R000, 0  + %1\n"
		"lv.q   R001, 16 + %1\n"
		"lv.q   R002, 32 + %1\n"
		"lv.q   R003, 48 + %1\n"

		"lv.q   R100, 0  + %2\n"
		"lv.q   R101, 16 + %2\n"
		"lv.q   R102, 32 + %2\n"
		"lv.q   R103, 48 + %2\n"

		"vmmul.q   M700, M000, M100\n"

		"sv.q   R700, 0  + %0\n"
		"sv.q   R701, 16 + %0\n"
		"sv.q   R702, 32 + %0\n"
		"sv.q   R703, 48 + %0\n"
        ".set			pop\n"					// restore assember option
		: "=m" (*res) : "m" (*a) ,"m" (*b) : "memory" );
}
#endif

/* ---- PSP fog ----
 * N64: factor = (z/w) * fog_mul + fog_offset (0..255), with z/w in [-1,1] (depends on the camera's near/far).
 * The PSP GE uses view distances (sceGuFog(near, far)). It is converted with the coefficients A=m[2][2], B=m[3][2]
 * of the loaded perspective: z/w = -A + B/d  =>  d = B / (z/w + A). */
extern float scegu_fog_near, scegu_fog_far; /* definidos en gfx_scegu.c */
static float sFogA = 0.0f, sFogB = 0.0f;
/* The N64 fog depends on z/w of the game's ORIGINAL projection. "Far Distance" lengthens the camera's far plane and with that the same
 * world point has another z/w: the fog stretched (it started farther and reached 100 % at the new plane). With PSP_FOG_FAR_INDEPENDENT the
 * fog is computed with A0/B0 of the projection with the original far: ndc0(d) = -A0 + B0/d, where d = w (distance to the camera). If the scale is 1
 * (the normal case) nothing is touched and the calculation is the usual one (z/w). sFogDA/sFogDB are the coefficients used to go from ndc to distance. */
#ifndef PSP_FOG_FAR_INDEPENDENT
#define PSP_FOG_FAR_INDEPENDENT 1
#endif
extern float gPspPerspFarScaleUsed; /* game/rendering_graph_node.c */
static int sFogScaled = 0;               /* 1 = the loaded projection carries the lengthened far and the fog uses the original */
static float sFogEffNA = 0.0f, sFogEffB = 0.0f; /* -A0 and B0 of the original projection (only if sFogScaled) */
static float sFogDA = 0.0f, sFogDB = 0.0f;      /* A and B to convert ndc -> distance (the original's if sFogScaled) */

/* Called when a perspective projection is loaded. With the far lengthened k times, from A = (n+f)/(n-f) and B = 2nf/(n-f) we get n = B/(A-1) and f = B/(A+1);
 * the original far is f/k and A0 and B0 are rebuilt with it. */
static void gfx_fog_set_projection(float A, float B) {
    sFogA = A; sFogB = B;
    sFogDA = A; sFogDB = B;
    sFogScaled = 0;
#if PSP_FOG_FAR_INDEPENDENT
    {
        const float k = gPspPerspFarScaleUsed;
        if (k > 1.0001f) {
            const float ap1 = A + 1.0f, am1 = A - 1.0f;
            if (fabsf(ap1) > 1.0e-6f && fabsf(am1) > 1.0e-6f) {
                const float n = B / am1;
                const float f0 = (B / ap1) / k;
                if (n > 0.0f && f0 > n * 1.001f) {
                    const float A0 = (n + f0) / (n - f0);
                    const float B0 = 2.0f * n * f0 / (n - f0);
                    sFogEffNA = -A0; sFogEffB = B0;
                    sFogDA = A0; sFogDB = B0;
                    sFogScaled = 1;
                }
            }
        }
    }
#endif
}

static float gfx_fog_ndc_to_dist(float ndc) {
    float den = ndc + sFogDA;
    if (den > -1e-4f) return 1.0e6f;
    float d = sFogDB / den;
    return d < 1.0f ? 1.0f : d;
}

/* ---- Fog clamp planes ----
 * The fog factor F of a vertex is limited to 0..255 and the GE interpolates that limited value in screen space. Unlimited, F is linear in 1/w and
 * that interpolation is exact (1/w is linear on screen); the limit breaks it: a big floor triangle with one vertex before the fog start (F = 0 only
 * because of the limit; its real value is very negative) and another far away got fog from its near vertex on, with hard edges against the
 * neighbors ("wedges" that the real per-pixel fog does not have; measured on a JRB capture: F = 79 instead of 0 on the floor near the camera).
 * Fix: a triangle that crosses the distance where F reaches 0 (or 255) is cut there in clip space (gfx_tri_fog_split). Each piece is then
 * entirely on one side of the limit: its F values are the real ones and the interpolation is exact. */
#ifndef PSP_FOG_CLAMP_SPLIT
#define PSP_FOG_CLAMP_SPLIT 1
#endif
static float sFogWLo = 0.0f, sFogWHi = 0.0f; /* w (distance) where F = 0 and where F = 255 */
static int sFogSplitOn = 0;

static void gfx_update_fog_range(void) {
    float n = 3600.0f, f = 32000.0f;
    sFogSplitOn = 0;
    if (sFogB != 0.0f && rsp.fog_mul != 0) {
        float mul = (float)rsp.fog_mul, off = (float)rsp.fog_offset;
#if PSP_FOG_CLAMP_SPLIT
        if (rsp.fog_mul > 0) {
            const float wlo = gfx_fog_ndc_to_dist((0.0f - off) / mul);
            const float whi = gfx_fog_ndc_to_dist((255.0f - off) / mul);
            if (wlo > 1.0f && wlo < 1.0e5f && whi > wlo) {
                sFogWLo = wlo; sFogWHi = (whi < 1.0e5f) ? whi : 1.0e6f;
                sFogSplitOn = 1;
            }
        }
#endif
        /* N64 fog is hyperbolic in distance (it builds up early) and the GE's is linear.
         * The exact start is kept and the end is chosen so that both coincide when the
         * N64 fog reaches PSP_FOG_MATCH (0..1). Lower = denser/closer fog. */
        const float PSP_FOG_MATCH = 0.5f;
        n = gfx_fog_ndc_to_dist((0.0f - off) / mul);
        float dm = gfx_fog_ndc_to_dist((255.0f * PSP_FOG_MATCH - off) / mul);
        f = n + (dm - n) / PSP_FOG_MATCH;
        if (f < n) { float t = n; n = f; f = t; }
        if (f < n + 1.0f) f = n + 1.0f;
    }
    if (n != scegu_fog_near || f != scegu_fog_far) {
        gfx_flush(); /* what has accumulated is drawn with the previous range */
        scegu_fog_near = n;
        scegu_fog_far = f;
    }
}

/* Base projection (last one loaded with LOAD) and view (product of the later MULs), see gfx_sp_matrix. */
static float sPBase[4][4] __attribute__((aligned(16)));
static float sPView[4][4] __attribute__((aligned(16)));
static int sPInit = 0;
static inline void gfx_pview_init(void) {
    if (!sPInit) {
        memset(sPBase, 0, sizeof(sPBase)); memset(sPView, 0, sizeof(sPView));
        for (int i = 0; i < 4; i++) { sPBase[i][i] = 1.0f; sPView[i][i] = 1.0f; }
        sPInit = 1;
    }
}

#if PSP_EYE_SPACE
/* sXfm[0..15] = model*view*projection (clip, = rsp.MP_matrix), sXfm[16..31] = model*view (eye space, what the GE receives). */
static float sXfm[32] __attribute__((aligned(16)));
static int sXfmMpDirty = 1; /* 1 = rsp.MP_matrix changed since the last copy to sXfm[0..15] */
static int sMVDirty = 1; /* 1 = model*view must be recomputed (the model matrix changed, a pop, or the view) */
/* 1 while the loaded projection is a perspective one (the 3D world). With orthographic or full-screen projections (menus, HUD,
 * transitions, sky) the usual path is used (the GE applies model and view): there are few calls there and the change is not worth it. */
static int sEyeOn = 0;
static void gfx_update_mv(void) {
    if (MTXVAR & 2) {
        gfx_pview_init();
        const float *pa0 = (rsp.modelview_matrix_stack_size > 0) ? &rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1][0][0]
                                                                  : (const float *)identity_matrix;
        const float *pa = pa0;
        const float *pb = &sPView[0][0];
        float *pr = &sXfm[16];
        __asm__("" : "+r"(pa));
        __asm__("" : "+r"(pb));
        __asm__("" : "+r"(pr));
        gfx_matrix_mul((float (*)[4])pr, (const float (*)[4])pa, (const float (*)[4])pb);
        sMVDirty = 0;
        return;
    }
    float a[4][4] __attribute__((aligned(16)));
    float b[4][4] __attribute__((aligned(16)));
    float r[4][4] __attribute__((aligned(16)));
    gfx_pview_init();
    if (rsp.modelview_matrix_stack_size > 0) {
        memcpy(a, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], sizeof(a));
    } else {
        memcpy(a, identity_matrix, sizeof(a));
    }
    memcpy(b, sPView, sizeof(b));
    gfx_matrix_mul(r, a, b);
    memcpy(&sXfm[16], r, sizeof(r));
    sMVDirty = 0;
}
#endif

/* MTXVAR & 1: rsp.MP_matrix = stack[top] * P_matrix is left pending and only computed if someone is going to use it (load vertices) or before
 * the state it depends on changes without recomputing it (gfx_sp_reset, pop that leaves the stack empty). Same value as computing it on every G_MTX. */
static int sMPPend = 0;
static inline void gfx_mp_flush(void) {
    if (sMPPend) {
        sMPPend = 0;
        gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
    }
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    sPcMtx++;
    float matrix[4][4] __attribute__((aligned(16)));
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if (rsp.modelview_matrix_stack_size == 0) {
        /* Empty stack (only possible with a malformed display list): start from an identity matrix so the stack indexes below never go before the array. */
        memcpy(rsp.modelview_matrix_stack[0], identity_matrix, sizeof(matrix));
        rsp.modelview_matrix_stack_size = 1;
    }

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
        /* The GE computes fog with the VIEW z (GU_VIEW matrix). The game puts the camera (lookat) inside the
         * projection with G_MTX_MUL, and with GU_VIEW=identity the fog was computed with world coordinates.
         * They are separated: base = last thing loaded with LOAD (perspective/ortho) -> GU_PROJECTION;
         * view = product of all later MULs (lookat, etc.) -> GU_VIEW. The result is the same product. */
        gfx_pview_init();
#if PSP_SKY_PATCH
        const int skyHit = sRecMtxOn && !(parameters & G_MTX_LOAD) && sSkyInterpPtr != NULL && (const void *) addr == (const void *) sSkyInterpPtr;
        if (skyHit) {
            memcpy(sRecSkyPre, sPView, sizeof(sRecSkyPre));
        }
#endif
        if (parameters & G_MTX_LOAD) {
            memcpy(sPBase, matrix, sizeof(matrix));
            memset(sPView, 0, sizeof(sPView));
            for (int i = 0; i < 4; i++) sPView[i][i] = 1.0f;
        } else {
            /* The VFPU asm of gfx_matrix_mul does not accept static symbols as operands: local copies are used. */
            float tmpA[4][4] __attribute__((aligned(16)));
            float tmpB[4][4] __attribute__((aligned(16)));
            memcpy(tmpA, sPView, sizeof(tmpA));
            gfx_matrix_mul(tmpB, matrix, tmpA);
            memcpy(sPView, tmpB, sizeof(tmpB));
        }
        /* sceGuSetMatrix copies the values as commands into the list, it does not keep the pointer: a local buffer is enough.
         * (Before, memory was requested from the list with sceGuGetMemory, which besides taking up list space makes a system call
         * to notify the GE on every matrix.) */
        float proj_buf[16] __attribute__((aligned(16)));
        float view_buf[16] __attribute__((aligned(16)));
        void *matrix_inline = proj_buf;
        void *view_inline = view_buf;
        memcpy(matrix_inline, sPBase, sizeof(rsp.P_matrix));
        memcpy(view_inline, sPView, sizeof(rsp.P_matrix));
        /* 16:9: the GE draws with this matrix, so the aspect correction is done here.
         * It is decided ONLY when the projection is loaded (G_MTX_LOAD) and kept when multiplying
         * (G_MTX_MUL, e.g. camera/lookat), because after a MUL the [3][3] no longer works as a test.
         *  - Ortografica (m[3][3]==1): menus, textos, power meter, transiciones -> se compensa.
         * - Perspective with ~4:3 aspect (Mario's goddard head) -> compensated.
         * - Perspective with 16:9 aspect (the game world) -> not touched. */
#if PSP_EYE_SPACE
        if (parameters & G_MTX_LOAD) {
            sEyeOn = (matrix[2][3] < -0.5f) ? 1 : 0;
        }
#endif
        if ((parameters & G_MTX_LOAD) && matrix[2][3] < -0.5f) {
            gfx_fog_set_projection(matrix[2][2], matrix[3][2]);
            gfx_update_fog_range();
        }
        if (parameters & G_MTX_LOAD) {
            if (matrix[3][3] > 0.5f) {
                sProjCompensate = 1; gfx_sc_touch();
            } else {
                float asp = (matrix[0][0] != 0.0f) ? (matrix[1][1] / matrix[0][0]) : 0.0f;
                sProjCompensate = (asp > 0.5f && asp < 1.5f); gfx_sc_touch();
            }
        }
        if (sProjCompensate) {
            float *m = (float *)matrix_inline;
            const float k = gfx_aspect_k();
            m[0] *= k; m[4] *= k; m[8] *= k; m[12] *= k; /* X column */
        }
        sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)matrix_inline);
#if PSP_SKY_PATCH
        const unsigned int skyOff = skyHit ? gfx_scegu_list_pos() : 0U;
#endif
#if PSP_EYE_SPACE
        if (sEyeOn) {
            /* the view is already inside the vertices (eye space): the GE uses view = identity (the GE's fog still measures view z) */
            sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)identity_matrix);
            sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity_matrix); /* (in case some other code left it different) */
            sMVDirty = 1;
        } else {
            sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)view_inline);
        }
#else
        sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)view_inline);
#endif
#if PSP_SKY_PATCH
        if (skyHit && (gfx_scegu_list_pos() - skyOff) == 13U * 4U && gfx_scegu_view_block_ok(skyOff, (const float *) view_inline)) {
            sRecSkyOff = skyOff;
            sRecSkyOk = 1;
        }
#endif
        memcpy(scegu_mtx_proj, matrix_inline, 64); /* copies for the noise mask (gfx_scegu.c) */
#if PSP_EYE_SPACE
        if (sEyeOn) {
            memcpy(scegu_mtx_view, identity_matrix, 64);
            memcpy(scegu_mtx_model, identity_matrix, 64);
        } else {
            memcpy(scegu_mtx_view, view_inline, 64);
        }
#else
        memcpy(scegu_mtx_view, view_inline, 64);
#endif
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        /* Local buffer: sceGuSetMatrix copies the values into the list (see above) */
        float model_buf[16] __attribute__((aligned(16)));
        void *matrix_inline = model_buf;
#if PSP_EYE_SPACE
        if (sEyeOn) {
            sMVDirty = 1; /* the model is inside the vertices: the GE keeps model = identity (the copy is not needed) */
        } else
#endif
        {
            memcpy(matrix_inline, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], sizeof(rsp.P_matrix));
            sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)matrix_inline);
            memcpy(scegu_mtx_model, matrix_inline, 64);
        }
        rsp.lights_changed = 1;
    }
    if (MTXVAR & 1) {
        sMPPend = 1;
    } else {
        gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
    }
#if PSP_EYE_SPACE
    sXfmMpDirty = 1;
#endif
}

static void gfx_sp_pop_matrix(uint32_t count) {
    if ((MTXVAR & 1) && rsp.modelview_matrix_stack_size <= count) {
        gfx_mp_flush(); /* the stack is left empty: the MP calculation of the current state would not be repeated */
    }
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
        rsp.lights_changed = 1; /* the lights are kept relative to the current matrix: after a pop it is the parent's again */
        if (MTXVAR & 1) {
            sMPPend = 1;
        } else {
            gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
        }
    }
#if PSP_EYE_SPACE
    sMVDirty = 1;
    sXfmMpDirty = 1;
#endif
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * gfx_aspect_k();
}

/* Second part of gfx_sp_vertex_impl (color/lights/texgen/per-vertex trivial reject). "lighting" and "texgen" arrive as constants in each
 * call (always_inline), so the compiler generates three loops without the per-vertex comparisons (before they were evaluated on each one). */
static inline __attribute__((always_inline)) void gfx_vtx_post(const bool lighting, const bool texgen, const bool one_light, size_t n_vertices,
    size_t dest_index, const Vtx *vertices, const float aspk, const int tsf_s, const int tsf_t, const int nlights,
    const int amb_r, const int amb_g, const int amb_b, const float lc[][3], const int lcol[][3], const float *lk0,
    const float *lk1) {
#if !defined(TARGET_PSP)
    float temp_vec[4] __attribute__((aligned(16)));
    float proj_vec[4] __attribute__((aligned(16)));
#endif
    const float scgx = sScGX, scgy = sScGY;
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];

#if !defined(TARGET_PSP)
        temp_vec[0] = v->ob[0];
        temp_vec[1] = v->ob[1];
        temp_vec[2] = v->ob[2];
        temp_vec[3] = 1.0f;

        __asm__ volatile (
            ".set			push\n"					// save assember option
            ".set			noreorder\n"			// suppress reordering
            "lv.q			c700,  0 + %1\n"		// c700 = MP_matrix->x
            "lv.q			c710, 16 + %1\n"		// c710 = MP_matrix->y
            "lv.q			c720, 32 + %1\n"		// c720 = MP_matrix->z
            "lv.q			c730, 48 + %1\n"		// c730 = MP_matrix->w
            "lv.q			c200, %2\n"				// c200 = *temp_vec
            "vtfm4.q		c000, e700, c200\n"		// c000 = e700 * c200
            "sv.q			c000, %0\n"				// *proj_vec = c000
            ".set			pop\n"					// restore assember option
            : "=m"(*proj_vec)
            : "m"(*rsp.MP_matrix), "m"(*temp_vec)
        );

        const float x = proj_vec[0] * aspk;
        const float y = proj_vec[1];
        const float z = proj_vec[2];
        const float w = proj_vec[3];
#else
        const float x = d->_x * aspk;
        const float y = d->_y;
        const float z = d->_z;
        const float w = d->_w;
#endif

        short U = v->tc[0] * tsf_s >> 16;
        short V = v->tc[1] * tsf_t >> 16;

        if (lighting) {
            const float n0 = vn->n[0], n1 = vn->n[1], n2 = vn->n[2];
            int r = amb_r;
            int g = amb_g;
            int b = amb_b;

            if (one_light) {
                /* case of almost all objects in the game: a single directional light (+ ambient), no loop */
                const float intensity = n0 * lc[0][0] + n1 * lc[0][1] + n2 * lc[0][2];
                if (intensity > 0.0f) {
                    r += (int)(intensity * lcol[0][0]);
                    g += (int)(intensity * lcol[0][1]);
                    b += (int)(intensity * lcol[0][2]);
                }
            } else {
                for (int li = 0; li < nlights; li++) {
                    const float intensity = n0 * lc[li][0] + n1 * lc[li][1] + n2 * lc[li][2];
                    if (intensity > 0.0f) {
                        r += (int)(intensity * lcol[li][0]);
                        g += (int)(intensity * lcol[li][1]);
                        b += (int)(intensity * lcol[li][2]);
                    }
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (texgen) {
                const float dotx = n0 * lk0[0] + n1 * lk0[1] + n2 * lk0[2];
                const float doty = n0 * lk1[0] + n1 * lk1[1] + n2 * lk1[2];

                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * tsf_s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * tsf_t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        uint32_t rej = 0;
        const float wxy = w * sRejXY;
        /* Vertex clearly inside (the most common among the ones that are drawn): 3 comparisons instead of 6. It is equivalent: inside
         * the three ranges <=> no bit; with negative or NaN w this test fails and the usual full calculation is done. */
        /* The "inside" test is done with integers on the float's bits (|x| <= wxy <=> unsigned bits <= wxy's bits when wxy is not
         * negative): 1 single branch and no floating-point comparisons (each c.cond.s + bc1 carries 2 nops and a branch). If it says "inside" it
         * really is (negative w => negative wxy => fails and falls back to the full calculation; with NaN the full calculation gives 0 anyway). */
        int32_t ixb, iyb, izb, iwb, iwxyb;
        __builtin_memcpy(&ixb, &x, 4);
        __builtin_memcpy(&iyb, &y, 4);
        __builtin_memcpy(&izb, &z, 4);
        __builtin_memcpy(&iwb, &w, 4);
        __builtin_memcpy(&iwxyb, &wxy, 4);
        ixb &= 0x7fffffff;
        iyb &= 0x7fffffff;
        izb &= 0x7fffffff;
        if (!((iwxyb >= ixb) & (iwxyb >= iyb) & (iwb >= izb))) {
            if (x < -wxy) rej |= X_POS;
            if (x > wxy) rej |= X_NEG;
            if (y < -wxy) rej |= Y_POS;
            if (y > wxy) rej |= Y_NEG;
            if (z < -w) rej |= Z_POS;
            if (z > w) rej |= Z_NEG;
        }
        /* Scissor pre-test: SC_IN = surely inside all the planes (gfx_tri_core then skips the plane calculation). */
        {
            const float wgx = w * scgx, wgy = w * scgy;
            int32_t igx, igy;
            __builtin_memcpy(&igx, &wgx, 4);
            __builtin_memcpy(&igy, &wgy, 4);
            rej |= (uint32_t)((igx >= ixb) & (igy >= iyb)) << 8;
        }
        d->clip_rej = rej;

#if !defined(TARGET_PSP)
        d->x = v->ob[0];
        d->y = v->ob[1];
        d->z = v->ob[2];
#endif

        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;

        d->color.a = v->cn[3];
    }
}



#if PSP_EYE2_VFPU
/* 2-vertices-per-turn VFPU kernel (gfx_transform_vfpu.s): 1 = checked at this boot (same result, bit for bit, as the usual one
 * for 1, 2, 3, 6 and 7 vertices, without touching anything else), 0 = the usual kernel is used, -1 = not tested yet. */
static int sEye2 = -1;
int gPspEye2Ok = -1; /* read by the log */
static __attribute__((noinline)) int gfx_eye2_selftest(void) {
    static struct LoadedVertex ra[8], rb[8];
    static Vtx tv[8];
    static float tm[32] __attribute__((aligned(16)));
    static const int counts[5] = {1, 2, 3, 6, 7};
    int ok = 1;
    for (int i = 0; i < 32; i++) {
        tm[i] = 0.37f * (float)(i + 1) - 1.9f * (float)(i % 5) + ((i & 1) ? 0.25f : -0.5f);
    }
    for (int k = 0; k < 8; k++) {
        tv[k].v.ob[0] = 1.7f * (float)k - 3.0f;
        tv[k].v.ob[1] = 2.3f - 0.9f * (float)k;
        tv[k].v.ob[2] = 0.31f * (float)(k * k) - 1.0f;
    }
    for (int t = 0; t < 5 && ok; t++) {
        memset(ra, 0xA5, sizeof(ra));
        memset(rb, 0xA5, sizeof(rb));
        gfx_transform_vertices_eye_vfpu(ra, tv, (uint32_t)counts[t], tm);
        gfx_transform_vertices_eye2_vfpu(rb, tv, (uint32_t)counts[t], tm);
        if (memcmp(ra, rb, sizeof(ra)) != 0) {
            ok = 0;
        }
        /* and that it really wrote something different from the filler */
        if (ok && memcmp(&ra[0], &rb[0], sizeof(ra[0])) == 0 && ((const unsigned char *)&ra[0])[0] == 0xA5 &&
            ((const unsigned char *)&ra[0])[1] == 0xA5) {
            ok = 0;
        }
    }
    gPspEye2Ok = ok;
    return ok;
}
#endif

__attribute__((noinline)) static void gfx_sp_vertex_impl(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    sLastVtxSrc = vertices;
    sPcVtx += (unsigned int)n_vertices;
    if (__builtin_expect(sSweepMode >= 7, 0)) {
        return; /* Profile Sweep */
    }
    /* Everything that does not change between vertices is taken out of the loop: since it is written through d->..., the compiler cannot
     * be sure that rsp.* is not modified and it re-read it (and recomputed the divisions) on every vertex. */
    (void)gfx_scissor_planes(); /* brings the sScGX/sScGY bounds used by gfx_vtx_post's scissor pre-test up to date */
    const float aspk = gfx_aspect_k();
    const uint32_t gmode = rsp.geometry_mode;
    const bool lighting = (gmode & G_LIGHTING) != 0;
    const bool texgen = (gmode & G_TEXTURE_GEN) != 0;
    const int tsf_s = rsp.texture_scaling_factor.s;
    const int tsf_t = rsp.texture_scaling_factor.t;
    int nlights = 0;
    int amb_r = 0, amb_g = 0, amb_b = 0;
    float lc[MAX_LIGHTS][3]; /* direction of each light already divided by 127 (before the product was divided per vertex) */
    int lcol[MAX_LIGHTS][3];
    float lk0[3] = {0.0f, 0.0f, 0.0f}, lk1[3] = {0.0f, 0.0f, 0.0f}; /* lookat_x / lookat_y */

    if (lighting) {
        if (rsp.lights_changed) {
            B5S0(b5s_lt);
            for (int li = 0; li < rsp.current_num_lights - 1; li++) {
                calculate_normal_dir(&rsp.current_lights[li], rsp.current_lights_coeffs[li]);
            }
            B5S1(b5s_lt, 1);
            /* lookat_x / lookat_y are only used by texgen (reflections): they are computed the first time they are needed (each one is 3 divisions and
             * a root, and most lit batches do not need them). */
            rsp.lookat_stale = true;
            rsp.lights_changed = false;
        }
        {
            const int amb = (rsp.current_num_lights > 0) ? (int)rsp.current_num_lights - 1 : 0;
            amb_r = rsp.current_lights[amb].col[0];
            amb_g = rsp.current_lights[amb].col[1];
            amb_b = rsp.current_lights[amb].col[2];
        }
        nlights = (int)rsp.current_num_lights - 1;
        if (nlights > MAX_LIGHTS) nlights = MAX_LIGHTS;
        for (int li = 0; li < nlights; li++) {
            lc[li][0] = rsp.current_lights_coeffs[li][0] * (1.0f / 127.0f);
            lc[li][1] = rsp.current_lights_coeffs[li][1] * (1.0f / 127.0f);
            lc[li][2] = rsp.current_lights_coeffs[li][2] * (1.0f / 127.0f);
            lcol[li][0] = rsp.current_lights[li].col[0];
            lcol[li][1] = rsp.current_lights[li].col[1];
            lcol[li][2] = rsp.current_lights[li].col[2];
        }
        if (texgen) {
            if (rsp.lookat_stale) {
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lookat_stale = false;
            }
            lk0[0] = rsp.current_lookat_coeffs[0][0]; lk0[1] = rsp.current_lookat_coeffs[0][1]; lk0[2] = rsp.current_lookat_coeffs[0][2];
            lk1[0] = rsp.current_lookat_coeffs[1][0]; lk1[1] = rsp.current_lookat_coeffs[1][1]; lk1[2] = rsp.current_lookat_coeffs[1][2];
        }
    }

    if (B5ABL(6)) {
        return;
    }
    if (MTXVAR & 1) {
        B5S0(b5s_mp);
        gfx_mp_flush();
        B5S1(b5s_mp, 5);
    }
#if defined(TARGET_PSP)
#if PSP_EYE_SPACE
    if (sEyeOn) {
        if (sMVDirty) {
            B5S0(b5s_mv);
            gfx_update_mv();
            B5S1(b5s_mv, 0);
        }
        if (sXfmMpDirty) {
            B5S0(b5s_xm);
            memcpy(sXfm, rsp.MP_matrix, sizeof(rsp.MP_matrix));
            sXfmMpDirty = 0;
            B5S1(b5s_xm, 2);
        }
#if PSP_EYE2_VFPU
        if (sEye2 < 0) {
            sEye2 = gfx_eye2_selftest();
        }
        if (sEye2) {
            gfx_transform_vertices_eye2_vfpu(&rsp.loaded_vertices[dest_index], vertices, n_vertices, sXfm);
        } else
#endif
        gfx_transform_vertices_eye_vfpu(&rsp.loaded_vertices[dest_index], vertices, n_vertices, sXfm);
    } else
#endif
    {
        gfx_transform_vertices_vfpu(&rsp.loaded_vertices[dest_index], vertices, n_vertices,
                                    rsp.MP_matrix);
    }
#endif
    if (__builtin_expect(sSweepMode >= 6, 0)) {
        return; /* Profile Sweep: transformation only */
    }
    if (!lighting) {
        gfx_vtx_post(false, false, false, n_vertices, dest_index, vertices, aspk, tsf_s, tsf_t, nlights, amb_r, amb_g, amb_b, lc, lcol, lk0, lk1);
    } else if (!texgen) {
        if (nlights == 1) {
            gfx_vtx_post(true, false, true, n_vertices, dest_index, vertices, aspk, tsf_s, tsf_t, nlights, amb_r, amb_g, amb_b, lc, lcol, lk0, lk1);
        } else {
            gfx_vtx_post(true, false, false, n_vertices, dest_index, vertices, aspk, tsf_s, tsf_t, nlights, amb_r, amb_g, amb_b, lc, lcol, lk0, lk1);
        }
    } else {
        if (nlights == 1) {
            gfx_vtx_post(true, true, true, n_vertices, dest_index, vertices, aspk, tsf_s, tsf_t, nlights, amb_r, amb_g, amb_b, lc, lcol, lk0, lk1);
        } else {
            gfx_vtx_post(true, true, false, n_vertices, dest_index, vertices, aspk, tsf_s, tsf_t, nlights, amb_r, amb_g, amb_b, lc, lcol, lk0, lk1);
        }
    }
}

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
#if PSP_DEVBENCH == 5
    if (sB5On) {
        const unsigned int b5v0 = B5C();
        gfx_sp_vertex_impl(n_vertices, dest_index, vertices);
        sB5Acc[0] += B5C() - b5v0;
        sB5Cnt[0] += (unsigned int) n_vertices;
        sB5Cnt[1]++;
        return;
    }
#endif
    if (sProfLogOn) {
        const unsigned int tv0 = sceKernelGetSystemTimeLow();
        gfx_sp_vertex_impl(n_vertices, dest_index, vertices);
        sPcVtxUs += sceKernelGetSystemTimeLow() - tv0;
    } else {
        gfx_sp_vertex_impl(n_vertices, dest_index, vertices);
    }
}

/* Subdivision for per-pixel LOD (Peach/Bowser frame): the GE interpolates color/alpha in screen
 * space (without perspective correction), but the N64's LOD is per pixel (~ perspective-correct w).
 * By subdividing the triangle and computing the exact LOD at each new vertex the error stays below
 * de un pixel. */
#define LOD_SUBDIV 8

static void gfx_tri_core(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3, int sub, float sub_lodk);

/* ---- Infrequent parts of gfx_tri_core, pulled out into separate functions (noinline) ----
 * gfx_tri_core took ~11.8 KB of code (the PSP's instruction cache is 16 KB) and the normal path (a triangle that is
 * drawn without clipping, without LOD or 2-pass fog) went through all that code interleaved with clipping, the subdivision for
 * Peach/Bowser's LOD and the caps/Boo state. Same calculation, only the place of the code changes. */

/* CPU clipping: the clipped vertices go to these tables (static: a recursive call of gfx_tri_core only happens after
 * the parent call has finished using them, see the LOD subdivision). */
#if defined(TARGET_PSP) && PSP_SCRATCH
#define sTriClipVerts ((struct LoadedVertex *) (PSP_SCRATCH_BASE + PSP_SCRATCH_CLIP_OFF))
#else
static struct LoadedVertex sTriClipVerts[54];
#endif
static struct LoadedVertex *sTriClipPtrs[54];

static __attribute__((noinline)) size_t gfx_tri_clip_cold(struct LoadedVertex *v_arr[3], bool do_frustum, int sc_np, uint32_t need) {
    size_t n = 0;
    size_t i;
    sPcClip++;
    sTriClass = 2;
    if (sProfLogOn) {
        const unsigned int tc0 = sceKernelGetSystemTimeLow();
        gfx_clip_single_vert(sTriClipVerts, &n, v_arr, do_frustum, sc_np, need);
        sPcClipUs += sceKernelGetSystemTimeLow() - tc0;
    } else {
        gfx_clip_single_vert(sTriClipVerts, &n, v_arr, do_frustum, sc_np, need);
    }
    for (i = 0; i < n; i++) {
        sTriClipPtrs[i] = &sTriClipVerts[i];
    }
    return n;
}

/* PSP_F2_REPLAY_MTX: does this object have triangles drawn very close to the near plane? (see gfx_psp_replay_prepare) */
static __attribute__((noinline)) void gfx_tri_note_near(const struct LoadedVertex *v1, const struct LoadedVertex *v2,
                                                        const struct LoadedVertex *v3) {
    /* It only counts if the vertex near the near plane also falls inside (with margin) the screen: a floor cut by the near plane has
     * those vertices well below the camera, out of view, and it leaves no gaps or unclipped triangles when moving. */
    const struct LoadedVertex *tv[3] = { v1, v2, v3 };
    int q;
    for (q = 0; q < 3; q++) {
        const float qw = tv[q]->_w;
        if (qw < sRecNearBand && qw > 0.0f &&
            fabsf(tv[q]->_x) <= qw * 1.3f && fabsf(tv[q]->_y) <= qw * 1.3f) {
            sRecNear[sRecCur] = 1;
            break;
        }
    }
}

/* Patches with alpha = ENV (caps, Mario, Boo fading out): returns enva_white (see gfx_get_vertex_color) */
static __attribute__((noinline)) bool gfx_tri_enva_cold(const struct ColorCombiner *comb, bool use_alpha, bool use_texture) {
    bool enva_white = false;
    if (use_alpha && comb->env_alpha && !use_texture) {
        /* color pass (SHADEFADEA): remember the vertex array */
        bool known = false;
        for (int r = 0; r < 8; r++) {
            if (sEnvaVtxRing[r] == sLastVtxSrc) {
                known = true;
            }
        }
        if (!known) {
            sEnvaVtxRing[sEnvaVtxRingPos] = sLastVtxSrc;
            sEnvaVtxRingPos = (sEnvaVtxRingPos + 1) & 7;
        }
    }
    if (comb->enva_tex) {
        for (int r = 0; r < 8; r++) {
            if (sEnvaVtxRing[r] == sLastVtxSrc && sLastVtxSrc != NULL) {
                enva_white = true;
            }
        }
        if (buf_vbo_len == 0) {
            scegu_enva_modulate = enva_white ? 1 : 0; /* decision per buffer (one buffer = one pass of a patch) */
        }
        enva_white = scegu_enva_modulate != 0;
    }
    return enva_white;
}

/* Exact N64 LOD (Peach/Bowser frame): texels/pixel ratio from the on-screen texture derivatives
 * (in N64 pixels: 120 px per NDC unit in Y, and the same square pixel size in X).
 * It is computed once per triangle and scaled by w at each vertex (on a plane, ratio ~ w).
 * Returns true if the triangle was subdivided and drawn here (gfx_tri_core must finish); if not, *lod_out is left for the normal case. */
static __attribute__((noinline)) bool gfx_tri_lod_cold(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3,
                                                        float *lod_out) {
    float lod_k = *lod_out;
    const float w1 = v1->_w, w2 = v2->_w, w3 = v3->_w;
    if (w1 > 0.01f && w2 > 0.01f && w3 > 0.01f) {
        /* _x already comes multiplied by gfx_adjust_x_for_aspect_ratio (= (4/3)/aspect), but the GE draws with the
         * projection WITHOUT that adjustment (it is already 16:9 through guPerspective). N64 pixels per unit of _x/_w:
         * 120 * aspect (square pixel at N64 vertical scale) divided by that factor = 90 * aspect^2. */
        const float aspect = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
        const float px = 90.0f * aspect * aspect;
        const float py = 120.0f;
        const float x1 = v1->_x / w1 * px, y1 = v1->_y / w1 * py;
        const float e1x = v2->_x / w2 * px - x1, e1y = v2->_y / w2 * py - y1;
        const float e2x = v3->_x / w3 * px - x1, e2y = v3->_y / w3 * py - y1;
        const float det = e1x * e2y - e1y * e2x;
        if (fabsf(det) > 1e-6f) {
            const float inv = 1.0f / det;
            const float ds1 = (v2->u - v1->u) / 32.0f, ds2 = (v3->u - v1->u) / 32.0f;
            const float dt1 = (v2->v - v1->v) / 32.0f, dt2 = (v3->v - v1->v) / 32.0f;
            float r = fabsf((ds1 * e2y - ds2 * e1y) * inv);
            float t;
            t = fabsf((ds2 * e1x - ds1 * e2x) * inv); if (t > r) r = t;
            t = fabsf((dt1 * e2y - dt2 * e1y) * inv); if (t > r) r = t;
            t = fabsf((dt2 * e1x - dt1 * e2x) * inv); if (t > r) r = t;
            lod_k = r / ((w1 + w2 + w3) * (1.0f / 3.0f));
        }
    }
    if (lod_k > 0.0f) {
        /* Subdivide into LOD_SUBDIV^2 triangles with exact LOD per vertex */
        struct LoadedVertex sv[(LOD_SUBDIV + 1) * (LOD_SUBDIV + 2) / 2];
        int base[LOD_SUBDIV + 1];
        int cnt = 0;
        for (int j = 0; j <= LOD_SUBDIV; j++) {
            base[j] = cnt;
            for (int i = 0; i <= LOD_SUBDIV - j; i++) {
                struct LoadedVertex *o = &sv[cnt++];
                const float b = (float)j / LOD_SUBDIV;
                const float a = (float)i / LOD_SUBDIV;
                struct LoadedVertex q;
                const float t = (j < LOD_SUBDIV) ? a / (1.0f - b) : 0.0f;
                gfx_clip_interpolate_vert(&q, v1, v2, t);
                gfx_clip_interpolate_vert(o, &q, v3, b);
                o->w = v1->w;
                o->clip_rej = 0;
                if (o->_x < -o->_w * sRejXY) o->clip_rej |= X_POS;
                if (o->_x > o->_w * sRejXY) o->clip_rej |= X_NEG;
                if (o->_y < -o->_w * sRejXY) o->clip_rej |= Y_POS;
                if (o->_y > o->_w * sRejXY) o->clip_rej |= Y_NEG;
                if (o->_z < -o->_w) o->clip_rej |= Z_POS;
                if (o->_z > o->_w) o->clip_rej |= Z_NEG;
            }
        }
        for (int j = 0; j < LOD_SUBDIV; j++) {
            for (int i = 0; i < LOD_SUBDIV - j; i++) {
                struct LoadedVertex *p00 = &sv[base[j] + i];
                struct LoadedVertex *p10 = &sv[base[j] + i + 1];
                struct LoadedVertex *p01 = &sv[base[j + 1] + i];
                gfx_tri_core(p00, p10, p01, 1, lod_k);
                if (i < LOD_SUBDIV - j - 1) {
                    struct LoadedVertex *p11 = &sv[base[j + 1] + i + 1];
                    gfx_tri_core(p10, p11, p01, 1, lod_k);
                }
            }
        }
        return true;
    }
    *lod_out = lod_k;
    return false;
}

#if PSP_FOG_CLAMP_SPLIT
/* Cuts the triangle with the plane w = wp (w = clip-space w = distance) into the part with w > wp and the part with w < wp, and hands each piece to
 * the next plane (and finally to gfx_tri_core). The cut vertices are interpolated in clip space (exact) and get w = wp exactly. */
static void gfx_fog_split_rec(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int plane) {
    if (plane >= 2) {
        gfx_tri_core(a, b, c, 1, 0.0f);
        return;
    }
    const float wp = plane == 0 ? sFogWLo : sFogWHi;
    struct LoadedVertex *vin[3] = { a, b, c };
    const float d[3] = { a->_w - wp, b->_w - wp, c->_w - wp };
    if (!((d[0] > 0.0f || d[1] > 0.0f || d[2] > 0.0f) && (d[0] < 0.0f || d[1] < 0.0f || d[2] < 0.0f))) {
        gfx_fog_split_rec(a, b, c, plane + 1); /* does not cross this plane */
        return;
    }
    struct LoadedVertex cut[2];
    struct LoadedVertex *pos[4], *neg[4];
    int np = 0, nn = 0, nc = 0;
    for (int i = 0; i < 3; i++) {
        struct LoadedVertex *cur = vin[i], *nxt = vin[(i + 1) % 3];
        const float dc = d[i], dn = d[(i + 1) % 3];
        if (dc > 0.0f) {
            pos[np++] = cur;
        } else if (dc < 0.0f) {
            neg[nn++] = cur;
        } else {
            pos[np++] = cur; neg[nn++] = cur;
        }
        if ((dc > 0.0f && dn < 0.0f) || (dc < 0.0f && dn > 0.0f)) {
            struct LoadedVertex *o = &cut[nc++];
            gfx_clip_interpolate_vert(o, cur, nxt, dc / (dc - dn));
            o->_w = wp;
            o->w = a->w;
            o->clip_rej = 0;
            if (o->_x < -o->_w * sRejXY) o->clip_rej |= X_POS;
            if (o->_x > o->_w * sRejXY) o->clip_rej |= X_NEG;
            if (o->_y < -o->_w * sRejXY) o->clip_rej |= Y_POS;
            if (o->_y > o->_w * sRejXY) o->clip_rej |= Y_NEG;
            if (o->_z < -o->_w) o->clip_rej |= Z_POS;
            if (o->_z > o->_w) o->clip_rej |= Z_NEG;
            pos[np++] = o; neg[nn++] = o;
        }
    }
    for (int k = 1; k + 1 < np; k++) gfx_fog_split_rec(pos[0], pos[k], pos[k + 1], plane + 1);
    for (int k = 1; k + 1 < nn; k++) gfx_fog_split_rec(neg[0], neg[k], neg[k + 1], plane + 1);
}

static __attribute__((noinline)) void gfx_tri_fog_split(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3) {
    if (buf_vbo_num_tris + 24 >= MAX_BUFFERED) {
        gfx_flush(); /* the pieces (and their clipping) need room in the buffer */
    }
    gfx_fog_split_rec(v1, v2, v3, 0);
}
#endif

/* Early discard of a triangle (all off screen or back-facing). It is the 1st part of gfx_tri_core, pulled out into an inline function:
 * this way the ~70 % of triangles that are discarded do not pay for the gfx_tri_core call (big stack, many saved registers). */
static inline __attribute__((always_inline)) bool gfx_tri_culled(const struct LoadedVertex *v1, const struct LoadedVertex *v2,
                                                                  const struct LoadedVertex *v3) {
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & CLIP_TEST_FLAGS) {
        // The whole triangle lies outside the visible area
        return true;
    }
    const uint32_t cull_mode = rsp.geometry_mode & G_CULL_BOTH;
    if (cull_mode != 0) {
        /* Sign of the on-screen area WITHOUT dividing by w. With dx1 = x1/w1 - x2/w2 etc.:
         * cross = (a*b - c*d) / (w1*w3*w2^2), and since w2^2 > 0 the sign is that of (a*b - c*d) times that of w1*w3.
         * The original also inverts the sign if there is an odd number of negative w; combining both inversions
         * leaves only "invert if w2 < 0". */
        const float w1 = v1->_w, w2 = v2->_w, w3 = v3->_w;
        const float a = v1->_x * w2 - v2->_x * w1;
        const float b = v3->_y * w2 - v2->_y * w3;
        const float c = v1->_y * w2 - v2->_y * w1;
        const float d = v3->_x * w2 - v2->_x * w3;
        float cross = a * b - c * d;
        if (w2 < 0.0f) {
            cross = -cross;
        }
        /* Recording (rejXY > 1): only what is clearly back-facing is discarded. sin(angle)^2 = cross^2 / (|e1|^2 |e2|^2); the w's cancel out. */
        const bool keep_edge_on = (sRejXY > 1.0f) && cross != 0.0f &&
                                  (cross * cross <= PSP_REC_CULL_SIN2 * ((a * a + c * c) * (b * b + d * d)));

        switch (cull_mode) {
            case G_CULL_FRONT:
                if (cross <= 0 && !keep_edge_on) return true;
                break;
            case G_CULL_BACK:
                if (cross >= 0 && !keep_edge_on) return true;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return true;
        }
    }
    return false;
}

static int sTriPrechecked; /* 1 = gfx_sp_tri1 already applied gfx_tri_culled to the next gfx_tri_core call */



static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    sPcTriSub++;
    if (__builtin_expect(sSweepMode >= 5, 0)) {
        return; /* Profile Sweep */
    }
    if (sProfLogOn && ((++sTriSampleCtr) & 7u) == 0u) {
        /* sampling: 1 in every 8 triangles is timed (not counting drawing or textures, which are already measured separately) */
        const unsigned int ta = sceKernelGetSystemTimeLow(), d0 = sPcDrawUs, x0 = sPcTexUs;
        gfx_tri_core(&rsp.loaded_vertices[vtx1_idx], &rsp.loaded_vertices[vtx2_idx], &rsp.loaded_vertices[vtx3_idx], 0, 0.0f);
        {
            const unsigned int dt = sceKernelGetSystemTimeLow() - ta;
            const unsigned int ex = (sPcDrawUs - d0) + (sPcTexUs - x0);
            sPcTriUs += dt > ex ? dt - ex : 0;
            sPcTriSmp++;
            sTriClassUs[sTriClass] += dt > ex ? dt - ex : 0;
            sTriClassN[sTriClass]++;
        }
    } else {
        struct LoadedVertex *const t1 = &rsp.loaded_vertices[vtx1_idx];
        struct LoadedVertex *const t2 = &rsp.loaded_vertices[vtx2_idx];
        struct LoadedVertex *const t3 = &rsp.loaded_vertices[vtx3_idx];
#if PSP_DEVBENCH == 5
        if (sB5On) {
            const unsigned int b5a = B5C();
            if (gfx_tri_culled(t1, t2, t3)) {
                sB5Acc[2] += B5C() - b5a;
                sB5Cnt[2]++;
                return;
            }
            {
                const unsigned int b5b = B5C();
                sB5Acc[3] += b5b - b5a; /* discard of the ones that are drawn */
                sTriPrechecked = 1;
                sB5Mark[1] = sB5Mark[2] = sB5Mark[3] = 0;
                gfx_tri_core(t1, t2, t3, 0, 0.0f);
                {
                    const unsigned int b5e = B5C();
                    if (sB5Mark[3]) {
                        sB5Acc[4] += sB5Mark[1] - b5b;         /* prelude: scissor / clipping */
                        sB5Acc[5] += sB5Mark[2] - sB5Mark[1];  /* prepare_tri_pipeline_state + enva + lod */
                        sB5Acc[6] += sB5Mark[3] - sB5Mark[2];  /* output loop (vertices to the buffer) */
                        sB5Acc[7] += b5e - sB5Mark[3];         /* tail (counters, possible flush) */
                        sB5Cnt[3]++;
                    } else {
                        sB5Acc[8] += b5e - b5b;                /* early exit inside gfx_tri_core */
                        sB5Cnt[4]++;
                    }
                }
            }
            return;
        }
#endif
        if (gfx_tri_culled(t1, t2, t3)) {
            return;
        }
#if PSP_DEVBENCH == 5
        if (sB5Cull) { sB5Cnt[3]++; return; }
        if (sB5Cap) b5_micro_capture(t1, t2, t3);
#endif
        sTriPrechecked = 1; /* gfx_tri_core does not repeat the discard (the signed area calculation used to be done twice per drawn triangle) */
        gfx_tri_core(t1, t2, t3, 0, 0.0f);
    }
}

/* Variant of gfx_tri_core's output (bits): 1 = fog mode / shader id already computed when preparing the state, 2 = new output loop
 * (loads everything from the vertex before writing, one loop per case, fog with integers), 4 = fog of the 3 vertices at once (with 2). */
#ifndef PSP_TRI_VAR
#define PSP_TRI_VAR 3 /* measured on the real PSP: ~0.9 ms less per frame in the gardens scene; 0 = the usual output */
#endif

/* Exact division by 255 of an integer 0..65025 (the result of c*inv): (p + 1 + (p >> 8)) >> 8 == p / 255 for all p in 0..65534. */
static inline __attribute__((always_inline)) unsigned int gfx_div255(unsigned int p) {
    return (p + 1u + (p >> 8)) >> 8;
}

/* Fog factor of a vertex: same as before (z/w * mul + offset, limited to 0..255), but the limit is done with integers. Only if |f| < 2^29
 * (always, except vertices with w almost 0): outside that range the conversion to integer is not defined and the usual limit is used. */
static inline __attribute__((always_inline)) unsigned int gfx_fog_clamp(float f) {
    int32_t fb;
    __builtin_memcpy(&fb, &f, 4);
    if (__builtin_expect((fb & 0x7fffffff) < 0x4e000000, 1)) {
        int fi = (int)f;
        fi = fi < 0 ? 0 : fi;
        fi = fi > 255 ? 255 : fi;
        return (unsigned int)fi;
    }
    return (f <= 0.0f) ? 0u : ((f >= 255.0f) ? 255u : (unsigned int)(unsigned char)f);
}

static inline __attribute__((always_inline)) unsigned int gfx_fog_fa(float z, float w, float fm, float fo) {
    if (fabsf(w) < 0.001f) w = 0.001f;
    if (__builtin_expect(sFogScaled, 0)) {
        return gfx_fog_clamp((sFogEffNA + sFogEffB / w) * fm + fo); /* z/w of the original projection (see sFogScaled) */
    }
    return gfx_fog_clamp(z / w * fm + fo);
}

static inline __attribute__((always_inline)) unsigned int gfx_fog_scale_color(unsigned int col, unsigned int fa) {
    const unsigned int inv = 255u - fa;
    const unsigned int r = gfx_div255((col & 0xffu) * inv);
    const unsigned int g = gfx_div255(((col >> 8) & 0xffu) * inv);
    const unsigned int b = gfx_div255(((col >> 16) & 0xffu) * inv);
    return (col & 0xff000000u) | r | (g << 8) | (b << 16);
}

/* New output of the n vertices (n multiple of 3) of the triangle to the buffer: each vertex is read whole before writing (the writes to the buffer
 * cannot be told apart from the vertex fields by the compiler and forced re-reads), and one loop per case. */
static inline __attribute__((always_inline)) void gfx_tri_emit_fast(struct LoadedVertex **cv, const size_t n, const bool use_texture,
        const bool shade_only, const int fog_mode, const int var, const struct ColorCombiner *comb, const bool use_alpha, const float lod_k,
        const float su, const float sv, const float bu, const float bv) {
    psp_fast_t *out = &buf_vbo[buf_num_vert];
    size_t i;
    if (shade_only) {
        if (use_texture) {
            for (i = 0; i < n; i++) {
                const struct LoadedVertex *vx = cv[i];
                const float x = vx->x, y = vx->y, z = vx->z, tu = vx->u, tv = vx->v;
                unsigned int col;
                __builtin_memcpy(&col, &vx->color, 4);
                out[i].x = x; out[i].y = y; out[i].z = z;
                out[i].u = tu * su + bu;
                out[i].v = tv * sv + bv;
                __builtin_memcpy(&out[i].color, &col, 4);
            }
        } else {
            for (i = 0; i < n; i++) {
                const struct LoadedVertex *vx = cv[i];
                const float x = vx->x, y = vx->y, z = vx->z;
                unsigned int col;
                __builtin_memcpy(&col, &vx->color, 4);
                out[i].x = x; out[i].y = y; out[i].z = z;
                out[i].u = 0.0f;
                out[i].v = 0.0f;
                __builtin_memcpy(&out[i].color, &col, 4);
            }
        }
    } else {
        for (i = 0; i < n; i++) {
            const struct LoadedVertex *vx = cv[i];
            const float x = vx->x, y = vx->y, z = vx->z;
            float tu = 0.0f, tv = 0.0f;
            if (use_texture) {
                tu = vx->u * su + bu;
                tv = vx->v * sv + bv;
            }
            const struct RGBA c = gfx_get_vertex_color(comb, use_alpha, &vx->color, lod_k * vx->_w, true); /* per-vertex LOD */
            out[i].x = x; out[i].y = y; out[i].z = z;
            out[i].u = tu;
            out[i].v = tv;
            out[i].color = c;
        }
    }
    if (fog_mode) {
        const float fm = (float)rsp.fog_mul, fo = (float)rsp.fog_offset;
        unsigned char *fao = &scegu_fog_alpha[buf_num_vert];
        if ((var & 4) && !sFogScaled) {
            for (i = 0; i + 3 <= n; i += 3) {
                const struct LoadedVertex *a = cv[i], *b = cv[i + 1], *c = cv[i + 2];
                float wa = a->_w, wb = b->_w, wc = c->_w;
                const float za = a->_z, zb = b->_z, zc = c->_z;
                if (fabsf(wa) < 0.001f) wa = 0.001f;
                if (fabsf(wb) < 0.001f) wb = 0.001f;
                if (fabsf(wc) < 0.001f) wc = 0.001f;
                const float qa = za / wa, qb = zb / wb, qc = zc / wc;
                const float fa_ = qa * fm + fo, fb_ = qb * fm + fo, fc_ = qc * fm + fo;
                int32_t ba, bb, bc;
                __builtin_memcpy(&ba, &fa_, 4); __builtin_memcpy(&bb, &fb_, 4); __builtin_memcpy(&bc, &fc_, 4);
                int ia, ib, ic;
                if (__builtin_expect((((ba & 0x7fffffff) < 0x4e000000) & ((bb & 0x7fffffff) < 0x4e000000) & ((bc & 0x7fffffff) < 0x4e000000)), 1)) {
                    ia = (int)fa_; ib = (int)fb_; ic = (int)fc_;
                    ia = ia < 0 ? 0 : ia; ib = ib < 0 ? 0 : ib; ic = ic < 0 ? 0 : ic;
                    ia = ia > 255 ? 255 : ia; ib = ib > 255 ? 255 : ib; ic = ic > 255 ? 255 : ic;
                } else {
                    ia = (int)gfx_fog_clamp(fa_); ib = (int)gfx_fog_clamp(fb_); ic = (int)gfx_fog_clamp(fc_);
                }
                fao[i] = (unsigned char)ia; fao[i + 1] = (unsigned char)ib; fao[i + 2] = (unsigned char)ic;
                if (fog_mode == 3) {
                    unsigned int ca, cb, cc;
                    __builtin_memcpy(&ca, &out[i].color, 4);
                    __builtin_memcpy(&cb, &out[i + 1].color, 4);
                    __builtin_memcpy(&cc, &out[i + 2].color, 4);
                    ca = gfx_fog_scale_color(ca, (unsigned int)ia);
                    cb = gfx_fog_scale_color(cb, (unsigned int)ib);
                    cc = gfx_fog_scale_color(cc, (unsigned int)ic);
                    __builtin_memcpy(&out[i].color, &ca, 4);
                    __builtin_memcpy(&out[i + 1].color, &cb, 4);
                    __builtin_memcpy(&out[i + 2].color, &cc, 4);
                }
            }
        } else {
            for (i = 0; i < n; i++) {
                const unsigned int fa = gfx_fog_fa(cv[i]->_z, cv[i]->_w, fm, fo);
                fao[i] = (unsigned char)fa;
                if (fog_mode == 3) {
                    unsigned int col;
                    __builtin_memcpy(&col, &out[i].color, 4);
                    col = gfx_fog_scale_color(col, fa);
                    __builtin_memcpy(&out[i].color, &col, 4);
                }
            }
        }
    }
    buf_num_vert += n;
    buf_vbo_len += n * sizeof(psp_fast_t);
}

static inline __attribute__((always_inline)) void gfx_tri_core_body(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3, int sub, float sub_lodk, const int VAR) {
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};
    const uint32_t clip_flags = v1->clip_rej | v2->clip_rej | v3->clip_rej;

    sTriClass = 0;

    if (sTriPrechecked) {
        sTriPrechecked = 0;
    } else if (gfx_tri_culled(v1, v2, v3)) {
        return;
    }

    /* Setup to clip but if we dont, we preload correct values and fix up pointers; */
    struct LoadedVertex **clipped_vertices = v_arr;
    size_t clipped_vertices_num = 3;

    /* RDP scissor: if the triangle crosses an "interior" scissor edge it must be clipped */
    const int sc_np = B5ABL(4) ? 0 : gfx_scissor_planes();
    B5H(6, sc_np > 0);
    bool need_sc = false;
    if (sc_np > 0 && !(v1->clip_rej & v2->clip_rej & v3->clip_rej & SC_IN)) {
        for (int p = 0; p < sc_np && !need_sc; p++) {
            /* The scissor planes are axis-aligned (a*x + d*w  or  b*y + d*w): only what is not zero is computed. Same value as
             * vec4_dot with the other terms at 0, but 2 products per vertex instead of 4. */
            int outside;
            const float *pl = sc_planes[p];
            const float dw = pl[3];
            if (sc_ptype[p] == 0) {
                const float a = pl[0];
                outside = (a * v1->_x + dw * v1->_w > 0.0f) + (a * v2->_x + dw * v2->_w > 0.0f) + (a * v3->_x + dw * v3->_w > 0.0f);
            } else {
                const float b = pl[1];
                outside = (b * v1->_y + dw * v1->_w > 0.0f) + (b * v2->_y + dw * v2->_w > 0.0f) + (b * v3->_y + dw * v3->_w > 0.0f);
            }
            if (outside == 3) {
                return; /* completely outside the scissor */
            }
            if (outside > 0) {
                need_sc = true;
            }
        }
    }

    B5H(7, need_sc);
    uint32_t clip_test = clip_flags & CLIP_TEST_FLAGS;
#if PSP_GE_XY_CLIP
    if (clip_test != 0 && (clip_test & (Z_POS | Z_NEG)) == 0) {
        const float g = PSP_GE_XY_GUARD;
        const float w1 = v1->_w, w2 = v2->_w, w3 = v3->_w;
        if (w1 > 0.0f && w2 > 0.0f && w3 > 0.0f &&
            fabsf(v1->_x) <= g * w1 && fabsf(v1->_y) <= g * w1 &&
            fabsf(v2->_x) <= g * w2 && fabsf(v2->_y) <= g * w2 &&
            fabsf(v3->_x) <= g * w3 && fabsf(v3->_y) <= g * w3) {
            clip_test = 0;
            sPcClipSkip++;
        }
    }
#endif
    B5H(8, clip_test != 0);
    if (clip_test || need_sc) {
        clipped_vertices_num = gfx_tri_clip_cold(v_arr, clip_test != 0, need_sc ? sc_np : 0, clip_test);
        if (!clipped_vertices_num) {
            /* No idea if this is possible */
            return;
        }
        clipped_vertices = sTriClipPtrs;
    }

    if (PSP_GPU_PROF && gPspGpuProf && !sub) {
        /* approximate on-screen area (GPU profile only): NDC clamped to [-1,1], 480x272 pixels */
        const float aw1 = v1->_w, aw2 = v2->_w, aw3 = v3->_w;
        if (aw1 > 0.01f && aw2 > 0.01f && aw3 > 0.01f) {
            const float kx = 1.0f / gfx_aspect_k();
            float ax = v1->_x / aw1 * kx, ay = v1->_y / aw1;
            float bx = v2->_x / aw2 * kx, by = v2->_y / aw2;
            float cx = v3->_x / aw3 * kx, cy = v3->_y / aw3;
#define PSP_NDC_CLAMP(q) ((q) < -1.0f ? -1.0f : ((q) > 1.0f ? 1.0f : (q)))
            ax = PSP_NDC_CLAMP(ax); ay = PSP_NDC_CLAMP(ay); bx = PSP_NDC_CLAMP(bx); by = PSP_NDC_CLAMP(by);
            cx = PSP_NDC_CLAMP(cx); cy = PSP_NDC_CLAMP(cy);
#undef PSP_NDC_CLAMP
            const float cr = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
            gPcAreaAcc += 0.5f * fabsf(cr) * (480.0f * 0.5f) * (272.0f * 0.5f);
        }
    }
    if (sTriClass == 0) sTriClass = 1;
    if (__builtin_expect(sRecCur >= 0, 0)) {
        gfx_tri_note_near(v1, v2, v3);
    }
    if (__builtin_expect(sSweepMode >= 4, 0)) {
        return; /* Profile Sweep */
    }
    B5MARK(1);
    if (!B5ABL(5)) gfx_prepare_tri_pipeline_state();
#if defined(TARGET_PSP) && PSP_DECAL_ADAPTIVE
    if (rendering_state.decal_mode && !sub) {
        const int bias = gfx_decal_bias(v1, v2, v3);
        if (bias != sDecalBias) {
            gfx_flush(); /* what has accumulated is drawn with the previous offset */
            gfx_scegu_set_decal_bias(bias);
            sDecalBias = bias;
        }
    }
#endif
    const struct TriPipelineState *state = &rendering_state.tri_pipeline;
    struct ColorCombiner *comb = state->comb;
    const bool use_alpha = state->use_alpha;
    const bool use_texture = state->use_texture;
    const float tex_u_scale = state->tex_u_scale;
    const float tex_v_scale = state->tex_v_scale;
    const float tex_u_bias = state->tex_u_bias;
    const float tex_v_bias = state->tex_v_bias;
    bool enva_white = false;
    if (__builtin_expect((use_alpha && comb->env_alpha && !use_texture) || comb->enva_tex, 0)) {
        enva_white = gfx_tri_enva_cold(comb, use_alpha, use_texture);
    }
    sEnvaWhite = enva_white;
    const uint32_t shader_program_id = (VAR & 1) ? state->shader_id : rendering_state.shader_program->shader_id;
    const int fog_mode = (VAR & 1) ? state->fog_mode : (((shader_program_id & SHADER_OPT_FOG) && !B5ABL(3)) ? scegu_fog_mode(shader_program_id) : 0);
#if PSP_DEVBENCH == 5
    sB5H[fog_mode & 3]++;
#endif

    float lod_k = sub_lodk;
    if (__builtin_expect(!sub && (comb->vertex_color_source[0] == CC_LOD || comb->vertex_color_source[1] == CC_LOD), 0)) {
        if (gfx_tri_lod_cold(v1, v2, v3, &lod_k)) {
            return;
        }
    }

#if PSP_FOG_CLAMP_SPLIT
    if (fog_mode && !sub && sFogSplitOn && comb->vertex_color_source[0] != CC_LOD && comb->vertex_color_source[1] != CC_LOD) {
        const float qa = v1->_w, qb = v2->_w, qc = v3->_w;
        const float mn = qa < qb ? (qa < qc ? qa : qc) : (qb < qc ? qb : qc);
        const float mx = qa > qb ? (qa > qc ? qa : qc) : (qb > qc ? qb : qc);
        if ((mn < sFogWLo && mx > sFogWLo) || (mn < sFogWHi && mx > sFogWHi)) {
            gfx_tri_fog_split(v1, v2, v3);
            return;
        }
    }
#endif

    /* Most common case (level geometry, most objects): the output color is the shade color as is, and there is no need to
     * go through gfx_get_vertex_color's switch for each vertex. */
    const bool shade_only = !sEnvaWhite && comb->vertex_color_source[0] == CC_SHADE &&
                            (!use_alpha || comb->vertex_color_source[1] == CC_SHADE);
    if (__builtin_expect(sSweepMode >= 3, 0)) {
        sEnvaWhite = false;
        return; /* Profile Sweep */
    }
    B5H(4, shade_only);
    B5H(5, use_texture);
    B5H(9, sEnvaWhite);
    B5MARK(2);
    size_t i = 0;
    if (VAR & 2) {
        gfx_tri_emit_fast(clipped_vertices, clipped_vertices_num, use_texture, shade_only, fog_mode, VAR, comb, use_alpha, lod_k,
                          tex_u_scale, tex_v_scale, tex_u_bias, tex_v_bias);
        i = clipped_vertices_num;
    }
    for (; i < clipped_vertices_num; i++) {
        const struct LoadedVertex *vertex = clipped_vertices[i];
        psp_fast_t *out = &buf_vbo[buf_num_vert];
        out->x = vertex->x;
        out->y = vertex->y;
        out->z = vertex->z;
        
        if (use_texture && !B5ABL(1)) {
            out->u = vertex->u * tex_u_scale + tex_u_bias;
            out->v = vertex->v * tex_v_scale + tex_v_bias;
        } else {
            out->u = 0.0f;
            out->v = 0.0f;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = clipped_vertices[i].color.a / 255.0f; // fog factor (not alpha)
        }
        */
        if (B5ABL(2)) {
        } else if (shade_only) {
            out->color = vertex->color;
        } else {
            out->color = gfx_get_vertex_color(comb, use_alpha, &vertex->color, lod_k * vertex->_w, true); /* per-vertex LOD */
        }

        if (fog_mode && !B5ABL(0)) {
            /* Same as the N64 / sm64ex: factor = (z/w) * fog_mul + fog_offset, limited to 0..255. */
            float fw = vertex->_w;
            if (fabsf(fw) < 0.001f) fw = 0.001f;
            float fz = (sFogScaled ? (sFogEffNA + sFogEffB / fw) : (vertex->_z / fw)) * (float)rsp.fog_mul + (float)rsp.fog_offset;
            unsigned char fa = (fz <= 0.0f) ? 0 : ((fz >= 255.0f) ? 255 : (unsigned char)fz);
            scegu_fog_alpha[buf_num_vert] = fa;
            if (fog_mode == 3) {
                /* translucent: the color comes out as C*(1-F); the fog is added later in the 2nd pass */
                const unsigned int inv = 255u - fa;
                out->color.r = (unsigned char)((out->color.r * inv) / 255u);
                out->color.g = (unsigned char)((out->color.g * inv) / 255u);
                out->color.b = (unsigned char)((out->color.b * inv) / 255u);
            }
        }

        /* (Before there was a "Blue Star color" patch here that copied the color of the 1st vertex to all of them and set the env alpha:
         * it left the blue stars and the Boo (SHADEFADEA shader) flat. It is no longer needed: gfx_get_vertex_color
         * takes RGB from shade and alpha from env separately.) */
        if (shader_program_id == 0x01A00045) {
            /* Matches the old code, which only updated the temporary pointer after the copy. */
        }
        buf_num_vert++;
        buf_vbo_len += sizeof(psp_fast_t);
    }
    B5MARK(3);
    sEnvaWhite = false;
    sPcTri += (unsigned int)(clipped_vertices_num / 3);
    buf_vbo_num_tris += clipped_vertices_num / 3;
    if (buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

#if PSP_DEVBENCH == 5
/* Diagnostic: one copy of gfx_tri_core per variant, chosen at run time (the measurement compares the same list with each one). */
static int sTriVarSel;
static __attribute__((noinline)) void gfx_tri_core_v0(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int sub, float k) { gfx_tri_core_body(a, b, c, sub, k, 0); }
static __attribute__((noinline)) void gfx_tri_core_v1(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int sub, float k) { gfx_tri_core_body(a, b, c, sub, k, 1); }
static __attribute__((noinline)) void gfx_tri_core_v3(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int sub, float k) { gfx_tri_core_body(a, b, c, sub, k, 3); }
static __attribute__((noinline)) void gfx_tri_core_v7(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int sub, float k) { gfx_tri_core_body(a, b, c, sub, k, 7); }
static void gfx_tri_core(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int sub, float k) {
    switch (sTriVarSel) {
        case 1: gfx_tri_core_v1(a, b, c, sub, k); break;
        case 3: gfx_tri_core_v3(a, b, c, sub, k); break;
        case 7: gfx_tri_core_v7(a, b, c, sub, k); break;
        default: gfx_tri_core_v0(a, b, c, sub, k); break;
    }
}
#else
static void gfx_tri_core(struct LoadedVertex *a, struct LoadedVertex *b, struct LoadedVertex *c, int sub, float k) {
    gfx_tri_core_body(a, b, c, sub, k, PSP_TRI_VAR);
}
#endif

/* This will be going away possibly, it all depends on how we end up treating hw sprites */
static void gfx_sp_tri1_2d(uint8_t vtx1_idx, uint8_t vtx2_idx, UNUSED uint8_t vtx3_idx) {
    struct VertexColor *v1 = &rsp.loaded_vertices_2D[vtx1_idx];
    struct VertexColor *v2 = &rsp.loaded_vertices_2D[vtx2_idx];
    struct VertexColor *v_arr[2] = {v1, v2};

    gfx_prepare_tri_pipeline_state();
    const struct TriPipelineState *state = &rendering_state.tri_pipeline;
    struct ColorCombiner *comb = state->comb;
    const bool use_alpha = state->use_alpha;
    const bool use_texture = state->use_texture;
    //uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    //uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

    VertexColor tri_buf[2] = {{0}};
    int tri_num_vert = 0;
    
    for (int i = 0; i < 2; i++) {
        tri_buf[tri_num_vert].x = v_arr[i]->x;
        tri_buf[tri_num_vert].y = v_arr[i]->y;
        tri_buf[tri_num_vert].z = 0;
        
        if (use_texture) {
            short u = (v_arr[i]->u - rdp.texture_tile.uls * 8)/ 32;
            short v = (v_arr[i]->v - rdp.texture_tile.ult * 8) / 32;
            /*
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            */
            tri_buf[tri_num_vert].u = u;
            tri_buf[tri_num_vert].v = v;
        } else {
            tri_buf[tri_num_vert].u = 0;
            tri_buf[tri_num_vert].v = 0;
        }
        
        /*
        //@Note no fog currently
        if (use_fog) {
            tri_buf[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            tri_buf[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            tri_buf[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }
        */
        tri_buf[tri_num_vert].color = gfx_get_vertex_color(comb, use_alpha, &v_arr[i]->color, 0.0f, false);
        tri_num_vert++;
    }
    gfx_scegu_draw_triangles_2d((float*)&tri_buf[0],0,1);
}

/* Texture rectangle with G_TEXRECTFLIP (the EU version's font uses it on every letter): on the RDP the texture coordinates are
 * transposed (s advances with y, t with x). A PSP "sprite" cannot transpose, so it is drawn as a 4-vertex strip:
 * 0=top-left (s0,t0) 1=top-right (s0,t1) 2=bottom-left (s1,t0) 3=bottom-right (s1,t1). Only EU uses it; the other versions do not go through here. */
static void gfx_sp_quad_flip_2d(void) {
    struct VertexColor *ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor *lr = &rsp.loaded_vertices_2D[1];

    gfx_prepare_tri_pipeline_state();
    const struct TriPipelineState *state = &rendering_state.tri_pipeline;
    struct ColorCombiner *comb = state->comb;
    const bool use_alpha = state->use_alpha;
    const bool use_texture = state->use_texture;

    VertexColor q[4] = {{0}};
    const unsigned short xs[4] = { ul->x, lr->x, ul->x, lr->x };
    const unsigned short ys[4] = { ul->y, ul->y, lr->y, lr->y };
    const unsigned short us[4] = { lr->u, lr->u, ul->u, ul->u };
    const unsigned short vs[4] = { lr->v, ul->v, lr->v, ul->v };
    for (int i = 0; i < 4; i++) {
        q[i].x = xs[i];
        q[i].y = ys[i];
        q[i].z = 0;
        if (use_texture) {
            q[i].u = (short)(((short)us[i] - rdp.texture_tile.uls * 8) / 32);
            q[i].v = (short)(((short)vs[i] - rdp.texture_tile.ult * 8) / 32);
        }
        q[i].color = gfx_get_vertex_color(comb, use_alpha, &ul->color, 0.0f, false);
    }
    gfx_scegu_draw_quad_2d((float *)&q[0]);
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    rdp.vp_l = x;
    rdp.vp_t = (viewport->vtrans[1] / 4.0f) - height / 2.0f;
    rdp.vp_w = width;
    rdp.vp_h = height;
    gfx_sc_touch();
    
    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
    
    rdp.viewport_or_scissor_changed = true;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    _UNUSED(offset);

    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            gfx_update_fog_range();
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    _UNUSED(level);
    _UNUSED(tile);
    _UNUSED(on);

    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    _UNUSED(mode);

    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
#if defined(TARGET_PSP)
    {
        /* The scissor fields are integers: truncating "y" and "height" separately moved the top edge
         * by up to ~2 N64 px (the top of the 1st line of dialog boxes got eaten). Each edge is rounded
         * outwards (left/top down, right/bottom up); the exact clipping of
         * triangles and rectangles is already done by the software. */
        const float cur_w = (float) gfx_current_dimensions.width;
        const float cur_h = (float) gfx_current_dimensions.height;
        float fl = ulx / 4.0f * RATIO_X, fr = lrx / 4.0f * RATIO_X;
        float ft = uly / 4.0f * RATIO_Y, fb = lry / 4.0f * RATIO_Y;
        if (fl < 0.0f) fl = 0.0f;
        if (ft < 0.0f) ft = 0.0f;
        if (fr > cur_w) fr = cur_w;
        if (fb > cur_h) fb = cur_h;
        {
            const int il = (int) floorf(fl + 0.001f);
            const int it = (int) floorf(ft + 0.001f);
            const int ir = (int) ceilf(fr - 0.001f);
            const int ib = (int) ceilf(fb - 0.001f);
            x = (float) il;
            width = (float) (ir > il ? ir - il : 0);
            height = (float) (ib > it ? ib - it : 0);
            y = (float) ((int) cur_h - ib);
        }
    }
#endif
    
    rdp.sc_ulx = ulx; rdp.sc_uly = uly; rdp.sc_lrx = lrx; rdp.sc_lry = lry;
    rdp.sc_valid = true;
    gfx_sc_touch();
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    _UNUSED(format);
    _UNUSED(width);

    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, UNUSED uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    _UNUSED(maskt);
    _UNUSED(shiftt);
    _UNUSED(masks);
    _UNUSED(shifts);

    if (tile == G_TX_RENDERTILE) {
        SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
        rdp.texture_tile.line_size_bytes = line * 8;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        gfx_mark_tri_pipeline_dirty();
    }
    
    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        gfx_mark_tri_pipeline_dirty();
    }
}

static void gfx_dp_load_tlut(UNUSED uint8_t tile, uint32_t high_index) {
    _UNUSED(high_index);

    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
}

static void gfx_dp_load_block(uint8_t tile, UNUSED uint32_t uls, UNUSED uint32_t ult, uint32_t lrs, uint32_t dxt) {
    _UNUSED(dxt);

    if (tile == 1) return;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
    
    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift = 0;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (tile == 1) return;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    uint32_t word_size_shift = 0;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t size_bytes = (((lrs >> G_TEXTURE_IMAGE_FRAC) + 1) * ((lrt >> G_TEXTURE_IMAGE_FRAC) + 1)) << word_size_shift;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;

    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
    gfx_mark_tri_pipeline_dirty();
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha) {
    rdp.combine_mode = rgb | (alpha << 12);
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

extern unsigned int scegu_fog_color; /* definido en gfx_scegu.c */

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;

    /* The fog is drawn by the PSP's GE (sceGuFog): the color must be passed in ABGR. */
    const unsigned int new_fog = 0xFF000000u | ((unsigned int)b << 16) | ((unsigned int)g << 8) | (unsigned int)r;
    if (new_fog != scegu_fog_color) {
        gfx_flush(); /* what has already accumulated is drawn with the previous color */
        scegu_fog_color = new_fog;
    }
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, bool flip) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
        gfx_mark_tri_pipeline_dirty();
    }
    
    /* Exact clipping against the RDP scissor in N64 units (U10.2). It is applied to
        rectangles (dialog text, etc.) because on the PSP the hardware scissor
        does not clip them. Scissor edges that are <=4 px from the screen edge
        are ignored (full-screen scissor / the game's 2 px margin). */
    if (rdp.sc_valid && lrx > ulx && lry > uly) {
        const int32_t m = 16; /* 4 px */
        int32_t cx0 = ulx, cy0 = uly, cx1 = lrx, cy1 = lry;
        if (rdp.sc_ulx > m && cx0 < rdp.sc_ulx) cx0 = rdp.sc_ulx;
        if (rdp.sc_uly > m && cy0 < rdp.sc_uly) cy0 = rdp.sc_uly;
        if (rdp.sc_lrx < SCREEN_WIDTH * 4 - m && cx1 > rdp.sc_lrx) cx1 = rdp.sc_lrx;
        if (rdp.sc_lry < SCREEN_HEIGHT * 4 - m && cy1 > rdp.sc_lry) cy1 = rdp.sc_lry;
        if (cx0 != ulx || cy0 != uly || cx1 != lrx || cy1 != lry) {
            if (cx1 <= cx0 || cy1 <= cy0) {
                if (cycle_type == G_CYC_COPY) {
                    rdp.other_mode_h = saved_other_mode_h;
                    gfx_mark_tri_pipeline_dirty();
                }
                return; /* completely outside the scissor */
            }
            struct VertexColor *cul = &rsp.loaded_vertices_2D[0];
            struct VertexColor *clr = &rsp.loaded_vertices_2D[1];
            float u0 = (short)cul->u, u1 = (short)clr->u;
            float v0 = (short)cul->v, v1 = (short)clr->v;
            float w = (float)(lrx - ulx), h = (float)(lry - uly);
            if (!flip) {
                cul->u = (unsigned short)(short)(u0 + (u1 - u0) * ((float)(cx0 - ulx) / w));
                clr->u = (unsigned short)(short)(u0 + (u1 - u0) * ((float)(cx1 - ulx) / w));
                cul->v = (unsigned short)(short)(v0 + (v1 - v0) * ((float)(cy0 - uly) / h));
                clr->v = (unsigned short)(short)(v0 + (v1 - v0) * ((float)(cy1 - uly) / h));
            } else { /* transposed: s advances with y, t with x */
                cul->u = (unsigned short)(short)(u0 + (u1 - u0) * ((float)(cy0 - uly) / h));
                clr->u = (unsigned short)(short)(u0 + (u1 - u0) * ((float)(cy1 - uly) / h));
                cul->v = (unsigned short)(short)(v0 + (v1 - v0) * ((float)(cx0 - ulx) / w));
                clr->v = (unsigned short)(short)(v0 + (v1 - v0) * ((float)(cx1 - ulx) / w));
            }
            ulx = cx0; uly = cy0; lrx = cx1; lry = cy1;
        }
    }

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = (ulyf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = (lryf / (4.0f * HALF_SCREEN_HEIGHT)) - 1.0f;
    
    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    { /* half of the REAL width of the frame (480 normal; 362 with the TV's 4:3 view): x = NDC * W/2 + W/2 */
        const float halfw = (float)gfx_current_dimensions.width * 0.5f;
        ulxf = (ulxf*halfw)+halfw;
        lrxf = (lrxf*halfw)+halfw;
    }

    ulyf = (ulyf*136)+136;
    lryf = (lryf*136)+136;
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    
    ul->x = (unsigned short)ulxf;
    ul->y = (unsigned short)ulyf;

    lr->x = (unsigned short)lrxf;
    lr->y = (unsigned short)lryf;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;
    gfx_mark_tri_pipeline_dirty();
    
    if (flip) {
        gfx_sp_quad_flip_2d();
    } else {
        gfx_sp_tri1_2d(0, 1, 2);
    }
    
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    gfx_mark_tri_pipeline_dirty();
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
        gfx_mark_tri_pipeline_dirty();
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    _UNUSED(tile);

    uint32_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0));
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct VertexColor* ul = &rsp.loaded_vertices_2D[0];
    struct VertexColor* lr = &rsp.loaded_vertices_2D[1];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    /*@Note: fix this */
    #if 0
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    #endif
    
    gfx_draw_rectangle(ulx, uly, lrx, lry, flip);
    rdp.combine_mode = saved_combine_mode;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    for (int i = 0; i < 2; i++) {
        struct VertexColor* v = &rsp.loaded_vertices_2D[i];
        v->color = rdp.fill_color;
    }
    
    uint32_t saved_combine_mode = rdp.combine_mode;
    gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE));
    gfx_draw_rectangle(ulx, uly, lrx, lry, false);
    rdp.combine_mode = saved_combine_mode;
    gfx_mark_tri_pipeline_dirty();
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    _UNUSED(format);
    _UNUSED(size);
    _UNUSED(width);

    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    gfx_mark_tri_pipeline_dirty();
}

static inline void *seg_addr(uintptr_t w1) {
    return (void *) w1;
}

/* ---- PSP_F2_REPLAY_MTX (stage 2): note where each object's matrix ended up in the GE list ----
 * The game leaves in gMtxTbl one entry per object: the position of the G_MTX command in the draw list (which carries the INTERPOLATED matrix)
 * and the real matrix. When interpreting that G_MTX (frame 1, only when recording) it notes at which offset of the GE list the
 * 13 words of sceGuSetMatrix(GU_MODEL) ended up and the translation of the interpolated matrix. The 2nd frame (gfx_psp_replay_prepare)
 * rewrites those 12 words with the real matrix. */
#if !defined(GBI_FLOATS)
#undef PSP_F2_REPLAY_MTX
#define PSP_F2_REPLAY_MTX 0
#endif
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
#define REC_MTX_MAX 1024 /* more objects than this in a frame (gMtxTbl[] allows 6400) -> that tick is not replayed */
extern int psp_mtx_tbl_count(void);
extern Gfx *psp_mtx_tbl_pos(int i);
extern const float *psp_mtx_tbl_mtx(int i);
extern float psp_persp_near(void);
extern unsigned int gfx_scegu_list_pos(void);
extern int gfx_scegu_mtx_block_ok(unsigned int off, int full, const float *m);
extern void gfx_scegu_write_model(unsigned int off, const float *m);
static Gfx *sRecPos[REC_MTX_MAX];          /* G_MTX command of each entry (copy of gMtxTbl[i].pos) */
static unsigned int sRecOff[REC_MTX_MAX];  /* offset in the GE list of the matrix block (0 = not emitted) */
typedef char sRecNearSizeCheck[(REC_MTX_MAX == 1024) ? 1 : -1]; /* sRecNear[] is 1024 long (declared earlier, next to sRejXY) */
static float sRecM[REC_MTX_MAX][12];       /* 3x3 and translation of the matrix that frame 1 carried: m[0..2], m[4..6], m[8..10], m[12..14] */
static int sRecCount;                      /* table entries when recording */
static int sRecCursor;
static int sRecMatched;
static int sRecWhy;                        /* first reason for sRecBad: 0 not recording, 1 too many, 2 nested MUL, 3 odd block, 4 unmatched */
static int sRecBad;                        /* a block did not have the expected shape: this tick is not patched */
static int sRecMtxOn;                      /* 1 while recording */
static int sRecMtxValid;                   /* this tick's annotation is usable */
static int sRecFull;                       /* full verifications done (the 1st matrix of each recording) */

static void gfx_rec_mtx_begin(void) {
    int i, n = psp_mtx_tbl_count();
    sRecMtxValid = 0;
    sRecBad = 0;
    sRecWhy = 0;
    sRecMatched = 0;
    sRecCursor = 0;
    sRecFull = 0;
    sRecCount = n;
    sRecCur = -1;
#if PSP_SKY_PATCH
    sRecSkyOk = 0;
    sSkyInterpPtr = psp_sky_interp_ortho();
#endif
    if (n < 0 || n >= REC_MTX_MAX) {
        sRecBad = 1;
        sRecWhy = 1;
        sRecMtxOn = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        sRecPos[i] = psp_mtx_tbl_pos(i);
        sRecOff[i] = 0;
        sRecNear[i] = 0;
    }
    sRecNearBand = psp_persp_near() * 1.4f; /* "close to the near" = within 0.4*near beyond the plane */
    sRecMtxOn = 1;
}

static int gfx_rec_mtx_find(const Gfx *cmd) {
    const int n = sRecCount;
    int k;
    for (k = 0; k < n; k++) {
        int i = sRecCursor + k;
        if (i >= n) {
            i -= n;
        }
        if (sRecPos[i] == cmd) {
            sRecCursor = (i + 1 >= n) ? 0 : i + 1;
            return i;
        }
    }
    return -1;
}

/* Right after gfx_sp_matrix of a G_MTX; off = position of the GE list before interpreting it. */
static void gfx_rec_mtx_done(const Gfx *cmd, unsigned int off, const float *m, unsigned int params) {
    const int i = gfx_rec_mtx_find(cmd);
    if (i < 0) {
        /* It is not an object matrix from the table. A projection or a loaded one (LOAD) cuts the object chain (HUD, menus...); one
         * multiplied (MUL) inside an object would come out in the list already composed with the 1st frame's matrix and could not be patched. */
        if ((params & G_MTX_PROJECTION) != 0 || (params & G_MTX_LOAD) != 0) {
            sRecCur = -1;
        } else if (sRecCur >= 0) {
            sRecBad = 1;
            if (sRecWhy == 0) sRecWhy = 2;
        }
        return;
    }
    sRecCur = i;
    if (gfx_scegu_list_pos() - off != 13U * 4U || !gfx_scegu_mtx_block_ok(off, sRecFull == 0, m)) {
        sRecBad = 1;
        if (sRecWhy == 0) sRecWhy = 3;
        return;
    }
    sRecFull = 1;
    sRecOff[i] = off;
    memcpy(&sRecM[i][0], &m[0], 3 * sizeof(float));
    memcpy(&sRecM[i][3], &m[4], 3 * sizeof(float));
    memcpy(&sRecM[i][6], &m[8], 3 * sizeof(float));
    memcpy(&sRecM[i][9], &m[12], 3 * sizeof(float));
    sRecMatched++;
}

/* How much the object's point that in frame 1 was at q0 = (0, 0, -near) moves (in view units) -- right on the near plane,
 * which is where the CPU clipped the geometry of frame 1 -- going from frame 1's matrix (a1, saved when recording) to the real one (m2).
 * It is the squared distance. A camera that only rotates, or a still object, gives ~0 even if its translation changes a lot. A degenerate
 * matrix (det ~ 0, draws nothing) gives 0. NaNs give a value that does not pass the check. */
static float gfx_rec_mtx_disp2(const float *a1, const float *m2, float near_z) {
    const float vx = -a1[9], vy = -a1[10], vz = -near_z - a1[11];
    const float r0x = a1[0], r0y = a1[1], r0z = a1[2];
    const float r1x = a1[3], r1y = a1[4], r1z = a1[5];
    const float r2x = a1[6], r2y = a1[7], r2z = a1[8];
    /* c12 = r1 x r2, c20 = r2 x r0, c01 = r0 x r1 */
    const float c12x = r1y * r2z - r1z * r2y, c12y = r1z * r2x - r1x * r2z, c12z = r1x * r2y - r1y * r2x;
    const float c20x = r2y * r0z - r2z * r0y, c20y = r2z * r0x - r2x * r0z, c20z = r2x * r0y - r2y * r0x;
    const float c01x = r0y * r1z - r0z * r1y, c01y = r0z * r1x - r0x * r1z, c01z = r0x * r1y - r0y * r1x;
    const float det = r0x * c12x + r0y * c12y + r0z * c12z;
    float w0, w1, w2, ix, iy, iz;
    if (fabsf(det) < 1e-20f) {
        return 0.0f;
    }
    {
        const float id = 1.0f / det;
        w0 = (vx * c12x + vy * c12y + vz * c12z) * id; /* w * A1 = v  ->  w = local coordinates of the point */
        w1 = (vx * c20x + vy * c20y + vz * c20z) * id;
        w2 = (vx * c01x + vy * c01y + vz * c01z) * id;
    }
    ix = w0 * m2[0] + w1 * m2[4] + w2 * m2[8] + m2[12];
    iy = w0 * m2[1] + w1 * m2[5] + w2 * m2[9] + m2[13];
    iz = w0 * m2[2] + w1 * m2[6] + w2 * m2[10] + m2[14];
    iz -= -near_z;
    return ix * ix + iy * iy + iz * iz;
}

static void gfx_rec_mtx_end(void) {
    sRecMtxOn = 0;
    sRecCur = -1;
    sRecMtxValid = (!sRecBad && sRecMatched == sRecCount);
    if (!sRecBad && sRecMatched != sRecCount) {
        sRecWhy = 4;
    }
}
#endif /* PSP_F2_REPLAY && PSP_F2_REPLAY_MTX */

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

static void gfx_run_dl(Gfx* cmd) {
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
#if PSP_DEVBENCH == 5
        sB5NCmd++;
        if (__builtin_expect(sB5Abl & 0x1F00, 0)) {
            /* Diagnostic: a group of commands is skipped (the cost includes what that group causes afterwards) */
            int b5skip = 0;
            switch (opcode) {
                case G_MTX: case (uint8_t)G_POPMTX: b5skip = B5ABL(8); break;
                case G_SETTIMG: case G_LOADBLOCK: case G_LOADTILE: case G_SETTILE: case G_SETTILESIZE: case G_LOADTLUT: case (uint8_t)G_TEXTURE:
                    b5skip = B5ABL(9); break;
                case G_SETCOMBINE: case (uint8_t)G_SETOTHERMODE_L: case (uint8_t)G_SETOTHERMODE_H: case G_SETENVCOLOR: case G_SETPRIMCOLOR: case G_SETFOGCOLOR:
#ifdef F3DEX_GBI_2
                case G_GEOMETRYMODE:
#else
                case (uint8_t)G_SETGEOMETRYMODE: case (uint8_t)G_CLEARGEOMETRYMODE:
#endif
                    b5skip = B5ABL(10); break;
                case (uint8_t)G_TRI1:
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
                case (uint8_t)G_TRI2:
#endif
                    b5skip = B5ABL(11); break;
                case G_VTX: b5skip = B5ABL(12); break;
                default: break;
            }
            if (b5skip) { ++cmd; continue; }
        }
#endif
        
#if PSP_DEVBENCH == 5
        const unsigned int b5ot = sB5OpOn ? sceKernelGetSystemTimeLow() : 0U;
#endif
        switch (opcode) {
            case G_NOOP:
                break;
            // RSP commands:
            case G_MTX:
#if PSP_EYE_SPACE
                /* a model/view matrix no longer changes anything on the GE: the batch continues (only the projection flushes) */
#ifdef F3DEX_GBI_2
                if (!sEyeOn || ((C0(0, 8) ^ G_MTX_PUSH) & G_MTX_PROJECTION)) {
                    gfx_flush();
                }
#else
                if (!sEyeOn || (C0(16, 8) & G_MTX_PROJECTION)) {
                    gfx_flush();
                }
#endif
#else
                gfx_flush();
#endif
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
                if (sRecMtxOn) {
                    const int32_t *ma = (const int32_t *) seg_addr(cmd->words.w1);
                    const unsigned int moff = gfx_scegu_list_pos();
                    B5T(12, gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, ma));
                    gfx_rec_mtx_done(cmd, moff, (const float *) ma, C0(0, 8) ^ G_MTX_PUSH);
                    break;
                }
#endif
                {
                    const unsigned int tm0 = sProfLogOn ? sceKernelGetSystemTimeLow() : 0U;
#ifdef F3DEX_GBI_2
                    B5T(12, gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1)));
#else
                    B5T(12, gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1)));
#endif
                    if (sProfLogOn) {
                        sPcMtxUs += sceKernelGetSystemTimeLow() - tm0;
                    }
                }
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
#endif
                /* Triangles almost always come in consecutive runs (G_TRI2 / G_TRI1): they are dispatched right here, without going back to the switch
                 * (indirect jump through the table) for every command. The order and what is done with each triangle is exactly as before. */
                for (;;) {
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
                    if (opcode == (uint8_t)G_TRI2) {
                        gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                        gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                    } else
#endif
                    {
#ifdef F3DEX_GBI_2
                        gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                        gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                        gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                    }
#if !PSP_TRI_CHAIN
                    break;
#endif
                    const uint32_t nop = cmd[1].words.w0 >> 24;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
                    if (nop != (uint8_t)G_TRI1 && nop != (uint8_t)G_TRI2) {
                        break;
                    }
#else
                    if (nop != (uint8_t)G_TRI1) {
                        break;
                    }
#endif
                    ++cmd;
                    opcode = nop;
                }
                break;
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
            
            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                B5T(13, gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12)));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                B5T(13, gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4)));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(
                    color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)));
                    /*color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                    color_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));*/
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
#ifdef F3DEX_GBI_2E
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#else
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
#endif
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
#if PSP_DEVBENCH == 5
        if (sB5OpOn) {
            sB5OpAcc[opcode & 0xff] += sceKernelGetSystemTimeLow() - b5ot;
            sB5OpN[opcode & 0xff]++;
        }
#endif
        ++cmd;
    }
}

static void gfx_sp_reset() {
    gfx_mp_flush();
#if PSP_EYE_SPACE
    sMVDirty = 1;
#endif
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}


float times[30];
float time_avg;
float time_first_200;
int total_frame_counter;
int frame_counter;

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();
#if defined(TARGET_PSP) && PSP_SCRATCH
    memset((void *) PSP_SCRATCH_BASE, 0, 0x4000); /* the scratchpad does not come zeroed like the BSS */
#endif
    rendering_state.color_combiner_valid = false;
    rendering_state.tri_pipeline_dirty = true;
    rendering_state.backend_state_dirty = true;

    int i;
    for(i=0;i<30;i++){
        times[i] = 0.0f;
    }
    frame_counter = 0;
    time_avg = 0.0f;
    time_first_200 = 0;
    total_frame_counter = 0;

    // Used in the 120 star TAS
    static uint32_t precomp_shaders[] = {
        0x01200200,
        0x00000045,
        0x00000200,
        0x01200a00,
        0x00000a00,
        0x01a00045,
        0x00000551,
        0x01045045,
        0x05a00a00,
        0x01200045,
        0x05045045,
        0x01045a00,
        0x01a00a00,
        0x0000038d,
        0x01081081,
        0x0120038d,
        0x03200045,
        0x03200a00,
        0x01a00a6f,
        0x01141045,
        0x07a00a00,
        0x05200200,
        0x03200200,
        0x09200200,
        0x0920038d,
        0x09200045
    };
    for (size_t i = 0; i < sizeof(precomp_shaders) / sizeof(uint32_t); i++) {
        gfx_lookup_or_create_shader_program(precomp_shaders[i]);
    }

    memcpy(rsp.P_matrix, identity_matrix, sizeof(identity_matrix));
    memcpy(rsp.modelview_matrix_stack[0], identity_matrix, sizeof(identity_matrix));

    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

unsigned int total_t0, total_t1;

/* Per-tick timing profile (microseconds, average of 30 ticks in tenths of ms so it can be shown with integers):
 * gPspProf[0]=LOG logic+object graph, [1]=GFX list emulation (CPU), [2]=GE wait for the GPU,
 * [3]=SLP sync wait (slack), [4]=MAX longest work tick (without waiting) in the window. */
int gPspProf[5];
extern int psp_opt_fps_cap(void);
extern int gPspOptFpsCap, gPspOptKeepSpeed, gPspClockWant, gPspOptCullObj;
extern float gPspObjDistScale, gPspFarScale; /* game/psp_options.c */
extern int psp_objects_free(void);              /* game/spawn_object.c */
extern int gPspFreeMin;                         /* game/spawn_object.c */
extern unsigned int gPspKicks, gPspRawDraws;   /* gfx_scegu.c */
extern unsigned int gPspWbCalls;                 /* gfx_scegu.c: vertex arena cache flushes */
extern unsigned int gPspOptChanges;
extern int gPspOptPipe;                                  /* game/psp_options.c */
extern unsigned int gPspGateUs, gPspGateN, gPspQMax;     /* gfx_scegu.c: Triple Buffer */
extern int gPspPipeBroken;
unsigned int gPspPresented; /* frames presented (read by game_init.c's FPS counter) */
static unsigned int sProfGfx, sProfSleep, sProfAccGfx, sProfAccGe, sProfAccSleep, sProfAccWork, sProfMax, sProfTicks;
static unsigned int sProfGe;
extern unsigned int gPspProfSync, gPspProfVblank; /* gfx_scegu.c */
int gPspProfX[2]; /* [0]=SYNC waits for the GPU, [1]=VBL waits for the vblank (tenths of ms) */
static unsigned int sProfSyncCur, sProfVblCur, sProfAccSync, sProfAccVbl;

/* Timing log to file (ms0:/sm64_profile.txt). It is activated ONLY while the game's FPS counter is
 * on screen (L+R): showing it starts the log and hiding it closes and saves it. Everything lives here so as not to read variables from other files or draw text.
 * One line every 30 ticks: N=window number, LV=level, AR=area, WALL=real ms of the window (1000 = 30 ticks/s),
 * DROP=ticks without drawing, LOG/GFX/GE/SLP/MAX in tenths of ms (average of 30 ticks; MAX = longest tick). */
#include <pspctrl.h>
#include <pspiofilemgr.h>
extern short gCurrLevelNum;
extern short gCurrAreaIndex;
static char sProfLogBuf[16384];
unsigned int gPspLp[32]; /* logic profile: see pc/psp_lprof.h */
unsigned int gPspLc[16];
static unsigned int sProfLate, sProfLateLog, sProfLateEmit, sProfLateSync;
static unsigned int sProfAcc2[8]; /* breakdown by triangle class (see sTriClass) */
static unsigned int sProfAcc3[4];
static unsigned int sProfAccTx[2]; /* TXS / TXE per window */
static unsigned int sProfBenchUc, sProfBenchCw; /* us per 1024 words written uncached / cached (+ flush) */
static unsigned int sProfAcc[16], sProfFrames, sProfListMax, sProfBenchGm, sProfBenchClk, sProfBenchCpu; /* per-window counters (see sPc*) */
static int sProfLogLen = 0;
int sProfLogOn = 0;
/* GPU profile (Triangle + L shortcut, only with the log ON): each draw call is serialized with the GE and the time the
 * GPU takes on it is measured (gfx_scegu.c), per pass type. It runs VERY slowly (tens of ms per frame): for measuring only. */
int gPspGpuProf = 0;
float gPcAreaAcc = 0.0f; /* area in pixels (approx.) of the triangles accumulated since the last draw call */
extern unsigned int gGpuUs[], gGpuTris[], gGpuPx[], gGpuCalls[];
static int sProfGpuWin = 0;   /* 1 if the GPU profile was active in the window */
#if PSP_GPU_PROF
static int sProfGpuPrevCombo = 0;
#endif
static unsigned int sProfLogWindows = 0;
static unsigned int sProfDroppedWin = 0;
static unsigned int sProfLastWall = 0;
static unsigned int sProfSixty = 0;
/* 60 fps controller diagnostic (per window): ticks, sum of A, ticks over budget, sum of pre / e / s, async frames */
/* Async presentation only pays off if there is GE queue to overlap with the logic (JRB: water/fog, queue ~4 ms). In
 * CPU-bound scenes (TTM, queue ~0) it only adds thread switches, so it is activated with hysteresis according to the measured average queue. */
int gPspAsyncWant = PSP_ASYNC_PRESENT;
static unsigned int sTailEma = 3000, sTailSum, sTailN;
#define PSP_ASYNC_ON_US 2000U
#define PSP_ASYNC_OFF_US 1200U
static unsigned int sDbgTk, sDbgA, sDbgOver, sDbgFr, sDbgPre, sDbgE, sDbgS, sDbgAsy;

#define PSP_STR2(x) #x
#define PSP_STR(x) PSP_STR2(x)
static void prof_put_str(const char *t) {
    while (*t && sProfLogLen < (int) sizeof(sProfLogBuf) - 1) {
        sProfLogBuf[sProfLogLen++] = *t++;
    }
}

static void prof_put_num(const char *label, int v) {
    char tmp[12];
    int n = 0;
    prof_put_str(label);
    if (v < 0) {
        prof_put_str("-");
        v = -v;
    }
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v > 0 && n < 11) {
        tmp[n++] = (char) ('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && sProfLogLen < (int) sizeof(sProfLogBuf) - 1) {
        sProfLogBuf[sProfLogLen++] = tmp[--n];
    }
}

int gPspLogStatus = 0;           /* 0 off, 1 writing fine, 2 the last write attempt failed (shown by the FPS counter) */
extern int gPspFpsCounterOn;     /* game_init.c: 1 while the FPS counter is drawn */
static unsigned int sProfLostW;  /* windows that did not fit in the buffer because writing was not possible */

/* Writes the buffer to the card. If something fails (card busy, short write...) the buffer is KEPT and retried on the
 * next call, instead of discarding the log as before (silently). */
static void prof_log_flush(void) {
    if (sProfLogLen > 0) {
        int ok = 0;
        SceUID fd = sceIoOpen("ms0:/sm64_profile.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            const int w = sceIoWrite(fd, sProfLogBuf, sProfLogLen);
            sceIoClose(fd);
            ok = (w == sProfLogLen);
        }
        if (ok) {
            sProfLogLen = 0;
            if (gPspLogStatus == 2) gPspLogStatus = 1;
        } else {
            gPspLogStatus = 2;
        }
    }
}

/* Called once per frame: the log follows the game's FPS counter (L+R) */
#ifndef PSP_PROFILE_LOG
#define PSP_PROFILE_LOG 0 /* 1 = allows writing ms0:/sm64_profile.txt with the FPS counter (for measuring only); 0 = final version, never writes or times */
#endif
static void prof_log_poll(void) {
    const int want = PSP_PROFILE_LOG ? gPspFpsCounterOn : 0;
    if (want && !sProfLogOn) {
        sProfLogOn = 1;
        gPspLogStatus = 1;
        sProfLogWindows = 0;
        sProfDroppedWin = 0;
        sProfLostW = 0;
        sProfLastWall = sceKernelGetSystemTimeLow();
        prof_put_str("--- registro ON ---\r\n");
        /* build signature: used to compare logs between builds (compiler, optimization, options of this file) */
        prof_put_str("BUILD " __DATE__ " " __TIME__ " gcc " __VERSION__
#ifdef __OPTIMIZE__
                     " OPT"
#else
                     " NOOPT"
#endif
#ifdef __OPTIMIZE_SIZE__
                     " Os"
#endif
                     " ASYNC=" PSP_STR(PSP_ASYNC_PRESENT) " GPUPROF=" PSP_STR(PSP_GPU_PROF) " XYCLIP=" PSP_STR(PSP_GE_XY_CLIP)
#ifdef PSP_BUILD_OPT
                     " CC=" PSP_BUILD_OPT " FALIGN=" PSP_STR(PSP_BUILD_FALIGN)
#endif
                     "\r\n");
#if PSP_EYE2_VFPU
        prof_put_num("EYE2 ", gPspEye2Ok); /* 1 = 2-vertex VFPU kernel checked and in use, 0 = the test failed (the usual one is used), -1 = not tested yet */
        prof_put_str("\r\n");
#endif
        {
            extern int psp_me_debug(int which);
            prof_put_str("ME");
            prof_put_num(" BOOT ", psp_me_debug(0));  /* meLibDefaultInit: <0 = the Media Engine did not start */
            prof_put_num(" INIT ", psp_me_debug(1));  /* 1 = audio is synthesized on the ME */
            prof_put_num(" PROG ", psp_me_debug(2));
            prof_put_num(" STATE ", psp_me_debug(3));
            prof_put_num(" BTRY ", psp_me_debug(4)); /* 1 = one attempt, 3 = it had to be retried from the savedata folder */
            prof_put_str("\r\n");
        }
        prof_log_flush(); /* the header is saved from the very first moment */
    } else if (!want && sProfLogOn) {
        sProfLogOn = 0;
        prof_put_str("--- registro OFF ---\r\n");
        prof_log_flush();
        if (sProfLogLen > 0) prof_log_flush(); /* a second attempt if the first failed */
        gPspLogStatus = 0;
        gPspGpuProf = 0;
    }
#if PSP_GPU_PROF
    /* GPU profile (Triangle + L, only with the log active). Only compiled with PSP_GPU_PROF 1: this way the pad is not read on
     * every frame when it is not needed. */
    {
        SceCtrlData pad;
        int gcombo;
        sceCtrlPeekBufferPositive(&pad, 1);
        gcombo = ((pad.Buttons & PSP_CTRL_TRIANGLE) && (pad.Buttons & PSP_CTRL_LTRIGGER)) ? 1 : 0;
        if (gcombo && !sProfGpuPrevCombo && sProfLogOn) {
            gPspGpuProf = !gPspGpuProf;
            prof_put_str(gPspGpuProf ? "--- GPU prof ON ---\r\n" : "--- GPU prof OFF ---\r\n");
        }
        sProfGpuPrevCombo = gcombo;
        if (gPspGpuProf) sProfGpuWin = 1;
    }
#endif
}

/* Called when closing each 30-tick window, with gPspProf already computed */
static void prof_log_window(void) {
    const unsigned int now = sceKernelGetSystemTimeLow();
    if (sProfLogOn) {
        if (sProfLogLen >= (int) sizeof(sProfLogBuf) - 1500) {
            prof_log_flush(); /* buffer almost full: try to flush it before losing the window */
        }
        if (sProfLogLen < (int) sizeof(sProfLogBuf) - 1500) {
            if (sProfLostW) {
                prof_put_num("--- se perdieron ventanas: ", (int) sProfLostW);
                prof_put_str(" ---\r\n");
                sProfLostW = 0;
            }
            prof_put_num("N", (int) sProfLogWindows);
            prof_put_num(" LV ", (int) gCurrLevelNum);
            prof_put_num(" AR ", (int) gCurrAreaIndex);
            prof_put_num(" WALL ", (int) ((now - sProfLastWall) / 1000));
            prof_put_num(" DROP ", (int) sProfDroppedWin);
            prof_put_num(" S60 ", (int) sProfSixty);
            prof_put_num(" LOG ", gPspProf[0]);
            prof_put_num(" GFX ", gPspProf[1]);
            prof_put_num(" SYNC ", gPspProfX[0]);
            prof_put_num(" VBL ", gPspProfX[1]);
            prof_put_num(" EMIT ", gPspProf[1] - gPspProfX[0] - gPspProfX[1]);
            prof_put_num(" GE ", gPspProf[2]);
            prof_put_num(" SLP ", gPspProf[3]);
            prof_put_num(" MAX ", gPspProf[4]);
            {
                /* averages per drawn frame of the window: TRI triangles, VTX vertices, DRW draw calls to the GE,
                 * MTX matrices, CLP triangles clipped by the CPU; TXM imported textures (sum over the window),
                 * TXC times the texture VRAM was emptied; TXMS ms importing textures, DRWMS ms inside the draw calls
                 * to the GE (CPU), both per window in tenths of ms; LST KB of GE command list (maximum). */
                const unsigned int fr = sProfFrames ? sProfFrames : 1;
                prof_put_num(" FR ", (int) sProfFrames);
                prof_put_num(" TRI ", (int) (sProfAcc[0] / fr));
                prof_put_num(" VTX ", (int) (sProfAcc[1] / fr));
                prof_put_num(" DRW ", (int) (sProfAcc[2] / fr));
                prof_put_num(" MTX ", (int) (sProfAcc[3] / fr));
                prof_put_num(" CLP ", (int) (sProfAcc[4] / fr));
                prof_put_num(" TXM ", (int) sProfAcc[5]);
                prof_put_num(" TXC ", (int) sProfAcc[6]);
                prof_put_num(" TXMS ", (int) (sProfAcc[8] / 100));
                prof_put_num(" DRWMS ", (int) (sProfAcc[7] / fr / 100));
                prof_put_num(" LST ", (int) (sProfListMax / 1024));
                prof_put_num(" GMUS ", (int) sProfBenchGm);   /* tenths of us per sceGuGetMemory */
                prof_put_num(" CLKUS ", (int) sProfBenchClk); /* tenths of us per clock read */
                prof_put_num(" LOOPUS ", (int) sProfBenchCpu); /* us for 20000 turns of a 3-instruction loop: ~180 at a real 333 MHz */
                prof_put_num(" MHZE ", sProfBenchCpu ? (int) (60000U / sProfBenchCpu) : 0); /* MHz reales estimados (si 1 instruccion/ciclo) */
                /* EMIT broken down: VTXMS ms (tenths, per frame) inside gfx_sp_vertex; TRIN triangles received per frame
                 * (TRI are the ones that came out drawn); TRIUS tenths of us per triangle (sampled, already without drawing or textures,
                 * includes ~1 clock read); FOGA/FOGK triangles with fog per frame / the ones that do a 2nd pass. */
                prof_put_num(" VTXMS ", (int) (sProfAcc[9] / fr / 100));
                prof_put_num(" TRIN ", (int) (sProfAcc[10] / fr));
                prof_put_num(" TRIUS ", sProfAcc[12] ? (int) (sProfAcc[11] * 10 / sProfAcc[12]) : 0);
                /* TRIUS broken down by class: average time (tenths of us) of a triangle discarded early (TRR), drawn without clipping
                 * (TRD) and clipped by the CPU (TRC); TRNR/TRND/TRNC = how many of each class per frame (estimated from the 1/8 sampling);
                 * CLPUS = total CPU clipping time per frame (tenths of ms) */
                prof_put_num(" TRR ", sProfAcc2[3] ? (int) (sProfAcc2[0] * 10 / sProfAcc2[3]) : 0);
                prof_put_num(" TRD ", sProfAcc2[4] ? (int) (sProfAcc2[1] * 10 / sProfAcc2[4]) : 0);
                prof_put_num(" TRC ", sProfAcc2[5] ? (int) (sProfAcc2[2] * 10 / sProfAcc2[5]) : 0);
                prof_put_num(" TRNR ", (int) (sProfAcc2[3] * 8 / fr));
                prof_put_num(" TRND ", (int) (sProfAcc2[4] * 8 / fr));
                prof_put_num(" TRNC ", (int) (sProfAcc2[5] * 8 / fr));
                prof_put_num(" CLPUS ", (int) (sProfAcc2[6] / fr / 100));
                prof_put_num(" FOGA ", (int) (sProfAcc[13] / fr));
                prof_put_num(" FOGK ", (int) (sProfAcc[14] / fr));
                prof_put_num(" CLPX ", (int) (sProfAcc[15] / fr)); /* triangles that crossed a side and were sent without clipping */
                {
                    /* menu options (game/psp_options.c) in effect when this window closed: OFPS fps limit, OPAC 0 = Keep FPS / 1 = Keep Speed;
                     * OOBJ / OFAR = object distance / camera far plane in %; OCLK MHz requested;
                     * OCHG = how many times the options were touched during the window (if not 0 the window mixes two configurations) */
                    static unsigned int sOptChg = 0;
                    prof_put_num(" OFPS ", gPspOptFpsCap);
                    prof_put_num(" OPAC ", gPspOptKeepSpeed);
                    prof_put_num(" OOBJ ", (int) (gPspObjDistScale * 100.0f + 0.5f));
                    prof_put_num(" OFAR ", (int) (gPspFarScale * 100.0f + 0.5f)); /* camera far plane, in % */
                    {
                        static unsigned int sPrevKick, sPrevRaw, sPrevWb;
                        prof_put_num(" OKCK ", (int) (gPspKicks - sPrevKick));    /* notices to the GE in the window */
                        prof_put_num(" ORAW ", (int) (gPspRawDraws - sPrevRaw));  /* draws sent without individual notice */
                        prof_put_num(" OWBC ", (int) (gPspWbCalls - sPrevWb));   /* arena cache flushes in the window */
                        sPrevWb = gPspWbCalls;
                        sPrevKick = gPspKicks;
                        sPrevRaw = gPspRawDraws;
                    }
                    prof_put_num(" OFREE ", psp_objects_free());
                    prof_put_num(" OFMIN ", gPspFreeMin);                              /* minimum of free objects in the window */
                    gPspFreeMin = 9999;                       /* free objects out of the game's 240 */
                    prof_put_num(" OSWP ", sSweepMode);                                /* Profile Sweep: etapa apagada (0 = ninguna) */
                    prof_put_num(" OCLK ", gPspClockWant);
                    prof_put_num(" OPIPE ", gPspOptPipe);                                /* Triple Buffer 0/1 */
                    prof_put_num(" PGW ", gPspGateN ? (int) (gPspGateUs * 10U / gPspGateN / 1000U) : 0); /* average start_frame wait per frame (0.1 ms) */
                    prof_put_num(" PQD ", (int) gPspQMax);                               /* largest queue of unpresented frames seen (0..3) */
                    prof_put_num(" PBRK ", gPspPipeBroken);                              /* 1 = un semaforo vencio y se volvio al modo sincrono */
                    prof_put_num(" OCUL ", gPspOptCullObj);                       /* 0 = original object culling, 1 = vertical too */
                    prof_put_num(" OCHG ", (int) (gPspOptChanges - sOptChg));
                    sOptChg = gPspOptChanges;
                }
                prof_put_num(" TXS ", (int) (sProfAccTx[0] / fr));            /* texture changes (tile 0) that were already the bound one: they do not break the batch (per frame) */
                prof_put_num(" TXE ", (int) (sProfAccTx[1] / fr));            /* texture changes (tile 0) requested by the game (per frame) */
                /* MTXMS: ms per frame (tenths) in gfx_sp_matrix; PREPMS: ms per frame (tenths) rebuilding the triangle pipeline
                 * state (texture / combiner / mode change; not counting drawing or texture import); PREPN: how many
                 * times per frame. UCW / CW: us per 1024 words written UNCACHED (as pspgu writes the list) / CACHED + flush. */
                prof_put_num(" MTXMS ", (int) (sProfAcc3[0] / fr / 100));
                prof_put_num(" PREPMS ", (int) (sProfAcc3[1] / fr / 100));
                prof_put_num(" PREPN ", (int) (sProfAcc3[2] / fr));
                prof_put_num(" UCW ", (int) sProfBenchUc);
                prof_put_num(" CW ", (int) sProfBenchCw);
                prof_put_num(" ARN ", (int) (gPspArenaMax / 1024)); /* maximum KB used of the vertex arena */
                /* "tight" frames (logic + drawing + GE wait > 14.5 ms): how many there were in the window and, on average,
                 * how much was logic / emission / GE wait (tenths of ms) */
                prof_put_num(" LATE ", (int) sProfLate);
                prof_put_num(" LLOG ", sProfLate ? (int) (sProfLateLog / sProfLate / 100) : 0);
                prof_put_num(" LEMT ", sProfLate ? (int) (sProfLateEmit / sProfLate / 100) : 0);
                prof_put_num(" LSYN ", sProfLate ? (int) (sProfLateSync / sProfLate / 100) : 0);
            }
            {
                /* Logic, average PER LOGIC TICK in tenths of ms (the timer adds ~0.5 us per measurement):
                 * AUD audio, INP controls, SCR full level script, UPD update_level, OBJ update_objects (TERR terrain,
                 * COLL collisions between objects, NTER rest of objects), CAM camera, MAR Mario (inside OBJ), FLR/WAL/CEL
                 * floor/wall/ceiling searches (inside OBJ/MAR/CAM), RND full render_game, GEO geo_process_root.
                 * Per-tick counts: NFLR/NWAL/NCEL calls, NOBJ objects updated, NGO objects and NGN graph nodes. */
                const unsigned int tk = gPspLc[0] ? gPspLc[0] : 1;
                static const char *const lpn[20] = {" AUD ", " INP ", " SCR ", " UPD ", " OBJ ", " TERR ", " COLL ", " NTER ",
                                                    " CAM ", " MAR ", " FLR ", " WAL ", " CEL ", " RND ", " GEO ",
                                                    " FN ", " ANI ", " SHD ", " MLS ", " VIEW "};
                int q;
                prof_put_num(" TK ", (int) gPspLc[0]);
                for (q = 0; q < 20; q++) {
                    prof_put_num(lpn[q], (int) (gPspLp[q] / tk / 100));
                }
                prof_put_num(" NFLR ", (int) (gPspLc[1] / tk));
                prof_put_num(" NWAL ", (int) (gPspLc[2] / tk));
                prof_put_num(" NCEL ", (int) (gPspLc[3] / tk));
                prof_put_num(" NOBJ ", (int) (gPspLc[4] / tk));
                prof_put_num(" NGO ", (int) (gPspLc[5] / tk));
                prof_put_num(" NGN ", (int) (gPspLc[6] / tk));
                prof_put_num(" NMTX ", (int) (gPspLc[7] / tk));
                prof_put_num(" NANIM ", (int) (gPspLc[8] / tk));
                prof_put_num(" NDL ", (int) (gPspLc[9] / tk));
                prof_put_num(" NSHD ", (int) (gPspLc[10] / tk));
                prof_put_num(" NFN ", (int) (gPspLc[11] / tk));
            }
            {
                const unsigned int tk2 = sDbgTk ? sDbgTk : 1, fr2 = sDbgFr ? sDbgFr : 1;
                prof_put_num(" AEST ", (int) (sDbgA / tk2 / 100));      /* A average per tick (tenths of ms) */
                prof_put_num(" AOVR ", (int) (sDbgOver * 100 / tk2));   /* % of ticks with A > budget */
                prof_put_num(" PRE ", (int) (sDbgPre / fr2 / 100));     /* logic before drawing, per frame */
                prof_put_num(" EUS ", (int) (sDbgE / fr2 / 100));       /* sending the frame (without waits) */
                prof_put_num(" SUS ", (int) (sDbgS / fr2 / 100));       /* GE queue used by the estimate */
                prof_put_num(" ASY ", (int) sDbgAsy);
                prof_put_num(" TAIL ", (int) (sTailEma / 100));          /* average GE queue (tenths of ms) */
                prof_put_num(" AWANT ", gPspAsyncWant);                 /* 1 = presentacion asincrona elegida */
                {
                    extern unsigned int gPspF2Skip;
                    static unsigned int sF2Last = 0;
                    prof_put_num(" F2SK ", (int) (gPspF2Skip - sF2Last)); /* 2nd frames dropped in the window */
                    {
                        /* PSP_F2_REPLAY: RPL = 2nd frames that replayed the 1st one's scene; the rest are the reasons why it
                         * could not (RPN: no recording, RPR: list restarted, RPT: texture store emptied, RPZ: several noise masks,
                         * RPB: the list check failed, RPO: disabled after consecutive failures).
                         * Stage 2: RPP = of the RPL, the ones that carried the real matrices patched; RPM = could not be patched because of
                         * the matrix annotation; RPD = an object moved more than the near distance; RPQ = another pending patch
                         * (HUD, dialog, transition, snow, bubbles, frames, fov). RPM/RPD/RPQ are frames drawn in full. */
                        extern unsigned int gPspRepOk, gPspRepNoRec, gPspRepRst, gPspRepTex, gPspRepBad, gPspRepNoise, gPspRepOff;
                        extern unsigned int gPspRepPatched, gPspRepMis, gPspRepDelta, gPspRepPend;
                        extern unsigned int gPspRepWhy[7];
                        extern int gPspReplayEma;
                        static unsigned int sRpM[4];
                        static unsigned int sRpL[7];
                        prof_put_num(" RPL ", (int) (gPspRepOk - sRpL[0]));
                        prof_put_num(" RPN ", (int) (gPspRepNoRec - sRpL[1]));
                        prof_put_num(" RPR ", (int) (gPspRepRst - sRpL[2]));
                        prof_put_num(" RPT ", (int) (gPspRepTex - sRpL[3]));
                        prof_put_num(" RPZ ", (int) (gPspRepNoise - sRpL[4]));
                        prof_put_num(" RPB ", (int) (gPspRepBad - sRpL[5]));
                        prof_put_num(" RPO ", (int) (gPspRepOff - sRpL[6]));
                        prof_put_num(" RPP ", (int) (gPspRepPatched - sRpM[0]));
                        prof_put_num(" RPM ", (int) (gPspRepMis - sRpM[1]));
                        prof_put_num(" RPD ", (int) (gPspRepDelta - sRpM[2]));
                        prof_put_num(" RPQ ", (int) (gPspRepPend - sRpM[3]));
                        prof_put_num(" RPE ", gPspReplayEma);
                        {
                            extern unsigned int gPspRepSky; /* frames 2 with the sky rewritten */
                            static unsigned int sRpS;
                            prof_put_num(" RPS ", (int) (gPspRepSky - sRpS));
                            sRpS = gPspRepSky;
                        }
                        {
                            static unsigned int sRpW[7];
                            int wq;
                            prof_put_str(" RPW ");
                            for (wq = 0; wq < 6; wq++) {
                                prof_put_num(wq == 0 ? "" : "/", (int) (gPspRepWhy[wq] - sRpW[wq]));
                                sRpW[wq] = gPspRepWhy[wq];
                            }
                        }
                        sRpM[0] = gPspRepPatched; sRpM[1] = gPspRepMis; sRpM[2] = gPspRepDelta; sRpM[3] = gPspRepPend;
                        sRpL[0] = gPspRepOk; sRpL[1] = gPspRepNoRec; sRpL[2] = gPspRepRst; sRpL[3] = gPspRepTex;
                        sRpL[4] = gPspRepNoise; sRpL[5] = gPspRepBad; sRpL[6] = gPspRepOff;
                    }
                    {
                        extern unsigned int gPspAudCpuUs, gPspAudCpuN;
                        static unsigned int sAudUsLast = 0, sAudNLast = 0;
                        prof_put_num(" AUDN ", (int) (gPspAudCpuN - sAudNLast));         /* audio blocks synthesized on the CPU (0 = on the ME) */
                        prof_put_num(" AUDUS ", (int) ((gPspAudCpuUs - sAudUsLast) / 100)); /* time of that synthesis (tenths of ms) */
                        sAudNLast = gPspAudCpuN; sAudUsLast = gPspAudCpuUs;
                    }
                    {
                        /* title voice (cumulative): requests/retries/confirmed playing/last (1=Hello 2=Press Start) */
                        extern unsigned int gPspVox[4];
                        prof_put_num(" VOX ", (int) gPspVox[0]);
                        prof_put_num("/", (int) gPspVox[1]);
                        prof_put_num("/", (int) gPspVox[2]);
                        prof_put_num("/", (int) gPspVox[3]);
                    }
                    {
                        /* Audio with the ME: the main thread sends blocks to the ME when the logic finishes and collects them at the start of the
                         * next tick. MEN = blocks sent in the window; MEWN = ticks in which the ME had not finished yet and the
                         * CPU had to wait; MEWMS = that total wait (tenths of ms per window); AUDUR = audio dropouts
                         * (the ring emptied and silence played) in the window. */
                        extern unsigned int gPspMePolls, gPspMeUs, gPspMeBlocks, gPspMeWaits, gPspAudUnder;
                        static unsigned int sMeU = 0, sMeB = 0, sMeW = 0, sAudUr = 0;
                        prof_put_num(" MEN ", (int) (gPspMeBlocks - sMeB));
                        prof_put_num(" MEWN ", (int) (gPspMeWaits - sMeW));
                        prof_put_num(" MEWMS ", (int) ((gPspMeUs - sMeU) / 100));
                        prof_put_num(" AUDUR ", (int) (gPspAudUnder - sAudUr));
                        extern unsigned int gPspMeTimeouts, gPspMeRecovered;
                        extern int psp_me_debug(int which);
                        prof_put_num(" MEFAIL ", (int) gPspMeTimeouts); /* cumulative: ME jobs that exceeded 100 ms */
                        prof_put_num(" MEREC ", (int) gPspMeRecovered); /* cumulative: of those, the ones that finished late and recovered */
                        prof_put_num(" MEBUSY ", psp_me_debug(5));
                        /* suspend: good/bad ME resumes, times meLibOnWake was called, ME state when sleeping (5 = stopped), and if it gave up: state/progress */
                        prof_put_num(" MERES ", psp_me_debug(6));
                        prof_put_num(" MEREF ", psp_me_debug(7));
                        prof_put_num(" MEWK ", psp_me_debug(8));
                        prof_put_num(" MESS ", psp_me_debug(9));
                        prof_put_num(" MERS ", psp_me_debug(10));
                        prof_put_num(" MERP ", psp_me_debug(11));
                        prof_put_num(" MEI ", psp_me_debug(1));
                        prof_put_num(" MEN0 ", psp_me_debug(12)); /* calls to power_event(0) / (1) and flags of the last power notices */
                        prof_put_num(" MEN1 ", psp_me_debug(13));
                        prof_put_num(" MEF0 ", psp_me_debug(14));
                        prof_put_num(" MEF1 ", psp_me_debug(15));
                        prof_put_num(" MEF2 ", psp_me_debug(16));
                        prof_put_num(" MEF3 ", psp_me_debug(17));
                        (void) gPspMePolls;
                        sMeU = gPspMeUs; sMeB = gPspMeBlocks; sMeW = gPspMeWaits; sAudUr = gPspAudUnder;
                    }
                    {
                        /* NOTAS = highest number of synthesizer notes in use (cap PSP_AUDIO_NOTES); NSTL = notes stolen from others
                         * in use; NDRP = layers left without a note (they do not sound); SFXC = sum of sounds competing for the channel
                         * of their bank (1 per bank) in each tick. All per window. */
                        extern unsigned int gPspNoteMax, gPspNoteSteal, gPspNoteDrop, gPspSfxOver;
                        static unsigned int sNs = 0, sNd = 0, sSo = 0;
                        prof_put_num(" NOTAS ", (int) gPspNoteMax);
                        prof_put_num(" NSTL ", (int) (gPspNoteSteal - sNs));
                        prof_put_num(" NDRP ", (int) (gPspNoteDrop - sNd));
                        prof_put_num(" SFXC ", (int) (gPspSfxOver - sSo));
                        sNs = gPspNoteSteal; sNd = gPspNoteDrop; sSo = gPspSfxOver; gPspNoteMax = 0;
                    }
                    sF2Last = gPspF2Skip;
                }
                {
                    /* real CPU/bus clock (MHz): if the system lowers it (power saving mode, battery, plugin) 333/166 is requested again */
                    static unsigned int sClkFix = 0;
                    const int cpu = scePowerGetCpuClockFrequencyInt(), bus = scePowerGetBusClockFrequencyInt();
                    prof_put_num(" CPU ", cpu);
                    prof_put_num(" BUS ", bus);
                    if (cpu < gPspClockWant || bus < gPspClockWant / 2) {
                        scePowerSetClockFrequency(gPspClockWant, gPspClockWant, gPspClockWant / 2);
                        sClkFix++;
                    }
                    prof_put_num(" CLKFIX ", (int) sClkFix);
                }
            }
            prof_put_num(" GPON ", sProfGpuWin);
            if (sProfGpuWin) {
                /* GPU profile, average per frame: Gi = us of GE, Ni = triangles, Pi = kpixels (approx. area), Ci = calls.
                 * i: 0 opaque 1 cutout 2 translucent (no fog); 3 4 5 same but with fog (1st pass);
                 * 6 fog pass (opaque/cutout) 7 fog pass (translucent) 8 special (noise/Peach) 9 2D sprites
                 * 10 GE wait between calls (clear, textures...). */
                static const char *const gl[11] = {" G0 ", " G1 ", " G2 ", " G3 ", " G4 ", " G5 ", " G6 ", " G7 ", " G8 ", " G9 ", " G10 "};
                static const char *const nl[11] = {" N0 ", " N1 ", " N2 ", " N3 ", " N4 ", " N5 ", " N6 ", " N7 ", " N8 ", " N9 ", " N10 "};
                static const char *const pl[11] = {" P0 ", " P1 ", " P2 ", " P3 ", " P4 ", " P5 ", " P6 ", " P7 ", " P8 ", " P9 ", " P10 "};
                static const char *const cl[11] = {" C0 ", " C1 ", " C2 ", " C3 ", " C4 ", " C5 ", " C6 ", " C7 ", " C8 ", " C9 ", " C10 "};
                const unsigned int fr = sProfFrames ? sProfFrames : 1;
                int q;
                for (q = 0; q < 11; q++) {
                    prof_put_num(gl[q], (int) (gGpuUs[q] / fr));
                    prof_put_num(nl[q], (int) (gGpuTris[q] / fr));
                    prof_put_num(pl[q], (int) (gGpuPx[q] / 1000u / fr));
                    prof_put_num(cl[q], (int) (gGpuCalls[q] / fr));
                }
            }
            prof_put_str("\r\n");
        } else {
            sProfLostW++; /* it did not fit and could not be written: the next line warns that it should be saved */
        }
        {
            int q;
            for (q = 0; q < 11; q++) { gGpuUs[q] = gGpuTris[q] = gGpuPx[q] = gGpuCalls[q] = 0; }
            sProfGpuWin = gPspGpuProf;
        }
        sProfLogWindows++;
        if ((sProfLogWindows % 5) == 0 || gPspLogStatus == 2) {
            prof_log_flush(); /* every 5 s (or on every window while there is a pending error) */
        }
    }
    sProfLastWall = now;
    sProfDroppedWin = 0;
    sProfSixty = 0;
    sDbgTk = sDbgA = sDbgOver = sDbgFr = sDbgPre = sDbgE = sDbgS = sDbgAsy = 0;
}

/* ---- Dynamic 60 fps (PSP): "at whatever frames it can reach" ----
 * The game logic still runs at 30 Hz. Each tick presents ONE or TWO frames: with two, the first comes out with the interpolated
 * matrices (midpoint between the previous tick and the current one) and the second with the real state. Mixing 1-frame and 2-frame
 * ticks gives intermediate speeds: if the fraction of ticks with 2 frames is p, the image runs at 30 + 30 * p fps
 * (p = 1/2 -> 45 fps, 2/3 -> 50 fps). The maximum is 60.
 * A tick "fits" if A = logic + drawing (CPU + GPU wait, without waiting for the vblank) <= PSP60_BUDGET_US, that is, if it would fit
 * in one vblank (16.7 ms). How often ticks fit is measured (h, 0..1000) and p = h is used: if in a zone all of them fit
 * -> 60 fps; if only half fit -> ~45 fps; if none fits -> 30 fps. An accumulator (Bresenham style) spreads the
 * 2-frame ticks evenly (45 fps = 2,1,2,1...). */
#define PSP_60FPS 1               /* 0 = always 30 fps */
#define PSP_60FPS_FORCE 1         /* 1 = fixed 60 fps: if the scene does not fit, it runs slower (stuttering) but always tries for 60. 0 = dynamic 30/60 */
#define PSP60_BUDGET_US 16000U    /* a 30 fps tick fits if A <= this (one vblank = 16667 us, with a little margin) */
#define PSP60_SLACK_US 1500U      /* "Keep Speed": a 30 fps tick (which carries the PSP60_EXTRA_US estimate) only counts as "does not fit" if it exceeds the
                                   * budget by more than this. In between it counts neither for nor against (see gfx_psp_update_mode). */
#define PSP60_WALL_OK_US 35000U   /* "Keep Speed": a 60 fps tick fits while the REAL time per tick (moving average) does not exceed this (2 vblanks = 33333 us) */
#define PSP60_WALL_CLAMP_US 45000U /* a real tick longer than this (loading, HOME menu, mode change) counts as this, so that a one-off stumble does not weigh too much */
#define PSP60_WALL_SHIFT 3        /* window of the real time per tick average (~8 ticks) */
#define PSP60_EXTRA_US 1000U      /* estimate of the cost of the 2nd frame's matrix patch (added on 1-frame ticks) */
#define PSP60_IGNORE_US 50000U    /* larger samples (level load, HOME menu...) do not count */
#define PSP60_TICK_FIT_US 41000U  /* "Keep Speed": a 60 fps tick fits if it lasted less than this (between 2 vblanks = 33 ms and 3 = 50 ms) */
#define PSP60_DOWN_SHIFT 4        /* si empeora, reacciona en ~8 ticks */
#define PSP60_UP_SHIFT 4          /* same when going up: this way p matches the real fraction of ticks that fit */

/* gfx_psp.c really defines it; this weak definition only avoids the link error if gfx_psp.c is the old version. */
int gPspSixtyMode __attribute__((weak)) = 0;
extern int gPspPresentAsync;           /* gfx_scegu.c: this frame was presented asynchronously (thread) */
extern unsigned int gPspProfPendWait;  /* us that start_frame waited for the previous async presentation */
extern unsigned int gPspAsyncSync;     /* of that presentation: us from end_frame until the GE finished */
extern int gPspPendingDone;
static unsigned int sLastAsyncS;       /* last GE queue measured in an async presentation (us) */
/* Async presentation model of the 2nd frame (see the A calculation in gfx_run): while the thread waits for the GE and the vblank of the 2nd
 * frame, the main thread is already doing the next tick's logic. That logic and the sending of the 2nd frame fit in the gap of a vblank
 * (PSP60_BUDGET_US); only what goes beyond that delays the sending of the 1st frame. With replay, sending the 2nd frame is almost nothing. */
static int sLastF2Async;               /* the previous 2nd frame was presented asynchronously */
static unsigned int sLastF2E = 1000;   /* su parche + envio (us) */
int gPspReplayEma = 0;                 /* 0..1000: recent fraction of 2nd frames that replayed the list (rises by itself at 30 fps to retry) */
static unsigned int sTickPre;          /* longest pre-draw time (logic) of the current tick's frames */
static unsigned int sTickMaxA;
static int sFitRate = 1000; /* thousandths of ticks that fit; starts optimistic and drops quickly if not */
static unsigned int sLastTickUs;     /* clock (us) at the previous call to gfx_psp_update_mode */
static int sWallEma = 33333;         /* moving average of the real time per 60 fps tick (us) */
static unsigned int sAcc;   /* acumulador de p en milesimas */

void gfx_psp_update_mode(void) {
#if PSP_60FPS
    unsigned int a = sTickMaxA;
    sTickMaxA = 0;
    sTickPre = 0;
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
    if (!gPspSixtyMode) {
        gPspReplayEma += (1000 - gPspReplayEma) >> 5; /* at 30 fps there are no samples: it retries from time to time */
    }
#endif
#if PSP_ASYNC_PRESENT
    if (sTailN) {
        const unsigned int t = sTailSum / sTailN;
        sTailEma = sTailEma - (sTailEma >> 3) + (t >> 3);
        sTailSum = sTailN = 0;
        if (sTailEma >= PSP_ASYNC_ON_US) gPspAsyncWant = 1;
        else if (sTailEma <= PSP_ASYNC_OFF_US) gPspAsyncWant = 0;
    }
#endif
    {
        /* REAL time of the tick that just ended (between two calls). On 60 fps ticks it is the direct measure of whether the game is running at
         * its speed (2 vblanks = 33333 us): the A estimate below is pessimistic (at stable 60 fps A can come out above the
         * budget on most ticks), and with it "Keep Speed" dropped below 50 fps where "Keep FPS" ran at a stable 60. */
        const unsigned int now = (unsigned int) sceKernelLibcClock();
        unsigned int w = sLastTickUs ? now - sLastTickUs : 0U;
        sLastTickUs = now;
        if (a != 0 && a < PSP60_IGNORE_US) { /* a == 0: no frame was drawn (skipped), there is no sample */
            int fit; /* 1 = raises the rate, -1 = lowers it, 0 = does not count */
            if (!gPspSixtyMode) {
                a += PSP60_EXTRA_US;
            }
            sDbgTk++; sDbgA += a; if (a > PSP60_BUDGET_US) sDbgOver++;
            if (gPspSixtyMode) {
                /* 60 fps tick: fits if the real time (moving average) does not exceed ~2 vblanks */
                /* The presentation is locked to the vblank, so a 2-frame tick lasts 2 vblanks (33 ms) if it fits and 3 (50 ms) if it does not: each one is
                 * judged on its own REAL time (no moving average, which made the verdict all-or-nothing and the mode jump between 30 and 60). The fraction
                 * of ticks that fit (sFitRate) then settles at the in-between values: 1/3 of them fit -> 40 fps, 1/2 -> 45 fps... */
                if (w != 0U) {
                    if (w > PSP60_WALL_CLAMP_US) w = PSP60_WALL_CLAMP_US;
                    sWallEma += ((int) w - sWallEma) >> PSP60_WALL_SHIFT;
                    fit = w <= PSP60_TICK_FIT_US ? 1 : -1;
                } else {
                    fit = 0;
                }
            } else {
                /* 30 fps tick: there is no real time to look at (always ~33 ms); it is estimated whether it would fit at 60 fps. It only counts if it clearly fits
                 * or clearly does not fit (the estimate carries PSP60_EXTRA_US, pessimistic because the 2nd frame is repeated almost for free). */
                fit = a <= PSP60_BUDGET_US ? 1 : (a > PSP60_BUDGET_US + PSP60_SLACK_US ? -1 : 0);
            }
            if (fit > 0) {
                sFitRate += (1000 - sFitRate) >> PSP60_UP_SHIFT;
            } else if (fit < 0) {
                sFitRate -= sFitRate >> PSP60_DOWN_SHIFT;
            }
            if (sFitRate < 0) sFitRate = 0;
            if (sFitRate > 1000) sFitRate = 1000;
        }
    }
    /* the extremes are rounded so as not to stay at 59 / 31 fps because of the average's tail */
    {
        unsigned int p = (unsigned int) sFitRate;
        if (gPspOptKeepSpeed && p < 940U) {
            /* A 2-frame tick that does not fit delays the game, and in between values the heavy ticks are not the ones that get the 2nd frame: asking for a
             * little less than the measured fraction leaves room, so the speed of the game stays (the image goes a bit under the possible fps). */
            p = p * (1000U + p) / 2000U;
        } else if (gPspOptKeepSpeed) {
            p = 1000U;
        }
        {
            /* Options menu (game/psp_options.c): fraction of ticks with 2 frames that corresponds to the fps limit (30 -> 0, 45 -> 1/2,
             * 60 -> all). "Keep FPS": always that fraction (if it does not fit, the game runs slower); "Keep Speed": at most that fraction and only
             * the ticks that fit (like the dynamic mode from before). */
            const unsigned int cap = (unsigned int) ((psp_opt_fps_cap() - 30) * 1000 / 30);
            if (!gPspOptKeepSpeed || p > cap) p = cap;
        }
        if (p >= 960U) p = 1000U;
        else if (p <= 40U) p = 0U;
        sAcc += p;
        if (sAcc >= 1000U) {
            sAcc -= 1000U;
            gPspSixtyMode = 1;
        } else {
            gPspSixtyMode = 0;
        }
    }
#if PSP_SAFE_WARP
    {
        extern int psp_warp_busy(void);
        if (psp_warp_busy()) {
            gPspSixtyMode = 0; /* level change / transition: normal mode (see level_update.c) */
        }
    }
#endif
#else
    gPspSixtyMode = 0;
#endif
}

/* ---- PSP_F2_REPLAY (option 3, stage 1) ----
 * Role of the frame about to be sent (set by pc_main.c): 0 = normal (30 fps, or a frame that does not take part), 1 = 1st frame of a
 * 60 fps tick (the scene is recorded in the GE list), 2 = 2nd frame (if the recording is valid, it is replayed instead of walking the list again).
 * The list mechanics are in gfx_scegu.c. */
extern void gfx_scegu_rec_reset(void);
extern int gfx_scegu_rec_begin(void);
extern void gfx_scegu_rec_end(void);
extern int gfx_scegu_replay_ready(void);
extern void gfx_scegu_replay_arm(int on);
extern void gfx_scegu_replay_scene(void);
static int sFrameRole;
static int sReplayDecision;  /* lo decide gfx_psp_replay_prepare() y lo consume gfx_run() */
/* Counters (per window in the log): RPP = replayed frames 2 with the real matrices patched; RPM = could not be patched because the
 * matrix annotation did not match; RPD = an object moved more than allowed; RPQ = another patch was pending (HUD, snow...). */
unsigned int gPspRepPatched, gPspRepMis, gPspRepDelta, gPspRepPend;
unsigned int gPspRepSky; /* frames 2 in which the sky was also updated */
unsigned int gPspRepWhy[7]; /* reasons for RPM: [0] not recorded, [1] too many entries, [2] nested MUL, [3] odd block, [4] unmatched, [5] the count changed, [6] other */
static void gfx_invalidate_render_state(void);

void gfx_psp_set_role(int role) {
    sFrameRole = role;
#if PSP_F2_REPLAY
    if (role != 2) {
        gfx_scegu_rec_reset(); /* a recording from a previous tick can never be used */
#if PSP_F2_REPLAY_MTX
        sRecMtxValid = 0;
        sRecMtxOn = 0;
        sRecCur = -1;
#endif
    }
#endif
}

/* Called by pc_main.c on the 2nd frame of a 60 fps tick, right before patch_interpolations() (which empties the matrix table).
 * pending = 1 if the game has any patch that is not just "change an object's matrix". It decides whether the frame is replayed from the
 * GE list (and in that case leaves the real matrices already written) or is drawn in full. */
void gfx_psp_replay_prepare(int pending) {
    sReplayDecision = 0;
#if PSP_F2_REPLAY
    if (sFrameRole != 2) {
        return;
    }
#if PSP_F2_REPLAY_MTX
    {
        const int n = psp_mtx_tbl_count();
        int i;
        if (!sRecMtxValid || n != sRecCount) {
            gPspRepMis++;
            gPspRepWhy[!sRecMtxValid ? sRecWhy : 5]++;
            return;
        }
        if (pending) {
            gPspRepPend++;
            return;
        }
        {
            /* The CPU clips and discards with frame 1's matrices. If an object has triangles against the near plane (sRecNear) and
             * it moved more than a few units in that zone between the two frames, the 2nd frame could leave a gap (old cut) or
             * send the GE a triangle crossing the near without clipping: in that (uncommon) case the scene is emitted again.
             * The other objects (the normal case) are not looked at: a distant object that rotates has nothing to do with the near plane. */
            const float near_z = psp_persp_near();
            const float lim = 0.08f * near_z;
            const float lim2 = lim * lim;
            for (i = 0; i < n; i++) {
                const float *m = psp_mtx_tbl_mtx(i);
                if (m != NULL && sRecNear[i] && !(gfx_rec_mtx_disp2(sRecM[i], m, near_z) <= lim2)) {
                    gPspRepDelta++;
                    return;
                }
            }
        }
        if (!gfx_scegu_replay_ready()) {
            return; /* (cuenta su propio motivo) */
        }
        for (i = 0; i < n; i++) {
            const float *m = psp_mtx_tbl_mtx(i);
            if (m != NULL) {
                gfx_scegu_write_model(sRecOff[i], m);
            }
        }
#if PSP_SKY_PATCH
        if (sRecSkyOk) {
            const float *so = psp_sky_real_ortho();
            if (so != NULL) {
                /* new GU_VIEW = real orthographic x what had accumulated before (same order as gfx_sp_matrix) */
                float ra[16] __attribute__((aligned(16)));
                float rb[16] __attribute__((aligned(16)));
                float rc[16] __attribute__((aligned(16)));
                memcpy(ra, so, sizeof(ra));
                memcpy(rb, sRecSkyPre, sizeof(rb));
                gfx_matrix_mul((float (*)[4]) rc, (const float (*)[4]) ra, (const float (*)[4]) rb);
                gfx_scegu_write_view(sRecSkyOff, rc);
                gPspRepSky++;
            }
        }
#endif
        gPspRepPatched++;
        sReplayDecision = 1;
    }
#else
    (void) pending;
    sReplayDecision = gfx_scegu_replay_ready();
#endif
#else
    (void) pending;
#endif
}

/* pc_main.c: the 2nd frame will replay the 1st one's list (decided in gfx_psp_replay_prepare, before sending) */
int gfx_psp_replay_decided(void) {
    return sReplayDecision;
}

void gfx_start_frame(void) {
    sProfGfx = 0;
    sProfSleep = 0;
    sProfGe = 0;
    //sceIoWrite(1, "----START FRAME!\n", 18);
    total_t0 = sceKernelLibcClock();
    gfx_wapi->handle_events();
}

#if PSP_DEVBENCH == 5
#include <pspctrl.h>
static void psp_devbench5(Gfx *root);
#endif
void gfx_run(Gfx *commands) {
    int replay = 0;
    gfx_sc_touch();
    int recording = 0;
    gfx_sp_reset();
    
    //INFO_MSG("New frame");
    
#if PSP_F2_REPLAY
    if (sFrameRole == 2) {
        replay = sReplayDecision; /* lo decidio gfx_psp_replay_prepare() */
    }
    sReplayDecision = 0;
    gfx_scegu_replay_arm(replay); /* decides now which list start_frame will open */
#endif
    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        sProfDroppedWin++;
        return;
    }
    dropped_frame = false;
    //double t0 = gfx_wapi->get_time();
    unsigned int t0 = sceKernelLibcClock();
    gfx_rapi->start_frame();
#ifdef TARGET_PSP
    {
        extern int gPspTvStateDirty; /* gfx_scegu.c: the previous frame's TV pass changed the GE state */
        if (gPspTvStateDirty) {
            gPspTvStateDirty = 0;
            gfx_invalidate_render_state();
        }
    }
#endif
    unsigned int pend_w = 0; /* us waiting for the previous frame's async presentation (inside start_frame) */
    if (gPspPendingDone) {
        pend_w = gPspProfPendWait;
        sLastAsyncS = gPspAsyncSync;
        gPspPendingDone = 0;
        gPspProfPendWait = 0;
    }
    if (sProfLogOn && sProfFrames == 0) {
        /* Measurement: cost of a sceGuGetMemory call (includes the system call that notifies the GE) and of reading the clock.
         * It is repeated 64 times once per window; it takes a few bytes of the list. Result in tenths of a microsecond. */
        unsigned int tb0, tb1, tb2;
        int bi;
        tb0 = sceKernelGetSystemTimeLow();
        for (bi = 0; bi < 64; bi++) {
            (void) sceGuGetMemory(4);
        }
        tb1 = sceKernelGetSystemTimeLow();
        for (bi = 0; bi < 64; bi++) {
            (void) sceKernelGetSystemTimeLow();
        }
        tb2 = sceKernelGetSystemTimeLow();
        sProfBenchGm = (tb1 - tb0) * 10 / 64;
        sProfBenchClk = (tb2 - tb1) * 10 / 64;
        {
            /* Writing 1024 words to UNCACHED memory (as pspgu writes the GE command list: sceGuStart adds 0x40000000)
             * versus CACHED and flushing afterwards (as gfx_scegu.c does with the vertex arena). Gives the ceiling of what would be saved by
             * moving the list to cached writes. It does not touch the GE list. */
            static unsigned int sBenchBuf[1024] __attribute__((aligned(64)));
            volatile unsigned int *uc = (volatile unsigned int *) ((uintptr_t) sBenchBuf | 0x40000000U);
            volatile unsigned int *cw = (volatile unsigned int *) sBenchBuf;
            unsigned int u0, u1, u2;
            int wi;
            u0 = sceKernelGetSystemTimeLow();
            for (wi = 0; wi < 1024; wi++) uc[wi] = (unsigned int) wi;
            u1 = sceKernelGetSystemTimeLow();
            for (wi = 0; wi < 1024; wi++) cw[wi] = (unsigned int) wi;
            sceKernelDcacheWritebackRange(sBenchBuf, sizeof(sBenchBuf));
            u2 = sceKernelGetSystemTimeLow();
            sProfBenchUc = u1 - u0;
            sProfBenchCw = u2 - u1;
        }
        {
            /* Measurement of the REAL CPU clock: a 3-instruction loop (addiu / bnez / nop) with a dependency chain, of
             * 20000 turns, is repeated 3 times and the smallest time is taken (another thread can interrupt a turn). With the CPU at
             * 333 MHz and 1 instruction per cycle it is ~180 us; if the real clock is higher/lower the time changes proportionally.
             * scePowerGetCpuClockFrequency only says what was REQUESTED (a CFW like ARK may apply something else), this checks it. */
            unsigned int best = 0xFFFFFFFFU;
            int rep;
            for (rep = 0; rep < 3; rep++) {
                unsigned int n = 20000;
                const unsigned int tc0 = sceKernelGetSystemTimeLow();
                __asm__ volatile (".set push\n.set noreorder\n1: addiu %0, %0, -1\n bnez %0, 1b\n nop\n.set pop\n" : "+r"(n));
                {
                    const unsigned int dtc = sceKernelGetSystemTimeLow() - tc0;
                    if (dtc < best) best = dtc;
                }
            }
            sProfBenchCpu = best;
        }
    }
#if PSP_F2_REPLAY
    if (replay) {
        /* 2nd frame: the 1st one's scene is already in the GE list; the renderer state (rendering_state, rsp...) and the GE's
         * were left as at the end of the 1st frame, and the scene leaves them the same again. */
        gfx_scegu_replay_scene();
    } else {
        if (sFrameRole == 1) {
            /* So that the recorded scene does not depend on how the previous frame ended, all cached state is considered lost and
             * is sent to the GE again the first time it is used (same as after a callback frame). */
            gfx_invalidate_render_state();
            recording = gfx_scegu_rec_begin();
#if PSP_F2_REPLAY_MTX
            if (recording) {
                gfx_rec_mtx_begin();
                if (sRecMtxOn) {
                    sRejXY = PSP_REC_XY_MARGIN; gfx_sc_touch();
                }
            }
#endif
        }
        if (sSweepMode < 8) {
            gfx_run_dl(commands);
        }
        gfx_flush();
        if (recording) {
            gfx_scegu_rec_end();
#if PSP_F2_REPLAY_MTX
            gfx_rec_mtx_end();
#endif
        }
        sRejXY = 1.0f; gfx_sc_touch();
    }
#else
    (void) replay;
    (void) recording;
    if (sSweepMode < 8) {
        gfx_run_dl(commands);
    }
    gfx_flush();
#if PSP_DEVBENCH == 5
    {
        /* triggered with L + R + SELECT at once (in the scene to be measured); to test in the emulator, at 500 frames */
        static int sB5N;
        SceCtrlData b5pad;
        sB5N++;
#ifdef PSP_B5_AUTO
#ifndef PSP_B5_AUTO_N
#define PSP_B5_AUTO_N 500
#endif
        if (sB5N == PSP_B5_AUTO_N) psp_devbench5(commands);
#endif
        if (sB5N > 120) {
            b5pad.Buttons = 0;
            sceCtrlPeekBufferPositive(&b5pad, 1);
            if ((b5pad.Buttons & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)) == (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT)) {
                psp_devbench5(commands);
            }
        }
    }
#endif
#endif
    {
        const unsigned int lst = (unsigned int) sceGuCheckList();
        if (lst > sProfListMax) sProfListMax = lst;
    }
    gfx_rapi->end_frame();
#if PSP_F2_REPLAY
    gfx_scegu_replay_arm(0); /* the short list B is only valid for this frame (other uses of the GU, e.g. the HOME menu, use the big one) */
#endif
    {
        const unsigned int ts0 = sceKernelLibcClock();
        sProfGfx = ts0 - t0;
        sProfSyncCur = gPspProfSync;
        sProfVblCur = gPspProfVblank + pend_w;
#if PSP_ASYNC_PRESENT
        {
            /* Cost of a frame with async presentation: the logic (pre) and the GE queue (s) overlap, so a
             * frame fits if  E + max(logic, GE queue) <= vblank,  with E = sending the frame (without waits). In the last frame
             * of a tick the queue is the one measured in the previous tick (the thread has not finished yet). */
            const unsigned int pre = t0 - total_t0;
            const unsigned int e_us = sProfGfx > sProfVblCur + sProfSyncCur ? sProfGfx - sProfVblCur - sProfSyncCur : 0;
            const unsigned int s_used = gPspPresentAsync ? sLastAsyncS : sProfSyncCur;
            unsigned int a;
            if (pre > sTickPre) {
                sTickPre = pre;
            }
            sTailSum += s_used; sTailN++;
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
            if (sFrameRole == 2 && gPspPresentAsync && replay) {
                /* 2nd frame replayed and async: its logic has already passed; what counts is that its GE finishes before the vblank */
                a = pre + e_us + s_used;
            } else if (sFrameRole != 2 && (sLastF2Async || gPspReplayEma >= 600)) {
                /* The previous 2nd frame was presented (or will be presented) asynchronously: this tick's logic + the sending of that frame
                 * overlap with its wait for the GE / vblank. Only what goes beyond one vblank delays the sending of this frame. */
                const unsigned int hide = (sLastF2Async ? sLastF2E : 1000U) + pre;
                a = e_us + s_used + (hide > PSP60_BUDGET_US ? hide - PSP60_BUDGET_US : 0U);
            } else
#endif
            if (gPspAsyncWant) {
                a = e_us + (sTickPre > s_used ? sTickPre : s_used); /* the logic overlaps with the GE queue */
            } else {
                a = sTickPre + e_us + s_used; /* no overlap: logic + send + wait for the GE */
            }
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
            if (sFrameRole == 2) {
                sLastF2Async = gPspPresentAsync ? 1 : 0;
                sLastF2E = pre + e_us;
                gPspReplayEma += ((replay ? 1000 : 0) - gPspReplayEma) >> 3;
            } else {
                sLastF2Async = 0;
            }
#endif
            sDbgFr++; sDbgPre += pre; sDbgE += e_us; sDbgS += s_used; if (gPspPresentAsync) sDbgAsy++;
            if (a > sTickMaxA) {
                sTickMaxA = a;
            }
        }
#else
        {
            const unsigned int render_us = sProfGfx > sProfVblCur ? sProfGfx - sProfVblCur : 0;
            const unsigned int a = (t0 - total_t0) + render_us;
            if (a > sTickMaxA) {
                sTickMaxA = a;
            }
        }
#endif
        gfx_wapi->swap_buffers_begin();
        sProfSleep = sceKernelLibcClock() - ts0;
    }
    //double t1 = gfx_wapi->get_time();
    unsigned int t1 = sceKernelLibcClock();
    //printf("Process %f %f\n", t1, t1 - t0);
    //printf("Process %d microsec, %f sec\n", t1 - t0, (t1 - t0)/1000000.0f);
    times[frame_counter] = (t1 - t0)/1000.0f;
    frame_counter++;
    time_first_200  += (t1 - t0)/1000.0f;
    total_frame_counter++;
    if(frame_counter>=30){
        frame_counter = 0;
        int i;
        for(i=0;i<30;i++)
            time_avg += times[i];
        time_avg /= 30;
        //printf("GFX AVG: %2.3f ms FPS %2.3f\n", time_avg, 1000/time_avg);
    }
    if(total_frame_counter == 200){
        printf("GFX FRAME 250 TIME TAKEN: %2.3f ms FPS %2.3f, AVG: %2.3f ms \n",  time_first_200, (250*1000)/time_first_200, 1000/(250/time_first_200));
    }
}

#if PSP_DEVBENCH
/* ---- Diagnostic on the real PSP (PSP_DEVBENCH 1): ~7 s after boot (frame 400) it measures, with real clocks, how much each stage of
 * vertex/triangle processing costs with fake data, writes ms0:/sm64_devbench.txt and closes the game. Each measurement = the MINIMUM of 3 repetitions
 * (other threads, such as audio, can only add time). "hot" = the same vertices over and over (in the data cache); "cold" = walks
 * 96 KB of vertices (more than the 16 KB cache), as happens in the game. Not used in the normal game. */
#include <stdarg.h>
extern void memcpy_vfpu(void *dst, const void *src, size_t size); /* mixer.c */
static char sDbBuf[24576];
static int sDbLen;
static void db_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (sDbLen < (int)sizeof(sDbBuf) - 200) sDbLen += vsnprintf(sDbBuf + sDbLen, sizeof(sDbBuf) - sDbLen, fmt, ap);
    va_end(ap);
}
#define DB_TIME(var, iters, body)                                                                    \
    do {                                                                                             \
        unsigned int db_best = 0xFFFFFFFFu;                                                          \
        for (int db_rep = 0; db_rep < 3; db_rep++) {                                                 \
            const unsigned int db_t0 = sceKernelGetSystemTimeLow();                                  \
            for (int db_it = 0; db_it < (iters); db_it++) { body; }                                  \
            const unsigned int db_dt = sceKernelGetSystemTimeLow() - db_t0;                          \
            if (db_dt < db_best) db_best = db_dt;                                                    \
        }                                                                                            \
        (var) = (double)db_best * 1000.0 / (double)(iters);                                          \
    } while (0)

#define DB_COLD_N 4096
static Vtx sDbCold[DB_COLD_N] __attribute__((aligned(64)));
static Vtx sDbHot[64] __attribute__((aligned(64)));
static float sDbM[32] __attribute__((aligned(16)));
static unsigned char sDbMemA[32768] __attribute__((aligned(64)));
static unsigned char sDbMemB[32768] __attribute__((aligned(64)));

static void psp_devbench(void) {
    double r;
    unsigned int seed = 12345u;
    for (int i = 0; i < DB_COLD_N; i++) {
        Vtx *v = (i < 64) ? &sDbHot[i] : &sDbCold[i];
        seed = seed * 1664525u + 1013904223u; v->v.ob[0] = (float)((int)(seed >> 20) - 2048) * 0.5f;
        seed = seed * 1664525u + 1013904223u; v->v.ob[1] = (float)((int)(seed >> 20) - 2048) * 0.5f;
        seed = seed * 1664525u + 1013904223u; v->v.ob[2] = (float)((int)(seed >> 20) - 2048) * 0.5f;
        seed = seed * 1664525u + 1013904223u; v->v.flag = 0;
        v->v.tc[0] = (short)(seed >> 16); seed = seed * 1664525u + 1013904223u; v->v.tc[1] = (short)(seed >> 16);
        seed = seed * 1664525u + 1013904223u; v->v.cn[0] = (unsigned char)(seed >> 8); v->v.cn[1] = (unsigned char)(seed >> 16);
        v->v.cn[2] = (unsigned char)(seed >> 24); v->v.cn[3] = (unsigned char)(seed);
    }
    for (int i = 0; i < 64; i++) sDbCold[i] = sDbHot[i];
    for (unsigned k = 0; k < sizeof(sDbMemA); k++) sDbMemA[k] = (unsigned char)(k * 7u);

    db_printf("DEVBENCH %s  CC=%s FALIGN=%d  cpu=%d bus=%d\n", __DATE__ " " __TIME__, PSP_BUILD_OPT, PSP_BUILD_FALIGN,
              scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());

    /* --- costes basicos --- */
    DB_TIME(r, 200000, __asm__ volatile("" ::: "memory"));
    db_printf("loop_empty            %8.1f ns/iter\n", r);
    { unsigned int s = 0; DB_TIME(r, 20000, s += sceKernelGetSystemTimeLow()); db_printf("timer_call            %8.1f ns/call (%u)\n", r, s & 1u); }
    { float x = 1.0f; DB_TIME(r, 20000, { x = x / 1.0001f; x = x / 1.0001f; x = x / 1.0001f; x = x / 1.0001f; __asm__ volatile("" : "+f"(x)); });
      db_printf("fdiv_chain            %8.1f ns/div\n", r / 4.0); }
    { float x = 1.0f; DB_TIME(r, 20000, { x = x * 1.0001f + 0.0001f; x = x * 1.0001f + 0.0001f; x = x * 1.0001f + 0.0001f; x = x * 1.0001f + 0.0001f; __asm__ volatile("" : "+f"(x)); });
      db_printf("fmul_add_chain        %8.1f ns/(mul+add)\n", r / 4.0); }

    /* --- memory --- */
    { volatile unsigned int *p = (volatile unsigned int *)sDbCold; unsigned int s = 0; int pos = 0;
      DB_TIME(r, 1536 * 4, { s += p[pos]; pos += 16; if (pos >= (int)(sizeof(sDbCold) / 4)) pos = 0; });
      db_printf("read_cold_64B_line    %8.1f ns/line (%u)\n", r, s & 1u); }
    { volatile unsigned int *p = (volatile unsigned int *)sDbHot; unsigned int s = 0; int pos = 0;
      DB_TIME(r, 1536 * 4, { s += p[pos]; pos += 16; if (pos >= 1024) pos = 0; });
      db_printf("read_hot_64B_line     %8.1f ns/line (%u)\n", r, s & 1u); }
    DB_TIME(r, 200, memcpy_vfpu(sDbMemB, sDbMemA, sizeof(sDbMemA)));
    db_printf("memcpy_vfpu_32KB      %8.1f ns/KB\n", r / 32.0);
    DB_TIME(r, 200, memcpy(sDbMemB, sDbMemA, sizeof(sDbMemA)));
    db_printf("memcpy_libc_32KB      %8.1f ns/KB\n", r / 32.0);

    /* --- writes to cold memory (what the game does when copying vertices to the arena) --- */
    {
        static unsigned char sDbWr[98304] __attribute__((aligned(64)));
        int off = 0;
        /* 1 KB per iteration, walking 96 KB: always lines that are not in the cache */
        DB_TIME(r, 4000, { unsigned char *d = sDbWr + off;
            for (int q = 0; q < 1024; q += 64) __asm__ volatile("vzero.q c000\n sv.q c000, 0(%0)\n sv.q c000, 16(%0)\n sv.q c000, 32(%0)\n sv.q c000, 48(%0)\n" :: "r"(d + q) : "memory");
            off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("write_cold_cached_1KB %8.1f ns/KB  (sv.q, data cache)\n", r);
        off = 0;
        DB_TIME(r, 4000, { unsigned char *d = (unsigned char *)((unsigned int)(sDbWr + off) | 0x40000000u);
            for (int q = 0; q < 1024; q += 64) __asm__ volatile("vzero.q c000\n sv.q c000, 0(%0)\n sv.q c000, 16(%0)\n sv.q c000, 32(%0)\n sv.q c000, 48(%0)\n" :: "r"(d + q) : "memory");
            off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("write_cold_uncached   %8.1f ns/KB  (sv.q, uncached address)\n", r);
        off = 0;
        DB_TIME(r, 4000, { memcpy_vfpu(sDbWr + off, sDbMemA, 1024); off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("copy_hot_to_cold_1KB  %8.1f ns/KB  (memcpy_vfpu, source in cache -> cold destination)\n", r);
        off = 0;
        DB_TIME(r, 4000, { memcpy_vfpu(sDbWr + off, sDbMemA, 1024); sceKernelDcacheWritebackRange(sDbWr + off, 1024); off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("copy+writeback_1KB    %8.1f ns/KB  (like the game's vertex arena)\n", r);
        off = 0;
        DB_TIME(r, 1000, { memcpy_vfpu(sDbWr + off, sDbMemA, 4096); sceKernelDcacheWritebackRange(sDbWr + off, 4096); off += 4096; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("copy+writeback_4KB    %8.1f ns/KB\n", r / 4.0);
        DB_TIME(r, 20000, sceKernelDcacheWritebackRange(sDbWr, 64));
        db_printf("writeback_range_call  %8.1f ns/call (64 B clean)\n", r);
    }

    /* --- matrices de prueba (finitas, normales) --- */
    memset(sDbM, 0, sizeof(sDbM));
    sDbM[0] = 0.001f; sDbM[5] = 0.001f; sDbM[10] = 0.001f; sDbM[15] = 1.0f; sDbM[11] = 0.0003f;
    sDbM[16] = sDbM[21] = sDbM[26] = sDbM[31] = 1.0f;

    /* --- standalone transform kernels (without the rest of gfx_sp_vertex_impl) --- */
    {
        const int N = 14;
        int off = 0;
        DB_TIME(r, 20000, gfx_transform_vertices_eye_vfpu(&rsp.loaded_vertices[0], sDbHot, N, sDbM));
        db_printf("kern_eye   hot  n=14 %8.1f ns/vtx\n", r / N);
        DB_TIME(r, 20000, { gfx_transform_vertices_eye_vfpu(&rsp.loaded_vertices[0], &sDbCold[off], N, sDbM); off += N; if (off > DB_COLD_N - N) off = 64; });
        db_printf("kern_eye   cold n=14 %8.1f ns/vtx\n", r / N);
        DB_TIME(r, 20000, gfx_transform_vertices_eye2_vfpu(&rsp.loaded_vertices[0], sDbHot, N, sDbM));
        db_printf("kern_eye2  hot  n=14 %8.1f ns/vtx\n", r / N);
        off = 64;
        DB_TIME(r, 20000, { gfx_transform_vertices_eye2_vfpu(&rsp.loaded_vertices[0], &sDbCold[off], N, sDbM); off += N; if (off > DB_COLD_N - N) off = 64; });
        db_printf("kern_eye2  cold n=14 %8.1f ns/vtx\n", r / N);
    }

    /* --- gfx_sp_vertex_impl completo --- */
    {
        const uint32_t save_gm = rsp.geometry_mode;
        rsp.current_num_lights = 2;
        rsp.current_lights[0].col[0] = 200; rsp.current_lights[0].col[1] = 180; rsp.current_lights[0].col[2] = 160;
        rsp.current_lights[0].dir[0] = 40; rsp.current_lights[0].dir[1] = 80; rsp.current_lights[0].dir[2] = 60;
        rsp.current_lights[1].col[0] = 60; rsp.current_lights[1].col[1] = 50; rsp.current_lights[1].col[2] = 40;
        sEyeOn = 1; sMVDirty = 0;
        memcpy(rsp.MP_matrix, sDbM, sizeof(rsp.MP_matrix)); sXfmMpDirty = 1;
        memset(&sXfm[16], 0, 64); sXfm[16] = sXfm[21] = sXfm[26] = sXfm[31] = 1.0f;
        static const char *names[4] = { "xfm-only", "unlit", "lit", "lit+texgen" };
        static const size_t sizes[2] = { 4, 14 };
        for (int mode = 0; mode < 4; mode++) {
            for (int si = 0; si < 2; si++) {
                const size_t n = sizes[si];
                sSweepMode = (mode == 0) ? 6 : 0;
                rsp.geometry_mode = (mode <= 1) ? 0 : (mode == 2 ? G_LIGHTING : (G_LIGHTING | G_TEXTURE_GEN));
                rsp.lights_changed = true;
                int it2 = 0;
                DB_TIME(r, 20000, { rsp.lights_changed = (it2++ & 7) == 0; gfx_sp_vertex_impl(n, 0, sDbHot); });
                const double hot = r / (double)n;
                int off = 64;
                DB_TIME(r, 20000, { rsp.lights_changed = (it2++ & 7) == 0; gfx_sp_vertex_impl(n, 0, &sDbCold[off]); off += (int)n; if (off > DB_COLD_N - 32) off = 64; });
                db_printf("vtx_impl %-10s n=%2d  hot %7.1f  cold %7.1f ns/vtx\n", names[mode], (int)n, hot, r / (double)n);
            }
        }
        sSweepMode = 0;
        rsp.geometry_mode = save_gm;
    }

    /* --- triangles, without touching the GE: Profile Sweep 5 = returns on entering gfx_sp_tri1 (cost of the call), 4 = up to before preparing the
     * state (discard, scissor, clipping) --- */
    {
        struct LoadedVertex *lv = &rsp.loaded_vertices[0];
        for (int k = 0; k < 6; k++) {
            lv[k].x = (k & 1) ? 10.0f : -10.0f; lv[k].y = (k & 2) ? 8.0f : -8.0f; lv[k].z = -100.0f - (float)k; lv[k].w = 1.0f;
            lv[k]._x = (k & 1) ? 0.2f : -0.2f; lv[k]._y = (k & 2) ? 0.1f : -0.1f; lv[k]._z = 50.0f; lv[k]._w = 100.0f;
            lv[k].u = 100.0f * k; lv[k].v = 50.0f * k;
            lv[k].color.r = 100; lv[k].color.g = 120; lv[k].color.b = 140; lv[k].color.a = 255;
            lv[k].clip_rej = 0;
        }
        rsp.geometry_mode = G_ZBUFFER | G_SHADE | G_CULL_BACK;
        for (int sm = 5; sm >= 4; sm--) {
            for (int pass = 0; pass < 2; pass++) {
                for (int k = 0; k < 6; k++) lv[k].clip_rej = pass ? X_POS : 0;
                sSweepMode = sm;
                DB_TIME(r, 20000, { gfx_sp_tri1(0, 1, 2); gfx_sp_tri1(1, 3, 2); });
                db_printf("tri sweep=%d %-14s %7.1f ns/tri\n", sm, pass ? "all-outside" : "in-view", r / 2.0);
            }
        }
        sSweepMode = 0;
    }

    /* --- instruction cache: the same triangle (Profile Sweep 4, on screen) alone, with vertices before each pair of triangles
     * (other code in between) and both things separately. If "mixed" comes out much higher than the sum, the code gets in its own way in the cache. */
    {
        struct LoadedVertex *lv = &rsp.loaded_vertices[0];
        for (int k = 0; k < 6; k++) lv[k].clip_rej = 0;
        rsp.geometry_mode = G_ZBUFFER | G_SHADE | G_CULL_BACK;
        double t_tri, t_vtx, t_mix;
        sSweepMode = 4;
        DB_TIME(t_tri, 20000, { gfx_sp_tri1(0, 1, 2); gfx_sp_tri1(1, 3, 2); });
        sSweepMode = 0;
        rsp.geometry_mode = G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING;
        int off = 64;
        DB_TIME(t_vtx, 20000, { gfx_sp_vertex_impl(4, 10, &sDbCold[off]); off += 4; if (off > DB_COLD_N - 8) off = 64; });
        off = 64;
        for (int k = 0; k < 6; k++) lv[k].clip_rej = 0;
        DB_TIME(t_mix, 20000, {
            sSweepMode = 0; rsp.geometry_mode = G_ZBUFFER | G_SHADE | G_CULL_BACK | G_LIGHTING;
            gfx_sp_vertex_impl(4, 10, &sDbCold[off]); off += 4; if (off > DB_COLD_N - 8) off = 64;
            sSweepMode = 4; rsp.geometry_mode = G_ZBUFFER | G_SHADE | G_CULL_BACK;
            gfx_sp_tri1(0, 1, 2); gfx_sp_tri1(1, 3, 2); });
        sSweepMode = 0;
        db_printf("icache: tri_x2 alone %7.1f  vtx_x4(lit) alone %7.1f  mixed %7.1f  (excess %7.1f ns)\n", t_tri, t_vtx, t_mix, t_mix - t_tri - t_vtx);
    }

    db_printf("END\n");
    {
        SceUID fd = sceIoOpen("ms0:/sm64_devbench.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) { sceIoWrite(fd, sDbBuf, sDbLen); sceIoClose(fd); }
    }
    sceKernelExitGame();
}
#endif

#if PSP_DEVBENCH == 5
extern int psp_me_debug(int which);
static struct GfxRenderingAPI sB5Rapi;
static struct GfxRenderingAPI *sB5RapiSaved;
static void b5n_bool(bool a) { (void) a; }
static void b5n_rect(int a, int b, int c, int d) { (void) a; (void) b; (void) c; (void) d; }
static void b5n_sel(int t, uint32_t id) { (void) t; (void) id; }
static void b5n_samp(int s, bool l, uint32_t a, uint32_t b) { (void) s; (void) l; (void) a; (void) b; }
static void b5n_up(const uint8_t *buf, int w, int h, unsigned int t) { (void) buf; (void) w; (void) h; (void) t; }
static uint32_t b5n_new(void) { return 0; }
static void b5n_draw(float *b, size_t l, size_t n) { (void) b; (void) l; (void) n; }

/* Microtest: the first triangle of each class (fog 0..3 x shade_only x texture) that passes the discard cleanly is repeated in isolation, with the real frame's
 * state and everything in cache, with each gfx_tri_core variant: "hot" cost of the output code, and the footprint of the buffer it produces. */
static const int sB5Vars[4] = { 0, 1, 3, 7 };
static double sB5MicroNs[16][4];
static unsigned int sB5MicroH[16][4];
static unsigned int sB5MicroN[16];
/* Writes what has accumulated so far to ms0:/sm64_devbench.txt (if the console hangs midway, what it got to is kept). */
static void b5_save(void) {
    SceUID fd = sceIoOpen("ms0:/sm64_devbench.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, sDbBuf, sDbLen); sceIoClose(fd); }
}
static unsigned int sB5FuzzN, sB5FuzzBad[2];
/* Equivalence test: triangles with random z/w, colors, fog mul/offset (includes w almost 0, negative, out of range) with the 3 output variants;
 * variant 1 (the usual loop) is the reference and 3 and 7 must give exactly the same bytes in the buffer and the same fog factors. */
static void b5_fuzz(const struct LoadedVertex *tmpl) {
    static struct LoadedVertex fv[3] __attribute__((aligned(16)));
    static unsigned char ref[3 * sizeof(psp_fast_t) + 3];
    static unsigned char got[3 * sizeof(psp_fast_t) + 3];
    struct TriPipelineState *st = &rendering_state.tri_pipeline;
    const int save_fm = st->fog_mode, save_sel = sTriVarSel;
    const int16_t save_mul = rsp.fog_mul, save_off = rsp.fog_offset;
    unsigned int rng = 0x9E3779B9u ^ (unsigned int)sB5FuzzN;
    int fm, it, vi, q;
#define B5RND() (rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5, rng)
    gfx_flush();
    for (fm = 1; fm <= 3; fm++) {
        st->fog_mode = fm;
        for (it = 0; it < 1500; it++) {
            for (q = 0; q < 3; q++) {
                const unsigned int r = B5RND();
                float w, z;
                fv[q] = tmpl[q];
                switch (r & 7u) {
                    case 0: w = 0.0005f * (float)((int)((r >> 8) & 3u) - 1); break;      /* casi 0 / 0 / negativa pequena */
                    case 1: w = -(float)((r >> 8) & 1023u) * 0.5f; break;               /* negativa */
                    default: w = 1.0f + (float)((r >> 8) & 4095u) * 0.25f; break;       /* normal */
                }
                {
                    const unsigned int r2 = B5RND();
                    const float u = ((float)(int)(r2 & 0xffffu) - 32768.0f) * (1.0f / 20000.0f); /* -1.6 .. 1.6 */
                    z = w * u;
                    if ((r2 >> 16) % 13u == 0) z = (float)(int)(r2 >> 16) * 7.0f;        /* disparado */
                }
                fv[q]._w = w;
                fv[q]._z = z;
                {
                    const unsigned int r3 = B5RND();
                    fv[q].color.r = (uint8_t)r3; fv[q].color.g = (uint8_t)(r3 >> 8); fv[q].color.b = (uint8_t)(r3 >> 16); fv[q].color.a = (uint8_t)(r3 >> 24);
                }
            }
            {
                const unsigned int r4 = B5RND();
                if (r4 & 1u) {
                    rsp.fog_mul = (int16_t)(-(int)(r4 >> 8 & 0xfffu) - 1); rsp.fog_offset = (int16_t)((r4 >> 20) & 0xfffu);   /* parecido a lo real */
                } else {
                    rsp.fog_mul = (int16_t)(r4 >> 8); rsp.fog_offset = (int16_t)(r4 >> 16);
                }
            }
            for (vi = 0; vi < 3; vi++) {
                const int var = (vi == 0) ? 1 : (vi == 1 ? 3 : 7);
                sTriVarSel = var;
                gfx_flush();
                memset(scegu_fog_alpha, 0xEE, 3);
                sTriPrechecked = 1;
                gfx_tri_core(&fv[0], &fv[1], &fv[2], 0, 0.0f);
                if (buf_num_vert != 3) { sB5FuzzBad[vi ? vi - 1 : 0] += (vi != 0); continue; }
                memcpy(vi == 0 ? ref : got, buf_vbo, 3 * sizeof(psp_fast_t));
                memcpy((vi == 0 ? ref : got) + 3 * sizeof(psp_fast_t), scegu_fog_alpha, 3);
                if (vi) {
                    sB5FuzzN++;
                    if (memcmp(ref, got, sizeof(ref)) != 0) sB5FuzzBad[vi - 1]++;
                }
            }
            gfx_flush();
        }
    }
#undef B5RND
    st->fog_mode = save_fm;
    sTriVarSel = save_sel;
    rsp.fog_mul = save_mul; rsp.fog_offset = save_off;
    sTriPrechecked = 0;
}

static void b5_micro_capture(struct LoadedVertex *t1, struct LoadedVertex *t2, struct LoadedVertex *t3) {
    static struct LoadedVertex tv[3] __attribute__((aligned(16)));
    gfx_prepare_tri_pipeline_state();
    const struct TriPipelineState *st = &rendering_state.tri_pipeline;
    const struct ColorCombiner *cb = st->comb;
    if (cb == NULL) return;
    const int shade = cb->vertex_color_source[0] == CC_SHADE && (!st->use_alpha || cb->vertex_color_source[1] == CC_SHADE) &&
                      cb->vertex_color_source[0] != CC_LOD && cb->vertex_color_source[1] != CC_LOD && !cb->env_alpha && !cb->enva_tex;
    const int key = (st->fog_mode & 3) * 4 + (shade ? 2 : 0) + (st->use_texture ? 1 : 0);
    sB5MicroN[key]++;
    if (sB5CapSeen[key]) return;
    if (cb->vertex_color_source[0] == CC_LOD || cb->vertex_color_source[1] == CC_LOD) return;
    if ((t1->clip_rej | t2->clip_rej | t3->clip_rej) & CLIP_TEST_FLAGS) return;
    if (!(t1->clip_rej & t2->clip_rej & t3->clip_rej & SC_IN) && gfx_scissor_planes() > 0) return;
    sB5CapSeen[key] = 1;
    gfx_flush();
    tv[0] = *t1; tv[1] = *t2; tv[2] = *t3;
    /* No decal adjustment during the microtest and the random test: that adjustment sends REAL commands to the GE when it changes (with random vertices
     * there would be thousands and they would overflow the list). */
    const bool saveDecal = rendering_state.decal_mode;
    rendering_state.decal_mode = false;
    db_printf("STAGE capturing class %d: random test\n", key); b5_save();
    b5_fuzz(tv);
    db_printf("STAGE class %d: microtest\n", key); b5_save();
    const int saveVar = sTriVarSel;
    for (int vi = 0; vi < 4; vi++) {
        double best = 1.0e30;
        sTriVarSel = sB5Vars[vi];
        sB5HashV = 0; sB5Hash = 1;
        for (int q = 0; q < 3; q++) { sTriPrechecked = 1; gfx_tri_core(&tv[0], &tv[1], &tv[2], 0, 0.0f); }
        gfx_flush();
        sB5Hash = 0;
        sB5MicroH[key][vi] = sB5HashV;
        for (int rep = 0; rep < 4; rep++) {
            int cnt = 0;
            for (int q = 0; q < 200; q++) { sTriPrechecked = 1; gfx_tri_core(&tv[0], &tv[1], &tv[2], 0, 0.0f); if (++cnt == 9) { cnt = 0; gfx_flush(); } }
            gfx_flush();
            const unsigned int u0 = sceKernelGetSystemTimeLow();
            for (int q = 0; q < 2000; q++) { sTriPrechecked = 1; gfx_tri_core(&tv[0], &tv[1], &tv[2], 0, 0.0f); if (++cnt == 9) { cnt = 0; gfx_flush(); } }
            const unsigned int u1 = sceKernelGetSystemTimeLow();
            gfx_flush();
            const double ns = (double)(u1 - u0) * 1000.0 / 2000.0;
            if (ns < best) best = ns;
        }
        sB5MicroNs[key][vi] = best;
    }
    sTriVarSel = saveVar;
    sTriPrechecked = 0;
    rendering_state.decal_mode = saveDecal;
}

/* Diagnostic 5 (user mode only, no kernel involved): replays the real frame several times cutting or removing parts of the work.
 * The time difference between two configurations is what the removed part costs. Nothing reaches the GE.
 * Each configuration is measured once cold (caches flushed right before) and 5 times hot (the smallest time is taken). */
static const char *b5_opname(unsigned int o) {
    switch (o) {
        case G_MTX: return "MTX";
        case (uint8_t)G_POPMTX: return "POPMTX";
        case G_MOVEMEM: return "MOVEMEM";
        case (uint8_t)G_MOVEWORD: return "MOVEWORD";
        case (uint8_t)G_TEXTURE: return "TEXTURE";
        case G_VTX: return "VTX";
        case G_DL: return "DL";
        case (uint8_t)G_TRI1: return "TRI1";
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
        case (uint8_t)G_TRI2: return "TRI2";
#endif
#ifdef F3DEX_GBI_2
        case G_GEOMETRYMODE: return "GEOMMODE";
#else
        case (uint8_t)G_SETGEOMETRYMODE: return "SETGEOM";
        case (uint8_t)G_CLEARGEOMETRYMODE: return "CLRGEOM";
#endif
        case (uint8_t)G_SETOTHERMODE_L: return "OTHERMODE_L";
        case (uint8_t)G_SETOTHERMODE_H: return "OTHERMODE_H";
        case G_SETTIMG: return "SETTIMG";
        case G_LOADBLOCK: return "LOADBLOCK";
        case G_LOADTILE: return "LOADTILE";
        case G_SETTILE: return "SETTILE";
        case G_SETTILESIZE: return "SETTILESZ";
        case G_LOADTLUT: return "LOADTLUT";
        case G_SETENVCOLOR: return "ENVCOLOR";
        case G_SETPRIMCOLOR: return "PRIMCOLOR";
        case G_SETFOGCOLOR: return "FOGCOLOR";
        case G_SETFILLCOLOR: return "FILLCOLOR";
        case G_SETCOMBINE: return "COMBINE";
        case G_TEXRECT: return "TEXRECT";
        case G_FILLRECT: return "FILLRECT";
        case G_SETSCISSOR: return "SCISSOR";
        case G_SETZIMG: return "ZIMG";
        case G_SETCIMG: return "CIMG";
        default: return "?";
    }
}

static void psp_devbench5(Gfx *root) {
    enum { NC = 22, ROUNDS = 6 };
    /* sweep, cull, abl, triangle output variant, matrix variant */
    static const int cfg[NC][5] = {
        { 7, 0, 0, 0, 0 }, { 6, 0, 0, 0, 0 }, { 5, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 4, 0, 0, 0, 0 }, { 3, 0, 0, 0, 0 }, { 2, 0, 0, 0, 0 },
        { 5, 0, 1 << 6, 0, 0 },
        { 2, 0, 0, 0, 0 }, { 2, 0, 1 << 0, 0, 0 }, { 2, 0, 1 << 1, 0, 0 }, { 2, 0, 1 << 2, 0, 0 }, { 2, 0, 1 << 3, 0, 0 }, { 2, 0, 1 << 4, 0, 0 }, { 2, 0, 1 << 5, 0, 0 }, { 2, 0, 7, 0, 0 },
        { 2, 0, 0, 3, 0 }, { 2, 0, 0, 3, 0 }, { 2, 0, 0, 3, 1 }, { 2, 0, 0, 3, 2 }, { 2, 0, 0, 3, 3 },
        { 2, 0, 1 << 8, 3, 0 } };
    static const char *cname[NC] = {
        "A0 walking the list (no vertices or triangles)", "A1 + vertex transformation", "A2 + lighting/rest of the vertices",
        "A3 + triangle dispatch and discard (clip/back face)", "A4 + gfx_tri_core prelude (scissor, clipping)", "A5 + prepare/enva/lod",
        "A6 + vertex output loop + flush (everything but the drawing)", "V  walk + preparation of each vertex load (without transforming)",
        "B0 control (usual output): everything but the drawing", "B1 without per-vertex fog", "B2 without texture u/v", "B3 without copying color", "B4 without calling scegu_fog_mode",
        "B5 without scissor test", "B6 without gfx_prepare_tri_pipeline_state", "B7 minimal output (no fog, u/v or color)",
        "P  production (patch 113): output variant 3", "P2 same as P (measures the noise)",
        "M1 production + lazy matrix MP", "M2 production + gfx_update_mv without copies", "M3 production + M1 + M2",
        "D8 production without G_MTX / G_POPMTX" };
    static unsigned int tcold[NC], thot[NC];
    static unsigned int hist[12], nVtx, nSub, nPass, nDraw, nCmd;
    const int saveSweep = sSweepMode;
    const int saveProf = sProfLogOn;
    int c, rd, k;
    db_printf("DEVBENCH5 %s  CC=%s FALIGN=%d  cpu=%d bus=%d\n", __DATE__ " " __TIME__, PSP_BUILD_OPT, PSP_BUILD_FALIGN,
              scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt());
    sB5Rapi = *gfx_rapi;
    sB5Rapi.set_depth_test = b5n_bool;
    sB5Rapi.set_depth_mask = b5n_bool;
    sB5Rapi.set_zmode_decal = b5n_bool;
    sB5Rapi.set_use_alpha = b5n_bool;
    sB5Rapi.set_viewport = b5n_rect;
    sB5Rapi.set_scissor = b5n_rect;
    sB5Rapi.select_texture = b5n_sel;
    sB5Rapi.set_sampler_parameters = b5n_samp;
    sB5Rapi.upload_texture = b5n_up;
    sB5Rapi.new_texture = b5n_new;
    sB5Rapi.draw_triangles = b5n_draw;
    sB5RapiSaved = gfx_rapi;
    gfx_rapi = &sB5Rapi;
    sProfLogOn = 0;
    for (c = 0; c < NC; c++) thot[c] = 0xFFFFFFFFu;
    {
    int stage;
    for (stage = 0; stage < 2; stage++) {
    const int clo = stage ? 16 : 0, chi = stage ? NC : 16;
    db_printf("STAGE configurations %d..%d\n", clo, chi - 1); b5_save();
    for (rd = 0; rd < ROUNDS; rd++) {
        for (c = clo; c < chi; c++) {
            unsigned int u0, u1, v0, s0, p0, cm0, h0[12];
            sSweepMode = cfg[c][0];
            sB5Cull = cfg[c][1];
            sB5Abl = cfg[c][2];
            sTriVarSel = cfg[c][3];
            sMtxVar = cfg[c][4];
            if (rd == 0) { /* repeticion en frio */
                sceKernelDcacheWritebackInvalidateAll();
                sceKernelIcacheInvalidateAll();
            }
            sB5Cnt[3] = 0;
            v0 = sPcVtx; s0 = sPcTriSub; p0 = sPcTri; cm0 = sB5NCmd;
            for (k = 0; k < 12; k++) h0[k] = sB5H[k];
            gfx_sp_reset();
            u0 = sceKernelGetSystemTimeLow();
            gfx_run_dl(root);
            gfx_flush();
            u1 = sceKernelGetSystemTimeLow();
            if (rd == 0) {
                tcold[c] = u1 - u0;
                if (c == 8) {
                    nVtx = sPcVtx - v0; nSub = sPcTriSub - s0; nDraw = sPcTri - p0; nCmd = sB5NCmd - cm0;
                    for (k = 0; k < 12; k++) hist[k] = sB5H[k] - h0[k];
                }
                if (c == 3) nPass = sB5Cnt[3];
            } else if ((u1 - u0) < thot[c]) {
                thot[c] = u1 - u0;
            }
        }
    }
    for (c = clo; c < chi; c++) db_printf("RAW %d %u %u\n", c, tcold[c], thot[c]);
    if (stage == 0) db_printf("RAW-CNT vtx %u sub %u pass %u draw %u cmd %u hist %u %u %u %u %u %u %u %u %u %u\n", nVtx, nSub, nPass, nDraw, nCmd,
                              hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
    b5_save();
    }
    }
    sB5Cull = 0;
    sB5Abl = 0;
    db_printf("STAGE whole-frame footprints\n"); b5_save();
    /* footprint of the output buffer of the WHOLE frame with each variant (they must be equal: same result bit for bit) */
    {
        unsigned int hv[7];
        static const int hc[7][2] = { { 0, 0 }, { 1, 0 }, { 3, 0 }, { 7, 0 }, { 3, 1 }, { 3, 2 }, { 3, 3 } };
        int vi, pass, same = 1;
        sSweepMode = 2;
        for (vi = 0; vi < 7; vi++) {
            sTriVarSel = hc[vi][0];
            sMtxVar = hc[vi][1];
            for (pass = 0; pass < 2; pass++) {
                sB5HashV = 0; sB5Hash = 1;
                gfx_sp_reset();
                gfx_run_dl(root);
                gfx_flush();
                sB5Hash = 0;
            }
            hv[vi] = sB5HashV;
            if (hv[vi] != hv[0]) same = 0;
        }
        sTriVarSel = 3;
        sMtxVar = 0;
        db_printf("whole-frame buffer footprint (output/matrices): 0/0 %08x  1/0 %08x  3/0 %08x  7/0 %08x  3/1 %08x  3/2 %08x  3/3 %08x   %s\n",
                  hv[0], hv[1], hv[2], hv[3], hv[4], hv[5], hv[6], same ? "EQUAL" : "DIFFERENT");
        db_printf("STAGE per-command times\n"); b5_save();
        {
            /* time of each command type in the list (inclusive: G_DL includes what it executes) and of parts of the matrix path */
            unsigned int t0, t1, ntm = 0, order[256], oi, oj, nord = 0;
            double ovns;
            memset(sB5OpAcc, 0, sizeof(sB5OpAcc)); memset(sB5OpN, 0, sizeof(sB5OpN));
            memset(sB5Sec, 0, sizeof(sB5Sec)); memset(sB5SecN, 0, sizeof(sB5SecN));
            t0 = sceKernelGetSystemTimeLow();
            for (k = 0; k < 2000; k++) ntm += sceKernelGetSystemTimeLow() & 1u;
            t1 = sceKernelGetSystemTimeLow();
            ovns = (double) (t1 - t0) * 1000.0 / 2000.0;
            sB5OpOn = 1;
            for (pass = 0; pass < 3; pass++) {
                gfx_sp_reset();
                gfx_run_dl(root);
                gfx_flush();
            }
            sB5OpOn = 0;
            for (oi = 0; oi < 256; oi++) if (sB5OpN[oi]) order[nord++] = oi;
            for (oi = 0; oi < nord; oi++) for (oj = oi + 1; oj < nord; oj++) if (sB5OpAcc[order[oj]] > sB5OpAcc[order[oi]]) { unsigned int tq = order[oi]; order[oi] = order[oj]; order[oj] = tq; }
            db_printf("time per command type, clock overhead subtracted (%.0f ns per read), per frame; %u\n", ovns, ntm & 1u);
            for (oi = 0; oi < nord && oi < 16; oi++) {
                const unsigned int o = order[oi];
                const double n = (double) sB5OpN[o] / 3.0;
                const double us = ((double) sB5OpAcc[o] / 3.0) - n * ovns / 1000.0;
                db_printf("  op 0x%02x %-10s %6.0f cmds  %8.0f us  %7.0f ns/cmd\n", o, b5_opname(o), n, us, n > 0 ? us * 1000.0 / n : 0.0);
            }
            {
                static const char *sn[8] = { "gfx_update_mv", "lights (calculate_normal_dir)", "copy MP -> sXfm", "", "", "lazy MP flush", "", "" };
                for (oi = 0; oi < 8; oi++) {
                    if (!sB5SecN[oi]) continue;
                    {
                        const double n = (double) sB5SecN[oi] / 3.0;
                        const double us = ((double) sB5Sec[oi] / 3.0) - n * ovns / 1000.0;
                        db_printf("  section %-30s %6.0f times  %8.0f us  %7.0f ns/time\n", sn[oi], n, us, n > 0 ? us * 1000.0 / n : 0.0);
                    }
                }
            }
        }
#ifdef PSP_B5_MICRO
        /* isolated microtest per triangle class (emulator only: on the real PSP it hung) */
        db_printf("STAGE class capture\n"); b5_save();
        sB5Cap = 1;
        gfx_sp_reset();
        gfx_run_dl(root);
        gfx_flush();
        sB5Cap = 0;
#endif
        db_printf("STAGE prepare times\n"); b5_save();
        /* times of the sections of gfx_prepare_tri_pipeline_state (each timestamp costs a clock call: it is subtracted) */
        {
            unsigned int t0, t1, ov, ntm = 0;
            double ovns;
            memset(sB5PAcc, 0, sizeof(sB5PAcc));
            sB5PN = 0;
            t0 = sceKernelGetSystemTimeLow();
            for (k = 0; k < 2000; k++) ntm += sceKernelGetSystemTimeLow() & 1u;
            t1 = sceKernelGetSystemTimeLow();
            ov = t1 - t0;
            ovns = (double) ov * 1000.0 / 2000.0;
            sB5PrepOn = 1;
            for (pass = 0; pass < 3; pass++) {
                gfx_sp_reset();
                gfx_run_dl(root);
                gfx_flush();
            }
            sB5PrepOn = 0;
            db_printf("real calls of gfx_prepare_tri_pipeline_state (dirty state): %u per frame (clock: %.0f ns per call, %u)\n", sB5PN / 3, ovns, ntm & 1u);
            {
                static const char *pn[6] = { "", "GE state (depth/zmode/viewport/scissor)", "combiner + shader + alpha", "textures (peek/import/sampler)",
                                             "texture scale", "fog mode (new)" };
                for (k = 1; k < 6; k++) {
                    const double per = sB5PN ? (double) sB5PAcc[k] * 1000.0 / (double) sB5PN - ovns : 0.0;
                    db_printf("  prepare section %-44s %8.0f ns per call\n", pn[k], per);
                }
            }
        }
    }
    sSweepMode = saveSweep;
    sProfLogOn = saveProf;
    gfx_rapi = sB5RapiSaved;
    db_printf("vertices %u, triangles received %u, pass the discard %u, drawn %u, DL commands %u\n", nVtx, nSub, nPass, nDraw, nCmd);
    db_printf("of the triangles that reach the output: fog mode0 %u mode1 %u mode2 %u mode3 %u | shade_only %u  with texture %u  scissor active %u  needs scissor clipping %u  CPU clipping %u  enva_white %u\n",
              hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
    db_printf("%-70s %9s %9s %9s %9s\n", "configuration", "cold us", "hot us", "d.cold", "d.hot");
    for (c = 0; c < 8; c++) {
        const int prev = (c == 0) ? -1 : (c == 7 ? 0 : c - 1);
        const int dc = prev < 0 ? (int) tcold[c] : (int) tcold[c] - (int) tcold[prev];
        const int dh = prev < 0 ? (int) thot[c] : (int) thot[c] - (int) thot[prev];
        db_printf("%-70s %9u %9u %9d %9d\n", cname[c], tcold[c], thot[c], dc, dh);
    }
    db_printf("-- removing parts of the output / prepare (difference against control B0, hot) --\n");
    for (c = 8; c < NC; c++) {
        const int dh = (int) thot[c] - (int) thot[8];
        const int dc = (int) tcold[c] - (int) tcold[8];
        db_printf("%-70s %9u %9u %9d %9d   (%+.0f ns per drawn triangle)\n", cname[c], tcold[c], thot[c], dc, dh, nDraw ? (double) dh * 1000.0 / nDraw : 0.0);
    }
    {
        const double nv = nVtx ? (double) nVtx : 1.0, ns = nSub ? (double) nSub : 1.0, np = nPass ? (double) nPass : 1.0, nd = nDraw ? (double) nDraw : 1.0;
        db_printf("per vertex: setup of each load %.0f ns, transformation %.0f ns, lighting %.0f ns (hot)\n",
                  (double) ((int) thot[7] - (int) thot[0]) * 1000.0 / nv,
                  (double) ((int) thot[1] - (int) thot[7]) * 1000.0 / nv, (double) ((int) thot[2] - (int) thot[1]) * 1000.0 / nv);
        db_printf("per triangle: dispatch+discard %.0f ns (per received) | prelude %.0f | prepare %.0f | output %.0f ns (per passed) | output per drawn %.0f ns\n",
                  (double) ((int) thot[3] - (int) thot[2]) * 1000.0 / ns, (double) ((int) thot[4] - (int) thot[3]) * 1000.0 / np,
                  (double) ((int) thot[5] - (int) thot[4]) * 1000.0 / np, (double) ((int) thot[6] - (int) thot[5]) * 1000.0 / np,
                  (double) ((int) thot[6] - (int) thot[5]) * 1000.0 / nd);
    }
    db_printf("equivalence test with random data (fog 1..3, several classes): %u comparisons, differing variant 3: %u, variant 7: %u   %s\n",
              sB5FuzzN, sB5FuzzBad[0], sB5FuzzBad[1], (sB5FuzzBad[0] | sB5FuzzBad[1]) ? "*** DIFFERENCES ***" : "OK");
    db_printf("-- isolated microtest: gfx_tri_core of a real triangle of each class, repeated with everything in cache (batches of 9 triangles as in the game) --\n");
    db_printf("%-34s %8s %9s %9s %9s %9s   %s\n", "class", "tris", "v0 ns", "v1 ns", "v3 ns", "v7 ns", "footprints");
    for (k = 0; k < 16; k++) {
        if (!sB5CapSeen[k]) continue;
        db_printf("fog=%d shade_only=%d texture=%d %*s %8u %9.0f %9.0f %9.0f %9.0f   %s\n", k >> 2, (k >> 1) & 1, k & 1, 4, "", sB5MicroN[k],
                  sB5MicroNs[k][0], sB5MicroNs[k][1], sB5MicroNs[k][2], sB5MicroNs[k][3],
                  (sB5MicroH[k][0] == sB5MicroH[k][1] && sB5MicroH[k][0] == sB5MicroH[k][2] && sB5MicroH[k][0] == sB5MicroH[k][3]) ? "equal" : "DIFFERENT");
    }
    db_printf("END\n");
    {
        SceUID fd = sceIoOpen("ms0:/sm64_devbench.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) { sceIoWrite(fd, sDbBuf, sDbLen); sceIoClose(fd); }
    }
    sceKernelExitGame();
}
#endif

void gfx_end_frame(void) {
#ifdef PSP_SWEEP_FORCE
    sSweepMode = PSP_SWEEP_FORCE;
#endif
#if PSP_DEVBENCH && PSP_DEVBENCH != 5
    { static int sDbN; if (++sDbN == 400) psp_devbench(); }
#endif
    
    //sceIoWrite(1, "----END FRAME!\n", 16);
    const unsigned int tg0 = sceKernelLibcClock();
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
    total_t1 = sceKernelLibcClock();
    sProfGe = total_t1 - tg0;
    prof_log_poll();
    if (gPspSixtyMode) {
        sProfSixty++;
    }
    {
        const unsigned int tick = total_t1 - total_t0;
        const unsigned int work = tick > sProfSleep ? tick - sProfSleep : 0;
        if (sProfLogOn) {
            const unsigned int emit_f = sProfGfx > sProfVblCur + sProfSyncCur ? sProfGfx - sProfVblCur - sProfSyncCur : 0;
            const unsigned int logic_f = work > sProfGfx + sProfGe ? work - sProfGfx - sProfGe : 0;
            if (logic_f + emit_f + sProfSyncCur + sProfGe > 14500u) {
                sProfLate++;
                sProfLateLog += logic_f;
                sProfLateEmit += emit_f;
                sProfLateSync += sProfSyncCur;
            }
        }
        sProfAccGfx += sProfGfx;
        sProfAccSync += sProfSyncCur;
        sProfAccVbl += sProfVblCur;
        sProfSyncCur = sProfVblCur = 0;
        sProfAccGe += sProfGe;
        sProfAcc[0] += sPcTri; sProfAcc[1] += sPcVtx; sProfAcc[2] += sPcDraw; sProfAcc[3] += sPcMtx;
        sProfAcc[4] += sPcClip; sProfAcc[5] += sPcTexMiss; sProfAcc[6] += sPcTexClr; sProfAcc[7] += sPcDrawUs;
        sProfAcc[8] += sPcTexUs;
        sProfAcc[9] += sPcVtxUs; sProfAcc[10] += sPcTriSub; sProfAcc[11] += sPcTriUs; sProfAcc[12] += sPcTriSmp;
        sProfAcc[13] += gPcFogAll; sProfAcc[14] += gPcFogKeep; sProfAcc[15] += sPcClipSkip;
        sProfAcc3[0] += sPcMtxUs; sProfAcc3[1] += sPcPrepUs; sProfAcc3[2] += sPcPrepN;
        sPcMtxUs = sPcPrepUs = sPcPrepN = 0;
        sProfAcc2[0] += sTriClassUs[0]; sProfAcc2[1] += sTriClassUs[1]; sProfAcc2[2] += sTriClassUs[2];
        sProfAcc2[3] += sTriClassN[0]; sProfAcc2[4] += sTriClassN[1]; sProfAcc2[5] += sTriClassN[2]; sProfAcc2[6] += sPcClipUs;
        sTriClassUs[0] = sTriClassUs[1] = sTriClassUs[2] = sTriClassN[0] = sTriClassN[1] = sTriClassN[2] = sPcClipUs = 0;
        sProfAccTx[0] += sPcTexSame; sProfAccTx[1] += sPcTexEv; sPcTexSame = sPcTexEv = 0;
        sPcClipSkip = 0;
        sPcTri = sPcVtx = sPcDraw = sPcMtx = sPcClip = sPcTexMiss = sPcTexClr = sPcDrawUs = sPcTexUs = 0;
        sPcVtxUs = sPcTriSub = sPcTriUs = sPcTriSmp = 0;
        gPcFogAll = gPcFogKeep = 0;
        if (!dropped_frame) { sProfFrames++; gPspPresented++; }
        sProfAccSleep += sProfSleep;
        sProfAccWork += work;
        if (work > sProfMax) sProfMax = work;
        if (++sProfTicks >= 30) {
            const unsigned int logic = sProfAccWork > sProfAccGfx + sProfAccGe ? sProfAccWork - sProfAccGfx - sProfAccGe : 0;
            gPspProf[0] = (int) (logic / 30 / 100);   /* decimas de ms */
            gPspProf[1] = (int) (sProfAccGfx / 30 / 100);
            gPspProf[2] = (int) (sProfAccGe / 30 / 100);
            gPspProf[3] = (int) (sProfAccSleep / 30 / 100);
            gPspProf[4] = (int) (sProfMax / 100);
            gPspProfX[0] = (int) (sProfAccSync / 30 / 100);
            gPspProfX[1] = (int) (sProfAccVbl / 30 / 100);
            sProfAccSync = sProfAccVbl = 0;
            prof_log_window();
            if (gPspSweep) {
                if (gMenuMode == -1) { /* only counts with the game running (not in the pause / menu) */
                    sSweepWin++;
                    sSweepMode = sSweepWin / 6;
                    if (sSweepMode > 8) {
                        gPspSweep = 0; /* end: psp_options_poll sees it and turns the row off */
                        sSweepMode = 0;
                        sSweepWin = 0;
                    }
                } else {
                    sSweepMode = 0;
                    sSweepWin = 0;
                }
            } else {
                sSweepMode = 0;
                sSweepWin = 0;
            }
            { int q; for (q = 0; q < 16; q++) sProfAcc[q] = 0; for (q = 0; q < 4; q++) sProfAcc3[q] = 0; for (q = 0; q < 8; q++) sProfAcc2[q] = 0; sProfAccTx[0] = sProfAccTx[1] = 0; for (q = 0; q < 32; q++) gPspLp[q] = 0; for (q = 0; q < 16; q++) gPspLc[q] = 0;
              sProfLate = sProfLateLog = sProfLateEmit = sProfLateSync = 0; }
            sProfFrames = 0;
            sProfListMax = 0;
            gPspArenaMax = 0;
            gPspGateUs = gPspGateN = gPspQMax = 0;
            sProfAccGfx = sProfAccGe = sProfAccSleep = sProfAccWork = sProfMax = sProfTicks = 0;
        }
    }
    float delta = (total_t1 - total_t0)/1000.0f;
    (void)delta;
    if(frame_counter>=29){
        //printf("TOTAL TIME FRAME: %2.3f ms FPS %2.3f\n", delta, 1000/delta);
    }
}

static void gfx_invalidate_render_state(void) {
    gfx_flush();
    gfx_rapi->unload_shader(NULL);
    rendering_state.shader_program = NULL;
    rendering_state.color_combiner_valid = false;
    rendering_state.tri_pipeline_dirty = true;
    rendering_state.backend_state_dirty = true;
    rdp.viewport_or_scissor_changed = true;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

void gfx_render_callback_frame(void (*draw_callback)(void *arg), void *arg) {
    gfx_start_frame();

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }

    dropped_frame = false;
#if PSP_F2_REPLAY
    /* The HOME menu uses the big list and overwrites the recorded scene: no replaying after that. */
    gfx_scegu_replay_arm(0);
    gfx_scegu_rec_reset();
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
    sRecMtxValid = 0;
    sRecMtxOn = 0;
    sRecCur = -1;
    sRejXY = 1.0f; gfx_sc_touch();
#endif
#endif
    gfx_rapi->start_frame();
    if (draw_callback != NULL) {
        draw_callback(arg);
    }
    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
    gfx_end_frame();
    gfx_invalidate_render_state();
}
