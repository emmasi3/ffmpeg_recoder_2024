#include "audiorecorder.h"

int g_aCollectFrameCnt = 0;	//音频采集帧数
int g_aEncodeFrameCnt = 0;	//音频编码帧数

std::string AudioRecorder::WideCharToUtf8(const std::wstring& wstr)
{
	if (wstr.empty()) return "";

	int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
		NULL, 0, NULL, NULL);
	std::string result(sizeNeeded - 1, 0); // -1 去掉 null terminator
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
		&result[0], sizeNeeded, NULL, NULL);
	return result;
}

AudioRecorder::AudioRecorder() :
	m_aFmtCtx(nullptr), m_oFmtCtx(nullptr), m_aDecodeCtx(nullptr),
	m_aEncodeCtx(nullptr), m_swrCtx(nullptr), m_aFifoBuf(nullptr),
	m_vOutFrame(nullptr), m_state(RecordState::NotStarted),m_aCurPts(0)
{
	std::cout << "AudioRecorder 开启了！" << std::endl;
}

void AudioRecorder::Init()
{
	SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	av_log_set_level(AV_LOG_WARNING);

	//设置实时输出的本地录音文件存放地址
	this->m_filePath = "./new/test1.aac";
	//this->m_filePath = "rtmp://127.0.0.1:1935/live/test001"; // 这样的话，就是推流了，往nginx服务器上推流，然后用VLC拉流播放，延迟是 6s 左右，但是这个延迟有疑问，缓存一部分播放本身就占用了一点时间，所以可能实际上从本地麦克风采集并推流到服务器上，拉流到本地播放用不了那么上时间
	this->m_audioBitrate = 192000; // 中高音质
	//初始化麦克风
	if (OpenAudio() < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "this OpenAudio() is failed\n");
		return;
	}
	// 打开输出文件或输出流（准备好解码、编码、重采样的一切事宜！）
	if (OpenOutput() < 0)
	{
		return;
	}
	//初始化共享队列
	if (InitAudioBuffer() < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to InitAudioBuffer!\n");
		return;
	}

	//开启线程（生产、消费）
	this->Start();
}

void AudioRecorder::Start()
{
	if (m_state == RecordState::NotStarted)
	{
		m_state = RecordState::Started;

		//消费者
		std::thread muxThread(&AudioRecorder::MuxThreadProc, this);
		//存放线程句柄（只能用移动的方式，thread不允许复制）
		Threads.emplace_back(std::move(muxThread));
	}


	//启动SDL事件监控
	sdl_event_work();
}

