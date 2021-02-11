/*
 * Copyright (C) 2014 Collabora Ltd.
 *     Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
 * Copyright (c) 2018-2019, NVIDIA CORPORATION. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "gstv4l2object.h"
#include "gstv4l2videodec.h"
#include "stdlib.h"

#include <string.h>
#include <gst/gst-i18n-plugin.h>

GST_DEBUG_CATEGORY_STATIC (gst_v4l2_video_dec_debug);
#define GST_CAT_DEFAULT gst_v4l2_video_dec_debug
#define ENABLE_DRAIN 0

#ifdef USE_V4L2_TARGET_NV
#ifdef USE_V4L2_TARGET_NV_CODECSDK
#define DEFAULT_NVBUF_API_VERSION_NEW   TRUE
#else
#define DEFAULT_NVBUF_API_VERSION_NEW   FALSE
#endif
#define DEFAULT_SKIP_FRAME_TYPE V4L2_SKIP_FRAMES_TYPE_NONE
#define DEFAULT_DISABLE_DPB FALSE
#define DEFAULT_FULL_FRAME FALSE
#define DEFAULT_FRAME_TYPR_REPORTING FALSE
#define DEFAULT_ERROR_CHECK FALSE
#define DEFAULT_MAX_PERFORMANCE FALSE
#define GST_TYPE_V4L2_VID_DEC_SKIP_FRAMES (gst_video_dec_skip_frames ())

#ifdef USE_V4L2_TARGET_NV_CODECSDK
#define DEFAULT_NUM_EXTRA_SURFACES 0 //default for dGPU
#else
#define DEFAULT_NUM_EXTRA_SURFACES 1 //default for Tegra
#endif

static gboolean enable_latency_measurement = FALSE;

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  gdouble in_timestamp;
  guint frame_num;
};

static void buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

static gdouble get_current_system_timestamp(void)
{
  struct timeval t1;
  double elapsedTime = 0;
  gettimeofday(&t1, NULL);
  elapsedTime = (t1.tv_sec) * 1000.0;
  elapsedTime += (t1.tv_usec) / 1000.0;
  return elapsedTime;
}

static GType
gst_video_dec_skip_frames (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {V4L2_SKIP_FRAMES_TYPE_NONE, "Decode all frames", "decode_all"},
      {V4L2_SKIP_FRAMES_TYPE_NONREF, "Decode non-ref frames",
          "decode_non_ref"},
      {V4L2_SKIP_FRAMES_TYPE_DECODE_IDR_ONLY, "decode key frames",
          "decode_key"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("SkipFrame", values);
  }
  return qtype;
}

#ifdef USE_V4L2_TARGET_NV_CODECSDK
/* Properties specifically applicable on GPU*/
#define GST_TYPE_V4L2_VID_CUDADEC_MEM_TYPE (gst_video_cudadec_mem_type ())
#define DEFAULT_CUDADEC_MEM_TYPE V4L2_CUDA_MEM_TYPE_UNIFIED
#define DEFAULT_CUDADEC_GPU_ID   0
#define MAX_CUDADEC_NUM_SURFACES 20
#define DEFAULT_CUDADEC_NUM_SURFACES 20

static GType
gst_video_cudadec_mem_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {V4L2_CUDA_MEM_TYPE_DEVICE, "Memory type Device", "memtype_device"},
      {V4L2_CUDA_MEM_TYPE_PINNED, "Memory type Host Pinned",
          "memtype_pinned"},
      {V4L2_CUDA_MEM_TYPE_UNIFIED, "Memory type Unified",
          "memtype_unified"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("CudaDecMemType", values);
  }
  return qtype;
}
#endif

#define CAPTURE_CAPS \
    "video/x-raw(memory:NVMM), " \
    "width = (gint) [ 1, MAX ], " \
    "height = (gint) [ 1, MAX ], " \
    "framerate = (fraction) [ 0, MAX ];"

#ifdef USE_V4L2_TARGET_NV_CODECSDK
//Caps on dGPU
static GstStaticPadTemplate gst_v4l2dec_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg"
        ";"
        "video/x-h264,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au }"
        ";"
        "video/x-h265,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au }"
        ";")
    );
#else
//Caps on Tegra
static GstStaticPadTemplate gst_v4l2dec_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("image/jpeg"
        ";"
        "video/x-h264,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au }"
        ";"
        "video/x-h265,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au }"
        ";"
        "video/mpeg, "
        "mpegversion= (int) 4, "
        "systemstream=(boolean) false, "
        "parsed=(boolean) true, "
        "width=(gint) [1,MAX],"
        "height=(gint) [1,MAX]"
        ";"
        "video/mpeg, "
        "mpegversion= (int) [1, 2], "
        "systemstream=(boolean) false, "
        "parsed=(boolean) true, "
        "width=(gint) [1,MAX],"
        "height=(gint) [1,MAX]"
        ";"
        "video/x-divx, "
        "divxversion=(int) [4, 5], "
        "width=(int) [1,MAX], " "height=(int) [1,MAX]"
        ";"
        "video/x-vp8"
        ";" "video/x-vp9," "width=(gint) [1,MAX]," "height=(gint) [1,MAX]" ";")
    );
#endif

static GstStaticPadTemplate gst_v4l2dec_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (CAPTURE_CAPS));
#endif

typedef struct
{
  gchar *device;
  GstCaps *sink_caps;
  GstCaps *src_caps;
  const gchar *longname;
  const gchar *description;
} GstV4l2VideoDecCData;

enum
{
  PROP_0,
  V4L2_STD_OBJECT_PROPS,
#ifdef USE_V4L2_TARGET_NV
/*Common properties*/
  PROP_SKIP_FRAME,
  PROP_DROP_FRAME_INTERVAL,
  PROP_NUM_EXTRA_SURFACES,
#ifndef USE_V4L2_TARGET_NV_CODECSDK
/*Properties exposed on Tegra only */
  PROP_DISABLE_DPB,
  PROP_USE_FULL_FRAME,
  PROP_ENABLE_FRAME_TYPE_REPORTING,
  PROP_ENABLE_ERROR_CHECK,
  PROP_ENABLE_MAX_PERFORMANCE,
  PROP_OPEN_MJPEG_BLOCK,
  PROP_NVBUF_API_VERSION
#else
/*Properties exposed on dGPU only*/
  PROP_CUDADEC_MEM_TYPE,
  PROP_CUDADEC_GPU_ID
#endif
#endif
};

#define gst_v4l2_video_dec_parent_class parent_class
G_DEFINE_ABSTRACT_TYPE (GstV4l2VideoDec, gst_v4l2_video_dec,
    GST_TYPE_VIDEO_DECODER);

#ifdef USE_V4L2_TARGET_NV
#ifndef USE_V4L2_TARGET_NV_CODECSDK
GType
gst_v4l2_dec_output_io_mode_get_type (void)
{
  static GType v4l2_dec_output_io_mode = 0;

  if (!v4l2_dec_output_io_mode) {
    static const GEnumValue dec_output_io_modes[] = {
      {GST_V4L2_IO_AUTO, "GST_V4L2_IO_AUTO", "auto"},
      {GST_V4L2_IO_MMAP, "GST_V4L2_IO_MMAP", "mmap"},
      {GST_V4L2_IO_USERPTR, "GST_V4L2_IO_USERPTR", "userptr"},
      {0, NULL, NULL}
    };

    v4l2_dec_output_io_mode = g_enum_register_static ("GstNvV4l2DecOutputIOMode",
        dec_output_io_modes);
  }
  return v4l2_dec_output_io_mode;
}

