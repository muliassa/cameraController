//
// Created by muli on 12/19/23.
//

#include <someNetwork.h>

#include "root_certificates.hpp"

#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <filesystem>

#include "peocl_logger.h"

typedef unsigned char uchar;

namespace net = boost::asio;        // from <boost/asio.hpp>
namespace ssl = net::ssl;           // from <boost/asio/ssl.hpp>
using tcp = net::ip::tcp;           // from <boost/asio/ip/tcp.hpp>


string someNetwork::urlencode(const std::string &s)
{
    static const char lookup[]= "0123456789abcdef";
    std::stringstream e;
    for(int i=0, ix=s.length(); i<ix; i++)
    {
        const char& c = s[i];
        if ( (48 <= c && c <= 57) ||//0-9
             (65 <= c && c <= 90) ||//abc...xyz
             (97 <= c && c <= 122) || //ABC...XYZ
             (c=='-' || c=='_' || c=='.' || c=='~')
                )
        {
            e << c;
        }
        else
        {
            e << '%';
            e << lookup[ (c&0xF0)>>4 ];
            e << lookup[ (c&0x0F) ];
        }
    }
    return e.str();
}

uint someNetwork::parse_response(http::response<http::dynamic_body> res, bool verbose) {

    // parse response

    if (log || verbose) {
        std::cout << "https response => " << endl;

        std::cout << res.result_int() << std::endl;
        std::cout << res.result() << std::endl;
        std::cout << res.reason() << std::endl;

        for (auto& h : res.base()) {
            std::cout << "Field: " << h.name() << "/text: " << h.name_string() << ", Value: " << h.value() << "\n";
        }
    }

    return res.result_int();

}


