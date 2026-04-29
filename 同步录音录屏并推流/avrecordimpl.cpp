#include "avrecordimpl.h"

int g_vCollectFrameCnt = 0;	//视频采集帧数
int g_vEncodeFrameCnt = 0;	//视频编码帧数
int g_aCollectFrameCnt = 0;	//音频采集帧数
int g_aEncodeFrameCnt = 0;	//音频编码帧数

int diff = 0;
int a = 0, b = 0; //验证 av_read_frame() 采集的帧数量是否与 写入帧的数量一致

std::string AVRecordImpl::WideCharToUtf8(const std::wstring& wstr)
{
	if (wstr.empty()) return "";

	int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
		NULL, 0, NULL, NULL);
	std::string result(sizeNeeded - 1, 0); // -1 去掉 null terminator
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
		&result[0], sizeNeeded, NULL, NULL);
	return result;
}

AVRecordImpl::AVRecordImpl():
	m_aFmtCtx(nullptr), m_oFmtCtx(nullptr), m_aDecodeCtx(nullptr),
	m_aEncodeCtx(nullptr), m_swrCtx(nullptr),m_vOutFrame(nullptr), 
	m_state(RecordState::NotStarted),
	//初始化“音视频同步”用到的 当前pts时间戳
	m_aCurPts(0),m_vCurPts(0),
	m_aIndex(-1),m_vIndex(-1),m_fps(24),m_audioBitrate(192000), m_nbSamples(-1),
	start_time(-1),now_time(-1),
	//输出视频流信息
	m_width(2560),m_height(1440),
	//共享队列
	m_vFifoBuf(nullptr),m_aFifoBuf(nullptr)
{
}

void AVRecordImpl::Init()
{
	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
	av_log_set_level(AV_LOG_DEBUG);
	//注册设备
	avdevice_register_all();

	if (OpenVideo() < 0) // 打开摄像头（屏幕 screen）
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to OpenVideo()!\n");
		return;
	}

	if (OpenAudio() < 0) // 打开麦克风
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to OpenAudio()!\n");
		return;
	}

	if (OpenOutput() < 0) // 设置输出文件格式、准备好：解码、编码、重采样（一切事宜）
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to OpenOutput()!\n");
		return;
	}

	if (this->m_oFmtCtx->streams[this->m_vOutIndex]->time_base.den != 30)
	{
		std::cout << "不 == 30！" << std::endl;
	}

	if (InitAudioBuffer() < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to InitAudioBuffer()!\n");
		return;
	}

	if (InitVideoBuffer() < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to InitVideoBuffer()!\n");
		return;
	}

	//开启线程
	this->Start();
}

void AVRecordImpl::Start()
{
	//测试
	//AVFrame* frame = nullptr;
	//frame = AllocAudioFrame(m_aEncodeCtx, m_aEncodeCtx->frame_size);

	//测试
	//std::thread c(&AVRecordImpl::SoundRecordThreadProc, this);
	//Threads.emplace_back(std::move(c));

	//测试
	//std::thread d(&AVRecordImpl::ScreenRecordThreadProc, this);
	//Threads.emplace_back(std::move(d));

	if (m_state == RecordState::NotStarted)
	{
		m_state = RecordState::Started;

		std::thread screenRecord(&AVRecordImpl::MuxThreadProc, this);

		Threads.emplace_back(std::move(screenRecord));
	}

	//启动事件循环监控
	sdl_event_work();
}

void AVRecordImpl::Stop()
{

	m_state = RecordState::Stopped;

	av_log(NULL, AV_LOG_WARNING, "don't too care! the program is exiting!\n");

	for (auto& it : this->Threads)
	{
		if (it.joinable())
		{
			it.join();
		}
	}

	av_log(NULL, AV_LOG_WARNING, "The all of the threads exit!\n");

	//释放所有资源
	Release();
	av_log(NULL, AV_LOG_WARNING, "the source of this class is free!\n");

	std::cout << a << " 采集 ：写入" << b << std::endl;
}

