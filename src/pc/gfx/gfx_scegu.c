#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

unsigned int __attribute__((aligned(16))) list[262144];


static unsigned int staticOffset = 0;
static unsigned int staticTexOffset = NULL;
static unsigned int currentTexOffset = NULL;

static unsigned int getMemorySize(unsigned int width, unsigned int height, unsigned int psm)
{
   switch (psm)
   {
      case GU_PSM_T4:
         return (width * height) >> 1;

      case GU_PSM_T8:
         return width * height;

      case GU_PSM_5650:
      case GU_PSM_5551:
      case GU_PSM_4444:
      case GU_PSM_T16:
         return 2 * width * height;

      case GU_PSM_8888:
      case GU_PSM_T32:
         return 4 * width * height;

      default:
         return 0;
   }
}

#define TEX_ALIGNMENT (16)
void* getStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm)
{
   unsigned int memSize = getMemorySize(width,height,psm);
   void* result = (void*)(staticOffset | 0x40000000);
   staticOffset += memSize;

   return result;
}

void* getStaticVramTexBuffer(unsigned int width, unsigned int height, unsigned int psm)
{
   unsigned int memSize = getMemorySize(width,height,psm);
   void* result = (void*)((((currentTexOffset + memSize + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT)|0x40000000);
   currentTexOffset = result;

   return result;
}

void setStaticTexBuffer(void *buffer_start){
    staticTexOffset = currentTexOffset = (unsigned int)buffer_start;
}

void resetStaticTexBuffer(void){
    currentTexOffset = staticTexOffset;
}

// since these can have different names, might as well redefine them to a single one
/*
#undef GL_FOG_COORD_SRC
#undef GL_FOG_COORD
#undef GL_FOG_COORD_ARRAY
#define GL_FOG_COORD_SRC 0x8450
#define GL_FOG_COORD 0x8451
#define GL_FOG_COORD_ARRAY 0x8457
*/

//#include "../platform.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"

static int val;

enum MixFlags {
    SH_MF_OVERRIDE_ALPHA = 1,

    SH_MF_MULTIPLY = 2,
    SH_MF_MIX = 4,
    SH_MF_SINGLE = 8,

    SH_MF_MULTIPLY_ALPHA = 16,
    SH_MF_MIX_ALPHA = 32,
    SH_MF_SINGLE_ALPHA = 64,

    SH_MF_INPUT_ALPHA = 128,
};

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    uint32_t shader_id;
    enum MixType mix;
    uint32_t mix_flags;
    bool texture_used[2];
    int num_inputs;
};

typedef struct Vertex
{
	float u, v;
	unsigned int color;
	float x,y,z;
} Vertex;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;

static const Vertex *cur_buf = NULL;
static const Vertex *cur_fog_ofs = NULL;
static size_t cur_buf_size = 0;
static size_t cur_buf_num_tris = 0;
static size_t cur_buf_stride = 0;
static bool gl_blend = false;
static bool gl_adv_fog = false;

struct Vertex __attribute__((aligned(16))) vertices[12*3] =
{
	{0, 0, 0xff7f0000,-1,-1, 1}, // 0
	{1, 0, 0xff7f0000,-1, 1, 1}, // 4
	{1, 1, 0xff7f0000, 1, 1, 1}, // 5

	{0, 0, 0xff7f0000,-1,-1, 1}, // 0
	{1, 1, 0xff7f0000, 1, 1, 1}, // 5
	{0, 1, 0xff7f0000, 1,-1, 1}, // 1

	{0, 0, 0xff7f0000,-1,-1,-1}, // 3
	{1, 0, 0xff7f0000, 1,-1,-1}, // 2
	{1, 1, 0xff7f0000, 1, 1,-1}, // 6

	{0, 0, 0xff7f0000,-1,-1,-1}, // 3
	{1, 1, 0xff7f0000, 1, 1,-1}, // 6
	{0, 1, 0xff7f0000,-1, 1,-1}, // 7

	{0, 0, 0xff007f00, 1,-1,-1}, // 0
	{1, 0, 0xff007f00, 1,-1, 1}, // 3
	{1, 1, 0xff007f00, 1, 1, 1}, // 7

	{0, 0, 0xff007f00, 1,-1,-1}, // 0
	{1, 1, 0xff007f00, 1, 1, 1}, // 7
	{0, 1, 0xff007f00, 1, 1,-1}, // 4

	{0, 0, 0xff007f00,-1,-1,-1}, // 0
	{1, 0, 0xff007f00,-1, 1,-1}, // 3
	{1, 1, 0xff007f00,-1, 1, 1}, // 7

	{0, 0, 0xff007f00,-1,-1,-1}, // 0
	{1, 1, 0xff007f00,-1, 1, 1}, // 7
	{0, 1, 0xff007f00,-1,-1, 1}, // 4

	{0, 0, 0xff00007f,-1, 1,-1}, // 0
	{1, 0, 0xff00007f, 1, 1,-1}, // 1
	{1, 1, 0xff00007f, 1, 1, 1}, // 2

	{0, 0, 0xff00007f,-1, 1,-1}, // 0
	{1, 1, 0xff00007f, 1, 1, 1}, // 2
	{0, 1, 0xff00007f,-1, 1, 1}, // 3

	{0, 0, 0xff00007f,-1,-1,-1}, // 4
	{1, 0, 0xff00007f,-1,-1, 1}, // 7
	{1, 1, 0xff00007f, 1,-1, 1}, // 6

	{0, 0, 0xff00007f,-1,-1,-1}, // 4
	{1, 1, 0xff00007f, 1,-1, 1}, // 6
	{0, 1, 0xff00007f, 1,-1,-1}, // 5
};


static bool gfx_scegu_z_is_from_0_to_1(void) {
    return true;
}
#if 0
#define TEXENV_COMBINE_ON() glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE)
#define TEXENV_COMBINE_OFF() glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE)

#define TEXENV_COMBINE_OP(num, cval, aval) \
    do { \
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND ## num ## _RGB, cval); \
        glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND ## num ## _ALPHA, aval); \
    } while (0)

#define TEXENV_COMBINE_SET1(what, mode, val) \
    do { \
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ ## what, mode); \
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ ## what, val); \
    } while (0)

#define TEXENV_COMBINE_SET2(what, mode, val1, val2) \
    do { \
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ ## what, mode); \
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ ## what, val1); \
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ ## what, val2); \
    } while (0)

#define TEXENV_COMBINE_SET3(what, mode, val1, val2, val3) \
    do { \
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ ## what, mode); \
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ ## what, val1); \
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ ## what, val2); \
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_ ## what, val3); \
    } while (0)
#endif
#if 0
static inline void texenv_set_texture_color(struct ShaderProgram *prg) {
    glActiveTexture(GL_TEXTURE0);

    if (prg->mix_flags & SH_MF_OVERRIDE_ALPHA) {
        TEXENV_COMBINE_ON();
        if (prg->mix_flags & SH_MF_SINGLE_ALPHA) {
            if (prg->mix_flags & SH_MF_MULTIPLY) {
                // keep the alpha but modulate the color
                const GLenum alphasrc = (prg->mix_flags & SH_MF_INPUT_ALPHA) ? GL_PRIMARY_COLOR : GL_TEXTURE;
                TEXENV_COMBINE_SET2(RGB, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
                TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, alphasrc);
            } else {
                // somehow makes it keep the color while taking the alpha from primary color
                TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_TEXTURE);
            }
        } else { // if (prg->mix_flags & SH_MF_SINGLE) {
            if (prg->mix_flags & SH_MF_MULTIPLY_ALPHA) {
                // modulate the alpha but keep the color
                TEXENV_COMBINE_SET2(ALPHA, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
                TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_TEXTURE);
            } else {
                // somehow makes it keep the alpha
                TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, GL_TEXTURE);
            }
        }
        // TODO: MIX and the other one
    } else if (prg->mix_flags & SH_MF_MULTIPLY) {
        // TODO: is this right?
        TEXENV_COMBINE_OFF();
    } else if (prg->mix_flags & SH_MF_MIX) {
        TEXENV_COMBINE_ON();
        // HACK: determine this using flags and not this crap
        if (prg->num_inputs > 1) {
            // out.rgb = mix(color0.rgb, color1.rgb, texel0.rgb);
            // no color1 tho, so mix with white (texenv color is set in init())
            TEXENV_COMBINE_OP(2, GL_SRC_COLOR, GL_SRC_ALPHA);
            TEXENV_COMBINE_SET3(RGB, GL_INTERPOLATE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_TEXTURE);
            TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, GL_CONSTANT);
        } else {
            // out.rgb = mix(color0.rgb, texel0.rgb, texel0.a);
            TEXENV_COMBINE_OP(2, GL_SRC_ALPHA, GL_SRC_ALPHA);
            TEXENV_COMBINE_SET3(RGB, GL_INTERPOLATE, GL_TEXTURE, GL_PRIMARY_COLOR, GL_TEXTURE);
        }
    } else {
        TEXENV_COMBINE_OFF();
    }
}
#endif

