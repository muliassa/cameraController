#include <focus.h>

    Focus::Focus() {
        cout << "INIT FOCUS" << endl;
    }

    Focus::~Focus() {
        cout << "DELETE FOCUS" << endl;
        if (swsContext != nullptr) sws_freeContext(swsContext);
    }

    bool Focus::isSupportedYUVFormat(int format) {
        AVPixelFormat pixFmt = static_cast<AVPixelFormat>(format);
        
        switch (pixFmt) {
            case AV_PIX_FMT_YUV420P:    // 0 - Standard YUV 4:2:0
            case AV_PIX_FMT_YUVJ420P:   // 12 - JPEG YUV 4:2:0 (full range)
            case AV_PIX_FMT_YUV422P:    // 4 - YUV 4:2:2
            case AV_PIX_FMT_YUVJ422P:   // 13 - JPEG YUV 4:2:2
            case AV_PIX_FMT_YUV444P:    // 5 - YUV 4:4:4
            case AV_PIX_FMT_YUVJ444P:   // 14 - JPEG YUV 4:4:4
            case AV_PIX_FMT_YUV410P:    // 6 - YUV 4:1:0
            case AV_PIX_FMT_YUV411P:    // 7 - YUV 4:1:1
            case AV_PIX_FMT_GRAY8:      // 8 - Grayscale
                return true;
            default:
                return false;
        }
    }
    
    // Work directly with decoded frame - no conversion needed
    double Focus::fastROI(AVFrame* frame, int x0, int y0, int x1, int y1) {
    
        // Check if it's a supported YUV format
        if (!isSupportedYUVFormat(frame->format)) {
            std::cerr << "Unsupported pixel format: " << frame->format << std::endl;
            return -1.0;
        }

        x0 = max(0, x0);
        y0 = max(0, y0);
        x1 = min(frame->width, x1);
        y1 = min(frame->height, y1);

        // cout << "fastROI: width: " << frame->width << " height: " << frame->height << " x0: " << x0 << " y0: " << y0 << " x1: " << x1 << " y1: " << y1 << endl;
        
        // Use Y channel directly (luminance)
        cv::Mat yMat(frame->height, frame->width, CV_8UC1, 
                    frame->data[0], frame->linesize[0]);

        // Extract ROI (Region of Interest)
        int width = x1 - x0;
        int height = y1 - y0;
        cv::Rect roi(x0, y0, width, height);
        cv::Mat roiMat = yMat(roi);  // This creates a view, no copying!
        
        // Measure focus using your existing function
        return laplacianVariance(roiMat);
    }

    double Focus::fast(AVFrame* frame) {
        return fastROI(frame, 0, 0, frame->width, frame->height);
    }

    double Focus::measure(AVFrame* frame, Method method) {
        
        AVFrame* frameRGB = av_frame_alloc();

        if (!swsContext) {
            // Initialize scaler
            swsContext = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),  // Cast here
                                       frame->width, frame->height, AV_PIX_FMT_RGB24,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);

        }

        // Convert to RGB
        sws_scale(swsContext, frame->data, frame->linesize, 0, frame->height, frameRGB->data, frameRGB->linesize);
                
        // Convert to OpenCV Mat
        cv::Mat cvFrame(frame->height, frame->width, CV_8UC3, frameRGB->data[0]);
        cv::cvtColor(cvFrame, cvFrame, cv::COLOR_RGB2BGR);

        double focus = measure(cvFrame, method);

        av_frame_free(&frameRGB);  

        return focus;
    }

    double Focus::measure(const cv::Mat& frame, Method method, const cv::Rect* bbox) {

        cv::Mat gray, roi;
        
        // Convert to grayscale
        if (frame.channels() == 3) {
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        } else {
            gray = frame;
        }
        
        // Extract ROI if bbox provided
        if (bbox) {
            roi = gray(*bbox);
        } else {
            roi = gray;
        }
        
        switch (method) {
            case LAPLACIAN:
                return laplacianVariance(roi);
            case SOBEL:
                return sobelVariance(roi);
            case BRENNAN:
                return brennanGradient(roi);
            case TENENGRAD:
                return tenengrad(roi);
            default:
                return laplacianVariance(roi);
        }
    }
    
    double Focus::laplacianVariance(const cv::Mat& image) {
        cv::Mat laplacian;
        cv::Laplacian(image, laplacian, CV_64F);
        
        cv::Scalar mean, stddev;
        cv::meanStdDev(laplacian, mean, stddev);
        return stddev[0] * stddev[0];
    }
    
    double Focus::sobelVariance(const cv::Mat& image) {
        cv::Mat sobelX, sobelY, magnitude;
        cv::Sobel(image, sobelX, CV_64F, 1, 0, 3);
        cv::Sobel(image, sobelY, CV_64F, 0, 1, 3);
        
        cv::magnitude(sobelX, sobelY, magnitude);
        return cv::mean(magnitude)[0];
    }
    
    double Focus::brennanGradient(const cv::Mat& image) {
        cv::Mat diff;
        image.convertTo(diff, CV_64F);
        
        double sum = 0;
        for (int i = 0; i < diff.rows; i++) {
            for (int j = 1; j < diff.cols; j++) {
                double gradient = diff.at<double>(i, j) - diff.at<double>(i, j-1);
                sum += gradient * gradient;
            }
        }
        return sum;
    }
    
    double Focus::tenengrad(const cv::Mat& image) {
        cv::Mat sobelX, sobelY;
        cv::Sobel(image, sobelX, CV_64F, 1, 0, 3);
        cv::Sobel(image, sobelY, CV_64F, 0, 1, 3);
        
        cv::Mat magnitude;
        cv::magnitude(sobelX, sobelY, magnitude);
        
        // Apply threshold to focus on strong edges
        cv::Mat mask = magnitude > 10.0;
        return cv::sum(magnitude.mul(mask / 255.0))[0];
    }
