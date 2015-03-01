/*
 * gst-droid
 *
 * Copyright (C) 2014-2015 Mohammed Sameer <msameer@foolab.org>
 * Copyright (C) 2015 Jolla LTD.
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
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstdroidenc.h"
#include "gst/droid/gstwrappedmemory.h"
#include "plugin.h"
#include <string.h>

#define gst_droidenc_parent_class parent_class
G_DEFINE_TYPE (GstDroidEnc, gst_droidenc, GST_TYPE_VIDEO_ENCODER);

GST_DEBUG_CATEGORY_EXTERN (gst_droid_enc_debug);
#define GST_CAT_DEFAULT gst_droid_enc_debug

static GstStaticPadTemplate gst_droidenc_sink_template_factory =
GST_STATIC_PAD_TEMPLATE (GST_VIDEO_ENCODER_SINK_NAME,
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_DROID_VIDEO_META_DATA, "{YV12}")));

enum
{
  PROP_0,
  PROP_TARGET_BITRATE,
};

#define GST_DROID_ENC_TARGET_BITRATE_DEFAULT 192000

static GstVideoCodecState *gst_droidenc_configure_state (GstDroidEnc * enc,
    GstCaps * caps);
static void gst_droidenc_signal_eos (void *data);
static void gst_droidenc_error (void *data, int err);
static void
gst_droidenc_data_available (void *data, DroidMediaCodecData * encoded);

static gboolean
gst_droidenc_negotiate_src_caps (GstDroidEnc * enc)
{
  GstCaps *caps;

  GST_DEBUG_OBJECT (enc, "negotiate src caps");

  caps =
      gst_pad_peer_query_caps (GST_VIDEO_ENCODER_SRC_PAD (GST_VIDEO_ENCODER
          (enc)), NULL);

  GST_LOG_OBJECT (enc, "peer caps %" GST_PTR_FORMAT, caps);

  caps = gst_caps_truncate (caps);

  enc->codec_type =
      gst_droid_codec_get_from_caps (caps, GST_DROID_CODEC_ENCODER);
  if (!enc->codec_type) {
    GST_ELEMENT_ERROR (enc, LIBRARY, FAILED, (NULL),
        ("Unknown codec type for caps %" GST_PTR_FORMAT, caps));

    gst_caps_unref (caps);
    goto error;
  }

  /* ownership of caps is transferred */
  enc->out_state = gst_droidenc_configure_state (enc, caps);

  return TRUE;

error:
  return FALSE;
}

static gboolean
gst_droidenc_create_codec (GstDroidEnc * enc)
{
  DroidMediaCodecEncoderMetaData md;

  GST_INFO_OBJECT (enc, "create codec of type %s: %dx%d",
      enc->codec_type->droid, enc->in_state->info.width,
      enc->in_state->info.height);
  md.parent.type = enc->codec_type->droid;
  md.parent.width = enc->in_state->info.width;
  md.parent.height = enc->in_state->info.height;
  md.parent.fps = enc->in_state->info.fps_n / enc->in_state->info.fps_d;        // TODO: bad
  md.parent.flags = DROID_MEDIA_CODEC_HW_ONLY;
  md.bitrate = enc->target_bitrate;
  md.stride = enc->in_state->info.width;
  md.slice_height = enc->in_state->info.height;

  /* TODO: get this from caps */
  md.meta_data = true;

  enc->codec = droid_media_codec_create_encoder (&md);

  if (!enc->codec) {
    GST_ELEMENT_ERROR (enc, LIBRARY, SETTINGS, NULL,
        ("Failed to create encoder"));
    return FALSE;
  }

  {
    DroidMediaCodecCallbacks cb;
    cb.signal_eos = gst_droidenc_signal_eos;
    cb.error = gst_droidenc_error;
    droid_media_codec_set_callbacks (enc->codec, &cb, enc);
  }

  {
    DroidMediaCodecDataCallbacks cb;
    cb.data_available = gst_droidenc_data_available;
    droid_media_codec_set_data_callbacks (enc->codec, &cb, enc);
  }

  if (!droid_media_codec_start (enc->codec)) {
    GST_ELEMENT_ERROR (enc, LIBRARY, INIT, (NULL),
        ("Failed to start the encoder"));

    droid_media_codec_destroy (enc->codec);
    enc->codec = NULL;
    return FALSE;
  }

  return TRUE;
}

