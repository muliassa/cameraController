

// FFmpeg C API headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}


class someFFMpeg {
public:
	static void saveAVFrameAsJPEG(AVFrame *frame, string path, int quality);
}
