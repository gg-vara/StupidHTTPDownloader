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

#pragma once

#include <string>
#include <vector>
#include <string_view>

#include <asio.hpp>
using asio::ip::tcp;

class UrlParser;

class Downloader {
 public:
    struct Response {
        bool hasContentLengthHeader = false;
        unsigned int statusCode = 0;
        std::string redirectUrl;
        std::string messageBody;
        std::vector<std::string> headers;
    };

    // follows redirections (3xx with a Location header), up to "MaxRedirections" times
    static Response dumbGet(const std::string &downloadUrl, bool head = false);

    static constexpr int MaxRedirections = 5;

 private:
    enum class HandledSchemes {
        HTTP,
        HTTPS
    };

    static Response _dumbGetOnce(const std::string &downloadUrl, bool head);

    template<HandledSchemes scheme>
    static Response _dumbGetFromScheme(const UrlParser &url, bool head, asio::io_context &io_context, const tcp::resolver::results_type &resolvedEndpoints);

    template<typename Sock>
    static Response _dumbGet(Sock& sock, const UrlParser &url, bool head);

    // header names are case-insensitive
    static constexpr std::string_view LocationTag = "location:";
};