#if 0
static inline void texenv_set_texture_texture(UNUSED struct ShaderProgram *prg) {
    glActiveTexture(GL_TEXTURE0);
    TEXENV_COMBINE_OFF();
    glActiveTexture(GL_TEXTURE1);
    TEXENV_COMBINE_ON();
    // out.rgb = mix(texel0.rgb, texel1.rgb, color0.rgb);
    TEXENV_COMBINE_OP(2, GL_SRC_COLOR, GL_SRC_ALPHA);
    TEXENV_COMBINE_SET3(RGB, GL_INTERPOLATE, GL_PREVIOUS, GL_TEXTURE, GL_PRIMARY_COLOR);
    // out.a = texel0.a;
    TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, GL_PREVIOUS);
}
#endif
static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
    const float *ofs = cur_buf;

    // vertices are always there
    //glVertexPointer(4, GL_FLOAT, cur_buf_stride, ofs);
    //ofs += 4;

    // have texture(s), specify same texcoords for every active texture
    for (int i = 0; i < 2; ++i) {
        if (prg->texture_used[i]) {
            /*
            glEnable(GL_TEXTURE0 + i);
            glClientActiveTexture(GL_TEXTURE0 + i);
            glActiveTexture(GL_TEXTURE0 + i);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, cur_buf_stride, ofs);
            */
            sceGuEnable(GU_TEXTURE_2D);
            //ofs += 2;
        }
    }

    /*
    if (prg->shader_id & SHADER_OPT_FOG) {
        // fog requested, we can deal with it in one of two ways
        if (gl_adv_fog) {
            // if GL_EXT_fog_coord is available, use the provided fog factor as scaled depth for GL fog
            const float fogrgb[] = { ofs[0], ofs[1], ofs[2] };
            glEnable(GL_FOG);
            glFogfv(GL_FOG_COLOR, fogrgb); // color is the same for all verts, only intensity is different
            glEnableClientState(GL_FOG_COORD_ARRAY);
            mglFogCoordPointer(GL_FLOAT, cur_buf_stride, ofs + 3); // point it to alpha, which is fog factor
        } else {
            // if there's no fog coords available, blend it on top of normal tris later
            cur_fog_ofs = ofs;
        }
        ofs += 4;
    }
    */

    if (prg->num_inputs) {
        // have colors
        // TODO: more than one color (maybe glSecondaryColorPointer?)
        // HACK: if there's a texture and two colors, one of them is likely for speculars or some shit (see mario head)
        //       if there's two colors but no texture, the real color is likely the second one
        /*
        const int hack = (prg->num_inputs > 1) * (4 - (int)prg->texture_used[0]);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, cur_buf_stride, ofs + hack);
        ofs += 4 * prg->num_inputs;
        */
    }

    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        // (horrible) alpha discard
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    }

    // configure formulae
    switch (prg->mix) {
        case SH_MT_TEXTURE:
            //glActiveTexture(GL_TEXTURE0);
            //TEXENV_COMBINE_OFF();
            break;

        case SH_MT_TEXTURE_COLOR:
            //texenv_set_texture_color(prg);
            break;

        case SH_MT_TEXTURE_TEXTURE:
            //texenv_set_texture_texture(prg);
            break;

        default:
            break;
    }
}

