#include <zcamSnapshot.h>
#include <iomanip>

#include <someFFMpeg.h>

ZCAMSnapshot::ZCAMSnapshot(json config) {

	this->config = config;
	root = config["files"].get<string>();
	cam_idx = stoi(config["cam_id"].get<string>());

	zcam = new ZCAM(config, cam_idx);
}

string ZCAMSnapshot::take() {

	auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);       
    stringstream ss;
    ss << root << "zcam/SNAP" << cam_id << std::put_time(std::localtime(&time_t), "%H%M") << ".JPG";	

    if (zcam->initStream()) {
	    AVFrame *frame = zcam->getFrame();
	    someFFMpeg::saveAVFrameAsJPEG(frame, ss.str(), 100);
		av_frame_free(&frame);
	    zcam->closeStream();    	
	    return ss.str();
    }

    return ""; // ERROR

}