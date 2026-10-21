// Behavioral regressions for find_dolby_vision_profile5_rpu, compiled verbatim
// from patch 0004 by test_avf_dovi_profile5_rpu.sh: which software-decoded
// frames vo_avfoundation uploads full range with their RPU attached. Only a
// profile 5 frame may be picked. Every other frame, profile 8 included, has to
// keep the upload it had before, so a wrong pick changes a picture that is
// correct today. mpv, libplacebo and libavutil are stubbed with just the
// fields the selector reads.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum pl_color_system {
    PL_COLOR_SYSTEM_UNKNOWN,
    PL_COLOR_SYSTEM_BT_2020_NC,
    PL_COLOR_SYSTEM_DOLBYVISION,
};

enum AVFrameSideDataType {
    AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
    AV_FRAME_DATA_DOVI_RPU_BUFFER,
    AV_FRAME_DATA_DOVI_METADATA,
};

typedef struct AVBufferRef {
    uint8_t *data;
    size_t size;
} AVBufferRef;

// libavutil/dovi_meta.h: the header lives header_offset bytes into the
// metadata allocation.
typedef struct AVDOVIRpuDataHeader {
    uint8_t vdr_rpu_profile;
    uint8_t bl_video_full_range_flag;
} AVDOVIRpuDataHeader;

typedef struct AVDOVIMetadata {
    size_t header_offset;
    size_t mapping_offset;
    size_t color_offset;
    size_t ext_block_offset;
    size_t ext_block_size;
    int num_ext_blocks;
} AVDOVIMetadata;

static AVDOVIRpuDataHeader *av_dovi_get_header(const AVDOVIMetadata *data)
{
    return (AVDOVIRpuDataHeader *)((uint8_t *)data + data->header_offset);
}

struct mp_ff_side_data {
    int type;
    AVBufferRef *buf;
};

typedef struct mp_image {
    struct {
        struct {
            enum pl_color_system sys;
        } repr;
    } params;
    struct mp_ff_side_data *ff_side_data;
    int num_ff_side_data;
} mp_image_t;

#include "dovi_profile5_rpu.inc"

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

struct dovi_metadata {
    AVDOVIMetadata meta;
    AVDOVIRpuDataHeader header;
};

// The metadata FFmpeg's HEVC decoder exports for a stream whose RPU header
// carries this profile and base-layer range.
static AVBufferRef metadata_buffer(struct dovi_metadata *storage, int profile, bool full_range)
{
    *storage = (struct dovi_metadata){
        .meta.header_offset = offsetof(struct dovi_metadata, header),
        .header.vdr_rpu_profile = profile,
        .header.bl_video_full_range_flag = full_range,
    };
    return (AVBufferRef){(uint8_t *)storage, sizeof(*storage)};
}

static uint8_t rpu_bytes[246];

static mp_image_t frame(enum pl_color_system sys, struct mp_ff_side_data *sd, int n)
{
    mp_image_t mpi = {.ff_side_data = sd, .num_ff_side_data = n};
    mpi.params.repr.sys = sys;
    return mpi;
}

int main(void)
{
    struct dovi_metadata storage;
    AVBufferRef rpu = {rpu_bytes, sizeof(rpu_bytes)};
    uint8_t hdr10plus_bytes[32] = {0};
    AVBufferRef hdr10plus = {hdr10plus_bytes, sizeof(hdr10plus_bytes)};

    // Profile 5: RPU profile 0 over a full-range base layer. The RPU handed
    // back is the decoder's own buffer, not a copy or a neighbour.
    AVBufferRef p5 = metadata_buffer(&storage, 0, true);
    struct mp_ff_side_data sd[] = {
        {AV_FRAME_DATA_DOVI_METADATA, &p5},
        {AV_FRAME_DATA_DOVI_RPU_BUFFER, &rpu},
    };
    mp_image_t mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, sd, 2);
    check(find_dolby_vision_profile5_rpu(&mpi) == &rpu, "a profile 5 frame yields its RPU");

    // Side data order is the decoder's, and other side data sits between.
    struct mp_ff_side_data mixed[] = {
        {AV_FRAME_DATA_DOVI_RPU_BUFFER, &rpu},
        {AV_FRAME_DATA_DYNAMIC_HDR_PLUS, &hdr10plus},
        {AV_FRAME_DATA_DOVI_METADATA, &p5},
    };
    mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, mixed, 3);
    check(find_dolby_vision_profile5_rpu(&mpi) == &rpu,
          "a profile 5 frame yields its RPU whatever the side-data order");

    // Profile 8 (RPU profile 1, limited range) presents its HDR10/HLG base
    // layer as before; a full-range profile 1 stream is not profile 5 either.
    AVBufferRef p8 = metadata_buffer(&storage, 1, false);
    sd[0].buf = &p8;
    mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, sd, 2);
    check(!find_dolby_vision_profile5_rpu(&mpi), "a profile 8 frame keeps its upload");
    AVBufferRef p1_full = metadata_buffer(&storage, 1, true);
    sd[0].buf = &p1_full;
    check(!find_dolby_vision_profile5_rpu(&mpi), "RPU profile 1 is not profile 5 at any range");

    // RPU profile 0 over a limited-range base layer is no profile FFmpeg
    // recognises, so it is left alone too.
    AVBufferRef p0_limited = metadata_buffer(&storage, 0, false);
    sd[0].buf = &p0_limited;
    check(!find_dolby_vision_profile5_rpu(&mpi), "a limited-range RPU profile 0 frame keeps its upload");

    // A frame mpv does not present as Dolby Vision is never reshaped, whatever
    // side data it still carries.
    p5 = metadata_buffer(&storage, 0, true);
    sd[0].buf = &p5;
    mpi = frame(PL_COLOR_SYSTEM_BT_2020_NC, sd, 2);
    check(!find_dolby_vision_profile5_rpu(&mpi), "a non-Dolby Vision frame keeps its upload");

    // Without both the metadata and a non-empty RPU there is nothing to
    // decide on or attach.
    mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, &sd[0], 1);
    check(!find_dolby_vision_profile5_rpu(&mpi), "a frame without an RPU keeps its upload");
    mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, &sd[1], 1);
    check(!find_dolby_vision_profile5_rpu(&mpi), "a frame without Dolby Vision metadata keeps its upload");
    mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, NULL, 0);
    check(!find_dolby_vision_profile5_rpu(&mpi), "a frame without side data keeps its upload");
    AVBufferRef empty_rpu = {rpu_bytes, 0};
    sd[1].buf = &empty_rpu;
    mpi = frame(PL_COLOR_SYSTEM_DOLBYVISION, sd, 2);
    check(!find_dolby_vision_profile5_rpu(&mpi), "an empty RPU is not attached");
    sd[1].buf = &rpu;

    // Metadata too short to hold its own offsets is not read.
    AVBufferRef short_meta = {(uint8_t *)&storage, sizeof(AVDOVIMetadata) - 1};
    sd[0].buf = &short_meta;
    check(!find_dolby_vision_profile5_rpu(&mpi), "truncated Dolby Vision metadata is not read");

    if (failures)
        return 1;
    printf("avfoundation Dolby Vision profile 5 RPU selection: all checks passed\n");
    return 0;
}
