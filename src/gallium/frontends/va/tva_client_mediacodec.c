/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In-process MediaCodec implementation of the TVA session API. This file is
 * selected only for Android/Bionic builds using -Dtermux-va-direct=enabled.
 */
#include "tva_client.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_WAIT_US  (100 * 1000LL)
#define OUTPUT_WAIT_US (100 * 1000LL)
#define ENCODE_OUTPUT_WAIT_US (5000 * 1000LL)
#define MAX_CODEC_DIM  8192

struct tva_session {
   AMediaCodec *codec;
   int encoder;
   int cancelled;
   int eos_queued;
   int codec_id;
   int width, height;
   int stride, slice_height;
   int crop_left, crop_top, crop_right, crop_bottom;
   int output_color_format;
   int encoder_stride, encoder_slice_height;
   uint32_t next_unit;
   uint32_t next_frame;
   uint32_t fps_num, fps_den;
   uint64_t received;
   uint64_t packets_received;
   struct tva_format fmt;
   struct tva_error err;
   uint8_t *frame_data;
   size_t frame_capacity;
   uint8_t *csd_data;
   size_t csd_size, csd_capacity;
};

static void
set_error(struct tva_session *s, int code, const char *msg)
{
   s->err.code = code;
   snprintf(s->err.msg, sizeof(s->err.msg), "%s", msg);
}

static const char *
codec_mime(int codec)
{
   switch (codec_base_id(codec)) {
   case CODEC_H264: return "video/avc";
   case CODEC_HEVC: return "video/hevc";
   case CODEC_VP9:  return "video/x-vnd.on2.vp9";
   case CODEC_VP8:  return "video/x-vnd.on2.vp8";
   case CODEC_AV1:  return "video/av01";
   default: return NULL;
   }
}

void
tva_session_config_defaults(struct tva_session_config *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->codec = CODEC_H264;
   cfg->fps_num = 30;
   cfg->fps_den = 1;
   cfg->io_timeout_ms = TVA_DEF_IO_MS;
}

/* Kept for the shared activation helper's dead socket auto-detection path.
 * Direct builds return active unconditionally and never resolve endpoints. */
const char *
tva_default_endpoint(char *buf, size_t bufsz)
{
   (void)buf;
   (void)bufsz;
   return NULL;
}

int
tva_format_display_width(const struct tva_format *fmt)
{
   return fmt && fmt->valid ? fmt->crop_right - fmt->crop_left + 1 : 0;
}

int
tva_format_display_height(const struct tva_format *fmt)
{
   return fmt && fmt->valid ? fmt->crop_bottom - fmt->crop_top + 1 : 0;
}

static void
read_output_format(struct tva_session *s)
{
   AMediaFormat *f = AMediaCodec_getOutputFormat(s->codec);
   if (!f)
      return;
   int x;
   if (AMediaFormat_getInt32(f, "width", &x)) s->width = x;
   if (AMediaFormat_getInt32(f, "height", &x)) s->height = x;
   if (AMediaFormat_getInt32(f, "stride", &x)) s->stride = x;
   if (AMediaFormat_getInt32(f, "slice-height", &x)) s->slice_height = x;
   s->crop_left = 0;
   s->crop_top = 0;
   s->crop_right = s->width - 1;
   s->crop_bottom = s->height - 1;
   if (AMediaFormat_getInt32(f, "crop-left", &x)) s->crop_left = x;
   if (AMediaFormat_getInt32(f, "crop-top", &x)) s->crop_top = x;
   if (AMediaFormat_getInt32(f, "crop-right", &x)) s->crop_right = x;
   if (AMediaFormat_getInt32(f, "crop-bottom", &x)) s->crop_bottom = x;
   if (!AMediaFormat_getInt32(f, "color-format", &s->output_color_format))
      s->output_color_format = codec_base_id(s->codec_id) == CODEC_AV1 ? 19 : 21;
   if (s->stride <= 0) s->stride = s->width;
   if (s->slice_height <= 0) s->slice_height = s->height;
   s->fmt.buf_width = s->width;
   s->fmt.buf_height = s->height;
   s->fmt.stride = s->stride;
   s->fmt.slice_height = s->slice_height;
   s->fmt.crop_left = s->crop_left;
   s->fmt.crop_top = s->crop_top;
   s->fmt.crop_right = s->crop_right;
   s->fmt.crop_bottom = s->crop_bottom;
   s->fmt.valid = 1;
   s->fmt.changes++;
   AMediaFormat_delete(f);
}

