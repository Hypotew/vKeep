#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <getopt.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "auth.hpp"
#include "frontend.hpp"
#include "tty.hpp"
#include "vt_guard.hpp"

namespace {

constexpr int RETRY_PAUSE_MS = 2000;

void print_usage(std::ostream& out, const char* program) {
    out << "Utilisation : " << program << " [options]\n"
        << "Verrouille la console jusqu'à la saisie du mot de passe.\n\n"
        << "  -a, --all    verrouille toutes les consoles virtuelles\n"
        << "  -h, --help   affiche cette aide\n";
}

std::string current_user() {
    const passwd* entry = getpwuid(getuid());
    if (entry == nullptr)
        throw std::runtime_error("utilisateur courant inconnu");
    return entry->pw_name;
}

int unlock_loop(Frontend& frontend, const std::string& user, const std::string& tty) {
    while (!frontend.hung_up()) {
        frontend.begin_attempt();
        // Une erreur interne garde l'écran verrouillé : quitter rendrait la main au shell.
        try {
            const AuthResult result = authenticate(user, tty, frontend);
            if (frontend.hung_up())
                break;
            if (result.outcome == AuthOutcome::Success) {
                frontend.report_success();
                return 0;
            }
            if (result.outcome == AuthOutcome::Denied)
                frontend.report_wrong_password();
            else
                frontend.report_error(result.detail);
            if (!frontend.prompted())
                frontend.pause(RETRY_PAUSE_MS); // PAM refuse sans rien demander : pas de boucle folle
        } catch (const std::exception&) {
            frontend.pause(RETRY_PAUSE_MS);
        }
    }
    return 1; // le terminal a disparu, et la session avec lui
}

int run(bool all_consoles) {
    if (!pam_service_available())
        throw std::runtime_error("service PAM « vlock » introuvable : personne ne pourrait déverrouiller");

    const std::string user = current_user();
    Tty tty;
    if (all_consoles && !tty.is_virtual_console())
        throw std::runtime_error(tty.path() + " n'est pas une console virtuelle : -a ne peut pas tout verrouiller");

    prctl(PR_SET_DUMPABLE, 0); // ni core dump ni ptrace sur le mot de passe

    std::optional<VtGuard> guard;
    if (all_consoles)
        guard.emplace(tty.fd());

    Frontend frontend(tty, user, all_consoles);
    return unlock_loop(frontend, user, tty.path());
}

} // namespace

int main(int argc, char** argv) {
    static const option options[] = {
        {"all", no_argument, nullptr, 'a'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    bool all_consoles = false;
    int choice = 0;
    while ((choice = getopt_long(argc, argv, "ah", options, nullptr)) != -1) {
        switch (choice) {
        case 'a':
            all_consoles = true;
            break;
        case 'h':
            print_usage(std::cout, argv[0]);
            return 0;
        default:
            print_usage(std::cerr, argv[0]);
            return 2;
        }
    }
    if (optind != argc) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }

    try {
        return run(all_consoles);
    } catch (const std::exception& error) {
        std::cerr << argv[0] << " : " << error.what() << '\n';
        return 1;
    }
}
