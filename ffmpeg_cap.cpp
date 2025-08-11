#include <iostream>
#include <string>
#include <vector>
#include <cstdint>

// FFmpeg C API headers
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

class SimpleZCAMCapture {
private:
    std::string rtsp_url;
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;
    SwsContext *sws_ctx = nullptr;
    int video_stream_index = -1;

public:
    SimpleZCAMCapture(const std::string& camera_ip) {
        rtsp_url = "rtsp://" + camera_ip + "/live_stream";
        
        // Initialize FFmpeg
        #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
        av_register_all();
        #endif
        avformat_network_init();
        
        std::cout << "🎥 ZCAM Simple Frame Capture" << std::endl;
        std::cout << "📡 RTSP URL: " << rtsp_url << std::endl;
    }
    
    ~SimpleZCAMCapture() {
        cleanup();
        avformat_network_deinit();
    }
    
    bool connect() {
        std::cout << "🔌 Connecting to ZCAM..." << std::endl;
        
        // Allocate format context
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            std::cout << "❌ Failed to allocate format context" << std::endl;
            return false;
        }
        
        // Set RTSP options - keep it simple but effective
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "10000000", 0);  // 10 second timeout
        av_dict_set(&options, "max_delay", "3000000", 0);   // 3 second max delay
        
        // Open the stream
        int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open stream: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ Connected to RTSP stream" << std::endl;
        
        // Skip stream info detection - it's causing segfault
        std::cout << "⚠️ Skipping stream info analysis (causes segfault with this camera)" << std::endl;
        std::cout << "🔍 Using manual stream detection..." << std::endl;
        
        // Check basic stream count first
        std::cout << "📊 Found " << format_ctx->nb_streams << " streams" << std::endl;
        
        if (format_ctx->nb_streams == 0) {
            std::cout << "❌ No streams found in RTSP feed" << std::endl;
            return false;
        }
        
        // Try to find video stream without calling avformat_find_stream_info
        if (!findVideoStreamManually()) {
            return false;
        }
        
        if (video_stream_index == -1) {
            std::cout << "❌ No video stream found" << std::endl;
            return false;
        }
        
        return setupDecoder();
    }
    
    bool findVideoStreamManually() {
        std::cout << "🔍 Manual stream detection..." << std::endl;
        
        // First, check stream parameters directly if available
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVStream *stream = format_ctx->streams[i];
            if (stream && stream->codecpar) {
                std::cout << "   Stream #" << i << ": codec_type=" << stream->codecpar->codec_type;
                std::cout << " (VIDEO=" << AVMEDIA_TYPE_VIDEO << ")" << std::endl;
                
                if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    video_stream_index = i;
                    std::cout << "✅ Found video stream at index " << i << " (direct check)" << std::endl;
                    return true;
                }
            }
        }
        
        // If direct check failed, try packet-based detection
        std::cout << "🔍 Trying packet-based detection..." << std::endl;
        
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            std::cout << "❌ Failed to allocate packet" << std::endl;
            return false;
        }
        
        std::vector<int> stream_sizes(format_ctx->nb_streams, 0);
        std::vector<int> stream_counts(format_ctx->nb_streams, 0);
        
        for (int i = 0; i < 20; i++) { // Read fewer packets to avoid issues
            int ret = av_read_frame(format_ctx, pkt);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                std::cout << "   Read error on packet " << i << ": " << errbuf << std::endl;
                break;
            }
            
            if (pkt->stream_index < static_cast<int>(stream_sizes.size())) {
                stream_sizes[pkt->stream_index] += pkt->size;
                stream_counts[pkt->stream_index]++;
            }
            
            av_packet_unref(pkt);
        }
        
        // Find stream with most data (likely video)
        int best_stream = -1;
        int max_data = 0;
        
        for (size_t i = 0; i < stream_sizes.size(); i++) {
            std::cout << "   Stream #" << i << ": " << stream_counts[i] 
                     << " packets, " << stream_sizes[i] << " bytes total" << std::endl;
            
            if (stream_sizes[i] > max_data && stream_sizes[i] > 5000) { // At least 5KB
                max_data = stream_sizes[i];
                best_stream = static_cast<int>(i);
            }
        }
        
        if (best_stream >= 0) {
            video_stream_index = best_stream;
            std::cout << "✅ Assuming stream #" << best_stream << " is video (largest data)" << std::endl;
            av_packet_free(&pkt);
            return true;
        }
        
        av_packet_free(&pkt);
        std::cout << "❌ Could not identify video stream" << std::endl;
        return false;
    }
    
    bool setupDecoder() {
        std::cout << "🔧 Setting up decoder..." << std::endl;
        
        if (video_stream_index < 0 || video_stream_index >= (int)format_ctx->nb_streams) {
            std::cout << "❌ Invalid video stream index: " << video_stream_index << std::endl;
            return false;
        }
        
        AVStream *video_stream = format_ctx->streams[video_stream_index];
        if (!video_stream || !video_stream->codecpar) {
            std::cout << "❌ Invalid video stream or codec parameters" << std::endl;
            return false;
        }
        
        AVCodecParameters *codec_params = video_stream->codecpar;
        
        std::cout << "📊 Stream info:" << std::endl;
        std::cout << "   Codec ID: " << codec_params->codec_id << std::endl;
        std::cout << "   Codec type: " << codec_params->codec_type << std::endl;
        std::cout << "   Width: " << codec_params->width << std::endl;
        std::cout << "   Height: " << codec_params->height << std::endl;
        
        // Find decoder
        codec = avcodec_find_decoder(codec_params->codec_id);
        if (!codec) {
            std::cout << "❌ Codec not found for ID: " << codec_params->codec_id << std::endl;
            std::cout << "   Codec name: " << avcodec_get_name(codec_params->codec_id) << std::endl;
            return false;
        }
        
        std::cout << "✅ Found codec: " << codec->name << std::endl;
        
        // Allocate codec context
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cout << "❌ Failed to allocate codec context" << std::endl;
            return false;
        }
        
        // Copy codec parameters - add error checking
        int ret = avcodec_parameters_to_context(codec_ctx, codec_params);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to copy codec parameters: " << errbuf << std::endl;
            return false;
        }
        
        // Open codec with error checking
        ret = avcodec_open2(codec_ctx, codec, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open codec: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ Decoder ready" << std::endl;
        std::cout << "   Resolution: " << codec_ctx->width << "x" << codec_ctx->height << std::endl;
        std::cout << "   Codec: " << codec->name << std::endl;
        std::cout << "   Pixel format: " << av_get_pix_fmt_name(codec_ctx->pix_fmt) << std::endl;
        
        return true;
    }
    
    bool captureOneFrame(std::vector<uint8_t>& rgb_data, int& width, int& height) {
        if (!format_ctx || !codec_ctx) {
            std::cout << "❌ Not connected" << std::endl;
            return false;
        }
        
        std::cout << "📷 Capturing frame..." << std::endl;
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *rgb_frame = av_frame_alloc();
        
        if (!packet || !frame || !rgb_frame) {
            std::cout << "❌ Failed to allocate memory" << std::endl;
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            if (rgb_frame) av_frame_free(&rgb_frame);
            return false;
        }
        
        bool success = false;
        int packets_read = 0;
        
        // Read packets until we get a decoded frame
        while (packets_read < 200) { // Safety limit
            int ret = av_read_frame(format_ctx, packet);
            packets_read++;
            
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                std::cout << "❌ Read error after " << packets_read << " packets: " << errbuf << std::endl;
                break;
            }
            
            // Process video packets only
            if (packet->stream_index == video_stream_index) {
                // Send packet to decoder
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret == 0) {
                    // Try to receive a frame
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == 0) {
                        // We got a frame! Convert it to RGB
                        width = frame->width;
                        height = frame->height;
                        
                        std::cout << "🎬 Frame decoded: " << width << "x" << height 
                                 << " (after " << packets_read << " packets)" << std::endl;
                        
                        // Setup color conversion
                        sws_ctx = sws_getContext(
                            width, height, (AVPixelFormat)frame->format,
                            width, height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                        );
                        
                        if (sws_ctx) {
                            // Allocate RGB buffer
                            int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
                            rgb_data.resize(rgb_size);
                            
                            // Setup RGB frame
                            av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize,
                                                rgb_data.data(), AV_PIX_FMT_RGB24, width, height, 1);
                            
                            // Convert to RGB
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                    rgb_frame->data, rgb_frame->linesize);
                            
                            std::cout << "✅ Frame converted to RGB (" << rgb_size << " bytes)" << std::endl;
                            success = true;
                        }
                        break;
                    } else if (ret != AVERROR(EAGAIN)) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                        std::cout << "⚠️ Decode error: " << errbuf << std::endl;
                    }
                }
            }
            
            av_packet_unref(packet);
        }
        
        // Cleanup
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        
        if (!success) {
            std::cout << "❌ Failed to capture frame after " << packets_read << " packets" << std::endl;
        }
        
        return success;
    }
    
    void cleanup() {
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
            codec_ctx = nullptr;
        }
        
        if (format_ctx) {
            avformat_close_input(&format_ctx);
            format_ctx = nullptr;
        }
        
        video_stream_index = -1;
        std::cout << "🧹 Cleaned up" << std::endl;
    }
};

