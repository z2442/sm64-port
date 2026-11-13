# Super Mario 64 para PSP

🌐 [English](README.md) · **Español** · [Français](README.fr.md) · [Deutsch](README.de.md) · [日本語](README.ja.md)

Port nativo de **Super Mario 64** para la **Sony PSP**, disponible para las versiones **US, japonesa (JP), europea (EU) y Shindou (SH)**, basado en la decompilación
[sm64-port](https://github.com/sm64-port/sm64-port) / [n64decomp/sm64](https://github.com/n64decomp/sm64)
y en el port a PSP de [z2442](https://github.com/z2442/sm64-port).

**Qué añade esta versión**

- Límite de **30 o 60 FPS**, con opciones de ritmo de fotogramas (frame pacing).
- El audio lo genera el **Media Engine** de la PSP (el segundo procesador), así el procesador principal queda libre para el juego.
- **Menú de opciones dentro del juego** (se abre desde la pausa) con tres páginas: **Graphics** (gráficos), **Audio** (volumen de música, de efectos y modo de sonido) y **Controls** (reasignar cada botón).
- Todos los ajustes se guardan en **un solo archivo de texto legible**, `sm64config.txt`, que también puedes editar a mano.
- El progreso se guarda en el Memory Stick como cualquier juego de PSP.
- **Cuatro versiones con el mismo código**: US, JP, EU (inglés, francés y alemán, con el formato PAL adaptado a la pantalla 16:9 de la PSP) y Shindou (experimental). Mira [Versiones compatibles](#versiones-compatibles).

> **Este repositorio NO incluye los datos del juego.** Necesitas **tu propia copia de la ROM de Super Mario 64** de la versión que quieras;
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
| **Una ROM de Super Mario 64** | La versión que quieras compilar, en formato **`.z64`** (8 MB, "big-endian"). El nombre exacto del archivo y el SHA-1 de cada versión están en la tabla de abajo. |
| **Docker Desktop** | Gratis para uso personal. Trae todo el compilador de PSP, así que **no** tienes que instalar ninguna herramienta de programación. |
| **Una PSP con firmware personalizado** | Por ejemplo ARK-4 o PRO CFW, y un Memory Stick. *O bien* el emulador [PPSSPP](https://www.ppsspp.org/). |

Lo único que instalas en el PC es Docker Desktop. Todo lo demás (compilador, Python, librerías) vive dentro de un contenedor Docker
que crea el script de compilación por ti.

**PSP-1000 (24 MB de RAM):** el juego está pensado para caber en la memoria de una PSP-1000 y arranca y corre en PPSSPP configurado como modelo de 24 MB con unos 2 MB de heap libres. **Todavía no se ha probado en una PSP-1000 real**, así que considéralo experimental ahí. La salida a TV no está disponible en una PSP-1000.

### Versiones compatibles

| Versión | Archivo(s) que debes aportar | SHA-1 de la ROM | Estado |
|---|---|---|---|
| **US** (EE. UU.) | `baserom.us.z64` | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` | Versión principal, la más probada. |
| **JP** (Japón) | `baserom.jp.z64` | `8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51` | Compila; menos probada. |
| **EU** (Europa) | `baserom.eu.z64` | `4ac5721683d0e0b6bbb561b58a71740845dceea9` | Inglés, francés y alemán. Probada en PSP y PPSSPP. |
| **SH** (Shindou, Japón 1997) | `baserom.sh.z64` **y** `baserom.jp.z64` | `3f319ae697533a255a1003d09202379d78d5a2e0` (SH) | **Experimental.** El juego y los gráficos salen de la ROM SH, pero el sonido todavía sale de la ROM JP, así que el audio aún no es el de Shindou real. |

Cada compilación es su propio juego: la versión que compilas es la que obtienes. Renombra tu ROM exactamente como se indica (todo en minúsculas).

**Limitaciones conocidas**

- Shindou necesita **las dos** ROM (SH y JP) y usa los datos y el motor de audio japoneses (el motor de sonido original de Shindou aún no está portado). No hay vibración (la PSP no tiene motor de vibración).
- Todas las versiones comparten la misma carpeta y nombre de partida (`PSP\SAVEDATA\SM64PORT\sm64_save_file.bin`). Las partidas de versiones distintas no tienen por qué ser compatibles, así que **haz una copia de ese archivo antes de cambiar de versión**.

---

## 2. Compilarlo en Windows, paso a paso

### Paso 1 — Comprueba tu ROM

(El ejemplo usa la ROM US; para otra versión compara con su SHA-1 de la [tabla](#versiones-compatibles).)

1. Haz que Windows **muestre las extensiones**: en el Explorador de archivos ve a **Ver → Mostrar → Extensiones de nombre de archivo**.
   (Con las extensiones ocultas, un archivo que renombras a `baserom.us.z64` puede quedar como `baserom.us.z64.z64` sin que lo notes, y la compilación no lo encontrará.)
2. Clic derecho en el botón Inicio → **Terminal** (o **Windows PowerShell**) y ejecuta, cambiando la ruta por la de tu ROM:

   ```powershell
   Get-FileHash -Algorithm SHA1 "C:\ruta\a\tu\rom.z64"
   ```

3. El resultado debe ser exactamente `9BEF1128717F958171A4AFAC3ED78EE2BB4E86CE` (US).
   Si es distinto, tu ROM no es esa versión, o está en otro formato (`.v64` / `.n64`). La compilación necesita el formato `.z64`.

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
2. Renómbrala exactamente como **`baserom.<versión>.z64`** (todo en minúsculas), donde `<versión>` es `us`, `jp`, `eu` o `sh`: por ejemplo `baserom.us.z64`.
   Shindou necesita dos archivos: `baserom.sh.z64` y `baserom.jp.z64`.

La carpeta debe contener, entre otros: `Makefile`, `build_psp.ps1`, `Dockerfile.psp`, `src` y tu ROM (por ejemplo `baserom.us.z64`).

### Paso 5 — Ejecuta el script de compilación

1. Abre la carpeta del proyecto en el Explorador, haz clic en la barra de direcciones (en una zona vacía), escribe `powershell` y pulsa **Enter**.
   (Eso abre PowerShell ya dentro de la carpeta. En Windows 11 también puedes hacer clic derecho en una zona vacía → **Abrir en Terminal**.)
2. Ejecuta:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1
   ```

   Eso compila la versión **US**. Para otra versión añade `-Version`:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu
   ```

   (`us`, `jp`, `eu` o `sh`.) La parte `-ExecutionPolicy Bypass` permite a Windows ejecutar el script solo esta vez, sin cambiar ningún ajuste.)

3. Espera. La **primera vez** hace tres cosas y tarda, así que ten paciencia:
   1. Descarga el compilador de PSP y crea una imagen Docker pequeña llamada `sm64psp` (unos minutos, necesita internet).
   2. Extrae los gráficos y sonidos de tu ROM.
   3. Compila el juego (es lo que más tarda; la pantalla se llena de texto, es normal).
4. Al terminar verás:

   ```
   Listo: build\us_psp\mario64\  (copia EBOOT.PBP y SM64.PRX juntos a la misma carpeta de la PSP)
   ```

   (El nombre de la carpeta cambia con la versión: `build\eu_psp\mario64\`, `build\jp_psp\mario64\`, `build\sh_psp\mario64\`.)

   Las siguientes compilaciones son mucho más rápidas: solo se recompila lo que cambiaste.

**Opciones del script (opcionales)**

| Comando | Qué hace |
|---|---|
| `.\build_psp.ps1 -Version eu` | Elige la versión: `us` (por defecto), `jp`, `eu` o `sh`. |
| `.\build_psp.ps1 -Clean` | Borra antes la compilación anterior de esa versión. Úsalo si algo quedó roto o trabado. |
| `.\build_psp.ps1 -Opt O2` | Compila con `-O2` en vez de `-O3` (en otra carpeta: `build_O2\`). |
| `.\build_psp.ps1 -Rebuild` | Vuelve a construir la imagen de Docker (solo si cambia `Dockerfile.psp`). |

(Recuerda poner delante `powershell -ExecutionPolicy Bypass -File` si las usas, por ejemplo:
`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu -Clean`.)

### Paso 6 — Encuentra el resultado

El juego son los dos archivos **`EBOOT.PBP`** y **`SM64.PRX`**, dentro de **`build\<versión>_psp\mario64\`** (por ejemplo `build\us_psp\mario64\`). Guárdalos siempre juntos en la misma carpeta: `EBOOT.PBP` es un cargador diminuto y `SM64.PRX` es el juego en sí (así arranca mucho más rápido en una PSP real).

---

## 3. Ponerlo en tu PSP (o abrirlo en PPSSPP)

### En una PSP real

1. Conecta la PSP al PC con el cable USB y activa **Conexión USB** desde el XMB, *o* saca el Memory Stick y ponlo en un lector de tarjetas.
2. En el Memory Stick abre la carpeta **`PSP`** y luego **`GAME`**.
3. Crea una carpeta nueva llamada **`SM64`** (usa otro nombre, p. ej. `SM64_EU`, si instalas varias versiones) y copia dentro **los dos** archivos, `EBOOT.PBP` y `SM64.PRX`, de modo que las rutas sean:

   ```
   PSP\GAME\SM64\EBOOT.PBP
   PSP\GAME\SM64\SM64.PRX
   ```

4. Desconecta la PSP. En el XMB ve a **Juego → Memory Stick** y ahí estará **Super Mario 64**.

> Tu PSP necesita **firmware personalizado** (ARK-4, PRO, ME, …) para ejecutar homebrew. Este port se desarrolló y probó en una PSP con **ARK-4 a 333 MHz**.
> En una **PSP Go**, copia la carpeta en el `PSP\GAME` del almacenamiento interno.

### Sin PC: instalar desde un release

Si hay un release publicado para tu versión (GitHub → *Releases*), no necesitas compilar nada ni instalar Docker. El release no contiene datos del juego: se prepara solo con **tu propia ROM** la primera vez que lo abres.

1. Descarga el zip de tu versión (`SM64-PSP-us.zip`, `-jp`, `-eu` o `-sh`), extráelo y copia la carpeta **`PSP`** a la raíz de tu Memory Stick (combina las carpetas si te lo pide).
2. Pon tu propia ROM en `PSP\GAME\SM64_<VERSIÓN>\data\` (cualquier nombre; `.z64`, `.v64` o `.n64`). La versión Shindou necesita la ROM SH y la JP.
3. Abre **Super Mario 64** desde el XMB. La primera vez comprueba tu ROM y construye el juego (muestra una barra de progreso y puede tardar), y luego arranca. Las siguientes veces arranca directamente; la ROM, `SM64.template.prx` y `manifest.bin` ya no hacen falta y se pueden borrar.

El instalador muestra sus mensajes en el idioma de tu PSP (inglés, francés, alemán, español, italiano o japonés). Si dice que falta la ROM, comprueba que es la ROM original de 8 MB de esa versión (SHA-1 en la [tabla de arriba](#versiones-compatibles)). El instalador se ha probado en una PSP-3000 real; se agradecen los reportes de otros modelos (sobre todo de la PSP-1000).

### Opcional: salida a TV (experimental)

El juego puede enviar la imagen a una TV con el cable de video de la PSP. Solo funciona en una **PSP-2000 / 3000** (no en una PSP-1000; la PSP Go no se ha tenido en cuenta) y usa un pequeño módulo auxiliar que ya va **dentro de `SM64.PRX`** (el juego lo escribe en la carpeta de partidas y lo carga cuando hace falta), así que no hay nada extra que copiar. Abre **Options → Graphics → TV Settings** (una sub-página que solo se abre con el cable de video puesto; sin el cable la fila sale en gris, dice *No cable* y, si desconectas el cable estando dentro, el menú te devuelve a Graphics) y pon **TV Output** en **240p**, **480i** o **480p** (**480p** necesita cable de *componentes*; **240p** y **480i** también funcionan con cable compuesto/AV, y **480i** es el que hay que usar con una TV solo AV). El cambio se aplica **al momento**, sin reiniciar el juego (espera medio segundo después de que dejes de cambiarlo). El juego lee el estado real del cable con un segundo módulo auxiliar de solo lectura (también dentro de `SM64.PRX`), y **la TV solo se usa con un cable de video puesto**: con TV Output seleccionado, abajo en el menú sale **No cable / AV cable / Component cable** (más los Hz medidos del modo de TV). Si el cable no está puesto la imagen se queda en la pantalla de la PSP (también al arrancar; el ajuste TV Output se conserva), y al conectar el cable la TV se enciende **sola** en el modo elegido; si lo desconectas, la imagen vuelve a la pantalla de la PSP. **El cable es obligatorio:** sin cable detectado la fila no se puede pasar a un modo de TV (suena el zumbido de "no se puede" y se ve en gris mientras está en Off); siempre se puede volver a Off. Si ese módulo no carga en tu firmware, el juego no puede saber del cable y vuelve al comportamiento anterior (supone cable de componentes; **L** reconecta a mano). La fila se ve **gris** (muestra el motivo, por ejemplo "PSP-1000" o "Safe mode", y suena el zumbido de "no se puede mover" si intentas cambiarla) si la consola es una PSP-1000 o si no se pudo cargar el módulo.

Opciones y herramientas relacionadas (en la misma página Graphics, salvo TV Output / TV Aspect / TV Width / TV Height / TV Vertical Shift / Reset TV Settings, que están en la sub-página **TV Settings**; *Reset to Defaults* de la página Graphics no toca los ajustes de TV):

- **TV Aspect**: qué vista dibuja el juego. *4:3* es la vista original de la N64 (una imagen más angosta, 362×272 por dentro, con la proyección 3D original) y *16:9* es la vista panorámica (480×272). En los dos casos la imagen llena todo el cuadro de video y **la TV decide cómo se muestra, así que pon tu TV en el modo de imagen que corresponda: 4:3 para *4:3*, 16:9 / panorámico / completo para *16:9*.** El cambio se aplica al momento. En *4:3* toda la escena 3D (el mundo, los menús de selección de archivo y de estrellas, la cabeza de Mario) usa la proyección 4:3 original, así que todo conserva las proporciones de la N64 en vez de verse achatado.
- **Con TV Output en *Off*** el resto de la página TV Settings (TV Aspect, TV Width, TV Height, TV Vertical Shift y Reset TV Settings) sale en gris y no hace nada, y no se dibuja el marco guía: no hay imagen de TV que ajustar. En todas las páginas de opciones el cursor se salta las filas en gris (salvo "TV Settings" en Graphics, que se puede seleccionar para que abajo se lea "No cable"; al pulsarla suena el zumbido); si todo lo que queda en esa dirección está en gris (como aquí) no se mueve y suena el zumbido de Lakitu ("la cámara no puede moverse").
- **TV Width** (ancho) y **TV Height** (alto): de *60%* a *100%* en pasos de *1%* (por defecto *100%*), por separado porque una TV casi nunca recorta lo mismo en horizontal que en vertical, y porque con valores distintos también se corrige una imagen que se ve algo aplastada o estirada (baja unos puntos *TV Width* respecto a *TV Height* si la imagen se ve demasiado ancha y baja, y al revés). Muchas TV recortan los bordes de la imagen (overscan), así que parte del juego (el HUD, por ejemplo) puede quedar fuera de la pantalla. Con la página **TV Settings** abierta se dibuja un marco blanco justo en el borde de la imagen real (en el formato *16:9*, todo el ancho panorámico): baja *TV Width* hasta que se vean completas las líneas de la izquierda y la derecha, y *TV Height* hasta que se vean las de arriba y abajo (mantener la cruceta repite). El resto del cuadro de video queda en negro. Se aplica al momento, sin reiniciar la salida de TV, y funciona en todos los modos de TV y con los dos formatos. Una línea `tv_area` de un archivo de ajustes antiguo se lee como los dos valores.
- **TV Vertical Shift** (desplazamiento vertical): de *Up 60* (sube) a *Down 60* (baja) (*0* por defecto), en líneas de la señal de video de 480 líneas. Mueve la imagen hacia arriba o hacia abajo dentro del cuadro de video, para TV que dejan una franja vacía arriba o abajo (el marco guía se mueve con la imagen). La imagen no puede salirse del cuadro, así que necesita espacio: con *TV Height* al *100%* no hay nada que mover; baja un poco *TV Height* primero.
- **Prueba de 480p**: al elegir 480p el juego espera **10 segundos** a que pulses **A** ("A: keep 480p" aparece abajo en el menú). Solo el 480p pregunta, porque muchas TV y cables AV no pueden mostrarlo; el 240p y el 480i cambian al momento sin preguntar. Si no confirmas (por ejemplo porque tu TV no puede mostrar el 480p, o no ves la imagen), vuelve solo al modo anterior. **B** cancela al instante. Mientras dura la prueba el resto del menú no hace nada.
- **Reconectar el cable**: si desconectas el cable de video y lo vuelves a conectar, selecciona **TV Output** y pulsa **L** para enviar la imagen otra vez a la TV.

Cómo funciona y sus límites:

- **Sonido con el cable de video**: el cable de AV usa la misma clavija que los auriculares, así que la PSP manda el sonido por ahí y silencia sus parlantes. Con un cable de video conectado y *TV Output* en *Off*, el juego le pide al sistema (`sceAudioRoutingSetMode`) que el sonido salga otra vez por los parlantes de la PSP; en cuanto se enciende TV Output, se quita el cable o se sale del juego, se restaura el modo automático normal. Con auriculares (sin cable de video detectado) no se cambia nada. No pude probarlo en hardware real: si los parlantes siguen en silencio, avísame.
- **Botón HOME con salida de TV**: mientras hay un modo de TV activo el botón HOME no hace nada. El diálogo de salida del firmware sale desordenado en la TV y salir del juego con la señal de TV puesta hace fallar la PSP, así que HOME se desactiva a propósito; al pulsarlo suena el zumbido de Lakitu ("la cámara no puede moverse") y unos segundos aparece abajo el aviso *Turn TV Output Off to exit*. Para salir del juego, primero pon *TV Output* en *Off* en el menú de opciones; con la pantalla de la PSP el HOME vuelve a ser el normal del firmware. Depende de `sceImposeSetHomePopup`; no pude probarlo en hardware real con ARK, así que si con la TV puesta sigue saliendo el diálogo de antes, avísame.
- El juego se sigue dibujando a 480×272 y el resultado se escala a la TV con filtro bilineal, (la vista panorámica en *16:9*, o la vista 4:3, 362×272, en *4:3*). No es un render nativo a 480p. El **240p** usa la misma señal de video que el 480i (la que se ha visto funcionar en una TV real), pero los dos campos llevan las mismas 240 líneas, así que sale una imagen estable de 240 líneas sin entrelazado (pedirle al DVE un modo 320×240 directo no hacía que la TV engranara en una PSP-3000 real: la imagen se quedaba en la pantalla de la PSP). El **480i** es 720×480 entrelazado (dos campos de 240 líneas, como el port de OoT) y es la resolución alta que suelen aceptar las TV; ya no se queda limitado a 30 fps como antes; el juego no sabe qué campo muestra primero tu TV, así que si la imagen se ve un poquito desplazada arriba/abajo respecto al 480p, es por eso.
- Para poder cambiar sin reiniciar, en una PSP-2000 / 3000 el juego carga el módulo y pasa la memoria de video a 4 MB **cada vez que arranca** (como hace el port de OoT), aunque TV Output esté en *Off*. Con TV Output en *Off* el juego se dibuja igual que antes. En **480i** y **480p** solo cabe un buffer de salida, así que puede verse una línea de corte cuando cambia la imagen. 240p usa tres buffers y no tiene ese problema.
- **Es experimental.** Se ha probado en una PSP-3000 real con cable componente; **el 480p no se ha probado** (la TV usada no lo soporta) y tampoco la salida compuesta/AV. Si lo pruebas, cuenta qué ves.
- Red de seguridad: si se encendió la TV y el juego no llegó a unos 5 segundos con ella, el siguiente arranque vuelve a la pantalla de la PSP y pone TV Output en *Off* solo.

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

- **Graphics** — FPS Limit (30 / 60), Frame Pacing (*Keep FPS* / *Keep Speed*), Object Distance y Far Distance (100 %–300 %),
  Object Culling (*Original* / *Vertical*), FPS Counter, Show Sound CPU, **TV Settings** (sub-página, necesita cable de video: ver sección 3: TV Output (*Off* / *240p* / *480i* / *480p*; se aplica al momento; solo el 480p pide confirmar), TV Aspect (*4:3* / *16:9*), TV Width y TV Height (*60%* a *100%*), TV Vertical Shift, Reset TV Settings), Reset to Defaults.
- **Audio** — Music Volume y Effects Volume, de 0 a 100 (100 es el volumen original del juego), y Sound Mode: Stereo, Mono o Headset. Es el mismo ajuste que el de la pantalla de selección de archivo (se guarda en el archivo de guardado, así que cambiarlo en cualquiera de los dos sitios cambia ambos).
- **Controls** — elige un botón y pulsa **✕**; la pantalla dice "Press a button": pulsa el botón de la PSP que quieras (espera unos 5 segundos).
  Si ese botón ya lo usaba otra acción, las dos se intercambian. También Stick Deadzone y Reset to Defaults.

### Dónde se guarda todo

Todo va al Memory Stick, en `PSP\SAVEDATA\SM64PORT\`:

| Archivo | Contenido |
|---|---|
| `sm64_save_file.bin` | Tu progreso (estrellas, monedas…) |
| `sm64config.txt` | Todos tus ajustes (gráficos, audio, controles) en texto plano |
| `freeze.txt` | Solo tras un congelamiento: un informe corto de lo que hacía el juego (ver Solución de problemas) |

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

**`Failed to open baserom.us.z64!`** (o `baserom.jp/eu/sh.z64`)
La ROM no está en el sitio correcto o tiene otro nombre. Debe estar **junto al `Makefile`** y llamarse exactamente `baserom.<versión>.z64`
(con las extensiones visibles, comprueba que no sea `baserom.us.z64.z64`). Para Shindou hacen falta `baserom.sh.z64` **y** `baserom.jp.z64`.

**`baserom.us.z64 has the wrong hash!`** (o lo mismo con `jp`, `eu` o `sh`)
No es la ROM de esa versión, o está en otro formato. Repite el [Paso 1](#paso-1--comprueba-tu-rom).

**La compilación se detiene con un error a la mitad, o cambiaste algo y se comporta raro**
Repítela con `-Clean`: `powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Clean`.

**"No space left on device" (sin espacio)**
Libera espacio en disco (Docker necesita unos GB). En Docker Desktop también puedes usar *Troubleshoot → Clean / Purge data*.

**La compilación es muy lenta**
La primera siempre lo es (mira el Paso 5). Deja el proyecto en un disco normal (no de red) y cierra programas pesados.
En Docker Desktop → Settings → Resources puedes darle más CPUs.

**El juego no aparece en el XMB**
Revisa que las rutas sean exactamente `PSP\GAME\SM64\EBOOT.PBP` y `PSP\GAME\SM64\SM64.PRX` (los dos archivos, en la misma carpeta) y que tu PSP tenga firmware personalizado. Si ves una pantalla "Cargando el juego…" con un error, falta `SM64.PRX` o está en otra carpeta. Vuelve a copiar el archivo por si se corrompió.

**La pantalla se queda en negro al activar TV Output**
Vuelve a iniciar el juego: si se encendió la TV y no duró unos 5 segundos, el juego pone TV Output en *Off* solo en el siguiente arranque y usa la pantalla de la propia PSP.

**El juego se congela**
Si el juego deja de dibujar cuadros durante unos 4 segundos, un pequeño vigía escribe `freeze.txt` en `PSP\SAVEDATA\SM64PORT\` (y lo reescribe cada 4 s mientras dure el congelamiento, hasta los 12 s). Indica dónde estaba el hilo principal, en qué espera cada hilo, el estado del GPU y del procesador de audio, el nivel / acción de Mario y la carga y el voltaje de la batería. Si el juego se recupera solo, lo añade al final. Adjunta el archivo a tu issue. Si la consola se bloquea tan a fondo que no corre ningún código, no se puede escribir nada: en ese caso la *ausencia* de `freeze.txt` también es un dato útil.

**Cualquier otra cosa**
Abre un *Issue* en la página de GitHub del repositorio e incluye las últimas 20–30 líneas de la salida de la compilación.

---

## 6. Compilar en Linux / WSL

Puedes usar la misma imagen de Docker desde una terminal de Linux (o dentro de WSL con Docker funcionando), en la raíz del proyecto y con tu ROM (por ejemplo `baserom.us.z64`) en su sitio. Cambia `VERSION=us` por `jp`, `eu` o `sh` para las otras versiones:

```bash
docker build -t sm64psp - < Dockerfile.psp
docker run --rm -v "$PWD:/sm64" -w /sm64 sm64psp make TARGET_PSP=1 pbp VERSION=us -j$(nproc)
```

El resultado vuelve a ser `build/<versión>_psp/mario64/` (`build/us_psp/mario64/` para US), con `EBOOT.PBP` + `SM64.PRX`. (En Linux los archivos que crea Docker pertenecen a `root`; si te estorba, arréglalo con `sudo chown -R $USER .`.)

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

### Crear un paquete de release

Los paquetes de la sección "Sin PC: instalar desde un release" se hacen con `make TARGET_PSP=1 release VERSION=us -j8` (o `.\build_psp.ps1 -Version us -Release`); el instalador está en `psp/installer/`. Mira `tools/psp/README.md`.

### Otros destinos (sin cambios respecto al proyecto original, sin mantenimiento aquí)

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
├── psp: icono, fondo y sonido del EBOOT, texturas específicas de PSP, `dvemgr` (el módulo auxiliar de salida a TV)
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

**El juego y la decompilación**

- **Nintendo** — Super Mario 64 (todos los derechos son suyos).
- **n64decomp/sm64** y todos sus colaboradores — la decompilación sobre la que se apoya todo.
- **sm64-port** — la versión portable (PC) de la decompilación. De su `enhancements/60fps.patch` sale el sistema de interpolación de cuadros que se usa para los 60 fps (matrices de objetos, HUD, cuadros de diálogo, pinturas, partículas de nieve y burbujas, plataformas móviles); este repositorio lo adapta a la PSP.
- **Emill y MaikelChan** — el renderizador Fast3D en C de `src/pc/gfx/` (su licencia está en `src/pc/gfx/LICENSE.txt`), sobre el que se construye la capa gráfica de la PSP.
- **queueRAM** (sm64tools), **David Benepe** (n64graphics) y **Silicon Graphics** (compilador IDO) — herramientas incluidas en `tools/`, cada una con su propia licencia.

**El port a PSP**

- **Hayden Kowalchuk (HaydenKow)** — el port original a PSP (2020): según el historial de Git, el primer backend gráfico para PSP (sceGu), los controles, el gestor de texturas y el sistema de compilación para PSP de los que parte todo lo demás.
- **z2442** — continuó el port a PSP (transformación y recorte de vértices con la VFPU, audio en el Media Engine) y es dueño del repositorio en el que se basa este proyecto.
- **demmis98** — optimizaciones y arreglos para PSP en el repositorio de z2442 (trabajo con la VFPU, Boos, explosiones de Bob-omb, cañones, transiciones de pantalla).
- **El port de Ocarina of Time de z2442 (oot-PSP) y Daedalus** — el módulo auxiliar de salida a TV (`psp/dvemgr`) está adaptado del que lleva el port de OoT, que a su vez sigue el método de salida de video del emulador de N64 Daedalus para la PSP.
- **ghurone / Sonic-R-PSP** — de su port de Sonic R para PSP salen las ideas de la arena de vértices con caché, de dejar que el GE haga el recorte lateral y la pista sobre las banderas del compilador.
- **beckerd ([psp_mk64_portable](https://github.com/beckerd/psp_mk64_portable)) y JustKidding23 ([Ocarina-of-Time-PSP](https://github.com/JustKidding23/Ocarina-of-Time-PSP), basado en [z2442/oot-PSP](https://github.com/z2442/oot-PSP))** — la idea de no distribuir datos del juego y construirlos en la propia PSP, desde la ROM del jugador, la primera vez que se abre el juego viene de estos dos ports para PSP (Mario Kart 64 y Ocarina of Time). El instalador de este repositorio (`psp/installer/`) es una implementación aparte: reconstruye `SM64.PRX` a partir de una plantilla y un manifiesto.
- **sm64coopdx / sm64ex-coop** — referencia para el diseño de sincronización del multijugador (desactivado en esta versión) y origen de los recursos de Luigi adaptados para este port: modelo y texturas, cabeza del HUD, bancos de sonido y muestras de voz. Las voces de Luigi se acreditan allí a Andrat. sm64coopdx se basa en sm64ex-coop (djoslin0 y colaboradores) y DynOS (PeachyPeach). Estos recursos pertenecen a sus respectivos autores.
- **Claude (Anthropic)** — asistente de IA que escribió o ayudó a escribir la mayor parte de los cambios de código posteriores de este port: la adaptación a 60 fps y el control del ritmo de cuadros, las optimizaciones de renderizado y del GE, el menú de opciones, el soporte de las versiones JP/EU/SH y la documentación.
- Todos los demás que han colaborado en el proyecto PSP y todos los que aparecen en el historial de Git de este repositorio (`git shortlog -sn`).

**Herramientas usadas**

- **pspdev** (PSPSDK y la cadena de herramientas de PSP) para compilar y **PPSSPP** para probar.

Los datos propios del juego (modelos, texturas, música, sonidos) **no** se distribuyen aquí: se extraen de la ROM que tú aportas.
El repositorio sí incluye unos pocos archivos derivados de Nintendo que la PSP necesita para mostrar el juego en su menú (`psp/EBOOT/`: icono, imagen de fondo y sonido). No están cubiertos por la licencia de este proyecto y pertenecen a sus dueños; si eres titular de los derechos y quieres que se retiren, abre un issue.
