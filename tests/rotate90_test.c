// Copyright 2026 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Tests for the 'rotation' decoding option (WebPDecoderOptions::rotation).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "webp/decode.h"
#include "webp/encode.h"

#define WIDTH 8
#define HEIGHT 6
#define NUM_CHANNELS 4

static int num_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++num_failures;                                                  \
    }                                                                  \
  } while (0)

// Fills 'rgba' with a position-dependent pattern.
static void MakeTestImage(uint8_t* rgba, int w, int h) {
  int x, y;
  for (y = 0; y < h; ++y) {
    for (x = 0; x < w; ++x) {
      uint8_t* const p = rgba + (y * w + x) * NUM_CHANNELS;
      p[0] = (uint8_t)(x * 32 + 1);
      p[1] = (uint8_t)(y * 40 + 2);
      p[2] = (uint8_t)(x + y * 16 + 3);
      p[3] = 255;
    }
  }
}

// Reference rotation of an RGBA buffer, by 'rotation' degrees clockwise,
// optionally followed by a vertical flip. 'dst' must have room for
// w * h * 4 bytes.
static void RefRotateRGBA(const uint8_t* src, int w, int h, uint8_t* dst,
                          int rotation, int flip) {
  const int ow = (rotation == 180) ? w : h;
  const int oh = (rotation == 180) ? h : w;
  int x, y;
  for (y = 0; y < oh; ++y) {
    for (x = 0; x < ow; ++x) {
      int sx, sy;
      if (rotation == 90) {
        sx = y;
        sy = h - 1 - x;
      } else if (rotation == 180) {
        sx = w - 1 - x;
        sy = h - 1 - y;
      } else {  // 270
        sx = w - 1 - y;
        sy = x;
      }
      {
        const int dy = flip ? (oh - 1 - y) : y;
        memcpy(dst + (dy * ow + x) * NUM_CHANNELS,
               src + (sy * w + sx) * NUM_CHANNELS, NUM_CHANNELS);
      }
    }
  }
}

// Decodes 'data' with the given options overrides. Returns the status and,
// when successful, the output dimensions in '*out_w'/'*out_h'.
static VP8StatusCode DecodeWithOptions(const uint8_t* data, size_t data_size,
                                       WEBP_CSP_MODE colorspace, int rotation,
                                       int flip, int use_cropping, int crop_x,
                                       int crop_y, int crop_w, int crop_h,
                                       int use_scaling, int scaled_w,
                                       int scaled_h, uint8_t** out_rgba,
                                       int* out_w, int* out_h) {
  WebPDecoderConfig config;
  VP8StatusCode status;
  if (!WebPInitDecoderConfig(&config)) {
    return VP8_STATUS_INVALID_PARAM;
  }
  config.output.colorspace = colorspace;
  config.options.rotation = rotation;
  config.options.flip = flip;
  config.options.use_cropping = use_cropping;
  config.options.crop_left = crop_x;
  config.options.crop_top = crop_y;
  config.options.crop_width = crop_w;
  config.options.crop_height = crop_h;
  config.options.use_scaling = use_scaling;
  config.options.scaled_width = scaled_w;
  config.options.scaled_height = scaled_h;
  status = WebPDecode(data, data_size, &config);
  if (status == VP8_STATUS_OK) {
    *out_w = config.output.width;
    *out_h = config.output.height;
    *out_rgba = config.output.u.RGBA.rgba;
  } else {
    WebPFreeDecBuffer(&config.output);
  }
  return status;
}

