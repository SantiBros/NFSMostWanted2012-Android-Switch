<div align="center">

[![English](https://img.shields.io/badge/lang-English-2b2f38?style=for-the-badge)](README.md)
[![Español](https://img.shields.io/badge/idioma-Espa%C3%B1ol-e11d2e?style=for-the-badge)](README.es.md)

<img src="docs/images/banner.jpg" alt="NFS Most Wanted 2012 (versión Android) para Nintendo Switch, un port homebrew no oficial" width="100%">

# NFS Most Wanted 2012 (Android) para Nintendo Switch

**Un port homebrew no oficial de la versión Android de *Need for Speed™ Most Wanted* (2012, versión 1.3.128) para Nintendo Switch.**<br>
El juego original funcionando en tu Switch: no es un emulador de teléfono. Trae tu propia copia,
ponla en una carpeta y corre por Fairhaven, en modo portátil o en el dock.

[![Última versión](https://img.shields.io/github/v/release/SantiBros/NFSMostWanted2012-Android-Switch?include_prereleases&style=for-the-badge&label=descargar&color=e11d2e&logo=github&logoColor=white)](../../releases)
[![Descargas](https://img.shields.io/github/downloads/SantiBros/NFSMostWanted2012-Android-Switch/total?style=for-the-badge&label=descargas&color=2b2f38)](../../releases)
![Nintendo Switch homebrew](https://img.shields.io/badge/Nintendo%20Switch-homebrew-2b2f38?style=for-the-badge)
![Versión Android 1.3.128](https://img.shields.io/badge/versi%C3%B3n%20Android-1.3.128-2b2f38?style=for-the-badge)
![Atmosphère](https://img.shields.io/badge/CFW-Atmosph%C3%A8re-2b2f38?style=for-the-badge)
[![Licencia: MIT](https://img.shields.io/badge/licencia-MIT-2b2f38?style=for-the-badge)](LICENSE)

[Instalar](#cómo-instalar) · [Controles](#controles) · [Preguntas frecuentes](#preguntas-frecuentes) · [Solución de problemas](docs/INSTALL.es.md#solución-de-problemas) · [Cambios](CHANGELOG.md)

</div>

> [!IMPORTANT]
> **No se incluye ningún archivo del juego, ni en este repositorio ni en las versiones publicadas.**
> Necesitas tu propia copia del juego para Android: el APK y el OBB de la versión 1.3.128 (o un solo
> archivo `.xapk` que traiga los dos). Por favor, no los pidas en los issues y nunca los subas a
> ningún sitio.
>
> Este port funciona **solo con la versión Android**. Los archivos de las versiones de PC,
> PlayStation 3, Xbox 360, PS Vita, Wii U o iOS de Most Wanted (2012) no sirven.

## Capturas

<table>
  <tr>
    <td width="50%"><img src="docs/images/screenshots/race-intro.jpg" alt="La presentación de una carrera"></td>
    <td width="50%"><img src="docs/images/screenshots/race-city.jpg" alt="Corriendo por la ciudad en primer lugar"></td>
  </tr>
  <tr>
    <td align="center"><sub>La presentación de la carrera, en tiempo real</sub></td>
    <td align="center"><sub>Corriendo por Fairhaven con el control</sub></td>
  </tr>
  <tr>
    <td><img src="docs/images/screenshots/map.jpg" alt="El mapa con el resaltado del control sobre un evento"></td>
    <td><img src="docs/images/screenshots/garage.jpg" alt="El garaje"></td>
  </tr>
  <tr>
    <td align="center"><sub>Todos los menús funcionan con la cruceta, con un resaltado al estilo del juego</sub></td>
    <td align="center"><sub>El garaje, con los botones indicados en pantalla</sub></td>
  </tr>
  <tr>
    <td><img src="docs/images/screenshots/showroom.jpg" alt="Un auto en la sala de exhibición 3D"></td>
    <td><img src="docs/images/screenshots/pause-menu.jpg" alt="El menú de pausa durante una carrera"></td>
  </tr>
  <tr>
    <td align="center"><sub>La sala de exhibición de autos en 3D</sub></td>
    <td align="center"><sub>El menú de pausa, en plena carrera</sub></td>
  </tr>
</table>

<sub>Capturadas con la versión de prueba para PC del port, que ejecuta el mismo código del juego a la
resolución portátil de la Switch (1280×720). El panel de la captura de carrera es el tutorial del
propio juego para manejar con control.</sub>

## Características

- **El juego real, no un emulador de teléfono.** El código ARM del juego Android se traduce al vuelo
  para la CPU de la Switch (con [Dynarmic](https://github.com/azahar-emu/dynarmic)); todo lo que lo
  rodea (archivos, gráficos, sonido, controles) está hecho de nuevo para la Switch.
- **720p en modo portátil, 1080p en el dock.**
- **Soporte completo de control.** Maneja con las palancas y los botones, y usa todos los menús con
  un resaltado que mueves con la cruceta, dibujado al estilo del juego. La pantalla táctil sigue
  funcionando.
- **Dirección con giroscopio (opcional).** Inclina la consola o el control como un volante.
- **Instalación fácil, sin PC.** Copia tu APK y tu OBB (o un solo `.xapk`) en una carpeta: el port
  se prepara solo la primera vez que lo abres, con una pantalla de progreso.
- **A prueba de errores.** Si las librerías quedaron dentro de `lib/armeabi-v7a/`, la carpeta de
  assets en el lugar equivocado o el OBB con otro nombre, el port los mueve a su sitio solo.
- **Totalmente sin conexión.** Todos los servicios en línea están bloqueados; la carrera, los autos
  y las partidas guardadas funcionan sin internet.
- **Tu partida en un solo archivo** (`save/progress/nfstr_save.sb`): fácil de respaldar, y se
  conserva al actualizar.
- **Menú de depuración** (ZL + ZR + Menos): límite de FPS, VSync, resolución 3D, nivel de detalle,
  relojes de CPU / GPU / memoria, volumen del sonido y contador de FPS.
- **Dos cierres inesperados del juego original, corregidos** en el port.

## Qué necesitas

| | Qué |
|---|---|
| **Consola** | Una Nintendo Switch con [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) y el Homebrew Menu. Usa emuMMC y guarda un respaldo de tu tarjeta SD. |
| **Juego** | Tu propio *Need for Speed Most Wanted* para Android, versión **1.3.128**: el APK con la versión de 32 bits (`lib/armeabi-v7a`) y el OBB `main.1003128.com.ea.games.nfs13_row.obb` (unos 650 MB). También sirve un solo archivo `.xapk` con los dos. |
| **Tarjeta SD** | Alrededor de 1 GB libre. |
| **PC** | No hace falta. |

## Cómo instalar

> [!TIP]
> **¿Es tu primera vez con homebrew en la Switch?** Primero prepara tu consola con firmware
> modificado (Atmosphère) y el Homebrew Menu: la guía de la comunidad
> [switch.hacks.guide](https://switch.hacks.guide/) (en inglés) lo explica paso a paso. ¿No sabes qué
> es un APK, un OBB o el emuMMC? Mira el [glosario](docs/INSTALL.es.md#glosario).

<img src="docs/images/install-steps.es.png" alt="Instalar en 4 pasos: descarga el zip, copia la carpeta switch a la SD, agrega tu APK y tu OBB a la carpeta game, abre con R + cualquier juego" width="100%">

1. **Descarga.** En la página de [Releases](../../releases), en la sección **Assets**,
   descarga `NFSMostWanted2012-0.2.6.zip` (no "Source code", que es solo para desarrolladores) y
   descomprímelo en tu computadora.
2. **Copia a la SD.** Copia la carpeta `switch` del zip a la raíz de tu tarjeta SD.
3. **Agrega tu juego.** Copia tu APK y tu OBB en `switch/NFSMostWanted2012/game/`. No los
   descomprimas ni les cambies el nombre. Si tu copia es un solo archivo `.xapk`, copia solo ese.
   ¿Todavía no los tienes como archivos? Aquí está
   [cómo sacarlos de tu propio teléfono](docs/INSTALL.es.md#sacar-los-archivos-de-tu-propio-teléfono).
4. **Juega.** Mantén presionado **R** mientras abres cualquier juego para entrar al Homebrew Menu y
   elige **NFS Most Wanted 2012 Loader**. La primera vez, el port prepara el juego a partir de tu APK
   (unos segundos, o cerca de un minuto con un `.xapk`). Presiona **A** y a correr.

> [!WARNING]
> Abre siempre el Homebrew Menu con **R + un juego** (modo title takeover). Desde el Álbum, el
> homebrew solo recibe unos 440 MB de memoria, y no le alcanza al juego.

<details>
<summary><b>Así queda la carpeta después de la primera vez</b></summary>

```
sd:/switch/NFSMostWanted2012/
├── NFSMostWanted2012-0.2.6.nro   el port
├── README.txt                    la guía del jugador (en inglés)
├── LICENSES.txt                  componentes de código abierto y sus licencias
├── game/                         TUS archivos del juego (nunca los compartas)
│   ├── main.1003128.com.ea.games.nfs13_row.obb   el OBB, sin cambios
│   ├── <tu APK>.apk              se puede borrar después de la primera vez
│   ├── lib/                      las 5 librerías del juego, sacadas del APK
│   └── assets/                   los assets del APK, sacados del APK
├── save/
│   ├── progress/                 tu carrera: dinero, reputación, autos, eventos
│   └── settings/                 los ajustes del propio juego (idioma, ...)
├── source/                       el código fuente del port (no hace falta para jugar)
├── settings.ini                  los ajustes del port (también se cambian en el menú de depuración)
└── log.txt                       qué pasó la última vez: adjúntalo al reportar un error
```

</details>

La [guía de instalación completa](docs/INSTALL.es.md) explica cómo actualizar, cómo hacer respaldos,
el script opcional para PC que le pone al port el ícono del juego, y la
[solución de problemas](docs/INSTALL.es.md#solución-de-problemas).

### Actualizar

Copia la carpeta `switch` de la nueva versión encima de la anterior: `game/`, `save/` y tus ajustes se
conservan, y la nueva versión borra sola el `.nro` viejo. ¿Vienes de una versión anterior en
`switch/NFSMostWanted/` (0.1.0 a 0.2.0)? Copia la carpeta nueva y ábrela: tus archivos del juego y tu
progreso se mueven solos.

## Controles

**Manejo**: el juego ve un control MOGA Pro, que soporta de forma nativa.

| Switch | Juego |
|---|---|
| Palanca izquierda | Girar |
| A | Derrapar |
| L, o palanca derecha hacia abajo | Frenar / reversa |
| R, o palanca derecha hacia arriba | Nitro |
| + | Pausa |

**Menús**

| Switch | Juego |
|---|---|
| Cruceta / palanca izquierda | Mover el resaltado (en el borde de una lista: desplazarla) |
| A | Presionar el botón resaltado |
| B | Atrás |
| L / R, luego A | Elegir una página en la barra de menú inferior |
| Palanca derecha | Arrastrar como con el dedo: desplazar listas, mover el mapa |
| + | EasyDrive (empezar el siguiente evento) en el mapa |
| Pantalla táctil | Funciona como en un teléfono |

**Giroscopio**: en el juego, *Options > Controls > TILT TO STEER*. Con la consola derecha vas en línea
recta; unos 30° de inclinación es el giro máximo. Todos los botones siguen funcionando.<br>
**Menú de depuración**: mantén **ZL + ZR** y presiona **Menos**. Los cambios se guardan al momento en
`settings.ini`.

## Estado

> [!NOTE]
> **Experimental.** Todo el juego funciona en la Switch, pero esta es una versión temprana. Respalda
> tu tarjeta SD y usa emuMMC.

| Parte | Estado |
|---|---|
| Carrera, competencias, garaje y autos | ✅ Funciona |
| Todos los menús con el control, pantalla táctil | ✅ Funciona |
| Música y efectos de sonido | ✅ Funciona |
| Partidas guardadas (se conservan al actualizar) | ✅ Funciona |
| 720p portátil / 1080p en el dock | ✅ Funciona (la resolución se elige al abrir el juego) |
| FPS | ⚠️ Todavía en ajuste en consola real: el juego apunta a 30 FPS, y en carreras con mucha acción puede bajar de eso con los relojes de fábrica. El menú de depuración permite subir el reloj de la CPU. La 0.2.6 quita las causas principales de los tirones cortos (leer los datos del juego y el log desde la tarjeta SD). |
| Giroscopio, relojes personalizados | 🧪 Nuevo, aún sin confirmar en muchas consolas: se agradecen comentarios |
| Funciones en línea (Autolog, clasificaciones, cuenta EA, tienda) | ❌ No disponibles: el port no usa internet a propósito |

**Problemas conocidos**
- La primerísima versión de prueba congeló una consola una vez (no se sabe la causa; las versiones
  siguientes no lo han hecho en las pruebas hasta ahora).
- Si conectas o sacas la consola del dock mientras juegas, se mantiene la resolución con la que se
  abrió el juego: reinícialo para cambiarla.

En el [registro de cambios](CHANGELOG.md) (en inglés) está lo que cambió en cada versión.

## Preguntas frecuentes

<details>
<summary><b>¿Es un emulador?</b></summary>

No de un teléfono completo. El port carga las propias librerías del juego y ejecuta su código ARM de
32 bits con un recompilador (Dynarmic), mientras que las partes de Android con las que habla el
juego (archivos, OpenGL ES, sonido, controles, la parte de Java) están hechas de nuevo para la
Switch. Más detalles en [cómo funciona](docs/TECHNICAL.md) (en inglés).
</details>

<details>
<summary><b>¿Es el port de Need for Speed: Most Wanted (2005)?</b></summary>

No. Ese es otro juego y otro proyecto: [nfsmw-nx](https://github.com/StevensND/nfsmw-nx). Este es el
juego de 2012 (su versión Android). Puedes tener los dos instalados a la vez.
</details>

<details>
<summary><b>¿Funciona con la versión de PC, PS3, Xbox 360 o Wii U de Most Wanted (2012)?</b></summary>

No. Esos son otros programas: la versión de Criterion para PC, PlayStation 3, Xbox 360 y PS Vita, y
*Most Wanted U* para Wii U. Este port ejecuta la versión **Android**, el juego para móviles hecho por
Firemonkeys, y solo funciona con su APK y su OBB (versión 1.3.128).
</details>

<details>
<summary><b>¿De dónde saco los archivos del juego?</b></summary>

De tu propia copia del juego. Este proyecto no puede ayudarte con eso, y los issues que pidan
archivos del juego se cerrarán.
</details>

<details>
<summary><b>¿Qué versión del juego funciona?</b></summary>

La versión **1.3.128** (paquete `com.ea.games.nfs13_row`), con las librerías de 32 bits
(`lib/armeabi-v7a`) dentro del APK. Los APK que solo traen la versión de 64 bits (`arm64-v8a`) no
funcionan: el port te lo dice la primera vez que lo abres.
</details>

<details>
<summary><b>¿El port se conecta a internet? ¿Me pueden banear?</b></summary>

El port nunca se conecta a internet: todas las funciones en línea del juego están bloqueadas, y ni
siquiera inicia el servicio de red de la consola. Para usar cualquier homebrew hace falta un
firmware modificado, lo que va contra las condiciones de Nintendo y puede hacer que una consola
quede baneada de los servicios en línea de Nintendo. Lo habitual es usar el homebrew en un emuMMC y
mantener un respaldo de tu tarjeta SD.
</details>

<details>
<summary><b>¿Dónde está mi partida y cómo la respaldo?</b></summary>

Toda tu carrera (dinero, reputación y rango Most Wanted, autos, eventos) es un solo archivo:
`switch/NFSMostWanted2012/save/progress/nfstr_save.sb`. Cópialo a un lugar seguro de vez en cuando.
</details>

<details>
<summary><b>¿El Homebrew Menu puede mostrar el ícono real del juego?</b></summary>

Sí, en tu propia copia: usa en una PC el script opcional `prepare_sd.py` que viene en la descarga,
con la opción `--nro` (está en la [guía de instalación](docs/INSTALL.es.md#opcional-el-script-para-pc)). El `.nro` publicado nunca trae imágenes de EA.
</details>

<details>
<summary><b>El juego va lento. ¿Qué puedo hacer?</b></summary>

Abre el menú de depuración (ZL + ZR + Menos) y activa el contador de FPS. Subir el reloj de la CPU es
lo que más ayuda; además, `log.txt` dice cada 10 segundos qué está frenando los FPS. Compartir esas
líneas `perf:` en un issue ayuda a mejorar el port.
</details>

<details>
<summary><b>El sonido cruje. ¿Qué puedo hacer?</b></summary>

El juego mezcla su sonido tan fuerte que los momentos fuertes se cortan, y eso suena como un
crujido. Desde la 0.2.6 el port baja la mezcla al 70% por defecto; puedes cambiarlo en *Sound volume*
(volumen del sonido) en el menú de depuración (ZL + ZR + Menos). Sube el volumen de la consola en su
lugar. Si el sonido se entrecorta o se detiene un momento, repórtalo con `log.txt`: ahí se cuentan
esos cortes ("dropouts").
</details>

<details>
<summary><b>¿Cómo reporto un error?</b></summary>

Abre un [issue](../../issues/new/choose) y adjunta `log.txt` de la carpeta del port. Ahí está lo que
pasó la última vez que lo abriste, y nunca contiene archivos del juego.
</details>

## Cómo funciona

El juego de Android es un programa ARM de 32 bits, y el homebrew de Switch solo ejecuta código de
64 bits. El port:

1. **Traduce el código del juego** con [Dynarmic](https://github.com/azahar-emu/dynarmic), el
   recompilador que usan los emuladores de 3DS, adaptado a las reglas de memoria de la Switch.
2. **Carga él mismo las cinco librerías `.so` del juego** y las conecta con reemplazos nativos de
   todo lo que usan: unas 330 funciones de libc, matemáticas e hilos, OpenGL ES 2.0 y APIs de Android.
3. **Hace el papel de la parte Java**: un JNI simulado con las clases Java del juego (actividad,
   bucle principal, assets, control MOGA, audio FMOD, dibujo de texto) reescritas en C++.
4. **Lleva el ciclo de vida de Android** igual que la app real, cuadro por cuadro, con las llamadas
   gráficas del juego en su propio núcleo de CPU.

Más detalles en [docs/TECHNICAL.md](docs/TECHNICAL.md) (en inglés).

## Compilar desde el código fuente

Necesitas [devkitPro](https://devkitpro.org/wiki/Getting_Started) con `switch-dev`, `switch-mesa` y
`switch-libdrm_nouveau`, CMake 3.16 o superior y los headers de Boost. Luego:

```sh
cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-switch -j
```

El resultado es `build-switch/NFSMostWanted2012-0.2.6.nro`. Los detalles están en
[docs/BUILDING.md](docs/BUILDING.md) (en inglés).

## Créditos

Gracias a quienes hicieron posible este proyecto:

- **Electronic Arts** y **Firemonkeys Studios**, que hicieron el juego. Apoya el original.
- [**Dynarmic**](https://github.com/azahar-emu/dynarmic) de merryhime y colaboradores, con oaknut,
  mcl, fmt, robin-map y Boost.
- [**libnx**](https://github.com/switchbrew/libnx) y la comunidad switchbrew,
  [**devkitPro**](https://devkitpro.org) (devkitA64, switch-tools, switch-mesa) y
  [**Atmosphère**](https://github.com/Atmosphere-NX/Atmosphere).
- [**Mesa**](https://mesa3d.org) y **libdrm_nouveau**, para OpenGL ES en la GPU de la Switch.
- [**stb**](https://github.com/nothings/stb) de Sean Barrett (dibujo de texto, descompresión del APK).
- **Barlow Condensed** de Jeremy Tribby (licencia SIL Open Font), usada en el banner y el ícono.

La lista completa con licencias está en [THIRD_PARTY.md](THIRD_PARTY.md).

## Licencia

El código propio del port (`src/`, `tools/`, `res/`, `patches/`) está bajo la
[licencia MIT](LICENSE). El código de terceros incluido mantiene sus propias licencias
([THIRD_PARTY.md](THIRD_PARTY.md)). El juego no está incluido y esta licencia no lo cubre de ninguna
manera.

## Aviso legal

Este es un proyecto no oficial hecho por fans. No está afiliado, respaldado ni patrocinado por
Electronic Arts Inc., Firemonkeys Studios ni Nintendo. *Need for Speed* y *Most Wanted* son marcas
de Electronic Arts Inc.; Nintendo Switch es una marca de Nintendo. Aquí no se distribuye ningún
código ni dato del juego: necesitas tu propia copia.

Usar homebrew requiere un firmware modificado, lo que va contra las condiciones de servicio de
Nintendo y puede hacer que una consola quede baneada de los servicios en línea; esa decisión es
tuya. El software se ofrece tal cual, sin garantía (ver [LICENSE](LICENSE)).

---

<div align="center">

Un proyecto de **SantiBros** · Si te gusta, dale una estrella ⭐ al repositorio y compártelo

</div>
