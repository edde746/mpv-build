// Behavioral regression for the libass series' event blocks: ass_render_frame
// no longer reads every event of the track for every frame, it skips blocks
// of events whose time span cannot hold the frame. What a frame shows must
// still be exactly the events with Start <= now < Start + Duration, however
// they got into the track or changed since:
// - through libass (ass_process_chunk, ass_flush_events, pruning);
// - by the caller editing the public array, as mpv does: an event filled in
//   after ass_alloc_event, the newest events' durations settled later, old
//   events dropped by moving the array down and lowering n_events;
// - by a write to an old event's times that nothing announces: a shorter
//   span at once, a longer or moved one within 32 frames.
// Every event draws a small square in a colour of its own, so the colours of
// a frame's images name the events it shows. Needs a system font, like any
// event: libass resolves the style's font before it looks at the drawing.
#include "libass/ass.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENTS 6000
#define RECHECK_FRAMES 32   // 1 << EVENT_RECHECK_SHIFT in ass.c

static const char header[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
    "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
    "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Sans,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n\n"
    "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

static void quiet(int level, const char *fmt, va_list args, void *data)
{
    (void)level; (void)fmt; (void)args; (void)data;
}

static int next_order;

// The event text of colour `id`; ASS colours are &HBBGGRR&, ASS_Image's RGBA.
static void event_text(char *buf, size_t size, unsigned id)
{
    unsigned bgr = (id & 0xFF) << 16 | (id & 0xFF00) | (id >> 16 & 0xFF);
    snprintf(buf, size, "{\\pos(%u,%u)\\1c&H%06X&\\p1}m 0 0 l 6 0 6 6 0 6",
             8 * (id % 70), 8 * (id / 70 % 40), bgr);
}

static void add_chunk(ASS_Track *track, unsigned id, long long start, long long duration)
{
    char text[128], line[192];
    event_text(text, sizeof(text), id);
    int len = snprintf(line, sizeof(line), "%d,0,Default,,0,0,0,,%s", next_order++, text);
    ass_process_chunk(track, line, len, start, duration);
}

static int cmp_unsigned(const void *a, const void *b)
{
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x > y) - (x < y);
}

