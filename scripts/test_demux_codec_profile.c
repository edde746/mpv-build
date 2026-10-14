// Behavioral regression for the codec profile the demuxers publish at file
// load (patch 0204), extracted by test_demux_codec_profile.sh.
//
// Tortuga and Plezy decide the decode path in on_preloaded, before mpv creates
// a decoder: H.264 High 10 or HEVC Main 10 goes to software on a device whose
// MediaCodec has no hardware decoder for that profile. demux_mkv (CodecPrivate)
// and demux_lavf (MP4 stsd) name the profile from the stream's decoder
// configuration record; mpv's decoder wrapper replaces it with the decoder's
// own answer once a frame comes out. Without an HEVC profile at load a Main 10
// track reached the hardware decoder, failed (MediaCodec error 14 on the
// Android TV emulator) and only then fell back.
//
// What must hold: for every sample, the profile at load is the one the pinned
// software decoder reports after decoding the stream's first access unit, and
// it is the profile the encode was made to have. A record that is not the one
// the codec's decoder configuration format defines names no profile rather
// than a wrong one.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

// The fields of mpv's struct mp_codec_params (demux/stheader.h) that
// mp_codec_info_from_av() fills.
struct mp_codec_params {
    const char *codec, *codec_desc, *codec_profile, *decoder, *decoder_desc;
};

#include "codec_profile.inc"
#include "test_demux_codec_profile_samples.h"

static int failures;

static void expect(const char *what, const char *got, const char *want)
{
    if (got == want || (got && want && strcmp(got, want) == 0))
        return;
    printf("FAIL: %s: %s, want %s\n", what, got ? got : "(none)", want ? want : "(none)");
    failures++;
}

// The profile mpv publishes once the decoder has output the first frame.
static const char *decoded_profile(const char *codec, const uint8_t *record, int record_size,
                                   const uint8_t *packet, int packet_size)
{
    const AVCodec *decoder = avcodec_find_decoder_by_name(codec);
    AVCodecContext *avctx = decoder ? avcodec_alloc_context3(decoder) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!avctx || !pkt || !frame) {
        fprintf(stderr, "no %s decoder in the oracle build\n", codec);
        exit(2);
    }
    avctx->extradata = av_mallocz(record_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(avctx->extradata, record, record_size);
    avctx->extradata_size = record_size;
    struct mp_codec_params params = {0};
    if (avcodec_open2(avctx, decoder, NULL) == 0 && av_new_packet(pkt, packet_size) == 0) {
        memcpy(pkt->data, packet, packet_size);
        if (avcodec_send_packet(avctx, pkt) == 0 && avcodec_send_packet(avctx, NULL) == 0 &&
            avcodec_receive_frame(avctx, frame) == 0)
            mp_codec_info_from_av(avctx, &params);
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&avctx);
    return params.codec_profile;
}

// The record and first access unit of a sample, checked both ways.
static void check(const char *name, const char *codec, const char *want, const uint8_t *record,
                  int record_size, const uint8_t *packet, int packet_size)
{
    char what[128];
    snprintf(what, sizeof(what), "%s at load", name);
    expect(what, mp_codec_profile_from_extradata(codec, record, record_size), want);
    snprintf(what, sizeof(what), "%s decoded", name);
    expect(what, decoded_profile(codec, record, record_size, packet, packet_size), want);
}

// HEVC NAL unit type of a NAL unit's first byte.
static int hevc_nal_type(const uint8_t *nal)
{
    return (nal[0] >> 1) & 0x3f;
}

// An encoder that writes general_profile_idc 0 and signals the profile only
// through general_profile_compatibility_flags: the record's header, and the
// VPS and SPS in its parameter set arrays (the decoder reads those), get
// profile_idc 0. The access unit must carry no parameter sets of its own, or
// the decoder would read those instead.
static void clear_hevc_profile_idc(uint8_t *record, int size, const uint8_t *packet, int packet_size)
{
    for (int pos = 0; pos + 4 < packet_size;) {
        int len = packet[pos] << 24 | packet[pos + 1] << 16 | packet[pos + 2] << 8 | packet[pos + 3];
        int type = hevc_nal_type(packet + pos + 4);
        if (type >= 32 && type <= 34) {
            fprintf(stderr, "sample access unit carries parameter set %d\n", type);
            exit(2);
        }
        pos += 4 + len;
    }
    record[1] &= ~0x1f;
    int pos = 23;
    for (int arrays = record[22]; arrays > 0; arrays--) {
        int nalus = record[pos + 1] << 8 | record[pos + 2];
        pos += 3;
        for (; nalus > 0; nalus--) {
            int len = record[pos] << 8 | record[pos + 1];
            uint8_t *nal = record + pos + 2;
            // profile_tier_level follows the VPS's first 32 bits and the SPS's
            // first 8; its first byte ends in general_profile_idc. Neither is
            // preceded by an emulation prevention byte in these samples.
            if (hevc_nal_type(nal) == 32)
                nal[2 + 4] &= ~0x1f;
            if (hevc_nal_type(nal) == 33)
                nal[2 + 1] &= ~0x1f;
            pos += 2 + len;
        }
    }
    if (pos != size) {
        fprintf(stderr, "hvcC parameter set arrays end at %d of %d bytes\n", pos, size);
        exit(2);
    }
}

