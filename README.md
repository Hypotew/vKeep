# vKeep

[![CI](https://github.com/Hypotew/vKeep/actions/workflows/ci.yml/badge.svg)](https://github.com/Hypotew/vKeep/actions/workflows/ci.yml)
[![Licence : MIT](https://img.shields.io/badge/licence-MIT-blue.svg)](LICENSE)

**Le verrou de console Linux qui vous dit qui a essayé d'entrer.**

vKeep protège vos consoles le temps de votre absence. Dans la lignée de `vlock`, il reste simple à
utiliser, avec un écran soigné en plus. Et si quelqu'un tente sa chance, il le note et prend sa photo.

![L'écran de verrouillage de vKeep](docs/screenshot.png)

## Fonctionnalités

- **Toutes vos consoles protégées** : avec `-a`, impossible de passer sur une autre console virtuelle.
- **Photo des tentatives** : avec `-a`, chaque mauvais mot de passe et chaque changement de console
  refusé déclenche une photo, sur une webcam détectée automatiquement.
- **Bilan au retour** : dès le déverrouillage, vous voyez ce qui s'est passé pendant votre absence.
- **Un écran agréable à regarder** : logo en dégradé, grande horloge, et un affichage qui s'adapte à la
  console comme aux terminaux graphiques.
- **Aucun droit particulier** : vKeep tourne avec votre compte et vérifie le mot de passe via PAM,
  le système d'authentification standard de Linux.
- **Un verrou qui ne dépend pas de la caméra** : webcam absente ou en panne, il reste actif et
  se déverrouille normalement.

## Installation

Il vous faut Linux, un compilateur C++20 et les en-têtes de PAM (`libpam0g-dev` sur Debian et Ubuntu,
déjà inclus dans le paquet `pam` sur Arch). `ffmpeg` est facultatif : il sert uniquement aux photos.

```sh
git clone https://github.com/Hypotew/vKeep.git
cd vKeep
make
sudo make install
```

`sudo make uninstall` retire vKeep de la machine.

## Utilisation

| Commande | Effet |
|---|---|
| `vkeep` | Verrouille la console courante |
| `vkeep -a` | Verrouille toutes les consoles virtuelles et photographie les tentatives |
| `vkeep -a --camera=/dev/video2` | Pareil, avec la webcam de votre choix |

Pour déverrouiller, saisissez le mot de passe de votre compte. `vkeep -a` doit être lancé depuis une
console virtuelle (`Ctrl-Alt-F3`, par exemple), pas depuis un terminal graphique.

## Vos données

Les photos et le journal `attempts.log` restent sur votre machine, dans `~/.local/state/vkeep/`, un
dossier accessible à vous seul. vKeep ne les envoie nulle part et ne les supprime jamais : à vous de
faire le tri.

Photographier une personne à son insu peut être illégal. Prévenez les personnes concernées, et
renseignez-vous sur les règles en vigueur avant d'utiliser `-a` ailleurs que chez vous.

## Ce que vKeep ne protège pas

Comme tout verrou logiciel, vKeep a ses limites :

- **La session graphique** : vKeep protège les consoles. Dans un terminal graphique, seule cette
  fenêtre est verrouillée, et le reste de votre bureau reste accessible.
- **L'accès physique** : quelqu'un devant la machine peut toujours l'éteindre ou la redémarrer.
  Ça n'ouvre pas votre session, mais le travail non enregistré est perdu, sans photo ni trace.
- **Les raccourcis du noyau** : selon votre configuration, `Ctrl-Alt-Suppr` peut redémarrer la machine,
  et `SysRq` peut arrêter vKeep. Pour un verrou plus solide, gardez `SysRq` limité (`kernel.sysrq=16`)
  et désactivez le raccourci avec `sudo systemctl mask ctrl-alt-del.target`.
- **Votre propre compte** : un programme lancé sous votre compte, par exemple via SSH, peut arrêter vKeep.

## Développement

```sh
make test       # tests dans un pseudo-terminal
make coverage   # les mêmes, avec échec sous 80 % de lignes couvertes (nécessite uv)
make vm-test    # vraies consoles dans une VM QEMU
```

## Licence

Distribué sous licence [MIT](LICENSE).
