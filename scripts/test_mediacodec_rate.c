// Behavioral regressions for the production media3_rate path extracted by
// test_mediacodec_rate.sh: with the option on, the decoder sets the operating
// rate itself once, after its first frame, and the host's operating-rate call
// leaves it alone. The configure keys it omits are in mediacodec_decode_init,
// which needs a codec; the decoder logs them. The codec, the formats and the
// receive path are fakes recording what reaches the platform.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR(e) (-(e))
#define ENOMEM 12
#define EINVAL 22
#define ENOSYS 38
#define EAGAIN 11
#define AVERROR_EOF (-0x20464f45)
#define AV_LOG_WARNING 24
#define AV_LOG_INFO 32
#define AV_LOG_VERBOSE 40

typedef struct AVFrame { int unused; } AVFrame;
typedef struct AVCodecContext { void *priv_data; } AVCodecContext;
typedef struct FFAMediaCodec { int unused; } FFAMediaCodec;
typedef struct FFAMediaFormat {
    int operating_rate; // -1 while unset
} FFAMediaFormat;

typedef struct MediaCodecDecContext {
    FFAMediaCodec *codec;
    bool use_ndk_codec;
    bool media3_rate;
} MediaCodecDecContext;

typedef struct MediaCodecH264DecContext {
    MediaCodecDecContext *ctx;
    int use_ndk_codec;
    float frame_rate;
    int media3_rate;
    int media3_rate_pending;
} MediaCodecH264DecContext;

static int infos;
static void av_log(void *avctx, int level, const char *fmt, ...)
{
    if (level <= AV_LOG_INFO)
        infos++;
}

// Platform fakes: every setParameters call and the format it carried.
static FFAMediaFormat params_object;
static int formats_live, last_format_ndk;
static int set_parameters_calls, set_parameters_rate, set_parameters_ret;

static FFAMediaFormat *ff_AMediaFormat_new(int ndk)
{
    formats_live++;
    last_format_ndk = ndk;
    params_object.operating_rate = -1;
    return &params_object;
}
static void ff_AMediaFormat_setInt32(FFAMediaFormat *format, const char *name, int32_t value)
{
    if (!strcmp(name, "operating-rate"))
        format->operating_rate = value;
}
static void ff_AMediaFormat_delete(FFAMediaFormat *format)
{
    formats_live--;
}
static int ff_AMediaCodec_setParameters(FFAMediaCodec *codec, const FFAMediaFormat *params)
{
    set_parameters_calls++;
    set_parameters_rate = params->operating_rate;
    return set_parameters_ret;
}

static MediaCodecDecContext *ff_mediacodec_dec_from_avctx(AVCodecContext *avctx)
{
    MediaCodecH264DecContext *s = avctx->priv_data;
    return s ? s->ctx : NULL;
}

// The real receive path, scripted: what each call answers.
static int receive_ret;
static int mediacodec_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    return receive_ret;
}

#include "mediacodec_rate.inc"

static void check(bool ok, const char *scenario)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", scenario);
        exit(1);
    }
}

static FFAMediaCodec codec;
static MediaCodecDecContext ctx;
static MediaCodecH264DecContext priv;
static AVCodecContext avctx;
static AVFrame frame;

static void open_decoder(bool media3_rate, float frame_rate, int ndk)
{
    ctx = (MediaCodecDecContext){.codec = &codec, .use_ndk_codec = ndk,
                                 .media3_rate = media3_rate};
    priv = (MediaCodecH264DecContext){.ctx = &ctx, .use_ndk_codec = ndk,
                                      .frame_rate = frame_rate,
                                      .media3_rate = media3_rate,
                                      .media3_rate_pending = media3_rate};
    avctx = (AVCodecContext){.priv_data = &priv};
    set_parameters_calls = set_parameters_ret = infos = 0;
    set_parameters_rate = -1;
}

static void test_rate_follows_the_first_frame_once(void)
{
    open_decoder(true, 23.976f, 1);
    receive_ret = AVERROR(EAGAIN);
    check(mediacodec_receive_frame_cb(&avctx, &frame) == AVERROR(EAGAIN),
          "a receive without a frame answers as the decoder did");
    receive_ret = AVERROR_EOF;
    check(mediacodec_receive_frame_cb(&avctx, &frame) == AVERROR_EOF,
          "an error answers as the decoder did");
    check(set_parameters_calls == 0, "nothing is set before a frame is out");

    receive_ret = 0;
    check(mediacodec_receive_frame_cb(&avctx, &frame) == 0, "the first frame is returned");
    check(set_parameters_calls == 1 && set_parameters_rate == 24,
          "the first frame sets the operating rate to the stream's, rounded");
    check(last_format_ndk == 1 && formats_live == 0,
          "the parameters use the codec's API and are freed");
    for (int i = 0; i < 5; i++)
        mediacodec_receive_frame_cb(&avctx, &frame);
    check(set_parameters_calls == 1, "later frames set nothing");
}

static void test_host_rate_is_ignored_only_under_media3_rate(void)
{
    open_decoder(true, 24, 0);
    check(av_mediacodec_set_operating_rate(&avctx, 60) == 0,
          "the host's rate is answered as taken");
    check(set_parameters_calls == 0, "the host's rate never reaches the codec");

    open_decoder(false, 24, 0);
    check(av_mediacodec_set_operating_rate(&avctx, 60) == 0 &&
          set_parameters_calls == 1 && set_parameters_rate == 60,
          "without the option the host's rate reaches the codec");
    receive_ret = 0;
    mediacodec_receive_frame_cb(&avctx, &frame);
    check(set_parameters_calls == 1, "without the option a frame sets nothing");
}

static void test_no_frame_rate_or_a_refusal_ends_the_attempt(void)
{
    open_decoder(true, 0, 0);
    receive_ret = 0;
    mediacodec_receive_frame_cb(&avctx, &frame);
    check(set_parameters_calls == 0 && !priv.media3_rate_pending && infos == 1,
          "with no frame rate the operating rate stays unset, and says so once");
    mediacodec_receive_frame_cb(&avctx, &frame);
    check(infos == 1, "the attempt is not repeated");

    open_decoder(true, 25, 0);
    set_parameters_ret = AVERROR(EINVAL);
    check(mediacodec_receive_frame_cb(&avctx, &frame) == 0,
          "a refused rate does not cost the frame");
    mediacodec_receive_frame_cb(&avctx, &frame);
    check(set_parameters_calls == 1 && formats_live == 0,
          "a refused rate is tried once and its parameters freed");
}

int main(void)
{
    test_rate_follows_the_first_frame_once();
    test_host_rate_is_ignored_only_under_media3_rate();
    test_no_frame_rate_or_a_refusal_ends_the_attempt();
    puts("PASS: media3_rate sets the operating rate once, after the first frame, "
         "and keeps the host's rate from the codec");
    return 0;
}