#define MAX_RECORD 256

int main(void)
{
    av_log_set_level(AV_LOG_QUIET);
    const int count = sizeof(samples) / sizeof(samples[0]);

    // 1. Every sample, from either container: the profile at load is the
    // profile the encode was made to have and the decoder's own answer.
    for (int n = 0; n < count; n++) {
        const struct sample *s = &samples[n];
        if (s->record_size > MAX_RECORD) {
            fprintf(stderr, "%s: record over %d bytes\n", s->name, MAX_RECORD);
            exit(2);
        }
        check(s->name, s->codec, s->profile, s->record, s->record_size, s->packet,
              s->packet_size);
    }

    // 2. HEVC general_profile_idc 0: the decoder takes the first profile the
    // compatibility flags name, and so does the profile at load.
    for (int n = 0; n < count; n++) {
        const struct sample *s = &samples[n];
        if (strcmp(s->codec, "hevc") != 0)
            continue;
        uint8_t record[MAX_RECORD];
        memcpy(record, s->record, s->record_size);
        clear_hevc_profile_idc(record, s->record_size, s->packet, s->packet_size);
        char name[96];
        snprintf(name, sizeof(name), "%s with profile_idc 0", s->name);
        check(name, s->codec, s->profile, record, s->record_size, s->packet, s->packet_size);
    }

    // 3. What is not the codec's configuration record names no profile.
    for (int n = 0; n < count; n++) {
        const struct sample *s = &samples[n];
        uint8_t record[MAX_RECORD];
        char what[128];
        memcpy(record, s->record, s->record_size);

        // A record whose configurationVersion is not 1 (HEVC's pre-standard
        // drafts laid the header out differently).
        record[0] = 0;
        snprintf(what, sizeof(what), "%s with configurationVersion 0", s->name);
        expect(what, mp_codec_profile_from_extradata(s->codec, record, s->record_size), NULL);
        record[0] = 1;

        // Shorter than the record's fixed header: avcC's 7 bytes, hvcC's 23.
        int header = strcmp(s->codec, "hevc") == 0 ? 23 : 7;
        snprintf(what, sizeof(what), "%s cut to %d bytes", s->name, header - 1);
        expect(what, mp_codec_profile_from_extradata(s->codec, record, header - 1), NULL);
        snprintf(what, sizeof(what), "%s cut to %d bytes", s->name, header);
        expect(what, mp_codec_profile_from_extradata(s->codec, record, header), s->profile);

        // Annex B parameter sets instead of a record: a start code where the
        // record's version and profile bytes would be.
        uint8_t annexb[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0, 0, 0,
                            0x90, 0, 0, 0, 0, 0, 0x5d, 0x95, 0x98, 0x09};
        snprintf(what, sizeof(what), "%s Annex B extradata", s->codec);
        expect(what, mp_codec_profile_from_extradata(s->codec, annexb, sizeof(annexb)), NULL);

        // Another codec's record, no codec, no extradata.
        snprintf(what, sizeof(what), "%s record as vp9", s->name);
        expect(what, mp_codec_profile_from_extradata("vp9", s->record, s->record_size), NULL);
        snprintf(what, sizeof(what), "%s record without a codec", s->name);
        expect(what, mp_codec_profile_from_extradata(NULL, s->record, s->record_size), NULL);
        snprintf(what, sizeof(what), "%s without extradata", s->codec);
        expect(what, mp_codec_profile_from_extradata(s->codec, NULL, 0), NULL);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("demux codec profile: %d samples, at load matches the decoder\n", count);
    return 0;
}
