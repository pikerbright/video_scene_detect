extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
};

#include <iostream>

#undef av_err2str
std::string av_err2str(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return std::string(errbuf);
}

static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;
typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
} FilteringContext;
static FilteringContext *filter_ctx;

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
} StreamContext;
static StreamContext *stream_ctx;

static AVCodecContext *video_dec_ctx = NULL;
static int video_stream_idx = -1;
static int scene_count = 0;
static int scene_frame_count = 0;
char output_file_name[128];
int sc_threshold = 100;
bool cut_video = true;

static int open_input_file(const char *filename)
{
    int ret;
    unsigned int i;
    AVDictionary *opts = NULL;

    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);
    //av_dict_set(&opts, "stimeout", "200000", 0);

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, &opts)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = (StreamContext *)av_mallocz_array(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream = ifmt_ctx->streams[i];
        AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
            //return AVERROR_DECODER_NOT_FOUND;
            continue;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                   "for stream #%u\n", i);
            return ret;
        }
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
//                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, &opts);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
        stream_ctx[i].dec_ctx = codec_ctx;
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO), filename);
        return ret;
    } else {
        video_stream_idx = ret;
    }

    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    if (!out_stream) {
        av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
        return AVERROR_UNKNOWN;
    }

    in_stream = ifmt_ctx->streams[video_stream_idx];
    dec_ctx = stream_ctx[video_stream_idx].dec_ctx;

    //if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
    //        || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        /* in this example, we choose transcoding to same codec */
        encoder = avcodec_find_encoder(dec_ctx->codec_id);
        if (!encoder) {
            av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
            return AVERROR_INVALIDDATA;
        }
        enc_ctx = avcodec_alloc_context3(encoder);
        if (!enc_ctx) {
            av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
            return AVERROR(ENOMEM);
        }

        /* In this example, we transcode to same properties (picture size,
         * sample rate etc.). These properties can be changed for output
         * streams easily using filters */
        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            enc_ctx->height = dec_ctx->height;
            enc_ctx->width = dec_ctx->width;
            enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
            /* take first format from list of supported formats */
            if (encoder->pix_fmts)
                enc_ctx->pix_fmt = encoder->pix_fmts[0];
            else
                enc_ctx->pix_fmt = dec_ctx->pix_fmt;
            /* video time_base can be set to whatever is handy and supported by encoder */
            enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
        } else {
            enc_ctx->sample_rate = dec_ctx->sample_rate;
            enc_ctx->channel_layout = dec_ctx->channel_layout;
            enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
            /* take first format from list of supported formats */
            enc_ctx->sample_fmt = encoder->sample_fmts[0];
            enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
        }

        //quality control, may effect the scene detect

        enc_ctx->me_range = 0;
        enc_ctx->max_qdiff = 10;
        enc_ctx->qmin = 10;
        enc_ctx->qmax = 18;
        enc_ctx->qcompress = 1.0;

        //scene detect parameter
        enc_ctx->gop_size = 2000;
        enc_ctx->profile = FF_PROFILE_H264_BASELINE;
        enc_ctx->scenechange_threshold = sc_threshold;
        av_opt_set(enc_ctx->priv_data, "tune","zerolatency",0);

        /* Third parameter can be used to pass settings to encoder */
        ret = avcodec_open2(enc_ctx, encoder, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
            return ret;
        }
        ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
            return ret;
        }
        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        out_stream->time_base = enc_ctx->time_base;
        stream_ctx[video_stream_idx].enc_ctx = enc_ctx;
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
        av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
        return AVERROR_INVALIDDATA;
    }

    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}


