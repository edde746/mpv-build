// Exercise the production MediaCodec static HDR metadata path
// ("hdr-static-info"), built by test_mediacodec_hdr.sh: the output-format
// parser, the frame tagging, and both frame wrappers that call it, the
// surface one's output buffer ownership included. No Android runtime is
// needed: the MediaFormat, the codec and FFmpeg's frames are host stand-ins.
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define AVERROR_EXTERNAL (-1)
#define AVERROR(value) (-(value))
#define AV_PIX_FMT_NONE (-1)
#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))
#define AV_BUFFER_FLAG_READONLY 1
#define FF_COMPLIANCE_NORMAL 0
#define FF_COMPLIANCE_STRICT 1
#define FFALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define AV_RL16(p) ((unsigned)((const uint8_t *)(p))[0] | ((unsigned)((const uint8_t *)(p))[1] << 8))
#define av_log(...) ((void)0)

typedef struct { int num, den; } AVRational;
static AVRational av_make_q(int num, int den) { return (AVRational){num, den}; }
static double av_q2d(AVRational q) { return q.den ? (double)q.num / q.den : 0; }

typedef struct {
    AVRational display_primaries[3][2];
    AVRational white_point[2];
    AVRational min_luminance;
    AVRational max_luminance;
    int has_primaries;
    int has_luminance;
} AVMasteringDisplayMetadata;

typedef struct {
    unsigned MaxCLL;
    unsigned MaxFALL;
} AVContentLightMetadata;

// FFmpeg numbers packet and frame side data apart; so does the stand-in, so a
// packet type passed where a frame type belongs does not pass by accident.
enum AVPacketSideDataType { AV_PKT_DATA_MASTERING_DISPLAY_METADATA = 20, AV_PKT_DATA_CONTENT_LIGHT_LEVEL = 22 };
enum AVFrameSideDataType { AV_FRAME_DATA_MASTERING_DISPLAY_METADATA = 11, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL = 14 };

typedef struct {
    uint8_t *data;
    size_t size;
    enum AVPacketSideDataType type;
} AVPacketSideData;

typedef struct {
    uint8_t *data;
    size_t size;
    enum AVFrameSideDataType type;
} AVFrameSideData;

typedef struct {
    void (*free)(void *opaque, uint8_t *data);
    void *opaque;
} AVBufferRef;

// Side data is a list, as in FFmpeg: nothing stops a second entry of a type.
typedef struct {
    AVBufferRef *buf[1];
    uint8_t *data[4];
    int linesize[3];
    int width, height, format;
    AVRational sample_aspect_ratio;
    int64_t pts, pkt_dts;
    int color_range, color_primaries, color_trc, colorspace;
    AVFrameSideData *side_data[8];
    int nb_side_data;
} AVFrame;

typedef struct {
    int width, height, pix_fmt, color_range, colorspace, color_primaries, color_trc;
    AVRational sample_aspect_ratio;
    AVPacketSideData coded_side_data[2];
    int nb_coded_side_data;
    int strict_std_compliance;
    int *side_data_prefer_packet;
    unsigned nb_side_data_prefer_packet;
} AVCodecContext;

typedef struct {
    const char *key;
    int32_t value;
} Field;
typedef struct {
    Field fields[16];
    int count;
    const uint8_t *hdr;
    size_t hdr_size;
} Format;
typedef struct FFAMediaCodec FFAMediaCodec;
typedef struct {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
} FFAMediaCodecBufferInfo;
typedef struct {
    int64_t input_ns, output_ns, dequeue_ns;
    int queued_outputs, held_buffers;
} AVMediaCodecBufferTiming;
typedef struct {
    AVCodecContext *avctx;
    FFAMediaCodec *codec;
    Format *format;
    const char *codec_name;
    int use_ndk_codec, width, height, stride, slice_height, color_format;
    int crop_left, crop_top, crop_right, crop_bottom, display_width, display_height;
    AVMasteringDisplayMetadata hdr_mastering;
    AVContentLightMetadata hdr_light;
    atomic_int serial;
    atomic_int hw_buffer_count;
    int delay_flush;
    int refcount;
} MediaCodecDecContext;
typedef struct MediaCodecBuffer {
    MediaCodecDecContext *ctx;
    ssize_t index;
    int64_t pts;
    atomic_int released;
    int serial;
    AVMediaCodecBufferTiming timing;
} AVMediaCodecBuffer;

