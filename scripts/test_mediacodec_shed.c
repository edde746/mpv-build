// Behavioral regressions for the production MediaCodec pre-decode frame
// shedding (libavcodec/mediacodec_shed.c, built for the host by
// test_mediacodec_shed.sh): which access units the classifiers agree to
// take out of a hardware decoder's input when the host asks for
// AVDISCARD_NONREF, and what is left of the packet afterwards.
//
// The safety property under test is "never lose a frame anything later
// references": H.264 by nal_ref_idc, HEVC by sub-layer non-reference type
// at the highest temporal sub-layer, VP9 and AV1 by refresh_frame_flags,
// with reference frames that share the packet (a VP9 superframe's hidden
// frames, an AV1 hidden alt-ref) kept in place. The AV1 corpus is a real
// SVT-AV1 stream; the coded-bitstream reader is the oracle for what each
// temporal unit's last frame is, and the shed decision must match it.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/av1.h"
#include "libavcodec/cbs.h"
#include "libavcodec/cbs_av1.h"
#include "libavcodec/mediacodec_shed.h"
#include "libavcodec/packet.h"
#include "libavutil/avutil.h"
#include "libavutil/mem.h"

#include "test_mediacodec_shed_av1_sample.h"

static int failures;

#define CHECK(cond, ...) do {                                    \
    if (!(cond)) {                                               \
        failures++;                                              \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);     \
        fprintf(stderr, __VA_ARGS__);                            \
        fprintf(stderr, "\n");                                   \
    }                                                            \
} while (0)

// --- H.264 -------------------------------------------------------------------

static int h264_nal(uint8_t *out, int ref_idc, int type)
{
    static const uint8_t sc[] = { 0, 0, 0, 1 };
    memcpy(out, sc, 4);
    out[4] = (uint8_t)((ref_idc << 5) | type);
    out[5] = 0xb4; // some slice payload
    out[6] = 0x21;
    return 7;
}

static void test_h264(void)
{
    uint8_t au[64];
    int n;

    n = h264_nal(au, 0, 9);          // AUD
    n += h264_nal(au + n, 0, 6);     // SEI
    n += h264_nal(au + n, 0, 1);     // non-reference slice
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 1, "h264: nal_ref_idc 0 slice is droppable");

    n = h264_nal(au, 2, 1);
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 0, "h264: reference slice is kept");

    n = h264_nal(au, 0, 1);
    n += h264_nal(au + n, 1, 1);
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 0, "h264: one reference slice keeps the AU");

    n = h264_nal(au, 3, 5);
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 0, "h264: IDR is kept");

    n = h264_nal(au, 3, 7);
    n += h264_nal(au + n, 0, 1);
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 0, "h264: an AU carrying an SPS is kept");

    n = h264_nal(au, 0, 14);         // SVC prefix NAL
    n += h264_nal(au + n, 0, 1);
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 0, "h264: layered streams are kept");

    n = h264_nal(au, 0, 6);
    CHECK(ff_mediacodec_shed_h264_droppable(au, n) == 0, "h264: no slice, nothing to shed");

    // Three-byte start code, and a slice header right at the end.
    au[0] = 0; au[1] = 0; au[2] = 1; au[3] = 0x01;
    CHECK(ff_mediacodec_shed_h264_droppable(au, 4) == 1, "h264: 3-byte start code");

    memset(au, 0x55, sizeof(au));
    CHECK(ff_mediacodec_shed_h264_droppable(au, sizeof(au)) == 0, "h264: no start code, kept");
}

// --- HEVC --------------------------------------------------------------------

static int hevc_nal(uint8_t *out, int type, int layer_id, int temporal_id, int sps_max_sub_layers_minus1)
{
    static const uint8_t sc[] = { 0, 0, 0, 1 };
    memcpy(out, sc, 4);
    out[4] = (uint8_t)((type << 1) | (layer_id >> 5));
    out[5] = (uint8_t)(((layer_id & 0x1f) << 3) | (temporal_id + 1));
    // For an SPS: sps_video_parameter_set_id(4) max_sub_layers_minus1(3) nesting(1)
    out[6] = type == 33 ? (uint8_t)((sps_max_sub_layers_minus1 << 1) | 1) : 0xa0;
    out[7] = 0x3c;
    return 8;
}

