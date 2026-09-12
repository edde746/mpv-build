// Exercise the production OSD-plane letterbox fill against a GL stub that
// records every scissored clear. No GL runtime is needed.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float GLclampf;
typedef int GLint;
typedef int GLsizei;
typedef unsigned GLbitfield;
typedef unsigned GLenum;
#define GLAPIENTRY
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000

struct GL {
    void (GLAPIENTRY *ClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
    void (GLAPIENTRY *Enable)(GLenum);
    void (GLAPIENTRY *Disable)(GLenum);
    void (GLAPIENTRY *Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (GLAPIENTRY *Clear)(GLbitfield);
};

struct sub_bitmap_list;
struct osd_gl {
    struct GL *gl;
};

// Recorded state: clears issued while the scissor test is on, and the clear
// color and scissor state left behind for the subtitle draw that follows.
struct clear {
    int x, y, w, h;
    float a;
};
static struct clear clears[8];
static int num_clears;
static int scissor_on;
static struct clear scissor;
static float clear_alpha;
static int unscissored_clears;

static void stub_clear_color(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    if (r != 0 || g != 0 || b != 0) {
        fprintf(stderr, "FAIL: non-black clear color\n");
        exit(1);
    }
    clear_alpha = a;
}
static void stub_enable(GLenum cap) { if (cap == GL_SCISSOR_TEST) scissor_on = 1; }
static void stub_disable(GLenum cap) { if (cap == GL_SCISSOR_TEST) scissor_on = 0; }
static void stub_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    scissor = (struct clear){x, y, w, h, 0};
}
static void stub_clear(GLbitfield mask)
{
    if (mask != GL_COLOR_BUFFER_BIT) {
        fprintf(stderr, "FAIL: unexpected clear mask %#x\n", mask);
        exit(1);
    }
    if (!scissor_on) {
        unscissored_clears++;
        return;
    }
    if (num_clears == 8) {
        fprintf(stderr, "FAIL: too many clears\n");
        exit(1);
    }
    clears[num_clears] = scissor;
    clears[num_clears].a = clear_alpha;
    num_clears++;
}

#include "mediacodec_letterbox.inc"

static struct GL gl = {
    .ClearColor = stub_clear_color, .Enable = stub_enable, .Disable = stub_disable,
    .Scissor = stub_scissor, .Clear = stub_clear,
};

static void run(const struct osd_slot *slot, int sw, int sh)
{
    num_clears = 0;
    unscissored_clears = 0;
    scissor_on = 0;
    clear_alpha = 0;
    struct osd_gl g = {.gl = &gl};
    osd_gl_fill_letterbox(&g, slot, sw, sh);
}

static void expect_clear(const char *what, int i, int x, int y, int w, int h)
{
    if (i >= num_clears) {
        fprintf(stderr, "FAIL: %s: clear %d missing (%d issued)\n", what, i, num_clears);
        exit(1);
    }
    const struct clear *c = &clears[i];
    if (c->x != x || c->y != y || c->w != w || c->h != h || c->a != 1) {
        fprintf(stderr, "FAIL: %s: clear %d is (%d,%d %dx%d a=%g), expected (%d,%d %dx%d a=1)\n",
                what, i, c->x, c->y, c->w, c->h, c->a, x, y, w, h);
        exit(1);
    }
}

// Every fill leaves the state the subtitle draw relies on: scissor off and a
// transparent clear color for the next frame's full clear.
static void expect_restored(const char *what, int count)
{
    if (num_clears != count || unscissored_clears || scissor_on || clear_alpha != 0) {
        fprintf(stderr, "FAIL: %s: %d clears (%d unscissored), scissor %d, clear alpha %g\n",
                what, num_clears, unscissored_clears, scissor_on, clear_alpha);
        exit(1);
    }
}

int main(void)
{
    // 3840x1604 fitted in a 1920x1080 plane: 139-pixel bars top and bottom,
    // the top one addressed from the bottom-left GL origin.
    struct osd_slot fit = {.canvas_w = 1920, .canvas_h = 1080, .mt = 139, .mb = 139};
    run(&fit, 1920, 1080);
    expect_clear("fit top", 0, 0, 941, 1920, 139);
    expect_clear("fit bottom", 1, 0, 0, 1920, 139);
    expect_restored("fit", 2);

    // A plane presented at half the canvas it was rendered for: 69.5 rounds
    // outward to 70 so no row of compositor background survives.
    run(&fit, 960, 540);
    expect_clear("scaled top", 0, 0, 470, 960, 70);
    expect_clear("scaled bottom", 1, 0, 0, 960, 70);
    expect_restored("scaled", 2);

    // Pillarbox: only the side bars.
    struct osd_slot pillar = {.canvas_w = 1920, .canvas_h = 1080, .ml = 240, .mr = 240};
    run(&pillar, 1920, 1080);
    expect_clear("pillar left", 0, 0, 0, 240, 1080);
    expect_clear("pillar right", 1, 1680, 0, 240, 1080);
    expect_restored("pillar", 2);

    // Cover and zoom push the picture past the plane: negative margins mean
    // no bar, and nothing may be painted over the picture.
    struct osd_slot cover = {.canvas_w = 1920, .canvas_h = 1080, .ml = -400, .mr = -400, .mt = -10, .mb = -10};
    run(&cover, 1920, 1080);
    expect_restored("cover", 0);

    // No geometry yet, or a 16:9 picture: nothing to fill.
    struct osd_slot full = {.canvas_w = 1920, .canvas_h = 1080};
    run(&full, 1920, 1080);
    expect_restored("full", 0);

    // A margin wider than the surface is clamped to the surface.
    struct osd_slot huge = {.canvas_w = 1920, .canvas_h = 1080, .mt = 5000};
    run(&huge, 1920, 1080);
    expect_clear("huge top", 0, 0, 0, 1920, 1080);
    expect_restored("huge", 1);

    printf("PASS: OSD-plane letterbox fill covers exactly the margins, rounded outward, "
           "and leaves the scissor and clear color for the subtitle draw\n");
    return 0;
}
