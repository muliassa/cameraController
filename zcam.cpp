#include <zcam.h>
#include <chrono>
#include <someFFMpeg.h>
   
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
    
    bool ZCAM::initStream() {
        
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

    AVFrame *ZCAM::getFrame() {
        
        if (!format_ctx || !codec_ctx) return false;
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        
        if (!packet || !frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            return false;
        }
        
        while (true) {
            int ret = av_read_frame(format_ctx, packet);
            
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