static void gfx_scegu_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader == old_prg || old_prg == NULL)
        cur_shader = NULL;

    //glClientActiveTexture(GL_TEXTURE0);
    //glActiveTexture(GL_TEXTURE0);
    //sceGuDisable(GU_TEXTURE_2D);

    //glClientActiveTexture(GL_TEXTURE1);
    //glActiveTexture(GL_TEXTURE1);
    sceGuDisable(GU_TEXTURE_2D);
    //glDisable(GL_TEXTURE1);
    //glDisable(GL_TEXTURE0);
    //glDisable(GL_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    cur_fog_ofs = NULL; // clear fog colors

    //if (gl_adv_fog) glDisableClientState(GL_FOG_COORD_ARRAY);
}

static void gfx_scegu_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    // gfx_scegu_apply_shader(cur_shader);
}

static struct ShaderProgram *gfx_scegu_create_and_load_new_shader(uint32_t shader_id) {
    uint8_t c[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
    }

    bool used_textures[2] = {0, 0};
    int num_inputs = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (c[i][j] >= SHADER_INPUT_1 && c[i][j] <= SHADER_INPUT_4) {
                if (c[i][j] > num_inputs) {
                    num_inputs = c[i][j];
                }
            }
            if (c[i][j] == SHADER_TEXEL0 || c[i][j] == SHADER_TEXEL0A) {
                used_textures[0] = true;
            }
            if (c[i][j] == SHADER_TEXEL1) {
                used_textures[1] = true;
            }
        }
    }

    const bool color_alpha_same = (shader_id & 0xfff) == ((shader_id >> 12) & 0xfff);
    const bool do_multiply[2] = {c[0][1] == 0 && c[0][3] == 0, c[1][1] == 0 && c[1][3] == 0};
    const bool do_mix[2] = {c[0][1] == c[0][3], c[1][1] == c[1][3]};
    const bool do_single[2] = {c[0][2] == 0, c[1][2] == 0};

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->shader_id = shader_id;
    prg->num_inputs = num_inputs;
    prg->texture_used[0] = used_textures[0];
    prg->texture_used[1] = used_textures[1];

    if (used_textures[0] && used_textures[1])
        prg->mix = SH_MT_TEXTURE_TEXTURE;
    else if (used_textures[0] && num_inputs)
        prg->mix = SH_MT_TEXTURE_COLOR;
    else if (used_textures[0])
        prg->mix = SH_MT_TEXTURE;
    else if (num_inputs > 1)
        prg->mix = SH_MT_COLOR_COLOR;
    else if (num_inputs)
        prg->mix = SH_MT_COLOR;

    if (do_single[0]) prg->mix_flags |= SH_MF_SINGLE;
    if (do_multiply[0]) prg->mix_flags |= SH_MF_MULTIPLY;
    if (do_mix[0]) prg->mix_flags |= SH_MF_MIX;

    if (!color_alpha_same && (shader_id & SHADER_OPT_ALPHA)) {
        prg->mix_flags |= SH_MF_OVERRIDE_ALPHA;
        if (do_single[1]) prg->mix_flags |= SH_MF_SINGLE_ALPHA;
        if (do_multiply[1]) prg->mix_flags |= SH_MF_MULTIPLY_ALPHA;
        if (do_mix[1]) prg->mix_flags |= SH_MF_MIX_ALPHA;
        if (c[1][3] < SHADER_TEXEL0) prg->mix_flags |= SH_MF_INPUT_ALPHA;
    }

    gfx_scegu_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_scegu_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_scegu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