static int ff_AMediaFormat_getInt32(Format *format, const char *key, int32_t *value)
{
    for (int i = 0; i < format->count; ++i) {
        if (!strcmp(format->fields[i].key, key)) {
            *value = format->fields[i].value;
            return 1;
        }
    }
    return 0;
}

static int ff_AMediaFormat_getRect(Format *format, const char *key,
                                   int32_t *left, int32_t *top, int32_t *right, int32_t *bottom)
{
    return 0;
}

// The wrapper's contract: a copy the caller frees, or 0 with *data NULL.
static int buffers_out;
static int ff_AMediaFormat_getBuffer(Format *format, const char *key, void **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    if (strcmp(key, "hdr-static-info") || !format->hdr)
        return 0;
    *data = malloc(format->hdr_size);
    memcpy(*data, format->hdr, format->hdr_size);
    *size = format->hdr_size;
    buffers_out++;
    return 1;
}

static void av_free(void *data)
{
    if (data)
        buffers_out--;
    free(data);
}

static void av_freep(void *arg)
{
    void *ptr;
    memcpy(&ptr, arg, sizeof(ptr));
    free(ptr);
    ptr = NULL;
    memcpy(arg, &ptr, sizeof(ptr));
}

static void *av_mallocz(size_t size) { return calloc(1, size); }

static char *ff_AMediaFormat_toString(Format *format)
{
    char *description = malloc(1);
    *description = '\0';
    return description;
}

static int mcdec_map_color_format(AVCodecContext *avctx, MediaCodecDecContext *s, int value) { return value; }
static AVRational av_div_q(AVRational a, AVRational b) { return (AVRational){a.num * b.den, a.den * b.num}; }
static void ff_set_sar(AVCodecContext *avctx, AVRational sar) { avctx->sample_aspect_ratio = sar; }
static int ff_AMediaFormatColorRange_to_AVColorRange(int value) { return value; }
static int ff_AMediaFormatColorStandard_to_AVColorSpace(int value) { return value; }
static int ff_AMediaFormatColorStandard_to_AVColorPrimaries(int value) { return value; }
static int ff_AMediaFormatColorTransfer_to_AVColorTransfer(int value) { return value; }
static int ff_set_dimensions(AVCodecContext *avctx, int width, int height)
{
    avctx->width = width;
    avctx->height = height;
    return 0;
}

// Side data allocations the code under test may make before the next one
// fails; -1 for no limit.
static int side_data_budget = -1;

static AVFrameSideData *append_side_data(AVFrame *frame, enum AVFrameSideDataType type, size_t size)
{
    AVFrameSideData *sd = calloc(1, sizeof(*sd));
    *sd = (AVFrameSideData){calloc(1, size), size, type};
    frame->side_data[frame->nb_side_data++] = sd;
    return sd;
}

static AVFrameSideData *av_frame_get_side_data(const AVFrame *frame, enum AVFrameSideDataType type)
{
    for (int i = 0; i < frame->nb_side_data; i++)
        if (frame->side_data[i]->type == type)
            return frame->side_data[i];
    return NULL;
}

static AVFrameSideData *av_frame_new_side_data(AVFrame *frame, enum AVFrameSideDataType type, size_t size)
{
    if (!side_data_budget)
        return NULL;
    if (side_data_budget > 0)
        side_data_budget--;
    return append_side_data(frame, type, size);
}

static void remove_side_data(AVFrame *frame, enum AVFrameSideDataType type)
{
    int kept = 0;
    for (int i = 0; i < frame->nb_side_data; i++) {
        if (frame->side_data[i]->type == type) {
            free(frame->side_data[i]->data);
            free(frame->side_data[i]);
        } else {
            frame->side_data[kept++] = frame->side_data[i];
        }
    }
    frame->nb_side_data = kept;
}

static const AVPacketSideData *ff_get_coded_side_data(const AVCodecContext *avctx, enum AVPacketSideDataType type)
{
    for (int i = 0; i < avctx->nb_coded_side_data; i++)
        if (avctx->coded_side_data[i].type == type)
            return &avctx->coded_side_data[i];
    return NULL;
}

