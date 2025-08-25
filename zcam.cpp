#include <zcam.h>
#include <chrono>
   
    ZCAM::ZCAM(const json& config, const int cam_idx) {

        root = config["files"].get<string>();
        camera_ip = config["ipaddr"][cam_idx].get<string>();
        camera_id = config["cameras"][cam_idx].get<string>();
        rtsp_url = "rtsp://" + camera_ip + "/live_stream";
        http_base_url = "http://" + camera_ip + "/ctrl";

        // Initialize FFmpeg
        #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
        av_register_all();
        #endif
        avformat_network_init();

    } 

    ZCAM::~ZCAM() {
        cleanup();
        avformat_network_deinit();        
    }
    
    bool ZCAM::detectVideoStream() {

        AVPacket *pkt = av_packet_alloc();
        if (!pkt) return false;
        
        for (int i = 0; i < 30; i++) {
            int ret = av_read_frame(format_ctx, pkt);
            if (ret < 0) break;
            
            if (pkt->size > 1000) {
                uint8_t *data = pkt->data;
                if (pkt->size >= 4 && 
                    ((data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) ||
                     (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01))) {
                    video_stream_index = pkt->stream_index;
                    break;
                }
            }
            av_packet_unref(pkt);
        }
        
        av_packet_free(&pkt);
        
        if (video_stream_index < 0) return false;
        
        // Setup H.264 decoder
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return false;
        
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) return false;
        
        codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx->codec_id = AV_CODEC_ID_H264;
        
        return avcodec_open2(codec_ctx, codec, nullptr) >= 0;
    }
    
    bool ZCAMC::initStream() {
        
        std::cout << "🔌 Connecting to RTSP..." << std::endl;
        
        format_ctx = avformat_alloc_context();
        if (!format_ctx) return false;
        
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "10000000", 0);
        av_dict_set(&options, "max_delay", "3000000", 0);
        
        int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) return false;
        
        // Skip stream info analysis, use manual detection
        if (!detectVideoStream()) return false;
        
        cout << "✅ RTSP stream ready" << std::endl;
        return true;
    }

    AVFrame *getFrame() {
        
        if (!format_ctx || !codec_ctx) return false;
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        
        if (!packet || !frame || !rgb_frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            return false;
        }
        
        bool success = false;
        int packets_read = 0;
        
        while (packets_read < 100 && keep_running) {
            int ret = av_read_frame(format_ctx, packet);
            packets_read++;
            
            if (ret < 0) break;
            
            if (packet->stream_index == video_stream_index) {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret == 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == 0) {
                        av_packet_free(&packet);
                        return frame;
                    }
                }
            }
        }
        
        return frame;
    }

    void ZCAM::closeStream() {
        cleanup();
    }

    
    void ZCAM::cleanup() {
        
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
            codec_ctx = nullptr;
        }
        
        if (format_ctx) {
            avformat_close_input(&format_ctx);
            format_ctx = nullptr;
        }
        
        video_stream_index = -1;

    }

    bool ZCAM::captureFrame(std::vector<uint8_t>& rgb_data, int& width, int& height) {
        
        if (!format_ctx || !codec_ctx) return false;
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *rgb_frame = av_frame_alloc();
        
        if (!packet || !frame || !rgb_frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            if (rgb_frame) av_frame_free(&rgb_frame);
            return false;
        }
        
        bool success = false;
        int packets_read = 0;
        
        while (packets_read < 100 && keep_running) {
            int ret = av_read_frame(format_ctx, packet);
            packets_read++;
            
            if (ret < 0) break;
            
            if (packet->stream_index == video_stream_index) {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret == 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == 0) {

                        auto now = std::chrono::system_clock::now();
                        auto time_t = std::chrono::system_clock::to_time_t(now);       
                        std::stringstream ss;
                        ss << root << "zcam/" << camera_id << std::put_time(std::localtime(&time_t), "%H%M");
                        snapshot = ss.str();
                        someFFMpeg::saveAVFrameAsJPEG(frame, snapshot + ".JPG", 100);

                        width = frame->width;
                        height = frame->height;
                        
                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(
                                width, height, (AVPixelFormat)frame->format,
                                width, height, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                            );
                        }
                        
                        if (sws_ctx) {
                            int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
                            rgb_data.resize(rgb_size);
                            
                            av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize,
                                                rgb_data.data(), AV_PIX_FMT_RGB24, width, height, 1);
                            
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                    rgb_frame->data, rgb_frame->linesize);
                            
                            success = true;
                        }
                        break;
                    }
                }
            }
            av_packet_unref(packet);
        }
        
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        
        return success;
    }
