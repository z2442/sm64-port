#define TARGET_SCEGU 1
#if defined(TARGET_SCEGU) || defined(TARGET_PSP)

#define PSP_ARENA_C18 0
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <psputils.h>
#include <pspgum.h>
#include <intraFont.h>
#include <string.h>

#include "psp_texture_manager.h"
#include "gfx_screen_config.h"
extern unsigned int texman_get_bound(void); /* psp_texture_manager.c */
#include "../psp_home_menu.h"

/* Vaciado diferido de la arena de vertices (PSP_WB_DEFER): los vertices de cada tanda se escriben a la RAM (sceKernelDcacheWritebackRange) una sola vez
 * por aviso al GE, no una vez por tanda. El GE solo lee la lista hasta el "stall" que se le dio por ultima vez, asi que lo que importa es que TODO lo
 * pendiente este en RAM antes de cualquier llamada de libgu que mueva ese tope (sceGuGetMemory, sceGuDrawArray, sceGuFinish, sceGuCallList e
 * intraFontPrint, que dibuja con sceGuDrawArray). Estas macros lo hacen por delante de cada una de ellas, en todo este archivo. */
#ifndef PSP_WB_DEFER
#define PSP_WB_DEFER 1
#endif
static __attribute__((noinline)) void gfx_scegu_varena_publish(void);
#if PSP_WB_DEFER
#define sceGuGetMemory(s_) (gfx_scegu_varena_publish(), sceGuGetMemory(s_))
#define sceGuDrawArray(...) (gfx_scegu_varena_publish(), sceGuDrawArray(__VA_ARGS__))
#define sceGuFinish() (gfx_scegu_varena_publish(), sceGuFinish())
#define sceGuCallList(l_) (gfx_scegu_varena_publish(), sceGuCallList(l_))
#define intraFontPrint(...) (gfx_scegu_varena_publish(), intraFontPrint(__VA_ARGS__))
#endif

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define FRAMEBUFFER_SIZE (BUF_WIDTH * SCR_HEIGHT * sizeof(uint16_t))

float identity_matrix[4][4] __attribute__((aligned(16))) = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };

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

// clang-format off
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
153092165,153092165,
153092608,153092608,
153093005,153093005,
153094656,153094656
};
/* Antes los shaders con NOISE (G_AC_DITHER: Mario desvaneciendose al teletransportarse, gorra de vanish, etc.)
 * se remapeaban al shader 69, que es TEXTURADO. Los de solo color (cuerpo de Mario, id 153092608) acababan
 * activando GU_TEXTURE_2D con la ultima textura enlazada, muestreando un texel transparente (u=v=0) y el
 * modelo desaparecia entero; solo se veian las partes con textura propia. Ahora se re-aplican a si mismos
 * (el GE no tiene ruido de alpha, se dibuja como alpha normal con el desvanecimiento progresivo). */
// clang-format on

static uint32_t shader_broken[27] = {
    153092165, // Noise
    153092608, // Noise
    153093005, // Noise
    153094656  // Noise
};

unsigned int __attribute__((aligned(64))) list[262144 * 2];

/* sceGuGetMemory stores transient vertices in the display list. Menu text can
 * emit many small sprites, so continue in a fresh list before an allocation
 * can overrun the fixed command buffer. GE state and buffers survive this. */
#define GU_LIST_COMMAND_RESERVE 4096

static size_t sVArenaUsed; /* bytes usados de la arena de vertices (ver mas abajo) */
static size_t sVArenaWb; /* bytes del principio de la arena que ya estan escritos a la RAM (solo con PSP_WB_DEFER) */

/* ---- Lista corta para el 2o cuadro (PSP_F2_REPLAY) ----
 * La lista grande `list` (A) guarda la escena del 1er cuadro de un tick de 60 fps. El 2o cuadro abre esta otra (B), solo con su
 * preambulo (limpiar pantalla, etc.) y una orden CALL a la escena que sigue en A. Asi A no se toca hasta el tick siguiente.
 * sCurList / sCurListBytes dicen cual esta abierta; sListGen cuenta los reinicios de lista (un reinicio pisa A desde el principio,
 * asi que invalida cualquier grabacion). */
#define F2_LIST_WORDS 2048
static unsigned int __attribute__((aligned(64))) listB[F2_LIST_WORDS];
static unsigned int *sCurList = list;
static size_t sCurListBytes = sizeof(list);
static unsigned int sListGen;
extern int ge_list_executed[2]; /* variable global de libpspgu: id de la lista del GE que abrio sceGuStart */
static int sFrameList = -1;     /* id de la lista del GE que esta abierta (la actualizan start_frame y list_restart) */

/* Cierra la lista, espera al GE y abre otra (el estado del GE se conserva). Tras esperar, el GE ya no lee la arena. */
static void gfx_scegu_list_restart(void) {
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceGuStart(GU_DIRECT, sCurList);
    sFrameList = ge_list_executed[0];
    sVArenaUsed = 0;
    sVArenaWb = 0;
    sListGen++;
}

static void gfx_scegu_reserve_list_memory(size_t data_size) {
    const size_t allocation_size = (data_size + 3) & ~(size_t)3;
    const size_t required_size = allocation_size + 8 + GU_LIST_COMMAND_RESERVE;

    if (((size_t)sceGuCheckList() + required_size) <= sCurListBytes) {
        return;
    }

    gfx_scegu_list_restart();
}

/* ---- Arena de vertices (cacheada) ----
 * sceGuGetMemory devuelve memoria DENTRO de la lista de comandos, y pspgu escribe esa lista por la direccion SIN cache
 * (sceGuStart le suma 0x40000000): cada palabra es una escritura de bus. Copiar ahi ~100 KB de vertices por cuadro costaba
 * ~0,8 ms de CPU. Aqui los vertices se copian a una arena normal (con cache) y se escribe la cache a RAM con
 * sceKernelDcacheWritebackRange justo antes de dibujar; el GE lee la arena por puntero (la lista solo lleva la orden de
 * dibujo). La arena se reutiliza al empezar cada cuadro (el cuadro anterior ya termino: end_frame sincroniza con el GE) y,
 * si se llena a mitad de cuadro, se espera al GE y se reinicia la lista, igual que gfx_scegu_reserve_list_memory.
 * (Idea tomada de ghurone/Sonic-R-PSP, que mide el mismo problema.) PSP_VERTEX_ARENA 0 vuelve a sceGuGetMemory. */
#ifndef PSP_VERTEX_ARENA
#define PSP_VERTEX_ARENA 1
#endif
#define VARENA_SIZE (512 * 1024)
static unsigned char sVArena[VARENA_SIZE] __attribute__((aligned(64)));
unsigned int gPspWbCalls; /* vaciados reales de cache de la arena (se imprime en el registro) */
static __attribute__((noinline)) void gfx_scegu_varena_publish(void) {
#if PSP_WB_DEFER
    if (sVArenaUsed > sVArenaWb) {
        sceKernelDcacheWritebackRange(sVArena + sVArenaWb, sVArenaUsed - sVArenaWb);
        sVArenaWb = sVArenaUsed;
        gPspWbCalls++;
    }
#endif
}
unsigned int gPspArenaMax; /* mayor uso de la arena en la ventana de perfil (bytes), lo imprime gfx_pc.c */

/* Garantiza que caben `bytes` mas en la arena (si no, espera al GE y la reinicia). Llamar ANTES de cambiar estado del GE. */
static void gfx_scegu_varena_ensure(size_t bytes) {
    bytes = (bytes + 63) & ~(size_t)63;
    if (sVArenaUsed + bytes > VARENA_SIZE) {
        gfx_scegu_list_restart();
    }
}

static void *gfx_scegu_varena_alloc(size_t bytes) {
    bytes = (bytes + 63) & ~(size_t)63;
    if (sVArenaUsed + bytes > VARENA_SIZE) {
        gfx_scegu_list_restart();
    }
    void *p = sVArena + sVArenaUsed;
    sVArenaUsed += bytes;
    if (sVArenaUsed > gPspArenaMax) {
        gPspArenaMax = (unsigned int)sVArenaUsed;
    }
#if defined(TARGET_PSP) && PSP_ARENA_C18
    /* APAGADO (parche 108): en el nivel real solo movio el coste de DRWMS (-0,2 ms) a VTXMS (+0,2 ms), sin ganancia neta.
     * Medido en la PSP real (devbench3): escribir en memoria fria obliga a leer antes cada linea de 64 B de la RAM (~220 ns) solo para
     * sobrescribirla entera. "cache 0x18" (create dirty exclusive) reserva la linea en la cache sin leerla: copiar un lote + writeback pasa de
     * 7,2 a 4,6 us (-35 %), sin datos corruptos. Es seguro aqui porque la arena esta alineada a 64 B y cada reserva se redondea a lineas enteras:
     * ninguna linea es compartida con otro lote, y de lo reservado el GE solo lee lo que se escribe (los bytes de relleno del final no se usan). */
    for (size_t o = 0; o < bytes; o += 64) {
        __asm__ volatile("cache 0x18, 0(%0)" :: "r"((unsigned char *)p + o) : "memory");
    }
#endif
    return p;
}

/* ---- Envio de draws sin una llamada al kernel por cada uno ----
 * sceGuDrawArray de libgu termina SIEMPRE con sceGeListUpdateStallAddr (una llamada al kernel que le dice al GE "ya puedes leer hasta aqui"):
 * con ~130 draws por cuadro eran ~130 llamadas (el costo fijo de ~35 us por draw que se ve en el registro, DRW). Aqui los 4 comandos del draw
 * (tipo de vertice, base, direccion, PRIM) se escriben en la lista con sceGuSendCommandi, que NO avisa al GE, y se avisa cada
 * PSP_KICK_DRAWS draws o PSP_KICK_TRIS triangulos (sceGuGetMemory(0) mueve el tope de la lista hasta aqui). El GE sigue trabajando mientras la
 * CPU arma el resto del cuadro, y el ultimo aviso lo da sceGuFinish. Lo dibujado es identico. PSP_GE_KICK 0 vuelve a sceGuDrawArray. */
#ifndef PSP_GE_KICK
#define PSP_GE_KICK 1
#endif
/* Cada aviso es una llamada al kernel (~30 us). Con el GE ocioso en las escenas que van lentas (el cuello es la CPU) se avisa cada
 * 12 draws / 220 triangulos en vez de 8 / 150: ~40% menos avisos, y al GE solo le queda por hacer al final del cuadro lo ultimo que se mando. */
#ifndef PSP_KICK_DRAWS
#define PSP_KICK_DRAWS 12
#endif
#ifndef PSP_KICK_TRIS
#define PSP_KICK_TRIS 220
#endif
unsigned int gPspKicks, gPspRawDraws; /* avisos al GE / draws enviados sin aviso (se imprimen en el registro) */
static unsigned int sKickDraws, sKickTris;

