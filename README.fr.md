# Super Mario 64 pour PSP

🌐 [English](README.md) · [Español](README.es.md) · **Français** · [Deutsch](README.de.md) · [日本語](README.ja.md)

Un portage natif de **Super Mario 64** pour la **Sony PSP**, disponible pour les versions **US, japonaise (JP), européenne (EU) et Shindou (SH)**, basé sur la décompilation
[sm64-port](https://github.com/sm64-port/sm64-port) / [n64decomp/sm64](https://github.com/n64decomp/sm64)
et sur le portage PSP de [z2442](https://github.com/z2442/sm64-port).

> Ce document est une traduction du [README en anglais](README.md), qui reste la référence. Les sections très techniques (sortie TV expérimentale, développement) y sont détaillées ; ici elles sont résumées.
> La traduction n'a pas été relue par un francophone natif : les corrections sont les bienvenues.

**Ce que cette version apporte**

- Choix de **30 ou 60 FPS**, avec des options de cadence d'images (frame pacing).
- L'audio est généré par le **Media Engine** de la PSP (le second processeur), ce qui libère le processeur principal pour le jeu.
- Un **menu d'options dans le jeu** (ouvrez-le depuis l'écran de pause) avec trois pages : **Graphismes**, **Audio** (volume de la musique, des effets et mode sonore) et **Commandes** (reconfiguration de chaque bouton).
- Tous les réglages sont dans **un seul fichier texte lisible**, `sm64config.txt`, que vous pouvez aussi modifier à la main.
- La progression est sauvegardée sur le Memory Stick comme un jeu PSP normal.
- **Quatre versions à partir du même code** : US, JP, EU (anglais, français et allemand, avec la mise en page PAL adaptée à l'écran 16:9 de la PSP) et Shindou (expérimentale). Voir [Versions prises en charge](#versions-prises-en-charge).

> **Ce dépôt ne contient pas les données du jeu.** Il vous faut **votre propre copie de la ROM de Super Mario 64** de la version souhaitée,
> que le processus de compilation transforme en version PSP. Aucune ROM n'est incluse et aucune ne sera fournie.
> Ceci est un projet de fans, non affilié à Nintendo.

---

## Sommaire

1. [Ce dont vous avez besoin](#1-ce-dont-vous-avez-besoin)
2. [Le compiler sous Windows, pas à pas](#2-le-compiler-sous-windows-pas-à-pas)
3. [L'installer sur votre PSP (ou l'ouvrir dans PPSSPP)](#3-linstaller-sur-votre-psp-ou-louvrir-dans-ppsspp)
4. [Jouer : commandes, menu d'options et sauvegardes](#4-jouer--commandes-menu-doptions-et-sauvegardes)
5. [Dépannage](#5-dépannage)
6. [Compiler sous Linux / WSL](#6-compiler-sous-linux--wsl)
7. [Pour les développeurs](#7-pour-les-développeurs)
8. [Crédits](#8-crédits)

---

## 1. Ce dont vous avez besoin

| Élément | Détails |
|---|---|
| **Un PC** | Windows 10 ou 11, 64 bits, environ **10 Go d'espace disque libre** et une connexion internet (uniquement pour la première compilation). |
| **Une ROM de Super Mario 64** | La version que vous voulez compiler, au format **`.z64`** (8 Mo, « big-endian »). Le nom de fichier exact et le SHA-1 de chaque version figurent dans le tableau ci-dessous. |
| **Docker Desktop** | Gratuit pour un usage personnel. Il fournit tout le compilateur PSP, vous n'avez donc **pas** à installer d'outils de développement. |
| **Une PSP avec firmware personnalisé** | Par exemple ARK-4 ou PRO CFW, et un Memory Stick. *Ou* l'émulateur [PPSSPP](https://www.ppsspp.org/). |

La seule chose à installer sur le PC est Docker Desktop. Tout le reste (compilateur, Python, bibliothèques) se trouve dans un conteneur Docker créé par le script de compilation.

**PSP-1000 (24 Mo de RAM) :** le jeu est conçu pour tenir dans la mémoire d'une PSP-1000 ; il démarre et fonctionne dans PPSSPP configuré en modèle 24 Mo avec environ 2 Mo de tas disponibles. Il **n'a pas encore été testé sur une vraie PSP-1000** : considérez-le comme expérimental sur ce modèle. La sortie TV n'est pas disponible sur PSP-1000.

### Versions prises en charge

| Version | Fichier(s) à fournir | SHA-1 de la ROM | État |
|---|---|---|---|
| **US** (États-Unis) | `baserom.us.z64` | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` | Version principale, la plus testée. |
| **JP** (Japon) | `baserom.jp.z64` | `8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51` | Compile ; moins testée. |
| **EU** (Europe) | `baserom.eu.z64` | `4ac5721683d0e0b6bbb561b58a71740845dceea9` | Anglais, français et allemand. Testée sur PSP et PPSSPP. |
| **SH** (Shindou, Japon 1997) | `baserom.sh.z64` **et** `baserom.jp.z64` | `3f319ae697533a255a1003d09202379d78d5a2e0` (SH) | **Expérimentale.** Les graphismes et le jeu viennent de la ROM SH, mais le son vient encore de la ROM JP : l'audio n'est donc pas encore celui de la vraie version Shindou. |

Chaque compilation est un jeu à part : la version que vous compilez est celle que vous obtenez. Renommez votre ROM exactement comme indiqué (tout en minuscules).

**Limitations connues**

- La version Shindou nécessite **les deux** ROM (SH et JP) et utilise les données sonores et le moteur audio japonais (le moteur sonore Shindou d'origine n'est pas encore porté). La vibration n'est pas disponible (la PSP n'a pas de moteur de vibration).
- Toutes les versions partagent le même dossier et le même nom de fichier de sauvegarde (`PSP\SAVEDATA\SM64PORT\sm64_save_file.bin`). La compatibilité des sauvegardes entre versions n'est pas garantie : **sauvegardez ce fichier avant de passer à une autre version**.

---

## 2. Le compiler sous Windows, pas à pas

### Étape 1 — Vérifiez votre ROM

(L'exemple utilise la ROM US ; pour une autre version, comparez avec son SHA-1 dans le [tableau](#versions-prises-en-charge).)

1. Assurez-vous que Windows affiche les **extensions de fichier** : dans l'Explorateur, ouvrez **Affichage → Afficher → Extensions de noms de fichiers**.
   (Si les extensions sont masquées, un fichier renommé en `baserom.us.z64` peut devenir `baserom.us.z64.z64` sans que vous le voyiez, et la compilation ne le trouvera pas.)
2. Clic droit sur le bouton Démarrer → **Terminal** (ou **Windows PowerShell**) et exécutez, en adaptant le chemin vers votre ROM :

   ```powershell
   Get-FileHash -Algorithm SHA1 "C:\chemin\vers\votre\rom.z64"
   ```

3. Le résultat doit être exactement `9BEF1128717F958171A4AFAC3ED78EE2BB4E86CE` (US).
   S'il est différent, votre ROM n'est pas de cette version, ou elle est dans un autre format (`.v64` / `.n64`). La compilation exige le format `.z64`.

### Étape 2 — Installez Docker Desktop

1. Téléchargez-le sur <https://www.docker.com/products/docker-desktop/> et lancez l'installateur.
   Laissez cochée l'option **« Use WSL 2 instead of Hyper-V »** (par défaut).
2. Redémarrez le PC si l'installateur le demande.
3. Ouvrez **Docker Desktop** depuis le menu Démarrer et acceptez les conditions. Attendez que le coin inférieur gauche affiche
   **« Engine running »** (en vert). **Docker Desktop doit être ouvert à chaque compilation.**
4. Pour vérifier, ouvrez PowerShell et exécutez :

   ```powershell
   docker --version
   ```

   Vous devriez voir quelque chose comme `Docker version 27.x.x`.

> Si Docker se plaint de la **virtualisation**, il faut l'activer dans le BIOS/UEFI du PC
> (cherchez « Intel VT-x », « SVM Mode » ou « Virtualization Technology »). Voir [Dépannage](#5-dépannage).

### Étape 3 — Téléchargez le projet

Le plus simple, sans programme supplémentaire :

1. Sur la page de ce dépôt, cliquez sur le bouton vert **Code** → **Download ZIP**.
2. Extrayez le ZIP dans un **dossier au chemin court, sans espaces ni accents**, par exemple `C:\sm64-psp`.
   Évitez les chemins très longs et les dossiers synchronisés par OneDrive.

*(Si vous utilisez déjà Git, `git clone https://github.com/Edwintendo/sm64-port-psp.git` fonctionne aussi.)*

### Étape 4 — Placez la ROM dans le dossier du projet

1. Copiez votre ROM dans le dossier du projet, **à côté du fichier `Makefile`**.
2. Renommez-la exactement en **`baserom.<version>.z64`** (tout en minuscules), où `<version>` vaut `us`, `jp`, `eu` ou `sh` : par exemple `baserom.us.z64`.
   La version Shindou nécessite deux fichiers : `baserom.sh.z64` et `baserom.jp.z64`.

Le dossier doit maintenant contenir, entre autres : `Makefile`, `build_psp.ps1`, `Dockerfile.psp`, `src` et votre ROM (`baserom.us.z64`, par exemple).

### Étape 5 — Lancez le script de compilation

1. Ouvrez le dossier du projet dans l'Explorateur, cliquez dans une zone vide de la barre d'adresse, tapez `powershell` et appuyez sur **Entrée**.
   (PowerShell s'ouvre alors directement dans le dossier. Sous Windows 11, vous pouvez aussi faire un clic droit dans une zone vide → **Ouvrir dans le Terminal**.)
2. Exécutez :

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1
   ```

   Cela compile la version **US**. Pour une autre version, ajoutez `-Version` :

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu
   ```

   (`us`, `jp`, `eu` ou `sh`.) Le `-ExecutionPolicy Bypass` autorise Windows à exécuter le script cette seule fois, sans modifier aucun réglage.

3. Patientez. La **première fois**, trois choses se passent et c'est long :
   1. Téléchargement du compilateur PSP et création d'une petite image Docker nommée `sm64psp` (quelques minutes, internet requis).
   2. Extraction des graphismes et des sons de votre ROM.
   3. Compilation du jeu (la partie la plus longue ; l'écran se remplit de texte, c'est normal).
4. À la fin, vous verrez :

   ```
   Listo: build\us_psp\mario64\  (copia EBOOT.PBP y SM64.PRX juntos a la misma carpeta de la PSP)
   ```

   (Le message est en espagnol ; le nom du dossier change avec la version : `build\eu_psp\mario64\`, `build\jp_psp\mario64\`, `build\sh_psp\mario64\`.)

   Les compilations suivantes sont bien plus rapides : seul ce que vous avez modifié est recompilé.

**Options du script**

| Commande | Effet |
|---|---|
| `.\build_psp.ps1 -Version eu` | Choisit la version : `us` (par défaut), `jp`, `eu` ou `sh`. |
| `.\build_psp.ps1 -Clean` | Supprime d'abord la compilation précédente de cette version. À utiliser si quelque chose est cassé ou bloqué. |
| `.\build_psp.ps1 -Opt O2` | Compile avec `-O2` au lieu de `-O3` (dossier séparé : `build_O2\`). |
| `.\build_psp.ps1 -Rebuild` | Reconstruit l'image Docker (utile seulement si `Dockerfile.psp` change). |

(N'oubliez pas de faire précéder la commande de `powershell -ExecutionPolicy Bypass -File`, par exemple
`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu -Clean`.)

### Étape 6 — Trouvez le résultat

Le jeu est la paire de fichiers **`EBOOT.PBP`** et **`SM64.PRX`** dans **`build\<version>_psp\mario64\`** (par exemple `build\us_psp\mario64\`). Gardez-les toujours ensemble dans le même dossier : `EBOOT.PBP` est un minuscule chargeur et `SM64.PRX` est le jeu lui-même (cela accélère nettement le démarrage sur une vraie PSP).

---

## 3. L'installer sur votre PSP (ou l'ouvrir dans PPSSPP)

### Sur une vraie PSP

1. Connectez la PSP au PC par câble USB et activez **Connexion USB** depuis le XMB, *ou* retirez le Memory Stick et placez-le dans un lecteur de cartes.
2. Sur le Memory Stick, ouvrez le dossier **`PSP`**, puis **`GAME`**.
3. Créez un dossier **`SM64`** (utilisez un autre nom, par ex. `SM64_EU`, si vous installez plusieurs versions) et copiez-y **les deux** fichiers `EBOOT.PBP` et `SM64.PRX`, de sorte que les chemins soient :

   ```
   PSP\GAME\SM64\EBOOT.PBP
   PSP\GAME\SM64\SM64.PRX
   ```

4. Déconnectez la PSP. Dans le XMB, allez dans **Jeu → Memory Stick** : vous y trouverez **Super Mario 64**.

> Votre PSP a besoin d'un **firmware personnalisé** (ARK-4, PRO, ME, …) pour lancer des homebrews. Ce portage a été développé et testé sur une PSP avec **ARK-4 à 333 MHz**.
> Sur une **PSP Go**, copiez plutôt le dossier dans le dossier `PSP\GAME` du stockage interne.

### Sans PC : installer depuis une release

Si une release est publiée pour votre version (GitHub → *Releases*), vous n'avez rien à compiler ni Docker à installer. La release ne contient aucune donnée du jeu : elle se prépare toute seule à partir de **votre propre ROM** au premier lancement.

1. Téléchargez le zip de votre version (`SM64-PSP-us.zip`, `-jp`, `-eu` ou `-sh`), extrayez-le et copiez le dossier **`PSP`** à la racine de votre Memory Stick (fusionnez les dossiers si on vous le demande).
2. Placez votre propre ROM dans `PSP\GAME\SM64_<VERSION>\data\` (n'importe quel nom de fichier ; `.z64`, `.v64` ou `.n64`). La version Shindou nécessite la ROM SH et la ROM JP.
3. Lancez **Super Mario 64** depuis le XMB. La première fois, il vérifie votre ROM et construit le jeu (une barre de progression s'affiche et cela peut prendre un moment), puis il démarre. Les fois suivantes, il démarre directement ; la ROM, `SM64.template.prx` et `manifest.bin` ne sont alors plus nécessaires et peuvent être supprimés.

L'installateur affiche ses messages dans la langue de votre PSP (anglais, français, allemand, espagnol, italien ou japonais). S'il indique que la ROM est absente, vérifiez que c'est bien la ROM originale de 8 Mo de cette version (SHA-1 dans le [tableau ci-dessus](#versions-prises-en-charge)). L'installateur n'a été testé que dans un émulateur pour l'instant ; les retours depuis de vraies PSP sont les bienvenus.

### Facultatif : sortie TV (expérimentale)

Le jeu peut envoyer son image vers un téléviseur via le câble vidéo de la PSP. Cela ne fonctionne que sur **PSP-2000 / 3000** et utilise un petit module auxiliaire déjà **inclus dans `SM64.PRX`** (rien d'autre à copier). Ouvrez **Options → Graphismes → Réglages TV** (la page ne s'ouvre que lorsqu'un câble vidéo est branché) et réglez **TV Output** sur **240p**, **480i** ou **480p** (**480p** exige un câble *composante* ; **240p** et **480i** fonctionnent aussi avec un câble composite/AV). Cette fonction est **expérimentale** : elle a été testée sur une vraie PSP-3000 avec un câble composante ; **le 480p n'a pas été testé** (le téléviseur utilisé ne le gère pas), pas plus que la sortie composite/AV. Si l'écran reste noir, relancez le jeu **en maintenant L** : cela ignore le module TV et utilise l'écran de la PSP. Tous les détails (cadrage, overscan, décalage vertical, limites, son avec le câble, bouton HOME) sont dans le [README en anglais](README.md#optional-tv-output-experimental).

### Dans PPSSPP (PC / Android)

Ouvrez PPSSPP et utilisez **Load…** pour choisir `EBOOT.PBP` (gardez `SM64.PRX` à côté), ou glissez le fichier sur la fenêtre de PPSSPP.

---

## 4. Jouer : commandes, menu d'options et sauvegardes

### Commandes par défaut

| PSP | Fonction |
|---|---|
| **Stick analogique** | Déplacer Mario |
| **✕ Croix** | A (saut) |
| **□ Carré** | B (coup de poing / plongeon) |
| **○ Rond** ou **L** | Z (s'accroupir / écrasement) |
| **R** | R (mode caméra) |
| **△ Triangle** | Bouton « L » de la N64 (le jeu ne l'utilise pas) |
| **Croix directionnelle** | Boutons C (caméra ; Haut = vue à la première personne) |
| **Start** | Pause |

Vous pouvez tout modifier dans le menu d'options.

### Le menu d'options

1. Appuyez sur **Start** pour mettre le jeu en pause.
2. Appuyez sur **R** (l'écran de pause affiche « **R Options** » en bas à droite).

| Boutons | Action |
|---|---|
| Croix directionnelle / stick **haut–bas** | Passer d'une option à l'autre |
| Croix directionnelle **gauche–droite** ou **✕** | Changer la valeur / entrer dans une page |
| **□** ou **○** | Retour |
| **Start** ou **R** | Fermer le menu (et tout enregistrer) |

**Pages**

- **Graphics** — FPS Limit (30 / 60), Frame Pacing (*Keep FPS* / *Keep Speed*), Object Distance et Far Distance (100 %–300 %), Object Culling (*Original* / *Vertical*), FPS Counter, Show Sound CPU, **TV Settings** (sous-page, câble vidéo requis), Reset to Defaults.
- **Audio** — Music Volume et Effects Volume, de 0 à 100 (100 = volume d'origine du jeu), et Sound Mode : Stereo, Mono ou Headset. Le Sound Mode est le même réglage que celui de l'écran de sélection de fichier (il est enregistré dans la sauvegarde : le changer à un endroit le change aux deux).
- **Controls** — choisissez un bouton et appuyez sur **✕** ; l'écran affiche « Press a button » : appuyez sur le bouton PSP voulu (il attend environ 5 secondes).
  Si ce bouton servait déjà à une autre action, les deux sont échangés. Également : Stick Deadzone et Reset to Defaults.

(Le menu est en anglais.)

### Où tout est enregistré

Tout est stocké sur le Memory Stick dans `PSP\SAVEDATA\SM64PORT\` :

| Fichier | Contenu |
|---|---|
| `sm64_save_file.bin` | Votre progression (étoiles, pièces…) |
| `sm64config.txt` | Tous vos réglages (graphismes, audio, commandes) en texte brut |
| `freeze.txt` | Seulement après un blocage : un petit rapport de ce que faisait le jeu (voir Dépannage) |

`sm64config.txt` s'ouvre avec le Bloc-notes. Chaque ligne est de la forme `nom = valeur`. Pour repartir de zéro, supprimez le fichier (ou utilisez *Reset to Defaults*).

---

## 5. Dépannage

**`docker` n'est pas reconnu / « The term 'docker' is not recognized »**
Docker Desktop n'est pas installé, ou PowerShell était ouvert avant l'installation. Installez-le, **fermez PowerShell et ouvrez-en un nouveau**.

**« error during connect… » / « Cannot connect to the Docker daemon » / « docker daemon is not running »**
Ouvrez **Docker Desktop** et attendez **« Engine running »**, puis réessayez.

**Docker Desktop indique que WSL 2 ou la virtualisation n'est pas disponible**
- Exécutez `wsl --install` dans un PowerShell administrateur et redémarrez.
- Activez la virtualisation dans le BIOS/UEFI (Intel VT-x / AMD SVM).

**« running scripts is disabled on this system »**
Lancez le script exactement ainsi : `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1`.

**`Failed to open baserom.us.z64!`** (ou la même chose avec `jp`, `eu`, `sh`)
La ROM n'est pas au bon endroit ou n'a pas le bon nom. Elle doit se trouver **à côté de `Makefile`** et s'appeler exactement `baserom.<version>.z64`
(avec les extensions visibles, vérifiez que ce n'est pas `baserom.us.z64.z64`). Pour Shindou il faut `baserom.sh.z64` **et** `baserom.jp.z64`.

**`baserom.us.z64 has the wrong hash!`** (idem avec `jp`, `eu` ou `sh`)
Ce n'est pas la ROM de cette version, ou elle est dans un autre format. Refaites l'[Étape 1](#étape-1--vérifiez-votre-rom).

**La compilation s'arrête sur une erreur, ou vous avez modifié quelque chose et le comportement est étrange**
Relancez avec `-Clean` : `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Clean`.

**« No space left on device »**
Libérez de l'espace disque (Docker a besoin de quelques Go). Dans Docker Desktop vous pouvez aussi utiliser *Troubleshoot → Clean / Purge data*.

**La compilation est très lente**
La première l'est toujours (voir l'étape 5). Gardez le projet sur un disque normal (pas un lecteur réseau) et fermez les programmes gourmands.
Dans Docker Desktop → Settings → Resources, vous pouvez lui donner plus de CPU.

**Le jeu n'apparaît pas dans le XMB**
Vérifiez que les chemins sont exactement `PSP\GAME\SM64\EBOOT.PBP` et `PSP\GAME\SM64\SM64.PRX` (les deux fichiers, même dossier) et que votre PSP a un firmware personnalisé. Si un écran de chargement affiche une erreur, `SM64.PRX` est absent ou dans un autre dossier. Recopiez le fichier s'il est corrompu.

**L'écran reste noir après avoir activé TV Output**
Relancez le jeu **en maintenant L** : cela ignore le module TV et utilise l'écran de la PSP. Si la TV a été activée et n'a pas tenu environ 5 secondes, le jeu remet aussi TV Output sur *Off* au lancement suivant.

**Le jeu se fige**
Si le jeu cesse d'afficher des images pendant environ 4 secondes, un petit chien de garde écrit `freeze.txt` dans `PSP\SAVEDATA\SM64PORT\` (et le réécrit toutes les 4 s tant que le blocage dure, jusqu'à 12 s). Il indique où se trouvait le thread principal, ce qu'attend chaque thread, l'état du GPU et du processeur audio, le niveau / l'action de Mario, ainsi que le niveau et la tension de la batterie. Joignez ce fichier à votre issue. Si la console se bloque au point qu'aucun code ne s'exécute, aucun fichier ne peut être écrit : dans ce cas, l'*absence* de `freeze.txt` est elle-même une information utile.

**Autre problème**
Ouvrez une *Issue* sur la page GitHub du dépôt et joignez les 20 à 30 dernières lignes de la sortie de compilation.

---

## 6. Compiler sous Linux / WSL

Vous pouvez utiliser la même image Docker, depuis un terminal Linux (ou dans WSL avec Docker lancé), à la racine du projet avec votre ROM (`baserom.us.z64`, par exemple) en place. Remplacez `VERSION=us` par `jp`, `eu` ou `sh` pour les autres versions :

```bash
docker build -t sm64psp - < Dockerfile.psp
docker run --rm -v "$PWD:/sm64" -w /sm64 sm64psp make TARGET_PSP=1 pbp VERSION=us -j$(nproc)
```

Le résultat est de nouveau `build/<version>_psp/mario64/` avec `EBOOT.PBP` + `SM64.PRX`. (Sous Linux, les fichiers créés par Docker appartiennent à `root` ; corrigez avec `sudo chown -R $USER .` si besoin.)

**Sans Docker** : installez la [chaîne d'outils PSP (pspdev)](https://github.com/pspdev/pspdev), ainsi que `python3` et les paquets de développement `build-essential`/`audiofile`, puis :

```bash
make TARGET_PSP=1 VERSION=us -j4        # construit l'ELF/PRX
make TARGET_PSP=1 VERSION=us pbp        # construit build/us_psp/mario64/{EBOOT.PBP,SM64.PRX}
```

L'audio passe par le Media Engine avec `libme-core` ; aucun PRX sonore externe n'est nécessaire à côté de l'EBOOT.

---

## 7. Pour les développeurs

Les détails (profilage avec gprof, autres cibles héritées du projet d'origine, structure du projet) sont dans le [README en anglais](README.md#7-for-developers).

Créer un paquet de release (installation sans PC) : `make TARGET_PSP=1 release VERSION=us -j8` (ou `.\build_psp.ps1 -Version us -Release`) ; l'installateur est dans `psp/installer/`. Voir `tools/psp/README.md`.

Les pull requests sont les bienvenues. Pour les changements importants, ouvrez d'abord une issue pour en discuter.

---

## 8. Crédits

**Le jeu et la décompilation**

- **Nintendo** — Super Mario 64 (tous les droits leur appartiennent).
- **n64decomp/sm64** et tous ses contributeurs — la décompilation sur laquelle tout repose.
- **sm64-port** — la version portable (PC) de la décompilation. Son `enhancements/60fps.patch` est à l'origine du système d'interpolation d'images utilisé pour les 60 fps ; ce dépôt l'adapte à la PSP.
- **Emill et MaikelChan** — le moteur de rendu Fast3D en C de `src/pc/gfx/` (sa licence est dans `src/pc/gfx/LICENSE.txt`), sur lequel repose la couche graphique PSP.
- **queueRAM** (sm64tools), **David Benepe** (n64graphics) et **Silicon Graphics** (compilateur IDO) — outils inclus dans `tools/`, chacun sous sa propre licence.

**Le portage PSP**

- **Hayden Kowalchuk (HaydenKow)** — le portage PSP d'origine (2020) : d'après l'historique Git, le premier backend graphique PSP (sceGu), les commandes, le gestionnaire de textures et le système de compilation PSP dont tout le reste découle.
- **z2442** — a poursuivi le portage PSP (transformation et découpage des sommets avec la VFPU, audio sur le Media Engine) et possède le dépôt sur lequel ce projet est basé.
- **demmis98** — optimisations et corrections PSP dans le dépôt de z2442.
- **Le portage d'Ocarina of Time de z2442 (oot-PSP) et Daedalus** — le module auxiliaire de sortie TV (`psp/dvemgr`) est adapté de celui du portage OoT, qui suit lui-même la méthode de sortie vidéo de l'émulateur N64 Daedalus pour PSP.
- **ghurone / Sonic-R-PSP** — les idées de l'arène de sommets en cache et de laisser le GE faire le découpage latéral, ainsi que l'indice sur les options du compilateur, viennent de leur portage de Sonic R pour PSP.
- **Claude (Anthropic)** — assistant d'IA qui a écrit ou aidé à écrire la majeure partie des changements de code récents de ce portage : l'adaptation à 60 fps et la cadence d'images, les optimisations de rendu et du GE, le menu d'options, la prise en charge des versions JP/EU/SH et la documentation.
- Toutes les autres personnes qui ont contribué au projet PSP et toutes celles qui figurent dans l'historique Git de ce dépôt (`git shortlog -sn`).

**Outils utilisés**

- **pspdev** (PSPSDK et chaîne d'outils PSP) pour compiler, et **PPSSPP** pour tester.

Les données propres au jeu (modèles, textures, musique, sons) ne sont **pas** distribuées ici : elles sont extraites de la ROM que vous fournissez.
Le dépôt inclut en revanche quelques fichiers dérivés de Nintendo dont la PSP a besoin pour afficher le jeu dans son menu (`psp/EBOOT/` : icône, image de fond et son). Ils ne sont pas couverts par la licence de ce projet et appartiennent à leurs propriétaires ; si vous êtes titulaire des droits et souhaitez leur retrait, ouvrez une issue.
