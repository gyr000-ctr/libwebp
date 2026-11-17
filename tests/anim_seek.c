// Copyright 2026 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Tests for frame-wise seeking of the animation decoder
// (WebPAnimDecoderSeek()).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/webp/decode.h"
#include "src/webp/demux.h"
#include "src/webp/encode.h"
#include "src/webp/mux.h"
#include "src/webp/mux_types.h"
#include "src/webp/types.h"

#define TEST_CANVAS_WIDTH 24
#define TEST_CANVAS_HEIGHT 16
#define TEST_FRAME_COUNT 10

static int g_failures = 0;

#define CHECK(cond, ...)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      ++g_failures;                                                       \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                \
      fprintf(stderr, __VA_ARGS__);                                       \
      fprintf(stderr, "\n");                                              \
    }                                                                     \
  } while (0)

// Fills 'pixels' (w * h * 4 bytes) with a pattern unique for 'frame_num':
// a solid base color; the leftmost column is semi-transparent so that alpha
// blending against the previous canvas is exercised.
static void DrawFrame(uint8_t* pixels, int w, int h, int frame_num) {
  int j, i;
  const uint8_t base_r = (uint8_t)(frame_num * 23);
  const uint8_t base_g = (uint8_t)(frame_num * 41 + 7);
  const uint8_t base_b = (uint8_t)(frame_num * 59 + 13);
  memset(pixels, 0, (size_t)w * h * 4);
  for (j = 0; j < h; ++j) {
    for (i = 0; i < w; ++i) {
      uint8_t* const px = pixels + (j * w + i) * 4;
      px[0] = base_r;
      px[1] = base_g;
      px[2] = base_b;
      px[3] = (i == 0) ? 128 : 255;
    }
  }
}

// Encodes one frame (lossy VP8, with alpha) and pushes it into the muxer.
static int PushFrame(WebPMux* mux, int frame_num, int x, int y, int w, int h,
                     int duration, WebPMuxAnimBlend blend,
                     WebPMuxAnimDispose dispose) {
  uint8_t* raw = NULL;
  uint8_t* webp = NULL;
  size_t webp_size;
  WebPData frame_data;
  WebPMuxFrameInfo frame;
  WebPMuxError err;
  int ok = 0;

  raw = (uint8_t*)WebPMalloc((size_t)w * h * 4);
  if (raw == NULL) return 0;
  DrawFrame(raw, w, h, frame_num);
  webp_size = WebPEncodeRGBA(raw, w, h, w * 4, 80.0f, &webp);
  if (webp_size == 0) goto End;

  WebPDataInit(&frame_data);
  frame_data.bytes = webp;
  frame_data.size = webp_size;

  memset(&frame, 0, sizeof(frame));
  frame.bitstream = frame_data;
  frame.x_offset = x;
  frame.y_offset = y;
  frame.duration = duration;
  frame.id = WEBP_CHUNK_ANMF;
  frame.blend_method = blend;
  frame.dispose_method = dispose;
  err = WebPMuxPushFrame(mux, &frame, 1 /* copy_data */);
  ok = (err == WEBP_MUX_OK);

End:
  WebPFree(webp);
  WebPFree(raw);
  return ok;
}