static void gfx_scegu_prim_tris(int vtype, int count, const void *vertices) {
#if PSP_GE_KICK
    sceGuSendCommandi(0x12, vtype);                                       /* VERTEX_TYPE */
    sceGuSendCommandi(0x10, (int)((((unsigned int)vertices) >> 8) & 0xf0000)); /* BASE */
    sceGuSendCommandi(0x01, (int)(unsigned int)vertices);                 /* VADDR */
    sceGuSendCommandi(0x04, (GU_TRIANGLES << 16) | count);                /* PRIM */
    gPspRawDraws++;
    sKickDraws++;
    sKickTris += (unsigned int)count / 3u;
    if (sKickDraws >= PSP_KICK_DRAWS || sKickTris >= PSP_KICK_TRIS) {
        sKickDraws = 0;
        sKickTris = 0;
        gPspKicks++;
        sceGuGetMemory(0); /* mueve el tope de la lista (y avisa al GE) hasta aqui */
    }
#else
    sceGuDrawArray(GU_TRIANGLES, vtype, count, 0, vertices);
#endif
}

static unsigned int staticOffset = 0;
/* Color de la niebla en formato ABGR del GE. Lo actualiza gfx_dp_set_fog_color() en gfx_pc.c.
 * Empieza en blanco, que era el valor fijo que se usaba antes. */
unsigned int scegu_fog_color = 0xFFFFFFFF;
static unsigned int sGuFogColor = 0xFFFFFFFF;
float scegu_fog_near = 3600.0f, scegu_fog_far = 32000.0f; /* distancias de vista, las calcula gfx_pc.c */
static float sGuFogNear = 3600.0f, sGuFogFar = 32000.0f; /* ultimo color enviado al GE con sceGuFog */

static unsigned int getMemorySize(unsigned int width, unsigned int height, unsigned int psm) {
    switch (psm) {
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
void *getStaticVramBuffer(unsigned int width, unsigned int height, unsigned int psm) {
    unsigned int memSize = getMemorySize(width, height, psm);
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return result;
}

void *getStaticVramBufferBytes(size_t bytes) {
    unsigned int memSize = bytes;
    void *result = (void *) (staticOffset | 0x40000000);
    staticOffset += memSize;

    return (void *) (((unsigned int) result) + ((unsigned int) sceGeEdramGetAddr()));
}

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    bool enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used[2];
    int texfunc; /* ultimo modo de sceGuTexFunc aplicado (para restaurar tras la pasada de niebla) */
    int texture_ord[2];
    int num_inputs;
};

struct SamplerState {
    int min_filter;
    int mag_filter;
    int wrap_s;
    int wrap_t;
    uint32_t tex;
};

typedef struct Vertex {
    float u, v;
    unsigned int color;
    float x, y, z;
} Vertex;

typedef struct VertexColor {
    unsigned short a, b;
    unsigned long color;
    unsigned short x, y, z;
} VertexColor;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;
static struct SamplerState tmu_state[2];
static bool gl_blend = false;
static void *sDrawBuffer;
static void *sDisplayBuffer;
#define PSP_NFB 3
static void *sFb[PSP_NFB]; /* buffers de color en VRAM; el cuadro N se dibuja en sFb[N % 3] */
static bool sHomeMenuBgActive;
static bool sHomeMenuBgCaptureRequested;
static bool sHomeMenuBgCaptured;
static uint16_t sHomeMenuBgBuffer[BUF_WIDTH * SCR_HEIGHT] __attribute__((aligned(64)));
static uint16_t sHomeMenuBgBlurScratch[BUF_WIDTH * SCR_HEIGHT] __attribute__((aligned(64)));
static intraFont *sHomeMenuFont;
static bool sHomeMenuFontInitTried;

extern void memcpy_vfpu(void *dst, const void *src, size_t size);

static void *gfx_scegu_vram_cpu_addr(const void *vram_buffer) {
    return (void *)(((uintptr_t)sceGeEdramGetAddr() | 0x40000000U) +
                    ((uintptr_t)vram_buffer & 0x00FFFFFFU));
}

static void gfx_scegu_copy_framebuffer_from_vram(uint16_t *dst, const void *src) {
    memcpy_vfpu(dst, gfx_scegu_vram_cpu_addr(src), FRAMEBUFFER_SIZE);
}

static void gfx_scegu_copy_framebuffer_to_vram(void *dst, const uint16_t *src) {
    void *dst_addr = gfx_scegu_vram_cpu_addr(dst);

    memcpy_vfpu(dst_addr, src, FRAMEBUFFER_SIZE);
    sceKernelDcacheWritebackRange(dst_addr, FRAMEBUFFER_SIZE);
}

static uint16_t gfx_scegu_make_rgb565(unsigned int r, unsigned int g, unsigned int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void gfx_scegu_accum_rgb565(uint16_t color, unsigned int *r, unsigned int *g,
                                   unsigned int *b) {
    *r += ((color >> 11) & 0x1F) << 3;
    *g += ((color >> 5) & 0x3F) << 2;
    *b += (color & 0x1F) << 3;
}

static int gfx_scegu_clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void gfx_scegu_blur_framebuffer_565(uint16_t *pixels) {
    static const int offsets[3] = { -3, 0, 3 };
    int x;
    int y;
    int ox;
    int oy;

    memcpy_vfpu(sHomeMenuBgBlurScratch, pixels, FRAMEBUFFER_SIZE);
    for (y = 0; y < SCR_HEIGHT; y++) {
        for (x = 0; x < SCR_WIDTH; x++) {
            unsigned int r = 0;
            unsigned int g = 0;
            unsigned int b = 0;

            for (oy = 0; oy < 3; oy++) {
                int sample_y = gfx_scegu_clamp_int(y + offsets[oy], 0, SCR_HEIGHT - 1);

                for (ox = 0; ox < 3; ox++) {
                    int sample_x = gfx_scegu_clamp_int(x + offsets[ox], 0, SCR_WIDTH - 1);
                    gfx_scegu_accum_rgb565(pixels[(sample_y * BUF_WIDTH) + sample_x], &r, &g, &b);
                }
            }

            sHomeMenuBgBlurScratch[(y * BUF_WIDTH) + x] =
                gfx_scegu_make_rgb565(r / 9, g / 9, b / 9);
        }
    }

    memcpy_vfpu(pixels, sHomeMenuBgBlurScratch, FRAMEBUFFER_SIZE);
    sceKernelDcacheWritebackRange(pixels, FRAMEBUFFER_SIZE);
}

static void gfx_scegu_apply_home_menu_2d_view(void) {
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2), SCR_WIDTH, SCR_HEIGHT);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)identity_matrix);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity_matrix);
}

static unsigned int gfx_scegu_rgba(unsigned int r, unsigned int g, unsigned int b,
                                   unsigned int a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

static void gfx_scegu_draw_rect(int x, int y, int width, int height, unsigned int color) {
    VertexColor *verts;

    if ((width <= 0) || (height <= 0)) {
        return;
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 2);
    verts = (VertexColor *)sceGuGetMemory(sizeof(VertexColor) * 2);
    if (verts == NULL) {
        return;
    }

    verts[0].a = 0;
    verts[0].b = 0;
    verts[0].color = color;
    verts[0].x = (unsigned short)x;
    verts[0].y = (unsigned short)y;
    verts[0].z = 0;
    verts[1].a = 0;
    verts[1].b = 0;
    verts[1].color = color;
    verts[1].x = (unsigned short)(x + width);
    verts[1].y = (unsigned short)(y + height);
    verts[1].z = 0;

    gfx_scegu_apply_home_menu_2d_view();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                                   GU_TRANSFORM_2D,
                   2, 0, verts);
}

static bool gfx_scegu_ensure_home_menu_font(void) {
    if (!sHomeMenuFontInitTried) {
        sHomeMenuFontInitTried = true;
        if (intraFontInit()) {
            sHomeMenuFont = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
        }
    }
    return sHomeMenuFont != NULL;
}

static void gfx_scegu_draw_home_menu_text(int x, int y, const char *text, float size,
                                          unsigned int color, unsigned int options) {
    if (!gfx_scegu_ensure_home_menu_font()) {
        return;
    }

    gfx_scegu_apply_home_menu_2d_view();
    intraFontActivate(sHomeMenuFont);
    intraFontSetStyle(sHomeMenuFont, size, color, gfx_scegu_rgba(0, 0, 0, 180), 0.0f,
                      options | INTRAFONT_STRING_ASCII);
    intraFontPrint(sHomeMenuFont, (float)x, (float)y, text);
}

static void gfx_scegu_prepare_home_menu_draw(void) {
    gfx_scegu_apply_home_menu_2d_view();
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexEnvColor(0xffffffff);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_FALSE);
}

static void gfx_scegu_render_home_menu_main(int selected_index, uint8_t highlight_red,
                                            uint8_t highlight_green, uint8_t highlight_blue) {
    static const char *items[] = {
        "Resume Game",
        "Controller Mapping",
        "Exit Game",
    };
    int i;

    gfx_scegu_draw_rect(0, 0, SCR_WIDTH, SCR_HEIGHT, gfx_scegu_rgba(0, 0, 0, 96));
    gfx_scegu_draw_rect(102, 44, 276, 184, gfx_scegu_rgba(0, 0, 0, 150));
    gfx_scegu_draw_home_menu_text(SCR_WIDTH / 2, 78, "Super Mario 64", 0.82f,
                                  gfx_scegu_rgba(255, 255, 245, 255), INTRAFONT_ALIGN_CENTER);

    for (i = 0; i < 3; i++) {
        int y = 122 + (i * 38);
        unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

        if (selected_index == i) {
            gfx_scegu_draw_rect(124, y - 22, 232, 28,
                                gfx_scegu_rgba(highlight_red, highlight_green,
                                               highlight_blue, 205));
            color = gfx_scegu_rgba(255, 255, 245, 255);
        }
        gfx_scegu_draw_home_menu_text(SCR_WIDTH / 2, y, items[i], 0.72f, color,
                                      INTRAFONT_ALIGN_CENTER);
    }
}

