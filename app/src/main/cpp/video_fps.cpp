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
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
}

namespace {

std::atomic_bool g_cancel{false};

std::string ffError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
}

void reportProgress(
    JNIEnv* env,
    jobject listener,
    jmethodID method,
    int value
) {
    if (!env || !listener || !method) return;

    value = std::clamp(value, 0, 100);
    env->CallVoidMethod(listener, method, static_cast<jint>(value));

    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

int writeEncodedVideo(
    AVCodecContext* encoder,
    AVFormatContext* output,
    AVStream* outputStream,
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

        packet->stream_index = outputStream->index;

        av_packet_rescale_ts(
            packet,
            encoder->time_base,
            outputStream->time_base
        );

        ret = av_interleaved_write_frame(output, packet);

        av_packet_unref(packet);

        if (ret < 0) break;
    }

    av_packet_free(&packet);
    return ret;
}

int drainFilter(
    AVFilterContext* sink,
    AVCodecContext* encoder,
    AVFormatContext* output,
    AVStream* outputStream
) {
    const AVRational sinkTimeBase =
        av_buffersink_get_time_base(sink);

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

        ret = writeEncodedVideo(
            encoder,
            output,
            outputStream,
            frame
        );

        av_frame_unref(frame);

        if (ret < 0) break;
    }

    av_frame_free(&frame);
    return ret;
}

