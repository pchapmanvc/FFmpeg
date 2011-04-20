/*
 * LXF demuxer
 * Copyright (c) 2010 Tomas Härdin
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

#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "riff.h"

#define LXF_MAX_HEADER_SIZE     72
#define LXF_HEADER_DATA_SIZE    120
#define LXF_IDENT               "LEITCH\0"
#define LXF_IDENT_LENGTH        8
#define LXF_SAMPLERATE          48000
#define LXF_MAX_AUDIO_PACKET    (8008*15*4) ///< 15-channel 32-bit NTSC audio frame
#define LXF_AUDIO_PACKET_SEARCH 5 ///< Number of packets to pull at start, looking for audio
#define LXF_NEWFORMAT_TIMEBASE  720000 ///< Version 1 format uses 720kHz time units

static const AVCodecTag lxf_tags[] = {
    { CODEC_ID_MJPEG,       0 },
    { CODEC_ID_MPEG1VIDEO,  1 },
    { CODEC_ID_MPEG2VIDEO,  2 },    //MpMl, 4:2:0
    { CODEC_ID_MPEG2VIDEO,  3 },    //MpPl, 4:2:2
    { CODEC_ID_DVVIDEO,     4 },    //DV25
    { CODEC_ID_DVVIDEO,     5 },    //DVCPRO
    { CODEC_ID_DVVIDEO,     6 },    //DVCPRO50
    { CODEC_ID_RAWVIDEO,    7 },    //PIX_FMT_ARGB, where alpha is used for chroma keying
    { CODEC_ID_RAWVIDEO,    8 },    //16-bit chroma key
    { CODEC_ID_MPEG2VIDEO,  9 },    //4:2:2 CBP ("Constrained Bytes per Gop")
    { CODEC_ID_NONE,        0 },
};

typedef struct {
    int channels;                       ///< number of audio channels. zero means no audio
    uint8_t temp[LXF_MAX_AUDIO_PACKET]; ///< temp buffer for de-planarizing the audio data
    unsigned int timebase_num;          ///< tick unit numerator, used for av_set_pts_info
    unsigned int timebase_denom;        ///< tick unit denominator, used for av_set_pts_info
} LXFDemuxContext;

static int lxf_probe(AVProbeData *p)
{
    if (!memcmp(p->buf, LXF_IDENT, LXF_IDENT_LENGTH))
        return AVPROBE_SCORE_MAX;

    return 0;
}

/**
 * Verify the checksum of an LXF packet header
 *
 * @param[in] header the packet header to check
 * @return zero if the checksum is OK, non-zero otherwise
 */
static int check_checksum(const uint8_t *header)
{
    int x;
    uint32_t sum = 0;
    const int size = AV_RL32(&header[12]);

    for (x = 0; x < size; x += 4)
        sum += AV_RL32(&header[x]);

    return sum;
}

/**
 * Read input until we find the next ident. If found, copy it to the header buffer
 *
 * @param[out] header where to copy the ident to
 * @return 0 if an ident was found, < 0 on I/O error
 */
static int sync(AVFormatContext *s, uint8_t *header)
{
    uint8_t buf[LXF_IDENT_LENGTH];
    int ret;

    if ((ret = avio_read(s->pb, buf, LXF_IDENT_LENGTH)) != LXF_IDENT_LENGTH)
        return ret < 0 ? ret : AVERROR_EOF;

    while (memcmp(buf, LXF_IDENT, LXF_IDENT_LENGTH)) {
        if (url_feof(s->pb))
            return AVERROR_EOF;

        memmove(buf, &buf[1], LXF_IDENT_LENGTH-1);
        buf[LXF_IDENT_LENGTH-1] = avio_r8(s->pb);
    }

    memcpy(header, LXF_IDENT, LXF_IDENT_LENGTH);

    return 0;
}

/**
 * Read and checksum the next packet header
 *
 * @param[out] header the read packet header
 * @param[out] format context dependent format information
 * @return the size of the payload following the header or < 0 on failure
 */
