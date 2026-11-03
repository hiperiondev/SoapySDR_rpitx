# SoapySDR_rpitx

[English](README.md) | **Español** | [Italiano](README.it.md)

**Un driver de transmisión SoapySDR que convierte una Raspberry Pi en un transmisor I/Q de banda angosta — sin
hardware adicional, solo el pin de reloj GPIO4.**

SoapySDR_rpitx expone la salida de reloj de propósito general de la Raspberry Pi (GPCLK0 en GPIO4) como un
dispositivo TX estándar de [SoapySDR](https://github.com/pothosware/SoapySDR). Cualquier aplicación compatible con
Soapy — GNU Radio, SDRangel, scripts en Python, el puente HPSDR incluido para piHPSDR / Thetis / linHPSDR / Quisk /
SparkSDR — puede enviarle muestras complejas en banda base y obtener RF en el pin del conector. Está construido
sobre una copia integrada y reforzada de [librpitx](https://github.com/F5OEO/librpitx) de Evariste Courjaud F5OEO.

> **Estado: experimental.** El camino de transmisión (anillo DMA, alimentador de tiempo real, modulación del PLL,
> recuperación) está cubierto por pruebas en el host sobre hardware simulado, incluidas ejecuciones con
> AddressSanitizer, UndefinedBehaviorSanitizer y ThreadSanitizer. Los informes desde placas reales son muy
> bienvenidos.

---

## Contenido

- [Aviso legal y de radio](#aviso-legal-y-de-radio)
- [Capacidades](#capacidades)
- [Hardware soportado](#hardware-soportado)
- [Requisitos y restricciones del sistema](#requisitos-y-restricciones-del-sistema)
- [Compilación en la Raspberry Pi](#compilación-en-la-raspberry-pi)
- [Compilación cruzada](#compilación-cruzada)
- [Instalación y verificación](#instalación-y-verificación)
- [Uso](#uso)
- [Herramientas](#herramientas)
- [Funcionamiento interno](#funcionamiento-interno)
- [Solución de problemas](#solución-de-problemas)
- [Pruebas](#pruebas)
- [Estructura del proyecto](#estructura-del-proyecto)
- [Licencia y créditos](#licencia-y-créditos)

---

## Aviso legal y de radio

GPIO4 entrega una **onda cuadrada**. Es rica en armónicas (armónicas impares de amplitud aproximada 1/n) y en
productos espurios del PLL fraccional. Coloque **siempre** un filtro pasabanda o pasabajos para la banda en uso
entre el pin y cualquier antena, y transmita solo en frecuencias y con potencias para las que tenga licencia — o
sobre una carga fantasma. Usted es responsable de lo que irradia el pin.

El driver mantiene la portadora apagada mientras no se reproducen muestras y la apaga mientras se reprograma el
PLL, pero el árbol de relojes se modifica en cuanto se crea un stream.

---

## Capacidades

| Característica | Detalle |
|----------------|---------|
| Dirección | Solo transmisión, un canal |
| Formatos de muestra | `CF32` (nativo, fondo de escala 1.0), `CS16` (fondo de escala 32768), `CS8` (fondo de escala 128) |
| Frecuencia de muestreo | 10 kHz – 200 kHz (cualquier valor; listadas 24 k / 48 k / 96 k / 192 k; por defecto 48 kHz) |
| Frecuencia | 50 kHz – 1500 MHz (BCM2711: 50 kHz – 1000 MHz) en la fundamental; `harmonic=n` multiplica el rango |
| Modulación | Cualquier señal I/Q: fase/frecuencia mediante el multiplicador fraccional de PLLC, amplitud mediante la corriente de salida de los pads (8 niveles) |
| Modos de amplitud | `envelope` (la corriente sigue a \|I/Q\|) o `constant` (corriente máxima siempre que haya muestra: FM, FSK, CW, PSK de envolvente constante) |
| Ganancia | Elemento `PAD`, 0–7 = 2 mA … 16 mA de corriente de salida |
| Corrección de frecuencia | Estimación NTP del kernel (`ntp_adjtime`) más ppm manual; `setFrequencyCorrection()` |
| Ráfagas | `SOAPY_SDR_END_BURST` respetado: la desactivación espera a que la ráfaga haya salido por la antena |
| Streaming | FIFO sin bloqueos, hilo alimentador de tiempo real, pre-roll configurable, contrapresión con timeouts, escrituras parciales |
| Cambios en vivo | Frecuencia, frecuencia de muestreo, ganancia y todos los ajustes durante la transmisión, sin perder muestras encoladas |
| Diagnóstico | Sensores de enganche del PLL, estado del DMA, llenado de la FIFO, underflows, overruns del DMA, reinicios del DMA y frecuencia de muestreo real; `readStreamStatus()` informa `SOAPY_SDR_UNDERFLOW` |
| Seguridad | Bloqueo a nivel de sistema, registro de caídas y recuperación automática, limpieza opcional ante señales, herramienta `rpitx_reset`, rechazo de usuarios en conflicto de GPIO / I2S / DMA |
| Integraciones | GNU Radio (Soapy sink), SDRangel (salida SoapySDR), Python, programas host HPSDR Protocol 1 mediante el puente |

Lo que el hardware hace bien: **NBFM, FSK/AFSK, modos digitales de envolvente constante tipo WSPR/FT8, CW y BPSK**.
AM y SSB funcionan, pero con solo 8 escalones de amplitud (y la portadora cortada por debajo del más bajo) su
calidad es limitada por naturaleza.

---

## Hardware soportado

| Placa | SoC | Estado |
|-------|-----|--------|
| Pi 1 A/A+/B/B+, Zero, Zero W/WH, CM1 | BCM2835 | soportada |
| Pi 2 B | BCM2836 / BCM2837 | soportada |
| Pi 3 B/B+/A+, CM3, Zero 2 W | BCM2837 / BCM2837B0 / RP3A0 | soportada |
| Pi 4 B, Pi 400, CM4 | BCM2711 | soportada |
| Pi 5, Pi 500, CM5 | BCM2712 + RP1 | **no soportada** (GPIO, relojes y DMA están en el chip RP1) |

- **Salida de RF: GPIO4, pin 7 del conector** (GPCLK0, función alternativa 0). Masa en el pin 9.
- Se soportan Raspberry Pi OS de 32 bits (armhf) y de 64 bits (arm64).
- La placa se identifica con `/proc/device-tree/model` y la base de periféricos en `/proc/device-tree/soc/ranges`
  (`0x20000000`, `0x3F000000` o `0xFE000000`). `SoapySDRUtil --find` lista el dispositivo solo en placas soportadas.
- El modo de periféricos altos de BCM2711 (`arm_peri_high=1`) no está soportado.

---

## Requisitos y restricciones del sistema

- **Privilegios de root.** El driver mapea los registros de periféricos mediante `/dev/mem` y reserva memoria DMA
  mediante el mailbox de VideoCore `/dev/vcio`. Sin acceso informa un error claro; nunca termina la aplicación
  anfitriona.
- **Reloj del núcleo fijo.** PLLC se modula, por lo que el reloj del núcleo VPU se mueve a PLLA mientras existe un
  stream. Como hace el instalador de rpitx, mantenga fija la frecuencia del núcleo:

  ```ini
  # /boot/firmware/config.txt (o /boot/config.txt en versiones anteriores)
  gpu_freq=250
  ```

  El árbol de relojes se guarda al crear el stream y se restaura al cerrarlo. Mientras el stream está abierto, los
  periféricos alimentados por el reloj de la VPU (mini-UART, I2C, SPI y el controlador SD de las Pi Zero/1/3)
  funcionan a una frecuencia distinta de la que cree el kernel.
- **Memoria de la GPU.** Cada posición del anillo DMA usa 140 bytes de memoria VideoCore (páginas de 4 KiB):
  4096 posiciones ≈ 560 KiB, 32768 posiciones ≈ 4,4 MiB. Mantenga `gpu_mem` en su valor por defecto (≥ 64 MB) para
  anillos grandes.
- **GPIO4 debe estar libre.** Si un driver del kernel reclama GPIO4 — en particular el overlay `w1-gpio` (1-Wire),
  cuyo pin por defecto es GPIO4 — el driver rechaza el stream.
- **El bloque PCM/I2S debe estar libre.** Se usa como reloj de cadencia del DMA; con un overlay de placa de sonido
  I2S o `dtparam=i2s=on` el stream se rechaza.
- **Un solo transmisor por sistema**, garantizado con un `flock()` sobre `/run/lock/soapy_rpitx.lock`.
- **Las funciones de GPIO 0–9 quedan congeladas durante la transmisión.** El DMA reescribe todo el registro
  `GPFSEL0` en cada muestra (es la única forma de conmutar el pin de reloj desde el DMA). Las funciones de GPIO 0–9
  se capturan al activar el stream; no las reconfigure (I2C en 2/3, SPI en 7–9, …) mientras transmite.
- **La corriente de salida de GPIO 0–27 sigue a la envolvente.** El registro de pads `PADS_GPIO_0` es compartido
  por GPIO 0–27 y se reescribe en cada muestra (se conserva la histéresis de entrada). Las muestras en reposo y el
  final del stream restauran el valor original.

---

## Compilación en la Raspberry Pi

### Dependencias

```sh
sudo apt update
sudo apt install git cmake g++ libsoapysdr-dev soapysdr-tools
# opcional, para las herramientas en Python
sudo apt install python3-numpy python3-soapysdr python3-libgpiod
```

### Compilar e instalar

```sh
git clone https://github.com/hiperiondev/SoapySDR_rpitx.git
cd SoapySDR_rpitx
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # opcional: pruebas en el host sobre hardware simulado
sudo cmake --install build
```

En una Pi Zero / Pi 1 use `-j1` (512 MB de RAM). Allí la compilación tarda unos minutos; en una Pi 4, bastante menos
de un minuto.

Si SoapySDR se compiló desde el código fuente en `/usr/local`, configure con `-DCMAKE_INSTALL_PREFIX=/usr/local`.

### Opciones de CMake

| Opción | Por defecto | Descripción |
|--------|-------------|-------------|
| `RPITX_BUILD_DRIVER` | `ON` | Compila el módulo SoapySDR (requiere SoapySDR) |
| `RPITX_BUILD_RESET_TOOL` | `ON` | Compila e instala `rpitx_reset` |
| `RPITX_INSTALL_TOOLS` | `ON` | Instala `rpitx_tone_test.py` y `hpsdr_rpitx_bridge.py` |
| `BUILD_TESTING` | `ON` (`OFF` en compilación cruzada) | Compila las pruebas para el host |
| `RPITX_WERROR` | `OFF` | Trata las advertencias (`-Wall -Wextra -Wshadow`) como errores |

SoapySDR se localiza mediante su paquete CMake (`SoapySDRConfig.cmake`, que provee `SOAPY_SDR_MODULE_UTIL`). Si
falta la configuración del paquete (típicamente un sysroot cruzado incompleto), la compilación recurre a buscar
directamente `SoapySDR/Device.hpp` y `libSoapySDR.so` y lee la versión de ABI de módulos de `SoapySDR/Version.h`.

---

## Compilación cruzada

### Con `rpi-cross.sh` (host x86_64 Fedora o derivado de Debian)

`rpi-cross.sh` conoce todos los modelos de Pi (CPU, ISA, ABI de punto flotante, triplete multiarch de Debian),
instala el toolchain cruzado, construye un sysroot a partir de una Pi en funcionamiento, genera un archivo de
toolchain de CMake y verifica el resultado.

```sh
./rpi-cross.sh models                                   # lista los modelos y los flags del compilador
./rpi-cross.sh deps    -m pi4 -b 64                     # instala el toolchain cruzado: dnf en Fedora, apt en Debian/Ubuntu/Mint (autodetectado)
./rpi-cross.sh sysroot -m pi4 -b 64 -H pi@raspberrypi.local --pi-packages "libsoapysdr-dev" --ask-pass
./rpi-cross.sh build   -m pi4 -b 64                     # -> build-rpi-pi4-64/
./rpi-cross.sh verify  -m pi4 -b 64 build-rpi-pi4-64/librpitxSupport.so
./rpi-cross.sh install -m pi4 -b 64 -H pi@raspberrypi.local --ask-pass   # compila e instala en la Pi por SSH
```

- `-m` selecciona la placa (`pi1 pizero cm1 pi2 pi2v12 pi3 pi3plus cm3 pizero2w pi4 pi400 cm4 pi5 pi500 cm5`); `-b` debe coincidir
  con el **userland** de la Pi (`dpkg --print-architecture`: `armhf` → 32, `arm64` → 64).
- `sysroot` sincroniza con rsync `/usr/include`, `/usr/lib` (sin módulos del kernel ni firmware), `/usr/share/cmake`,
  `/usr/share/pkgconfig`, `/usr/local` y `/lib` desde la Pi a `~/rpi-sysroot/<arch>` y convierte en relativos los
  enlaces simbólicos absolutos. `--pi-packages` instala antes paquetes en la Pi; `libsoapysdr-dev` es obligatorio.
  `--ask-pass` permite al script usar `sudo` en la Pi para leer archivos solo legibles por root (el ayudante que
  contiene la contraseña se crea en un tmpfs y se elimina después).
- Pi 1 / Zero / CM1 son ARMv6: los compiladores armhf de Fedora y de Debian/Ubuntu generan código ARMv7. Use un toolchain compatible con
  ARMv6 (por ejemplo un toolchain glibc `armv6-eabihf` de Bootlin) con `--toolchain-dir <dir> --prefix arm-linux-`.
- Los compiladores cruzados `gcc-<arch>-linux-gnu` de Fedora están pensados para compilar kernels y no incluyen
  `libgcc_s` para el destino: el script lo detecta y enlaza la `libgcc_s.so.1` de la Pi desde el sysroot (lo
  muestra `info`); de lo contrario los programas C++ fallan con `_Unwind_Resume` sin definir.
- La distribución del host se lee de `/etc/os-release` (`ID` / `ID_LIKE`): Fedora, RHEL y derivados usan `dnf`;
  Debian, Ubuntu, Linux Mint, Pop!_OS y otros derivados usan `apt` (`g++-aarch64-linux-gnu` o
  `g++-arm-linux-gnueabihf`). `--distro fedora|debian` (o `RPI_DISTRO`) fuerza la elección; `info` muestra lo que se
  detectó. En Fedora el sysroot es obligatorio (su cross-gcc no incluye libc del destino); en Debian/Ubuntu también se
  usa para que la compilación vea los encabezados y bibliotecas exactos de la Pi.
- `install` compila el proyecto (se omite con `--no-build`), comprueba el resultado con `verify` y lo instala en la Pi
  en funcionamiento mediante una sola conexión SSH: `librpitxSupport.so` va al directorio de módulos de SoapySDR (el
  primer `Search path` que informa `SoapySDRUtil --info` en la Pi, p. ej.
  `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8`; se cambia con `--module-dir`), y `rpitx_reset` y las herramientas
  Python van a `/usr/local/bin` (`--pi-prefix`, `--no-tools`). Se detiene si el userland de la Pi o la ABI de SoapySDR
  no coinciden con la compilación (`--force` lo ignora), instala `soapysdr-tools` en la Pi si falta `SoapySDRUtil` e
  instala allí los `--pi-packages`. `sudo` en la Pi funciona igual que en `sysroot` (`--ask-pass`, `--pi-password`,
  sudo sin contraseña o acceso como root) y no hace falta reiniciar.
- `cmake-toolchain -o archivo.cmake` solo escribe el archivo de toolchain, para usarlo en un IDE u otra compilación.
- `verify` comprueba la arquitectura, la ABI de punto flotante y que las versiones de símbolos de glibc / libstdc++
  requeridas existan en el sysroot.

En compilación cruzada `LIB_SUFFIX` se fuerza a vacío: SoapySDR 0.7.x derivaría `lib64` del host de compilación y no
encontraría la biblioteca en el `usr/lib/<multiarch>` del sysroot.

### Manualmente (cualquier host Linux)

1. Instale un compilador cruzado que coincida con el userland de la Pi, por ejemplo en Debian/Ubuntu
   `sudo apt install g++-aarch64-linux-gnu` (arm64) o `g++-arm-linux-gnueabihf` (armhf, ARMv7 o superior).
2. Copie un sysroot desde la Pi (después de `sudo apt install libsoapysdr-dev` en ella):

   ```sh
   mkdir -p ~/rpi-sysroot
   rsync -a --exclude=/modules/ --exclude=/firmware/ pi@raspberrypi.local:/usr/lib     ~/rpi-sysroot/usr/
   rsync -a pi@raspberrypi.local:/usr/include ~/rpi-sysroot/usr/
   rsync -a pi@raspberrypi.local:/usr/share/cmake ~/rpi-sysroot/usr/share/
   ln -sfn usr/lib ~/rpi-sysroot/lib
   ```

   Los enlaces simbólicos absolutos dentro del `usr/lib` copiado (por ejemplo `libm.so`) deben convertirse en
   relativos, o bien indicar al enlazador los archivos correctos con `-Wl,-rpath-link`.
3. Escriba un archivo de toolchain:

   ```cmake
   # rpi-arm64.cmake
   set(CMAKE_SYSTEM_NAME Linux)
   set(CMAKE_SYSTEM_PROCESSOR aarch64)
   set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
   set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
   set(CMAKE_SYSROOT $ENV{HOME}/rpi-sysroot)
   set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
   set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
   set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
   set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
   ```

4. Compile:

   ```sh
   cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=rpi-arm64.cmake -DCMAKE_BUILD_TYPE=Release
   cmake --build build-arm64 -j
   ```

### Despliegue de una compilación cruzada

`rpi-cross.sh install` hace todo esto automáticamente. Para hacerlo a mano, copie los resultados a la Pi:

```sh
scp build-rpi-pi4-64/librpitxSupport.so build-rpi-pi4-64/rpitx_reset pi@raspberrypi.local:
ssh pi@raspberrypi.local
SoapySDRUtil --info | grep -A3 "Search path"       # muestra el directorio de módulos
sudo cp librpitxSupport.so /usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/   # use la ruta mostrada arriba
sudo cp rpitx_reset /usr/local/bin/
```

---

## Instalación y verificación

`cmake --install` coloca:

| Archivo | Destino |
|---------|---------|
| `librpitxSupport.so` | `<libdir>/SoapySDR/modules<ABI>/` (el propio directorio de módulos de SoapySDR, p. ej. `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/`) |
| `rpitx_reset` | `<prefix>/bin` |
| `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py` | `<prefix>/bin` |

No hace falta reiniciar ni copiar nada a mano. Verifique:

```sh
SoapySDRUtil --info                         # "rpitx" en "Available factories"
sudo SoapySDRUtil --find="driver=rpitx"     # modelo y número de serie de la placa
sudo SoapySDRUtil --probe="driver=rpitx"    # canales, rangos, ajustes, sensores
sudo rpitx_tone_test.py --freq 144.800M     # tono NBFM de prueba de 30 s (¡carga fantasma!)
```

---

## Uso

Cadena de dispositivo: **`driver=rpitx`**. Las aplicaciones deben ejecutarse como root.

### Python

```python
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_TX, SOAPY_SDR_CF32, SOAPY_SDR_END_BURST

sdr = SoapySDR.Device(dict(driver="rpitx", amplitude_mode="constant"))
sdr.setSampleRate(SOAPY_SDR_TX, 0, 48000)
sdr.setFrequency(SOAPY_SDR_TX, 0, 144.800e6)
sdr.setGain(SOAPY_SDR_TX, 0, 7)

st = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0], {"preroll_ms": "100"})
sdr.activateStream(st)

fs, dev, tone = 48000, 2500, 1000
n = np.arange(fs * 5)                                      # tono NBFM de 5 s
iq = np.exp(1j * (dev / tone) * np.sin(2 * np.pi * tone * n / fs)).astype(np.complex64)
for i in range(0, len(iq), 4096):
    sr = sdr.writeStream(st, [iq[i:i + 4096]], len(iq[i:i + 4096]), timeoutUs=1000000)

sdr.writeStream(st, [np.zeros(0, np.complex64)], 0, SOAPY_SDR_END_BURST)
sdr.deactivateStream(st)                                    # espera a que se envíe la ráfaga
sdr.closeStream(st)
```

### GNU Radio y SDRangel

- **GNU Radio:** *Soapy Custom Sink* (o *Soapy Sink*), dispositivo `driver=rpitx`, tipo `fc32`, frecuencia de
  muestreo 48000, la frecuencia central y argumentos de stream opcionales como `preroll_ms=150,fifo_ms=300`.
  Ejecute `gnuradio-companion` o el script generado como root.
- **SDRangel:** agregue un dispositivo *SoapySDR output* y seleccione el dispositivo rpitx; los ajustes aparecen en el
  panel del dispositivo.

### Semántica del stream

- `writeStream()` se bloquea mientras la FIFO está llena, respeta `timeoutUs`, puede aceptar menos muestras de las
  ofrecidas (el valor de retorno es la cantidad consumida) y devuelve `SOAPY_SDR_TIMEOUT` cuando no pudo encolar nada
  a tiempo. `SOAPY_SDR_HAS_TIME` no está soportado (`SOAPY_SDR_NOT_SUPPORTED`). La MTU es `min(4096, max(FIFO/4, 256))`.
- `SOAPY_SDR_END_BURST` en la última escritura (o en una escritura de longitud cero) marca el fin de la ráfaga: el DMA
  arranca de inmediato aunque no se haya alcanzado el pre-roll, y tras la última muestra la portadora queda apagada.
- `deactivateStream()` apaga la portadora. Si la última escritura llevaba `END_BURST`, primero espera (como máximo la
  duración de la FIFO más el anillo más 200 ms) a que se hayan reproducido todas las muestras encoladas; mientras
  tanto no retiene el mutex del dispositivo, así que la API de control sigue disponible. Sin `END_BURST`, las
  muestras encoladas se descartan.
- `readStreamStatus()` devuelve `SOAPY_SDR_UNDERFLOW` (máscara de canal 1) una vez por cada nuevo vaciado de la FIFO
  dentro de una ráfaga, overrun del DMA o reinicio del DMA; si no, `SOAPY_SDR_TIMEOUT`.
- La frecuencia, la frecuencia de muestreo, los ppm y la armónica pueden cambiar durante la transmisión: el
  alimentador detiene el DMA, apaga la portadora, reprograma el PLL y el reloj de cadencia y reanuda con las muestras
  que ya estaban en el anillo DMA (ninguna se pierde; como mucho una se envía dos veces), seguidas por la FIFO.

### Argumentos del stream

| Clave | Por defecto | Rango | Descripción |
|-------|-------------|-------|-------------|
| `ring_size` | 85 ms de muestras, ≥ 4096, redondeado a 512 | 512 – 32768, y ≥ 10 ms a la frecuencia de muestreo | Tamaño del anillo DMA en muestras |
| `fifo_ms` | 250 | 20 – 5000 | Capacidad de la FIFO de software en ms (sigue los cambios posteriores de frecuencia de muestreo) |
| `preroll_ms` | 100 | 0 – 5000 | Señal encolada antes de arrancar el DMA |
| `dma_channel` | -1 | -1 – 14 | Canal DMA; -1 = primer canal libre que no pertenece al kernel |
| `dma_force` | false | bool | Reinicia un canal elegido explícitamente aunque esté activo |
| `rt_priority` | 50 | 0 – 99 | Prioridad `SCHED_FIFO` del hilo alimentador (0 = planificación normal) |
| `cpu` | -1 | -1 – 63 | CPU a la que se fija el hilo alimentador |
| `signal_cleanup` | false | bool | Apaga la portadora cuando el proceso muere por una señal con la acción por defecto |

Latencia desde `writeStream()` hasta la antena ≈ llenado de la FIFO + ¾ del anillo (≈ 64 ms con un anillo de 4096
posiciones a 48 kHz). Una aplicación que escribe más rápido que el tiempo real mantiene la FIFO llena: latencia ≈
`fifo_ms` + ¾ del anillo.

### Ajustes (también aceptados como argumentos del dispositivo)

| Clave | Por defecto | Descripción |
|-------|-------------|-------------|
| `ppm` | 0 | Corrección manual del cristal, −200 … 200 ppm. **Positiva cuando el cristal es lento**, es decir, cuando la placa transmite por debajo de la frecuencia pedida (mismo signo que la estimación NTP del kernel) |
| `ppm_ntp` | true | Usa la estimación de frecuencia NTP del kernel como corrección base (se suma a `ppm`) |
| `harmonic` | 1 | Transmite en esta armónica de la onda cuadrada: el PLL funciona a frecuencia / armónica (1 – 15) |
| `amplitude_mode` | envelope | `envelope` o `constant` |
| `iq_scale` | 1.0 | Ganancia aplicada a \|I/Q\| antes de cuantizar la amplitud (modo envelope), 0 – 100 |
| `debug_level` | 0 | Diagnóstico de librpitx en stderr: 0 errores, 1 configuración, 2 volcados de registros |

Ejemplo: `driver=rpitx,amplitude_mode=constant,ppm=1.5`. Los argumentos de dispositivo desconocidos se informan con
una advertencia.

### Sensores

| Clave | Tipo | Descripción |
|-------|------|-------------|
| `pll_locked` | bool | Bit de enganche de PLLC (`CM_LOCK.FLOCKC`) |
| `dma_running` | bool | El alimentador arrancó el DMA y el bit `ACTIVE` del canal está activo |
| `fifo_fill` | float % | Nivel de llenado de la FIFO de software |
| `underflows` | int | La FIFO se vació en medio de una ráfaga (aplicación demasiado lenta) |
| `late` | int | El motor DMA alcanzó al alimentador (hilo alimentador demorado) |
| `dma_errors` | int | El motor DMA se detuvo solo y fue reiniciado |
| `sample_rate` | float Hz | Frecuencia producida realmente por los divisores del reloj PCM |

---

## Herramientas

### `rpitx_tone_test.py`

Transmisor de prueba: tono NBFM, o un archivo WAV modulado en FM, AM, USB, LSB o enviado como I/Q (WAV estéreo,
I = izquierdo, Q = derecho). Muestra los sensores cada segundo.

```sh
sudo rpitx_tone_test.py --freq 144.8M                         # tono NBFM de 1 kHz durante 30 s
sudo rpitx_tone_test.py --freq 7.074M --wav voz.wav --mod usb
sudo rpitx_tone_test.py --freq 144.8M --jitter-ms 20 --args "preroll_ms=200,fifo_ms=500"
sudo rpitx_tone_test.py --freq 144e6 --seconds 0              # solo abre/cierra: recupera una sesión caída
```

### `rpitx_reset`

Devuelve el hardware al reposo después de que muriera un proceso que usaba el driver: recupera el registro de sesión
(ver [Recuperación tras una caída](#recuperación-tras-una-caída)); sin registro, apaga una salida de reloj que haya
quedado en GPIO4 y detiene GPCLK0. `--dma-channel N` además reinicia el canal DMA N (se rechaza si pertenece al
kernel). Apto para `ExecStopPost=` en una unidad de systemd.

### `hpsdr_rpitx_bridge.py` — puente HPSDR Protocol 1

Hace que la Pi aparezca en la red como una radio openHPSDR Protocol 1, de modo que piHPSDR, Thetis, PowerSDR mRX,
linHPSDR, Quisk o SparkSDR la descubren y transmiten a través de rpitx.

```sh
sudo hpsdr_rpitx_bridge.py                          # sigue la frecuencia TX del host, emulación Hermes
sudo hpsdr_rpitx_bridge.py --freq 144.8M            # frecuencia TX fija
sudo hpsdr_rpitx_bridge.py --freq-offset 116M       # transverter: host 28,8 MHz -> 144,8 MHz
sudo hpsdr_rpitx_bridge.py --amplitude-mode constant --follow-drive
sudo hpsdr_rpitx_bridge.py --key-dot-gpio 17 --key-dash-gpio 27 --ptt-gpio 22
hpsdr_rpitx_bridge.py --dry-run --verbose           # solo protocolo, sin transmisor, sin root
```

| Área | Implementación |
|------|----------------|
| Descubrimiento | UDP broadcast/dirigido en el puerto 1024 y TCP; MAC, firmware, id de placa, receptores; bytes extra de Hermes-Lite 2 |
| Placas | `metis`, `hermes` (por defecto), `hermes2`/`griffin`, `angelia`, `orion`, `orion2`, `hermeslite` |
| Inicio / parada | I/Q EP6 y wideband EP4, por UDP o TCP; otro host toma el control con un inicio; una parada desde otro host se ignora |
| C&C del host (EP2) | Todas las direcciones decodificadas (frecuencia, receptores, dúplex, NCO de TX/RX, potencia, filtros, atenuadores, micrófono, PureSignal, manipulador CW, extensiones HL2); `--verbose` registra los cambios |
| Estado (EP6) | PTT/punto/raya, entradas IO inactivas, firmware, potencia directa (`--fwd-counts`), tensión de alimentación, temperatura HL2, nivel de la FIFO TX y confirmaciones |
| I/Q de recepción | Ruido sintético o ceros a 48/96/192/384 kHz, micrófono en silencio; cadenciado para que el stream TX del host vaya a 48 kHz |
| Transmisión | I/Q TX del host mientras MOX está activo, en el NCO de TX (o RX1 cuando el dúplex está desactivado) |
| CW | Manipulador iámbico A/B o recto con paletas en GPIO y temporización del host, CWX de HL2, entrada PTT por GPIO |
| Watchdog | El streaming se detiene y se libera el TX tras `--host-timeout` s sin paquetes del host |

Notas: en SSB con el micrófono en silencio no hay portadora (en modo `envelope` las magnitudes por debajo de
0,5 / (PAD + 1) del fondo de escala apagan la portadora; el puente muestra un aviso). Con la opción *CW handled in
Radio* de piHPSDR el host no activa MOX en CW: conecte paletas a las entradas GPIO o desactive esa opción. piHPSDR
limita las radios Protocol 1 a 61,44 MHz; use `--freq-offset` o un LO de transverter para VHF.

---

## Funcionamiento interno

### Visión general

```
 hilo de la aplicación           hilo alimentador (SCHED_FIFO 50, periodo 1 ms)     motor DMA (hardware)
 ─────────────────────           ──────────────────────────────────────────────     ────────────────────
 writeStream()                    lee la posición del DMA (CONBLK_AD)                por posición del anillo:
   │ CS16/CS8 → CF32               │ seguimiento de posición absoluta (vueltas,       CB0  PAD      ← corriente
   ▼                               │ overruns)                                        CB1  PLLC_FRAC← frecuencia
 SpscFifo<complex<float>> ───────► │ borra con reposo las posiciones reproducidas     CB2  GPFSEL0  ← portadora on/off
 (sin bloqueos, contrapresión)     │ IQ → (amplitud, Δφ) → palabras de registro ──►   CB3  FIFO PCM ← cadencia por DREQ
                                   │ mantiene el anillo a ¾, relleno en reposo         └─ next → lazo cerrado
                                   │ retune / reinicio del DMA / underflows
```

Cada muestra I/Q se convierte en una **posición del anillo**: tres palabras de 32 bits en la memoria de VideoCore
(multiplicador fraccional del PLL, palabra de corriente de los pads, palabra de `GPFSEL0`) y cuatro bloques de
control DMA que las copian a los periféricos. El cuarto bloque de control escribe una palabra ficticia en la FIFO de
transmisión del PCM y está cadenciado por la línea DREQ del PCM, de modo que el motor avanza exactamente una posición
por periodo de muestra. El último bloque de control apunta de nuevo al primero: el DMA reproduce el anillo
indefinidamente sin intervención de la CPU, y el hilo alimentador solo tiene que mantenerse por delante.

### Síntesis de frecuencia: modulación de PLLC

La portadora es GPCLK0 alimentado por **PLLC**. Al crear un stream, el driver activa el "modo PLL avanzado" de
librpitx:

1. Se guardan los registros del árbol de relojes que van a cambiar (`CORECLK_CNTL/DIV`, `EMMCCLK_CNTL/DIV`,
   `CM_GP0CTL/DIV`, `CM_PLLC`, `PLLC_CTRL/FRAC`, `PLLC_CORE0/1/2`, `PLLC_PER`, `A2W_PLLC_ANA0..3`) y se anotan en el
   archivo de sesión.
2. El reloj del núcleo VPU se mueve a PLLA (divisor 4) y el reloj EMMC a PLLD, para que nada más dependa de PLLC.
3. Se desactiva `PLLC_CORE0`, se habilita el canal de periféricos de PLLC con divisor 1 (2 en BCM2711) y GPCLK0 toma
   de él su fuente, con MASH 0 (divisor entero, sin espurias de modelado de ruido).

Para una portadora F (frecuencia del PLL F/armónica), `ComputeBestLO()` elige el divisor entero D de GPCLK0: recorre
D de 4095 a 2 buscando una frecuencia de VCO F·D entre 200 MHz y 1,5 GHz tal que toda la banda F ± B
(B = frecuencia de muestreo) corresponda al mismo multiplicador **entero** del PLL. Por encima de una portadora de
750 MHz (375 MHz en BCM2711) se usa en cambio el pre-divisor del PLL (divisor de GPCLK 2). El multiplicador del VCO es

```
M = (F + f) · K / (XOSC · (1 − ppm·10⁻⁶))          f = desplazamiento instantáneo respecto de la portadora
K = D (BCM283x),  2·D (BCM2711, PLLC_PER = /2),  ½ o 2 con el pre-divisor
XOSC = 19,2 MHz (BCM283x) o 54 MHz (BCM2711)
```

`M` se divide en la parte entera `PLLC_CTRL.NDIV` (programada una vez por sintonía) y la parte fraccional de 20 bits
`PLLC_FRAC`, el único registro que reescribe el DMA. La resolución en frecuencia es `XOSC / 2²⁰ / K`: p. ej. 1,83 Hz
a 144 MHz en una Pi 3 (D = 10) y 5,15 Hz en una Pi 4 (D = 5, K = 10). Si un desplazamiento requiriera otro
multiplicador entero, el valor fraccional se satura en 0 o 0xFFFFF en lugar de dar la vuelta; cuando el divisor
elegido no puede mantener toda la banda dentro de un escalón entero, se registra una advertencia. Tras programar, se
espera hasta 2 ms el enganche de PLLC (`CM_LOCK.FLOCKC`) y se registra una advertencia si no engancha.

Los generadores de reloj (GPCLK0, PCM, EMMC) siempre se reprograman con la secuencia del hardware: borrar `ENAB`,
esperar a que se borre `BUSY` (kill tras 1 ms), escribir divisor y fuente, habilitar.

### De I/Q a palabras de registro

Para cada muestra el alimentador calcula (`dsp::pushsample()`):

- la **amplitud** a = |s|;
- la **frecuencia instantánea** f = wrap(arg(sₙ) − arg(sₙ₋₁)) · fs / 2π, con la diferencia de fase acotada a
  [−π, π), de modo que f ∈ [−fs/2, fs/2).

Las muestras por debajo de un umbral de magnitud de 1e-3 tienen fase sin sentido: informan f = 0 (portadora en la
frecuencia central) y se conserva la última fase válida, de modo que la siguiente muestra válida aplica **todo** el
cambio de fase a través del hueco. Por eso una inversión BPSK que pasa exactamente por cero sigue girando la
portadora en π.

El PLL realiza una frecuencia cuantizada. El driver calcula la frecuencia producida realmente por la palabra
fraccional elegida (`GetMasterFracFrequency()`), y el error de fase resultante **se traslada a la siguiente
muestra** (realimentación de error de primer orden, acotada a ±0,5 rad), de modo que la fase de la portadora sigue a
la fase I/Q sin deriva en transmisiones PSK largas.

Las tres palabras de una posición de datos:

| Palabra | Valor |
|---------|-------|
| `PLLC_FRAC` | `0x5A000000 \| frac(f / armónica + arrastre)` |
| `PADS_GPIO_0` | `0x5A000000 \| HYST(original) \| SLEW \| nivel` — modo envelope: `nivel = round(min(a·iq_scale, 1)·(drive+1)) − 1`, portadora apagada si es negativo; modo constant: `nivel = drive` |
| `GPFSEL0` | Funciones de GPIO 0–9 capturadas al activar, con GPIO4 = ALT0 (portadora encendida) o entrada (portadora apagada) |

Una **posición en reposo** contiene la palabra fraccional de la frecuencia central, el valor original de los pads y
GPIO4 = entrada: el GPCLK sigue funcionando internamente, pero nada sale por el pin.

### Cadencia de muestreo: el bloque PCM

El controlador PCM/I2S se configura como transmisor de un canal y 8 bits cuya longitud de trama (`PCM_MODE.FLEN`) es
el menor valor ≥ 10 que mantiene el divisor del reloj PCM entre 2 y 4095. El reloj PCM proviene de PLLD con MASH 1 y
un divisor fraccional de 12 bits, corregido con los mismos ppm que la portadora. El umbral DREQ de la FIFO TX es de
64 palabras; cada trama consume una palabra, por lo que cada escritura de `CB3` se bloquea hasta que la FIFO tiene
lugar — una posición por trama. La frecuencia producida (sensor `sample_rate`) difiere de la nominal por la
cuantización del divisor, menos de 1 ppm.

### Motor DMA y memoria

- **Memoria:** una única reserva contigua de VideoCore (`mem_alloc` con `MEM_FLAG_DIRECT` en BCM2836 y posteriores,
  el alias sin caché `0xC`, o `MEM_FLAG_L1_NONALLOCATING` en BCM2835; `HINT_PERMALOCK`), bloqueada para obtener su
  dirección de bus y mapeada en el proceso mediante `/dev/mem`. Primero los bloques de control (alineados a
  32 bytes), luego las palabras de las posiciones: `anillo × (4·32 + 3·4)` = 140 bytes por posición. Cada respuesta
  del mailbox se verifica (resultado del ioctl, código de respuesta, bit de respuesta de la etiqueta).
- **Bloques de control:** `TI = NO_WIDE_BURSTS | WAIT_RESP`, más `D_DREQ | PERMAP(2)` (PCM TX) para el bloque de
  cadencia; una transferencia de 4 bytes cada uno, con destino la dirección de bus del periférico (`0x7E102220`
  PLLC_FRAC, `0x7E10002C` PADS_GPIO_0, `0x7E200000` GPFSEL0, `0x7E203004` FIFO PCM).
- **Canal:** `CS = PRIORITY(7) | PANIC_PRIORITY(7) | DISDEBUG | ACTIVE`. Solo se usan canales clásicos (0–14; 0–10 en
  BCM2711, donde 11–14 son canales DMA4 con otro formato de bloque de control). Los canales del kernel
  (`brcm,dma-channel-mask` de cada nodo `soc/dma@…`) nunca se tocan; por defecto se usa el primero de 7, 8, 9, 10, 6, 5
  (BCM2711) o 14 … 5 que no sea del kernel ni esté activo. Un canal explícito que esté activo solo se reinicia con
  `dma_force=true`.

### El hilo alimentador

El alimentador se ejecuta cada 1 ms (`clock_nanosleep` sobre un plazo absoluto de `CLOCK_MONOTONIC`), con prioridad
`SCHED_FIFO` `rt_priority` y afinidad de CPU opcional. Las posiciones se siguen como **contadores absolutos de
64 bits** (`DmaRingTracker`), porque el motor solo informa su posición módulo el tamaño del anillo:

- `readPos` avanza la distancia modular desde la lectura anterior; cuando el tiempo transcurrido × frecuencia supera
  medio anillo, se suman las vueltas completas que una distancia modular no puede mostrar
  (`vueltas = round((esperado − delta) / anillo)`).
- `fill = writePos − readPos`; `fill ≤ 0` significa que el motor alcanzó al escritor: un **overrun** (`late`). Entonces
  todo el anillo, salvo la posición en ejecución, se sobrescribe con muestras en reposo y la escritura se reanuda a
  una distancia de guarda (`max(32, frecuencia/500)`, ≤ ¼ del anillo) por delante del motor.
- En cada ciclo **borra** las posiciones completadas que no fueron reescritas con datos más nuevos (las sobrescribe
  con reposo), de modo que si el alimentador se detiene alguna vez el motor reproduce silencio, nunca la vuelta
  anterior.
- Pasa muestras de la FIFO al anillo hasta un **llenado objetivo de ¾ del anillo**, nunca a menos de 2 posiciones de
  la que se está ejecutando.
- Cuando la FIFO está vacía y el llenado cae por debajo de `low = max(anillo/8, frecuencia/200)`, rellena con
  posiciones en reposo; si había una ráfaga en curso sin `END_BURST`, se cuenta un **underflow**.
- Si la posición del DMA es inválida o el canal deja de estar `ACTIVE` durante 5 lecturas seguidas, se considera que
  el motor se detuvo: el evento se registra con `DMA_CS` / `DMA_DEBUG`, se incrementa `dma_errors` y el stream se
  reanuda con las muestras no reproducidas.

**Arranque y pre-roll:** tras la activación, el alimentador llena el anillo con el DMA detenido. El DMA arranca en la
posición 0 cuando hay `preroll_ms` de señal encolada, cuando se señala el fin de ráfaga, cuando la FIFO está llena o
cuando la primera muestra encolada lleva `preroll_ms` esperando (así las ráfagas cortas también salen).

**Retune:** el hilo de control publica la petición con un número de secuencia y espera hasta 3 s a que el
alimentador la tome (y luego hasta que termine). El alimentador registra la posición exacta del motor, detiene el
DMA, **pasa GPIO4 a entrada y restaura los pads** antes de tocar el PLL, copia las posiciones no reproducidas del
anillo (las muestras I/Q originales se conservan en un arreglo sombra) a un búfer de repetición, reprograma PLLC y el
reloj PCM, reinicia el anillo y continúa: el búfer de repetición se envía antes que la FIFO. Una petición que el PLL
no puede satisfacer deja vigente la configuración anterior.

**Detección de vaciado:** `WaitDrained()` compara el contador de muestras introducidas en la FIFO con un contador de
muestras consumidas que el alimentador publica solo *después* de que las muestras están en el anillo y se actualizó
la posición de fin de datos, de modo que nunca se informa una ráfaga como enviada mientras sus últimas muestras están
en tránsito entre la FIFO y el anillo.

### La FIFO

`SpscFifo` es una cola acotada sin bloqueos de un productor y un consumidor con contadores de cabeza y cola de
64 bits (acquire/release), de modo que el nivel de llenado es siempre `head − tail`. Su almacenamiento se reserva
para `fifo_ms` a la frecuencia de muestreo máxima; la capacidad utilizable es `max(frecuencia·fifo_ms/1000, 1024)` y se
actualiza en cada retune. Un productor bloqueado espera en una variable de condición en intervalos de 2 ms, y el
alimentador lo despierta cuando libera espacio.

### Cierre, recuperación tras caídas y seguridad

- **Cierre normal** (`deactivateStream` / `closeStream` / destrucción del dispositivo): se espera al alimentador, se
  reinicia el DMA, GPIO4 vuelve a su función original (entrada si era la salida de reloj), se restauran los pads, se
  desactiva el bloque PCM y se restaura su reloj, se restaura el árbol de relojes (primero PLLC, esperando el enganche,
  luego sus canales, los relojes VPU y EMMC y GPCLK0), se libera la memoria VideoCore y se vacía el archivo de bloqueo.
- **`exit()`:** un manejador `atexit` realiza una parada de emergencia (reinicio del DMA, portadora apagada, GPCLK0
  detenido) sin invalidar los objetos, de modo que un destructor posterior todavía completa el cierre.
- **Señales** (`signal_cleanup=true`): manejadores para SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL
  y SIGABRT. Cuando la disposición previa era la acción por defecto (el proceso está por morir) realizan escrituras de
  registros seguras en señales (reinicio del DMA, GPIO4 a entrada, pads, GPCLK0 apagado), restauran la acción previa y
  vuelven a lanzar la señal; las señales ignoradas y los manejadores de la aplicación pasan sin cambios.

#### Recuperación tras una caída

Mientras existe un transmisor, `/run/lock/soapy_rpitx.lock` (tmpfs, nunca sobrevive a un reinicio) se mantiene con un
`flock()` exclusivo y contiene un registro `clave=valor` de todo lo que posee el proceso: PID, canal DMA, handle de
memoria VideoCore, valor original de los pads y función de GPIO4, reloj PCM original y el árbol de relojes guardado.
El kernel libera el bloqueo cuando el proceso muere, incluso con `kill -9`. El siguiente proceso que obtiene el
bloqueo y encuentra un registro no vacío sabe que el dueño murió sin limpiar y lo recupera: reinicio del canal DMA,
GPIO4 y pads restaurados, GPCLK0 detenido, PCM desactivado y su reloj restaurado, árbol de relojes restaurado, y la
memoria de GPU del proceso muerto liberada (los handles de VideoCore son globales para el firmware). `rpitx_reset`
hace lo mismo a pedido.

#### Comprobaciones de conflictos al abrir

- otro proceso con el bloqueo → "the transmitter is in use by process N";
- GPIO4 reclamado por cualquier consumidor en el chip GPIO del SoC (`pinctrl-bcm2…`, mediante la información de línea
  del dispositivo de caracteres GPIO);
- `soc/i2s@7e203000` habilitado en el device tree;
- el canal DMA pedido pertenece al kernel, o está activo sin `dma_force`;
- una advertencia cuando GPCLK0 ya está en marcha (otro programa podría estar usándolo).

### Higiene del módulo

El módulo SoapySDR se enlaza con visibilidad oculta y `--no-undefined`: los símbolos de la librpitx integrada no se
exportan, así que no puede chocar con otra librpitx en el mismo proceso. librpitx nunca llama a `exit()`; todo fallo
de hardware se convierte en un `std::runtime_error` que la aplicación recibe desde SoapySDR.

---

## Solución de problemas

| Síntoma | Causa y solución |
|---------|------------------|
| `No devices found` | `SoapySDRUtil --info` debe listar `rpitx` (directorio de módulos); ejecute como root; la familia Pi 5 no está soportada |
| "/dev/mem and the VideoCore mailbox are not accessible" | Ejecute como root (o dé al binario acceso a ambos dispositivos) |
| "the transmitter is in use by process N" | Otro programa posee el transmisor; ciérrelo |
| "DMA channel N is reserved for the Linux kernel" / "is active" | Elija otro `dma_channel` o deje -1; `dma_force=true` solo para un canal que se sabe libre |
| "GPIO4 is claimed by …" | Quite el overlay que usa el pin (p. ej. `dtoverlay=w1-gpio`) y reinicie |
| "the PCM/I2S controller is enabled" | Quite el overlay de placa de sonido / `dtparam=i2s=on` y reinicie |
| "VideoCore memory allocation failed" | Reduzca `ring_size`, aumente `gpu_mem` o ejecute `sudo rpitx_reset` para liberar memoria dejada por una caída |
| `underflows` aumenta | La aplicación es demasiado lenta: aumente `preroll_ms` y `fifo_ms`, escriba bloques más grandes |
| `late` aumenta | El alimentador se demoró: mantenga `rt_priority` > 0, fíjelo (`cpu=3`), considere `isolcpus=3` y el gobernador `performance` |
| `dma_errors` aumenta | Otra cosa reinició o usó el canal DMA; elija otro canal |
| "crosses a PLL integer step" | Las desviaciones más allá del escalón se recortan: mueva levemente la frecuencia o baje la frecuencia de muestreo |
| "PLLC did not report lock" | Las frecuencias muy altas pueden no enganchar en todos los chips; baje la frecuencia o use `harmonic` |
| Consola serie / I2C / SPI con errores durante la transmisión | Esperado: el reloj de la VPU está en PLLA mientras el stream está abierto (`gpu_freq=250` lo hace predecible) |
| Portadora trabada tras una caída | `sudo rpitx_reset` (o abra cualquier stream): se recupera el registro de sesión |
| Sin portadora en SSB con el puente HPSDR | Micrófono en silencio; vea las notas del puente más arriba |

---

## Pruebas

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Las pruebas se ejecutan en cualquier host Linux; `tests/mock_hw.cpp` reemplaza el mailbox, `/dev/mem`, las consultas al
device tree y el motor DMA (un hilo que ejecuta el anillo de bloques de control a la frecuencia de muestreo y aplica
sus palabras de `GPFSEL0`).

| Prueba | Cubre |
|--------|-------|
| `rpitx_unit_tests` | Orden de la FIFO con concurrencia y límite de capacidad; contabilidad del anillo contra un motor simulado (régimen estable, demoras cortas, vueltas perdidas, doble demora, falta de datos del host); estimación de frecuencia del DSP, umbral y continuidad de fase a través de cero |
| `rpitx_transmitter_tests` | Alimentador, pre-roll, vaciado, retune con la portadora apagada y repetición del anillo, parada de emergencia y luego destrucción, reinicio del DMA, histéresis de los pads, capacidad de la FIFO según la frecuencia, fallos del constructor (frecuencia inalcanzable, reserva rechazada, anillo corto), selección de canal DMA, bloqueo de sesión y recuperación tras caídas, manejadores de señales |
| `rpitx_driver_tests` | Dispositivo SoapySDR: ajustes y rangos, corrección de frecuencia, argumentos del stream, escalado CF32/CS16/CS8, vaciado con `END_BURST`, estado del stream, sensores |

La CI (`.github/workflows/ci.yml`) ejecuta las pruebas en una compilación normal, con AddressSanitizer +
UndefinedBehaviorSanitizer y con ThreadSanitizer (`tests/tsan.supp` suprime solo los accesos a registros simulados),
y verifica `clang-format`, `shellcheck` sobre `rpi-cross.sh` y `pyflakes` sobre las herramientas en Python.

---

## Estructura del proyecto

```
src/
  SoapyRPITX.hpp                 clase de dispositivo SoapySDR y envoltorio del stream
  SoapyRPITX_Registration.cpp    detección de la placa, enumeración, registro en SoapySDR
  SoapyRPITX_Settings.cpp        identificación, ajustes, antena, ganancia, frecuencia, frecuencia de muestreo
  SoapyRPITX_Streaming.cpp       configuración del stream, conversión de formatos, escritura / estado / ráfagas
  SoapySDR_Sensors.cpp           sensores
  SoapyRPITX_IQ_dmaSync.*        transmisor: anillo DMA, hilo alimentador, modulación, retune, cierre
  SoapyRPITX_Buffers.hpp         SpscFifo y DmaRingTracker (independientes del hardware)
  SoapyRPITX_Session.*           archivo de bloqueo, registro de caídas y recuperación, comprobación de conflictos
librpitx/src/                    librpitx integrada (dma, gpio/reloj/PCM/pads, mailbox, dsp, device tree)
tools/
  rpitx_reset.cpp                herramienta de recuperación
  rpitx_tone_test.py             transmisor de prueba (tono / WAV)
  hpsdr_rpitx_bridge.py          emulación de radio openHPSDR Protocol 1
tests/                           pruebas unitarias, del transmisor y del driver, simulación de hardware, supresiones de TSan
rpi-cross.sh                     ayudante de compilación cruzada (host Fedora / derivado de Debian)
```

---

## Licencia y créditos

GPL-3.0 — ver [LICENSE](LICENSE).

- Copyright © 2023 Emiliano Augusto Gonzalez (hiperion), <https://github.com/hiperiondev/SoapySDR_rpitx>.
- Basado en [librpitx](https://github.com/F5OEO/librpitx), Copyright © 2018 Evariste Courjaud F5OEO; la copia
  integrada y sus diferencias con el original se describen en [librpitx/README.md](librpitx/README.md).
- Estructura del dispositivo inspirada en [SoapyPlutoSDR](https://github.com/pothosware/SoapyPlutoSDR).
- Búsqueda del divisor del PLL adaptada de [PiFmRds](https://github.com/SaucySoliton/PiFmRds) (fork de SaucySoliton);
  ayudante de ángulos de [RadiantBee](https://github.com/f4gkr/RadiantBee) de F4GKR.
