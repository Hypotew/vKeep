#!/usr/bin/env python3
"""Tests de bout en bout dans un pseudo-terminal, avec une vraie pile PAM de test.

Le mot de passe de test est vérifié par pam_exec : le vrai compte n'est jamais touché.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

PASSWORD = "sécret-42"
BINARY = os.path.abspath(sys.argv[1])
HERE = os.path.dirname(os.path.abspath(__file__))
failures = []


def check(name, condition, detail=""):
    print(f"{'ok  ' if condition else 'FAIL'} {name}" + (f" : {detail}" if detail and not condition else ""))
    if not condition:
        failures.append(name)


class Session:
    """Lance le verrou comme un shell le ferait : session et terminal propres."""

    def __init__(self, env, args=(), rows=30, cols=100, background=False):
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            if background:
                pid = os.fork()
                if pid:
                    os.waitpid(pid, 0)
                    os._exit(0)
                os.setpgid(0, 0)
                signal.signal(signal.SIGTTOU, signal.SIG_IGN)
            os.execve(BINARY, [BINARY, *args], env)
        self.output = b""
        self.status = None

    def read(self, duration=0.3):
        end = time.monotonic() + duration
        chunk = b""
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            ready, _, _ = select.select([self.master], [], [], left)
            if not ready:
                continue
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            chunk += data
        self.output += chunk
        return chunk

    def send(self, data):
        os.write(self.master, data if isinstance(data, bytes) else data.encode())
        return self.read().decode(errors="replace")

    def wait(self, timeout=3.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.read(0.05)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = status
                return os.waitstatus_to_exitcode(status)
        return None

    def alive(self):
        if self.status is not None:
            return False
        pid, status = os.waitpid(self.pid, os.WNOHANG)
        if pid:
            self.status = status
            return False
        return True

    def stop(self):
        """Ferme le terminal : une vraie sortie, qui laisse aussi gcov écrire ses compteurs."""
        try:
            os.close(self.master)
        except OSError:
            pass
        end = time.monotonic() + 3
        while self.alive() and time.monotonic() < end:
            time.sleep(0.02)
        if self.alive():
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
            self.status = 0


def make_pam_dir(root):
    script = os.path.join(root, "check.sh")
    with open(script, "w") as f:
        f.write(f"#!/bin/bash\nIFS= read -r -d '' p\n[ \"$p\" = '{PASSWORD}' ]\n")
    os.chmod(script, 0o755)
    with open(os.path.join(root, "vkeep"), "w") as f:
        # pam_exec signale un échec comme une erreur système : pam_deny en fait un vrai refus.
        f.write(f"auth [success=1 default=ignore] pam_exec.so quiet expose_authtok {script}\n"
                "auth requisite pam_deny.so\nauth required pam_permit.so\n")
    slow = os.path.join(root, "slow.sh")
    with open(slow, "w") as f:
        f.write(f"#!/bin/bash\nsleep 0.8\nexec {script}\n")
    os.chmod(slow, 0o755)
    module = os.path.join(root, "pam_prompts.so")
    subprocess.run(["cc", "-shared", "-fPIC", "-o", module, os.path.join(HERE, "pam_prompts.c"), "-lpam"], check=True)
    for name, stack in (("prompts", f"auth required {module}\n"),
                        ("slow", f"auth [success=1 default=ignore] pam_exec.so quiet expose_authtok {slow}\n"
                                 "auth requisite pam_deny.so\nauth required pam_permit.so\n"),
                        ("broken", f"auth required pam_exec.so quiet expose_authtok {script}\n"),
                        ("silent", "auth required pam_deny.so\n")):
        os.mkdir(os.path.join(root, name))
        with open(os.path.join(root, name, "vkeep"), "w") as f:
            f.write(stack)


def main():
    with tempfile.TemporaryDirectory() as root:
        make_pam_dir(root)
        env = {"VKEEP_TEST_PAM_DIR": root, "PATH": os.environ["PATH"], "TERM": "xterm-256color",
               "LANG": "C.UTF-8", "TZ": "UTC", "XDG_STATE_HOME": os.path.join(root, "state"),
               # Jamais la vraie configuration : les tests posteraient sur le vrai salon.
               "XDG_CONFIG_HOME": os.path.join(root, "no-config")}
        run_tests(env, root)
    print(f"\n{len(failures)} échec(s)" if failures else "\nTous les tests passent")
    sys.exit(1 if failures else 0)


def make_fake_ffmpeg(root, name, body):
    """Un faux ffmpeg dans son dossier : les tests n'ouvrent jamais la vraie webcam."""
    directory = os.path.join(root, name)
    os.mkdir(directory)
    path = os.path.join(directory, "ffmpeg")
    with open(path, "w") as f:
        f.write(f"#!/bin/bash\n{body}\n")
    os.chmod(path, 0o755)
    return directory