// Checks that decoding with 'rotation' (+ 'flip') produces exactly the
// reference transform of the unrotated decode.
static void TestRotation(const uint8_t* data, size_t data_size, int rotation,
                         int flip) {
  uint8_t* ref = NULL;
  uint8_t* rotated = NULL;
  int ref_w = 0, ref_h = 0, rot_w = 0, rot_h = 0;
  const int expect_w = (rotation == 180) ? WIDTH : HEIGHT;
  const int expect_h = (rotation == 180) ? HEIGHT : WIDTH;
  uint8_t* expected = (uint8_t*)malloc(WIDTH * HEIGHT * NUM_CHANNELS);

  CHECK(expected != NULL);
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 0, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, &ref, &ref_w, &ref_h) == VP8_STATUS_OK);
  CHECK(ref_w == WIDTH && ref_h == HEIGHT);
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, rotation, flip, 0, 0, 0,
                          0, 0, 0, 0, 0, &rotated, &rot_w,
                          &rot_h) == VP8_STATUS_OK);
  CHECK(rot_w == expect_w && rot_h == expect_h);
  if (ref != NULL && rotated != NULL && rot_w == expect_w &&
      rot_h == expect_h) {
    RefRotateRGBA(ref, WIDTH, HEIGHT, expected, rotation, flip);
    CHECK(memcmp(rotated, expected, WIDTH * HEIGHT * NUM_CHANNELS) == 0);
  }
  free(expected);
  WebPFree(ref);
  WebPFree(rotated);
}

// The default (rotation == 0) output must be byte-identical to a decode
// that does not use the option at all.
static void TestDefaultUnchanged(const uint8_t* data, size_t data_size) {
  int w = 0, h = 0;
  uint8_t* simple = WebPDecodeRGBA(data, data_size, &w, &h);
  uint8_t* with_zero = NULL;
  int zw = 0, zh = 0;
  CHECK(simple != NULL);
  CHECK(w == WIDTH && h == HEIGHT);
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 0, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, &with_zero, &zw, &zh) == VP8_STATUS_OK);
  if (simple != NULL && with_zero != NULL) {
    CHECK(zw == w && zh == h);
    CHECK(memcmp(simple, with_zero, WIDTH * HEIGHT * NUM_CHANNELS) == 0);
  }
  WebPFree(simple);
  WebPFree(with_zero);
}

static void TestInvalidAngle(const uint8_t* data, size_t data_size) {
  static const int kBadAngles[] = {1, 45, 360, -90, 180 + 1};
  size_t i;
  for (i = 0; i < sizeof(kBadAngles) / sizeof(kBadAngles[0]); ++i) {
    uint8_t* out = NULL;
    int w = 0, h = 0;
    const VP8StatusCode status =
        DecodeWithOptions(data, data_size, MODE_RGBA, kBadAngles[i], 0, 0, 0,
                          0, 0, 0, 0, 0, 0, &out, &w, &h);
    CHECK(status == VP8_STATUS_INVALID_ROTATION);
  }
}

// A crop window that fits the source frame but not the rotated frame must
// report VP8_STATUS_ROTATION_CROP_CONFLICT.
static void TestCropConflict(const uint8_t* data, size_t data_size) {
  uint8_t* out = NULL;
  int w = 0, h = 0;
  // Source frame is 8x6, rotated (90) frame is 6x8: crop_left=7, width=1
  // fits the source (7 + 1 <= 8) but not the rotated frame (7 + 1 > 6).
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 90, 0, 1, 7, 0, 1, 6, 0,
                          0, 0, &out, &w,
                          &h) == VP8_STATUS_ROTATION_CROP_CONFLICT);
  // The same crop window is fine without rotation.
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 0, 0, 1, 7, 0, 1, 6, 0,
                          0, 0, &out, &w, &h) == VP8_STATUS_OK);
  WebPFree(out);
}

// Crop window (expressed in the rotated frame) combined with a rotation.
static void TestCropAndRotation(const uint8_t* data, size_t data_size) {
  uint8_t* ref = NULL;
  uint8_t* out = NULL;
  int ref_w = 0, ref_h = 0, w = 0, h = 0;
  const int crop_x = 2, crop_y = 2, crop_w = 2, crop_h = 2;
  uint8_t* rotated = (uint8_t*)malloc(WIDTH * HEIGHT * NUM_CHANNELS);
  CHECK(rotated != NULL);
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 0, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, &ref, &ref_w, &ref_h) == VP8_STATUS_OK);
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 90, 0, 1, crop_x, crop_y,
                          crop_w, crop_h, 0, 0, 0, &out, &w,
                          &h) == VP8_STATUS_OK);
  CHECK(w == crop_w && h == crop_h);
  if (ref != NULL && out != NULL && w == crop_w && h == crop_h) {
    int x, y;
    RefRotateRGBA(ref, WIDTH, HEIGHT, rotated, 90, 0);
    // The crop window is expressed in the rotated (6x8) frame.
    for (y = 0; y < crop_h; ++y) {
      for (x = 0; x < crop_w; ++x) {
        const uint8_t* const p = rotated + ((crop_y + y) * HEIGHT + crop_x + x) *
                                           NUM_CHANNELS;
        const uint8_t* const q = out + (y * crop_w + x) * NUM_CHANNELS;
        CHECK(memcmp(p, q, NUM_CHANNELS) == 0);
      }
    }
  }
  free(rotated);
  WebPFree(ref);
  WebPFree(out);
}

