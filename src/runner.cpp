#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <optional>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Minimal JSON helpers
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default: out += c; break;
        }
    }
    return out;
}

struct ProcessCtx {
    pid_t pid = -1;
    int stdin_fd = -1;   // parent write end
    int stdout_fd = -1;  // parent read end
    bool running = false;
};

struct SessionState {
    std::string cwd;                 // per-connection working directory
    std::vector<std::string> history;
    std::optional<ProcessCtx> proc;
};

static std::string get_cwd_safe() {
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
    return std::string(".");
}

static bool chdir_safe(const std::string& path, std::string& err) {
    if (chdir(path.c_str()) != 0) {
        err = std::string("cd failed: ") + strerror(errno);
        return false;
    }
    return true;
}

// Use bash -lc so quotes/pipes/redirection work.
// Child process starts in the session's current directory.
static std::optional<ProcessCtx> spawn_process_in_cwd(const std::string& cmdline, const std::string& cwd) {
    int toChild[2];    // parent writes -> child stdin
    int fromChild[2];  // child stdout+stderr -> parent reads
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
        return std::nullopt;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        // Child
        (void)cwd;
        if (chdir(cwd.c_str()) != 0) {
            // If chdir fails, still attempt exec; errors will surface.
        }

        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        dup2(fromChild[1], STDERR_FILENO);

        close(toChild[1]);
        close(fromChild[0]);

        execlp("bash", "bash", "-lc", cmdline.c_str(), (char*)nullptr);
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
    return std::optional<ProcessCtx>{ctx};
}

static void send_prompt(websocket::stream<tcp::socket>& ws, const std::string& cwd) {
    std::string msg = std::string("{\"type\":\"prompt\",\"cwd\":\"") + json_escape(cwd) + "\"}";
    ws.write(net::buffer(msg));
}

static void send_error(websocket::stream<tcp::socket>& ws, const std::string& message) {
    std::string msg = std::string("{\"type\":\"error\",\"message\":\"") + json_escape(message) + "\"}";
    ws.write(net::buffer(msg));
}

static void send_out(websocket::stream<tcp::socket>& ws, const std::string& data) {
    std::string msg = std::string("{\"type\":\"out\",\"data\":\"") + json_escape(data) + "\"}";
    ws.write(net::buffer(msg));
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

        auto stop_process_if_any = [&]() {
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

            // Naive JSON extraction: "key":"value"
            auto get_field = [&](const std::string& key) -> std::optional<std::string> {
                std::string pattern = "\"" + key + "\":\"";
                auto p = msg.find(pattern);
                if (p == std::string::npos) return std::nullopt;
                auto start = p + pattern.size();
                auto end = msg.find("\"", start);
                if (end == std::string::npos) return std::nullopt;
                return msg.substr(start, end - start);
            };

            auto type = get_field("type");
            if (!type.has_value()) {
                send_error(ws, "invalid message: missing type");
                continue;
            }

            if (type.value() == "quit") {
                stop_process_if_any();
                break;
            }

            if (type.value() == "in") {
                if (!state.proc.has_value()) {
                    send_error(ws, "no active process");
                    continue;
                }
                auto data = get_field("data");
                if (!data.has_value()) {
                    send_error(ws, "missing input data");
                    continue;
                }
                write(state.proc->stdin_fd, data->c_str(), data->size());
                continue;
            }

            if (type.value() == "ctrl") {
                auto sig = get_field("signal");
                if (state.proc.has_value() && sig.has_value()) {
                    if (sig.value() == "SIGINT") kill(state.proc->pid, SIGINT);
                    else if (sig.value() == "SIGTERM") kill(state.proc->pid, SIGTERM);
                }
                continue;
            }

            if (type.value() == "cmd") {
                auto line = get_field("line");
                if (!line.has_value() || line->empty()) {
                    send_error(ws, "empty command");
                    continue;
                }

                // Record history
                state.history.push_back(line.value());

                // Built-ins in server: cd, pwd, echo, history, exit
                std::istringstream iss(line.value());
                std::vector<std::string> tokens;
                std::string tok;
                while (iss >> tok) tokens.push_back(tok);

                if (!tokens.empty()) {
                    const std::string& cmd = tokens[0];

                    if (cmd == "exit") {
                        stop_process_if_any();
                        break;
                    } else if (cmd == "cd") {
                        std::string err;
                        std::string target;
                        if (tokens.size() > 1) {
                            target = tokens[1];
                        } else {
                            const char* home = getenv("HOME");
                            target = home ? std::string(home) : std::string("/");
                        }

                        // Change per-session cwd (without affecting global server cwd)
                        std::string old_global_cwd = get_cwd_safe();
                        if (chdir(state.cwd.c_str()) == 0) {
                            if (chdir_safe(target, err)) {
                                state.cwd = get_cwd_safe();
                                send_prompt(ws, state.cwd);
                            } else {
                                send_error(ws, err);
                            }
                            chdir(old_global_cwd.c_str());
                        } else {
                            send_error(ws, "unable to access session cwd");
                        }
                        continue;
                    } else if (cmd == "pwd") {
                        send_out(ws, state.cwd + "\n");
                        continue;
                    } else if (cmd == "echo") {
                        std::ostringstream oss;
                        for (size_t i = 1; i < tokens.size(); ++i) {
                            if (i > 1) oss << " ";
                            oss << tokens[i];
                        }
                        oss << "\n";
                        send_out(ws, oss.str());
                        continue;
                    } else if (cmd == "history") {
                        std::ostringstream oss;
                        for (size_t i = 0; i < state.history.size(); ++i) {
                            oss << (i + 1) << "  " << state.history[i] << "\n";
                        }
                        send_out(ws, oss.str());
                        continue;
                    }
                }

                // External command: run in session cwd with bash -lc
                // Stop previous process first (one at a time)
                stop_process_if_any();

                std::string old_global_cwd = get_cwd_safe();
                if (chdir(state.cwd.c_str()) != 0) {
                    send_error(ws, "failed to switch to session cwd");
                    chdir(old_global_cwd.c_str());
                    continue;
                }

                auto ctx_opt = spawn_process_in_cwd(line.value(), state.cwd);
                chdir(old_global_cwd.c_str());

                if (!ctx_opt.has_value()) {
                    send_error(ws, "failed to start process");
                    continue;
                }

                state.proc.emplace(std::move(*ctx_opt));
                send_prompt(ws, state.cwd); // prompt before output

                // Start reader thread
                reader_running = true;
                reader_thread = std::thread([&]() {
                    try {
                        char buf[4096];
                        while (reader_running) {
                            ssize_t n = read(state.proc->stdout_fd, buf, sizeof(buf));
                            if (n > 0) {
                                send_out(ws, std::string(buf, n));
                            } else {
                                break;
                            }
                        }
                    } catch (...) {}
                });

                // Wait for process in a detached watcher, then send EOF and prompt
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

                continue;
            }

            send_error(ws, "unknown message type");
        }

        // Cleanup
        if (state.proc.has_value()) {
            reader_running = false;
            kill(state.proc->pid, SIGTERM);
            int st = 0;
            waitpid(state.proc->pid, &st, 0);
            if (reader_thread.joinable()) reader_thread.join();
            close(state.proc->stdin_fd);
            close(state.proc->stdout_fd);
            state.proc.reset();
        }

        ws.close(websocket::close_code::normal);

    } catch (std::exception const& e) {
        std::cerr << "Session error: " << e.what() << "\n";
    }
}

int main() {
    try {
        net::io_context ioc;
        tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 9002));
        std::cout << "Runner listening on ws://127.0.0.1:9002\n";
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
