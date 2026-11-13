# Super Mario 64 für PSP

🌐 [English](README.md) · [Español](README.es.md) · [Français](README.fr.md) · **Deutsch** · [日本語](README.ja.md)

Eine native Portierung von **Super Mario 64** für die **Sony PSP**, verfügbar für die Versionen **US, japanisch (JP), europäisch (EU) und Shindou (SH)**, basierend auf der Dekompilierung
[sm64-port](https://github.com/sm64-port/sm64-port) / [n64decomp/sm64](https://github.com/n64decomp/sm64)
und auf der PSP-Portierung von [z2442](https://github.com/z2442/sm64-port).

> Dieses Dokument ist eine Übersetzung der [englischen README](README.md), die maßgeblich bleibt. Sehr technische Abschnitte (experimenteller TV-Ausgang, Entwicklung) sind dort ausführlich beschrieben; hier sind sie zusammengefasst.
> Die Übersetzung wurde nicht von Muttersprachlern geprüft – Korrekturen sind willkommen.

**Was diese Version bietet**

- Wahl zwischen **30 oder 60 FPS**, mit Optionen für das Frame-Pacing.
- Der Ton wird von der **Media Engine** der PSP (der zweiten CPU) erzeugt, wodurch die Haupt-CPU für das Spiel frei bleibt.
- Ein **Optionsmenü im Spiel** (aus dem Pausenbildschirm zu öffnen) mit drei Seiten: **Grafik**, **Audio** (Musiklautstärke, Effektlautstärke und Tonmodus) und **Steuerung** (jede Taste frei belegbar).
- Alle Einstellungen stehen in **einer lesbaren Textdatei**, `sm64config.txt`, die Sie auch von Hand bearbeiten können.
- Der Spielstand wird wie bei jedem normalen PSP-Spiel auf dem Memory Stick gespeichert.
- **Vier Versionen aus demselben Code**: US, JP, EU (Englisch, Französisch und Deutsch, mit an den 16:9-Bildschirm der PSP angepasstem PAL-Layout) und Shindou (experimentell). Siehe [Unterstützte Versionen](#unterstützte-versionen).

> **Dieses Repository enthält keine Spieldaten.** Sie benötigen **Ihre eigene Kopie der Super-Mario-64-ROM** der gewünschten Version,
> die der Build-Prozess in die PSP-Version umwandelt. Es ist keine ROM enthalten, und es wird auch keine bereitgestellt.
> Dies ist ein Fan-Projekt und steht in keiner Verbindung zu Nintendo.

---

## Inhaltsverzeichnis

1. [Was Sie brauchen](#1-was-sie-brauchen)
2. [Unter Windows bauen, Schritt für Schritt](#2-unter-windows-bauen-schritt-für-schritt)
3. [Auf der PSP installieren (oder in PPSSPP öffnen)](#3-auf-der-psp-installieren-oder-in-ppsspp-öffnen)
4. [Spielen: Steuerung, Optionsmenü und Spielstände](#4-spielen-steuerung-optionsmenü-und-spielstände)
5. [Problemlösung](#5-problemlösung)
6. [Unter Linux / WSL bauen](#6-unter-linux--wsl-bauen)
7. [Für Entwickler](#7-für-entwickler)
8. [Danksagungen](#8-danksagungen)

---

## 1. Was Sie brauchen

| Was | Details |
|---|---|
| **Ein PC** | Windows 10 oder 11, 64 Bit, etwa **10 GB freier Speicherplatz** und eine Internetverbindung (nur für den ersten Build). |
| **Eine Super-Mario-64-ROM** | Die Version, die Sie bauen möchten, im Format **`.z64`** (8 MB, „Big Endian“). Den genauen Dateinamen und die SHA-1 jeder Version finden Sie in der Tabelle unten. |
| **Docker Desktop** | Für den privaten Gebrauch kostenlos. Es liefert den gesamten PSP-Compiler, Sie müssen also **keine** Entwicklungswerkzeuge installieren. |
| **Eine PSP mit Custom Firmware** | Zum Beispiel ARK-4 oder PRO CFW, und ein Memory Stick. *Oder* der Emulator [PPSSPP](https://www.ppsspp.org/). |

Auf dem PC installieren Sie nur Docker Desktop. Alles andere (Compiler, Python, Bibliotheken) liegt in einem Docker-Container, den das Build-Skript für Sie erstellt.

**PSP-1000 (24 MB RAM):** Das Spiel ist darauf ausgelegt, in den Speicher einer PSP-1000 zu passen; es startet und läuft in PPSSPP, das auf ein 24-MB-Modell eingestellt ist, mit etwa 2 MB freiem Heap. Es wurde **noch nicht auf einer echten PSP-1000 getestet** – betrachten Sie es dort als experimentell. Der TV-Ausgang ist auf der PSP-1000 nicht verfügbar.

### Unterstützte Versionen

| Version | Bereitzustellende Datei(en) | SHA-1 der ROM | Status |
|---|---|---|---|
| **US** (USA) | `baserom.us.z64` | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` | Hauptversion, am besten getestet. |
| **JP** (Japan) | `baserom.jp.z64` | `8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51` | Lässt sich bauen; weniger getestet. |
| **EU** (Europa) | `baserom.eu.z64` | `4ac5721683d0e0b6bbb561b58a71740845dceea9` | Englisch, Französisch und Deutsch. Auf PSP und in PPSSPP getestet. |
| **SH** (Shindou, Japan 1997) | `baserom.sh.z64` **und** `baserom.jp.z64` | `3f319ae697533a255a1003d09202379d78d5a2e0` (SH) | **Experimentell.** Grafik und Spiel stammen aus der SH-ROM, der Ton aber noch aus der JP-ROM; der Ton ist also noch nicht der echte Shindou-Ton. |

Jeder Build ist ein eigenes Spiel: Die Version, die Sie bauen, ist die Version, die Sie erhalten. Benennen Sie Ihre ROM genau wie angegeben (alles klein geschrieben).

**Bekannte Einschränkungen**

- Die Shindou-Version benötigt **beide** ROMs (SH und JP) und verwendet die japanischen Sounddaten und die japanische Audio-Engine (die ursprüngliche Shindou-Sound-Engine ist noch nicht portiert). Rumble ist nicht verfügbar (die PSP hat keinen Vibrationsmotor).
- Alle Versionen verwenden denselben Speicherordner und Dateinamen (`PSP\SAVEDATA\SM64PORT\sm64_save_file.bin`). Die Kompatibilität der Spielstände zwischen Versionen ist nicht garantiert: **Sichern Sie diese Datei, bevor Sie zu einer anderen Version wechseln.**

---

## 2. Unter Windows bauen, Schritt für Schritt

### Schritt 1 — ROM prüfen

(Das Beispiel nutzt die US-ROM; bei einer anderen Version vergleichen Sie mit deren SHA-1 aus der [Tabelle](#unterstützte-versionen).)

1. Stellen Sie sicher, dass Windows die **Dateiendungen** anzeigt: Im Explorer **Ansicht → Anzeigen → Dateinamenerweiterungen** öffnen.
   (Sind Endungen ausgeblendet, wird eine in `baserom.us.z64` umbenannte Datei unbemerkt zu `baserom.us.z64.z64`, und der Build findet sie nicht.)
2. Rechtsklick auf die Start-Schaltfläche → **Terminal** (oder **Windows PowerShell**) und ausführen, mit dem Pfad zu Ihrer ROM:

   ```powershell
   Get-FileHash -Algorithm SHA1 "C:\Pfad\zu\Ihrer\rom.z64"
   ```

3. Das Ergebnis muss genau `9BEF1128717F958171A4AFAC3ED78EE2BB4E86CE` lauten (US).
   Weicht es ab, ist Ihre ROM nicht von dieser Version oder in einem anderen Format (`.v64` / `.n64`). Der Build benötigt das Format `.z64`.

### Schritt 2 — Docker Desktop installieren

1. Laden Sie es von <https://www.docker.com/products/docker-desktop/> herunter und starten Sie das Installationsprogramm.
   Lassen Sie die Option **„Use WSL 2 instead of Hyper-V“** aktiviert (Standard).
2. Starten Sie den PC neu, falls das Installationsprogramm dazu auffordert.
3. Öffnen Sie **Docker Desktop** über das Startmenü und akzeptieren Sie die Bedingungen. Warten Sie, bis unten links
   **„Engine running“** (grün) steht. **Docker Desktop muss bei jedem Build geöffnet sein.**
4. Zur Kontrolle öffnen Sie PowerShell und führen aus:

   ```powershell
   docker --version
   ```

   Es sollte etwa `Docker version 27.x.x` erscheinen.

> Beschwert sich Docker über **Virtualisierung**, muss sie im BIOS/UEFI des PCs aktiviert werden
> (suchen Sie nach „Intel VT-x“, „SVM Mode“ oder „Virtualization Technology“). Siehe [Problemlösung](#5-problemlösung).

### Schritt 3 — Projekt herunterladen

Am einfachsten, ohne zusätzliche Programme:

1. Klicken Sie auf der Seite dieses Repositorys auf den grünen Button **Code** → **Download ZIP**.
2. Entpacken Sie die ZIP-Datei in einen **kurzen Ordner ohne Leerzeichen oder Umlaute**, zum Beispiel `C:\sm64-psp`.
   Vermeiden Sie sehr lange Pfade und von OneDrive synchronisierte Ordner.

*(Wenn Sie bereits Git benutzen, funktioniert auch `git clone https://github.com/Edwintendo/sm64-port-psp.git`.)*

### Schritt 4 — ROM in den Projektordner legen

1. Kopieren Sie Ihre ROM in den Projektordner, **neben die Datei `Makefile`**.
2. Benennen Sie sie genau in **`baserom.<version>.z64`** um (alles klein), wobei `<version>` `us`, `jp`, `eu` oder `sh` ist: zum Beispiel `baserom.us.z64`.
   Die Shindou-Version benötigt zwei Dateien: `baserom.sh.z64` und `baserom.jp.z64`.

Der Ordner sollte nun unter anderem enthalten: `Makefile`, `build_psp.ps1`, `Dockerfile.psp`, `src` und Ihre ROM (zum Beispiel `baserom.us.z64`).

### Schritt 5 — Build-Skript ausführen

1. Öffnen Sie den Projektordner im Explorer, klicken Sie in einen leeren Bereich der Adressleiste, tippen Sie `powershell` und drücken Sie **Enter**.
   (So öffnet sich PowerShell direkt im Ordner. Unter Windows 11 geht auch: Rechtsklick in einen leeren Bereich → **Im Terminal öffnen**.)
2. Führen Sie aus:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1
   ```

   Das baut die **US**-Version. Für eine andere Version fügen Sie `-Version` hinzu:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu
   ```

   (`us`, `jp`, `eu` oder `sh`.) `-ExecutionPolicy Bypass` erlaubt Windows nur diesmal, das Skript auszuführen, ohne eine Einstellung zu ändern.

3. Warten Sie. Beim **ersten Mal** passieren drei Dinge, und es dauert lange – haben Sie Geduld:
   1. Der PSP-Compiler wird heruntergeladen und ein kleines Docker-Image namens `sm64psp` gebaut (einige Minuten, Internet nötig).
   2. Grafiken und Töne werden aus Ihrer ROM extrahiert.
   3. Das Spiel wird kompiliert (der längste Teil; der Bildschirm füllt sich mit Text, das ist normal).
4. Am Ende sehen Sie:

   ```
   Listo: build\us_psp\mario64\  (copia EBOOT.PBP y SM64.PRX juntos a la misma carpeta de la PSP)
   ```

   (Der Text ist spanisch; der Ordnername ändert sich je nach Version: `build\eu_psp\mario64\`, `build\jp_psp\mario64\`, `build\sh_psp\mario64\`.)

   Spätere Builds sind deutlich schneller: Nur das Geänderte wird neu kompiliert.

**Optionale Skript-Schalter**

| Befehl | Wirkung |
|---|---|
| `.\build_psp.ps1 -Version eu` | Wählt die Version: `us` (Standard), `jp`, `eu` oder `sh`. |
| `.\build_psp.ps1 -Clean` | Löscht zuerst den vorherigen Build dieser Version. Nutzen Sie das, wenn etwas kaputt ist oder hängt. |
| `.\build_psp.ps1 -Opt O2` | Baut mit `-O2` statt `-O3` (eigener Ordner: `build_O2\`). |
| `.\build_psp.ps1 -Rebuild` | Baut das Docker-Image neu (nur nötig, wenn sich `Dockerfile.psp` ändert). |

(Denken Sie daran, `powershell -ExecutionPolicy Bypass -File` voranzustellen, z. B.
`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu -Clean`.)

### Schritt 6 — Ergebnis finden

Das Spiel besteht aus den beiden Dateien **`EBOOT.PBP`** und **`SM64.PRX`** in **`build\<version>_psp\mario64\`** (zum Beispiel `build\us_psp\mario64\`). Halten Sie beide immer im selben Ordner zusammen: `EBOOT.PBP` ist ein winziger Lader und `SM64.PRX` ist das Spiel selbst (das lässt es auf einer echten PSP deutlich schneller starten).

---

## 3. Auf der PSP installieren (oder in PPSSPP öffnen)

### Auf einer echten PSP

1. Verbinden Sie die PSP per USB-Kabel mit dem PC und aktivieren Sie im XMB **USB-Verbindung**, *oder* nehmen Sie den Memory Stick heraus und stecken Sie ihn in einen Kartenleser.
2. Öffnen Sie auf dem Memory Stick den Ordner **`PSP`**, dann **`GAME`**.
3. Legen Sie einen neuen Ordner **`SM64`** an (bei mehreren Versionen einen anderen Namen, z. B. `SM64_EU`) und kopieren Sie **beide** Dateien `EBOOT.PBP` und `SM64.PRX` hinein, sodass die Pfade so lauten:

   ```
   PSP\GAME\SM64\EBOOT.PBP
   PSP\GAME\SM64\SM64.PRX
   ```

4. Trennen Sie die PSP. Gehen Sie im XMB zu **Spiel → Memory Stick**, dort finden Sie **Super Mario 64**.

> Ihre PSP braucht eine **Custom Firmware** (ARK-4, PRO, ME, …), um Homebrew auszuführen. Diese Portierung wurde auf einer PSP mit **ARK-4 bei 333 MHz** entwickelt und getestet.
> Bei einer **PSP Go** kopieren Sie den Ordner stattdessen in den Ordner `PSP\GAME` des internen Speichers.

### Ohne PC: aus einem Release installieren

Wenn für Ihre Version ein Release veröffentlicht ist (GitHub → *Releases*), müssen Sie nichts bauen und kein Docker installieren. Das Release enthält keine Spieldaten: Es bereitet sich beim ersten Start aus **Ihrer eigenen ROM** selbst vor.

1. Laden Sie das Zip Ihrer Version herunter (`SM64-PSP-us.zip`, `-jp`, `-eu` oder `-sh`), entpacken Sie es und kopieren Sie den Ordner **`PSP`** in das Hauptverzeichnis Ihres Memory Sticks (Ordner zusammenführen, falls gefragt).
2. Legen Sie Ihre eigene ROM in `PSP\GAME\SM64_<VERSION>\data\` ab (beliebiger Dateiname; `.z64`, `.v64` oder `.n64`). Die Shindou-Version benötigt die SH- und die JP-ROM.
3. Starten Sie **Super Mario 64** aus dem XMB. Beim ersten Mal prüft es Ihre ROM und baut das Spiel (ein Fortschrittsbalken wird angezeigt, und es kann eine Weile dauern), danach startet es. Bei den nächsten Malen startet es direkt; ROM, `SM64.template.prx` und `manifest.bin` werden dann nicht mehr gebraucht und können gelöscht werden.

Der Installer zeigt seine Meldungen in der Sprache Ihrer PSP an (Englisch, Französisch, Deutsch, Spanisch, Italienisch oder Japanisch). Meldet er, dass die ROM fehlt, prüfen Sie, ob es die originale 8-MB-ROM dieser Version ist (SHA-1 in der [Tabelle oben](#unterstützte-versionen)). Der Installer wurde auf einer echten PSP-3000 getestet; Rückmeldungen zu anderen Modellen (vor allem zur PSP-1000) sind willkommen.

### Optional: TV-Ausgang (experimentell)

Das Spiel kann sein Bild über das Videokabel der PSP an einen Fernseher senden. Das funktioniert nur auf der **PSP-2000 / 3000** und nutzt ein kleines Hilfsmodul, das bereits **in `SM64.PRX` enthalten** ist (es muss nichts weiter kopiert werden). Öffnen Sie **Options → Graphics → TV Settings** (die Seite öffnet sich nur bei angeschlossenem Videokabel) und stellen Sie **TV Output** auf **240p**, **480i** oder **480p** (**480p** erfordert ein *Component*-Kabel; **240p** und **480i** funktionieren auch mit einem Composite/AV-Kabel). Die Funktion ist **experimentell**: Sie wurde auf einer echten PSP-3000 mit Komponentenkabel getestet; **480p wurde nicht getestet** (der verwendete Fernseher unterstützt es nicht), ebenso wenig die Composite/AV-Ausgabe. Bleibt der Bildschirm schwarz, starten Sie das Spiel einfach neu: Hielt TV keine 5 Sekunden, stellt es TV Output beim nächsten Start selbst auf *Off*. Alle Details (Bildausschnitt, Overscan, vertikale Verschiebung, Grenzen, Ton mit Kabel, HOME-Taste) stehen in der [englischen README](README.md#optional-tv-output-experimental).

### In PPSSPP (PC / Android)

Öffnen Sie PPSSPP und wählen Sie mit **Load…** die Datei `EBOOT.PBP` (lassen Sie `SM64.PRX` daneben liegen), oder ziehen Sie die Datei auf das PPSSPP-Fenster.

---

## 4. Spielen: Steuerung, Optionsmenü und Spielstände

### Standardsteuerung

| PSP | Funktion |
|---|---|
| **Analogstick** | Mario bewegen |
| **✕ Kreuz** | A (Sprung) |
| **□ Quadrat** | B (Schlag / Hechtsprung) |
| **○ Kreis** oder **L** | Z (Ducken / Bodenstampfer) |
| **R** | R (Kameramodus) |
| **△ Dreieck** | „L“-Taste des N64 (das Spiel nutzt sie nicht) |
| **Steuerkreuz** | C-Tasten (Kamera; Hoch = Ich-Perspektive) |
| **Start** | Pause |

Alles lässt sich im Optionsmenü ändern.

### Das Optionsmenü

1. Drücken Sie **Start**, um das Spiel zu pausieren.
2. Drücken Sie **R** (der Pausenbildschirm zeigt unten rechts „**R Options**“).

| Tasten | Aktion |
|---|---|
| Steuerkreuz / Stick **hoch–runter** | Zwischen Optionen wechseln |
| Steuerkreuz **links–rechts** oder **✕** | Wert ändern / Seite öffnen |
| **□** oder **○** | Zurück |
| **Start** oder **R** | Menü schließen (und alles speichern) |

**Seiten**

- **Graphics** — FPS Limit (30 / 60), Frame Pacing (*Keep FPS* / *Keep Speed*), Object Distance und Far Distance (100 %–300 %), Object Culling (*Original* / *Vertical*), FPS Counter, Show Sound CPU, **TV Settings** (Unterseite, Videokabel nötig), Reset to Defaults.
- **Audio** — Music Volume und Effects Volume, von 0 bis 100 (100 ist die Originallautstärke des Spiels), und Sound Mode: Stereo, Mono oder Headset. Sound Mode ist dieselbe Einstellung wie im Dateiauswahlbildschirm (sie steht im Spielstand; eine Änderung an einer Stelle gilt für beide).
- **Controls** — Taste auswählen und **✕** drücken; der Bildschirm zeigt „Press a button“ – drücken Sie die gewünschte PSP-Taste (es wartet etwa 5 Sekunden).
  War diese Taste schon einer anderen Aktion zugeordnet, werden beide getauscht. Außerdem: Stick Deadzone und Reset to Defaults.

(Das Menü ist auf Englisch.)

### Wo alles gespeichert wird

Alles liegt auf dem Memory Stick in `PSP\SAVEDATA\SM64PORT\`:

| Datei | Inhalt |
|---|---|
| `sm64_save_file.bin` | Ihr Spielfortschritt (Sterne, Münzen …) |
| `sm64config.txt` | Alle Ihre Einstellungen (Grafik, Audio, Steuerung) als Klartext |
| `freeze.txt` | Nur nach einem Einfrieren: ein kleiner Bericht, was das Spiel gerade tat (siehe Problemlösung) |

`sm64config.txt` lässt sich mit dem Editor öffnen. Jede Zeile hat die Form `name = wert`. Um bei null anzufangen, löschen Sie die Datei (oder nutzen Sie *Reset to Defaults*).

---

## 5. Problemlösung

**`docker` wird nicht erkannt / „The term 'docker' is not recognized“**
Docker Desktop ist nicht installiert, oder PowerShell war schon vor der Installation geöffnet. Installieren Sie es, **schließen Sie PowerShell und öffnen Sie ein neues Fenster**.

**„error during connect…“ / „Cannot connect to the Docker daemon“ / „docker daemon is not running“**
Öffnen Sie **Docker Desktop**, warten Sie auf **„Engine running“** und versuchen Sie es erneut.

**Docker Desktop meldet, WSL 2 oder Virtualisierung sei nicht verfügbar**
- Führen Sie `wsl --install` in einer PowerShell mit Administratorrechten aus und starten Sie neu.
- Aktivieren Sie die Virtualisierung im BIOS/UEFI (Intel VT-x / AMD SVM).

**„running scripts is disabled on this system“**
Starten Sie das Skript genau so: `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1`.

**`Failed to open baserom.us.z64!`** (oder dasselbe mit `jp`, `eu`, `sh`)
Die ROM liegt nicht am richtigen Ort oder hat den falschen Namen. Sie muss **neben `Makefile`** liegen und genau `baserom.<version>.z64` heißen
(prüfen Sie bei sichtbaren Endungen, dass es nicht `baserom.us.z64.z64` ist). Für Shindou brauchen Sie `baserom.sh.z64` **und** `baserom.jp.z64`.

**`baserom.us.z64 has the wrong hash!`** (ebenso mit `jp`, `eu` oder `sh`)
Es ist nicht die ROM dieser Version oder sie hat ein anderes Format. Wiederholen Sie [Schritt 1](#schritt-1--rom-prüfen).

**Der Build bricht mit einem Fehler ab, oder nach einer Änderung verhält sich alles seltsam**
Starten Sie erneut mit `-Clean`: `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Clean`.

**„No space left on device“**
Geben Sie Speicherplatz frei (Docker braucht einige GB). In Docker Desktop können Sie auch *Troubleshoot → Clean / Purge data* nutzen.

**Der Build ist sehr langsam**
Der erste ist es immer (siehe Schritt 5). Halten Sie das Projekt auf einem normalen Laufwerk (kein Netzlaufwerk) und schließen Sie ressourcenhungrige Programme.
In Docker Desktop → Settings → Resources können Sie mehr CPUs zuweisen.

**Das Spiel erscheint nicht im XMB**
Prüfen Sie, dass die Pfade genau `PSP\GAME\SM64\EBOOT.PBP` und `PSP\GAME\SM64\SM64.PRX` lauten (beide Dateien, derselbe Ordner) und dass Ihre PSP eine Custom Firmware hat. Zeigt sich ein Ladebildschirm mit Fehler, fehlt `SM64.PRX` oder liegt in einem anderen Ordner. Kopieren Sie die Datei erneut, falls sie beschädigt ist.

**Der Bildschirm bleibt nach dem Einschalten von TV Output schwarz**
Starten Sie das Spiel erneut: Wurde TV eingeschaltet und hielt keine 5 Sekunden, stellt das Spiel TV Output beim nächsten Start selbst auf *Off* und nutzt den PSP-Bildschirm.

**Das Spiel friert ein**
Zeichnet das Spiel etwa 4 Sekunden lang keine Bilder mehr, schreibt ein kleiner Watchdog `freeze.txt` nach `PSP\SAVEDATA\SM64PORT\` (und schreibt sie alle 4 s neu, solange das Einfrieren anhält, bis 12 s). Sie listet, wo der Haupt-Thread war, worauf jeder Thread wartet, den Zustand von GPU und Audioprozessor, das Level / Marios Aktion sowie Akkustand und -spannung. Hängen Sie die Datei an Ihr Issue an. Hängt sich die ganze Konsole so auf, dass kein Code mehr läuft, kann keine Datei geschrieben werden: Dann ist das *Fehlen* von `freeze.txt` selbst eine nützliche Information.

**Alles andere**
Öffnen Sie ein *Issue* auf der GitHub-Seite des Repositorys und fügen Sie die letzten 20–30 Zeilen der Build-Ausgabe bei.

---

## 6. Unter Linux / WSL bauen

Sie können dasselbe Docker-Image von einem Linux-Terminal aus verwenden (oder in WSL mit laufendem Docker), im Projektstamm, mit Ihrer ROM (zum Beispiel `baserom.us.z64`) am Platz. Ersetzen Sie `VERSION=us` durch `jp`, `eu` oder `sh` für die anderen Versionen:

```bash
docker build -t sm64psp - < Dockerfile.psp
docker run --rm -v "$PWD:/sm64" -w /sm64 sm64psp make TARGET_PSP=1 pbp VERSION=us -j$(nproc)
```

Das Ergebnis liegt wieder in `build/<version>_psp/mario64/` mit `EBOOT.PBP` + `SM64.PRX`. (Unter Linux gehören von Docker erzeugte Dateien `root`; bei Bedarf mit `sudo chown -R $USER .` korrigieren.)

**Ohne Docker**: Installieren Sie die [PSP-Toolchain (pspdev)](https://github.com/pspdev/pspdev) sowie `python3` und die Entwicklungspakete `build-essential`/`audiofile`, dann:

```bash
make TARGET_PSP=1 VERSION=us -j4        # baut das ELF/PRX
make TARGET_PSP=1 VERSION=us pbp        # baut build/us_psp/mario64/{EBOOT.PBP,SM64.PRX}
```

Der Ton läuft über die Media Engine mit `libme-core`; neben dem EBOOT ist kein externes Sound-PRX nötig.

---

## 7. Für Entwickler

Die Details (Profiling mit gprof, weitere Ziele aus dem ursprünglichen Projekt, Projektstruktur) stehen in der [englischen README](README.md#7-for-developers).

Ein Release-Paket erstellen (Installation ohne PC): `make TARGET_PSP=1 release VERSION=us -j8` (oder `.\build_psp.ps1 -Version us -Release`); der Installer liegt in `psp/installer/`. Siehe `tools/psp/README.md`.

Pull Requests sind willkommen. Bei größeren Änderungen eröffnen Sie bitte zuerst ein Issue, um sie zu besprechen.

---

## 8. Danksagungen

**Das Spiel und die Dekompilierung**

- **Nintendo** — Super Mario 64 (alle Rechte liegen bei ihnen).
- **n64decomp/sm64** und alle Mitwirkenden — die Dekompilierung, auf der alles aufbaut.
- **sm64-port** — die portable (PC-)Version der Dekompilierung. Aus ihrem `enhancements/60fps.patch` stammt das Frame-Interpolationssystem für 60 fps; dieses Repository passt es an die PSP an.
- **Emill und MaikelChan** — der Fast3D-in-C-Renderer in `src/pc/gfx/` (Lizenz in `src/pc/gfx/LICENSE.txt`), auf dem die PSP-Grafikschicht aufbaut.
- **queueRAM** (sm64tools), **David Benepe** (n64graphics) und **Silicon Graphics** (IDO-Compiler) — in `tools/` enthaltene Werkzeuge, jeweils unter eigener Lizenz.

**Die PSP-Portierung**

- **Hayden Kowalchuk (HaydenKow)** — die ursprüngliche PSP-Portierung (2020): laut Git-Verlauf das erste PSP-Grafik-Backend (sceGu), die Steuerung, der Texturmanager und das PSP-Build-System, aus denen alles Weitere hervorging.
- **z2442** — führte die PSP-Portierung fort (VFPU-Vertextransformation und -Clipping, Audio auf der Media Engine) und besitzt das Repository, auf dem dieses Projekt basiert.
- **demmis98** — PSP-Optimierungen und Korrekturen im Repository von z2442.
- **z2442s Ocarina-of-Time-Portierung (oot-PSP) und Daedalus** — das Hilfsmodul für den TV-Ausgang (`psp/dvemgr`) ist von dem der OoT-Portierung übernommen, das wiederum der Videoausgabe-Methode des N64-Emulators Daedalus für die PSP folgt.
- **ghurone / Sonic-R-PSP** — die Ideen des zwischengespeicherten Vertex-Arenas und, das seitliche Clipping vom GE erledigen zu lassen, sowie der Hinweis zu den Compiler-Flags stammen aus ihrer Sonic-R-Portierung für die PSP.
- **beckerd ([psp_mk64_portable](https://github.com/beckerd/psp_mk64_portable)) und JustKidding23 ([Ocarina-of-Time-PSP](https://github.com/JustKidding23/Ocarina-of-Time-PSP), basierend auf [z2442/oot-PSP](https://github.com/z2442/oot-PSP))** — die Idee, keine Spieldaten auszuliefern und sie beim ersten Start auf der PSP aus der ROM des Spielers zu erzeugen, stammt von diesen beiden PSP-Portierungen (Mario Kart 64 und Ocarina of Time). Der Installer in diesem Repository (`psp/installer/`) ist eine eigenständige Implementierung: Er baut `SM64.PRX` aus einer Vorlage und einem Manifest neu auf.
- **sm64coopdx / sm64ex-coop** — Referenz für das Synchronisationsdesign des Multiplayers (in dieser Version deaktiviert) und Quelle der für diese Portierung angepassten Luigi-Assets: Modell und Texturen, HUD-Kopf, Sound-Banks und Sprachsamples. Die Luigi-Sprachausgaben werden dort Andrat zugeschrieben. sm64coopdx baut auf sm64ex-coop (djoslin0 und Mitwirkende) und DynOS (PeachyPeach) auf. Diese Assets gehören ihren jeweiligen Urhebern.
- **Claude (Anthropic)** — KI-Assistent, der den Großteil der späteren Codeänderungen dieser Portierung geschrieben oder mitgeschrieben hat: die 60-fps-Anpassung und das Frame-Pacing, die Render- und GE-Optimierungen, das Optionsmenü, die Unterstützung der Versionen JP/EU/SH und die Dokumentation.
- Alle anderen, die zum PSP-Projekt beigetragen haben, und alle im Git-Verlauf dieses Repositorys (`git shortlog -sn`).

**Verwendete Werkzeuge**

- **pspdev** (PSPSDK und PSP-Toolchain) zum Bauen und **PPSSPP** zum Testen.

Die eigentlichen Spieldaten (Modelle, Texturen, Musik, Töne) werden hier **nicht** verteilt: Sie werden aus der ROM extrahiert, die Sie bereitstellen.
Das Repository enthält allerdings einige von Nintendo abgeleitete Dateien, die die PSP braucht, um das Spiel im Menü anzuzeigen (`psp/EBOOT/`: Symbol, Hintergrundbild und Ton). Sie fallen nicht unter die Lizenz dieses Projekts und gehören ihren Eigentümern; wenn Sie Rechteinhaber sind und eine Entfernung wünschen, eröffnen Sie ein Issue.