static void test_hevc(void)
{
    uint8_t au[64];
    int n, max = -1;

    n = hevc_nal(au, 0 /*TRAIL_N*/, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: unknown sub-layer count keeps the AU");

    n = hevc_nal(au, 33 /*SPS*/, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0 && max == 0,
          "hevc: SPS is kept and records one sub-layer (max=%d)", max);

    n = hevc_nal(au, 0, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 1, "hevc: TRAIL_N at the highest sub-layer is droppable");

    n = hevc_nal(au, 1 /*TRAIL_R*/, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: TRAIL_R is kept");

    n = hevc_nal(au, 0, 0, 0, 0);
    n += hevc_nal(au + n, 1, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: one reference slice keeps the AU");

    n = hevc_nal(au, 19 /*IDR_W_RADL*/, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: IDR is kept");

    // Two sub-layers: only the top one's non-reference pictures are safe.
    n = hevc_nal(au, 33, 0, 0, 1);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0 && max == 1, "hevc: SPS with two sub-layers (max=%d)", max);
    n = hevc_nal(au, 0, 0, 0, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: TRAIL_N at tid 0 of 2 may be referenced by tid 1");
    n = hevc_nal(au, 0, 0, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 1, "hevc: TRAIL_N at the top sub-layer is droppable");
    n = hevc_nal(au, 2 /*TSA_N*/, 0, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 1, "hevc: TSA_N at the top sub-layer is droppable");
    n = hevc_nal(au, 8 /*RASL_N*/, 0, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 1, "hevc: RASL_N at the top sub-layer is droppable");
    n = hevc_nal(au, 9 /*RASL_R*/, 0, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: RASL_R is kept");

    n = hevc_nal(au, 0, 1, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: a second layer is kept");

    n = hevc_nal(au, 34 /*PPS*/, 0, 0, 0);
    n += hevc_nal(au + n, 0, 0, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: an AU carrying a PPS is kept");

    n = hevc_nal(au, 39 /*SEI prefix*/, 0, 1, 0);
    n += hevc_nal(au + n, 0, 0, 1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 1, "hevc: SEI rides along with a droppable picture");

    // temporal_id_plus1 == 0 is malformed: keep.
    n = hevc_nal(au, 0, 0, -1, 0);
    CHECK(ff_mediacodec_shed_hevc_droppable(au, n, &max) == 0, "hevc: malformed NAL header keeps the AU");
}

// --- VP9 ---------------------------------------------------------------------

// frame_marker(2)=10 profile(2)=00 show_existing(1) frame_type(1) show_frame(1)
// error_res(1) [reset_frame_context(2)] refresh_frame_flags(8)
static const uint8_t vp9_shown_nonref[]   = { 0x86, 0x00, 0x00, 0x00 }; // type=1 show=1 er=0 reset=00 refresh=0
static const uint8_t vp9_shown_ref[]      = { 0x86, 0x00, 0x40, 0x00 }; // refresh bit 0 set (bits 10..17)
static const uint8_t vp9_key[]            = { 0x82, 0x49, 0x83, 0x42 }; // frame_type=0
static const uint8_t vp9_show_existing[]  = { 0x88, 0x00, 0x00, 0x00 };
static const uint8_t vp9_hidden[]         = { 0x84, 0x00, 0x40, 0x00 }; // show_frame=0
static const uint8_t vp9_er_nonref[]      = { 0x87, 0x00, 0x00, 0x00 }; // error_resilient=1: refresh at bits 8..15
static const uint8_t vp9_er_ref[]         = { 0x87, 0x01, 0x00, 0x00 };

static void vp9_packet(AVPacket *pkt, const uint8_t *data, int size)
{
    av_packet_unref(pkt);
    av_new_packet(pkt, size);
    memcpy(pkt->data, data, size);
}

static void test_vp9(void)
{
    AVPacket *pkt = av_packet_alloc();
    uint8_t sf[64];
    int n;

    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_shown_nonref, 4) == 1, "vp9: shown inter frame refreshing nothing");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_shown_ref, 4) == 0, "vp9: refresh_frame_flags != 0 is kept");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_key, 4) == 0, "vp9: key frame is kept");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_show_existing, 4) == 0, "vp9: show_existing_frame is kept");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_hidden, 4) == 0, "vp9: hidden frame is kept");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_er_nonref, 4) == 1, "vp9: error-resilient shown non-ref");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_er_ref, 4) == 0, "vp9: error-resilient reference is kept");
    CHECK(ff_mediacodec_shed_vp9_frame_droppable(vp9_shown_nonref, 1) == 0, "vp9: truncated header is kept");

    // A plain frame packet is emptied.
    vp9_packet(pkt, vp9_shown_nonref, 4);
    CHECK(ff_mediacodec_shed_vp9_packet(pkt) == 1 && pkt->size == 0, "vp9: single non-ref frame packet emptied (size %d)", pkt->size);
    vp9_packet(pkt, vp9_shown_ref, 4);
    CHECK(ff_mediacodec_shed_vp9_packet(pkt) == 0 && pkt->size == 4, "vp9: reference frame packet untouched");

    // Superframe [hidden ref][shown non-ref] + index (c1 04 04 c1): the
    // hidden frame stays, the index goes with the shed frame.
    n = 0;
    memcpy(sf + n, vp9_hidden, 4); n += 4;
    memcpy(sf + n, vp9_shown_nonref, 4); n += 4;
    sf[n++] = 0xc1; sf[n++] = 4; sf[n++] = 4; sf[n++] = 0xc1;
    vp9_packet(pkt, sf, n);
    CHECK(ff_mediacodec_shed_vp9_packet(pkt) == 1 && pkt->size == 4 &&
          !memcmp(pkt->data, vp9_hidden, 4),
          "vp9: two-frame superframe keeps its hidden frame (size %d)", pkt->size);

    // Three frames: the index is rewritten for two.
    n = 0;
    memcpy(sf + n, vp9_hidden, 4); n += 4;
    memcpy(sf + n, vp9_hidden, 4); n += 4;
    memcpy(sf + n, vp9_shown_nonref, 4); n += 4;
    sf[n++] = 0xc2; sf[n++] = 4; sf[n++] = 4; sf[n++] = 4; sf[n++] = 0xc2;
    vp9_packet(pkt, sf, n);
    {
        static const uint8_t index2[] = { 0xc1, 4, 4, 0xc1 };
        CHECK(ff_mediacodec_shed_vp9_packet(pkt) == 1 && pkt->size == 12 &&
              !memcmp(pkt->data, vp9_hidden, 4) && !memcmp(pkt->data + 4, vp9_hidden, 4) &&
              !memcmp(pkt->data + 8, index2, 4),
              "vp9: three-frame superframe keeps two and a rewritten index (size %d)", pkt->size);
    }

    // A superframe whose last frame is a reference is untouched.
    n = 0;
    memcpy(sf + n, vp9_hidden, 4); n += 4;
    memcpy(sf + n, vp9_shown_ref, 4); n += 4;
    sf[n++] = 0xc1; sf[n++] = 4; sf[n++] = 4; sf[n++] = 0xc1;
    vp9_packet(pkt, sf, n);
    CHECK(ff_mediacodec_shed_vp9_packet(pkt) == 0 && pkt->size == 12, "vp9: superframe ending in a reference is kept");

    // A corrupt index (sizes past the packet) is kept.
    sf[9] = 40;
    vp9_packet(pkt, sf, n);
    CHECK(ff_mediacodec_shed_vp9_packet(pkt) == 0 && pkt->size == 12, "vp9: inconsistent superframe index is kept");

    av_packet_free(&pkt);
}

