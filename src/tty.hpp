#pragma once

#include <cstddef>
#include <string>

#include <termios.h>

struct TtySize {
    int rows;
    int cols;
};

enum class TtyRead { Data, Timeout, Interrupted, Hangup };

// Le terminal de contrôle du processus, qui doit être au premier plan.
class Tty {
public:
    Tty();
    ~Tty();
    Tty(const Tty&) = delete;
    Tty& operator=(const Tty&) = delete;

    void enter_raw_mode();
    void flush_input();

    // Attend au plus timeout_ms. `count` n'est valable que si le résultat est Data.
    TtyRead read_input(char* buffer, std::size_t capacity, int timeout_ms, std::size_t& count);
    bool write_all(const std::string& data);

    int fd() const { return fd_; }
    const std::string& path() const { return path_; }
    bool is_virtual_console() const;
    TtySize size() const;

private:
    int fd_ = -1;
    std::string path_;
    termios saved_{};
    bool raw_ = false;
};