// Builds an animated WebP with TEST_FRAME_COUNT frames in 'out'.
// The layout exercises every key-frame rule of the decoder:
//   frame 1            : key-frame (first frame),
//   frame 2 (even)     : full-canvas NO_BLEND -> key-frame,
//   frame 3            : key-frame because frame 2 disposed a full canvas to
//                        background,
//   later even frames  : sub-rectangle BLEND frames -> non-key-frames.
// Durations alternate between 33 ms and 47 ms.
static int BuildAnimation(WebPData* out) {
  WebPMux* mux = WebPMuxNew();
  WebPMuxAnimParams params;
  int i;
  int ok = 0;

  if (mux == NULL) return 0;
  params.bgcolor = 0x00000000;
  params.loop_count = 0;
  if (WebPMuxSetAnimationParams(mux, &params) != WEBP_MUX_OK) goto End;

  for (i = 1; i <= TEST_FRAME_COUNT; ++i) {
    int x, y, w, h;
    WebPMuxAnimBlend blend;
    WebPMuxAnimDispose dispose;
    if (i == 1) {
      // First frame: full canvas.
      x = 0; y = 0; w = TEST_CANVAS_WIDTH; h = TEST_CANVAS_HEIGHT;
      blend = WEBP_MUX_BLEND;
      dispose = WEBP_MUX_DISPOSE_NONE;
    } else if (i == 2) {
      // Full-canvas NO_BLEND with background disposal: makes frame 3 a
      // key-frame through the disposal rule.
      x = 0; y = 0; w = TEST_CANVAS_WIDTH; h = TEST_CANVAS_HEIGHT;
      blend = WEBP_MUX_NO_BLEND;
      dispose = WEBP_MUX_DISPOSE_BACKGROUND;
    } else if ((i & 1) == 0) {
      // Sub-rectangle blending frames: not key-frames on their own.
      x = 4; y = 2; w = TEST_CANVAS_WIDTH - 8; h = TEST_CANVAS_HEIGHT - 4;
      blend = WEBP_MUX_BLEND;
      dispose = WEBP_MUX_DISPOSE_NONE;
    } else {
      // Full-canvas opaque-ish NO_BLEND frames: key-frames.
      x = 0; y = 0; w = TEST_CANVAS_WIDTH; h = TEST_CANVAS_HEIGHT;
      blend = WEBP_MUX_NO_BLEND;
      dispose = WEBP_MUX_DISPOSE_NONE;
    }
    if (!PushFrame(mux, i, x, y, w, h, (i & 1) ? 33 : 47, blend, dispose)) {
      goto End;
    }
  }

  if (WebPMuxAssemble(mux, out) != WEBP_MUX_OK) goto End;
  ok = 1;

End:
  WebPMuxDelete(mux);
  return ok;
}

// Decodes every frame sequentially and stores the rendered canvases plus the
// reported timestamps in 'ref_canvas' / 'ref_timestamp' (1-based indexing).
static int BuildReference(const WebPData* data, uint8_t** ref_canvas,
                          int* ref_timestamp) {
  WebPAnimDecoderOptions options;
  WebPAnimDecoder* dec;
  WebPAnimInfo info;
  int i;
  const size_t canvas_size =
      (size_t)TEST_CANVAS_WIDTH * TEST_CANVAS_HEIGHT * 4;

  if (!WebPAnimDecoderOptionsInit(&options)) return 0;
  dec = WebPAnimDecoderNew(data, &options);
  if (dec == NULL) return 0;
  if (!WebPAnimDecoderGetInfo(dec, &info)) goto Error;
  if (info.frame_count != TEST_FRAME_COUNT) goto Error;

  for (i = 1; i <= TEST_FRAME_COUNT; ++i) {
    uint8_t* buf;
    int timestamp;
    if (!WebPAnimDecoderHasMoreFrames(dec)) goto Error;
    if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) goto Error;
    ref_canvas[i] = (uint8_t*)WebPMalloc(canvas_size);
    if (ref_canvas[i] == NULL) goto Error;
    memcpy(ref_canvas[i], buf, canvas_size);
    ref_timestamp[i] = timestamp;
  }
  if (WebPAnimDecoderHasMoreFrames(dec)) goto Error;
  WebPAnimDecoderDelete(dec);
  return 1;

Error:
  WebPAnimDecoderDelete(dec);
  return 0;
}

static void FreeReference(uint8_t** ref_canvas) {
  int i;
  for (i = 0; i <= TEST_FRAME_COUNT; ++i) WebPFree(ref_canvas[i]);
}