static void
gst_droidenc_signal_eos (void *data)
{
  GstDroidEnc *enc = (GstDroidEnc *) data;

  GST_DEBUG_OBJECT (enc, "codec signaled EOS");

  g_mutex_lock (&enc->eos_lock);

  if (!enc->eos) {
    GST_WARNING_OBJECT (enc, "codec signaled EOS but we are not expecting it");
  }

  g_cond_signal (&enc->eos_cond);
  g_mutex_unlock (&enc->eos_lock);
}

static void
gst_droidenc_error (void *data, int err)
{
  GstDroidEnc *enc = (GstDroidEnc *) data;

  GST_DEBUG_OBJECT (enc, "codec error");

  g_mutex_lock (&enc->eos_lock);

  if (enc->eos) {
    /* Gotta love Android. We will ignore errors if we are expecting EOS */
    g_cond_signal (&enc->eos_cond);
    goto out;
  }

  GST_VIDEO_ENCODER_STREAM_LOCK (enc);
  enc->downstream_flow_ret = GST_FLOW_ERROR;
  GST_VIDEO_ENCODER_STREAM_UNLOCK (enc);

  GST_ELEMENT_ERROR (enc, LIBRARY, FAILED, NULL,
      ("error 0x%x from android codec", -err));

out:
  g_mutex_unlock (&enc->eos_lock);
}

static void
gst_droidenc_data_available (void *data, DroidMediaCodecData * encoded)
{
  GstVideoCodecFrame *frame;
  DroidMediaData out;
  GstFlowReturn flow_ret;
  GstDroidEnc *enc = (GstDroidEnc *) data;
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (enc);

  GST_DEBUG_OBJECT (enc, "data available");

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

  if (encoded->codec_config) {
    GstBuffer *codec_data = NULL;

    g_assert (enc->codec_type->construct_encoder_codec_data);

    GST_INFO_OBJECT (enc, "received codec_data");

    if (!enc->codec_type->construct_encoder_codec_data (encoded->data.data,
            encoded->data.size, &codec_data)) {

      GST_ELEMENT_ERROR (enc, STREAM, FORMAT, (NULL),
          ("Failed to construct codec_data. Expect corrupted stream"));
    }

    if (codec_data) {
      /* encoder is allowed to return a NULL codec_data */
      gst_buffer_replace (&enc->out_state->codec_data, codec_data);
    }

    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
    return;
  }

  frame = gst_video_encoder_get_oldest_frame (GST_VIDEO_ENCODER (enc));
  if (G_UNLIKELY (!frame)) {
    /* TODO: what should we do here? */
    GST_WARNING_OBJECT (enc, "buffer without frame");

    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
    return;
  }

  if (enc->codec_type->process_encoder_data) {
    if (!enc->codec_type->process_encoder_data (&(encoded->data), &out)) {
      /* TODO: error */
      GST_WARNING_OBJECT (enc, "failed to process data");
    } else {
      frame->output_buffer = gst_buffer_new_wrapped (out.data, out.size);
    }
  } else {
    frame->output_buffer =
        gst_video_encoder_allocate_output_buffer (GST_VIDEO_ENCODER (enc),
        encoded->data.size);
    gst_buffer_fill (frame->output_buffer, 0, encoded->data.data,
        encoded->data.size);
  }

  GST_BUFFER_PTS (frame->output_buffer) = encoded->ts;
  GST_BUFFER_DTS (frame->output_buffer) = encoded->decoding_ts;

  if (encoded->sync) {
    GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
  }

  flow_ret = gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (enc), frame);

  if (flow_ret == GST_FLOW_OK || flow_ret == GST_FLOW_FLUSHING) {
    goto out;
  } else if (flow_ret == GST_FLOW_EOS) {
    GST_INFO_OBJECT (enc, "eos");
  } else if (flow_ret < GST_FLOW_OK) {
    GST_ELEMENT_ERROR (enc, STREAM, FAILED,
        ("Internal data stream error."), ("stream stopped, reason %s",
            gst_flow_get_name (flow_ret)));
  }