// decode.c's ff_frame_new_side_data: an existing entry stays when the caller
// prefers the container's (a lone -1 meaning every type), and is replaced
// otherwise.
static int decoder_side_data_new(const AVCodecContext *avctx, AVFrame *frame, enum AVFrameSideDataType type,
                                 enum AVPacketSideDataType packet, size_t size, void **out)
{
    AVFrameSideData *sd;

    *out = NULL;
    if (av_frame_get_side_data(frame, type)) {
        int prefer = avctx->nb_side_data_prefer_packet == 1 && avctx->side_data_prefer_packet[0] == -1;
        for (unsigned i = 0; i < avctx->nb_side_data_prefer_packet; i++)
            prefer |= avctx->side_data_prefer_packet[i] == (int)packet;
        if (prefer)
            return 0;
        remove_side_data(frame, type);
    }
    sd = av_frame_new_side_data(frame, type, size);
    if (!sd)
        return AVERROR(ENOMEM);
    *out = sd->data;
    return 0;
}

static int ff_decode_mastering_display_new(const AVCodecContext *avctx, AVFrame *frame,
                                           AVMasteringDisplayMetadata **mastering)
{
    return decoder_side_data_new(avctx, frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                 AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof(**mastering), (void **)mastering);
}

static int ff_decode_content_light_new(const AVCodecContext *avctx, AVFrame *frame,
                                       AVContentLightMetadata **light)
{
    return decoder_side_data_new(avctx, frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                 AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof(**light), (void **)light);
}

// The codec and the decoder context, as far as the wrappers reach them.
static struct {
    int releases, rendered, released_calls, copies;
    ssize_t last_index;
} codec_log;

static int ff_AMediaCodec_releaseOutputBuffer(FFAMediaCodec *codec, size_t idx, int render)
{
    codec_log.releases++;
    codec_log.rendered += render != 0;
    codec_log.last_index = (ssize_t)idx;
    return 0;
}

static void ff_mediacodec_dec_released(MediaCodecDecContext *s) { codec_log.released_calls++; }
static void ff_mediacodec_dec_ref(MediaCodecDecContext *s) { s->refcount++; }
static void ff_mediacodec_dec_unref(MediaCodecDecContext *s) { s->refcount--; }

static int64_t mediacodec_dec_frame_pts(AVCodecContext *avctx, MediaCodecDecContext *s,
                                        int64_t presentation_time_us)
{
    return presentation_time_us;
}

static AVBufferRef *av_buffer_create(uint8_t *data, size_t size, void (*free_cb)(void *opaque, uint8_t *data),
                                     void *opaque, int flags)
{
    AVBufferRef *ref = calloc(1, sizeof(*ref));
    ref->free = free_cb;
    ref->opaque = opaque;
    return ref;
}

static void av_frame_unref(AVFrame *frame)
{
    if (frame->buf[0]) {
        if (frame->buf[0]->free)
            frame->buf[0]->free(frame->buf[0]->opaque, NULL);
        free(frame->buf[0]);
    }
    for (int i = 0; i < frame->nb_side_data; i++) {
        free(frame->side_data[i]->data);
        free(frame->side_data[i]);
    }
    memset(frame, 0, sizeof(*frame));
}

// ff_get_buffer, as far as side data goes: ff_decode_frame_props attaches
// the container's (coded) static HDR metadata to a copied frame.
static int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    static const struct {
        enum AVPacketSideDataType packet;
        enum AVFrameSideDataType frame;
    } map[] = {
        { AV_PKT_DATA_MASTERING_DISPLAY_METADATA, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA },
        { AV_PKT_DATA_CONTENT_LIGHT_LEVEL,        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL        },
    };
    for (int i = 0; i < 2; i++) {
        const AVPacketSideData *src = ff_get_coded_side_data(avctx, map[i].packet);
        if (src)
            memcpy(append_side_data(frame, map[i].frame, src->size)->data, src->data, src->size);
    }
    frame->buf[0] = av_buffer_create(NULL, 0, NULL, NULL, 0);
    return 0;
}

