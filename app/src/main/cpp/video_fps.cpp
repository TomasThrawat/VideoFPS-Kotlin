#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {
std::atomic_bool g_cancel{false};

std::string ffError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return std::string(buf);
}

void progress(JNIEnv* env, jobject listener, jmethodID method, int value) {
    if (!env || !listener || !method) return;
    value = std::max(0, std::min(100, value));
    env->CallVoidMethod(listener, method, static_cast<jint>(value));
    if (env->ExceptionCheck()) env->ExceptionClear();
}

int writeVideoPacket(
    AVCodecContext* encoder,
    AVFormatContext* output,
    AVStream* stream,
    AVFrame* frame
) {
    int ret = avcodec_send_frame(encoder, frame);
    if (ret < 0) return ret;

    AVPacket* packet = av_packet_alloc();
    if (!packet) return AVERROR(ENOMEM);

    while (true) {
        ret = avcodec_receive_packet(encoder, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) break;

        packet->stream_index = stream->index;
        av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
        ret = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (ret < 0) break;
    }

    av_packet_free(&packet);
    return ret;
}

int writeAudioPacket(
    AVCodecContext* encoder,
    AVFormatContext* output,
    AVStream* stream,
    AVFrame* frame
) {
    int ret = avcodec_send_frame(encoder, frame);
    if (ret < 0) return ret;

    AVPacket* packet = av_packet_alloc();
    if (!packet) return AVERROR(ENOMEM);

    while (true) {
        ret = avcodec_receive_packet(encoder, packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) break;

        packet->stream_index = stream->index;
        av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
        ret = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (ret < 0) break;
    }

    av_packet_free(&packet);
    return ret;
}

int drainVideo(
    AVFilterContext* sink,
    AVCodecContext* encoder,
    AVFormatContext* output,
    AVStream* stream
) {
    const AVRational sinkTimeBase = av_buffersink_get_time_base(sink);
    AVFrame* frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);

    int ret = 0;
    while (true) {
        ret = av_buffersink_get_frame(sink, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            ret = 0;
            break;
        }
        if (ret < 0) break;

        if (frame->pts != AV_NOPTS_VALUE) {
            frame->pts = av_rescale_q(
                frame->pts,
                sinkTimeBase,
                encoder->time_base
            );
        }

        ret = writeVideoPacket(encoder, output, stream, frame);
        av_frame_unref(frame);
        if (ret < 0) break;
    }

    av_frame_free(&frame);
    return ret;
}

int chooseSampleRate(const AVCodec* codec, int requested) {
    if (!codec->supported_samplerates) {
        return requested > 0 ? requested : 48000;
    }

    for (const int* rate = codec->supported_samplerates; *rate; ++rate) {
        if (*rate == requested) return requested;
    }

    for (const int* rate = codec->supported_samplerates; *rate; ++rate) {
        if (*rate == 48000) return 48000;
    }

    return codec->supported_samplerates[0];
}

AVSampleFormat chooseSampleFormat(const AVCodec* codec) {
    return codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
}

int convertAudio(
    AVFrame* input,
    AVFrame* output,
    SwrContext* swr,
    const AVChannelLayout* outputLayout,
    int outputRate,
    AVSampleFormat outputFormat,
    int64_t outputPts
) {
    const int outputSamples = static_cast<int>(av_rescale_rnd(
        swr_get_delay(swr, input->sample_rate) + input->nb_samples,
        outputRate,
        input->sample_rate,
        AV_ROUND_UP
    ));

    output->format = outputFormat;
    output->sample_rate = outputRate;
    output->ch_layout = *outputLayout;
    output->nb_samples = outputSamples;
    output->pts = outputPts;

    int ret = av_frame_get_buffer(output, 0);
    if (ret < 0) return ret;

    const uint8_t** inData =
        const_cast<const uint8_t**>(input->extended_data);

    ret = swr_convert(
        swr,
        output->data,
        output->nb_samples,
        inData,
        input->nb_samples
    );
    if (ret < 0) return ret;

    output->nb_samples = ret;
    return 0;
}