void AVRecordImpl::MuxThreadProc()
{
	AVFrame* aframe = nullptr;
	AVPacket* pkt = nullptr;

	int vFrameIndex = 0, aFrameIndex = 0; // 用来计算时间戳

	int ret = -1;
	bool done = false; // 是否结束

	aframe = av_frame_alloc();
	pkt = av_packet_alloc();
	if (!pkt || !aframe)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc to \"pkt/aframe of\" MuxThread!\n");
		return;
	}
	//分配aframe，用于接受原始数据
	do
	{
		aframe->format = this->m_aEncodeCtx->sample_fmt;
		aframe->sample_rate = this->m_aEncodeCtx->sample_rate;
		ret = av_channel_layout_copy(&aframe->ch_layout, &this->m_aEncodeCtx->ch_layout);
		if (ret < 0)
		{
			av_log(m_aEncodeCtx, AV_LOG_ERROR, "Failed to copy the ch_layout to aframe of MuxThread!\n");
			av_frame_free(&aframe);
			return;
		}
		aframe->nb_samples = this->m_nbSamples;
		
		ret = av_frame_get_buffer(aframe, 0);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to av_frame_get_buffer()!\n");
			av_frame_free(&aframe);
			return;
		}

	} while (0);

	//启动生产者线程
	std::thread soundRound(&AVRecordImpl::SoundRecordThreadProc, this);
	std::thread screenRound(&AVRecordImpl::ScreenRecordThreadProc, this);

	this->Threads.emplace_back(std::move(screenRound));
	this->Threads.emplace_back(std::move(soundRound));

	//等待一段时间	200ms
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	//循环读取、编码
	while (true)
	{
		//循环退出逻辑
		do 
		{
			if (m_state == Stopped && !done)
			{
				done = true;
			}

			//std::cout << "当前的 done == " << done << std::endl;

			//循环出口
			if (done)
			{
				std::unique_lock<std::mutex> video(this->m_mtxVBuf, std::defer_lock); // defer_lock -- 创建锁，但不是马上加锁
				std::unique_lock<std::mutex> audio(this->m_mtxABuf, std::defer_lock);

				std::lock(video, audio); // 同时加锁，避免死锁

				//printf("av_fifo_can_read(this->m_vFifoBuf) < this->m_vOutFrameSize吗？：%d\n\
				//	av_audio_fifo_size(this->m_aFifoBuf) < this->m_nbSamples吗？%d\n", 
				//	av_fifo_can_read(this->m_vFifoBuf) < this->m_vOutFrameSize,
				//	av_audio_fifo_size(this->m_aFifoBuf) < this->m_nbSamples
				//);

				//printf("视频 FIfo 剩余数据量：%d，一帧大小：%d\n", av_fifo_can_read(this->m_vFifoBuf), this->m_vOutFrameSize);

				//如果缓冲区中：视频不满一帧大小、音频样本不足，退出循环
				if (av_fifo_can_read(this->m_vFifoBuf) < this->m_vOutFrameSize
					&& av_audio_fifo_size(this->m_aFifoBuf) < this->m_nbSamples)
				{
					av_log(NULL, AV_LOG_WARNING, "The MuxThread of while is return!is exiting!\n");
					//6、break;
					goto _MuxThreadProc_Main_Exit;
				}
			}
		} while (0);

		//（video）音视频同步策略
		ret = av_compare_ts(this->m_vCurPts, this->m_oFmtCtx->streams[this->m_vOutIndex]->time_base,
			this->m_aCurPts, this->m_oFmtCtx->streams[this->m_aOutIndex]->time_base);
		//printf("音视频同步策略的 时间戳比较结果：ret = %d:（视频）%d -- （音频）%d\n", ret, this->m_vCurPts / 15360, this->m_aCurPts / 44100);
		//防止因为该函数对 INT_MAX 做运算，从而导致逻辑变味！（也就是执行逻辑不符合预期）
		do
		{
			//先判断是否有一个时间戳达到 退出条件
			if (this->m_vCurPts == INT_MAX || this->m_aCurPts == INT_MAX)
			{
				//在这种情况下，直接判断哪一个是 INT_MAX，直接改变ret的值，从而修正同步策略

				//音频（消费完）退出
				if (this->m_vCurPts <= this->m_aCurPts)
				{
					ret = -1;
				}
				else
				{
					ret = 1;
				}
			}

		} while (0);

		if (ret <= 0)
		{
			if (done)
			{
				//printf("消费者（视频）已接收、并正在执行 m_state 的结束命令！\n");
				std::lock_guard<std::mutex> lk(this->m_mtxVBuf);
				//没数据了
				if (av_fifo_can_read(this->m_vFifoBuf) < this->m_vOutFrameSize)
				{
					//printf("消费者（视频）的 m_vCurPts 已标记为 ：INT_MAX\n");
					this->m_vCurPts = INT_MAX;
					continue; //去检查音频或者退出消费者线程
				}
			}

			//音频pts >= 视频pts

			do
			{
				//printf("（视频） 消费者 锁已到达！\n");
				std::unique_lock<std::mutex> u(this->m_mtxVBuf);
				this->m_cvVBufNotEmpty.wait(u,
					[this] {return av_fifo_can_read(this->m_vFifoBuf) >= this->m_vOutFrameSize; }
				);

				//printf("读取前，video共享队列剩余数据(bytes)：%d\n",av_fifo_can_read(this->m_vFifoBuf));
				//if (done)
				//{
				//	printf("done == 1 时，读取前，video共享队列剩余数据(bytes)：%d\n", av_fifo_can_read(this->m_vFifoBuf));
				//}

				//读取（写到分配好的 缓冲区 uint8_t* 中，已经和 m_vOutFrame 绑定）
				ret = av_fifo_read(this->m_vFifoBuf, this->m_vOutFrameBuf, this->m_vOutFrameSize);
				if (ret < 0)
				{
					av_log(NULL, AV_LOG_ERROR, "Failed to av_fifo_read video_frame!\n");
					return;
				}

				//if (done)
				//{
				//	printf("done == 1 时，读取后，video共享队列剩余数据(bytes)：%d\n", av_fifo_can_read(this->m_vFifoBuf));
				//}

				//printf("读取后，video共享队列剩余数据(bytes)：%d\n",av_fifo_can_read(this->m_vFifoBuf));

				this->m_cvVBufNotFull.notify_one();
				//printf("\033[0;32m[DEBUG] 消费者通知生产者（视频）一次！\033[0m\n");

			} while (0); 

			//5、设置参数（为什么这里设置的 时间戳每次都 +1 的情况也包括：send和receive因为数据不足而执行不成功？不怕丢帧吗？）
			this->m_vOutFrame->pts = vFrameIndex++;

			//编码
			ret = avcodec_send_frame(this->m_vEncodeCtx, this->m_vOutFrame);
			if (ret < 0)
			{
				av_log(this->m_vEncodeCtx, AV_LOG_WARNING, "the video_out_frame send frame Failed!,ret = %d\n", ret);
				continue;
			}

			//接收
			ret = avcodec_receive_packet(this->m_vEncodeCtx, pkt);
			if (ret < 0)
			{
				av_log(m_vEncodeCtx, AV_LOG_WARNING, "the video_out_packet receive frame Failed!,ret = %d\n", ret);
				continue;
			}

			av_log(m_vEncodeCtx, AV_LOG_INFO, "Encoded frame PTS: %ld, DTS: %ld\n", pkt->pts, pkt->dts);

			pkt->stream_index = this->m_vOutIndex;
			//4、转换时间基：从“底层时间基”转换为“封装层时间基”（为什么要）
			av_packet_rescale_ts(pkt, this->m_vEncodeCtx->time_base, this->m_oFmtCtx->streams[this->m_vOutIndex]->time_base);

			pkt->pts += diff;
			pkt->dts += diff;

			//记录当前时间戳（修改后的）
			this->m_vCurPts = pkt->pts;

			//printf("（视频）当前的时间戳：The m_vCurPts = %d\n", this->m_vCurPts);
			
			//视频帧编码并写入文件（交错）
			ret = av_interleaved_write_frame(this->m_oFmtCtx, pkt);
			if (ret < 0)
			{
				av_log(m_oFmtCtx, AV_LOG_ERROR, "Failed to interleaved_write frame to m_oFmtCtx!\n");
				return;
			}
			av_log(m_oFmtCtx, AV_LOG_DEBUG, "Write video packet id :%d\n", g_vEncodeFrameCnt++);

			b++;

			av_packet_unref(pkt);
		}
		else // （audio）
		{
			if (done)
			{
				//printf("（音频）消费者已接收、并正在执行 m_state 的结束命令！\n");
				std::lock_guard<std::mutex> lk(this->m_mtxABuf);
				//没数据了
				if (av_audio_fifo_size(this->m_aFifoBuf) < this->m_nbSamples)
				{
					//printf("（音频）消费者的 m_aCurPts 已标记为 ：INT_MAX\n");
					this->m_aCurPts = INT_MAX;
					continue; //去检查视频或者退出消费者线程
				}
			}

			//视频帧 pts > 音频pts
			
			do
			{
				//printf("（音频） 消费者 锁已到达！\n");
				//printf("（音频）消费者，还没上锁前、读取前，缓冲区剩余数据：%d\n", av_audio_fifo_size(this->m_aFifoBuf));
				std::unique_lock<std::mutex> u(this->m_mtxABuf);

				this->m_cvABufNotEmpty.wait(u,
					[this] {return av_audio_fifo_size(this->m_aFifoBuf) >= this->m_nbSamples; }
				);

				//printf("（音频）消费者，读取前，缓冲区剩余数据：%d		", av_audio_fifo_size(this->m_aFifoBuf));
				//读取
				ret = av_audio_fifo_read(this->m_aFifoBuf, (void* const*)aframe->data, this->m_nbSamples);
				if (ret < 0)
				{
					av_log(NULL, AV_LOG_ERROR, "Failed to av_audio_fifo_read(),the ret :%d\n", ret);
					return;
				}
				//唤醒
				this->m_cvABufNotFull.notify_one();
				//printf("（音频）消费者，读取后，缓冲区剩余数据：%d，通知音频生产者一次\n", av_audio_fifo_size(this->m_aFifoBuf));

			} while (0);

			//设置参数（编码前）
			aframe->pts = this->m_nbSamples * aFrameIndex++;

			//编码
			ret = avcodec_send_frame(this->m_aEncodeCtx, aframe);
			if (ret < 0)
			{
				av_log(m_aEncodeCtx, AV_LOG_WARNING, "Failed to avcodec_send_frame audio!,ret = %d\n", ret);
				av_frame_make_writable(aframe);
				av_samples_set_silence(
					aframe->data,
					0,
					this->m_nbSamples,
					this->m_aEncodeCtx->ch_layout.nb_channels,
					this->m_aEncodeCtx->sample_fmt
				);
				continue;
			}

			//接收
			ret = avcodec_receive_packet(this->m_aEncodeCtx, pkt);
			if (ret < 0)
			{
				av_log(m_aEncodeCtx, AV_LOG_WARNING, "Failed to avcodec_receive_frame audio!,ret = %d\n", ret);
				av_frame_make_writable(aframe);
				av_samples_set_silence(
					aframe->data,
					0,
					this->m_nbSamples,
					this->m_aEncodeCtx->ch_layout.nb_channels,
					this->m_aEncodeCtx->sample_fmt
				);
				continue;
			}

			//时间戳（记录、转换）、流ID
			pkt->stream_index = this->m_aOutIndex;
			av_packet_rescale_ts(pkt, this->m_aEncodeCtx->time_base, this->m_oFmtCtx->streams[this->m_aOutIndex]->time_base);
			this->m_aCurPts = pkt->pts;
			//av_log(NULL, AV_LOG_DEBUG, "The m_aCurPts = %d\n", this->m_aCurPts);

			//音频帧编码并写入文件（交错）
			ret = av_interleaved_write_frame(this->m_oFmtCtx, pkt);
			if (ret < 0)
			{
				av_log(this->m_oFmtCtx, AV_LOG_ERROR, "Failed to write audio_pkt to m_oFmtCtx!\n");
				av_frame_free(&aframe);
				av_packet_free(&pkt);
				return;
			}

		}
		//循环结束
	}

_MuxThreadProc_Main_Exit: // 别问我为什么这里有这个“死人”

	//刷新
	FlushEncoders();

	//文件尾
	ret = av_write_trailer(this->m_oFmtCtx);
	if (ret < 0)
	{
		av_log(m_oFmtCtx, AV_LOG_ERROR, "Failed to write trailer to handle of file!\n");
	}
	
	printf("文件尾已经成功写入！\n");

	//释放
	av_packet_free(&pkt);
	av_frame_free(&aframe);

	printf("（消费者）线程退出！\n");
}