#define SW_COPY(name) \
    static void name(AVCodecContext *avctx, MediaCodecDecContext *s, uint8_t *data, size_t size, \
                     FFAMediaCodecBufferInfo *info, AVFrame *frame) { codec_log.copies++; }
SW_COPY(ff_mediacodec_sw_buffer_copy_yuv420_planar)
SW_COPY(ff_mediacodec_sw_buffer_copy_yuv420_semi_planar)
SW_COPY(ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar)
SW_COPY(ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar_64x32Tile2m8ka)

#include "mediacodec_hdr.inc"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

// hdr-static-info as OMX.Nvidia.h265.decode reported it for a P3-D65
// mastered HDR10 stream (1000 / 0.0001 cd/m2, MaxCLL 1000, MaxFALL 41) once
// its first frame was out, read back through MediaFormat on the device.
static const uint16_t shield[12] = {34000, 16000, 13250, 34500, 7500, 3000, 15635, 16450, 1000, 1, 1000, 41};

static void blob(uint8_t out[25], uint8_t id, const uint16_t value[12])
{
    out[0] = id;
    for (int i = 0; i < 12; i++) {
        out[1 + 2 * i] = value[i] & 0xff;
        out[2 + 2 * i] = value[i] >> 8;
    }
}

static int same_q(AVRational a, int num, int den)
{
    return (long long)a.num * den == (long long)num * a.den && a.den;
}

static Format video_format(void)
{
    return (Format){
        .fields = {{"width", 3840}, {"height", 2160}, {"color-format", 1}},
        .count = 3,
    };
}

// What the decoder reports after a format change.
static void parse_strict(MediaCodecDecContext *s, Format *format, int strict)
{
    AVCodecContext avctx = {.strict_std_compliance = strict};
    s->format = format;
    s->codec_name = "OMX.Nvidia.h265.decode";
    CHECK(mediacodec_dec_parse_video_format(&avctx, s, 1) == 0, "the format must parse");
}

static void parse(MediaCodecDecContext *s, Format *format)
{
    parse_strict(s, format, FF_COMPLIANCE_NORMAL);
}

// Parse a descriptor with one value changed from the SHIELD's.
static MediaCodecDecContext parse_with(int index, unsigned value, int strict)
{
    uint8_t data[25];
    uint16_t values[12];
    Format format = video_format();
    MediaCodecDecContext s = {0};

    memcpy(values, shield, sizeof(values));
    values[index] = value;
    blob(data, 0, values);
    format.hdr = data;
    format.hdr_size = sizeof(data);
    parse_strict(&s, &format, strict);
    return s;
}

static void test_reported_values_and_units(void)
{
    uint8_t data[25];
    Format format = video_format();
    MediaCodecDecContext s = {0};
    blob(data, 0, shield);
    format.hdr = data;
    format.hdr_size = sizeof(data);
    parse(&s, &format);

    AVMasteringDisplayMetadata *m = &s.hdr_mastering;
    CHECK(m->has_primaries && m->has_luminance, "primaries and luminance must be known");
    // FFmpeg's order is red, green, blue, as Android's descriptor's.
    CHECK(same_q(m->display_primaries[0][0], 34000, 50000) && same_q(m->display_primaries[0][1], 16000, 50000),
          "red is (0.68, 0.32), got %d/%d %d/%d", m->display_primaries[0][0].num, m->display_primaries[0][0].den,
          m->display_primaries[0][1].num, m->display_primaries[0][1].den);
    CHECK(same_q(m->display_primaries[1][0], 13250, 50000) && same_q(m->display_primaries[1][1], 34500, 50000),
          "green is (0.265, 0.69)");
    CHECK(same_q(m->display_primaries[2][0], 7500, 50000) && same_q(m->display_primaries[2][1], 3000, 50000),
          "blue is (0.15, 0.06)");
    CHECK(same_q(m->white_point[0], 15635, 50000) && same_q(m->white_point[1], 16450, 50000), "white is D65");
    CHECK(av_q2d(m->max_luminance) == 1000.0, "max luminance is 1000 cd/m2, got %f", av_q2d(m->max_luminance));
    CHECK(av_q2d(m->min_luminance) == 0.0001, "min luminance is 0.0001 cd/m2, got %f", av_q2d(m->min_luminance));
    CHECK(s.hdr_light.MaxCLL == 1000 && s.hdr_light.MaxFALL == 41, "MaxCLL 1000, MaxFALL 41, got %u %u",
          s.hdr_light.MaxCLL, s.hdr_light.MaxFALL);
    CHECK(buffers_out == 0, "the copied buffer must be freed");
}

