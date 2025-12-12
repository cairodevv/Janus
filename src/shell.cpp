#include <iostream>
#include <sstream>
#include <vector>
#include <unistd.h>    // chdir, getcwd, execvp, fork
#include <sys/wait.h>  // waitpid
#include <limits.h>    // PATH_MAX
#include <cstdlib>     // getenv

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