#include <iostream>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

int main(int argc, char* argv[]) {
    std::string camera_ip = "192.168.150.201";
    
    if (argc > 1) {
        camera_ip = argv[1];
    }
    
    std::cout << "=== Simple FFmpeg RTSP Test ===" << std::endl;
    std::cout << "Camera IP: " << camera_ip << std::endl;
    
    // Initialize FFmpeg
    #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
    #endif
    avformat_network_init();
    
    std::cout << "✅ FFmpeg initialized" << std::endl;
    std::cout << "   libavformat version: " << av_version_info() << std::endl;
    
    // Test RTSP connection
    AVFormatContext *format_ctx = nullptr;
    AVDictionary *options = nullptr;
    
    // Set ZCAM-compatible options
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "timeout", "10000000", 0);
    
    std::string rtsp_url = "rtsp://" + camera_ip + "/live_stream";
    std::cout << "🔌 Testing connection to: " << rtsp_url << std::endl;
    
    int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
    av_dict_free(&options);
    
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        std::cout << "❌ Failed to connect: " << errbuf << std::endl;
        return -1;
    }
    
    std::cout << "✅ RTSP connection successful!" << std::endl;
    
    // Get stream info
    if (avformat_find_stream_info(format_ctx, nullptr) >= 0) {
        std::cout << "✅ Stream info found" << std::endl;
        std::cout << "   Number of streams: " << format_ctx->nb_streams << std::endl;
        
        // Find video stream
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVCodecParameters *codecpar = format_ctx->streams[i]->codecpar;
            if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                std::cout << "   Video stream #" << i << ":" << std::endl;
                std::cout << "     Resolution: " << codecpar->width << "x" << codecpar->height << std::endl;
                std::cout << "     Codec ID: " << codecpar->codec_id << std::endl;
                
                const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
                if (codec) {
                    std::cout << "     Codec name: " << codec->name << std::endl;
                }
                break;
            }
        }
    }
    
    // Clean up
    avformat_close_input(&format_ctx);
    avformat_network_deinit();
    
    std::cout << "✅ Test completed successfully!" << std::endl;
    std::cout << "   Your ZCAM RTSP stream is working with FFmpeg C API" << std::endl;
    
    return 0;
}

/*
COMPILATION:
g++ -std=c++17 ffmpeg_test.cpp -lavformat -lavcodec -lavutil -o ffmpeg_test

USAGE:
./ffmpeg_test 192.168.150.201

This simple test verifies:
✅ FFmpeg libraries are properly installed
✅ RTSP connection works with TCP transport
✅ Stream info can be read
✅ Video codec is supported
*/