static void test_unknown_and_malformed(void)
{
    uint8_t data[26];
    uint16_t value[12];
    Format format = video_format();
    MediaCodecDecContext s = {0};

    // The decoder's all-zero descriptor before its first frame (and for
    // ByteBuffer output) says nothing, and replaces what an earlier
    // format said.
    blob(data, 0, shield);
    format.hdr = data;
    format.hdr_size = 25;
    parse(&s, &format);
    memset(data, 0, sizeof(data));
    parse(&s, &format);
    CHECK(!s.hdr_mastering.has_primaries && !s.hdr_mastering.has_luminance &&
          !s.hdr_light.MaxCLL && !s.hdr_light.MaxFALL, "an all-zero descriptor must be unknown");

    // A format without the key clears it too.
    blob(data, 0, shield);
    parse(&s, &format);
    format.hdr = NULL;
    parse(&s, &format);
    CHECK(!s.hdr_mastering.has_luminance && !s.hdr_light.MaxCLL, "a missing key must clear the metadata");

    // Only Type 1 descriptors of at least 25 bytes are read.
    format.hdr = data;
    blob(data, 1, shield);
    parse(&s, &format);
    CHECK(!s.hdr_mastering.has_luminance && !s.hdr_light.MaxCLL, "descriptor ID 1 must be ignored");
    blob(data, 0, shield);
    format.hdr_size = 24;
    parse(&s, &format);
    CHECK(!s.hdr_mastering.has_luminance && !s.hdr_light.MaxCLL, "a truncated descriptor must be ignored");
    format.hdr_size = 26;
    parse(&s, &format);
    CHECK(s.hdr_mastering.has_luminance && s.hdr_light.MaxCLL == 1000, "trailing bytes must not matter");

    // Each part is known on its own: a missing chromaticity drops the
    // primaries, a zero peak the luminance, and the light levels stand alone.
    memcpy(value, shield, sizeof(value));
    value[7] = 0;
    value[8] = 0;
    blob(data, 0, value);
    format.hdr_size = 25;
    parse(&s, &format);
    CHECK(!s.hdr_mastering.has_primaries, "a zero white point y must leave the primaries unknown");
    CHECK(!s.hdr_mastering.has_luminance, "a zero peak must leave the luminance unknown");
    CHECK(s.hdr_light.MaxCLL == 1000 && s.hdr_light.MaxFALL == 41, "the light levels must stand alone");
    CHECK(buffers_out == 0, "every copied buffer must be freed");
}