struct tva_session *
tva_session_create(const struct tva_session_config *cfg, struct tva_error *err)
{
   if (err) memset(err, 0, sizeof(*err));
   if (!cfg || cfg->width < 2 || cfg->height < 2 ||
       (cfg->width & 1) || (cfg->height & 1) ||
       cfg->width > MAX_CODEC_DIM || cfg->height > MAX_CODEC_DIM) {
      if (err) { err->code = TVA_ERR_INVAL; snprintf(err->msg, sizeof(err->msg), "invalid codec dimensions"); }
      return NULL;
   }
   const char *mime = codec_mime(cfg->codec);
   if (!mime) {
      if (err) { err->code = TVA_ERR_INVAL; snprintf(err->msg, sizeof(err->msg), "unsupported codec"); }
      return NULL;
   }
   struct tva_session *s = calloc(1, sizeof(*s));
   if (!s) return NULL;
   s->codec_id = cfg->codec;
   s->fps_num = cfg->fps_num ? cfg->fps_num : 30;
   s->fps_den = cfg->fps_den ? cfg->fps_den : 1;
   s->encoder = codec_is_encoder(cfg->codec);
   s->width = cfg->width;
   s->height = cfg->height;
   s->stride = cfg->width;
   s->slice_height = cfg->height;
   s->crop_right = cfg->width - 1;
   s->crop_bottom = cfg->height - 1;
   s->codec = s->encoder ? AMediaCodec_createEncoderByType(mime) :
                           AMediaCodec_createDecoderByType(mime);
   AMediaFormat *f = AMediaFormat_new();
   if (!s->codec || !f) goto fail;
   /* Pass the MediaFormat keys as strings. The NDK's exported key globals
    * are absent on some Android libmediandk builds despite being declared. */
   AMediaFormat_setString(f, "mime", mime);
   AMediaFormat_setInt32(f, "width", cfg->width);
   AMediaFormat_setInt32(f, "height", cfg->height);
   AMediaFormat_setInt32(f, "max-input-size",
                         s->encoder ? TVA_MAX_RAW_FRAME_BYTES : TVA_MAX_UNIT_BYTES);
   if (s->encoder) {
      AMediaFormat_setInt32(f, "bitrate", cfg->bitrate ? cfg->bitrate : 4000000);
      AMediaFormat_setInt32(f, "frame-rate", cfg->fps_num ? cfg->fps_num : 30);
      AMediaFormat_setInt32(f, "i-frame-interval", 1);
      AMediaFormat_setInt32(f, "color-format", 21); /* YUV420 semi-planar */
      AMediaFormat_setInt32(f, "bitrate-mode", 2);  /* CBR */
      AMediaFormat_setInt32(f, "max-bframes", 0);
      if (cfg->codec == CODEC_H264_BASELINE_ENC)
         AMediaFormat_setInt32(f, "profile", 1);
      else if (cfg->codec == CODEC_H264_MAIN_ENC)
         AMediaFormat_setInt32(f, "profile", 2);
      else if (cfg->codec == CODEC_H264_HIGH_ENC)
         AMediaFormat_setInt32(f, "profile", 8);
   } else {
      AMediaFormat_setInt32(f, "max-width", cfg->width > 1920 ? cfg->width : 1920);
      AMediaFormat_setInt32(f, "max-height", cfg->height > 1088 ? cfg->height : 1088);
      AMediaFormat_setInt32(f, "low-latency", 1);
      AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
   }
   media_status_t st = AMediaCodec_configure(s->codec, f, NULL, NULL, s->encoder ? 1 : 0);
   AMediaFormat_delete(f);
   f = NULL;
   if (st != AMEDIA_OK || AMediaCodec_start(s->codec) != AMEDIA_OK) goto fail;
   if (s->encoder) {
      s->fmt.valid = 1;
      s->fmt.buf_width = s->width;
      s->fmt.buf_height = s->height;
      s->fmt.stride = s->stride;
      s->fmt.slice_height = s->slice_height;
      s->fmt.crop_right = s->crop_right;
      s->fmt.crop_bottom = s->crop_bottom;
   }
   return s;
fail:
   if (f) AMediaFormat_delete(f);
   if (s->codec) { AMediaCodec_stop(s->codec); AMediaCodec_delete(s->codec); }
   set_error(s, TVA_ERR_CONNECT, "MediaCodec create/configure/start failed");
   if (err) *err = s->err;
   free(s);
   return NULL;
}

