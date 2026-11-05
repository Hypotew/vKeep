#!/usr/bin/env python3
"""Teste le verrou sur de vraies consoles virtuelles, dans une VM QEMU jetable.

La VM démarre le noyau de l'hôte avec un initramfs minimal, le vrai pam_unix et un compte
de test. Les touches passent par le clavier émulé : rien ne touche aux consoles de l'hôte.
"""
import os
import re
import select
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

PASSWORD = "testlock"
USER = "invite"
KERNEL = "/boot/vmlinuz-linux"
HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.abspath(sys.argv[1])
SCREENS = os.path.abspath(os.path.join(os.path.dirname(BINARY), "vm-screens"))
failures = []


def check(name, condition, detail=""):
    print(f"{'ok  ' if condition else 'FAIL'} {name}" + (f" : {detail}" if detail and not condition else ""), flush=True)
    if not condition:
        failures.append(name)


class Cpio:
    def __init__(self):
        self.data = bytearray()
        self.names = set()
        self.ino = 1

    def _entry(self, name, mode, body=b"", uid=0, gid=0):
        if name in self.names:
            return
        self.names.add(name)
        raw = name.encode() + b"\0"
        fields = [self.ino, mode, uid, gid, 1, 0, len(body), 0, 0, 0, 0, len(raw), 0]
        self.ino += 1
        self.data += b"070701" + b"".join(b"%08X" % f for f in fields) + raw
        self.data += b"\0" * (-len(self.data) % 4) + body
        self.data += b"\0" * (-len(self.data) % 4)

    def directory(self, name):
        self._entry(name, stat.S_IFDIR | 0o755)

    def symlink(self, name, target):
        self._entry(name, stat.S_IFLNK | 0o777, target.encode())

    def file(self, name, body, mode=0o755, uid=0, gid=0):
        parts = name.split("/")
        for i in range(1, len(parts)):
            self.directory("/".join(parts[:i]))
        self._entry(name, stat.S_IFREG | mode, body, uid, gid)

    def host_file(self, path, mode=0o755):
        self.file(path.lstrip("/"), open(os.path.realpath(path), "rb").read(), mode)

    def finish(self):
        self._entry("TRAILER!!!", 0)
        return bytes(self.data)


def libraries(path):
    out = subprocess.run(["ldd", path], capture_output=True, text=True, check=True).stdout
    found = re.findall(r"(?:=> )?(/\S+) \(0x", out)
    return [os.path.join("/usr/lib", os.path.basename(p)) if p.startswith("/lib") else p for p in found]


def build_initramfs(root):
    init = os.path.join(root, "init")
    subprocess.run(["gcc", "-static", "-O1", "-o", init, os.path.join(HERE, "vm_init.c")], check=True)
    secret = subprocess.run(["openssl", "passwd", "-6", PASSWORD], capture_output=True, text=True, check=True).stdout.strip()

    archive = Cpio()
    for directory in ("usr", "usr/bin", "usr/lib", "etc", "etc/pam.d", "proc", "sys", "dev", "tmp"):
        archive.directory(directory)
    archive.symlink("lib", "usr/lib")
    archive.symlink("lib64", "usr/lib")
    archive.symlink("bin", "usr/bin")
    archive.symlink("usr/sbin", "bin")
    archive.symlink("usr/lib64", "lib")
    archive.file("init", open(init, "rb").read())
    # Faux ffmpeg : le test porte sur le déclenchement, et la VM n'a pas de caméra.
    fake = os.path.join(root, "ffmpeg")
    subprocess.run(["gcc", "-static", "-O1", "-x", "c", "-o", fake, "-"], check=True, text=True,
                   input='#include <stdio.h>\nint main(int c, char** v) { FILE* f = fopen(v[c - 1], "w"); '
                         'if (f) fputs("image", f); return f == NULL; }\n')
    archive.file("usr/bin/ffmpeg", open(fake, "rb").read())
    archive.file("usr/bin/vkeep", open(BINARY, "rb").read())
    archive.host_file("/usr/bin/unix_chkpwd", 0o4755)
    archive.host_file("/usr/lib/security/pam_unix.so")
    archive.file("etc/pam.d/vkeep", open(os.path.join(HERE, "..", "pam", "vkeep"), "rb").read(), 0o644)
    archive.file("etc/nsswitch.conf", b"passwd: files\nshadow: files\ngroup: files\n", 0o644)
    archive.file("etc/passwd", f"root:x:0:0::/:/x\n{USER}:x:1000:1000::/:/x\n".encode(), 0o644)
    archive.file("etc/group", f"root:x:0:\n{USER}:x:1000:\n".encode(), 0o644)
    archive.file("etc/shadow", f"root:*:20000::::::\n{USER}:{secret}:20000::::::\n".encode(), 0o600)
    for program in (BINARY, "/usr/bin/unix_chkpwd", "/usr/lib/security/pam_unix.so"):
        for library in libraries(program):
            archive.host_file(library)
    path = os.path.join(root, "initramfs")
    with open(path, "wb") as f:
        f.write(archive.finish())
    return path