// The ranges FFmpeg's HEVC/H.264 decoders hold the same SEI to (h2645_sei.c),
// in Android's units: chromaticities in 0.00002, peak in cd/m2, floor in
// 0.0001 cd/m2.
static void test_sei_ranges(void)
{
    // Every x within [5, 37000], every y within [5, 42000].
    CHECK(parse_with(0, 37000, 0).hdr_mastering.has_primaries, "a red x of 0.74 must be known");
    CHECK(!parse_with(0, 37001, 0).hdr_mastering.has_primaries, "a red x past 0.74 must be unknown");
    CHECK(parse_with(3, 42000, 0).hdr_mastering.has_primaries, "a green y of 0.84 must be known");
    CHECK(!parse_with(3, 42001, 0).hdr_mastering.has_primaries, "a green y past 0.84 must be unknown");
    CHECK(parse_with(6, 5, 0).hdr_mastering.has_primaries, "a white x of 0.0001 must be known");
    CHECK(!parse_with(6, 4, 0).hdr_mastering.has_primaries, "a white x below 0.0001 must be unknown");
    CHECK(parse_with(0, 37001, 0).hdr_mastering.has_luminance, "bad primaries must leave the luminance known");

    // A peak of 5 to 10000 cd/m2.
    CHECK(parse_with(8, 5, 0).hdr_mastering.has_luminance, "a 5 cd/m2 peak must be known");
    CHECK(!parse_with(8, 4, 0).hdr_mastering.has_luminance, "a peak below 5 cd/m2 must be unknown");
    CHECK(parse_with(8, 10000, 0).hdr_mastering.has_luminance, "a 10000 cd/m2 peak must be known");
    CHECK(!parse_with(8, 10001, 0).hdr_mastering.has_luminance, "a peak past 10000 cd/m2 must be unknown");
    CHECK(parse_with(8, 10001, 0).hdr_mastering.has_primaries, "a bad peak must leave the primaries known");

    // A floor of at most 5 cd/m2, below the peak.
    CHECK(parse_with(9, 50000, 0).hdr_mastering.has_luminance, "a 5 cd/m2 floor must be known");
    CHECK(!parse_with(9, 50001, 0).hdr_mastering.has_luminance, "a floor past 5 cd/m2 must be unknown");
    {
        uint8_t data[25];
        uint16_t values[12];
        Format format = video_format();
        MediaCodecDecContext s = {0};
        memcpy(values, shield, sizeof(values));
        values[8] = 5;
        values[9] = 50000;
        blob(data, 0, values);
        format.hdr = data;
        format.hdr_size = sizeof(data);
        parse(&s, &format);
        CHECK(!s.hdr_mastering.has_luminance, "a floor at the peak must be unknown");
    }

    // A zero floor, as real (Blu-ray) releases carry, unless strict.
    CHECK(parse_with(9, 0, FF_COMPLIANCE_NORMAL).hdr_mastering.has_luminance, "a zero floor must be known");
    CHECK(!parse_with(9, 0, FF_COMPLIANCE_STRICT).hdr_mastering.has_luminance,
          "a zero floor must be unknown under strict compliance");
    CHECK(parse_with(9, 1, FF_COMPLIANCE_STRICT).hdr_mastering.has_luminance,
          "a 0.0001 cd/m2 floor must be known under strict compliance");

    // The light levels are not range-checked, as FFmpeg's SEI export's are not.
    CHECK(parse_with(8, 0, 0).hdr_light.MaxCLL == 1000, "the light levels must not depend on the luminance");
    CHECK(buffers_out == 0, "every copied buffer must be freed");
}

static void attach_coded(AVCodecContext *avctx, AVMasteringDisplayMetadata *mastering, AVContentLightMetadata *light)
{
    avctx->coded_side_data[0] = (AVPacketSideData){(uint8_t *)mastering, sizeof(*mastering),
                                                   AV_PKT_DATA_MASTERING_DISPLAY_METADATA};
    avctx->coded_side_data[1] = (AVPacketSideData){(uint8_t *)light, sizeof(*light), AV_PKT_DATA_CONTENT_LIGHT_LEVEL};
    avctx->nb_coded_side_data = 2;
}

static int entries(const AVFrame *frame, enum AVFrameSideDataType type)
{
    int n = 0;
    for (int i = 0; i < frame->nb_side_data; i++)
        n += frame->side_data[i]->type == type;
    return n;
}

// The frame's one mastering display entry and its peak, or -1 when it has none
// or more than one.
static double peak(const AVFrame *frame)
{
    const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (!sd || entries(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) != 1)
        return -1;
    return av_q2d(((const AVMasteringDisplayMetadata *)sd->data)->max_luminance);
}

// The frame's one content light entry's MaxCLL, or -1.
static int max_cll(const AVFrame *frame)
{
    const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (!sd || entries(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) != 1)
        return -1;
    return (int)((const AVContentLightMetadata *)sd->data)->MaxCLL;
}

// An MP4's mdcv and clli boxes, unlike the stream's SEI.
static AVMasteringDisplayMetadata container_mastering = {
    .max_luminance = {4000, 1}, .min_luminance = {50, 10000}, .has_luminance = 1,
};
static AVContentLightMetadata container_light = {.MaxCLL = 2000, .MaxFALL = 400};

