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
ZCAMController *leftCamera;
ZCAMController *rightCamera;

	string site = argc > 1 ? argv[1] : "tlv1"; 

	config = someLogger::loadConfig(string("config/") + site + string(".json"));
	
	config["host"] = site;	

	root = config["files"].get<string>();
    someLogger::getInstance(root + "logs/zcam.log")->log("start zcam controller");

    json cameras = config["cameras"];

	leftCamera = new ZCAMController(config, 0);

	// rightCamera = new ZCAMController(config, 1);

    thread leftThread([leftCamera]() {
        leftCamera->run();
    });

    // thread rightThread([rightCamera]() {
    //     rightCamera->run();
    // });

    string serviceName = config["service"].get<string>();

    auto service = new someService(config, serviceName);
    someLogger::getInstance()->log("start service");
    service->run();
        
    if (leftThread.joinable()) {
        leftThread.join();
    }
        
    // if (rightThread.joinable()) {
    //     rightThread.join();
    // }

}