static void gfx_scegu_render_controller_mapping(int selected_index, const char *status_message,
                                                 uint8_t highlight_red, uint8_t highlight_green,
                                                 uint8_t highlight_blue) {
    char line[96];
    char value[64];
    int binding_count = psp_home_menu_get_binding_count();
    int deadzone_row = binding_count;
    int save_row = binding_count + 1;
    int reset_row = binding_count + 2;
    int back_row = binding_count + 3;
    int total_rows = back_row + 1;
    int visible_rows = 7;
    int first_row = selected_index - (visible_rows / 2);
    int row;

    if (first_row < 0) {
        first_row = 0;
    }
    if ((first_row + visible_rows) > total_rows) {
        first_row = total_rows - visible_rows;
    }

    gfx_scegu_draw_rect(0, 0, SCR_WIDTH, SCR_HEIGHT, gfx_scegu_rgba(0, 0, 0, 112));
    gfx_scegu_draw_rect(34, 20, 412, 232, gfx_scegu_rgba(0, 0, 0, 166));
    gfx_scegu_draw_home_menu_text(SCR_WIDTH / 2, 50, "Controller Mapping", 0.82f,
                                  gfx_scegu_rgba(255, 255, 245, 255), INTRAFONT_ALIGN_CENTER);

    for (row = first_row; row < first_row + visible_rows; row++) {
        int y = 82 + ((row - first_row) * 22);
        unsigned int color = gfx_scegu_rgba(218, 224, 218, 255);

        if (selected_index == row) {
            gfx_scegu_draw_rect(54, y - 17, 372, 22,
                                gfx_scegu_rgba(highlight_red, highlight_green,
                                               highlight_blue, 205));
            color = gfx_scegu_rgba(255, 255, 245, 255);
        }

        if (row < binding_count) {
            psp_home_menu_get_binding_value_text(row, value, sizeof(value));
            snprintf(line, sizeof(line), "%s: %.40s", psp_home_menu_get_binding_name(row), value);
        } else if (row == deadzone_row) {
            snprintf(line, sizeof(line), "Deadzone: %d", psp_home_menu_get_deadzone());
        } else if (row == save_row) {
            snprintf(line, sizeof(line), "Save sm64config.txt");
        } else if (row == reset_row) {
            snprintf(line, sizeof(line), "Reset defaults");
        } else {
            snprintf(line, sizeof(line), "Back");
        }

        gfx_scegu_draw_home_menu_text(70, y, line, 0.68f, color, INTRAFONT_ALIGN_LEFT);
    }

    gfx_scegu_draw_home_menu_text(
        SCR_WIDTH / 2, 242,
        ((status_message != NULL) && status_message[0])
            ? status_message
            : "Left/Right change  Cross select  Circle back",
        0.48f, gfx_scegu_rgba(185, 195, 190, 255), INTRAFONT_ALIGN_CENTER);
}

static inline uint32_t get_shader_index(uint32_t id) {
    size_t i;
    for (i = 0; i < 27; i++) {
        if (shader_ids[i] == id) {
            return i;
        }
    }
    char msg[32];
    sprintf(msg, "ERROR! Shader not known %lu\n", (unsigned long)id);
    sceIoWrite(2, msg, strlen(msg));
    return 0;
}

static inline uint32_t get_shader_remap(uint32_t id) {
    size_t index = get_shader_index(id);
    return shader_remap[index * 2 + 1];
}

static inline bool is_shader_enabled(uint32_t id) {
    size_t i;
    for (i = 0; i < 27; i++) {
        if (shader_broken[i] == id) {
            return false;
        }
    }
    return true;
}

static struct ShaderProgram *get_shader_from_id(uint32_t id) {
    size_t i;
    for (i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static bool gfx_scegu_z_is_from_0_to_1(void) {
    return true;
}

static inline int texenv_set_color(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture(UNUSED struct ShaderProgram *prg) {
    return GU_TFX_MODULATE;
}

static inline int texenv_set_texture_color(struct ShaderProgram *prg) {
    int mode;
    /* BLENDRGBFADEA (TEXEL0, SHADE, TEXEL0_ALPHA, SHADE / alpha = ENV): textura sobre el color segun SU alpha = DECAL en el GE,
     * en todas las variantes (con ruido, con niebla). BLENDRGBA (alpha = SHADE, explosiones y humo) sigue en MODULATE.
     * Si el parche ya se dibujo en una pasada de color (Boo), gfx_pc.c pide MODULATE para esa pasada (scegu_enva_modulate). */
    if ((prg->shader_id & 0xFFFu) == 0x38Du && (prg->shader_id & (1u << 28))) { /* SHADER_OPT_ENVA: alpha = ENV */
        return GU_TFX_DECAL;
    }
    /*@Hack: lord forgive me for this, but this is easier */
    switch (prg->shader_id) {
        case 0x0000038D: // mario's eyes
        case 0x0920038D: // ojos/emblema de Mario desvaneciendose (BLENDRGBFADEA + ruido): textura sobre el color por su alpha
        case 0x01045A00: // peach letter
        case 0x01200A00: // intro copyright fade in
            mode = GU_TFX_DECAL;
            break;
        case 0x00000551: // goddard
            mode = GU_TFX_BLEND;
            break;
        default:
            mode = GU_TFX_MODULATE;
            break;
    }

    return mode;
}

static inline int texenv_set_texture_texture(UNUSED struct ShaderProgram *prg) {
    /*@Note: hack shader 0x1A00A6F for Bowser/Peach Paintings (still broken, but just fixed on peach)*/
    return GU_TFX_DECAL;
}

static void gfx_scegu_apply_shader(struct ShaderProgram *prg) {
    /* El alpha test se decide SIEMPRE, tambien en shaders sin textura. Antes, los shaders sin textura salian
     * de la funcion antes de tocarlo y heredaban el estado del shader anterior: con el test activo
     * (GREATER 0x55) se descartaban los fragmentos con alpha <= 85, p.ej. el relleno de color de los bloques
     * de gorra (alpha 80), que solo se veia segun lo ultimo que se hubiera dibujado. */
    if (prg->shader_id & SHADER_OPT_TEXTURE_EDGE) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); /* 0.3f  */
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    // If we have textures, Enable otherwise Disable
    if (prg->texture_used[0] || prg->texture_used[1]) {
        sceGuEnable(GU_TEXTURE_2D);
    } else {
        sceGuDisable(GU_TEXTURE_2D);
        return;
    }

    /* La niebla (GE lineal o 2da pasada exacta) se decide en gfx_scegu_draw_triangles. */

    if (prg->num_inputs) {
        // have colors
        // TODO: more than one color (maybe glSecondaryColorPointer?)
        // HACK: if there's a texture and two colors, one of them is likely for speculars or some shit
        // (see mario head)
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
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }

    if (!prg->enabled) {
        // configure formulae, we only need to do this once
        prg->enabled = true;

        int mode;
        switch (prg->mix) {
            case SH_MT_TEXTURE:
                mode = texenv_set_texture(prg);
                break;
            case SH_MT_TEXTURE_TEXTURE:
                mode = texenv_set_texture_texture(prg);
                break;
            case SH_MT_TEXTURE_COLOR:
                mode = texenv_set_texture_color(prg);
                break;
            default:
                mode = texenv_set_color(prg);
                break;
        }

        /* Transition Screens */
        if (prg->shader_id == 0x01A00045) {
            mode = GU_TFX_REPLACE;
        }
        prg->texfunc = mode;
        sceGuTexFunc(mode, GU_TCC_RGBA);
    }
}

static void gfx_scegu_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg)) {
        cur_shader->enabled = false;
        cur_shader = NULL;
    }
}

static void gfx_scegu_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    gfx_scegu_apply_shader(cur_shader);
    if (cur_shader)
        cur_shader->enabled = false;
}

static struct ShaderProgram *gfx_scegu_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0];
    prg->texture_used[1] = ccf.used_textures[1];

    if (ccf.used_textures[0] && ccf.used_textures[1]) {
        prg->mix = SH_MT_TEXTURE_TEXTURE;
        if (ccf.do_single[1]) {
            prg->texture_ord[0] = 1;
            prg->texture_ord[1] = 0;
        } else {
            prg->texture_ord[0] = 0;
            prg->texture_ord[1] = 1;
        }
    } else if (ccf.used_textures[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_textures[0]) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    }

    prg->enabled = false;

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

static uint32_t gfx_scegu_new_texture(void) {
    return texman_create();
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_MIRROR)
        return GU_REPEAT;
    if (val & G_TX_CLAMP)
        return GU_CLAMP;
    return GU_REPEAT;
}

static inline int ispow2(uint32_t x) {
    return (x & (x - 1)) == 0;
}

// compute the next highest power of 2 of 32-bit v
static inline int nextpow2(int v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v++;
    return v;
}

static inline void gfx_scegu_apply_tmu_state(const int tile) {
    sceGuTexFilter(tmu_state[tile].min_filter, tmu_state[tile].mag_filter);
    sceGuTexWrap(tmu_state[tile].wrap_s, tmu_state[tile].wrap_t);
}

static void gfx_scegu_set_sampler_parameters(const int tile, const bool linear_filter, const uint32_t cms, const uint32_t cmt) {
    const int filter = linear_filter ? GU_LINEAR : GU_NEAREST;

    const int wrap_s = gfx_cm_to_opengl(cms);
    const int wrap_t = gfx_cm_to_opengl(cmt);

    tmu_state[tile].min_filter = filter;
    tmu_state[tile].mag_filter = filter;
    tmu_state[tile].wrap_s = wrap_s;
    tmu_state[tile].wrap_t = wrap_t;

    // set state for the first texture right away
    if (!tile)
        gfx_scegu_apply_tmu_state(tile);
}

static void gfx_scegu_select_texture(int tile, uint32_t texture_id) {
    /* tmu_state[].tex solo se actualizaba aqui, pero las subidas de textura (texman_upload*) y mis pasadas
     * dobles enlazan otra textura sin avisar. Si despues se pedia una textura igual a la ULTIMA registrada,
     * se saltaba el enlace y se dibujaba con otra textura (p.ej. el mural del lobby con la parte equivocada).
     * Ahora tambien se compara con la textura realmente enlazada. */
    if (tmu_state[tile].tex != texture_id || texman_get_bound() != texture_id) {
        tmu_state[tile].tex = texture_id;
        texman_bind_tex(texture_id);
        gfx_scegu_set_sampler_parameters(tile, false, 0, 0);
    }
}

/* Used for rescaling textures ROUGHLY into pow2 dims */
static unsigned int __attribute__((aligned(16))) scaled[256 * 256 * sizeof(unsigned int)]; /* 16kb */
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