// A decoder context that has parsed the SHIELD's descriptor, or an all-zero one.
static void decoder_reports(MediaCodecDecContext *s, int known)
{
    static uint8_t data[25];
    static Format format;
    format = video_format();
    if (known)
        blob(data, 0, shield);
    else
        memset(data, 0, sizeof(data));
    format.hdr = data;
    format.hdr_size = sizeof(data);
    parse(s, &format);
}

static void test_frame_tagging(void)
{
    MediaCodecDecContext s = {0};
    AVCodecContext avctx = {0};
    AVFrame frame = {0};
    int prefer_mastering[] = {AV_PKT_DATA_MASTERING_DISPLAY_METADATA};
    int prefer_all[] = {-1};

    // Nothing known anywhere: an untouched frame.
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 1) == 0, "export must succeed");
    CHECK(!frame.nb_side_data, "nothing known must attach nothing");

    // A surface frame gets the container's when the decoder knows nothing:
    // ff_get_buffer never runs for it.
    attach_coded(&avctx, &container_mastering, &container_light);
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 1) == 0, "export must succeed");
    CHECK(peak(&frame) == 4000.0, "a surface frame must carry the container's mastering display");
    CHECK(max_cll(&frame) == 2000, "and its content light");
    av_frame_unref(&frame);

    // What the decoder reports replaces the container's, as FFmpeg's own
    // decoders' SEI does, once per type.
    decoder_reports(&s, 1);
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 1) == 0, "export must succeed");
    CHECK(peak(&frame) == 1000.0, "the decoder's mastering display must replace the container's, once");
    CHECK(max_cll(&frame) == 1000, "the decoder's content light must replace the container's, once");
    av_frame_unref(&frame);

    // Unless the caller prefers the container's, type by type.
    avctx.side_data_prefer_packet = prefer_mastering;
    avctx.nb_side_data_prefer_packet = 1;
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 1) == 0, "export must succeed");
    CHECK(peak(&frame) == 4000.0, "a preferred container mastering display must stay, once");
    CHECK(max_cll(&frame) == 1000, "an unpreferred container content light must still give way");
    av_frame_unref(&frame);
    avctx.side_data_prefer_packet = prefer_all;
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 1) == 0, "export must succeed");
    CHECK(peak(&frame) == 4000.0 && max_cll(&frame) == 2000, "-1 must prefer every container type");
    av_frame_unref(&frame);

    // A preference for something the container lacks takes nothing away.
    avctx.nb_coded_side_data = 0;
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 1) == 0, "export must succeed");
    CHECK(peak(&frame) == 1000.0 && max_cll(&frame) == 1000,
          "without the container's, the decoder's must be attached whatever the preference");
    av_frame_unref(&frame);
    avctx.nb_side_data_prefer_packet = 0;
    attach_coded(&avctx, &container_mastering, &container_light);

    // A copied frame already holds the container's from ff_get_buffer; the
    // decoder's replaces it and nothing is added twice.
    ff_get_buffer(&avctx, &frame, 0);
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 0) == 0, "export must succeed");
    CHECK(peak(&frame) == 1000.0 && max_cll(&frame) == 1000, "a copied frame must carry the decoder's, once");
    av_frame_unref(&frame);

    // A copied frame whose decoder reports nothing keeps ff_get_buffer's and
    // gains nothing more.
    decoder_reports(&s, 0);
    ff_get_buffer(&avctx, &frame, 0);
    CHECK(mediacodec_dec_export_hdr(&avctx, &s, &frame, 0) == 0, "export must succeed");
    CHECK(peak(&frame) == 4000.0 && max_cll(&frame) == 2000,
          "an unknown decoder report must leave ff_get_buffer's container metadata alone");
    av_frame_unref(&frame);
}

static MediaCodecDecContext surface_decoder(AVCodecContext *avctx)
{
    MediaCodecDecContext s = {0};
    decoder_reports(&s, 1);
    s.avctx = avctx;
    atomic_init(&s.serial, 3);
    atomic_init(&s.hw_buffer_count, 0);
    memset(&codec_log, 0, sizeof(codec_log));
    return s;
}