// --- AV1 ---------------------------------------------------------------------

// leb128 as written by the OBU stream: obu_size follows the 1-byte header.
static int obu_size(const uint8_t *p, int avail, int *len)
{
    int value = 0;
    for (int i = 0; i < 8 && i < avail; i++) {
        value |= (p[i] & 0x7f) << (7 * i);
        if (!(p[i] & 0x80)) {
            *len = i + 1;
            return value;
        }
    }
    return -1;
}

// Split the Section 5 stream into temporal units at temporal delimiters.
static int split_temporal_units(const uint8_t *data, int size, int *starts, int max)
{
    int pos = 0, n = 0;
    while (pos < size) {
        int type = (data[pos] >> 3) & 0xf;
        int has_ext = (data[pos] >> 2) & 1;
        int hdr = 1 + has_ext, len, sz;
        if (type == AV1_OBU_TEMPORAL_DELIMITER) {
            if (n == max)
                return -1;
            starts[n++] = pos;
        }
        sz = obu_size(data + pos + hdr, size - pos - hdr, &len);
        if (sz < 0)
            return -1;
        pos += hdr + len + sz;
    }
    return n;
}

typedef struct LastFrame {
    int present, show_existing, show_frame, frame_type, refresh, offset, only_delimiter_before;
} LastFrame;

