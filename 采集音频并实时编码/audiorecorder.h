#define _CRT_SECURE_NO_WARNINGS
#define SDL_MAIN_HANDLED
#pragma once
#include <iostream>
#include <atomic>
#include <condition_variable>
#include <string>
#include <thread>
#include <windows.h>
#include <vector>

extern "C"
{
#include <libavutil/frame.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#include <SDL.h>
}

extern "C"
{
    struct AVFormatContext;
    struct AVCodecContext;
    struct AVCodec;
    struct AVAudioFifo;
    struct AVFrame;
    struct SwrContext;
};



class AudioRecorder
{
private:
    enum RecordState {
        NotStarted,
        Started,
        Paused,
        Stopped,
        Unknown,
    };

public:
    std::string WideCharToUtf8(const std::wstring& wstr);
    explicit AudioRecorder();
    void Init();

public:
    void Start();

    void sdl_event_work();

    void Stop();

private:
    //从fifobuf读取音视频帧，写入输出流，复用，生成文件
    void MuxThreadProc();

    //从音频输入流读取帧，写入fifobuf
    void SoundRecordThreadProc();

    /// 分配音频帧：根据采样率、采样格式、声道数、采样点数
    AVFrame* AllocAudioFrame(AVCodecContext* c, int nbSamples);


    int OpenAudio();
    int OpenOutput();

    /// 初始化音频共享队列
    int InitAudioBuffer();

    void FlushAudioDecoder();
    //void FlushVideoEncoder();
    //void FlushAudioEncoder();
    void FlushEncoders();
    void Release();
private:
    std::string				m_filePath;
    int					m_audioBitrate;

    int m_aIndex;		//输入音频流索引
    int m_aOutIndex;	//输出音频流索引

    AVFormatContext* m_aFmtCtx; ///输入流的上下文参数
    AVFormatContext* m_oFmtCtx; ///输出流的上下文参数

    AVCodecContext* m_aDecodeCtx;/// 解麦克风
    AVCodecContext* m_aEncodeCtx;/// 编重采样后的pcm
    SwrContext* m_swrCtx;
    AVAudioFifo* m_aFifoBuf;

    AVFrame* m_vOutFrame;


    int					m_nbSamples;
    std::atomic<RecordState>			m_state;
    /// C++2.0： 课程（c++11/14/17/20)
    /// std::condition_variable m_cvNotPause;	//当点击暂停的时候，两个采集线程挂起
    /// std::mutex				m_mtxPause;

    /// 共享队列的 同步机制：条件量和互斥体
    std::condition_variable m_cvABufNotFull;    /// “不满”
    std::condition_variable m_cvABufNotEmpty;   /// “不空”
    std::mutex				m_mtxABuf; //AudioFifo的互斥体
    int64_t					m_aCurPts;
    
    std::vector<std::thread> Threads;
};