static void gfx_scegu_upload_texture(const uint8_t *rgba32_buf, int width, int height, unsigned int type) {
    if (ispow2(width) && ispow2(height)) {
        texman_upload_swizzle(width, height, type, (void *) rgba32_buf);
    } else {
        int scaled_width = nextpow2(width);
        int scaled_height = nextpow2(height);

        if (type == GU_PSM_8888) {
            gfx_scegu_resample_32bit((const unsigned int *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        } else if (type == GU_PSM_5551) {
            gfx_scegu_resample_16bit((const unsigned short *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
        } else {
            gfx_scegu_resample_8bit((const unsigned char *) rgba32_buf, width, height, (void *) scaled, scaled_width, scaled_height);
            texman_upload_swizzle(scaled_width, scaled_height, type, (void *) scaled);
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

static bool sDepthMaskOn = false; /* true = el GE NO escribe z (GU_TRUE) */
static void gfx_scegu_set_depth_mask(bool z_upd) {
    sDepthMaskOn = !z_upd;
    sceGuDepthMask(z_upd ? GU_FALSE : GU_TRUE);
}

/* Desplazamiento de profundidad por triangulo dentro del modo decal (gfx_pc.c, PSP_DECAL_ADAPTIVE). */
void gfx_scegu_set_decal_bias(int bias) {
    sceGuDepthOffset(bias);
}

static void gfx_scegu_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        sceGuDepthOffset(PSP_DECAL_OFFSET); /* z-buffer de 16 bits en la PSP: ver gfx_screen_config.h */
    } else {
        sceGuDepthOffset(0);
    }
}

static void gfx_scegu_set_viewport(int x, int y, int width, int height) {
    sceGuViewport(2048 - (SCR_WIDTH / 2) + x + (width / 2), 2048 + (SCR_HEIGHT / 2) - y - (height / 2), width, height);
    sceGuScissor(x, SCR_HEIGHT - y - height, x + width, SCR_HEIGHT - y);
}

static void gfx_scegu_set_scissor(int x, int y, int width, int height) {
    sceGuScissor(x, SCR_HEIGHT - y - height, x + width, SCR_HEIGHT - y);
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

/* Niebla exacta N64:  rgb_final = mezcla(rgb_combinador, color_niebla, F)  (F por vertice, 0..255), alpha intacto.
 * El GE no puede hacerlo en 1 pasada, asi que segun el tipo de shader:
 *  1 = opaco:            2da pasada "overlay" (src=niebla, alpha=F, blend normal), depth GEQUAL.
 *  2 = recorte (edge):   igual pero depth EQUAL, asi solo se pinta donde la 1ra pasada escribio z (texels no descartados).
 *  3 = translucido:      la 1ra pasada ya sale con rgb*(1-F) (lo hace gfx_pc.c en los vertices) y la 2da suma
 *                        niebla*alpha*F con blend aditivo: a*(C*(1-F)+Niebla*F)+(1-a)*Fondo, igual que la N64.
 *  0 = sin niebla exacta (modos de textura especiales): niebla lineal del GE (aproximada).
 * PSP_EXACT_FOG 0 desactiva todo y usa siempre la lineal. */
#ifndef PSP_EXACT_FOG
#define PSP_EXACT_FOG 1
#endif
extern uint32_t scegu_mt_tex[2]; /* gfx_pc.c */
extern float scegu_mtx_proj[16], scegu_mtx_view[16], scegu_mtx_model[16]; /* gfx_pc.c: ultimas matrices enviadas al GE */
extern unsigned char scegu_fog_alpha[]; /* gfx_pc.c: factor 0..255 por vertice del buffer */

int scegu_fog_mode(uint32_t sid) {
    if (!PSP_EXACT_FOG || !(sid & SHADER_OPT_FOG)) return 0;
    if (sid & SHADER_OPT_ALPHA) {
        /* solo vale si la textura se MODULA con el color del vertice (los demas modos ignoran el color) */
        switch (sid) {
            case 0x0000038D: case 0x01045A00: case 0x01200A00: case 0x00000551: case 0x01A00045:
                return 0;
        }
        return 3;
    }
    if (sid & SHADER_OPT_TEXTURE_EDGE) return 2;
    return 1;
}

typedef struct VertexFog {
    unsigned int color;
    float x, y, z;
} VertexFog;


/* ---- Transparencia por ruido de la N64 (G_AC_DITHER): Mario desvaneciendose, gorra vanish ----
 * El RDP compara el alpha con un ruido aleatorio de 3 bits por pixel (31,63,...,255) y descarta el pixel si
 * alpha < ruido. El pixel queda opaco y escribe z (sin mezcla). Aqui:
 *   1) pasada de profundidad: se dibuja la geometria con una textura-mascara 64x64 (alpha de 1 bit = alpha>=ruido)
 *      proyectada en espacio de pantalla (1 texel = 1 pixel), con alpha test y sin escribir color: solo
 *      queda z en los pixeles que "sobreviven";
 *   2) pasada normal con depth EQUAL, sin blend: solo se pinta donde sobrevivio la pasada 1.
 * El ruido se vuelve a sortear cada frame. Poner PSP_NOISE_STIPPLE 0 para volver al blend normal. */
#define PSP_NOISE_STIPPLE 1
#ifndef ALIGN
#define ALIGN(VAL_, ALIGNMENT_) (((VAL_) + ((ALIGNMENT_) - 1)) & ~((ALIGNMENT_) - 1))
#endif
#define NOISE_MASK_SIZE 64
static unsigned short sNoiseMask[NOISE_MASK_SIZE * NOISE_MASK_SIZE] __attribute__((aligned(16)));
static unsigned int sNoiseFrame = 0, sNoiseMaskFrame = 0xFFFFFFFFu;
static int sNoiseMaskAlpha = -1;
static unsigned int sNoiseRng = 0x2545F491u;
static unsigned int sNoiseRegen; /* veces que se regenero la mascara (la usa la grabacion de PSP_F2_REPLAY) */

static void gfx_scegu_noise_update_mask(int alpha) {
    if (sNoiseMaskFrame == sNoiseFrame && sNoiseMaskAlpha == alpha) return;
    sNoiseMaskFrame = sNoiseFrame;
    sNoiseMaskAlpha = alpha;
    sNoiseRegen++;
    for (int i = 0; i < NOISE_MASK_SIZE * NOISE_MASK_SIZE; i++) {
        sNoiseRng ^= sNoiseRng << 13; sNoiseRng ^= sNoiseRng >> 17; sNoiseRng ^= sNoiseRng << 5; /* xorshift32 */
        const int noise = (int)(((sNoiseRng >> 8) & 7) << 5) | 0x1f;
        sNoiseMask[i] = (alpha >= noise) ? 0xFFFF : 0x7FFF; /* bit 15 = alpha 1 bit: sobrevive */
    }
    sceKernelDcacheWritebackRange(sNoiseMask, sizeof(sNoiseMask));
}

static void gfx_scegu_mat4_mul(float *out, const float *a, const float *b) { /* out = a * b (filas, convencion N64) */
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, t, sizeof(t));
}

/* ---- Perfil de GPU (solo con el atajo Triangulo+L del registro; gfx_pc.c) ----
 * Cada llamada de dibujo se rodea de "cerrar lista + esperar al GE + abrir lista": asi el tiempo entre ambos puntos es lo que
 * tarda el GE en esa llamada (menos el coste fijo de reiniciar la lista, que se mide al empezar cada cuadro). Los cuadros van
 * muy lentos, pero los tiempos por pasada son validos. Buckets: ver el comentario del log en gfx_pc.c. */
#define GPUP_N 11
extern int gPspGpuProf;
#if PSP_GPU_PROF
#define PSP_GPUP() (gPspGpuProf)
#else
#define PSP_GPUP() (0)
#endif
extern float gPcAreaAcc;
unsigned int gGpuUs[GPUP_N], gGpuTris[GPUP_N], gGpuPx[GPUP_N], gGpuCalls[GPUP_N];
static unsigned int sGpupT0, sGpupBase;
static int sGpupBucket = -1, sGpupOpen = 0;
static unsigned int sGpupTris, sGpupPx;

static inline unsigned int gpup_now(void) { return sceKernelGetSystemTimeLow(); }

static __attribute__((unused)) void gpup_calibrate(void) {
    unsigned int acc = 0;
    for (int i = 0; i < 8; i++) {
        const unsigned int t = gpup_now();
        gfx_scegu_list_restart();
        acc += gpup_now() - t;
    }
    sGpupBase = acc / 8;
}

static __attribute__((unused)) void gpup_begin(void) {
    const unsigned int t = gpup_now();
    gfx_scegu_list_restart(); /* espera a todo lo anterior (limpiar pantalla, 2D, ...): se apunta aparte */
    const unsigned int dt = gpup_now() - t;
    gGpuUs[10] += dt > sGpupBase ? dt - sGpupBase : 0;
    sGpupT0 = gpup_now();
    sGpupOpen = 1;
}

static __attribute__((unused)) void gpup_end(void) {
    if (!sGpupOpen || sGpupBucket < 0) { sGpupOpen = 0; return; }
    gfx_scegu_list_restart();
    const unsigned int dt = gpup_now() - sGpupT0;
    gGpuUs[sGpupBucket] += dt > sGpupBase ? dt - sGpupBase : 0;
    gGpuTris[sGpupBucket] += sGpupTris;
    gGpuPx[sGpupBucket] += sGpupPx;
    gGpuCalls[sGpupBucket]++;
    sGpupOpen = 0;
}

/* abre la medicion de una llamada de dibujo de triangulos con el shader `sid` */
static __attribute__((unused)) void gpup_open_tris(uint32_t sid, bool blend, size_t ntris) {
    const int cat = blend ? 2 : ((sid & SHADER_OPT_TEXTURE_EDGE) ? 1 : 0);
    sGpupBucket = cat + ((sid & SHADER_OPT_FOG) ? 3 : 0);
    sGpupTris = (unsigned int)ntris;
    sGpupPx = (unsigned int)gPcAreaAcc;
    gPcAreaAcc = 0.0f;
    gpup_begin();
}

/* primera pasada terminada: pasa a medir la pasada de niebla */
static __attribute__((unused)) void gpup_split(int bucket) {
    gpup_end();
    sGpupBucket = bucket;
    gpup_begin(); /* (el coste del reinicio entre pasadas queda en el bucket 10) */
}

extern int scegu_enva_modulate; /* gfx_pc.c */
extern unsigned int gPcFogAll, gPcFogKeep; /* gfx_pc.c: contadores de la 2a pasada de niebla */
static void gfx_scegu_draw_triangles_real(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    const uint32_t sid = cur_shader->shader_id;
    if (PSP_GPUP()) {
        gpup_open_tris(sid, gl_blend, buf_vbo_num_tris);
    }
    if ((sid & (1u << 28)) && scegu_enva_modulate) {
        /* parche BLENDRGBFADEA ya dibujado en una pasada de color: solo texels con alpha (color de vertice blanco), ver gfx_pc.c */
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    }

    /* Cuadro Peach/Bowser: la N64 mezcla 2 texturas (mipmap TRILERP: T0 + lod*(T1-T0)). El GE solo
     * lee una textura por pasada, asi que se dibuja T0 opaca y encima T1 con alpha = lod (el alpha del
     * color de vertice ya trae el lod). */
    if ((sid & ~(SHADER_OPT_FOG | SHADER_OPT_TEXTURE_EDGE | SHADER_OPT_NOISE)) == 0x01A00A6Fu) {
        if (PSP_GPUP()) sGpupBucket = 8;
        const size_t mv = 3 * buf_vbo_num_tris;
        gfx_scegu_reserve_list_memory(sizeof(Vertex) * mv * 2 + 64);
        Vertex *b0 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        Vertex *b1 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        memcpy_vfpu(b1, buf_vbo, sizeof(Vertex) * mv);
        memcpy_vfpu(b0, buf_vbo, sizeof(Vertex) * mv);
        for (size_t k = 0; k < mv; k++) b0[k].color = 0xFFFFFFFFu;

        sceGuDisable(GU_FOG);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexFunc(GU_TFX_DECAL, GU_TCC_RGBA);

        sceGuDisable(GU_BLEND);
        texman_bind_tex(scegu_mt_tex[0]);
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b0);

        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDepthMask(GU_TRUE);              /* misma z que la 1ra pasada: GEQUAL la deja pasar */
        texman_bind_tex(scegu_mt_tex[1]);
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b1);

        sceGuDepthMask(sDepthMaskOn ? GU_TRUE : GU_FALSE);
        if (!gl_blend) sceGuDisable(GU_BLEND);
        return;
    }


#if PSP_NOISE_STIPPLE
    if (sid & SHADER_OPT_NOISE) {
        if (PSP_GPUP()) sGpupBucket = 8;
        const size_t mv = 3 * buf_vbo_num_tris;
        gfx_scegu_reserve_list_memory(sizeof(Vertex) * mv * 2 + 128);
        Vertex *b0 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        Vertex *b1 = (Vertex *)sceGuGetMemory(sizeof(Vertex) * mv);
        memcpy_vfpu(b1, buf_vbo, sizeof(Vertex) * mv);
        memcpy_vfpu(b0, buf_vbo, sizeof(Vertex) * mv);
        const int alpha = (int)(b1[0].color >> 24);
        for (size_t k = 0; k < mv; k++) b0[k].color = 0xFFFFFFFFu;
        gfx_scegu_noise_update_mask(alpha);

        /* matriz de textura: [pos,1] * (M*V*P) -> clip, y de clip a (s,t,q) en texels de la mascara (1 texel = 1 pixel) */
        float F[16], X[16], T[16];
        gfx_scegu_mat4_mul(F, scegu_mtx_model, scegu_mtx_view);
        gfx_scegu_mat4_mul(F, F, scegu_mtx_proj);
        memset(X, 0, sizeof(X));
        const float ks = (float)SCR_WIDTH / NOISE_MASK_SIZE, kt = (float)SCR_HEIGHT / NOISE_MASK_SIZE;
        X[0 * 4 + 0] = 0.5f * ks; X[3 * 4 + 0] = 0.5f * ks;          /* s = ks*(cx+cw)/2 */
        X[1 * 4 + 1] = -0.5f * kt; X[3 * 4 + 1] = 0.5f * kt;         /* t = kt*(cw-cy)/2 */
        X[3 * 4 + 2] = 1.0f;                                         /* q = cw */
        X[2 * 4 + 3] = 0.0f; X[3 * 4 + 3] = 1.0f;
        gfx_scegu_mat4_mul(T, F, X);
        ScePspFMatrix4 *tm = (ScePspFMatrix4 *)ALIGN((unsigned int)sceGuGetMemory(sizeof(ScePspFMatrix4) + 15), 16);
        memcpy(tm, T, sizeof(T));

        /* 1) pasada de profundidad con la mascara */
        sceGuDisable(GU_FOG);
        sceGuDisable(GU_BLEND);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_5551, 0, 0, 0);
        sceGuTexImage(0, NOISE_MASK_SIZE, NOISE_MASK_SIZE, NOISE_MASK_SIZE, sNoiseMask);
        sceGuTexFilter(GU_NEAREST, GU_NEAREST);
        sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
        sceGuSetMatrix(GU_TEXTURE, tm);
        sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
        sceGuTexProjMapMode(GU_POSITION);
        sceGuPixelMask(0xFFFFFFFF);
        sceGuDepthMask(GU_FALSE);
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b0);

        /* restaurar estado de textura/color */
        sceGuPixelMask(0);
        sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
        texman_bind_tex(texman_get_bound());
        gfx_scegu_apply_tmu_state(0);
        sceGuTexFunc(cur_shader->texfunc, GU_TCC_RGBA);
        if (cur_shader->texture_used[0] || cur_shader->texture_used[1]) sceGuEnable(GU_TEXTURE_2D); else sceGuDisable(GU_TEXTURE_2D);
        if (sid & SHADER_OPT_TEXTURE_EDGE) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); }
        else sceGuDisable(GU_ALPHA_TEST);

        /* 2) pasada normal, solo donde sobrevivio la 1a: depth EQUAL, sin blend, sin escribir z */
        sceGuDepthMask(GU_TRUE);
        sceGuDepthFunc(GU_EQUAL);
        if (cur_shader->texture_used[0] || cur_shader->texture_used[1]) {
            /* texturas con recorte (alas de la gorra...): la mascara de ruido solo conoce el alpha del vertice; el alpha de la
             * textura se aplica aqui con alpha test (sin mezcla). Alpha de vertice a 255 para que solo cuente el de la textura. */
            for (size_t k = 0; k < mv; k++) b1[k].color |= 0xFF000000u;
            if (!(sid & SHADER_OPT_TEXTURE_EDGE)) { sceGuEnable(GU_ALPHA_TEST); sceGuAlphaFunc(GU_GREATER, 0x55, 0xff); }
        }
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, mv, 0, b1);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthMask(sDepthMaskOn ? GU_TRUE : GU_FALSE);
        if (!(sid & SHADER_OPT_TEXTURE_EDGE)) sceGuDisable(GU_ALPHA_TEST); /* el alpha test de textura de la 2a pasada no debe filtrarse */
        if (gl_blend) sceGuEnable(GU_BLEND);
        return;
    }