// Simple test of just the frame capture
int main(int argc, char* argv[]) {
    std::string camera_ip = "192.168.150.201";
    
    if (argc > 1) {
        camera_ip = argv[1];
    }
    
    try {
        SimpleZCAMCapture capture(camera_ip);
        
        // Connect to camera
        if (!capture.connect()) {
            std::cout << "❌ Failed to connect to camera" << std::endl;
            return -1;
        }
        
        // Try to capture ONE frame
        std::vector<uint8_t> rgb_data;
        int width, height;
        
        if (capture.captureOneFrame(rgb_data, width, height)) {
            std::cout << "\n🎉 SUCCESS!" << std::endl;
            std::cout << "📊 Frame captured: " << width << "x" << height << std::endl;
            std::cout << "📊 RGB data size: " << rgb_data.size() << " bytes" << std::endl;
            
            // Quick brightness check
            if (!rgb_data.empty()) {
                uint64_t sum = 0;
                for (size_t i = 0; i < rgb_data.size(); i += 3) {
                    uint8_t r = rgb_data[i];
                    uint8_t g = rgb_data[i + 1];
                    uint8_t b = rgb_data[i + 2];
                    uint8_t gray = (uint8_t)(0.299 * r + 0.587 * g + 0.114 * b);
                    sum += gray;
                }
                double avg_brightness = (double)sum / (rgb_data.size() / 3);
                std::cout << "💡 Average brightness: " << avg_brightness << "/255" << std::endl;
                
                if (avg_brightness < 50) {
                    std::cout << "📊 Image appears DARK 🌙" << std::endl;
                } else if (avg_brightness > 200) {
                    std::cout << "📊 Image appears BRIGHT ☀️" << std::endl;
                } else {
                    std::cout << "📊 Image brightness looks good ✅" << std::endl;
                }
            }
            
        } else {
            std::cout << "\n❌ FAILED to capture frame" << std::endl;
            std::cout << "🔧 Check camera streaming and network connection" << std::endl;
            return -1;
        }
        
        std::cout << "\n✅ Test completed successfully!" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
}