#ifndef ZCAM_H
#define ZCAM_H

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

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
// using namespace nlohmann;

class ZCAM {


	string root;
    string camera_ip;
    string camera_id;
    string rtsp_url;
    string http_base_url;

    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    int video_stream_index = -1;

	bool detectVideoStream();

public:

    ZCAM(const nlohmann::json& config, const int cam_idx);
    ~ZCAM();
    void initStream();
    void closeStream();
    AVFrame* getFrame();
    bool captureFrame(vector<uint8_t>& rgb_data, int& width, int& height);
    void cleanup();
};

#endif