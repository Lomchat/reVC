#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "orthros_web.h"

EM_ASYNC_JS(void, OrthrosFrameYield, (), {
	await new Promise((resolve) => requestAnimationFrame(resolve));
});

EM_ASYNC_JS(void, OrthrosYieldNow, (), {
	await new Promise((resolve) => setTimeout(resolve, 0));
});

EM_JS(int, OrthrosCanvasWidth, (), { return Module.canvas && Module.canvas.clientWidth || 1024; });
EM_JS(int, OrthrosCanvasHeight, (), { return Module.canvas && Module.canvas.clientHeight || 768; });

void
OrthrosCanvasSize(int *width, int *height)
{
	*width = OrthrosCanvasWidth();
	*height = OrthrosCanvasHeight();
}

EM_JS(void, OrthrosMouseMode, (int relative), { if (Module.orthrosMouse) Module.orthrosMouse(!!relative); });

static double lastLoadingYield;

void
OrthrosLoadingYield(void)
{
	double now = emscripten_get_now();
	if(now - lastLoadingYield < 50.0)
		return;
	OrthrosYieldNow();
	lastLoadingYield = emscripten_get_now();
}
#endif