//视频线程（生产）
void AVRecordImpl::ScreenRecordThreadProc()
{
	AVPacket* pkt = nullptr;
	AVFrame* frame = nullptr;
	AVFrame* newFrame = nullptr;

	int ret = -1;
	int y_size = -1;

	pkt = av_packet_alloc();
	frame = av_frame_alloc();
	newFrame = av_frame_alloc();

	//检测
	//FILE* f = nullptr;
	//f = fopen("./new/test.yuv", "wb");

	if (!pkt || !frame || !newFrame)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc video_frame_or_pkt!\n");
		return;
	}

	//分配接收重采样的newFrame
	do
	{	
		y_size = m_vEncodeCtx->width * m_vEncodeCtx->height;

		//计策了一下，是相等的，也就是通过 av_image_get_buffer_size() 对其选项为 “1”的情况下，计算出来的一帧所需大小和 width * height * 1.5是相等的
		//std::cout << (y_size * 1.5 == this->m_vOutFrameSize) << std::endl;

		newFrame->format = this->m_vEncodeCtx->pix_fmt;
		newFrame->width = this->m_vEncodeCtx->width;
		newFrame->height = this->m_vEncodeCtx->height;

		ret = av_frame_get_buffer(newFrame, 1);
		if (ret < 0)
		{
			av_log(newFrame, AV_LOG_ERROR, "Failed to fill the video_newFrame by av_frame_get_buffer()!\n");
			return;
		}

	} while (0);

	//8、获取开始录屏的本地时间，这种方式误差太大
	this->start_time = av_gettime_relative();

	while (m_state != Stopped)
	{
		//读取原始帧
		ret = av_read_frame(this->m_vFmtCtx, pkt);
		std::cout << pkt->pts << std::endl;
		if (ret < 0)
		{
			printf("av_read_frame() video < 0\n");
			continue;
		}
			//检查流id
		if (pkt->stream_index != this->m_vIndex)
		{
			av_log(NULL, AV_LOG_ERROR, "the pkt->stream_index is not!\n");
			av_packet_unref(pkt);
			continue;
		}

		//解码
		ret = avcodec_send_packet(this->m_vDecodeCtx, pkt);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_WARNING, "the video frmae Decode is failed!\n");
			av_packet_unref(pkt);
			continue;
		}

		//接收解码(后)数据
		ret = avcodec_receive_frame(this->m_vDecodeCtx, frame);
		if (ret < 0)
		{
			printf("the video -- avcodec_receive_frame() < 0!\n");
			av_packet_unref(pkt);
			continue;
		}

		++g_vCollectFrameCnt;

		//重采样
		sws_scale(
			this->m_swsCtx,
			(const uint8_t* const*)frame->data,
			frame->linesize,
			0,
			frame->height,
			newFrame->data,
			newFrame->linesize
		);

		//检测（写入yuv文件） ffplay -format yuv420p -video_size 1920x1080 -i test.yuv 播放正常
		do
		{
			//if (!f)
			//{
			//	av_log(NULL, AV_LOG_ERROR, "Failed to open the test.yuv!\n");
			//	return;
			//}
			//for (int i = 0; i < newFrame->height; i++)
			//	fwrite(newFrame->data[0] + i * newFrame->linesize[0], 1, newFrame->width, f);

			//for (int i = 0; i < newFrame->height / 2; i++)
			//	fwrite(newFrame->data[1] + i * newFrame->linesize[1], 1, newFrame->width / 2, f);

			//for (int i = 0; i < newFrame->height / 2; i++)
			//	fwrite(newFrame->data[2] + i * newFrame->linesize[2], 1, newFrame->width / 2, f);

		} while (0);

		// 写入共享队列（使用 AVFifo），在此处好像不太可行，如果要用这个API写入的话，必须在写入之前将y\u\v数据都放在同一个数组中，也就是拷贝，这不划算
		do
		{
			std::unique_lock<std::mutex> u(this->m_mtxVBuf);

			this->m_cvVBufNotFull.wait(u,
				[this] {
					return av_fifo_can_write(this->m_vFifoBuf) >= this->m_vOutFrameSize || m_state == Stopped; // 初始化视频共享队列
				});

			//printf("写入前，video共享队列剩余空间(bytes)：%d, 当前帧大小:%d\n",
				//av_fifo_can_write(this->m_vFifoBuf), this->m_vOutFrameSize);

			ret = av_fifo_write(this->m_vFifoBuf, newFrame->data[0], y_size);
			if (ret < 0) {
				av_log(nullptr, AV_LOG_ERROR, "Failed to write Y plane to video_fifo_buf! error: %d\n", ret);
				return;
			}
			ret = av_fifo_write(this->m_vFifoBuf, newFrame->data[1], y_size / 4);
			if (ret < 0) {
				av_log(nullptr, AV_LOG_ERROR, "Failed to write U plane to video_fifo_buf! error: %d\n", ret);
				return;
			}
			ret = av_fifo_write(this->m_vFifoBuf, newFrame->data[2], y_size / 4);
			if (ret < 0) {
				av_log(nullptr, AV_LOG_ERROR, "Failed to write V plane to video_fifo_buf! error: %d\n", ret);
				return;
			}

			//printf("写入后，video共享队列剩余空间(bytes)：%d, 当前帧大小:%d\n",
				//av_fifo_can_write(this->m_vFifoBuf), this->m_vOutFrameSize);

			a++;

			this->m_cvVBufNotEmpty.notify_one();

		} while (0);
		//我的一个思路是：如同线程池中的线程一样，现在开始初始化的时候，多创建几个newFrame，之后只需要在消费端不断 消耗，并通知生产者哪些被使用了，就直接在生产端填充就好，但是不用轮训机制的话，实现起来有点麻烦！！！！

		av_packet_unref(pkt);
		av_frame_unref(frame);
	}

	FlushVideoDecoder();

	av_packet_free(&pkt);
	av_frame_free(&frame);
	if (newFrame)
		av_frame_free(&newFrame);

	//fclose(f);

	printf("（视频）生产者线程退出！\n");
}