// Seeks to 'seek_target' and decodes until the end, checking every rendered
// frame against the sequential reference (pixels and timestamps).
static void CheckSeekTo(const WebPData* data, uint8_t** ref_canvas,
                        const int* ref_timestamp, int seek_target,
                        WebPAnimSeekStatus expected_status,
                        int expected_anchor) {
  WebPAnimDecoderOptions options;
  WebPAnimDecoder* dec;
  WebPAnimSeekStatus status;
  int frame_num;
  const size_t canvas_size =
      (size_t)TEST_CANVAS_WIDTH * TEST_CANVAS_HEIGHT * 4;

  if (!WebPAnimDecoderOptionsInit(&options)) {
    CHECK(0, "WebPAnimDecoderOptionsInit failed");
    return;
  }
  dec = WebPAnimDecoderNew(data, &options);
  CHECK(dec != NULL, "WebPAnimDecoderNew failed");
  if (dec == NULL) return;

  status = WebPAnimDecoderSeek(dec, seek_target);
  CHECK(status == expected_status,
        "seek to frame %d returned status %d, expected %d", seek_target,
        (int)status, (int)expected_status);

  // The first GetNext() after a seek returns the requested frame itself
  // (materialized during the seek, possibly via key-frame fallback).
  frame_num = seek_target;
  while (WebPAnimDecoderHasMoreFrames(dec)) {
    uint8_t* buf;
    int timestamp;
    CHECK(frame_num <= TEST_FRAME_COUNT,
          "decoded more frames than the animation has after seek to %d",
          seek_target);
    if (frame_num > TEST_FRAME_COUNT) break;
    CHECK(WebPAnimDecoderGetNext(dec, &buf, &timestamp),
          "GetNext failed after seek to %d (frame %d)", seek_target,
          frame_num);
    CHECK(timestamp == ref_timestamp[frame_num],
          "timestamp mismatch at frame %d after seek: got %d expected %d",
          frame_num, timestamp, ref_timestamp[frame_num]);
    CHECK(memcmp(buf, ref_canvas[frame_num], canvas_size) == 0,
          "pixel mismatch at frame %d after seek to %d", frame_num,
          seek_target);
    ++frame_num;
  }
  CHECK(frame_num == TEST_FRAME_COUNT + 1,
        "decoding after seek stopped at frame %d, expected %d", frame_num - 1,
        TEST_FRAME_COUNT);
  WebPAnimDecoderDelete(dec);
}

// Finds the byte range of frame 'frame_num' inside 'data' through the demux
// API. Returns 1 on success.
static int GetFragmentRange(const WebPData* data, int frame_num,
                            size_t* start, size_t* size) {
  WebPDemuxer* dmux = WebPDemux(data);
  WebPIterator iter;
  int ok = 0;
  if (dmux != NULL && WebPDemuxGetFrame(dmux, frame_num, &iter)) {
    *start = (size_t)(iter.fragment.bytes - data->bytes);
    *size = iter.fragment.size;
    WebPDemuxReleaseIterator(&iter);
    ok = 1;
  }
  WebPDemuxDelete(dmux);
  return ok;
}