std::string processVideo(
    int inputFd,
    int outputFd,
    int targetFps,
    int64_t durationUs,
    JNIEnv* env,
    jobject listener
) {
    g_cancel.store(false);

    jclass listenerClass =
        env->FindClass(
            "com/hyouka/videofps/ProgressListener"
        );

    if (!listenerClass) {
        env->ExceptionClear();
        return "تعذر تجهيز مستمع التقدم";
    }

    jmethodID progressMethod =
        env->GetMethodID(
            listenerClass,
            "onProgress",
            "(I)V"
        );

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

    AVCodecContext* decoder = nullptr;
    AVCodecContext* encoder = nullptr;

    AVFilterGraph* graph = nullptr;
    AVFilterContext* source = nullptr;
    AVFilterContext* sink = nullptr;

    AVPacket* packet = nullptr;
    AVFrame* decodedFrame = nullptr;

    int videoInputIndex = -1;
    int audioInputIndex = -1;

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
            error =
                "قراءة معلومات الفيديو فشلت: " +
                ffError(ret);
            break;
        }

        for (unsigned i = 0; i < input->nb_streams; ++i) {
            const AVCodecParameters* params =
                input->streams[i]->codecpar;

            if (params->codec_type == AVMEDIA_TYPE_VIDEO &&
                videoInputIndex < 0) {
                videoInputIndex = static_cast<int>(i);
            }

            if (params->codec_type == AVMEDIA_TYPE_AUDIO &&
                audioInputIndex < 0) {
                audioInputIndex = static_cast<int>(i);
            }
        }

        if (videoInputIndex < 0) {
            error = "لم يتم العثور على مسار فيديو";
            break;
        }

        AVStream* inputVideo =
            input->streams[videoInputIndex];

        const AVCodec* decoderCodec =
            avcodec_find_decoder(
                inputVideo->codecpar->codec_id
            );

        if (!decoderCodec) {
            error = "ترميز الفيديو غير مدعوم";
            break;
        }

        decoder =
            avcodec_alloc_context3(decoderCodec);

        if (!decoder) {
            error = "تعذر إنشاء مفكك الفيديو";
            break;
        }

        ret = avcodec_parameters_to_context(
            decoder,
            inputVideo->codecpar
        );

        if (ret < 0) {
            error =
                "تهيئة مفكك الفيديو فشلت: " +
                ffError(ret);
            break;
        }

        decoder->thread_count = 0;

        ret = avcodec_open2(
            decoder,
            decoderCodec,
            nullptr
        );

        if (ret < 0) {
            error =
                "فتح مفكك الفيديو فشل: " +
                ffError(ret);
            break;
        }

        ret = avformat_alloc_output_context2(
            &output,
            nullptr,
            "mp4",
            outputPath.c_str()
        );

        if (ret < 0 || !output) {
            error =
                "تعذر إنشاء MP4: " +
                ffError(ret);
            break;
        }

        const AVCodec* encoderCodec =
            avcodec_find_encoder_by_name(
                "libopenh264"
            );

        if (!encoderCodec) {
            error = "OpenH264 encoder غير موجود";
            break;
        }

        encoder =
            avcodec_alloc_context3(encoderCodec);

        if (!encoder) {
            error = "تعذر إنشاء encoder الفيديو";
            break;
        }

        int width = decoder->width;
        int height = decoder->height;

        if (width & 1) --width;
        if (height & 1) --height;

        if (width <= 0 || height <= 0 ||
            width > 3840 || height > 2160) {
            error =
                "الدقة غير مناسبة. الحد الأقصى 3840 × 2160";
            break;
        }

        encoder->codec_type = AVMEDIA_TYPE_VIDEO;
        encoder->codec_id = AV_CODEC_ID_H264;
        encoder->width = width;
        encoder->height = height;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = AVRational{1, targetFps};
        encoder->framerate = AVRational{targetFps, 1};
        encoder->gop_size = targetFps * 2;
        encoder->max_b_frames = 0;

        const int64_t estimatedBitrate =
            static_cast<int64_t>(width) *
            height *
            targetFps *
            6LL;

        encoder->bit_rate = std::clamp<int64_t>(
            estimatedBitrate,
            4000000LL,
            80000000LL
        );

        if (output->oformat->flags &
            AVFMT_GLOBALHEADER) {
            encoder->flags |=
                AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        ret = avcodec_open2(
            encoder,
            encoderCodec,
            nullptr
        );

        if (ret < 0) {
            error =
                "فتح OpenH264 فشل: " +
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
            encoder->time_base;

        ret = avcodec_parameters_from_context(
            videoOutput->codecpar,
            encoder
        );

        if (ret < 0) {
            error =
                "تعذر تجهيز إعدادات الفيديو";
            break;
        }

        videoOutput->codecpar->codec_tag = 0;

        AVStream* audioOutput = nullptr;

        if (audioInputIndex >= 0) {
            AVStream* inputAudio =
                input->streams[audioInputIndex];

            audioOutput =
                avformat_new_stream(output, nullptr);

            if (!audioOutput) {
                error = "تعذر إنشاء مسار الصوت";
                break;
            }

            ret = avcodec_parameters_copy(
                audioOutput->codecpar,
                inputAudio->codecpar
            );

            if (ret < 0) {
                error =
                    "تعذر نسخ إعدادات الصوت: " +
                    ffError(ret);
                break;
            }

            audioOutput->time_base =
                inputAudio->time_base;

            audioOutput->codecpar->codec_tag = 0;
        }

        {
            const AVRational guessedRate =
                av_guess_frame_rate(
                    input,
                    inputVideo,
                    nullptr
                );

            const AVRational inputRate =
                (guessedRate.num > 0 &&
                 guessedRate.den > 0)
                    ? guessedRate
                    : AVRational{30, 1};

            char bufferArgs[512];

            const AVRational pixelAspect =
                (decoder->sample_aspect_ratio.num > 0 &&
                 decoder->sample_aspect_ratio.den > 0)
                    ? decoder->sample_aspect_ratio
                    : AVRational{1, 1};

            std::snprintf(
                bufferArgs,
                sizeof(bufferArgs),
                "video_size=%dx%d:"
                "pix_fmt=%d:"
                "time_base=%d/%d:"
                "pixel_aspect=%d/%d:"
                "frame_rate=%d/%d",
                decoder->width,
                decoder->height,
                decoder->pix_fmt,
                inputVideo->time_base.num,
                inputVideo->time_base.den,
                pixelAspect.num,
                pixelAspect.den,
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
                error =
                    "فلاتر الفيديو الأساسية غير موجودة";
                break;
            }

            ret = avfilter_graph_create_filter(
                &source,
                bufferFilter,
                "source",
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
                "sink",
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

            std::string filterChain =
                "format=pix_fmts=yuv420p";

            if ((decoder->width & 1) ||
                (decoder->height & 1)) {
                filterChain +=
                    ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
            }

            if (decoder->field_order !=
                    AV_FIELD_PROGRESSIVE &&
                decoder->field_order !=
                    AV_FIELD_UNKNOWN) {
                filterChain +=
                    ",yadif=mode=send_frame";
            }

            filterChain +=
                ",minterpolate=fps=" +
                std::to_string(targetFps) +
                ":mi_mode=mci:"
                "mc_mode=aobmc:"
                "me_mode=bidir:"
                "me=epzs:"
                "vsbmc=1:"
                "scd=fdiff";

            AVFilterInOut* inputs =
                avfilter_inout_alloc();

            AVFilterInOut* outputs =
                avfilter_inout_alloc();

            if (!inputs || !outputs) {
                avfilter_inout_free(&inputs);
                avfilter_inout_free(&outputs);
                error =
                    "تعذر إنشاء وصلات الفلاتر";
                break;
            }

            inputs->name = av_strdup("out");
            inputs->filter_ctx = sink;
            inputs->pad_idx = 0;
            inputs->next = nullptr;

            outputs->name = av_strdup("in");
            outputs->filter_ctx = source;
            outputs->pad_idx = 0;
            outputs->next = nullptr;

            ret = avfilter_graph_parse_ptr(
                graph,
                filterChain.c_str(),
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
        decodedFrame = av_frame_alloc();

        if (!packet || !decodedFrame) {
            error =
                "تعذر تخصيص ذاكرة التحويل";
            break;
        }

        int lastProgress = -1;

        while (!g_cancel.load() &&
               (ret = av_read_frame(
                    input,
                    packet
               )) >= 0) {

            if (packet->stream_index ==
                videoInputIndex) {

                ret = avcodec_send_packet(
                    decoder,
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
                        decoder,
                        decodedFrame
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

                    const int64_t timestamp =
                        decodedFrame->best_effort_timestamp;

                    ret = av_buffersrc_add_frame_flags(
                        source,
                        decodedFrame,
                        AV_BUFFERSRC_FLAG_KEEP_REF
                    );

                    av_frame_unref(decodedFrame);

                    if (ret < 0) {
                        error =
                            "إرسال الفيديو للفلاتر فشل: " +
                            ffError(ret);
                        break;
                    }

                    ret = drainFilter(
                        sink,
                        encoder,
                        output,
                        videoOutput
                    );

                    if (ret < 0) {
                        error =
                            "ترميز الفيديو فشل: " +
                            ffError(ret);
                        break;
                    }

                    if (durationUs > 0 &&
                        timestamp != AV_NOPTS_VALUE) {
                        const int64_t timestampUs =
                            av_rescale_q(
                                timestamp,
                                inputVideo->time_base,
                                AVRational{1, 1000000}
                            );

                        const int percent =
                            static_cast<int>(
                                std::clamp<int64_t>(
                                    timestampUs * 100 /
                                        durationUs,
                                    0,
                                    99
                                )
                            );

                        if (percent != lastProgress) {
                            reportProgress(
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
                audioOutput &&
                packet->stream_index ==
                    audioInputIndex
            ) {
                AVPacket* audioPacket =
                    av_packet_clone(packet);

                if (!audioPacket) {
                    error = "تعذر نسخ إطار الصوت";
                    break;
                }

                audioPacket->stream_index =
                    audioOutput->index;

                av_packet_rescale_ts(
                    audioPacket,
                    input->streams[audioInputIndex]
                        ->time_base,
                    audioOutput->time_base
                );

                ret = av_interleaved_write_frame(
                    output,
                    audioPacket
                );

                av_packet_free(&audioPacket);

                if (ret < 0) {
                    error =
                        "كتابة الصوت فشلت: " +
                        ffError(ret);
                    break;
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
            decoder,
            nullptr
        );

        if (ret < 0) {
            error =
                "إنهاء فك الفيديو فشل: " +
                ffError(ret);
            break;
        }

        while (true) {
            ret = avcodec_receive_frame(
                decoder,
                decodedFrame
            );

            if (ret == AVERROR(EAGAIN) ||
                ret == AVERROR_EOF) {
                ret = 0;
                break;
            }

            if (ret < 0) {
                error =
                    "تفريغ مفكك الفيديو فشل: " +
                    ffError(ret);
                break;
            }

            ret = av_buffersrc_add_frame_flags(
                source,
                decodedFrame,
                AV_BUFFERSRC_FLAG_KEEP_REF
            );

            av_frame_unref(decodedFrame);

            if (ret < 0) break;

            ret = drainFilter(
                sink,
                encoder,
                output,
                videoOutput
            );

            if (ret < 0) break;
        }

        if (ret < 0) {
            error =
                "تفريغ الفيديو فشل: " +
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
                "إنهاء filter فشل: " +
                ffError(ret);
            break;
        }

        ret = drainFilter(
            sink,
            encoder,
            output,
            videoOutput
        );

        if (ret < 0) {
            error =
                "تفريغ filter فشل: " +
                ffError(ret);
            break;
        }

        ret = writeEncodedVideo(
            encoder,
            output,
            videoOutput,
            nullptr
        );

        if (ret < 0) {
            error =
                "إنهاء encoder الفيديو فشل: " +
                ffError(ret);
            break;
        }

        ret = av_write_trailer(output);

        if (ret < 0) {
            error =
                "إنهاء MP4 فشل: " +
                ffError(ret);
            break;
        }

        reportProgress(
            env,
            listener,
            progressMethod,
            100
        );

        error.clear();
    } while (false);

    if (packet) av_packet_free(&packet);
    if (decodedFrame) av_frame_free(&decodedFrame);

    if (graph) avfilter_graph_free(&graph);

    if (output) {
        if (output->pb) avio_closep(&output->pb);
        avformat_free_context(output);
    }

    if (encoder) avcodec_free_context(&encoder);
    if (decoder) avcodec_free_context(&decoder);

    if (input) avformat_close_input(&input);

    close(inputFd);
    close(outputFd);

    return error;
}

}  // namespace

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
        return env->NewStringUTF(
            "FPS غير مدعوم"
        );
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

    const std::string error =
        processVideo(
            inputFd,
            outputFd,
            targetFps,
            durationUs,
            env,
            listener
        );

    if (error.empty()) {
        return nullptr;
    }

    return env->NewStringUTF(
        error.c_str()
    );
}

extern "C"
JNIEXPORT void JNICALL
Java_com_hyouka_videofps_FpsProcessor_cancel(
    JNIEnv*,
    jobject
) {
    g_cancel.store(true);
}
