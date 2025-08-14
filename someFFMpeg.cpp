#include <someFFMpeg.h>

#include <iostream>

void someFFMpeg::saveAVFrameAsJPEG(AVFrame *frame, const string& path, int quality) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    
    ctx->width = frame->width;
    ctx->height = frame->height;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base = {1, 1};
    
    // Minimal settings - avoid buffer/rate completely
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * (32 - quality * 31 / 100);
    
    if (avcodec_open2(ctx, codec, nullptr) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        
        if (avcodec_send_frame(ctx, frame) >= 0 && 
            avcodec_receive_packet(ctx, pkt) >= 0) {
            
            FILE *f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(pkt->data, 1, pkt->size, f);
                fclose(f);
                std::cout << "✅ JPEG saved: " << path << std::endl;
            }
        }
        
        av_packet_free(&pkt);
    }
    
    avcodec_free_context(&ctx);
}