class Vm:
    def __init__(self, root, initramfs, args):
        os.chdir(root)  # un chemin de socket Unix est limité à 108 caractères
        self.monitor_path = "mon.sock"
        self.log = ""
        kvm = ["-enable-kvm"] if os.access("/dev/kvm", os.W_OK) else []
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64", *kvm, "-m", "512", "-display", "none", "-vga", "std", "-no-reboot",
             "-kernel", KERNEL, "-initrd", initramfs,
             "-append", f"console=ttyS0 loglevel=1 panic=-1 VM_ARGS={args}",
             "-serial", "stdio", "-monitor", f"unix:{self.monitor_path},server,nowait"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                self.monitor = socket.socket(socket.AF_UNIX)
                self.monitor.connect(self.monitor_path)
                break
            except OSError:
                time.sleep(0.1)
        os.set_blocking(self.qemu.stdout.fileno(), False)
        if not self.wait_for("VM: started", 60):
            sys.exit(f"La VM n'a pas démarré :\n{self.log[-2000:]}")
        time.sleep(1.5)

    def pump(self, duration=0.0):
        end = time.monotonic() + duration
        while True:
            left = max(0, end - time.monotonic())
            ready, _, _ = select.select([self.qemu.stdout], [], [], left)
            if ready:
                data = self.qemu.stdout.read()
                if data:
                    self.log += data.decode(errors="replace")
            if time.monotonic() >= end:
                return

    def wait_for(self, text, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump(0.1)
            if text in self.log:
                return True
        return False

    def active(self):
        self.pump(0.1)
        found = re.findall(r"VM: active=(tty\d+)", self.log)
        return found[-1] if found else None

    def hmp(self, command):
        self.monitor.sendall(command.encode() + b"\n")
        time.sleep(0.1)

    def key(self, name, settle=0.08):
        self.hmp(f"sendkey {name}")
        time.sleep(settle)

    def type(self, text):
        for char in text:
            self.key(char)
        self.key("ret")

    def switch(self, number):
        self.key(f"alt-f{number}", 0.6)
        return self.active()

    def serial(self, command):
        self.qemu.stdin.write(command)
        self.qemu.stdin.flush()
        self.pump(0.6)

    def shot(self, name):
        os.makedirs(SCREENS, exist_ok=True)
        path = os.path.join(SCREENS, name + ".png")
        self.hmp(f"screendump {path} -f png")
        time.sleep(0.5)

    def close(self):
        self.qemu.kill()
        self.qemu.wait()
        self.monitor.close()


def scenario_all(root, initramfs):
    vm = Vm(root, initramfs, "-a")
    try:
        vm.shot("all-locked")
        check("-a : démarre sur tty1", vm.active() == "tty1", vm.log[-400:])
        check("-a : Alt-F2 refusé", vm.switch(2) == "tty1")
        vm.serial(b"C")
        check("-a : VT_ACTIVATE par root refusé aussi", vm.active() == "tty1", vm.log[-200:])
        vm.type("mauvais")
        vm.shot("all-checking")
        vm.pump(3)
        vm.shot("all-wrong-password")
        check("-a : un mauvais mot de passe ne déverrouille pas", "VM: exit" not in vm.log)
        vm.serial(b"L")
        check("-a : sans webcam, le verrou tient et le note", "no-photo:no-camera" in vm.log, vm.log[-300:])
        check("-a : Alt-F2 toujours refusé après un échec", vm.switch(2) == "tty1")
        vm.type("presque")
        vm.pump(3)
        vm.key("t")
        vm.key("e")
        vm.key("s")
        vm.shot("all-typing")
        vm.key("ctrl-u")
        vm.type(PASSWORD)
        check("-a : le vrai pam_unix accepte le bon mot de passe", vm.wait_for("VM: exit=0", 8), vm.log[-300:])
        vm.pump(0.5)
        vm.shot("all-unlocked")
        check("-a : Alt-F2 refonctionne après le déverrouillage", vm.switch(2) == "tty2")
    finally:
        vm.close()


def scenario_current(root, initramfs):
    vm = Vm(root, initramfs, "none")
    try:
        vm.shot("current-locked")
        check("classique : Alt-F2 reste permis", vm.switch(2) == "tty2")
        check("classique : retour sur tty1", vm.switch(1) == "tty1")
        check("classique : toujours verrouillé au retour", "VM: exit" not in vm.log)
        vm.type("mauvais")
        vm.pump(3)
        vm.serial(b"L")
        check("classique : la caméra n'est pas utilisée", "wrong-password no-photo:not-requested" in vm.log, vm.log[-300:])
        vm.type(PASSWORD)
        check("classique : déverrouille", vm.wait_for("VM: exit=0", 8), vm.log[-300:])
    finally:
        vm.close()


def scenario_photo(root, initramfs):
    vm = Vm(root, initramfs, "-a,--camera=/dev/video0")
    try:
        check("photo : Alt-F2 refusé", vm.switch(2) == "tty1")
        vm.pump(0.5)
        vm.shot("photo-console-switch")
        vm.serial(b"L")
        check("photo : le changement de console est noté avec sa photo",
              re.search(r"console-switch /tmp/state/vkeep/\S+\.jpg", vm.log) is not None, vm.log[-300:])
        vm.type(PASSWORD)
        check("photo : déverrouille", vm.wait_for("VM: exit=0", 8), vm.log[-300:])
        vm.pump(0.5)
        vm.shot("photo-summary")
    finally:
        vm.close()


def scenario_killed(root, initramfs):
    vm = Vm(root, initramfs, "-a")
    try:
        check("mort du processus : Alt-F2 refusé avant", vm.switch(2) == "tty1")
        vm.serial(b"K")
        check("mort du processus : SIGKILL reçu", vm.wait_for("signal=9", 3), vm.log[-200:])
        check("mort du processus : la console n'est pas figée", vm.switch(2) == "tty2")
    finally:
        vm.close()


def main():
    if not os.access(KERNEL, os.R_OK) or shutil.which("qemu-system-x86_64") is None:
        sys.exit(f"QEMU ou {KERNEL} indisponible")
    with tempfile.TemporaryDirectory() as root:
        initramfs = build_initramfs(root)
        scenario_all(root, initramfs)
        scenario_current(root, initramfs)
        scenario_photo(root, initramfs)
        scenario_killed(root, initramfs)
    print(f"\n{len(failures)} échec(s)" if failures else "\nTous les tests passent")
    print(f"Captures : {SCREENS}")
    sys.exit(1 if failures else 0)


main()
