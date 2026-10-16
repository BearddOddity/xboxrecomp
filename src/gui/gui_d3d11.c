/*
 * Direct3D 11 renderer for gui.h. See gui_d3d11.h.
 *
 * One pipeline: a 20-byte vertex (position, atlas UV, RGBA8), one shader pair
 * compiled at start-up, alpha blending, a scissor per draw command. The font
 * atlas is single-channel coverage; untextured shapes sample its solid block.
 */
#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#include "gui_d3d11.h"

static ID3D11Device             *s_dev;
static ID3D11DeviceContext      *s_ctx;
static ID3D11VertexShader       *s_vs;
static ID3D11PixelShader        *s_ps;
static ID3D11InputLayout        *s_layout;
static ID3D11Buffer             *s_cb, *s_vb, *s_ib;
static UINT                      s_vb_cap, s_ib_cap;
static ID3D11ShaderResourceView *s_font;
static ID3D11SamplerState       *s_samp;
static ID3D11BlendState         *s_blend;
static ID3D11RasterizerState    *s_raster;
static ID3D11DepthStencilState  *s_depth;

static const char s_hlsl[] =
    "cbuffer xf : register(b0) { float4 xf; };\n"
    "struct vin { float2 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };\n"
    "struct pin { float4 p : SV_POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
    "pin vs(vin i) { pin o; o.p = float4(i.p * xf.xy + xf.zw, 0, 1); o.c = i.c; o.uv = i.uv; return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(pin i) : SV_Target { return i.c * float4(1, 1, 1, t.Sample(s, i.uv).r); }\n";

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

void gui_d3d11_shutdown(void)
{
    RELEASE(s_vs); RELEASE(s_ps); RELEASE(s_layout);
    RELEASE(s_cb); RELEASE(s_vb); RELEASE(s_ib);
    RELEASE(s_font); RELEASE(s_samp);
    RELEASE(s_blend); RELEASE(s_raster); RELEASE(s_depth);
    s_vb_cap = s_ib_cap = 0;
    s_dev = NULL;
    s_ctx = NULL;
}

int gui_d3d11_init(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_INPUT_ELEMENT_DESC el[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_SAMPLER_DESC smp;
    D3D11_BLEND_DESC bl;
    D3D11_RASTERIZER_DESC rs;
    D3D11_DEPTH_STENCIL_DESC ds;
    ID3D11Texture2D *tex = NULL;
    const uint8_t *pixels;
    int w, h;
    HRESULT hr;

    if (s_dev == dev && s_vs)
        return 1;
    gui_d3d11_shutdown();
    if (!dev || !ctx || !gui_ready())
        return 0;
    s_dev = dev;
    s_ctx = ctx;

    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gui", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gui", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GUI] shader compile failed: %s\n", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        goto fail;
    }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(dev, el, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &s_layout);
    if (FAILED(hr)) goto fail;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &s_cb))) goto fail;

    gui_font_atlas(&pixels, &w, &h);
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = pixels;
    sd.SysMemPitch = (UINT)w;
    sd.SysMemSlicePitch = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, &sd, &tex))) goto fail;
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &s_font);
    ID3D11Texture2D_Release(tex);
    if (FAILED(hr)) goto fail;

    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(ID3D11Device_CreateSamplerState(dev, &smp, &s_samp))) goto fail;

    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(ID3D11Device_CreateBlendState(dev, &bl, &s_blend))) goto fail;

    memset(&rs, 0, sizeof rs);
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.ScissorEnable = TRUE;
    rs.DepthClipEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(dev, &rs, &s_raster))) goto fail;

    memset(&ds, 0, sizeof ds);
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    ds.BackFace = ds.FrontFace;
    if (FAILED(ID3D11Device_CreateDepthStencilState(dev, &ds, &s_depth))) goto fail;

    RELEASE(vsb); RELEASE(psb); RELEASE(err);
    return 1;
fail:
    RELEASE(vsb); RELEASE(psb); RELEASE(err);
    gui_d3d11_shutdown();
    return 0;
}

static int ensure_buffer(ID3D11Buffer **b, UINT *cap, UINT need, UINT bind)
{
    D3D11_BUFFER_DESC bd;
    if (*b && *cap >= need)
        return 1;
    RELEASE(*b);
    *cap = need + need / 2 + 4096;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = *cap;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, b));
}

static int upload(ID3D11Buffer *b, const void *src, size_t n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return 0;
    memcpy(m.pData, src, n);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)b, 0);
    return 1;
}

/* What the caller had bound, put back after drawing. */
typedef struct {
    UINT                      n_scissor, n_vp;
    D3D11_RECT                scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_VIEWPORT            vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState    *rs;
    ID3D11BlendState         *blend;
    FLOAT                     blend_factor[4];
    UINT                      sample_mask, stencil_ref;
    ID3D11DepthStencilState  *depth;
    ID3D11ShaderResourceView *ps_srv;
    ID3D11SamplerState       *ps_samp;
    ID3D11PixelShader        *ps;
    ID3D11VertexShader       *vs;
    ID3D11GeometryShader     *gs;
    ID3D11Buffer             *vs_cb;
    D3D11_PRIMITIVE_TOPOLOGY  topo;
    ID3D11Buffer             *ib, *vb;
    UINT                      ib_off, vb_stride, vb_off;
    DXGI_FORMAT               ib_fmt;
    ID3D11InputLayout        *layout;
    ID3D11RenderTargetView   *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView   *dsv;
} Backup;

