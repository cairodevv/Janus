#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <optional>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Naive JSON field extractor: expects {"key":"value"} pairs
static std::optional<std::string> get_field(const std::string& msg, const std::string& key) {
    std::string pattern = "\"" + key + "\":\"";
    auto p = msg.find(pattern);
    if (p == std::string::npos) return std::nullopt;
    auto start = p + pattern.size();
    auto end = msg.find("\"", start);
    if (end == std::string::npos) return std::nullopt;
    return msg.substr(start, end - start);
}

int main() {
    try {
        net::io_context ioc;
        tcp::resolver resolver{ioc};

        // 👉 Replace with your VM’s external IP
        auto results = resolver.resolve("10.152.0.5", "9002");
        websocket::stream<tcp::socket> ws{ioc};
        net::connect(ws.next_layer(), results);
        ws.handshake("10.152.0.5:9002", "/");

        std::atomic<bool> running{true};
        std::string prompt_cwd = "";

        // Reader thread: server -> console
        std::thread reader([&]() {
            try {
                while (running.load()) {
                    beast::flat_buffer buffer;
                    ws.read(buffer);
                    std::string msg = beast::buffers_to_string(buffer.data());

                    auto type = get_field(msg, "type");
                    if (!type.has_value()) {
                        // Raw output (command stdout/stderr)
                        std::cout << msg << std::flush;
                        continue;
                    }

                    if (type.value() == "prompt") {
                        auto cwd = get_field(msg, "cwd");
                        if (cwd.has_value()) {
                            prompt_cwd = cwd.value();
                            std::cout << "mini-shell:" << prompt_cwd << "> " << std::flush;
                        }
                    } else if (type.value() == "eof") {
                        std::cout << "\nmini-shell:" << prompt_cwd << "> " << std::flush;
                    } else if (type.value() == "error") {
                        auto m = get_field(msg, "message");
                        std::cerr << "error: " << (m.has_value() ? m.value() : msg) << "\n";
                        std::cout << "mini-shell:" << prompt_cwd << "> " << std::flush;
                    } else {
                        // Unknown control message
                        std::cout << msg << std::flush;
                    }
                }
            } catch (...) {
                // connection closed
            }
        });

        std::cout << "Connected. Type commands directly; input goes to running process.\n";
        std::cout << "Special commands:\n";
        std::cout << "  ^C line: send SIGINT\n";
        std::cout << "  :quit   : end client\n";
        std::cout << "To send input to the running process, prefix the line with '> '.\n";
        std::cout << "Built-ins (server-side): cd, pwd, echo, history, exit\n";

        // Input loop
        for (std::string line; std::getline(std::cin, line); ) {
            if (line == ":quit") {
                ws.write(net::buffer(std::string("{\"type\":\"quit\"}")));
                break;
            }
            if (line == "^C") {
                ws.write(net::buffer(std::string("{\"type\":\"ctrl\",\"signal\":\"SIGINT\"}")));
                continue;
            }

            if (!line.empty() && line.size() > 2 && line.rfind("> ", 0) == 0) {
                std::string data = line.substr(2);
                data.push_back('\n'); // typical terminal behavior
                std::string msg = "{\"type\":\"in\",\"data\":\"" + data + "\"}";
                ws.write(net::buffer(msg));
            } else {
                std::string msg = "{\"type\":\"cmd\",\"line\":\"" + line + "\"}";
                ws.write(net::buffer(msg));
            }
        }

        running = false;
        ws.close(websocket::close_code::normal);
        if (reader.joinable()) reader.join();

    } catch (std::exception const& e) {
        std::cerr << "Client error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