//音频线程（生产）
void AVRecordImpl::SoundRecordThreadProc()
{
	AVPacket* pkt = nullptr;
	AVFrame* frame = nullptr;
	AVFrame* newFrame = nullptr;
	int nbSamples = this->m_nbSamples;
	int dstNbSamples = 0, maxNbSamples = 0;

	int ret = 0;
	
	//首先根据采样率来重新计算样本个数
	dstNbSamples = maxNbSamples = av_rescale_rnd(nbSamples, this->m_aEncodeCtx->sample_rate, this->m_aDecodeCtx->sample_rate, AV_ROUND_UP);

	//重采样所需buffer
	newFrame = AllocAudioFrame(this->m_aEncodeCtx, nbSamples);
	if (!newFrame)
	{
		av_log(NULL, AV_LOG_ERROR, "AllocAudioFrame is Failed!\n");
		return;
	}

	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc audio_packet!\n");
		return;
	}

	frame = av_frame_alloc();
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc audio_frame!\n");
		return;
	}


	while (m_state != Stopped)
	{

		//读取音频帧
		ret = av_read_frame(this->m_aFmtCtx, pkt);
		if (ret < 0)
		{
			av_log(this->m_aFmtCtx, AV_LOG_ERROR, "Failed to read_frame from audio_device!\n");
			return;
		}

			//检查（即使 m_aIndex == m_vIndex ，也要检查，因为可能处于某些不可控的因素，导致读取到的pkt不是我们选定的音频设备记录的！！！，视频也一样）
		if (pkt->stream_index != m_aIndex)
		{
			av_log(NULL, AV_LOG_WARNING, "The pkt->stream_index != m_aIndex!\n");
			av_packet_unref(pkt);
			continue;
		}

		//解码
		ret = avcodec_send_packet(this->m_aDecodeCtx, pkt);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_WARNING, "Failed to send_packet to m_aDecodeCtx! the error: %d\n", ret);
			av_packet_unref(pkt);
			continue;
		}

		//接收
		ret = avcodec_receive_frame(this->m_aDecodeCtx, frame);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_WARNING, "Failed to receive_frame from m_aDecodeCtx! the error: %d\n", ret);
			av_packet_unref(pkt);
			av_frame_unref(frame);
			continue;
		}

		//重采样
		++g_aCollectFrameCnt; //采集帧数+1

		//重采样
		do
		{
			//根据“延迟的（重采样器缓存的--未转换的）”+ “当前帧的样本数”---> 当做“输入重采样器”的总样本数，也就是输入，
			//然后根据采样率的比例关系，重新计算此次，应该输出多少用采样数，以便于重新分配接受者 的缓冲区大小！
			dstNbSamples = av_rescale_rnd(
				swr_get_delay(this->m_swrCtx, this->m_aDecodeCtx->sample_rate) + frame->nb_samples,
				this->m_aEncodeCtx->sample_rate,
				this->m_aDecodeCtx->sample_rate,
				AV_ROUND_UP
			);
			
			//计算出来的 输出样本数如果大于 max 样本数，就重新分配
			if (dstNbSamples > maxNbSamples)
			{
				maxNbSamples = dstNbSamples;

				if (newFrame)
				{
					av_frame_free(&newFrame);
					newFrame = AllocAudioFrame(this->m_aEncodeCtx, dstNbSamples);
				}

				if (!newFrame)
				{
					av_log(NULL, AV_LOG_ERROR, "Failed to alloc_audio_newFrame_swr!\n");
					return;
				}
			}

			//转换
			newFrame->nb_samples = swr_convert(
				this->m_swrCtx, 
				(uint8_t**)newFrame->data,
				dstNbSamples, 
				(const uint8_t**)frame->data,
				frame->nb_samples
			);

			//检查
			if (newFrame->nb_samples < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "The swr_convert() return Error:%d\n", newFrame->nb_samples);
				return;
			}

		} while (0);

		//用于测试原始数据是否正确
		do 
		{
			//检测
			//FILE* f = nullptr;
			//f = fopen("./new/test.pcm", "wb");
			//if (!f)
			//{
			//	av_log(f, AV_LOG_ERROR, "Failed to open the test.pcm\n");
			//	return;
			//}

			//写入pcm先看看对不对
			//int bytesPerSample = av_get_bytes_per_sample((AVSampleFormat)(newFrame->format)); // 这里的C风格的强制类型转换不推荐，推荐使用c++的 

			//for (int i = 0; i < newFrame->ch_layout.nb_channels-1; i++)
			//{
			//	if (fwrite(newFrame->data[i], 1, newFrame->nb_samples * bytesPerSample*2, f) < 0)
			//	{
			//		av_log(NULL, AV_LOG_ERROR, "Failed to fwrite test.pcm to test!\n");
			//		return;
			//	}
			//}

			//for (int sample = 0; sample < newFrame->nb_samples; sample++) {
			//	for (int ch = 0; ch < this->m_aEncodeCtx->ch_layout.nb_channels; ch++) {
			//		fwrite(newFrame->data[ch] + sample * bytesPerSample, 1, bytesPerSample, f);
			//	}
			//}
		} while (0);

		//加锁 && 写入 audio_fifo_buf
		do
		{
			std::unique_lock<std::mutex> u(this->m_mtxABuf);

			this->m_cvABufNotFull.wait(u,
				[this, &newFrame] {
				return av_audio_fifo_space(this->m_aFifoBuf) >= newFrame->nb_samples; // 这里是为了防止极端情况出现，导致该线程一直被挂起，以至于整个程序无法结束
				});

			//printf("写入audio_fifo_buf之前，剩余空间为：%d,要写入的per_nb_samples：%d     ",
			//	av_audio_fifo_space(this->m_aFifoBuf), newFrame->nb_samples);

			//不满的话，就继续写入
			ret = av_audio_fifo_write(this->m_aFifoBuf, (void* const*)newFrame->data, newFrame->nb_samples);
			if (ret < 0)
			{
				av_log(m_aFifoBuf, AV_LOG_ERROR, "Failed to av_audio_fifo_write()!\n");
				return;
			}

			//printf("写入audio_fifo_buf之后，剩余空间为：%d\n",
			//	av_audio_fifo_space(this->m_aFifoBuf));

			//printf("唤醒消费者一次!\n");
			this->m_cvABufNotEmpty.notify_one();

		} while (0);



		//循环1次结束后的释放工作
		av_packet_unref(pkt);
		av_frame_unref(frame);
		//av_frame_unref(newFrame); 
		av_frame_make_writable(newFrame);
		av_samples_set_silence(
			newFrame->data,
			0,
			newFrame->nb_samples,
			newFrame->ch_layout.nb_channels,
			(AVSampleFormat)newFrame->format
		);
	}

	//结束后的收尾工作

	//刷新
	FlushAudioDecoder();

	if (pkt)
		av_packet_free(&pkt);
	if (frame)
		av_frame_free(&frame);
	if (newFrame)
		av_frame_free(&newFrame);

	printf("（音频）生产者线程退出！\n");
}

void AVRecordImpl::FlushEncoders()
{
	int ret = -1;
	bool vBeginFlush = false;
	bool aBeginFlush = false;

	m_vCurPts = m_aCurPts = 0;

	int nFlush = 2;

	AVPacket* pkt = nullptr;
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to av_packet_alloc()!\n");
		return;
	}

	while (1)
	{
		if (av_compare_ts(m_vCurPts, m_oFmtCtx->streams[m_vOutIndex]->time_base,
			m_aCurPts, m_oFmtCtx->streams[m_aOutIndex]->time_base) <= 0)
		{
			if (!vBeginFlush)
			{
				vBeginFlush = true;

				/// * 清空编码器缓存
				///It can be NULL, in which case it is considered a flush
				///  packet.  This signals the end of the stream.
				////
				ret = avcodec_send_frame(m_vEncodeCtx, nullptr);
				if (ret != 0)
				{
					std::cerr << "flush video avcodec_send_frame failed, ret: \n" << ret;
					return;
				}
			}
			ret = avcodec_receive_packet(m_vEncodeCtx, pkt);
			if (ret < 0)
			{
				av_packet_unref(pkt);
				if (ret == AVERROR(EAGAIN))
				{
					std::cerr << "flush video EAGAIN avcodec_receive_packet\n";
					ret = 1;
					continue;
				}
				else if (ret == AVERROR_EOF)
				{
					std::cerr << "flush video encoder finished\n";
					//break;
					if (!(--nFlush))
						break;
					m_vCurPts = INT_MAX;
					continue;
				}
				std::cerr << "flush video avcodec_receive_packet failed, ret: \n" << ret;
				return;
			}
			pkt->stream_index = m_vOutIndex;
			//将pts从编码层的timebase转成复用层的timebase
			av_packet_rescale_ts(pkt, m_vEncodeCtx->time_base, m_oFmtCtx->streams[m_vOutIndex]->time_base);

			pkt->pts += diff;
			pkt->dts += diff;

			m_vCurPts = pkt->pts;
			std::cerr << "m_vCurPts: \n" << m_vCurPts;

			ret = av_interleaved_write_frame(m_oFmtCtx, pkt);
			if (ret == 0)
				std::cerr << "flush Write video packet id: \n" << ++g_vEncodeFrameCnt;
			else
				std::cerr << "flush video av_interleaved_write_frame failed, ret:\n" << ret;
			av_packet_unref(pkt);

			b++;
		}
		else
		{
			if (!aBeginFlush)
			{
				aBeginFlush = true;
				ret = avcodec_send_frame(m_aEncodeCtx, nullptr);
				if (ret != 0)
				{
					std::cerr << "flush audio avcodec_send_frame failed, ret: \n" << ret;
					return;
				}
			}
			ret = avcodec_receive_packet(m_aEncodeCtx, pkt);
			if (ret < 0)
			{
				av_packet_unref(pkt);
				if (ret == AVERROR(EAGAIN))
				{
					std::cerr << "flush EAGAIN avcodec_receive_packet\n";
					ret = 1;
					continue;
				}
				else if (ret == AVERROR_EOF)
				{
					std::cerr << "flush audio encoder finished\n";
					/*break;*/
					if (!(--nFlush))
						break;
					m_aCurPts = INT_MAX;
					continue;
				}
				std::cerr << "flush audio avcodec_receive_packet failed, ret: \n" << ret;
				return;
			}
			pkt->stream_index = m_aOutIndex;
			//将pts从编码层的timebase转成复用层的timebase
			av_packet_rescale_ts(pkt, m_aEncodeCtx->time_base, m_oFmtCtx->streams[m_aOutIndex]->time_base);
			m_aCurPts = pkt->pts;
			std::cerr << "m_aCurPts: \n" << m_aCurPts;
			ret = av_interleaved_write_frame(m_oFmtCtx, pkt);
			if (ret == 0)
				std::cerr << "flush write audio packet id:\n " << ++g_aEncodeFrameCnt;
			else
				std::cerr << "flush audio av_interleaved_write_frame failed, ret: \n" << ret;
			av_packet_unref(pkt);
		}
	}

	av_packet_free(&pkt);
	printf("（消费者）线程中的“刷新编码器”函数以执行完毕！\n");
}

