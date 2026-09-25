/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * ponytail: fixed-function and pre-transformed batches only; vertex-program
 * batches are still skipped (the executor does not run programs yet), and
 * blend/depth/combiner state is not passed on. Extend Nv2aBatch when those
 * land rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
} Nv2aBatch;

typedef struct {
    void (*clear)(const Nv2aSurface *s, uint32_t argb,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

#endif /* NV2A_BACKEND_H */