#endif
    const bool fogged = (sid & SHADER_OPT_FOG) != 0;
    const int fogmode = scegu_fog_mode(sid);

    if (fogged && fogmode == 0) {
        /* Niebla lineal del GE (aproximacion con near/far calculados en gfx_pc.c). */
        sceGuEnable(GU_FOG);
        if (scegu_fog_color != sGuFogColor || scegu_fog_near != sGuFogNear || scegu_fog_far != sGuFogFar) {
            sGuFogColor = scegu_fog_color;
            sGuFogNear = scegu_fog_near; sGuFogFar = scegu_fog_far;
            sceGuFog(sGuFogNear, sGuFogFar, sGuFogColor);
        }
    } else {
        sceGuDisable(GU_FOG);
    }

    const size_t nverts = 3 * buf_vbo_num_tris;
#if PSP_VERTEX_ARENA
    /* en la lista solo van comandos; los vertices (1a pasada y 2a de niebla) van a la arena con cache */
    gfx_scegu_reserve_list_memory(64);
    gfx_scegu_varena_ensure(sizeof(Vertex) * nverts * (fogmode ? 2 : 1));
    void *buf = gfx_scegu_varena_alloc(sizeof(Vertex) * nverts);
    memcpy_vfpu(buf, buf_vbo, sizeof(Vertex) * nverts);
#if !PSP_WB_DEFER
    sceKernelDcacheWritebackRange(buf, sizeof(Vertex) * nverts);
#endif
#else
    gfx_scegu_reserve_list_memory(sizeof(Vertex) * nverts * (fogmode ? 2 : 1) + 64);
    void *buf = sceGuGetMemory(sizeof(Vertex) * nverts);
    memcpy_vfpu(buf, buf_vbo, sizeof(Vertex) * nverts);