void
tva_session_cancel(struct tva_session *s)
{
   if (s) __atomic_store_n(&s->cancelled, 1, __ATOMIC_RELEASE);
}

void
tva_session_destroy(struct tva_session *s)
{
   if (!s) return;
   if (s->codec) { AMediaCodec_stop(s->codec); AMediaCodec_delete(s->codec); }
   free(s->frame_data);
   free(s->csd_data);
   free(s);
}

static int
queue_input(struct tva_session *s, const void *data, size_t len, int64_t pts, uint32_t flags)
{
   if (!s || !s->codec || __atomic_load_n(&s->cancelled, __ATOMIC_ACQUIRE) || (!data && len)) return TVA_ERR_STATE;
   for (int retry = 0; retry < 50; retry++) {
      ssize_t ix = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_WAIT_US);
      if (ix == AMEDIACODEC_INFO_TRY_AGAIN_LATER) continue;
      if (ix < 0) { set_error(s, TVA_ERR_IO, "MediaCodec input dequeue failed"); return TVA_ERR_IO; }
      size_t cap = 0;
      uint8_t *buf = AMediaCodec_getInputBuffer(s->codec, ix, &cap);
      if (!buf || len > cap) {
         AMediaCodec_queueInputBuffer(s->codec, ix, 0, 0, pts, 0);
         set_error(s, len > cap ? TVA_ERR_TOOBIG : TVA_ERR_IO, "MediaCodec input buffer unavailable or too small");
         return len > cap ? TVA_ERR_TOOBIG : TVA_ERR_IO;
      }
      if (len) memcpy(buf, data, len);
      media_status_t st = AMediaCodec_queueInputBuffer(s->codec, ix, 0, len, pts, flags);
      if (st != AMEDIA_OK) { set_error(s, TVA_ERR_IO, "MediaCodec input queue failed"); return TVA_ERR_IO; }
      return TVA_OK;
   }
   set_error(s, TVA_ERR_TIMEOUT, "timed out waiting for MediaCodec input buffer");
   return TVA_ERR_TIMEOUT;
}

static int
is_parameter_set(int codec, const uint8_t *data, size_t len)
{
   size_t offset = 0;
   if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1)
      offset = 4;
   else if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
      offset = 3;
   if (!offset || offset >= len)
      return 0;
   if (codec == CODEC_H264) {
      int type = data[offset] & 0x1f;
      return type == 7 || type == 8;
   }
   if (codec == CODEC_HEVC) {
      int type = (data[offset] >> 1) & 0x3f;
      return type == 32 || type == 33 || type == 34;
   }
   return 0;
}

static int grow(uint8_t **buf, size_t *cap, size_t need);