void AVRecordImpl::Release()
{
	//音频、视频原始帧 -- 重采样上下文
	if (this->m_swsCtx)
		sws_freeContext(m_swsCtx);
	if (this->m_swrCtx)
		swr_free(&m_swrCtx);

	//音频、视频 -- 编解码器
	if (this->m_aDecodeCtx)
		avcodec_free_context(&m_aDecodeCtx);
	if (this->m_aEncodeCtx)
		avcodec_free_context(&m_aEncodeCtx);

	if (this->m_vDecodeCtx)
		avcodec_free_context(&m_aDecodeCtx);
	if (this->m_vEncodeCtx)
		avcodec_free_context(&m_aEncodeCtx);

	//音频、视频环形缓冲区
	if (this->m_aFifoBuf)
		av_audio_fifo_free(m_aFifoBuf);
	if (this->m_vFifoBuf)
		av_fifo_freep2(&m_vFifoBuf);

	// 释放视频帧缓冲区（解码后）
	if (m_vOutFrameBuf)
		av_freep(m_vOutFrameBuf);
	if (m_vOutFrame)
		av_frame_free(&m_vOutFrame);

	//麦克风（录音）
	if (this->m_aFmtCtx)
	{
		avformat_close_input(&m_aFmtCtx);
		avformat_free_context(m_aFmtCtx);
	}
	//摄像头（录屏）
	if (this->m_vFmtCtx)
	{
		avformat_close_input(&m_vFmtCtx);
		avformat_free_context(m_vFmtCtx);
	}
	//输出文件上下文
	if (this->m_oFmtCtx)
	{
		avio_close(m_oFmtCtx->pb);
		avformat_free_context(m_oFmtCtx);
	}
}

/**
* @brief GUI监听键盘事件
* @note 
*	1、其中的 SDL_WaitEvent() 和 epoll_wait() 都是 Reactor 事件驱动模式，也就是无事件发生时当前线程处于睡眠状态
*	事件发生时才被唤醒，不占用太多系统资源，但是他俩区别很大，只是效果上相似而已
*/
void AVRecordImpl::sdl_event_work()
{
	SDL_Window* window = SDL_CreateWindow("Recording Program", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN);
	if (!window)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to CreateWindow!\n");
		return;
	}

	SDL_Event event;
	while (m_state != RecordState::Stopped)
	{
		SDL_WaitEvent(&event);

		switch (event.type)
		{
		case SDL_QUIT:
			av_log(NULL, AV_LOG_WARNING, "the recording program is SDL_QUIT!\n");
			Stop();
			break;
		case SDL_KEYDOWN:
			if (event.key.keysym.sym == SDLK_q)
			{
				av_log(NULL, AV_LOG_WARNING, "the SDLK_q is exit!\n");
				Stop();
			}
			break;
		default:
			av_log(NULL, AV_LOG_INFO, "the key is Invilid\n");
			break;
		}
	}

	SDL_DestroyWindow(window);
	SDL_Quit();
}

int AVRecordImpl::OpenVideo()
{
	int ret = -1;
	AVStream* stream = nullptr;
	const AVCodec* codec = nullptr;
	AVBufferRef* hw_device_ctx = nullptr;
	char err_buf[AV_ERROR_MAX_STRING_SIZE];

	const AVInputFormat* ifmt = av_find_input_format("gdigrab"); // gdigrab dshow
	if (!ifmt) {
		av_log(NULL, AV_LOG_ERROR, "Failed to find input format dshow\n");
		return -1;
	}


	//参数
	AVDictionary* options = nullptr;
	/*
		ffmpeg命令行：
		ffmpeg -f gdigrab -framerate 5 -offset_x 100 -offset_y 200
		-video_size 640x360 -i  desktop -pix_fmt yuv420p
		-vcodec libx264   -y out3.mp4
	*/

	m_fps;
	// 为输入流增加缓冲区大小
	//av_dict_set(&options, "rtbufsize", "2000M", 0); // 设置为10MB
	//av_dict_set(&options, "draw_mouse", "1", 0); // 可选，绘制鼠标
	//// 设置采集区域为全屏，比如 1920x1080，左上角为 (0, 0)
	//av_dict_set(&options, "capture_width", "1920", 0);  // 采集宽度
	//av_dict_set(&options, "capture_height", "1080", 0); // 采集高度
	//av_dict_set(&options, "capture_x", "0", 0);         // 左上角 X 坐标
	//av_dict_set(&options, "capture_y", "0", 0);         // 左上角 Y 坐标

	av_dict_set(&options, "framerate", std::to_string(this->m_fps + 6).c_str(), NULL);

	//开始录屏
	ret = avformat_open_input(&m_vFmtCtx, "desktop", ifmt, &options); // video=screen-capture-recorder desktop
	if (ret < 0)
	{
		av_strerror(ret, err_buf, sizeof(err_buf) / sizeof(err_buf[0]));
		av_log(NULL, AV_LOG_ERROR, "%s\n", err_buf);
		av_log(NULL, AV_LOG_ERROR, "Failed to avformat_open_input()-->Video \"desktop\"\n");
		return ret;
	}
	// 填充流信息
	ret = avformat_find_stream_info(m_vFmtCtx, nullptr);
	if (ret < 0)
	{
		av_log(m_vFmtCtx, AV_LOG_ERROR, "Failed to avformat_find_stream_info()!\n");
		return ret;
	}

	for (int i = 0; i < m_vFmtCtx->nb_streams; i++)
	{
		stream = m_vFmtCtx->streams[i];
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			this->m_vIndex = i;
			break;
		}
	}

	av_log(NULL, AV_LOG_INFO, "Codec ID: %d\n", stream->codecpar->codec_id);

	//配置解码器（这里的h264_cuvid 只能用来解码 h264 格式的文件，用到这里用来练手而已）
	codec = avcodec_find_decoder_by_name("h264_cuvid");
	if (!codec || stream->codecpar->codec_id != AV_CODEC_ID_H264)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to find the \"h264_cuvid\"\n");
		codec = avcodec_find_decoder(stream->codecpar->codec_id);
		if (!codec)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to find the personal decoder!\n");
			return -1;
		}
	}

	//解码器上下文 
	this->m_vDecodeCtx = avcodec_alloc_context3(codec);
	if (!m_vDecodeCtx)
	{
		av_log(&codec, AV_LOG_ERROR, "No memory to this->m_vDecodeCtx!\n");
		return -1;
	}

	ret = avcodec_parameters_to_context(this->m_vDecodeCtx, stream->codecpar);
	if (ret < 0)
	{
		av_log(this->m_vDecodeCtx, AV_LOG_ERROR, "No memory to this->m_vDecodeCtx!\n");
		return ret;
	}

	//av_log(NULL, AV_LOG_INFO, "AVCodec name        : %s\n", codec ? codec->name : "NULL");
	//av_log(NULL, AV_LOG_INFO, "AVStream index      : %d\n", m_vIndex);
	//av_log(NULL, AV_LOG_INFO, "AVCodecParameters codec_id: %d\n", stream->codecpar->codec_id);
	//av_log(NULL, AV_LOG_INFO, "AVCodecParameters width x height: %d x %d\n", stream->codecpar->width, stream->codecpar->height);


	//// 打印 AVCodecContext 的所有关键属性
	//PrintAVCodecContext(m_vDecodeCtx);

	//av_log(this->m_vDecodeCtx, AV_LOG_INFO, "Timebase: %d/%d\n", m_vDecodeCtx->time_base.num, m_vDecodeCtx->time_base.den);
	//av_log(this->m_vDecodeCtx, AV_LOG_INFO, "Frame rate: %d/%d\n", m_vDecodeCtx->framerate.num, m_vDecodeCtx->framerate.den);
	//av_log(this->m_vDecodeCtx, AV_LOG_INFO, "Pixel format: %d\n", m_vDecodeCtx->pix_fmt);

	//设置硬件设备上下文（这里必修设置，否则codec和上下文参数不一致，有解释，但是一句话概括为：
	//					因为 h264_cuvid 是一个依赖于 GPU的，如：libx264 这种是软解码，有CPU就可以使用的解码器，不依赖于其他硬件设备
	//					而 h264_cuvid 具体的解码工作，是由 GPU 进行的，那么它的 解码器上下文就必须要有 相关的硬件设备的信息，
	//					这就是额外的一步：获取解码器硬件信息）
	if (std::string(codec->name) == "h264_cuvid")
	{
		if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
			fprintf(stderr, "Failed to create CUDA device\n");
			return -1;
		}

		this->m_vDecodeCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx); // 复制引用
	}


	//打开解码器 
	ret = avcodec_open2(m_vDecodeCtx, codec, nullptr);
	if (ret < 0)
	{
		av_log(this->m_vDecodeCtx, AV_LOG_ERROR, "Failed to open the m_vDecodeCtx!--> avcodec_open2()\n");
		return ret;
	}

	return 0;
}

