
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <peocl_logger.h>
#include <service.h>
#include <network.h>

namespace fs = std::filesystem; // For C++17
namespace fs = std::filesystem;

const std::string b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";//=

const string SESSIONS = "/home/surfai/files/sessions/"; // TODO: load from config file
const string CACHE = "/home/surfai/files/cache/"; // TODO: load from config file

someService::someService(json config, string serviceName) {

    this->config = config;
    this->server = this->config["server"].get<string>();
    this->host = config["host"].get<string>();
    this->serviceName = serviceName;

    std::cout << "service ready" << std::endl;
}

bool isUrl(const string& file) {
    return file.substr(0, 7) == "http://" || file.substr(0, 8) == "https://";
}

struct UrlParts {
    string host;
    string path;
    string last;
};

UrlParts splitUrl(const string& url) {
    // Skip the protocol part
    size_t start = url.find("://");
    if (start == string::npos) {
        start = 0;
    } else {
        start += 3; // Skip "://"
    }
    
    // Find the first '/' after the protocol
    size_t pathStart = url.find('/', start);

    string host;
    string path;
    string last;    

    if (pathStart == string::npos) {
        // No path found, return only host
       host = url.substr(start);
       path = "/";
       last = "";
    }
    
    host = url.substr(start, pathStart - start);
    path = url.substr(pathStart);

    // Find the last component
    size_t lastSlash = path.rfind('/');
    if (lastSlash != string::npos) {
        last = path.substr(lastSlash + 1);
    } else {
        last = path;
    }
    
    return {host, path, last};
}

void someService::post_response(json request, string status, json response) {
    Network network;
    auto params = json();
    params["request"] = request;
    params["status"] = status;
    params["host"] = host;
    if (!response.empty()) params["response"] = response;
    network.https_request(server, "/apis/requests/response", http::verb::post, params);
}

void someService::post_status(string status) {
    Network network;
    auto params = json();
    params["service"] = serviceName;
    params["host"] = host;
    params["status"] = status;
    network.https_request(server, "/apis/requests/status", http::verb::post, params);
}

