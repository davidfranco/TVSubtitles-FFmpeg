/*
 * generic encoding-related code
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/samplefmt.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "frame_thread_encoder.h"
#include "internal.h"

int ff_alloc_packet(AVCodecContext *avctx, AVPacket *avpkt, int64_t size)
{
    if (size < 0 || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "Invalid minimum required packet size %"PRId64" (max allowed is %d)\n",
               size, INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE);
        return AVERROR(EINVAL);
    }

    av_assert0(!avpkt->data);

    av_fast_padded_malloc(&avctx->internal->byte_buffer,
                          &avctx->internal->byte_buffer_size, size);
    avpkt->data = avctx->internal->byte_buffer;
    if (!avpkt->data) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet of size %"PRId64"\n", size);
        return AVERROR(ENOMEM);
    }
    avpkt->size = size;

    return 0;
}

int avcodec_default_get_encode_buffer(AVCodecContext *avctx, AVPacket *avpkt, int flags)
{
    int ret;

    if (avpkt->size < 0 || avpkt->size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);

    if (avpkt->data || avpkt->buf) {
        av_log(avctx, AV_LOG_ERROR, "avpkt->{data,buf} != NULL in avcodec_default_get_encode_buffer()\n");
        return AVERROR(EINVAL);
    }

    ret = av_buffer_realloc(&avpkt->buf, avpkt->size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate packet of size %d\n", avpkt->size);
        return ret;
    }
    avpkt->data = avpkt->buf->data;

    return 0;
}

int ff_get_encode_buffer(AVCodecContext *avctx, AVPacket *avpkt, int64_t size, int flags)
{
    int ret;

    if (size < 0 || size > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(EINVAL);

    av_assert0(!avpkt->data && !avpkt->buf);

    avpkt->size = size;
    ret = avctx->get_encode_buffer(avctx, avpkt, flags);
    if (ret < 0)
        goto fail;

    if (!avpkt->data || !avpkt->buf) {
        av_log(avctx, AV_LOG_ERROR, "No buffer returned by get_encode_buffer()\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }
    memset(avpkt->data + avpkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    ret = 0;
fail:
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_encode_buffer() failed\n");
        av_packet_unref(avpkt);
    }

    return ret;
}

static int encode_make_refcounted(AVCodecContext *avctx, AVPacket *avpkt)
{
    uint8_t *data = avpkt->data;
    int ret;

    if (avpkt->buf)
        return 0;

    avpkt->data = NULL;
    ret = ff_get_encode_buffer(avctx, avpkt, avpkt->size, 0);
    if (ret < 0)
        return ret;
    memcpy(avpkt->data, data, avpkt->size);

    return 0;
}

/**
 * Pad last frame with silence.
 */
static int pad_last_frame(AVCodecContext *s, AVFrame *frame, const AVFrame *src, int out_samples)
{
    int ret;

    frame->format         = src->format;
    frame->nb_samples     = out_samples;
    ret = av_channel_layout_copy(&frame->ch_layout, &s->ch_layout);
    if (ret < 0)
        goto fail;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        goto fail;

    ret = av_frame_copy_props(frame, src);
    if (ret < 0)
        goto fail;

    if ((ret = av_samples_copy(frame->extended_data, src->extended_data, 0, 0,
                               src->nb_samples, s->ch_layout.nb_channels,
                               s->sample_fmt)) < 0)
        goto fail;
    if ((ret = av_samples_set_silence(frame->extended_data, src->nb_samples,
                                      frame->nb_samples - src->nb_samples,
                                      s->ch_layout.nb_channels, s->sample_fmt)) < 0)
        goto fail;

    return 0;

fail:
    av_frame_unref(frame);
    s->internal->last_audio_frame = 0;
    return ret;
}