def run_photo_tests(env, root):
    state = os.path.join(root, "photo-state")
    calls = os.path.join(root, "ffmpeg-calls")
    working = make_fake_ffmpeg(root, "ffmpeg-ok", f'printf "%s\\n" "$*" >> {calls}\necho image > "${{@: -1}}"')
    env = dict(env, XDG_STATE_HOME=state, PATH=f"{working}:{env['PATH']}")
    journal = os.path.join(state, "vkeep", "attempts.log")

    # Deux échecs rapprochés : deux lignes au journal, mais une seule photo
    s = Session(dict(env, VKEEP_TEST_CAMERA="/dev/videoTEST"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.3)
    s.send("faux2\r")
    s.read(0.3)
    check("photographie une tentative", os.path.exists(calls))
    args = open(calls).read() if os.path.exists(calls) else ""
    check("utilise la caméra demandée", "/dev/videoTEST" in args, args)
    check("une seule photo pour deux échecs rapprochés", args.count("\n") == 1, args)
    check("n'attend pas la caméra pour redemander", s.alive())
    s.send(PASSWORD + "\r")
    code = s.wait(6.0)
    check("déverrouille avec une photo prise", code == 0, f"code={code}")
    text = s.output.decode(errors="replace")
    check("résume les tentatives au déverrouillage", "2 tentatives" in text and ".jpg" in text, text[-300:])
    check("le mot de passe n'apparaît pas dans le résumé", PASSWORD not in text and "faux" not in text)
    s.stop()
    lines = open(journal).read().splitlines() if os.path.exists(journal) else []
    check("note les tentatives au journal", len(lines) == 2 and all("wrong-password" in l for l in lines), str(lines))
    check("le journal ne contient pas les saisies", not any("faux" in l for l in lines), str(lines))
    check("le journal explique une photo manquante", len(lines) == 2 and "no-photo:too-soon" in lines[1], str(lines))
    photos = [f for f in os.listdir(os.path.dirname(journal)) if f.endswith(".jpg")]
    check("range la photo avec le journal", len(photos) == 1, str(photos))
    mode = os.stat(os.path.dirname(journal)).st_mode & 0o777
    check("dossier accessible au seul propriétaire", mode == 0o700, oct(mode))

    # Sans -a : le journal, mais pas de caméra
    os.remove(calls)
    s = Session(env)
    s.read(0.3)
    s.send("faux\r")
    s.read(0.2)
    s.send(PASSWORD + "\r")
    code = s.wait()
    check("sans -a, aucune photo", code == 0 and not os.path.exists(calls), f"code={code}")
    check("sans -a, le résumé reste", b"1 tentative" in s.output)
    check("sans -a, le journal le dit", "no-photo:not-requested" in open(journal).read().splitlines()[-1])
    s.stop()

    # ffmpeg absent : le verrou fonctionne et le dit
    empty = os.path.join(root, "no-ffmpeg")
    os.mkdir(empty)
    s = Session(dict(env, PATH=empty, VKEEP_TEST_CAMERA="/dev/videoTEST"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.2)
    s.send(PASSWORD + "\r")
    code = s.wait()
    check("sans ffmpeg, le verrou marche", code == 0, f"code={code}")
    check("sans ffmpeg, le résumé l'indique", "pas de photo".encode() in s.output, repr(s.output[-200:]))
    check("sans ffmpeg, le journal donne l'erreur", "no-photo:spawn-failed" in open(journal).read().splitlines()[-1],
          open(journal).read()[-200:])
    s.stop()

    # Détection automatique : la vraie webcam si la machine en a une, jamais ouverte par le faux ffmpeg
    s = Session(dict(env, VKEEP_TEST_CAMERA="auto"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.3)
    s.send(PASSWORD + "\r")
    code = s.wait()
    last = open(journal).read().splitlines()[-1]
    check("détection : le verrou marche, avec ou sans caméra", code == 0, f"code={code}")
    check("détection : photo, ou absence de caméra expliquée", last.endswith(".jpg") or "no-photo:no-camera" in last, last)
    s.stop()

    # ffmpeg qui échoue : pas de photo annoncée à tort
    failing = make_fake_ffmpeg(root, "ffmpeg-fail", 'echo "caméra occupée" >&2\nexit 1')
    s = Session(dict(env, PATH=f"{failing}:{env['PATH']}", VKEEP_TEST_CAMERA="/dev/videoTEST"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.3)
    s.send(PASSWORD + "\r")
    code = s.wait()
    check("caméra en panne : le verrou marche et ne promet pas de photo",
          code == 0 and "pas de photo".encode() in s.output, f"code={code}")
    s.stop()
    errors = os.path.join(os.path.dirname(journal), "ffmpeg-errors.log")
    check("garde les erreurs de ffmpeg", os.path.exists(errors) and "caméra occupée" in open(errors).read())

    # ffmpeg bloqué : il ne doit pas retenir le déverrouillage indéfiniment
    stuck = make_fake_ffmpeg(root, "ffmpeg-stuck", "trap '' TERM\nsleep 60")
    s = Session(dict(env, PATH=f"{stuck}:{env['PATH']}", VKEEP_TEST_CAMERA="/dev/videoTEST"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.2)
    start = time.monotonic()
    s.send(PASSWORD + "\r")
    code = s.wait(7.0)
    elapsed = time.monotonic() - start
    check("caméra bloquée : déverrouille quand même, avec un délai borné", code == 0 and elapsed < 6.5, f"code={code} {elapsed:.1f}s")
    s.stop()


WEBHOOK = "https://discord.com/api/webhooks/123/jeton-secret"


def make_fake_curl(root, calls):
    """Un faux curl : garde ce qu'il reçoit sur son entrée, sans jamais toucher au réseau."""
    os.mkdir(calls)
    directory = os.path.join(root, "curl-bin")
    os.mkdir(directory)
    path = os.path.join(directory, "curl")
    with open(path, "w") as f:
        f.write(f'#!/bin/bash\nout={calls}/$(date +%s%N)-$$\n{{ printf "ARGS %s\\n" "$*"; cat; }} > "$out.tmp"\n'
                f'mv "$out.tmp" "$out.txt"\n'
                '[ -n "$FAKE_CURL_EXIT" ] && { echo "curl: (22) erreur simulée" >&2; exit "$FAKE_CURL_EXIT"; }\nexit 0\n')
    os.chmod(path, 0o755)
    return directory


def write_config(root, name, text, mode=0o600):
    home = os.path.join(root, name)
    os.makedirs(os.path.join(home, "vkeep"))
    path = os.path.join(home, "vkeep", "config")
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)
    return home


def sent_messages(calls, timeout=2.0):
    """Les envois reçus par le faux curl : (arguments, contenu JSON, photo jointe ou None)."""
    import json
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        names = sorted(n for n in os.listdir(calls) if n.endswith(".txt"))
        time.sleep(0.2)
        if names == sorted(n for n in os.listdir(calls) if n.endswith(".txt")) and names:
            break
    messages = []
    for name in sorted(n for n in os.listdir(calls) if n.endswith(".txt")):
        lines = open(os.path.join(calls, name)).read().splitlines()
        args, payload, photo, url = lines[0], None, None, None
        for line in lines[1:]:
            key, _, value = line.partition(" = ")
            if value.startswith('"'):
                value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
            if key == "form-string" and value.startswith("payload_json="):
                payload = json.loads(value[len("payload_json="):])
            elif key == "form" and value.startswith("files[0]=@"):
                photo = value[len('files[0]=@"'):value.rindex('";type=')]
            elif key == "url":
                url = value
        embed = payload["embeds"][0] if payload and payload.get("embeds") else {}
        fields = {f["name"]: f["value"] for f in embed.get("fields", [])}
        # Tout le texte visible, pour chercher dedans d'un coup.
        text = "\n".join([payload["content"] if payload else "", embed.get("title", ""), embed.get("description", "")]
                         + [f"{k}: {v}" for k, v in fields.items()])
        messages.append({"args": args, "content": text, "embed": embed, "fields": fields, "photo": photo, "url": url})
    return messages


def clear(calls):
    for name in os.listdir(calls):
        os.remove(os.path.join(calls, name))


def run_notify_tests(env, root):
    calls = os.path.join(root, "curl-calls")
    curl_dir = make_fake_curl(root, calls)
    camera = make_fake_ffmpeg(root, "ffmpeg-notify", 'sleep 0.3\necho image > "${@: -1}"')
    state = os.path.join(root, "notify-state")
    config_home = write_config(root, "config-ok", f"# alertes\ndiscord_webhook = {WEBHOOK}\n")
    env = dict(env, PATH=f"{curl_dir}:{camera}:{env['PATH']}", XDG_STATE_HOME=state, XDG_CONFIG_HOME=config_home)

    # Sans fichier de configuration : aucune alerte
    s = Session(dict(env, XDG_CONFIG_HOME=os.path.join(root, "absent")))
    s.read(0.3)
    s.send("faux\r")
    s.send(PASSWORD + "\r")
    check("sans configuration : se déverrouille", s.wait() == 0)
    s.stop()
    check("sans configuration : aucune alerte", sent_messages(calls, 0.5) == [])

    # Verrouillage, tentative avec photo, déverrouillage
    s = Session(dict(env, VKEEP_TEST_CAMERA="/dev/videoTEST"))
    s.read(0.3)
    s.send("faux\r")
    s.read(1.0)  # le temps de la photo
    s.send(PASSWORD + "\r")
    code = s.wait(6.0)
    check("alertes : se déverrouille", code == 0, f"code={code}")
    s.stop()
    messages = sent_messages(calls)
    contents = [m["content"] for m in messages]
    check("alertes : trois messages", len(messages) == 3, str(contents))
    check("alerte au verrouillage", any("Session verrouillée" in c and "Compte: " in c for c in contents), str(contents))
    attempt = [m for m in messages if "Mauvais mot de passe" in m["content"]]
    check("alerte de tentative avec la photo", len(attempt) == 1 and attempt[0]["photo"] is not None
          and attempt[0]["photo"].endswith(".jpg") and os.path.exists(attempt[0]["photo"]), str(messages))
    check("la photo s'affiche dans l'embed", len(attempt) == 1 and attempt[0]["photo"] is not None
          and attempt[0]["embed"].get("image", {}).get("url") == "attachment://" + os.path.basename(attempt[0]["photo"]),
          str(attempt))
    check("embed complet : couleur, heure et pied", all(
        isinstance(m["embed"].get("color"), int) and m["embed"].get("timestamp", "").endswith("Z")
        and m["embed"].get("footer", {}).get("text", "").startswith("vKeep ") for m in messages), str(messages))
    check("la tentative est numérotée", len(attempt) == 1 and attempt[0]["fields"].get("Tentative") == "n° 1", str(attempt))
    check("alerte au déverrouillage avec le bilan",
          any("Session déverrouillée" in m["content"] and m["fields"].get("Tentatives") == "1" and "Durée" in m["fields"]
              for m in messages), str(contents))
    check("envoie au webhook configuré", all(m["url"] == WEBHOOK for m in messages), str(messages))
    check("le webhook n'est pas sur la ligne de commande",
          all(m["args"] == "ARGS -q --config -" for m in messages), str([m["args"] for m in messages]))
    check("le mot de passe ne part jamais", not any(PASSWORD in m["content"] or "faux" in m["content"] for m in messages))
    clear(calls)

    # Caméra en panne : l'alerte part quand même, sans photo
    failing = os.path.join(root, "ffmpeg-fail")
    s = Session(dict(env, PATH=f"{curl_dir}:{failing}:{env['PATH']}", VKEEP_TEST_CAMERA="/dev/videoTEST"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.5)
    s.send(PASSWORD + "\r")
    s.wait()
    s.stop()
    attempt = [m for m in sent_messages(calls) if "Mauvais mot de passe" in m["content"]]
    check("caméra en panne : alerte sans photo", len(attempt) == 1 and attempt[0]["photo"] is None
          and attempt[0]["fields"].get("Photo") == "impossible" and "image" not in attempt[0]["embed"], str(attempt))
    clear(calls)

    # Pas de caméra du tout, et alerte immédiate sans attendre de photo
    s = Session(dict(env, VKEEP_TEST_CAMERA="auto", PATH=f"{curl_dir}:{os.environ['PATH']}"))
    s.read(0.3)
    s.send("faux\r")
    s.send(PASSWORD + "\r")
    s.wait()
    s.stop()
    attempt = [m for m in sent_messages(calls) if "Mauvais mot de passe" in m["content"]]
    check("détection : une alerte par tentative", len(attempt) == 1, str(attempt))
    clear(calls)

    # Terminal fermé : on le sait aussi
    s = Session(env)
    s.read(0.3)
    s.stop()
    contents = [m["content"] for m in sent_messages(calls)]
    check("alerte si le verrou s'arrête sans mot de passe", any("terminal a été fermé" in c for c in contents), str(contents))
    clear(calls)

    # curl qui échoue : le verrou continue et l'erreur est gardée
    s = Session(dict(env, FAKE_CURL_EXIT="22"))
    s.read(0.3)
    s.send("faux\r")
    s.read(0.3)
    s.send(PASSWORD + "\r")
    check("envoi en échec : se déverrouille", s.wait() == 0)
    s.stop()
    sent_messages(calls)
    errors = os.path.join(state, "vkeep", "notify-errors.log")
    log = open(errors).read() if os.path.exists(errors) else ""
    check("envoi en échec : erreur gardée", "erreur simulée" in log and "code 22" in log, log)
    clear(calls)

    # curl absent : le verrou fonctionne et le note
    fresh = os.path.join(root, "notify-state-fresh")
    s = Session(dict(env, PATH=camera, XDG_STATE_HOME=fresh))
    s.read(0.3)
    s.send(PASSWORD + "\r")
    check("sans curl : se déverrouille", s.wait() == 0)
    s.stop()
    fresh_errors = os.path.join(fresh, "vkeep", "notify-errors.log")
    check("sans curl : erreur gardée", "curl n'a pas pu être lancé" in open(fresh_errors).read())
    mode = os.stat(fresh_errors).st_mode & 0o777
    check("journal d'erreurs lisible par le seul propriétaire", mode == 0o600, oct(mode))

    # Configuration douteuse : avertie avant de verrouiller
    bad = write_config(root, "config-bad", "discord_webhook = http://exemple.org/hook\ncouleur = rouge\nn'importe quoi\n", 0o644)
    s = Session(dict(env, XDG_CONFIG_HOME=bad))
    s.read(0.3)
    s.send(PASSWORD + "\r")
    s.wait()
    s.stop()
    text = s.output.decode(errors="replace")
    check("avertit d'une configuration lisible par tous", "chmod 600" in text, text[:400])
    check("avertit d'un réglage inconnu", "réglage inconnu « couleur »" in text, text[:400])
    check("avertit d'une ligne sans =", "il manque « = »" in text, text[:400])
    check("refuse un webhook sans https", "https://" in text and sent_messages(calls, 0.5) == [], text[:400])

    quoted = write_config(root, "config-quoted", f'discord_webhook = "{WEBHOOK}"\n')

    # --test-notify
    s = Session(dict(env, XDG_CONFIG_HOME=quoted), args=("--test-notify",))
    code = s.wait()
    s.stop()
    messages = sent_messages(calls)
    check("--test-notify envoie un message", code == 0 and len(messages) == 1 and "Test des alertes" in messages[0]["content"]
          and messages[0]["url"] == WEBHOOK and b"1049h" not in s.output, f"code={code} {messages}")
    clear(calls)
    s = Session(dict(env, FAKE_CURL_EXIT="22"), args=("--test-notify",))
    code = s.wait()
    s.stop()
    check("--test-notify signale un échec", code == 1 and b"erreur simul" in s.output and b"code 22" in s.output,
          f"code={code} {s.output!r}")
    clear(calls)
    s = Session(dict(env, PATH=camera), args=("--test-notify",))
    check("--test-notify sans curl", s.wait() == 1 and b"curl" in s.output, repr(s.output))
    s.stop()
    s = Session(dict(env, XDG_CONFIG_HOME=os.path.join(root, "absent")), args=("--test-notify",))
    check("--test-notify sans webhook explique quoi faire", s.wait() == 1 and b"discord_webhook =" in s.output,
          repr(s.output))
    s.stop()
    s = Session(env, args=("--test-notify", "-a"))
    check("--test-notify ne se combine pas avec -a", s.wait() == 2)
    s.stop()


def run_tests(env, root):
    run_photo_tests(env, root)
    run_notify_tests(env, root)
    # Écran et mauvais mot de passe
    s = Session(env)
    screen = s.read(0.5).decode(errors="replace")
    check("affiche l'écran de verrouillage", "Cette console est verrouillée" in screen, screen)
    check("passe sur l'écran alternatif", "\x1b[?1049h" in screen)
    check("cadre arrondi hors console", "╭" in screen)
    check("logo en dégradé", "██╗" in screen and "\x1b[38;" in screen)
    check("barre entre le logo et l'heure", "── & ──" in screen)
    s.send("mauvais")
    screen = s.send("\r")
    screen += s.read(0.5).decode(errors="replace")
    check("refuse un mauvais mot de passe", s.alive())
    check("signale l'échec", "Mot de passe incorrect" in screen, screen)
    screen = s.send("encore\r") + s.read(0.5).decode(errors="replace")
    check("compte les échecs", "2 échecs" in screen, screen)
    check("n'affiche jamais la saisie", b"mauvais" not in s.output and b"encore" not in s.output)

    # Saisie vide
    before = s.alive()
    screen = s.send("\r\r\r")
    check("une saisie vide ne compte pas", before and s.alive() and "3 échecs" not in screen)

    # Signaux et raccourcis clavier
    s.send("\x03\x1a\x1c\x04")
    check("Ctrl-C, Ctrl-Z, Ctrl-\\ et Ctrl-D ne déverrouillent pas", s.alive())
    s.send("\x15")
    for signum in (signal.SIGINT, signal.SIGQUIT, signal.SIGTSTP, signal.SIGTERM):
        os.kill(s.pid, signum)
    s.read(0.2)
    check("les signaux ne déverrouillent pas", s.alive())
    pid, status = os.waitpid(s.pid, os.WNOHANG | os.WUNTRACED)
    check("le processus n'est pas suspendu", pid == 0, str(status))

    # Édition : le bon mot de passe doit être reconstitué malgré les touches parasites
    s.send("\x15zzz\x7f\x7f\x7f")          # effacement
    s.send("poubelle\x15")                  # Ctrl-U
    s.send("xé\x7f\x7f")                    # effacement d'un caractère UTF-8 entier
    s.send("poubelle")
    s.send(b"\x1b")                         # Échap seul : efface
    s.send(b"\x1b[A\x1b[3~\x1bOP\x1b[[A")   # flèche, Suppr, F1 xterm et F1 console
    s.send(PASSWORD[:3])
    s.send(b"\x1bx")                        # Alt+x est ignoré
    s.send(PASSWORD[3:] + "\r")
    code = s.wait()
    check("déverrouille avec le bon mot de passe", code == 0, f"code={code}")
    check("quitte l'écran alternatif et rend le curseur",
          b"\x1b[?25h\x1b[?1049l" in s.output)
    s.stop()

    # Le terminal retrouve sa configuration
    master, slave = pty.openpty()
    before = termios.tcgetattr(slave)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.execve(BINARY, [BINARY], env)
    time.sleep(0.4)
    during = termios.tcgetattr(slave)
    check("désactive écho et signaux pendant le verrou",
          not during[3] & (termios.ECHO | termios.ISIG | termios.ICANON))
    os.write(master, (PASSWORD + "\r").encode())
    _, status = os.waitpid(pid, 0)
    check("restaure le terminal", termios.tcgetattr(slave) == before)
    os.close(slave)
    os.close(master)

    # Fermeture du terminal : pas de processus qui tourne à vide
    s = Session(env)
    s.read(0.3)
    os.close(s.master)
    code = None
    end = time.monotonic() + 2
    while time.monotonic() < end and code is None:
        pid, status = os.waitpid(s.pid, os.WNOHANG)
        if pid:
            code = os.waitstatus_to_exitcode(status)
        time.sleep(0.02)
    check("quitte quand le terminal disparaît", code is not None, f"code={code}")
    if code is None:
        os.kill(s.pid, signal.SIGKILL)
        os.waitpid(s.pid, 0)

    # L'écran complet doit tenir dans un terminal classique
    s = Session(env, rows=24, cols=80)
    screen = s.read(0.4).decode(errors="replace")
    check("écran complet sur 80x24", "── & ──" in screen and "╭" in screen and "valider" in screen, screen[:200])
    s.stop()

    # Redimensionnement et petits terminaux
    s = Session(env, rows=10, cols=30)
    screen = s.read(0.4).decode(errors="replace")
    check("mode compact sur un petit terminal", "╭" not in screen and "verrouill" in screen, screen)
    fcntl.ioctl(s.master, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    screen = s.read(1.3).decode(errors="replace")
    check("redessine après un redimensionnement", "╭" in screen, screen)
    s.stop()
    s = Session(env, rows=2, cols=8)
    s.read(0.3)
    s.send(PASSWORD + "\r")
    code = s.wait()
    check("reste utilisable sur un minuscule terminal", code == 0, f"code={code}")
    s.stop()

    # Pendant une vérification lente : l'indicateur tourne et le verrou tient
    s = Session(dict(env, VKEEP_TEST_PAM_DIR=os.path.join(root, "slow")))
    s.read(0.3)
    s.output = b""
    os.write(s.master, b"faux\r")
    screen = s.read(0.7).decode(errors="replace")
    frames = {c for c in "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏" if c in screen}
    check("l'indicateur d'attente s'anime", len(frames) >= 3 and "Vérification" in screen, screen[:300])
    # Un seul repaint complet reste normal : l'écran entier est redessiné quand la minute change.
    check("seule la ligne de l'indicateur est redessinée", screen.count("\x1b[2J") <= 1, str(screen.count("\x1b[2J")))
    os.kill(s.pid, signal.SIGINT)
    screen = s.read(0.8).decode(errors="replace")
    check("reste verrouillé après une vérification lente", s.alive() and "incorrect" in screen, screen[:300])
    s.send(PASSWORD + "\r")
    code = s.wait(4.0)
    check("le bon mot de passe passe après l'indicateur", code == 0 and "Déverrouillé".encode() in s.output, f"code={code}")
    s.stop()

    # Messages du module et saisie visible (un code à usage unique, par exemple)
    s = Session(dict(env, VKEEP_TEST_PAM_DIR=os.path.join(root, "prompts")))
    screen = s.read(0.5).decode(errors="replace")
    check("affiche les messages du module PAM", "Alerte du module" in screen and "Information du module" in screen, screen[-300:])
    check("affiche la question du module", "Code de test" in screen, screen[-300:])
    screen = s.send("42")
    check("montre une saisie visible", "42" in screen, screen)
    screen = s.send("\x7f\x7f9\r") + s.read(0.3).decode(errors="replace")
    check("refuse un mauvais code", s.alive() and "incorrect" in screen, screen[-300:])
    s.send("4242\r")
    code = s.wait()
    check("accepte le bon code", code == 0, f"code={code}")
    s.stop()

    # Une pile PAM en panne ne doit jamais déverrouiller
    s = Session(dict(env, VKEEP_TEST_PAM_DIR=os.path.join(root, "broken")))
    s.read(0.3)
    screen = s.send("nimportequoi\r") + s.read(0.5).decode(errors="replace")
    check("reste verrouillé si PAM est en erreur", s.alive())
    check("affiche l'erreur PAM", "incorrect" not in screen and "chec" not in screen and "error" in screen.lower(), screen)
    s.stop()

    # PAM qui refuse sans rien demander : pas de boucle à pleine vitesse
    s = Session(dict(env, VKEEP_TEST_PAM_DIR=os.path.join(root, "silent")))
    s.read(3.0)
    check("reste verrouillé si PAM refuse tout", s.alive())
    check("ne boucle pas à vide", b"3 \xc3\xa9checs" not in s.output and b"incorrect" in s.output, repr(s.output[-200:]))
    s.stop()

    # Refus avant de verrouiller
    s = Session(env, args=("-a",))
    code = s.wait()
    check("-a refuse un terminal qui n'est pas une console", code == 1 and b"console virtuelle" in s.output
          and b"1049h" not in s.output, f"code={code} {s.output!r}")
    s.stop()

    s = Session(env, background=True)
    code = s.wait()
    check("refuse de tourner en arrière-plan", b"premier plan" in s.output and b"1049h" not in s.output,
          f"{s.output!r}")
    s.stop()

    no_service = dict(env, VKEEP_TEST_PAM_DIR=os.path.join(root, "absent"))
    s = Session(no_service)
    code = s.wait()
    check("refuse de verrouiller sans service PAM", code == 1 and b"1049h" not in s.output,
          f"code={code} {s.output!r}")
    s.stop()

    s = Session(env, args=("--camera=/dev/video0",))
    check("--camera sans -a est refusé", s.wait() == 2 and b"1049h" not in s.output)
    s.stop()
    s = Session(env, args=("-p",))
    check("l'ancienne option -p n'existe plus", s.wait() == 2)
    s.stop()
    s = Session(env, args=("--inconnu",))
    check("rejette une option inconnue", s.wait() == 2)
    s.stop()
    s = Session(env, args=("--version",))
    check("affiche la version", s.wait() == 0 and b"vKeep " in s.output and b"1049h" not in s.output)
    s.stop()
    s = Session(env, args=("-h",))
    check("affiche l'aide", s.wait() == 0 and b"--all" in s.output)
    s.stop()


main()