void AudioRecorder::sdl_event_work()
{
	SDL_Window* window = SDL_CreateWindow("Invisible Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480, SDL_WINDOW_SHOWN);
	if (!window) {
		SDL_Log("Window could not be created! SDL_Error: %s\n", SDL_GetError());
		return;
	}

	SDL_Event event;
	do
	{
		SDL_WaitEvent(&event);

		switch (event.type)
		{
		case SDL_QUIT:
			av_log(NULL, AV_LOG_WARNING, "the key is : NULL,but forced to QUIT!\n");
			Stop();
			break;
		case SDL_KEYDOWN:
			if (event.key.keysym.sym == SDLK_q)
			{
				av_log(NULL, AV_LOG_WARNING, "the key is SDLK_q,so QUIT!\n");
				Stop();
			}
			break;
		default:
			av_log(NULL, AV_LOG_INFO, "the key is Invilid\n");
			break;
		}

	} while (this->m_state != RecordState::Stopped);

	SDL_DestroyWindow(window);
	SDL_Quit();
}

void AudioRecorder::Stop()
{
	av_log(NULL, AV_LOG_WARNING, "the AudioRecorderState is Stopped!\n");
	m_state = RecordState::Stopped;

	//等待所以线程结束
	for (auto &it : Threads)
	{
		if (it.joinable())
		{
			it.join();
		}
	}
	av_log(NULL, AV_LOG_WARNING, "the each of threads is already exit!\n");

	//释放所有资源
	Release();
	av_log(NULL, AV_LOG_WARNING, "the source of this class is free!\n");
}

void AudioRecorder::MuxThreadProc()
{
	int ret = -1;
	bool Is_first_pts = true;
	int aFrameIndex = 0;
	bool done = false;
	AVPacket* pkt = nullptr;
	AVFrame* frame = nullptr;

	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory to MuxThreadProc() pkt!\n");
		return;
	}

	frame = AllocAudioFrame(m_aEncodeCtx, m_nbSamples);
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to av_frame_get_buffer() to MuxThreadProc() frame!\n");
		return;
	}

	if (av_frame_make_writable(frame) < 0) {
		printf("Frame is not writable\n");
	}


	//在这里开启生产者，是因为 notify_one() 的通知可能会丢失，所以选择让消费者进入 wait() 阻塞状态 以此来等待
	std::thread soundRecord(&AudioRecorder::SoundRecordThreadProc, this);
	Threads.emplace_back(std::move(soundRecord));

	//读取-->编码-->封装-->文件尾-->释放
	while (true)
	{
		//处理已经暂停但是 AudioFifoBuf 中还有数据的情况
		if (m_state == RecordState::Stopped && !done)
			done = true;

		if (done)
		{
			std::lock_guard<std::mutex> uu(m_mtxABuf);
			int a = av_audio_fifo_size(m_aFifoBuf);
			if (a < this->m_nbSamples)
			{
				av_log(NULL, AV_LOG_WARNING, "audioFifoBuf read done! a = %d\n", a);
				break;
			}

		}
		//从共享队列中读取数据-->frame
		{
			std::unique_lock<std::mutex> u(m_mtxABuf);

			printf("第1次消费者开始阻塞！\n");

			printf("BeforeBefore!!! read: fifo_size=%d, space=%d\n",
				av_audio_fifo_size(m_aFifoBuf),
				av_audio_fifo_space(m_aFifoBuf));

			this->m_cvABufNotEmpty.wait(u,  // 不空，就开始取数据
				[this]
				{return av_audio_fifo_size(m_aFifoBuf) >= this->m_nbSamples ; }
			);

			//int a = av_audio_fifo_space(m_aFifoBuf);
			//printf("消费之前 audio_fifo_space: %d \n", a);
			printf("Before read: fifo_size=%d, space=%d\n",
				av_audio_fifo_size(m_aFifoBuf),
				av_audio_fifo_space(m_aFifoBuf));

			//读取数据
			ret = av_audio_fifo_read(this->m_aFifoBuf, (void* const*)frame->data, frame->nb_samples);
			if (ret < 0)
			{
				av_log(m_aFifoBuf, AV_LOG_ERROR, "av_audio_fifo_read() Error!\n");
				break;
			}

			printf("After read: ret=%d, fifo_size=%d\n", ret, av_audio_fifo_size(m_aFifoBuf));

			/*printf("消费了一次后，剩余的样本数：%d \n", av_audio_fifo_space(m_aFifoBuf));*/
		}

		this->m_cvABufNotFull.notify_one();

		frame->pts = m_nbSamples * aFrameIndex++;

		//发送给编码器
		char err_buf[AV_ERROR_MAX_STRING_SIZE] = { 0 };

		printf("sample_fmt = %s\n", av_get_sample_fmt_name(m_aEncodeCtx->sample_fmt));
		printf("channel_layout = %" PRId64 "\n", m_aEncodeCtx->ch_layout);
		printf("channels = %d\n", m_aEncodeCtx->ch_layout.nb_channels);
		printf("sample_rate = %d\n", m_aEncodeCtx->sample_rate);
		printf("frame->nb_samples = %d\n", frame->nb_samples);
		printf("codecCtx->frame_size = %d\n", m_aEncodeCtx->frame_size);
		printf("format = %s\n", m_oFmtCtx->oformat->name);

		ret = avcodec_send_frame(this->m_aEncodeCtx, frame);
		if (ret < 0)
		{
			av_strerror(ret, err_buf, sizeof(err_buf));
			printf("avcodec_send_frame failed: %s\n", err_buf);
			printf("frame->format = %d, expected = %d\n", frame->format, m_aEncodeCtx->sample_fmt);
			printf("frame->nb_samples = %d, expected = %d\n", frame->nb_samples, m_aEncodeCtx->frame_size);
			printf("frame->channels = %d\n", frame->ch_layout.nb_channels);

			//av_frame_unref(frame);
			av_frame_make_writable(frame);
			av_samples_set_silence(
				frame->data,
				0,                         // 起始样本
				frame->nb_samples,         // 总样本
				frame->ch_layout.nb_channels,
				(AVSampleFormat)frame->format
			);
			std::cout << "send_frame 函数执行了1次" << std::endl;
			continue;
		}
		//接收数据
		ret = avcodec_receive_packet(this->m_aEncodeCtx, pkt);
		if (ret < 0)
		{
			//av_frame_unref(frame);
			av_frame_make_writable(frame);
			av_samples_set_silence(
				frame->data,
				0,                          // 起始样本
				frame->nb_samples,         // 总样本
				frame->ch_layout.nb_channels,
				(AVSampleFormat)frame->format
			);

			av_packet_unref(pkt);
			continue;
		}

		//设置pkt包的各种参数（便于编码器处理）
		//stream_id
		pkt->stream_index = this->m_aOutIndex;
		this->m_aCurPts = pkt->pts;
		pkt->dts = pkt->pts;


		// 将“编码器”时间基 转换为“封装格式”时间基，其实就是方便单位等价换算
		av_packet_rescale_ts(
			pkt,
			m_aEncodeCtx->time_base,
			m_oFmtCtx->streams[m_aOutIndex]->time_base
		);

		std::cout << "aCurPts: " << m_aCurPts << std::endl;


		//写入文件内容
		ret = av_interleaved_write_frame(this->m_oFmtCtx, pkt);
		if (ret < 0)
		{
			av_log(m_oFmtCtx, AV_LOG_ERROR, "av_interleaverd_write_frame() is Failed!,ret = %d\n", ret);
			break;
		}
		else
		{
			av_log(m_oFmtCtx, AV_LOG_WARNING, "Write audio packet id:%d\n", ++g_aEncodeFrameCnt);
		}

		//av_frame_unref(frame);
		av_frame_make_writable(frame);
		av_samples_set_silence(
			frame->data,
			0,                          // 起始样本
			frame->nb_samples,        // 总样本
			frame->ch_layout.nb_channels,
			(AVSampleFormat)frame->format
		);

		av_packet_unref(pkt);
	}
	FlushEncoders();
	//写入文件尾
	av_write_trailer(this->m_oFmtCtx);

	if (frame)
		av_frame_free(&frame);
	av_packet_free(&pkt);

	printf("文件尾已写入!\n");
	return;
}
//生产者（采集麦克风-->重采样-->扔到 FIFO 共享队列中）
void AudioRecorder::SoundRecordThreadProc()
{
	int ret = -1;
	AVPacket* pkt = nullptr;
	AVFrame* rawframe = nullptr;
	AVFrame* newFrame = nullptr;
	int dstNbSamples = 0, maxNbSamples = 0;

	FILE* rawDump = fopen("./new/test.pcm", "wb");
	if (rawDump)
	{
		av_log(NULL, AV_LOG_WARNING, "The Pcm is open!\n");
	}

	//这个函数根据“比例关系”重新校准第一个参数
	dstNbSamples = maxNbSamples = av_rescale_rnd(m_nbSamples, m_aEncodeCtx->sample_rate, m_aDecodeCtx->sample_rate, AV_ROUND_UP);
	
	pkt = av_packet_alloc();
	rawframe = av_frame_alloc();

	newFrame = this->AllocAudioFrame(this->m_aEncodeCtx, this->m_nbSamples);
	if (!newFrame)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc memory to newFrame!\n");
		return;
	}

	/////////
	while (m_state != RecordState::Stopped)
	{
		//从麦克风的缓冲区读取数据，只要没有停止标志，一直读取
		if (av_read_frame(this->m_aFmtCtx, pkt) < 0)
		{
			av_log(pkt, AV_LOG_WARNING, "audio av_read_frame < 0\n");
			continue;
		}
		//这里本应该加上 pkt->stream_index 是否为目标流iD的，但是鉴于这里是麦克风录制的，所以默认就只有一路音频流而已，但是直播时需要区分
		if (avcodec_send_packet(this->m_aDecodeCtx, pkt) < 0)
		{
			av_log(pkt, AV_LOG_WARNING, "audio av_send_frame < 0\n");
			av_packet_unref(pkt);
			continue;
		}

		if (avcodec_receive_frame(this->m_aDecodeCtx, rawframe) < 0)
		{
			av_log(rawframe, AV_LOG_WARNING, "audio av_receive_frame < 0\n");
			av_packet_unref(pkt);
			continue;
		}
		//重采样
		++g_aCollectFrameCnt; //采集帧数+1

		do
		{
			/// 计算 目标采样点数（由于重采样需要时间，所以有延迟，此延迟以采样点为单位，重新计算，以保证重采样器输出数据最大，
			/// 判断是否需要分配更大的 newFrame->data？）
			dstNbSamples = av_rescale_rnd(
				swr_get_delay(this->m_swrCtx, m_aDecodeCtx->sample_rate) + rawframe->nb_samples,
				m_aEncodeCtx->sample_rate,
				m_aDecodeCtx->sample_rate,
				AV_ROUND_UP
			);
			if (dstNbSamples > maxNbSamples)
			{
				if (newFrame) {
					av_frame_free(&newFrame);
					newFrame = this->AllocAudioFrame(this->m_aEncodeCtx, dstNbSamples);
				}
			}
			maxNbSamples = dstNbSamples;

			newFrame->nb_samples = swr_convert(
				this->m_swrCtx,
				(uint8_t**)newFrame->data,
				dstNbSamples,
				(const uint8_t**)rawframe->data,
				rawframe->nb_samples);

			if (newFrame->nb_samples < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "the swr_convert() is Failed!\n");
				return;
			}

		} while (0);

		////写入pcm来看看音频对不对
		int bytesPerSample = av_get_bytes_per_sample((AVSampleFormat)newFrame->format);
		for (int ch = 0; ch < newFrame->ch_layout.nb_channels-1; ++ch)
		{
			fwrite(newFrame->data[ch], 1, newFrame->nb_samples * bytesPerSample * 2, rawDump);
		}


		//写入 m_FifoBuf
		{
			std::unique_lock<std::mutex> u(this->m_mtxABuf);
			//“不满”的话，就继续添加

			printf("第1次到达不满的 wait处！\n");

			int space = av_audio_fifo_space(m_aFifoBuf);

			printf("生产者比较结果-->m_state == RecordState::Stopped ::::: %d\n", m_state == RecordState::Stopped);

			printf("Before write!!!: fifo_size=%d, space=%d,newFrame->nb_samples= %d\n",
				av_audio_fifo_size(m_aFifoBuf),
				av_audio_fifo_space(m_aFifoBuf), newFrame->nb_samples);

			this->m_cvABufNotFull.wait(
				u, [this, newFrame] {return av_audio_fifo_space(m_aFifoBuf) >= newFrame->nb_samples || m_state == RecordState::Stopped; }
			);

			printf("第一次执行完不满的wait()处!\n");

			// 这里传递 nb_samples 一个声道的采样点数，合理，因为在 av_audio_fifo_alloc() 时，队列已经知道了音频的声道布局，所以它内部会处理的
			// 不会因为多个声道数在这里导致音频数据没写完
			ret = av_audio_fifo_write(m_aFifoBuf, (void* const*)newFrame->data, newFrame->nb_samples);
			if (ret < 0)
			{
				av_log(m_aFifoBuf, AV_LOG_ERROR, "Failed to write newFrame to m_aFifoBuf!\n");
				return;
			}
		}
		//通知一个（可以取了）“不空”
		this->m_cvABufNotEmpty.notify_one();

		av_log(NULL, AV_LOG_WARNING, "audio_write to audio_fifo nb_samples=%d\n", ret);

		printf("生产者通知了一次\n");

		av_log(m_oFmtCtx, AV_LOG_WARNING, "sound Write audio packet id:%d\n",g_aCollectFrameCnt);

		av_packet_unref(pkt);
		av_frame_unref(rawframe);
		//av_frame_unref(newFrame); 
		av_frame_make_writable(newFrame);
		av_samples_set_silence(
			newFrame->data,
			0,
			newFrame->nb_samples,
			newFrame->ch_layout.nb_channels,
			(AVSampleFormat)newFrame->format
			);

	}//循环：pkt-->rawframe-->newFrame-->m_aFifoBuf
	

	//读取解码器中剩余的数据
	FlushAudioDecoder();

	//
	fclose(rawDump);

	av_packet_free(&pkt);
	av_frame_free(&rawframe);
	if (newFrame)
		av_frame_free(&newFrame);

	av_log(NULL, AV_LOG_WARNING, "the thread of Sound exit!\n");
	return;
}
//给重采样的Frame分配空间
AVFrame* AudioRecorder::AllocAudioFrame(AVCodecContext* c, int nbSamples)
{
	int ret = -1;
	AVFrame* frame = av_frame_alloc();
	if (!frame)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory to alloc to newframe!\n");
		return nullptr;
	}
	
	/*frame->ch_layout = c->ch_layout;*/

	ret = av_channel_layout_copy(&frame->ch_layout, &c->ch_layout);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to copy channel layout");
		av_frame_free(&frame);
		return nullptr;
	}

	frame->sample_rate = c->sample_rate;
	frame->nb_samples = nbSamples;
	frame->format = c->sample_fmt;

	if (nbSamples == 0)
	{
		return frame;
	}

	ret = av_frame_get_buffer(frame, 0);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to av_frame_get_buffer() to newFrame!\n");
		av_frame_free(&frame);
		return nullptr;
	}

	return frame;
}
//打开麦克风and解码器（麦克风的）
int AudioRecorder::OpenAudio()
{
	int ret = -1;
	const AVCodec* codec = nullptr;
	AVStream* stream = nullptr;
	avdevice_register_all();

	std::wstring mkname = L"audio=麦克风 (Realtek(R) Audio)";
	std::string audioDeviceName = this->WideCharToUtf8(mkname);
	
	//这一步的分配不是必须的，会在avformat_opne_input()中自动调用 alloc() 函数
	this->m_aFmtCtx = avformat_alloc_context();
	
	//这里是指定输入格式
	const AVInputFormat* ifmt = av_find_input_format("dshow");
	if (!ifmt)
	{
		printf("the AVInputFormat *ifmt failed!\n");
		return -1;
	}
	//打开麦克风设备录音
	ret = avformat_open_input(&m_aFmtCtx, audioDeviceName.c_str(), ifmt, nullptr);
	if (ret < 0)
	{
		av_log(this->m_aFmtCtx, AV_LOG_ERROR, "the avformat_open_input() error:\n");
		return -1;
	}
	//填充avforamt中的streams流信息
	ret = avformat_find_stream_info(this->m_aFmtCtx, nullptr);
	if (ret < 0)
	{
		av_log(this->m_aFmtCtx, AV_LOG_ERROR, "avformat_find_stream_info() error:\n");
		return -1;
	}
	//根据流信息寻找音频流（一般麦克风就是 0 ，第一路流）
	this->m_aIndex;
	for (int i = 0; i < this->m_aFmtCtx->nb_streams; i++)
	{
		stream = this->m_aFmtCtx->streams[i];
		if (stream->codecpar->codec_id == AVMEDIA_TYPE_AUDIO)
		{
			this->m_aIndex = i;
			av_log(NULL, AV_LOG_DEBUG, "the stream[%d] is selected of needed!\n", this->m_aIndex);
			break;
		}
	}

	//确定解码器
	codec = avcodec_find_decoder(this->m_aFmtCtx->streams[this->m_aIndex]->codecpar->codec_id);
	if (!codec)
	{
		av_log(NULL, AV_LOG_ERROR, "the codec of mkfeng is failed!\n");
		return -1;
	}

	//分配解码器上下文
	this->m_aDecodeCtx = avcodec_alloc_context3(codec);
	if (!m_aDecodeCtx)
	{
		av_log(NULL, AV_LOG_ERROR, "the codecctx of mkfeng is failed!\n");
		return -1;
	}
	//填充解码上下文的，解码器参数
	ret = avcodec_parameters_to_context(this->m_aDecodeCtx, stream->codecpar);
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "the codecctx of mkfeng is failed!\n");
		return -1;
	}

	//打开解码器
	if (avcodec_open2(this->m_aDecodeCtx, codec, nullptr) < 0)
	{
		av_log(this->m_aDecodeCtx, AV_LOG_ERROR, "Failed to open audioDevice! avcodec_open2()\n");
		return -1;
	}

	return 0;
}
//输出文件格式上下文的配置（以及重采样的初始化,RTMP推流的准备）
int AudioRecorder::OpenOutput()
{
	int ret = -1;
	AVStream* astream = nullptr;
	const AVCodec* encoder = nullptr;
	bool bIsRtmp = false; //是否RTMP直播推流
	if (m_filePath.find("rtmp://") != std::string::npos)
	{
		bIsRtmp = true;
	}

	//分配输出格式上下文
	//（这里的flv为什么不给到第2个参数（看看参数类型不一致，用不了）？后三个参数的作用都是用来填充 oformat 这个字段的）
	ret = avformat_alloc_output_context2(&this->m_oFmtCtx, nullptr, bIsRtmp ? "flv" : nullptr, m_filePath.c_str());
	if (ret < 0)
	{
		av_log(NULL, AV_LOG_ERROR, "avformat_alloc_output_context2() is error:m_oFmtCtx\n");
		return -1;
	}

	if (this->m_aFmtCtx->streams[m_aIndex]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
	{
		//为输出文件创建“一路新流”（可以是音频流、视频流、字幕流），这个API直接是将 astream 加到了 m_oFmtCtx->streams[] 数组中，也就是返回的
		//是二级指针，你对astream做的修改，都会映射到 m_oFmtCtx 中，以便于接收后续的编码器所有参数
		astream = avformat_new_stream(this->m_oFmtCtx, nullptr);
		if (!astream)
		{
			av_log(NULL, AV_LOG_ERROR, "the astream is failed!(100-200-->line)\n");
			return -1;
		}

		this->m_aOutIndex = astream->index;//记录一下

		//查找编码器
		if (this->m_oFmtCtx->oformat->audio_codec == AV_CODEC_ID_MP3)
		{
			encoder = avcodec_find_encoder_by_name("libmp3lame");
			if (!encoder)
			{
				av_log(NULL, AV_LOG_ERROR, "not find the libmp3lame!!!\n");
				return -1;
			}
		}
		else
		{
			//encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
			encoder = avcodec_find_encoder_by_name("libfdk_aac");
			if (!encoder)
			{
				av_log(NULL, AV_LOG_ERROR, "not find the libmp3lame!!!\n");
				return -1;
			}
		}
		if(!encoder)
			encoder = avcodec_find_encoder(this->m_oFmtCtx->audio_codec_id);

		//分配编码器格式上下文
		this->m_aEncodeCtx = avcodec_alloc_context3(encoder);
		if (!m_aEncodeCtx)
		{
			av_log(NULL, AV_LOG_ERROR, "the m_aEncodeCtx alloc context3 is failed!\n");
			return -1;
		}

		//设置编码器参数（必须的是 5个：sample_fmt、sample_rate、ch_layout->nb_channels、ch_layout、bit_rate）
		//但是这里的API av_channel_layout_default() 初始化 ch_layout 这个结构体，声道数、声道布局就一块儿设置了，所以是4行代码

		m_aEncodeCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // 这个fltp--高精度浮点型采样格式---是ffmpeg自带的 AAC 编解码器唯一支持的采样格式
		m_aEncodeCtx->sample_rate = 44100;
		av_channel_layout_default(&m_aEncodeCtx->ch_layout, 2); // ← 先设声道布局（自动设置 nb_channels）
		m_aEncodeCtx->bit_rate = this->m_audioBitrate;
		//检查编码器是否支持该采样格式,libfdk_aac仅仅支持 s16，而mp3支持的最多，其中就有fltp

		std::string name_encoder = encoder->name;
		if (name_encoder == "libfdk_aac")
		{
			m_aEncodeCtx->sample_fmt = AV_SAMPLE_FMT_S16;
		}
		else if(name_encoder == "libmp3lame")
		{
			m_aEncodeCtx->sample_fmt = AV_SAMPLE_FMT_S32P;
		}

		m_aEncodeCtx->codec_tag = 0; // 这个字段的作用：用来标识具体的轨道信息（一般是编码信息）
		m_aEncodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // 用来表明，将头信息写入 extardata中，而不是每一帧都写头信息，全局头信息

		//打开编码器，，，这里我发现：即使你在上面给 上下文设置的采样格式和encoder有出入。。这里也会直接给你修改过来
		ret = avcodec_open2(this->m_aEncodeCtx, encoder, nullptr);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "failed to open the encoder of audio!\n");
			return -1;
		}

		//将这些必要信息都给到 输出格式上下文 m_oFmtCtx 中
		astream->time_base = AVRational({ 1,this->m_aEncodeCtx->sample_rate });
		ret = avcodec_parameters_from_context(astream->codecpar, m_aEncodeCtx);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "avcodec_parameters_from_context(astream->codecpar, m_aEncodeCtx) is failed!\n");
			return -1;
		}

		//初始化重采样（参数）
		do
		{
			AVChannelLayout in_ch_layout = this->m_aDecodeCtx->ch_layout;
			AVChannelLayout out_ch_layout = this->m_aEncodeCtx->ch_layout;
			swr_alloc_set_opts2(
				&this->m_swrCtx,
				&out_ch_layout,
				m_aEncodeCtx->sample_fmt,
				m_aEncodeCtx->sample_rate,
				&in_ch_layout,
				m_aDecodeCtx->sample_fmt,
				m_aDecodeCtx->sample_rate,
				0,nullptr
				);

			if (swr_init(this->m_swrCtx) < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "swr_init(m_swrCtx) failed!\n");
				return -1;
			}

		} while (0);

		//打开输出文件（写入权限）
		if (!(m_oFmtCtx->flags & AVFMT_NOFILE)) // 如果标志中没有设置这个 AVFMT_NOFILE，含义是：“这个格式不需要一个文件句柄”
		{
			if (avio_open2(&m_oFmtCtx->pb, this->m_filePath.c_str(), AVIO_FLAG_WRITE, NULL, NULL) < 0)
			{
				av_log(NULL, AV_LOG_ERROR, "Failed to avio_open2() the outputfile!\n");
				return -1;
			}
		}
		else
		{
			av_log(NULL, AV_LOG_WARNING, "success to open the output file handle!\n");
		}

		//写入文件头
		ret = avformat_write_header(this->m_oFmtCtx, nullptr);
		printf("%s\n", m_oFmtCtx->oformat->name);
		if (ret < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "Failed to write header to the outputfile!\n");
			return -1;
		}
	}


	return 0;
}
//初始化共享队列
int AudioRecorder::InitAudioBuffer()
{
	m_nbSamples = m_aEncodeCtx->frame_size; // 一个音频帧的采样点个数（1个声道的）
	if (!m_nbSamples)
	{
		av_log(NULL, AV_LOG_WARNING, "the m_aDecodeCtx->frame_size is NULL!!!\n");
		m_aEncodeCtx->frame_size = 1024;
		m_nbSamples = 1024;
	}
	if (this->m_aEncodeCtx->codec->name == "libmp3lame")
	{
		m_aEncodeCtx->frame_size = 1152;
		m_nbSamples = 1152;
	}

	//这里存储的是解码（解码器）并且“重采样（用的是编码器的参数）”麦克风采集的 PCM 原始数据
	this->m_aFifoBuf = av_audio_fifo_alloc(m_aEncodeCtx->sample_fmt, m_aEncodeCtx->ch_layout.nb_channels, 60 * m_nbSamples);
	if (!m_aFifoBuf)
	{
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc FifoBuf!\n");
		return -1;
	}

	return 0;
}
//最后一个解码器包
void AudioRecorder::FlushAudioDecoder()
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
			printf("生产者的最后一个解码包通知了一次\n");
		}
	}
	printf("第1次到达刷新“解码包”的 结尾处！\n");
}
//最后一个编码包
void AudioRecorder::FlushEncoders()
{
	int ret = -1;
	bool vBeginFlush = false;
	bool aBeginFlush = false;

	m_aCurPts = 0;

	int nFlush = 2;

	AVPacket* pkt = nullptr;
	pkt = av_packet_alloc();
	if (!pkt)
	{
		av_log(NULL, AV_LOG_ERROR, "no memory to pkt of FlushEncoders()!\n");
		return;
	}

	while (1)
	{
		if (!aBeginFlush)
		{
			aBeginFlush = true;
			ret = avcodec_send_frame(m_aEncodeCtx, nullptr);
			if (ret != 0)
			{
				std::cout << "flush audio avcodec_send_frame failed, ret: \n" << ret;
				return;
			}
		}
		ret = avcodec_receive_packet(m_aEncodeCtx, pkt);
		if (ret < 0)
		{
			av_packet_unref(pkt);
			if (ret == AVERROR(EAGAIN))
			{
				std::cout << "flush EAGAIN avcodec_receive_packet\n";
				ret = 1;
				continue;
			}
			else if (ret == AVERROR_EOF)
			{
				std::cout << "flush audio encoder finished\n";
				/*break;*/
				if (!(--nFlush))
					break;
				m_aCurPts = INT_MAX;
				continue;
			}
			std::cout << "flush audio avcodec_receive_packet failed, ret: \n" << ret;
			return;
		}
		pkt->stream_index = m_aOutIndex;
		//将pts从编码层的timebase转成复用层的timebase
		av_packet_rescale_ts(pkt, m_aEncodeCtx->time_base, m_oFmtCtx->streams[m_aOutIndex]->time_base);
		m_aCurPts = pkt->pts;
		std::cout << "m_aCurPts: \n" << m_aCurPts;
		ret = av_interleaved_write_frame(m_oFmtCtx, pkt);
		if (ret == 0)
			std::cout << "flush write audio packet id: \n" << ++g_aEncodeFrameCnt;
		else
			std::cout << "flush audio av_interleaved_write_frame failed, ret: \n" << ret;
		av_packet_unref(pkt);
	}

	av_packet_free(&pkt);
	return;
}