int avcodec_encode_subtitle(AVCodecContext *avctx, uint8_t *buf, int buf_size, const AVSubtitle *sub)
{
    int ret = 0;
    AVFrame *frame = NULL;
    AVPacket* avpkt = NULL;

    if (sub->start_display_time) {
        av_log(avctx, AV_LOG_ERROR, "start_display_time must be 0.\n");
        return -1;
    }

    memset(buf, 0, buf_size);
    // Create a temporary frame for calling the regular api:
    frame = av_frame_alloc();
    if (!frame) {
        ret = AVERROR(ENOMEM);
        goto exit;
    }

    frame->format = sub->format;
    frame->type = AVMEDIA_TYPE_SUBTITLE;
    ret = av_frame_get_buffer2(frame, 0);
    if (ret < 0)
        goto exit;

    // Create a temporary packet
    avpkt = av_packet_alloc();
    if (!avpkt) {
        ret = AVERROR(ENOMEM);
        goto exit;
    }

    // Copy legacy subtitle data to temp frame
    ret = ff_frame_put_subtitle(frame, sub);
    if (ret < 0)
        goto exit;

    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0)
        goto exit;

    ret = avcodec_receive_packet(avctx, avpkt);

    if (ret < 0 && ret != AVERROR(EAGAIN))
        goto exit;

    //ret = avctx->codec->encode2(avctx, avpkt, frame, &got_packet);

    avctx->frame_number++;

    if (avpkt->size) {
        if (avpkt->size > buf_size) {
            ret = AVERROR_BUFFER_TOO_SMALL;
            goto exit;
        }

        memcpy(buf, avpkt->data, avpkt->size);
        ret = avpkt->size;
    }

exit:

    av_packet_free(&avpkt);
    av_frame_free(&frame);
    return ret;
}

int ff_encode_get_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;

    if (avci->draining)
        return AVERROR_EOF;

    if (!avci->buffer_frame->buf[0])
        return AVERROR(EAGAIN);

    av_frame_move_ref(frame, avci->buffer_frame);

    return 0;
}

int ff_encode_encode_cb(AVCodecContext *avctx, AVPacket *avpkt,
                        AVFrame *frame, int *got_packet)
{
    const FFCodec *const codec = ffcodec(avctx->codec);
    int ret;

    ret = codec->cb.encode(avctx, avpkt, frame, got_packet);
    emms_c();
    av_assert0(ret <= 0);

    if (!ret && *got_packet) {
        if (avpkt->data) {
            ret = encode_make_refcounted(avctx, avpkt);
            if (ret < 0)
                goto unref;
            // Date returned by encoders must always be ref-counted
            av_assert0(avpkt->buf);
        }

        // set the timestamps for the simple no-delay case
        // encoders with delay have to set the timestamps themselves
        if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY)) {
            if (avpkt->pts == AV_NOPTS_VALUE)
                avpkt->pts = frame->pts;

            if (avctx->codec->type == AVMEDIA_TYPE_AUDIO) {
                if (!avpkt->duration)
                    avpkt->duration = ff_samples_to_time_base(avctx,
                                                              frame->nb_samples);
            }
        }

        // dts equals pts unless there is reordering
        // there can be no reordering if there is no encoder delay
        if (!(avctx->codec_descriptor->props & AV_CODEC_PROP_REORDER) ||
            !(avctx->codec->capabilities & AV_CODEC_CAP_DELAY))
            avpkt->dts = avpkt->pts;
    } else {
unref:
        av_packet_unref(avpkt);
    }
#if !FF_API_THREAD_SAFE_CALLBACKS
    if (frame)
        av_frame_unref(frame);
#endif

    return ret;
}

