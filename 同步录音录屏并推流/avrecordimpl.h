#define _CRT_SECURE_NO_WARNINGS
#define SDL_MAIN_HANDLED
#pragma once
#include <iostream>
#include <string>
#include <windows.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
extern"C"
{
#include <SDL.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/log.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/fifo.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>

}

class AVRecordImpl
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
    AVRecordImpl();
    void Init();

public:
    void Start();
    void Pause();
    void Stop();

private:
    //从fifobuf读取音视频帧，写入输出流，复用，生成文件
    void MuxThreadProc();
    //从视频输入流读取帧，写入fifobuf
    void ScreenRecordThreadProc();
    //从音频输入流读取帧，写入fifobuf
    void SoundRecordThreadProc();
    int OpenVideo();
    int OpenAudio();

    ///// yuv的编码封装输出的 初始化工作
    int OpenOutput();
    std::string GetSpeakerDeviceName();
    //获取麦克风设备名称
    std::string GetMicrophoneDeviceName();
    AVFrame* AllocAudioFrame(AVCodecContext* c, int nbSamples);
    int InitVideoBuffer();
    int InitAudioBuffer();
    void FlushVideoDecoder();
    void FlushAudioDecoder();
    //void FlushVideoEncoder();
    //void FlushAudioEncoder();
    void FlushEncoders();
    void Release();

    void sdl_event_work();

    std::string WideCharToUtf8(const std::wstring& wstr);
    void PrintAVCodecContext(const AVCodecContext* ctx);

private:
    std::string			m_filePath;
    int					m_width;
    int					m_height;
    int					m_fps;
    int					m_audioBitrate;
    int					m_offsetx;
    int					m_offsety;


    int m_vIndex;		//输入视频流索引
    int m_aIndex;		//输入音频流索引
    int m_vOutIndex;	//输出视频流索引
    int m_aOutIndex;	//输出音频流索引
    AVFormatContext* m_vFmtCtx;
    AVFormatContext* m_aFmtCtx;
    AVFormatContext* m_oFmtCtx;
    AVCodecContext* m_vDecodeCtx;  ///解码器上下文参数
    AVCodecContext* m_aDecodeCtx;
    AVCodecContext* m_vEncodeCtx;
    AVCodecContext* m_aEncodeCtx;
    SwsContext* m_swsCtx;  ///// 视频颜色空间转换、缩放操作
    SwrContext* m_swrCtx;  ///// 音频重采样
    AVFifo* m_vFifoBuf;    /// 视频共享队列  AVFifoBuffer* 这个结构体暂时没用，因为在视频存储时，用不了，很多函数被否决了
    AVAudioFifo* m_aFifoBuf;    /// 音频共享队列

    AVFrame* m_vOutFrame;
    uint8_t* m_vOutFrameBuf;
    int					m_vOutFrameSize;

    int					m_nbSamples;
    std::atomic<RecordState>			m_state;
    std::condition_variable m_cvNotPause;	//当点击暂停的时候，两个采集线程挂起
    std::mutex				m_mtxPause;

    /// 视频共享队列的保护机制：Mutex,Condition_Variable
    /// 生产者消费者模式：
    /// “不满”：我是消费者，如果我消费了一个，肯定队列就“不满”
    /// “不空”：我是生产者，如果我生成了一个，肯定队列就“不空”
    /// Ｃ＋＋１１的课程： C++2.0(C++11,14,17,20)
    std::condition_variable m_cvVBufNotFull;
    std::condition_variable m_cvVBufNotEmpty;
    std::mutex				m_mtxVBuf;


    std::condition_variable m_cvABufNotFull;
    std::condition_variable m_cvABufNotEmpty;
    std::mutex				m_mtxABuf;
    int64_t				m_vCurPts;
    int64_t			    m_aCurPts;
    int64_t             start_time; //用于（重新设置视频pts）的录屏开始时间，一般在av_read_frame时开始录制，不是avformat_open_input
    int64_t             now_time;

    std::vector<std::thread> Threads;
};
