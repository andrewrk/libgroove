/* play the specified audio file on the default audio device */

#include <inttypes.h>

#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/samplefmt.h"
#include "libavformat/avformat.h"
#include "libavresample/avresample.h"
#include "libavutil/opt.h"

#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_AUDIOQ_SIZE (20 * 16 * 1024)

/* SDL audio buffer size, in samples. Should be small to have precise
   A/V sync as SDL does not have hardware buffer fullness info. */
#define SDL_AUDIO_BUFFER_SIZE 1024

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

typedef struct AudioState {
    SDL_Thread *parse_tid;
    int abort_request;
    int paused;
    int last_paused;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    AVFormatContext *ic;

    int audio_stream;

    double audio_clock;
    AVStream *audio_st;
    AVCodec *decoder;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t silence_buf[SDL_AUDIO_BUFFER_SIZE];
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    int audio_buf_index; /* in bytes */
    AVPacket audio_pkt_temp;
    AVPacket audio_pkt;
    enum AVSampleFormat sdl_sample_fmt;
    uint64_t sdl_channel_layout;
    int sdl_channels;
    int sdl_sample_rate;
    enum AVSampleFormat resample_sample_fmt;
    uint64_t resample_channel_layout;
    int resample_sample_rate;
    AVAudioResampleContext *avr;
    AVFrame *frame;

    char filename[1024];
    int width;

    int refresh;
    int seek_by_bytes;
} AudioState;

/* options specified by the user */
static int autoexit;
static int loop = 1;

/* current context */
static AudioState *cur_stream;

static AVPacket flush_pkt;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static int packet_queue_put(PacketQueue *q, AVPacket *pkt);

/* packet queue handling */
static void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    packet_queue_put(q, &flush_pkt);
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    /* duplicate the packet */
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    pkt1 = av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;


    SDL_LockMutex(q->mutex);

    if (!q->last_pkt)

        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}


/* get the current audio output buffer size, in samples. With SDL, we
   cannot have a precise information */
static int audio_write_get_buf_size(AudioState *is)
{
    return is->audio_buf_size - is->audio_buf_index;
}


/* get the current audio clock value */
static double get_audio_clock(AudioState *is)
{
    double pts;
    int hw_buf_size, bytes_per_sec;
    pts = is->audio_clock;
    hw_buf_size = audio_write_get_buf_size(is);
    bytes_per_sec = 0;
    if (is->audio_st) {
        bytes_per_sec = is->sdl_sample_rate * is->sdl_channels *
                        av_get_bytes_per_sample(is->sdl_sample_fmt);
    }
    if (bytes_per_sec)
        pts -= (double)hw_buf_size / bytes_per_sec;
    return pts;
}

/* seek in the stream */
static void stream_seek(AudioState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
    }
}

/* pause or resume the video */
static void stream_pause(AudioState *is)
{
    is->paused = !is->paused;
}

static void stream_close(AudioState *is)
{
    is->abort_request = 1;
    SDL_WaitThread(is->parse_tid, NULL);

    av_free(is);
}

static void do_exit(void)
{
    if (cur_stream) {
        stream_close(cur_stream);
        cur_stream = NULL;
    }
    avformat_network_deinit();
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "");
    exit(0);
}