#endif
    gfx_scegu_prim_tris(GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, (int)nverts, buf);
    if (PSP_GPUP() && fogmode) {
        const unsigned int px = sGpupPx;
        gpup_split(fogmode == 3 ? 7 : 6);
        sGpupPx = px;
    }

    if (fogmode) {
        /* La 2a pasada (color de niebla con alpha = F por vertice) no cambia NADA en un triangulo cuyos 3 vertices tienen F = 0:
         * alpha 0 -> el blend deja el fondo tal cual (y en modo 3 se suma niebla * (alpha*F) = 0). Esos triangulos (la geometria
         * cercana, antes de que empiece la niebla) se saltan: menos copia de vertices en la CPU y menos trabajo para el GE.
         * El resultado en pantalla es identico. */
        const size_t ntris_all = buf_vbo_num_tris;
        size_t keep_tris = 0;
        for (size_t t = 0; t < ntris_all; t++) {
            if (scegu_fog_alpha[3 * t] | scegu_fog_alpha[3 * t + 1] | scegu_fog_alpha[3 * t + 2]) {
                keep_tris++;
            }
        }
        gPcFogAll += (unsigned int)ntris_all;
        gPcFogKeep += (unsigned int)keep_tris;
        if (PSP_GPUP() && ntris_all) {
            sGpupPx = (unsigned int)((unsigned long long)sGpupPx * keep_tris / ntris_all);
            sGpupTris = (unsigned int)keep_tris;
        }
        if (keep_tris == 0) {
            return;
        }
        const size_t fnverts = 3 * keep_tris;
        const Vertex *src = (const Vertex *)buf_vbo;
        const bool textured = cur_shader->texture_used[0] || cur_shader->texture_used[1];
        const bool tex_alpha_pass = (fogmode == 3) && textured; /* alpha de la textura: GU_TFX_BLEND con Cf=Cc=niebla */
        const bool depth_equal = (fogmode == 2);
        const unsigned int rgb = scegu_fog_color & 0x00FFFFFFu;

        void *fbuf;
        if (tex_alpha_pass) {
#if PSP_VERTEX_ARENA
            Vertex *fvt = (Vertex *)gfx_scegu_varena_alloc(sizeof(Vertex) * fnverts);
#else
            Vertex *fvt = (Vertex *)sceGuGetMemory(sizeof(Vertex) * fnverts);
#endif
            size_t o = 0;
            for (size_t t = 0; t < ntris_all; t++) {
                const size_t k0 = 3 * t;
                if (!(scegu_fog_alpha[k0] | scegu_fog_alpha[k0 + 1] | scegu_fog_alpha[k0 + 2])) continue;
                for (size_t k = k0; k < k0 + 3; k++, o++) {
                    unsigned int a = (scegu_fog_alpha[k] * (src[k].color >> 24)) / 255; /* alpha_vertice * F */
                    fvt[o].u = src[k].u; fvt[o].v = src[k].v;
                    fvt[o].color = (a << 24) | rgb;
                    fvt[o].x = src[k].x; fvt[o].y = src[k].y; fvt[o].z = src[k].z;
                }
            }
#if PSP_VERTEX_ARENA && !PSP_WB_DEFER
            sceKernelDcacheWritebackRange(fvt, sizeof(Vertex) * fnverts);
#endif
            fbuf = fvt;
        } else {
#if PSP_VERTEX_ARENA
            VertexFog *fv = (VertexFog *)gfx_scegu_varena_alloc(sizeof(VertexFog) * fnverts);
#else
            VertexFog *fv = (VertexFog *)sceGuGetMemory(sizeof(VertexFog) * fnverts);
#endif
            size_t o = 0;
            for (size_t t = 0; t < ntris_all; t++) {
                const size_t k0 = 3 * t;
                if (!(scegu_fog_alpha[k0] | scegu_fog_alpha[k0 + 1] | scegu_fog_alpha[k0 + 2])) continue;
                for (size_t k = k0; k < k0 + 3; k++, o++) {
                    unsigned int a = scegu_fog_alpha[k];
                    if (fogmode == 3) a = (a * (src[k].color >> 24)) / 255;
                    fv[o].color = (a << 24) | rgb;
                    fv[o].x = src[k].x; fv[o].y = src[k].y; fv[o].z = src[k].z;
                }
            }
#if PSP_VERTEX_ARENA && !PSP_WB_DEFER
            sceKernelDcacheWritebackRange(fv, sizeof(VertexFog) * fnverts);
#endif
            fbuf = fv;
        }

        if (tex_alpha_pass) {
            sceGuTexEnvColor(scegu_fog_color);
            sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
            sceGuEnable(GU_TEXTURE_2D);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
        sceGuEnable(GU_BLEND);
        if (fogmode == 3) {
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF); /* dest += niebla * (alpha*F) */
        } else {
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        }
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
        sceGuDepthMask(GU_TRUE);
        if (depth_equal) sceGuDepthFunc(GU_EQUAL);

        gfx_scegu_prim_tris((tex_alpha_pass ? GU_TEXTURE_32BITF : 0) | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
                            (int)fnverts, fbuf);

        /* restaurar el estado de la 1ra pasada */
        if (depth_equal) sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthMask(sDepthMaskOn ? GU_TRUE : GU_FALSE);
        sceGuAlphaFunc(GU_GREATER, 0x55, 0xff);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        if (!gl_blend) sceGuDisable(GU_BLEND);
        if (tex_alpha_pass) {
            sceGuTexEnvColor(0xffffffff);
            sceGuTexFunc(cur_shader->texfunc, GU_TCC_RGBA);
        }
        if (textured) sceGuEnable(GU_TEXTURE_2D); else sceGuDisable(GU_TEXTURE_2D);
    }
}

static void gfx_scegu_draw_triangles_impl(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!PSP_GPUP()) {
        gfx_scegu_draw_triangles_real(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        return;
    }
    sGpupOpen = 0;
    sGpupBucket = -1;
    gfx_scegu_draw_triangles_real(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
    gpup_end(); /* cierra la medicion que quedara abierta (ultima pasada) */
}

static void gfx_scegu_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    gfx_scegu_draw_triangles_impl(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
    if (cur_shader && (cur_shader->shader_id & (1u << 28)) && scegu_enva_modulate) {
        sceGuTexFunc(cur_shader->texfunc, GU_TCC_RGBA); /* restaurar el modo del shader */
    }
}

void gfx_scegu_draw_triangles_2d(float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 2);
    void *quad_buf = sceGuGetMemory(sizeof(VertexColor) * 2);
    memcpy(quad_buf, buf_vbo, sizeof(VertexColor) * 2);
    if (PSP_GPUP()) {
        sGpupBucket = 9; sGpupTris = 1; sGpupPx = 0;
        gpup_begin();
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, quad_buf);
    if (PSP_GPUP()) {
        gpup_end();
    }
}

/* Cuadrilatero 2D (tira de 4 vertices) para G_TEXRECTFLIP; ver gfx_sp_quad_flip_2d() en gfx_pc.c */
void gfx_scegu_draw_quad_2d(float buf_vbo[]) {
    if (!is_shader_enabled(cur_shader->shader_id)) {
        gfx_scegu_apply_shader(get_shader_from_id(get_shader_remap(cur_shader->shader_id)));
    }

    gfx_scegu_reserve_list_memory(sizeof(VertexColor) * 4);
    void *quad_buf = sceGuGetMemory(sizeof(VertexColor) * 4);
    memcpy(quad_buf, buf_vbo, sizeof(VertexColor) * 4);
    if (PSP_GPUP()) {
        sGpupBucket = 9; sGpupTris = 2; sGpupPx = 0;
        gpup_begin();
    }
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 4, 0, quad_buf);
    if (PSP_GPUP()) {
        gpup_end();
    }
}

#ifndef PSP_ASYNC_PRESENT
#define PSP_ASYNC_PRESENT 1
#endif
#if PSP_ASYNC_PRESENT
static void gfx_scegu_present_init(void);
#endif

static void gfx_scegu_init(void) {
    sceGuInit();

    void *fbp0 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *fbp1 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650);
    void *zbp = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_4444);
    void *fbp2 = getStaticVramBuffer(BUF_WIDTH, SCR_HEIGHT, GU_PSM_5650); /* 3er buffer de color (ver "Presentacion" mas abajo) */

    sFb[0] = fbp0;
    sFb[1] = fbp1;
    sFb[2] = fbp2;
    sDrawBuffer = fbp0;
    sDisplayBuffer = fbp2; /* el que se muestra al arrancar; el cuadro 0 se dibuja en fbp0, el 1 en fbp1 y el 2 en fbp2 (cuando el 0 ya este en pantalla) */

    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_5650, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, fbp2, BUF_WIDTH);
    sceGuDepthBuffer(zbp, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2), SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0xffff, 0);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
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

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    void *texman_buffer = getStaticVramBufferBytes(TEXMAN_BUFFER_SIZE);
    void *texman_aligned = (void *) ((((unsigned int) texman_buffer + TEX_ALIGNMENT - 1) / TEX_ALIGNMENT) * TEX_ALIGNMENT);
    texman_reset(texman_aligned, TEXMAN_BUFFER_SIZE);
    if (!texman_buffer) {
        char msg[32];
        sprintf(msg, "OUT OF MEMORY!\n");
        sceIoWrite(1, msg, strlen(msg));

        sceKernelExitGame();
    }

    /* Load the firmware font before game assets consume the remaining heap. */
    gfx_scegu_ensure_home_menu_font();
#if PSP_ASYNC_PRESENT
    gfx_scegu_present_init();
#endif
}

/* ---- Grabar / repetir la escena (PSP_F2_REPLAY) ----
 * Cuadro 1 (rol 1, lo llama gfx_pc.c): despues del preambulo se anota donde empieza la escena en la lista A (rec_begin) y donde
 * acaba (rec_end). Esa parte de la lista es solo comandos del GE y punteros a memoria que no cambia hasta el tick siguiente
 * (arena de vertices, texturas en VRAM, datos metidos en la propia lista con sceGuGetMemory).
 * Cuadro 2 (rol 2): replay_ready() decide si se puede repetir y cambia el FINISH/END con que acabo la lista A por un RET; luego
 * start_frame abre la lista B y replay_scene() hace CALL a la escena. El GE ejecuta exactamente las mismas ordenes del cuadro 1.
 * Contadores (por ventana, los imprime gfx_pc.c): ver gPspRep*. */
unsigned int gPspRepOk, gPspRepNoRec, gPspRepRst, gPspRepTex, gPspRepBad, gPspRepNoise, gPspRepOff;
extern unsigned int gPspTexClears; /* psp_texture_manager.c */
static int sReplayArmed;           /* 1 = el cuadro que empieza usa la lista B y repite la escena */
#if PSP_F2_REPLAY
static int sRecState;              /* 0 = nada grabado, 1 = grabando, 2 = grabacion valida */
static unsigned int sRecBegin, sRecEnd, sRecGen, sRecTexGen, sRecNoise;
static int sRepDisabled;           /* se pone si la comprobacion de la lista falla varias veces seguidas */
static int sRepBadStreak;

#define GE_CMD_RET 0x0B000000u
#define GE_CMD_END 0x0C000000u
#define GE_CMD_FINISH 0x0F000000u
#endif

void gfx_scegu_rec_reset(void) {
#if PSP_F2_REPLAY
    sRecState = 0;
#endif
}

/* Estado propio de este backend que la escena grabada NO debe dar por supuesto (el del renderizador esta en gfx_pc.c). */
static __attribute__((unused)) void gfx_scegu_state_invalidate(void) {
    sGuFogColor = ~scegu_fog_color;
    sGuFogNear = __builtin_nanf(""); /* NaN: cualquier comparacion != es verdadera, asi que la 1a niebla envia sceGuFog */
    sGuFogFar = __builtin_nanf("");
    tmu_state[0].tex = UINT32_MAX;
    tmu_state[1].tex = UINT32_MAX;
}

/* Cuadro 1, justo antes de recorrer la lista del juego. Devuelve 1 si se graba. */
int gfx_scegu_rec_begin(void) {
#if PSP_F2_REPLAY
    if (sReplayArmed) {
        sRecState = 0;
        return 0;
    }
    gfx_scegu_state_invalidate();
    sRecBegin = (unsigned int) sceGuCheckList();
    sRecGen = sListGen;
    sRecTexGen = gPspTexClears;
    sRecNoise = sNoiseRegen;
    sRecState = 1;
    return 1;
#else
    return 0;
#endif
}

/* Cuadro 1, justo despues de la escena (antes de end_frame, que escribe FINISH y END). */
void gfx_scegu_rec_end(void) {
#if PSP_F2_REPLAY
    if (sRecState != 1) {
        return;
    }
    sRecEnd = (unsigned int) sceGuCheckList();
    sRecState = 2;
#endif
}

