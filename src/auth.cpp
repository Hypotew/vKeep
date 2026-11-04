#include "auth.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>

#include <security/pam_appl.h>
#include <unistd.h>

#include "frontend.hpp"

namespace {

// Le service installé par le paquet kbd avec le vlock d'origine.
constexpr const char* SERVICE_NAME = "vlock";

#ifdef VKEEP_TESTING
const char* test_pam_dir() {
    return std::getenv("VKEEP_TEST_PAM_DIR");
}
#endif

void free_responses(pam_response* responses, int count) {
    for (int i = 0; i < count; ++i) {
        if (responses[i].resp != nullptr) {
            explicit_bzero(responses[i].resp, std::strlen(responses[i].resp));
            std::free(responses[i].resp);
        }
    }
    std::free(responses);
}

int converse(int count, const pam_message** messages, pam_response** out, void* data) {
    if (count <= 0)
        return PAM_CONV_ERR;
    auto* responses = static_cast<pam_response*>(
        std::calloc(static_cast<std::size_t>(count), sizeof(pam_response)));
    if (responses == nullptr)
        return PAM_BUF_ERR;

    auto* frontend = static_cast<Frontend*>(data);
    try { // une exception ne doit pas traverser le code C de PAM
        for (int i = 0; i < count; ++i) {
            const pam_message* message = messages[i];
            switch (message->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
            case PAM_PROMPT_ECHO_ON:
                responses[i].resp =
                    frontend->ask(message->msg, message->msg_style == PAM_PROMPT_ECHO_ON);
                if (responses[i].resp == nullptr) {
                    free_responses(responses, count);
                    return PAM_CONV_ERR;
                }
                break;
            case PAM_ERROR_MSG:
                frontend->notice(message->msg, true);
                break;
            case PAM_TEXT_INFO:
                frontend->notice(message->msg, false);
                break;
            default:
                free_responses(responses, count);
                return PAM_CONV_ERR;
            }
        }
    } catch (const std::exception&) {
        free_responses(responses, count);
        return PAM_CONV_ERR;
    }

    *out = responses;
    return PAM_SUCCESS;
}

} // namespace

bool pam_service_available() {
#ifdef VKEEP_TESTING
    if (const char* dir = test_pam_dir())
        return access((std::string(dir) + "/" + SERVICE_NAME).c_str(), R_OK) == 0;
#endif
    for (const char* dir : {"/etc/pam.d/", "/usr/lib/pam.d/"})
        if (access((std::string(dir) + SERVICE_NAME).c_str(), R_OK) == 0)
            return true;
    return false;
}

AuthResult authenticate(const std::string& user, const std::string& tty, Frontend& frontend) {
    const pam_conv conversation{converse, &frontend};
    pam_handle_t* handle = nullptr;

    const char* confdir = nullptr;
#ifdef VKEEP_TESTING
    confdir = test_pam_dir();
#endif
    int status = pam_start_confdir(SERVICE_NAME, user.c_str(), &conversation, confdir, &handle);
    if (status != PAM_SUCCESS)
        return {AuthOutcome::Failed, "Impossible de démarrer PAM"};

    pam_set_item(handle, PAM_TTY, tty.c_str());
    status = pam_authenticate(handle, 0);

    AuthResult result{AuthOutcome::Failed, ""};
    switch (status) {
    case PAM_SUCCESS:
        pam_setcred(handle, PAM_REFRESH_CRED); // sans droits root, l'échec est sans conséquence
        result.outcome = AuthOutcome::Success;
        break;
    case PAM_AUTH_ERR:
    case PAM_USER_UNKNOWN:
    case PAM_PERM_DENIED:
    case PAM_CRED_INSUFFICIENT:
    case PAM_MAXTRIES:
        result.outcome = AuthOutcome::Denied;
        break;
    default:
        result.detail = pam_strerror(handle, status);
        break;
    }
    pam_end(handle, status);
    return result;
}
