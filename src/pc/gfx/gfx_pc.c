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
#define PSP_TRI_CHAIN 0 /* parche 107 lo probo en la PSP real: sin mejora medible (+0,1..0,3 ms), asi que queda apagado */
#define PSP_DEVBENCH 0 /* PARCHE DE DIAGNOSTICO: mide la CPU en la PSP real a los ~7 s de arrancar y cierra el juego (ver psp_devbench). 0 = juego normal */
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
    uint8_t env_alpha; /* el canal alpha sale del color de entorno (SHADEFADEA / BLENDRGBFADEA) */
    uint8_t enva_tex;  /* BLENDRGBFADEA con textura */
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
    uint32_t shader_id; /* id del shader activo y su modo de niebla (scegu_fog_mode es una funcion pura del id): se calculan una vez al preparar el estado */
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
    bool lookat_stale; // current_lookat_coeffs hay que recalcularlos (solo hacen falta con texgen)
    
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

/* PSP_SCRATCH: el estado del "RSP" (matrices, luces y sobre todo los 64 vertices cargados, ~4.3 KB) y los vertices temporales del recorte viven en
 * el scratchpad de la PSP (16 KB en 0x00010000, memoria rapida fuera de la cache de datos de 16 KB). Asi no compiten con la lista de
 * comandos del juego ni con los vertices que se van leyendo de la ROM, que son lo que mas ensucia la cache. Compilar con -DPSP_SCRATCH=0 para
 * volver a la memoria normal. */
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
    float vp_l, vp_t, vp_w, vp_h; /* viewport en pixeles N64 (desde arriba) */
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
/* Factor de niebla EXACTO de la N64 (0..255) por vertice del buffer; lo usa gfx_scegu.c para la 2da pasada. */
unsigned char scegu_fog_alpha[MAX_BUFFERED * 3];
/* PSP_EYE_SPACE: los vertices se transforman en la CPU a ESPACIO DE VISTA (modelo*vista) y el GE solo aplica la proyeccion; las
 * matrices de modelo y de vista del GE quedan en identidad. Antes cada G_MTX de modelo vaciaba el lote (una llamada de dibujo por
 * objeto/parte) porque el GE necesitaba su matriz de modelo; ahora objetos distintos con el mismo estado (monedas, enemigos iguales,
 * partes de un mismo modelo) comparten llamada. El resultado es el mismo producto de matrices. Solo con PSP_F2_REPLAY 0 (la
 * repeticion de lista parchea las matrices del GE). PSP_EYE_SPACE 0 vuelve al camino anterior. */
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
uint32_t scegu_mt_tex[2]; /* texture_id de las texturas de tile 0 y 1 del draw actual */
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
#define SC_IN (0x100) /* bit extra de clip_rej: el vertice esta SEGURO por dentro de todos los planos del scissor (ver gfx_vtx_post); 0 = no se sabe */
//#define CLIP_TEST_FLAGS ( Z_POS | Z_NEG ) /* Faster but worse */

/* Recorte en X/Y por el GE: la PSP no recorta lados, pero su "guard band" acepta primitivas con vertices hasta ~2048 px del
 * centro del viewport y el scissor ya recorta el raster. Un triangulo que solo cruza un borde lateral/superior/inferior de la
 * pantalla, con todos sus vertices delante de la camara (w > 0) y a menos de PSP_GE_XY_GUARD veces w del centro (NDC), se
 * manda tal cual (recortar en 3D no cambia como se interpolan los atributos). Lo que toque near/far o se salga de la guarda
 * sigue recortandose en la CPU. PSP_GE_XY_CLIP 0 vuelve a recortar todo en la CPU. (Idea de ghurone/Sonic-R-PSP.) */
#ifndef PSP_GE_XY_CLIP
#define PSP_GE_XY_CLIP 1
#endif
#define PSP_GE_XY_GUARD 3.0f

/* PSP_F2_REPLAY_MTX: durante la grabacion de un cuadro 1 el rechazo trivial y el recorte lateral/vertical dejan pasar hasta este
 * multiplo de w (1.0 = pantalla exacta). El 2o cuadro mueve los objetos medio tick sin volver a recortar: lo que quedaba justo fuera
 * de pantalla en el 1o puede entrar en el 2o, y lo recorta el GE (guard band + scissor). */
#define PSP_REC_XY_MARGIN 1.25f
static float sRejXY = 1.0f;
/* Al grabar, un triangulo que NO se descarta se descarta igual si es claramente de espalda; los de canto (seno del angulo entre sus dos
 * aristas en pantalla menor que esto, ~7 grados) se dejan pasar, porque con la camara del 2o cuadro pueden pasar a ser de frente. */
#define PSP_REC_CULL_SIN2 0.0150f
/* PSP_F2_REPLAY_MTX: objeto (entrada de gMtxTbl) cuyas matrices se estan interpretando y banda cercana al plano near (ver mas abajo) */
static int sRecCur = -1;
static float sRecNearBand;
static unsigned char sRecNear[1024];

/* PSP_F2_SKY: el cielo lleva su matriz ortografica en su propia lista (G_MTX_PROJECTION | G_MTX_MUL), que el GE recibe como un bloque
 * GU_VIEW. Al grabar (cuadro 1) se anota donde quedo ese bloque; en el cuadro 2 se reescribe con la ortografica del estado real. */
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX && PSP_F2_SKY && defined(GBI_FLOATS)
#define PSP_SKY_PATCH 1
extern const float *psp_sky_interp_ortho(void);
extern const float *psp_sky_real_ortho(void);
extern int gfx_scegu_view_block_ok(unsigned int off, const float *m);
extern void gfx_scegu_write_view(unsigned int off, const float *m);
extern unsigned int gfx_scegu_list_pos(void);
static int sRecMtxOn;                                       /* 1 mientras se graba (se define mas abajo) */
static const float *sSkyInterpPtr;                          /* ortografica del cielo interpolado (la que lleva el cuadro 1) */
static unsigned int sRecSkyOff;                             /* desplazamiento del bloque GU_VIEW del cielo en la lista A */
static int sRecSkyOk;                                       /* ese bloque esta anotado y verificado */
static float sRecSkyPre[16] __attribute__((aligned(16)));   /* GU_VIEW acumulada ANTES de multiplicar la ortografica del cielo */
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

/* Igual que NDCPlane pero con los lados a PSP_REC_XY_MARGIN (solo mientras se graba un cuadro 1, ver sRejXY) */
static const float NDCPlaneWide[6][4] __attribute__((aligned(16))) =
{
	{  0.f,  0.f,  1.f, -1.f },	// near
	{  1.f,  0.f,  0.f, -PSP_REC_XY_MARGIN },	// left
	{ -1.f,  0.f,  0.f, -PSP_REC_XY_MARGIN },	// right
	{  0.f,  1.f,  0.f, -PSP_REC_XY_MARGIN },	// bottom
	{  0.f, -1.f,  0.f, -PSP_REC_XY_MARGIN },	// top
	{  0.f,  0.f, -1.f, -1.f }	// far
};

/* Version de siempre (las 6 pasadas, en este orden; cada una deja el poligono girado una posicion). Solo se usa para comprobar la nueva (PSP_CLIPCHECK). */
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

/* Recorte contra el frustum. Antes: siempre las 6 pasadas (cada una recorre todos los vertices con la VFPU), aunque casi todos los triangulos solo
 * cruzan 1 plano (el near). Ahora: solo las pasadas de los planos por los que algun vertice esta fuera (bits de clip_rej en "need"). Una pasada
 * sobre un poligono entero por dentro devuelve los mismos vertices, SIN modificarlos, empezando por el 2o (= girado una posicion), asi que
 * saltarla equivale a girar el poligono una posicion: se lleva la cuenta (rot) y se aplica al hacer la siguiente pasada real o al final.
 * El resultado es el mismo, vertice a vertice y en el mismo orden, que el de las 6 pasadas. *res = buffer donde queda (v0 o v1). */
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
			/* poligono girado r posiciones en el otro buffer, y la pasada vuelve a escribir en este */
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

/* Planos de recorte del scissor del RDP (espacio de recorte ajustado, igual que _x/_y/_z/_w).
 * Cada plano: a*x + b*y + c*z + d*w <= 0 es "dentro". Solo se generan los bordes que NO estan
 * pegados al borde de pantalla (margen de 4 px), asi el scissor normal del juego no cambia nada. */
static float gfx_adjust_x_for_aspect_ratio(float x);
static float sc_planes[4][4] __attribute__((aligned(16)));
static unsigned char sc_ptype[4]; /* 0 = plano en x (a*x + d*w), 1 = plano en y (b*y + d*w) */

/* Factor (4/3)/aspecto: solo cambia cuando cambian las dimensiones, asi que se calcula una vez (antes eran 2 divisiones
 * por vertice y por triangulo). */
static float sAspectK = 1.0f;
static int sAspectW = -1, sAspectH = -1;
static inline float gfx_aspect_k(void) {
    const int w = (int)gfx_current_dimensions.width, h = (int)gfx_current_dimensions.height;
    if (w != sAspectW || h != sAspectH) {
        sAspectW = w;
        sAspectH = h;
        sAspectK = (4.0f / 3.0f) / ((float)w / (float)h);
    }
    return sAspectK;
}

static int sProjCompensate = 0; /* 1 si el GE dibuja con la matriz compensada a 16:9 (x * k) */