// Scaling dimensions are expressed in the rotated frame.
static void TestScaleAndRotation(const uint8_t* data, size_t data_size) {
  uint8_t* out = NULL;
  int w = 0, h = 0;
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 90, 0, 0, 0, 0, 0, 0, 1,
                          3, 4, &out, &w, &h) == VP8_STATUS_OK);
  CHECK(w == 3 && h == 4);
  WebPFree(out);
}

// Decoding into a user-supplied buffer with rotation.
static void TestExternalMemory(const uint8_t* data, size_t data_size) {
  WebPDecoderConfig config;
  uint8_t* ref = NULL;
  int ref_w = 0, ref_h = 0;
  uint8_t* expected = (uint8_t*)malloc(WIDTH * HEIGHT * NUM_CHANNELS);
  uint8_t* buffer =
      (uint8_t*)malloc(WIDTH * HEIGHT * NUM_CHANNELS);
  CHECK(expected != NULL && buffer != NULL);
  CHECK(WebPInitDecoderConfig(&config));
  config.output.colorspace = MODE_RGBA;
  config.output.is_external_memory = 1;
  config.output.u.RGBA.rgba = buffer;
  config.output.u.RGBA.stride = HEIGHT * NUM_CHANNELS;  // rotated width
  config.output.u.RGBA.size = WIDTH * HEIGHT * NUM_CHANNELS;
  config.options.rotation = 90;
  CHECK(WebPDecode(data, data_size, &config) == VP8_STATUS_OK);
  CHECK(config.output.width == HEIGHT && config.output.height == WIDTH);
  CHECK(DecodeWithOptions(data, data_size, MODE_RGBA, 0, 0, 0, 0, 0, 0, 0, 0,
                          0, 0, &ref, &ref_w, &ref_h) == VP8_STATUS_OK);
  if (ref != NULL) {
    RefRotateRGBA(ref, WIDTH, HEIGHT, expected, 90, 0);
    CHECK(memcmp(buffer, expected, WIDTH * HEIGHT * NUM_CHANNELS) == 0);
  }
  free(expected);
  free(buffer);
  WebPFree(ref);
}

// YUV output: each plane is rotated independently.
static void TestYuvRotation(const uint8_t* data, size_t data_size) {
  WebPDecoderConfig config;
  const WebPYUVABuffer* yuv;
  const int uv_w = (WIDTH + 1) / 2, uv_h = (HEIGHT + 1) / 2;
  int x, y;
  CHECK(WebPInitDecoderConfig(&config));
  config.output.colorspace = MODE_YUV;
  config.options.rotation = 90;
  CHECK(WebPDecode(data, data_size, &config) == VP8_STATUS_OK);
  CHECK(config.output.width == HEIGHT && config.output.height == WIDTH);
  yuv = &config.output.u.YUVA;
  // Spot-check the luma plane corners against a direct YUV decode.
  {
    WebPDecoderConfig ref_config;
    const WebPYUVABuffer* ref;
    CHECK(WebPInitDecoderConfig(&ref_config));
    ref_config.output.colorspace = MODE_YUV;
    CHECK(WebPDecode(data, data_size, &ref_config) == VP8_STATUS_OK);
    ref = &ref_config.output.u.YUVA;
    // 90 degrees clockwise: dst(x, y) = src(y, HEIGHT - 1 - x).
    for (y = 0; y < WIDTH; ++y) {
      for (x = 0; x < HEIGHT; ++x) {
        CHECK(yuv->y[y * yuv->y_stride + x] ==
              ref->y[(HEIGHT - 1 - x) * ref->y_stride + y]);
      }
    }
    for (y = 0; y < uv_w; ++y) {
      for (x = 0; x < uv_h; ++x) {
        CHECK(yuv->u[y * yuv->u_stride + x] ==
              ref->u[(uv_h - 1 - x) * ref->u_stride + y]);
      }
    }
    WebPFreeDecBuffer(&ref_config.output);
  }
  WebPFreeDecBuffer(&config.output);
}