void AudioRecorder::Release()
{
	if (this->m_aDecodeCtx)
		avcodec_free_context(&m_aDecodeCtx);
	if (this->m_aEncodeCtx)
		avcodec_free_context(&m_aEncodeCtx);
	if (this->m_aFifoBuf)
		av_audio_fifo_free(m_aFifoBuf);
	if (this->m_aFmtCtx)
	{
		avformat_close_input(&m_aFmtCtx);
		avformat_free_context(m_aFmtCtx);
	}
	if (this->m_oFmtCtx)
	{
		avio_close(m_oFmtCtx->pb);
		avformat_free_context(m_oFmtCtx);
	}
	if (this->m_swrCtx)
		swr_free(&m_swrCtx);

}

/*
* 1、codecCtx->frame_size是一个音频帧的采样点个数，不是该音频帧大小（1个声道）
* 
* 2、重采样的步骤中，用了 swr_get_delay() 这个函数，来计算播放“不卡顿”的“最低”“输出采样数”：也就是重采样器每次输出的采样数需要不低于这个结果才能在理论上不卡顿
* 这个函数也是根据“比例关系”去计算的，举例：假设现在接收重采样后的数据的缓冲区 newFrame 的data域的采样数为：1024，输入采样率44100，输出采样率48000
* 那么计算出的 “最终输出采样数”--> 1111，也就是每次重采样器至少要输出1111个采样数才不会卡顿（理论），而这里的 1024 < 1111，所以必须扩充newFrame的data
* 空间，也就是重新分配更大的
* 
* 那么倘若反过来呢 1111 > 1024呢，很显然，就不需要额外增加了，因为空间够大，输出够多
* 
* 但是为什么在 swr_get_delay()还要 + rawFrame->nb_samples呢？都说了 1111 是最低的界限，大一点也没什么，这是一点
* 第二点：不知道，这些理论有点错误吧，懒得看了，反正你就知道这一步不是必须的就行了
* 
* 3、av_frame_unref() 这个API并不对像delete那样直接释放掉 通过av_frame_get_buffer() 分配的空间，而仅仅是像memset那样重置这个内容，
* 所以不用担心通过 av_frame_get_buffer() 分配的内存空间会被丢掉
* 
*/