out:
  enc->downstream_flow_ret = flow_ret;
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
}

static GstVideoCodecState *
gst_droidenc_configure_state (GstDroidEnc * enc, GstCaps * caps)
{
  GstVideoCodecState *out = NULL;

  GST_DEBUG_OBJECT (enc, "configure state: width: %d, height: %d",
      enc->in_state->info.width, enc->in_state->info.height);

  GST_LOG_OBJECT (enc, "caps %" GST_PTR_FORMAT, caps);

  caps = gst_caps_fixate (caps);

  if (enc->codec_type->compliment) {
    enc->codec_type->compliment (caps);
  }

  out = gst_video_encoder_set_output_state (GST_VIDEO_ENCODER (enc),
      caps, enc->in_state);

  return out;
}

static void
gst_droidenc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDroidEnc *enc = GST_DROIDENC (object);

  switch (prop_id) {
    case PROP_TARGET_BITRATE:
      enc->target_bitrate = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_droidenc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstDroidEnc *enc = GST_DROIDENC (object);

  switch (prop_id) {
    case PROP_TARGET_BITRATE:
      g_value_set_int (value, enc->target_bitrate);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_droidenc_finalize (GObject * object)
{
  GstDroidEnc *enc = GST_DROIDENC (object);

  GST_DEBUG_OBJECT (enc, "finalize");

  enc->codec = NULL;

  g_mutex_clear (&enc->eos_lock);
  g_cond_clear (&enc->eos_cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_droidenc_open (GstVideoEncoder * encoder)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "open");

  /* nothing to do here */

  return TRUE;
}

static gboolean
gst_droidenc_close (GstVideoEncoder * encoder)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "close");

  /* nothing to do here */

  return TRUE;
}

static gboolean
gst_droidenc_start (GstVideoEncoder * encoder)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "start");

  enc->eos = FALSE;
  enc->downstream_flow_ret = GST_FLOW_OK;
  enc->dirty = TRUE;

  return TRUE;
}

static gboolean
gst_droidenc_stop (GstVideoEncoder * encoder)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "stop");

  if (enc->codec) {
    droid_media_codec_stop (enc->codec);
    droid_media_codec_destroy (enc->codec);
    enc->codec = NULL;
  }

  if (enc->in_state) {
    gst_video_codec_state_unref (enc->in_state);
    enc->in_state = NULL;
  }

  if (enc->out_state) {
    gst_video_codec_state_unref (enc->out_state);
    enc->out_state = NULL;
  }

  enc->codec_type = NULL;

  return TRUE;
}

static gboolean
gst_droidenc_set_format (GstVideoEncoder * encoder, GstVideoCodecState * state)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (enc, "set format %" GST_PTR_FORMAT, state->caps);

  if (enc->codec) {
    GST_FIXME_OBJECT (enc, "What to do here?");
    GST_ERROR_OBJECT (enc, "codec already renegotiate");
    goto error;
  }

  enc->first_frame_sent = FALSE;

  enc->in_state = gst_video_codec_state_ref (state);

  if (!gst_droidenc_negotiate_src_caps (enc)) {
    goto error;
  }

  if (!gst_droidenc_create_codec (enc)) {
    goto error;
  }

  return TRUE;