/* decode one audio frame and returns its uncompressed size */
static int audio_decode_frame(AudioState *is)
{
    AVPacket *pkt_temp = &is->audio_pkt_temp;
    AVPacket *pkt = &is->audio_pkt;
    AVCodecContext *dec = is->audio_st->codec;
    int n, len1, data_size, got_frame;
    double pts;
    int new_packet = 0;
    int flush_complete = 0;

    for (;;) {
        /* NOTE: the audio packet can contain several frames */
        while (pkt_temp->size > 0 || (!pkt_temp->data && new_packet)) {
            if (!is->frame) {
                if (!(is->frame = avcodec_alloc_frame()))
                    return AVERROR(ENOMEM);
            } else
                avcodec_get_frame_defaults(is->frame);

            if (flush_complete)
                break;
            new_packet = 0;
            len1 = avcodec_decode_audio4(dec, is->frame, &got_frame, pkt_temp);
            if (len1 < 0) {
                /* if error, we skip the frame */
                pkt_temp->size = 0;
                break;
            }

            pkt_temp->data += len1;
            pkt_temp->size -= len1;

            if (!got_frame) {
                /* stop sending empty packets if the decoder is finished */
                if (!pkt_temp->data && dec->codec->capabilities & CODEC_CAP_DELAY)
                    flush_complete = 1;
                continue;
            }
            data_size = av_samples_get_buffer_size(NULL, dec->channels,
                                                   is->frame->nb_samples,
                                                   is->frame->format, 1);

            int audio_resample = is->frame->format         != is->sdl_sample_fmt     ||
                             is->frame->channel_layout != is->sdl_channel_layout ||
                             is->frame->sample_rate    != is->sdl_sample_rate;

            int resample_changed = is->frame->format         != is->resample_sample_fmt     ||
                               is->frame->channel_layout != is->resample_channel_layout ||
                               is->frame->sample_rate    != is->resample_sample_rate;

            if ((!is->avr && audio_resample) || resample_changed) {
                int ret;
                if (is->avr)
                    avresample_close(is->avr);
                else if (audio_resample) {
                    is->avr = avresample_alloc_context();
                    if (!is->avr) {
                        fprintf(stderr, "error allocating AVAudioResampleContext\n");
                        break;
                    }
                }
                if (audio_resample) {
                    av_opt_set_int(is->avr, "in_channel_layout",  is->frame->channel_layout, 0);
                    av_opt_set_int(is->avr, "in_sample_fmt",      is->frame->format,         0);
                    av_opt_set_int(is->avr, "in_sample_rate",     is->frame->sample_rate,    0);
                    av_opt_set_int(is->avr, "out_channel_layout", is->sdl_channel_layout,    0);
                    av_opt_set_int(is->avr, "out_sample_fmt",     is->sdl_sample_fmt,        0);
                    av_opt_set_int(is->avr, "out_sample_rate",    is->sdl_sample_rate,       0);

                    if ((ret = avresample_open(is->avr)) < 0) {
                        fprintf(stderr, "error initializing libavresample\n");
                        break;
                    }
                }
                is->resample_sample_fmt     = is->frame->format;
                is->resample_channel_layout = is->frame->channel_layout;
                is->resample_sample_rate    = is->frame->sample_rate;
            }

            if (audio_resample) {
                void *tmp_out;
                int out_samples, out_size, out_linesize;
                int osize      = av_get_bytes_per_sample(is->sdl_sample_fmt);
                int nb_samples = is->frame->nb_samples;

                out_size = av_samples_get_buffer_size(&out_linesize,
                                                      is->sdl_channels,
                                                      nb_samples,
                                                      is->sdl_sample_fmt, 0);
                tmp_out = av_realloc(is->audio_buf1, out_size);
                if (!tmp_out)
                    return AVERROR(ENOMEM);
                is->audio_buf1 = tmp_out;

                out_samples = avresample_convert(is->avr,
                                                 &is->audio_buf1,
                                                 out_linesize, nb_samples,
                                                 is->frame->data,
                                                 is->frame->linesize[0],
                                                 is->frame->nb_samples);
                if (out_samples < 0) {
                    fprintf(stderr, "avresample_convert() failed\n");
                    break;
                }
                is->audio_buf = is->audio_buf1;
                data_size = out_samples * osize * is->sdl_channels;
            } else {
                is->audio_buf = is->frame->data[0];
            }

            /* if no pts, then compute it */
            pts = is->audio_clock;
            n = is->sdl_channels * av_get_bytes_per_sample(is->sdl_sample_fmt);
            is->audio_clock += (double)data_size / (double)(n * is->sdl_sample_rate);
            return data_size;
        }

        /* free the current packet */
        if (pkt->data)
            av_free_packet(pkt);
        memset(pkt_temp, 0, sizeof(*pkt_temp));

        if (is->paused || is->audioq.abort_request) {
            return -1;
        }

        /* read next packet */
        if ((new_packet = packet_queue_get(&is->audioq, pkt, 1)) < 0)
            return -1;

        if (pkt->data == flush_pkt.data) {
            avcodec_flush_buffers(dec);
            flush_complete = 0;
        }

        *pkt_temp = *pkt;

        /* if update the audio clock with the pts */
        if (pkt->pts != AV_NOPTS_VALUE) {
            is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
        }
    }
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    AudioState *is = opaque;
    int audio_size, len1;

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(is);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf      = is->silence_buf;
               is->audio_buf_size = sizeof(is->silence_buf);
           } else {
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(AudioState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx = ic->streams[stream_index]->codec;

    if (avcodec_open2(avctx, is->decoder, NULL) < 0) {
        fprintf(stderr, "unable to open decoder\n");
        return -1;
    }

    /* prepare audio output */
    is->sdl_sample_rate = avctx->sample_rate;

    if (!avctx->channel_layout)
        avctx->channel_layout = av_get_default_channel_layout(avctx->channels);
    if (!avctx->channel_layout) {
        fprintf(stderr, "unable to guess channel layout\n");
        return -1;
    }
    is->sdl_channel_layout = (avctx->channels == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    is->sdl_channels = av_get_channel_layout_nb_channels(is->sdl_channel_layout);

    SDL_AudioSpec wanted_spec;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.freq = is->sdl_sample_rate;
    wanted_spec.channels = is->sdl_channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = is;
    SDL_AudioSpec spec;
    if (SDL_OpenAudio(&wanted_spec, &spec) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        return -1;
    }
    is->audio_hw_buf_size = spec.size;
    is->sdl_sample_fmt          = AV_SAMPLE_FMT_S16;
    is->resample_sample_fmt     = is->sdl_sample_fmt;
    is->resample_channel_layout = avctx->channel_layout;
    is->resample_sample_rate    = avctx->sample_rate;

    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    is->audio_stream = stream_index;
    is->audio_st = ic->streams[stream_index];
    is->audio_buf_size  = 0;
    is->audio_buf_index = 0;

    memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
    packet_queue_init(&is->audioq);
    SDL_PauseAudio(0);

    return 0;
}

static void stream_component_close(AudioState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    avctx = ic->streams[stream_index]->codec;

    packet_queue_abort(&is->audioq);

    SDL_CloseAudio();

    packet_queue_end(&is->audioq);
    av_free_packet(&is->audio_pkt);
    if (is->avr)
        avresample_free(&is->avr);
    av_freep(&is->audio_buf1);
    is->audio_buf = NULL;
    avcodec_free_frame(&is->frame);

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    avcodec_close(avctx);
    is->audio_st = NULL;
    is->audio_stream = -1;
}

static int decode_interrupt_cb(void *ctx)
{
    AudioState *is = ctx;
    return is && is->abort_request;
}

/* this thread gets the stream from the disk or the network */
static int decode_thread(void *arg)
{
    int ret;
    int eof = 0;

    AVPacket pkt1;
    AVPacket *pkt = &pkt1;

    AudioState *is = arg;
    is->audio_stream = -1;

    AVFormatContext *ic = avformat_alloc_context();
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    int err = avformat_open_input(&ic, is->filename, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "error opening input file\n");
        ret = -1;
        goto fail;
    }
    is->ic = ic;

    err = avformat_find_stream_info(ic, NULL);
    if (err < 0) {
        fprintf(stderr, "%s: could not find codec parameters\n", is->filename);
        ret = -1;
        goto fail;
    }

    is->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT);


    for (int i = 0; i < ic->nb_streams; i++)
        ic->streams[i]->discard = AVDISCARD_ALL;

    int stream_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, &is->decoder, 0);

    if (stream_index < 0) {
        fprintf(stderr, "%s: no audio stream found\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (!is->decoder) {
        fprintf(stderr, "%s: no decoder found\n", is->filename);
        ret = -1;
        goto fail;
    }

    /* open the streams */
    ret = stream_component_open(is, stream_index);
    if (ret < 0) {
        fprintf(stderr, "%s: error opening stream\n", is->filename);
        goto fail;
    }

    for (;;) {
        if (is->abort_request)
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                av_read_pause(ic);
            else
                av_read_play(ic);
        }
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                fprintf(stderr, "%s: error while seeking\n", is->ic->filename);
            } else {
                packet_queue_flush(&is->audioq);
                packet_queue_put(&is->audioq, &flush_pkt);
            }
            is->seek_req = 0;
            eof = 0;
        }

        /* if the queue are full, no need to read more */
        if (is->audioq.size > MAX_QUEUE_SIZE || is->audioq.size > MIN_AUDIOQ_SIZE) {
            /* wait 10 ms */
            SDL_Delay(10);
            continue;
        }
        if (eof) {
            if (is->audio_st->codec->codec->capabilities & CODEC_CAP_DELAY) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = is->audio_stream;
                packet_queue_put(&is->audioq, pkt);
            }
            SDL_Delay(10);
            if (is->audioq.size == 0) {
                if (loop != 1 && (!loop || --loop)) {
                    stream_seek(cur_stream, 0, 0, 0);
                } else if (autoexit) {
                    ret = AVERROR_EOF;
                    goto fail;
                }
            }
            continue;
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                eof = 1;
            SDL_Delay(100); /* wait for user event */
            continue;
        }
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        if (pkt->stream_index == is->audio_stream) {
            packet_queue_put(&is->audioq, pkt);
        } else {
            av_free_packet(pkt);
        }
    }
    /* wait until the end */
    while (!is->abort_request) {
        SDL_Delay(100);
    }

    ret = 0;
 fail:
    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);

    /* disable interrupting */
    is->abort_request = 0;

    if (is->ic) {
        avformat_close_input(&is->ic);
    }

    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    return 0;
}

