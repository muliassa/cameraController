#include <overlays.h>
#include <someLogger.h>

    FrameOverlayProcessor::FrameOverlayProcessor(int width, int height, AVPixelFormat format) 
        : filterGraph(nullptr), bufferSrcCtx(nullptr), bufferSinkCtx(nullptr),
          logoFrame(nullptr), frameWidth(width), frameHeight(height), 
          pixelFormat(format), fontSize(24), fontColor("white"), logoLoaded(false) {
    }
    
    FrameOverlayProcessor::~FrameOverlayProcessor() {
        if (filterGraph) {
            avfilter_graph_free(&filterGraph);
        }
        if (logoFrame) {
            av_frame_free(&logoFrame);
        }
    }

    void FrameOverlayProcessor::clearGridText() {
        grid = vector<GridText>();
    }

    void FrameOverlayProcessor::setGridText(const GridText& point) {
        grid.push_back(point);
    }
    
    void FrameOverlayProcessor::setCaptionText(const string& text) {
        cout << "SET CAPTION# " << text << endl;
        captionText = text;
        initialized = false;
        initializeFilterGraph();
    }
    
    void FrameOverlayProcessor::setFont(const std::string& path, int size) {
        fontPath = path;
        fontSize = size;
    }
    
    void FrameOverlayProcessor::setFontColor(const std::string& color) {
        fontColor = color;
    }
    
    bool FrameOverlayProcessor::loadLogo(const std::string& path) {
        // DEBUG_PRINT("Loading logo from: " << path);
        logoPath = path;
        
        // Clean up previous logo if exists
        if (logoFrame) {
            av_frame_free(&logoFrame);
            logoFrame = nullptr;
        }
        
        AVFormatContext* formatCtx = nullptr;
        int ret = avformat_open_input(&formatCtx, path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            ERROR_PRINT("Failed to open logo file: " << path << " - " << error_buf);
            return false;
        }
        // DEBUG_PRINT("Logo file opened successfully");
        
        ret = avformat_find_stream_info(formatCtx, nullptr);
        if (ret < 0) {
            ERROR_PRINT("Failed to find stream info for logo");
            avformat_close_input(&formatCtx);
            return false;
        }
        // DEBUG_PRINT("Stream info found");
        
        // Find video stream
        int videoStreamIndex = -1;
        for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
            if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStreamIndex = i;
                break;
            }
        }
        
        if (videoStreamIndex == -1) {
            ERROR_PRINT("No video stream found in logo file");
            avformat_close_input(&formatCtx);
            return false;
        }
        // DEBUG_PRINT("Video stream found at index: " << videoStreamIndex);
        
        // Setup decoder
        const AVCodec* codec = avcodec_find_decoder(formatCtx->streams[videoStreamIndex]->codecpar->codec_id);
        if (!codec) {
            ERROR_PRINT("Could not find decoder for logo");
            avformat_close_input(&formatCtx);
            return false;
        }
        
        AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            ERROR_PRINT("Could not allocate codec context");
            avformat_close_input(&formatCtx);
            return false;
        }
        
        ret = avcodec_parameters_to_context(codecCtx, formatCtx->streams[videoStreamIndex]->codecpar);
        if (ret < 0) {
            ERROR_PRINT("Could not copy codec parameters");
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            return false;
        }
        
        ret = avcodec_open2(codecCtx, codec, nullptr);
        if (ret < 0) {
            ERROR_PRINT("Could not open codec");
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            return false;
        }
        
        // Read and decode first frame
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        bool frameDecoded = false;
        
        while (av_read_frame(formatCtx, packet) >= 0 && !frameDecoded) {
            if (packet->stream_index == videoStreamIndex) {
                ret = avcodec_send_packet(codecCtx, packet);
                if (ret == 0) {
                    ret = avcodec_receive_frame(codecCtx, frame);
                    if (ret == 0) {
                        // Convert to RGBA
                        logoFrame = convertToRGBA(frame);
                        if (logoFrame) {
                            // DEBUG_PRINT("Logo converted to RGBA: " << logoFrame->width << "x" << logoFrame->height);
                            frameDecoded = true;
                            logoLoaded = true;
                        } else {
                            ERROR_PRINT("Failed to convert logo to RGBA");
                        }
                    }
                }
            }
            av_packet_unref(packet);
        }
        
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        
        if (!frameDecoded) {
            ERROR_PRINT("Could not decode any frame from logo file");
            return false;
        }
        
        // Force re-initialization of filter graph to include logo
        initialized = false;        
        return logoLoaded;
    }
    
    // Add these methods:
    void FrameOverlayProcessor::setBox(int x, int y, int width, int height, const string& color, int thickness) {
        boxX = x;
        boxY = y;
        boxWidth = width;
        boxHeight = height;
        boxColor = color;
        boxThickness = thickness;
        showBox = true;
        
        // Reinitialize filter graph to include box
        // initialized = false;
        // bool success = initializeFilterGraph();
    }
   
    void FrameOverlayProcessor::hideBox() {
        showBox = false;
        initialized = false;
        initializeFilterGraph();
    }

    // Add these methods:
    void FrameOverlayProcessor::setCrop(int x, int y, int width, int height, const string& color, int thickness) {
        cropX = x;
        cropY = y;
        cropWidth = width;
        cropHeight = height;
        cropColor = color;
        cropThickness = thickness;
        showCrop = true;
        
        // Reinitialize filter graph to include box
        // initialized = false;
        // bool success = initializeFilterGraph();
    }
   
    void FrameOverlayProcessor::hideCrop() {
        showCrop = false;
        // initialized = false;
        // initializeFilterGraph();
    }

    AVFrame* FrameOverlayProcessor::convertToRGBA(AVFrame* inputFrame) {
        AVFrame* rgbaFrame = av_frame_alloc();
        rgbaFrame->format = AV_PIX_FMT_RGBA;
        rgbaFrame->width = inputFrame->width;
        rgbaFrame->height = inputFrame->height;
        
        if (av_frame_get_buffer(rgbaFrame, 0) < 0) {
            av_frame_free(&rgbaFrame);
            return nullptr;
        }
        
        SwsContext* swsCtx = sws_getContext(
            inputFrame->width, inputFrame->height, (AVPixelFormat)inputFrame->format,
            rgbaFrame->width, rgbaFrame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!swsCtx) {
            av_frame_free(&rgbaFrame);
            return nullptr;
        }
        
        sws_scale(swsCtx, inputFrame->data, inputFrame->linesize, 0, inputFrame->height,
                  rgbaFrame->data, rgbaFrame->linesize);
        
        sws_freeContext(swsCtx);
        return rgbaFrame;
    }
    
    bool FrameOverlayProcessor::initializeFilterGraph() {

        DEBUG_PRINT("reinit filter graph");

        if (initialized) {
            // DEBUG_PRINT("Filter graph already initialized");
            return true;
        }
        
        // Clean up previous graph if exists
        if (filterGraph) {
            // DEBUG_PRINT("Cleaning up existing filter graph");
            avfilter_graph_free(&filterGraph);
            bufferSrcCtx = nullptr;
            bufferSinkCtx = nullptr;
        }
        
        filterGraph = avfilter_graph_alloc();
        if (!filterGraph) {
            ERROR_PRINT("Failed to allocate new filter graph");
            return false;
        }
        
        char args[512];
        int ret;
        
        DEBUG_PRINT("set args");
        
        // Buffer source
        const AVFilter* bufferSrc = avfilter_get_by_name("buffer");
        if (!bufferSrc) {
            ERROR_PRINT("Could not find buffer filter - FFmpeg may not be properly compiled");
            return false;
        }
        
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/30:pixel_aspect=1/1",
                 frameWidth, frameHeight, pixelFormat);

        ret = avfilter_graph_create_filter(&bufferSrcCtx, bufferSrc, "in",
                                           args, nullptr, filterGraph);
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            ERROR_PRINT("Could not create buffer source filter. Error: " << error_buf);
            bufferSrcCtx = nullptr;
            return false;
        }
        
        if (!bufferSrcCtx) {
            ERROR_PRINT("Buffer source context is null despite successful creation");
            return false;
        }

        // ADD THIS SECTION FOR LOGO BUFFER:
        if (logoLoaded && logoFrame) {
            
            // Get the buffer filter (same as bufferSrc above)
            const AVFilter* logoBuffer = avfilter_get_by_name("buffer");
            if (!logoBuffer) {
                ERROR_PRINT("Could not find buffer filter for logo");
                return false;
            }
            
            // Create args for logo buffer (different size and format than main video)
            char logoArgs[512];
            snprintf(logoArgs, sizeof(logoArgs),
                     "video_size=%dx%d:pix_fmt=%d:time_base=1/30:pixel_aspect=1/1",
                     logoFrame->width, logoFrame->height, AV_PIX_FMT_RGBA);
            
            // NOW create the logo buffer context
            ret = avfilter_graph_create_filter(&logoBufferCtx, logoBuffer, "logo_in", 
                                               logoArgs, nullptr, filterGraph);
            if (ret < 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                ERROR_PRINT("Could not create logo buffer filter. Error: " << error_buf);
                logoBufferCtx = nullptr;
                return false;
            }
            
        }

        // Now we need to create the overlay filter and connect everything
        if (logoBufferCtx) {
            
            // Get the overlay filter
            const AVFilter* overlayFilter = avfilter_get_by_name("overlay");
            if (!overlayFilter) {
                ERROR_PRINT("Could not find overlay filter");
                return false;
            }
            
            // Create overlay filter context
            overlayCtx = nullptr;
            std::string overlayArgs = "x=W-w-20:y=H-h-20"; // Bottom right position
            
            ret = avfilter_graph_create_filter(&overlayCtx, overlayFilter, "overlay",
                                               overlayArgs.c_str(), nullptr, filterGraph);
            if (ret < 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                ERROR_PRINT("Could not create overlay filter. Error: " << error_buf);
                return false;
            }
                        
            // IMPORTANT: We need to remember this overlayCtx to connect it later
            // Store it as a member variable or use it directly in the linking section
        }

        // Buffer sink
        const AVFilter* bufferSink = avfilter_get_by_name("buffersink");
        if (!bufferSink) {
            ERROR_PRINT("Could not find buffersink filter");
            return false;
        }
        
        ret = avfilter_graph_create_filter(&bufferSinkCtx, bufferSink, "out",
                                           nullptr, nullptr, filterGraph);
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            ERROR_PRINT("Could not create buffer sink filter. Error: " << error_buf);
            bufferSinkCtx = nullptr;
            return false;
        }
        
        if (!bufferSinkCtx) {
            ERROR_PRINT("Buffer sink context is null despite successful creation");
            return false;
        }
        
        // SET THE OUTPUT FORMAT TO MATCH YOUR ENCODER
        enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
        ret = av_opt_set_int_list(bufferSinkCtx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ERROR_PRINT("Could not set output pixel format");
            return false;
        }
        
        // Build filter chain
        AVFilterContext* lastFilter = bufferSrcCtx;

        if (showBox) {
            AVFilterContext* drawBoxCtx = nullptr;
            const AVFilter* drawBox = avfilter_get_by_name("drawbox");
            if (!drawBox) {
                ERROR_PRINT("Could not find drawbox filter");
                // Continue without box overlay
            } else {
                DEBUG_PRINT("Creating drawbox filter...");
                
                // Use the actual boxX/boxY position (not center calculation)
                std::string drawBoxArgs = "x=" + std::to_string(boxX);        // NOW using boxX
                drawBoxArgs += ":y=" + std::to_string(boxY);                  // NOW using boxY
                drawBoxArgs += ":w=" + std::to_string(boxWidth);
                drawBoxArgs += ":h=" + std::to_string(boxHeight);
                drawBoxArgs += ":color=" + boxColor;
                drawBoxArgs += ":t=" + std::to_string(boxThickness);
                
                DEBUG_PRINT("drawBoxArgs: " << drawBoxArgs);
                DEBUG_PRINT("Box position: (" << boxX << "," << boxY << "), size: " << boxWidth << "x" << boxHeight);
                
                ret = avfilter_graph_create_filter(&drawBoxCtx, drawBox, "drawbox",
                                                   drawBoxArgs.c_str(), nullptr, filterGraph);

                          // CRITICAL: Link the drawbox to the previous filter in the chain
                ret = avfilter_link(lastFilter, 0, drawBoxCtx, 0);
                if (ret < 0) {
                    char error_buf[256];
                    av_strerror(ret, error_buf, sizeof(error_buf));
                    ERROR_PRINT("Could not link to drawbox filter. Error: " << error_buf);
                    return false;
                }
            
                DEBUG_PRINT("SUCCESS: Linked drawbox filter!");
                lastFilter = drawBoxCtx;  // CRITICAL: Update lastFilter
                // ... rest of the code stays the same
            }
        }

        if (showCrop) {
            AVFilterContext* drawCropCtx = nullptr;
            const AVFilter* drawCrop = avfilter_get_by_name("drawbox");
            if (!drawCrop) {
                ERROR_PRINT("Could not find drawbox filter");
                // Continue without box overlay
            } else {
                DEBUG_PRINT("Creating drawbox filter...");
                
                // Use the actual boxX/boxY position (not center calculation)
                std::string drawCropArgs = "x=" + std::to_string(cropX);        // NOW using boxX
                drawCropArgs += ":y=" + std::to_string(cropY);                  // NOW using boxY
                drawCropArgs += ":w=" + std::to_string(cropWidth);
                drawCropArgs += ":h=" + std::to_string(cropHeight);
                drawCropArgs += ":color=" + cropColor;
                drawCropArgs += ":t=" + std::to_string(cropThickness);
                
                DEBUG_PRINT("drawBoxArgs: " << drawCropArgs);
                DEBUG_PRINT("Box position: (" << cropX << "," << cropY << "), size: " << cropWidth << "x" << cropHeight);
                
                ret = avfilter_graph_create_filter(&drawCropCtx, drawCrop, "drawbox",
                                                   drawCropArgs.c_str(), nullptr, filterGraph);

                          // CRITICAL: Link the drawbox to the previous filter in the chain
                ret = avfilter_link(lastFilter, 0, drawCropCtx, 0);
                if (ret < 0) {
                    char error_buf[256];
                    av_strerror(ret, error_buf, sizeof(error_buf));
                    ERROR_PRINT("Could not link to drawbox filter. Error: " << error_buf);
                    return false;
                }
            
                DEBUG_PRINT("SUCCESS: Linked drawbox filter!");
                lastFilter = drawCropCtx;  // CRITICAL: Update lastFilter
                // ... rest of the code stays the same
            }
        }
        
        // Add text overlay if caption is set

        if (!captionText.empty()) {
            AVFilterContext* drawTextCtx = nullptr;
            const AVFilter* drawText = avfilter_get_by_name("drawtext");
            if (!drawText) {
                ERROR_PRINT("Could not find drawtext filter - your FFmpeg may not have libfreetype support");
                // Continue without text overlay
            } else {
                
                std::string drawTextArgs = "text='" + captionText + "'";
                drawTextArgs += ":x=20:y=h-th-20";
                drawTextArgs += ":fontsize=" + to_string(fontSize);
                drawTextArgs += ":fontcolor=" + fontColor;
                // drawTextArgs += ":box=1:boxcolor=black@0.3:boxborderw=5";

                DEBUG_PRINT("drawTextArgs: " << drawTextArgs);
                
                if (!fontPath.empty()) {
                    drawTextArgs += ":fontfile=" + fontPath;
                } else if (!fontName.empty()) {
                    drawTextArgs += ":font=" + fontName;
                } else {
                    // Use system default font
                    #ifdef _WIN32
                        drawTextArgs += ":font=Arial";
                    #elif __APPLE__
                        drawTextArgs += ":font=Helvetica";
                    #else
                        drawTextArgs += ":font=DejaVu Sans";
                    #endif
                }

                DEBUG_PRINT("drawTextArgs+: " << drawTextArgs);
                
                ret = avfilter_graph_create_filter(&drawTextCtx, drawText, "drawtext",
                                                   drawTextArgs.c_str(), nullptr, filterGraph);
                if (ret < 0) {
                    char error_buf[256];
                    av_strerror(ret, error_buf, sizeof(error_buf));
                    ERROR_PRINT("Could not create drawtext filter. Error: " << error_buf);
                    DEBUG_PRINT("Continuing without text overlay");
                } else {
                    
                    ret = avfilter_link(lastFilter, 0, drawTextCtx, 0);
                    if (ret < 0) {
                        char error_buf[256];
                        av_strerror(ret, error_buf, sizeof(error_buf));
                        ERROR_PRINT("Could not link to drawtext filter. Error: " << error_buf);
                        return false;
                    }
                    
                    lastFilter = drawTextCtx;
                }
            }
        }

        double maxValue = 0;

        for (int i = 0; i < grid.size(); i++)
            maxValue = max(maxValue, grid[i].value);

        cout << "MAX VALUE: " << to_string(maxValue) << endl;

        // Create multiple drawtext filters - one for each grid cell
        for (int i = 0; i < grid.size(); i++) {

                AVFilterContext* drawTextCtx = nullptr;
                const AVFilter* drawText = avfilter_get_by_name("drawtext");
                
                if (!drawText) {
                    ERROR_PRINT("Could not find drawtext filter");
                    continue;
                }
                
                int x = grid[i].x;
                int y = grid[i].y;

                // Format focus value
                // double focusValue = focusGrid[row][col];
                // std::string focusText = std::to_string(static_cast<int>(focusValue));
                string focusText = grid[i].text;

                string color = "red";

                if (grid[i].value == maxValue) color = "yellow";
                
                // Choose color based on focus quality
                // std::string color = getFocusColor(focusValue);
                
                std::string drawTextArgs = "text='" + to_string(i) + ":" + focusText + "'";
                drawTextArgs += ":x=" + std::to_string(x);
                drawTextArgs += ":y=" + std::to_string(y);
                drawTextArgs += ":fontsize=20";
                drawTextArgs += ":fontcolor=" + color;
                
                // Add font
                #ifdef _WIN32
                    drawTextArgs += ":font=Arial";
                #elif __APPLE__
                    drawTextArgs += ":font=Helvetica";
                #else
                    drawTextArgs += ":font=DejaVu Sans";
                #endif
                                
                ret = avfilter_graph_create_filter(&drawTextCtx, drawText, 
                                                 ("drawtext_" + to_string(x) + "_" + to_string(y)).c_str(),
                                                 drawTextArgs.c_str(), nullptr, filterGraph);
                
                if (ret < 0) {
                    char error_buf[256];
                    av_strerror(ret, error_buf, sizeof(error_buf));
                    ERROR_PRINT("Could not create drawtext filter: " << error_buf);
                    continue;
                }
                
                // Link this filter to the previous one
                ret = avfilter_link(lastFilter, 0, drawTextCtx, 0);
                if (ret < 0) {
                    char error_buf[256];
                    av_strerror(ret, error_buf, sizeof(error_buf));
                    ERROR_PRINT("Could not link drawtext filter: " << error_buf);
                    return false;
                }
                
                // Update lastFilter for next iteration
                lastFilter = drawTextCtx;

        }

        // Connect through overlay if logo exists
        if (logoBufferCtx && overlayCtx) {

            DEBUG_PRINT("logoBufferCtx: " << lastFilter);
            
            // Connect: lastFilter -> overlay:0 (main video with text)
            ret = avfilter_link(lastFilter, 0, overlayCtx, 0);
            if (ret < 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                ERROR_PRINT("Could not link to overlay input 0. Error: " << error_buf);
                return false;
            }
            
            // Connect: logoBuffer -> overlay:1 (logo)
            ret = avfilter_link(logoBufferCtx, 0, overlayCtx, 1);
            if (ret < 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                ERROR_PRINT("Could not link logo to overlay input 1. Error: " << error_buf);
                return false;
            }
            
            // Connect: overlay -> bufferSink
            ret = avfilter_link(overlayCtx, 0, bufferSinkCtx, 0);
            if (ret < 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                ERROR_PRINT("Could not link overlay to sink. Error: " << error_buf);
                return false;
            }
        } else {

            DEBUG_PRINT("lastFilter: " << lastFilter);

            ret = avfilter_link(lastFilter, 0, bufferSinkCtx, 0);
            if (ret < 0) {
                char error_buf[256];
                av_strerror(ret, error_buf, sizeof(error_buf));
                ERROR_PRINT("Could not link to buffer sink. Error: " << error_buf);
                return false;
            }
        }        

        DEBUG_PRINT("configGraph");
        
        // Configure graph
        ret = avfilter_graph_config(filterGraph, nullptr);
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            ERROR_PRINT("Could not configure filter graph. Error: " << error_buf);
            return false;
        }

        DEBUG_PRINT("final verification");
        
        // Final verification
        if (!bufferSrcCtx || !bufferSinkCtx) {
            ERROR_PRINT("One or more filter contexts became null during initialization");
            ERROR_PRINT("bufferSrcCtx: " << bufferSrcCtx << ", bufferSinkCtx: " << bufferSinkCtx);
            return false;
        }

        DEBUG_PRINT("end of init");
        
        initialized = true;
        return true;
    }
    
    AVFrame* FrameOverlayProcessor::processFrame(AVFrame* inputFrame) {

    if (inputFrame->width != frameWidth || inputFrame->height != frameHeight) {
        frameWidth = inputFrame->width;
        frameHeight = inputFrame->height;
        initialized = false;
        cout << "processFrame# frame size change to: " << frameWidth << " x " << frameHeight << endl;
        if (initializeFilterGraph()) 
            processFrame(inputFrame);
        else 
            return nullptr;
    }
        
    if (!filterGraph) {
        ERROR_PRINT("missing filterGraph");
        return nullptr;
    }
    
    if (!bufferSrcCtx) {
        ERROR_PRINT("missing bufferSrcCtx");
        return nullptr;
    }
    
    if (!bufferSinkCtx) {
        ERROR_PRINT("missing bufferSinkCtx");
        return nullptr;
    }
    
    if (inputFrame->format != pixelFormat) {
        ERROR_PRINT("Frame format mismatch! Input: " << inputFrame->format << ", Expected: " << pixelFormat);
        return nullptr;
    }        

    // Push frame to buffer source
    DEBUG_PRINT("Pushing frame to buffer source...");
    int ret = av_buffersrc_add_frame_flags(bufferSrcCtx, inputFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) {
        char error_buf[256];
        av_strerror(ret, error_buf, sizeof(error_buf));
        ERROR_PRINT("Error adding frame to buffer source: " << error_buf);
        return nullptr;
    }
    DEBUG_PRINT("Successfully pushed frame to buffer source");
    
    // Handle logo if present
    if (logoLoaded && logoFrame && logoBufferCtx) {
        DEBUG_PRINT("Adding logo frame...");
        logoFrame->pts = inputFrame->pts;
        logoFrame->time_base = inputFrame->time_base;
        
        ret = av_buffersrc_add_frame_flags(logoBufferCtx, logoFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) {
            char error_buf[256];
            av_strerror(ret, error_buf, sizeof(error_buf));
            ERROR_PRINT("Error while feeding logo to filtergraph: " << error_buf);
            return nullptr;
        }
        DEBUG_PRINT("Successfully added logo frame");
    }        
    
    // Get processed frame
    DEBUG_PRINT("Getting processed frame from buffer sink...");
    AVFrame* outputFrame = av_frame_alloc();
    ret = av_buffersink_get_frame(bufferSinkCtx, outputFrame);
    if (ret < 0) {
        char error_buf[256];
        av_strerror(ret, error_buf, sizeof(error_buf));
        ERROR_PRINT("av_buffersink_get_frame failed: " << error_buf);
        av_frame_free(&outputFrame);
        return nullptr;
    }
    
    DEBUG_PRINT("Successfully got output frame: " << outputFrame->width << "x" << outputFrame->height 
                << ", format=" << outputFrame->format << ", pts=" << outputFrame->pts);
    DEBUG_PRINT("=== PROCESS FRAME END ===");
    
    return outputFrame;
}