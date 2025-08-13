#include <someFFMpeg.h>

#include <iostream>

void someFFMpeg::saveAVFrameAsJPEG(AVFrame *frame, string path, int quality) {

    // Find MJPEG encoder
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        throw std::runtime_error("MJPEG codec not found");
    }
    
    AVCodecContext *codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        throw std::runtime_error("Could not allocate codec context");
    }
    
    // Configure codec
    codecContext->width = frame->width;
    codecContext->height = frame->height;
    codecContext->pix_fmt = AV_PIX_FMT_YUVJ420P;  // MJPEG prefers YUVJ420P
    codecContext->time_base = {1, 1};              // Single frame
    
    // JPEG quality settings - FIX THE BUFFER/RATE ERROR
    codecContext->bit_rate = 0;                    // Set to 0 for quality-based encoding
    codecContext->rc_buffer_size = 0;              // Set to 0 
    codecContext->rc_max_rate = 0;                 // Set to 0
    codecContext->global_quality = FF_QP2LAMBDA * (31 - (quality * 31 / 100));
    codecContext->flags |= AV_CODEC_FLAG_QSCALE;
    
    // Compliance settings
    codecContext->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    
    // Open codec
    int ret = avcodec_open2(codecContext, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        avcodec_free_context(&codecContext);
        throw std::runtime_error(string("Could not open codec: ") + errbuf);
    }
    
    // Convert frame format if needed
    AVFrame *jpeg_frame = frame;
    SwsContext *sws_ctx = nullptr;
    
    if (frame->format != AV_PIX_FMT_YUVJ420P) {
        jpeg_frame = av_frame_alloc();
        jpeg_frame->width = frame->width;
        jpeg_frame->height = frame->height;
        jpeg_frame->format = AV_PIX_FMT_YUVJ420P;
        
        av_frame_get_buffer(jpeg_frame, 0);
        
        sws_ctx = sws_getContext(
            frame->width, frame->height, (AVPixelFormat)frame->format,
            frame->width, frame->height, AV_PIX_FMT_YUVJ420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (sws_ctx) {
            sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                     jpeg_frame->data, jpeg_frame->linesize);
        }
    }
    
    // Encode frame
    AVPacket *packet = av_packet_alloc();
    ret = avcodec_send_frame(codecContext, jpeg_frame);
    if (ret >= 0) {
        ret = avcodec_receive_packet(codecContext, packet);
        if (ret >= 0) {
            // Write JPEG data directly to file
            FILE *f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(packet->data, 1, packet->size, f);
                fclose(f);
                cout << "✅ JPEG saved: " << path << " (" << packet->size << " bytes)" << endl;
            } else {
                cout << "❌ Could not open file for writing: " << path << endl;
            }
        } else {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            cout << "❌ Error receiving packet: " << errbuf << endl;
        }
    } else {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        cout << "❌ Error sending frame: " << errbuf << endl;
    }
    
    // Cleanup
    av_packet_free(&packet);
    
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
    }
    if (jpeg_frame != frame) {
        av_frame_free(&jpeg_frame);
    }
    
    avcodec_free_context(&codecContext);
}