error:
  if (enc->in_state) {
    gst_video_codec_state_unref (enc->in_state);
    enc->in_state = NULL;
  }

  if (enc->out_state) {
    gst_video_codec_state_unref (enc->out_state);
    enc->out_state = NULL;
  }

  return ret;
}

static GstFlowReturn
gst_droidenc_finish (GstVideoEncoder * encoder)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "finish");

  g_mutex_lock (&enc->eos_lock);
  enc->eos = TRUE;

  if (enc->codec) {
    droid_media_codec_drain (enc->codec);
  }

  /* release the lock to allow _frame_available () to do its job */
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  /* Now we wait for the codec to signal EOS */
  g_cond_wait (&enc->eos_cond, &enc->eos_lock);
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

  enc->eos = FALSE;

  g_mutex_unlock (&enc->eos_lock);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_droidenc_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);
  GstFlowReturn ret = GST_FLOW_ERROR;

  GST_DEBUG_OBJECT (enc, "handle frame");

  if (!enc->codec) {
    GST_ERROR_OBJECT (enc, "component not initialized");
    goto error;
  }

  if (enc->downstream_flow_ret != GST_FLOW_OK) {
    GST_WARNING_OBJECT (enc, "not handling frame in error state: %s",
        gst_flow_get_name (enc->downstream_flow_ret));
    ret = enc->downstream_flow_ret;
    goto error;
  }

  g_mutex_lock (&enc->eos_lock);
  if (enc->eos) {
    GST_WARNING_OBJECT (enc, "got frame in eos state");
    g_mutex_unlock (&enc->eos_lock);
    ret = GST_FLOW_EOS;
    goto error;
  }
  g_mutex_unlock (&enc->eos_lock);

  /* This can deadlock if droidmedia/stagefright input buffer queue is full thus we
   * cannot write the input buffer. We end up waiting for the write operation
   * which does not happen because stagefright needs us to provide
   * output buffers to be filled (which can not happen because _loop() tries
   * to call get_oldest_frame() which acquires the stream lock the base class
   * is holding before calling us
   */
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  if (!gst_droid_codec_consume_frame (enc->codec, frame, frame->pts)) {
    ret = GST_FLOW_ERROR;
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
    goto error;
  }
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

  if (enc->downstream_flow_ret != GST_FLOW_OK) {
    GST_WARNING_OBJECT (enc, "not handling frame in error state: %s",
        gst_flow_get_name (enc->downstream_flow_ret));
    ret = enc->downstream_flow_ret;
    goto out;
  }

  g_mutex_lock (&enc->eos_lock);
  if (enc->eos) {
    GST_WARNING_OBJECT (enc, "got frame in eos state");
    g_mutex_unlock (&enc->eos_lock);
    ret = GST_FLOW_EOS;
    goto out;
  }
  g_mutex_unlock (&enc->eos_lock);

  ret = GST_FLOW_OK;

out:
  return ret;

error:
  /* don't leak the frame */
  gst_video_encoder_finish_frame (encoder, frame);

  return ret;
}

static gboolean
gst_droidenc_flush (GstVideoEncoder * encoder)
{
  GstDroidEnc *enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "flush");


  GST_DEBUG_OBJECT (enc, "Flushed");

  return TRUE;
}

static void
gst_droidenc_init (GstDroidEnc * enc)
{
  enc->codec = NULL;
  enc->codec_type = NULL;
  enc->in_state = NULL;
  enc->out_state = NULL;
  enc->target_bitrate = GST_DROID_ENC_TARGET_BITRATE_DEFAULT;
  enc->downstream_flow_ret = GST_FLOW_OK;
  g_mutex_init (&enc->eos_lock);
  g_cond_init (&enc->eos_cond);
}

