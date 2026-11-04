#include "vt_guard.hpp"

#include <cerrno>
#include <csignal>
#include <stdexcept>

#include <sys/ioctl.h>

namespace {

volatile sig_atomic_t guarded_fd = -1;

// En mode VT_PROCESS, le noyau nous demande l'accord avant chaque changement de console.
void refuse_release(int) {
    const int saved_errno = errno;
    ioctl(guarded_fd, VT_RELDISP, 0);
    errno = saved_errno;
}

void acknowledge_acquire(int) {
    const int saved_errno = errno;
    ioctl(guarded_fd, VT_RELDISP, VT_ACKACQ);
    errno = saved_errno;
}

void install_handler(int signum, void (*handler)(int)) {
    struct sigaction action{};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(signum, &action, nullptr) != 0)
        throw std::runtime_error("impossible d'installer les gestionnaires de signaux");
}

} // namespace

VtGuard::VtGuard(int tty_fd) : fd_(tty_fd) {
    if (ioctl(fd_, VT_GETMODE, &saved_) != 0)
        throw std::runtime_error("impossible de lire le mode de la console");

    guarded_fd = fd_;
    install_handler(SIGUSR1, refuse_release);
    install_handler(SIGUSR2, acknowledge_acquire);

    vt_mode locked = saved_;
    locked.mode = VT_PROCESS;
    locked.waitv = 0;
    locked.relsig = SIGUSR1;
    locked.acqsig = SIGUSR2;
    locked.frsig = 0;
    if (ioctl(fd_, VT_SETMODE, &locked) != 0)
        throw std::runtime_error("impossible de bloquer le changement de console");
}

VtGuard::~VtGuard() {
    ioctl(fd_, VT_SETMODE, &saved_);
}
