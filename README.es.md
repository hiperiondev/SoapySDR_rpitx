# SoapySDR_rpitx

[English](README.md) | **Español** | [Italiano](README.it.md)

**Un driver de transmisión SoapySDR que convierte una Raspberry Pi en un transmisor I/Q de banda angosta — sin
hardware adicional, solo el pin de reloj GPIO4. Con un RTL-SDR y un relé se convierte en un transceptor
semidúplex.**

SoapySDR_rpitx expone la salida de reloj de propósito general de la Raspberry Pi (GPCLK0 en GPIO4) como un
dispositivo TX estándar de [SoapySDR](https://github.com/pothosware/SoapySDR). Cualquier aplicación compatible con
Soapy — GNU Radio, SDRangel, scripts en Python, el puente HPSDR incluido para piHPSDR / Thetis / linHPSDR / Quisk /
SparkSDR — puede enviarle muestras complejas en banda base y obtener RF en el pin del conector. Con `rx=rtlsdr` un
dongle RTL-SDR pasa a ser el canal RX del mismo dispositivo, y `relay_gpio` maneja un relé de antena T/R: el
dispositivo se anuncia y se comporta entonces como un transceptor TX/RX (véase
[Transceptor](#transceptor-receptor-rtl-sdr-y-relé-de-antena)). Está construido
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
- [Limitaciones según el modelo de Raspberry Pi](#limitaciones-según-el-modelo-de-raspberry-pi)
- [Requisitos y restricciones del sistema](#requisitos-y-restricciones-del-sistema)
- [Compilación en la Raspberry Pi](#compilación-en-la-raspberry-pi)
- [Compilación cruzada](#compilación-cruzada)
- [Instalación y verificación](#instalación-y-verificación)
- [Uso](#uso)
- [Herramientas](#herramientas)
- [Amplitud precisa y EER](#amplitud-precisa-y-eer)
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
| Dirección | Un canal TX; con `rx=<driver>` (un RTL-SDR) también un canal RX: un transceptor con streams simultáneos (full duplex) y conmutación de antena semidúplex |
| Receptor | Cualquier módulo receptor de SoapySDR (en la práctica un RTL-SDR) abierto por el driver; toda llamada RX se le reenvía; sus muestras se silencian mientras el transmisor está activado |
| Relé de antena | `relay_gpio`: GPIO de un relé T/R, conmutado a TX antes de la primera muestra de una transmisión (`relay_tx_delay_ms`) y de vuelta a RX tras la última (`relay_hang_ms`) |
| Línea PTT | `ptt_gpio`: GPIO que activa un amplificador o una radio, afirmado `relay_tx_delay_ms` después de que el relé pasó a TX y `ptt_tx_delay_ms` antes de la primera muestra; se libera antes de que el relé vuelva a RX (`ptt_rx_delay_ms`) |
| Formatos de muestra | `CF32` (nativo, fondo de escala 1.0), `CS16` (fondo de escala 32768), `CS8` (fondo de escala 128) |
| Frecuencia de muestreo | 10 kHz – 200 kHz (cualquier valor; listadas 24 k / 48 k / 96 k / 192 k; por defecto 48 kHz) |
| Frecuencia | 50 kHz – 1500 MHz (BCM2711: 50 kHz – 1000 MHz) en la fundamental; `harmonic=n` multiplica el rango |
| Modulación | Cualquier señal I/Q: fase/frecuencia mediante el multiplicador fraccional de PLLC, amplitud mediante la corriente de salida de los pads (8 niveles, tablas calibradas, conformación de ruido sigma-delta opcional y un segundo pin de reloj) o un DAC de envolvente externo (EER) |
| Modos de amplitud | `envelope` (la corriente sigue a \|I/Q\|) o `constant` (corriente máxima siempre que haya muestra: FM, FSK, CW, PSK de envolvente constante); salida de envolvente `none` (corriente de los pads), `pcm` (DAC I2S de 16 bits) o `pwm` |
| Ganancia | Elemento `PAD`, 0–7 = 2 mA … 16 mA de corriente de salida |
| Corrección de frecuencia | Estimación NTP del kernel (`ntp_adjtime`) más ppm manual; `setFrequencyCorrection()` |
| Ráfagas | `SOAPY_SDR_END_BURST` respetado: la desactivación espera a que la ráfaga haya salido por la antena |
| Streaming | FIFO sin bloqueos, hilo alimentador de tiempo real, pre-roll configurable, contrapresión con timeouts, escrituras parciales |
| Cambios en vivo | Frecuencia, frecuencia de muestreo, ganancia y todos los ajustes durante la transmisión, sin perder muestras encoladas |
| Diagnóstico | Sensores de enganche del PLL, estado del DMA, llenado de la FIFO, underflows, overruns del DMA, reinicios del DMA, frecuencia de muestreo real, recortes de la envolvente y el estado de transmisión (PTT); `readStreamStatus()` informa `SOAPY_SDR_UNDERFLOW` |
| Seguridad | Bloqueo a nivel de sistema, registro de caídas y recuperación automática, limpieza opcional ante señales, herramienta `rpitx_reset`, rechazo de usuarios en conflicto de GPIO / I2S / DMA |
| Integraciones | GNU Radio (Soapy sink / source), SDRangel (salida / entrada SoapySDR), Python, programas host HPSDR Protocol 1 mediante el puente (como transceptor con `--rx`) |

Lo que el hardware hace bien: **NBFM, FSK/AFSK, modos digitales de envolvente constante tipo WSPR/FT8, CW y BPSK**.
AM y SSB funcionan; con el pin solo, su calidad está limitada por los 8 escalones de amplitud (ver
[Amplitud precisa y EER](#amplitud-precisa-y-eer) para calibración, conformación de ruido y un modulador de
envolvente externo).

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

## Limitaciones según el modelo de Raspberry Pi

El driver solo se registra en las placas que puede manejar: lee la cadena de modelo y la dirección base de los
periféricos del device tree (`src/SoapyRPITX_Registration.cpp`, `librpitx/src/gpio.cpp`). La tabla muestra qué cambia
de una familia de SoC a otra; "—" significa que el driver no funciona en esa placa. Los valores salen del código
fuente (los umbrales de ancho de banda, de una réplica de su búsqueda de PLL); se agradecen los informes de placas
reales.

| | Pi 1 A/A+/B/B+, Zero, Zero W/WH, CM1 | Pi 2 B | Pi 3 B/B+/A+, CM3, Zero 2 W | Pi 4 B, Pi 400, CM4 | Pi 5, Pi 500, CM5 |
|---|---|---|---|---|---|
| **SoC** | BCM2835 | BCM2836 / BCM2837 | BCM2837 / BCM2837B0 / RP3A0 | BCM2711 | BCM2712 + RP1 |
| **Estado del driver** | Soportada | Soportada | Soportada | Soportada (solo modo de periféricos bajos) | **Rechazada en la detección** |
| **Base de periféricos** | `0x20000000` | `0x3F000000` | `0x3F000000` | `0xFE000000` | — |
| **Cristal (XOSC)** | 19,2 MHz | 19,2 MHz | 19,2 MHz | 54 MHz | — |
| **Rango de portadora** (`harmonic=1`) | 50 kHz – 1500 MHz | 50 kHz – 1500 MHz | 50 kHz – 1500 MHz | 50 kHz – **1000 MHz** | — |
| **Techo nominal con `harmonic=15`** | 22,5 GHz | 22,5 GHz | 22,5 GHz | 15 GHz | — |
| **Paso de frecuencia** (K = factor de escala del PLL) | 18,31 Hz / K | 18,31 Hz / K | 18,31 Hz / K | **51,5 Hz / K** | — |
| **Divisor periférico de PLLC** | /1 | /1 | /1 | /2 (/1 no funciona) | — |
| **Tasa de slots máxima (ancho de banda)** | 200 kHz | 200 kHz | 200 kHz | 200 kHz | — |
| **Portadora desde la cual 200 kHz siempre caben en un paso del PLL** | unos 2,8 MHz | unos 2,8 MHz | unos 2,8 MHz | unos **1,1 MHz** | — |
| **Canales DMA utilizables** | 0–14 (auto: 14 … 5) | 0–14 (auto: 14 … 5) | 0–14 (auto: 14 … 5) | **0–10** (auto: 7, 8, 9, 10, 6, 5); 11–14 son DMA4 | — |
| **Alias de memoria de VideoCore** | `0x40000000` (L1 sin asignación) | `0xC0000000` (directo) | `0xC0000000` (directo) | `0xC0000000` (directo) | — |
| **Relojes sacados de PLLC** | VPU, EMMC (divisor recalculado) | VPU, EMMC (divisor recalculado) | VPU, EMMC (divisor recalculado) | VPU, EMMC, **EMMC2** (tarjeta SD; divisores recalculados) | — |
| **Núcleos de CPU** | **1** (ARMv6) | 4 | 4 | 4 | — |
| **Hilo alimentador (SCHED_FIFO, 1 ms)** | Comparte el único núcleo con la aplicación: `preroll_ms` / `fifo_ms` mayores, aplicaciones livianas | Se puede fijar (`cpu=3`, `isolcpus=3`) | Se puede fijar | Se puede fijar | — |
| **Receptor USB local (autotune)** | Avisa (`--rx-local-policy`) | Avisa | Avisa | Sin aviso | — |
| **Objetivo de compilación cruzada** | Requiere toolchain ARMv6 | ARMv7 | ARMv7 / ARMv8 | ARMv8 / ARMv7 | — |

Notas:

- **Paso de frecuencia.** El multiplicador del PLL tiene 20 bits fraccionarios, así que el paso es `XOSC / 2²⁰ / K`.
  Por ejemplo, 1,83 Hz a 144 MHz en una Pi 3 (K = 10) frente a 5,15 Hz en una Pi 4 (K = 10): la Pi 4 tiene una grilla
  más gruesa, lo que importa en modos digitales de banda angosta.
- **Techo de la Pi 4.** Como PLLC_PER debe funcionar a /2, el driver programa el VCO a cuatro veces la portadora, y
  el límite de 4 GHz del VCO fija el máximo en 1000 MHz. Los demás SoC llegan al límite de 1500 MHz de los ajustes.
- **Techos nominales.** El driver solo impone los números de arriba (50 kHz × `harmonic` hasta 1500 MHz × `harmonic`).
  El nivel de la onda cuadrada cae con el número de armónico y con la frecuencia, y el PLL puede no engancharse en los
  extremos (aviso "PLLC did not report lock"); el techo realmente utilizable de cada placa no está caracterizado en
  este repositorio.
- **DMA de la Pi 4.** Solo se pueden usar los canales heredados 0–10 (11–14 son motores DMA4 con otro formato de
  bloque de control), y el kernel se queda con algunos; la elección automática prueba 7, 8, 9, 10, 6 y 5. Si la
  máscara del kernel no se puede leer, indique `dma_channel` explícitamente.
- **Modo de periféricos altos de la Pi 4.** Con `arm_peri_high=1` los periféricos quedan por encima del rango físico
  de 32 bits que mapea el driver, por lo que la placa se rechaza.
- **Placas de un solo núcleo.** Fijar el alimentador a un núcleo inexistente solo genera un aviso en el registro;
  deje `cpu=-1` en Pi 1 / Zero / CM1. Compile con `-j1`.
- **Familia Pi 5.** GPIO, los generadores de reloj y el DMA que manejan el conector están en el chip RP1, detrás de
  PCIe, y no en las direcciones de registros BCM283x/BCM2711 que programa librpitx. No existe una ruta GPCLK0 / PLLC
  que modular, así que el driver no se registra (`SoapySDRUtil --find` no lista nada) y ningún ajuste lo cambia.
- **Compute Modules.** GPIO4 debe estar llevado a un conector en su placa portadora.

### Ancho de banda según la frecuencia de portadora

Toda la banda instantánea de la señal, la portadora ± 0,58 × la tasa de slots, debe quedar dentro de **un paso entero
del multiplicador del PLL**; el paso a la salida es `XOSC / D`, donde D es el divisor de GPCLK0 elegido para la
portadora. Con portadoras bajas D no puede ser lo bastante grande (el VCO debe quedar entre 200 MHz y 1,5 GHz), y los
pasos se vuelven más angostos que una señal ancha. El driver no rechaza la frecuencia: registra "crosses a PLL integer
step" y **recorta las desviaciones que pasan del paso**. Por encima de 750 MHz (375 MHz en BCM2711) se usa el
pre-divisor del PLL, con un paso fijo de 38,4 MHz (27 MHz en BCM2711). En cualquier portadora, una banda centrada a menos de
±0,58 × la tasa de slots de un múltiplo exacto de 19,2 MHz (27 MHz en BCM2711) cruza un paso sea cual sea el
divisor (por ejemplo 38,4 MHz, o 54 MHz en una Pi 4); eso afecta a cerca del 1 % de las portadoras a 200 kHz y a
menos con tasas de slots menores.

Portadora desde la cual la banda completa de una tasa de slots dada siempre cabe en un paso del PLL (calculada con la
búsqueda de divisor del propio driver; por debajo, algunas portadoras todavía caben):

| Tasa de slots | Pi 1 / 2 / 3 / Zero (BCM283x) | Pi 4 / 400 / CM4 (BCM2711) |
|---------------|-------------------------------|----------------------------|
| 24 kHz | 0,3 MHz | 0,13 MHz |
| 48 kHz (por defecto) | 0,6 MHz | 0,25 MHz |
| 96 kHz | 1,2 MHz | 0,51 MHz |
| 192 kHz | 2,7 MHz | 1,03 MHz |
| 200 kHz (máxima) | 2,8 MHz | 1,08 MHz |

En la práctica, los 200 kHz completos están disponibles desde la banda de 80 m hacia arriba en todos los modelos
(desde 160 m en una Pi 4), y las portadoras de LF/MF requieren una tasa de muestreo menor. Elegir una portadora
levemente distinta también evita el recorte.

### Límites de uso comunes a todos los modelos soportados

| Limitación | Detalle |
|------------|---------|
| Un único pin de RF | Solo GPIO4 (pin 7 del conector, GPCLK0); ningún otro driver debe reclamarlo (por ejemplo `w1-gpio`) |
| Rango de frecuencia | 50 kHz – 1500 MHz (1000 MHz en BCM2711) en la fundamental; `harmonic` 1 – 15 multiplica ambos extremos (el PLL funciona a frecuencia / armónico) |
| Ancho de banda máximo | Tasa de slots (tasa de muestreo × `slots_per_sample`) entre 10 kHz y 200 kHz, es decir, banda base compleja de como máximo 200 kHz de ancho (±100 kHz). El driver reserva ±0,58 × tasa de slots (±116 kHz a 200 kHz) en el PLL. No hay modo de banda ancha |
| Tasas de muestreo y formatos | Cualquier tasa de 10 kHz a 200 kHz (listadas 24, 48, 96, 192 kHz; por defecto 48 kHz); `slots_per_sample` 1 – 4 interpola, pero la tasa de slots sigue ≤ 200 kHz; `CF32`, `CS16`, `CS8`; un canal TX |
| Onda cuadrada | Armónicos impares de amplitud cercana a 1/n más espurias del PLL fraccionario: es obligatorio un filtro pasabanda o pasabajos |
| Amplitud gruesa | 8 niveles de corriente del pad (ganancia `PAD` 0 – 7, 2 – 16 mA) en el pin directo; el registro del pad es compartido por GPIO 0–27. Mejorar la amplitud requiere calibración, sigma-delta o un DAC EER externo |
| Nivel de salida bajo | Solo corriente del pad; cualquier potencia útil necesita un amplificador externo |
| Exactitud de frecuencia | La fija el cristal de la placa: estimación NTP del kernel más `ppm` manual (−200 … 200); 1 ppm son 144 Hz a 144 MHz. Sin compensación de temperatura: los perfiles de ajuste son específicos de la placa, la frecuencia, la carga y la temperatura |
| Latencia y buffers | Latencia ≈ ocupación del FIFO + ¾ del anillo DMA (unos 64 ms a 48 kHz con un anillo de 4096 slots); `fifo_ms` 20 – 5000, `preroll_ms` 0 – 5000, `ring_size` 512 – 32768 slots (al menos 10 ms a la tasa de slots). Sin transmisión temporizada |
| El reajuste no es continuo | Cambiar frecuencia, tasa de muestreo, ppm o armónico durante la transmisión detiene el DMA, apaga la portadora, reprograma el PLL y reinicia: no es salto de frecuencia |
| Temporización del DAC de envolvente | Con `envelope_output=pwm`, la tasa de slots × 2^`pwm_bits` debe ser como máximo la mitad de la frecuencia de PLLD: 11 o 12 bits limitan la tasa de slots a unos 122 kHz o 61 kHz con un PLLD típico de 500 MHz (183 kHz o 91 kHz a 750 MHz). `envelope_output=pcm` no tiene ese límite |
| Hardware exclusivo | Un canal DMA, el bloque PCM/I2S (o PWM con `envelope_output=pwm`), memoria de VideoCore (140 bytes por slot del anillo), GPIO 0–9 congelados durante la transmisión, un transmisor por sistema |
| Árbol de relojes | Mientras hay un stream abierto, el reloj de la VPU funciona desde PLLA, que el firmware debe dejar activo; mantenga `gpu_freq=250`. El mini-UART, I2C, SPI y el host SD funcionan a una velocidad distinta de la que cree el kernel |
| Privilegios | Root: `/dev/mem` y `/dev/vcio` |
| Solo transmisión | Sin ruta de recepción ni escrituras temporizadas (`SOAPY_SDR_HAS_TIME`) |

---

## Requisitos y restricciones del sistema

- **Privilegios de root.** El driver mapea los registros de periféricos mediante `/dev/mem` y reserva memoria DMA
  mediante el mailbox de VideoCore `/dev/vcio`. Sin acceso informa un error claro; nunca termina la aplicación
  anfitriona.
- **Reloj del núcleo fijo.** PLLC se modula, por lo que el reloj del núcleo VPU se mueve a PLLA mientras existe un
  stream (el stream se rechaza cuando el canal periférico de PLLA no está funcionando). Como hace el instalador de rpitx, mantenga fija la frecuencia del núcleo:

  ```ini
  # /boot/firmware/config.txt (o /boot/config.txt en versiones anteriores)
  gpu_freq=250
  ```

  El árbol de relojes se guarda al crear el stream y se restaura al cerrarlo. Mientras el stream está abierto, los
  periféricos alimentados por el reloj de la VPU (mini-UART, I2C, SPI y el controlador SD de las Pi Zero/1/3)
  funcionan a una frecuencia distinta de la que cree el kernel.
- **Memoria de la GPU.** Cada posición del anillo DMA usa 140 bytes de memoria VideoCore (páginas de 4 KiB; 144 bytes
  con un DAC de envolvente, 180 bytes con el RF-DAC y un DAC de envolvente):
  4096 posiciones ≈ 560 KiB, 32768 posiciones ≈ 4,4 MiB. Mantenga `gpu_mem` en su valor por defecto (≥ 64 MB) para
  anillos grandes.
- **GPIO4 debe estar libre.** Si un driver del kernel reclama GPIO4 — en particular el overlay `w1-gpio` (1-Wire),
  cuyo pin por defecto es GPIO4 — el driver rechaza el stream.
- **El bloque PCM/I2S debe estar libre.** Se usa como reloj de cadencia del DMA (y como DAC de envolvente con
  `envelope_output=pcm`); con un overlay de placa de sonido I2S o `dtparam=i2s=on` el stream se rechaza. Con
  `envelope_output=pwm` es el bloque PWM el que da la cadencia y debe estar libre: sin overlay `pwm` y con
  `dtparam=audio=off`.
- **La máscara de canales DMA del kernel debe poder leerse** del device tree (`soc/dma@…` o `soc/dma-controller@…`,
  propiedad `dma-channel-mask` o `brcm,dma-channel-mask`) para la elección automática del canal; si no, indique
  `dma_channel` explícitamente. El registro de caídas anota que la máscara no se podía leer, de modo que la
  recuperación igualmente reinicia ese canal.
- **Un solo transmisor por sistema**, garantizado con un `flock()` sobre `/run/soapy_rpitx/soapy_rpitx.lock`. El
  directorio se crea con modo 0700 y debe pertenecer al usuario que ejecuta el driver; se rechaza un archivo de
  bloqueo que no sea un archivo regular de ese usuario (por ejemplo un enlace simbólico).
- **Las funciones de GPIO 0–9 quedan congeladas durante la transmisión.** El DMA reescribe todo el registro
  `GPFSEL0` en cada muestra (es la única forma de conmutar el pin de reloj desde el DMA). Las funciones de GPIO 0–9
  se capturan al activar el stream; no las reconfigure (I2C en 2/3, SPI en 7–9, …) mientras transmite. Con
  `rf_dac=true` lo mismo vale para GPIO 20–29 (`GPFSEL2`).
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

Un archivo de fuentes con solo los archivos confirmados (sin árboles de compilación, bytecode ni archivos de toolchain
locales) se genera con `cmake --build build --target dist` (requiere un checkout de git); se escribe como
`build/SoapySDR_rpitx-<versión>.zip`.

### Opciones de CMake

| Opción | Por defecto | Descripción |
|--------|-------------|-------------|
| `RPITX_BUILD_DRIVER` | `ON` | Compila el módulo SoapySDR (requiere SoapySDR) |
| `RPITX_BUILD_RESET_TOOL` | `ON` | Compila e instala `rpitx_reset` |
| `RPITX_INSTALL_TOOLS` | `ON` | Instala `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py`, `rpitx_amp_calibrate.py` y `rpitx_autotune.py` |
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
  contiene la contraseña se crea en un tmpfs y se elimina después, también cuando el script se interrumpe; un
  temporizador en la Pi lo borra a las 3 horas aunque el script muera o se corte la conexión). `--pi-password` también
  funciona, pero deja la contraseña en `ps` y en el historial del shell (el script lo advierte); prefiera
  `--ask-pass` o `RPI_PI_PASSWORD`.
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
| `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py`, `rpitx_amp_calibrate.py`, `rpitx_autotune.py` | `<prefix>/bin` |

No hace falta reiniciar ni copiar nada a mano. Verifique:

```sh
SoapySDRUtil --info                         # "rpitx" en "Available factories"
sudo SoapySDRUtil --find="driver=rpitx"     # modelo y número de serie de la placa
sudo SoapySDRUtil --probe="driver=rpitx"    # canales, rangos, ajustes, sensores
sudo rpitx_tone_test.py --freq 144.800M     # tono NBFM de prueba de 30 s (¡carga fantasma!)
sudo SoapySDRUtil --probe="driver=rpitx,rx=rtlsdr"   # transceptor: un canal RX y uno TX, full duplex
```

Con un RTL-SDR conectado (y su módulo SoapySDR, `soapysdr-module-rtlsdr`, instalado), `--find` lista primero el
dispositivo solo TX y luego una entrada *RPITX transceiver* por cada dongle.

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
  panel del dispositivo. Con una entrada de transceptor (`rx=rtlsdr`) un *SoapySDR input* sobre el mismo dispositivo
  recibe a través del RTL-SDR.

### Transceptor: receptor RTL-SDR y relé de antena

```sh
# Transceptor: el primer RTL-SDR como canal RX 0, relé en GPIO17 (alto = TX)
driver=rpitx,rx=rtlsdr,relay_gpio=17
# Un dongle concreto, muestreo directo para HF, módulo de relé activo en bajo, tiempo de retención mayor
driver=rpitx,rx=rtlsdr,rx.serial=00000002,rx.direct_samp=2,relay_gpio=17,relay_active_low=true,relay_hang_ms=300
# Solo TX con el PTT de un amplificador / relé T-R en GPIO27
driver=rpitx,relay_gpio=27
# Relé en GPIO17, PTT del amplificador en GPIO27: 30 ms para el relé, luego PTT, 15 ms para el amplificador, luego RF
driver=rpitx,rx=rtlsdr,relay_gpio=17,relay_tx_delay_ms=30,ptt_gpio=27,ptt_tx_delay_ms=15,ptt_rx_delay_ms=20
```

**Receptor.** `rx=<driver>` abre un receptor SoapySDR mediante su módulo instalado (`rtlsdr` para un RTL-SDR;
`none` o vacío = solo TX) y expone su canal 0 como canal RX 0 del dispositivo rpitx. Cada argumento de dispositivo
`rx.<clave>=<valor>` se le pasa como `<clave>=<valor>` (`rx.serial=…` elige el dongle). El dispositivo informa
entonces un canal RX y uno TX y `getFullDuplex()` verdadero: un stream RX y uno TX pueden estar abiertos y activos a
la vez. Toda llamada RX — `setupStream`/`readStream`/`readStreamStatus`, frecuencia, ganancia (y AGC), antenas,
frecuencia de muestreo, ancho de banda, correcciones de DC / balance IQ / frecuencia, sensores y ajustes de canal —
se reenvía al receptor; sus ajustes globales están disponibles como `rx.<clave>` (por ejemplo `rx.direct_samp`,
`rx.offset_tune`, `rx.biastee` del RTL-SDR) y su información de hardware aparece en `getHardwareInfo()` con el
prefijo `rx.`, junto a `rx_driver` y `rx_hardware`. `rx=rpitx` se rechaza.

**Activación.** El transmisor se *activa* con la primera muestra escrita tras un período inactivo y se *desactiva*
cuando todas las muestras encoladas se reprodujeron y no se escribió nada durante `relay_hang_ms`, o de inmediato
cuando el stream TX se desactiva o se cierra. Mientras está activado el sensor `ptt` lee `true` y — con
`rx_mute_tx=true`, el valor por defecto — las muestras recibidas se reemplazan por silencio (ceros, media escala
para formatos sin signo, incluidos los empaquetados `CS4`/`CS12`/`CU4`/`CU12`; los flags y las marcas de tiempo se
conservan), de modo que una aplicación puede mantener ambos streams en marcha como con cualquier transceptor. El
receptor entrega sus muestras algo después de capturarlas (búferes USB), así que el silencio dura `rx_mute_tail_ms`
(50 ms por defecto) más tras la desactivación; auméntelo si el final de una transmisión todavía se cuela en la señal
recibida.

**Relé de antena.** Con `relay_gpio=<n>` el driver maneja ese GPIO como salida: posición RX al abrir, posición TX
cuando el transmisor se activa, de vuelta a RX cuando se desactiva y al cerrar el dispositivo (el pin queda como
salida en posición RX). El primer `writeStream()` de una transmisión conmuta el relé y espera `relay_tx_delay_ms`
antes de encolar cualquiera de sus muestras, así ninguna RF llega a los contactos en movimiento (la espera no cuenta
en `timeoutUs`; desactivar o cerrar el stream la termina de inmediato, y la escritura devuelve entonces
`SOAPY_SDR_STREAM_ERROR`). `relay_active_low=true` es para módulos de relé con entrada activa en bajo. El pin se maneja con los
registros GPSET/GPCLR, que el DMA del transmisor nunca toca; su función se fija mientras no existe un stream TX, por
lo que `relay_gpio` y `relay_active_low` no pueden cambiar con un stream TX abierto (los tiempos sí). Se aceptan
GPIO2–27 salvo GPIO4, y un stream TX que usaría el pin del relé (`rf_dac` en GPIO20, `envelope_output=pcm` en
GPIO18/19/21, `pwm` en `pwm_gpio`) se rechaza.

**Línea PTT.** Con `ptt_gpio=<n>` el driver también activa un amplificador, transversor o radio externo, en
secuencia con el relé, de modo que el relé nunca conmuta con potencia y el amplificador se activa antes de recibir
excitación:

```
activar:     relé → TX ── relay_tx_delay_ms ──▶ PTT on ── ptt_tx_delay_ms ──▶ primera muestra (RF)
desactivar:  última muestra ── relay_hang_ms ──▶ PTT off ── ptt_rx_delay_ms ──▶ relé → RX
```

El primer `writeStream()` de una transmisión espera la secuencia de activación completa (`relay_tx_delay_ms` +
`ptt_tx_delay_ms`; no se cuenta en `timeoutUs` y termina de inmediato al desactivar o cerrar el stream). Una demora
cuyo GPIO no está configurado se omite: sin relé la línea PTT se afirma de inmediato y las muestras siguen
`ptt_tx_delay_ms` después; sin línea PTT las muestras siguen a `relay_tx_delay_ms` (el comportamiento de arriba).
Cuando la transmisión termina, o el stream se desactiva o se cierra, primero se libera la línea PTT y el relé vuelve
a RX `ptt_rx_delay_ms` después (de inmediato si la línea PTT todavía no se había afirmado); `deactivateStream()` y
`closeStream()` esperan esa demora. Una escritura que llega mientras el relé todavía espera para volver a RX afirma
de nuevo la línea PTT sin mover el relé, así que solo se espera `ptt_tx_delay_ms`. El sensor `ptt` sigue en `true`
(y el receptor silenciado) hasta que el relé está de vuelta en RX; el sensor `ptt_line` muestra el propio GPIO de PTT.
`ptt_active_low=true` es para entradas de PTT que activan con nivel bajo (etapa a colector abierto, la mayoría de
las radios). Valen las mismas reglas que para el pin del relé: GPIO2–27 salvo GPIO4, distinto del pin del relé, no
usado por el stream TX, y `ptt_gpio` / `ptt_active_low` no pueden cambiar con un stream TX abierto (las demoras sí).

> **Proteja el receptor.** La entrada del RTL-SDR soporta solo unos pocos milivatios. Conéctelo a través del relé (o
> de un conmutador T/R) y mantenga `relay_tx_delay_ms` mayor que el tiempo de operación del relé; con dos antenas
> separadas asegúrese de que el aislamiento alcance para la potencia de transmisión. El relé se maneja desde un GPIO
> (3,3 V, pocos mA): use un transistor o un módulo de relé, nunca la bobina directamente.

### Semántica del stream

- Los tiempos de espera (`timeoutUs` de `writeStream()` y `readStreamStatus()`) se limitan a un día, así que
  `LONG_MAX` sirve para "esperar indefinidamente".
- `writeStream()` se bloquea mientras la FIFO está llena, respeta `timeoutUs`, puede aceptar menos muestras de las
  ofrecidas (el valor de retorno es la cantidad consumida) y devuelve `SOAPY_SDR_TIMEOUT` cuando no pudo encolar nada
  a tiempo. Una escritura bloqueada cuando el stream se desactiva retorna de inmediato; no se encola ninguna muestra
  después, y ninguna de sus muestras llega a una activación posterior. `SOAPY_SDR_HAS_TIME` no está soportado (`SOAPY_SDR_NOT_SUPPORTED`). La MTU es `min(4096, max(FIFO/4, 256))`.
- `SOAPY_SDR_END_BURST` en la última escritura (o en una escritura de longitud cero) marca el fin de la ráfaga: el DMA
  arranca de inmediato aunque no se haya alcanzado el pre-roll, y tras la última muestra la portadora queda apagada.
- `activateStream()` sobre un stream activo no tiene efecto (los contadores siguen, no se informa ningún evento).
- `deactivateStream()` apaga la portadora. Si la última escritura llevaba `END_BURST`, primero espera (como máximo la
  duración de la FIFO a la frecuencia de muestreo, más dos anillos a la frecuencia de posiciones, más 200 ms) a que se hayan reproducido todas las muestras encoladas; mientras
  tanto no retiene el mutex del dispositivo, así que la API de control sigue disponible. Sin `END_BURST`, las
  muestras encoladas se descartan.
- `closeStream()` (y la destrucción del dispositivo) apaga la portadora de inmediato y descarta las muestras
  encoladas, también mientras otro hilo espera en `deactivateStream()`; el hardware queda libre para un nuevo stream
  enseguida. Un `writeStream()` o `readStreamStatus()` en otro hilo se despierta y retorna antes de destruir el stream
  (las llamadas en curso se cuentan y `closeStream()` las espera), así que cerrar durante un apagado es seguro.
- `readStreamStatus()` devuelve `SOAPY_SDR_UNDERFLOW` (máscara de canal 1) una vez por cada nuevo vaciado de la FIFO
  dentro de una ráfaga, overrun del DMA o reinicio del DMA; si no, `SOAPY_SDR_TIMEOUT`; con varios hilos consultando,
  cada evento se informa a uno solo de ellos. Las ráfagas consecutivas cerradas cada una con `END_BURST` nunca cuentan
  como vaciado.
- `readStream()` / `readStreamStatus()` de RX llaman al receptor con tiempos de espera de 100 ms como máximo hasta
  agotar `timeoutUs`, así que `closeStream()` nunca espera más que eso por una lectura en curso.
- Sin stream TX, la frecuencia, la frecuencia de muestreo, los ppm y la armónica se verifican contra el PLL de la
  portadora al fijarlos (una combinación inalcanzable se rechaza de inmediato, no en un `setupStream()` posterior).
- La frecuencia, la frecuencia de muestreo, los ppm y la armónica pueden cambiar durante la transmisión: el
  alimentador detiene el DMA, apaga la portadora, reprograma el PLL y el reloj de cadencia y reanuda con las muestras
  que ya estaban en el anillo DMA (ninguna se pierde; como mucho una se envía dos veces), seguidas por la FIFO. Las
  posiciones inactivas anteriores a la primera muestra encolada no se repiten; un hueco inactivo dentro de una ráfaga
  (FIFO vacía) se repite como inactivo.

### Argumentos del stream

| Clave | Por defecto | Rango | Descripción |
|-------|-------------|-------|-------------|
| `ring_size` | 85 ms de posiciones, ≥ 4096, redondeado a 512 | 512 – 32768, y ≥ 10 ms a la frecuencia de posiciones | Tamaño del anillo DMA en posiciones (una por muestra, o `slots_per_sample` por muestra) |
| `fifo_ms` | 250 | 20 – 5000 | Capacidad de la FIFO de software en ms (sigue los cambios posteriores de frecuencia de muestreo) |
| `preroll_ms` | 100 | 0 – 5000 | Señal encolada antes de arrancar el DMA |
| `dma_channel` | -1 | -1 – 14 | Canal DMA; -1 = primer canal libre que no pertenece al kernel |
| `dma_force` | false | bool | Reinicia un canal elegido explícitamente aunque esté activo |
| `rt_priority` | 50 | 0 – 99 | Prioridad `SCHED_FIFO` del hilo alimentador (0 = planificación normal) |
| `cpu` | -1 | -1 – 63 | CPU a la que se fija el hilo alimentador |
| `signal_cleanup` | false | bool | Apaga la portadora cuando el proceso muere por una señal con la acción por defecto |
| `slots_per_sample` | 1 | 1 – 4 | Posiciones DMA por muestra I/Q; la señal I/Q se interpola y la amplitud se cuantiza a la frecuencia de posiciones (frecuencia de muestreo × posiciones ≤ 200 kHz) |
| `envelope_output` | none | `none`, `pcm`, `pwm` | Salida de envolvente para EER: `pcm` = I2S de 16 bits en GPIO18/19/21, `pwm` = ciclo de trabajo PWM en `pwm_gpio` |
| `pwm_gpio` | 18 | 12 o 18 | Pin de la salida de envolvente PWM (GPIO18 ALT5, GPIO12 ALT0) |
| `pwm_bits` | 8 | 6 – 12 | Resolución de la envolvente PWM (periodo de 2^bits relojes PWM) |
| `rf_dac` | false | bool | GPIO20 como segunda salida de reloj, sumada a GPIO4 por una red ponderada externa |

Latencia desde `writeStream()` hasta la antena ≈ llenado de la FIFO + ¾ del anillo (≈ 64 ms con un anillo de 4096
posiciones a 48 kHz). Una aplicación que escribe más rápido que el tiempo real mantiene la FIFO llena: latencia ≈
`fifo_ms` + ¾ del anillo.

### Ajustes (también aceptados como argumentos del dispositivo)

| Clave | Por defecto | Descripción |
|-------|-------------|-------------|
| `ppm` | 0 | Corrección manual del cristal, −200 … 200 ppm. **Positiva cuando el cristal es lento**, es decir, cuando la placa transmite por debajo de la frecuencia pedida (mismo signo que la estimación NTP del kernel) |
| `ppm_ntp` | true | Usa la estimación de frecuencia NTP del kernel como corrección base (se suma a `ppm`); la estimación se usa en todos los estados del reloj salvo un reloj no sincronizado (se conserva en torno a los segundos intercalares) |
| `harmonic` | 1 | Transmite en esta armónica de la onda cuadrada: el PLL funciona a frecuencia / armónica (1 – 15) |
| `amplitude_mode` | envelope | `envelope` o `constant` |
| `iq_scale` | 1.0 | Ganancia aplicada a \|I/Q\| antes de cuantizar la amplitud (modo envelope), 0 – 100 |
| `debug_level` | 0 | Diagnóstico de librpitx en stderr: 0 errores, 1 configuración, 2 volcados de registros |
| `amplitude_lut` | vacío | Amplitud RF relativa medida de los niveles de corriente 0–7 (8 valores crecientes separados por comas); vacío = `(nivel + 1) / 8` |
| `ampm_lut` | vacío | Fase medida de los niveles 0–7 en grados (8 valores); se compensa en la portadora |
| `amplitude_quantizer` | nearest | `nearest` o `sigma_delta` (selección de amplitud con conformación de ruido) |
| `sigma_delta_order` | 2 | Orden de la realimentación del error, 1 o 2 |
| `envelope_delay` | 0.5 | Retardo de la envolvente respecto de la fase en muestras, −32 … 32 (0,5 alinea los dos caminos del pin solo) |
| `envelope_floor` | 0 | Envolvente por debajo de la cual la portadora se apaga tras 2 ms (0 = nunca) |
| `rfdac_lut` | vacío | Amplitud relativa con solo GPIO4, solo GPIO20 y ambos pines de reloj (3 valores); vacío = 2/3, 1/3, 1 |
| `eer_lut` | vacío | Amplitud RF medida en códigos de envolvente equiespaciados de 0 a fondo de escala (2–64 valores crecientes); vacío = lineal |
| `eer_ampm_lut` | vacío | Fase medida en los mismos códigos, en grados (2–64 valores) |
| `eer_gain`, `eer_offset` | 1, 0 | Ganancia y desplazamiento aplicados al código de envolvente |
| `eer_idle` | 0 | Código de envolvente mientras no se transmite (fracción del fondo de escala) |
| `eer_drive` | 7 | Corriente de los pines de reloj mientras se usa el DAC de envolvente |
| `profile` | vacío | Archivo de perfil: sus ajustes y `gain` se aplican de inmediato, sus líneas `stream.*` pasan a ser los valores por defecto del próximo stream (véase [Archivos de perfil](#archivos-de-perfil)); vacío olvida esos valores del stream |
| `relay_gpio` | -1 | GPIO del relé de antena T/R (2 – 27, no 4), -1 = sin relé; no puede cambiar con un stream TX abierto |
| `relay_active_low` | false | El relé está en posición TX con el pin en bajo; no puede cambiar con un stream TX abierto |
| `relay_tx_delay_ms` | 20 | Tiempo de asentamiento del relé antes de afirmar la línea PTT (antes de encolar la primera muestra de una transmisión cuando no hay línea PTT), 0 – 1000 ms |
| `relay_hang_ms` | 100 | Tiempo que el transmisor sigue activado (relé en TX, receptor silenciado) tras reproducirse la última muestra, 0 – 10000 ms |
| `ptt_gpio` | -1 | GPIO de la línea PTT de un amplificador o radio (2 – 27, no 4, no `relay_gpio`), -1 = sin línea PTT; no puede cambiar con un stream TX abierto |
| `ptt_active_low` | false | La línea PTT se afirma con el pin en bajo; no puede cambiar con un stream TX abierto |
| `ptt_tx_delay_ms` | 10 | Tiempo desde que se afirma la línea PTT hasta la primera muestra de una transmisión, 0 – 1000 ms |
| `ptt_rx_delay_ms` | 10 | Tiempo desde que se libera la línea PTT hasta que el relé vuelve a RX, 0 – 1000 ms |
| `rx_mute_tx` | true | Reemplazar las muestras recibidas por silencio mientras el transmisor está activado |
| `rx_mute_tail_ms` | 50 | Tiempo que las muestras recibidas siguen silenciadas tras la desactivación (latencia de los búferes del receptor), 0 – 10000 ms |
| `rx.<clave>` | — | Un ajuste del receptor (solo con `rx=<driver>`), por ejemplo `rx.direct_samp` |

Solo como argumentos de dispositivo: `rx=<driver>` agrega un receptor, `rx.<clave>=<valor>` se le pasa (véase
[Transceptor](#transceptor-receptor-rtl-sdr-y-relé-de-antena)).

Ejemplo: `driver=rpitx,amplitude_mode=constant,ppm=1.5`. Los argumentos de dispositivo desconocidos se informan con
una advertencia, salvo las claves de identificación (`driver`, `label`, `product`, `serial`, `manufacturer`, `tuner`,
`type`), las claves del receptor (`rx`, `rx.*`) y las claves que contienen `:`. Los enteros son decimales (un cero inicial no significa octal) y los números no pueden llevar
caracteres al final.

### Archivos de perfil

`profile=<archivo>` (argumento del dispositivo, o `writeSetting("profile", archivo)` en ejecución) carga un archivo
de texto de líneas `clave=valor`, como el `profile_<path>.rpitx` que escribe [`rpitx_autotune.py`](#rpitx_autotunepy):

```ini
# comentario
ppm=1.37
amplitude_lut=0.12,0.27,0.42,0.56,0.69,0.8,0.9,1
envelope_delay=0.62
stream.slots_per_sample=2
stream.fifo_ms=300
gain=7
```

- Las claves de ajuste se aplican en el orden del archivo; un ajuste dado también como argumento del dispositivo tiene
  prioridad sobre el archivo. Un valor rechazado detiene la carga con un error que nombra el archivo (las líneas
  anteriores no se aplican: la carga es todo o nada, y con un stream abierto hay como máximo un retune).
- Las líneas `stream.<clave>` pasan a ser los valores por defecto de `setupStream()`; los argumentos que pasa la
  aplicación tienen prioridad.
- `gain` fija la ganancia PAD (0–7).
- También se aceptan las claves del relé y del PTT (`relay_gpio`, `relay_active_low`, `relay_tx_delay_ms`,
  `relay_hang_ms`, `ptt_gpio`, `ptt_active_low`, `ptt_tx_delay_ms`, `ptt_rx_delay_ms`), `rx_mute_tx` y
  `rx_mute_tail_ms`; `rx` y `rx.*` son argumentos de dispositivo y no se leen de un perfil.
- Las líneas vacías y las que empiezan con `#` se ignoran, las claves desconocidas se informan con una advertencia y se
  ignoran, una línea sin `=` es un error, y una línea posterior reemplaza a una anterior con la misma clave.
- `readSetting("profile")` devuelve el último archivo cargado; `writeSetting("profile", "")` olvida sus valores por
  defecto del stream (los ajustes se mantienen).
- El archivo debe ser un archivo regular de 64 KiB como máximo que el grupo y los demás no puedan escribir (modo 0644
  o más estricto); no se siguen enlaces simbólicos, y los dispositivos, tuberías y sockets se rechazan sin leerlos.
  El driver se ejecuta como root y el ajuste puede llegar desde un cliente remoto (SoapyRemote), así que no se abre
  nada más.
- El archivo y todos los directorios de su ruta deben pertenecer a root o al usuario que ejecuta el driver; los
  directorios no deben poder ser escritos por los demás (salvo con bit sticky). Cuando la aplicación se ejecuta como
  root mediante `sudo`, también se aceptan archivos y directorios del usuario que ejecutó `sudo` (`SUDO_UID`), siempre
  que ni el grupo ni los demás puedan escribirlos — así funciona
  `sudo app profile=/home/pi/rpitx_autotune_results/profile_non_eer.rpitx`.

### Sensores

| Clave | Tipo | Descripción |
|-------|------|-------------|
| `pll_locked` | bool | Bit de enganche de PLLC (`CM_LOCK.FLOCKC`) |
| `dma_running` | bool | El alimentador arrancó el DMA y el bit `ACTIVE` del canal está activo |
| `fifo_fill` | float % | Nivel de llenado de la FIFO de software |
| `underflows` | int | La FIFO se vació en medio de una ráfaga (aplicación demasiado lenta) |
| `late` | int | El motor DMA alcanzó al alimentador (hilo alimentador demorado) |
| `dma_errors` | int | El motor DMA se detuvo solo y fue reiniciado |
| `sample_rate` | float Hz | Frecuencia de muestreo I/Q producida realmente por los divisores del reloj de cadencia |
| `envelope_clip` | int | Posiciones cuya envolvente (\|I/Q\| × `iq_scale`, o el código EER) superó el fondo de escala |
| `envelope_late` | int | Palabras de envolvente que no pudieron escribirse por adelantado (anillo casi vacío) |
| `ptt` | bool | El transmisor está activado: desde el inicio de la secuencia de activación hasta que el relé vuelve a RX; con `rx_mute_tx` el receptor está silenciado (siempre disponible) |
| `ptt_line` | bool | El GPIO de PTT (`ptt_gpio`) está afirmado; siempre `false` sin línea PTT (siempre disponible) |
| `ptt_count` | int | Veces que se activó el transmisor desde que se abrió el dispositivo (siempre disponible) |

Los sensores de canal del receptor están disponibles en el canal RX (`listSensors(SOAPY_SDR_RX, 0)`).

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

Las listas `clave=valor` de las herramientas (`--args`, `--stream-args`, `--rx-args`) se verifican antes de abrir
cualquier radio: un elemento sin `=` detiene la herramienta con un mensaje que lo nombra.

### `rpitx_amp_calibrate.py`

Mide el camino de amplitud con un receptor SoapySDR (basta un RTL-SDR junto a una carga fantasma) e imprime los
ajustes a usar. Cada estado se alterna con una referencia de fondo de escala, de modo que la deriva de fase entre
las dos radios se cancela; el receptor se sintoniza `--offset` Hz por debajo de la portadora.

```sh
sudo rpitx_amp_calibrate.py pad   --freq 7.1M --rx-args driver=rtlsdr         # amplitude_lut, ampm_lut
sudo rpitx_amp_calibrate.py rfdac --freq 7.1M                                 # rfdac_lut (rf_dac=true)
sudo rpitx_amp_calibrate.py eer   --freq 7.1M --envelope-output pcm --points 17   # eer_lut, eer_ampm_lut
sudo rpitx_amp_calibrate.py delay --freq 7.1M --envelope-output pcm           # envelope_delay (IMD3 con dos tonos)
rpitx_amp_calibrate.py self-test                                              # verificación del análisis, sin radio
```

Las tablas impresas ya tienen la forma que acepta el driver: tablas de amplitud estrictamente crecientes (un valor
que el ruido de medición dejó fuera de orden se eleva lo mínimo, con una advertencia), fases dentro de ±180° y la fase
del código EER 0 (sin portadora) tomada del código 1.

### `rpitx_autotune.py`

Prueba y ajuste en lazo cerrado sobre el aire. La herramienta transmite ráfagas de prueba conocidas en cada familia
de modulación, las recibe con un receptor SoapySDR (basta un RTL-SDR junto a una carga fantasma y un atenuador), mide
las degradaciones contra la forma de onda transmitida y ajusta los parámetros y argumentos de stream del driver hasta
que cada modo seleccionado cumple sus umbrales o se agota un presupuesto. El camino del pin directo y el camino EER se
ajustan por separado (`--path non-eer|eer|both`).

```sh
sudo rpitx_autotune.py run --freq 144.39M --confirm-dummy-load                     # pin directo, modos por defecto
sudo rpitx_autotune.py run --freq 7.1M --path eer --envelope-output pcm \
     --rx-args driver=rtlsdr,direct_samp=2 --confirm-dummy-load                    # camino EER en HF
sudo rpitx_autotune.py run --freq 144.39M --path both --modes nbfm,fsk,bpsk,qpsk,usb --max-minutes 30 --confirm-dummy-load
sudo rpitx_autotune.py run --freq 144.39M --resume rpitx_autotune_results/checkpoint.json --confirm-dummy-load
sudo rpitx_autotune.py verify --freq 144.39M --profile rpitx_autotune_results/profile_non_eer.json --confirm-dummy-load
rpitx_autotune.py run --simulate good --freq 144.39M                               # banco simulado, sin radio
rpitx_autotune.py replay --out rpitx_autotune_results                              # vuelve a analizar las capturas guardadas
rpitx_autotune.py self-test [--quick]                                              # verificación del análisis y del optimizador, sin radio
```

Cada ráfaga es una portadora inicial, un hueco, una palabra de sincronismo MSK, la carga útil, una segunda palabra de
sincronismo y una portadora final. De cada captura la herramienta mide el error de frecuencia de la portadora y el
error del reloj de muestreo (por el retardo entre las dos palabras de sincronismo), alinea la captura muestra a muestra
con la señal transmitida y ajusta un filtro lineal corto que absorbe la banda pasante del receptor; el error restante
y los demoduladores de cada modo dan las métricas.

| Modos | Métricas |
|-------|----------|
| `cw` | tiempos de los flancos de manipulación, fuga con la llave abierta, clics de manipulación |
| `nbfm`, `wbfm` (frecuencia de muestreo ≥ 180 kHz) | error de desviación, SINAD |
| `fsk` (300 Bd), `afsk` (Bell 202), `mfsk` (8-FSK) | errores de bit / símbolo, error de frecuencia de los tonos |
| `bpsk` (conformado PSK31), `qpsk`, `8psk`, `qam16`, `ofdm` | EVM, errores de bit / símbolo, deriva de fase, ACPR |
| `am` | distorsión del tono demodulado |
| `usb`, `lsb` | IMD3 con dos tonos, supresión de la banda lateral opuesta, portadora residual |
| `iq_raw` | ráfaga larga de banda ancha: contadores de robustez y error de forma de onda |

Todos los modos verifican además el error de frecuencia, el error del reloj de muestreo, los contadores `underflows`,
`late`, `dma_errors` y `envelope_late` y, en modo envolvente, `envelope_clip`. Los modos por defecto son todos salvo
`wbfm` y `ofdm`.

Las etapas de ajuste se ejecutan en este orden (`--stages` las selecciona, `--stage-limit S4=40,...` limita sus
evaluaciones):

| Etapa | Parámetros | Método |
|-------|------------|--------|
| S0 | `rt_priority`, `cpu`, `fifo_ms`, `preroll_ms`, `ring_size` | escalados hasta que dos ráfagas largas `iq_raw` terminan con todos los contadores en cero |
| S1 | `ppm` | error de la portadora de una ráfaga CW; con `--freq-ref tx` se ajusta en cambio la corrección del receptor, con `--freq-ref external` el receptor se calibra antes con la portadora en `--ref-freq` |
| S2 | ganancia PAD de los modos de envolvente constante, o `eer_drive` | la excitación más baja a menos de `--drive-margin` dB de la mayor salida |
| S3 | `amplitude_lut`, `ampm_lut` (`rfdac_lut` con `--rf-dac-available`), o `eer_lut`, `eer_ampm_lut` | medición referencia / estado de `rpitx_amp_calibrate.py`, refinada luego en lazo cerrado; el signo con el que el driver aplica la tabla AM/PM se mide, no se supone |
| S4 | `envelope_delay` | menor IMD3 con dos tonos (rejilla y luego sección áurea) |
| S5 | `amplitude_quantizer`, `sigma_delta_order`, `slots_per_sample` (y `rf_dac`) | todas las combinaciones, puntuadas por EVM de QPSK e IMD3 sin ceder ACPR (solo pin directo) |
| S6 | `iq_scale` (EER: también `eer_gain`, `eer_offset`) | mejor EVM de OFDM sin `envelope_clip` |
| S7 | `envelope_floor` (EER: también `eer_idle`) | sin ruido en las pausas y sin recortar la señal |

Después se verifican todos los modos seleccionados (`--verify-repeats` ráfagas, cuenta el peor valor). Las etapas que
pueden cambiar una métrica que falla se vuelven a ejecutar, hasta `--max-rounds` rondas. Un valor nuevo se acepta solo
cuando supera al actual en más de `--min-delta` y de `--sigma-k` veces la dispersión medida.

Cada métrica termina como `PASS`, `FAIL`, `UNVERIFIABLE` (el umbral está a menos de `--floor-margin` dB de lo que el
banco puede resolver: el ruido del receptor y, para productos junto a una señal fuerte, `--rx-sfdr`), `HARDWARE_LIMIT`
(todas las etapas habilitadas que podían mejorarla se ajustaron de nuevo sin éxito), `INFEASIBLE` o `INVALID`. La ejecución
termina cuando todo pasa, al alcanzar `--max-trials`, `--max-minutes` o `--max-tx-seconds` (`BUDGET_EXHAUSTED`), o con
Ctrl-C (`ABORTED`: la ráfaga termina con `END_BURST`, el stream se cierra y el punto de control se conserva). `--duty`
limita el ciclo de trabajo de transmisión. El código de salida es 0 solo cuando todo pasa (`--allow-unverifiable`
acepta también `UNVERIFIABLE`).

El directorio de salida (`--out`) contiene `profile_<path>.json` (entrada de `verify`), `profile_<path>.rpitx`
(archivo de perfil del driver, véase [Archivos de perfil](#archivos-de-perfil)), `profile_<path>.args`
(argumentos del dispositivo, los ajustes de tablas a aplicar con `writeSetting()` — contienen comas y no pueden formar
parte de una cadena de argumentos del dispositivo —, argumentos del stream y ganancias), `report.md`, `report.json`,
`checkpoint.json` (para `--resume`) y `captures/` (`--save-captures fail|all`, para `replay`). La herramienta se
ejecuta como root, así que rechaza un directorio de salida que sea un enlace simbólico, pertenezca a otro usuario (se
aceptan root, el usuario efectivo y el usuario que ejecutó `sudo`) o que el grupo o los demás puedan escribir; crea
uno que falte con modo 0755, nunca escribe a través de un enlace simbólico y escribe los archivos JSON mediante un
archivo temporal de nombre impredecible. Los umbrales por
defecto están incorporados y son valores iniciales; se reemplazan con `--thresholds archivo.json`, por ejemplo
`{"non_eer": {"qpsk": {"evm_db": -20}}, "global": {"freq_abs_hz": 2}}`.

Otras opciones:

| Opción | Valores (primero el predeterminado) | Efecto |
|--------|-------------------------------------|--------|
| `--profile-format` | `both`, `args`, `driver` | escribe `profile_<path>.args`, el perfil del driver `profile_<path>.rpitx`, o ambos |
| `--profile-gain`, `--profile-amplitude-mode` | `envelope`, `constant`; `none`, `envelope`, `constant` | ganancia (de los modos de envolvente o de los de envolvente constante) y `amplitude_mode` guardados en el archivo `.rpitx` |
| `--check-driver-profile` | opción | después del ajuste, vuelve a verificar cada modo con el driver abierto con `profile=<archivo .rpitx>` |
| `verify --apply` | `settings`, `driver-profile` | aplica el perfil (`.json` o `.rpitx`) con `writeSetting()`, o mediante `profile=` |
| `--rx-host`, `--rx-remote` | `local`, `remote`; `tcp://host:55132` | receptor en este equipo, o mediante SoapyRemote (`--rx-args` describe entonces el dispositivo remoto: `driver=rtlsdr` pasa a `remote:driver=rtlsdr`) |
| `--rx-local-policy` | `warn`, `refuse`, `ignore` | qué hacer con un receptor USB local en una Pi Zero / 1 / 2 / 3 o CM1 / CM3 (USB compartido con Ethernet, o un solo núcleo) |
| `--required-modes`, `--required-modes-eer` | `default`, `all`, `none`, lista | modos que deciden el resultado; los demás se informan como opcionales. `default`: en el pin directo los modos de envolvente constante, `bpsk`, `usb`, `lsb` e `iq_raw`; en el camino EER todos |
| `--allow-hardware-limit`, `--allow-hardware-limit-eer` | lista, `all`, `none` (`qam16,ofdm` y `none`) | modos cuyo `HARDWARE_LIMIT` cuenta como aprobado |
| `--threshold-set` | `auto`, `non-eer`, `eer` | tabla de umbrales de cada camino, o la misma tabla para todos (`eer` es la más estricta) |
| `--tune-optional` | `yes`, `no` | si los modos opcionales que fallan también provocan un nuevo ajuste |
| `--freqs` | lista, p. ej. `7.1M,14.2M,144.39M` | un ajuste por banda en `<out>/<banda>/`, luego `bands.md` / `bands.json` con cada parámetro por banda, clasificado `independent` o `dependent` |
| `--band-reuse` | `none`, `start`, `skip` | cada banda desde los valores por defecto, desde el perfil de la banda anterior, o desde él omitiendo las `--band-independent-stages` (por defecto `S0,S5`) |
| `--band-tolerance` | `0.05` | los valores coinciden entre bandas cuando difieren como mucho en esto por max(1, \|valor\|), elemento a elemento en las tablas |
| `--budget-scope` | `band`, `run` | límites del presupuesto por banda o para toda la ejecución |
| `--resume` | archivo, `auto` | `auto` reanuda cada directorio de salida (cada banda) desde su propio punto de control |

Notas sobre el banco: siempre un filtro de banda y una carga fantasma, y la atenuación suficiente para que el receptor
vea como mucho unos −30 dBm (un amplificador EER puede entregar vatios). El receptor puede estar en la misma Pi o en
otro equipo a través de SoapyRemote (`--rx-host remote --rx-remote tcp://host:55132`); en una
Pi Zero / 1 / 3 el receptor USB comparte el bus con Ethernet y puede perturbar el hilo alimentador (la etapa S0 lo
muestra; `--rx-local-policy` avisa por defecto).
`--rx-offset` mantiene el pico de continua y la imagen del receptor lejos de la señal. La frecuencia debe
estar dentro del rango de ambas radios (en HF un RTL-SDR necesita muestreo directo o un conversor). `--freq-ref rx`
(por defecto) confía en el reloj del receptor después de `--rx-ppm`: use un receptor con TCXO para precisión absoluta.
El perfil es propio de la placa, la frecuencia, la carga y la temperatura.

### `rpitx_reset`

Devuelve el hardware al reposo después de que muriera un proceso que usaba el driver: recupera el registro de sesión
(ver [Recuperación tras una caída](#recuperación-tras-una-caída)); sin registro, apaga una salida de reloj que haya
quedado en GPIO4 y detiene GPCLK0. `--dma-channel N` además reinicia el canal DMA N (se rechaza si pertenece al
kernel, y también cuando la máscara de canales DMA del kernel no se puede leer del device tree, salvo que se indique
`--force` para un canal que se sabe libre). Apto para `ExecStopPost=` en una unidad de systemd (por ejemplo
`ExecStopPost=/usr/local/bin/rpitx_reset`), que además restaura el árbol de relojes tras un proceso terminado por una
señal.

### `hpsdr_rpitx_bridge.py` — puente HPSDR Protocol 1

Hace que la Pi aparezca en la red como una radio openHPSDR Protocol 1, de modo que piHPSDR, Thetis, PowerSDR mRX,
linHPSDR, Quisk o SparkSDR la descubren y transmiten a través de rpitx. Con `--rx rtlsdr` es un transceptor
completo: los receptores del host reciben I/Q real del RTL-SDR, y `--relay-gpio` conmuta la antena.

```sh
sudo hpsdr_rpitx_bridge.py                          # sigue la frecuencia TX del host, emulación Hermes
sudo hpsdr_rpitx_bridge.py --freq 144.8M            # frecuencia TX fija
sudo hpsdr_rpitx_bridge.py --freq-offset 116M       # transverter: host 28,8 MHz -> 144,8 MHz
sudo hpsdr_rpitx_bridge.py --amplitude-mode constant --follow-drive
sudo hpsdr_rpitx_bridge.py --key-dot-gpio 17 --key-dash-gpio 27 --ptt-gpio 22
sudo hpsdr_rpitx_bridge.py --allow 192.168.1.20         # responde solo a este host
hpsdr_rpitx_bridge.py --dry-run --verbose           # solo protocolo, sin transmisor, sin root
sudo hpsdr_rpitx_bridge.py --rx rtlsdr --relay-gpio 17              # transceptor: receptor RTL-SDR, relé en GPIO17
sudo hpsdr_rpitx_bridge.py --rx rtlsdr --rx-args serial=00000002,direct_samp=2 --follow-rx-gain   # HF, ganancia del host
sudo hpsdr_rpitx_bridge.py --rx rtlsdr --rx-freq-offset -125M       # RTL-SDR detrás de un conversor de 125 MHz
hpsdr_rpitx_bridge.py --dry-run --rx rtlsdr         # solo recepción (sin transmisor), p. ej. para probar en una PC
```

| Área | Implementación |
|------|----------------|
| Descubrimiento | UDP broadcast/dirigido en el puerto 1024 y TCP; MAC, firmware, id de placa, receptores; bytes extra de Hermes-Lite 2 |
| Placas | `metis`, `hermes` (por defecto), `hermes2`/`griffin`, `angelia`, `orion`, `orion2`, `hermeslite` |
| Inicio / parada | I/Q EP6 y wideband EP4, por UDP o TCP; otro host toma el control con un inicio; una parada desde otro host se ignora |
| C&C del host (EP2) | Todas las direcciones decodificadas (frecuencia, receptores, dúplex, NCO de TX/RX, potencia, filtros, atenuadores, micrófono, PureSignal, manipulador CW, extensiones HL2); `--verbose` registra los cambios |
| Estado (EP6) | PTT/punto/raya, entradas IO inactivas, firmware, potencia directa (`--fwd-counts`), tensión de alimentación, sobrecarga del ADC (recorte del receptor, con `--rx`), temperatura HL2, nivel de la FIFO TX y confirmaciones |
| I/Q de recepción | Con `--rx`: la captura del receptor (un múltiplo entero de la frecuencia del host que acepte, p. ej. 240 kS/s para 48 kHz en un RTL-SDR) convertida a banda base y diezmada a 48/96/192/384 kHz para cada receptor del host (hasta 12) en su propia frecuencia; los receptores fuera de la captura envían ceros; EP6 va cadenciado por el reloj del receptor. Sin `--rx`: ruido sintético o ceros cadenciados por el reloj del sistema. Micrófono en silencio en ambos casos |
| Control del receptor | Sigue las frecuencias RX del host (más `--rx-freq-offset`, por defecto `--freq-offset`); el centro de la captura es RX1 más un desplazamiento que deja la espuria de DC del RTL-SDR fuera de la banda (`--rx-offset`) y se resintoniza solo cuando RX1 sale del 90 % de la captura; ganancia `--rx-gain` (dB o `auto`) o, con `--follow-rx-gain`, desde el atenuador del host / ganancia LNA de HL2; `--rx-antenna`, `--rx-ppm`, `--rx-rate`, `--rx-args`, `--rx-stream-args` |
| Conmutación T/R | La hace el driver: `--relay-gpio`, `--relay-active-low`, `--relay-tx-delay-ms`, `--relay-hang-ms`; salida PTT a un amplificador o radio con `--ptt-out-gpio`, `--ptt-out-active-low`, `--ptt-tx-delay-ms`, `--ptt-rx-delay-ms` (`--ptt-gpio` es la *entrada* de PTT); el receptor se silencia mientras se transmite salvo con `--rx-during-tx` |
| Transmisión | I/Q TX del host mientras MOX está activo, en el NCO de TX (o RX1 cuando el dúplex está desactivado) |
| CW | Manipulador iámbico A/B o recto con paletas en GPIO y temporización del host, CWX de HL2, entrada PTT por GPIO |
| Watchdog | El streaming se detiene y se libera el TX tras `--host-timeout` s sin paquetes del host |
| Acceso | Solo se responde a los hosts de `--allow`: `private` (por defecto: direcciones loopback, privadas y de enlace local), `any`, o direcciones / redes CIDR separadas por comas; solo el host que inició el stream puede activar el transmisor; como máximo 4 conexiones TCP (la inactiva más antigua deja lugar) |

Notas: en SSB con el micrófono en silencio no hay portadora (en modo `envelope` las magnitudes por debajo de
0,5 / (PAD + 1) del fondo de escala apagan la portadora; el puente muestra un aviso). Con la opción *CW handled in
Radio* de piHPSDR el host no activa MOX en CW: conecte paletas a las entradas GPIO o desactive esa opción. piHPSDR
limita las radios Protocol 1 a 61,44 MHz; use `--freq-offset` o un LO de transverter para VHF (con `--rx` el receptor
sigue el mismo desplazamiento salvo que se indique `--rx-freq-offset`). El informe de estado agrega el centro del
receptor, la frecuencia de captura, overflows, muestras descartadas y cortes; cuando el receptor deja de entregar
muestras el puente mantiene al host en marcha con ruido.

---

## Amplitud precisa y EER

El pin solo modula la amplitud mediante la corriente de salida del pad: 8 niveles más "apagado", aproximadamente un
cuantizador de 3 bits cuyos escalones ni siquiera son lineales (el ajuste de corriente cambia la resistencia de
salida y los niveles altos se saturan). El driver ofrece tres formas de mejorarlo, que pueden combinarse.

### 1. Tablas calibradas y conformación de ruido (sin hardware adicional)

- `amplitude_lut` reemplaza el modelo lineal `(nivel + 1) / 8` por la amplitud medida de cada nivel, de modo que el
  nivel más cercano lo es de verdad; `ampm_lut` compensa en la portadora el desfase de cada nivel. Mida ambos con
  `rpitx_amp_calibrate.py pad` en la banda en uso.
- `amplitude_quantizer=sigma_delta` realimenta el error de cuantización (orden 1 o 2): la amplitud media sigue
  exactamente a la envolvente y el error se convierte en ruido alejado de la portadora. La mejora dentro de banda
  crece con la relación de sobremuestreo (frecuencia de posiciones / ancho de banda de la señal); `slots_per_sample`
  eleva aún más la frecuencia de posiciones (la señal I/Q se interpola). **El ruido conformado cae hasta ±
  frecuencia de posiciones / 2 de la portadora, en los canales vecinos**, donde un filtro de banda no lo elimina:
  use una frecuencia de posiciones alta y un filtro de salida estrecho, y verifique el espectro.
- `envelope_delay` alinea la envolvente con la fase. El valor por defecto de 0,5 muestras hace coincidir la
  envolvente mantenida de una posición con la rampa de fase que el PLL produce durante ella; una prueba de dos tonos
  (`rpitx_amp_calibrate.py delay`) encuentra el mejor valor para una instalación dada.
- `envelope_floor` apaga la portadora cuando la envolvente se mantiene por debajo de un nivel durante 2 ms (silencio
  entre palabras), en lugar de transmitir el ruido de cuantización.

### 2. RF-DAC: un segundo pin de reloj (dos resistencias y un transformador)

GPCLK0 también está disponible en **GPIO20** (ALT5). Con `rf_dac=true` el DMA conmuta GPIO4 y GPIO20 de forma
independiente; sumados mediante resistencias ponderadas (p. ej. GPIO4 a través de R y GPIO20 a través de 2R hacia un
transformador de banda ancha), los dos pines y los 8 niveles de corriente dan unas tres veces más escalones de
amplitud. Mida los pesos reales con `rpitx_amp_calibrate.py rfdac` (`rfdac_lut`). Las funciones de GPIO 20–29
quedan congeladas durante la transmisión.

### 3. Eliminación y Restauración de Envolvente (modulador de envolvente externo)

Con `envelope_output=pcm` o `pwm`, GPIO4 lleva solo la **fase**: funciona con la corriente fija `eer_drive` en cada
muestra de una ráfaga y excita un amplificador conmutado (clase D/E, p. ej. BS170 en paralelo o una etapa IRF510
tras un buffer 74AC). La **envolvente** la escribe la misma posición DMA en la FIFO de cadencia y sale por un DAC
que excita el modulador de alimentación de ese amplificador (un amplificador operacional de potencia para QRP, o un
modulador buck/híbrido):

```
I/Q ─► camino de fase: frac de PLLC + GPIO4 encendido, corriente fija ──► GPIO4 ─► buffer ─► PA clase D/E ─► filtro ─► antena
     └► camino de envolvente: eer_lut⁻¹ → código de 16 bits (misma posición DMA) ─► DAC ─► modulador de alimentación ─┘ V_DD(t)
```

- **`pcm`**: maestro I2S estéreo de 16 bits, 64 relojes de bit por trama, ambos canales llevan la envolvente.
  Conecte un DAC I2S **acoplado en continua** (p. ej. PCM5102A, SCK a masa): GPIO18 → BCK, GPIO19 → LRCK,
  GPIO21 → DIN. Los códecs acoplados en alterna no pueden entregar una envolvente. Código 0 … 32767 = envolvente
  0 … fondo de escala (la mitad positiva de la salida).
- **`pwm`**: canal PWM 1 en modo marca/espacio en GPIO18 (ALT5) o GPIO12 (ALT0), periodo de 2^`pwm_bits` relojes
  PWM, un periodo por posición; sígalo de un pasabajos LC/RC y un buffer, o excite el conmutador de un modulador
  buck. El bloque PWM da entonces también la cadencia del DMA (el bloque PCM no se usa). Requiere
  `dtparam=audio=off`.

Ambos caminos salen de una misma posición DMA, así que son síncronos por construcción. La palabra del DAC sale de la
FIFO de cadencia un número fijo de posiciones después de que se aplican los registros de su posición (nivel DREQ
+ 1: 17 posiciones con PCM, 9 con PWM); el driver escribe cada palabra de envolvente esa cantidad de posiciones
antes, y `envelope_delay` compensa lo que queda (retardo del filtro del DAC, ancho de banda del modulador). Calibre
el modulador y el amplificador con `rpitx_amp_calibrate.py eer` (`eer_lut`, `eer_ampm_lut`) y el retardo con la
prueba de dos tonos. Por debajo de `envelope_floor` también se apaga el pin de reloj, lo que elimina la fuga de
excitación del amplificador durante los silencios.

El ancho de banda de la envolvente de SSB es varias veces el ancho de banda de audio: 48 kS/s bastan para SSB de
3 kHz, 96 kS/s dan margen. El modulador de alimentación debe tener al menos ese ancho de banda.

Estos tiempos y secuencias de registros se verifican contra el motor DMA simulado de las pruebas; confírmelos en la
placa de destino (osciloscopio en GPIO4 y en la salida del DAC, espectro con un SDR) antes de transmitir al aire.

[`rpitx_autotune.py`](#rpitx_autotunepy) ejecuta estas calibraciones, la búsqueda del retardo y la elección del
cuantizador, el sobremuestreo, la escala y los ajustes de silencio automáticamente en lazo cerrado con un receptor,
y verifica el resultado en cada modo de modulación.

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

Cada muestra I/Q se convierte en una **posición del anillo** (o `slots_per_sample` posiciones de la señal
interpolada): tres palabras de 32 bits en la memoria de VideoCore (multiplicador fraccional del PLL, palabra de
corriente de los pads, palabra de `GPFSEL0`, más una palabra de `GPFSEL2` con `rf_dac` y una palabra de envolvente con
un DAC de envolvente) y cuatro o cinco bloques de control DMA que las copian a los periféricos. El último bloque de
control escribe una palabra en la FIFO de transmisión del PCM (la FIFO PWM con `envelope_output=pwm`) y está
cadenciado por su línea DREQ, de modo que el motor avanza exactamente una posición por periodo; la palabra es un valor
ficticio, o el código de envolvente con un DAC de envolvente. El último bloque de control apunta de nuevo al primero: el DMA reproduce el anillo
indefinidamente sin intervención de la CPU, y el hilo alimentador solo tiene que mantenerse por delante.

### Síntesis de frecuencia: modulación de PLLC

La portadora es GPCLK0 alimentado por **PLLC**. Al crear un stream, el driver activa el "modo PLL avanzado" de
librpitx:

1. Se guardan los registros del árbol de relojes que van a cambiar (`CORECLK_CNTL/DIV`, `EMMCCLK_CNTL/DIV`,
   `CM_GP0CTL/DIV`, `CM_PLLC`, `PLLC_CTRL/FRAC`, `PLLC_CORE0/1/2`, `PLLC_PER`, `A2W_PLLC_ANA0..3`; en BCM2711 también
   `EMMC2CLK_CNTL/DIV`) y se anotan en el archivo de sesión.
2. El reloj del núcleo VPU se mueve a PLLA (divisor 4), para que no dependa de PLLC. El generador EMMC (host SD o
   SDIO) y, en BCM2711, el generador EMMC2 (tarjeta SD) pasan a PLLD_PER si estaban en PLLC_PER, recalculando el
   divisor para conservar su frecuencia; con cualquier otra fuente no se tocan. Los generadores que sigan alimentados por PLLC (por ejemplo vía `PLLC_CORE1/2`) se
   listan en un aviso.
3. Se desactiva `PLLC_CORE0`, se habilita el canal de periféricos de PLLC con divisor 1 (2 en BCM2711) y GPCLK0 toma
   de él su fuente, con MASH 0 (divisor entero, sin espurias de modelado de ruido).

Para una portadora F (frecuencia del PLL F/armónica), `ComputeBestLO()` elige el divisor entero D de GPCLK0: recorre
D de 4095 a 2 buscando una frecuencia de VCO F·D entre 200 MHz y 1,5 GHz tal que toda la banda F ± B
(B = 0,58 × frecuencia de posiciones, el mayor desplazamiento instantáneo incluidas las correcciones de fase) corresponda al mismo multiplicador **entero** del PLL. Por encima de una portadora de
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
muestra** (realimentación de error de primer orden, como mucho 0,5 rad aplicados por posición, el resto se conserva
para las siguientes), de modo que la fase de la portadora sigue a la fase I/Q sin deriva en transmisiones PSK largas.
Las correcciones AM/PM (`ampm_lut`, `eer_ampm_lut`) se suman a esa misma fase arrastrada.

La amplitud usada en una posición es la envolvente retardada `envelope_delay` (interpolación lineal de las
magnitudes recientes). Con un DAC de envolvente, la latencia de su FIFO se compensa escribiendo las palabras de
envolvente de una muestra en una posición anterior; las posiciones de las últimas muestras de una ráfaga conservan
entonces la envolvente de reposo.

Las tres palabras de una posición de datos:

| Palabra | Valor |
|---------|-------|
| `PLLC_FRAC` | `0x5A000000 \| frac(f / armónica + arrastre)` |
| `PADS_GPIO_0` | `0x5A000000 \| HYST(original) \| SLEW \| nivel` — modo envelope: el nivel del estado de amplitud elegido para `a·iq_scale` (más cercano o sigma-delta; sin tablas `nivel = round(min(a·iq_scale, 1)·(drive+1)) − 1`), valor original de los pads si el estado es "apagado"; modo constant: `nivel = drive`; EER: `eer_drive` |
| `GPFSEL0` | Funciones de GPIO 0–9 capturadas al activar, con GPIO4 = ALT0 (portadora encendida) o entrada (portadora apagada) |
| `GPFSEL2` (`rf_dac`) | Funciones de GPIO 20–29 capturadas al activar, con GPIO20 = ALT5 o entrada |
| envolvente (`pcm` / `pwm`) | `código << 16 \| código` con `código = round(32767·c)` (PCM), o `round(2^pwm_bits·c)` (PWM), `c` = `eer_lut⁻¹(a·iq_scale)·eer_gain + eer_offset` limitado a [0, 1] |

Una **posición en reposo** contiene la palabra fraccional de la frecuencia central, el valor original de los pads y
GPIO4 = entrada: el GPCLK sigue funcionando internamente, pero nada sale por el pin.

### Cadencia de muestreo: el bloque PCM

El controlador PCM/I2S se configura como transmisor de un canal y 8 bits cuya longitud de trama (`PCM_MODE.FLEN`) es
el menor valor ≥ 10 que mantiene el divisor del reloj PCM entre 2 y 4095. El reloj PCM proviene de PLLD con MASH 1 y
un divisor fraccional de 12 bits, corregido con los mismos ppm que la portadora (con `envelope_output=pcm`: un maestro
I2S estéreo de 16 bits con 64 relojes de bit por trama y palabras de FIFO empaquetadas). Los umbrales DREQ y de
pánico de la FIFO TX son de 16 palabras; cada trama consume una palabra, por lo que la escritura de cadencia de cada
posición se bloquea mientras la FIFO contiene 16 palabras — una posición por trama. Con `envelope_output=pwm` el canal
PWM 1 cumple la misma función (marca/espacio, FIFO desde el DMA, nivel DREQ 8, reloj desde PLLD a frecuencia de
posiciones × 2^`pwm_bits`).

Cada arranque del DMA (activación, retune, reinicio tras un error del DMA) empieza con posiciones en reposo: el motor
las recorre hasta que la FIFO de cadencia alcanza su nivel DREQ, de modo que ninguna posición de datos se reproduce
más rápido que la frecuencia de posiciones. La FIFO se vacía y el transmisor se reinicia en cada arranque, y se
detiene cuando se detiene el DMA, lo que silencia un DAC de envolvente. La frecuencia producida (sensor
`sample_rate`) difiere de la nominal por la cuantización del divisor, menos de 1 ppm.

### Motor DMA y memoria

- **Memoria:** una única reserva contigua de VideoCore (`mem_alloc` con `MEM_FLAG_DIRECT` en BCM2836 y posteriores,
  el alias sin caché `0xC`, o `MEM_FLAG_L1_NONALLOCATING` en BCM2835; `HINT_PERMALOCK`), bloqueada para obtener su
  dirección de bus y mapeada en el proceso mediante `/dev/mem`. Primero los bloques de control (alineados a
  32 bytes), luego las palabras de las posiciones: `anillo × (4·32 + 3·4)` = 140 bytes por posición (hasta `5·32 + 5·4` = 180 bytes con `rf_dac` y un DAC de
  envolvente). Cada respuesta
  del mailbox se verifica (resultado del ioctl, código de respuesta, bit de respuesta de la etiqueta).
- **Bloques de control:** `TI = NO_WIDE_BURSTS | WAIT_RESP`, más `D_DREQ | PERMAP(2)` (PCM TX) o `PERMAP(5)` (PWM)
  para el bloque de cadencia; una transferencia de 4 bytes cada uno, con destino la dirección de bus del periférico
  (`0x7E102220` PLLC_FRAC, `0x7E10002C` PADS_GPIO_0, `0x7E200000` GPFSEL0, `0x7E200008` GPFSEL2, `0x7E203004` FIFO
  PCM, `0x7E20C018` FIFO PWM).
- **Canal:** `CS = PRIORITY(7) | PANIC_PRIORITY(7) | DISDEBUG | ACTIVE`. Solo se usan canales clásicos (0–14; 0–10 en
  BCM2711, donde 11–14 son canales DMA4 con otro formato de bloque de control). Los canales del kernel
  (`dma-channel-mask` o `brcm,dma-channel-mask` de cada nodo `soc/dma@…` y `soc/dma-controller@…`) nunca se tocan; por
  defecto se usa el primero de 7, 8, 9, 10, 6, 5 (BCM2711) o 14 … 5 que no sea del kernel ni esté activo. Si no puede
  leerse ninguna máscara, el canal no se adivina: hay que indicar `dma_channel`. Un canal explícito que esté activo
  solo se reinicia con `dma_force=true`.

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
- Cuando la FIFO está vacía y el llenado cae por debajo de `low = max(anillo/8, frecuencia/200, anticipación de la envolvente + guarda + 16)`, rellena con
  posiciones en reposo; si había una ráfaga en curso sin `END_BURST`, se cuenta un **underflow**.
- Si la posición del DMA es inválida o el canal deja de estar `ACTIVE` durante 5 lecturas seguidas, se considera que
  el motor se detuvo: el evento se registra con `DMA_CS` / `DMA_DEBUG`, se incrementa `dma_errors` y el stream se
  reanuda con las muestras no reproducidas.

**Arranque y pre-roll:** tras la activación, el alimentador llena el anillo con el DMA detenido. El DMA arranca en la
posición 0 cuando hay `preroll_ms` de señal encolada, cuando se señala el fin de ráfaga, cuando la FIFO está llena o
cuando la primera muestra encolada lleva `preroll_ms` esperando (así las ráfagas cortas también salen).

**Retune:** el hilo de control publica la petición con un número de secuencia y espera hasta 3 s a que el
alimentador la tome (y luego hasta que termine). El alimentador pausa el motor y registra su posición exacta (a un
motor que se detuvo solo no se le atribuyen vueltas), detiene el DMA, **pasa GPIO4 a entrada y restaura los pads**
antes de tocar el PLL, copia las posiciones de datos no reproducidas del anillo (las muestras I/Q originales se
conservan en un arreglo sombra y cada posición se marca como de datos o inactiva) a un búfer de repetición,
reprograma PLLC y el reloj PCM, reinicia el anillo y continúa: el búfer de repetición se envía antes que la FIFO. Las
posiciones inactivas anteriores a la primera posición de datos se descartan; las inactivas entre posiciones de datos se
repiten como inactivas. Tras un error del DMA sin una posición válida del motor, pueden enviarse dos veces hasta 5
periodos del alimentador. Una petición que el PLL, el reloj de cadencia o el DAC de envolvente no pueden satisfacer se
rechaza antes de reprogramar nada y deja vigente la configuración anterior (incluidas las correcciones del cristal).

**Detección de vaciado:** el alimentador evalúa la condición de vaciado en un solo lugar al final de cada periodo
(todas las muestras introducidas hasta ese momento consumidas de la FIFO — se comparan los contadores de inserción y
extracción de la propia FIFO, así que ninguna instantánea puede quedar desactualizada —, búfer de repetición vacío,
última posición de datos completada) y publica el contador de muestras introducidas al que corresponde; `WaitDrained()` lo compara con
el contador actual. Un retune lo borra antes de reconstruir el anillo, de modo que nunca se informa una ráfaga como
enviada mientras hay muestras en tránsito entre la FIFO, el búfer de repetición y el anillo.

### La FIFO

`SpscFifo` es una cola acotada sin bloqueos de un productor y un consumidor con contadores de cabeza y cola de
64 bits (acquire/release), de modo que el nivel de llenado es siempre `head − tail`. Su almacenamiento se reserva
para `fifo_ms` a la frecuencia de muestreo máxima; la capacidad utilizable es `max(frecuencia·fifo_ms/1000, 1024)` y se
actualiza en cada retune. Un productor bloqueado espera en una variable de condición en intervalos de 2 ms, y el
alimentador lo despierta cuando libera espacio. Cada inserción se registra antes de que el productor compruebe que el
transmisor está activo; `Stop()` espera las inserciones registradas antes de vaciar la FIFO, y una escritura nunca
inserta en una activación posterior a aquella en la que empezó.

El fin de ráfaga se guarda como el contador de inserciones en el momento de `END_BURST`: una ráfaga terminó cuando no
se insertó nada después, así que una ráfaga nueva que empieza justo tras un `END_BURST` nunca se confunde con un
vaciado de la anterior.

**Higiene del hilo de tiempo real:** el alimentador (`SCHED_FIFO`) no reserva memoria en su camino de retune (los
búferes de repetición se reservan al abrir), nunca libera un modelo de amplitud (los modelos reemplazados los libera
el hilo de control cuando el alimentador ya no los usa) y no llama al logger de SoapySDR: sus mensajes van a una cola
sin bloqueos que el stream TX imprime cada milisegundo (y que `Stop()` y el destructor vacían).

### Conmutación transmisión/recepción

`SoapyRPITX_Ptt` guarda el estado de transmisión que comparten el stream TX y el camino RX y secuencia los pines del
relé y del PTT. `writeStream()` registra cada escritura en él: la primera escritura tras un período inactivo activa
el transmisor, lleva el pin del relé a la posición TX y programa la línea PTT `relay_tx_delay_ms` después y la
primera muestra `ptt_tx_delay_ms` después de la línea PTT. La escritura duerme en tramos de 2 ms como máximo,
consultando al secuenciador cada vez, hasta que las muestras pueden empujarse a la FIFO — el DMA solo ve datos que
llegaron después de que el relé se asentó y transcurrió la demora del PTT. La demora del PTT se cuenta desde el
momento en que la línea se afirmó realmente, por la escritura o por el supervisor, lo que ocurra primero. Un hilo supervisor del stream TX consulta cada milisegundo si el
transmisor se vació (el alimentador publica el contador de inserciones hasta el que todo se reprodujo, el mismo valor
que usa `WaitDrained()`) y lo desactiva cuando lleva `relay_hang_ms` vacío y sin escrituras en curso. Cada escritura
avanza un número de secuencia que el supervisor lee *antes* de evaluar el estado de vaciado; si una escritura empezó
entre medio, el transmisor sigue activado. Así nunca se reproducen muestras con el relé en posición RX ni con la
línea PTT liberada, ni siquiera con `relay_hang_ms=0`. La desactivación libera la línea PTT y el supervisor devuelve
el relé a RX cuando transcurre `ptt_rx_delay_ms`; hasta entonces el transmisor cuenta como activado. El camino de
recepción sigue silenciado `rx_mute_tail_ms` después de que el relé vuelve a RX. Desactivar, cerrar o destruir el
stream detiene primero el transmisor (portadora apagada) y desactiva después, esperando `ptt_rx_delay_ms` entre la
línea PTT y el relé. Como el DMA reescribe palabras GPFSEL0/GPFSEL2 completas con los valores tomados en `Start()`,
los pines del relé y del PTT se configuran como salidas antes de que exista un stream TX y sus funciones nunca
cambian con uno abierto; sus niveles pasan por GPSET0/GPCLR0, que el DMA no escribe.

El receptor se abre con la función de fábrica que registró su módulo (no mediante `SoapySDR::Device::make()`, que
ya está creando el dispositivo rpitx); pertenece al dispositivo rpitx y se cierra con él. `readStream()` corre sin el
mutex del dispositivo, como `writeStream()`, y se contabiliza, así `closeStream()` espera una lectura en curso antes
de cerrar el stream del receptor.

### Cierre, recuperación tras caídas y seguridad

- **Cierre normal** (`deactivateStream` / `closeStream` / destrucción del dispositivo): se espera al alimentador, se
  reinicia el DMA, GPIO4 (y GPIO20) vuelven a su función original (entrada si era la salida de reloj), se restauran
  los pads, los pines del DAC de envolvente vuelven a su función original, se desactiva el bloque PCM y se restaura su
  reloj, se restaura el bloque PWM, se restaura el árbol de relojes (primero PLLC, esperando el enganche,
  luego sus canales, los relojes VPU y EMMC y GPCLK0), se libera la memoria VideoCore y se vacía el archivo de bloqueo.
  El registro deja de nombrar el canal DMA y la memoria VideoCore antes de liberarlos, así que una caída en ese
  intervalo puede dejar el bloque reservado (se recupera con un reinicio) pero nunca hace que el siguiente proceso
  libere memoria que puede pertenecer a otro cliente de VideoCore.
- **`exit()`:** un manejador `atexit` realiza una parada de emergencia (reinicio del DMA, portadora apagada, GPCLK0
  detenido) sin invalidar los objetos, de modo que un destructor posterior todavía completa el cierre.
- **Señales** (`signal_cleanup=true`): manejadores para SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL
  y SIGABRT. Cuando la disposición previa era la acción por defecto (el proceso está por morir) realizan escrituras de
  registros seguras en señales (reinicio del DMA, GPIO4 / GPIO20 a entrada, pads, GPCLK0 apagado, DAC de envolvente detenido), restauran la acción previa y
  vuelven a lanzar la señal; las señales ignoradas y los manejadores de la aplicación pasan sin cambios (un manejador
  de la aplicación se ejecuta con su propia máscara de señales y, si se instaló con `SA_RESETHAND`, tras restaurar la
  acción por defecto). El árbol de relojes no se puede restaurar desde un manejador de señales (requiere esperas y
  enganches del PLL): la VPU queda en PLLA y PLLC conserva su última configuración hasta que la siguiente apertura del
  driver recupera el registro, o hasta que se ejecuta `rpitx_reset` — use `ExecStopPost=/usr/local/bin/rpitx_reset`
  en los servicios.
- **Salida mientras otro hilo cierra el dispositivo:** el manejador de salida y el destructor del transmisor están
  serializados, así que el destructor espera a una parada de emergencia en curso.

#### Recuperación tras una caída

Mientras existe un transmisor, `/run/soapy_rpitx/soapy_rpitx.lock` (tmpfs, nunca sobrevive a un reinicio; directorio
privado) se mantiene con un `flock()` exclusivo y contiene un registro `clave=valor` de todo lo que posee el proceso:
PID, canal DMA, handle de memoria VideoCore, dirección de bus y tamaño (escritos en cuanto se reserva la memoria y
borrados antes de liberarla, junto con si la máscara de canales DMA del kernel se podía leer), valor original de los pads y
funciones de GPIO4 y de los demás pines usados, reloj PCM original o estado del PWM, y el árbol de relojes (guardado
y registrado antes de su primera modificación). El registro termina con una línea `end=1`: lo que haya después
(dejado por una actualización interrumpida entre la escritura de un registro más corto y el recorte del archivo) se
ignora.
El kernel libera el bloqueo cuando el proceso muere, incluso con `kill -9`. El siguiente proceso que obtiene el
bloqueo y encuentra un registro no vacío sabe que el dueño murió sin limpiar y lo recupera: reinicio del canal DMA (solo
si el canal está inactivo o su bloque de control actual está dentro del bloque registrado; si otro programa tomó el
canal entretanto, no se toca y el informe dice «now used by another program; not reset»; un registro sin la
ubicación del bloque, de una versión anterior, usa la dirección de bus que da el mailbox),
GPIO4 y pads restaurados, GPCLK0 detenido, PCM desactivado y su reloj restaurado, PWM restaurado, árbol de relojes
restaurado, y la memoria de GPU del proceso muerto liberada (los handles de VideoCore son globales para el firmware).
Antes se verifica el registro: un canal DMA que no sea un canal clásico del SoC o que pertenezca al kernel, pines
fuera de GPIO 0–27 o valores de reloj inválidos hacen que se **descarte** (se informa, no se aplica nada). Cuando la
máscara DMA del kernel no se puede leer, el canal se acepta solo si el registro indica que tampoco se podía leer
cuando se reservó el canal. Un registro sin la marca final (escritura interrumpida, o registro de una versión
anterior del driver) se recupera, pero su handle de memoria VideoCore no se libera.
`rpitx_reset` hace lo mismo a pedido. Un proceso creado con `fork()` sin `exec()` comparte el bloqueo: mientras ese
hijo viva, el siguiente proceso es informado de que el bloqueo lo tiene un hijo del proceso terminado.

#### Comprobaciones de conflictos al abrir

- otro proceso con el bloqueo → "the transmitter is in use by process N";
- GPIO4 (y GPIO20, los pines PCM 18/19/21 o el pin PWM cuando se usan) reclamado por cualquier consumidor en el chip
  GPIO del SoC (`pinctrl-bcm2…`, mediante la información de línea del dispositivo de caracteres GPIO, petición v2
  con alternativa v1; se registra un aviso cuando ninguna responde);
- `soc/i2s@7e203000` habilitado en el device tree (salvo con `envelope_output=pwm`); con `envelope_output=pwm`,
  `soc/pwm@7e20c000` o el nodo de audio analógico habilitados (un nodo sin propiedad `status` cuenta como habilitado);
- el directorio o el archivo de bloqueo no privados del usuario que ejecuta el driver;
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
| "the PWM block is enabled" | `envelope_output=pwm`: ponga `dtparam=audio=off`, quite los overlays `pwm` y reinicie |
| "the kernel DMA channel mask cannot be read" | Indique en `dma_channel` un canal fuera de la máscara del kernel (`xxd /proc/device-tree/soc/dma*/*dma-channel-mask`) |
| "the lock directory … must be … mode 0700" | `sudo rm -rf /run/soapy_rpitx` (se vuelve a crear) |
| "in use by a child process of terminated process N" | Un hijo creado con fork conservó el bloqueo; termínelo |
| `envelope_clip` aumenta | La envolvente supera el fondo de escala: baje `iq_scale` o el nivel de la aplicación |
| `envelope_late` aumenta | El anillo quedó casi vacío durante una ráfaga: aumente `preroll_ms` / `fifo_ms` |
| "VideoCore memory allocation failed" | Reduzca `ring_size`, aumente `gpu_mem` o ejecute `sudo rpitx_reset` para liberar memoria dejada por una caída |
| `underflows` aumenta | La aplicación es demasiado lenta: aumente `preroll_ms` y `fifo_ms`, escriba bloques más grandes |
| `late` aumenta | El alimentador se demoró: mantenga `rt_priority` > 0, fíjelo (`cpu=3`), considere `isolcpus=3` y el gobernador `performance` |
| `dma_errors` aumenta | Otra cosa reinició o usó el canal DMA; elija otro canal |
| "crosses a PLL integer step" | Las desviaciones más allá del escalón se recortan: mueva levemente la frecuencia o baje la frecuencia de muestreo |
| "PLLC did not report lock" | Las frecuencias muy altas pueden no enganchar en todos los chips; baje la frecuencia o use `harmonic` |
| Consola serie / I2C / SPI con errores durante la transmisión | Esperado: el reloj de la VPU está en PLLA mientras el stream está abierto (`gpu_freq=250` lo hace predecible) |
| Portadora trabada tras una caída | `sudo rpitx_reset` (o abra cualquier stream): se recupera el registro de sesión |
| "the PLLA peripheral channel is not running" | El firmware dejó PLLA apagado, así que el reloj de la VPU no puede salir de PLLC; la placa no puede transmitir con este driver |
| "the transmitter thread did not respond within 3 s" | El hilo alimentador quedó sin CPU (¿otra tarea `SCHED_FIFO`?); el cambio no se aplicó, repítalo |
| "not a regular file" / "writable by group or others" / "owned by another user" en un perfil | Use un archivo regular con modo 0644 (o más estricto), perteneciente a root, al usuario que ejecuta el driver o (con `sudo`) al usuario que ejecutó `sudo` |
| "a directory of the path is owned by another user" / "writable by others" / "symbolic link" en un perfil | Todos los directorios de la ruta deben pertenecer a root o a ese usuario, no deben poder ser escritos por los demás (salvo con bit sticky) y no deben ser enlaces simbólicos: indique la ruta real. Con `sudo`, se aceptan los directorios del usuario que ejecutó `sudo` si ni el grupo ni los demás pueden escribirlos |
| "output directory … is owned by another user" / "writable by group or others" (autotune) | Use un directorio `--out` propio (o de root) con modo 0755 o más estricto, que no sea un enlace simbólico |
| Sin portadora en SSB con el puente HPSDR | Micrófono en silencio; vea las notas del puente más arriba |
| "receiver driver 'rtlsdr' is not installed" | Instale el módulo SoapySDR del receptor (`soapysdr-module-rtlsdr`); `SoapySDRUtil --info` debe listar `rtlsdr` |
| "cannot open the receiver 'rtlsdr'" | Falta el dongle, lo usa otro programa (rtl_tcp, dump1090…) o está cargado el driver DVB del kernel (ponga en la lista negra `dvb_usb_rtl28xxu`); revise `rx.serial` |
| "GPIOn is the antenna relay (relay_gpio)" | El stream TX usa ese pin (`rf_dac`, `envelope_output`); mueva el relé a otro GPIO |
| "relay_gpio and relay_active_low cannot change while a TX stream is open" | Cierre el stream TX, cambie el ajuste y vuelva a abrirlo |
| "GPIOn is the PTT line (ptt_gpio)" | El stream TX usa ese pin (`rf_dac`, `envelope_output`); mueva la línea PTT a otro GPIO |
| "ptt_gpio and ptt_active_low cannot change while a TX stream is open" | Cierre el stream TX, cambie el ajuste y vuelva a abrirlo |
| "relay_gpio and ptt_gpio cannot be the same GPIO" | El relé y la línea PTT necesitan dos GPIO distintos |
| El amplificador se activa mientras el relé todavía se mueve, o hay RF antes de que el amplificador esté listo | Aumente `relay_tx_delay_ms` (tiempo de operación del relé) o `ptt_tx_delay_ms` (tiempo de activación del amplificador) |
| El relé conmuta mientras el amplificador todavía entrega potencia al final de una transmisión | Aumente `ptt_rx_delay_ms` |
| Receptor mudo mientras se transmite | Es lo esperado con `rx_mute_tx=true` (puente: `--rx-during-tx` para mantenerlo); sigue silenciado `rx_mute_tail_ms` tras la desactivación |
| El final de la transmisión se oye en el receptor | El receptor acumula más que `rx_mute_tail_ms`: auméntelo (o `relay_hang_ms`) |

---

## Pruebas

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Las pruebas se ejecutan en cualquier host Linux; `tests/mock_hw.cpp` reemplaza el mailbox, `/dev/mem`, las consultas al
device tree y el motor DMA (un hilo que interpreta la cadena de bloques de control, escribe los registros simulados y
modela la FIFO de cadencia PCM / PWM con su nivel DREQ, registrando en cada trama los registros vigentes y la palabra
que sale de la FIFO).

| Prueba | Cubre |
|--------|-------|
| `rpitx_unit_tests` | Orden de la FIFO con concurrencia y límite de capacidad; contabilidad del anillo contra un motor simulado (régimen estable, demoras cortas, vueltas perdidas, doble demora, falta de datos del host, motor detenido sin vueltas atribuidas); estimación de frecuencia del DSP, umbral y continuidad de fase a través de cero; modelo de amplitud, tablas, medias del sigma-delta, retardo de la envolvente y alineación envolvente/fase, correspondencia del código EER |
| `rpitx_devicetree_tests` | Base de periféricos, límite de canales DMA clásicos y máscara DMA del kernel con la nomenclatura antigua y la actual del device tree |
| `rpitx_transmitter_tests` | Alimentador, pre-roll, vaciado, retune con la portadora apagada y repetición del anillo, parada de emergencia y luego destrucción, reinicio del DMA, histéresis de los pads, capacidad de la FIFO según la frecuencia, fallos del constructor (frecuencia inalcanzable, reserva rechazada, anillo corto), selección de canal DMA (y sin adivinar sin la máscara del kernel), bloqueo de sesión y recuperación tras caídas (pines, PWM, máscara del kernel ilegible, marca final del registro, registro incompleto), directorio de bloqueo privado, enlace simbólico y registros inverosímiles, registro temprano de la reserva, nodos del device tree sin `status`, manejadores de señales, arranque cadenciado del DMA, retardo de la envolvente, sigma-delta, corrección AM/PM, RF-DAC, posiciones por muestra, salida de envolvente PCM y PWM con alineación envolvente/fase, fin de ráfaga antes de una ráfaga corta, repetición solo de las posiciones de datos (retune antes del arranque del DMA y tras un hueco inactivo), vaciado durante un retune concurrente, liberación registrada antes de liberar la memoria VideoCore, parada con un productor bloqueado (ninguna muestra llega a la activación siguiente, el vaciado se sigue detectando), ráfagas consecutivas sin falsos vaciados, canal PWM apagado hasta que arranca el DMA (también tras un retune con el transmisor detenido), EMMC pasado a PLLD a la misma frecuencia solo cuando está en PLLC, exploración de los clientes de PLLC, viabilidad del PLL sin hardware |
| `rpitx_driver_tests` | Dispositivo SoapySDR: ajustes y rangos, análisis estricto de números, ajustes de amplitud, corrección de frecuencia, argumentos del stream (incluida la salida de envolvente), escalado CF32/CS16/CS8, vaciado con `END_BURST`, `closeStream()` durante un vaciado, `activateStream()` sobre un stream activo, estado del stream, sensores, archivos de perfil y `profile=`, comprobaciones de los archivos de perfil (dispositivos, tuberías, tamaño, permisos, enlaces simbólicos), enteros y ganancias fuera de rango, límites de los tiempos de espera; modo transceptor con un receptor simulado registrado por la prueba (apertura, reenvío de la API y los ajustes RX, streams RX/TX simultáneos, verificación de handles, entradas de enumeración, silenciado durante la transmisión en formatos con y sin signo); relé de antena sobre los registros GPIO simulados (validación, polaridad, liberación del pin, conflictos con streams TX, demora de activación, tiempo de retención, desactivación al desactivar/cerrar, pines de GPFSEL0 bajo el DMA, claves del relé en perfiles); línea PTT (validación, polaridad, liberación del pin, conflictos con el relé y con streams TX, secuencia de activación relé → demora → PTT → demora → muestras, secuencia de desactivación PTT → demora → relé, reactivación durante la demora de liberación, PTT sin relé, sensor `ptt_line`, la secuencia a través de `writeStream()`); desactivación solo tras un vaciado observado después de que empezó la última escritura, cola de silencio, silencio en todos los formatos RX (incluidos los empaquetados), `closeStream()` que termina de inmediato una espera del relé y una lectura RX bloqueada (tiempos de espera del receptor por tramos), cada evento de estado informado una sola vez a consultas concurrentes, perfiles del usuario de `sudo` (ejecutando como root; si no, se omite) |

La CI (`.github/workflows/ci.yml`) ejecuta las pruebas con `-Werror` en una compilación normal, con AddressSanitizer +
UndefinedBehaviorSanitizer y con ThreadSanitizer (`tests/tsan.supp` suprime solo los accesos a registros simulados),
como root para que también corran las pruebas de propietarios, y verifica `clang-format`, `shellcheck` sobre
`rpi-cross.sh`, `pyflakes` sobre las herramientas en Python y `rpitx_autotune.py self-test --quick`.

`rpitx_autotune.py self-test` verifica el análisis y el optimizador de la herramienta de ajuste sobre un transmisor
y un receptor simulados (sin radio, sin root): recuperación de frecuencia, reloj, distorsión y tasas de error
conocidas, convergencia en ambos caminos, finales `HARDWARE_LIMIT` y `UNVERIFIABLE`, los límites de presupuesto y
`--resume`. `--quick` es la variante corta.

---

## Estructura del proyecto

```
src/
  SoapyRPITX.hpp                 clase de dispositivo SoapySDR y envoltorio del stream
  SoapyRPITX_Registration.cpp    detección de la placa, enumeración, registro en SoapySDR
  SoapyRPITX_Settings.cpp        identificación, ajustes, antena, ganancia, frecuencia, frecuencia de muestreo (TX, y reenvío RX)
  SoapyRPITX_Streaming.cpp       configuración del stream, conversión de formatos, escritura / lectura / estado / ráfagas
  SoapyRPITX_Receiver.cpp        apertura del receptor y reenvío del stream RX (modo transceptor)
  SoapyRPITX_Ptt.*               estado de transmisión y GPIO del relé de antena y de la línea PTT
  SoapySDR_Sensors.cpp           sensores
  SoapyRPITX_IQ_dmaSync.*        transmisor: anillo DMA, hilo alimentador, modulación, retune, cierre
  SoapyRPITX_Buffers.hpp         SpscFifo y DmaRingTracker (independientes del hardware)
  SoapyRPITX_Amplitude.hpp       estados de amplitud, sigma-delta, retardo de envolvente, correspondencia EER (independiente del hardware)
  SoapyRPITX_Profile.hpp         lector de archivos de perfil para profile=<archivo> (independiente del hardware)
  SoapyRPITX_Session.*           archivo de bloqueo, registro de caídas y recuperación, comprobación de conflictos
librpitx/src/                    librpitx integrada (dma, gpio/reloj/PCM/PWM/pads, mailbox, dsp, device tree)
tools/
  rpitx_reset.cpp                herramienta de recuperación
  rpitx_tone_test.py             transmisor de prueba (tono / WAV)
  rpitx_amp_calibrate.py         calibración de amplitud, AM/PM, RF-DAC, EER y retardo de la envolvente
  rpitx_autotune.py              prueba y ajuste en lazo cerrado sobre el aire de cada modo (pin directo y EER)
  hpsdr_rpitx_bridge.py          emulación de radio openHPSDR Protocol 1 (transmisor o transceptor)
tests/                           pruebas unitarias, del device tree, del transmisor y del driver, simulación de hardware, supresiones de TSan
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
