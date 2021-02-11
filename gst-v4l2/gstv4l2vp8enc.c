/*
 * Copyright (C) 2017 Collabora Inc.
 *    Author: Nicolas Dufresne <nicolas.dufresne@collabora.com>
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
#include "gstv4l2vp8enc.h"

#include <string.h>
#include <gst/gst-i18n-plugin.h>

GST_DEBUG_CATEGORY_STATIC (gst_v4l2_vp8_enc_debug);
#define GST_CAT_DEFAULT gst_v4l2_vp8_enc_debug

static GstStaticCaps src_template_caps =
GST_STATIC_CAPS ("video/x-vp8, profile=(string) { 0, 1, 2, 3 }");

enum
{
  PROP_0,
  V4L2_STD_OBJECT_PROPS,
#ifdef USE_V4L2_TARGET_NV
  PROP_ENABLE_HEADER,
#endif
  /* TODO */
};

#define gst_v4l2_vp8_enc_parent_class parent_class
G_DEFINE_TYPE (GstV4l2Vp8Enc, gst_v4l2_vp8_enc, GST_TYPE_V4L2_VIDEO_ENC);

static void
gst_v4l2_vp8_enc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  /* TODO */
#ifdef USE_V4L2_TARGET_NV
  GstV4l2Vp8Enc *self = GST_V4L2_VP8_ENC (object);
  GstV4l2VideoEnc *video_enc = GST_V4L2_VIDEO_ENC (object);

  switch (prop_id) {
    case PROP_ENABLE_HEADER:
      self->EnableHeaders = g_value_get_boolean (value);
      video_enc->v4l2capture->Enable_headers = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
#endif
}

static void
gst_v4l2_vp8_enc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  /* TODO */
#ifdef USE_V4L2_TARGET_NV
  GstV4l2Vp8Enc *self = GST_V4L2_VP8_ENC (object);

  switch (prop_id) {
    case PROP_ENABLE_HEADER:
      g_value_set_boolean (value, self->EnableHeaders);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
#endif
}

static gint
v4l2_profile_from_string (const gchar * profile)
{
  gint v4l2_profile = -1;

  if (g_str_equal (profile, "0"))
    v4l2_profile = 0;
  else if (g_str_equal (profile, "1"))
    v4l2_profile = 1;
  else if (g_str_equal (profile, "2"))
    v4l2_profile = 2;
  else if (g_str_equal (profile, "3"))
    v4l2_profile = 3;
  else
    GST_WARNING ("Unsupported profile string '%s'", profile);

  return v4l2_profile;
}

static const gchar *
v4l2_profile_to_string (gint v4l2_profile)
{
  switch (v4l2_profile) {
    case 0:
      return "0";
    case 1:
      return "1";
    case 2:
      return "2";
    case 3:
      return "3";
    default:
      GST_WARNING ("Unsupported V4L2 profile %i", v4l2_profile);
      break;
  }

  return NULL;
}

static void
gst_v4l2_vp8_enc_init (GstV4l2Vp8Enc * self)
{
}

static void
gst_v4l2_vp8_enc_class_init (GstV4l2Vp8EncClass * klass)
{
  GstElementClass *element_class;
  GObjectClass *gobject_class;
  GstV4l2VideoEncClass *baseclass;

  parent_class = g_type_class_peek_parent (klass);

  element_class = (GstElementClass *) klass;
  gobject_class = (GObjectClass *) klass;
  baseclass = (GstV4l2VideoEncClass *) (klass);


  GST_DEBUG_CATEGORY_INIT (gst_v4l2_vp8_enc_debug, "v4l2vp8enc", 0,
      "V4L2 VP8 Encoder");

  gst_element_class_set_static_metadata (element_class,
      "V4L2 VP8 Encoder",
      "Codec/Encoder/Video",
      "Encode VP8 video streams via V4L2 API",
      "Nicolas Dufresne <nicolas.dufresne@collabora.com");

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_vp8_enc_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_v4l2_vp8_enc_get_property);

#ifdef USE_V4L2_TARGET_NV
  g_object_class_install_property (gobject_class, PROP_ENABLE_HEADER,
      g_param_spec_boolean ("enable-headers",
          "Enable VP8 headers",
          "Enable VP8 file and frame headers, if enabled, dump elementary stream",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
#endif
  baseclass->codec_name = "VP8";
  baseclass->profile_cid = V4L2_CID_MPEG_VIDEO_VPX_PROFILE;
  baseclass->profile_to_string = v4l2_profile_to_string;
  baseclass->profile_from_string = v4l2_profile_from_string;
}

/* Probing functions */
gboolean
gst_v4l2_is_vp8_enc (GstCaps * sink_caps, GstCaps * src_caps)
{
  return gst_v4l2_is_video_enc (sink_caps, src_caps,
      gst_static_caps_get (&src_template_caps));
}

void
gst_v4l2_vp8_enc_register (GstPlugin * plugin, const gchar * basename,
    const gchar * device_path, GstCaps * sink_caps, GstCaps * src_caps)
{
  gst_v4l2_video_enc_register (plugin, GST_TYPE_V4L2_VP8_ENC,
      "vp8", basename, device_path, sink_caps,
      gst_static_caps_get (&src_template_caps), src_caps);
}
