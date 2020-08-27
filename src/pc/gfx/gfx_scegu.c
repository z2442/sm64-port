#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <malloc.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>

#include "psp_texture_manager.h"

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)

float identity_matrix[4][4] __attribute__((aligned(16))) = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}};


/* Shader IDs
id        alp fog edg nse ut0 ut1 num sin0 sin1 mul0 mul1 mix0 mix1 cas
-----------------------------------------------------------------------
69        0   0   0   0   1   0   1   0    1    1    1    1    1    0  
512       0   0   0   0   0   0   1   1    1    0    1    0    1    0  
909       0   0   0   0   1   0   1   0    1    0    1    1    1    0  
1361      0   0   0   0   1   0   2   0    1    0    1    1    1    0  
2560      0   0   0   0   1   0   0   1    1    0    1    0    1    0  
17059909  1   0   0   0   1   0   1   0    0    1    1    1    1    1  
17062400  1   0   0   0   1   0   1   1    0    0    1    0    1    0  
17305729  1   0   0   0   0   0   2   0    0    1    1    1    1    1  
18092101  1   0   0   0   1   0   1   0    0    1    1    1    1    0  
18874437  1   0   0   0   1   0   1   0    1    1    0    1    0    0  
18874880  1   0   0   0   0   0   1   1    1    0    0    0    0    1  
18875277  1   0   0   0   1   0   1   0    1    0    0    1    0    0  
18876928  1   0   0   0   1   0   1   1    1    0    0    0    0    0
27263045  1   0   0   0   1   0   1   0    1    1    0    1    0    0  
27265536  1   0   0   0   1   0   0   1    1    0    0    0    0    1  
27265647  1   0   0   0   1   1   1   0    1    0    0    1    0    0  
52428869  1   1   0   0   1   0   1   0    1    1    0    1    0    0  
52429312  1   1   0   0   0   0   1   1    1    0    0    0    0    1  
52431360  1   1   0   0   1   0   1   1    1    0    0    0    0    0  
84168773  1   0   1   0   1   0   1   0    0    1    1    1    1    1  
85983744  1   0   1   0   0   0   1   1    1    0    0    0    0    1  
94374400  1   0   1   0   1   0   0   1    1    0    0    0    0    1  
127928832 1   1   1   0   1   0   0   1    1    0    0    0    0    1  
153092165 1   0   0   1   1   0   1   0    1    1    0    1    0    0  
153092608 1   0   0   1   0   0   1   1    1    0    0    0    0    1  
153093005 1   0   0   1   1   0   1   0    1    0    0    1    0    0  
153094656 1   0   0   1   1   0   1   1    1    0    0    0    0    0

printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", shader_id,
    cc_features.opt_alpha,
    cc_features.opt_fog,
    cc_features.opt_texture_edge,
    cc_features.opt_noise,
    cc_features.used_textures[0],
    cc_features.used_textures[1],
    cc_features.num_inputs,
    cc_features.do_single[0],
    cc_features.do_single[1],
    cc_features.do_multiply[0],
    cc_features.do_multiply[1],
    cc_features.do_mix[0],
    cc_features.do_mix[1],
    cc_features.color_alpha_same
);
*/

/* Shader Working List:
84168773    - Menu Overlays
*/

/* Shader Broken List: 
153092165   - Noise
153092608   - Noise
153093005   - Noise
153094656   - Noise
*/

static uint32_t shader_ids[27] =
{
69       ,
512      ,
909      ,
1361     ,
2560     ,
17059909 ,
17062400 ,
17305729 ,
18092101 ,
18874437 ,
18874880 ,
18875277 ,
18876928 ,
27263045 ,
27265536 ,
27265647 ,
52428869 ,
52429312 ,
52431360 ,
84168773 ,
85983744 ,
94374400 ,
127928832,
153092165,
153092608,
153093005,
153094656
};

static uint32_t shader_remap[27*2] = {
69       ,69       ,
512      ,512      ,
909      ,909      ,
1361     ,1361     ,
2560     ,2560     ,
17059909 ,17059909 ,
17062400 ,17062400 ,
17305729 ,17305729 ,
18092101 ,18092101 ,
18874437 ,18874437 ,
18874880 ,18874880 ,
18875277 ,18875277 ,
18876928 ,18876928 ,
27263045 ,27263045 ,
27265536 ,27265536 ,
27265647 ,27265647 ,
52428869 ,52428869 ,
52429312 ,52429312 ,
52431360 ,52431360 ,
84168773 ,84168773 ,
85983744 ,85983744 ,
94374400 ,94374400 ,
127928832,127928832,
153092165,69,
153092608,69,
153093005,69,
153094656,69
};