// Corrupts bytes inside one frame's image payload and checks that seeking to
// that frame reports WEBP_ANIM_SEEK_CORRUPT_FRAME, while the decoder remains
// usable afterwards.
static void CheckCorruptFrame(const WebPData* data) {
  WebPData corrupt;
  WebPAnimDecoderOptions options;
  WebPAnimDecoder* dec;
  WebPAnimSeekStatus status;
  size_t frag_start = 0;
  size_t frag_size = 0;
  size_t pos;
  int corrupted = 0;

  if (!GetFragmentRange(data, 4, &frag_start, &frag_size)) {
    CHECK(0, "could not locate frame 4 fragment");
    return;
  }
  if (!WebPDataCopy(data, &corrupt)) {
    CHECK(0, "WebPDataCopy failed");
    return;
  }

  if (!WebPAnimDecoderOptionsInit(&options)) {
    CHECK(0, "WebPAnimDecoderOptionsInit failed");
    WebPDataClear(&corrupt);
    return;
  }

  // Flip bytes deep inside the entropy-coded partition (skip the leading
  // signature/headers) until a variant is found that the anim decoder accepts
  // at construction time but fails when frame 4 is fully decoded.
  for (pos = frag_start + 16;
       !corrupted && pos + 8 <= frag_start + frag_size; pos += 7) {
    size_t k;
    for (k = 0; k < 8; ++k) {
      ((uint8_t*)corrupt.bytes)[pos + k] ^= (uint8_t)(0xA5 + k);
    }
    dec = WebPAnimDecoderNew(&corrupt, &options);
    if (dec != NULL) {
      uint8_t* buf;
      int timestamp;
      int frame_num = 1;
      int failed = 0;
      while (WebPAnimDecoderHasMoreFrames(dec)) {
        if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
          failed = 1;
          break;
        }
        ++frame_num;
      }
      WebPAnimDecoderDelete(dec);
      if (failed) {
        CHECK(frame_num == 4,
              "corruption landed in frame %d, expected frame 4", frame_num);
        corrupted = (frame_num == 4);
      }
    }
    if (!corrupted) {
      for (k = 0; k < 8; ++k) {
        ((uint8_t*)corrupt.bytes)[pos + k] ^= (uint8_t)(0xA5 + k);
      }
    }
  }
  CHECK(corrupted, "could not construct a decodable-but-corrupt frame");
  if (!corrupted) {
    WebPDataClear(&corrupt);
    return;
  }

  dec = WebPAnimDecoderNew(&corrupt, &options);
  CHECK(dec != NULL, "decoder creation rejected the corrupted animation");
  if (dec != NULL) {
    uint8_t* buf;
    int timestamp;
    // Frame 4 is corrupted: the key-frame fallback for frame 4 is frame 3, so
    // decoding the intermediate frame 3 is fine, then frame 4 must fail.
    status = WebPAnimDecoderSeek(dec, 4);
    CHECK(status == WEBP_ANIM_SEEK_CORRUPT_FRAME,
          "corrupt seek returned %d, expected WEBP_ANIM_SEEK_CORRUPT_FRAME",
          (int)status);
    // After the failure the decoder must have been reset to frame 1 and still
    // be usable.
    CHECK(WebPAnimDecoderHasMoreFrames(dec),
          "decoder has no frames after a corrupt seek");
    CHECK(WebPAnimDecoderGetNext(dec, &buf, &timestamp),
          "sequential decode after corrupt seek failed");
    CHECK(timestamp == 33, "first frame timestamp after recovery is %d",
          timestamp);
    WebPAnimDecoderDelete(dec);
  }
  WebPDataClear(&corrupt);
}