GType
gst_v4l2_dec_capture_io_mode_get_type (void)
{
  static GType v4l2_dec_capture_io_mode = 0;

  if (!v4l2_dec_capture_io_mode) {
    static const GEnumValue dec_capture_io_modes[] = {
      {GST_V4L2_IO_AUTO, "GST_V4L2_IO_AUTO", "auto"},
      {GST_V4L2_IO_MMAP, "GST_V4L2_IO_MMAP", "mmap"},
      {0, NULL, NULL}
    };

    v4l2_dec_capture_io_mode = g_enum_register_static ("GstNvV4l2DecCaptureIOMode",
        dec_capture_io_modes);
  }
  return v4l2_dec_capture_io_mode;
}
#endif
#endif

/* prototypes */
static GstFlowReturn gst_v4l2_video_dec_finish (GstVideoDecoder * decoder);

#ifdef USE_V4L2_GST_HEADER_VER_1_8
/**
 * TODO: This function gst_pad_get_task_state is introduced in newer gstreamer
 * version which we use on Tegra but on Desktop we are still at 1.8.3 (16.04)
 * At the moment I am copying the code here but eventually when we move to 1.14,
 * this shouldn't be needed.
 * gst_pad_get_task_state:
 * @pad: the #GstPad to get task state from
 *
 * Get @pad task state. If no task is currently
 * set, GST_TASK_STOPPED is returned.
 *
 * Returns: The current state of @pad's task.
 */
GstTaskState
gst_pad_get_task_state (GstPad * pad)
{
  GstTask *task;
  GstTaskState res;

  g_return_val_if_fail (GST_IS_PAD (pad), GST_TASK_STOPPED);

  GST_OBJECT_LOCK (pad);
  task = GST_PAD_TASK (pad);
  if (task == NULL)
    goto no_task;
  res = gst_task_get_state (task);
  GST_OBJECT_UNLOCK (pad);

  return res;

no_task:
  {
    GST_DEBUG_OBJECT (pad, "pad has no task");
    GST_OBJECT_UNLOCK (pad);
    return GST_TASK_STOPPED;
  }
}
#endif

static void
gst_v4l2_video_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);


  switch (prop_id) {
    case PROP_CAPTURE_IO_MODE:
      if (!gst_v4l2_object_set_property_helper (self->v4l2capture,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
#ifdef USE_V4L2_TARGET_NV
    case PROP_SKIP_FRAME:
      self->skip_frames = g_value_get_enum (value);
      break;
    case PROP_DROP_FRAME_INTERVAL:
      self->drop_frame_interval = g_value_get_uint (value);
      break;
    case PROP_NUM_EXTRA_SURFACES:
      self->num_extra_surfaces = g_value_get_uint (value);
      break;
#ifndef USE_V4L2_TARGET_NV_CODECSDK
    case PROP_DISABLE_DPB:
      self->disable_dpb = g_value_get_boolean (value);
      break;

    case PROP_USE_FULL_FRAME:
      self->enable_full_frame = g_value_get_boolean (value);
      break;

    case PROP_ENABLE_FRAME_TYPE_REPORTING:
      self->enable_frame_type_reporting = g_value_get_boolean (value);
      self->v4l2capture->Enable_frame_type_reporting =
          g_value_get_boolean (value);
      break;

    case PROP_ENABLE_ERROR_CHECK:
      self->enable_error_check = g_value_get_boolean (value);
      self->v4l2capture->Enable_error_check = g_value_get_boolean (value);
      break;

    case PROP_ENABLE_MAX_PERFORMANCE:
      self->enable_max_performance = g_value_get_boolean (value);
      break;

    case PROP_NVBUF_API_VERSION:
      self->nvbuf_api_version_new = g_value_get_boolean (value);
      self->v4l2capture->nvbuf_api_version_new = self->nvbuf_api_version_new;
      self->v4l2output->nvbuf_api_version_new = self->nvbuf_api_version_new;
      break;

    case PROP_OPEN_MJPEG_BLOCK:
      self->v4l2output->open_mjpeg_block = g_value_get_boolean (value);
      break;
#else
    case PROP_CUDADEC_MEM_TYPE:
      self->cudadec_mem_type = g_value_get_enum (value);
      break;

    case PROP_CUDADEC_GPU_ID:
      self->cudadec_gpu_id = g_value_get_uint (value);
      break;
#endif
#endif
      /* By default, only set on output */
    default:
      if (!gst_v4l2_object_set_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_v4l2_video_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  switch (prop_id) {
    case PROP_CAPTURE_IO_MODE:
      if (!gst_v4l2_object_get_property_helper (self->v4l2capture,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
#ifdef USE_V4L2_TARGET_NV
    case PROP_SKIP_FRAME:
      g_value_set_enum (value, self->skip_frames);
      break;

    case PROP_DROP_FRAME_INTERVAL:
      g_value_set_uint (value, self->drop_frame_interval);
      break;

    case PROP_NUM_EXTRA_SURFACES:
      g_value_set_uint (value, self->num_extra_surfaces);
      break;

#ifndef USE_V4L2_TARGET_NV_CODECSDK
    case PROP_DISABLE_DPB:
      g_value_set_boolean (value, self->disable_dpb);
      break;

    case PROP_USE_FULL_FRAME:
      g_value_set_boolean (value, self->enable_full_frame);
      break;

    case PROP_ENABLE_FRAME_TYPE_REPORTING:
      g_value_set_boolean (value, self->enable_frame_type_reporting);
      break;

    case PROP_ENABLE_ERROR_CHECK:
      g_value_set_boolean (value, self->enable_error_check);
      break;

    case PROP_ENABLE_MAX_PERFORMANCE:
      g_value_set_boolean (value, self->enable_max_performance);
      break;

    case PROP_NVBUF_API_VERSION:
      g_value_set_boolean (value, self->nvbuf_api_version_new);
      break;

    case PROP_OPEN_MJPEG_BLOCK:
      g_value_set_boolean (value, self->v4l2output->open_mjpeg_block);
      break;
#else
    case PROP_CUDADEC_MEM_TYPE:
      g_value_set_enum(value, self->cudadec_mem_type);
      break;

    case PROP_CUDADEC_GPU_ID:
      g_value_set_uint(value, self->cudadec_gpu_id);
      break;
#endif
#endif
      /* By default read from output */
    default:
      if (!gst_v4l2_object_get_property_helper (self->v4l2output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static gboolean
gst_v4l2_video_dec_open (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstCaps *codec_caps;

  GST_DEBUG_OBJECT (self, "Opening");

  if (!gst_v4l2_object_open (self->v4l2output))
    goto failure;

  if (!gst_v4l2_object_open_shared (self->v4l2capture, self->v4l2output))
    goto failure;

#ifdef USE_V4L2_TARGET_NV
#ifndef USE_V4L2_TARGET_NV_CODECSDK
  // Downstream plugins may export this environment variable
  if (!g_strcmp0 (g_getenv("DS_NEW_BUFAPI"), "1")){
    self->v4l2capture->nvbuf_api_version_new = TRUE;
    self->v4l2output->nvbuf_api_version_new = TRUE;
    self->nvbuf_api_version_new = TRUE;
  }
#endif
#endif

  codec_caps = gst_pad_get_pad_template_caps (decoder->sinkpad);
  self->probed_sinkcaps = gst_v4l2_object_probe_caps (self->v4l2output,
      codec_caps);
  gst_caps_unref (codec_caps);

  if (gst_caps_is_empty (self->probed_sinkcaps))
    goto no_encoded_format;

  self->probed_srccaps = gst_v4l2_object_probe_caps (self->v4l2capture,
      gst_v4l2_object_get_raw_caps ());

  if (gst_caps_is_empty (self->probed_srccaps))
    goto no_raw_format;

  return TRUE;

no_encoded_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Encoder on device %s has no supported input format"),
          self->v4l2output->videodev), (NULL));
  goto failure;


no_raw_format:
  GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
      (_("Encoder on device %s has no supported output format"),
          self->v4l2output->videodev), (NULL));
  goto failure;

failure:
  if (GST_V4L2_IS_OPEN (self->v4l2output))
    gst_v4l2_object_close (self->v4l2output);

  if (GST_V4L2_IS_OPEN (self->v4l2capture))
    gst_v4l2_object_close (self->v4l2capture);

  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return FALSE;
}

static gboolean
gst_v4l2_video_dec_close (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing");

  gst_v4l2_object_close (self->v4l2output);
  gst_v4l2_object_close (self->v4l2capture);
  gst_caps_replace (&self->probed_srccaps, NULL);
  gst_caps_replace (&self->probed_sinkcaps, NULL);

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_start (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");

  gst_v4l2_object_unlock (self->v4l2output);
  g_atomic_int_set (&self->active, TRUE);
  self->output_flow = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_stop (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  gst_v4l2_object_unlock (self->v4l2output);
  gst_v4l2_object_unlock (self->v4l2capture);

  /* Wait for capture thread to stop */
#ifdef USE_V4L2_TARGET_NV
  set_v4l2_video_mpeg_class (self->v4l2capture,
              V4L2_CID_MPEG_SET_POLL_INTERRUPT, 0);
#endif
  gst_pad_stop_task (decoder->srcpad);

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  self->output_flow = GST_FLOW_OK;
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);

  gst_v4l2_object_stop (self->v4l2output);
  gst_v4l2_object_stop (self->v4l2capture);

  if (self->input_state) {
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstV4l2Error error = GST_V4L2_ERROR_INIT;
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  if (self->input_state) {
    if (gst_v4l2_object_caps_equal (self->v4l2output, state->caps)) {
      GST_DEBUG_OBJECT (self, "Compatible caps");
      goto done;
    }
    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    gst_v4l2_video_dec_finish (decoder);
    gst_v4l2_object_stop (self->v4l2output);

    /* The renegotiation flow don't blend with the base class flow. To
     * properly stop the capture pool we need to reclaim our buffers, which
     * will happend through the allocation query. The allocation query is
     * triggered by gst_video_decoder_negotiate() which requires the output
     * caps to be set, but we can't know this information as we rely on the
     * decoder, which requires the capture queue to be stopped.
     *
     * To workaround this issue, we simply run an allocation query with the
     * old negotiated caps in order to drain/reclaim our buffers. That breaks
     * the complexity and should not have much impact in performance since the
     * following allocation query will happen on a drained pipeline and won't
     * block. */
    {
      GstCaps *caps = gst_pad_get_current_caps (decoder->srcpad);
      GstQuery *query = gst_query_new_allocation (caps, FALSE);
      gst_pad_peer_query (decoder->srcpad, query);
      gst_query_unref (query);
      gst_caps_unref (caps);
    }

    gst_v4l2_object_stop (self->v4l2capture);
    self->output_flow = GST_FLOW_OK;
  }

  ret = gst_v4l2_object_set_format (self->v4l2output, state->caps, &error);

  if (ret)
    self->input_state = gst_video_codec_state_ref (state);
  else
    gst_v4l2_error (self, &error);

#ifdef USE_V4L2_TARGET_NV
  {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT, 0)) {
      g_print ("S_EXT_CTRLS for DISABLE_COMPLETE_FRAME_INPUT failed\n");
      return FALSE;
    }
  }

  if (self->skip_frames != V4L2_SKIP_FRAMES_TYPE_NONE) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_SKIP_FRAMES,
        self->skip_frames)) {
      g_print ("S_EXT_CTRLS for SKIP_FRAMES failed\n");
      return FALSE;
    }
  }

  if (self->drop_frame_interval != 0) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEODEC_DROP_FRAME_INTERVAL,
        self->drop_frame_interval)) {
      g_print ("S_EXT_CTRLS for DROP_FRAME_INTERVAL failed\n");
      return FALSE;
    }
  }
#ifndef USE_V4L2_TARGET_NV_CODECSDK
  if (self->disable_dpb != DEFAULT_DISABLE_DPB) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_DISABLE_DPB,
        self->disable_dpb)) {
      g_print ("S_EXT_CTRLS for DISABLE_DPB failed\n");
      return FALSE;
    }
  }

  if (self->enable_full_frame != DEFAULT_FULL_FRAME) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT,
        self->enable_full_frame)) {
      g_print ("S_EXT_CTRLS for DISABLE_COMPLETE_FRAME_INPUT failed\n");
      return FALSE;
    }
  }

  if (self->enable_frame_type_reporting) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_ERROR_REPORTING,
        self->enable_frame_type_reporting)) {
      g_print ("S_EXT_CTRLS for ERROR_REPORTING failed\n");
      return FALSE;
    }
  }

  if (self->enable_error_check) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_ERROR_REPORTING,
        self->enable_error_check)) {
      g_print ("S_EXT_CTRLS for ERROR_REPORTING failed\n");
      return FALSE;
    }
  }

  if (self->enable_max_performance != DEFAULT_MAX_PERFORMANCE) {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_MAX_PERFORMANCE,
        self->enable_max_performance)) {
      g_print ("S_EXT_CTRLS for MAX_PERFORMANCE failed\n");
      return FALSE;
    }
  }