static float sScGX = 1.0e30f, sScGY = 1.0e30f; /* cotas del pre-test de scissor por vertice (ver mas abajo) */
static int gfx_scissor_planes_compute(void) {
    if (!rdp.sc_valid || rdp.vp_w <= 1.0f || rdp.vp_h <= 1.0f) {
        sScGX = sScGY = 1.0e30f;
        return 0;
    }
    /* El scissor del RDP esta en pixeles N64 de la pantalla; el recorte se hace en el espacio de recorte de la CPU
     * (_x = k * x_proyectado siempre; el GE dibuja con x*k solo si la proyeccion esta compensada).
     *  - Borde del scissor casi pegado al borde del viewport (ventanas de los creditos, margen de 1-2 px):
     *    el borde es una fraccion del viewport; con proyeccion compensada ya lo recorta el frustum de la CPU.
     *  - Borde interior (cajas de dialogo): coordenadas N64 exactas, comprimidas por k como el resto del 2D. */
    const float k = gfx_aspect_k();
    const float sl = rdp.sc_ulx / 4.0f, sr = rdp.sc_lrx / 4.0f;
    const float st = rdp.sc_uly / 4.0f, sb = rdp.sc_lry / 4.0f;
    const float il = sl - rdp.vp_l;
    const float ir = (rdp.vp_l + rdp.vp_w) - sr;
    const float it = st - rdp.vp_t;
    const float ib = (rdp.vp_t + rdp.vp_h) - sb;
    const float near_edge = 2.5f;
    /* Mientras se graba un cuadro 1 (sRejXY > 1) los bordes del scissor que estan a pocos pixeles del borde de la pantalla no se
     * recortan en la CPU: lo hace el scissor del GE (la lista lo lleva) y asi el 2o cuadro, con la camara movida, tiene geometria
     * por fuera del borde para mostrar (si no, aparecia una franja con el fondo en los laterales). */
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

/* Se llama una vez por triangulo, pero el resultado solo depende del scissor, el viewport, el factor de aspecto y la
 * compensacion de proyeccion: se guarda y solo se recalcula si alguno cambia (resultado identico, sin las ~6 divisiones). */
static int sScDirty = 1, sScNp = 0;
/* Cotas para el pre-test de scissor por vertice (gfx_vtx_post): |x| <= sScGX * w y |y| <= sScGY * w => el vertice no esta fuera de ningun plano
 * del scissor (los planos son de eje y estan a distancia m = -dw del centro; se usa m con un margen del 0,1 % que cubre de sobra los redondeos).
 * Sin planos en un eje, la cota es enorme (siempre cumple). */
/* Cambio de scissor / viewport / proyeccion / margen: hay que recalcular los planos y los vertices ya cargados pierden su marca SC_IN. */
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

/* Cada triangulo solo mira una bandera: quien cambia scissor/viewport/proyeccion/margen/dimensiones pone sScDirty. */
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
    struct LoadedVertex *cur = temp_a; /* buffer donde esta el poligono */
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

    /* Recorte contra el scissor del RDP (planos del scissor) */
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


/* Profile Sweep (menu de opciones, solo para medir): cada 6 ventanas del registro (~6 s) se apaga una etapa mas del dibujo, con la camara quieta,
 * y el registro anota el modo en OSWP. La diferencia de EUS entre dos modos seguidos es lo que cuesta esa etapa en la PSP real.
 *   0 normal | 1 el GE dibuja pero con scissor de 1 pixel (sin rellenar pantalla) | 2 + sin enviar nada al GE | 3 + sin copiar triangulos
 *   4 + sin preparar estado ni recortar | 5 + sin descartar triangulos | 6 + vertices solo transformados (sin color / recorte)
 *   7 + sin vertices | 8 + sin recorrer la lista del juego */
int gPspSweep = 0;
static int sSweepMode = 0;
static int sSweepWin = 0;

/* Contadores de perfil (se imprimen en ms0:/sm64_profile.txt). Contar es gratis; los cronometros solo corren mientras
 * el registro esta activo (Triangulo + R), asi no cuestan nada en el juego normal. */
extern int sProfLogOn; /* se define mas abajo */
extern int gPspGpuProf; /* perfil de GPU, se define mas abajo */
extern float gPcAreaAcc;
extern unsigned int gPspArenaMax; /* gfx_scegu.c */
static unsigned int sPcTri, sPcVtx, sPcDraw, sPcMtx, sPcClip, sPcTexMiss, sPcTexClr, sPcDrawUs, sPcTexUs;
static unsigned int sPcTexEv, sPcTexSame; /* cambios de textura pedidos por el juego (tile 0) / de ellos, los que ya era la textura enlazada (sin cortar el lote) */
static unsigned int sPcClipSkip;
extern short gMenuMode;           /* ingame_menu.c: -1 = sin menu de pausa */
static unsigned int sPcMtxUs, sPcPrepUs, sPcPrepN; /* tiempo en gfx_sp_matrix, tiempo (sin dibujo ni texturas) y numero de veces que se rehizo el estado del pipeline de triangulos */
static unsigned int sPcVtxUs, sPcTriSub, sPcTriUs, sPcTriSmp, sTriSampleCtr; /* tiempo en gfx_sp_vertex, triangulos recibidos, muestreo de gfx_sp_tri1 */
/* desglose del tiempo por triangulo (solo con el registro activo): clase 0 = descartado pronto (cara trasera / fuera de pantalla),
 * 1 = dibujado sin recorte por CPU, 2 = pasa por el recorte de la CPU. Tiempo y numero de muestras (1 de cada 8) por clase. */
static unsigned int sTriClass, sTriClassUs[3], sTriClassN[3], sPcClipUs;
unsigned int gPcFogAll, gPcFogKeep; /* triangulos con niebla (todos / los que necesitan 2a pasada), los incrementa gfx_scegu.c */

#if PSP_DEVBENCH == 5
/* Diagnostico 5: se repite un cuadro real con un contador de ciclos exacto (CP0 Count, solo en modo kernel) y se desglosa cada etapa. */
static int sB5On, sB5Cull, sB5Abl, sB5Cap, sB5Hash, sB5PrepOn;
static unsigned int sB5HashV, sB5PAcc[6], sB5PN, sB5CapSeen[16];
static void b5_micro_capture(struct LoadedVertex *t1, struct LoadedVertex *t2, struct LoadedVertex *t3);
static unsigned int sB5H[12], sB5NCmd;
#define B5ABL(b_) ((sB5Abl >> (b_)) & 1)
#define B5H(i_, c_) do { sB5H[i_] += (c_) ? 1u : 0u; } while (0)
static unsigned int sB5Acc[16], sB5Cnt[16], sB5Mark[4];
#ifdef PSP_B5_TIMER
#define B5C() (sceKernelGetSystemTimeLow() * 333u) /* solo para probar la logica en el emulador (no hay CP0 de usuario) */
#else
#define B5C() ({ unsigned int c_; __asm__ volatile("mfc0 %0, $9" : "=r"(c_)); c_; })
#endif
#define B5MARK(i_) do { if (sB5On) sB5Mark[i_] = B5C(); } while (0)
#else
#define B5MARK(i_) do { } while (0)
#define B5ABL(b_) 0
#define B5H(i_, c_) do { } while (0)
#endif
/* Variante de las matrices (bits): 1 = el producto modelo*proyeccion (rsp.MP_matrix) se calcula al cargar vertices y no en cada G_MTX / G_POPMTX,
 * 2 = gfx_update_mv multiplica directamente sobre sus matrices, sin copias intermedias. */
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
            /* Profile Sweep: no se manda nada al GE */
        } else if (__builtin_expect(sSweepMode == 1, 0)) {
            /* Profile Sweep: el GE transforma y prepara todo pero casi no rellena pixeles (scissor de 1x1) */
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
     * comparten el id de shader 0x38D pero necesitan modos de textura distintos en el GE; se separa con un bit propio. */
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

/* Parches con textura BLENDRGBFADEA (ojos/boca del Boo): el modelo los dibuja ANTES con SHADEFADEA (mismo array de vertices).
 * Con DECAL (que da lerp(shade, textura, alpha_tex) con alpha = ENV en TODO el parche) la cara se acumulaba como una 3a capa translucida
 * y se veia mas opaca. Si el parche ya se dibujo en la pasada de color, la pasada de textura se hace con MODULATE y color de vertice
 * blanco: solo se dibujan los texels con alpha, con el color de la textura. Los parches que NO tienen pasada previa (parte superior del
 * sombrero suelto, Mario) siguen con DECAL. */
int scegu_enva_modulate = 0; /* leido por gfx_scegu.c al dibujar */
static const void *sLastVtxSrc = NULL;
static const void *sEnvaVtxRing[8];
static int sEnvaVtxRingPos = 0;
static bool sEnvaWhite = false;

/* El GE solo tiene UN color por vertice. Antes, con alpha (objetos que se desvanecen: toads, Bowser, estrellas azules)
 * se devolvia entero el color de la fuente de ALPHA (p.ej. env = 255,255,255,a), perdiendo la iluminacion (shade)
 * del RGB y dejando el objeto plano y muy claro. Ahora el RGB viene de la fuente del canal de color y solo el
 * alpha viene de la fuente del canal alpha, como hace el combinador de la N64. */
static inline struct RGBA gfx_get_vertex_color(const struct ColorCombiner *comb, bool use_alpha, const struct RGBA *shade_color, float lod_w, bool allow_lod) {
    struct RGBA c = gfx_vertex_color_from(comb->vertex_color_source[0], shade_color, lod_w, allow_lod);
    if (sEnvaWhite) {
        c.r = c.g = c.b = 255;
    }
    if (use_alpha) {
        const struct RGBA a = gfx_vertex_color_from(comb->vertex_color_source[1], shade_color, lod_w, allow_lod);
        c.a = a.a;
    }
    /* Mezcla de 2 texturas por LOD (cuadro Peach/Bowser): el canal de alpha del combinador es solo TEXEL0A, asi que
     * sin esto el alpha salia 255 y se perdia el factor LOD que el backend usa para mezclar la 2da pasada. */
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
                /* lod_w = relacion texels/pixel (N64) de este vertice. Con 2 niveles (max level 1):
                 * <=1 -> nivel 0, frac 0 ; 1..2 -> frac = ratio-1 ; >=2 -> satura en el nivel 1 (frac 1) */
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
extern void gfx_scegu_sync_all(void); /* gfx_scegu.c: espera a que el GE termine todo lo enviado */

#if defined(TARGET_PSP)
/* Igual que la busqueda de gfx_texture_cache_lookup pero SIN efectos (no enlaza ni cambia el muestreador del GE): devuelve el nodo o NULL. */
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
        gfx_scegu_sync_all(); /* el GE podria seguir leyendo texturas que las nuevas van a pisar */
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
/* ---- Modo decal EXACTO de la N64 (ZMODE_DEC) ----
 * Formula tomada de un emulador de RDP exacto (ParaLLEl-RDP, depth_test.h / rdp_renderer.cpp): el decal pasa si |z - z_memoria| <= (combined_dz << 3)
 * en z de 18 bits (z18 = 8 * z15, z15 = 0..0x7FFF), donde
 *   dzpix       = |dz/dx| + |dz/dy| del triangulo en pixeles de la N64 (z15 por pixel), redondeado hacia ARRIBA a potencia de 2 (normalize_dzpix)
 *   memory_dz   = el del pixel ya dibujado (aqui: el mismo plano, el piso); si z < 0.875 del rango (exponente de z comprimido < 3):
 *                 max(memory_dz << 1, 16 >> exponente)
 *   combined_dz = mayor potencia de 2 <= (dzpix | memory_dz)
 * El GE solo puede desplazar la profundidad (z de 16 bits = 2 * z15) hacia la camara, asi que la tolerancia de la N64 se aplica como
 * desplazamiento = 2 * combined_dz (en z15 = combined_dz) + un margen chico para el error de interpolacion del GE (PSP_DECAL_OFFSET).
 * Sale en escalones de potencias de 2: los triangulos de un mismo plano caen en el mismo y casi nunca cortan el lote de dibujo.
 * Las pendientes se miden en pixeles de la N64 (240 de alto, cuadrados), igual que el calculo del LOD. */
extern void gfx_scegu_set_decal_bias(int bias);
static int sDecalBias = PSP_DECAL_OFFSET; /* el que tiene puesto el GE (solo vale dentro del modo decal) */
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
            return sDecalBias; /* de canto: no se ve, se deja el desplazamiento que haya */
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
        tol = (int) ((float) (comb * 2) * PSP_DECAL_SCALE) + PSP_DECAL_OFFSET; /* z15 -> z16 del GE (x2), por la escala, mas el margen */
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

    const unsigned int pp0 = sProfLogOn ? sceKernelGetSystemTimeLow() : 0U; /* cronometro del perfil: ver PREPMS */
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
        sDecalBias = PSP_DECAL_OFFSET; /* set_zmode_decal(true) del backend lo deja en PSP_DECAL_OFFSET */
#endif
    }

    if (backend_state_dirty || rdp.viewport_or_scissor_changed) {
        bool vp_changed = false;
        if (backend_state_dirty ||
            memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
            vp_changed = true; /* en PSP set_viewport pisa el scissor: hay que reaplicarlo */
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
        use_alpha = true; /* el desvanecimiento por ruido siempre necesita alpha */
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
                /* El juego repite la carga de la MISMA textura (cada instancia de un objeto, cada trozo de un nivel con el mismo material) y antes
                 * eso cortaba el lote aunque el GE no tuviera nada que cambiar: un draw de ~6 triangulos por vez. Si la textura que se pide es
                 * justo la que ya esta enlazada (mismo nodo de la cache, mismos espejos) no hay nada que enviar: sigue el mismo lote. Los
                 * parametros del muestreador se comparan mas abajo, como siempre, y si difieren si se corta. */
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
            scegu_mt_tex[i] = rendering_state.textures[i]->texture_id; /* ids para el mezclado de 2 texturas (cuadro Peach/Bowser) */
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

            /* La textura espejo se sube duplicada ([reflejo | original]) y se muestrea con REPEAT.
             * El espejo exacto de la N64 sobre esa textura es u' = u / (2 * ancho) + 0.5, o sea
             * escala * 0.5 y sesgo * 0.5 + 0.5. Antes solo se hacia para tiles de 32x64; los demas
             * (p.ej. sombras 16x16) salian al doble de frecuencia y sin desfase. Se aplica por eje. */
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

/* dir / 127.0f para cada valor posible de un signed char (mismo resultado, bit a bit, que dividir: 3 divisiones menos por cada luz que se recalcula). */
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

/* ---- Niebla PSP ----
 * N64: factor = (z/w) * fog_mul + fog_offset (0..255), con z/w en [-1,1] (depende del near/far de la camara).
 * El GE de la PSP usa distancias de vista (sceGuFog(near, far)). Se convierte con los coeficientes A=m[2][2], B=m[3][2]
 * de la perspectiva cargada: z/w = -A + B/d  =>  d = B / (z/w + A). */
extern float scegu_fog_near, scegu_fog_far; /* definidos en gfx_scegu.c */
static float sFogA = 0.0f, sFogB = 0.0f;
/* La niebla de la N64 depende de z/w de la proyeccion ORIGINAL del juego. "Far Distance" alarga el plano lejano de la camara y con eso el mismo
 * punto del mundo tiene otro z/w: la niebla se estiraba (empezaba mas lejos y llegaba al 100 % en el plano nuevo). Con PSP_FOG_FAR_INDEPENDENT la
 * niebla se calcula con A0/B0 de la proyeccion con el far original: ndc0(d) = -A0 + B0/d, donde d = w (distancia a la camara). Si la escala es 1
 * (lo normal) no se toca nada y la cuenta es la de siempre (z/w). sFogDA/sFogDB son los coeficientes con los que se pasa de ndc a distancia. */
#ifndef PSP_FOG_FAR_INDEPENDENT
#define PSP_FOG_FAR_INDEPENDENT 1
#endif
extern float gPspPerspFarScaleUsed; /* game/rendering_graph_node.c */
static int sFogScaled = 0;               /* 1 = la proyeccion cargada lleva el far alargado y la niebla usa la original */
static float sFogEffNA = 0.0f, sFogEffB = 0.0f; /* -A0 y B0 de la proyeccion original (solo si sFogScaled) */
static float sFogDA = 0.0f, sFogDB = 0.0f;      /* A y B para convertir ndc -> distancia (los de la original si sFogScaled) */

/* Se llama al cargar una proyeccion de perspectiva. Con el far alargado k veces, de A = (n+f)/(n-f) y B = 2nf/(n-f) salen n = B/(A-1) y f = B/(A+1);
 * el far original es f/k y con el se rehacen A0 y B0. */
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

static void gfx_update_fog_range(void) {
    float n = 3600.0f, f = 32000.0f;
    if (sFogB != 0.0f && rsp.fog_mul != 0) {
        float mul = (float)rsp.fog_mul, off = (float)rsp.fog_offset;
        /* La niebla N64 es hiperbolica en la distancia (se acumula pronto) y la del GE es lineal.
         * Se mantiene el inicio exacto y se elige el final para que ambas coincidan cuando la niebla
         * N64 llega a PSP_FOG_MATCH (0..1). Mas bajo = niebla mas densa/cercana. */
        const float PSP_FOG_MATCH = 0.5f;
        n = gfx_fog_ndc_to_dist((0.0f - off) / mul);
        float dm = gfx_fog_ndc_to_dist((255.0f * PSP_FOG_MATCH - off) / mul);
        f = n + (dm - n) / PSP_FOG_MATCH;
        if (f < n) { float t = n; n = f; f = t; }
        if (f < n + 1.0f) f = n + 1.0f;
    }
    if (n != scegu_fog_near || f != scegu_fog_far) {
        gfx_flush(); /* lo acumulado se dibuja con el rango anterior */
        scegu_fog_near = n;
        scegu_fog_far = f;
    }
}

/* Proyeccion base (ultima cargada con LOAD) y vista (producto de los MUL posteriores), ver gfx_sp_matrix. */
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
/* sXfm[0..15] = modelo*vista*proyeccion (recorte, = rsp.MP_matrix), sXfm[16..31] = modelo*vista (espacio de vista, lo que recibe el GE). */
static float sXfm[32] __attribute__((aligned(16)));
static int sXfmMpDirty = 1; /* 1 = rsp.MP_matrix cambio desde la ultima copia a sXfm[0..15] */
static int sMVDirty = 1; /* 1 = hay que recalcular modelo*vista (cambio la matriz de modelo, un pop o la vista) */
/* 1 mientras la proyeccion cargada es en perspectiva (el mundo 3D). Con proyecciones ortograficas o de pantalla completa (menus, HUD,
 * transiciones, cielo) se usa el camino de siempre (el GE aplica modelo y vista): ahi hay pocas llamadas y no merece el cambio. */
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

/* MTXVAR & 1: rsp.MP_matrix = pila[tope] * P_matrix se deja pendiente y se calcula solo si alguien lo va a usar (cargar vertices) o antes de que
 * el estado del que depende cambie sin recalcularlo (gfx_sp_reset, pop que deja la pila vacia). Mismo valor que el calculo en cada G_MTX. */
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

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
        /* El GE calcula la niebla con la z de VISTA (matriz GU_VIEW). El juego mete la camara (lookat) dentro de la
         * proyeccion con G_MTX_MUL, y con GU_VIEW=identidad la niebla se calculaba con coordenadas de mundo.
         * Se separa: base = lo ultimo cargado con LOAD (perspectiva/ortho) -> GU_PROJECTION;
         * view = producto de todos los MUL posteriores (lookat, etc.) -> GU_VIEW. El resultado es el mismo producto. */
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
            /* El asm VFPU de gfx_matrix_mul no admite simbolos estaticos como operandos: se usan copias locales. */
            float tmpA[4][4] __attribute__((aligned(16)));
            float tmpB[4][4] __attribute__((aligned(16)));
            memcpy(tmpA, sPView, sizeof(tmpA));
            gfx_matrix_mul(tmpB, matrix, tmpA);
            memcpy(sPView, tmpB, sizeof(tmpB));
        }
        /* sceGuSetMatrix copia los valores como comandos dentro de la lista, no guarda el puntero: basta un buffer local.
         * (Antes se pedia memoria a la lista con sceGuGetMemory, que ademas de ocupar lista hace una llamada al sistema
         * para avisar al GE en cada matriz.) */
        float proj_buf[16] __attribute__((aligned(16)));
        float view_buf[16] __attribute__((aligned(16)));
        void *matrix_inline = proj_buf;
        void *view_inline = view_buf;
        memcpy(matrix_inline, sPBase, sizeof(rsp.P_matrix));
        memcpy(view_inline, sPView, sizeof(rsp.P_matrix));
        /* 16:9: el GE dibuja con esta matriz, asi que la correccion de aspecto se hace aqui.
         * Se decide SOLO al cargar la proyeccion (G_MTX_LOAD) y se conserva al multiplicar
         * (G_MTX_MUL, p.ej. camara/lookat), porque tras un MUL el [3][3] ya no sirve de prueba.
         *  - Ortografica (m[3][3]==1): menus, textos, power meter, transiciones -> se compensa.
         *  - Perspectiva con aspecto ~4:3 (cabeza de Mario de goddard) -> se compensa.
         *  - Perspectiva con aspecto 16:9 (mundo del juego) -> no se toca. */
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
            m[0] *= k; m[4] *= k; m[8] *= k; m[12] *= k; /* columna X */
        }
        sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)matrix_inline);