static uint32_t shader_broken[27] ={
    153092165,  // Noise
    153092608,  // Noise
    153093005,  // Noise
    153094656   // Noise
};

unsigned int __attribute__((aligned(64))) list[262144*2];

static unsigned int staticOffset = 0;

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

void* getStaticVramBufferBytes(size_t bytes)
{
   unsigned int memSize = bytes;
   void* result = (void*)(staticOffset | 0x40000000);
   staticOffset += memSize;

   return (void*)(((unsigned int)result) + ((unsigned int)sceGeEdramGetAddr()));
}
/*
void* getStaticVramTexBuffer(unsigned int width, unsigned int height, unsigned int psm)
{
   unsigned int memSize = getMemorySize(width,height,psm);
   void* result = (void*)((((currentTexOffset + memSize + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT)|0x40000000);
   currentTexOffset = (unsigned int)result;

   return result;
}

void setStaticTexBuffer(void *buffer_start){
    staticTexOffset = currentTexOffset = (unsigned int)buffer_start;
}

void resetStaticTexBuffer(void){
    currentTexOffset = staticTexOffset;
}
*/


//#include "../platform.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"

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

typedef struct VertexColor
{
	unsigned short a, b;
	unsigned long color;
	unsigned short x, y, z;
} VertexColor;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;

static bool gl_blend = false;

static inline uint32_t get_shader_index(uint32_t id){
    size_t i;
    for(i = 0;i<27;i++){
        if(shader_ids[i] == id){
            return i;//return shader_ids[i];
        }
    }
    char msg[32];
    sprintf(msg, "ERROR! Shader not known %u\n", id);
    sceIoWrite(2, msg, strlen(msg));
    return 0;
}

static inline uint32_t get_shader_remap(uint32_t id){
    size_t index = get_shader_index(id);
    return shader_remap[index*2+1];
}

static inline bool is_shader_enabled(uint32_t id){
    size_t i;
    for(i = 0;i<27;i++){
        if(shader_broken[i] == id){
            return false;
        }
    }
    return true;
}

static struct ShaderProgram *get_shader_from_id(uint32_t id){
    size_t i;
    for(i = 0;i<shader_program_pool_size;i++){
        if(shader_program_pool[i].shader_id == id){
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static bool gfx_scegu_z_is_from_0_to_1(void) {
    return true;
}

#define TEXENV_COMBINE_ON() sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA )
#define TEXENV_COMBINE_OFF() sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA )

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

static inline void texenv_set_texture_color(struct ShaderProgram *prg) {
    if (prg->mix_flags & SH_MF_OVERRIDE_ALPHA) {
        TEXENV_COMBINE_ON();
        if (prg->mix_flags & SH_MF_SINGLE_ALPHA) {
            if (prg->mix_flags & SH_MF_MULTIPLY) {
                // keep the alpha but modulate the color
                //const unsigned int alphasrc = (prg->mix_flags & SH_MF_INPUT_ALPHA) ? GL_PRIMARY_COLOR : GL_TEXTURE;
                //TEXENV_COMBINE_SET2(RGB, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
               //TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, alphasrc);
                //sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA );
                sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA );
                //printf("SH_MF_SINGLE_ALPHA & SH_MF_MULTIPLY_ALPHA\n");
            } else {
                // somehow makes it keep the color while taking the alpha from primary color
                //TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_TEXTURE);
                sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA );
                //printf("SH_MF_SINGLE_ALPHA & !SH_MF_MULTIPLY_ALPHA\n");
            }
        } else { // if (prg->mix_flags & SH_MF_SINGLE) {
            if (prg->mix_flags & SH_MF_MULTIPLY_ALPHA) {
                // modulate the alpha but keep the color
                //TEXENV_COMBINE_SET2(ALPHA, GL_MODULATE, GL_TEXTURE, GL_PRIMARY_COLOR);
                //TEXENV_COMBINE_SET1(RGB, GL_REPLACE, GL_TEXTURE);
                sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA );
                //printf("!SH_MF_SINGLE_ALPHA & SH_MF_MULTIPLY_ALPHA\n");
            } else {
                // somehow makes it keep the alpha
                //TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, GL_TEXTURE);
                //sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA );
                printf("!SH_MF_SINGLE_ALPHA & !SH_MF_MULTIPLY_ALPHA\n");
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
            //TEXENV_COMBINE_OP(2, GL_SRC_COLOR, GL_SRC_ALPHA);
            //TEXENV_COMBINE_SET3(RGB, GL_INTERPOLATE, GL_CONSTANT, GL_PRIMARY_COLOR, GL_TEXTURE);
            //TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, GL_CONSTANT);
        } else {
            // out.rgb = mix(color0.rgb, texel0.rgb, texel0.a);
            //TEXENV_COMBINE_OP(2, GL_SRC_ALPHA, GL_SRC_ALPHA);
            //TEXENV_COMBINE_SET3(RGB, GL_INTERPOLATE, GL_TEXTURE, GL_PRIMARY_COLOR, GL_TEXTURE);
        }
    } else {
        TEXENV_COMBINE_OFF();
    }
}

static inline void texenv_set_texture_texture(UNUSED struct ShaderProgram *prg) {
    //glActiveTexture(GL_TEXTURE0);
    //TEXENV_COMBINE_OFF();
    //glActiveTexture(GL_TEXTURE1);
    //TEXENV_COMBINE_ON();
    // out.rgb = mix(texel0.rgb, texel1.rgb, color0.rgb);
    //TEXENV_COMBINE_OP(2, GL_SRC_COLOR, GL_SRC_ALPHA);
    //TEXENV_COMBINE_SET3(RGB, GL_INTERPOLATE, GL_PREVIOUS, GL_TEXTURE, GL_PRIMARY_COLOR);
    // out.a = texel0.a;
    //TEXENV_COMBINE_SET1(ALPHA, GL_REPLACE, GL_PREVIOUS);
}

static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
//    const float *ofs = cur_buf;

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

#if 0
    if (prg->shader_id & SHADER_OPT_FOG) {
        // Yea this doesnt work at all */
        //sceGuFog(scegu_fog_near, scegu_fog_far, 0x00FF0000);//scegu_fog_color); // color is the same for all verts, only intensity is different
        //sceGuEnable(GU_FOG);
        sceGuEnable(GU_BLEND);
    }
#endif

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
            TEXENV_COMBINE_OFF();
            break;