#else
  {
    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_CUDA_MEM_TYPE,
        self->cudadec_mem_type)) {
      g_print ("S_EXT_CTRLS for CUDA_MEM_TYPE failed\n");
      return FALSE;
    }

    if (!set_v4l2_video_mpeg_class (self->v4l2output,
        V4L2_CID_MPEG_VIDEO_CUDA_GPU_ID,
        self->cudadec_gpu_id)) {
      g_print ("S_EXT_CTRLS for CUDA_GPU_ID failed\n");
      return FALSE;
    }
  }
#endif
#endif

done:
  return ret;
}

static gboolean
gst_v4l2_video_dec_flush (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushed");

  /* Ensure the processing thread has stopped for the reverse playback
   * discount case */
  if (gst_pad_get_task_state (decoder->srcpad) == GST_TASK_STARTED) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

    gst_v4l2_object_unlock (self->v4l2output);
    gst_v4l2_object_unlock (self->v4l2capture);
#ifdef USE_V4L2_TARGET_NV
    set_v4l2_video_mpeg_class (self->v4l2capture,
              V4L2_CID_MPEG_SET_POLL_INTERRUPT, 0);
#endif
    gst_pad_stop_task (decoder->srcpad);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }

  self->output_flow = GST_FLOW_OK;

  gst_v4l2_object_unlock_stop (self->v4l2output);
  gst_v4l2_object_unlock_stop (self->v4l2capture);

  if (self->v4l2output->pool)
    gst_v4l2_buffer_pool_flush (self->v4l2output->pool);

  if (self->v4l2capture->pool)
    gst_v4l2_buffer_pool_flush (self->v4l2capture->pool);

  return TRUE;
}

