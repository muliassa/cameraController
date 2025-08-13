#include <someFFMpeg.h>

void someFFMpeg::saveAVFrameAsJPEG(AVFrame *c, string path, int quality) {

    cout << "saveAVFrame# " << frame->width << " x " << frame->height << " format: " << frame->format << endl;
    
    // Set up output format
    AVFormatContext *formatContext = nullptr;
    avformat_alloc_output_context2(&formatContext, nullptr, "mjpeg", path.c_str());
    
    if (!formatContext) {
        throw std::runtime_error("Could not create output context");
    }
    
    // Add video stream
    AVStream *stream = avformat_new_stream(formatContext, nullptr);
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext *codecContext = avcodec_alloc_context3(codec);
    
    // Configure codec for YUV420P
    codecContext->width = frame->width;
    codecContext->height = frame->height;
    codecContext->pix_fmt = frame->format; // AV_PIX_FMT_YUV420P;
    codecContext->time_base = {1, 25};
    codecContext->global_quality = FF_QP2LAMBDA * (31 - (quality * 31 / 100));
    codecContext->flags |= AV_CODEC_FLAG_QSCALE;
    
    // CRITICAL: Allow non-standard YUV420P for JPEG
    codecContext->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    
    // Open codec
    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        avcodec_free_context(&codecContext);
        avformat_free_context(formatContext);
        throw std::runtime_error("Could not open codec");
    }
    
    // Copy codec parameters to stream
    avcodec_parameters_from_context(stream->codecpar, codecContext);
    
    // Open output file
    if (avio_open(&formatContext->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
        avcodec_free_context(&codecContext);
        avformat_free_context(formatContext);
        throw std::runtime_error("Could not open output file");
    }
    
    // Write header
    avformat_write_header(formatContext, nullptr);
    
    // Encode directly
    AVPacket *packet = av_packet_alloc();
    int ret = avcodec_send_frame(codecContext, frame);
    if (ret >= 0) {
        ret = avcodec_receive_packet(codecContext, packet);
        if (ret >= 0) {
            av_write_frame(formatContext, packet);
        } else {
            cout << "Error receiving packet: " << ret << endl;
        }
    } else {
        cout << "Error sending frame: " << ret << endl;
    }
    
    // Write trailer and cleanup
    av_write_trailer(formatContext);
    
    av_packet_free(&packet);
    avcodec_free_context(&codecContext);
    avio_closep(&formatContext->pb);
    avformat_free_context(formatContext);

}