// What the oracle says about the temporal unit's last frame.
static int oracle(CodedBitstreamContext *cbs, CodedBitstreamFragment *frag,
                  const AVPacket *pkt, LastFrame *lf)
{
    int err = ff_cbs_read_packet(cbs, frag, pkt);
    memset(lf, 0, sizeof(*lf));
    if (err < 0)
        return err;
    lf->only_delimiter_before = 1;
    for (int i = 0; i < frag->nb_units; i++) {
        const CodedBitstreamUnit *u = &frag->units[i];
        const AV1RawOBU *obu = u->content;
        if (u->type == AV1_OBU_FRAME || u->type == AV1_OBU_FRAME_HEADER) {
            const AV1RawFrameHeader *h = u->type == AV1_OBU_FRAME ? &obu->obu.frame.header
                                                                  : &obu->obu.frame_header;
            if (lf->present)
                lf->only_delimiter_before = 0;
            lf->present = 1;
            lf->show_existing = h->show_existing_frame;
            lf->show_frame = h->show_frame;
            lf->frame_type = h->frame_type;
            lf->refresh = h->refresh_frame_flags;
            lf->offset = (int)(u->data - frag->data);
        } else if (u->type != AV1_OBU_TEMPORAL_DELIMITER && u->type != AV1_OBU_TILE_GROUP &&
                   u->type != AV1_OBU_PADDING && u->type != AV1_OBU_METADATA) {
            if (!lf->present)
                lf->only_delimiter_before = 0; // a sequence header before it
        }
    }
    ff_cbs_fragment_reset(frag);
    return 0;
}