static gboolean
gst_v4l2_video_dec_negotiate (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  /* We don't allow renegotiation without carefull disabling the pool */
  if (self->v4l2capture->pool &&
      gst_buffer_pool_is_active (GST_BUFFER_POOL (self->v4l2capture->pool)))
    return TRUE;

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static gboolean
gst_v4l2_decoder_cmd (GstV4l2Object * v4l2object, guint cmd, guint flags)
{
  struct v4l2_decoder_cmd dcmd = { 0, };

  GST_DEBUG_OBJECT (v4l2object->element,
      "sending v4l2 decoder command %u with flags %u", cmd, flags);

  if (!GST_V4L2_IS_OPEN (v4l2object))
    return FALSE;

  dcmd.cmd = cmd;
  dcmd.flags = flags;
  if (v4l2object->ioctl (v4l2object->video_fd, VIDIOC_DECODER_CMD, &dcmd) < 0)
    goto dcmd_failed;

  return TRUE;

dcmd_failed:
  if (errno == ENOTTY) {
    GST_INFO_OBJECT (v4l2object->element,
        "Failed to send decoder command %u with flags %u for '%s'. (%s)",
        cmd, flags, v4l2object->videodev, g_strerror (errno));
  } else {
    GST_ERROR_OBJECT (v4l2object->element,
        "Failed to send decoder command %u with flags %u for '%s'. (%s)",
        cmd, flags, v4l2object->videodev, g_strerror (errno));
  }
  return FALSE;
}

static GstFlowReturn
gst_v4l2_video_dec_finish (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  GstBuffer *buffer;

  if (gst_pad_get_task_state (decoder->srcpad) != GST_TASK_STARTED)
    goto done;

  GST_DEBUG_OBJECT (self, "Finishing decoding");

  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

#ifndef USE_V4L2_TARGET_NV
  if (gst_v4l2_decoder_cmd (self->v4l2output, V4L2_DEC_CMD_STOP, 0)) {
#else
  if (gst_v4l2_decoder_cmd (self->v4l2output, V4L2_DEC_CMD_STOP,
          V4L2_DEC_CMD_STOP_TO_BLACK)) {
#endif
    GstTask *task = decoder->srcpad->task;

    /* If the decoder stop command succeeded, just wait until processing is
     * finished */
    GST_OBJECT_LOCK (task);
#ifdef USE_V4L2_TARGET_NV
    GST_TASK_STATE (task) = GST_TASK_PAUSED;
    GST_TASK_STATE (task) = GST_TASK_STARTED;
#endif
    while (GST_TASK_STATE (task) == GST_TASK_STARTED)
      GST_TASK_WAIT (task);
    GST_OBJECT_UNLOCK (task);
    ret = GST_FLOW_FLUSHING;
  } else {
    /* otherwise keep queuing empty buffers until the processing thread has
     * stopped, _pool_process() will return FLUSHING when that happened */
    while (ret == GST_FLOW_OK) {
      buffer = gst_buffer_new ();
      ret =
          gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->
              v4l2output->pool), &buffer);
      gst_buffer_unref (buffer);
    }
  }

  /* and ensure the processing thread has stopped in case another error
   * occured. */
  gst_v4l2_object_unlock (self->v4l2capture);
#ifdef USE_V4L2_TARGET_NV
  set_v4l2_video_mpeg_class (self->v4l2capture,
              V4L2_CID_MPEG_SET_POLL_INTERRUPT, 0);
#endif
  gst_pad_stop_task (decoder->srcpad);
  GST_VIDEO_DECODER_STREAM_LOCK (decoder);

  if (ret == GST_FLOW_FLUSHING)
    ret = self->output_flow;

  GST_DEBUG_OBJECT (decoder, "Done draining buffers");

  /* TODO Shall we cleanup any reffed frame to workaround broken decoders ? */

done:
  return ret;
}

static gboolean
gst_v4l2_video_dec_drain (GstVideoDecoder * decoder)
{
#if ENABLE_DRAIN
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Draining...");
  gst_v4l2_video_dec_finish (decoder);
  gst_v4l2_video_dec_flush (decoder);

  return TRUE;
#else
  return TRUE;
#endif
}

static GstVideoCodecFrame *
gst_v4l2_video_dec_get_oldest_frame (GstVideoDecoder * decoder)
{
  GstVideoCodecFrame *frame = NULL;
  GList *frames, *l;
  gint count = 0;

  frames = gst_video_decoder_get_frames (decoder);

  for (l = frames; l != NULL; l = l->next) {
    GstVideoCodecFrame *f = l->data;

    if (!frame || frame->pts > f->pts)
      frame = f;

    count++;
  }

  if (frame) {
    GST_LOG_OBJECT (decoder,
        "Oldest frame is %d %" GST_TIME_FORMAT " and %d frames left",
        frame->system_frame_number, GST_TIME_ARGS (frame->pts), count - 1);
    gst_video_codec_frame_ref (frame);
  }

  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);

  return frame;
}

static void
gst_v4l2_video_dec_loop (GstVideoDecoder * decoder)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstV4l2BufferPool *v4l2_pool = GST_V4L2_BUFFER_POOL (self->v4l2capture->pool);
  GstBufferPool *pool;
  GstVideoCodecFrame *frame;
  GstBuffer *buffer = NULL;
  GstFlowReturn ret;

  GST_LOG_OBJECT (decoder, "Allocate output buffer");

  self->output_flow = GST_FLOW_OK;
  do {
    /* We cannot use the base class allotate helper since it taking the internal
     * stream lock. we know that the acquire may need to poll until more frames
     * comes in and holding this lock would prevent that.
     */
    pool = gst_video_decoder_get_buffer_pool (decoder);

    /* Pool may be NULL if we started going to READY state */
    if (pool == NULL) {
      ret = GST_FLOW_FLUSHING;
      goto beach;
    }

    ret = gst_buffer_pool_acquire_buffer (pool, &buffer, NULL);
    g_object_unref (pool);

    if (ret != GST_FLOW_OK)
      goto beach;

    GST_LOG_OBJECT (decoder, "Process output buffer");
    ret = gst_v4l2_buffer_pool_process (v4l2_pool, &buffer);

  } while (ret == GST_V4L2_FLOW_CORRUPTED_BUFFER);

  if (ret != GST_FLOW_OK)
    goto beach;

  frame = gst_v4l2_video_dec_get_oldest_frame (decoder);
#ifdef USE_V4L2_TARGET_NV
#ifndef USE_V4L2_TARGET_NV_CODECSDK
  if (frame && self->enable_frame_type_reporting) {
    g_print ("Frame %d\n", frame->system_frame_number);
  }
#endif
#endif
  if (frame) {
    frame->output_buffer = buffer;
    buffer = NULL;

    if(enable_latency_measurement) /* TODO with better option */
    {
      BufferIdentification *id = (BufferIdentification *)gst_video_codec_frame_get_user_data (frame);
      GstCaps *reference = gst_caps_new_simple ("video/x-raw",
          "component_name", G_TYPE_STRING, GST_ELEMENT_NAME(self),
          "frame_num", G_TYPE_INT, self->frame_num++,
          "in_timestamp", G_TYPE_DOUBLE, id->in_timestamp,
          "out_timestamp", G_TYPE_DOUBLE, get_current_system_timestamp(),
          NULL);
      GstReferenceTimestampMeta * dec_meta =
        gst_buffer_add_reference_timestamp_meta (frame->output_buffer, reference,
            0, 0);
      if(dec_meta == NULL)
      {
        GST_DEBUG_OBJECT (decoder, "dec_meta: %p", dec_meta);
      }
      gst_caps_unref(reference);
    }

    ret = gst_video_decoder_finish_frame (decoder, frame);

    if (ret != GST_FLOW_OK)
      goto beach;
  } else {
    GST_WARNING_OBJECT (decoder, "Decoder is producing too many buffers");
    gst_buffer_unref (buffer);
  }

  return;

beach:
  GST_DEBUG_OBJECT (decoder, "Leaving output thread: %s",
      gst_flow_get_name (ret));

  gst_buffer_replace (&buffer, NULL);
  self->output_flow = ret;
  gst_v4l2_object_unlock (self->v4l2output);
  gst_pad_pause_task (decoder->srcpad);
}