static GstCaps *
gst_droidenc_getcaps (GstVideoEncoder * encoder, GstCaps * filter)
{
  GstDroidEnc *enc;
  GstCaps *caps;
  GstCaps *ret;

  enc = GST_DROIDENC (encoder);

  GST_DEBUG_OBJECT (enc, "getcaps with filter %" GST_PTR_FORMAT, filter);

#if 0
  /*
   * TODO: Seems _proxy_getcaps() is not working for us. It might be related to the feature we use
   * If it's the case then file a bug upstream, try to fix it and then enable this.
   */
  if (enc->out_state && enc->out_state->caps) {
    caps = gst_caps_copy (enc->out_state->caps);
  } else {
    caps = gst_pad_get_pad_template_caps (GST_VIDEO_ENCODER_SINK_PAD (encoder));
  }

  GST_DEBUG_OBJECT (enc, "our caps %" GST_PTR_FORMAT, caps);

  ret = gst_video_encoder_proxy_getcaps (encoder, caps, filter);

  GST_DEBUG_OBJECT (enc, "returning %" GST_PTR_FORMAT, ret);

  gst_caps_unref (caps);
#endif

  if (enc->out_state && enc->out_state->caps) {
    caps = gst_caps_copy (enc->out_state->caps);
  } else {
    caps = gst_pad_get_pad_template_caps (GST_VIDEO_ENCODER_SINK_PAD (encoder));
  }

  GST_DEBUG_OBJECT (enc, "our caps %" GST_PTR_FORMAT, caps);

  if (caps && filter) {
    ret = gst_caps_intersect_full (caps, filter, GST_CAPS_INTERSECT_FIRST);
  } else if (caps) {
    ret = gst_caps_ref (caps);
  } else {
    ret = NULL;
  }

  if (caps) {
    gst_caps_unref (caps);
  }

  GST_DEBUG_OBJECT (enc, "returning caps %" GST_PTR_FORMAT, ret);

  return ret;
}

static void
gst_droidenc_class_init (GstDroidEncClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstVideoEncoderClass *gstvideoencoder_class;
  GstCaps *caps;
  GstPadTemplate *tpl;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstvideoencoder_class = (GstVideoEncoderClass *) klass;

  gst_element_class_set_static_metadata (gstelement_class,
      "Video encoder", "Encoder/Video/Device",
      "Android HAL encoder", "Mohammed Sameer <msameer@foolab.org>");

  caps = gst_droid_codec_get_all_caps (GST_DROID_CODEC_ENCODER);

  tpl = gst_pad_template_new (GST_VIDEO_ENCODER_SRC_NAME,
      GST_PAD_SRC, GST_PAD_ALWAYS, caps);
  gst_element_class_add_pad_template (gstelement_class, tpl);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_droidenc_sink_template_factory));

  gobject_class->finalize = gst_droidenc_finalize;
  gobject_class->set_property = gst_droidenc_set_property;
  gobject_class->get_property = gst_droidenc_get_property;

  gstvideoencoder_class->open = GST_DEBUG_FUNCPTR (gst_droidenc_open);
  gstvideoencoder_class->close = GST_DEBUG_FUNCPTR (gst_droidenc_close);
  gstvideoencoder_class->start = GST_DEBUG_FUNCPTR (gst_droidenc_start);
  gstvideoencoder_class->stop = GST_DEBUG_FUNCPTR (gst_droidenc_stop);
  gstvideoencoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_droidenc_set_format);
  gstvideoencoder_class->getcaps = GST_DEBUG_FUNCPTR (gst_droidenc_getcaps);
  gstvideoencoder_class->finish = GST_DEBUG_FUNCPTR (gst_droidenc_finish);
  gstvideoencoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_droidenc_handle_frame);
  gstvideoencoder_class->flush = GST_DEBUG_FUNCPTR (gst_droidenc_flush);

  g_object_class_install_property (gobject_class, PROP_TARGET_BITRATE,
      g_param_spec_int ("target-bitrate", "Target Bitrate",
          "Target bitrate", 0, G_MAXINT,
          GST_DROID_ENC_TARGET_BITRATE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}
