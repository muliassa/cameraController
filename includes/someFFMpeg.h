
#ifndef SOME_FFMPEG_H
#define SOME_FFMPEG_H

#include <string>

// FFmpeg C API headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

using namespace std;

class someFFMpeg {
public:
	static void saveAVFrameAsJPEG(AVFrame *frame, string path, int quality);
};

#endif 
