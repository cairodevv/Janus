#include <iostream>
#include <sstream>
#include <vector>
#include <unistd.h>    // chdir, getcwd, execvp, fork
#include <sys/wait.h>  // waitpid
#include <limits.h>    // PATH_MAX
#include <cstdlib>     // getenv

void runCommand(const std::string& line, std::vector<std::string>& history) {
    // Save command to history
    history.push_back(line);

    // Tokenize input
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) tokens.push_back(token);
    if (tokens.empty()) return;

    // Built-ins
    if (tokens[0] == "exit") {
        exit(0);
    } else if (tokens[0] == "cd") {
        const char* path = tokens.size() > 1 ? tokens[1].c_str() : getenv("HOME");
        if (chdir(path) != 0) perror("cd failed");
        return;
    } else if (tokens[0] == "pwd") {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) std::cout << cwd << "\n";
        else perror("pwd failed");
        return;
    } else if (tokens[0] == "echo") {
        for (size_t i = 1; i < tokens.size(); i++) std::cout << tokens[i] << (i + 1 < tokens.size() ? " " : "");
        std::cout << "\n";
        return;
    } else if (tokens[0] == "history") {
        for (size_t i = 0; i < history.size(); i++) {
            std::cout << i + 1 << "  " << history[i] << "\n";
        }
        return;
    }

    // External commands
    std::vector<char*> argv;
    for (auto& t : tokens) argv.push_back(const_cast<char*>(t.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv.data());
        perror("execvp failed");
        _exit(1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        perror("fork failed");
    }
}
void shell() {
    std::vector<std::string> history;

    while (true) {
        // Show current directory in the prompt
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            std::cout << "Janus-shell:" << cwd << "> ";
        } else {
            std::cout << "Janus-shell:?> ";
        }

        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line == "exit") break;

        runCommand(line, history);
    }
}