std::string process(
    int inputFd,
    int outputFd,
    int targetFps,
    int64_t durationUs,
    JNIEnv* env,
    jobject listener
) {
    g_cancel.store(false);

    jclass listenerClass =
        env->FindClass("com/hyouka/videofps/ProgressListener");
    if (!listenerClass) {
        env->ExceptionClear();
        return "تعذر تجهيز مستمع التقدم";
    }

    const jmethodID progressMethod =
        env->GetMethodID(listenerClass, "onProgress", "(I)V");
    if (!progressMethod) {
        env->ExceptionClear();
        return "تعذر تجهيز التقدم";
    }

    const std::string inputPath =
        "/proc/self/fd/" + std::to_string(inputFd);
    const std::string outputPath =
        "/proc/self/fd/" + std::to_string(outputFd);

    AVFormatContext* input = nullptr;
    AVFormatContext* output = nullptr;
    AVCodecContext* videoDecoder = nullptr;
    AVCodecContext* audioDecoder = nullptr;
    AVCodecContext* videoEncoder = nullptr;
    AVCodecContext* audioEncoder = nullptr;
    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;
    SwrContext* swr = nullptr;

    AVPacket* packet = nullptr;
    AVFrame* videoFrame = nullptr;
    AVFrame* audioFrame = nullptr;
    AVFrame* convertedAudio = nullptr;

    int videoIndex = -1;
    int audioIndex = -1;
    int ret = 0;
    std::string error;

    av_log_set_level(AV_LOG_ERROR);

    do {
        ret = avformat_open_input(
            &input,
            inputPath.c_str(),
            nullptr,
            nullptr
        );
        if (ret < 0) {
            error = "فتح الفيديو فشل: " + ffError(ret);
            break;
        }

        ret = avformat_find_stream_info(input, nullptr);
        if (ret < 0) {
            error = "قراءة معلومات الفيديو فشلت: " + ffError(ret);
            break;
        }

        for (unsigned i = 0; i < input->nb_streams; ++i) {
            const AVCodecParameters* params =
                input->streams[i]->codecpar;

            if (params->codec_type == AVMEDIA_TYPE_VIDEO &&
                videoIndex < 0) {
                videoIndex = static_cast<int>(i);
            } else if (
                params->codec_type == AVMEDIA_TYPE_AUDIO &&
                audioIndex < 0
            ) {
                audioIndex = static_cast<int>(i);
            }
        }

        if (videoIndex < 0) {
            error = "لم يتم العثور على مسار فيديو";
            break;
        }

        const AVStream* videoInput =
            input->streams[videoIndex];

        const AVCodec* videoDecoderCodec =
            avcodec_find_decoder(
                videoInput->codecpar->codec_id
            );

        if (!videoDecoderCodec) {
            error = "ترميز الفيديو غير مدعوم";
            break;
        }

        videoDecoder =
            avcodec_alloc_context3(videoDecoderCodec);

        if (!videoDecoder) {
            error = "تعذر إنشاء مفكك الفيديو";
            break;
        }

        ret = avcodec_parameters_to_context(
            videoDecoder,
            videoInput->codecpar
        );

        if (ret < 0) {
            error = "تهيئة مفكك الفيديو فشلت: " +
                ffError(ret);
            break;
        }

        videoDecoder->thread_count = 0;

        ret = avcodec_open2(
            videoDecoder,
            videoDecoderCodec,
            nullptr
        );

        if (ret < 0) {
            error = "فتح مفكك الفيديو فشل: " +
                ffError(ret);
            break;
        }

        if (audioIndex >= 0) {
            const AVStream* audioInput =
                input->streams[audioIndex];

            const AVCodec* audioDecoderCodec =
                avcodec_find_decoder(
                    audioInput->codecpar->codec_id
                );

            if (audioDecoderCodec) {
                audioDecoder =
                    avcodec_alloc_context3(
                        audioDecoderCodec
                    );

                if (audioDecoder) {
                    ret = avcodec_parameters_to_context(
                        audioDecoder,
                        audioInput->codecpar
                    );

                    if (ret < 0 ||
                        avcodec_open2(
                            audioDecoder,
                            audioDecoderCodec,
                            nullptr
                        ) < 0) {
                        avcodec_free_context(&audioDecoder);
                    }
                }
            }
        }

        ret = avformat_alloc_output_context2(
            &output,
            nullptr,
            "mp4",
            outputPath.c_str()
        );

        if (ret < 0 || !output) {
            error = "تعذر إنشاء MP4: " +
                ffError(ret);
            break;
        }

        const AVCodec* videoEncoderCodec =
            avcodec_find_encoder_by_name("libopenh264");

        if (!videoEncoderCodec) {
            error = "OpenH264 encoder غير موجود";
            break;
        }

        videoEncoder =
            avcodec_alloc_context3(videoEncoderCodec);

        if (!videoEncoder) {
            error = "تعذر إنشاء encoder الفيديو";
            break;
        }

        int width = videoDecoder->width;
        int height = videoDecoder->height;

        if (width & 1) --width;
        if (height & 1) --height;

        if (width <= 0 || height <= 0 ||
            width > 3840 || height > 2160) {
            error = "الدقة غير مناسبة. الحد الأقصى 3840 × 2160";
            break;
        }

        videoEncoder->codec_type = AVMEDIA_TYPE_VIDEO;
        videoEncoder->codec_id = AV_CODEC_ID_H264;
        videoEncoder->width = width;
        videoEncoder->height = height;
        videoEncoder->pix_fmt = AV_PIX_FMT_YUV420P;
        videoEncoder->time_base = AVRational{1, targetFps};
        videoEncoder->framerate = AVRational{targetFps, 1};
        videoEncoder->gop_size = targetFps * 2;
        videoEncoder->max_b_frames = 0;
        videoEncoder->bit_rate = std::clamp<int64_t>(
            static_cast<int64_t>(width) * height * targetFps / 50,
            2000000LL,
            20000000LL
        );

        if (output->oformat->flags & AVFMT_GLOBALHEADER) {
            videoEncoder->flags |=
                AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        ret = avcodec_open2(
            videoEncoder,
            videoEncoderCodec,
            nullptr
        );

        if (ret < 0) {
            error = "فتح OpenH264 فشل: " +
                ffError(ret);
            break;
        }

        AVStream* videoOutput =
            avformat_new_stream(output, nullptr);

        if (!videoOutput) {
            error = "تعذر إنشاء مسار الفيديو";
            break;
        }

        videoOutput->time_base =
            videoEncoder->time_base;

        ret = avcodec_parameters_from_context(
            videoOutput->codecpar,
            videoEncoder
        );

        if (ret < 0) {
            error = "تعذر تجهيز إعدادات الفيديو";
            break;
        }

        videoOutput->codecpar->codec_tag = 0;

        AVStream* audioOutput = nullptr;

        if (audioDecoder) {
            const AVCodec* audioEncoderCodec =
                avcodec_find_encoder(AV_CODEC_ID_AAC);

            if (audioEncoderCodec) {
                audioEncoder =
                    avcodec_alloc_context3(
                        audioEncoderCodec
                    );

                if (audioEncoder) {
                    audioEncoder->sample_rate =
                        chooseSampleRate(
                            audioEncoderCodec,
                            audioDecoder->sample_rate
                        );

                    audioEncoder->sample_fmt =
                        chooseSampleFormat(audioEncoderCodec);

                    audioEncoder->bit_rate =
                        audioDecoder->ch_layout.nb_channels <= 2
                            ? 128000
                            : 192000;

                    audioEncoder->time_base =
                        AVRational{
                            1,
                            audioEncoder->sample_rate
                        };

                    if (audioDecoder->ch_layout.nb_channels > 0) {
                        av_channel_layout_copy(
                            &audioEncoder->ch_layout,
                            &audioDecoder->ch_layout
                        );
                    } else {
                        av_channel_layout_default(
                            &audioEncoder->ch_layout,
                            2
                        );
                    }

                    if (output->oformat->flags &
                        AVFMT_GLOBALHEADER) {
                        audioEncoder->flags |=
                            AV_CODEC_FLAG_GLOBAL_HEADER;
                    }

                    ret = avcodec_open2(
                        audioEncoder,
                        audioEncoderCodec,
                        nullptr
                    );

                    if (ret >= 0) {
                        audioOutput =
                            avformat_new_stream(
                                output,
                                nullptr
                            );

                        if (!audioOutput) {
                            ret = AVERROR(ENOMEM);
                        } else {
                            audioOutput->time_base =
                                audioEncoder->time_base;

                            ret =
                                avcodec_parameters_from_context(
                                    audioOutput->codecpar,
                                    audioEncoder
                                );
                        }
                    }

                    if (ret < 0) {
                        avcodec_free_context(
                            &audioEncoder
                        );
                        audioOutput = nullptr;
                    }
                }
            }
        }

        if (audioDecoder && audioEncoder && audioOutput) {
            AVChannelLayout inputLayout;
            AVChannelLayout outputLayout;

            if (audioDecoder->ch_layout.nb_channels > 0) {
                av_channel_layout_copy(
                    &inputLayout,
                    &audioDecoder->ch_layout
                );
            } else {
                av_channel_layout_default(
                    &inputLayout,
                    2
                );
            }

            av_channel_layout_copy(
                &outputLayout,
                &audioEncoder->ch_layout
            );

            ret = swr_alloc_set_opts2(
                &swr,
                &outputLayout,
                audioEncoder->sample_fmt,
                audioEncoder->sample_rate,
                &inputLayout,
                audioDecoder->sample_fmt,
                audioDecoder->sample_rate,
                0,
                nullptr
            );

            av_channel_layout_uninit(&inputLayout);
            av_channel_layout_uninit(&outputLayout);

            if (ret < 0 || !swr) {
                error =
                    "تهيئة محول الصوت فشلت: " +
                    ffError(ret);
                break;
            }

            ret = swr_init(swr);
            if (ret < 0) {
                error =
                    "تهيئة resampler فشلت: " +
                    ffError(ret);
                break;
            }
        }

        {
            const AVRational guessedRate =
                av_guess_frame_rate(
                    input,
                    videoInput,
                    nullptr
                );

            const AVRational inputRate =
                guessedRate.num > 0 &&
                guessedRate.den > 0
                    ? guessedRate
                    : AVRational{30, 1};

            char bufferArgs[512];

            std::snprintf(
                bufferArgs,
                sizeof(bufferArgs),
                "video_size=%dx%d:pix_fmt=%d:"
                "time_base=%d/%d:pixel_aspect=%d/%d:"
                "frame_rate=%d/%d",
                videoDecoder->width,
                videoDecoder->height,
                videoDecoder->pix_fmt,
                videoDecoder->time_base.num,
                videoDecoder->time_base.den,
                videoDecoder->sample_aspect_ratio.num > 0
                    ? videoDecoder->sample_aspect_ratio.num
                    : 1,
                videoDecoder->sample_aspect_ratio.den > 0
                    ? videoDecoder->sample_aspect_ratio.den
                    : 1,
                inputRate.num,
                inputRate.den
            );

            graph = avfilter_graph_alloc();
            if (!graph) {
                error = "تعذر إنشاء filter graph";
                break;
            }

            const AVFilter* bufferFilter =
                avfilter_get_by_name("buffer");

            const AVFilter* sinkFilter =
                avfilter_get_by_name("buffersink");

            if (!bufferFilter || !sinkFilter) {
                error = "فلاتر الفيديو الأساسية غير موجودة";
                break;
            }

            ret = avfilter_graph_create_filter(
                &source,
                bufferFilter,
                "in",
                bufferArgs,
                nullptr,
                graph
            );

            if (ret < 0) {
                error =
                    "إنشاء مصدر الفيديو فشل: " +
                    ffError(ret);
                break;
            }

            ret = avfilter_graph_create_filter(
                &sink,
                sinkFilter,
                "out",
                nullptr,
                nullptr,
                graph
            );

            if (ret < 0) {
                error =
                    "إنشاء مخرج الفيديو فشل: " +
                    ffError(ret);
                break;
            }

            const int sinkFormats[] = {
                AV_PIX_FMT_YUV420P,
                AV_PIX_FMT_NONE
            };

            ret = av_opt_set_int_list(
                sink,
                "pix_fmts",
                sinkFormats,
                AV_PIX_FMT_NONE,
                AV_OPT_SEARCH_CHILDREN
            );

            if (ret < 0) {
                error = "ضبط صيغة الفيديو فشل";
                break;
            }

            std::string chain =
                "format=pix_fmts=yuv420p";

            if ((videoDecoder->width & 1) ||
                (videoDecoder->height & 1)) {
                chain +=
                    ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
            }

            if (videoDecoder->field_order !=
                    AV_FIELD_PROGRESSIVE &&
                videoDecoder->field_order !=
                    AV_FIELD_UNKNOWN) {
                chain += ",yadif=mode=send_frame";
            }

            chain +=
                ",minterpolate=fps=" +
                std::to_string(targetFps) +
                ":mi_mode=mci:mc_mode=aobmc:"
                "me_mode=bidir:me=epzs:vsbmc=1:"
                "scd=fdiff";

            AVFilterInOut* inputs =
                avfilter_inout_alloc();

            AVFilterInOut* outputs =
                avfilter_inout_alloc();

            if (!inputs || !outputs) {
                avfilter_inout_free(&inputs);
                avfilter_inout_free(&outputs);
                error = "تعذر إنشاء وصلات الفلاتر";
                break;
            }

            outputs->name = av_strdup("in");
            outputs->filter_ctx = source;
            outputs->pad_idx = 0;
            outputs->next = nullptr;

            inputs->name = av_strdup("out");
            inputs->filter_ctx = sink;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            ret = avfilter_graph_parse_ptr(
                graph,
                chain.c_str(),
                &inputs,
                &outputs,
                nullptr
            );

            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);

            if (ret < 0) {
                error =
                    "بناء فلتر interpolation فشل: " +
                    ffError(ret);
                break;
            }

            ret = avfilter_graph_config(
                graph,
                nullptr
            );

            if (ret < 0) {
                error =
                    "تهيئة فلتر interpolation فشلت: " +
                    ffError(ret);
                break;
            }
        }

        ret = avio_open(
            &output->pb,
            outputPath.c_str(),
            AVIO_FLAG_WRITE
        );

        if (ret < 0) {
            error =
                "فتح ملف MP4 فشل: " +
                ffError(ret);
            break;
        }

        ret = avformat_write_header(
            output,
            nullptr
        );

        if (ret < 0) {
            error =
                "كتابة رأس MP4 فشلت: " +
                ffError(ret);
            break;
        }

        packet = av_packet_alloc();
        videoFrame = av_frame_alloc();
        audioFrame = av_frame_alloc();
        convertedAudio = av_frame_alloc();

        if (!packet || !videoFrame ||
            !audioFrame || !convertedAudio) {
            error = "تعذر تخصيص ذاكرة التحويل";
            break;
        }

        int lastProgress = -1;

        while (!g_cancel.load() &&
               (ret = av_read_frame(
                    input,
                    packet
               )) >= 0) {

            if (packet->stream_index ==
                videoIndex) {

                ret = avcodec_send_packet(
                    videoDecoder,
                    packet
                );

                if (ret < 0) {
                    error =
                        "إرسال الفيديو للفك فشل: " +
                        ffError(ret);
                    break;
                }

                while (!g_cancel.load()) {
                    ret = avcodec_receive_frame(
                        videoDecoder,
                        videoFrame
                    );

                    if (ret == AVERROR(EAGAIN) ||
                        ret == AVERROR_EOF) {
                        ret = 0;
                        break;
                    }

                    if (ret < 0) {
                        error =
                            "فك الفيديو فشل: " +
                            ffError(ret);
                        break;
                    }

                    const int64_t frameTimestamp =
                        videoFrame->best_effort_timestamp;

                    ret = av_buffersrc_add_frame_flags(
                        source,
                        videoFrame,
                        AV_BUFFERSRC_FLAG_KEEP_REF
                    );

                    av_frame_unref(videoFrame);

                    if (ret < 0) {
                        error =
                            "إرسال الفيديو للفلاتر فشل: " +
                            ffError(ret);
                        break;
                    }

                    ret = drainVideo(
                        sink,
                        videoEncoder,
                        output,
                        output->streams[0]
                    );

                    if (ret < 0) {
                        error =
                            "ترميز الفيديو فشل: " +
                            ffError(ret);
                        break;
                    }

                    if (durationUs > 0 &&
                        frameTimestamp != AV_NOPTS_VALUE) {
                        const int64_t frameUs =
                            av_rescale_q(
                                frameTimestamp,
                                videoInput->time_base,
                                AVRational{1, 1000000}
                            );

                        const int percent =
                            static_cast<int>(
                                std::clamp<int64_t>(
                                    frameUs * 100 /
                                        durationUs,
                                    0,
                                    99
                                )
                            );

                        if (percent != lastProgress) {
                            progress(
                                env,
                                listener,
                                progressMethod,
                                percent
                            );
                            lastProgress = percent;
                        }
                    }
                }
            } else if (
                packet->stream_index == audioIndex &&
                audioDecoder &&
                audioEncoder &&
                audioOutput &&
                swr
            ) {
                ret = avcodec_send_packet(
                    audioDecoder,
                    packet
                );

                if (ret < 0) {
                    error =
                        "إرسال الصوت للفك فشل: " +
                        ffError(ret);
                    break;
                }

                while (!g_cancel.load()) {
                    ret = avcodec_receive_frame(
                        audioDecoder,
                        audioFrame
                    );

                    if (ret == AVERROR(EAGAIN) ||
                        ret == AVERROR_EOF) {
                        ret = 0;
                        break;
                    }

                    if (ret < 0) {
                        error =
                            "فك الصوت فشل: " +
                            ffError(ret);
                        break;
                    }

                    av_frame_unref(
                        convertedAudio
                    );

                    int64_t inputPts =
                        audioFrame->best_effort_timestamp;

                    if (inputPts == AV_NOPTS_VALUE) {
                        inputPts = 0;
                    }

                    const int64_t outputPts =
                        av_rescale_q(
                            inputPts,
                            input->streams[audioIndex]
                                ->time_base,
                            audioEncoder->time_base
                        );

                    ret = convertAudio(
                        audioFrame,
                        convertedAudio,
                        swr,
                        &audioEncoder->ch_layout,
                        audioEncoder->sample_rate,
                        audioEncoder->sample_fmt,
                        outputPts
                    );

                    if (ret < 0) {
                        error =
                            "تحويل الصوت فشل: " +
                            ffError(ret);
                        break;
                    }

                    ret = writeAudioPacket(
                        audioEncoder,
                        output,
                        audioOutput,
                        convertedAudio
                    );

                    if (ret < 0) {
                        error =
                            "ترميز الصوت فشل: " +
                            ffError(ret);
                        break;
                    }
                }
            }

            av_packet_unref(packet);

            if (ret < 0 || g_cancel.load()) {
                break;
            }
        }

        if (g_cancel.load()) {
            error = "تم إلغاء العملية";
            break;
        }

        if (ret != AVERROR_EOF && ret < 0) {
            error =
                "قراءة الفيديو فشلت: " +
                ffError(ret);
            break;
        }

        ret = avcodec_send_packet(
            videoDecoder,
            nullptr
        );

        if (ret < 0) {
            error = "إنهاء فك الفيديو فشل";
            break;
        }

        while (true) {
            ret = avcodec_receive_frame(
                videoDecoder,
                videoFrame
            );

            if (ret == AVERROR(EAGAIN) ||
                ret == AVERROR_EOF) {
                ret = 0;
                break;
            }

            if (ret < 0) break;

            av_buffersrc_add_frame_flags(
                source,
                videoFrame,
                AV_BUFFERSRC_FLAG_KEEP_REF
            );

            av_frame_unref(videoFrame);

            ret = drainVideo(
                sink,
                videoEncoder,
                output,
                output->streams[0]
            );

            if (ret < 0) break;
        }

        if (ret < 0) {
            error =
                "تفريغ فك الفيديو فشل: " +
                ffError(ret);
            break;
        }

        ret = av_buffersrc_add_frame_flags(
            source,
            nullptr,
            0
        );

        if (ret < 0) {
            error =
                "تعذر إنهاء فلتر interpolation: " +
                ffError(ret);
            break;
        }

        ret = drainVideo(
            sink,
            videoEncoder,
            output,
            output->streams[0]
        );

        if (ret < 0) {
            error =
                "تفريغ filter فشل: " +
                ffError(ret);
            break;
        }

        ret = writeVideoPacket(
            videoEncoder,
            output,
            output->streams[0],
            nullptr
        );

        if (ret < 0) {
            error =
                "تفريغ encoder الفيديو فشل: " +
                ffError(ret);
            break;
        }

        if (audioDecoder &&
            audioEncoder &&
            audioOutput) {

            ret = avcodec_send_packet(
                audioDecoder,
                nullptr
            );

            if (ret >= 0) {
                while (true) {
                    ret = avcodec_receive_frame(
                        audioDecoder,
                        audioFrame
                    );

                    if (ret == AVERROR(EAGAIN) ||
                        ret == AVERROR_EOF) {
                        ret = 0;
                        break;
                    }

                    if (ret < 0) break;

                    av_frame_unref(
                        convertedAudio
                    );

                    int64_t inputPts =
                        audioFrame->best_effort_timestamp;

                    if (inputPts == AV_NOPTS_VALUE) {
                        inputPts = 0;
                    }

                    const int64_t outputPts =
                        av_rescale_q(
                            inputPts,
                            input->streams[audioIndex]
                                ->time_base,
                            audioEncoder->time_base
                        );

                    ret = convertAudio(
                        audioFrame,
                        convertedAudio,
                        swr,
                        &audioEncoder->ch_layout,
                        audioEncoder->sample_rate,
                        audioEncoder->sample_fmt,
                        outputPts
                    );

                    if (ret < 0) break;

                    ret = writeAudioPacket(
                        audioEncoder,
                        output,
                        audioOutput,
                        convertedAudio
                    );

                    if (ret < 0) break;
                }
            }

            ret = writeAudioPacket(
                audioEncoder,
                output,
                audioOutput,
                nullptr
            );

            if (ret < 0) {
                error =
                    "تفريغ encoder الصوت فشل: " +
                    ffError(ret);
                break;
            }
        }

        ret = av_write_trailer(output);

        if (ret < 0) {
            error =
                "إنهاء MP4 فشل: " +
                ffError(ret);
            break;
        }

        progress(
            env,
            listener,
            progressMethod,
            100
        );

        error.clear();
    } while (false);

    if (packet) av_packet_free(&packet);
    if (videoFrame) av_frame_free(&videoFrame);
    if (audioFrame) av_frame_free(&audioFrame);
    if (convertedAudio) av_frame_free(&convertedAudio);

    if (swr) swr_free(&swr);
    if (graph) avfilter_graph_free(&graph);

    if (output) {
        if (output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
    }

    if (videoEncoder) avcodec_free_context(&videoEncoder);
    if (audioEncoder) avcodec_free_context(&audioEncoder);
    if (videoDecoder) avcodec_free_context(&videoDecoder);
    if (audioDecoder) avcodec_free_context(&audioDecoder);

    if (input) avformat_close_input(&input);

    close(inputFd);
    close(outputFd);

    return error;
}
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_hyouka_videofps_FpsProcessor_process(
    JNIEnv* env,
    jobject,
    jint inputFd,
    jint outputFd,
    jint targetFps,
    jlong durationUs,
    jobject listener
) {
    if (targetFps != 60 &&
        targetFps != 90 &&
        targetFps != 120) {
        return env->NewStringUTF("FPS غير مدعوم");
    }

    if (inputFd < 0 || outputFd < 0) {
        return env->NewStringUTF(
            "ملف الإدخال أو الإخراج غير صالح"
        );
    }

    if (!listener) {
        return env->NewStringUTF(
            "مستمع التقدم غير صالح"
        );
    }

    const std::string error = process(
        inputFd,
        outputFd,
        targetFps,
        durationUs,
        env,
        listener
    );

    if (error.empty()) return nullptr;

    return env->NewStringUTF(error.c_str());
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hyouka_videofps_FpsProcessor_cancel(
    JNIEnv*,
    jobject
) {
    g_cancel.store(true);
}