static int get_packet_header(AVFormatContext *s, uint8_t *header, uint32_t *format)
{
    AVIOContext   *pb  = s->pb;
    LXFDemuxContext *lxf = s->priv_data;
    int track_size, samples, ret, header_size, version;
    AVStream *st;

    //find and read the ident
    if ((ret = sync(s, header)) < 0)
        return ret;

    //read version & header size
    if ((ret = get_buffer(pb, header + LXF_IDENT_LENGTH, 8)) != 8) {
        return ret < 0 ? ret : AVERROR_EOF;
    }
    header_size = AV_RL32(&header[12]);
    version =  AV_RL32(&header[8]);

    //read the rest of the packet header
    if ((ret = avio_read(pb, header + LXF_IDENT_LENGTH + 8,
                          header_size - LXF_IDENT_LENGTH - 8)) !=
                          header_size - LXF_IDENT_LENGTH - 8) {
        return ret < 0 ? ret : AVERROR_EOF;
    }

    if (check_checksum(header))
        av_log(s, AV_LOG_ERROR, "checksum error\n");

    *format = AV_RL32(&header[version ? 40 : 32]);
    ret     = AV_RL32(&header[version ? 44 : 36]);

    //type
    switch (AV_RL32(&header[16])) {
    case 0:
        //video
        //skip VBI data and metadata
        avio_skip(pb, (int64_t)(uint32_t)AV_RL32(&header[version ? 52 : 44]) +
                      (int64_t)(uint32_t)AV_RL32(&header[version ? 60 : 52]));
        break;
    case 1:
        //audio
        if (!(st = s->streams[1])) {
            av_log(s, AV_LOG_INFO, "got audio packet, but no audio stream present\n");
            break;
        }

        //set codec based on specified audio bitdepth
        //we only support tightly packed 16-, 20-, 24- and 32-bit PCM at the moment
        *format                          = AV_RL32(&header[40]);
        st->codec->bits_per_coded_sample = (*format >> 6) & 0x3F;

        if (st->codec->bits_per_coded_sample != (*format & 0x3F)) {
            av_log(s, AV_LOG_WARNING, "only tightly packed PCM currently supported\n");
            return AVERROR_PATCHWELCOME;
        }

        switch (st->codec->bits_per_coded_sample) {
        case 16: st->codec->codec_id = CODEC_ID_PCM_S16LE; break;
        case 20: st->codec->codec_id = CODEC_ID_PCM_LXF;   break;
        case 24: st->codec->codec_id = CODEC_ID_PCM_S24LE; break;
        case 32: st->codec->codec_id = CODEC_ID_PCM_S32LE; break;
        default:
            av_log(s, AV_LOG_WARNING,
                   "only 16-, 20-, 24- and 32-bit PCM currently supported\n");
            return AVERROR_PATCHWELCOME;
        }

        track_size = AV_RL32(&header[48]);
        samples = track_size * 8 / st->codec->bits_per_coded_sample;

        //use audio packet size to determine video frame rate
        //for NTSC we have one 8008-sample audio frame per five video frames
        if (lxf->timebase_num) {
            // already got timebase, don't bother again
        } else if (version) {
            // new LXF format; always 1/720000
            lxf->timebase_num = 1;
            lxf->timebase_denom = LXF_NEWFORMAT_TIMEBASE;
        } else if (samples == LXF_SAMPLERATE * 5005 / 30000) {
            lxf->timebase_num = 1001; //NTSC
            lxf->timebase_denom = 30000 * 2; // old time base is in fields
        } else {
            //assume PAL, but warn if we don't have 1920 samples
            if (samples != LXF_SAMPLERATE / 25)
                av_log(s, AV_LOG_WARNING,
                       "video doesn't seem to be PAL or NTSC. guessing PAL\n");

            lxf->timebase_num = 1;
            lxf->timebase_denom = 25 * 2; //  old time base is in fields
        }

        //TODO: warning if track mask != (1 << channels) - 1?
        ret = av_popcount(AV_RL32(&header[44])) * track_size;

        break;
    default:
        break;
    }

    return ret;
}

