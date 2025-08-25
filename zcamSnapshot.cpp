#include <zcamSnapshot.h>
#include <iomanip>

#include <someFFMpeg.h>
#include <focus.h>

ZCAMSnapshot::ZCAMSnapshot(json config) {

	this->config = config;
	root = config["files"].get<string>();
	cam_idx = stoi(config["cam_id"].get<string>());

	overlayProcessor = make_unique<FrameOverlayProcessor>(1920, 1080, AV_PIX_FMT_YUVJ420P); // TODO: Automatic
    overlayProcessor->setFont("", 50);
    overlayProcessor->setFontColor("0x443D24");  

	zcam = new ZCAM(config, cam_idx);
}

string ZCAMSnapshot::take() {

	auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);       
    stringstream ss;
    ss << root << "zcam/SNAP" << cam_idx << std::put_time(std::localtime(&time_t), "%H%M") << ".JPG";	

    if (zcam->initStream()) {
	    AVFrame *frame = zcam->getFrame();

	    overlayProcessor->clearGridText();

        auto dw = frame->width / 4;
        auto dh = frame->height / 4;
        
        for (int i = 0; i < 4; i++)
        	for (int j = 0; j< 4; j++) {
                                int x0 = static_cast<int>(i * dw);
                                int y0 = static_cast<int>(j * dh);
                                int x1 = static_cast<int>(i * dw + dw);
                                int y1 = static_cast<int>(j * dh + dh);
                                cout << "FAST ROI: " << x0 << "," << y0 << "," << x1 << "," << y1 << endl;
        	double focus = Focus::fastROI(frame, x0, y0, x1, y1);
            string text = to_string(static_cast<int>(focus));
            overlayProcessor->setGridText({x0 + 10, y0 + 10, focus, text});
        }

        overlayProcessor->initializeFilterGraph();

        AVFrame* snapFrame = overlayProcessor->processFrame(frame);
		av_frame_free(&frame);

	    someFFMpeg::saveAVFrameAsJPEG(snapFrame, ss.str(), 100);
		av_frame_free(&snapFrame);

	    zcam->closeStream();    	
	    return ss.str();
    }

    return ""; // ERROR

}