static unsigned int gfx_scegu_new_texture(void) {
    unsigned int ret = 0 ;
    //glGenTextures(1, &ret);
    return ret;
}

static void gfx_scegu_select_texture(int tile, unsigned int texture_id) {
    //glActiveTexture(GL_TEXTURE0 + tile);
    //glBindTexture(GL_TEXTURE_2D, texture_id);
}

/* Used for rescaling textures ROUGHLY into pow2 dims */
static unsigned int scaled[256 * 256]; /* 256kb */
static void gfx_scegu_resample_32bit(const unsigned *in, int inwidth, int inheight, unsigned *out, int outwidth, int outheight) {
  int i, j;
  const unsigned int *inrow;
  unsigned int frac, fracstep;

  fracstep = inwidth * 0x10000 / outwidth;
  for (i = 0; i < outheight; i++, out += outwidth) {
    inrow = in + inwidth * (i * inheight / outheight);
    frac = fracstep >> 1;
    for (j = 0; j < outwidth; j += 4) {
      out[j] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 1] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 2] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 3] = inrow[frac >> 16];
      frac += fracstep;
    }
  }
}

static void gfx_scegu_resample_8bit(const unsigned char *in, int inwidth, int inheight, unsigned char *out, int outwidth, int outheight) {
  int i, j;
  const unsigned char *inrow;
  unsigned int frac, fracstep;

  fracstep = inwidth * 0x10000 / outwidth;
  for (i = 0; i < outheight; i++, out += outwidth) {
    inrow = in + inwidth * (i * inheight / outheight);
    frac = fracstep >> 1;
    for (j = 0; j < outwidth; j += 4) {
      out[j] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 1] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 2] = inrow[frac >> 16];
      frac += fracstep;
      out[j + 3] = inrow[frac >> 16];
      frac += fracstep;
    }
  }
}