someNetwork::Response someNetwork::http_get(string host, string url, string port) {

    Response response;

    try {

        someLogger::getInstance()->log(string("http_get: ") + host + " " + url);

        // The io_context is required for all I/O
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        auto const results = resolver.resolve(host, port);
        stream.connect(results);
        // Set up an HTTP GET request message
        http::request<http::string_body> req{http::verb::get, url, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        // Send the HTTP request to the remote host
        http::write(stream, req);
        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;
        // Declare a container to hold the response
        http::response<http::dynamic_body> res;
        // Receive the HTTP response
        http::read(stream, buffer, res);
        response.str = boost::beast::buffers_to_string(res.body().data());
        if (!response.str.empty()) {
            try {
                response.json = nlohmann::json::parse(response.str);
            } catch(exception const&e) {

            }
        }
        // Gracefully close the stream
//        beast::error_code ec;
//        stream.shutdown(ec);
//        if(ec == net::error::eof)
//        {
//            // Rationale:
//            // http://stackoverflow.com/questions/25587403/boost-asio-ssl-async-shutdown-always-finishes-with-an-error
//            ec = {};
//        }
//        if(ec)
//            throw beast::system_error{ec};

        // If we get here then the connection is closed gracefully
    }
    catch(std::exception const& e)
    {
        std::cerr << "http_get Error: " << e.what() << std::endl;
    }

    return response;
}

someNetwork::Response someNetwork::http_request(string host, string url, http::verb method, nlohmann::json params, string port) {

    if (log)
        std::cout << "http_request# " << url << " " << method << " " << params << std::endl;

    Response response;

    try {

        // The io_context is required for all I/O
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        auto const results = resolver.resolve(host, port);
        stream.connect(results);
        http::request<http::string_body> req{method, url, 11};

        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        req.set(http::field::content_type, "application/json");
        req.body() = params.dump();

//        req.set(http::field::content_type, "application/x-www-form-urlencoded");
//        req.body() = "name=foo";

        req.prepare_payload();

        // Send the HTTP request to the remote host
        http::write(stream, req);
        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;
        // Declare a container to hold the response
        http::response<http::dynamic_body> res;
        // Receive the HTTP response
        http::read(stream, buffer, res);
        response.str = boost::beast::buffers_to_string(res.body().data());

        if (!response.str.empty()) {
            std::cout << "http_request response# " << response.str << std::endl;
            response.json = nlohmann::json::parse(response.str);
        }

    }
    catch(std::exception const& e)
    {
        std::cerr << "http_request Error# " << e.what() << std::endl;
    }
    return response;
}

future<someNetwork::Response> someNetwork::https_async_get(string host, string url, string authorization, string port) {

    std::cout << "async_get# " << host << " " << url << std::endl;

    auto promise = std::make_shared<std::promise<Response>>();
    std::future<Response> future = promise->get_future();

    // The io_context is required for all I/O
    auto ioc = std::make_shared<net::io_context>();

    // The SSL context is required, and holds certificates
    auto ctx = std::make_shared<ssl::context>(ssl::context::tlsv12_client);

    // This holds the root certificate used for verification
    load_root_certificates(*ctx);

    // Verify the remote server's certificate
    ctx->set_verify_mode(ssl::verify_peer);

    // Create a timer for the entire operation
    auto timeout_timer = std::make_shared<net::steady_timer>(*ioc, std::chrono::seconds(60));

    // These objects perform our I/O
    auto resolver = std::make_shared<tcp::resolver>(*ioc);
    auto stream = std::make_shared<beast::ssl_stream<beast::tcp_stream>>(*ioc, *ctx);

    // Timeout flag
    auto timer_cancelled = std::make_shared<std::atomic<bool>>(false);

        // Run the io_context BEFORE any blocking operations
        // Use a thread to run the context

    // Set up the timeout handler
    timeout_timer->async_wait([stream, promise, timer_cancelled](beast::error_code ec) {
        if (ec != net::error::operation_aborted) {
            std::cerr << "Stream timeout#" << std::endl;
            *timer_cancelled = true;
            beast::error_code close_ec;
            stream->next_layer().socket().close(close_ec);
            
            // Set an exception on the promise if timeout occurs
            // promise->set_exception(std::make_exception_ptr(std::runtime_error("Operation timed out")));

            Response response;  
            response.timeout = true;
            promise->set_value(response); // std::move(*res);

            // ioc->stop();
            // if (work_thread && work_thread->joinable()) {
            //     work_thread->join();
            // }

        }
    });

    // Set SNI Hostname (many hosts need this to handshake successfully)
    // if(! SSL_set_tlsext_host_name(stream->native_handle(), host.c_str()))
    // {
    //     beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
    //     throw beast::system_error{ec};
    // }

        // Resolver
        resolver->async_resolve(host, port, 
            [=](beast::error_code ec, tcp::resolver::results_type results) {
            
                if (ec) {
    
                    Response response;  
                    promise->set_value(response); // std::move(*res);


                    ioc->stop();
                    // if (work_thread && work_thread->joinable()) {
                    //     work_thread->join();
                    // }

                    // promise->set_exception(std::make_exception_ptr(
                    //     beast::system_error(ec, "Resolve failed")
                    // ));
                    return;
                }

            // Connect
            stream->next_layer().async_connect(results,
                [=](beast::error_code ec, tcp::endpoint) {
                    if (ec) {

                        Response response;  
                        promise->set_value(response); // std::move(*res);


                        ioc->stop();
                        // if (work_thread && work_thread->joinable()) {
                        //     work_thread->join();
                        // }
                        return;
                    }

                    // SSL Handshake
                    stream->async_handshake(ssl::stream_base::client, 
                        [=](beast::error_code ec) {

                            if (ec) {

                                Response response;  
                                promise->set_value(response); // std::move(*res);

                                ioc->stop();
                                // if (work_thread && work_thread->joinable()) {
                                //     work_thread->join();
                                // }
                                return;
                            }

                            // Prepare request
                            auto req = std::make_shared<http::request<http::string_body>>(
                                http::verb::get, url, 11
                            );
                            req->set(http::field::host, host);
                            req->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
                            if (!authorization.empty())
                                req->set(http::field::authorization, authorization);
                            req->prepare_payload();
                    
                            // Send the HTTP request to the remote host
                            http::write(*stream, *req);

                            // Send request
                            // http::async_write(*stream, *req, 
                            //     [=](beast::error_code ec, std::size_t) {
                            //         if (ec) 
                            //         {
                            //             std::cout << "write error" << std::endl;

                            //             promise->set_exception(std::make_exception_ptr(
                            //                 beast::system_error(ec, "Write failed")
                            //             ));
                            //             return;
                            //         }

                                    // Prepare response buffer
                                    auto buffer = std::make_shared<beast::flat_buffer>();
                                    auto res = std::make_shared<http::response<http::dynamic_body>>();

                                    // Read response
                                    http::async_read(*stream, *buffer, *res, 
                                        [=](beast::error_code ec, std::size_t) {

                                            // Cancel timeout timer
                                            timeout_timer->cancel();

                                            Response response;  

                                            if (ec) {

                                                std::cout << "read error" << ec.message() << std::endl;

                                                promise->set_value(response); // std::move(*res);

                                                ioc->stop();
                                                // if (work_thread && work_thread->joinable()) {
                                                //     work_thread->join();
                                                // }

                                                return;
                                            }

                                            // Check if timeout occurred
                                            if (*timer_cancelled) {

                                                std::cout << "timeout detected" << std::endl;

                                                promise->set_value(response); // std::move(*res);

                                                ioc->stop();
                                                // if (work_thread && work_thread->joinable()) {
                                                //     work_thread->join();
                                                // }

                                                return;
                                            }
                                            
                                            res->result_int();

                                            // Extract body as string
                                            const auto& body = res->body();
                                            std::string bodyStr = boost::beast::buffers_to_string(body.data());
                                            response.str = bodyStr;

                                            // Try to parse JSON (if possible)
                                            try {
                                                response.json = nlohmann::json::parse(bodyStr);
                                            } catch (const nlohmann::json::parse_error&) {
                                                // If parsing fails, just leave json as default constructed
                                            }

                                            promise->set_value(response); // std::move(*res);

                                            ioc->stop();
                                            // if (work_thread && work_thread->joinable()) {
                                            //     work_thread->join();
                                            // }

                                            stream->async_shutdown(
                                                [](beast::error_code ec) {
                                                    if (ec == net::error::eof || 
                                                        ec == ssl::error::stream_truncated) {
                                                        ec = {};
                                                    }
                                                    if (ec) {
                                                        std::cerr << "Shutdown error: " 
                                                                  << ec.message() << std::endl;
                                                    }

                                                }
                                            );


                                        });


                                // });


                        });

                });
            
            });


    auto work_thread = std::make_shared<std::thread>([ioc]() {
            ioc->run();
            std::cout << "ioc done" << std::endl;
    });

    return future;

}


someNetwork::Response someNetwork::https_get(string host, string url, string authorization, string port) {

    Response response;

    try {
        net::io_context ioc;
        ssl::context ctx(ssl::context::sslv23_client);
        load_root_certificates(ctx);
        ctx.set_verify_mode(ssl::verify_peer);
        
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        
        // Set SNI
        if(!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }
        
        // Resolve with timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        auto const results = resolver.resolve(host, port);
        
        // Connect with timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        beast::get_lowest_layer(stream).connect(results);
        
        // SSL handshake with timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        stream.handshake(ssl::stream_base::client);
        
        // HTTP write with timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        http::request<http::string_body> req{http::verb::get, url, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        if (!authorization.empty())
            req.set(http::field::authorization, authorization);
        req.prepare_payload();
        http::write(stream, req);
        
        // HTTP read with timeout
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(60));
        beast::flat_buffer buffer;
        http::response<http::dynamic_body> res;
        http::read(stream, buffer, res);
        
        // Disable timeout after success
        beast::get_lowest_layer(stream).expires_never();
        
        // Process response...
        response.str = boost::beast::buffers_to_string(res.body().data());
        response.status = parse_response(res);
        if (!response.str.empty())
            response.json = nlohmann::json::parse(response.str);
        
        // Clean shutdown
        beast::error_code ec;
        stream.shutdown(ec);
        if(ec == net::error::eof || ec == ssl::error::stream_truncated) {
            ec = {};
        }
        if(ec) {
            std::cerr << "Stream error# " << ec << std::endl;
        }
    }
    catch(beast::system_error const& se) {
        std::cerr << "Error: " << se.code().message() << std::endl;
    }

    /*

    try {

        std::cout << "https_get server# " << host + " url# " << url << " port# " << port << std::endl;

        // The io_context is required for all I/O
        net::io_context ioc;

        // The SSL context is required, and holds certificates
        ssl::context ctx(ssl::context::tlsv13_client);
        // ssl::context ctx(ssl::context::sslv23_client); // Auto-negotiates best version

        // This holds the root certificate used for verification
        load_root_certificates(ctx);

        // Verify the remote server's certificate
        ctx.set_verify_mode(ssl::verify_peer);
        
        net::steady_timer timeout_timer(ioc, std::chrono::seconds(60));

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Look up the domain name
        auto const results = resolver.resolve(host, port);

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(stream).connect(results);

        // Perform the SSL handshake
        stream.handshake(ssl::stream_base::client);

        // Set up an HTTP GET request message
        http::request<http::string_body> req{http::verb::get, url, 11};

        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        if (!authorization.empty())
            req.set(http::field::authorization, authorization);

        req.prepare_payload();

        // Send the HTTP request to the remote host
        http::write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::dynamic_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);

        // Cancel the timeout timer
        timeout_timer.cancel();

        response.str = boost::beast::buffers_to_string(res.body().data());
        response.status = parse_response(res);

        if (!response.str.empty())
            response.json = nlohmann::json::parse(response.str);

        // Optional: Close the stream gracefully
        beast::error_code ec;
        stream.shutdown(ec);
        if(ec == net::error::eof || ec == ssl::error::stream_truncated) {
            // Normalization of error
            ec = {};
        }

        if(ec) {
            std::cerr << "Stream error#" << ec << std::endl;
            // throw beast::system_error{ec};
        }

    }
    catch(beast::system_error const& se)
    {
        // Handle specific Beast/Boost errors
        std::cerr << "Error: " << se.code().message() << std::endl;
        // throw;
    }
    catch(std::exception const& e)
    {
        // Handle other exceptions (including timeout)
        std::cerr << "Exception: " << e.what() << std::endl;
        // throw;
    }

    */

    return response;
}