int AVRecordImpl::OpenAudio()
{
	int ret = -1;
	const AVInputFormat* ifmt = nullptr;
	AVDeviceInfoList* device_list = nullptr;
	AVStream* stream = nullptr;

	const AVCodec* codec = nullptr;


	//获取麦克风名称并转换为 Utf-8 便于ffmpegAPI识别
	ifmt = av_find_input_format("dshow");
	if (!ifmt)
	{
		av_log(NULL, AV_LOG_DEBUG, "Failed to find input format--dshow!\n");
		return -1;
	}

	ret = avdevice_list_input_sources(ifmt, NULL, NULL, &device_list);
	if (ret < 0)
	{
		av_log(&ifmt, AV_LOG_ERROR, "Failed to get the device_list of Audio!\n");
		return -1;
	}

	const char* audioDeviceName = device_list->devices[2]->device_name; // [0]是 摄像头、[1]是电脑麦克风、[2]是外设麦克风（现在是 iqoo 的耳机）

	//for (int i = 0; i < device_list->nb_devices; i++)
	//{
	//	std::cout << device_list->devices[i]->device_description << std::endl;
	//}

	std::string name = std::string("audio=") + audioDeviceName;

	av_log(NULL, AV_LOG_DEBUG, "The audioDeviceName = \"%s\"\n", name.c_str());
	//开启麦克风
	ret = avformat_open_input(&this->m_aFmtCtx, name.c_str(), ifmt, nullptr); // 这里不用转化为 utf-8，直接用 audio+device_name即可，但是这里的device_name不是给人看的，是给计算机看的
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to open_input_Audio!\n");
		return ret;
	}

	ret = avformat_find_stream_info(m_aFmtCtx, nullptr);
	if (ret < 0)
	{
		av_log(m_aFmtCtx, AV_LOG_ERROR, "avformat_find_stream_info(m_aFmtCtx) is Failed!\n");
		return -1;
	}
	//寻找音频流 id
	for (int i = 0; i < m_aFmtCtx->nb_streams; i++)
	{
		stream = m_aFmtCtx->streams[i];
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			this->m_aIndex = i;
			break;
		}
	}

	if (m_aIndex < 0)
	{
		av_log(stream, AV_LOG_ERROR, "m_aIndex < 0 Error-->OpenAudio()\n");
		return -1;
	}

	codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to audio_find_decoder!\n");
		return -1;
	}

	this->m_aDecodeCtx = avcodec_alloc_context3(codec);

	ret = avcodec_parameters_to_context(m_aDecodeCtx, stream->codecpar);
	if (ret < 0)
	{
		av_log(m_aDecodeCtx, AV_LOG_ERROR, "Failed to avcodec_parameters_to_context(m_aDecodeCtx, stream->codecpar)!\n");
		return -1;
	}



	ret = avcodec_open2(m_aDecodeCtx, codec, nullptr);
	if (ret < 0)
	{
		av_log(m_aDecodeCtx, AV_LOG_ERROR, "Failed to open the m_aDecodeCtx! avcodec_open2()\n");
		return -1;
	}

	return 0;
}