static int lxf_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    LXFDemuxContext *lxf = s->priv_data;
    AVIOContext   *pb  = s->pb;
    uint8_t header[LXF_MAX_HEADER_SIZE], header_data[LXF_HEADER_DATA_SIZE];
    int ret, packets_read = 0;
    unsigned int stream;
    AVStream *st;
    uint32_t format, video_params, disk_params, header_version;
    uint16_t record_date, expiration_date;
    int64_t header_end_pos;

    if ((ret = get_packet_header(s, header, &format)) < 0)
        return ret;
    header_version = AV_RL32(&header[8]);

    if (ret != LXF_HEADER_DATA_SIZE) {
        av_log(s, AV_LOG_ERROR, "expected %d B size header, got %d\n",
               LXF_HEADER_DATA_SIZE, ret);
        return AVERROR_INVALIDDATA;
    }

    if ((ret = avio_read(pb, header_data, LXF_HEADER_DATA_SIZE)) != LXF_HEADER_DATA_SIZE)
        return ret < 0 ? ret : AVERROR_EOF;

    if (!(st = av_new_stream(s, 0)))
        return AVERROR(ENOMEM);

    // duration in segment header is duration of entire clip
    st->duration          = header_version ? AV_RL64(&header[32]) : (int64_t)AV_RL32(&header[28]);

    video_params          = AV_RL32(&header_data[40]);
    record_date           = AV_RL16(&header_data[56]);
    expiration_date       = AV_RL16(&header_data[58]);
    disk_params           = AV_RL32(&header_data[116]);

    st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codec->bit_rate   = 1000000 * ((video_params >> 14) & 0xFF);
    st->codec->codec_id   = ff_codec_get_id(lxf_tags, video_params & 0xF);

    av_log(s, AV_LOG_DEBUG, "record: %x = %i-%02i-%02i\n",
           record_date, 1900 + (record_date & 0x7F), (record_date >> 7) & 0xF,
           (record_date >> 11) & 0x1F);

    av_log(s, AV_LOG_DEBUG, "expire: %x = %i-%02i-%02i\n",
           expiration_date, 1900 + (expiration_date & 0x7F), (expiration_date >> 7) & 0xF,
           (expiration_date >> 11) & 0x1F);

    if ((video_params >> 22) & 1)
        av_log(s, AV_LOG_WARNING, "VBI data not yet supported\n");

    if (format == 1) {
        //skip extended field data
        avio_skip(s->pb, (uint32_t)AV_RL32(&header[header_version ? 48 : 40]));
    }

    lxf->timebase_num = 0; // No time base found yet

    if ((lxf->channels = (disk_params >> 2) & 0xF)) {
        if (!(st = av_new_stream(s, 1)))
            return AVERROR(ENOMEM);

        st->codec->codec_type  = AVMEDIA_TYPE_AUDIO;
        st->codec->sample_rate = LXF_SAMPLERATE;
        st->codec->channels    = lxf->channels;

        header_end_pos = avio_tell(s->pb);

        // look for an audio packet; get_packet_header sets
        // lxf->timebase_{num,denom} on encountering an audio packet
        while (++packets_read < LXF_AUDIO_PACKET_SEARCH &&
               AV_RL32(&header[16]) != 1) {
            if ((ret = get_packet_header(s, header, &format)) < 0)
                break;
            avio_skip(s->pb, ret); // skip over packet content
        }

        // Set audio bit rate, if we found an audio packet
        if (AV_RL32(&header[16]) == 1)
            st->codec->bit_rate = lxf->channels * LXF_SAMPLERATE * (format & 0x3F);

        avio_seek(s->pb, header_end_pos, SEEK_SET); // Rewind back
    }

    // Fallback in case we didn't find an audio packet.
    if (!lxf->timebase_num) {
        lxf->timebase_num = 1;
        if (header_version) {
            // No audio packet necessary to determine time base: new LXF format
            // time base is fixed
            lxf->timebase_denom = LXF_NEWFORMAT_TIMEBASE;
        } else {
            av_log(s, AV_LOG_WARNING,
                   "can't find audio packet to determine video frame rate. guessing PAL\n");
            lxf->timebase_denom = 50; // Arbitrary PAL field rate
        }
    }

    // Set the same timebase on all streams.
    for (stream = 0; stream < s->nb_streams; ++stream)
        av_set_pts_info(s->streams[stream], 64, lxf->timebase_num, lxf->timebase_denom);

    return 0;
}

