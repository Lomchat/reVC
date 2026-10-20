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

// (a streamed file's node: the page's hooks; a fetch per wanted range at a time, kept on the node until it settles)
EM_JS(int, OrthrosFileReady, (int fd, double position, int length), {
	var stream = FS.getStream(fd), s = stream && stream.node && stream.node.orthrosStreamed;
	if (!s || !s.missing(position, length)) return 1;
	var pending = stream.node.orthrosPending || (stream.node.orthrosPending = new Map()), key = position + ':' + length;
	if (!pending.has(key)) pending.set(key, s.ensure(position, length).catch(() => {}).finally(() => pending.delete(key)));
	return 0;
});

EM_ASYNC_JS(void, OrthrosFileWait, (int fd, double position, int length), {
	var stream = FS.getStream(fd), s = stream && stream.node && stream.node.orthrosStreamed;
	if (s && s.missing(position, length)) {
		var t0 = performance.now();
		try { await s.ensure(position, length); } catch (e) {}
		var b = Module.orthrosBlocked || (Module.orthrosBlocked = { count: 0, ms: 0, max: 0, files: {} }), ms = performance.now() - t0;
		b.count++; b.ms += ms; if (ms > b.max) b.max = ms;
		if (stream.path) b.files[stream.path] = (b.files[stream.path] || 0) + ms;
	}
});

EM_JS(void, OrthrosFileComplete, (int fd), {
	var stream = FS.getStream(fd), s = stream && stream.node && stream.node.orthrosStreamed;
	if (s && s.complete) s.complete();
});

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
