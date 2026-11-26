# Silent Hill — Port para Wii U

**Idioma / Language:** [English](README.md) | **Español**

Port nativo de **Silent Hill (PS1)** para **Wii U**, construido sobre la [decompilación exacta de Silent Hill](https://github.com/Vatuu/silent-hill-decomp) y el [port multiplataforma de SlickAmogus](https://github.com/SlickAmogus/silent-hill-decomp) (usando [PsyCross](https://github.com/OpenDriver2/PsyCross)).

> **Aviso legal:** *Silent Hill* es © Konami. Este repositorio **no contiene archivos ni recursos protegidos por derechos de autor** del juego. Debes proporcionar tu propia copia legal en formato `.bin` del disco de *Silent Hill* (PS1, región USA) para jugar.

---

## Características

- **Renderizador nativo GX2** (`wiiu_port/src/gpu_gx2.c`): shaders con CafeGLSL, texturas de VRAM de PSX con paleta en shader, corrección de perspectiva (PGXP) y salida en **1920×1080** (16:9 Hor+ durante el juego y 1440×1080 en pantallas 4:3).
- **Integración completa con el Wii U GamePad** (`wiiu_port/src/gamepad_screen.c`, `gamepad_game.c`):
  - **Mapa en vivo:** muestra el mapa de papel de la zona actual con todas sus marcas y la flecha de Harry en cuanto lo consigues (zoom de 1× a 8× en **Options → GamePad Map Zoom**).
  - **Barra de salud y munición:** trazo ECG discreto en la parte superior del GamePad (con alertas `CAUTION` / `DANGER`) y munición del arma equipada. Toca la barra para ocultarla si prefieres la experiencia clásica sin indicador de salud.
  - **Radio en el altavoz del GamePad:** reproduce la estática de la radio portátil directamente en el altavoz del GamePad (`radio_gamepad` en `silenthill.cfg`: `0` = TV, `1` = GamePad [predeterminado], `2` = ambos).
  - **Modo Off-TV:** toca **PAD** (esquina superior derecha del GamePad) para pasar el juego a la pantalla del GamePad y apagar la TV; toca **TV** para volver.
  - **Vibración y notificaciones de logros:** activa el motor de vibración del GamePad (incluyendo el latido de salud baja) y muestra los avisos de logros en el GamePad.
- **Dificultades y mejoras de calidad de vida:** dificultades Easy / Modern / Normal / Hard, autoguardado en escaleras y jefes, respaldos automáticos de Memory Card y brillo en objetos clave.

---

## Instalación y ejecución en Wii U

### Requisitos previos
- Una consola Wii U modificada con **Aroma** (o **Tiramisu / Homebrew Launcher**).
- Tu propia imagen de disco `.bin` legal de *Silent Hill* de PS1 (USA, `SLUS-00707`). Puede tener cualquier nombre terminado en `.bin`: el port la detecta automáticamente.
- El ejecutable compilado `silenthill_gx2.rpx` (descargado desde Releases o compilado desde el código fuente).

### Estructura en la tarjeta SD
Copia los archivos en tu tarjeta SD (formateada en FAT32) con la siguiente estructura:

```text
sd:/
├── wiiu/
│   └── apps/
│       └── silenthill/
│           └── silenthill_gx2.rpx
└── silenthill/
    └── Silent Hill (USA).bin       <-- tu propia imagen .bin de PS1 (cualquier nombre .bin)
```

*(También puedes colocar el archivo `.bin` directamente en `sd:/wiiu/apps/silenthill/` o en la raíz `sd:/`. El port busca en `sd:/silenthill/`, `sd:/wiiu/apps/silenthill/` y `sd:/`, en ese orden.)*

### Cómo ejecutarlo
- **Aroma:** Abre **Homebrew** desde el Menú de Wii U y ejecuta `silenthill_gx2.rpx`.
- **Tiramisu / Homebrew Launcher:** Abre el **Homebrew Launcher** y selecciona `silenthill`.

### Archivos generados en la tarjeta SD
En la misma carpeta donde esté tu archivo `.bin`, el port utiliza:
- **`save/`** — partidas guardadas (Memory Card) y copias de seguridad automáticas.
- **`silenthill_NNN.log`** — registros de depuración (se conservan los últimos 20).
- **`silenthill.cfg`** — archivo opcional de configuración (ver [Configuración](#configuración-silenthillcfg)).
- **`gamepad_nohealth`** — se crea automáticamente al tocar la barra de salud del GamePad para ocultarla.
- **`gputest`** — crea un archivo vacío con este nombre para mostrar la imagen de prueba del renderizador al arrancar.

---

## Ejecución en Cemu (emulador en PC)

1. Abre Cemu y ubica su carpeta de tarjeta SD virtual (`sdcard/` dentro de la carpeta de datos de Cemu, por ejemplo `<Cemu>/sdcard/`).
2. Crea `sdcard/silenthill/` y copia ahí tu imagen `.bin` de *Silent Hill* (PS1).
3. En Cemu, ve a **File → Load...** y selecciona `silenthill_gx2.rpx`.
4. Para ver la pantalla del GamePad (mapa en vivo, barra de salud, botón Off-TV), activa **Options → Separate GamePad View** (o mantén presionada la tecla **Tab**).

---

## Controles

Los botones están asignados por posición física para coincidir con el mando de PlayStation:

| PlayStation | Wii U GamePad / Pro Controller |
|---|---|
| Cruz / Círculo / Cuadrado / Triángulo | B / A / Y / X |
| L1 / R1 | L / R |
| L2 / R2 | ZL / ZR |
| Select / Start | − / + |
| Movimiento | Stick izquierdo / Cruceta (D-Pad) |
| Cámara | Stick derecho |

---

## Configuración (`silenthill.cfg`)

Puedes crear un archivo opcional `silenthill.cfg` junto a tu `.bin` para ajustar opciones:

```ini
widescreen_mode = 1      # 1 = 16:9 Hor+ (1920x1080), 0 = 4:3 (1440x1080)
minimap = 0              # 0 = desactivado en TV (usa el mapa del GamePad), 1 = mostrar también en TV
radio_gamepad = 1        # 0 = solo TV, 1 = altavoz del GamePad (predeterminado), 2 = ambos
gamepad_map_zoom = 4     # de 1 a 8 (predeterminado: 4)
preload_chunks = 1       # precargar zonas del mapa en MEM2
```

---

## Compilar desde el código fuente

Requiere **Linux** o **WSL (Ubuntu)** con **devkitPro** instalado en `/opt/devkitpro`.

### 1. Instalar las herramientas de Wii U de devkitPro
Sigue la [guía de devkitPro pacman](https://devkitpro.org/wiki/devkitPro_pacman) e instala los paquetes de Wii U:

```bash
sudo dkp-pacman -S wiiu-dev wiiu-sdl2 ppc-zlib
```

### 2. Clonar el repositorio
```bash
git clone -b wiiu-port https://github.com/comaro-sudo/silent-hill-decomp.git
cd silent-hill-decomp
```

### 3. Descargar CafeGLSL (una sola vez, para el renderizador GX2)
```bash
bash wiiu_port/fetch_cafeglsl.sh
```
Esto descarga CafeGLSL en `wiiu_port/cafeglsl/` (se enlaza estáticamente dentro del `.rpx`, así que no hace falta copiar ningún `.rpl` extra en la tarjeta SD).

### 4. Compilar el `.rpx`
```bash
bash wiiu_port/build.sh
```
El ejecutable final se genera en:
- `wiiu_port/bin/silenthill_gx2.rpx`

### Opciones útiles de compilación
- **Renderizador alternativo SDL2** (no requiere CafeGLSL):
  ```bash
  WIIU_RENDERER=sdl bash wiiu_port/build.sh
  ```
  Genera `wiiu_port/bin/silenthill.rpx`.
- **Compilar solo el motor y el HAL:** `bash wiiu_port/gate.sh`
- **Compilar solo los overlays de mapas:** `bash wiiu_port/maps.sh`
- **Solo reenlazar:** `SKIP_GATE=1 SKIP_MAPS=1 bash wiiu_port/build.sh`

---

## Estructura del proyecto

| Ruta | Propósito |
|---|---|
| `wiiu_port/` | HAL de Wii U (renderizadores GX2 y SDL2, pantalla del GamePad, controles VPAD/KPAD, audio AX/SDL, sistema de archivos SD, punto de entrada y scripts de compilación) |
| `src/`, `include/` | Código fuente y cabeceras decompiladas de Silent Hill (PS1) |
| `pc_port/` | Runtime compartido del port de PC, PsyCross integrado (con correcciones big-endian de GTE), decodificador FMV y datos extraídos de mapas |
| `xbox_port/`, `xbox360_port/` | Capas compartidas de memoria/GPU/CD/Memory Card para consola y correcciones PowerPC big-endian de 32 bits |

---

## Estado actual

- Arranca y reproduce el video de introducción (FMV), pantalla de título, demos y partida completa en **Cemu**, con todas las funciones del GamePad activas.
- **Aún no probado en una consola Wii U real** — en particular, la calibración táctil del GamePad asume coordenadas en 1280×720 y conviene verificarla en hardware real. ¡Cualquier reporte de pruebas en consola es bienvenido!