static gboolean
gst_v4l2_video_remove_padding (GstCapsFeatures * features,
    GstStructure * structure, gpointer user_data)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (user_data);
  GstVideoAlignment *align = &self->v4l2capture->align;
  GstVideoInfo *info = &self->v4l2capture->info;
#ifndef USE_V4L2_TARGET_NV
  int width, height;

  if (!gst_structure_get_int (structure, "width", &width))
    return TRUE;

  if (!gst_structure_get_int (structure, "height", &height))
    return TRUE;
#else
  guint width, height;

  if (!gst_structure_get_int (structure, "width", (gint *) & width))
    return TRUE;

  if (!gst_structure_get_int (structure, "height", (gint *) & height))
    return TRUE;
#endif
  if (align->padding_left != 0 || align->padding_top != 0 ||
      height != info->height + align->padding_bottom)
    return TRUE;

  if (height == info->height + align->padding_bottom) {
    /* Some drivers may round up width to the padded with */
    if (width == info->width + align->padding_right)
      gst_structure_set (structure,
          "width", G_TYPE_INT, width - align->padding_right,
          "height", G_TYPE_INT, height - align->padding_bottom, NULL);
    /* Some drivers may keep visible width and only round up bytesperline */
#ifndef USE_V4L2_TARGET_NV
    else if (width == info->width)
#else
    else if (width == (guint) info->width)
#endif
      gst_structure_set (structure,
          "height", G_TYPE_INT, height - align->padding_bottom, NULL);
  }

  return TRUE;
}

static GstFlowReturn
gst_v4l2_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstV4l2Error error = GST_V4L2_ERROR_INIT;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean processed = FALSE;
  GstBuffer *tmp;
  GstTaskState task_state;
#ifdef USE_V4L2_TARGET_NV
  GstV4l2BufferPool *v4l2pool = GST_V4L2_BUFFER_POOL (self->v4l2output->pool);
  GstV4l2Object *obj = v4l2pool->obj;
#endif

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);


#ifdef USE_V4L2_TARGET_NV_CODECSDK
  if (self->skip_frames == V4L2_SKIP_FRAMES_TYPE_DECODE_IDR_ONLY) {
    // Decode only I Frames and drop others.
    if (GST_BUFFER_FLAG_IS_SET (GST_BUFFER_CAST(frame->input_buffer),
                                GST_BUFFER_FLAG_DELTA_UNIT)) {
      gst_video_decoder_drop_frame (decoder, frame);
      return GST_FLOW_OK;
    }
  }
#endif

  if(enable_latency_measurement)
  {
    BufferIdentification *id = g_slice_new0 (BufferIdentification);
    id->in_timestamp = get_current_system_timestamp();
    gst_video_codec_frame_set_user_data (frame, id,
        (GDestroyNotify) buffer_identification_free);
  }

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2output))) {
    if (!self->input_state)
      goto not_negotiated;
    if (!gst_v4l2_object_set_format (self->v4l2output, self->input_state->caps,
            &error))
      goto not_negotiated;
  }

  if (G_UNLIKELY (!GST_V4L2_IS_ACTIVE (self->v4l2capture))) {
    GstBufferPool *pool = GST_BUFFER_POOL (self->v4l2output->pool);
    GstVideoInfo info;
    GstVideoCodecState *output_state;
    GstBuffer *codec_data;
    GstCaps *acquired_caps, *available_caps, *caps, *filter;
    GstStructure *st;

    GST_DEBUG_OBJECT (self, "Sending header");

    codec_data = self->input_state->codec_data;

    /* We are running in byte-stream mode, so we don't know the headers, but
     * we need to send something, otherwise the decoder will refuse to
     * intialize.
     */
    if (codec_data) {
      gst_buffer_ref (codec_data);
    } else {
      codec_data = gst_buffer_ref (frame->input_buffer);
      processed = TRUE;
    }
    /* Ensure input internal pool is active */
    if (!gst_buffer_pool_is_active (pool)) {
      GstStructure *config = gst_buffer_pool_get_config (pool);
#ifndef USE_V4L2_TARGET_NV
      gst_buffer_pool_config_set_params (config, self->input_state->caps,
          self->v4l2output->info.size, 2, 2);
#else
      if (obj->mode != GST_V4L2_IO_USERPTR)
        obj->min_buffers = 2;

      if (V4L2_TYPE_IS_OUTPUT (obj->type)) {
        gst_buffer_pool_config_set_params (config, self->input_state->caps,
            self->v4l2output->info.size, obj->min_buffers, obj->min_buffers);
      }
#endif

      /* There is no reason to refuse this config */
      if (!gst_buffer_pool_set_config (pool, config))
        goto activate_failed;

      if (!gst_buffer_pool_set_active (pool, TRUE))
        goto activate_failed;
    }

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->
            v4l2output->pool), &codec_data);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    gst_buffer_unref (codec_data);

    /* TODO: nvparser should return proper format of a stream with first
     * few bytes of stream header*/
#ifdef USE_V4L2_TARGET_NV
    if (!processed) {
      processed = TRUE;
      GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
      ret =
          gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->v4l2output->
              pool), &frame->input_buffer);
      GST_VIDEO_DECODER_STREAM_LOCK (decoder);

      if (ret == GST_FLOW_FLUSHING) {
        if (gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self)) !=
            GST_TASK_STARTED)
          ret = self->output_flow;
        goto drop;
      } else if (ret != GST_FLOW_OK) {
        goto process_failed;
      }
    }

    if (V4L2_TYPE_IS_OUTPUT (obj->type)) {
      gint retval;
      struct v4l2_event ev;
      while (1) {
        memset (&ev, 0, sizeof (ev));
        retval = obj->ioctl (obj->video_fd, VIDIOC_DQEVENT, &ev);
        if (retval != 0)
        {
          usleep(100*1000); //TODO is this needed ?
          continue;
        }
        else
          break;
      }
    }
#endif

    /* For decoders G_FMT returns coded size, G_SELECTION returns visible size
     * in the compose rectangle. gst_v4l2_object_acquire_format() checks both
     * and returns the visible size as with/height and the coded size as
     * padding. */
    if (!gst_v4l2_object_acquire_format (self->v4l2capture, &info))
      goto not_negotiated;

    /* Create caps from the acquired format, remove the format field */
    acquired_caps = gst_video_info_to_caps (&info);
    GST_DEBUG_OBJECT (self, "Acquired caps: %" GST_PTR_FORMAT, acquired_caps);
    st = gst_caps_get_structure (acquired_caps, 0);
    gst_structure_remove_field (st, "format");

    /* Probe currently available pixel formats */
    available_caps = gst_v4l2_object_probe_caps (self->v4l2capture, NULL);
    available_caps = gst_caps_make_writable (available_caps);
    GST_DEBUG_OBJECT (self, "Available caps: %" GST_PTR_FORMAT, available_caps);

    /* Replace coded size with visible size, we want to negotiate visible size
     * with downstream, not coded size. */
    gst_caps_map_in_place (available_caps, gst_v4l2_video_remove_padding, self);

    filter = gst_caps_intersect_full (available_caps, acquired_caps,
        GST_CAPS_INTERSECT_FIRST);
    GST_DEBUG_OBJECT (self, "Filtered caps: %" GST_PTR_FORMAT, filter);
    gst_caps_unref (acquired_caps);
    gst_caps_unref (available_caps);
#ifndef USE_V4L2_TARGET_NV
    caps = gst_pad_peer_query_caps (decoder->srcpad, filter);
    gst_caps_unref (filter);
