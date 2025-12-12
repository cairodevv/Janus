#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <optional>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct ProcessCtx {
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    bool running = false;
};

struct SessionState {
    std::string cwd;
    std::vector<std::string> history;
    std::optional<ProcessCtx> proc;
};

static std::string get_cwd_safe() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
    return ".";
}

static std::optional<ProcessCtx> spawn_process(const std::string& cmdline, const std::string& cwd) {
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) return std::nullopt;

    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        chdir(cwd.c_str());
        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        dup2(fromChild[1], STDERR_FILENO);
        close(toChild[1]); close(fromChild[0]);
        execlp("bash", "bash", "-lc", cmdline.c_str(), (char*)nullptr);
        _exit(127);
    }

    close(toChild[0]); close(fromChild[1]);
    ProcessCtx ctx{pid, toChild[1], fromChild[0], true};
    return std::optional<ProcessCtx>{ctx};
}

static void send_prompt(websocket::stream<tcp::socket>& ws, const std::string& cwd) {
    std::string msg = "{\"type\":\"prompt\",\"cwd\":\"" + cwd + "\"}";
    ws.write(net::buffer(msg));
}

static void send_error(websocket::stream<tcp::socket>& ws, const std::string& message) {
    std::string msg = "{\"type\":\"error\",\"message\":\"" + message + "\"}";
    ws.write(net::buffer(msg));
}

static void send_out(websocket::stream<tcp::socket>& ws, const std::string& data) {
    // raw text frame
    ws.write(net::buffer(data));
}

static void send_eof(websocket::stream<tcp::socket>& ws) {
    ws.write(net::buffer(std::string("{\"type\":\"eof\"}")));
}

void session(tcp::socket socket) {
    try {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();

        SessionState state;
        state.cwd = get_cwd_safe();
        send_prompt(ws, state.cwd);

        std::thread reader_thread;
        bool reader_running = false;

        auto stop_process = [&]() {
            if (state.proc.has_value()) {
                reader_running = false;
                kill(state.proc->pid, SIGTERM);
                int st = 0;
                waitpid(state.proc->pid, &st, 0);
                if (reader_thread.joinable()) reader_thread.join();
                close(state.proc->stdin_fd);
                close(state.proc->stdout_fd);
                state.proc.reset();
                send_eof(ws);
            }
        };

        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());

            if (msg == "{\"type\":\"quit\"}") {
                stop_process();
                break;
            }
            if (msg.rfind("{\"type\":\"in\"", 0) == 0) {
                if (state.proc.has_value()) {
                    auto pos = msg.find("\"data\":\"");
                    if (pos != std::string::npos) {
                        auto start = pos + 8;
                        auto end = msg.find("\"", start);
                        std::string data = msg.substr(start, end - start);
                        write(state.proc->stdin_fd, data.c_str(), data.size());
                    }
                }
                continue;
            }
            if (msg.rfind("{\"type\":\"cmd\"", 0) == 0) {
                auto pos = msg.find("\"line\":\"");
                if (pos == std::string::npos) continue;
                auto start = pos + 8;
                auto end = msg.find("\"", start);
                std::string line = msg.substr(start, end - start);

                state.history.push_back(line);

                if (line == "exit") { stop_process(); break; }
                if (line == "pwd") { send_out(ws, state.cwd + "\n"); continue; }

                stop_process();
                auto ctx_opt = spawn_process(line, state.cwd);
                if (!ctx_opt.has_value()) { send_error(ws, "failed to start"); continue; }
                state.proc.emplace(std::move(*ctx_opt));
                send_prompt(ws, state.cwd);

                reader_running = true;
                reader_thread = std::thread([&]() {
                    char buf[4096];
                    while (reader_running) {
                        ssize_t n = read(state.proc->stdout_fd, buf, sizeof(buf));
                        if (n > 0) send_out(ws, std::string(buf, n));
                        else break;
                    }
                });

                std::thread([&ws, &state, &reader_running]() {
                    int status = 0;
                    waitpid(state.proc->pid, &status, 0);
                    reader_running = false;
                    if (state.proc.has_value()) {
                        close(state.proc->stdin_fd);
                        close(state.proc->stdout_fd);
                        state.proc.reset();
                        send_eof(ws);
                        send_prompt(ws, state.cwd);
                    }
                }).detach();
            }
        }

        stop_process();
        ws.close(websocket::close_code::normal);

    } catch (std::exception const& e) {
        std::cerr << "Session error: " << e.what() << "\n";
    }
}

int main() {
    try {
        net::io_context ioc;
        tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 9002));
        std::cout << "Runner listening on ws://0.0.0.0:9002\n";
        for (;;) {
            tcp::socket socket(ioc);
            acceptor.accept(socket);
            std::thread(&session, std::move(socket)).detach();
        }
    } catch (std::exception const& e) {
        std::cerr << "Runner error: " << e.what() << "\n";
        return 1;
    }
}
