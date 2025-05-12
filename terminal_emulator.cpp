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
#include <stdexcept>
#include <cstring>

class TerminalEmulator {
private:
    struct termios original_termios_; // Original terminal settings
    int master_fd_ = -1;              // PTY master file descriptor
    pid_t child_pid_ = -1;            // Child process ID
    bool is_running_ = false;         // Emulator running state

    std::string input_buffer_;        // Current user input
    std::vector<std::string> history_;// Command history
    size_t history_index_ = 0;        // Current history navigation index

    static TerminalEmulator* instance_; // Singleton instance for signal handling

public:
    TerminalEmulator() {
        configureTerminal();
        setupSignalHandlers();
        initializePty();
    }

    ~TerminalEmulator() {
        cleanup();
    }

    // Runs the terminal emulator's main I/O loop
    void run() {
        try {
            processIO();
        } catch (const std::runtime_error& e) {
            std::cerr << "Fatal error: " << e.what() << std::endl;
        }
    }

private:
    // Writes data to a file descriptor with retry on interrupt
    bool safeWrite(int fd, const void* buf, size_t count) {
        const char* ptr = static_cast<const char*>(buf);
        size_t remaining = count;

        while (remaining > 0) {
            ssize_t written = write(fd, ptr, remaining);
            if (written == -1) {
                if (errno == EINTR) continue;
                std::cerr << "Write error: " << std::strerror(errno) << std::endl;
                return false;
            }
            ptr += written;
            remaining -= written;
        }
        return true;
    }

    // Restores terminal settings and cleans up resources
    void cleanup() {
        if (master_fd_ != -1) {
            close(master_fd_);
            master_fd_ = -1;
        }
        if (child_pid_ > 0) {
            kill(child_pid_, SIGTERM);
            waitpid(child_pid_, nullptr, 0);
            child_pid_ = -1;
        }
        restoreTerminal();
        instance_ = nullptr;
    }