static inline int ispow2(uint32_t x)
{
	return (x & (x - 1)) == 0;
}

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    if( ((uintptr_t )rgba32_buf % 16) == 0){
        sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
        if(ispow2(width) && ispow2(height)){
            sceGuTexImage(0, width, height, width,rgba32_buf);
        } else {
            int scaled_width, scaled_height;

            for (scaled_width = 1; scaled_width < width; scaled_width <<= 1)
                ;
            for (scaled_height = 1; scaled_height < height; scaled_height <<= 1)
                ;

            //@Note: is psp min tex width 8?
            if (height < 8 || scaled_height < 8) {
                scaled_height = 8;
            }
            if (width < 8 || scaled_width < 8) {
                scaled_width = 8;
            }
            scaled_width >>= 1;
            scaled_height >>= 1;

            /*
            //@Note: we should maybe actually error out 
            if (scaled_width * scaled_height > (int)sizeof(scaled) / 4)
                {return;}
            */
            gfx_scegu_resample_32bit((const unsigned int*)rgba32_buf, width, height, scaled, scaled_width, scaled_height);
            sceGuTexImage(0, scaled_width, scaled_height, scaled_width, rgba32_buf);
        }
    }
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP)
        return GU_CLAMP;
    return GU_REPEAT;
    //return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_scegu_set_sampler_parameters(UNUSED int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const int filter = linear_filter ? GU_LINEAR : GU_NEAREST;
    //glActiveTexture(GL_TEXTURE0 + tile);
    sceGuTexFilter(filter, filter);
    sceGuTexWrap(gfx_cm_to_opengl(cms), gfx_cm_to_opengl(cmt));
}

static void gfx_scegu_set_depth_test(bool depth_test) {
    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
}

static void gfx_scegu_set_depth_mask(bool z_upd) {
    sceGuDepthMask(z_upd ? GU_TRUE : GU_FALSE);
}

static void gfx_scegu_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        sceGuDepthOffset(-2);
    } else {
        sceGuDepthOffset(0);
    }
}

static void gfx_scegu_set_viewport(int x, int y, int width, int height) {
    return;
    sceGuViewport(x, y, width, height);
}

static void gfx_scegu_set_scissor(int x, int y, int width, int height) {
    return;
    /*@Note: maybe this is right */
    sceGuScissor(x, y, x+width, y+height);
}

static void gfx_scegu_set_use_alpha(bool use_alpha) {
    return;
    gl_blend = use_alpha;
    if (use_alpha) {
        sceGuEnable(GU_BLEND);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

// draws the same triangles as plain fog color + fog intensity as alpha
// on top of the normal tris and blends them to achieve sort of the same effect
// as fog would
static inline void gfx_scegu_blend_fog_tris(void) {
      /*@Todo: figure this out! */
    return;
#if 0
    // if a texture was used, replace it with fog color instead, but still keep the alpha
    if (cur_shader->texture_used[0]) {
        glActiveTexture(GL_TEXTURE0);
        TEXENV_COMBINE_ON();
        // out.rgb = input0.rgb
        TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_PRIMARY_COLOR);
        // out.a = texel0.a * input0.a
        TEXENV_COMBINE_SET2(ALPHA, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
    }

    glEnableClientState(GL_COLOR_ARRAY); // enable color array temporarily
    glColorPointer(4, GL_FLOAT, cur_buf_stride, cur_fog_ofs); // set fog colors as primary colors
    if (!gl_blend) glEnable(GL_BLEND); // enable blending temporarily
    glDepthFunc(GL_LEQUAL); // Z is the same as the base triangles

    glDrawArrays(GL_TRIANGLES, 0, 3 * cur_buf_num_tris);

    glDepthFunc(GL_LESS); // set back to default
    if (!gl_blend) glDisable(GL_BLEND); // disable blending if it was disabled
    glDisableClientState(GL_COLOR_ARRAY); // will get reenabled later anyway
#endif
}

static void gfx_scegu_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);

    cur_buf = (Vertex *)buf_vbo;
    cur_buf_size = buf_vbo_len * 4;
    cur_buf_num_tris = buf_vbo_num_tris;
    cur_buf_stride = cur_buf_size / (3 * cur_buf_num_tris);

    gfx_scegu_apply_shader(cur_shader);

    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D, 3 * cur_buf_num_tris, 0, cur_buf);

    // cur_fog_ofs is only set if GL_EXT_fog_coord isn't used
    if (cur_fog_ofs) gfx_scegu_blend_fog_tris();
}

