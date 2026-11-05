#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <getopt.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "attempts.hpp"
#include "auth.hpp"
#include "camera.hpp"
#include "frontend.hpp"
#include "tty.hpp"
#include "vt_guard.hpp"

namespace {

constexpr int RETRY_PAUSE_MS = 2000;
#ifndef VKEEP_VERSION
#define VKEEP_VERSION "dev"
#endif

void print_usage(std::ostream& out, const char* program) {
    out << "Utilisation : " << program << " [options]\n"
        << "Verrouille la console jusqu'à la saisie du mot de passe.\n\n"
        << "  -a, --all         verrouille toutes les consoles virtuelles et photographie chaque tentative\n"
        << "      --camera=DEV  webcam à utiliser avec -a (détectée automatiquement sinon)\n"
        << "  -v, --version     affiche la version\n"
        << "  -h, --help        affiche cette aide\n";
}

const passwd& current_user() {
    const passwd* entry = getpwuid(getuid());
    if (entry == nullptr)
        throw std::runtime_error("utilisateur courant inconnu");
    return *entry;
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

int run(bool all_consoles, const std::string& chosen_camera) {
    if (!pam_service_available())
        throw std::runtime_error("service PAM « vkeep » introuvable, personne ne pourrait déverrouiller : lance sudo make install");

    const passwd& account = current_user();
    const std::string user = account.pw_name;
    const std::string home = account.pw_dir;
    Tty tty;
    if (all_consoles && !tty.is_virtual_console())
        throw std::runtime_error(tty.path() + " n'est pas une console virtuelle : -a ne peut pas tout verrouiller");

    prctl(PR_SET_DUMPABLE, 0); // ni core dump ni ptrace sur le mot de passe

    std::optional<VtGuard> guard;
    if (all_consoles)
        guard.emplace(tty.fd());

    // Avant de verrouiller : une caméra lente à répondre ne doit pas bloquer l'écran.
    bool photos_wanted = all_consoles;
    std::string camera = chosen_camera;
#ifdef VKEEP_TESTING
    // -a est refusé hors d'une vraie console : les tests en pseudo-terminal passent par ici.
    if (const char* test_camera = std::getenv("VKEEP_TEST_CAMERA")) {
        photos_wanted = true;
        camera = std::string(test_camera) == "auto" ? "" : test_camera;
    }
#endif
    if (photos_wanted && camera.empty())
        camera = find_camera();
    AttemptLog attempts(home, photos_wanted, camera);
    int status = 1;
    {
        Frontend frontend(tty, user, all_consoles, attempts);
        status = unlock_loop(frontend, user, tty.path());
    }
    attempts.finish();
    if (status == 0)
        tty.write_all(attempts.summary());
    return status;
}

} // namespace

int main(int argc, char** argv) {
    static const option options[] = {
        {"all", no_argument, nullptr, 'a'},
        {"camera", required_argument, nullptr, 'c'},
        {"version", no_argument, nullptr, 'v'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    bool all_consoles = false;
    std::string camera;
    int choice = 0;
    while ((choice = getopt_long(argc, argv, "ahv", options, nullptr)) != -1) {
        switch (choice) {
        case 'a':
            all_consoles = true;
            break;
        case 'c':
            camera = optarg;
            break;
        case 'v':
            std::cout << "vKeep " << VKEEP_VERSION << '\n';
            return 0;
        case 'h':
            print_usage(std::cout, argv[0]);
            return 0;
        default:
            print_usage(std::cerr, argv[0]);
            return 2;
        }
    }
    if (optind != argc || (!camera.empty() && !all_consoles)) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }

    try {
        return run(all_consoles, camera);
    } catch (const std::exception& error) {
        std::cerr << argv[0] << " : " << error.what() << '\n';
        return 1;
    }
}
