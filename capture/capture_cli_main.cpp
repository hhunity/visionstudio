// Debug CLI tool for capture HTTP server communication.
//
// Usage:
//   capture_cli post <path> [--host HOST] [--port PORT]
//   capture_cli sse          [--host HOST] [--port PORT]
//
// Examples:
//   capture_cli post /connect
//   capture_cli post /start --host 192.168.1.10 --port 9090
//   capture_cli sse
//   capture_cli sse --port 9090

#include <CLI/CLI.hpp>
#include <httplib.h>
#include <iostream>
#include <string>

static void run_post(const std::string& host, int port, const std::string& path,
                     const std::string& body, const std::string& content_type) {
    const std::string url = host + ":" + std::to_string(port) + path;
    std::cout << "POST " << url << "\n";
    if (!body.empty())
        std::cout << "body: " << body << "\n";

    httplib::Client cli(host, port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    httplib::Result res;
    if (body.empty())
        res = cli.Post(path);
    else
        res = cli.Post(path, body, content_type);

    if (!res) {
        std::cerr << "-> (no response / connection error)\n";
        return;
    }
    std::cout << "-> " << res->status << "\n";
    if (!res->body.empty())
        std::cout << "body:\n" << res->body << "\n";
}

static void run_sse(const std::string& host, int port, const std::string& path) {
    const std::string url = host + ":" + std::to_string(port) + path;
    std::cout << "GET " << url << " (SSE stream, Ctrl-C to stop)\n";

    httplib::Client cli(host, port);
    cli.set_read_timeout(0, 0); // no timeout for streaming

    std::string buf;

    cli.Get(
        path,
        httplib::Headers{{"Accept",        "text/event-stream"},
                         {"Cache-Control", "no-cache"},
                         {"Connection",    "keep-alive"}},

        [&](const httplib::Response& r) -> bool {
            std::cout << "-> " << r.status << "\n";
            if (r.status < 200 || r.status >= 300) {
                std::cerr << "error: HTTP " << r.status << "\n";
                return false;
            }
            std::cout << "stream open\n";
            return true;
        },

        [&](const char* data, size_t len) -> bool {
            const std::string chunk(data, len);
            std::cout << "raw << " << chunk;
            if (!chunk.empty() && chunk.back() != '\n')
                std::cout << "\n";
            std::cout.flush();

            buf.append(chunk);
            size_t pos;
            std::string cur_event, cur_data;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();

                if (line.empty()) {
                    if (!cur_event.empty() || !cur_data.empty()) {
                        std::cout << "[event] event=" << cur_event
                                  << "  data=" << cur_data << "\n";
                        cur_event.clear();
                        cur_data.clear();
                    }
                } else if (line.rfind("event:", 0) == 0) {
                    cur_event = line.substr(6);
                    if (!cur_event.empty() && cur_event.front() == ' ')
                        cur_event.erase(0, 1);
                } else if (line.rfind("data:", 0) == 0) {
                    cur_data = line.substr(5);
                    if (!cur_data.empty() && cur_data.front() == ' ')
                        cur_data.erase(0, 1);
                }
            }
            return true;
        });

    std::cout << "stream closed\n";
}

int main(int argc, char** argv) {
    CLI::App app{"capture_cli - debug HTTP client for capture server"};
    app.require_subcommand(1);

    // ---------- post ----------
    auto* post_cmd = app.add_subcommand("post", "Send POST to a path");

    std::string post_host = "localhost";
    int         post_port = 8080;
    std::string post_path = "/connect";
    std::string post_body;
    std::string post_ct = "application/json";

    post_cmd->add_option("path", post_path, "URL path, e.g. /start")->required();
    post_cmd->add_option("--host", post_host, "Server host")->default_str("localhost");
    post_cmd->add_option("--port", post_port, "Server port")->default_str("8080");
    post_cmd->add_option("--body,-b", post_body, "Request body (optional)");
    post_cmd->add_option("--content-type,-c", post_ct, "Content-Type")->default_str("application/json");

    post_cmd->callback([&] {
        run_post(post_host, post_port, post_path, post_body, post_ct);
    });

    // ---------- sse ----------
    auto* sse_cmd = app.add_subcommand("sse", "Connect to SSE stream");

    std::string sse_host = "localhost";
    int         sse_port = 8080;
    std::string sse_path = "/events";

    sse_cmd->add_option("--host", sse_host, "Server host")->default_str("localhost");
    sse_cmd->add_option("--port", sse_port, "Server port")->default_str("8080");
    sse_cmd->add_option("--path", sse_path, "SSE path")->default_str("/events");

    sse_cmd->callback([&] {
        run_sse(sse_host, sse_port, sse_path);
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