void someService::run() {

    someNetwork network;

    post_status("init");

    someLogger::getInstance()->log("START SERVICE");

    while (true) {

        try {

            auto response = network.https_get(server, "/apis/requests?service=" + serviceName + "&host=" + host);

            if (response.timeout) continue;

            auto json = response.json;

            string api;

            if (response.status == 200 && json.contains("api")) {
                api = json["api"].get<string>();
                if (api == "keepalive") continue;
            }

            string request_id;

            auto params = nlohmann::json();

            if (response.status == 200 && !response.str.empty()) {
                if (json.contains("id")) request_id = json["id"].get<string>();
                if (json.contains("params")) params = json["params"];
            }

            // auto job = response.json["job"];
            // size_t job_id = job["jobId"];
            // string task = job["task"];

            // auto jparams = job["params"];
            // string access_token = jparams.contains("access_token") ? jparams["access_token"] : "";

            // auto params = nlohmann::json();
            // params["jobId"] = job_id;
            // params["host"] = host;
            // params["data"] = nlohmann::json();

            // if (task == "file") {
            //     string event_id = jparams["eventId"];
            //     string file_name = jparams["fileName"];
            //     string path = string("downloads/").append(event_id).append("/swfiles/").append(file_name);
            //     auto size = uploader.tcpUpload(path, job_id);
//                if (size == 0)
//                    params["status"] = "error";
//                else {
//                    params["data"]["size"] = size;
//                    params["status"] = "done";
//                }
//                network.https_request(server, "/updateJob", http::verb::put, params);

            // else if (task == "download") {
            //     string event_id = jparams["eventId"];
            //     vector<string> files = jparams["fileIds"];
            //     string fileName = jparams.contains("fileName") ? jparams["fileName"].get<string>() : to_string(job_id);
            //     string driveId = storage.uploadFiles(event_id, fileName, files, access_token);
            //     storage.setFilePermissions(driveId, access_token);
//                auto jfile = storage.getFile(driveId, access_token);
//                cout << "getFile response# "  << jfile.dump() << endl;
//                https://drive.google.com/uc?id=1ZRRukmSHNF6lWuL4G1-fycJyYQsXXvzb&export=download
//                params["data"]["downloadLink"] = jfile["webContentLink"];
            //     params["data"]["downloadLink"] = "https://drive.google.com/uc?id=" + driveId + "&export=download";
            //     params["status"] = "ready";
            //     network.https_request(server, "/updateJob", http::verb::put, params);
            // }

            // else if (task == "scan") {
            //     params["data"]["files"] = nlohmann::json();
            //     params["data"]["filesIds"] = nlohmann::json();
            //     params["data"]["filesInfo"] = nlohmann::json();
            //     string event_id = jparams["eventId"];
            //     string folder_id = jparams["folderId"];
            //     auto st = storage.downloadFiles(event_id + "/files", folder_id, params, access_token);
            //     if (st) {
            //         storage.reportProgress(params);
            //         scan(event_id + "/files", params, true);
            //         params["status"] = "done";
            //     }
            //     else
            //         params["status"] = "error";
            //     params["processTime"] = Logger::timeSinceEpochMilli() - start_time;
            //     network.https_request(server, "/updateJob", http::verb::put, params);
            //     if (st)
            //         Database::getInstance()->log();
            // }

            // else if (task == "search" && job["params"].contains("folderId")) {
            //     params["data"]["files"] = nlohmann::json();
            //     params["data"]["filesIds"] = nlohmann::json();
            //     params["data"]["filesInfo"] = nlohmann::json();
            //     string event_id = jparams["eventId"];
            //     string folder_id = jparams["folderId"];
            //     auto ok = storage.downloadFiles(event_id + "/selfies", folder_id, params, access_token);
            //     if (!ok) params["status"] = "error";
            //     if (ok) {
            //         Logger::getInstance()->log("downloadFiles ok!");
            //         auto dbValues = scan(event_id + "/selfies", params, true);
            //         Logger::getInstance()->log("post scan files# " + params["data"]["files"].dump());
            //         auto skip = string(event_id + "/selfies/").size();
            //         printf("scanned %ld faces", dbValues.size());
            //         for (int i = 0; i < dbValues.size(); i++) {
            //             auto dbValue = dbValues[i];
            //             auto path = dbValue.dbPath;
            //             string id = dbValue.fileId; // path.substr(skip);
            //             Logger::getInstance()->log("post fileId# " + id);
            //             params["data"]["filesIds"][id] = true;
            //             if (!params["data"]["files"][id].contains("faces"))
            //                 params["data"]["files"][id]["faces"] = vector<nlohmann::json>();
            //             nlohmann::json jface;
            //             // todo: add face attributes
            //             jface["neighbors"] = vector<nlohmann::json>();
            //             auto faces = search(dbValue);
            //             for (DBValue face: faces) {
            //                 nlohmann::json n;
            //                 n["fileId"] = face.fileId;
            //                 n["index"] = face.faceIndex;
            //                 n["similarity"] = face.similarity;
            //                 jface["neighbors"].push_back(n);
            //                 if (!face.fileId.empty() && face.fileId != "selfie")
            //                     params["data"]["filesIds"][face.fileId] = true;
            //             }
            //             params["data"]["files"][id]["faces"].push_back(jface);
            //         }
            //         Logger::getInstance()->log("post search files# " + params["data"]["files"].dump());
            //         params["status"] = "done";
            //         Database::getInstance()->log();
            //     }
            //     network.https_request(server, "/updateJob", http::verb::put, params);
            // }

            else if (api == "shutdown") break;

            else { 
                this_thread::sleep_for(std::chrono::milliseconds(10000));
            }

        }
        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }

    }
}