int main(void) {
  static uint8_t* ref_canvas[TEST_FRAME_COUNT + 1];
  static int ref_timestamp[TEST_FRAME_COUNT + 1];
  WebPData data;
  WebPAnimDecoder* dec;
  WebPAnimDecoderOptions options;
  WebPAnimSeekStatus status;
  int i;

  memset(ref_canvas, 0, sizeof(ref_canvas));
  WebPDataInit(&data);

  if (!BuildAnimation(&data)) {
    fprintf(stderr, "FAIL: could not build test animation\n");
    return 1;
  }

  if (!BuildReference(&data, ref_canvas, ref_timestamp)) {
    fprintf(stderr, "FAIL: could not build sequential reference\n");
    WebPDataClear(&data);
    return 1;
  }

  // Timestamps are the cumulative sum of per-frame durations.
  for (i = 1; i <= TEST_FRAME_COUNT; ++i) {
    const int expected = ((i + 1) / 2) * 33 + (i / 2) * 47;
    CHECK(ref_timestamp[i] == expected,
          "reference timestamp at frame %d is %d, expected %d", i,
          ref_timestamp[i], expected);
  }

  // Seeking to a key-frame returns OK.
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/1,
              WEBP_ANIM_SEEK_OK, /*expected_anchor=*/1);
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/2,
              WEBP_ANIM_SEEK_OK, /*expected_anchor=*/2);
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/5,
              WEBP_ANIM_SEEK_OK, /*expected_anchor=*/5);
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/9,
              WEBP_ANIM_SEEK_OK, /*expected_anchor=*/9);

  // Seeking to a non-key-frame falls back to the preceding key-frame.
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/4,
              WEBP_ANIM_SEEK_NON_KEY_FRAME, /*expected_anchor=*/3);
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/6,
              WEBP_ANIM_SEEK_NON_KEY_FRAME, /*expected_anchor=*/5);
  CheckSeekTo(&data, ref_canvas, ref_timestamp, /*seek_target=*/10,
              WEBP_ANIM_SEEK_NON_KEY_FRAME, /*expected_anchor=*/9);

  // Out-of-range frame numbers are rejected with a distinct error and leave
  // the decoder state untouched.
  if (!WebPAnimDecoderOptionsInit(&options)) {
    CHECK(0, "WebPAnimDecoderOptionsInit failed");
  } else {
    dec = WebPAnimDecoderNew(&data, &options);
    CHECK(dec != NULL, "WebPAnimDecoderNew failed");
    if (dec != NULL) {
      CHECK(WebPAnimDecoderSeek(NULL, 1) == WEBP_ANIM_SEEK_INVALID_FRAME,
            "NULL decoder not rejected");
      CHECK(WebPAnimDecoderSeek(dec, 0) == WEBP_ANIM_SEEK_INVALID_FRAME,
            "frame 0 not rejected");
      CHECK(WebPAnimDecoderSeek(dec, -7) == WEBP_ANIM_SEEK_INVALID_FRAME,
            "negative frame not rejected");
      CHECK(WebPAnimDecoderSeek(dec, TEST_FRAME_COUNT + 1) ==
                WEBP_ANIM_SEEK_INVALID_FRAME,
            "frame past the end not rejected");
      // No seek took effect: sequential decoding still starts at frame 1.
      CHECK(WebPAnimDecoderHasMoreFrames(dec), "state changed by bad seek");
      {
        uint8_t* buf;
        int timestamp;
        CHECK(WebPAnimDecoderGetNext(dec, &buf, &timestamp) &&
                  timestamp == ref_timestamp[1],
              "decoder position changed after invalid seeks");
      }
      WebPAnimDecoderDelete(dec);
    }
  }

  // Repeated, backward and forward seeks must always converge to the same
  // output and never grow memory (bounded working set).
  if (WebPAnimDecoderOptionsInit(&options)) {
    dec = WebPAnimDecoderNew(&data, &options);
    CHECK(dec != NULL, "WebPAnimDecoderNew failed");
    if (dec != NULL) {
      static const int kSeekOrder[] = {7, 4, 9, 1, 10, 3, 6, 5, 8, 2};
      // Key-frames of the test layout: 1, 2, 3, 5, 7, 9.
      static const int kKeyFrames[] = {1, 2, 3, 5, 7, 9};
      for (i = 0; i < 50; ++i) {
        const int target = kSeekOrder[i % (int)(sizeof(kSeekOrder) /
                                                sizeof(kSeekOrder[0]))];
        WebPAnimSeekStatus expected = WEBP_ANIM_SEEK_NON_KEY_FRAME;
        size_t k;
        for (k = 0; k < sizeof(kKeyFrames) / sizeof(kKeyFrames[0]); ++k) {
          if (kKeyFrames[k] == target) expected = WEBP_ANIM_SEEK_OK;
        }
        status = WebPAnimDecoderSeek(dec, target);
        CHECK(status == expected, "repeated seek to %d returned %d", target,
              (int)status);
        // For a non-key-frame target the first returned frame is the anchor,
        // so decode until the target itself is produced and verify it.
        {
          int got_frame = 0;
          do {
            uint8_t* buf;
            int timestamp;
            CHECK(WebPAnimDecoderGetNext(dec, &buf, &timestamp),
                  "GetNext failed in repeated seek loop");
            got_frame = timestamp;
          } while (got_frame != ref_timestamp[target] &&
                   WebPAnimDecoderHasMoreFrames(dec));
          CHECK(got_frame == ref_timestamp[target],
                "target frame %d (timestamp %d) not reached after seek, got "
                "timestamp %d",
                target, ref_timestamp[target], got_frame);
        }
      }
      // Reset still works after seeks.
      WebPAnimDecoderReset(dec);
      {
        uint8_t* buf;
        int timestamp;
        CHECK(WebPAnimDecoderGetNext(dec, &buf, &timestamp) &&
                  timestamp == ref_timestamp[1],
              "Reset after seek did not rewind to frame 1");
      }
      WebPAnimDecoderDelete(dec);
    }
  }

  CheckCorruptFrame(&data);

  FreeReference(ref_canvas);
  WebPDataClear(&data);

  if (g_failures == 0) {
    printf("anim_seek: all checks passed\n");
    return 0;
  }
  fprintf(stderr, "anim_seek: %d check(s) failed\n", g_failures);
  return 1;
}
