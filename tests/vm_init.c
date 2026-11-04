// PID 1 de la VM de test : lance le verrou sur tty1 et rapporte l'état sur le port série.
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static pid_t start_lock(const char* args) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;

    setsid();
    int fd = open("/dev/tty1", O_RDWR);
    ioctl(fd, TIOCSCTTY, 0);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    dprintf(1, "\033[2J\033[HCONTENU-AVANT-LE-VERROU\n");
    setgid(1000);
    setuid(1000);
    char* const env[] = {"TERM=linux", "PATH=/usr/bin", "TZ=UTC", NULL};
    if (args[0] != '\0')
        execle("/usr/bin/vkeep", "vkeep", args, NULL, env);
    else
        execle("/usr/bin/vkeep", "vkeep", NULL, env);
    _exit(127);
}

int main(void) {
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    int serial = open("/dev/ttyS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    struct termios raw;
    tcgetattr(serial, &raw);
    cfmakeraw(&raw);
    tcsetattr(serial, TCSANOW, &raw);

    // Le noyau ignore Alt-Fn vers une console non allouée : sans ça, un refus ne prouverait rien.
    int second = open("/dev/tty2", O_RDWR | O_NOCTTY);
    dprintf(second, "\033[2J\033[HCONSOLE-2\n");

    const char* args = getenv("VM_ARGS");
    pid_t child = start_lock(args != NULL && strcmp(args, "none") != 0 ? args : "");
    dprintf(serial, "\nVM: started\n");

    char last[32] = "";
    int console = open("/dev/tty0", O_RDWR | O_NOCTTY);
    for (;;) {
        char active[32] = "";
        int fd = open("/sys/class/tty/tty0/active", O_RDONLY);
        ssize_t got = read(fd, active, sizeof active - 1);
        close(fd);
        if (got > 0 && strcmp(active, last) != 0) {
            strcpy(last, active);
            dprintf(serial, "\nVM: active=%s", active);
        }

        int status;
        if (child > 0 && waitpid(child, &status, WNOHANG) == child) {
            dprintf(serial, "\nVM: exit=%d signal=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            child = 0;
            // Couleurs témoins : la capture montre si la palette normale est revenue.
            int first = open("/dev/tty1", O_RDWR | O_NOCTTY);
            dprintf(first, "\033[31mROUGE \033[32mVERT \033[34mBLEU \033[91mCLAIR\033[0m\n");
            close(first);
        }

        char command;
        if (read(serial, &command, 1) == 1) {
            if (command == 'K' && child > 0)
                kill(child, SIGKILL);
            else if (command == 'C')
                dprintf(serial, "\nVM: chvt=%d\n", ioctl(console, VT_ACTIVATE, 2));
        }
        usleep(50000);
    }
}