#else
    caps = gst_pad_peer_query_caps (decoder->srcpad,
        gst_pad_get_pad_template_caps (GST_VIDEO_DECODER_SRC_PAD (decoder)));
    gst_caps_unref (filter);

    if (gst_caps_is_empty (caps)) {
      GstPad *pad = GST_VIDEO_DECODER_SRC_PAD (decoder);
      caps = gst_pad_get_pad_template_caps (pad);
    }
#endif

    GST_DEBUG_OBJECT (self, "Possible decoded caps: %" GST_PTR_FORMAT, caps);
    if (gst_caps_is_empty (caps)) {
      gst_caps_unref (caps);
      goto not_negotiated;
    }

    /* Fixate pixel format */
    caps = gst_caps_fixate (caps);

    GST_DEBUG_OBJECT (self, "Chosen decoded caps: %" GST_PTR_FORMAT, caps);

    /* Try to set negotiated format, on success replace acquired format */
#ifndef USE_V4L2_TARGET_NV
    if (gst_v4l2_object_set_format (self->v4l2capture, caps, &error))
      gst_video_info_from_caps (&info, caps);
    else
      gst_v4l2_clear_error (&error);
#endif
    gst_caps_unref (caps);

    output_state = gst_video_decoder_set_output_state (decoder,
        info.finfo->format, info.width, info.height, self->input_state);

#ifdef USE_V4L2_TARGET_NV
    output_state = gst_video_decoder_get_output_state (decoder);
    output_state->caps = gst_video_info_to_caps (&output_state->info);
    GstCapsFeatures *features = gst_caps_features_new ("memory:NVMM", NULL);
    gst_caps_set_features (output_state->caps, 0, features);
#endif
    /* Copy the rest of the information, there might be more in the future */
    output_state->info.interlace_mode = info.interlace_mode;
    gst_video_codec_state_unref (output_state);

    if (!gst_video_decoder_negotiate (decoder)) {
      if (GST_PAD_IS_FLUSHING (decoder->srcpad))
        goto flushing;
      else
        goto not_negotiated;
    }

    /* Ensure our internal pool is activated */
    if (!gst_buffer_pool_set_active (GST_BUFFER_POOL (self->v4l2capture->pool),
            TRUE))
      goto activate_failed;
  }

  task_state = gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self));
  if (task_state == GST_TASK_STOPPED || task_state == GST_TASK_PAUSED) {
    /* It's possible that the processing thread stopped due to an error */
    if (self->output_flow != GST_FLOW_OK &&
        self->output_flow != GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Processing loop stopped with error, leaving");
      ret = self->output_flow;
      goto drop;
    }

#ifdef USE_V4L2_TARGET_NV
    set_v4l2_video_mpeg_class (self->v4l2capture,
              V4L2_CID_MPEG_SET_POLL_INTERRUPT, 1);
#endif

    GST_DEBUG_OBJECT (self, "Starting decoding thread");

    /* Start the processing task, when it quits, the task will disable input
     * processing to unlock input if draining, or prevent potential block */
    self->output_flow = GST_FLOW_FLUSHING;
    if (!gst_pad_start_task (decoder->srcpad,
            (GstTaskFunction) gst_v4l2_video_dec_loop, self, NULL))
      goto start_task_failed;
  }

  if (!processed) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    ret =
        gst_v4l2_buffer_pool_process (GST_V4L2_BUFFER_POOL (self->v4l2output->
            pool), &frame->input_buffer);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    if (ret == GST_FLOW_FLUSHING) {
      if (gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self)) !=
          GST_TASK_STARTED)
        ret = self->output_flow;
      goto drop;
    } else if (ret != GST_FLOW_OK) {
      goto process_failed;
    }
  }


  /* No need to keep input arround */
  tmp = frame->input_buffer;
  frame->input_buffer = gst_buffer_new ();
  gst_buffer_copy_into (frame->input_buffer, tmp,
      GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS |
      GST_BUFFER_COPY_META, 0, 0);
  gst_buffer_unref (tmp);

  gst_video_codec_frame_unref (frame);
  return ret;

  /* ERRORS */
not_negotiated:
  {
    GST_ERROR_OBJECT (self, "not negotiated");
    ret = GST_FLOW_NOT_NEGOTIATED;
    gst_v4l2_error (self, &error);
    goto drop;
  }
activate_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        (_("Failed to allocate required memory.")),
        ("Buffer pool activation failed"));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
flushing:
  {
    ret = GST_FLOW_FLUSHING;
    goto drop;
  }

start_task_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (_("Failed to start decoding thread.")), (NULL));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
process_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        (_("Failed to process frame.")),
        ("Maybe be due to not enough memory or failing driver"));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
drop:
  {
    gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }
}

static gboolean
gst_v4l2_video_dec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstClockTime latency;
  gboolean ret = FALSE;

  if (gst_v4l2_object_decide_allocation (self->v4l2capture, query))
    ret = GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
        query);

  if (GST_CLOCK_TIME_IS_VALID (self->v4l2capture->duration)) {
    latency = self->v4l2capture->min_buffers * self->v4l2capture->duration;
    GST_DEBUG_OBJECT (self, "Setting latency: %" GST_TIME_FORMAT " (%"
        G_GUINT32_FORMAT " * %" G_GUINT64_FORMAT, GST_TIME_ARGS (latency),
        self->v4l2capture->min_buffers, self->v4l2capture->duration);
    gst_video_decoder_set_latency (decoder, latency, latency);
  } else {
    GST_WARNING_OBJECT (self, "Duration invalid, not setting latency");
  }

  return ret;
}

static gboolean
gst_v4l2_video_dec_src_query (GstVideoDecoder * decoder, GstQuery * query)
{
  gboolean ret = TRUE;
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *filter, *result = NULL;
      GstPad *pad = GST_VIDEO_DECODER_SRC_PAD (decoder);

      gst_query_parse_caps (query, &filter);

      if (self->probed_srccaps)
        result = gst_caps_ref (self->probed_srccaps);
      else
        result = gst_pad_get_pad_template_caps (pad);

      if (filter) {
        GstCaps *tmp = result;
        result =
            gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (tmp);
      }

      GST_DEBUG_OBJECT (self, "Returning src caps %" GST_PTR_FORMAT, result);

      gst_query_set_caps_result (query, result);
      gst_caps_unref (result);
      break;
    }

    default:
      ret = GST_VIDEO_DECODER_CLASS (parent_class)->src_query (decoder, query);
      break;
  }

  return ret;
}

static GstCaps *
gst_v4l2_video_dec_sink_getcaps (GstVideoDecoder * decoder, GstCaps * filter)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  GstCaps *result;

  result = gst_video_decoder_proxy_getcaps (decoder, self->probed_sinkcaps,
      filter);

  GST_DEBUG_OBJECT (self, "Returning sink caps %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_v4l2_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (decoder);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "flush start");
      gst_v4l2_object_unlock (self->v4l2output);
      gst_v4l2_object_unlock (self->v4l2capture);
      break;
    default:
      break;
  }

  ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      /* The processing thread should stop now, wait for it */
      gst_pad_stop_task (decoder->srcpad);
      GST_DEBUG_OBJECT (self, "flush start done");
      break;
    default:
      break;
  }

  return ret;
}

static GstStateChangeReturn
gst_v4l2_video_dec_change_state (GstElement * element,
    GstStateChange transition)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (element);
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    gst_v4l2_object_unlock (self->v4l2output);
    gst_v4l2_object_unlock (self->v4l2capture);
#ifdef USE_V4L2_TARGET_NV
    set_v4l2_video_mpeg_class (self->v4l2capture,
              V4L2_CID_MPEG_SET_POLL_INTERRUPT, 0);