static void toggle_pause(void)
{
    if (cur_stream)
        stream_pause(cur_stream);
}


/* handle an event sent by the GUI */
static int event_loop() {
    SDL_Event event;
    double incr, pos, frac;
    double x;

    for (;;) {
        while(SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    do_exit();
                    break;
                case SDLK_p:
                case SDLK_SPACE:
                    toggle_pause();
                    break;
                case SDLK_LEFT:
                    incr = -10.0;
                    goto do_seek;
                case SDLK_RIGHT:
                    incr = 10.0;
                    goto do_seek;
                case SDLK_UP:
                    incr = 60.0;
                    goto do_seek;
                case SDLK_DOWN:
                    incr = -60.0;
                do_seek:
                    if (cur_stream) {
                        if (cur_stream->seek_by_bytes) {
                            if (cur_stream->audio_pkt.pos >= 0) {
                                pos = cur_stream->audio_pkt.pos;
                            } else {
                                pos = avio_tell(cur_stream->ic->pb);
                            }
                            if (cur_stream->ic->bit_rate)
                                incr *= cur_stream->ic->bit_rate / 8.0;
                            else
                                incr *= 180000.0;
                            pos += incr;
                            stream_seek(cur_stream, pos, incr, 1);
                        } else {
                            pos = get_audio_clock(cur_stream);
                            pos += incr;
                            stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                        }
                    }
                    break;
                default:
                    break;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                do_exit();
                break;
            case SDL_MOUSEMOTION:
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    x = event.button.x;
                } else {
                    if (event.motion.state != SDL_PRESSED)
                        break;
                    x = event.motion.x;
                }
                if (cur_stream) {
                    if (cur_stream->seek_by_bytes || cur_stream->ic->duration <= 0) {
                        uint64_t size =  avio_size(cur_stream->ic->pb);
                        stream_seek(cur_stream, size*x/cur_stream->width, 0, 1);
                    } else {
                        int64_t ts;
                        int ns, hh, mm, ss;
                        int tns, thh, tmm, tss;
                        tns  = cur_stream->ic->duration / 1000000LL;
                        thh  = tns / 3600;
                        tmm  = (tns % 3600) / 60;
                        tss  = (tns % 60);
                        frac = x / cur_stream->width;
                        ns   = frac * tns;
                        hh   = ns / 3600;
                        mm   = (ns % 3600) / 60;
                        ss   = (ns % 60);
                        fprintf(stderr, "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                                hh, mm, ss, thh, tmm, tss);
                        ts = frac * cur_stream->ic->duration;
                        if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                            ts += cur_stream->ic->start_time;
                        stream_seek(cur_stream, ts, 0, 0);
                    }
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit();
                break;
            default:
                break;
            }
        }
        SDL_Delay(10);
    }
    return 0;
}

/* Called from the main */
int main(int argc, char **argv)
{
    // uncomment to get rid of those annoying log messages
    //av_log_set_level(AV_LOG_QUIET);

    /* register all codecs, demux and protocols */
    avcodec_register_all();
    av_register_all();
    avformat_network_init();


    char * input_filename = argv[1];
    if (!input_filename) {
        fprintf(stderr, "Usage: %s input_file\n", argv[0]);
        exit(1);
    }

    int flags = SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init (flags)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        exit(1);
    }


    av_init_packet(&flush_pkt);

    cur_stream = av_mallocz(sizeof(AudioState));
    if (!cur_stream) {
        fprintf(stderr, "Error initializing state: Out of memory\n");
        exit(1);
    }
    av_strlcpy(cur_stream->filename, input_filename, sizeof(cur_stream->filename));

    cur_stream->parse_tid = SDL_CreateThread(decode_thread, cur_stream);
    if (!cur_stream->parse_tid) {
        av_free(cur_stream);
        fprintf(stderr, "Error creating decode thread: Out of memory\n");
        exit(1);
    }

    return event_loop();
}

