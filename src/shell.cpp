#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <optional>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

struct ProcessCtx {
    pid_t pid = -1;
    int stdin_fd = -1;   // parent write end
    int stdout_fd = -1;  // parent read end
    std::atomic<bool> running{false};
};

static std::vector<char*> build_argv(const std::string& cmdline, std::vector<std::string>& store) {
    store.clear();
    // naive split by whitespace (no quotes/escapes)
    std::istringstream iss(cmdline);
    std::string tok;
    while (iss >> tok) store.push_back(tok);
    std::vector<char*> argv;
    for (auto& s : store) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    return argv;
}

static std::optional<ProcessCtx> spawn_process(const std::string& cmdline) {
    int toChild[2];    // parent writes -> child stdin
    int fromChild[2];  // child stdout -> parent reads
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
        perror("pipe");
        return std::nullopt;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        // Child: wire stdin/stdout/stderr
        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        dup2(fromChild[1], STDERR_FILENO);

        // Close extra fds
        close(toChild[1]);
        close(fromChild[0]);

        // Parse command
        std::vector<std::string> store;
        auto argv = build_argv(cmdline, store);
        if (!argv[0]) _exit(127);

        execvp(argv[0], argv.data());
        // If exec fails
        perror("execvp");
        _exit(127);
    }

    // Parent
    close(toChild[0]);
    close(fromChild[1]);

    ProcessCtx ctx;
    ctx.pid = pid;
    ctx.stdin_fd = toChild[1];
    ctx.stdout_fd = fromChild[0];
    ctx.running = true;
    return ctx;
}

void session(tcp::socket socket) {
    try {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();

        ProcessCtx proc;
        std::atomic<bool> has_process{false};

        // Reader thread for child stdout -> websocket
        std::thread out_thread;
        auto start_reader = [&](ProcessCtx& ctx) {
            out_thread = std::thread([&ws, &ctx]() {
                try {
                    char buf[4096];
                    for (;;) {
                        ssize_t n = read(ctx.stdout_fd, buf, sizeof(buf));
                        if (n > 0) {
                            ws.write(net::buffer(buf, static_cast<size_t>(n)));
                        } else {
                            break; // EOF or error
                        }
                    }
                } catch (...) {
                    // ignore write errors
                }
            });
        };

        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());

            if (msg.rfind("CMD:", 0) == 0) {
                std::string cmdline = msg.substr(4);

                // If previous process exists, kill/cleanup
                if (has_process.load()) {
                    kill(proc.pid, SIGTERM);
                    if (out_thread.joinable()) out_thread.join();
                    close(proc.stdin_fd);
                    close(proc.stdout_fd);
                    int st = 0;
                    waitpid(proc.pid, &st, 0);
                    has_process = false;
                }

                auto ctx_opt = spawn_process(cmdline);
                if (!ctx_opt) {
                    ws.write(net::buffer(std::string("Failed to start command\n")));
                    continue;
                }
                proc = *ctx_opt;
                has_process = true;
                start_reader(proc);

            } else if (msg.rfind("IN:", 0) == 0) {
                std::string input = msg.substr(3);
                if (has_process.load()) {
                    // Write raw input to child's stdin
                    ssize_t written = write(proc.stdin_fd, input.c_str(), input.size());
                    (void)written;
                } else {
                    ws.write(net::buffer(std::string("No active process\n")));
                }

            } else if (msg == "QUIT") {
                break;

            } else {
                ws.write(net::buffer(std::string("Unknown message. Use CMD:<cmd> or IN:<text>\n")));
            }

            // Check if process exited (non-blocking)
            if (has_process.load()) {
                int status = 0;
                pid_t r = waitpid(proc.pid, &status, WNOHANG);
                if (r == proc.pid) {
                    // Child ended
                    close(proc.stdin_fd);
                    close(proc.stdout_fd);
                    if (out_thread.joinable()) out_thread.join();
                    has_process = false;
                    ws.write(net::buffer(std::string("EOF\n")));
                }
            }
        }

        // Cleanup
        if (has_process.load()) {
            kill(proc.pid, SIGTERM);
            if (out_thread.joinable()) out_thread.join();
            close(proc.stdin_fd);
            close(proc.stdout_fd);
            int st = 0;
            waitpid(proc.pid, &st, 0);
        }

        ws.close(websocket::close_code::normal);

    } catch (std::exception const& e) {
        std::cerr << "Session error: " << e.what() << "\n";
    }
}

int main(int argc, char** argv) {
    try {
        net::io_context ioc;
        tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 9002));
        for (;;) {
            tcp::socket socket(ioc);
            acceptor.accept(socket);
            std::thread(&session, std::move(socket)).detach();
        }
    } catch (std::exception const& e) {
        std::cerr << "Runner error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