        case SH_MT_TEXTURE_COLOR:
            texenv_set_texture_color(prg);
            break;

        case SH_MT_TEXTURE_TEXTURE:
            texenv_set_texture_texture(prg);
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

    //if (gl_adv_fog) glDisableClientState(GL_FOG_COORD_ARRAY);
}

static void gfx_scegu_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    gfx_scegu_apply_shader(cur_shader);
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
    return texman_create();
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP)
        return GU_CLAMP;
    return GU_REPEAT;
    //return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_scegu_set_sampler_parameters(UNUSED int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const int filter = linear_filter ? GU_LINEAR : GU_NEAREST;
    sceGuTexFilter(filter, filter);
    sceGuTexWrap(gfx_cm_to_opengl(cms), gfx_cm_to_opengl(cmt));
}

static void gfx_scegu_select_texture(int tile, unsigned int texture_id) {
    texman_bind_tex(texture_id);
    gfx_scegu_set_sampler_parameters(tile, false, 0, 0);
}

/* Used for rescaling textures ROUGHLY into pow2 dims */
static unsigned int __attribute__((aligned(16))) scaled[256 * 256]; /* 256kb */
static void gfx_scegu_resample_32bit(const unsigned int *in, int inwidth, int inheight, unsigned int *out, int outwidth, int outheight) {
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

static void gfx_scegu_resample_16bit(const unsigned short *in, int inwidth, int inheight, unsigned short *out, int outwidth, int outheight) {
  int i, j;
  const unsigned short *inrow;
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

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    if(ispow2(width) && ispow2(height)){
        texman_upload_swizzle(width,  height, type, (void*)rgba32_buf);
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
        if(type == GU_PSM_8888){
            gfx_scegu_resample_32bit((const unsigned int*)rgba32_buf, width, height, (void*)scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void*)scaled);
        } else if(type == GU_PSM_5551){
            gfx_scegu_resample_16bit((const unsigned short*)rgba32_buf, width, height, (void*)scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void*)scaled);
        }else{ 
            gfx_scegu_resample_8bit((const unsigned char*)rgba32_buf, width, height, (void*)scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void*)scaled);
        }
    }
}

static void gfx_scegu_set_depth_test(bool depth_test) {
    if (depth_test) {
        sceGuEnable(GU_DEPTH_TEST);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
}
static bool z_depth = false; 
static void gfx_scegu_set_depth_mask(bool z_upd) {
    z_depth = !z_upd;
    sceGuDepthMask(z_upd ? GU_FALSE : GU_TRUE);
}

static void gfx_scegu_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        sceGuDepthOffset(12); /* I think we need a little more on psp because of 16bit depth buffer */
    } else {
        sceGuDepthOffset(0);
    }
}

static void gfx_scegu_set_viewport(int x, int y, int width, int height) {
    printf("sceGuViewport(%d, %d, %d, %d)\n", x, y, width, height);
    //sceGuViewport(x, y, width, height);
}