/* Cuadro 2, antes de abrir la lista: 1 si la escena del cuadro 1 se puede repetir tal cual. */
int gfx_scegu_replay_ready(void) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu;
    if (sRepDisabled) {
        gPspRepOff++;
        return 0;
    }
    if (sRecState != 2) {
        gPspRepNoRec++;
        return 0;
    }
    if (sListGen != sRecGen) {
        gPspRepRst++; /* se reinicio la lista (llena / arena llena): la escena grabada ya no esta entera */
        return 0;
    }
    if (gPspTexClears != sRecTexGen) {
        gPspRepTex++; /* se vacio el almacen de texturas durante el cuadro 1 */
        return 0;
    }
    if (sNoiseRegen - sRecNoise > 1U) {
        gPspRepNoise++; /* varias mascaras de ruido distintas en un cuadro: la lista solo conserva la ultima */
        return 0;
    }
    if (sRecEnd < sRecBegin || sRecEnd + 8U > sizeof(list) || ((sRecBegin | sRecEnd) & 3U) != 0U) {
        gPspRepBad++;
        return 0;
    }
    /* Comprobacion: end_frame del cuadro 1 cerro la lista con FINISH(0) y END justo donde acabo la escena. Si no es asi, la
     * cuenta de la posicion no es la que creo y NO se repite (tres fallos seguidos desactivan la funcion). */
    lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    if (lu[sRecEnd >> 2] != GE_CMD_FINISH || lu[(sRecEnd >> 2) + 1] != GE_CMD_END) {
        gPspRepBad++;
        if (++sRepBadStreak >= 3) {
            sRepDisabled = 1;
        }
        return 0;
    }
    sRepBadStreak = 0;
    lu[sRecEnd >> 2] = GE_CMD_RET; /* la escena acaba ahora en RET: vuelve a la lista que la llame */
    return 1;
#else
    return 0;
#endif
}

/* gfx_run, antes de gfx_rapi->start_frame(): on = 1 -> este cuadro usa la lista B y repite la escena; 0 -> lista A de siempre. */
void gfx_scegu_replay_arm(int on) {
#if PSP_F2_REPLAY
    if (on) {
        sCurList = listB;
        sCurListBytes = sizeof(listB);
        sReplayArmed = 1;
    } else {
        sCurList = list;
        sCurListBytes = sizeof(list);
        sReplayArmed = 0;
    }
#else
    (void) on;
#endif
}

/* Cuadro 2, despues de start_frame: mete la escena grabada en la lista B. */
void gfx_scegu_replay_scene(void) {
#if PSP_F2_REPLAY
    sceGuCallList((const void *) ((unsigned int) list + sRecBegin));
    gPspRepOk++;
    sRecState = 0; /* una grabacion se repite una sola vez */
#endif
}

/* ---- Etapa 2 (PSP_F2_REPLAY_MTX): reescribir matrices de modelo dentro de la escena grabada ----
 * sceGuSetMatrix(GU_MODEL, m) deja en la lista 13 palabras: WORLD_MATRIX_NUMBER(0) y 12 de WORLD_MATRIX_DATA con m[j + i*4]
 * (i = 0..3, j = 0..2) como (cmd << 24) | (bits_del_float >> 8). (Comprobado en pspsdk: sceGuSetMatrix.c y sendCommandf.) */
#define GE_CMD_WORLD_NUMBER 0x3Au
#define GE_CMD_WORLD_DATA 0x3Bu

unsigned int gfx_scegu_list_pos(void) {
    return (unsigned int) sceGuCheckList();
}

#if PSP_F2_REPLAY
static inline unsigned int ge_float_word(unsigned int cmd, float f) {
    union { float f; unsigned int i; } t;
    t.f = f;
    return (cmd << 24) | (t.i >> 8);
}
#endif

/* Durante la grabacion: comprueba que en la posicion off de la lista A hay de verdad un bloque de matriz de modelo. full != 0 ademas
 * compara las 12 palabras con las que saldrian de m (valida el orden); si no, solo el principio y el final del bloque. */
int gfx_scegu_mtx_block_ok(unsigned int off, int full, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu;
    int i, j;
    if ((off & 3U) != 0U || off + 13U * 4U > sizeof(list)) {
        return 0;
    }
    lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    lu += off >> 2;
    if (lu[0] != (GE_CMD_WORLD_NUMBER << 24)) {
        return 0;
    }
    if (full) {
        for (i = 0; i < 4; i++) {
            for (j = 0; j < 3; j++) {
                if (lu[1 + i * 3 + j] != ge_float_word(GE_CMD_WORLD_DATA, m[j + i * 4])) {
                    return 0;
                }
            }
        }
        return 1;
    }
    return (lu[12] >> 24) == GE_CMD_WORLD_DATA;
#else
    (void) off; (void) full; (void) m;
    return 0;
#endif
}

/* Cuadro 2, antes de abrir la lista: pone en el bloque de la lista A (desplazamiento off) la matriz m (16 floats, orden de Mtx). */
void gfx_scegu_write_model(unsigned int off, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    int i, j;
    lu += off >> 2;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            lu[1 + i * 3 + j] = ge_float_word(GE_CMD_WORLD_DATA, m[j + i * 4]);
        }
    }
#else
    (void) off; (void) m;
#endif
}

/* Igual que lo anterior para sceGuSetMatrix(GU_VIEW, m): 13 palabras = VIEW_MATRIX_NUMBER(0) + 12 de VIEW_MATRIX_DATA con m[j + i*4]
 * (i = 0..3, j = 0..2). El cielo lleva su matriz ortografica ahi (el juego la multiplica con G_MTX_MUL sobre la proyeccion). */
#define GE_CMD_VIEW_NUMBER 0x3Cu
#define GE_CMD_VIEW_DATA 0x3Du

/* Durante la grabacion: 1 si en la posicion off de la lista A hay exactamente el bloque GU_VIEW de m. */
int gfx_scegu_view_block_ok(unsigned int off, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu;
    int i, j;
    if ((off & 3U) != 0U || off + 13U * 4U > sizeof(list)) {
        return 0;
    }
    lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    lu += off >> 2;
    if (lu[0] != (GE_CMD_VIEW_NUMBER << 24)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            if (lu[1 + i * 3 + j] != ge_float_word(GE_CMD_VIEW_DATA, m[j + i * 4])) {
                return 0;
            }
        }
    }
    return 1;
#else
    (void) off; (void) m;
    return 0;
#endif
}

/* Cuadro 2, antes de abrir la lista: pone en el bloque GU_VIEW de la lista A (desplazamiento off) la matriz m. */
void gfx_scegu_write_view(unsigned int off, const float *m) {
#if PSP_F2_REPLAY
    volatile unsigned int *lu = (volatile unsigned int *) ((unsigned int) list | 0x40000000U);
    int i, j;
    lu += off >> 2;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 3; j++) {
            lu[1 + i * 3 + j] = ge_float_word(GE_CMD_VIEW_DATA, m[j + i * 4]);
        }
    }
#else
    (void) off; (void) m;
#endif
}

/* ---- Presentacion: anillo de 3 buffers de color + hilo que espera al GE y al vblank ----
 * Antes la CPU no podia empezar el cuadro siguiente hasta que el anterior pasaba a pantalla (esperaba al vblank con el procesador parado), asi que
 * cada cuadro costaba un NUMERO ENTERO de vblanks: uno que necesita 17 ms de CPU tardaba 33 (30 fps) y el juego iba a 40 en vez de a 58.
 * Ahora hay 3 buffers de color. Un hilo de prioridad alta presenta los cuadros en orden (espera a que el GE termine ese cuadro, espera al vblank y
 * pone el buffer en pantalla) y la CPU ya puede armar el cuadro siguiente en otro buffer mientras tanto. La lista de comandos y la arena de
 * vertices siguen siendo una sola: el cuadro siguiente no empieza hasta que el GE termino el anterior (suele ser casi nada de espera), asi que ninguna
 * memoria que lee el GE se pisa. Lo dibujado es identico.
 * gPspOptPipe = 0 ("Triple Buffer: Off"): se espera a que el cuadro anterior este en pantalla antes de empezar el siguiente, igual que antes.
 * gPspOptPipe = 1: se puede empezar con UN cuadro anterior todavia sin presentar (la imagen puede llegar un cuadro mas tarde si la CPU va sobrada). */
extern int gPspOptPipe;         /* game/psp_options.c */

static unsigned int sFrameN;          /* cuadros ya cerrados con end_frame (el que se arma ahora es el numero sFrameN) */
static int sPrevList = -1;            /* id de la lista del cuadro anterior */
static int sPrevAsync;                /* el cuadro anterior se mando sin esperar a que llegara a pantalla */
struct PresItem {
    void *fb;
    int list;
    unsigned int t_submit;
};
static struct PresItem sPresQ[4];
static volatile unsigned int sPresHead, sPresTail; /* cola de cuadros por presentar (la llena el hilo principal, la vacia el hilo de presentacion) */
static volatile unsigned int sShown;               /* cuadros que ya estan puestos en pantalla */
static SceUID sPresThread = -1, sPresReq = -1, sPresDone = -1;
static volatile unsigned int sPresSyncUs, sPresVblUs;
volatile int gPspSuspending;          /* 1 entre el aviso de suspension y el de reanudacion: se presenta de forma sincrona */
static int sPipeBroken;               /* un semaforo vencio (no deberia pasar): se presenta de forma sincrona y sin hilo */

/* Medicion (microsegundos): espera al GPU y espera al vblank dentro de end_frame. Las lee gfx_pc.c. */
unsigned int gPspProfSync, gPspProfVblank;
/* hora (us) del ultimo vblank en que se presento un cuadro, y del anterior (pc_main.c los usa para saber si un cuadro se paso de un vblank) */
unsigned int gPspLastSwapT, gPspPrevSwapT;
int gPspPresentAsync = 0;            /* lo pone pc_main.c: el cuadro que se esta enviando no espera a llegar a pantalla */
unsigned int gPspProfPendWait = 0;   /* us que start_frame espero (buffer libre / GE del cuadro anterior) */
unsigned int gPspAsyncSync = 0;      /* de la ultima presentacion: us desde end_frame hasta que el GE termino */
unsigned int gPspAsyncVbl = 0;       /*                            us esperando al vblank */
int gPspPendingDone = 0;             /* 1 si el cuadro anterior se mando sin esperar (start_frame mide cuanto espero ahora) */
unsigned int gPspGateUs, gPspGateN, gPspQMax; /* espera acumulada de start_frame, cuadros y mayor cola vista en la ventana (los imprime gfx_pc.c) */
int gPspPipeBroken;                  /* copia publica de sPipeBroken para el registro */

/* Pone en pantalla un cuadro de la cola (espera a que el GE termine esa lista, espera al vblank y cambia el buffer). */
static void gfx_scegu_present_item(const struct PresItem *it) {
    unsigned int t1, t2;
    if (it->list >= 0) {
        sceGeListSync(it->list, 0); /* termino el GE de ese cuadro (si el id ya no existe vuelve enseguida) */
    }
    t1 = sceKernelGetSystemTimeLow();
    sceDisplayWaitVblankStart();
    t2 = sceKernelGetSystemTimeLow();
    sceDisplaySetFrameBuf((void *) ((unsigned int) sceGeEdramGetAddr() + (unsigned int) it->fb), BUF_WIDTH, GU_PSM_5650,
                          PSP_DISPLAY_SETBUF_NEXTHSYNC);
    sDisplayBuffer = it->fb;
    gPspPrevSwapT = gPspLastSwapT;
    gPspLastSwapT = t2;
    sPresSyncUs = t1 - it->t_submit;
    sPresVblUs = t2 - t1;
    sShown++;
}

