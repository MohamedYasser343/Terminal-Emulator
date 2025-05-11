#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdexcept>
#include <cstring>

class TerminalEmulator {
    private:
        // Terminal state
        struct termios orig_term;
        int master_fd = -1;
        pid_t child_pid = -1;
        bool running = false;
    
        // Input handling
        std::string buffer;
        std::vector<std::string> history;
        size_t history_index = 0;
    
        // Singleton instance for signal handling
        static TerminalEmulator* instance;
    
        // --- Initialization & Cleanup ---
    public:
        TerminalEmulator() {
            setRawMode();
            setupSignals();
            createPty();
        }
    
        ~TerminalEmulator() {
            cleanup();
        }
    
        void run() {
            try {
                handleIO();
            } catch (const std::runtime_error& e) {
                std::cerr << "Fatal error: " << e.what() << std::endl;
            }
        }
    
    private:
        void cleanup() {
            if (master_fd != -1) close(master_fd);
            if (child_pid > 0) {
                kill(child_pid, SIGTERM);
                waitpid(child_pid, nullptr, 0);
            }
            restoreTerminal();
            instance = nullptr;
        }
    
        // --- Terminal & PTY setup ---
        void setRawMode() {
            if (tcgetattr(STDIN_FILENO, &orig_term) == -1)
                throw std::runtime_error("Failed to get terminal attributes: " + std::string(strerror(errno)));
    
            struct termios raw = orig_term;
            raw.c_lflag &= ~(ECHO | ICANON | ISIG);
            raw.c_iflag &= ~(IXON | ICRNL);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 1;
    
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
                throw std::runtime_error("Failed to set raw mode: " + std::string(strerror(errno)));
        }
    
        void restoreTerminal() {
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term) == -1)
                std::cerr << "Warning: Failed to restore terminal: " << strerror(errno) << std::endl;
        }
    
        void createPty() {
            struct winsize ws;
            if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1)
                ws = {24, 80, 0, 0};
    
            struct termios term;
    
            child_pid = forkpty(&master_fd, nullptr, &term, &ws);
            if (child_pid == -1)
                throw std::runtime_error("PTY fork failed: " + std::string(strerror(errno)));
    
            if (child_pid == 0) {
                tcgetattr(STDIN_FILENO, &term);
                term.c_lflag &= ~ECHO;
                tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
                execlp("/bin/bash", "bash", nullptr);
                std::cerr << "Failed to execute bash: " << strerror(errno) << std::endl;
                exit(1);
            }
        }
    
        void resizePty() {
            struct winsize ws;
            if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1)
                return;
            ioctl(master_fd, TIOCSWINSZ, &ws);
        }
    
        // --- Signal Handling ---
        static void handleSignal(int sig) {
            if (!instance) return;
            if (sig == SIGWINCH) {
                instance->resizePty();
            } else if (sig == SIGINT && instance->child_pid > 0) {
                kill(instance->child_pid, SIGINT);
                instance->running = false;
            }
        }
    
        void setupSignals() {
            instance = this;
            struct sigaction sa {};
            sa.sa_handler = handleSignal;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGWINCH, &sa, nullptr);
            sigaction(SIGINT, &sa, nullptr);
            sigaction(SIGTERM, &sa, nullptr);
        }
    
        // --- IO Handling ---
        void handleIO() {
            struct pollfd fds[2] = {
                {STDIN_FILENO, POLLIN, 0},
                {master_fd, POLLIN, 0}
            };
            char buf[1024];
            running = true;
    
            while (running) {
                if (poll(fds, 2, -1) == -1) {
                    if (errno == EINTR) continue;
                    throw std::runtime_error("Poll error: " + std::string(strerror(errno)));
                }
    
                if (fds[0].revents & POLLIN) readUserInput(buf);
                if (fds[1].revents & POLLIN) readShellOutput(buf);
            }
        }
    
        void readUserInput(char* buf) {
            ssize_t n = read(STDIN_FILENO, buf, 1024);
            if (n <= 0) return;
    
            for (ssize_t i = 0; i < n; ++i) {
                if (!processInput(buf[i])) {
                    running = false;
                    break;
                }
            }
        }
    
        void readShellOutput(char* buf) {
            ssize_t n = read(master_fd, buf, 1024);
            if (n > 0) write(STDOUT_FILENO, buf, n);
        }
    
        // --- Input Processing ---
        bool processInput(char c) {
            static std::string escape_seq;
    
            if (c == 3) return killChild(SIGINT);    // Ctrl+C
            if (c == 26) return killChild(SIGTSTP);  // Ctrl+Z
            if (c == 4) return false;                // Ctrl+D
            if (c == 127) return handleBackspace();  // Backspace
    
            if (handleEscapeSequence(c, escape_seq)) return true;
    
            if (c == '\r' || c == '\n') return handleEnter();
    
            buffer += c;
            write(STDOUT_FILENO, &c, 1);
            write(master_fd, &c, 1);
            return true;
        }
    
        bool handleEnter() {
            if (buffer == "exit") return false;
    
            if (!buffer.empty()) {
                history.push_back(buffer);
                history_index = history.size();
            }
            buffer.clear();
    
            write(master_fd, "\n", 1);
            write(STDOUT_FILENO, "\n", 1);
            return true;
        }
    
        bool handleBackspace() {
            if (buffer.empty()) return true;
            buffer.pop_back();
            write(STDOUT_FILENO, "\b \b", 3);
            write(master_fd, "\b", 1);
            return true;
        }
    
        bool handleEscapeSequence(char c, std::string& seq) {
            if (c == 27) {
                seq = "\033";
                return true;
            }
            if (!seq.empty()) {
                seq += c;
                if (seq.size() == 2 && c != '[') {
                    seq.clear();
                    return true;
                } else if (seq.size() == 3) {
                    handleArrowKey(seq[2]);
                    seq.clear();
                    return true;
                }
            }
            return false;
        }
    
        void handleArrowKey(char c) {
            if (c == 'A' && history_index > 0) { // Up
                history_index--;
                showHistory();
            } else if (c == 'B' && history_index < history.size()) { // Down
                history_index++;
                showHistory();
            }
        }
    
        void showHistory() {
            clearLine();
            if (history_index < history.size())
                buffer = history[history_index];
            else
                buffer.clear();
            std::cout << buffer << std::flush;
        }
    
        bool killChild(int signal) {
            if (child_pid > 0) kill(child_pid, signal);
            return true;
        }
    
        void clearLine() {
            write(STDOUT_FILENO, "\r\x1B[K", 4);
        }
    };
    
    // Static init
    TerminalEmulator* TerminalEmulator::instance = nullptr;
    
    // --- Entry Point ---
    int main() {
        try {
            TerminalEmulator terminal;
            terminal.run();
        } catch (const std::exception& e) {
            std::cerr << "Startup error: " << e.what() << std::endl;
            return 1;
        }
        return 0;
    }
