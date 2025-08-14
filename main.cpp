#include <string>
#include <iostream>

#include <zcamController.h>
#include <someService.h>
#include <someLogger.h>

using namespace std;
using json = nlohmann::json;

// Simple test of just the frame capture
int main(int argc, char* argv[]) {

json config;
string root;
ZCAMController *camera;

	string site = argv[1]; 

	string cam_id = argv[2];
	
	config = someLogger::loadConfig(string("config/") + site + string(".json"));
	
	config["host"] = site;	

	root = config["files"].get<string>();

    someLogger::getInstance(root + "logs/zcam" + cam_id + ".log")->log("start zcam controller");

    json cameras = config["cameras"];

	camera = new ZCAMController(config, stoi(cam_id));

    thread camThread([leftCamera]() {
        camera->run();
    });

    string serviceName = config["service"].get<string>();

    auto service = new someService(config, serviceName + cam_id);

    someLogger::getInstance()->log("start service");
    service->run();
        
    if (camThread.joinable()) {
        camThread.join();
    }

}
