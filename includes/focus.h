#ifndef FOCUS_H
#define FOCUS_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <algorithm>

// FFmpeg headers for direct API usage
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>     // Add scaling support
#include <libavutil/opt.h>          // Add this line for av_opt_set
}

using namespace std;

class Focus {

    SwsContext* swsContext = nullptr;

public:
    enum Method {
        LAPLACIAN,
        SOBEL,
        BRENNAN,
        TENENGRAD
    };
    Focus();
    ~Focus();
    double measure(AVFrame* frame, Method method = LAPLACIAN);
    static double fast(AVFrame* frame);
	static double fastROI(AVFrame* frame, int x0, int y0, int x1, int y1);
    static bool isSupportedYUVFormat(int format);

private:
    static double measure(const cv::Mat& frame, Method method = LAPLACIAN, const cv::Rect* bbox = nullptr);
	static double laplacianVariance(const cv::Mat& image);
    static double sobelVariance(const cv::Mat& image);
    static double brennanGradient(const cv::Mat& image);
    static double tenengrad(const cv::Mat& image);

};

#endif