static int encode_simple_internal(AVCodecContext *avctx, AVPacket *avpkt)
{
    AVCodecInternal   *avci = avctx->internal;
    AVFrame          *frame = avci->in_frame;
    const FFCodec *const codec = ffcodec(avctx->codec);
    int got_packet;
    int ret;

    if (avci->draining_done)
        return AVERROR_EOF;

    if (!frame->buf[0] && !avci->draining) {
        av_frame_unref(frame);
        ret = ff_encode_get_frame(avctx, frame);
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    if (!frame->buf[0]) {
        if (!(avctx->codec->capabilities & AV_CODEC_CAP_DELAY ||
              avci->frame_thread_encoder))
            return AVERROR_EOF;

        // Flushing is signaled with a NULL frame
        frame = NULL;
    }

    got_packet = 0;

    av_assert0(codec->cb_type == FF_CODEC_CB_TYPE_ENCODE);

    if (CONFIG_FRAME_THREAD_ENCODER && avci->frame_thread_encoder)
        /* This will unref frame. */
        ret = ff_thread_video_encode_frame(avctx, avpkt, frame, &got_packet);
    else {
        ret = ff_encode_encode_cb(avctx, avpkt, frame, &got_packet);
#if FF_API_THREAD_SAFE_CALLBACKS
        if (frame)
            av_frame_unref(frame);
#endif
    }

    if (avci->draining && !got_packet)
        avci->draining_done = 1;

    return ret;
}

static int encode_simple_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    int ret;

    while (!avpkt->data && !avpkt->side_data) {
        ret = encode_simple_internal(avctx, avpkt);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int encode_receive_packet_internal(AVCodecContext *avctx, AVPacket *avpkt)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    if (avci->draining_done)
        return AVERROR_EOF;

    av_assert0(!avpkt->data && !avpkt->side_data);

    if (avctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        if ((avctx->flags & AV_CODEC_FLAG_PASS1) && avctx->stats_out)
            avctx->stats_out[0] = '\0';
        if (av_image_check_size2(avctx->width, avctx->height, avctx->max_pixels, AV_PIX_FMT_NONE, 0, avctx))
            return AVERROR(EINVAL);
    }

    if (ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_RECEIVE_PACKET) {
        ret = ffcodec(avctx->codec)->cb.receive_packet(avctx, avpkt);
        if (ret < 0)
            av_packet_unref(avpkt);
        else
            // Encoders must always return ref-counted buffers.
            // Side-data only packets have no data and can be not ref-counted.
            av_assert0(!avpkt->data || avpkt->buf);
    } else
        ret = encode_simple_receive_packet(avctx, avpkt);
    if (ret >= 0)
        avpkt->flags |= avci->intra_only_flag;

    if (ret == AVERROR_EOF)
        avci->draining_done = 1;

    return ret;
}

#if CONFIG_LCMS2
static int encode_generate_icc_profile(AVCodecContext *avctx, AVFrame *frame)
{
    enum AVColorTransferCharacteristic trc = frame->color_trc;
    enum AVColorPrimaries prim = frame->color_primaries;
    const FFCodec *const codec = ffcodec(avctx->codec);
    AVCodecInternal *avci = avctx->internal;
    cmsHPROFILE profile;
    int ret;

    /* don't generate ICC profiles if disabled or unsupported */
    if (!(avctx->flags2 & AV_CODEC_FLAG2_ICC_PROFILES))
        return 0;
    if (!(codec->caps_internal & FF_CODEC_CAP_ICC_PROFILES))
        return 0;

    if (trc == AVCOL_TRC_UNSPECIFIED)
        trc = avctx->color_trc;
    if (prim == AVCOL_PRI_UNSPECIFIED)
        prim = avctx->color_primaries;
    if (trc == AVCOL_TRC_UNSPECIFIED || prim == AVCOL_PRI_UNSPECIFIED)
        return 0; /* can't generate ICC profile with missing csp tags */

    if (av_frame_get_side_data(frame, AV_FRAME_DATA_ICC_PROFILE))
        return 0; /* don't overwrite existing ICC profile */

    if (!avci->icc.avctx) {
        ret = ff_icc_context_init(&avci->icc, avctx);
        if (ret < 0)
            return ret;
    }

    ret = ff_icc_profile_generate(&avci->icc, prim, trc, &profile);
    if (ret < 0)
        return ret;

    ret = ff_icc_profile_attach(&avci->icc, profile, frame);
    cmsCloseProfile(profile);
    return ret;
}
#else /* !CONFIG_LCMS2 */
static int encode_generate_icc_profile(av_unused AVCodecContext *c, av_unused AVFrame *f)
{
    return 0;
}
#endif

static int encode_send_frame_internal(AVCodecContext *avctx, const AVFrame *src)
{
    AVCodecInternal *avci = avctx->internal;
    AVFrame *dst = avci->buffer_frame;
    int ret;

    if (avctx->codec->type == AVMEDIA_TYPE_AUDIO) {
        /* extract audio service type metadata */
        AVFrameSideData *sd = av_frame_get_side_data(src, AV_FRAME_DATA_AUDIO_SERVICE_TYPE);
        if (sd && sd->size >= sizeof(enum AVAudioServiceType))
            avctx->audio_service_type = *(enum AVAudioServiceType*)sd->data;

        /* check for valid frame size */
        if (!(avctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {
            /* if we already got an undersized frame, that must have been the last */
            if (avctx->internal->last_audio_frame) {
                av_log(avctx, AV_LOG_ERROR, "frame_size (%d) was not respected for a non-last frame\n", avctx->frame_size);
                return AVERROR(EINVAL);
            }
            if (src->nb_samples > avctx->frame_size) {
                av_log(avctx, AV_LOG_ERROR, "nb_samples (%d) > frame_size (%d)\n", src->nb_samples, avctx->frame_size);
                return AVERROR(EINVAL);
            }
            if (src->nb_samples < avctx->frame_size) {
                avctx->internal->last_audio_frame = 1;
                if (!(avctx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)) {
                    int pad_samples = avci->pad_samples ? avci->pad_samples : avctx->frame_size;
                    int out_samples = (src->nb_samples + pad_samples - 1) / pad_samples * pad_samples;

                    if (out_samples != src->nb_samples) {
                        ret = pad_last_frame(avctx, dst, src, out_samples);
                        if (ret < 0)
                            return ret;
                        goto finish;
                    }
                }
            }
        }
    }

    ret = av_frame_ref(dst, src);
    if (ret < 0)
        return ret;

finish:

#if FF_API_PKT_DURATION
FF_DISABLE_DEPRECATION_WARNINGS
    if (dst->pkt_duration && dst->pkt_duration != dst->duration)
        dst->duration = dst->pkt_duration;
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    if (avctx->codec->type == AVMEDIA_TYPE_VIDEO) {
        ret = encode_generate_icc_profile(avctx, dst);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int attribute_align_arg avcodec_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    if (!avcodec_is_open(avctx) || !av_codec_is_encoder(avctx->codec))
        return AVERROR(EINVAL);

    if (avci->draining)
        return AVERROR_EOF;

    if (avci->buffer_frame->buf[0])
        return AVERROR(EAGAIN);

    if (!frame) {
        avci->draining = 1;
    } else {
        ret = encode_send_frame_internal(avctx, frame);
        if (ret < 0)
            return ret;
    }

    if (!avci->buffer_pkt->data && !avci->buffer_pkt->side_data) {
        ret = encode_receive_packet_internal(avctx, avci->buffer_pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return ret;
    }

    avctx->frame_number++;

    return 0;
}

int attribute_align_arg avcodec_receive_packet(AVCodecContext *avctx, AVPacket *avpkt)
{
    AVCodecInternal *avci = avctx->internal;
    int ret;

    av_packet_unref(avpkt);

    if (!avcodec_is_open(avctx) || !av_codec_is_encoder(avctx->codec))
        return AVERROR(EINVAL);

    if (avci->buffer_pkt->data || avci->buffer_pkt->side_data) {
        av_packet_move_ref(avpkt, avci->buffer_pkt);
    } else {
        ret = encode_receive_packet_internal(avctx, avpkt);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int encode_preinit_video(AVCodecContext *avctx)
{
    const AVPixFmtDescriptor *pixdesc = av_pix_fmt_desc_get(avctx->pix_fmt);
    int i;

    if (avctx->codec->pix_fmts) {
        for (i = 0; avctx->codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++)
            if (avctx->pix_fmt == avctx->codec->pix_fmts[i])
                break;
        if (avctx->codec->pix_fmts[i] == AV_PIX_FMT_NONE) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d", avctx->pix_fmt);
            av_log(avctx, AV_LOG_ERROR, "Specified pixel format %s is invalid or not supported\n",
                   (char *)av_x_if_null(av_get_pix_fmt_name(avctx->pix_fmt), buf));
            return AVERROR(EINVAL);
        }
        if (avctx->codec->pix_fmts[i] == AV_PIX_FMT_YUVJ420P ||
            avctx->codec->pix_fmts[i] == AV_PIX_FMT_YUVJ411P ||
            avctx->codec->pix_fmts[i] == AV_PIX_FMT_YUVJ422P ||
            avctx->codec->pix_fmts[i] == AV_PIX_FMT_YUVJ440P ||
            avctx->codec->pix_fmts[i] == AV_PIX_FMT_YUVJ444P)
            avctx->color_range = AVCOL_RANGE_JPEG;
    }

    if (    avctx->bits_per_raw_sample < 0
        || (avctx->bits_per_raw_sample > 8 && pixdesc->comp[0].depth <= 8)) {
        av_log(avctx, AV_LOG_WARNING, "Specified bit depth %d not possible with the specified pixel formats depth %d\n",
            avctx->bits_per_raw_sample, pixdesc->comp[0].depth);
        avctx->bits_per_raw_sample = pixdesc->comp[0].depth;
    }
    if (avctx->width <= 0 || avctx->height <= 0) {
        av_log(avctx, AV_LOG_ERROR, "dimensions not set\n");
        return AVERROR(EINVAL);
    }

    if (avctx->ticks_per_frame && avctx->time_base.num &&
        avctx->ticks_per_frame > INT_MAX / avctx->time_base.num) {
        av_log(avctx, AV_LOG_ERROR,
               "ticks_per_frame %d too large for the timebase %d/%d.",
               avctx->ticks_per_frame,
               avctx->time_base.num,
               avctx->time_base.den);
        return AVERROR(EINVAL);
    }

    if (avctx->hw_frames_ctx) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
        if (frames_ctx->format != avctx->pix_fmt) {
            av_log(avctx, AV_LOG_ERROR,
                   "Mismatching AVCodecContext.pix_fmt and AVHWFramesContext.format\n");
            return AVERROR(EINVAL);
        }
        if (avctx->sw_pix_fmt != AV_PIX_FMT_NONE &&
            avctx->sw_pix_fmt != frames_ctx->sw_format) {
            av_log(avctx, AV_LOG_ERROR,
                   "Mismatching AVCodecContext.sw_pix_fmt (%s) "
                   "and AVHWFramesContext.sw_format (%s)\n",
                   av_get_pix_fmt_name(avctx->sw_pix_fmt),
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            return AVERROR(EINVAL);
        }
        avctx->sw_pix_fmt = frames_ctx->sw_format;
    }

    return 0;
}

static int encode_preinit_audio(AVCodecContext *avctx)
{
    int i;

    if (avctx->codec->sample_fmts) {
        for (i = 0; avctx->codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++) {
            if (avctx->sample_fmt == avctx->codec->sample_fmts[i])
                break;
            if (avctx->ch_layout.nb_channels == 1 &&
                av_get_planar_sample_fmt(avctx->sample_fmt) ==
                av_get_planar_sample_fmt(avctx->codec->sample_fmts[i])) {
                avctx->sample_fmt = avctx->codec->sample_fmts[i];
                break;
            }
        }
        if (avctx->codec->sample_fmts[i] == AV_SAMPLE_FMT_NONE) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d", avctx->sample_fmt);
            av_log(avctx, AV_LOG_ERROR, "Specified sample format %s is invalid or not supported\n",
                   (char *)av_x_if_null(av_get_sample_fmt_name(avctx->sample_fmt), buf));
            return AVERROR(EINVAL);
        }
    }
    if (avctx->codec->supported_samplerates) {
        for (i = 0; avctx->codec->supported_samplerates[i] != 0; i++)
            if (avctx->sample_rate == avctx->codec->supported_samplerates[i])
                break;
        if (avctx->codec->supported_samplerates[i] == 0) {
            av_log(avctx, AV_LOG_ERROR, "Specified sample rate %d is not supported\n",
                   avctx->sample_rate);
            return AVERROR(EINVAL);
        }
    }
    if (avctx->sample_rate < 0) {
        av_log(avctx, AV_LOG_ERROR, "Specified sample rate %d is not supported\n",
                avctx->sample_rate);
        return AVERROR(EINVAL);
    }
    if (avctx->codec->ch_layouts) {
        for (i = 0; avctx->codec->ch_layouts[i].nb_channels; i++) {
            if (!av_channel_layout_compare(&avctx->ch_layout, &avctx->codec->ch_layouts[i]))
                break;
        }
        if (!avctx->codec->ch_layouts[i].nb_channels) {
            char buf[512];
            int ret = av_channel_layout_describe(&avctx->ch_layout, buf, sizeof(buf));
            if (ret > 0)
                av_log(avctx, AV_LOG_ERROR, "Specified channel layout '%s' is not supported\n", buf);
            return AVERROR(EINVAL);
        }
    }

    if (!avctx->bits_per_raw_sample)
        avctx->bits_per_raw_sample = 8 * av_get_bytes_per_sample(avctx->sample_fmt);

    return 0;
}

int ff_encode_preinit(AVCodecContext *avctx)
{
    AVCodecInternal *avci = avctx->internal;
    int ret = 0;

    if (avctx->time_base.num <= 0 || avctx->time_base.den <= 0) {
        av_log(avctx, AV_LOG_ERROR, "The encoder timebase is not set.\n");
        return AVERROR(EINVAL);
    }

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO: ret = encode_preinit_video(avctx); break;
    case AVMEDIA_TYPE_AUDIO: ret = encode_preinit_audio(avctx); break;
    }
    if (ret < 0)
        return ret;

    if (   (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        && avctx->bit_rate>0 && avctx->bit_rate<1000) {
        av_log(avctx, AV_LOG_WARNING, "Bitrate %"PRId64" is extremely low, maybe you mean %"PRId64"k\n", avctx->bit_rate, avctx->bit_rate);
    }

    if (!avctx->rc_initial_buffer_occupancy)
        avctx->rc_initial_buffer_occupancy = avctx->rc_buffer_size * 3LL / 4;

    if (avctx->codec_descriptor->props & AV_CODEC_PROP_INTRA_ONLY)
        avctx->internal->intra_only_flag = AV_PKT_FLAG_KEY;

    if (ffcodec(avctx->codec)->cb_type == FF_CODEC_CB_TYPE_ENCODE) {
        avci->in_frame = av_frame_alloc();
        if (!avci->in_frame)
            return AVERROR(ENOMEM);
    }

    if ((avctx->flags & AV_CODEC_FLAG_RECON_FRAME)) {
        if (!(avctx->codec->capabilities & AV_CODEC_CAP_ENCODER_RECON_FRAME)) {
            av_log(avctx, AV_LOG_ERROR, "Reconstructed frame output requested "
                   "from an encoder not supporting it\n");
            return AVERROR(ENOSYS);
        }

        avci->recon_frame = av_frame_alloc();
        if (!avci->recon_frame)
            return AVERROR(ENOMEM);
    }

    if (CONFIG_FRAME_THREAD_ENCODER) {
        ret = ff_frame_thread_encoder_init(avctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

int ff_encode_alloc_frame(AVCodecContext *avctx, AVFrame *frame)
{
    int ret;

    switch (avctx->codec->type) {
    case AVMEDIA_TYPE_VIDEO:
        frame->format = avctx->pix_fmt;
        if (frame->width <= 0 || frame->height <= 0) {
            frame->width  = FFMAX(avctx->width,  avctx->coded_width);
            frame->height = FFMAX(avctx->height, avctx->coded_height);
        }

        break;
    case AVMEDIA_TYPE_AUDIO:
        frame->sample_rate = avctx->sample_rate;
        frame->format      = avctx->sample_fmt;
        if (!frame->ch_layout.nb_channels) {
            ret = av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout);
            if (ret < 0)
                return ret;
        }
        break;
    }

    ret = avcodec_default_get_buffer2(avctx, frame, 0);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        av_frame_unref(frame);
        return ret;
    }

    return 0;
}

int ff_encode_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    AVCodecInternal *avci = avctx->internal;

    if (!avci->recon_frame)
        return AVERROR(EINVAL);
    if (!avci->recon_frame->buf[0])
        return avci->draining_done ? AVERROR_EOF : AVERROR(EAGAIN);

    av_frame_move_ref(frame, avci->recon_frame);
    return 0;
}
