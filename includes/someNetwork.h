//
// Created by muli on 12/19/23.
//

#ifndef FACEVALUE_NETWORK_H
#define FACEVALUE_NETWORK_H

#include <string>
#include <nlohmann/json.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <future>

namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http = beast::http;       // from <boost/beast/http.hpp>

using namespace std;

class someNetwork {

    bool log = false;
    uint parse_response(http::response<http::dynamic_body> res, bool verbose = false);

public:

    struct Response {
        string str;
        nlohmann::json json;
        uint status = 200; // wishful thinking
        bool timeout = false;
    };

    static string urlencode(const std::string &s);
    void set_log(bool enabled) { log = enabled; };
    Response http_get(string host, string url, string port = "80");
    Response http_request(string host, string url, http::verb method, nlohmann::json params = nlohmann::json(), string port = "80");
    std::future<Response> https_async_get(string host, string url, string authorization = "", string port = "443");
    Response https_get(string host, string url, string authorization = "", string port = "443");
    Response https_request(string host, string url, http::verb method, nlohmann::json params = nlohmann::json(), string authorization = "", string port = "443");
    bool https_download(string host, string url, string path, string authorization = "", string port = "443");
};


#endif //FACEVALUE_NETWORK_H
