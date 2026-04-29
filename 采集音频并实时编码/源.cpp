#define SDL_MAIN_HANDLED
#include <iostream>
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <SDL.h>                      
}

int main()
{
	av_log_set_level(AV_LOG_DEBUG);
	SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO);

	std::cout << "ÄãºÃ" << std::endl;

	av_log(NULL, AV_LOG_WARNING, "hello,ffmpeg!\n");


	return 0;
}