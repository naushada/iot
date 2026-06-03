/// L18/D2 — Router unit tests.
/// Pure logic: register routes, dispatch requests, assert responses.

#include <gtest/gtest.h>

#include <string>

#include "http_server/parser.hpp"
#include "http_server/router.hpp"

using http_server::HttpParser;
using http_server::HttpResponse;
using http_server::Router;

namespace {

HttpParser::Request make_req(std::string method, std::string path,
                              std::string body = {}) {
    HttpParser::Request r;
    r.method = std::move(method);
    r.path   = std::move(path);
    r.body   = std::move(body);
    return r;
}

} // namespace

TEST(Router, Route_exact_match) {
    Router r;
    r.add("GET", "/a", [](const HttpParser::Request&) {
        HttpResponse resp;
        resp.body = "OK";
        return resp;
    });

    auto resp = r.route(make_req("GET", "/a"));
    EXPECT_EQ(200, resp.status);
    EXPECT_EQ("OK", resp.body);
}

TEST(Router, Route_method_mismatch_404) {
    Router r;
    r.add("GET", "/a", [](const HttpParser::Request&) {
        return HttpResponse{};
    });

    auto resp = r.route(make_req("POST", "/a"));
    EXPECT_EQ(405, resp.status);   // path exists but method mismatch
}

TEST(Router, Route_path_mismatch_404) {
    Router r;
    r.add("GET", "/a", [](const HttpParser::Request&) {
        return HttpResponse{};
    });

    auto resp = r.route(make_req("GET", "/b"));
    EXPECT_EQ(404, resp.status);
}

TEST(Router, Route_multiple_routes) {
    Router r;
    r.add("GET",  "/x", [](const HttpParser::Request&) {
        HttpResponse resp; resp.body = "get-x"; return resp; });
    r.add("POST", "/x", [](const HttpParser::Request&) {
        HttpResponse resp; resp.body = "post-x"; return resp; });
    r.add("GET",  "/y", [](const HttpParser::Request&) {
        HttpResponse resp; resp.body = "get-y"; return resp; });

    EXPECT_EQ("get-x",  r.route(make_req("GET",  "/x")).body);
    EXPECT_EQ("post-x", r.route(make_req("POST", "/x")).body);
    EXPECT_EQ("get-y",  r.route(make_req("GET",  "/y")).body);
}

TEST(Router, Route_handler_receives_request) {
    Router r;
    r.add("POST", "/submit", [](const HttpParser::Request& req) {
        HttpResponse resp;
        resp.body = req.body;
        return resp;
    });

    auto resp = r.route(make_req("POST", "/submit", "payload"));
    EXPECT_EQ(200, resp.status);
    EXPECT_EQ("payload", resp.body);
}
