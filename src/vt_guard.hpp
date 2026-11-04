#pragma once

#include <linux/vt.h>

// Interdit de quitter la console virtuelle tant que l'objet existe.
// Le noyau repasse la console en mode automatique si le processus meurt.
class VtGuard {
public:
    explicit VtGuard(int tty_fd);
    ~VtGuard();
    VtGuard(const VtGuard&) = delete;
    VtGuard& operator=(const VtGuard&) = delete;

private:
    int fd_;
    vt_mode saved_{};
};