#endif
    gst_pad_stop_task (decoder->srcpad);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_v4l2_video_dec_dispose (GObject * object)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  gst_caps_replace (&self->probed_sinkcaps, NULL);
  gst_caps_replace (&self->probed_srccaps, NULL);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_v4l2_video_dec_finalize (GObject * object)
{
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (object);

  gst_v4l2_object_destroy (self->v4l2capture);
  gst_v4l2_object_destroy (self->v4l2output);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_v4l2_video_dec_init (GstV4l2VideoDec * self)
{
  /* V4L2 object are created in subinstance_init */
#ifdef USE_V4L2_TARGET_NV
  self->skip_frames = DEFAULT_SKIP_FRAME_TYPE;
  self->nvbuf_api_version_new = DEFAULT_NVBUF_API_VERSION_NEW;
  self->drop_frame_interval = 0;
  self->num_extra_surfaces = DEFAULT_NUM_EXTRA_SURFACES;
#ifndef USE_V4L2_TARGET_NV_CODECSDK
  self->disable_dpb = DEFAULT_DISABLE_DPB;
  self->enable_full_frame = DEFAULT_FULL_FRAME;
  self->enable_frame_type_reporting = DEFAULT_FRAME_TYPR_REPORTING;
  self->enable_error_check = DEFAULT_ERROR_CHECK;
  self->enable_max_performance = DEFAULT_MAX_PERFORMANCE;
#else
  self->cudadec_mem_type = DEFAULT_CUDADEC_MEM_TYPE;
  self->cudadec_gpu_id = DEFAULT_CUDADEC_GPU_ID;
#endif
#endif

  const gchar * latency = g_getenv("NVDS_ENABLE_LATENCY_MEASUREMENT");
  if(latency)
  {
    enable_latency_measurement = TRUE;
  }
}

static void
gst_v4l2_video_dec_subinstance_init (GTypeInstance * instance, gpointer g_class)
{
  GstV4l2VideoDecClass *klass = GST_V4L2_VIDEO_DEC_CLASS (g_class);
  GstV4l2VideoDec *self = GST_V4L2_VIDEO_DEC (instance);
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (instance);

  gst_video_decoder_set_packetized (decoder, TRUE);

  self->v4l2output = gst_v4l2_object_new (GST_ELEMENT (self),
      GST_OBJECT (GST_VIDEO_DECODER_SINK_PAD (self)),
      V4L2_BUF_TYPE_VIDEO_OUTPUT, klass->default_device,
      gst_v4l2_get_output, gst_v4l2_set_output, NULL);
  self->v4l2output->no_initial_format = TRUE;
  self->v4l2output->keep_aspect = FALSE;

  self->v4l2capture = gst_v4l2_object_new (GST_ELEMENT (self),
      GST_OBJECT (GST_VIDEO_DECODER_SRC_PAD (self)),
      V4L2_BUF_TYPE_VIDEO_CAPTURE, klass->default_device,
      gst_v4l2_get_input, gst_v4l2_set_input, NULL);
  self->v4l2capture->no_initial_format = TRUE;
  self->v4l2output->keep_aspect = FALSE;
#ifdef USE_V4L2_TARGET_NV
  self->v4l2capture->nvbuf_api_version_new = self->nvbuf_api_version_new;
  self->v4l2output->nvbuf_api_version_new = self->nvbuf_api_version_new;
#endif
}

static void
gst_v4l2_video_dec_class_init (GstV4l2VideoDecClass * klass)
{
  GstElementClass *element_class;
  GObjectClass *gobject_class;
  GstVideoDecoderClass *video_decoder_class;

  parent_class = g_type_class_peek_parent (klass);

  element_class = (GstElementClass *) klass;
  gobject_class = (GObjectClass *) klass;
  video_decoder_class = (GstVideoDecoderClass *) klass;

#ifndef USE_V4L2_TARGET_NV
  GST_DEBUG_CATEGORY_INIT (gst_v4l2_video_dec_debug, "v4l2videodec", 0,
      "V4L2 Video Decoder");
#else
  GST_DEBUG_CATEGORY_INIT (gst_v4l2_video_dec_debug, "v4l2videodec", 0,
      "NVIDIA V4L2 Video Decoder");
#endif

  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_finalize);
  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_get_property);
#ifdef USE_V4L2_TARGET_NV
  g_object_class_install_property (gobject_class, PROP_SKIP_FRAME,
      g_param_spec_enum ("skip-frames",
          "Skip frames",
          "Type of frames to skip during decoding",
          GST_TYPE_V4L2_VID_DEC_SKIP_FRAMES,
          DEFAULT_SKIP_FRAME_TYPE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_PLAYING));

  g_object_class_install_property (gobject_class, PROP_DROP_FRAME_INTERVAL,
      g_param_spec_uint ("drop-frame-interval",
          "Drop frames interval",
          "Interval to drop the frames,ex: value of 5 means every 5th frame will be given by decoder, rest all dropped",
          0,
          30, 30,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_NUM_EXTRA_SURFACES,
      g_param_spec_uint ("num-extra-surfaces",
          "Number of extra surfaces",
          "Additional number of surfaces in addition to min decode surfaces given by the v4l2 driver",
          0,
          24, 24,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
#ifndef USE_V4L2_TARGET_NV_CODECSDK
  g_object_class_install_property (gobject_class, PROP_DISABLE_DPB,
      g_param_spec_boolean ("disable-dpb",
          "Disable DPB buffer",
          "Set to disable DPB buffer for low latency",
          DEFAULT_DISABLE_DPB, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_USE_FULL_FRAME,
      g_param_spec_boolean ("enable-full-frame",
          "Full Frame",
          "Whether or not the data is full framed",
          DEFAULT_FULL_FRAME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      PROP_ENABLE_FRAME_TYPE_REPORTING,
      g_param_spec_boolean ("enable-frame-type-reporting",
          "enable-frame-type-reporting", "Set to enable frame type reporting",
          DEFAULT_FRAME_TYPR_REPORTING,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_OPEN_MJPEG_BLOCK,
      g_param_spec_boolean ("mjpeg",
          "Open MJPEG Block",
          "Set to open MJPEG block",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ENABLE_ERROR_CHECK,
      g_param_spec_boolean ("enable-error-check",
          "enable-error-check",
          "Set to enable error check",
          DEFAULT_ERROR_CHECK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ENABLE_MAX_PERFORMANCE,
      g_param_spec_boolean ("enable-max-performance",
          "Enable max performance", "Set to enable max performance",
          DEFAULT_MAX_PERFORMANCE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NVBUF_API_VERSION,
      g_param_spec_boolean ("bufapi-version",
          "Use new buf API",
          "Set to use new buf API",
          DEFAULT_NVBUF_API_VERSION_NEW, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
#else
  g_object_class_install_property (gobject_class, PROP_CUDADEC_MEM_TYPE,
      g_param_spec_enum ("cudadec-memtype",
          "Memory type for cuda decoder buffers",
          "Set to specify memory type for cuda decoder buffers",
          GST_TYPE_V4L2_VID_CUDADEC_MEM_TYPE, DEFAULT_CUDADEC_MEM_TYPE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_CUDADEC_GPU_ID,
      g_param_spec_uint ("gpu-id",
          "GPU Device ID",
          "Set to GPU Device ID for decoder ",
          0,
          G_MAXUINT, DEFAULT_CUDADEC_GPU_ID,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

#endif
#endif

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_stop);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_finish);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_flush);
  video_decoder_class->drain = GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_drain);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_set_format);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_negotiate);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_decide_allocation);
  /* FIXME propose_allocation or not ? */
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_handle_frame);
  video_decoder_class->getcaps =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_sink_getcaps);
  video_decoder_class->src_query =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_src_query);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_sink_event);

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_v4l2_video_dec_change_state);

  gst_v4l2_object_install_m2m_properties_helper (gobject_class);
#ifdef USE_V4L2_TARGET_NV
#ifndef USE_V4L2_TARGET_NV_CODECSDK
  gst_v4l2_object_install_m2m_dec_iomode_properties_helper (gobject_class);
#endif
#endif
}

static void
gst_v4l2_video_dec_subclass_init (gpointer g_class, gpointer data)
{
  GstV4l2VideoDecClass *klass = GST_V4L2_VIDEO_DEC_CLASS (g_class);
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstV4l2VideoDecCData *cdata = data;

  klass->default_device = cdata->device;

#ifndef USE_V4L2_TARGET_NV
  /* Note: gst_pad_template_new() take the floating ref from the caps */
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          cdata->sink_caps));
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          cdata->src_caps));

  gst_element_class_set_static_metadata (element_class, cdata->longname,
      "Codec/Decoder/Video", cdata->description,
      "Nicolas Dufresne <nicolas.dufresne@collabora.com>");

  gst_caps_unref (cdata->sink_caps);
  gst_caps_unref (cdata->src_caps);