    // Configures stdin for raw mode
    void configureTerminal() {
        if (tcgetattr(STDIN_FILENO, &original_termios_) == -1) {
            throw std::runtime_error("Failed to get terminal attributes: " + std::string(std::strerror(errno)));
        }

        termios raw = original_termios_;
        raw.c_lflag &= ~(ECHO | ICANON); // Disable echo and canonical mode
        raw.c_iflag &= ~(IXON | ICRNL);  // Disable flow control and CR-to-NL
        raw.c_lflag |= ISIG;             // Enable signal handling
        raw.c_cc[VMIN] = 1;              // Read one byte at a time
        raw.c_cc[VTIME] = 0;             // No timeout

        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
            throw std::runtime_error("Failed to set raw mode: " + std::string(std::strerror(errno)));
        }
    }

    // Restores original terminal settings
    void restoreTerminal() {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios_) == -1) {
            std::cerr << "Warning: Failed to restore terminal: " << std::strerror(errno) << std::endl;
        }
    }

    // Creates a PTY and forks a bash process
    void initializePty() {
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
            ws = {24, 80, 0, 0}; // Default size if retrieval fails
        }

        struct termios term;
        child_pid_ = forkpty(&master_fd_, nullptr, &term, &ws);
        if (child_pid_ == -1) {
            throw std::runtime_error("PTY fork failed: " + std::string(std::strerror(errno)));
        }

        if (child_pid_ == 0) { // Child process
            tcgetattr(STDIN_FILENO, &term);
            term.c_lflag &= ~ECHO;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
            execlp("/bin/bash", "bash", nullptr);
            std::cerr << "Failed to execute bash: " << std::strerror(errno) << std::endl;
            exit(1);
        }
    }

    // Adjusts PTY size to match terminal window
    void resizePty() {
        struct winsize ws;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
            return;
        }
        ioctl(master_fd_, TIOCSWINSZ, &ws);
    }

    // Signal handler for window resize and interrupts
    static void handleSignal(int sig) {
        if (!instance_) return;
        if (sig == SIGWINCH) {
            instance_->resizePty();
        } else if (sig == SIGINT || sig == SIGTERM) {
            if (instance_->child_pid_ > 0) {
                kill(instance_->child_pid_, sig);
            }
        }
    }

    // Sets up signal handlers for SIGWINCH, SIGINT, SIGTERM
    void setupSignalHandlers() {
        instance_ = this;
        struct sigaction sa = {};
        sa.sa_handler = handleSignal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGWINCH, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    // Main I/O loop using poll
    void processIO() {
        struct pollfd fds[2] = {
            {STDIN_FILENO, POLLIN, 0},
            {master_fd_, POLLIN, 0}
        };
        char buffer[1024];
        is_running_ = true;

        while (is_running_) {
            if (poll(fds, 2, -1) == -1) {
                if (errno == EINTR) continue;
                throw std::runtime_error("Poll error: " + std::string(std::strerror(errno)));
            }

            if (fds[0].revents & POLLIN) {
                readUserInput(buffer);
            }
            if (fds[1].revents & POLLIN) {
                readShellOutput(buffer);
            }
        }
    }

    // Reads and processes user input
    void readUserInput(char* buffer) {
        ssize_t bytes_read = read(STDIN_FILENO, buffer, 1024);
        if (bytes_read <= 0) return;

        for (ssize_t i = 0; i < bytes_read; ++i) {
            if (!processInput(buffer[i])) {
                is_running_ = false;
                break;
            }
        }
    }

    // Reads and forwards shell output to stdout
    void readShellOutput(char* buffer) {
        ssize_t bytes_read = read(master_fd_, buffer, 1024);
        if (bytes_read > 0) {
            safeWrite(STDOUT_FILENO, buffer, bytes_read);
        }
    }

    // Processes single character input
    bool processInput(char c) {
        static std::string escape_sequence;

        if (c == 3) { // Ctrl+C
            return sendSignalToChild(SIGINT);
        }
        if (c == 26) { // Ctrl+Z
            return sendSignalToChild(SIGTSTP);
        }
        if (c == 4) { // Ctrl+D
            safeWrite(master_fd_, &c, 1);
            is_running_ = false;
            return false;
        }
        if (c == 127) { // Backspace
            return handleBackspace();
        }
        if (handleEscapeSequence(c, escape_sequence)) {
            return true;
        }
        if (c == '\r' || c == '\n') {
            return handleEnter();
        }

        input_buffer_ += c;
        safeWrite(STDOUT_FILENO, &c, 1);
        safeWrite(master_fd_, &c, 1);
        return true;
    }

    // Handles enter key press
    bool handleEnter() {
        if (input_buffer_ == "exit") {
            is_running_ = false;
            return false;
        }

        if (!input_buffer_.empty()) {
            history_.push_back(input_buffer_);
            history_index_ = history_.size();
        }
        input_buffer_.clear();

        safeWrite(master_fd_, "\n", 1);
        safeWrite(STDOUT_FILENO, "\n", 1);
        return true;
    }

    // Handles backspace key
    bool handleBackspace() {
        if (input_buffer_.empty()) return true;
        input_buffer_.pop_back();
        safeWrite(STDOUT_FILENO, "\b \b", 3);
        safeWrite(master_fd_, "\b", 1);
        return true;
    }

    // Processes escape sequences (e.g., arrow keys)
    bool handleEscapeSequence(char c, std::string& sequence) {
        if (c == 27) { // Escape key
            sequence = "\033";
            return true;
        }

        if (!sequence.empty()) {
            sequence += c;
            if (sequence.size() == 2 && c != '[') {
                safeWrite(master_fd_, sequence.c_str(), sequence.size());
                sequence.clear();
                return true;
            }
            if (sequence.size() == 3) {
                handleArrowKey(sequence[2]);
                safeWrite(master_fd_, sequence.c_str(), sequence.size());
                sequence.clear();
                return true;
            }
        }
        return false;
    }

    // Handles arrow key navigation in command history
    void handleArrowKey(char c) {
        if (c == 'A' && history_index_ > 0) { // Up arrow
            --history_index_;
            displayHistoryEntry();
        } else if (c == 'B') { // Down arrow
            if (history_index_ + 1 < history_.size()) {
                ++history_index_;
                displayHistoryEntry();
            } else {
                history_index_ = history_.size();
                input_buffer_.clear();
                displayHistoryEntry();
            }
        }
    }

    // Displays the current history entry
    void displayHistoryEntry() {
        clearLine();
        std::string prompt = "$ ";
        safeWrite(STDOUT_FILENO, prompt.c_str(), prompt.size());

        input_buffer_ = (history_index_ < history_.size()) ? history_[history_index_] : "";
        safeWrite(STDOUT_FILENO, input_buffer_.c_str(), input_buffer_.size());
    }

    // Sends a signal to the child process
    bool sendSignalToChild(int signal) {
        if (child_pid_ <= 0) return true;
        if (kill(child_pid_, signal) == -1) {
            std::cerr << "Failed to send signal to child: " << std::strerror(errno) << std::endl;
            return false;
        }
        if (signal == SIGINT || signal == SIGTERM || signal == SIGKILL) {
            waitpid(child_pid_, nullptr, 0);
            child_pid_ = -1;
            is_running_ = false;
        }
        return true;
    }

    // Clears the current input line
    void clearLine() {
        safeWrite(STDOUT_FILENO, "\r\x1B[K", 4);
    }
};

TerminalEmulator* TerminalEmulator::instance_ = nullptr;

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