#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>

#include <ao/ao.h>

static ao_device *device = NULL;

static char strbuf[512];
static AVFilterGraph *filter_graph = NULL;
static AVFilterContext *abuffer_ctx = NULL;
static AVFilterContext *volume_ctx = NULL;
static AVFilterContext *aformat_ctx = NULL;
static AVFilterContext *abuffersink_ctx = NULL;

static AVFrame *oframe = NULL;

static int init_filter_graph(AVFormatContext *ic, AVStream *audio_st) {
    // create new graph
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "unable to create filter graph: out of memory\n");
        return -1;
    }

    AVFilter *abuffer = avfilter_get_by_name("abuffer");
    AVFilter *volume = avfilter_get_by_name("volume");
    AVFilter *aformat = avfilter_get_by_name("aformat");
    AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    int err;
    // create abuffer filter
    AVCodecContext *avctx = audio_st->codec;
    AVRational time_base = audio_st->time_base;
    snprintf(strbuf, sizeof(strbuf),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64, 
            time_base.num, time_base.den, avctx->sample_rate,
            av_get_sample_fmt_name(avctx->sample_fmt),
            avctx->channel_layout);
    fprintf(stderr, "abuffer: %s\n", strbuf);
    err = avfilter_graph_create_filter(&abuffer_ctx, abuffer,
            NULL, strbuf, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing abuffer filter\n");
        return err;
    }
    // create volume filter
    double vol = 0.90;
    snprintf(strbuf, sizeof(strbuf), "volume=%f", vol);
    fprintf(stderr, "volume: %s\n", strbuf);
    err = avfilter_graph_create_filter(&volume_ctx, volume, NULL,
            strbuf, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error initializing volume filter\n");
        return err;
    }
    // create aformat filter
    snprintf(strbuf, sizeof(strbuf),
            "sample_fmts=%s:sample_rates=%d:channel_layouts=0x%"PRIx64,
            av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), 44100,
            (uint64_t)AV_CH_LAYOUT_STEREO);
    fprintf(stderr, "aformat: %s\n", strbuf);
    err = avfilter_graph_create_filter(&aformat_ctx, aformat,
            NULL, strbuf, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter\n");
        return err;
    }
    // create abuffersink filter
    err = avfilter_graph_create_filter(&abuffersink_ctx, abuffersink,
            NULL, NULL, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to create aformat filter\n");
        return err;
    }

    // connect inputs and outputs
    if (err >= 0) err = avfilter_link(abuffer_ctx, 0, volume_ctx, 0);
    if (err >= 0) err = avfilter_link(volume_ctx, 0, aformat_ctx, 0);
    if (err >= 0) err = avfilter_link(aformat_ctx, 0, abuffersink_ctx, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error connecting filters\n");
        return err;
    }
    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "error configuring the filter graph\n");
        return err;
    }
    return 0;
}

static int audio_decode_frame(AVFormatContext *ic, AVStream *audio_st,
        AVPacket *pkt, AVFrame *frame)
{
    AVPacket pkt_temp_;
    memset(&pkt_temp_, 0, sizeof(pkt_temp_));
    AVPacket *pkt_temp = &pkt_temp_;

    *pkt_temp = *pkt;

    int len1, got_frame;
    int new_packet = 1;
    while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
        avcodec_get_frame_defaults(frame);
        new_packet = 0;

        len1 = avcodec_decode_audio4(audio_st->codec, frame, &got_frame, pkt_temp);
        if (len1 < 0) {
            // if error we skip the frame
            pkt_temp->size = 0;
            return -1;
        }

        pkt_temp->data += len1;
        pkt_temp->size -= len1;

        if (!got_frame) {
            // stop sending empty packets if the decoder is finished
            if (!pkt_temp->data &&
                    audio_st->codec->codec->capabilities&CODEC_CAP_DELAY)
            {
                return 0;
            }
            continue;
        }

        // push the audio data from decoded frame into the filtergraph
        int err = av_buffersrc_write_frame(abuffer_ctx, frame);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "error writing frame to buffersrc\n");
            return -1;
        }
        // pull filtered audio from the filtergraph
        for (;;) {
            int err = av_buffersink_get_frame(abuffersink_ctx, oframe);
            if (err == AVERROR_EOF || err == AVERROR(EAGAIN))
                break;
            if (err < 0) {
                av_log(NULL, AV_LOG_ERROR, "error reading buffer from buffersink\n");
                return -1;
            }
            ao_play(device, (void*)oframe->data[0], oframe->linesize[0]);
        }
        return 0;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        return 1;
    }


    ao_initialize();
    avcodec_register_all();
    av_register_all();
    avformat_network_init();
    avfilter_register_all();

    ao_sample_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.bits = 16;
    fmt.channels = 2;
    fmt.rate = 44100;
    fmt.byte_format = AO_FMT_NATIVE;
    device = ao_open_live(ao_default_driver_id(), &fmt, NULL);
    if (!device) {
        av_log(NULL, AV_LOG_ERROR, "opening audio device\n");
        return 1;
    }

    AVFormatContext *ic = NULL;
    char *filename = argv[1];
    if (avformat_open_input(&ic, filename, NULL, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "error opening %s\n", filename);
        return 1;
    }

    if (avformat_find_stream_info(ic, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: could not find codec parameters\n", filename);
        return 1;
    }

    // set all streams to discard. in a few lines here we will find the audio
    // stream and cancel discarding it
    for (int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;

    AVCodec *decoder = NULL;
    int audio_stream_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1,
            &decoder, 0);

    if (audio_stream_index < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: no audio stream found\n", ic->filename);
        return 1;
    }

    if (!decoder) {
        av_log(NULL, AV_LOG_ERROR, "%s: no decoder found\n", ic->filename);
        return 1;
    }

    AVStream *audio_st = ic->streams[audio_stream_index];
    audio_st->discard = AVDISCARD_DEFAULT;

    AVCodecContext *avctx = audio_st->codec;

    if (avcodec_open2(avctx, decoder, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to open decoder\n");
        return 1;
    }

    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        av_log(NULL, AV_LOG_ERROR, "unable to guess channel layout\n");
        return 1;
    }

    if (init_filter_graph(ic, audio_st) < 0) {
        av_log(NULL, AV_LOG_ERROR, "unable to init filter graph\n");
        return 1;
    }

    AVPacket audio_pkt;
    memset(&audio_pkt, 0, sizeof(audio_pkt));
    AVPacket *pkt = &audio_pkt;
    AVFrame *frame = avcodec_alloc_frame();

    oframe = av_frame_alloc();
    if (!oframe) {
        av_log(NULL, AV_LOG_ERROR, "error allocating oframe\n");
        return 1;
    }

    int eof = 0;
    for (;;) {
        if (eof) {
            if (avctx->codec->capabilities & CODEC_CAP_DELAY) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = audio_stream_index;
                if (audio_decode_frame(ic, audio_st, pkt, frame) > 0) {
                    // keep flushing
                    continue;
                }
            }
            break;
        }
        int err = av_read_frame(ic, pkt);
        if (err < 0) {
            if (err != AVERROR_EOF)
                av_log(NULL, AV_LOG_WARNING, "error reading frames\n");
            eof = 1;
            continue;
        }
        if (pkt->stream_index != audio_stream_index) {
            av_free_packet(pkt);
            continue;
        }
        audio_decode_frame(ic, audio_st, pkt, frame);
        av_free_packet(pkt);
    }

    avformat_network_deinit();
    ao_close(device);
    ao_shutdown();

    return 0;
}