#else
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_v4l2dec_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_v4l2dec_src_template));

  gst_element_class_set_static_metadata (element_class,
      "NVIDIA v4l2 video decoder" /*cdata->longname */ ,
      "Codec/Decoder/Video",
      "Decode video streams via V4L2 API" /*cdata->description */ ,
      "Nicolas Dufresne <nicolas.dufresne@collabora.com>, Viranjan Pagar <vpagar@nvidia.com>");
#endif

  g_free (cdata);
}

/* Probing functions */
gboolean
gst_v4l2_is_video_dec (GstCaps * sink_caps, GstCaps * src_caps)
{
  gboolean ret = FALSE;

  if (gst_caps_is_subset (sink_caps, gst_v4l2_object_get_codec_caps ())
      && gst_caps_is_subset (src_caps, gst_v4l2_object_get_raw_caps ()))
    ret = TRUE;

  return ret;
}

#ifndef USE_V4L2_TARGET_NV
static gchar *
gst_v4l2_video_dec_set_metadata (GstStructure * s, GstV4l2VideoDecCData * cdata,
    const gchar * basename)
{
  gchar *codec_name = NULL;
  gchar *type_name = NULL;

#define SET_META(codec) \
G_STMT_START { \
  cdata->longname = "V4L2 " codec " Decoder"; \
  cdata->description = "Decodes " codec " streams via V4L2 API"; \
  codec_name = g_ascii_strdown (codec, -1); \
} G_STMT_END

  if (gst_structure_has_name (s, "image/jpeg")) {
    SET_META ("JPEG");
  } else if (gst_structure_has_name (s, "video/mpeg")) {
    gint mpegversion = 0;
    gst_structure_get_int (s, "mpegversion", &mpegversion);

    if (mpegversion == 2) {
      SET_META ("MPEG2");
    } else {
      SET_META ("MPEG4");
    }
  } else if (gst_structure_has_name (s, "video/x-h263")) {
    SET_META ("H263");
  } else if (gst_structure_has_name (s, "video/x-h264")) {
    SET_META ("H264");
  } else if (gst_structure_has_name (s, "video/x-wmv")) {
    SET_META ("VC1");
  } else if (gst_structure_has_name (s, "video/x-vp8")) {
    SET_META ("VP8");
  } else if (gst_structure_has_name (s, "video/x-vp9")) {
    SET_META ("VP9");
  } else if (gst_structure_has_name (s, "video/x-bayer")) {
    SET_META ("BAYER");
  } else if (gst_structure_has_name (s, "video/x-sonix")) {
    SET_META ("SONIX");
  } else if (gst_structure_has_name (s, "video/x-pwc1")) {
    SET_META ("PWC1");
  } else if (gst_structure_has_name (s, "video/x-pwc2")) {
    SET_META ("PWC2");
  } else {
    /* This code should be kept on sync with the exposed CODEC type of format
     * from gstv4l2object.c. This warning will only occure in case we forget
     * to also add a format here. */
    gchar *s_str = gst_structure_to_string (s);
    g_warning ("Missing fixed name mapping for caps '%s', this is a GStreamer "
        "bug, please report at https://bugs.gnome.org", s_str);
    g_free (s_str);
  }

  if (codec_name) {
    type_name = g_strdup_printf ("v4l2%sdec", codec_name);
    if (g_type_from_name (type_name) != 0) {
      g_free (type_name);
      type_name = g_strdup_printf ("v4l2%s%sdec", basename, codec_name);
    }

    g_free (codec_name);
  }

  return type_name;
#undef SET_META
}

void
gst_v4l2_video_dec_register (GstPlugin * plugin, const gchar * basename,
    const gchar * device_path, GstCaps * sink_caps, GstCaps * src_caps)
{
  gint i;

  for (i = 0; i < gst_caps_get_size (sink_caps); i++) {
    GstV4l2VideoDecCData *cdata;
    GstStructure *s;
    GTypeQuery type_query;
    GTypeInfo type_info = { 0, };
    GType type, subtype;
    gchar *type_name;

    s = gst_caps_get_structure (sink_caps, i);

    cdata = g_new0 (GstV4l2VideoDecCData, 1);
    cdata->device = g_strdup (device_path);
    cdata->sink_caps = gst_caps_new_empty ();
    gst_caps_append_structure (cdata->sink_caps, gst_structure_copy (s));
    cdata->src_caps = gst_caps_ref (src_caps);
    type_name = gst_v4l2_video_dec_set_metadata (s, cdata, basename);

    /* Skip over if we hit an unmapped type */
    if (!type_name) {
      g_free (cdata);
      continue;
    }

    type = gst_v4l2_video_dec_get_type ();
    g_type_query (type, &type_query);
    memset (&type_info, 0, sizeof (type_info));
    type_info.class_size = type_query.class_size;
    type_info.instance_size = type_query.instance_size;
    type_info.class_init = gst_v4l2_video_dec_subclass_init;
    type_info.class_data = cdata;
    type_info.instance_init = gst_v4l2_video_dec_subinstance_init;

    subtype = g_type_register_static (type, type_name, &type_info, 0);
    if (!gst_element_register (plugin, type_name, GST_RANK_PRIMARY + 1,
            subtype))
      GST_WARNING ("Failed to register plugin '%s'", type_name);

    g_free (type_name);
  }
}

#else

void
gst_v4l2_video_dec_register (GstPlugin * plugin, const gchar * basename,
    const gchar * device_path, GstCaps * sink_caps, GstCaps * src_caps)
{
  GTypeQuery type_query;
  GTypeInfo type_info = { 0, };
  GType type, subtype;
  GstV4l2VideoDecCData *cdata;

  cdata = g_new0 (GstV4l2VideoDecCData, 1);
  cdata->device = g_strdup (device_path);

  type = gst_v4l2_video_dec_get_type ();
  g_type_query (type, &type_query);
  memset (&type_info, 0, sizeof (type_info));
  type_info.class_size = type_query.class_size;
  type_info.instance_size = type_query.instance_size;
  type_info.class_init = gst_v4l2_video_dec_subclass_init;
  type_info.class_data = cdata;
  type_info.instance_init = gst_v4l2_video_dec_subinstance_init;

  subtype = g_type_register_static (type, "nvv4l2decoder", &type_info, 0);
  gst_element_register (plugin, "nvv4l2decoder", GST_RANK_PRIMARY + 11, subtype);
}
#endif