static int encode_write_frame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {
    int ret;
    int got_frame_local;
    AVPacket enc_pkt = {};
    int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
        (ifmt_ctx->streams[stream_index]->codecpar->codec_type ==
         AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;

    if (!got_frame)
        got_frame = &got_frame_local;

encode:
    if (filt_frame != NULL)
        filt_frame->pts = scene_frame_count;
    av_log(NULL, AV_LOG_INFO, "Encoding frame\n");
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    ret = enc_func(stream_ctx[stream_index].enc_ctx, &enc_pkt,
            filt_frame, got_frame);

    //av_frame_free(&filt_frame);
    if (ret < 0)
        return ret;
    if (!(*got_frame))
        return 0;

    auto type = (int)enc_pkt.data[5];
    //new sequence or I frame
    if (cut_video && scene_frame_count > 10 && ( type == 0x42 || type == 0x88)) {
        av_write_trailer(ofmt_ctx);

        for (int i = 0; i < ofmt_ctx->nb_streams; i++) {
            if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
                avcodec_free_context(&stream_ctx[i].enc_ctx);
        }

        if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);

        scene_count++;

        char tmp_name[128];
        sprintf(tmp_name, "%d_%s", scene_count, output_file_name);
        open_output_file(tmp_name);
        av_packet_unref(&enc_pkt);

        scene_frame_count = 0;
        goto encode;
    }

    printf("scene: %d, frame: %d\n", scene_count, scene_frame_count);

    scene_frame_count++;
    enc_pkt.duration = 1;
    enc_pkt.pts = enc_pkt.dts;
    enc_pkt.stream_index = 0;
    av_packet_rescale_ts(&enc_pkt,
                         stream_ctx[stream_index].enc_ctx->time_base,
                         ofmt_ctx->streams[0]->time_base);

    av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
    /* mux encoded frame */
    ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
    return ret;
}


static int flush_encoder(unsigned int stream_index)
{
    int ret;
    int got_frame;

    if (!(stream_ctx[stream_index].enc_ctx->codec->capabilities &
                AV_CODEC_CAP_DELAY))
        return 0;

    while (1) {
        av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
        ret = encode_write_frame(NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}


int main(int argc, char **argv)
{
    int ret;
    AVPacket packet = {};
    AVFrame *frame = NULL;
    enum AVMediaType type;
    unsigned int stream_index;
    unsigned int i;
    int got_frame;
    int64_t pts = 0;

    av_log_set_level(AV_LOG_WARNING);

    int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);

    if (argc != 3 && argc != 4) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file> [sc_threshold]\n", argv[0]);
        return 1;
    }
    if (argc == 4)
        sc_threshold = atoi(argv[3]);

    av_register_all();
    avfilter_register_all();
    avformat_network_init();

    strcpy(output_file_name, argv[2]);
    char tmp_name[128];
    sprintf(tmp_name, "%d_%s", scene_count, output_file_name);

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = open_output_file(tmp_name)) < 0)
        goto end;

    video_dec_ctx = ifmt_ctx->streams[video_stream_idx]->codec;

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0)
            break;
        if (video_stream_idx != packet.stream_index)
            continue;

        stream_index = packet.stream_index;
        type = ifmt_ctx->streams[packet.stream_index]->codecpar->codec_type;
        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n",
                stream_index);

        {
            av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");
            frame = av_frame_alloc();
            if (!frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            av_packet_rescale_ts(&packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 stream_ctx[stream_index].dec_ctx->time_base);
            dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 :
                avcodec_decode_audio4;
            ret = dec_func(stream_ctx[stream_index].dec_ctx, frame,
                    &got_frame, &packet);

            if (ret < 0) {
                av_frame_free(&frame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }

            if (got_frame) {
                //frame->pts = pts;
                //pts += 1;

                //let encoder to decide the frame type
                frame->key_frame = 0;
                frame->pict_type = AV_PICTURE_TYPE_NONE;

                ret = encode_write_frame(frame, stream_index, NULL);
                av_frame_free(&frame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&frame);
            }
        }
        av_packet_unref(&packet);
    }

    /* flush filters and encoders */
    ret = encode_write_frame(NULL, video_stream_idx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
        goto end;
    }

    /* flush encoder */
    ret = flush_encoder(video_stream_idx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
        goto end;
    }

    av_write_trailer(ofmt_ctx);
end:
    av_packet_unref(&packet);
    av_frame_free(&frame);
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);
        if (filter_ctx && filter_ctx[i].filter_graph)
            avfilter_graph_free(&filter_ctx[i].filter_graph);
    }
    av_free(filter_ctx);
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_err2str(ret).c_str());

    return ret ? 1 : 0;
}
