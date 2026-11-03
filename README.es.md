# Super Mario 64 para PSP

🇬🇧 [Read it in English](README.md)

Port nativo de **Super Mario 64** (versión US) para la **Sony PSP**, basado en la decompilación
[sm64-port](https://github.com/sm64-port/sm64-port) / [n64decomp/sm64](https://github.com/n64decomp/sm64)
y en el port a PSP de [z2442](https://github.com/z2442/sm64-port).

**Qué añade esta versión**

- Límite de **30 o 60 FPS**, con opciones de ritmo de fotogramas (frame pacing).
- El audio lo genera el **Media Engine** de la PSP (el segundo procesador), así el procesador principal queda libre para el juego.
- **Menú de opciones dentro del juego** (se abre desde la pausa) con tres páginas: **Graphics** (gráficos), **Audio** (volumen de música y de efectos) y **Controls** (reasignar cada botón).
- Todos los ajustes se guardan en **un solo archivo de texto legible**, `sm64config.txt`, que también puedes editar a mano.
- El progreso se guarda en el Memory Stick como cualquier juego de PSP.

> **Este repositorio NO incluye los datos del juego.** Necesitas **tu propia copia de la ROM de Super Mario 64 (US)**;
> el proceso de compilación la convierte en la versión de PSP. No se incluye ninguna ROM ni se va a facilitar.
> Es un proyecto de fans, sin relación con Nintendo.

---

## Índice

1. [Qué necesitas](#1-qué-necesitas)
2. [Compilarlo en Windows, paso a paso](#2-compilarlo-en-windows-paso-a-paso)
3. [Ponerlo en tu PSP (o abrirlo en PPSSPP)](#3-ponerlo-en-tu-psp-o-abrirlo-en-ppsspp)
4. [Jugar: controles, menú de opciones y partidas](#4-jugar-controles-menú-de-opciones-y-partidas)
5. [Solución de problemas](#5-solución-de-problemas)
6. [Compilar en Linux / WSL](#6-compilar-en-linux--wsl)
7. [Para desarrolladores](#7-para-desarrolladores)
8. [Créditos](#8-créditos)

---

## 1. Qué necesitas

| Cosa | Detalle |
|---|---|
| **Un PC** | Windows 10 u 11 de 64 bits, unos **10 GB libres** en disco e internet (solo para la primera compilación). |
| **Una ROM de Super Mario 64** | Versión **US**, en formato **`.z64`** (8 MB, "big-endian"). Su SHA-1 debe ser `9bef1128717f958171a4afac3ed78ee2bb4e86ce`. |
| **Docker Desktop** | Gratis para uso personal. Trae todo el compilador de PSP, así que **no** tienes que instalar ninguna herramienta de programación. |
| **Una PSP con firmware personalizado** | Por ejemplo ARK-4 o PRO CFW, y un Memory Stick. *O bien* el emulador [PPSSPP](https://www.ppsspp.org/). |

Lo único que instalas en el PC es Docker Desktop. Todo lo demás (compilador, Python, librerías) vive dentro de un contenedor Docker
que crea el script de compilación por ti.

---

## 2. Compilarlo en Windows, paso a paso

### Paso 1 — Comprueba tu ROM

1. Haz que Windows **muestre las extensiones**: en el Explorador de archivos ve a **Ver → Mostrar → Extensiones de nombre de archivo**.
   (Con las extensiones ocultas, un archivo que renombras a `baserom.us.z64` puede quedar como `baserom.us.z64.z64` sin que lo notes, y la compilación no lo encontrará.)
2. Clic derecho en el botón Inicio → **Terminal** (o **Windows PowerShell**) y ejecuta, cambiando la ruta por la de tu ROM:

   ```powershell
   Get-FileHash -Algorithm SHA1 "C:\ruta\a\tu\rom.z64"
   ```

3. El resultado debe ser exactamente `9BEF1128717F958171A4AFAC3ED78EE2BB4E86CE`.
   Si es distinto, tu ROM no es la versión US, o está en otro formato (`.v64` / `.n64`). La compilación necesita el formato `.z64`.

### Paso 2 — Instala Docker Desktop

1. Descárgalo de <https://www.docker.com/products/docker-desktop/> y ejecuta el instalador.
   Deja marcada la opción **"Use WSL 2 instead of Hyper-V"** (viene así por defecto).
2. Reinicia el PC si el instalador te lo pide.
3. Abre **Docker Desktop** desde el menú Inicio y acepta los términos. Espera a que abajo a la izquierda ponga
   **"Engine running"** (en verde). **Docker Desktop tiene que estar abierto cada vez que compiles.**
4. Para comprobar que funciona, abre PowerShell y ejecuta:

   ```powershell
   docker --version
   ```

   Debe salir algo como `Docker version 27.x.x`.

> Si Docker se queja de la **virtualización**, hay que activarla en la BIOS/UEFI de tu PC
> (busca "Intel VT-x", "SVM Mode" o "Virtualization Technology" y actívala). Mira la [solución de problemas](#5-solución-de-problemas).

### Paso 3 — Descarga el proyecto

La forma más fácil, sin programas extra:

1. En la página de este repositorio pulsa el botón verde **Code** → **Download ZIP**.
2. Extrae el ZIP en una **carpeta corta, sin espacios ni tildes**, por ejemplo `C:\sm64-psp`.
   Evita rutas muy largas y carpetas sincronizadas con OneDrive.

*(Si ya usas Git, también sirve `git clone https://github.com/Edwintendo/sm64-port-psp.git`.)*

### Paso 4 — Pon la ROM en la carpeta del proyecto

1. Copia tu ROM dentro de la carpeta del proyecto, **junto al archivo `Makefile`**.
2. Renómbrala exactamente como **`baserom.us.z64`** (todo en minúsculas).

La carpeta debe contener, entre otros: `Makefile`, `build_psp.ps1`, `Dockerfile.psp`, `src` y `baserom.us.z64`.

### Paso 5 — Ejecuta el script de compilación

1. Abre la carpeta del proyecto en el Explorador, haz clic en la barra de direcciones (en una zona vacía), escribe `powershell` y pulsa **Enter**.
   (Eso abre PowerShell ya dentro de la carpeta. En Windows 11 también puedes hacer clic derecho en una zona vacía → **Abrir en Terminal**.)
2. Ejecuta:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1
   ```

   (La parte `-ExecutionPolicy Bypass` permite a Windows ejecutar el script solo esta vez, sin cambiar ningún ajuste.)

3. Espera. La **primera vez** hace tres cosas y tarda, así que ten paciencia:
   1. Descarga el compilador de PSP y crea una imagen Docker pequeña llamada `sm64psp` (unos minutos, necesita internet).
   2. Extrae los gráficos y sonidos de tu ROM.
   3. Compila el juego (es lo que más tarda; la pantalla se llena de texto, es normal).
4. Al terminar verás:

   ```
   Listo: build\us_psp\mario64\  (copia EBOOT.PBP y SM64.PRX juntos a la misma carpeta de la PSP)
   ```

   Las siguientes compilaciones son mucho más rápidas: solo se recompila lo que cambiaste.

**Opciones del script (opcionales)**

| Comando | Qué hace |
|---|---|
| `.\build_psp.ps1 -Clean` | Borra antes la compilación anterior. Úsalo si algo quedó roto o trabado. |
| `.\build_psp.ps1 -Opt O2` | Compila con `-O2` en vez de `-O3` (en otra carpeta: `build_O2\`). |
| `.\build_psp.ps1 -Rebuild` | Vuelve a construir la imagen de Docker (solo si cambia `Dockerfile.psp`). |

(Recuerda poner delante `powershell -ExecutionPolicy Bypass -File` si las usas, por ejemplo:
`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Clean`.)

### Paso 6 — Encuentra el resultado

El juego son los dos archivos **`EBOOT.PBP`** y **`SM64.PRX`**, dentro de **`build\us_psp\mario64\`**. Guárdalos siempre juntos en la misma carpeta: `EBOOT.PBP` es un cargador diminuto y `SM64.PRX` es el juego en sí (así arranca mucho más rápido en una PSP real).

---

## 3. Ponerlo en tu PSP (o abrirlo en PPSSPP)

### En una PSP real

1. Conecta la PSP al PC con el cable USB y activa **Conexión USB** desde el XMB, *o* saca el Memory Stick y ponlo en un lector de tarjetas.
2. En el Memory Stick abre la carpeta **`PSP`** y luego **`GAME`**.
3. Crea una carpeta nueva llamada **`SM64`** y copia dentro **los dos** archivos, `EBOOT.PBP` y `SM64.PRX`, de modo que las rutas sean:

   ```
   PSP\GAME\SM64\EBOOT.PBP
   PSP\GAME\SM64\SM64.PRX
   ```

4. Desconecta la PSP. En el XMB ve a **Juego → Memory Stick** y ahí estará **Super Mario 64**.

> Tu PSP necesita **firmware personalizado** (ARK-4, PRO, ME, …) para ejecutar homebrew. Este port se desarrolló y probó en una PSP con **ARK-4 a 333 MHz**.
> En una **PSP Go**, copia la carpeta en el `PSP\GAME` del almacenamiento interno.

### En PPSSPP (PC / Android)

Abre PPSSPP y usa **Cargar…** para elegir `EBOOT.PBP` (con `SM64.PRX` al lado), o arrastra el archivo a la ventana de PPSSPP.

---

## 4. Jugar: controles, menú de opciones y partidas

### Controles por defecto

| PSP | Hace |
|---|---|
| **Stick analógico** | Mover a Mario |
| **✕ Cruz** | A (saltar) |
| **□ Cuadrado** | B (puñetazo / lanzarse) |
| **○ Círculo** o **L** | Z (agacharse / golpe en picada) |
| **R** | R (modo de cámara) |
| **△ Triángulo** | Botón "L" del N64 (el juego no lo usa) |
| **Cruceta** | Botones C (cámara; Arriba = vista en primera persona) |
| **Start** | Pausa |

Todo esto se puede cambiar en el menú de opciones.

### El menú de opciones

1. Pulsa **Start** para pausar el juego.
2. Pulsa **R** (la pantalla de pausa muestra "**R Options**" abajo a la derecha).

| Botones | Acción |
|---|---|
| Cruceta / stick **arriba–abajo** | Moverse entre opciones |
| Cruceta **izquierda–derecha** o **✕** | Cambiar el valor / entrar a una página |
| **□** o **○** | Volver |
| **Start** o **R** | Cerrar el menú (y guardar todo) |

**Páginas**

- **Graphics** — FPS Limit (30 / 60), Frame Pacing (*Keep FPS* / *Keep Speed*), Object Distance y Far Distance (100 %–300 % o Unlimited),
  Object Culling (*Original* / *Vertical*), FPS Counter, Show Sound CPU, Reset to Defaults.
- **Audio** — Music Volume y Effects Volume, de 0 a 100 (100 es el volumen original del juego).
- **Controls** — elige un botón y pulsa **✕**; la pantalla dice "Press a button": pulsa el botón de la PSP que quieras (espera unos 5 segundos).
  Si ese botón ya lo usaba otra acción, las dos se intercambian. También Stick Deadzone y Reset to Defaults.

### Dónde se guarda todo

Todo va al Memory Stick, en `PSP\SAVEDATA\SM64PORT\`:

| Archivo | Contenido |
|---|---|
| `sm64_save_file.bin` | Tu progreso (estrellas, monedas…) |
| `sm64config.txt` | Todos tus ajustes (gráficos, audio, controles) en texto plano |

`sm64config.txt` se puede abrir con el Bloc de notas. Cada línea es `nombre = valor`. Para empezar de cero, borra el archivo (o usa *Reset to Defaults*).

---

## 5. Solución de problemas

**`docker` no se reconoce / "The term 'docker' is not recognized"**
Docker Desktop no está instalado, o PowerShell estaba abierto antes de instalarlo. Instálalo, **cierra PowerShell y abre uno nuevo**.

**"error during connect…" / "Cannot connect to the Docker daemon" / "docker daemon is not running"**
Abre **Docker Desktop** y espera a que ponga **"Engine running"**; luego repite.

**Docker Desktop dice que WSL 2 o la virtualización no está disponible**
- Ejecuta `wsl --install` en un PowerShell de administrador y reinicia.
- Activa la virtualización en la BIOS/UEFI (Intel VT-x / AMD SVM).

**"running scripts is disabled on this system" (la ejecución de scripts está deshabilitada)**
Lanza el script exactamente así: `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1`.

**`Failed to open baserom.us.z64!`**
La ROM no está en el sitio correcto o tiene otro nombre. Debe estar **junto al `Makefile`** y llamarse exactamente `baserom.us.z64`
(con las extensiones visibles, comprueba que no sea `baserom.us.z64.z64`).

**`baserom.us.z64 has the wrong hash!`**
No es la ROM US, o está en otro formato. Repite el [Paso 1](#paso-1--comprueba-tu-rom).

**La compilación se detiene con un error a la mitad, o cambiaste algo y se comporta raro**
Repítela con `-Clean`: `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Clean`.

**"No space left on device" (sin espacio)**
Libera espacio en disco (Docker necesita unos GB). En Docker Desktop también puedes usar *Troubleshoot → Clean / Purge data*.

**La compilación es muy lenta**
La primera siempre lo es (mira el Paso 5). Deja el proyecto en un disco normal (no de red) y cierra programas pesados.
En Docker Desktop → Settings → Resources puedes darle más CPUs.

**El juego no aparece en el XMB**
Revisa que las rutas sean exactamente `PSP\GAME\SM64\EBOOT.PBP` y `PSP\GAME\SM64\SM64.PRX` (los dos archivos, en la misma carpeta) y que tu PSP tenga firmware personalizado. Si ves una pantalla "Cargando el juego…" con un error, falta `SM64.PRX` o está en otra carpeta. Vuelve a copiar el archivo por si se corrompió.

**Cualquier otra cosa**
Abre un *Issue* en la página de GitHub del repositorio e incluye las últimas 20–30 líneas de la salida de la compilación.

---

## 6. Compilar en Linux / WSL

Puedes usar la misma imagen de Docker desde una terminal de Linux (o dentro de WSL con Docker funcionando), en la raíz del proyecto y con `baserom.us.z64` en su sitio:

```bash
docker build -t sm64psp - < Dockerfile.psp
docker run --rm -v "$PWD:/sm64" -w /sm64 sm64psp make TARGET_PSP=1 pbp VERSION=us -j$(nproc)
```

El resultado vuelve a ser `build/us_psp/mario64/` (`EBOOT.PBP` + `SM64.PRX`). (En Linux los archivos que crea Docker pertenecen a `root`; si te estorba, arréglalo con `sudo chown -R $USER .`.)

**Sin Docker**: instala el [toolchain de PSP (pspdev)](https://github.com/pspdev/pspdev), más `python3` y los paquetes de desarrollo `build-essential`/`audiofile`, y luego:

```bash
make TARGET_PSP=1 VERSION=us -j4        # compila el ELF/PRX
make TARGET_PSP=1 VERSION=us pbp        # genera build/us_psp/mario64/{EBOOT.PBP,SM64.PRX}
```

El audio se genera con el Media Engine mediante `libme-core`; no hace falta ningún PRX de sonido junto al EBOOT.

---

## 7. Para desarrolladores

### Perfilado con gprof

1. Compila con perfilado: `make TARGET_PSP=1 PROFILE_PSP=1` (añade `pbp` para generar el EBOOT).
2. Ejecútalo en la PSP o en el emulador. `gmon.out` se escribe al salir del juego con normalidad.
3. Copia `gmon.out` de vuelta y analízalo: `gprof build/us_psp/sm64.us gmon.out > profile_report.txt`

El perfilado añade sobrecarga. Con él activado, el PBP usa el ELF directamente en vez de un PRX.

### Otros destinos (sin cambios respecto al proyecto original, sin mantenimiento en este fork)

El proyecto original también puede compilar ejecutables nativos para **Linux** y **Windows (MSYS2)**, y ROMs de N64.
Mira [sm64-port/sm64-port](https://github.com/sm64-port/sm64-port) y [n64decomp/sm64](https://github.com/n64decomp/sm64) para esas instrucciones.

### Estructura del proyecto

```
sm64
├── actors: comportamientos de objetos, geo layout y display lists
├── asm: código ensamblador escrito a mano, cabecera de la ROM
├── assets: datos de animaciones y demos
├── bin: archivos C para ordenar display lists y texturas
├── build: carpeta de salida
├── data: scripts de comportamiento, datos varios
├── enhancements: ejemplos de modificaciones del código
├── include: archivos de cabecera
├── levels: scripts de niveles, geo layout y display lists
├── lib: código de la librería SDK
├── psp: icono, fondo y sonido del EBOOT, texturas específicas de PSP
├── rsp: ensamblador RSP de audio y Fast3D
├── sound: secuencias, muestras de sonido y bancos de sonido
├── src: código fuente C del juego
│   ├── audio: código de audio
│   ├── buffers: pilas, heaps y buffers de tareas
│   ├── engine: motores de scripts y utilidades
│   ├── game: comportamientos y resto del juego (psp_options.c = el menú de opciones)
│   ├── goddard: pantalla de intro con la cara de Mario
│   ├── menu: pantalla de título y menús de archivo, actos y selección de nivel de depuración
│   └── pc: código del port, audio y renderizador (PSP: gfx_psp.c, audio_psp.c, psp_me.c)
├── text: diálogos, nombres de niveles y de actos
├── textures: texturas de cielos y genéricas
└── tools: herramientas de compilación
```

### Contribuir

Los pull requests son bienvenidos. Para cambios grandes, abre antes un issue para comentar qué te gustaría cambiar.

---

## 8. Créditos

- **Nintendo** — Super Mario 64 (todos los derechos son suyos).
- **n64decomp/sm64** — la decompilación sobre la que se apoya todo.
- **sm64-port** — la versión portable (PC) de la decompilación.
- **z2442** y colaboradores — el port original a PSP del que sale este repositorio.
- Todos los que han colaborado en el proyecto PSP (Discord: <https://discord.gg/5w4B69>).

Los datos propios del juego (modelos, texturas, música, sonidos) **no** se distribuyen aquí: se extraen de la ROM que tú aportas.
El repositorio sí incluye unos pocos archivos derivados de Nintendo que la PSP necesita para mostrar el juego en su menú (`psp/EBOOT/`: icono, imagen de fondo y sonido) y unas cuantas texturas específicas de PSP (`psp/textures/`). No están cubiertos por la licencia de este proyecto y pertenecen a sus dueños; si eres titular de los derechos y quieres que se retiren, abre un issue.