// mediacodec_wrap_hw_buffer: the frame that holds a codec output buffer for
// the surface, which only its owner may give back.
static void test_surface_wrap(void)
{
    AVCodecContext avctx = {.width = 3840, .height = 2160};
    FFAMediaCodecBufferInfo info = {.presentationTimeUs = 41708};
    AVMediaCodecBufferTiming timing = {0};
    AVFrame frame = {0};
    MediaCodecDecContext s;

    attach_coded(&avctx, &container_mastering, &container_light);
    s = surface_decoder(&avctx);
    CHECK(mediacodec_wrap_hw_buffer(&avctx, &s, 7, &info, &timing, &frame) == 0, "wrapping must succeed");
    CHECK(peak(&frame) == 1000.0 && max_cll(&frame) == 1000, "a surface frame must carry the decoder's, once");
    CHECK(frame.buf[0] && atomic_load(&s.hw_buffer_count) == 1 && s.refcount == 1 && !codec_log.releases,
          "the frame must hold the output buffer");
    av_frame_unref(&frame);
    CHECK(codec_log.releases == 1 && !codec_log.rendered && codec_log.last_index == 7,
          "dropping the frame must give the buffer back once, unrendered");
    CHECK(atomic_load(&s.hw_buffer_count) == 0 && s.refcount == 0, "and settle the decoder's counts");

    // Side data that cannot be allocated fails the frame, and the output
    // buffer it already holds goes back exactly once.
    for (int budget = 0; budget < 2; budget++) {
        s = surface_decoder(&avctx);
        side_data_budget = budget;
        CHECK(mediacodec_wrap_hw_buffer(&avctx, &s, 9, &info, &timing, &frame) == AVERROR(ENOMEM),
              "allocation failure %d must fail the frame", budget);
        side_data_budget = -1;
        CHECK(!frame.buf[0] && !frame.nb_side_data, "a failed frame must hold nothing");
        CHECK(codec_log.releases == 1 && !codec_log.rendered && codec_log.last_index == 9,
              "allocation failure %d must give the buffer back once, unrendered, got %d release(s)", budget,
              codec_log.releases);
        CHECK(codec_log.released_calls == 1, "and tell the decoder it came back");
        CHECK(atomic_load(&s.hw_buffer_count) == 0 && s.refcount == 0,
              "allocation failure %d must settle the decoder's counts, got %d buffer(s) and %d ref(s)", budget,
              atomic_load(&s.hw_buffer_count), s.refcount);
    }
}

// mediacodec_wrap_sw_video_buffer: a frame copied out of a ByteBuffer.
static void test_copied_wrap(void)
{
    AVCodecContext avctx = {.width = 3840, .height = 2160};
    FFAMediaCodecBufferInfo info = {.presentationTimeUs = 41708};
    uint8_t pixels[16] = {0};
    AVFrame frame = {0};
    MediaCodecDecContext s;

    attach_coded(&avctx, &container_mastering, &container_light);
    s = surface_decoder(&avctx);
    s.color_format = COLOR_FormatYUV420SemiPlanar;
    CHECK(mediacodec_wrap_sw_video_buffer(&avctx, &s, pixels, sizeof(pixels), 4, &info, &frame) == 0,
          "copying must succeed");
    CHECK(peak(&frame) == 1000.0 && max_cll(&frame) == 1000, "a copied frame must carry the decoder's, once");
    CHECK(codec_log.copies == 1 && codec_log.releases == 1 && !codec_log.rendered,
          "the buffer must be copied and given back");
    av_frame_unref(&frame);

    s = surface_decoder(&avctx);
    s.color_format = COLOR_FormatYUV420SemiPlanar;
    side_data_budget = 0;
    CHECK(mediacodec_wrap_sw_video_buffer(&avctx, &s, pixels, sizeof(pixels), 4, &info, &frame) == AVERROR(ENOMEM),
          "an allocation failure must fail the frame");
    side_data_budget = -1;
    CHECK(codec_log.releases == 1 && !codec_log.copies, "a failed copy must still give the buffer back, once");
    av_frame_unref(&frame);
}

int main(void)
{
    test_reported_values_and_units();
    test_unknown_and_malformed();
    test_sei_ranges();
    test_frame_tagging();
    test_surface_wrap();
    test_copied_wrap();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: MediaCodec hdr-static-info parsing and ranges, frame tagging, and both frame wrappers");
    return 0;
}