int
tva_session_send_unit(struct tva_session *s, const void *data, size_t len)
{
   if (!s || s->encoder || !data || !len || len > TVA_MAX_UNIT_BYTES) return TVA_ERR_INVAL;
   if (is_parameter_set(codec_base_id(s->codec_id), data, len)) {
      if (s->csd_size > TVA_MAX_UNIT_BYTES - len ||
          grow(&s->csd_data, &s->csd_capacity, s->csd_size + len) < 0)
         return TVA_ERR_NOMEM;
      memcpy(s->csd_data + s->csd_size, data, len);
      s->csd_size += len;
      return TVA_OK;
   }
   if (s->csd_size) {
      int config_rc = queue_input(s, s->csd_data, s->csd_size, 0,
                                  AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
      if (config_rc != TVA_OK)
         return config_rc;
      s->csd_size = 0;
   }
   uint32_t id = ++s->next_unit;
   int rc = queue_input(s, data, len, (int64_t)id * 1000, 0);
   return rc;
}

int
tva_session_send_raw_frame(struct tva_session *s, const void *data, size_t len)
{
   if (!s || !s->encoder || !data || !len || len > TVA_MAX_RAW_FRAME_BYTES) return TVA_ERR_INVAL;
   if (!s->encoder_stride) {
#if __ANDROID_API__ >= 28
      AMediaFormat *f = AMediaCodec_getInputFormat(s->codec);
      int stride = 0, slice = 0, color = 0;
      if (f) {
         AMediaFormat_getInt32(f, "stride", &stride);
         AMediaFormat_getInt32(f, "slice-height", &slice);
         AMediaFormat_getInt32(f, "color-format", &color);
         AMediaFormat_delete(f);
      }
      if (color && color != 21) {
         set_error(s, TVA_ERR_PROTOCOL, "encoder does not accept NV12 input");
         return TVA_ERR_PROTOCOL;
      }
      s->encoder_stride = stride > 0 ? stride : s->width;
      s->encoder_slice_height = slice > 0 ? slice : s->height;
#else
      /* NDK exposes AMediaCodec_getInputFormat from API 28. Older Termux
       * targets use the configured YUV420 semi-planar dimensions directly. */
      s->encoder_stride = s->width;
      s->encoder_slice_height = s->height;
#endif
   }
   uint32_t id = ++s->next_frame;
   uint64_t scaled = (uint64_t)id * 1000000;
   int64_t pts = (int64_t)(scaled * s->fps_den / s->fps_num);
   ssize_t ix = AMEDIACODEC_INFO_TRY_AGAIN_LATER;
   for (int retry = 0; retry < 50 && ix == AMEDIACODEC_INFO_TRY_AGAIN_LATER; retry++)
      ix = AMediaCodec_dequeueInputBuffer(s->codec, INPUT_WAIT_US);
   if (ix < 0) return ix == AMEDIACODEC_INFO_TRY_AGAIN_LATER ? TVA_ERR_TIMEOUT : TVA_ERR_IO;
   size_t cap = 0;
   uint8_t *dst = AMediaCodec_getInputBuffer(s->codec, ix, &cap);
   size_t w = s->width, h = s->height;
   size_t stride = (size_t)s->encoder_stride, slice = (size_t)s->encoder_slice_height;
   size_t y_size = stride * slice;
   size_t uv_size = stride * (h / 2 - 1) + w;
   size_t tight = w * h + w * (h / 2);
   if (!dst || len != tight || stride < w || slice < h || y_size > cap || uv_size > cap - y_size) {
      AMediaCodec_queueInputBuffer(s->codec, ix, 0, 0, pts, 0);
      set_error(s, TVA_ERR_TOOBIG, "packed NV12 frame does not fit MediaCodec input layout");
      return TVA_ERR_TOOBIG;
   }
   if (stride == w && slice == h) {
      memcpy(dst, data, tight);
   } else {
      const uint8_t *src = data;
      memset(dst, 16, y_size);
      memset(dst + y_size, 128, uv_size);
      for (size_t row = 0; row < h; row++)
         memcpy(dst + row * stride, src + row * w, w);
      src += w * h;
      for (size_t row = 0; row < h / 2; row++)
         memcpy(dst + y_size + row * stride, src + row * w, w);
   }
   size_t queued = stride == w && slice == h ? tight : y_size + uv_size;
   if (AMediaCodec_queueInputBuffer(s->codec, ix, 0, queued, pts, 0) != AMEDIA_OK)
      return TVA_ERR_IO;
   return TVA_OK;
}

int
tva_session_drain(struct tva_session *s)
{
   if (!s || s->eos_queued) return TVA_ERR_STATE;
   int rc = queue_input(s, NULL, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
   if (rc == TVA_OK) s->eos_queued = 1;
   return rc;
}

static int
grow(uint8_t **buf, size_t *cap, size_t need)
{
   if (need <= *cap) return 0;
   uint8_t *p = realloc(*buf, need);
   if (!p) return -1;
   *buf = p;
   *cap = need;
   return 0;
}

static size_t
vp9_keyframe_prefix(const uint8_t *data, size_t len)
{
   if (!data || len < 4)
      return 0;
   size_t limit = len < 64 ? len - 3 : 61;
   for (size_t i = 0; i < limit; i++) {
      if ((data[i] & 0x03) == 0x02 && data[i + 1] == 0x49 &&
          data[i + 2] == 0x83 && data[i + 3] == 0x42)
         return i;
   }
   return 0;
}

static int
copy_i420_to_nv12(struct tva_session *s, const uint8_t *src, size_t src_len,
                  size_t *out_len)
{
   size_t stride = (size_t)s->stride;
   size_t rows = (size_t)s->slice_height;
   size_t cw = (stride + 1) / 2;
   size_t ch = (rows + 1) / 2;
   size_t y_size = stride * rows;
   size_t c_stride = (stride + 1) / 2;
   if (!stride || !rows || 2 * cw > stride || c_stride > SIZE_MAX / ch)
      return -1;
   size_t c_size = c_stride * ch;
   if (c_size > (SIZE_MAX - y_size) / 2)
      return -1;
   size_t tight_size = y_size + c_size * 2;
   if (stride > SIZE_MAX / ch)
      return -1;
   size_t full_c_size = stride * ch;
   if (full_c_size > (SIZE_MAX - y_size) / 2)
      return -1;
   if (src_len == y_size + full_c_size * 2 && full_c_size != c_size) {
      c_stride = stride;
      c_size = full_c_size;
   } else if (src_len < tight_size) {
      return -1;
   }
   size_t total = y_size + stride * ch;
   if (grow(&s->frame_data, &s->frame_capacity, total) < 0)
      return -1;
   memcpy(s->frame_data, src, y_size);
   const uint8_t *u = src + y_size;
   const uint8_t *v = u + c_size;
   uint8_t *uv = s->frame_data + y_size;
   for (size_t y = 0; y < ch; y++) {
      for (size_t x = 0; x < cw; x++) {
         uv[y * stride + 2 * x] = u[y * c_stride + x];
         uv[y * stride + 2 * x + 1] = v[y * c_stride + x];
      }
   }
   *out_len = total;
   return 0;
}

int
tva_session_next_frame(struct tva_session *s, struct tva_frame *out, int timeout_ms)
{
   if (!s || !out || s->encoder) return TVA_ERR_INVAL;
   if (__atomic_load_n(&s->cancelled, __ATOMIC_ACQUIRE)) return TVA_ERR_CANCELLED;
   int64_t timeout = timeout_ms < 0 ? OUTPUT_WAIT_US : (int64_t)timeout_ms * 1000;
   for (;;) {
      AMediaCodecBufferInfo info;
      ssize_t ix = AMediaCodec_dequeueOutputBuffer(s->codec, &info, timeout);
      if (ix == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return TVA_ERR_TIMEOUT;
      if (ix == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { read_output_format(s); timeout = 0; continue; }
      if (ix == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) { timeout = 0; continue; }
      if (ix < 0) { set_error(s, TVA_ERR_IO, "MediaCodec output dequeue failed"); return TVA_ERR_IO; }
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) && info.size == 0) {
         AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
         return TVA_EOS;
      }
      size_t cap = 0;
      uint8_t *buf = AMediaCodec_getOutputBuffer(s->codec, ix, &cap);
      if (!buf || info.offset < 0 || info.size < 0 || (size_t)info.offset > cap || (size_t)info.size > cap - (size_t)info.offset) {
         AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
         set_error(s, TVA_ERR_PROTOCOL, "invalid MediaCodec output buffer range");
         return TVA_ERR_PROTOCOL;
      }
      if (!s->fmt.valid) read_output_format(s);
      size_t bytes = (size_t)info.size;
      if (s->output_color_format == 19 || s->output_color_format == 20) {
         if (copy_i420_to_nv12(s, buf + info.offset, bytes, &bytes) < 0) {
            AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
            set_error(s, TVA_ERR_PROTOCOL, "invalid planar MediaCodec output");
            return TVA_ERR_PROTOCOL;
         }
      } else {
         if (grow(&s->frame_data, &s->frame_capacity, bytes) < 0) {
            AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
            return TVA_ERR_NOMEM;
         }
         memcpy(s->frame_data, buf + info.offset, bytes);
      }
      AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
      out->data = s->frame_data;
      out->size = bytes;
      out->width = s->width;
      out->height = s->height;
      out->unit_seq = info.presentationTimeUs > 0 ? (uint32_t)(info.presentationTimeUs / 1000) : 0;
      out->stride = s->stride;
      out->slice_height = s->slice_height;
      out->crop_left = s->crop_left;
      out->crop_top = s->crop_top;
      out->crop_right = s->crop_right;
      out->crop_bottom = s->crop_bottom;
      out->shm_slot = -1;
      out->seq = s->received++;
      return TVA_OK;
   }
}

int
tva_session_release_frame(struct tva_session *s, struct tva_frame *f)
{
   if (!s || !f) return TVA_OK;
   memset(f, 0, sizeof(*f));
   return TVA_OK;
}

int
tva_session_receive_packet(struct tva_session *s, void *data, size_t capacity,
                           size_t *size, uint32_t *flags, uint32_t *pts)
{
   if (!s || !s->encoder || !data || !size) return TVA_ERR_INVAL;
   for (;;) {
      AMediaCodecBufferInfo info;
      ssize_t ix = AMediaCodec_dequeueOutputBuffer(s->codec, &info, ENCODE_OUTPUT_WAIT_US);
      if (ix == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return TVA_ERR_TIMEOUT;
      if (ix == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || ix == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
      if (ix < 0) return TVA_ERR_IO;
      size_t cap = 0;
      uint8_t *buf = AMediaCodec_getOutputBuffer(s->codec, ix, &cap);
      if (!buf || info.offset < 0 || info.size < 0 || (size_t)info.offset > cap || (size_t)info.size > cap - (size_t)info.offset || (size_t)info.size > capacity) {
         AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
         return TVA_ERR_TOOBIG;
      }
      const uint8_t *payload = buf + info.offset;
      size_t payload_size = (size_t)info.size;
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
         if (codec_base_id(s->codec_id) != CODEC_VP9) {
            if (grow(&s->csd_data, &s->csd_capacity, s->csd_size + payload_size) < 0) {
               AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
               return TVA_ERR_NOMEM;
            }
            memcpy(s->csd_data + s->csd_size, payload, payload_size);
            s->csd_size += payload_size;
         }
         AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
         continue;
      }
      if (codec_base_id(s->codec_id) == CODEC_VP9 && !s->packets_received &&
          (info.flags & 1u)) {
         size_t prefix = vp9_keyframe_prefix(payload, payload_size);
         payload += prefix;
         payload_size -= prefix;
      }
      if (s->csd_size + payload_size > capacity) {
         AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
         return TVA_ERR_TOOBIG;
      }
      if (s->csd_size) memcpy(data, s->csd_data, s->csd_size);
      memcpy((uint8_t *)data + s->csd_size, payload, payload_size);
      *size = s->csd_size + payload_size;
      s->csd_size = 0;
      if (flags) *flags = info.flags;
      if (pts) *pts = info.presentationTimeUs > 0 ? (uint32_t)((uint64_t)info.presentationTimeUs * s->fps_num / ((uint64_t)1000000 * s->fps_den)) : 0;
      s->packets_received++;
      AMediaCodec_releaseOutputBuffer(s->codec, ix, false);
      return TVA_OK;
   }
}

const struct tva_format *tva_session_format(const struct tva_session *s) { return s ? &s->fmt : NULL; }
const char *tva_session_last_error(const struct tva_session *s) { return s ? s->err.msg : "invalid session"; }
int tva_session_last_error_code(const struct tva_session *s) { return s ? s->err.code : TVA_ERR_INVAL; }
int tva_session_xfer_mode(const struct tva_session *s) { (void)s; return XFER_INLINE; }
uint64_t tva_session_units_sent(const struct tva_session *s) { return s ? s->next_unit : 0; }
uint64_t tva_session_frames_received(const struct tva_session *s) { return s ? s->received : 0; }
