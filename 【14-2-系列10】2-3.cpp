#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <windows.h>
#include <codecvt>
#include <locale>
#include <thread>
#include <atomic>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <SDL.h>
}

//捕获音频
#define MAX_AUDIO_FRAME_SIZE 192000
#define AV_CH_LAYOUT_STEREO     (AV_CH_FRONT_LEFT|AV_CH_FRONT_RIGHT)
std::atomic<bool> _running = true;

std::string WideCharToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
        NULL, 0, NULL, NULL);
    std::string result(sizeNeeded - 1, 0); // -1 去掉 null terminator
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
        &result[0], sizeNeeded, NULL, NULL);
    return result;
}

/*
* 1、这个事件处理函数，也就是 SDL 专属的这样一套机制，是基于窗口来处理事件的，大多数情况下，必须在窗口下进行，
* 例如：通过 q 键（英文）来退出录音，必须是“英文输入法”+“窗口置顶”，否则检测不到，但是ctrl+c，强制退出程序的话，不要窗口，SDL_QUIT 也能检测到
* 这个事件！！！
*/
static int sdl_event_work(void *data)
{

    SDL_Window* window = SDL_CreateWindow("Invisible Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    //SDL_ShowWindow(window); 2、这里不用这句也行

    do {
        SDL_Event event;
        SDL_WaitEvent(&event); // 这个函数会从事件队列中取事件来填充 event,直接阻塞接收

        switch (event.type)
        {
        case SDL_QUIT: // 这个强制终止程序也会“(间接)触发”这个事件，SDL_QUIT，不只是SDL创建出来的窗口的关闭键！！！
            SDL_Log("the Ctrl + C key is pressed!\n");
            _running = false;
            break;
        case SDL_KEYDOWN:
            SDL_Log("the key is: q --> quit\n");
            if (event.key.keysym.sym == SDLK_q) _running = false;
            break;
        default:
            SDL_Log("the event of which not to work:%d\n", event.type);
            break;
        }

    } while (_running);

    //这里采用循环，方便后续功能的添加
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

static int audioCapture()
{
    int ret = -1;

    AVFormatContext* avFmtCtx = nullptr;
    const AVInputFormat* avInputFmt = nullptr;
    AVCodecContext* avCodecCtx = nullptr;
    const AVCodec* avCodec = nullptr;

    /// register devices: audio, video
    avdevice_register_all();
    int audioStreamIndex = -1;

    /// codec . paramters
    int sampleRate = 0;
    int bitRate = 0;
    int bitPerSample = 0;
    int channels = 0;
    int sampleFmt = 0;

    /* 我本地
    audioStreamIndex=0
    codec_id=65536
    sampleRate=44100
    bitRate=1411200
    bitPerSample=16
    channels=2
    sampleFmt=1
    */
    FILE* fpPCM = nullptr;
    fpPCM = fopen("./new/1.pcm", "wb");
    if (!fpPCM)
    {
        av_log(NULL, AV_LOG_ERROR, "failed to open the file:%s\n", "./new/1.pcm");
        return -1;
    }

    /// open microphone
    do {
        /// 1.设置格式dshow
        avInputFmt = av_find_input_format("dshow");
        if (!avInputFmt) {
            break;
        }


        avFmtCtx = avformat_alloc_context();
        ///// 必须要是utf编码
        /// qt5： toUtf8
        /// 改为你本地的 麦克风名称
        std::wstring strName = L"audio=麦克风 (2- Realtek(R) Audio)";
        std::string mkname = WideCharToUtf8(strName);
        if (mkname == "")
        {
            av_log(NULL, AV_LOG_ERROR, "the utf-8 is failed!\n");
            return -1;
        }

        /// 2.打开设备
        ret = avformat_open_input(&avFmtCtx,
            mkname.c_str(), ///utf8
            avInputFmt, NULL);
        if (ret < 0) {
            break;
        }

        /// 调用此api之后，avFmtCtx参数已经很“充分”了
        /// 3.进一步获取流的详细信息
        ret = avformat_find_stream_info(avFmtCtx, NULL);
        if (ret < 0) {
            break;
        }


        /// 查找 音频流的 索引
        for (int i = 0; i < avFmtCtx->nb_streams; i++) {
            if (avFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIndex = i;
                break;
            }
        }
        av_log(NULL, AV_LOG_INFO, "audioStreamIndex = %d\n", audioStreamIndex);

        /// 打开解码器： 麦克风（pcm）， 也需要解码
        ///我本地麦克风： s16le_44100
        /// 4.根据麦克风的PCM编码id，来查找对应的 解码器

        avCodec = avcodec_find_decoder(avFmtCtx->streams[audioStreamIndex]->codecpar->codec_id); //decoder
        if (!avCodec) {
            av_log(NULL, AV_LOG_ERROR, "avCodec is failure! the avFmtCtx->audio_codec_id:\n", avFmtCtx->streams[audioStreamIndex]->codecpar->codec_id);
            return -1;
        }

        avCodecCtx = avcodec_alloc_context3(avCodec);
        if (!avCodecCtx)
        {
            av_log(NULL, AV_LOG_ERROR, "the avCodecCtx of parameters is empty!\n");
            return -1;
        }

        ret = avcodec_parameters_to_context(avCodecCtx, avFmtCtx->streams[audioStreamIndex]->codecpar);
        if (ret < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "the avCodecCtx of parameters is empty!\n");
            return -1;
        }
        /// AV_CODEC_ID_PCM_S16LE = 0x10000,
        av_log(avCodecCtx, AV_LOG_INFO, "codec_id=%d\n", avCodecCtx->codec_id);///codec_id=65536

        /// open decoder
        /// 5. 打开解码器
        ret = avcodec_open2(avCodecCtx, avCodec, NULL);
        if (ret != 0) {
            break;
        }


        sampleRate = avCodecCtx->sample_rate; //44100
        bitRate = avCodecCtx->bit_rate; //1411200
        bitPerSample = avCodecCtx->bits_per_coded_sample; //16
        channels = avCodecCtx->ch_layout.nb_channels; //2
        sampleFmt = avCodecCtx->sample_fmt; ///AV_SAMPLE_FMT_S16

        printf("sampleRate=%d\n", sampleRate);
        printf("bitRate=%d\n", bitRate);
        printf("bitPerSample=%d\n", bitPerSample);
        printf("channels=%d\n", channels);
        printf("sampleFmt=%d\n", sampleFmt);


    } while (0);


    /// 6. 开始循环，读取麦克风的 “音频帧”
    /// AVPacket： 压缩的包(264,aac,mp3, 265, pcm(s16le-44100-ch2), yuv422)
    /// AVFrame：  原始的帧(yuv, pcm(aac: pcm-->fltp), yuv420p)
    /// av_read_frame(......)：读出来的是AVPacket
    /// 解码： avcodec_send_packet(...)
    /// avcodec_receive_frame(...)，
    /// 可以直接存储为pcm文件，用ffplay播放
    /// 也可以重采样，专为fltp，然后送给AAC的编码器，此时VLC就可以播放
    /// “拿来主义”：不要  “抄来主义”


    //可以把此函数封装为CPP类，_running作为成员变量，增加Stop()函数，注意线程同步机制
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();


    /// 重采样
    //init resample
    /// s32le-48000-2
    int output_channels = 2;
    int output_rate = 48000;
    int input_channels = avCodecCtx->ch_layout.nb_channels;
    int input_rate = avCodecCtx->sample_rate;
    AVSampleFormat input_sample_fmt = avCodecCtx->sample_fmt;
    AVSampleFormat output_sample_fmt = AV_SAMPLE_FMT_S32; ///AV_SAMPLE_FMT_FLTP
    printf("channels[%d=>%d],rate[%d=>%d],sample_fmt[%d=>%d]\n",
        input_channels, output_channels,
        input_rate, output_rate,
        input_sample_fmt, output_sample_fmt);

    SwrContext* resample_ctx = nullptr;

    //这个AVChannelLayout数据结构中存放的是：频道的布局信息
    AVChannelLayout in_ch_layout, out_ch_layout;
    av_channel_layout_default(&in_ch_layout, input_channels);
    av_channel_layout_default(&out_ch_layout, output_channels);

    ret = swr_alloc_set_opts2(&resample_ctx,
        &out_ch_layout, output_sample_fmt, output_rate,
        &in_ch_layout, input_sample_fmt, input_rate,
        0, NULL);


    if (!resample_ctx) {
        av_log(NULL, AV_LOG_ERROR, "av_audio_resample_init fail!!!\n");
        return -1;
    }
    swr_init(resample_ctx);

    int size = 0;
    uint8_t* out_buffer = (uint8_t*)av_malloc(MAX_AUDIO_FRAME_SIZE);

    while (_running) {
        /// 每次都要初始化包
        ret = av_read_frame(avFmtCtx, packet);
        if (ret < 0) {
            printf("av_read_frame return < 0, error\n");
            break;
        }

        //判断是否 音频包
        if (packet->stream_index == audioStreamIndex) {
            /// 将收到的麦克风的AVPacket，发送给 ffmpeg自带的解码器
            /// 264：libx264,openh264：第三方的编码器
            ret = avcodec_send_packet(avCodecCtx, packet);
            if (ret < 0) {
                printf("avcodec_send_packet return < 0, error\n");
                break;
            }

            while (ret >= 0)
            {
                /*
                    Note that the function will always call
                    * av_frame_unref(frame) before doing anything else.
                    * reference:
                    * un.reference:
                    * 引用计数机制：
                */
                ret = avcodec_receive_frame(avCodecCtx, frame);
                if (ret == AVERROR(EAGAIN)) {
                    av_log(NULL, AV_LOG_WARNING, "avcodec_receive_frame EAGAIN, error\n");
                    break;
                }
                if (ret == AVERROR_EOF) {
                    av_log(NULL, AV_LOG_WARNING, "avcodec_receive_frame AVERROR_EOF OKOk\n");
                    break;
                }
                /// frame->nb_samples : 采样点的个数
                int frameBytes = frame->nb_samples *
                    av_get_bytes_per_sample((AVSampleFormat)frame->format);
                //s16le: 16bits, 2Bytes
                //s32le: 32bits, 4Bytes

        /// 将pcm写入文件
        /// 只针对packed模式，
        /// 如果是planar模式的，剩下的问题请参考系列9：pcm重采样
        /// 效果比价差，尽量重采样
//                fwrite(frame->data[0], 1, frameBytes, fpPCM);
//                printf("frameBytes=%d, nb_samples=%d\n",
//                       frameBytes, frame->nb_samples);

                /// 重采样
                memset(out_buffer, 0, sizeof(out_buffer));
                int out_samples = swr_convert(resample_ctx,
                    &out_buffer,
                    frame->nb_samples,
                    (const uint8_t**)frame->data,
                    frame->nb_samples);
                if (out_samples > 0) {
                    size = av_samples_get_buffer_size(NULL, output_channels, out_samples, output_sample_fmt, 1);//out_samples*output_channels*av_get_bytes_per_sample(output_sample_fmt);
                    /// 这个存储只支持打包模式，如果是planar模式，请参考系列9的内容
                    fwrite(out_buffer, 1, size, fpPCM);
                }

                /// 释放
                av_frame_unref(frame);
                //need to do this?
                //avcodec_receive_frame said will call unref before receive
            }

        }
        /// 这个必须要手工调用
        /// The packet must be freed with av_packet_unref() when
        /// * it is no longer needed.
        av_packet_unref(packet);

    }

    printf("release delete ...... \n");
    if (avFmtCtx) {
        avformat_free_context(avFmtCtx);
        avFmtCtx = nullptr;
    }

    if (fpPCM) {
        fclose(fpPCM);
        fpPCM = nullptr;
    }

    av_freep(out_buffer);

    printf("over over\n");
}

int main(int argc, char* argv[])
{
    //    QCoreApplication a(argc, argv);

    //    return a.exec();

    //添加事件循环
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    SDL_CreateThread(sdl_event_work, "SDL_EVENT", NULL);

    if (audioCapture() < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "the audioCapture() failed!\n");
        return -1;
    }

    SDL_Quit();

    return 0;
}
/*
* 1、为什么 Ctrl+C 能直接触发退出（SDL_QUIT）？
这是 控制台信号（SIGINT）触发的，不是 SDL 捕获的键盘事件。

SDL 有个默认行为：在 SIGINT 发生时（比如 Ctrl+C），会往事件队列里投一个 SDL_QUIT 事件。

所以即使没有窗口，也可以响应 SDL_QUIT，但那是 系统信号机制，不是 SDL 的输入事件系统。


* 2、因为创建窗口时，使用了flag：SDL_WINDOW_SHOWN ,所以不用显示的调用 API SDL_ShowWindow();
* 
* 
*/