/**
 * De-planerize the PCM data in lxf->temp
 * FIXME: remove this once support for planar audio is added to libavcodec
 *
 * @param[out] out where to write the de-planerized data to
 * @param[in] bytes the total size of the PCM data
 */
static void deplanarize(LXFDemuxContext *lxf, AVStream *ast, uint8_t *out, int bytes)
{
    int x, y, z, i, bytes_per_sample = ast->codec->bits_per_coded_sample >> 3;

    for (z = i = 0; z < lxf->channels; z++)
        for (y = 0; y < bytes / bytes_per_sample / lxf->channels; y++)
            for (x = 0; x < bytes_per_sample; x++, i++)
                out[x + bytes_per_sample*(z + y*lxf->channels)] = lxf->temp[i];
}

static int lxf_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    LXFDemuxContext *lxf = s->priv_data;
    AVIOContext   *pb  = s->pb;
    uint8_t header[LXF_MAX_HEADER_SIZE], *buf;
    AVStream *ast = NULL;
    uint32_t stream, format;
    int ret, ret2, version;
    int64_t pos;

    if ((ret = get_packet_header(s, header, &format)) < 0)
        return ret;
    version = AV_RL32(&header[8]);

    pos = avio_tell(pb) - AV_RL32(&header[12]);
    stream = AV_RL32(&header[16]);

    if (stream > 1) {
        av_log(s, AV_LOG_WARNING, "got packet with illegal stream index %u\n", stream);
        return AVERROR(EAGAIN);
    }

    if (stream == 1 && !(ast = s->streams[1])) {
        av_log(s, AV_LOG_ERROR, "got audio packet without having an audio stream\n");
        return AVERROR_INVALIDDATA;
    }

    //make sure the data fits in the de-planerization buffer
    if (ast && ret > LXF_MAX_AUDIO_PACKET) {
        av_log(s, AV_LOG_ERROR, "audio packet too large (%i > %i)\n",
            ret, LXF_MAX_AUDIO_PACKET);
        return AVERROR_INVALIDDATA;
    }

    if ((ret2 = av_new_packet(pkt, ret)) < 0)
        return ret2;

    //read non-20-bit audio data into lxf->temp so we can deplanarize it
    buf = ast && ast->codec->codec_id != CODEC_ID_PCM_LXF ? lxf->temp : pkt->data;

    if ((ret2 = avio_read(pb, buf, ret)) != ret) {
        av_free_packet(pkt);
        return ret2 < 0 ? ret2 : AVERROR_EOF;
    }

    pkt->pos = pos;
    pkt->stream_index = stream;

    if (ast) {
        if(ast->codec->codec_id != CODEC_ID_PCM_LXF)
            deplanarize(lxf, ast, pkt->data, ret);
    } else {
        //picture type (0 = closed I, 1 = open I, 2 = P, 3 = B)
        if (((format >> 22) & 0x3) < 2)
            pkt->flags |= AV_PKT_FLAG_KEY;

    }

    pkt->dts      = version ? AV_RL64(&header[24]) : (int64_t)AV_RL32(&header[24]);
    pkt->duration = version ? AV_RL64(&header[32]) : (int64_t)AV_RL32(&header[28]);

    return ret;
}

AVInputFormat ff_lxf_demuxer = {
    .name           = "lxf",
    .long_name      = NULL_IF_CONFIG_SMALL("VR native stream format (LXF)"),
    .priv_data_size = sizeof(LXFDemuxContext),
    .read_probe     = lxf_probe,
    .read_header    = lxf_read_header,
    .read_packet    = lxf_read_packet,
    .flags          = AVFMT_GENERIC_INDEX,
};

