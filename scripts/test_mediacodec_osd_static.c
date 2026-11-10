// vo_mediacodec's osd_static_id(), extracted verbatim: the id a frame's OSD
// request carries while the OSD cannot change from one frame to the next.
// The window is asked for its size only while that id can settle the
// pipeline: up to API 30 each ANativeWindow getter is a binder call.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ANativeWindow ANativeWindow;
struct osd_state { uint64_t static_id; };
struct priv {
    ANativeWindow *osd_window;
    int osd_vsync_delay;
    int osd_static_w, osd_static_h;
    uint32_t osd_static_sizes;
};
struct vo {
    struct priv *priv;
    struct osd_state *osd;
};

static int window_w = 1920, window_h = 1080, window_queries;
static int ANativeWindow_getWidth(ANativeWindow *w) { (void)w; window_queries++; return window_w; }
static int ANativeWindow_getHeight(ANativeWindow *w) { (void)w; window_queries++; return window_h; }
static uint64_t osd_get_static_id(struct osd_state *osd) { return osd->static_id; }

#include "mediacodec_osd_static.inc"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    struct osd_state osd = {.static_id = 3};
    struct priv p = {0};
    struct vo vo = {.priv = &p, .osd = &osd};

    uint64_t a = osd_static_id(&vo);
    check(a != 0 && window_queries == 2, "a static OSD has an id made with the window's size");
    check(osd_static_id(&vo) == a, "which stays the same while neither changes");
    window_w = 1080, window_h = 1920;
    uint64_t b = osd_static_id(&vo);
    check(b != 0 && b != a, "a resized window is another id");
    osd.static_id = 4;
    uint64_t c = osd_static_id(&vo);
    check(c != 0 && c != b, "and so is an OSD change");
    window_w = 0;
    check(osd_static_id(&vo) == 0, "a window without a size settles nothing");
    window_w = 1080;

    // A subtitle decoder is attached: every frame is requested, and the
    // window is left alone.
    osd.static_id = 0;
    window_queries = 0;
    check(osd_static_id(&vo) == 0 && window_queries == 0, "subtitles: no id, no window query");

    // A plane presented on every frame never settles either.
    osd.static_id = 4;
    p.osd_vsync_delay = 1;
    check(osd_static_id(&vo) == 0 && window_queries == 0, "delayed plane: no id, no window query");
    p.osd_vsync_delay = 0;
    check(osd_static_id(&vo) != 0 && window_queries == 2, "the delay gone, the id is back");

    puts("mediacodec OSD static id: ok");
    return 0;
}
