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
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * A back end that draws "texture x diffuse" sees stage 0 and the diffuse
 * colour. One that emulates the pixel pipeline itself (all four texture stages
 * and the register combiners) sets NV2A_BACKEND_WANT_PIPELINE in
 * Nv2aBackend.flags and then also receives Nv2aBatch.ex / stages / comb. The
 * executor builds that data only for such a back end, so every other path is
 * unchanged. Extend Nv2aBatch rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

#include "nv2a_combiner.h"      /* Nv2aCombiner: the pixel pipeline's registers */

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
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    /* Palettised formats (P8, colour code 0x0B): guest address of the palette
     * (A8R8G8B8 entries) and how many entries it holds; 0 for every other
     * format. A back end that ignores them cannot draw a P8 texture, and the
     * executor's own sampler already reads the palette. Appended last, so a
     * back end built against the older struct is unaffected. */
    uint32_t palette, palette_len;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
} Nv2aRenderState;

/* What the NV2A's pixel pipeline sees per vertex, for a back end that emulates
 * the register combiners rather than drawing texture x colour. Parallel to
 * Nv2aBatch.vertices. d0/d1 are clamped to 0..1 (d1 is 0,0,0,1 with specular
 * off); fog is the fog factor before the pixel stage clamps it (1 = no fog);
 * t[] are the texture coordinates s t r q per stage, with s and t already
 * divided by the texture size for a linear (texel-addressed) texture, so a
 * back end samples t.xy / t.w. */
typedef struct {
    float d0[4], d1[4];
    float fog;
    float t[4][4];
} Nv2aVertexEx;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    /* Only with NV2A_BACKEND_WANT_PIPELINE (NULL otherwise). Appended last. */
    const Nv2aVertexEx *ex;         /* per vertex, parallel to vertices */
    const Nv2aTexture  *stages[4];  /* texture per stage; NULL = none bound */
    const Nv2aCombiner *comb;       /* the pixel pipeline's registers */
} Nv2aBatch;

/* A vertex-program batch handed over untransformed, for a back end that runs
 * the program on the GPU (NV2A_BACKEND_GPU_VERTEX_PROGRAMS + Nv2aBackend.draw_vp).
 * The executor only gathers the program's inputs:
 *
 *   attrs: vertex_count vertices, each the inputs v[i] for every bit i set in
 *          attr_mask, as float4, packed in ascending i (so the stride is
 *          16 * popcount(attr_mask) bytes). Component conversion (D3DCOLOR,
 *          shorts, current values for absent arrays) is already done.
 *   indices: index_count (a multiple of 3) triangle-list indices into attrs;
 *          strips, fans and quads are already expanded.
 *   program: program_len microcode slots (4 dwords each) from the start slot to
 *          the FINAL instruction, in the format xemu's vsh.c documents.
 *   constants: the 192 float4 constants c[0..191]. The program may write
 *          constants; the back end applies those writes to a private copy.
 *   *_gen: change whenever the program / the constants change, for caching
 *          compiled shaders and constant uploads.
 *
 * The program leaves oPos in surface pixels with w = clip w (the XDK tail does
 * the viewport scale and the perspective divide), so a back end scales x and y
 * by aa_sx / aa_sy, as for pre-transformed vertices. Fog is the fixed
 * pipeline's job, so its registers ride along; oD1 is used as the program
 * writes it, as the executor's own path does. */
typedef struct {
    uint32_t        vertex_count;
    const float    *attrs;
    uint32_t        index_count;
    const uint32_t *indices;
    uint32_t        attr_mask;
    uint32_t        program_len;
    const uint32_t (*program)[4];
    uint32_t        program_gen;
    const float   (*constants)[4];
    uint32_t        constants_gen;
    float           aa_sx, aa_sy;
    uint32_t        specular_on;                /* oD1 is used as written (else 0,0,0,1);
                                                   the executor reports 1, as its CPU path uses it */
    uint32_t        fog_enable, fog_mode;       /* SET_FOG_ENABLE / SET_FOG_MODE */
    float           fog_p0, fog_p1;             /* SET_FOG_PARAMS */
    float           tex_scale[4][2];            /* s,t multiplier per stage: 1/size for
                                                   texel-addressed (linear) textures, else 1 */
    const Nv2aRenderState *state;
    const Nv2aTexture *stages[4];
    const Nv2aCombiner *comb;
} Nv2aVpBatch;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* NV2A_BACKEND_* bits. Appended last: a back end whose initialiser stops
     * at flip leaves it 0, which asks for nothing. */
    uint32_t flags;
    /* Only with NV2A_BACKEND_GPU_VERTEX_PROGRAMS: vertex-program batches go
     * here instead of to draw(), untransformed. Leave NULL otherwise. */
    void (*draw_vp)(const Nv2aSurface *s, const Nv2aVpBatch *b);
} Nv2aBackend;

/* Nv2aBackend.flags bits (0: the historical behaviour). */

/* The back end clips triangles in homogeneous space, so a vertex-program or
 * fixed-function triangle that reaches behind the eye is handed over whole,
 * with the signed 1/w in Nv2aVertex.rhw (negative behind the eye), instead of
 * being clipped to the near plane by the executor. A vertex with w == 0 still
 * drops its triangle. The CPU rasteriser path is unchanged. */
#define NV2A_BACKEND_CLIPS_HOMOGENEOUS 0x1u

/* The back end runs vertex programs itself (draw_vp). The executor then skips
 * its CPU interpreter for vertex-program batches and hands the raw program
 * inputs over; Nv2aVpBatch carries the stage textures and combiner registers
 * itself. Needs a loaded program and indices below 64 K; anything else takes
 * the CPU route unchanged. RECOMP_GPU_VP=0 turns it off. */
#define NV2A_BACKEND_GPU_VERTEX_PROGRAMS 0x2u

/* The back end wants Nv2aBatch.ex / stages / comb. */
#define NV2A_BACKEND_WANT_PIPELINE     0x4u

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