int AVRecordImpl::OpenOutput()
{
	this->m_filePath = "./new/test.mp4";
	int ret = -1;

	AVStream* vStream = nullptr;
	AVStream* aStream = nullptr;

	const AVCodec* vCodec = nullptr;
	const AVCodec* aCodec = nullptr;

	//判断是否直播推流
	bool IsRtmp = false;
	if (this->m_filePath.find("rtmp://") != std::string::npos)
	{
		//开启直播推流标志
		IsRtmp = true;
	}

	//分配输出上下文
	ret = avformat_alloc_output_context2(&this->m_oFmtCtx, nullptr, IsRtmp ? "flv" : nullptr, this->m_filePath.c_str());
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc_output_context!\n");
		return ret;
	}

	//video
	if (this->m_vFmtCtx->streams[this->m_vIndex]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
	{
		//创建一路新的视频流，索引一般为0
		vStream = avformat_new_stream(this->m_oFmtCtx, nullptr);
		//流id
		this->m_vOutIndex = vStream->index;
		//流时间基
		vStream->time_base = AVRational{ 1,this->m_fps };

		//找编码器
		vCodec = avcodec_find_encoder_by_name("h264_nvenc");
		if (!vCodec)
		{
			av_log(this->m_oFmtCtx, AV_LOG_ERROR, "Failed to find decoder--> h264_nvenc\n");

			vCodec = avcodec_find_encoder(this->m_oFmtCtx->video_codec_id);
			if (!vCodec)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to find _decoder! now exit this __.exe!\n");
				return -1;
			}
		}
		//分配编码器上下文
		this->m_vEncodeCtx = avcodec_alloc_context3(vCodec);
		if(!this->m_vEncodeCtx)
		{
			av_log(&vCodec, AV_LOG_ERROR, "Failed to avcodec_alloc_context3()! to m_vEncodeCtx!\n");
			return -1;
		}
		//配置参数

			//视频分辨率
		this->m_vEncodeCtx->width = m_width;
		this->m_vEncodeCtx->height = m_height;
			//帧像素格式
		this->m_vEncodeCtx->pix_fmt = AV_PIX_FMT_YUV420P;
		this->m_vEncodeCtx->codec_id = AV_CODEC_ID_H264;
			//帧时间基
		this->m_vEncodeCtx->time_base.num = 1;
		this->m_vEncodeCtx->time_base.den = this->m_fps;
			//码率等相关设置
		this->m_vEncodeCtx->bit_rate = 10 * 1000 * 1000; // 4000kbps == 4Mbps != 4Mb/s ，比特率是 bit ，8 bits == 1 bytes(字节)，所以这里的进制是10进制
		this->m_vEncodeCtx->rc_max_rate = 18 * 1000 * 1000; // 最大比特率
		this->m_vEncodeCtx->rc_buffer_size = this->m_vEncodeCtx->bit_rate * 2; // 2倍的缓冲，求稳！
			//设置图像组限制
		this->m_vEncodeCtx->gop_size = this->m_fps * 2; //越小越好 ---> 这句话是错的，和其他因素尤其是 bit_rate 的大小有关
		this->m_vEncodeCtx->max_b_frames = 0;
			//设置h264中相关的参数,不设置avcodec_open2()会失败
		this->m_vEncodeCtx->qmin = 10;	//2
		this->m_vEncodeCtx->qmax = 31;	//31
		this->m_vEncodeCtx->me_range = 16;	//0
		this->m_vEncodeCtx->max_qdiff = 4;	//3
		this->m_vEncodeCtx->qcompress = 0.6;	//0.5

		this->m_vEncodeCtx->codec_tag = 0; // 由封装器自动填充
		this->m_vEncodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		av_opt_set(this->m_vEncodeCtx->priv_data, "preset", "p1", 0);

		//打开编码器
		ret = avcodec_open2(this->m_vEncodeCtx, vCodec, nullptr);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to open the encoder -- m_vEncodeCtx and Vcodec!\n");
			return ret;
		}

		//给到 vStream，进而映射到 m_oFmtCtx
		ret = avcodec_parameters_from_context(vStream->codecpar, this->m_vEncodeCtx);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to vStream-- avcodec_parameters_from_context!\n");
			return -1;
		}

		//视频重采样初始化 ,这个API就是 “sws_alloc_context + sws_init_context”的 mix 版本，获得的是初始化后的 swsContext
		this->m_swsCtx = sws_getContext(
			this->m_vDecodeCtx->width,
			this->m_vDecodeCtx->height,
			this->m_vDecodeCtx->pix_fmt,

			this->m_vEncodeCtx->width,
			this->m_vEncodeCtx->height,
			AV_PIX_FMT_YUV420P,
			SWS_FAST_BILINEAR,
			nullptr, nullptr, nullptr
		);

		if (!this->m_swsCtx)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to sws_getContext() -- video!\n");
			return -1;
		}
	}
	
	//audio
	if (this->m_aFmtCtx->streams[this->m_aIndex]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		aStream = avformat_new_stream(this->m_oFmtCtx, nullptr);
		if (!aStream)
		{
			av_log(NULL, AV_LOG_ERROR, "could not create a new stream to output!\n");
			return -1;
		}
		this->m_aOutIndex = aStream->index;

		//编码器
		aCodec = avcodec_find_encoder_by_name("libmp3lame");
		if (!aCodec)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to find the encoder -- libmp3lame!\n");
			aCodec = avcodec_find_encoder(this->m_oFmtCtx->audio_codec_id);
			if (!aCodec)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to find encoder by id -- error!\n");
				return -1;
			}
		}

		//上下文
		this->m_aEncodeCtx = avcodec_alloc_context3(aCodec);
		if (!m_aEncodeCtx)
		{
			av_log(NULL, AV_LOG_ERROR, "no memory to alloc to m_aEncodeCtx!\n");
			return -1;
		}
		
		//设置参数
		this->m_aEncodeCtx->sample_fmt = aCodec->sample_fmts ? aCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		this->m_aEncodeCtx->sample_rate = 44100;
		av_channel_layout_default(&this->m_aEncodeCtx->ch_layout, 2);
		this->m_aEncodeCtx->bit_rate = this->m_audioBitrate;

		this->m_aEncodeCtx->time_base = AVRational{ 1,this->m_aEncodeCtx->sample_rate };
		this->m_aEncodeCtx->codec_tag = 0;
		this->m_aEncodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		//打开编码器
		ret = avcodec_open2(this->m_aEncodeCtx, aCodec, nullptr);
		if(ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to open the aCodec_Out!\n");
			return -1;
		}

		//给到aStream
		ret = avcodec_parameters_from_context(aStream->codecpar, this->m_aEncodeCtx);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to avcodec_parameters_from_context(aStream->codecpar, this->m_aEncodeCtx)!\n");
			return -1;
		}

		//音频重采样初始化
		ret = swr_alloc_set_opts2(
			&this->m_swrCtx,
			&m_aEncodeCtx->ch_layout,
			m_aEncodeCtx->sample_fmt,
			m_aEncodeCtx->sample_rate,

			&m_aDecodeCtx->ch_layout,
			m_aDecodeCtx->sample_fmt,
			m_aDecodeCtx->sample_rate,
			0,
			nullptr
			);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to swr_alloc_set_opts2()! -- audio\n");
			return -1;
		}

		if (swr_init(this->m_swrCtx))
		{
			av_log(this->m_swrCtx, AV_LOG_ERROR, "Failed to initialize the swrContext! -- audio\n");
			return -1;
		}

	}

	//打开输出文件（写入权限）
	if (!(m_oFmtCtx->flags & AVFMT_NOFILE)) // 如果标志中没有设置这个 AVFMT_NOFILE，含义是：“这个格式需要一个文件句柄”
	{
		ret = avio_open2(&m_oFmtCtx->pb, this->m_filePath.c_str(), AVIO_FLAG_WRITE, NULL, NULL);//效果类似于 fopen()
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to avio_open2() the outputfile!\n");
			return -1;
		}
	}
	else
	{
		av_log(NULL, AV_LOG_WARNING, "success to open the output file handle!\n");
	}

	//if (this->m_oFmtCtx->streams[this->m_vOutIndex]->time_base.den != 30)
	//{
	//	std::cout << "不 == 30！" << std::endl;
	//}

	//写入文件头
	ret = avformat_write_header(this->m_oFmtCtx, nullptr);
	if (ret < 0)
	{
		av_log(m_oFmtCtx, AV_LOG_ERROR, "Failed to write_header to m_oFmtCtx!\n");
		return -1;
	}

	if (this->m_oFmtCtx->streams[this->m_vOutIndex]->time_base.den != 30)
	{
		std::cout << "不 == 30！" << std::endl;
	}

	return 0;
}

AVFrame* AVRecordImpl::AllocAudioFrame(AVCodecContext* c, int nbSamples)
{
	AVFrame* frame = nullptr;
	int ret = -1;

	frame = av_frame_alloc();
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc_newFrame_audio!\n");
		return nullptr;
	}

	//frame->ch_layout = c->ch_layout;
	ret = av_channel_layout_copy(&frame->ch_layout, &c->ch_layout);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to copy ch_layout to newFrame->ch_layout!\n");
		av_frame_free(&frame);
		return nullptr;
	}

	frame->sample_rate = c->sample_rate;
	frame->nb_samples = nbSamples;
	frame->format = c->sample_fmt;

	if (frame->nb_samples <= 0)
	{
		av_log(NULL, AV_LOG_ERROR, "The frame of AllocAudioFrame nb_samples <= 0!\n");
		av_frame_free(&frame);
		return nullptr;
	}

	ret = av_frame_get_buffer(frame, 0);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to av_frame_get_buffer() AllocAudioFrame !\n");
		av_frame_free(&frame);
		return nullptr;
	}

	return frame;
}

int AVRecordImpl::InitVideoBuffer()
{
	//之前的 m_vFifoBuf好像用不了 
	do {
		this->m_vOutFrameSize = av_image_get_buffer_size(this->m_vEncodeCtx->pix_fmt, m_vEncodeCtx->width, m_vEncodeCtx->height, 1);
		this->m_vOutFrameBuf = (uint8_t*)av_malloc(this->m_vOutFrameSize);
		//用于消费者
		this->m_vOutFrame = av_frame_alloc();
		int ret = av_image_fill_arrays( // 1、这里解释一下，其实就是根据：缓冲区大小、像素格式、宽高：来设置m_vOutFrame的data和linesize字段，方便使用
			this->m_vOutFrame->data,
			m_vOutFrame->linesize,
			this->m_vOutFrameBuf,
			this->m_vEncodeCtx->pix_fmt,
			m_vEncodeCtx->width,
			m_vEncodeCtx->height,
			1
		);

		this->m_vOutFrame->format = this->m_vEncodeCtx->pix_fmt;
		this->m_vOutFrame->width = this->m_vEncodeCtx->width;
		this->m_vOutFrame->height = this->m_vEncodeCtx->height; //编码之前，显式设置

		//这里为什么要创建这个，m_vOutFrameBuf？是因为yuv420p是 y\u\v 3部分数据，在消费端接收的时候，其实想要只调用一次 read函数就行了，所有需要一个
		//缓冲区直接接收 3部分数据，之后直接用 m_vOutFrame就好了，因为他俩绑定了，data[0\1\2] 和 linesize[0\1\2] 已经被设置了，嗯嗯

		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to fill_m_vOutFrame!\n");
			return -1;
		}

		//储存视频的共享缓冲区（不是队列，交给消费者的帧：顺序问题是由内部的环形缓冲区设计解决的）
		this->m_vFifoBuf = av_fifo_alloc2(30 * this->m_vOutFrameSize, 1, 0);
		if (!m_vFifoBuf)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to av_fifo_alloc2()!\n");
			return -1;
		}

		av_log(NULL, AV_LOG_DEBUG, "The m_vFifoBuf size: %d\n", av_fifo_can_write(m_vFifoBuf));
	} while (0);


	return 0;
}

int AVRecordImpl::InitAudioBuffer()
{
	this->m_nbSamples = this->m_aEncodeCtx->frame_size;
	if (m_nbSamples < 0)
	{
		av_log(m_aEncodeCtx, AV_LOG_ERROR, "The m_nbSamples is Failed,the reason is this->m_aEncodeCtx->frame_size ==%d\n", this->m_aEncodeCtx->frame_size);
		return -1;
	}
	// 30倍的缓存
	this->m_aFifoBuf = av_audio_fifo_alloc(m_aEncodeCtx->sample_fmt, m_aEncodeCtx->ch_layout.nb_channels, 30 * m_nbSamples);
	if (!m_aFifoBuf)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to av_audio_fifo_alloc()\n");
		return -1;
	}

	return 0;
}