#if PSP_SKY_PATCH
        const unsigned int skyOff = skyHit ? gfx_scegu_list_pos() : 0U;
#endif
#if PSP_EYE_SPACE
        if (sEyeOn) {
            /* la vista ya va dentro de los vertices (espacio de vista): el GE usa vista = identidad (la niebla del GE sigue midiendo z de vista) */
            sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)identity_matrix);
            sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity_matrix); /* (por si algun otro codigo la dejo distinta) */
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
        memcpy(scegu_mtx_proj, matrix_inline, 64); /* copias para la mascara de ruido (gfx_scegu.c) */
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
        /* Buffer local: sceGuSetMatrix copia los valores a la lista (ver arriba) */
        float model_buf[16] __attribute__((aligned(16)));
        void *matrix_inline = model_buf;
#if PSP_EYE_SPACE
        if (sEyeOn) {
            sMVDirty = 1; /* el modelo va dentro de los vertices: el GE conserva modelo = identidad (no hace falta la copia) */
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
        gfx_mp_flush(); /* la pila queda vacia: el calculo de MP del estado actual no se repetiria */
    }
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
        }
    }
    if (rsp.modelview_matrix_stack_size > 0) {
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

/* Segunda parte de gfx_sp_vertex_impl (color/luces/texgen/rechazo trivial por vertice). "lighting" y "texgen" llegan como constantes en cada
 * llamada (always_inline), asi el compilador genera tres bucles sin las comparaciones por vertice (antes se evaluaban en cada uno). */
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
                /* caso de casi todos los objetos del juego: una sola luz direccional (+ ambiente), sin bucle */
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
        /* Vertice claramente dentro (lo mas comun entre los que se dibujan): 3 comparaciones en vez de 6. Es equivalente: dentro de
         * los tres rangos <=> ningun bit; con w negativo o NaN esta prueba falla y se hace la cuenta completa de siempre. */
        /* La prueba "dentro" se hace con enteros sobre los bits del float (|x| <= wxy <=> bits sin signo <= bits de wxy cuando wxy no es
         * negativo): 1 sola rama y sin comparaciones de coma flotante (cada c.cond.s + bc1 lleva 2 nops y una rama). Si dice "dentro" lo
         * es de verdad (w negativo => wxy negativo => no cumple y se cae a la cuenta completa; con NaN la cuenta completa da 0 igual). */
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
        /* Pre-test del scissor: SC_IN = seguro por dentro de todos los planos (gfx_tri_core se salta entonces la cuenta de los planos). */
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
/* Kernel VFPU de 2 vertices por vuelta (gfx_transform_vfpu.s): 1 = comprobado en este arranque (mismo resultado, bit a bit, que el de siempre
 * para 1, 2, 3, 6 y 7 vertices, sin tocar nada mas), 0 = se usa el kernel de siempre, -1 = todavia sin probar. */
static int sEye2 = -1;
int gPspEye2Ok = -1; /* lo lee el log */
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
        /* y que de verdad escribio algo distinto del relleno */
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
    /* Todo lo que no cambia entre vertices se saca del bucle: como se escribe por d->..., el compilador no puede
     * asegurar que rsp.* no se modifica y lo releia (y recalculaba las divisiones) en cada vertice. */
    (void)gfx_scissor_planes(); /* deja al dia las cotas sScGX/sScGY que usa el pre-test de scissor de gfx_vtx_post */
    const float aspk = gfx_aspect_k();
    const uint32_t gmode = rsp.geometry_mode;
    const bool lighting = (gmode & G_LIGHTING) != 0;
    const bool texgen = (gmode & G_TEXTURE_GEN) != 0;
    const int tsf_s = rsp.texture_scaling_factor.s;
    const int tsf_t = rsp.texture_scaling_factor.t;
    int nlights = 0;
    int amb_r = 0, amb_g = 0, amb_b = 0;
    float lc[MAX_LIGHTS][3]; /* direccion de cada luz ya dividida entre 127 (antes se dividia el producto por vertice) */
    int lcol[MAX_LIGHTS][3];
    float lk0[3] = {0.0f, 0.0f, 0.0f}, lk1[3] = {0.0f, 0.0f, 0.0f}; /* lookat_x / lookat_y */

    if (lighting) {
        if (rsp.lights_changed) {
            B5S0(b5s_lt);
            for (int li = 0; li < rsp.current_num_lights - 1; li++) {
                calculate_normal_dir(&rsp.current_lights[li], rsp.current_lights_coeffs[li]);
            }
            B5S1(b5s_lt, 1);
            /* lookat_x / lookat_y solo los usa el texgen (reflejos): se calculan la primera vez que hace falta (cada uno son 3 divisiones y
             * una raiz, y la mayoria de los lotes iluminados no los necesitan). */
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
        return; /* Profile Sweep: solo la transformacion */
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

/* Subdivision para el LOD por pixel (cuadro Peach/Bowser): el GE interpola el color/alpha en espacio de
 * pantalla (sin correccion de perspectiva), pero el LOD de la N64 es por pixel (~ w perspectiva-correcta).
 * Subdividiendo el triangulo y calculando el LOD exacto en cada vertice nuevo el error queda por debajo
 * de un pixel. */
#define LOD_SUBDIV 8

static void gfx_tri_core(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3, int sub, float sub_lodk);

/* ---- Partes poco frecuentes de gfx_tri_core, sacadas a funciones aparte (noinline) ----
 * gfx_tri_core ocupaba ~11.8 KB de codigo (la cache de instrucciones de la PSP es de 16 KB) y el camino normal (triangulo que se
 * dibuja sin recorte, sin LOD ni niebla en 2 pasadas) pasaba por todo ese codigo intercalado con el recorte, la subdivision para el
 * LOD de Peach/Bowser y el estado de las gorras/Boo. Mismo calculo, solo cambia donde esta el codigo. */

/* Recorte por CPU: los vertices recortados van a estas tablas (estaticas: una llamada recursiva de gfx_tri_core solo ocurre despues
 * de que la llamada padre ya termino de usarlas, ver la subdivision del LOD). */
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

/* PSP_F2_REPLAY_MTX: ¿este objeto tiene triangulos dibujados muy cerca del plano near? (ver gfx_psp_replay_prepare) */
static __attribute__((noinline)) void gfx_tri_note_near(const struct LoadedVertex *v1, const struct LoadedVertex *v2,
                                                        const struct LoadedVertex *v3) {
    /* Solo cuenta si el vertice cercano al near cae ademas dentro (con margen) de la pantalla: un suelo cortado por el near tiene
     * esos vertices muy por debajo de la camara, fuera de la vista, y no deja huecos ni triangulos sin recortar al moverse. */
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

/* Parches con alpha = ENV (gorras, Mario, Boo que se desvanecen): devuelve enva_white (ver gfx_get_vertex_color) */
static __attribute__((noinline)) bool gfx_tri_enva_cold(const struct ColorCombiner *comb, bool use_alpha, bool use_texture) {
    bool enva_white = false;
    if (use_alpha && comb->env_alpha && !use_texture) {
        /* pasada de color (SHADEFADEA): recordar el array de vertices */
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
            scegu_enva_modulate = enva_white ? 1 : 0; /* decision por buffer (un buffer = una pasada de un parche) */
        }
        enva_white = scegu_enva_modulate != 0;
    }
    return enva_white;
}

/* LOD exacto de la N64 (cuadro Peach/Bowser): relacion texels/pixel a partir de las derivadas de textura en pantalla
 * (en pixeles N64: 120 px por unidad NDC en Y, y mismo tamano de pixel cuadrado en X).
 * Se calcula una vez por triangulo y se escala por w en cada vertice (en un plano, ratio ~ w).
 * Devuelve true si el triangulo se subdividio y dibujo aqui (gfx_tri_core debe terminar); si no, *lod_out queda para el caso normal. */
static __attribute__((noinline)) bool gfx_tri_lod_cold(struct LoadedVertex *v1, struct LoadedVertex *v2, struct LoadedVertex *v3,
                                                        float *lod_out) {
    float lod_k = *lod_out;
    const float w1 = v1->_w, w2 = v2->_w, w3 = v3->_w;
    if (w1 > 0.01f && w2 > 0.01f && w3 > 0.01f) {
        /* _x ya viene multiplicado por gfx_adjust_x_for_aspect_ratio (= (4/3)/aspect), pero el GE dibuja con la
         * proyeccion SIN ese ajuste (ya es 16:9 por guPerspective). Pixeles N64 por unidad de _x/_w:
         * 120 * aspect (pixel cuadrado a escala vertical N64) dividido entre ese factor = 90 * aspect^2. */
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
        /* Subdividir en LOD_SUBDIV^2 triangulos con LOD exacto por vertice */
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

/* Descarte previo de un triangulo (todo fuera de pantalla o de espalda). Es la 1a parte de gfx_tri_core, sacada a una funcion en linea:
 * asi el ~70 % de los triangulos que se descartan no pagan la llamada a gfx_tri_core (pila grande, muchos registros guardados). */
static inline __attribute__((always_inline)) bool gfx_tri_culled(const struct LoadedVertex *v1, const struct LoadedVertex *v2,
                                                                  const struct LoadedVertex *v3) {
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej & CLIP_TEST_FLAGS) {
        // The whole triangle lies outside the visible area
        return true;
    }
    const uint32_t cull_mode = rsp.geometry_mode & G_CULL_BOTH;
    if (cull_mode != 0) {
        /* Signo del area en pantalla SIN dividir por w. Con dx1 = x1/w1 - x2/w2 etc.:
         *   cross = (a*b - c*d) / (w1*w3*w2^2), y como w2^2 > 0 el signo es el de (a*b - c*d) por el de w1*w3.
         * El original ademas invierte el signo si hay un numero impar de w negativas; juntando ambas inversiones
         * queda solo "invertir si w2 < 0". */
        const float w1 = v1->_w, w2 = v2->_w, w3 = v3->_w;
        const float a = v1->_x * w2 - v2->_x * w1;
        const float b = v3->_y * w2 - v2->_y * w3;
        const float c = v1->_y * w2 - v2->_y * w1;
        const float d = v3->_x * w2 - v2->_x * w3;
        float cross = a * b - c * d;
        if (w2 < 0.0f) {
            cross = -cross;
        }
        /* Grabando (rejXY > 1): solo se descarta lo claramente de espalda. sin(angulo)^2 = cross^2 / (|e1|^2 |e2|^2); los w se simplifican. */
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

static int sTriPrechecked; /* 1 = gfx_sp_tri1 ya aplico gfx_tri_culled a la proxima llamada de gfx_tri_core */



static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    sPcTriSub++;
    if (__builtin_expect(sSweepMode >= 5, 0)) {
        return; /* Profile Sweep */
    }
    if (sProfLogOn && ((++sTriSampleCtr) & 7u) == 0u) {
        /* muestreo: 1 de cada 8 triangulos se cronometra (sin contar dibujo ni texturas, que ya se miden aparte) */
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
                sB5Acc[3] += b5b - b5a; /* descarte de los que se dibujan */
                sTriPrechecked = 1;
                sB5Mark[1] = sB5Mark[2] = sB5Mark[3] = 0;
                gfx_tri_core(t1, t2, t3, 0, 0.0f);
                {
                    const unsigned int b5e = B5C();
                    if (sB5Mark[3]) {
                        sB5Acc[4] += sB5Mark[1] - b5b;         /* preludio: scissor / recorte */
                        sB5Acc[5] += sB5Mark[2] - sB5Mark[1];  /* prepare_tri_pipeline_state + enva + lod */
                        sB5Acc[6] += sB5Mark[3] - sB5Mark[2];  /* bucle de salida (vertices al buffer) */
                        sB5Acc[7] += b5e - sB5Mark[3];         /* cola (contadores, posible flush) */
                        sB5Cnt[3]++;
                    } else {
                        sB5Acc[8] += b5e - b5b;                /* salida temprana dentro de gfx_tri_core */
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
        sTriPrechecked = 1; /* gfx_tri_core no repite el descarte (el calculo del area con signo se hacia dos veces por triangulo dibujado) */
        gfx_tri_core(t1, t2, t3, 0, 0.0f);
    }
}

/* Variante de la salida de gfx_tri_core (bits): 1 = modo de niebla / id del shader ya calculados al preparar el estado, 2 = bucle de salida nuevo
 * (carga todo del vertice antes de escribir, un bucle por caso, niebla con enteros), 4 = niebla de los 3 vertices a la vez (con 2). */
#ifndef PSP_TRI_VAR
#define PSP_TRI_VAR 3 /* medido en la PSP real: ~0,9 ms menos por cuadro en la escena de los jardines; 0 = salida de siempre */
#endif

/* Division exacta por 255 de un entero 0..65025 (el resultado de c*inv): (p + 1 + (p >> 8)) >> 8 == p / 255 para todo p en 0..65534. */
static inline __attribute__((always_inline)) unsigned int gfx_div255(unsigned int p) {
    return (p + 1u + (p >> 8)) >> 8;
}

/* Factor de niebla de un vertice: igual que antes (z/w * mul + offset, limitado a 0..255), pero el limite se hace con enteros. Solo si |f| < 2^29
 * (siempre, salvo vertices con w casi 0): fuera de ese rango la conversion a entero no esta definida y se usa el limite de siempre. */
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
        return gfx_fog_clamp((sFogEffNA + sFogEffB / w) * fm + fo); /* z/w de la proyeccion original (ver sFogScaled) */
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

/* Salida nueva de los n vertices (n multiplo de 3) del triangulo al buffer: cada vertice se lee entero antes de escribir (las escrituras al buffer
 * no se pueden distinguir de los campos del vertice para el compilador y obligaban a releer), y un bucle por caso. */
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
            const struct RGBA c = gfx_get_vertex_color(comb, use_alpha, &vx->color, lod_k * vx->_w, true); /* LOD por vertice */
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

    /* Scissor del RDP: si el triangulo cruza un borde "interior" del scissor hay que recortarlo */
    const int sc_np = B5ABL(4) ? 0 : gfx_scissor_planes();
    B5H(6, sc_np > 0);
    bool need_sc = false;
    if (sc_np > 0 && !(v1->clip_rej & v2->clip_rej & v3->clip_rej & SC_IN)) {
        for (int p = 0; p < sc_np && !need_sc; p++) {
            /* Los planos del scissor son de eje (a*x + d*w  o  b*y + d*w): se calcula solo lo que no es cero. Mismo valor que
             * vec4_dot con los otros terminos en 0, pero 2 productos por vertice en vez de 4. */
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
                return; /* completamente fuera del scissor */
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
        /* area aproximada en pantalla (solo perfil de GPU): NDC recortado a [-1,1], 480x272 pixeles */
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
            gfx_flush(); /* lo acumulado se dibuja con el desplazamiento anterior */
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

    /* Caso mas comun (geometria del nivel, la mayoria de objetos): el color de salida es el de shade tal cual, y no hace falta
     * pasar por el switch de gfx_get_vertex_color por cada vertice. */
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
            out->color = gfx_get_vertex_color(comb, use_alpha, &vertex->color, lod_k * vertex->_w, true); /* LOD por vertice */
        }

        if (fog_mode && !B5ABL(0)) {
            /* Igual que la N64 / sm64ex: factor = (z/w) * fog_mul + fog_offset, limitado a 0..255. */
            float fw = vertex->_w;
            if (fabsf(fw) < 0.001f) fw = 0.001f;
            float fz = (sFogScaled ? (sFogEffNA + sFogEffB / fw) : (vertex->_z / fw)) * (float)rsp.fog_mul + (float)rsp.fog_offset;
            unsigned char fa = (fz <= 0.0f) ? 0 : ((fz >= 255.0f) ? 255 : (unsigned char)fz);
            scegu_fog_alpha[buf_num_vert] = fa;
            if (fog_mode == 3) {
                /* translucido: el color sale como C*(1-F); la niebla se suma luego en la 2da pasada */
                const unsigned int inv = 255u - fa;
                out->color.r = (unsigned char)((out->color.r * inv) / 255u);
                out->color.g = (unsigned char)((out->color.g * inv) / 255u);
                out->color.b = (unsigned char)((out->color.b * inv) / 255u);
            }
        }

        /* (Antes aqui habia un parche "Blue Star color" que copiaba el color del 1er vertice a todos y ponia el alpha de env:
         * dejaba planos a las estrellas azules y al Boo (shader SHADEFADEA). Ya no hace falta: gfx_get_vertex_color
         * toma RGB de shade y alpha de env por separado.) */
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
/* Diagnostico: una copia de gfx_tri_core por variante, se elige en tiempo de ejecucion (la medida compara la misma lista con cada una). */
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

/* Rectangulo de textura con G_TEXRECTFLIP (la fuente de la version EU lo usa en cada letra): en la RDP las coordenadas de textura van
 * transpuestas (s avanza con y, t con x). Un "sprite" de la PSP no puede transponer, asi que se dibuja como tira de 4 vertices:
 * 0=arriba-izq (s0,t0) 1=arriba-der (s0,t1) 2=abajo-izq (s1,t0) 3=abajo-der (s1,t1). Solo lo usa EU; las demas versiones no pasan por aqui. */
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
        /* Los campos del scissor son enteros: truncar "y" y "height" por separado movia el borde superior
         * hasta ~2 px N64 (se comia la parte de arriba de la 1a linea de los cuadros de dialogo). Se redondea
         * cada borde hacia afuera (izq/arriba hacia abajo, der/abajo hacia arriba); el recorte exacto de
         * triangulos y rectangulos ya lo hace el software. */
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
    uint32_t word_size_shift;
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

    uint32_t word_size_shift;
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

    /* La niebla la dibuja el GE de la PSP (sceGuFog): hay que pasarle el color en ABGR. */
    const unsigned int new_fog = 0xFF000000u | ((unsigned int)b << 16) | ((unsigned int)g << 8) | (unsigned int)r;
    if (new_fog != scegu_fog_color) {
        gfx_flush(); /* lo ya acumulado se dibuja con el color anterior */
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
    
    /* Recorte exacto contra el scissor del RDP en unidades N64 (U10.2). Se aplica a
       los rectangulos (texto de dialogos, etc.) porque en PSP el scissor por hardware
       no los recorta. Los bordes del scissor que estan a <=4 px del borde de pantalla
       se ignoran (scissor de pantalla completa / margen de 2 px del juego). */
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
                return; /* completamente fuera del scissor */
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
            } else { /* transpuesto: s avanza con y, t con x */
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

    ulxf = (ulxf*240)+240;
    lrxf = (lrxf*240)+240;

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

/* ---- PSP_F2_REPLAY_MTX (etapa 2): anotar donde quedo la matriz de cada objeto en la lista del GE ----
 * El juego deja en gMtxTbl una entrada por objeto: la posicion del comando G_MTX en la lista de dibujo (que lleva la matriz INTERPOLADA)
 * y la matriz real. Al interpretar ese G_MTX (cuadro 1, solo cuando se graba) se anota en que desplazamiento de la lista del GE
 * quedaron las 13 palabras de sceGuSetMatrix(GU_MODEL) y la traslacion de la matriz interpolada. El 2o cuadro (gfx_psp_replay_prepare)
 * reescribe esas 12 palabras con la matriz real. */
#if !defined(GBI_FLOATS)
#undef PSP_F2_REPLAY_MTX
#define PSP_F2_REPLAY_MTX 0
#endif
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
#define REC_MTX_MAX 1024 /* mas objetos que esto en un cuadro (gMtxTbl[] admite 6400) -> ese tick no se repite */
extern int psp_mtx_tbl_count(void);
extern Gfx *psp_mtx_tbl_pos(int i);
extern const float *psp_mtx_tbl_mtx(int i);
extern float psp_persp_near(void);
extern unsigned int gfx_scegu_list_pos(void);
extern int gfx_scegu_mtx_block_ok(unsigned int off, int full, const float *m);
extern void gfx_scegu_write_model(unsigned int off, const float *m);
static Gfx *sRecPos[REC_MTX_MAX];          /* comando G_MTX de cada entrada (copia de gMtxTbl[i].pos) */
static unsigned int sRecOff[REC_MTX_MAX];  /* desplazamiento en la lista del GE del bloque de matriz (0 = no se emitio) */
typedef char sRecNearSizeCheck[(REC_MTX_MAX == 1024) ? 1 : -1]; /* sRecNear[] mide 1024 (declarado antes, junto a sRejXY) */
static float sRecM[REC_MTX_MAX][12];       /* 3x3 y traslacion de la matriz que llevo el cuadro 1: m[0..2], m[4..6], m[8..10], m[12..14] */
static int sRecCount;                      /* entradas de la tabla al grabar */
static int sRecCursor;
static int sRecMatched;
static int sRecWhy;                        /* primer motivo de sRecBad: 0 sin grabar, 1 demasiadas, 2 MUL anidada, 3 bloque raro, 4 sin emparejar */
static int sRecBad;                        /* un bloque no tenia la forma esperada: no se parchea este tick */
static int sRecMtxOn;                      /* 1 mientras se graba */
static int sRecMtxValid;                   /* la anotacion de este tick sirve */
static int sRecFull;                       /* verificaciones completas hechas (la 1a matriz de cada grabacion) */

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
    sRecNearBand = psp_persp_near() * 1.4f; /* "cerca del near" = a menos de 0.4*near mas alla del plano */
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

/* Justo despues de gfx_sp_matrix de un G_MTX; off = posicion de la lista del GE antes de interpretarlo. */
static void gfx_rec_mtx_done(const Gfx *cmd, unsigned int off, const float *m, unsigned int params) {
    const int i = gfx_rec_mtx_find(cmd);
    if (i < 0) {
        /* No es una matriz de objeto de la tabla. Una de proyeccion o una cargada (LOAD) corta la cadena de objetos (HUD, menus...); una
         * multiplicada (MUL) dentro de un objeto saldria en la lista ya compuesta con la matriz del 1er cuadro y no se podria parchear. */
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

/* Cuanto se mueve (en unidades de vista) el punto del objeto que en el cuadro 1 estaba en q0 = (0, 0, -near) -- justo en el plano near,
 * que es donde la CPU recorto la geometria del cuadro 1 -- al pasar de la matriz del cuadro 1 (a1, guardada al grabar) a la real (m2).
 * Es la distancia al cuadrado. Una camara que solo gira, o un objeto quieto, da ~0 aunque su traslacion cambie mucho. Una matriz
 * degenerada (det ~ 0, no dibuja nada) da 0. Los NaN dan un valor que no pasa la comprobacion. */
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
        w0 = (vx * c12x + vy * c12y + vz * c12z) * id; /* w * A1 = v  ->  w = coordenadas locales del punto */
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
            /* Diagnostico: se salta un grupo de comandos (el coste incluye lo que ese grupo provoca despues) */
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
                /* una matriz de modelo/vista ya no cambia nada del GE: el lote sigue (solo la proyeccion vacia) */
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
                /* Los triangulos vienen casi siempre en tandas seguidas (G_TRI2 / G_TRI1): se despachan aqui mismo, sin volver al switch
                 * (salto indirecto por la tabla) por cada comando. El orden y lo que se hace con cada triangulo es exactamente el de antes. */
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
    memset((void *) PSP_SCRATCH_BASE, 0, 0x4000); /* el scratchpad no viene en cero como la BSS */
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

/* Perfil de tiempos por tick (microsegundos, promedio de 30 ticks en decimas de ms para mostrarlo con enteros):
 * gPspProf[0]=LOG logica+grafo de objetos, [1]=GFX emulacion de la lista (CPU), [2]=GE espera del GPU,
 * [3]=SLP espera de sincronizacion (holgura), [4]=MAX tick de trabajo mas largo (sin espera) en la ventana. */
int gPspProf[5];
extern int gPspOptFpsCap, gPspOptKeepSpeed, gPspClockWant, gPspOptCullObj;
extern float gPspObjDistScale, gPspFarScale; /* game/psp_options.c */
extern int psp_objects_free(void);              /* game/spawn_object.c */
extern int gPspFreeMin;                         /* game/spawn_object.c */
extern unsigned int gPspKicks, gPspRawDraws;   /* gfx_scegu.c */
extern unsigned int gPspWbCalls;                 /* gfx_scegu.c: vaciados de cache de la arena de vertices */
extern unsigned int gPspOptChanges;
extern int gPspOptPipe;                                  /* game/psp_options.c */
extern unsigned int gPspGateUs, gPspGateN, gPspQMax;     /* gfx_scegu.c: Triple Buffer */
extern int gPspPipeBroken;
unsigned int gPspPresented; /* cuadros presentados (lo lee el contador de FPS de game_init.c) */
static unsigned int sProfGfx, sProfSleep, sProfAccGfx, sProfAccGe, sProfAccSleep, sProfAccWork, sProfMax, sProfTicks;
static unsigned int sProfGe;
extern unsigned int gPspProfSync, gPspProfVblank; /* gfx_scegu.c */
int gPspProfX[2]; /* [0]=SYNC espera al GPU, [1]=VBL espera al vblank (decimas de ms) */
static unsigned int sProfSyncCur, sProfVblCur, sProfAccSync, sProfAccVbl;

/* Registro de tiempos en archivo (ms0:/sm64_profile.txt). Se activa SOLO mientras el contador de FPS del juego esta
 * en pantalla (L+R): al mostrarlo empieza el registro y al ocultarlo se cierra y se guarda. Todo vive aqui para no leer variables desde otros archivos ni dibujar texto.
 * Una linea cada 30 ticks: N=numero de ventana, LV=nivel, AR=area, WALL=ms reales de la ventana (1000 = 30 ticks/s),
 * DROP=ticks sin dibujar, LOG/GFX/GE/SLP/MAX en decimas de ms (promedio de 30 ticks; MAX = tick mas largo). */
#include <pspctrl.h>
#include <pspiofilemgr.h>
extern short gCurrLevelNum;
extern short gCurrAreaIndex;
static char sProfLogBuf[16384];
unsigned int gPspLp[32]; /* perfil de la logica: ver pc/psp_lprof.h */
unsigned int gPspLc[16];
static unsigned int sProfLate, sProfLateLog, sProfLateEmit, sProfLateSync;
static unsigned int sProfAcc2[8]; /* desglose por clase de triangulo (ver sTriClass) */
static unsigned int sProfAcc3[4];
static unsigned int sProfAccTx[2]; /* TXS / TXE por ventana */
static unsigned int sProfBenchUc, sProfBenchCw; /* us por 1024 palabras escritas sin cache / con cache (+ vaciado) */
static unsigned int sProfAcc[16], sProfFrames, sProfListMax, sProfBenchGm, sProfBenchClk, sProfBenchCpu; /* contadores por ventana (ver sPc*) */
static int sProfLogLen = 0;
int sProfLogOn = 0;
/* Perfil de GPU (atajo Triangulo + L, solo con el registro ON): cada llamada de dibujo se serializa con el GE y se mide cuanto
 * tarda el GPU en ella (gfx_scegu.c), por tipo de pasada. Va MUY lento (decenas de ms por cuadro): solo para medir. */
int gPspGpuProf = 0;
float gPcAreaAcc = 0.0f; /* area en pixeles (aprox.) de los triangulos acumulados desde la ultima llamada de dibujo */
extern unsigned int gGpuUs[], gGpuTris[], gGpuPx[], gGpuCalls[];
static int sProfGpuWin = 0;   /* 1 si el perfil de GPU estuvo activo en la ventana */
#if PSP_GPU_PROF
static int sProfGpuPrevCombo = 0;
#endif
static unsigned int sProfLogWindows = 0;
static unsigned int sProfDroppedWin = 0;
static unsigned int sProfLastWall = 0;
static unsigned int sProfSixty = 0;
/* diagnostico del controlador de 60 fps (por ventana): ticks, suma de A, ticks sobre presupuesto, suma de pre / e / s, cuadros asincronos */
/* La presentacion asincrona solo compensa si hay cola de GE que solapar con la logica (JRB: agua/niebla, cola ~4 ms). En escenas
 * limitadas por CPU (TTM, cola ~0) solo anade cambios de hilo, asi que se activa con histeresis segun la cola media medida. */
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

int gPspLogStatus = 0;           /* 0 apagado, 1 escribiendo bien, 2 el ultimo intento de escribir fallo (lo muestra el contador de FPS) */
extern int gPspFpsCounterOn;     /* game_init.c: 1 mientras el contador de FPS se dibuja */
static unsigned int sProfLostW;  /* ventanas que no cupieron en el buffer porque no se pudo escribir */

/* Escribe el buffer en la tarjeta. Si algo falla (tarjeta ocupada, escritura corta...) el buffer se CONSERVA y se reintenta en la
 * proxima llamada, en vez de descartar el registro como antes (sin avisar). */
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

/* Se llama una vez por cuadro: el registro sigue al contador de FPS del juego (L+R) */
#ifndef PSP_PROFILE_LOG
#define PSP_PROFILE_LOG 0 /* 1 = permite escribir ms0:/sm64_profile.txt con el contador de FPS (solo para medir); 0 = version final, nunca escribe ni cronometra */
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
        /* firma del build: sirve para comparar logs entre compilaciones (compilador, optimizacion, opciones de este archivo) */
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
        prof_put_num("EYE2 ", gPspEye2Ok); /* 1 = kernel VFPU de 2 vertices comprobado y en uso, 0 = fallo la prueba (se usa el de siempre), -1 = aun sin probar */
        prof_put_str("\r\n");
#endif
        {
            extern int psp_me_debug(int which);
            prof_put_str("ME");
            prof_put_num(" BOOT ", psp_me_debug(0));  /* meLibDefaultInit: <0 = el Media Engine no arranco */
            prof_put_num(" INIT ", psp_me_debug(1));  /* 1 = el audio se sintetiza en el ME */
            prof_put_num(" PROG ", psp_me_debug(2));
            prof_put_num(" STATE ", psp_me_debug(3));
            prof_put_num(" BTRY ", psp_me_debug(4)); /* 1 = un intento, 3 = hubo que reintentar desde la carpeta de partidas */
            prof_put_str("\r\n");
        }
        prof_log_flush(); /* la cabecera queda guardada desde el primer momento */
    } else if (!want && sProfLogOn) {
        sProfLogOn = 0;
        prof_put_str("--- registro OFF ---\r\n");
        prof_log_flush();
        if (sProfLogLen > 0) prof_log_flush(); /* un segundo intento si el primero fallo */
        gPspLogStatus = 0;
        gPspGpuProf = 0;
    }
#if PSP_GPU_PROF
    /* Perfil de GPU (Triangulo + L, solo con el registro activo). Solo se compila con PSP_GPU_PROF 1: asi no se lee el pad en
     * cada cuadro cuando no hace falta. */
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

/* Se llama al cerrar cada ventana de 30 ticks, con gPspProf ya calculado */
static void prof_log_window(void) {
    const unsigned int now = sceKernelGetSystemTimeLow();
    if (sProfLogOn) {
        if (sProfLogLen >= (int) sizeof(sProfLogBuf) - 1500) {
            prof_log_flush(); /* buffer casi lleno: intentar vaciarlo antes de perder la ventana */
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
                /* promedios por cuadro dibujado de la ventana: TRI triangulos, VTX vertices, DRW llamadas de dibujo al GE,
                 * MTX matrices, CLP triangulos recortados por la CPU; TXM texturas importadas (suma de la ventana),
                 * TXC veces que se vacio la VRAM de texturas; TXMS ms en importar texturas, DRWMS ms dentro de las llamadas
                 * de dibujo al GE (CPU), ambos por ventana en decimas de ms; LST KB de lista de comandos del GE (maximo). */
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
                prof_put_num(" GMUS ", (int) sProfBenchGm);   /* decimas de us por sceGuGetMemory */
                prof_put_num(" CLKUS ", (int) sProfBenchClk); /* decimas de us por lectura del reloj */
                prof_put_num(" LOOPUS ", (int) sProfBenchCpu); /* us de 20000 vueltas de un bucle de 3 instrucciones: ~180 a 333 MHz reales */
                prof_put_num(" MHZE ", sProfBenchCpu ? (int) (60000U / sProfBenchCpu) : 0); /* MHz reales estimados (si 1 instruccion/ciclo) */
                /* EMIT desglosado: VTXMS ms (decimas, por cuadro) dentro de gfx_sp_vertex; TRIN triangulos recibidos por cuadro
                 * (TRI son los que salieron dibujados); TRIUS decimas de us por triangulo (muestreado, ya sin dibujo ni texturas,
                 * incluye ~1 lectura de reloj); FOGA/FOGK triangulos con niebla por cuadro / los que hacen 2a pasada. */
                prof_put_num(" VTXMS ", (int) (sProfAcc[9] / fr / 100));
                prof_put_num(" TRIN ", (int) (sProfAcc[10] / fr));
                prof_put_num(" TRIUS ", sProfAcc[12] ? (int) (sProfAcc[11] * 10 / sProfAcc[12]) : 0);
                /* TRIUS desglosado por clase: tiempo medio (decimas de us) de un triangulo descartado pronto (TRR), dibujado sin recorte
                 * (TRD) y recortado por la CPU (TRC); TRNR/TRND/TRNC = cuantos de cada clase por cuadro (estimado del muestreo 1/8);
                 * CLPUS = tiempo total del recorte por CPU por cuadro (decimas de ms) */
                prof_put_num(" TRR ", sProfAcc2[3] ? (int) (sProfAcc2[0] * 10 / sProfAcc2[3]) : 0);
                prof_put_num(" TRD ", sProfAcc2[4] ? (int) (sProfAcc2[1] * 10 / sProfAcc2[4]) : 0);
                prof_put_num(" TRC ", sProfAcc2[5] ? (int) (sProfAcc2[2] * 10 / sProfAcc2[5]) : 0);
                prof_put_num(" TRNR ", (int) (sProfAcc2[3] * 8 / fr));
                prof_put_num(" TRND ", (int) (sProfAcc2[4] * 8 / fr));
                prof_put_num(" TRNC ", (int) (sProfAcc2[5] * 8 / fr));
                prof_put_num(" CLPUS ", (int) (sProfAcc2[6] / fr / 100));
                prof_put_num(" FOGA ", (int) (sProfAcc[13] / fr));
                prof_put_num(" FOGK ", (int) (sProfAcc[14] / fr));
                prof_put_num(" CLPX ", (int) (sProfAcc[15] / fr)); /* triangulos que cruzaban un lado y se mandaron sin recortar */
                {
                    /* opciones del menu (game/psp_options.c) vigentes al cerrar esta ventana: OFPS limite de fps, OPAC 0 = Keep FPS / 1 = Keep Speed;
                     * OOBJ / OFAR = distancia de objetos / plano lejano de la camara en %; OCLK MHz pedidos;
                     * OCHG = cuantas veces se tocaron las opciones durante la ventana (si no es 0 la ventana mezcla dos configuraciones) */
                    static unsigned int sOptChg = 0;
                    prof_put_num(" OFPS ", gPspOptFpsCap);
                    prof_put_num(" OPAC ", gPspOptKeepSpeed);
                    prof_put_num(" OOBJ ", (int) (gPspObjDistScale * 100.0f + 0.5f));
                    prof_put_num(" OFAR ", (int) (gPspFarScale * 100.0f + 0.5f)); /* plano lejano de la camara, en % */
                    {
                        static unsigned int sPrevKick, sPrevRaw, sPrevWb;
                        prof_put_num(" OKCK ", (int) (gPspKicks - sPrevKick));    /* avisos al GE en la ventana */
                        prof_put_num(" ORAW ", (int) (gPspRawDraws - sPrevRaw));  /* draws enviados sin aviso individual */
                        prof_put_num(" OWBC ", (int) (gPspWbCalls - sPrevWb));   /* vaciados de cache de la arena en la ventana */
                        sPrevWb = gPspWbCalls;
                        sPrevKick = gPspKicks;
                        sPrevRaw = gPspRawDraws;
                    }
                    prof_put_num(" OFREE ", psp_objects_free());
                    prof_put_num(" OFMIN ", gPspFreeMin);                              /* minimo de objetos libres en la ventana */
                    gPspFreeMin = 9999;                       /* objetos libres de los 240 del juego */
                    prof_put_num(" OSWP ", sSweepMode);                                /* Profile Sweep: etapa apagada (0 = ninguna) */
                    prof_put_num(" OCLK ", gPspClockWant);
                    prof_put_num(" OPIPE ", gPspOptPipe);                                /* Triple Buffer 0/1 */
                    prof_put_num(" PGW ", gPspGateN ? (int) (gPspGateUs * 10U / gPspGateN / 1000U) : 0); /* espera media de start_frame por cuadro (0.1 ms) */
                    prof_put_num(" PQD ", (int) gPspQMax);                               /* mayor cola de cuadros sin presentar vista (0..3) */
                    prof_put_num(" PBRK ", gPspPipeBroken);                              /* 1 = un semaforo vencio y se volvio al modo sincrono */
                    prof_put_num(" OCUL ", gPspOptCullObj);                       /* 0 = culling de objetos original, 1 = tambien vertical */
                    prof_put_num(" OCHG ", (int) (gPspOptChanges - sOptChg));
                    sOptChg = gPspOptChanges;
                }
                prof_put_num(" TXS ", (int) (sProfAccTx[0] / fr));            /* cambios de textura (tile 0) que ya era la enlazada: no cortan el lote (por cuadro) */
                prof_put_num(" TXE ", (int) (sProfAccTx[1] / fr));            /* cambios de textura (tile 0) pedidos por el juego (por cuadro) */
                /* MTXMS: ms por cuadro (decimas) en gfx_sp_matrix; PREPMS: ms por cuadro (decimas) rehaciendo el estado del pipeline de
                 * triangulos (cambio de textura / combinador / modo; sin contar el dibujo ni la importacion de texturas); PREPN: cuantas
                 * veces por cuadro. UCW / CW: us por 1024 palabras escritas SIN cache (como pspgu escribe la lista) / CON cache + vaciado. */
                prof_put_num(" MTXMS ", (int) (sProfAcc3[0] / fr / 100));
                prof_put_num(" PREPMS ", (int) (sProfAcc3[1] / fr / 100));
                prof_put_num(" PREPN ", (int) (sProfAcc3[2] / fr));
                prof_put_num(" UCW ", (int) sProfBenchUc);
                prof_put_num(" CW ", (int) sProfBenchCw);
                prof_put_num(" ARN ", (int) (gPspArenaMax / 1024)); /* KB maximos usados de la arena de vertices */
                /* cuadros "justos" (logica + dibujado + espera del GE > 14,5 ms): cuantos hubo en la ventana y, en promedio,
                 * cuanto fue de logica / emision / espera al GE (decimas de ms) */
                prof_put_num(" LATE ", (int) sProfLate);
                prof_put_num(" LLOG ", sProfLate ? (int) (sProfLateLog / sProfLate / 100) : 0);
                prof_put_num(" LEMT ", sProfLate ? (int) (sProfLateEmit / sProfLate / 100) : 0);
                prof_put_num(" LSYN ", sProfLate ? (int) (sProfLateSync / sProfLate / 100) : 0);
            }
            {
                /* Logica, promedio POR TICK DE LOGICA en decimas de ms (el cronometro suma ~0,5 us por medicion):
                 * AUD audio, INP controles, SCR script del nivel completo, UPD update_level, OBJ update_objects (TERR terreno,
                 * COLL colisiones entre objetos, NTER resto de objetos), CAM camara, MAR Mario (dentro de OBJ), FLR/WAL/CEL
                 * busquedas de suelo/pared/techo (dentro de OBJ/MAR/CAM), RND render_game completo, GEO geo_process_root.
                 * Conteos por tick: NFLR/NWAL/NCEL llamadas, NOBJ objetos actualizados, NGO objetos y NGN nodos del grafo. */
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
                prof_put_num(" AEST ", (int) (sDbgA / tk2 / 100));      /* A medio por tick (decimas de ms) */
                prof_put_num(" AOVR ", (int) (sDbgOver * 100 / tk2));   /* % de ticks con A > presupuesto */
                prof_put_num(" PRE ", (int) (sDbgPre / fr2 / 100));     /* logica previa al dibujo, por cuadro */
                prof_put_num(" EUS ", (int) (sDbgE / fr2 / 100));       /* enviar el cuadro (sin esperas) */
                prof_put_num(" SUS ", (int) (sDbgS / fr2 / 100));       /* cola del GE usada por la estimacion */
                prof_put_num(" ASY ", (int) sDbgAsy);
                prof_put_num(" TAIL ", (int) (sTailEma / 100));          /* cola media del GE (decimas de ms) */
                prof_put_num(" AWANT ", gPspAsyncWant);                 /* 1 = presentacion asincrona elegida */
                {
                    extern unsigned int gPspF2Skip;
                    static unsigned int sF2Last = 0;
                    prof_put_num(" F2SK ", (int) (gPspF2Skip - sF2Last)); /* 2os cuadros descartados en la ventana */
                    {
                        /* PSP_F2_REPLAY: RPL = 2os cuadros que repitieron la escena del 1o; el resto son los motivos por los que no
                         * se pudo (RPN: sin grabacion, RPR: lista reiniciada, RPT: almacen de texturas vaciado, RPZ: varias mascaras
                         * de ruido, RPB: la comprobacion de la lista fallo, RPO: desactivado tras fallos seguidos).
                         * Etapa 2: RPP = de los RPL, los que llevaron las matrices reales parcheadas; RPM = no se pudo parchear por
                         * la anotacion de matrices; RPD = un objeto se movio mas que la distancia near; RPQ = otro parche pendiente
                         * (HUD, dialogo, transicion, nieve, burbujas, cuadros, fov). RPM/RPD/RPQ son cuadros dibujados enteros. */
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
                            extern unsigned int gPspRepSky; /* cuadros 2 con el cielo reescrito */
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
                        prof_put_num(" AUDN ", (int) (gPspAudCpuN - sAudNLast));         /* bloques de audio sintetizados en la CPU (0 = en el ME) */
                        prof_put_num(" AUDUS ", (int) ((gPspAudCpuUs - sAudUsLast) / 100)); /* tiempo de esa sintesis (decimas de ms) */
                        sAudNLast = gPspAudCpuN; sAudUsLast = gPspAudCpuUs;
                    }
                    {
                        /* voz del titulo (acumulado): peticiones/reintentos/confirmadas sonando/ultima (1=Hello 2=Press Start) */
                        extern unsigned int gPspVox[4];
                        prof_put_num(" VOX ", (int) gPspVox[0]);
                        prof_put_num("/", (int) gPspVox[1]);
                        prof_put_num("/", (int) gPspVox[2]);
                        prof_put_num("/", (int) gPspVox[3]);
                    }
                    {
                        /* Audio con el ME: el hilo principal manda bloques al ME al terminar la logica y los recoge al empezar el
                         * siguiente tick. MEN = bloques mandados en la ventana; MEWN = ticks en que el ME aun no habia terminado y la
                         * CPU tuvo que esperar; MEWMS = esa espera total (decimas de ms por ventana); AUDUR = cortes de audio
                         * (el anillo se vacio y sono silencio) en la ventana. */
                        extern unsigned int gPspMePolls, gPspMeUs, gPspMeBlocks, gPspMeWaits, gPspAudUnder;
                        static unsigned int sMeU = 0, sMeB = 0, sMeW = 0, sAudUr = 0;
                        prof_put_num(" MEN ", (int) (gPspMeBlocks - sMeB));
                        prof_put_num(" MEWN ", (int) (gPspMeWaits - sMeW));
                        prof_put_num(" MEWMS ", (int) ((gPspMeUs - sMeU) / 100));
                        prof_put_num(" AUDUR ", (int) (gPspAudUnder - sAudUr));
                        extern unsigned int gPspMeTimeouts, gPspMeRecovered;
                        extern int psp_me_debug(int which);
                        prof_put_num(" MEFAIL ", (int) gPspMeTimeouts); /* acumulado: trabajos del ME que pasaron los 100 ms */
                        prof_put_num(" MEREC ", (int) gPspMeRecovered); /* acumulado: de esos, los que terminaron tarde y se recuperaron */
                        prof_put_num(" MEBUSY ", psp_me_debug(5));
                        /* suspension: reanudaciones del ME buenas/malas, veces que se llamo a meLibOnWake, estado del ME al dormir (5 = detenido), y si se rindio: estado/progreso */
                        prof_put_num(" MERES ", psp_me_debug(6));
                        prof_put_num(" MEREF ", psp_me_debug(7));
                        prof_put_num(" MEWK ", psp_me_debug(8));
                        prof_put_num(" MESS ", psp_me_debug(9));
                        prof_put_num(" MERS ", psp_me_debug(10));
                        prof_put_num(" MERP ", psp_me_debug(11));
                        prof_put_num(" MEI ", psp_me_debug(1));
                        prof_put_num(" MEN0 ", psp_me_debug(12)); /* llamadas a power_event(0) / (1) y flags de los ultimos avisos de energia */
                        prof_put_num(" MEN1 ", psp_me_debug(13));
                        prof_put_num(" MEF0 ", psp_me_debug(14));
                        prof_put_num(" MEF1 ", psp_me_debug(15));
                        prof_put_num(" MEF2 ", psp_me_debug(16));
                        prof_put_num(" MEF3 ", psp_me_debug(17));
                        (void) gPspMePolls;
                        sMeU = gPspMeUs; sMeB = gPspMeBlocks; sMeW = gPspMeWaits; sAudUr = gPspAudUnder;
                    }
                    {
                        /* NOTAS = mayor numero de notas del sintetizador en uso (tope PSP_AUDIO_NOTES); NSTL = notas robadas a otras
                         * en uso; NDRP = capas que se quedaron sin nota (no suenan); SFXC = suma de sonidos que competian por el canal
                         * de su banco (1 por banco) en cada tick. Todo en la ventana. */
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
                    /* reloj real de CPU/bus (MHz): si el sistema lo baja (modo ahorro de energia, bateria, plugin) se vuelve a pedir 333/166 */
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
                /* Perfil de GPU, promedio por cuadro: Gi = us de GE, Ni = triangulos, Pi = kpixeles (area aprox.), Ci = llamadas.
                 * i: 0 opaco 1 recorte 2 translucido (sin niebla); 3 4 5 igual pero con niebla (1a pasada);
                 * 6 pasada de niebla (opaco/recorte) 7 pasada de niebla (translucido) 8 especial (ruido/Peach) 9 sprites 2D
                 * 10 espera de GE entre llamadas (limpiar, texturas...). */
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
            sProfLostW++; /* no cupo y no se pudo escribir: se avisa en la siguiente linea que se guarde */
        }
        {
            int q;
            for (q = 0; q < 11; q++) { gGpuUs[q] = gGpuTris[q] = gGpuPx[q] = gGpuCalls[q] = 0; }
            sProfGpuWin = gPspGpuProf;
        }
        sProfLogWindows++;
        if ((sProfLogWindows % 5) == 0 || gPspLogStatus == 2) {
            prof_log_flush(); /* cada 5 s (o en cada ventana mientras haya un error pendiente) */
        }
    }
    sProfLastWall = now;
    sProfDroppedWin = 0;
    sProfSixty = 0;
    sDbgTk = sDbgA = sDbgOver = sDbgFr = sDbgPre = sDbgE = sDbgS = sDbgAsy = 0;
}

/* ---- 60 fps dinamicos (PSP): "a los cuadros que alcance" ----
 * La logica del juego sigue a 30 Hz. Cada tick presenta UNO o DOS cuadros: con dos, el primero sale con las matrices
 * interpoladas (punto medio entre el tick anterior y el actual) y el segundo con el estado real. Mezclando ticks de 1 y de 2
 * cuadros se obtienen velocidades intermedias: si la fraccion de ticks con 2 cuadros es p, la imagen va a 30 + 30 * p fps
 * (p = 1/2 -> 45 fps, 2/3 -> 50 fps). El maximo es 60.
 * Un tick "cabe" si A = logica + dibujado (CPU + espera del GPU, sin esperar al vblank) <= PSP60_BUDGET_US, o sea si cabria
 * en un vblank (16,7 ms). Se mide con que frecuencia caben los ticks (h, 0..1000) y se usa p = h: si en una zona caben todos
 * -> 60 fps; si solo caben la mitad -> ~45 fps; si no cabe ninguno -> 30 fps. Un acumulador (tipo Bresenham) reparte los
 * ticks de 2 cuadros de forma pareja (45 fps = 2,1,2,1...). */
#define PSP_60FPS 1               /* 0 = siempre 30 fps */
#define PSP_60FPS_FORCE 1         /* 1 = 60 fps fijos: si la escena no cabe, va mas lento (con trompicones) pero siempre intenta 60. 0 = dinamico 30/60 */
#define PSP60_BUDGET_US 16000U    /* un tick de 30 fps cabe si A <= esto (un vblank = 16667 us, con un poco de margen) */
#define PSP60_SLACK_US 1500U      /* "Keep Speed": un tick de 30 fps (que lleva la estimacion PSP60_EXTRA_US) solo cuenta como "no cabe" si pasa del
                                   * presupuesto por mas de esto. Entre medias no cuenta ni a favor ni en contra (ver gfx_psp_update_mode). */
#define PSP60_WALL_OK_US 35000U   /* "Keep Speed": un tick de 60 fps cabe mientras el tiempo REAL por tick (promedio movil) no pase de esto (2 vblanks = 33333 us) */
#define PSP60_WALL_CLAMP_US 45000U /* un tick real mas largo que esto (carga, menu HOME, cambio de modo) cuenta como esto, para que un tropiezo suelto no pese de mas */
#define PSP60_WALL_SHIFT 3        /* ventana del promedio del tiempo real por tick (~8 ticks) */
#define PSP60_EXTRA_US 1000U      /* estimacion del coste del parche de matrices del 2o cuadro (se suma en ticks de 1 cuadro) */
#define PSP60_IGNORE_US 50000U    /* muestras mayores (carga de nivel, menu HOME...) no cuentan */
#define PSP60_DOWN_SHIFT 3        /* si empeora, reacciona en ~8 ticks */
#define PSP60_UP_SHIFT 3          /* igual al subir: asi p coincide con la fraccion real de ticks que caben */

/* gfx_psp.c lo define de verdad; esta definicion debil solo evita el error de enlace si gfx_psp.c es la version vieja. */
int gPspSixtyMode __attribute__((weak)) = 0;
extern int gPspPresentAsync;           /* gfx_scegu.c: este cuadro se presento de forma asincrona (hilo) */
extern unsigned int gPspProfPendWait;  /* us que start_frame espero a la presentacion asincrona anterior */
extern unsigned int gPspAsyncSync;     /* de esa presentacion: us desde end_frame hasta que el GE termino */
extern int gPspPendingDone;
static unsigned int sLastAsyncS;       /* ultima cola del GE medida en una presentacion asincrona (us) */
/* Modelo de la presentacion asincrona del 2o cuadro (ver el calculo de A en gfx_run): mientras el hilo espera al GE y al vblank del 2o
 * cuadro, el hilo principal ya hace la logica del tick siguiente. Esa logica y el envio del 2o cuadro caben en el hueco de un vblank
 * (PSP60_BUDGET_US); solo lo que pase de ahi retrasa el envio del 1er cuadro. Con el replay, el envio del 2o cuadro es casi nada. */
static int sLastF2Async;               /* el 2o cuadro anterior se presento de forma asincrona */
static unsigned int sLastF2E = 1000;   /* su parche + envio (us) */
int gPspReplayEma = 0;                 /* 0..1000: fraccion reciente de 2os cuadros que repitieron la lista (sube sola en 30 fps para reintentar) */
static unsigned int sTickPre;          /* mayor tiempo previo a dibujar (logica) de los cuadros del tick actual */
static unsigned int sTickMaxA;
static int sFitRate = 1000; /* milesimas de ticks que caben; empieza optimista y cae rapido si no */
static unsigned int sLastTickUs;     /* reloj (us) en la llamada anterior a gfx_psp_update_mode */
static int sWallEma = 33333;         /* promedio movil del tiempo real por tick de 60 fps (us) */
static unsigned int sAcc;   /* acumulador de p en milesimas */

void gfx_psp_update_mode(void) {
#if PSP_60FPS
    unsigned int a = sTickMaxA;
    sTickMaxA = 0;
    sTickPre = 0;
#if PSP_F2_REPLAY && PSP_F2_REPLAY_MTX
    if (!gPspSixtyMode) {
        gPspReplayEma += (1000 - gPspReplayEma) >> 5; /* en 30 fps no hay muestras: reintenta de vez en cuando */
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
        /* Tiempo REAL del tick que acaba de terminar (entre dos llamadas). En los ticks de 60 fps es la medida directa de si el juego va a
         * su velocidad (2 vblanks = 33333 us): la estimacion A de abajo es pesimista (a 60 fps estables A puede salir por encima del
         * presupuesto en la mayoria de ticks), y con ella "Keep Speed" bajaba de 50 fps donde "Keep FPS" iba a 60 estables. */
        const unsigned int now = (unsigned int) sceKernelLibcClock();
        unsigned int w = sLastTickUs ? now - sLastTickUs : 0U;
        sLastTickUs = now;
        if (a != 0 && a < PSP60_IGNORE_US) { /* a == 0: no se dibujo ningun cuadro (saltado), no hay muestra */
            int fit; /* 1 = sube la tasa, -1 = baja, 0 = no cuenta */
            if (!gPspSixtyMode) {
                a += PSP60_EXTRA_US;
            }
            sDbgTk++; sDbgA += a; if (a > PSP60_BUDGET_US) sDbgOver++;
            if (gPspSixtyMode) {
                /* tick de 60 fps: cabe si el tiempo real (promedio movil) no pasa de ~2 vblanks */
                if (w != 0U) {
                    if (w > PSP60_WALL_CLAMP_US) w = PSP60_WALL_CLAMP_US;
                    sWallEma += ((int) w - sWallEma) >> PSP60_WALL_SHIFT;
                }
                fit = sWallEma <= (int) PSP60_WALL_OK_US ? 1 : -1;
            } else {
                /* tick de 30 fps: no hay tiempo real que mirar (siempre ~33 ms); se estima si cabria en 60 fps. Solo cuenta si claramente cabe
                 * o claramente no cabe (la estimacion lleva PSP60_EXTRA_US, pesimista porque el 2o cuadro se repite casi gratis). */
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
    /* los extremos se redondean para no quedar en 59 / 31 fps por la cola del promedio */
    {
        unsigned int p = (unsigned int) sFitRate;
        {
            /* Menu de opciones (game/psp_options.c): fraccion de ticks con 2 cuadros que corresponde al limite de fps (30 -> 0, 45 -> 1/2,
             * 60 -> todos). "Keep FPS": siempre esa fraccion (si no cabe, el juego va mas lento); "Keep Speed": a lo sumo esa fraccion y solo
             * los ticks que caben (como el modo dinamico de antes). */
            const unsigned int cap = (unsigned int) ((gPspOptFpsCap - 30) * 1000 / 30);
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
            gPspSixtyMode = 0; /* cambio de nivel / transicion: modo normal (ver level_update.c) */
        }
    }
#endif
#else
    gPspSixtyMode = 0;
#endif
}

/* ---- PSP_F2_REPLAY (opcion 3, etapa 1) ----
 * Rol del cuadro que se va a enviar (lo pone pc_main.c): 0 = normal (30 fps, o un cuadro que no participa), 1 = 1er cuadro de un tick de
 * 60 fps (se graba la escena en la lista del GE), 2 = 2o cuadro (si la grabacion vale, se repite en vez de recorrer la lista otra vez).
 * La mecanica de la lista esta en gfx_scegu.c. */
extern void gfx_scegu_rec_reset(void);
extern int gfx_scegu_rec_begin(void);
extern void gfx_scegu_rec_end(void);
extern int gfx_scegu_replay_ready(void);
extern void gfx_scegu_replay_arm(int on);
extern void gfx_scegu_replay_scene(void);
static int sFrameRole;
static int sReplayDecision;  /* lo decide gfx_psp_replay_prepare() y lo consume gfx_run() */
/* Contadores (por ventana en el log): RPP = cuadros 2 repetidos con las matrices reales parcheadas; RPM = no se pudo parchear porque la
 * anotacion de matrices no cuadraba; RPD = un objeto se movio mas de lo permitido; RPQ = habia otro parche pendiente (HUD, nieve...). */
unsigned int gPspRepPatched, gPspRepMis, gPspRepDelta, gPspRepPend;
unsigned int gPspRepSky; /* cuadros 2 en los que tambien se actualizo el cielo */
unsigned int gPspRepWhy[7]; /* motivos de RPM: [0] sin grabar, [1] demasiadas entradas, [2] MUL anidada, [3] bloque raro, [4] sin emparejar, [5] la cuenta cambio, [6] otro */
static void gfx_invalidate_render_state(void);

void gfx_psp_set_role(int role) {
    sFrameRole = role;
#if PSP_F2_REPLAY
    if (role != 2) {
        gfx_scegu_rec_reset(); /* una grabacion de un tick anterior no puede usarse nunca */
#if PSP_F2_REPLAY_MTX
        sRecMtxValid = 0;
        sRecMtxOn = 0;
        sRecCur = -1;
#endif
    }
#endif
}

/* Lo llama pc_main.c en el 2o cuadro de un tick de 60 fps, justo antes de patch_interpolations() (que vacia la tabla de matrices).
 * pending = 1 si el juego tiene algun parche que no es solo "cambiar la matriz de un objeto". Decide si el cuadro se repite desde la
 * lista del GE (y en ese caso deja ya escritas las matrices reales) o se dibuja entero. */
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
            /* La CPU recorta y descarta con las matrices del cuadro 1. Si un objeto tiene triangulos pegados al plano near (sRecNear) y
             * se movio mas de unas unidades en esa zona entre los dos cuadros, el 2o cuadro podria dejar un hueco (corte viejo) o
             * mandar al GE un triangulo cruzando el near sin recortar: en ese caso (poco habitual) se vuelve a emitir la escena.
             * Los demas objetos (lo normal) no se miran: un objeto lejano que gira no tiene nada que ver con el plano near. */
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
                /* GU_VIEW nueva = ortografica real x lo que habia acumulado antes (mismo orden que gfx_sp_matrix) */
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

/* pc_main.c: el 2o cuadro repetira la lista del 1o (se decide en gfx_psp_replay_prepare, antes de enviar) */
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
    gfx_scegu_replay_arm(replay); /* decide ya que lista abrira start_frame */
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
    unsigned int pend_w = 0; /* us esperando a la presentacion asincrona del cuadro anterior (dentro de start_frame) */
    if (gPspPendingDone) {
        pend_w = gPspProfPendWait;
        sLastAsyncS = gPspAsyncSync;
        gPspPendingDone = 0;
        gPspProfPendWait = 0;
    }
    if (sProfLogOn && sProfFrames == 0) {
        /* Medicion: coste de una llamada sceGuGetMemory (incluye la llamada al sistema que avisa al GE) y de leer el reloj.
         * Se repite 64 veces una vez por ventana; ocupa unos bytes de la lista. Resultado en decimas de microsegundo. */
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
            /* Escribir 1024 palabras en memoria SIN cache (como pspgu escribe la lista de comandos del GE: sceGuStart suma 0x40000000)
             * frente a CON cache y vaciando despues (como hace gfx_scegu.c con la arena de vertices). Da el tope de lo que ahorraria
             * pasar la lista a escritura con cache. No toca la lista del GE. */
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
            /* Medicion del reloj REAL de la CPU: un bucle de 3 instrucciones (addiu / bnez / nop) con una cadena de dependencia, de
             * 20000 vueltas, se repite 3 veces y se toma el menor tiempo (otro hilo puede interrumpir una vuelta). Con la CPU a
             * 333 MHz y 1 instruccion por ciclo son ~180 us; si el reloj real es mayor/menor el tiempo cambia en proporcion.
             * scePowerGetCpuClockFrequency solo dice lo que se PIDIO (CFW como ARK puede aplicar otra cosa), esto lo comprueba. */
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
        /* 2o cuadro: la escena del 1o ya esta en la lista del GE; el estado del renderizador (rendering_state, rsp...) y el del GE
         * quedaron como al final del 1er cuadro, y la escena los deja igual otra vez. */
        gfx_scegu_replay_scene();
    } else {
        if (sFrameRole == 1) {
            /* Para que la escena grabada no dependa de como terminara el cuadro anterior, todo el estado cacheado se da por perdido y
             * se vuelve a enviar al GE la primera vez que se usa (igual que tras un cuadro de callback). */
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
        /* se dispara con L + R + SELECT a la vez (en la escena que se quiera medir); para probar en el emulador, a los 500 cuadros */
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
    gfx_scegu_replay_arm(0); /* la lista corta B solo vale para este cuadro (otros usos del GU, p.ej. el menu HOME, usan la grande) */
#endif
    {
        const unsigned int ts0 = sceKernelLibcClock();
        sProfGfx = ts0 - t0;
        sProfSyncCur = gPspProfSync;
        sProfVblCur = gPspProfVblank + pend_w;
#if PSP_ASYNC_PRESENT
        {
            /* Coste de un cuadro con la presentacion asincrona: la logica (pre) y la cola del GE (s) se solapan, asi que un
             * cuadro cabe si  E + max(logica, cola GE) <= vblank,  con E = enviar el cuadro (sin esperas). En el ultimo cuadro
             * de un tick la cola es la medida en el tick anterior (el hilo aun no ha terminado). */
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
                /* 2o cuadro repetido y asincrono: su logica ya paso; lo que cuenta es que su GE termine antes del vblank */
                a = pre + e_us + s_used;
            } else if (sFrameRole != 2 && (sLastF2Async || gPspReplayEma >= 600)) {
                /* El 2o cuadro anterior se presento (o se presentara) de forma asincrona: la logica de este tick + el envio de aquel cuadro
                 * se solapan con su espera al GE / vblank. Solo lo que pasa de un vblank retrasa el envio de este cuadro. */
                const unsigned int hide = (sLastF2Async ? sLastF2E : 1000U) + pre;
                a = e_us + s_used + (hide > PSP60_BUDGET_US ? hide - PSP60_BUDGET_US : 0U);
            } else
#endif
            if (gPspAsyncWant) {
                a = e_us + (sTickPre > s_used ? sTickPre : s_used); /* la logica se solapa con la cola del GE */
            } else {
                a = sTickPre + e_us + s_used; /* sin solape: logica + enviar + esperar al GE */
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
/* ---- Diagnostico en la PSP real (PSP_DEVBENCH 1): a los ~7 s de arrancar (cuadro 400) mide, con relojes reales, cuanto cuesta cada etapa del
 * procesado de vertices/triangulos con datos falsos, escribe ms0:/sm64_devbench.txt y cierra el juego. Cada medicion = el MINIMO de 3 repeticiones
 * (los otros hilos, como el audio, solo pueden sumar tiempo). "hot" = mismos vertices una y otra vez (en la cache de datos); "cold" = recorre
 * 96 KB de vertices (mas que los 16 KB de cache), como pasa en el juego. No se usa en el juego normal. */
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

    /* --- memoria --- */
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

    /* --- escrituras en memoria fria (lo que hace el juego al copiar vertices a la arena) --- */
    {
        static unsigned char sDbWr[98304] __attribute__((aligned(64)));
        int off = 0;
        /* 1 KB por iteracion, recorriendo 96 KB: siempre lineas que no estan en la cache */
        DB_TIME(r, 4000, { unsigned char *d = sDbWr + off;
            for (int q = 0; q < 1024; q += 64) __asm__ volatile("vzero.q c000\n sv.q c000, 0(%0)\n sv.q c000, 16(%0)\n sv.q c000, 32(%0)\n sv.q c000, 48(%0)\n" :: "r"(d + q) : "memory");
            off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("write_cold_cached_1KB %8.1f ns/KB  (sv.q, cache de datos)\n", r);
        off = 0;
        DB_TIME(r, 4000, { unsigned char *d = (unsigned char *)((unsigned int)(sDbWr + off) | 0x40000000u);
            for (int q = 0; q < 1024; q += 64) __asm__ volatile("vzero.q c000\n sv.q c000, 0(%0)\n sv.q c000, 16(%0)\n sv.q c000, 32(%0)\n sv.q c000, 48(%0)\n" :: "r"(d + q) : "memory");
            off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("write_cold_uncached   %8.1f ns/KB  (sv.q, direccion sin cache)\n", r);
        off = 0;
        DB_TIME(r, 4000, { memcpy_vfpu(sDbWr + off, sDbMemA, 1024); off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("copy_hot_to_cold_1KB  %8.1f ns/KB  (memcpy_vfpu, origen en cache -> destino frio)\n", r);
        off = 0;
        DB_TIME(r, 4000, { memcpy_vfpu(sDbWr + off, sDbMemA, 1024); sceKernelDcacheWritebackRange(sDbWr + off, 1024); off += 1024; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("copy+writeback_1KB    %8.1f ns/KB  (como la arena de vertices del juego)\n", r);
        off = 0;
        DB_TIME(r, 1000, { memcpy_vfpu(sDbWr + off, sDbMemA, 4096); sceKernelDcacheWritebackRange(sDbWr + off, 4096); off += 4096; if (off >= (int)sizeof(sDbWr)) off = 0; });
        db_printf("copy+writeback_4KB    %8.1f ns/KB\n", r / 4.0);
        DB_TIME(r, 20000, sceKernelDcacheWritebackRange(sDbWr, 64));
        db_printf("writeback_range_call  %8.1f ns/call (64 B limpios)\n", r);
    }

    /* --- matrices de prueba (finitas, normales) --- */
    memset(sDbM, 0, sizeof(sDbM));
    sDbM[0] = 0.001f; sDbM[5] = 0.001f; sDbM[10] = 0.001f; sDbM[15] = 1.0f; sDbM[11] = 0.0003f;
    sDbM[16] = sDbM[21] = sDbM[26] = sDbM[31] = 1.0f;

    /* --- kernels de transformacion sueltos (sin el resto de gfx_sp_vertex_impl) --- */
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

    /* --- triangulos, sin tocar el GE: Profile Sweep 5 = vuelve al entrar en gfx_sp_tri1 (coste de la llamada), 4 = hasta antes de preparar el
     * estado (descarte, scissor, recorte) --- */
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

    /* --- cache de instrucciones: el mismo triangulo (Profile Sweep 4, en pantalla) solo, con vertices antes de cada par de triangulos
     * (otro codigo en medio) y las dos cosas por separado. Si "mezclado" sale mucho mas que la suma, el codigo se estorba en la cache. */
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
        db_printf("icache: tri_x2 solo %7.1f  vtx_x4(lit) solo %7.1f  mezclado %7.1f  (exceso %7.1f ns)\n", t_tri, t_vtx, t_mix, t_mix - t_tri - t_vtx);
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

/* Microprueba: el primer triangulo de cada clase (niebla 0..3 x shade_only x textura) que pasa limpio por el descarte se repite aislado, con el estado
 * del cuadro real y todo en cache, con cada variante de gfx_tri_core: coste "en caliente" del codigo de salida, y la huella del buffer que produce. */
static const int sB5Vars[4] = { 0, 1, 3, 7 };
static double sB5MicroNs[16][4];
static unsigned int sB5MicroH[16][4];
static unsigned int sB5MicroN[16];
/* Escribe lo acumulado hasta ahora en ms0:/sm64_devbench.txt (si la consola se cuelga a mitad, queda hasta donde llego). */
static void b5_save(void) {
    SceUID fd = sceIoOpen("ms0:/sm64_devbench.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) { sceIoWrite(fd, sDbBuf, sDbLen); sceIoClose(fd); }
}
static unsigned int sB5FuzzN, sB5FuzzBad[2];
/* Prueba de equivalencia: triangulos con z/w, colores, mul/offset de niebla al azar (incluye w casi 0, negativas, fuera de rango) con las 3 variantes de salida;
 * la variante 1 (bucle de siempre) es la referencia y las 3 y 7 deben dar exactamente los mismos bytes en el buffer y los mismos factores de niebla. */
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
    /* Sin ajuste de decal durante la microprueba y la prueba al azar: ese ajuste manda comandos REALES al GE cuando cambia (con vertices al azar
     * serian miles y desbordarian la lista). */
    const bool saveDecal = rendering_state.decal_mode;
    rendering_state.decal_mode = false;
    db_printf("STAGE capturando clase %d: prueba al azar\n", key); b5_save();
    b5_fuzz(tv);
    db_printf("STAGE clase %d: microprueba\n", key); b5_save();
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

/* Diagnostico 5 (solo modo usuario, sin nada de kernel): repite el cuadro real varias veces cortando o quitando partes del trabajo.
 * La diferencia de tiempo entre dos configuraciones es lo que cuesta lo que se quito. Nada llega al GE.
 * Cada configuracion se mide 1 vez en frio (caches vaciadas justo antes) y 5 en caliente (se toma el menor tiempo). */
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
    /* sweep, cull, abl, variante de salida de triangulos, variante de matrices */
    static const int cfg[NC][5] = {
        { 7, 0, 0, 0, 0 }, { 6, 0, 0, 0, 0 }, { 5, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 4, 0, 0, 0, 0 }, { 3, 0, 0, 0, 0 }, { 2, 0, 0, 0, 0 },
        { 5, 0, 1 << 6, 0, 0 },
        { 2, 0, 0, 0, 0 }, { 2, 0, 1 << 0, 0, 0 }, { 2, 0, 1 << 1, 0, 0 }, { 2, 0, 1 << 2, 0, 0 }, { 2, 0, 1 << 3, 0, 0 }, { 2, 0, 1 << 4, 0, 0 }, { 2, 0, 1 << 5, 0, 0 }, { 2, 0, 7, 0, 0 },
        { 2, 0, 0, 3, 0 }, { 2, 0, 0, 3, 0 }, { 2, 0, 0, 3, 1 }, { 2, 0, 0, 3, 2 }, { 2, 0, 0, 3, 3 },
        { 2, 0, 1 << 8, 3, 0 } };
    static const char *cname[NC] = {
        "A0 recorrido de la lista (sin vertices ni triangulos)", "A1 + transformacion de vertices", "A2 + iluminacion/resto de vertices",
        "A3 + despacho del triangulo y descarte (clip/cara trasera)", "A4 + preludio de gfx_tri_core (scissor, recorte)", "A5 + prepare/enva/lod",
        "A6 + bucle de salida de vertices + flush (todo menos el dibujo)", "V  recorrido + preparacion de cada carga de vertices (sin transformar)",
        "B0 control (salida de siempre): todo menos el dibujo", "B1 sin niebla por vertice", "B2 sin u/v de textura", "B3 sin copiar color", "B4 sin llamar a scegu_fog_mode",
        "B5 sin test de scissor", "B6 sin gfx_prepare_tri_pipeline_state", "B7 salida minima (sin niebla, u/v ni color)",
        "P  produccion (parche 113): salida variante 3", "P2 igual que P (mide el ruido)",
        "M1 produccion + MP de matrices perezoso", "M2 produccion + gfx_update_mv sin copias", "M3 produccion + M1 + M2",
        "D8 produccion sin G_MTX / G_POPMTX" };
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
    db_printf("STAGE configuraciones %d..%d\n", clo, chi - 1); b5_save();
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
    db_printf("STAGE huellas de todo el cuadro\n"); b5_save();
    /* huella del buffer de salida de TODO el cuadro con cada variante (deben ser iguales: mismo resultado bit a bit) */
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
        db_printf("huella del buffer de todo el cuadro (salida/matrices): 0/0 %08x  1/0 %08x  3/0 %08x  7/0 %08x  3/1 %08x  3/2 %08x  3/3 %08x   %s\n",
                  hv[0], hv[1], hv[2], hv[3], hv[4], hv[5], hv[6], same ? "IGUALES" : "DISTINTAS");
        db_printf("STAGE tiempos por comando\n"); b5_save();
        {
            /* tiempo de cada tipo de comando de la lista (inclusivo: G_DL incluye lo que ejecuta) y de partes del camino de matrices */
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
            db_printf("tiempo por tipo de comando, descontado el reloj (%.0f ns por lectura), por cuadro; %u\n", ovns, ntm & 1u);
            for (oi = 0; oi < nord && oi < 16; oi++) {
                const unsigned int o = order[oi];
                const double n = (double) sB5OpN[o] / 3.0;
                const double us = ((double) sB5OpAcc[o] / 3.0) - n * ovns / 1000.0;
                db_printf("  op 0x%02x %-10s %6.0f cmds  %8.0f us  %7.0f ns/cmd\n", o, b5_opname(o), n, us, n > 0 ? us * 1000.0 / n : 0.0);
            }
            {
                static const char *sn[8] = { "gfx_update_mv", "luces (calculate_normal_dir)", "copia MP -> sXfm", "", "", "flush de MP perezoso", "", "" };
                for (oi = 0; oi < 8; oi++) {
                    if (!sB5SecN[oi]) continue;
                    {
                        const double n = (double) sB5SecN[oi] / 3.0;
                        const double us = ((double) sB5Sec[oi] / 3.0) - n * ovns / 1000.0;
                        db_printf("  seccion %-30s %6.0f veces  %8.0f us  %7.0f ns/vez\n", sn[oi], n, us, n > 0 ? us * 1000.0 / n : 0.0);
                    }
                }
            }
        }
#ifdef PSP_B5_MICRO
        /* microprueba aislada por clase de triangulo (solo emulador: en la PSP real se colgaba) */
        db_printf("STAGE captura de clases\n"); b5_save();
        sB5Cap = 1;
        gfx_sp_reset();
        gfx_run_dl(root);
        gfx_flush();
        sB5Cap = 0;
#endif
        db_printf("STAGE tiempos de prepare\n"); b5_save();
        /* tiempos de las secciones de gfx_prepare_tri_pipeline_state (cada marca de tiempo cuesta una llamada al reloj: se resta) */
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
            db_printf("llamadas reales de gfx_prepare_tri_pipeline_state (estado sucio): %u por cuadro (reloj: %.0f ns por llamada, %u)\n", sB5PN / 3, ovns, ntm & 1u);
            {
                static const char *pn[6] = { "", "estado GE (depth/zmode/viewport/scissor)", "combinador + shader + alpha", "texturas (peek/import/sampler)",
                                             "escala de textura", "modo de niebla (nuevo)" };
                for (k = 1; k < 6; k++) {
                    const double per = sB5PN ? (double) sB5PAcc[k] * 1000.0 / (double) sB5PN - ovns : 0.0;
                    db_printf("  prepare seccion %-44s %8.0f ns por llamada\n", pn[k], per);
                }
            }
        }
    }
    sSweepMode = saveSweep;
    sProfLogOn = saveProf;
    gfx_rapi = sB5RapiSaved;
    db_printf("vertices %u, triangulos recibidos %u, pasan el descarte %u, dibujados %u, comandos del DL %u\n", nVtx, nSub, nPass, nDraw, nCmd);
    db_printf("de los triangulos que llegan a la salida: niebla modo0 %u modo1 %u modo2 %u modo3 %u | shade_only %u  con textura %u  scissor activo %u  necesita recorte de scissor %u  recorte de CPU %u  enva_blanco %u\n",
              hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
    db_printf("%-70s %9s %9s %9s %9s\n", "configuracion", "frio us", "calie. us", "d.frio", "d.calie.");
    for (c = 0; c < 8; c++) {
        const int prev = (c == 0) ? -1 : (c == 7 ? 0 : c - 1);
        const int dc = prev < 0 ? (int) tcold[c] : (int) tcold[c] - (int) tcold[prev];
        const int dh = prev < 0 ? (int) thot[c] : (int) thot[c] - (int) thot[prev];
        db_printf("%-70s %9u %9u %9d %9d\n", cname[c], tcold[c], thot[c], dc, dh);
    }
    db_printf("-- quitando partes de la salida / prepare (diferencia contra el control B0, caliente) --\n");
    for (c = 8; c < NC; c++) {
        const int dh = (int) thot[c] - (int) thot[8];
        const int dc = (int) tcold[c] - (int) tcold[8];
        db_printf("%-70s %9u %9u %9d %9d   (%+.0f ns por triangulo dibujado)\n", cname[c], tcold[c], thot[c], dc, dh, nDraw ? (double) dh * 1000.0 / nDraw : 0.0);
    }
    {
        const double nv = nVtx ? (double) nVtx : 1.0, ns = nSub ? (double) nSub : 1.0, np = nPass ? (double) nPass : 1.0, nd = nDraw ? (double) nDraw : 1.0;
        db_printf("por vertice: setup de cada carga %.0f ns, transformacion %.0f ns, iluminacion %.0f ns (caliente)\n",
                  (double) ((int) thot[7] - (int) thot[0]) * 1000.0 / nv,
                  (double) ((int) thot[1] - (int) thot[7]) * 1000.0 / nv, (double) ((int) thot[2] - (int) thot[1]) * 1000.0 / nv);
        db_printf("por triangulo: despacho+descarte %.0f ns (por recibido) | preludio %.0f | prepare %.0f | salida %.0f ns (por pasado) | salida por dibujado %.0f ns\n",
                  (double) ((int) thot[3] - (int) thot[2]) * 1000.0 / ns, (double) ((int) thot[4] - (int) thot[3]) * 1000.0 / np,
                  (double) ((int) thot[5] - (int) thot[4]) * 1000.0 / np, (double) ((int) thot[6] - (int) thot[5]) * 1000.0 / np,
                  (double) ((int) thot[6] - (int) thot[5]) * 1000.0 / nd);
    }
    db_printf("prueba de equivalencia con datos al azar (niebla 1..3, varias clases): %u comparaciones, distintas variante 3: %u, variante 7: %u   %s\n",
              sB5FuzzN, sB5FuzzBad[0], sB5FuzzBad[1], (sB5FuzzBad[0] | sB5FuzzBad[1]) ? "*** DIFERENCIAS ***" : "OK");
    db_printf("-- microprueba aislada: gfx_tri_core de un triangulo real de cada clase, repetido con todo en cache (lotes de 9 triangulos como en el juego) --\n");
    db_printf("%-34s %8s %9s %9s %9s %9s   %s\n", "clase", "tris", "v0 ns", "v1 ns", "v3 ns", "v7 ns", "huellas");
    for (k = 0; k < 16; k++) {
        if (!sB5CapSeen[k]) continue;
        db_printf("niebla=%d shade_only=%d textura=%d %*s %8u %9.0f %9.0f %9.0f %9.0f   %s\n", k >> 2, (k >> 1) & 1, k & 1, 4, "", sB5MicroN[k],
                  sB5MicroNs[k][0], sB5MicroNs[k][1], sB5MicroNs[k][2], sB5MicroNs[k][3],
                  (sB5MicroH[k][0] == sB5MicroH[k][1] && sB5MicroH[k][0] == sB5MicroH[k][2] && sB5MicroH[k][0] == sB5MicroH[k][3]) ? "iguales" : "DISTINTAS");
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
                if (gMenuMode == -1) { /* solo cuenta con el juego en marcha (no en la pausa / menu) */
                    sSweepWin++;
                    sSweepMode = sSweepWin / 6;
                    if (sSweepMode > 8) {
                        gPspSweep = 0; /* fin: psp_options_poll lo ve y apaga la fila */
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
    /* El menu HOME usa la lista grande y pisa la escena grabada: nada de repetirla despues. */
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
