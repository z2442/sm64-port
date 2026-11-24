#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stdint.h>

#include <PR/gbi.h>

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

#ifdef __cplusplus
extern "C" {
#endif

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen);
struct GfxRenderingAPI *gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_end_frame(void);
void gfx_render_callback_frame(void (*draw_callback)(void *arg), void *arg);

void gfx_texture_cache_forget(const void *addr);
#ifdef TARGET_PSP
int gfx_psp_sample_screen(uint16_t *out, int w, int h, int x0, int y0, int sampleW, int sampleH);
#endif

#ifdef __cplusplus
}
#endif

#endif