void AVRecordImpl::FlushVideoDecoder()
{
	int ret = -1;
	int y_size = m_width * m_height;
	AVFrame* oldFrame = av_frame_alloc();
	AVFrame* newFrame = av_frame_alloc();

	ret = avcodec_send_packet(m_vDecodeCtx, nullptr);
	if (ret != 0)
	{
		std::cerr << "flush video avcodec_send_packet failed, ret: \n" << ret;
		return;
	}
	while (ret >= 0)
	{
		ret = avcodec_receive_frame(m_vDecodeCtx, oldFrame);
		if (ret < 0)
		{
			if (ret == AVERROR(EAGAIN))
			{
				std::cerr << "flush EAGAIN avcodec_receive_frame\n";
				ret = 1;
				continue;
			}
			else if (ret == AVERROR_EOF)
			{
				std::cerr << "flush video decoder finished\n";
				break;
			}
			std::cerr << "flush video avcodec_receive_frame error, ret: \n" << ret;
			return;
		}
		++g_vCollectFrameCnt;
		sws_scale(m_swsCtx, (const uint8_t* const*)oldFrame->data, oldFrame->linesize, 0,
			m_vEncodeCtx->height, newFrame->data, newFrame->linesize);

		{
			std::unique_lock<std::mutex> lk(m_mtxVBuf);
			m_cvVBufNotFull.wait(lk, [this] { return av_fifo_can_write(m_vFifoBuf) >= m_vOutFrameSize; });
		}
		av_fifo_write(m_vFifoBuf, newFrame->data[0], y_size);
		av_fifo_write(m_vFifoBuf, newFrame->data[1], y_size / 4);
		av_fifo_write(m_vFifoBuf, newFrame->data[2], y_size / 4);

		a++;
		m_cvVBufNotEmpty.notify_one();
	}
	
	printf("生产者（视频）解码器已刷新完毕！\n");
}

void AVRecordImpl::FlushAudioDecoder()
{
	int ret = -1;
	AVPacket pkt = { 0 };
	int dstNbSamples, maxDstNbSamples;
	AVFrame* rawFrame = av_frame_alloc();
	AVFrame* newFrame = AllocAudioFrame(m_aEncodeCtx, m_nbSamples);
	maxDstNbSamples = dstNbSamples = av_rescale_rnd(m_nbSamples,
		m_aEncodeCtx->sample_rate, m_aDecodeCtx->sample_rate, AV_ROUND_UP);

	ret = avcodec_send_packet(m_aDecodeCtx, nullptr);
	if (ret != 0)
	{
		std::cerr << "flush audio avcodec_send_packet  failed, ret: " << ret;
		return;
	}
	while (ret >= 0)
	{
		ret = avcodec_receive_frame(m_aDecodeCtx, rawFrame);
		if (ret < 0)
		{
			if (ret == AVERROR(EAGAIN))
			{
				std::cerr << "flush audio EAGAIN avcodec_receive_frame";
				ret = 1;
				continue;
			}
			else if (ret == AVERROR_EOF)
			{
				std::cerr << "flush audio decoder finished\n";
				break;
			}
			std::cerr << "flush audio avcodec_receive_frame error, ret: " << ret;
			return;
		}
		++g_aCollectFrameCnt;

		dstNbSamples = av_rescale_rnd(swr_get_delay(m_swrCtx, m_aDecodeCtx->sample_rate) + rawFrame->nb_samples,
			m_aEncodeCtx->sample_rate, m_aDecodeCtx->sample_rate, AV_ROUND_UP);
		if (dstNbSamples > maxDstNbSamples)
		{
			av_freep(&newFrame->data[0]);
			ret = av_samples_alloc(newFrame->data, newFrame->linesize, m_aEncodeCtx->ch_layout.nb_channels,
				dstNbSamples, m_aEncodeCtx->sample_fmt, 1);
			if (ret < 0)
			{
				std::cerr << "flush av_samples_alloc failed";
				return;
			}
			maxDstNbSamples = dstNbSamples;
			//m_aEncodeCtx->frame_size = dstNbSamples;
			m_nbSamples = newFrame->nb_samples;
		}
		newFrame->nb_samples = swr_convert(m_swrCtx, newFrame->data, dstNbSamples,
			(const uint8_t**)rawFrame->data, rawFrame->nb_samples);
		if (newFrame->nb_samples < 0)
		{
			std::cerr << "flush swr_convert failed";
			return;
		}

		{
			std::unique_lock<std::mutex> lk(m_mtxABuf);
			m_cvABufNotFull.wait(lk, [newFrame, this] { return av_audio_fifo_space(m_aFifoBuf) >= newFrame->nb_samples; });

			if (av_audio_fifo_write(m_aFifoBuf, (void**)newFrame->data, newFrame->nb_samples) < newFrame->nb_samples)
			{
				std::cerr << "av_audio_fifo_write\n";
				return;
			}
			m_cvABufNotEmpty.notify_one();
		}
	}
	printf("生产者（音频）解码器已刷新完毕！\n");
}

void AVRecordImpl::PrintAVCodecContext(const AVCodecContext* ctx) {
	if (!ctx) {
		av_log(NULL, AV_LOG_ERROR, "AVCodecContext is null!\n");
		return;
	}

	av_log(NULL, AV_LOG_INFO, "===== AVCodecContext Parameters =====\n");
	av_log(NULL, AV_LOG_INFO, "Codec Name       : %s\n", ctx->codec ? ctx->codec->name : "NULL");
	av_log(NULL, AV_LOG_INFO, "Codec Type       : %d (%s)\n", ctx->codec_type, av_get_media_type_string(ctx->codec_type));
	av_log(NULL, AV_LOG_INFO, "Codec ID         : %d\n", ctx->codec_id);
	av_log(NULL, AV_LOG_INFO, "Width x Height   : %d x %d\n", ctx->width, ctx->height);
	av_log(NULL, AV_LOG_INFO, "Format (pix_fmt) : %d (%s)\n", ctx->pix_fmt, av_get_pix_fmt_name(ctx->pix_fmt));
	av_log(NULL, AV_LOG_INFO, "Bitrate          : %" PRId64 "\n", ctx->bit_rate);
	av_log(NULL, AV_LOG_INFO, "Timebase         : %d/%d\n", ctx->time_base.num, ctx->time_base.den);
	av_log(NULL, AV_LOG_INFO, "Framerate        : %d/%d\n", ctx->framerate.num, ctx->framerate.den);
	av_log(NULL, AV_LOG_INFO, "GOP Size         : %d\n", ctx->gop_size);
	av_log(NULL, AV_LOG_INFO, "Max B-frames     : %d\n", ctx->max_b_frames);
	av_log(NULL, AV_LOG_INFO, "Thread Count     : %d\n", ctx->thread_count);
	av_log(NULL, AV_LOG_INFO, "Profile          : %d\n", ctx->profile);
	av_log(NULL, AV_LOG_INFO, "Level            : %d\n", ctx->level);
	av_log(NULL, AV_LOG_INFO, "Flags            : 0x%X\n", ctx->flags);
	av_log(NULL, AV_LOG_INFO, "=====================================\n");
}

/*
* 1、这里解释一下：av_image_fill_arrays() 这个API是用来填充 data 和linesize 字段的，根据传进去的 pix_fmt 以及 buffer，那为什么需要这个呢？
* frame 本身是具有字段和缓冲区的结构体，但是这里的 m_voutFrameBuffer仅仅是一个 缓冲区，没有任何字段，直接交给编码器绝对不行，因为没有字段去
* 描述，这篇数据到底是什么格式，所以这个API会根据你传输进去的 frame 和 outBuffer，来确定data字段、linesize字段，数据存放在 outBuffer里，但是
* frame 的data 有着outbuffer 数据的指针，也就是手动构建了一个 frame 内部机制中的数据缓冲区，大概就是这个意思
* 
* 但是这里明显是为了存放原始视频数据的，送给编码器之前会处理好它的字段的，这里用这个API我搞不懂？也就是我感觉没必要
* 
*/