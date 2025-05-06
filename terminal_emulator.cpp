#include <iostream>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdexcept>

class TerminalEmulator {
private:
    struct termios orig_term;
    int master_fd;
    pid_t child_pid;
    std::string buffer;

    void setRawMode() {
        struct termios raw;
        if (tcgetattr(STDIN_FILENO, &orig_term) == -1 || 
            (raw = orig_term, raw.c_lflag &= ~(ECHO | ICANON | ISIG), 
             raw.c_cc[VMIN] = 0, raw.c_cc[VTIME] = 1, 
             tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1))
            throw std::runtime_error("Terminal setup failed");
    }

    void restoreTerminal() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
    }

    void createPty() {
        struct winsize ws = {24, 80, 0, 0};
        struct termios term;
        master_fd = -1;

        if ((child_pid = forkpty(&master_fd, nullptr, &term, &ws)) == -1)
            throw std::runtime_error("PTY fork failed");

        if (child_pid == 0) {
            tcgetattr(STDIN_FILENO, &term);
            term.c_lflag &= ~(ECHO);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
            execlp("/bin/bash", "bash", nullptr);
            exit(1);
        }
    }

    void handleIO() {
        struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {master_fd, POLLIN, 0}};
        char buf[1024];

        while (true) {
            if (poll(fds, 2, -1) == -1)
                throw std::runtime_error("Poll error");

            if (fds[0].revents & POLLIN) {
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                if (n <= 0) return;

                for (ssize_t i = 0; i < n; i++) {
                    if (buf[i] == 4) return; // Ctrl+D
                    if (buf[i] == 127) { // Backspace
                        if (!buffer.empty()) {
                            buffer.pop_back();
                            if (write(STDOUT_FILENO, "\b \b", 3) != 3 ||
                                write(master_fd, &buf[i], 1) != 1)
                                throw std::runtime_error("Write error");
                        }
                    } else if (buf[i] == '\r' || buf[i] == '\n') {
                        if (buffer == "exit") return;
                        buffer.clear();
                        if (write(master_fd, "\n", 1) != 1 ||
                            write(STDOUT_FILENO, "\n", 1) != 1)
                            throw std::runtime_error("Write error");
                    } else {
                        buffer += buf[i];
                        if (write(STDOUT_FILENO, &buf[i], 1) != 1 ||
                            write(master_fd, &buf[i], 1) != 1)
                            throw std::runtime_error("Write error");
                    }
                }
                std::cout.flush();
            }

            if (fds[1].revents & POLLIN) {
                ssize_t n = read(master_fd, buf, sizeof(buf));
                if (n <= 0) return;
                if (write(STDOUT_FILENO, buf, n) != n)
                    throw std::runtime_error("Write error");
                std::cout.flush();
            }
        }
    }

public:
    TerminalEmulator() : master_fd(-1), child_pid(-1) {
        setRawMode();
        createPty();
    }

    ~TerminalEmulator() {
        if (master_fd != -1) close(master_fd);
        if (child_pid > 0) {
            kill(child_pid, SIGTERM);
            waitpid(child_pid, nullptr, 0);
        }
        restoreTerminal();
    }

    void run() {
        try {
            handleIO();
        } catch (const std::runtime_error& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
};

int main() {
    TerminalEmulator terminal;
    terminal.run();
    return 0;
}
