#include <snapshot.h>

Snapshot::Snapshot(json config) {
	root = config["files"].get<string>();
	cam_id = config["cam_id"].get<string>();
}

string Snapshot::take() {
	string path = root + "/"
	auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);       
    stringstream ss;
    ss << root << "zcam/snap" << cma_id << std::put_time(std::localtime(&time_t), "%H%M") << ".JPG";	
    return ss.str();
}