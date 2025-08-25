#ifndef OVERLAYS_H
#define OVERLAYS_H

#include <iostream>
#include <string>
#include <vector>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libswscale/swscale.h>
}

using namespace std;

#define DEBUG_PRINT(msg) ((void)0) // cout << "[DEBUG] " << msg << std::endl
// #define DEBUG_PRINT(msg) cout << "[DEBUG] " << msg << std::endl
#define ERROR_PRINT(msg) cerr << "[ERROR] " << msg << std::endl

struct GridText {
    int x;
    int y;
    double value;
    string text;
};

class FrameOverlayProcessor {

private:
    AVFilterGraph* filterGraph;
    AVFilterContext* bufferSrcCtx;
    AVFilterContext* bufferSinkCtx;
    AVFilterContext* logoBufferCtx = nullptr;
    AVFilterContext* overlayCtx;
    AVFrame* logoFrame;
    int frameWidth;
    int frameHeight;
    AVPixelFormat pixelFormat;
    bool initialized = false;
    
    // Text overlay settings
    std::string captionText;
    std::string fontPath;
    std::string fontName;
    std::string fontColor;
    int fontSize;

    vector<GridText> grid;
    
    // Logo settings
    std::string logoPath;
    bool logoLoaded;

    // Box overlay parameters
    bool showBox = false;
    int boxX, boxY, boxWidth, boxHeight;
    string boxColor = "red";
    int boxThickness = 3;

    // Box overlay parameters
    bool showCrop = false;
    int cropX, cropY, cropWidth, cropHeight;
    string cropColor = "blue";
    int cropThickness = 3;

    bool initializeFilterGraph();
    AVFrame* convertToRGBA(AVFrame* inputFrame);
    
public:
    FrameOverlayProcessor(int width, int height, AVPixelFormat format);
    ~FrameOverlayProcessor();
    bool isInitialized() { return initialized; }
    AVFrame* processFrame(AVFrame* inputFrame);
    void setBox(int x, int y, int width, int height, const string& color = "red", int thickness = 3);
    void hideBox();
    void setCrop(int x, int y, int width, int height, const string& color = "blue", int thickness = 3);
    void hideCrop();
    void setCaptionText(const string& text);
    void clearGridText();
    void setGridText(const GridText& point);
    void setFont(const string& path, int size = 24);
    void setFontColor(const string& color);    
    bool loadLogo(const string& path);

};

#endif