static void backup(Backup *b)
{
    ID3D11DeviceContext *c = s_ctx;
    b->n_scissor = b->n_vp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetScissorRects(c, &b->n_scissor, b->scissor);
    ID3D11DeviceContext_RSGetViewports(c, &b->n_vp, b->vp);
    ID3D11DeviceContext_RSGetState(c, &b->rs);
    ID3D11DeviceContext_OMGetBlendState(c, &b->blend, b->blend_factor, &b->sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &b->depth, &b->stencil_ref);
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b->rtv, &b->dsv);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &b->ps_srv);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 1, &b->ps_samp);
    ID3D11DeviceContext_PSGetShader(c, &b->ps, NULL, NULL);
    ID3D11DeviceContext_VSGetShader(c, &b->vs, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &b->gs, NULL, NULL);
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 1, &b->vs_cb);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &b->topo);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &b->ib, &b->ib_fmt, &b->ib_off);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &b->vb, &b->vb_stride, &b->vb_off);
    ID3D11DeviceContext_IAGetInputLayout(c, &b->layout);
}

static void restore(Backup *b)
{
    ID3D11DeviceContext *c = s_ctx;
    UINT i;
    ID3D11DeviceContext_RSSetScissorRects(c, b->n_scissor, b->scissor);
    ID3D11DeviceContext_RSSetViewports(c, b->n_vp, b->vp);
    ID3D11DeviceContext_RSSetState(c, b->rs); RELEASE(b->rs);
    ID3D11DeviceContext_OMSetBlendState(c, b->blend, b->blend_factor, b->sample_mask); RELEASE(b->blend);
    ID3D11DeviceContext_OMSetDepthStencilState(c, b->depth, b->stencil_ref); RELEASE(b->depth);
    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b->rtv, b->dsv);
    for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) RELEASE(b->rtv[i]);
    RELEASE(b->dsv);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &b->ps_srv); RELEASE(b->ps_srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &b->ps_samp); RELEASE(b->ps_samp);
    ID3D11DeviceContext_PSSetShader(c, b->ps, NULL, 0); RELEASE(b->ps);
    ID3D11DeviceContext_VSSetShader(c, b->vs, NULL, 0); RELEASE(b->vs);
    ID3D11DeviceContext_GSSetShader(c, b->gs, NULL, 0); RELEASE(b->gs);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &b->vs_cb); RELEASE(b->vs_cb);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, b->topo);
    ID3D11DeviceContext_IASetIndexBuffer(c, b->ib, b->ib_fmt, b->ib_off); RELEASE(b->ib);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &b->vb, &b->vb_stride, &b->vb_off); RELEASE(b->vb);
    ID3D11DeviceContext_IASetInputLayout(c, b->layout); RELEASE(b->layout);
}

void gui_d3d11_render(const GuiDrawData *dd, ID3D11RenderTargetView *rtv)
{
    ID3D11DeviceContext *c = s_ctx;
    D3D11_VIEWPORT vp;
    float xf[4];
    UINT stride = sizeof(GuiVertex), off = 0;
    Backup b;
    int i;

    if (!s_vs || !dd || !rtv || dd->cmd_count <= 0 || dd->display_w <= 0.0f || dd->display_h <= 0.0f)
        return;
    if (!ensure_buffer(&s_vb, &s_vb_cap, (UINT)(dd->vtx_count * (int)sizeof(GuiVertex)), D3D11_BIND_VERTEX_BUFFER)
            || !ensure_buffer(&s_ib, &s_ib_cap, (UINT)(dd->idx_count * (int)sizeof(uint32_t)), D3D11_BIND_INDEX_BUFFER))
        return;
    if (!upload(s_vb, dd->vtx, sizeof(GuiVertex) * (size_t)dd->vtx_count)
            || !upload(s_ib, dd->idx, sizeof(uint32_t) * (size_t)dd->idx_count))
        return;
    xf[0] = 2.0f / dd->display_w;
    xf[1] = -2.0f / dd->display_h;
    xf[2] = -1.0f;
    xf[3] = 1.0f;
    if (!upload(s_cb, xf, sizeof xf))
        return;

    backup(&b);
    memset(&vp, 0, sizeof vp);
    vp.Width = dd->display_w;
    vp.Height = dd->display_h;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(c, s_layout);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, 1, &s_vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(c, s_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, 1, &s_cb);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &s_font);
    ID3D11DeviceContext_PSSetSamplers(c, 0, 1, &s_samp);
    ID3D11DeviceContext_OMSetBlendState(c, s_blend, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s_depth, 0);
    ID3D11DeviceContext_RSSetState(c, s_raster);

    for (i = 0; i < dd->cmd_count; i++) {
        const GuiDrawCmd *cmd = &dd->cmd[i];
        D3D11_RECT r;
        r.left = (LONG)(cmd->clip[0] < 0.0f ? 0.0f : cmd->clip[0]);
        r.top = (LONG)(cmd->clip[1] < 0.0f ? 0.0f : cmd->clip[1]);
        r.right = (LONG)(cmd->clip[2] > dd->display_w ? dd->display_w : cmd->clip[2]);
        r.bottom = (LONG)(cmd->clip[3] > dd->display_h ? dd->display_h : cmd->clip[3]);
        if (r.right <= r.left || r.bottom <= r.top)
            continue;
        ID3D11DeviceContext_RSSetScissorRects(c, 1, &r);
        ID3D11DeviceContext_DrawIndexed(c, cmd->idx_count, cmd->idx_offset, 0);
    }
    restore(&b);
}