// A VP8X canvas whose rotated output exceeds MAX_ROTATED_PIXELS must report
// VP8_STATUS_IMAGE_TOO_LARGE (before any large allocation is attempted).
static void TestOversized(void) {
  // Minimal "RIFF + VP8X" file with a 20000x20000 canvas and no image data.
  const uint8_t kData[] = {'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P',
                           'V', 'P', '8', 'X', 10, 0, 0, 0,
                           0, 0, 0, 0,                     // flags + reserved
                           0x1f, 0x4e, 0x00,               // width - 1
                           0x1f, 0x4e, 0x00};              // height - 1
  WebPDecoderConfig config;
  CHECK(WebPInitDecoderConfig(&config));
  config.output.colorspace = MODE_RGBA;
  config.options.rotation = 90;
  CHECK(WebPDecode(kData, sizeof(kData), &config) == VP8_STATUS_IMAGE_TOO_LARGE);
  // Without rotation, the same input must not report this error.
  {
    WebPDecoderConfig config2;
    CHECK(WebPInitDecoderConfig(&config2));
    config2.output.colorspace = MODE_RGBA;
    CHECK(WebPDecode(kData, sizeof(kData), &config2) !=
          VP8_STATUS_IMAGE_TOO_LARGE);
  }
}

// WebPValidateDecoderConfig() and the incremental API reject 'rotation'.
static void TestApiGuards(const uint8_t* data, size_t data_size) {
  WebPDecoderConfig config;
  CHECK(WebPInitDecoderConfig(&config));
  config.options.rotation = 45;
  CHECK(!WebPValidateDecoderConfig(&config));
  config.options.rotation = 270;
  CHECK(WebPValidateDecoderConfig(&config));
  // The incremental API does not support rotation.
  {
    WebPDecoderConfig rot_config;
    WebPDecoderConfig ref_config;
    WebPIDecoder* idec;
    CHECK(WebPInitDecoderConfig(&rot_config));
    CHECK(WebPInitDecoderConfig(&ref_config));
    rot_config.options.rotation = 90;
    CHECK(WebPIDecode(data, data_size, &rot_config) == NULL);
    idec = WebPIDecode(data, data_size, &ref_config);
    CHECK(idec != NULL);
    WebPIDelete(idec);
  }
}

int main(void) {
  uint8_t rgba[WIDTH * HEIGHT * NUM_CHANNELS];
  uint8_t* webp_data = NULL;
  size_t webp_size;

  MakeTestImage(rgba, WIDTH, HEIGHT);
  webp_size = WebPEncodeLosslessRGBA(rgba, WIDTH, HEIGHT,
                                     WIDTH * NUM_CHANNELS, &webp_data);
  if (webp_size == 0 || webp_data == NULL) {
    fprintf(stderr, "Failed to encode the test image.\n");
    return 1;
  }

  TestRotation(webp_data, webp_size, 90, 0);
  TestRotation(webp_data, webp_size, 180, 0);
  TestRotation(webp_data, webp_size, 270, 0);
  TestRotation(webp_data, webp_size, 90, 1);   // rotation + flip
  TestRotation(webp_data, webp_size, 180, 1);  // rotation + flip
  TestDefaultUnchanged(webp_data, webp_size);
  TestInvalidAngle(webp_data, webp_size);
  TestCropConflict(webp_data, webp_size);
  TestCropAndRotation(webp_data, webp_size);
  TestScaleAndRotation(webp_data, webp_size);
  TestExternalMemory(webp_data, webp_size);
  TestYuvRotation(webp_data, webp_size);
  TestOversized();
  TestApiGuards(webp_data, webp_size);

  WebPFree(webp_data);
  if (num_failures > 0) {
    fprintf(stderr, "%d check(s) failed.\n", num_failures);
    return 1;
  }
  printf("rotate90: all tests passed.\n");
  return 0;
}