someNetwork::Response someNetwork::https_request(string host, string url, http::verb method, nlohmann::json params, string authorization, string port) {

    someLogger::getInstance()->log("https_request# " + host + " " + url + " " + params.dump(4));
    
    Response response;

    // The io_context is required for all I/O
    net::io_context ioc;

    try {


        // The SSL context is required, and holds certificates
        ssl::context ctx(ssl::context::tlsv12_client);

        // This holds the root certificate used for verification
        load_root_certificates(ctx);

        // Verify the remote server's certificate
        ctx.set_verify_mode(ssl::verify_peer);

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
//        beast::tcp_stream stream(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Look up the domain name
        auto const results = resolver.resolve(host, port);

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(stream).connect(results);

        // Perform the SSL handshake
        stream.handshake(ssl::stream_base::client);

        // Set up an HTTP GET request message
        http::request<http::string_body> req{method, url, 11};

        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::content_type, "application/json");

        req.body() = params.dump();

        if (!authorization.empty())
            req.set(http::field::authorization, authorization);

        req.prepare_payload();

        // Send the HTTP request to the remote host
        http::write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        // Declare a container to hold the response
        http::response<http::dynamic_body> res;

        // Receive the HTTP response
        http::read(stream, buffer, res);

        response.str = boost::beast::buffers_to_string(res.body().data());

        if (!response.str.empty())
            response.json = nlohmann::json::parse(response.str);

        parse_response(res);
    }
    catch(std::exception const& e)
    {
        someLogger::getInstance()->log("https_request# " + host + " " + url + " error# " + e.what());
    }

    ioc.run();

    return response;
}