static void gfx_scegu_set_scissor(int x, int y, int width, int height) {
    /*@Note: maybe this is right, fixes signs so should be correct */
    if((x || y)){
        //printf("set_scissor(%d, %d, %d, %d) -> sceGuScissor(%d, %d, %d, %d)\n",x, y, width, height, x, SCR_HEIGHT-y-height, x+width, SCR_HEIGHT-y);
        sceGuScissor(x, SCR_HEIGHT-y-height, x+width, SCR_HEIGHT-y);
    } else {
        //printf("sceGuScissor(%d, %d, %d, %d)\n", x, y, width, height);
        sceGuScissor(x, y, x+width, y+height);
    }
}

static void gfx_scegu_set_use_alpha(bool use_alpha) {
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

static void gfx_scegu_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);

    if(is_shader_enabled(cur_shader->shader_id)){
        gfx_scegu_apply_shader(cur_shader);
    } else {
        if(cur_shader->shader_id < 153092165)
            printf("Remapping shader %u -> %u\n", cur_shader->shader_id, get_shader_remap(cur_shader->shader_id));
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    sceKernelDcacheWritebackRange (buf_vbo, sizeof(Vertex)* 3 * buf_vbo_num_tris);
	sceKernelDcacheInvalidateRange(buf_vbo, sizeof(Vertex)* 3 * buf_vbo_num_tris);
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_3D, 3 * buf_vbo_num_tris, 0, (void *)buf_vbo);

    // cur_fog_ofs is only set if GL_EXT_fog_coord isn't used
    //if (cur_fog_ofs) gfx_scegu_blend_fog_tris();

}

void gfx_scegu_draw_triangles_2d(float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);

    if(is_shader_enabled(cur_shader->shader_id)){
        gfx_scegu_apply_shader(cur_shader);
    } else {
        printf("Remapping shader %u -> %u\n", cur_shader->shader_id, get_shader_remap(cur_shader->shader_id));
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    sceKernelDcacheWritebackRange (buf_vbo, sizeof(VertexColor)* 2);
	sceKernelDcacheInvalidateRange(buf_vbo, sizeof(VertexColor)* 2);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT|GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, 0, (void *)buf_vbo);
}

static void gfx_scegu_init(void) {
	sceGuInit();

    void* fbp0 = getStaticVramBuffer(BUF_WIDTH,SCR_HEIGHT,GU_PSM_5650);
    void* fbp1 = getStaticVramBuffer(BUF_WIDTH,SCR_HEIGHT,GU_PSM_5650);
    void* zbp = getStaticVramBuffer(BUF_WIDTH,SCR_HEIGHT,GU_PSM_4444);

	sceGuStart(GU_DIRECT,list);
    sceGuDrawBuffer(GU_PSM_5650,fbp0,BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH,SCR_HEIGHT,fbp1,BUF_WIDTH);
    sceGuDepthBuffer(zbp,BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
	sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
	sceGuDepthRange(0xffff, 0);
	sceGuScissor(0, 0, SCR_WIDTH,SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuShadeModel(GU_SMOOTH);
	sceGuEnable(GU_CLIP_PLANES);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuDisable(GU_LIGHTING);
	sceGuDisable(GU_BLEND);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CCW);
	sceGuDepthMask(GU_FALSE);
    sceGuTexEnvColor(0xffffffff);
	sceGuTexOffset(0.0f, 0.0f);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
#if 0 
    // Broken 
    sceGuViewport(2048,2048,SCR_WIDTH,SCR_HEIGHT);
	sceGuDepthRange(0xffff,0);
	sceGuScissor(0,0,SCR_WIDTH,SCR_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_GREATER);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuShadeModel(GU_SMOOTH);
	sceGuDisable(GU_CULL_FACE);
	sceGuEnable(GU_CLIP_PLANES);
    sceGuDepthOffset(-128);
    sceGuTexEnvColor(0xffffffff);
#endif
    sceGuFinish();
	sceGuSync(0,0);

	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);

    void *texman_buffer = getStaticVramBufferBytes(TEXMAN_BUFFER_SIZE);
    void *texman_aligned = (void *) ((((unsigned int) texman_buffer + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT);
    texman_reset(texman_aligned, TEXMAN_BUFFER_SIZE);
    if(!texman_buffer){
        char msg[32];
        sprintf(msg, "OUT OF MEMORY!\n");
        sceIoWrite(1, msg, strlen(msg));

        sceKernelExitGame();
    }
}

static void gfx_scegu_start_frame(void) {
    sceGuStart(GU_DIRECT, list);
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_FALSE);
   
    //Identity every frame? unsure.
    sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)identity_matrix);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)identity_matrix);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity_matrix);
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
    /* There should be something here! */
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