static void gfx_scegu_init(void) {

    gl_adv_fog = false;

#if 0
    printf("GL_VERSION = %s\n", glGetString(GL_VERSION));
    printf("GL_EXTENSIONS =\n%s\n", glGetString(GL_EXTENSIONS));

    if (gl_adv_fog) {
        // set fog params, they never change
        printf("GL_EXT_fog_coord available, using that for fog\n");
        glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 0.0f);
        glFogf(GL_FOG_END, 1.0f);
    }
#endif
    val = 0;
	sceGuInit();


    void* fbp0 = getStaticVramBuffer(BUF_WIDTH,SCR_HEIGHT,GU_PSM_8888);
    void* fbp1 = getStaticVramBuffer(BUF_WIDTH,SCR_HEIGHT,GU_PSM_8888);
    void* zbp = getStaticVramBuffer(BUF_WIDTH,SCR_HEIGHT,GU_PSM_4444);

	sceGuStart(GU_DIRECT,list);
    sceGuDrawBuffer(GU_PSM_8888,fbp0,BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH,SCR_HEIGHT,fbp1,BUF_WIDTH);
    sceGuDepthBuffer(zbp,BUF_WIDTH);
	/*
    sceGuDrawBuffer(GU_PSM_8888,(void*)0,BUF_WIDTH);   
	sceGuDispBuffer(SCR_WIDTH,SCR_HEIGHT,(void*)0x88000,BUF_WIDTH);   
	sceGuDepthBuffer((void*)0x110000,BUF_WIDTH);   
	*/
    sceGuOffset(2048 - (SCR_WIDTH/2),2048 - (SCR_HEIGHT/2));
	sceGuViewport(2048,2048,SCR_WIDTH,SCR_HEIGHT);
	sceGuDepthRange(65535,0);
	sceGuScissor(0,0,SCR_WIDTH,SCR_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_EQUAL);
	sceGuEnable(GU_DEPTH_TEST);
	//sceGuFrontFace(GU_CCW);
	sceGuShadeModel(GU_SMOOTH);
	//sceGuEnable(GU_CULL_FACE);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuFinish();
	sceGuSync(0,0);

	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
    setStaticTexBuffer(malloc(4096*1024));
}

static void gfx_scegu_start_frame(void) {
    resetStaticTexBuffer();
    sceGuStart(GU_DIRECT, list);
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    //sceGuClearColor(0xFF000000);
    sceGuClearColor(0xFF554433);// light blue
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_SCISSOR_TEST);
    
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();

    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();

    sceGumUpdateMatrix();
}

void gfx_scegu_on_resize(void) {
}

static void gfx_scegu_end_frame(void) {
    sceGuFinish();
    sceGuSync(0,0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static void gfx_scegu_finish_render(void) {
    val++;
    return;
    sceGuFinish();
    sceGuSync(0,0);
    val++;
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_scegu_z_is_from_0_to_1,
    gfx_scegu_unload_shader,
    gfx_scegu_load_shader,
    gfx_scegu_create_and_load_new_shader,
    gfx_scegu_lookup_shader,
    gfx_scegu_shader_get_info,
    gfx_scegu_new_texture,
    gfx_scegu_select_texture,
    gfx_scegu_upload_texture,
    gfx_scegu_set_sampler_parameters,
    gfx_scegu_set_depth_test,
    gfx_scegu_set_depth_mask,
    gfx_scegu_set_zmode_decal,
    gfx_scegu_set_viewport,
    gfx_scegu_set_scissor,
    gfx_scegu_set_use_alpha,
    gfx_scegu_draw_triangles,
    gfx_scegu_init,
    gfx_scegu_on_resize,
    gfx_scegu_start_frame,
    gfx_scegu_end_frame,
    gfx_scegu_finish_render
};

#endif // RAPI_GL_LEGACY
