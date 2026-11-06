#!/usr/bin/env python3
"""Teste l'archive de release et les scripts d'installation, dans un dossier jetable.

Un faux sudo exécute les commandes avec le compte courant, et DESTDIR garde tout dans ce dossier.
"""
import os
import pty
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures = []


def check(name, condition, detail=""):
    print(f"{'ok  ' if condition else 'FAIL'} {name}" + (f" : {detail}" if detail and not condition else ""), flush=True)
    if not condition:
        failures.append(name)


def run(args, env, input_text=None):
    result = subprocess.run(args, env=env, capture_output=True, text=True, input=input_text)
    return result.returncode, result.stdout + result.stderr


def mode(path):
    return os.stat(path).st_mode & 0o7777


def run_in_terminal(args, env):
    """Lance un script dans un pseudo-terminal, pour passer par l'affichage complet."""
    master, slave = pty.openpty()
    process = subprocess.Popen(args, env=env, stdin=slave, stdout=slave, stderr=slave)
    os.close(slave)
    output = b""
    while True:
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        output += data
    os.close(master)
    return process.wait(), output.decode(errors="replace")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        shims = os.path.join(tmp, "shims")
        os.mkdir(shims)
        with open(os.path.join(shims, "sudo"), "w") as f:
            f.write('#!/bin/bash\n[[ $1 == -* ]] && exit 0\nexec "$@"\n')
        os.chmod(os.path.join(shims, "sudo"), 0o755)

        dest = os.path.join(tmp, "dest")
        state = os.path.join(tmp, "state")
        env = {"PATH": f"{shims}:{os.environ['PATH']}", "HOME": tmp, "DESTDIR": dest, "XDG_STATE_HOME": state,
               "LANG": "C.UTF-8", "TERM": "xterm-256color", "CXX": os.environ.get("CXX", "g++")}
        binary = os.path.join(dest, "usr/local/bin/vkeep")
        pam = os.path.join(dest, "etc/pam.d/vkeep")

        # Une copie du dépôt : la version compilée dans l'archive ne doit pas polluer build/.
        source = os.path.join(tmp, "source")
        shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
        code, output = run(["make", "-C", source, "-j8", "VERSION=9.9.9", "dist"], env)
        check("make dist construit l'archive", code == 0, output[-400:])
        archive = os.path.join(source, "build/dist/vkeep-9.9.9-linux-" + os.uname().machine + ".tar.gz")
        check("make dist donne une empreinte", os.path.exists(archive + ".sha256"))

        extract = os.path.join(tmp, "extract")
        with tarfile.open(archive) as tar:
            tar.extractall(extract, filter="tar")
        unpacked = os.path.join(extract, os.listdir(extract)[0])
        for needed in ("build/vkeep", "pam/vkeep", "scripts/install.sh", "scripts/uninstall.sh",
                       "scripts/lib/ui.sh", "LICENSE", "README.md"):
            check(f"l'archive contient {needed}", os.path.exists(os.path.join(unpacked, needed)))
        check("l'archive ne contient pas les sources", not os.path.exists(os.path.join(unpacked, "src")))

        # Installation depuis l'archive
        code, output = run([os.path.join(unpacked, "scripts/install.sh"), "--no-deps", "--no-camera"], env)
        check("install.sh réussit depuis l'archive", code == 0, output[-500:])
        check("install.sh annonce la version de l'archive", "vKeep 9.9.9" in output, output[:200])
        check("n'a pas compilé depuis l'archive", "binaire de l'archive" in output, output[:300])
        check("installe le binaire", os.path.exists(binary) and mode(binary) == 0o755)
        check("le binaire n'a pas de setuid", os.path.exists(binary) and not mode(binary) & 0o6000)
        check("installe le fichier PAM", os.path.exists(pam) and mode(pam) == 0o644)
        check("le fichier PAM n'utilise pas system-auth", "system-auth" not in
              "".join(l for l in open(pam) if not l.startswith("#")))
        code, output = run([binary, "--version"], env)
        check("le binaire installé donne sa version", code == 0 and "9.9.9" in output, output)
        code, _ = run([os.path.join(unpacked, "scripts/install.sh"), "--no-deps", "--no-camera"], env)
        check("install.sh peut être relancé", code == 0)

        # Désinstallation : les photos restent, sauf avec --purge
        os.makedirs(os.path.join(state, "vkeep"))
        photo = os.path.join(state, "vkeep", "photo.jpg")
        open(photo, "w").close()
        config = os.path.join(tmp, ".config", "vkeep", "config")
        os.makedirs(os.path.dirname(config))
        open(config, "w").close()
        uninstall = os.path.join(unpacked, "scripts/uninstall.sh")
        code, output = run([uninstall], env)
        check("uninstall.sh réussit", code == 0, output[-400:])
        check("retire le binaire et le fichier PAM", not os.path.exists(binary) and not os.path.exists(pam))
        check("garde les photos par défaut", os.path.exists(photo))
        check("garde la configuration des alertes par défaut", os.path.exists(config))
        code, _ = run([uninstall], env)
        check("uninstall.sh peut être relancé", code == 0)
        code, _ = run([uninstall, "--purge"], env, input_text="n\nn\n")
        check("--purge respecte un refus", code == 0 and os.path.exists(photo) and os.path.exists(config))
        code, _ = run([uninstall, "--purge"], env, input_text="o\nn\n")
        check("--purge supprime après confirmation", code == 0 and not os.path.exists(photo) and os.path.exists(config))
        os.makedirs(os.path.join(state, "vkeep"))
        code, _ = run([uninstall, "--purge", "--yes"], env)
        check("--purge --yes ne demande rien", code == 0 and not os.path.exists(os.path.join(state, "vkeep"))
              and not os.path.exists(os.path.dirname(config)))

        # Depuis les sources
        code, output = run([os.path.join(source, "scripts/install.sh"), "--no-deps", "--no-camera"], env)
        check("install.sh réussit depuis les sources", code == 0 and os.path.exists(binary), output[-500:])
        check("indique la compilation locale", "compilation locale" in output, output[:300])
        run([os.path.join(source, "scripts/uninstall.sh")], env)

        # Affichage complet dans un terminal, avec le logo et le récapitulatif
        code, output = run_in_terminal([os.path.join(unpacked, "scripts/install.sh"), "--no-deps", "--no-camera"], env)
        check("affichage complet dans un terminal", code == 0 and "██╗" in output and "[4/4]" in output
              and "╭" in output and "vKeep est installé" in output, output[-400:])
        code, output = run_in_terminal([uninstall], env)
        check("désinstallation complète dans un terminal", code == 0 and "vKeep est désinstallé" in output, output[-300:])

        # Cas d'erreur, sans rien installer
        code, output = run([os.path.join(unpacked, "scripts/install.sh"), "--inconnu"], env)
        check("rejette une option inconnue", code == 2 and not os.path.exists(binary))
        code, output = run([uninstall, "--inconnu"], env)
        check("uninstall rejette une option inconnue", code == 2)

        broken = os.path.join(tmp, "broken")
        shutil.copytree(unpacked, broken)
        with open(os.path.join(broken, "build/vkeep"), "w") as f:
            f.write("#!/bin/false\n")
        code, output = run([os.path.join(broken, "scripts/install.sh"), "--no-deps", "--no-camera"], env)
        check("refuse un binaire qui ne démarre pas", code == 1 and "sources" in output and not os.path.exists(binary), output[-300:])

        empty = os.path.join(tmp, "empty")
        shutil.copytree(unpacked, empty)
        os.remove(os.path.join(empty, "build/vkeep"))
        code, output = run([os.path.join(empty, "scripts/install.sh"), "--no-deps"], env)
        check("signale une archive incomplète", code == 1 and "incomplète" in output, output)

    print(f"\n{len(failures)} échec(s)" if failures else "\nTous les tests passent")
    sys.exit(1 if failures else 0)


main()