// The ids a frame shows against the ids of the events that are active.
static int check(ASS_Renderer *renderer, ASS_Track *track, long long now, const char *label)
{
    static unsigned shown[4 * EVENTS], active[4 * EVENTS];
    size_t n_shown = 0, n_active = 0;
    for (ASS_Image *img = ass_render_frame(renderer, track, now, NULL); img; img = img->next)
        shown[n_shown++] = img->color >> 8;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        unsigned b, g, r;
        if (event->Start <= now && now < event->Start + event->Duration &&
            sscanf(strstr(event->Text, "1c&H"), "1c&H%2x%2x%2x", &b, &g, &r) == 3)
            active[n_active++] = r << 16 | g << 8 | b;
    }
    qsort(shown, n_shown, sizeof(*shown), cmp_unsigned);
    qsort(active, n_active, sizeof(*active), cmp_unsigned);
    if (n_shown != n_active || memcmp(shown, active, n_shown * sizeof(*shown))) {
        fprintf(stderr, "FAIL: %s at %lld ms: the frame shows %zu events, %zu are active\n",
                label, now, n_shown, n_active);
        return 1;
    }
    return 0;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ass_set_message_cb(lib, quiet, NULL);
    ASS_Renderer *r = ass_renderer_init(lib);
    ass_set_threads(r, 2);
    ass_set_frame_size(r, 640, 360);
    ass_set_fonts(r, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    ASS_Track *t = ass_new_track(lib);
    ass_process_codec_private(t, (char *)header, sizeof(header) - 1);

    // Frame-by-frame typesetting: an event per 40 ms, four at a time, as a
    // demuxer delivers them; a few long ones across them, one out of order.
    int failed = 0;
    unsigned id = 1;
    for (int i = 0; i < EVENTS; i++) {
        add_chunk(t, id++, 40LL * (i / 4), 40);
        if (i % 1000 == 500)
            add_chunk(t, id++, 40LL * (i / 4) - 3000, 9000);
        // rendering while the track grows: the newest events count at once
        if (i % 600 == 599)
            failed |= check(r, t, 40LL * (i / 4) + 10, "growing track");
    }
    if (!ass_render_frame(r, t, 40LL * 100 + 5, NULL)) {
        fprintf(stderr, "FAIL: nothing is rendered; are fonts available?\n");
        return 1;
    }
    add_chunk(t, id++, 1000, 500);
    if (t->n_events < EVENTS) {
        fprintf(stderr, "FAIL: only %d events were added\n", t->n_events);
        return 1;
    }

    for (long long now = 0; now < 40LL * EVENTS / 4 + 200 && !failed; now += 417)
        failed |= check(r, t, now, "playback");
    unsigned seed = 12345;
    for (int i = 0; i < 300 && !failed; i++) {
        seed = seed * 1103515245 + 12345;
        failed |= check(r, t, seed % (40LL * EVENTS / 4), "seek");
    }

    // An old event the caller cuts short is gone at once.
    long long now = 40LL * 137 + 5;
    failed |= check(r, t, now, "before the cut");
    for (int i = 0; i < t->n_events; i++)
        if (t->events[i].Start <= now && now < t->events[i].Start + t->events[i].Duration)
            t->events[i].Duration = 1;
    failed |= check(r, t, now, "old events cut short");

    // One it extends or moves, without a word to libass, shows up within
    // RECHECK_FRAMES frames; rendering before that must not show anything
    // that is not active either.
    t->events[10].Start = 40LL * 5000;
    t->events[10].Duration = 4000;
    t->events[2000].Duration = 40LL * 5200;
    now = 40LL * 5000 + 5;
    for (int i = 0; i < RECHECK_FRAMES - 1; i++)
        ass_render_frame(r, t, now, NULL);
    failed |= check(r, t, now, "old events extended and moved");

    // The newest events' times settled after they were rendered, at once:
    // also when the newest event is the first of its block of 64 and the
    // ones before it are the last of the block before.
    while (t->n_events % 64 != 61)
        add_chunk(t, id++, 40LL * 7000, 40);
    add_chunk(t, id++, 40LL * 7000, 40);
    add_chunk(t, id++, 40LL * 7000, 1000000);
    add_chunk(t, id++, 40LL * 7000, 1000000);
    add_chunk(t, id++, 40LL * 7000, 40);
    failed |= check(r, t, 40LL * 7000 + 5, "open-ended newest events");
    t->events[t->n_events - 2].Duration = 80;
    t->events[t->n_events - 3].Start = 40LL * 7100;
    t->events[t->n_events - 4].Start = 40LL * 50000;
    t->events[t->n_events - 4].Duration = 4000;
    failed |= check(r, t, 40LL * 7000 + 5, "newest events settled");
    failed |= check(r, t, 40LL * 7100 + 5, "newest events settled");
    failed |= check(r, t, 40LL * 50000 + 5, "newest events settled");

    // ass_alloc_event() and the fields filled in by hand, with a frame
    // rendered while the event is still blank.
    int eid = ass_alloc_event(t);
    ass_render_frame(r, t, 40LL * 3000 + 5, NULL);
    char text[128];
    event_text(text, sizeof(text), id++);
    t->events[eid].Start = 40LL * 3000;
    t->events[eid].Duration = 400;
    t->events[eid].ReadOrder = next_order++;
    t->events[eid].Style = t->events[0].Style;  // the script's: no border, no shadow
    t->events[eid].Text = strdup(text);
    failed |= check(r, t, 40LL * 3000 + 5, "event filled in by hand");

    // Old events dropped the way mpv does it (mp_ass_flush_old_events): free,
    // move the rest down, lower n_events; then new ones through libass, so
    // that the count is back where it was by the next frame.
    int drop = 7;
    for (int i = 0; i < drop; i++)
        ass_free_event(t, i);
    memmove(t->events, t->events + drop, (t->n_events - drop) * sizeof(*t->events));
    t->n_events -= drop;
    for (int i = 0; i < drop; i++)
        add_chunk(t, id++, 40LL * 3000, 400);
    // Every event now sits seven places lower: look where events crossed
    // from one block of 64 into the one before it, four events to a frame.
    for (int block = 3; block < 40 && !failed; block += 5)
        failed |= check(r, t, 40LL * 16 * block + 5, "array moved down by the caller");
    failed |= check(r, t, 40LL * 3000 + 5, "array moved down by the caller");

    // Pruning through libass: the frame's own render drops what ended
    // before it, and the demuxer has delivered as many new events by the
    // next frame.
    int before = t->n_events;
    ass_configure_prune(t, 0);
    failed |= check(r, t, 40LL * 1000 + 5, "pruning");
    ass_configure_prune(t, -1);
    int pruned = before - t->n_events;
    if (pruned < 1000) {
        fprintf(stderr, "FAIL: only %d events were pruned\n", pruned);
        failed = 1;
    }
    for (int i = 0; i < pruned; i++)
        add_chunk(t, id++, 40LL * 9000 + 40 * (i / 4), 40);
    for (int k = 1010; k < 1500 && !failed; k += 37)
        failed |= check(r, t, 40LL * k + 5, "refilled after pruning");
    failed |= check(r, t, 40LL * 9100 + 5, "refilled after pruning");

    // And a flush, with more events after it than before.
    before = t->n_events;
    ass_flush_events(t);
    for (int i = 0; i < before + 100; i++)
        add_chunk(t, id++, 40LL * (i / 2), 40);
    for (int k = 5; k < before / 2 && !failed; k += 211)
        failed |= check(r, t, 40LL * k + 5, "refilled after a flush");

    ass_free_track(t);
    ass_renderer_done(r);
    ass_library_done(lib);
    if (failed)
        return 1;
    printf("PASS: every frame shows exactly its active events, through appends, edits, drops, pruning and a flush\n");
    return 0;
}
