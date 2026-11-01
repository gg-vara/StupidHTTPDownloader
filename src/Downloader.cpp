// StupidHTTPDownloader
// Really stupid library to download HTTP(S) content
// Copyright (C) 2021 Guillaume Vara

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <spdlog/spdlog.h>

#include "Downloader.h"
#include "UrlParser.h"

#include <cctype>

#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

namespace ssl = asio::ssl;

namespace {

// value of a header line if its name matches "lowercaseName" (including ':'), else empty
std::string headerValue(const std::string &headerLine, std::string_view lowercaseName) {
    if (headerLine.size() < lowercaseName.size()) return {};

    for (std::size_t i = 0; i < lowercaseName.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(headerLine[i])) != lowercaseName[i]) return {};
    }

    // trim surrounding spaces and the trailing '\r' left by getline()
    const auto begin = headerLine.find_first_not_of(" \t", lowercaseName.size());
    if (begin == std::string::npos) return {};
    const auto end = headerLine.find_last_not_of(" \t\r");

    return headerLine.substr(begin, end - begin + 1);
}

}  // namespace

template<>
Downloader::Response Downloader::_dumbGetFromScheme<Downloader::HandledSchemes::HTTPS>(const UrlParser &url, bool head, asio::io_context &io_context, const tcp::resolver::results_type &resolvedEndpoints) {
    // setup SSL
    ssl::context ssl_ctx(ssl::context::tls_client);
    ssl_ctx.set_default_verify_paths();

    // initiate
    asio::ssl::stream<tcp::socket> ssl_sock(io_context, ssl_ctx);

    // Server Name Indication, required by most shared hosts / CDNs to present the right certificate
    const auto host = url.host();
    SSL_set_tlsext_host_name(ssl_sock.native_handle(), host.c_str());

    // Connect to host
    asio::connect(ssl_sock.lowest_layer(), resolvedEndpoints);
    ssl_sock.lowest_layer().set_option(tcp::no_delay(true));

        // Perform SSL handshake and verify the remote host's certificate.
        // TODO(amphaal) might need to implement https://stackoverflow.com/questions/39772878/reliable-way-to-get-root-ca-certificates-on-windows
        // ssl_sock.set_verify_mode(ssl::verify_peer);
        // ssl_sock.set_verify_callback(ssl::host_name_verification(host));

    // no check of certificates
    ssl_sock.set_verify_mode(ssl::verify_none);
    ssl_sock.handshake(ssl::stream<tcp::socket>::client);

    //
    return _dumbGet(ssl_sock, url, head);
}

template<>
Downloader::Response Downloader::_dumbGetFromScheme<Downloader::HandledSchemes::HTTP>(const UrlParser &url, bool head, asio::io_context &io_context, const tcp::resolver::results_type &resolvedEndpoints) {
    // initiate
    tcp::socket socket(io_context);

    // Try each endpoint until we successfully establish a connection.
    asio::connect(socket, resolvedEndpoints);

    //
    return _dumbGet(socket, url, head);
}

template<typename Sock>
Downloader::Response Downloader::_dumbGet(Sock& sock, const UrlParser &url, bool head) {
    // start writing
    asio::streambuf request;
    std::ostream request_stream(&request);

    auto method = head ? "HEAD" : "GET";

    const auto getCommand = url.pathAndQuery();
    const auto host = url.host();

    request_stream << method << " " << getCommand << " HTTP/1.0\r\n";
    request_stream << "Host: " << host << "\r\n";
    request_stream << "Accept: */*\r\n";
    request_stream << "Connection: close\r\n";
    request_stream << "User-Agent: StupidHTTPDownloader\r\n";
    request_stream << "\r\n"; // signals end

    // Send the request.
    asio::write(sock, request);

    if (!head)
        spdlog::debug("StupidHTTPDownloader : Downloading [{}, {}]...",
            host, getCommand);

    // Read the response status line.
    asio::streambuf response;
    asio::read_until(sock, response, "\r\n");

        // Check that response is OK.
        std::istream response_stream(&response);

        // HTTP VERSION
        std::string http_version;
        response_stream >> http_version;

        // STATUS CODE
        unsigned int status_code;
        response_stream >> status_code;

        // STATUS MESSAGE
        std::string status_message;
        std::getline(response_stream, status_message);

    // Read the response headers, which are terminated by a blank line.
    asio::read_until(sock, response, "\r\n\r\n");

    // Process the response headers.
    std::string headerTmp;
    std::vector<std::string> headers;
    bool hasContentLengthHeader = false;
    std::string redirectUrl;

    // iterate to get headers
    while (std::getline(response_stream, headerTmp) && headerTmp != "\r") {
        headers.push_back(headerTmp);

        //
        if (!hasContentLengthHeader) {
            hasContentLengthHeader =
                headerTmp.find("Content-Length") != std::string::npos;
        }

        // find redirection url, on any redirection status
        if (status_code >= 300 && status_code < 400 && redirectUrl.empty()) {
            redirectUrl = headerValue(headerTmp, LocationTag);
        }
    }

    if (!headers.size())
        throw std::logic_error("StupidHTTPDownloader : Response have no headers !");

    // if not HEAD, read body message
    std::ostringstream output_stream;
    if (!head) {
        // Write whatever content we already have to output.
        if (response.size() > 0) {
            output_stream << &response;
        }
        // Read until EOF, writing data to output as we go.
        asio::error_code error;
        while (asio::read(sock, response, asio::transfer_at_least(1), error)) {
            output_stream << &response;
        }
    }

    spdlog::debug("StupidHTTPDownloader : Finished downloading [{}, {}]",
        host, getCommand);

    Response outResponse {
        hasContentLengthHeader,
        status_code,
        redirectUrl,
        output_stream.str(),
        headers
    };

    spdlog::debug("StupidHTTPDownloader : Response length {}, headers {}",
        outResponse.messageBody.size(),
        outResponse.headers.size());

    return outResponse;
}

Downloader::Response Downloader::dumbGet(const std::string &downloadUrl, bool head) {
    auto url = downloadUrl;

    for (int redirections = 0;; ++redirections) {
        auto response = _dumbGetOnce(url, head);
        if (response.redirectUrl.empty() || redirections == MaxRedirections) return response;

        // relative location
        if (response.redirectUrl.front() == '/') {
            const UrlParser current(url);
            response.redirectUrl = current.scheme() + "://" + current.host() + response.redirectUrl;
        }

        spdlog::debug("StupidHTTPDownloader : Redirected to [{}]", response.redirectUrl);
        url = response.redirectUrl;
    }
}

Downloader::Response Downloader::_dumbGetOnce(const std::string &downloadUrl, bool head) {
    // decompose url
    UrlParser url_decomposer(downloadUrl);
    auto serverName = url_decomposer.host();
    auto scheme = url_decomposer.scheme();

    // setup service
    asio::io_context io_context;

    // resolve IP
    tcp::resolver resolver(io_context);
    const auto endpoints = resolver.resolve(serverName, scheme);

    // switch HTTP / HTTPS
    if (url_decomposer.isHTTPS()) {
        return _dumbGetFromScheme<HandledSchemes::HTTPS>(url_decomposer, head, io_context, endpoints);
    } else {
        return _dumbGetFromScheme<HandledSchemes::HTTP> (url_decomposer, head, io_context, endpoints);
    }
}
