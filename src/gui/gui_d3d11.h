/*
 * Direct3D 11 renderer for gui.h draw data.
 *
 * Draws over whatever is in the render target, usually the back buffer just
 * before Present. Every piece of pipeline state it touches is saved first and
 * put back after, so the caller's own state caching stays correct.
 */
#ifndef RECOMP_GUI_D3D11_H
#define RECOMP_GUI_D3D11_H

#include <d3d11.h>
#include "gui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates shaders, states and the font texture (gui_init() must have run).
 * Returns 0 on failure. A second call with the same device does nothing. */
int  gui_d3d11_init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
void gui_d3d11_shutdown(void);
void gui_d3d11_render(const GuiDrawData *dd, ID3D11RenderTargetView *rtv);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_GUI_D3D11_H */
