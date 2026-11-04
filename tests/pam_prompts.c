// Module PAM de test : envoie des messages, puis demande un code avec une saisie visible.
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <stdlib.h>
#include <string.h>

static int converse(pam_handle_t* handle, int style, const char* text, char** answer) {
    const struct pam_conv* conversation;
    if (pam_get_item(handle, PAM_CONV, (const void**)&conversation) != PAM_SUCCESS)
        return PAM_CONV_ERR;
    struct pam_message message = {style, text};
    const struct pam_message* messages = &message;
    struct pam_response* response = NULL;
    int status = conversation->conv(1, &messages, &response, conversation->appdata_ptr);
    if (status == PAM_SUCCESS && response != NULL) {
        if (answer != NULL)
            *answer = response->resp;
        else
            free(response->resp);
        free(response);
    }
    return status;
}

int pam_sm_authenticate(pam_handle_t* handle, int flags, int argc, const char** argv) {
    (void)flags, (void)argc, (void)argv;
    converse(handle, PAM_ERROR_MSG, "Alerte du module", NULL);
    converse(handle, PAM_TEXT_INFO, "Information du module", NULL);

    char* code = NULL;
    int status = converse(handle, PAM_PROMPT_ECHO_ON, "Code de test : ", &code);
    if (status != PAM_SUCCESS || code == NULL)
        return PAM_AUTH_ERR;
    status = strcmp(code, "4242") == 0 ? PAM_SUCCESS : PAM_AUTH_ERR;
    free(code);
    return status;
}

int pam_sm_setcred(pam_handle_t* handle, int flags, int argc, const char** argv) {
    (void)handle, (void)flags, (void)argc, (void)argv;
    return PAM_SUCCESS;
}
