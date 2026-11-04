#include "tty.hpp"

#include <cerrno>
#include <stdexcept>

#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

Tty::Tty() {
    const char* name = ttyname(STDIN_FILENO);
    if (name == nullptr)
        throw std::runtime_error("l'entrée standard n'est pas un terminal");
    path_ = name;

    // /dev/tty plutôt que son chemin réel : après un su, ce dernier appartient à quelqu'un d'autre.
    fd_ = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0)
        throw std::runtime_error("aucun terminal de contrôle");

    // En arrière-plan, le shell garderait la main : le verrou ne servirait à rien.
    if (tcgetpgrp(fd_) != getpgrp()) {
        close(fd_);
        throw std::runtime_error("le programme doit tourner au premier plan de son terminal");
    }
    if (tcgetattr(fd_, &saved_) != 0) {
        close(fd_);
        throw std::runtime_error("impossible de lire la configuration du terminal");
    }
}

Tty::~Tty() {
    if (raw_)
        tcsetattr(fd_, TCSAFLUSH, &saved_);
    close(fd_);
}

void Tty::enter_raw_mode() {
    termios raw = saved_;
    raw.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP | BRKINT);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fd_, TCSAFLUSH, &raw) != 0)
        throw std::runtime_error("impossible de configurer le terminal");
    raw_ = true;
}

void Tty::flush_input() {
    tcflush(fd_, TCIFLUSH);
}

TtyRead Tty::read_input(char* buffer, std::size_t capacity, int timeout_ms, std::size_t& count) {
    pollfd pfd{fd_, POLLIN, 0};
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0)
        return errno == EINTR ? TtyRead::Interrupted : TtyRead::Hangup;
    if (ready == 0)
        return TtyRead::Timeout;

    const ssize_t got = read(fd_, buffer, capacity);
    if (got > 0) {
        count = static_cast<std::size_t>(got);
        return TtyRead::Data;
    }
    if (got < 0 && errno == EINTR)
        return TtyRead::Interrupted;
    return TtyRead::Hangup;
}

bool Tty::write_all(const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t written = write(fd_, data.data() + done, data.size() - done);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += static_cast<std::size_t>(written);
    }
    return true;
}

bool Tty::is_virtual_console() const {
    vt_stat state{};
    return ioctl(fd_, VT_GETSTATE, &state) == 0;
}

TtySize Tty::size() const {
    winsize ws{};
    if (ioctl(fd_, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0)
        return {24, 80};
    return {ws.ws_row, ws.ws_col};
}