/* Presenta aqui (en el hilo que llama) todo lo que haya en la cola. Solo se usa si el hilo de presentacion no existe o dejo de responder. */
static void gfx_scegu_present_drain(void) {
    while ((int) (sPresTail - sPresHead) > 0) {
        const struct PresItem it = sPresQ[sPresHead & 3];
        sPresHead++;
        gfx_scegu_present_item(&it);
    }
}

#if PSP_ASYNC_PRESENT
static int gfx_scegu_present_thread(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    for (;;) {
        if (sceKernelWaitSema(sPresReq, 1, NULL) < 0) {
            break;
        }
        if ((int) (sPresTail - sPresHead) > 0 && !sPipeBroken) {
            const struct PresItem it = sPresQ[sPresHead & 3];
            sPresHead++;
            gfx_scegu_present_item(&it);
        }
        sceKernelSignalSema(sPresDone, 1);
    }
    return 0;
}

static void gfx_scegu_present_init(void) {
    sPresReq = sceKernelCreateSema("sm64_pres_req", 0, 0, 8, NULL);
    sPresDone = sceKernelCreateSema("sm64_pres_done", 0, 0, 8, NULL);
    if (sPresReq < 0 || sPresDone < 0) {
        return;
    }
    sPresThread = sceKernelCreateThread("sm64_present", gfx_scegu_present_thread, 0x1D, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (sPresThread < 0 || sceKernelStartThread(sPresThread, 0, NULL) < 0) {
        sPresThread = -1;
    }
}
#endif

/* Espera a que haya `need` cuadros (o mas) en pantalla. Devuelve los us esperados. */
static unsigned int gfx_scegu_wait_shown(unsigned int need) {
    unsigned int t0;
    if ((int) (sShown - need) >= 0) {
        return 0;
    }
    t0 = sceKernelGetSystemTimeLow();
    while ((int) (sShown - need) < 0) {
        SceUInt timeout = 2000000; /* 2 s: si el hilo no responde no se congela el juego */
        if (sPresThread < 0 || sPipeBroken) {
            gfx_scegu_present_drain();
            break;
        }
        if (sceKernelWaitSema(sPresDone, 1, &timeout) < 0) {
            sPipeBroken = 1;
            gPspPipeBroken = 1;
            gfx_scegu_present_drain();
            break;
        }
    }
    return sceKernelGetSystemTimeLow() - t0;
}

/* Antes de abrir la lista de un cuadro: hay un buffer de color libre y el GE ya termino el cuadro anterior (la lista y la arena de vertices se
 * reutilizan). Con la opcion apagada se espera ademas a que el anterior este en pantalla (comportamiento de siempre). */
static void gfx_scegu_frame_gate(void) {
    const unsigned int t0 = sceKernelGetSystemTimeLow();
    unsigned int need = sFrameN; /* todos los anteriores en pantalla */
    unsigned int qd;
    if (gPspOptPipe && !gPspSuspending && sFrameN > 0 && sPresThread >= 0 && !sPipeBroken) {
        need = sFrameN - 1; /* el anterior puede seguir en cola: el buffer de este es el de hace 3 cuadros, que ya se cambio */
    }
    qd = sPresTail - sPresHead;
    if (qd > 3) qd = 3;
    if (qd > gPspQMax) gPspQMax = qd;
    gfx_scegu_wait_shown(need);
    if (sFrameN > 0 && sPrevList >= 0) {
        sceGeListSync(sPrevList, 0);
    }
    gPspProfPendWait = sceKernelGetSystemTimeLow() - t0;
    gPspAsyncSync = sPresSyncUs;
    gPspAsyncVbl = sPresVblUs;
    gPspPendingDone = sPrevAsync ? 1 : 0;
    gPspGateUs += gPspProfPendWait;
    gPspGateN++;
}

/* Espera a que el GE termine TODO lo enviado hasta ahora y sigue en una lista nueva (el estado del GE se conserva). La usa gfx_pc.c justo antes de
 * vaciar la VRAM de texturas: las texturas nuevas se escriben donde estaban las viejas y el GE podria estar leyendolas todavia. */
void gfx_scegu_sync_all(void) {
    gfx_scegu_list_restart();
}

/* Aviso de energia (lo llama el callback de pc_main.c, que corre en otro hilo). Al suspender: se deja de encolar cuadros, se espera (maximo ~0,3 s) a que
 * los que hay lleguen a pantalla y a que el GE quede libre, para que la consola no se duerma con una lista a medias o con el hilo de presentacion en
 * pleno cambio de buffer. Al reanudar se vuelve a lo normal. */
void gfx_scegu_power_event(int suspending) {
    if (suspending) {
        int i;
        gPspSuspending = 1;
        for (i = 0; i < 300 && (int) (sFrameN - sShown) > 0; i++) {
            sceKernelDelayThread(1000);
        }
        sceGeDrawSync(0);
    } else {
        gPspSuspending = 0;
    }
}

static void gfx_scegu_start_frame(void) {
    bool has_home_menu_background;
    gfx_scegu_frame_gate(); /* el cuadro anterior puede seguir en el GE / sin presentar: nada de tocar lista / arena / buffers antes */
    sNoiseFrame++;

    /* Callback rendering (intraFont in particular) changes GU texture state
     * outside the normal Fast3D cache. Rebind the first texture used by every
     * frame so resuming from the custom HOME menu cannot inherit that state. */
    tmu_state[0].tex = UINT32_MAX;
    tmu_state[1].tex = UINT32_MAX;

    if (sHomeMenuBgCaptureRequested) {
        gfx_scegu_wait_shown(sFrameN); /* lo que hay en pantalla tiene que ser el ultimo cuadro completo */
        gfx_scegu_copy_framebuffer_from_vram(sHomeMenuBgBuffer, sDisplayBuffer);
        gfx_scegu_blur_framebuffer_565(sHomeMenuBgBuffer);
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = true;
    }

    sDrawBuffer = sFb[sFrameN % PSP_NFB];
    has_home_menu_background = sHomeMenuBgActive && sHomeMenuBgCaptured;
    if (has_home_menu_background) {
        gfx_scegu_copy_framebuffer_to_vram(sDrawBuffer, sHomeMenuBgBuffer);
    }

    sceGuStart(GU_DIRECT, sCurList);
    sFrameList = ge_list_executed[0];
    sceGuDrawBuffer(GU_PSM_5650, sDrawBuffer, BUF_WIDTH); /* sceGuStart puso el buffer del cuadro anterior: este cuadro se dibuja en otro */
    if (!sReplayArmed) {
        sVArenaUsed = 0; /* el cuadro anterior ya termino (end_frame espera al GE): la arena de vertices se reutiliza */
        sVArenaWb = 0;
    }
    /* (en un cuadro que repite la escena grabada NO se toca la arena: sus vertices los lee el GE al ejecutar la escena) */
    if (PSP_GPUP()) {
        gpup_calibrate();
    }
    sceGuDisable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_TRUE); // Must be set to clear Z-buffer
    sceGuClearDepth(0);
    if (has_home_menu_background) {
        sceGuClear(GU_DEPTH_BUFFER_BIT);
    } else {
        sceGuClearColor(0xFF000000);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    }
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthMask(GU_FALSE);

    // Identity every frame? unsure.
    //sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *) identity_matrix);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *) identity_matrix);
    //sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *) identity_matrix);

#if 0
    const int DitherMatrix[2][16] = { { 0, 8, 0, 8,
                         8, 0, 8, 0,
                         0, 8, 0, 8,
                         8, 0, 8, 0 },
                        { 8, 8, 8, 8,
                          0, 8, 0, 8,
                          8, 8, 8, 8,
                          0, 8, 0, 8 } };

    extern int gDoDither;
    extern int gFrame;

    sceGuDisable(GU_DITHER);
    if(gDoDither){
        // every frame
        sceGuSetDither((const ScePspIMatrix4 *)DitherMatrix[(gFrame&1)]);
        sceGuEnable(GU_DITHER);
    }
#endif
}

void gfx_scegu_on_resize(void) {
}

static void gfx_scegu_end_frame(void) {
    const int async = (gPspPresentAsync != 0 && !gPspSuspending && sPresThread >= 0 && !sPipeBroken) ? 1 : 0;
    struct PresItem *it;

    sceGuFinish();
    it = &sPresQ[sPresTail & 3];
    it->fb = sDrawBuffer;
    it->list = sFrameList;
    it->t_submit = sceKernelGetSystemTimeLow();
    sPresTail++;
    sPrevList = sFrameList;
    sPrevAsync = async;
    sFrameN++;
    if (sPresThread >= 0 && !sPipeBroken) {
        sceKernelSignalSema(sPresReq, 1);
        if (async) {
            /* el hilo espera al GE y al vblank y cambia el buffer; la CPU sigue con lo suyo */
            gPspProfSync = 0;
            gPspProfVblank = 0;
            return;
        }
        gfx_scegu_wait_shown(sFrameN);
    } else {
        gfx_scegu_present_drain();
    }
    gPspPresentAsync = 0; /* esta vez se presento de forma sincrona (asi gfx_pc.c / gfx_psp.c lo saben) */
    gPspProfSync = sPresSyncUs;
    gPspProfVblank = sPresVblUs;
}

static void gfx_scegu_finish_render(void) {
    /* There should be something here! */
}

// clang-format off
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

void gfx_scegu_request_home_menu_background(void) {
    sHomeMenuBgCaptureRequested = true;
}

void gfx_scegu_set_home_menu_background_active(bool active) {
    sHomeMenuBgActive = active;
    if (!active) {
        sHomeMenuBgCaptureRequested = false;
        sHomeMenuBgCaptured = false;
    }
}

void gfx_scegu_render_home_menu(int selected_index, int screen, int control_selected_index,
                                const char *status_message, uint8_t highlight_red,
                                uint8_t highlight_green, uint8_t highlight_blue) {
    gfx_scegu_prepare_home_menu_draw();
    if (screen == 1) {
        gfx_scegu_render_controller_mapping(control_selected_index, status_message, highlight_red,
                                             highlight_green, highlight_blue);
    } else {
        gfx_scegu_render_home_menu_main(selected_index, highlight_red, highlight_green,
                                        highlight_blue);
    }
}

#endif // RAPI_GL_LEGACY