static void test_av1(void)
{
    static int starts[128];
    int n = split_temporal_units(av1_sample, sizeof(av1_sample), starts, 128);
    AVCodecContext avctx = { .codec_id = AV_CODEC_ID_AV1 };
    MediaCodecShedContext sh;
    CodedBitstreamContext *cbs = NULL;
    CodedBitstreamFragment frag = {0};
    AVPacket *pkt = av_packet_alloc();
    int shed = 0, kept_with_refs = 0, emptied = 0, show_existing_units = 0;

    CHECK(n > 10, "av1: corpus splits into temporal units (%d)", n);
    CHECK(ff_mediacodec_shed_init(&sh, &avctx) == 0 && sh.cbs, "av1: shed context has a reader");
    CHECK(ff_cbs_init(&cbs, AV_CODEC_ID_AV1, NULL) == 0, "av1: oracle reader");

    for (int i = 0; i < n; i++) {
        int start = starts[i];
        int end = i + 1 < n ? starts[i + 1] : (int)sizeof(av1_sample);
        LastFrame lf;
        int expect_shed, ret, original_size = end - start;

        av_packet_unref(pkt);
        av_new_packet(pkt, original_size);
        memcpy(pkt->data, av1_sample + start, original_size);
        pkt->flags = i == 0 ? AV_PKT_FLAG_KEY : 0;
        pkt->pts = i;

        CHECK(oracle(cbs, &frag, pkt, &lf) == 0 && lf.present, "av1: unit %d decomposes", i);
        expect_shed = i > 0 && !lf.show_existing && lf.show_frame && lf.refresh == 0 &&
                      lf.frame_type != AV1_FRAME_KEY;
        show_existing_units += lf.show_existing;

        // The production reader follows the stream through observe() when
        // no discard is requested; alternate to prove both paths keep its
        // state right.
        if (i % 3 == 1) {
            ff_mediacodec_shed_observe(&sh, &avctx, pkt);
            ret = 0;
            CHECK(pkt->size == original_size, "av1: observe leaves unit %d alone", i);
            continue;
        }
        ret = ff_mediacodec_shed_packet(&sh, &avctx, pkt);
        CHECK(ret == expect_shed, "av1: unit %d shed=%d expected %d (show_existing=%d show=%d refresh=0x%02x type=%d)",
              i, ret, expect_shed, lf.show_existing, lf.show_frame, lf.refresh, lf.frame_type);
        if (ret == 1) {
            shed++;
            if (lf.only_delimiter_before) {
                emptied++;
                CHECK(pkt->size == 0, "av1: unit %d held only the shed frame, expected empty, got %d", i, pkt->size);
            } else {
                AVPacket *rest = av_packet_alloc();
                LastFrame rl;
                kept_with_refs++;
                CHECK(pkt->size == lf.offset, "av1: unit %d truncated at %d, expected the frame's offset %d", i, pkt->size, lf.offset);
                // What stays is a temporal unit the oracle still reads,
                // ending in a frame that is not shown.
                av_new_packet(rest, pkt->size);
                memcpy(rest->data, pkt->data, pkt->size);
                CHECK(oracle(cbs, &frag, rest, &rl) == 0 && rl.present && !rl.show_frame,
                      "av1: unit %d's remainder is a valid unit of hidden frames", i);
                av_packet_free(&rest);
            }
        } else {
            CHECK(pkt->size == original_size, "av1: unit %d untouched", i);
        }
    }
    CHECK(shed >= 3, "av1: corpus exercised shedding (%d shed)", shed);
    CHECK(kept_with_refs >= 1, "av1: at least one unit kept its hidden reference frames (%d)", kept_with_refs);
    CHECK(emptied >= 1, "av1: at least one unit was emptied (%d)", emptied);
    CHECK(show_existing_units >= 1, "av1: corpus has show_existing_frame units (%d)", show_existing_units);

    // A key-frame packet is never touched, however the flags read.
    av_packet_unref(pkt);
    av_new_packet(pkt, starts[1]);
    memcpy(pkt->data, av1_sample, starts[1]);
    pkt->flags = 0;
    ff_mediacodec_shed_flush(&sh);
    CHECK(ff_mediacodec_shed_packet(&sh, &avctx, pkt) == 0 && pkt->size == starts[1], "av1: key frame unit is kept");

    // Garbage is kept, and the reader survives it.
    av_packet_unref(pkt);
    av_new_packet(pkt, 16);
    memset(pkt->data, 0xff, 16);
    CHECK(ff_mediacodec_shed_packet(&sh, &avctx, pkt) == 0 && pkt->size == 16, "av1: undecomposable unit is kept");
    CHECK(sh.cbs != NULL, "av1: one bad unit does not switch shedding off");

    // A stream the reader keeps refusing switches shedding off for good:
    // units it would have shed are then kept.
    for (int i = 0; i < 8; i++) {
        av_packet_unref(pkt);
        av_new_packet(pkt, 16);
        memset(pkt->data, 0xff, 16);
        ff_mediacodec_shed_packet(&sh, &avctx, pkt);
    }
    CHECK(sh.cbs == NULL, "av1: repeated failures close the reader");
    for (int i = 1; i < n; i++) {
        int start = starts[i], end = i + 1 < n ? starts[i + 1] : (int)sizeof(av1_sample);
        av_packet_unref(pkt);
        av_new_packet(pkt, end - start);
        memcpy(pkt->data, av1_sample + start, end - start);
        CHECK(ff_mediacodec_shed_packet(&sh, &avctx, pkt) == 0 && pkt->size == end - start,
              "av1: unit %d kept once shedding is off", i);
    }

    ff_cbs_fragment_free(&frag);
    ff_cbs_close(&cbs);
    ff_mediacodec_shed_uninit(&sh);
    av_packet_free(&pkt);
}

// Codecs the classifier does not know are never touched.
static void test_other_codecs(void)
{
    AVCodecContext avctx = { .codec_id = AV_CODEC_ID_MPEG2VIDEO };
    MediaCodecShedContext sh;
    AVPacket *pkt = av_packet_alloc();
    av_new_packet(pkt, 8);
    memset(pkt->data, 0, 8);
    CHECK(ff_mediacodec_shed_init(&sh, &avctx) == 0, "other: init");
    CHECK(ff_mediacodec_shed_packet(&sh, &avctx, pkt) == 0 && pkt->size == 8, "other: MPEG-2 packet untouched");
    ff_mediacodec_shed_uninit(&sh);
    av_packet_free(&pkt);
}

int main(void)
{
    test_h264();
    test_hevc();
    test_vp9();
    test_av1();
    test_other_codecs();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mediacodec shed: ok\n");
    return 0;
}