bool someNetwork::https_download(string host, string url, string path, string authorization, string port) {

    someLogger::getInstance()->log("download# " + path + " url# " + url + " auth# " + authorization);

    if (std::filesystem::exists(path)) { 
        someLogger::getInstance()->log(path + " is cached!");
        return true;
    }

    try {

        // The io_context is required for all I/O
        net::io_context ioc;

        // The SSL context is required, and holds certificates
        ssl::context ctx(ssl::context::tlsv12_client);

        // This holds the root certificate used for verification
        load_root_certificates(ctx);

        // Verify the remote server's certificate
        ctx.set_verify_mode(ssl::verify_peer);

        // These objects perform our I/O
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

        // Set SNI Hostname (many hosts need this to handshake successfully)
        if(! SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
        {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Look up the domain name
        auto const results = resolver.resolve(host, "443");

        // Make the connection on the IP address we get from a lookup
        beast::get_lowest_layer(stream).connect(results);

        // Perform the SSL handshake
        stream.handshake(ssl::stream_base::client);

        // Set up an HTTP GET request message
        http::request<http::string_body> req{http::verb::get, url, 11};

        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);

        if (!authorization.empty())
            req.set("Authorization"/*http::field::authorization*/, authorization);

        // Send the HTTP request to the remote host
        http::write(stream, req);

        // This buffer is used for reading and must be persisted
        beast::flat_buffer buffer;

        beast::error_code ec;

        // Declare a container to hold the response
//        http::response<http::file_body> res;

        http::response_parser<http::file_body> parser;
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
        parser.get().body().open(path.c_str(), boost::beast::file_mode::write, ec);

        if (ec)
            someLogger::getInstance()->log("open error# " + ec.message());

        // Receive the HTTP response
        http::read(stream, buffer, /*res*/parser);

        parser.get().body().close();

        return true;

    }
    catch(std::exception const& e)
    {
        someLogger::getInstance()->log("download exception# " + string(e.what()));

        return false;
    }

}