# SoapySDR_rpitx

[English](README.md) | [Español](README.es.md) | **Italiano**

**Un driver di trasmissione SoapySDR che trasforma un Raspberry Pi in un trasmettitore I/Q a banda stretta — senza
hardware aggiuntivo, solo il pin di clock GPIO4.**

SoapySDR_rpitx espone l'uscita di clock generica del Raspberry Pi (GPCLK0 su GPIO4) come un dispositivo TX standard
di [SoapySDR](https://github.com/pothosware/SoapySDR). Qualsiasi applicazione compatibile con Soapy — GNU Radio,
SDRangel, script Python, il bridge HPSDR incluso per piHPSDR / Thetis / linHPSDR / Quisk / SparkSDR — può inviargli
campioni complessi in banda base e ottenere RF dal pin del connettore. È basato su una copia integrata e rinforzata
di [librpitx](https://github.com/F5OEO/librpitx) di Evariste Courjaud F5OEO.

> **Stato: sperimentale.** Il percorso di trasmissione (anello DMA, alimentatore in tempo reale, modulazione del PLL,
> ripristino) è coperto da test sull'host con hardware simulato, comprese esecuzioni con AddressSanitizer,
> UndefinedBehaviorSanitizer e ThreadSanitizer. Le segnalazioni da schede reali sono molto gradite.

---

## Indice

- [Avvertenze radio e legali](#avvertenze-radio-e-legali)
- [Funzionalità](#funzionalità)
- [Hardware supportato](#hardware-supportato)
- [Requisiti e vincoli di sistema](#requisiti-e-vincoli-di-sistema)
- [Compilazione sul Raspberry Pi](#compilazione-sul-raspberry-pi)
- [Compilazione incrociata](#compilazione-incrociata)
- [Installazione e verifica](#installazione-e-verifica)
- [Utilizzo](#utilizzo)
- [Strumenti](#strumenti)
- [Ampiezza precisa ed EER](#ampiezza-precisa-ed-eer)
- [Funzionamento interno](#funzionamento-interno)
- [Risoluzione dei problemi](#risoluzione-dei-problemi)
- [Test](#test)
- [Struttura del progetto](#struttura-del-progetto)
- [Licenza e crediti](#licenza-e-crediti)

---

## Avvertenze radio e legali

GPIO4 emette un'**onda quadra**. È ricca di armoniche (armoniche dispari di ampiezza circa 1/n) e di prodotti spuri
del PLL frazionario. Inserire **sempre** un filtro passa-banda o passa-basso per la banda in uso tra il pin e
qualsiasi antenna, e trasmettere solo su frequenze e con potenze per cui si possiede la licenza — oppure su un carico
fittizio. Si è responsabili di ciò che il pin irradia.

Il driver mantiene la portante spenta quando non vengono riprodotti campioni e la spegne mentre il PLL viene
riprogrammato, ma l'albero dei clock viene modificato appena si crea uno stream.

---

## Funzionalità

| Caratteristica | Dettagli |
|----------------|----------|
| Direzione | Solo trasmissione, un canale |
| Formati dei campioni | `CF32` (nativo, fondo scala 1.0), `CS16` (fondo scala 32768), `CS8` (fondo scala 128) |
| Frequenza di campionamento | 10 kHz – 200 kHz (qualsiasi valore; elencate 24 k / 48 k / 96 k / 192 k; predefinita 48 kHz) |
| Frequenza | 50 kHz – 1500 MHz (BCM2711: 50 kHz – 1000 MHz) sulla fondamentale; `harmonic=n` moltiplica l'intervallo |
| Modulazione | Qualsiasi segnale I/Q: fase/frequenza tramite il moltiplicatore frazionario di PLLC, ampiezza tramite la corrente d'uscita dei pad (8 livelli, tabelle calibrate, conformazione del rumore sigma-delta opzionale e un secondo pin di clock) o un DAC d'inviluppo esterno (EER) |
| Modi di ampiezza | `envelope` (la corrente segue \|I/Q\|) o `constant` (corrente massima quando è presente un campione: FM, FSK, CW, PSK a inviluppo costante); uscita d'inviluppo `none` (corrente dei pad), `pcm` (DAC I2S a 16 bit) o `pwm` |
| Guadagno | Elemento `PAD`, 0–7 = 2 mA … 16 mA di corrente d'uscita |
| Correzione di frequenza | Stima NTP del kernel (`ntp_adjtime`) più ppm manuali; `setFrequencyCorrection()` |
| Burst | `SOAPY_SDR_END_BURST` rispettato: la disattivazione attende che il burst sia uscito dall'antenna |
| Streaming | FIFO lock-free, thread alimentatore in tempo reale, pre-roll configurabile, contropressione con timeout, scritture parziali |
| Modifiche al volo | Frequenza, frequenza di campionamento, guadagno e tutte le impostazioni durante la trasmissione, senza perdere campioni in coda |
| Diagnostica | Sensori per aggancio del PLL, stato del DMA, riempimento della FIFO, underflow, overrun del DMA, riavvii del DMA, frequenza di campionamento effettiva e saturazioni dell'inviluppo; `readStreamStatus()` segnala `SOAPY_SDR_UNDERFLOW` |
| Sicurezza | Lock a livello di sistema, registro dei crash e ripristino automatico, pulizia opzionale sui segnali, strumento `rpitx_reset`, rifiuto di utilizzatori in conflitto di GPIO / I2S / DMA |
| Integrazioni | GNU Radio (Soapy sink), SDRangel (uscita SoapySDR), Python, programmi host HPSDR Protocol 1 tramite il bridge |

Ciò che l'hardware fa bene: **NBFM, FSK/AFSK, modi digitali a inviluppo costante tipo WSPR/FT8, CW e BPSK**. AM e SSB
funzionano; con il solo pin la loro qualità è limitata dagli 8 gradini di ampiezza (vedi
[Ampiezza precisa ed EER](#ampiezza-precisa-ed-eer) per calibrazione, conformazione del rumore e un modulatore
d'inviluppo esterno).

---

## Hardware supportato

| Scheda | SoC | Stato |
|--------|-----|-------|
| Pi 1 A/A+/B/B+, Zero, Zero W/WH, CM1 | BCM2835 | supportata |
| Pi 2 B | BCM2836 / BCM2837 | supportata |
| Pi 3 B/B+/A+, CM3, Zero 2 W | BCM2837 / BCM2837B0 / RP3A0 | supportata |
| Pi 4 B, Pi 400, CM4 | BCM2711 | supportata |
| Pi 5, Pi 500, CM5 | BCM2712 + RP1 | **non supportata** (GPIO, clock e DMA si trovano nel chip RP1) |

- **Uscita RF: GPIO4, pin 7 del connettore** (GPCLK0, funzione alternativa 0). Massa sul pin 9.
- Sono supportati Raspberry Pi OS a 32 bit (armhf) e a 64 bit (arm64).
- La scheda viene identificata da `/proc/device-tree/model` e dalla base delle periferiche in
  `/proc/device-tree/soc/ranges` (`0x20000000`, `0x3F000000` o `0xFE000000`). `SoapySDRUtil --find` elenca il
  dispositivo solo sulle schede supportate.
- La modalità periferiche alte di BCM2711 (`arm_peri_high=1`) non è supportata.

---

## Requisiti e vincoli di sistema

- **Privilegi di root.** Il driver mappa i registri delle periferiche tramite `/dev/mem` e alloca memoria DMA tramite
  la mailbox VideoCore `/dev/vcio`. Senza accesso segnala un errore chiaro; non termina mai l'applicazione ospite.
- **Clock del core fisso.** PLLC viene modulato, quindi il clock del core VPU viene spostato su PLLA finché esiste uno
  stream. Come fa l'installer di rpitx, mantenere fissa la frequenza del core:

  ```ini
  # /boot/firmware/config.txt (o /boot/config.txt nelle versioni precedenti)
  gpu_freq=250
  ```

  L'albero dei clock viene salvato alla creazione dello stream e ripristinato alla chiusura. Mentre lo stream è
  aperto, le periferiche alimentate dal clock della VPU (mini-UART, I2C, SPI e il controller SD di Pi Zero/1/3)
  funzionano a una frequenza diversa da quella che il kernel crede.
- **Memoria della GPU.** Ogni posizione dell'anello DMA usa 140 byte di memoria VideoCore (pagine da 4 KiB; 144 byte
  con un DAC d'inviluppo, 180 byte con l'RF-DAC e un DAC d'inviluppo):
  4096 posizioni ≈ 560 KiB, 32768 posizioni ≈ 4,4 MiB. Mantenere `gpu_mem` al valore predefinito (≥ 64 MB) per anelli
  grandi.
- **GPIO4 deve essere libero.** Se un driver del kernel reclama GPIO4 — in particolare l'overlay `w1-gpio` (1-Wire),
  il cui pin predefinito è GPIO4 — il driver rifiuta lo stream.
- **Il blocco PCM/I2S deve essere libero.** Viene usato come clock di cadenza del DMA (e come DAC d'inviluppo con
  `envelope_output=pcm`); con un overlay di scheda audio I2S o `dtparam=i2s=on` lo stream viene rifiutato. Con
  `envelope_output=pwm` è il blocco PWM a dare la cadenza e deve essere libero: nessun overlay `pwm` e
  `dtparam=audio=off`.
- **La maschera dei canali DMA del kernel deve essere leggibile** dal device tree (`soc/dma@…` o
  `soc/dma-controller@…`, proprietà `dma-channel-mask` o `brcm,dma-channel-mask`) per la scelta automatica del canale;
  altrimenti indicare `dma_channel` esplicitamente.
- **Un solo trasmettitore per sistema**, garantito da un `flock()` su `/run/soapy_rpitx/soapy_rpitx.lock`. La
  directory viene creata con modo 0700 e deve appartenere all'utente che esegue il driver; un file di lock che non sia
  un file regolare di quell'utente (per esempio un collegamento simbolico) viene rifiutato.
- **Le funzioni di GPIO 0–9 restano congelate durante la trasmissione.** Il DMA riscrive l'intero registro `GPFSEL0` a
  ogni campione (è l'unico modo di commutare il pin di clock dal DMA). Le funzioni di GPIO 0–9 vengono campionate
  all'attivazione dello stream; non riconfigurarle (I2C su 2/3, SPI su 7–9, …) durante la trasmissione. Con
  `rf_dac=true` lo stesso vale per GPIO 20–29 (`GPFSEL2`).
- **La corrente d'uscita di GPIO 0–27 segue l'inviluppo.** Il registro dei pad `PADS_GPIO_0` è condiviso da GPIO 0–27 e
  viene riscritto a ogni campione (l'isteresi d'ingresso è conservata). I campioni a riposo e la fine dello stream
  ripristinano il valore originale.

---

## Compilazione sul Raspberry Pi

### Dipendenze

```sh
sudo apt update
sudo apt install git cmake g++ libsoapysdr-dev soapysdr-tools
# opzionale, per gli strumenti Python
sudo apt install python3-numpy python3-soapysdr python3-libgpiod
```

### Compilare e installare

```sh
git clone https://github.com/hiperiondev/SoapySDR_rpitx.git
cd SoapySDR_rpitx
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # opzionale: test sull'host con hardware simulato
sudo cmake --install build
```

Su un Pi Zero / Pi 1 usare `-j1` (512 MB di RAM). Lì la compilazione richiede alcuni minuti; su un Pi 4 ben meno di un
minuto.

Se SoapySDR è stato compilato dai sorgenti in `/usr/local`, configurare con `-DCMAKE_INSTALL_PREFIX=/usr/local`.

### Opzioni di CMake

| Opzione | Predefinito | Descrizione |
|---------|-------------|-------------|
| `RPITX_BUILD_DRIVER` | `ON` | Compila il modulo SoapySDR (richiede SoapySDR) |
| `RPITX_BUILD_RESET_TOOL` | `ON` | Compila e installa `rpitx_reset` |
| `RPITX_INSTALL_TOOLS` | `ON` | Installa `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py` e `rpitx_amp_calibrate.py` |
| `BUILD_TESTING` | `ON` (`OFF` in compilazione incrociata) | Compila i test per l'host |
| `RPITX_WERROR` | `OFF` | Tratta gli avvisi (`-Wall -Wextra -Wshadow`) come errori |

SoapySDR viene individuato tramite il suo pacchetto CMake (`SoapySDRConfig.cmake`, che fornisce
`SOAPY_SDR_MODULE_UTIL`). Se la configurazione del pacchetto manca (tipicamente un sysroot incrociato incompleto), la
compilazione ripiega sulla ricerca diretta di `SoapySDR/Device.hpp` e `libSoapySDR.so` e legge la versione ABI dei
moduli da `SoapySDR/Version.h`.

---

## Compilazione incrociata

### Con `rpi-cross.sh` (host x86_64 Fedora o derivato di Debian)

`rpi-cross.sh` conosce ogni modello di Pi (CPU, ISA, ABI in virgola mobile, tripletta multiarch di Debian), installa
la toolchain incrociata, costruisce un sysroot da un Pi in funzione, genera un file di toolchain CMake e verifica il
risultato.

```sh
./rpi-cross.sh models                                   # elenca i modelli e i flag del compilatore
./rpi-cross.sh deps    -m pi4 -b 64                     # installa la toolchain incrociata: dnf su Fedora, apt su Debian/Ubuntu/Mint (rilevato in automatico)
./rpi-cross.sh sysroot -m pi4 -b 64 -H pi@raspberrypi.local --pi-packages "libsoapysdr-dev" --ask-pass
./rpi-cross.sh build   -m pi4 -b 64                     # -> build-rpi-pi4-64/
./rpi-cross.sh verify  -m pi4 -b 64 build-rpi-pi4-64/librpitxSupport.so
./rpi-cross.sh install -m pi4 -b 64 -H pi@raspberrypi.local --ask-pass   # compila e installa sul Pi via SSH
```

- `-m` seleziona la scheda (`pi1 pizero cm1 pi2 pi2v12 pi3 pi3plus cm3 pizero2w pi4 pi400 cm4 pi5 pi500 cm5`); `-b` deve
  corrispondere allo **userland** del Pi (`dpkg --print-architecture`: `armhf` → 32, `arm64` → 64).
- `sysroot` sincronizza con rsync `/usr/include`, `/usr/lib` (senza moduli del kernel né firmware),
  `/usr/share/cmake`, `/usr/share/pkgconfig`, `/usr/local` e `/lib` dal Pi in `~/rpi-sysroot/<arch>` e rende relativi
  i link simbolici assoluti. `--pi-packages` installa prima dei pacchetti sul Pi; `libsoapysdr-dev` è obbligatorio.
  `--ask-pass` permette allo script di usare `sudo` sul Pi per leggere i file accessibili solo a root (l'helper che
  contiene la password viene creato su un tmpfs e rimosso dopo).
- Pi 1 / Zero / CM1 sono ARMv6: i compilatori armhf di Fedora e di Debian/Ubuntu generano codice ARMv7. Usare una toolchain compatibile
  con ARMv6 (per esempio una toolchain glibc `armv6-eabihf` di Bootlin) con `--toolchain-dir <dir> --prefix arm-linux-`.
- I compilatori incrociati `gcc-<arch>-linux-gnu` di Fedora sono pensati per compilare kernel e non includono
  `libgcc_s` per la destinazione: lo script lo rileva e collega la `libgcc_s.so.1` del Pi dal sysroot (lo mostra
  `info`); altrimenti i programmi C++ falliscono con `_Unwind_Resume` non definito.
- La distribuzione dell'host viene letta da `/etc/os-release` (`ID` / `ID_LIKE`): Fedora, RHEL e derivate usano `dnf`;
  Debian, Ubuntu, Linux Mint, Pop!_OS e altre derivate usano `apt` (`g++-aarch64-linux-gnu` o
  `g++-arm-linux-gnueabihf`). `--distro fedora|debian` (o `RPI_DISTRO`) forza la scelta; `info` mostra ciò che è stato
  rilevato. Su Fedora il sysroot è obbligatorio (il suo cross-gcc non include la libc di destinazione); su
  Debian/Ubuntu viene comunque usato perché la compilazione veda le intestazioni e le librerie esatte del Pi.
- `install` compila il progetto (si salta con `--no-build`), controlla il risultato con `verify` e lo installa sul Pi
  in funzione tramite un'unica connessione SSH: `librpitxSupport.so` va nella directory dei moduli di SoapySDR (il
  primo `Search path` riportato da `SoapySDRUtil --info` sul Pi, per esempio
  `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8`; si cambia con `--module-dir`), mentre `rpitx_reset` e gli
  strumenti Python vanno in `/usr/local/bin` (`--pi-prefix`, `--no-tools`). Si ferma se lo userland del Pi o l'ABI di
  SoapySDR non corrispondono alla compilazione (`--force` li ignora), installa `soapysdr-tools` sul Pi se manca
  `SoapySDRUtil` e vi installa i `--pi-packages`. `sudo` sul Pi funziona come in `sysroot` (`--ask-pass`,
  `--pi-password`, sudo senza password o accesso come root) e non serve riavviare.
- `cmake-toolchain -o file.cmake` scrive solo il file di toolchain, da usare in un IDE o in un'altra compilazione.
- `verify` controlla l'architettura, l'ABI in virgola mobile e che le versioni dei simboli glibc / libstdc++ richieste
  esistano nel sysroot.

In compilazione incrociata `LIB_SUFFIX` viene forzato a vuoto: SoapySDR 0.7.x ricaverebbe `lib64` dall'host di
compilazione e non troverebbe la libreria nella `usr/lib/<multiarch>` del sysroot.

### Manualmente (qualsiasi host Linux)

1. Installare un compilatore incrociato corrispondente allo userland del Pi, per esempio su Debian/Ubuntu
   `sudo apt install g++-aarch64-linux-gnu` (arm64) o `g++-arm-linux-gnueabihf` (armhf, ARMv7 o superiore).
2. Copiare un sysroot dal Pi (dopo `sudo apt install libsoapysdr-dev` sul Pi):

   ```sh
   mkdir -p ~/rpi-sysroot
   rsync -a --exclude=/modules/ --exclude=/firmware/ pi@raspberrypi.local:/usr/lib     ~/rpi-sysroot/usr/
   rsync -a pi@raspberrypi.local:/usr/include ~/rpi-sysroot/usr/
   rsync -a pi@raspberrypi.local:/usr/share/cmake ~/rpi-sysroot/usr/share/
   ln -sfn usr/lib ~/rpi-sysroot/lib
   ```

   I link simbolici assoluti dentro la `usr/lib` copiata (per esempio `libm.so`) vanno resi relativi, oppure occorre
   indicare al linker i file corretti con `-Wl,-rpath-link`.
3. Scrivere un file di toolchain:

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

4. Compilare:

   ```sh
   cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=rpi-arm64.cmake -DCMAKE_BUILD_TYPE=Release
   cmake --build build-arm64 -j
   ```

### Distribuire una compilazione incrociata

`rpi-cross.sh install` fa tutto questo in automatico. Per farlo a mano, copiare i risultati sul Pi:

```sh
scp build-rpi-pi4-64/librpitxSupport.so build-rpi-pi4-64/rpitx_reset pi@raspberrypi.local:
ssh pi@raspberrypi.local
SoapySDRUtil --info | grep -A3 "Search path"       # mostra la directory dei moduli
sudo cp librpitxSupport.so /usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/   # usare il percorso mostrato sopra
sudo cp rpitx_reset /usr/local/bin/
```

---

## Installazione e verifica

`cmake --install` installa:

| File | Destinazione |
|------|--------------|
| `librpitxSupport.so` | `<libdir>/SoapySDR/modules<ABI>/` (la directory dei moduli di SoapySDR, es. `/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/`) |
| `rpitx_reset` | `<prefix>/bin` |
| `rpitx_tone_test.py`, `hpsdr_rpitx_bridge.py`, `rpitx_amp_calibrate.py` | `<prefix>/bin` |

Non servono riavvii né copie manuali. Verificare:

```sh
SoapySDRUtil --info                         # "rpitx" in "Available factories"
sudo SoapySDRUtil --find="driver=rpitx"     # modello e numero di serie della scheda
sudo SoapySDRUtil --probe="driver=rpitx"    # canali, intervalli, impostazioni, sensori
sudo rpitx_tone_test.py --freq 144.800M     # tono NBFM di prova di 30 s (carico fittizio!)
```

---

## Utilizzo

Stringa del dispositivo: **`driver=rpitx`**. Le applicazioni devono essere eseguite come root.

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
n = np.arange(fs * 5)                                      # tono NBFM di 5 s
iq = np.exp(1j * (dev / tone) * np.sin(2 * np.pi * tone * n / fs)).astype(np.complex64)
for i in range(0, len(iq), 4096):
    sr = sdr.writeStream(st, [iq[i:i + 4096]], len(iq[i:i + 4096]), timeoutUs=1000000)

sdr.writeStream(st, [np.zeros(0, np.complex64)], 0, SOAPY_SDR_END_BURST)
sdr.deactivateStream(st)                                    # attende l'invio del burst
sdr.closeStream(st)
```

### GNU Radio e SDRangel

- **GNU Radio:** *Soapy Custom Sink* (o *Soapy Sink*), dispositivo `driver=rpitx`, tipo `fc32`, frequenza di
  campionamento 48000, la frequenza centrale e argomenti di stream opzionali come `preroll_ms=150,fifo_ms=300`.
  Eseguire `gnuradio-companion` o lo script generato come root.
- **SDRangel:** aggiungere un dispositivo *SoapySDR output* e selezionare il dispositivo rpitx; le impostazioni
  compaiono nel pannello del dispositivo.

### Semantica dello stream

- `writeStream()` si blocca finché la FIFO è piena, rispetta `timeoutUs`, può accettare meno campioni di quelli offerti
  (il valore restituito è il numero consumato) e restituisce `SOAPY_SDR_TIMEOUT` quando non è riuscito ad accodare nulla
  in tempo. `SOAPY_SDR_HAS_TIME` non è supportato (`SOAPY_SDR_NOT_SUPPORTED`). L'MTU è `min(4096, max(FIFO/4, 256))`.
- `SOAPY_SDR_END_BURST` sull'ultima scrittura (o su una scrittura di lunghezza zero) segna la fine del burst: il DMA
  parte subito anche sotto il pre-roll, e dopo l'ultimo campione la portante resta spenta.
- `deactivateStream()` spegne la portante. Se l'ultima scrittura portava `END_BURST`, prima attende (al massimo la durata
  della FIFO più l'anello più 200 ms) che tutti i campioni in coda siano stati riprodotti; nel frattempo non trattiene il
  mutex del dispositivo, quindi l'API di controllo resta utilizzabile. Senza `END_BURST` i campioni in coda vengono
  scartati.
- `closeStream()` (e la distruzione del dispositivo) spegne subito la portante e scarta i campioni in coda, anche mentre
  un altro thread attende in `deactivateStream()`; l'hardware è subito libero per un nuovo stream.
- `readStreamStatus()` restituisce `SOAPY_SDR_UNDERFLOW` (maschera di canale 1) una volta per ogni nuovo svuotamento
  della FIFO durante un burst, overrun del DMA o riavvio del DMA; altrimenti `SOAPY_SDR_TIMEOUT`.
- Frequenza, frequenza di campionamento, ppm e armonica possono cambiare durante la trasmissione: l'alimentatore ferma
  il DMA, spegne la portante, riprogramma il PLL e il clock di cadenza e riparte con i campioni che erano già
  nell'anello DMA (nessuno perso; al massimo uno inviato due volte), seguiti dalla FIFO.

### Argomenti dello stream

| Chiave | Predefinito | Intervallo | Descrizione |
|--------|-------------|------------|-------------|
| `ring_size` | 85 ms di posizioni, ≥ 4096, arrotondato a 512 | 512 – 32768, e ≥ 10 ms alla frequenza delle posizioni | Dimensione dell'anello DMA in posizioni (una per campione, o `slots_per_sample` per campione) |
| `fifo_ms` | 250 | 20 – 5000 | Capacità della FIFO software in ms (segue i successivi cambi di frequenza di campionamento) |
| `preroll_ms` | 100 | 0 – 5000 | Segnale in coda prima dell'avvio del DMA |
| `dma_channel` | -1 | -1 – 14 | Canale DMA; -1 = primo canale libero non appartenente al kernel |
| `dma_force` | false | bool | Reimposta un canale scelto esplicitamente anche se è attivo |
| `rt_priority` | 50 | 0 – 99 | Priorità `SCHED_FIFO` del thread alimentatore (0 = scheduling normale) |
| `cpu` | -1 | -1 – 63 | CPU a cui è vincolato il thread alimentatore |
| `signal_cleanup` | false | bool | Spegne la portante quando il processo viene terminato da un segnale con l'azione predefinita |
| `slots_per_sample` | 1 | 1 – 4 | Posizioni DMA per campione I/Q; il segnale I/Q viene interpolato e l'ampiezza quantizzata alla frequenza delle posizioni (frequenza di campionamento × posizioni ≤ 200 kHz) |
| `envelope_output` | none | `none`, `pcm`, `pwm` | Uscita d'inviluppo per EER: `pcm` = I2S a 16 bit su GPIO18/19/21, `pwm` = duty PWM su `pwm_gpio` |
| `pwm_gpio` | 18 | 12 o 18 | Pin dell'uscita d'inviluppo PWM (GPIO18 ALT5, GPIO12 ALT0) |
| `pwm_bits` | 8 | 6 – 12 | Risoluzione dell'inviluppo PWM (periodo di 2^bit clock PWM) |
| `rf_dac` | false | bool | GPIO20 come seconda uscita di clock, sommata a GPIO4 da una rete pesata esterna |

Latenza da `writeStream()` all'antenna ≈ riempimento della FIFO + ¾ dell'anello (≈ 64 ms con un anello di 4096
posizioni a 48 kHz). Un'applicazione che scrive più veloce del tempo reale mantiene la FIFO piena: latenza ≈
`fifo_ms` + ¾ dell'anello.

### Impostazioni (accettate anche come argomenti del dispositivo)

| Chiave | Predefinito | Descrizione |
|--------|-------------|-------------|
| `ppm` | 0 | Correzione manuale del quarzo, −200 … 200 ppm. **Positiva quando il quarzo è lento**, cioè quando la scheda trasmette sotto la frequenza richiesta (stesso segno della stima NTP del kernel) |
| `ppm_ntp` | true | Usa la stima di frequenza NTP del kernel come correzione di base (sommata a `ppm`) |
| `harmonic` | 1 | Trasmette su questa armonica dell'onda quadra: il PLL lavora a frequenza / armonica (1 – 15) |
| `amplitude_mode` | envelope | `envelope` o `constant` |
| `iq_scale` | 1.0 | Guadagno applicato a \|I/Q\| prima della quantizzazione dell'ampiezza (modo envelope), 0 – 100 |
| `debug_level` | 0 | Diagnostica di librpitx su stderr: 0 errori, 1 configurazione, 2 dump dei registri |
| `amplitude_lut` | vuoto | Ampiezza RF relativa misurata dei livelli di corrente 0–7 (8 valori crescenti separati da virgole); vuoto = `(livello + 1) / 8` |
| `ampm_lut` | vuoto | Fase misurata dei livelli 0–7 in gradi (8 valori); compensata sulla portante |
| `amplitude_quantizer` | nearest | `nearest` o `sigma_delta` (selezione dell'ampiezza con conformazione del rumore) |
| `sigma_delta_order` | 2 | Ordine della retroazione dell'errore, 1 o 2 |
| `envelope_delay` | 0.5 | Ritardo dell'inviluppo rispetto alla fase in campioni, −32 … 32 (0,5 allinea i due percorsi del solo pin) |
| `envelope_floor` | 0 | Inviluppo sotto il quale la portante si spegne dopo 2 ms (0 = mai) |
| `rfdac_lut` | vuoto | Ampiezza relativa con solo GPIO4, solo GPIO20 ed entrambi i pin di clock (3 valori); vuoto = 2/3, 1/3, 1 |
| `eer_lut` | vuoto | Ampiezza RF misurata a codici d'inviluppo equispaziati da 0 al fondo scala (2–64 valori crescenti); vuoto = lineare |
| `eer_ampm_lut` | vuoto | Fase misurata agli stessi codici, in gradi (2–64 valori) |
| `eer_gain`, `eer_offset` | 1, 0 | Guadagno e offset applicati al codice d'inviluppo |
| `eer_idle` | 0 | Codice d'inviluppo mentre non si trasmette (frazione del fondo scala) |
| `eer_drive` | 7 | Corrente dei pin di clock mentre si usa il DAC d'inviluppo |

Esempio: `driver=rpitx,amplitude_mode=constant,ppm=1.5`. Gli argomenti del dispositivo sconosciuti vengono segnalati con
un avviso. Gli interi sono decimali (uno zero iniziale non significa ottale) e i numeri non possono avere caratteri in
coda.

### Sensori

| Chiave | Tipo | Descrizione |
|--------|------|-------------|
| `pll_locked` | bool | Bit di aggancio di PLLC (`CM_LOCK.FLOCKC`) |
| `dma_running` | bool | L'alimentatore ha avviato il DMA e il bit `ACTIVE` del canale è attivo |
| `fifo_fill` | float % | Livello di riempimento della FIFO software |
| `underflows` | int | La FIFO si è svuotata durante un burst (applicazione troppo lenta) |
| `late` | int | Il motore DMA ha superato l'alimentatore (thread alimentatore in ritardo) |
| `dma_errors` | int | Il motore DMA si è fermato da solo ed è stato riavviato |
| `sample_rate` | float Hz | Frequenza di campionamento I/Q prodotta effettivamente dai divisori del clock di cadenza |
| `envelope_clip` | int | Posizioni il cui inviluppo (\|I/Q\| × `iq_scale`, o il codice EER) ha superato il fondo scala |
| `envelope_late` | int | Parole d'inviluppo che non è stato possibile scrivere in anticipo (anello quasi vuoto) |

---

## Strumenti

### `rpitx_tone_test.py`

Trasmettitore di prova: tono NBFM, oppure un file WAV modulato in FM, AM, USB, LSB o inviato come I/Q (WAV stereo,
I = sinistro, Q = destro). Mostra i sensori ogni secondo.

```sh
sudo rpitx_tone_test.py --freq 144.8M                         # tono NBFM a 1 kHz per 30 s
sudo rpitx_tone_test.py --freq 7.074M --wav voce.wav --mod usb
sudo rpitx_tone_test.py --freq 144.8M --jitter-ms 20 --args "preroll_ms=200,fifo_ms=500"
sudo rpitx_tone_test.py --freq 144e6 --seconds 0              # solo apre/chiude: ripristina una sessione caduta
```

### `rpitx_amp_calibrate.py`

Misura il percorso dell'ampiezza con un ricevitore SoapySDR (basta un RTL-SDR vicino a un carico fittizio) e stampa
le impostazioni da usare. Ogni stato viene alternato a un riferimento a fondo scala, così la deriva di fase tra le due
radio si annulla; il ricevitore è sintonizzato `--offset` Hz sotto la portante.

```sh
sudo rpitx_amp_calibrate.py pad   --freq 7.1M --rx-args driver=rtlsdr         # amplitude_lut, ampm_lut
sudo rpitx_amp_calibrate.py rfdac --freq 7.1M                                 # rfdac_lut (rf_dac=true)
sudo rpitx_amp_calibrate.py eer   --freq 7.1M --envelope-output pcm --points 17   # eer_lut, eer_ampm_lut
sudo rpitx_amp_calibrate.py delay --freq 7.1M --envelope-output pcm           # envelope_delay (IMD3 a due toni)
rpitx_amp_calibrate.py self-test                                              # verifica dell'analisi, senza radio
```

### `rpitx_reset`

Riporta l'hardware a riposo dopo la morte di un processo che usava il driver: ripristina il registro di sessione
(vedere [Ripristino dopo un crash](#ripristino-dopo-un-crash)); senza registro spegne un'uscita di clock rimasta su
GPIO4 e ferma GPCLK0. `--dma-channel N` reimposta inoltre il canale DMA N (rifiutato se appartiene al kernel). Adatto a
`ExecStopPost=` in un'unità systemd.

### `hpsdr_rpitx_bridge.py` — bridge HPSDR Protocol 1

Fa apparire il Pi in rete come una radio openHPSDR Protocol 1, così piHPSDR, Thetis, PowerSDR mRX, linHPSDR, Quisk o
SparkSDR lo rilevano e trasmettono tramite rpitx.

```sh
sudo hpsdr_rpitx_bridge.py                          # segue la frequenza TX dell'host, emulazione Hermes
sudo hpsdr_rpitx_bridge.py --freq 144.8M            # frequenza TX fissa
sudo hpsdr_rpitx_bridge.py --freq-offset 116M       # transverter: host 28,8 MHz -> 144,8 MHz
sudo hpsdr_rpitx_bridge.py --amplitude-mode constant --follow-drive
sudo hpsdr_rpitx_bridge.py --key-dot-gpio 17 --key-dash-gpio 27 --ptt-gpio 22
hpsdr_rpitx_bridge.py --dry-run --verbose           # solo protocollo, senza trasmettitore, senza root
```

| Area | Implementazione |
|------|-----------------|
| Rilevamento | UDP broadcast/diretto sulla porta 1024 e TCP; MAC, firmware, id della scheda, ricevitori; byte extra di Hermes-Lite 2 |
| Schede | `metis`, `hermes` (predefinita), `hermes2`/`griffin`, `angelia`, `orion`, `orion2`, `hermeslite` |
| Avvio / arresto | I/Q EP6 e wideband EP4, su UDP o TCP; un altro host prende il controllo con un avvio; un arresto da un altro host viene ignorato |
| C&C dell'host (EP2) | Tutti gli indirizzi decodificati (frequenza, ricevitori, duplex, NCO TX/RX, potenza, filtri, attenuatori, microfono, PureSignal, keyer CW, estensioni HL2); `--verbose` registra le modifiche |
| Stato (EP6) | PTT/punto/linea, ingressi IO inattivi, firmware, potenza diretta (`--fwd-counts`), tensione di alimentazione, temperatura HL2, livello della FIFO TX e conferme |
| I/Q di ricezione | Rumore sintetico o zeri a 48/96/192/384 kHz, microfono muto; cadenzato perché lo stream TX dell'host vada a 48 kHz |
| Trasmissione | I/Q TX dell'host finché MOX è attivo, sull'NCO TX (o RX1 quando il duplex è disattivato) |
| CW | Keyer iambico A/B o tasto verticale con palette su GPIO e temporizzazione dell'host, CWX di HL2, ingresso PTT su GPIO |
| Watchdog | Lo streaming si ferma e il TX viene rilasciato dopo `--host-timeout` s senza pacchetti dall'host |

Note: in SSB con il microfono muto non c'è portante (in modo `envelope` le ampiezze sotto 0,5 / (PAD + 1) del fondo
scala spengono la portante; il bridge mostra un suggerimento). Con l'opzione *CW handled in Radio* di piHPSDR l'host non
attiva MOX in CW: collegare le palette agli ingressi GPIO o disattivare quell'opzione. piHPSDR limita le radio
Protocol 1 a 61,44 MHz; usare `--freq-offset` o un LO di transverter per il VHF.

---

## Ampiezza precisa ed EER

Il solo pin modula l'ampiezza tramite la corrente d'uscita del pad: 8 livelli più "spento", all'incirca un
quantizzatore a 3 bit i cui gradini non sono nemmeno lineari (l'impostazione di corrente cambia la resistenza
d'uscita e i livelli alti saturano). Il driver offre tre modi per migliorare, combinabili tra loro.

### 1. Tabelle calibrate e conformazione del rumore (senza hardware aggiuntivo)

- `amplitude_lut` sostituisce il modello lineare `(livello + 1) / 8` con l'ampiezza misurata di ogni livello, così il
  livello più vicino lo è davvero; `ampm_lut` compensa sulla portante lo sfasamento di ogni livello. Misurarli entrambi
  con `rpitx_amp_calibrate.py pad` sulla banda in uso.
- `amplitude_quantizer=sigma_delta` retroaziona l'errore di quantizzazione (ordine 1 o 2): l'ampiezza media segue
  esattamente l'inviluppo e l'errore diventa rumore spostato lontano dalla portante. Il miglioramento in banda cresce
  con il rapporto di sovracampionamento (frequenza delle posizioni / banda del segnale); `slots_per_sample` aumenta
  ancora la frequenza delle posizioni (il segnale I/Q viene interpolato). **Il rumore conformato cade fino a ±
  frequenza delle posizioni / 2 dalla portante, nei canali adiacenti**, dove un filtro di banda non lo elimina: usare
  una frequenza delle posizioni alta e un filtro d'uscita stretto, e controllare lo spettro.
- `envelope_delay` allinea l'inviluppo con la fase. Il valore predefinito di 0,5 campioni fa coincidere l'inviluppo
  mantenuto di una posizione con la rampa di fase che il PLL produce durante essa; un test a due toni
  (`rpitx_amp_calibrate.py delay`) trova il valore migliore per una data installazione.
- `envelope_floor` spegne la portante quando l'inviluppo resta sotto un livello per 2 ms (silenzio tra le parole),
  invece di trasmettere il rumore di quantizzazione.

### 2. RF-DAC: un secondo pin di clock (due resistenze e un trasformatore)

GPCLK0 è disponibile anche su **GPIO20** (ALT5). Con `rf_dac=true` il DMA commuta GPIO4 e GPIO20 in modo indipendente;
sommati tramite resistenze pesate (p. es. GPIO4 attraverso R e GPIO20 attraverso 2R verso un trasformatore a larga
banda) i due pin e gli 8 livelli di corrente danno circa tre volte più gradini di ampiezza. Misurare i pesi reali con
`rpitx_amp_calibrate.py rfdac` (`rfdac_lut`). Le funzioni di GPIO 20–29 restano congelate durante la trasmissione.

### 3. Eliminazione e Ripristino dell'Inviluppo (modulatore d'inviluppo esterno)

Con `envelope_output=pcm` o `pwm`, GPIO4 porta soltanto la **fase**: funziona con la corrente fissa `eer_drive` per
ogni campione di un burst e pilota un amplificatore a commutazione (classe D/E, p. es. BS170 in parallelo o uno stadio
IRF510 dopo un buffer 74AC). L'**inviluppo** viene scritto dalla stessa posizione DMA nella FIFO di cadenza ed esce da
un DAC che pilota il modulatore d'alimentazione di quell'amplificatore (un amplificatore operazionale di potenza per
QRP, o un modulatore buck/ibrido):

```
I/Q ─► percorso di fase: frac di PLLC + GPIO4 acceso, corrente fissa ──► GPIO4 ─► buffer ─► PA classe D/E ─► filtro ─► antenna
     └► percorso d'inviluppo: eer_lut⁻¹ → codice a 16 bit (stessa posizione DMA) ─► DAC ─► modulatore d'alimentazione ─┘ V_DD(t)
```

- **`pcm`**: master I2S stereo a 16 bit, 64 clock di bit per trama, entrambi i canali portano l'inviluppo. Collegare
  un DAC I2S **accoppiato in continua** (p. es. PCM5102A, SCK a massa): GPIO18 → BCK, GPIO19 → LRCK, GPIO21 → DIN. I
  codec accoppiati in alternata non possono fornire un inviluppo. Codice 0 … 32767 = inviluppo 0 … fondo scala (la
  metà positiva dell'uscita).
- **`pwm`**: canale PWM 1 in modo mark/space su GPIO18 (ALT5) o GPIO12 (ALT0), periodo di 2^`pwm_bits` clock PWM,
  un periodo per posizione; farlo seguire da un passa-basso LC/RC e da un buffer, o pilotare l'interruttore di un
  modulatore buck. Il blocco PWM dà allora anche la cadenza al DMA (il blocco PCM non viene usato). Richiede
  `dtparam=audio=off`.

Entrambi i percorsi provengono da una sola posizione DMA, quindi sono sincroni per costruzione. La parola del DAC esce
dalla FIFO di cadenza un numero fisso di posizioni dopo che i registri della sua posizione hanno effetto (livello
DREQ + 1: 17 posizioni con PCM, 9 con PWM); il driver scrive ogni parola d'inviluppo altrettante posizioni prima, e
`envelope_delay` compensa ciò che resta (ritardo del filtro del DAC, banda del modulatore). Calibrare il modulatore e
l'amplificatore con `rpitx_amp_calibrate.py eer` (`eer_lut`, `eer_ampm_lut`) e il ritardo con il test a due toni.
Sotto `envelope_floor` si spegne anche il pin di clock, il che elimina il passaggio del pilotaggio attraverso
l'amplificatore durante i silenzi.

La banda dell'inviluppo dell'SSB è diverse volte la banda audio: 48 kS/s bastano per SSB a 3 kHz, 96 kS/s danno
margine. Il modulatore d'alimentazione deve avere almeno quella banda.

Queste temporizzazioni e sequenze di registri sono verificate con il motore DMA simulato dei test; confermarle sulla
scheda di destinazione (oscilloscopio su GPIO4 e sull'uscita del DAC, spettro con un SDR) prima di trasmettere in aria.

---

## Funzionamento interno

### Panoramica

```
 thread dell'applicazione        thread alimentatore (SCHED_FIFO 50, periodo 1 ms)  motore DMA (hardware)
 ────────────────────────        ─────────────────────────────────────────────────  ─────────────────────
 writeStream()                    legge la posizione del DMA (CONBLK_AD)             per posizione dell'anello:
   │ CS16/CS8 → CF32               │ tracciamento della posizione assoluta (giri,     CB0  PAD      ← corrente
   ▼                               │ overrun)                                         CB1  PLLC_FRAC← frequenza
 SpscFifo<complex<float>> ───────► │ cancella con riposo le posizioni riprodotte      CB2  GPFSEL0  ← portante on/off
 (lock-free, contropressione)      │ IQ → (ampiezza, Δφ) → parole di registro ────►   CB3  FIFO PCM ← cadenza da DREQ
                                   │ mantiene l'anello a ¾, riempimento a riposo       └─ next → anello chiuso
                                   │ retune / riavvio del DMA / underflow
```

Ogni campione I/Q diventa una **posizione dell'anello** (o `slots_per_sample` posizioni del segnale interpolato): tre
parole a 32 bit nella memoria VideoCore (moltiplicatore frazionario del PLL, parola di corrente dei pad, parola di
`GPFSEL0`, più una parola di `GPFSEL2` con `rf_dac` e una parola d'inviluppo con un DAC d'inviluppo) e quattro o cinque
blocchi di controllo DMA che le copiano nelle periferiche. L'ultimo blocco di controllo scrive una parola nella FIFO
di trasmissione del PCM (la FIFO PWM con `envelope_output=pwm`) ed è cadenzato dalla sua linea DREQ, così il motore
avanza esattamente di una posizione per periodo; la parola è un valore fittizio, o il codice d'inviluppo con un DAC
d'inviluppo.
L'ultimo blocco di controllo punta di nuovo al primo: il DMA riproduce l'anello all'infinito senza intervento della
CPU, e il thread alimentatore deve soltanto restargli davanti.

### Sintesi di frequenza: modulazione di PLLC

La portante è GPCLK0 pilotato da **PLLC**. Alla creazione di uno stream il driver attiva la "modalità PLL avanzata" di
librpitx:

1. Vengono salvati i registri dell'albero dei clock che stanno per cambiare (`CORECLK_CNTL/DIV`, `EMMCCLK_CNTL/DIV`,
   `CM_GP0CTL/DIV`, `CM_PLLC`, `PLLC_CTRL/FRAC`, `PLLC_CORE0/1/2`, `PLLC_PER`, `A2W_PLLC_ANA0..3`) e annotati nel file
   di sessione.
2. Il clock del core VPU viene spostato su PLLA (divisore 4) e il clock EMMC su PLLD, perché nient'altro dipenda da
   PLLC.
3. `PLLC_CORE0` viene disattivato, il canale periferiche di PLLC viene abilitato con divisore 1 (2 su BCM2711) e GPCLK0
   vi prende la sorgente, con MASH 0 (divisore intero, senza spurie da noise shaping).

Per una portante F (frequenza del PLL F/armonica), `ComputeBestLO()` sceglie il divisore intero D di GPCLK0: scorre D da
4095 a 2 cercando una frequenza del VCO F·D tra 200 MHz e 1,5 GHz tale che l'intera banda F ± B (B = 0,58 × frequenza delle
posizioni, il massimo scostamento istantaneo comprese le correzioni di fase) corrisponda allo stesso moltiplicatore **intero** del PLL. Sopra una portante di 750 MHz (375 MHz su
BCM2711) si usa invece il pre-divisore del PLL (divisore di GPCLK 2). Il moltiplicatore del VCO è

```
M = (F + f) · K / (XOSC · (1 − ppm·10⁻⁶))          f = scostamento istantaneo dalla portante
K = D (BCM283x),  2·D (BCM2711, PLLC_PER = /2),  ½ o 2 con il pre-divisore
XOSC = 19,2 MHz (BCM283x) o 54 MHz (BCM2711)
```

`M` viene diviso nella parte intera `PLLC_CTRL.NDIV` (programmata una volta per sintonia) e nella parte frazionaria a
20 bit `PLLC_FRAC`, l'unico registro che il DMA riscrive. La risoluzione in frequenza è `XOSC / 2²⁰ / K`: per esempio
1,83 Hz a 144 MHz su un Pi 3 (D = 10) e 5,15 Hz su un Pi 4 (D = 5, K = 10). Se uno scostamento richiedesse un altro
moltiplicatore intero, il valore frazionario si satura a 0 o 0xFFFFF invece di ricominciare da capo; quando il
divisore scelto non riesce a tenere l'intera banda in un solo gradino intero, viene registrato un avviso. Dopo la
programmazione si attende fino a 2 ms l'aggancio di PLLC (`CM_LOCK.FLOCKC`) e si registra un avviso se non aggancia.

I generatori di clock (GPCLK0, PCM, EMMC) vengono sempre riprogrammati con la sequenza dell'hardware: azzerare `ENAB`,
attendere che `BUSY` si azzeri (kill dopo 1 ms), scrivere divisore e sorgente, abilitare.

### Da I/Q a parole di registro

Per ogni campione l'alimentatore calcola (`dsp::pushsample()`):

- l'**ampiezza** a = |s|;
- la **frequenza istantanea** f = wrap(arg(sₙ) − arg(sₙ₋₁)) · fs / 2π, con la differenza di fase ricondotta a
  [−π, π), così f ∈ [−fs/2, fs/2).

I campioni sotto una soglia di ampiezza di 1e-3 hanno una fase priva di significato: riportano f = 0 (portante alla
frequenza centrale) e si conserva l'ultima fase valida, così il campione valido successivo applica **tutto** il cambio
di fase attraverso l'intervallo. Per questo un'inversione BPSK che passa esattamente per zero ruota comunque la
portante di π.

Il PLL realizza una frequenza quantizzata. Il driver calcola la frequenza prodotta realmente dalla parola frazionaria
scelta (`GetMasterFracFrequency()`), e l'errore di fase risultante **viene riportato sul campione successivo**
(retroazione dell'errore del primo ordine, al massimo 0,5 rad applicati per posizione, il resto conservato per le
successive), così la fase della portante segue la fase I/Q senza deriva nelle trasmissioni PSK lunghe. Le correzioni
AM/PM (`ampm_lut`, `eer_ampm_lut`) si sommano alla stessa fase riportata.

L'ampiezza usata in una posizione è l'inviluppo ritardato di `envelope_delay` (interpolazione lineare delle magnitudini
recenti). Con un DAC d'inviluppo, la latenza della sua FIFO viene compensata scrivendo le parole d'inviluppo di un
campione in una posizione precedente; le posizioni degli ultimi campioni di un burst mantengono allora l'inviluppo di
riposo.

Le tre parole di una posizione di dati:

| Parola | Valore |
|--------|--------|
| `PLLC_FRAC` | `0x5A000000 \| frac(f / armonica + riporto)` |
| `PADS_GPIO_0` | `0x5A000000 \| HYST(originale) \| SLEW \| livello` — modo envelope: il livello dello stato di ampiezza scelto per `a·iq_scale` (più vicino o sigma-delta; senza tabelle `livello = round(min(a·iq_scale, 1)·(drive+1)) − 1`), valore originale dei pad se lo stato è "spento"; modo constant: `livello = drive`; EER: `eer_drive` |
| `GPFSEL0` | Funzioni di GPIO 0–9 campionate all'attivazione, con GPIO4 = ALT0 (portante accesa) o ingresso (portante spenta) |
| `GPFSEL2` (`rf_dac`) | Funzioni di GPIO 20–29 campionate all'attivazione, con GPIO20 = ALT5 o ingresso |
| inviluppo (`pcm` / `pwm`) | `codice << 16 \| codice` con `codice = round(32767·c)` (PCM), o `round(2^pwm_bits·c)` (PWM), `c` = `eer_lut⁻¹(a·iq_scale)·eer_gain + eer_offset` limitato a [0, 1] |

Una **posizione a riposo** contiene la parola frazionaria della frequenza centrale, il valore originale dei pad e
GPIO4 = ingresso: il GPCLK continua a funzionare internamente ma nulla esce dal pin.

### Cadenza di campionamento: il blocco PCM

Il controller PCM/I2S è configurato come trasmettitore a un canale e 8 bit la cui lunghezza di trama (`PCM_MODE.FLEN`)
è il valore minimo ≥ 10 che mantiene il divisore del clock PCM tra 2 e 4095. Il clock PCM proviene da PLLD con MASH 1 e
un divisore frazionario a 12 bit, corretto con gli stessi ppm della portante (con `envelope_output=pcm`: un master I2S
stereo a 16 bit con 64 clock di bit per trama e parole di FIFO impacchettate). Le soglie DREQ e di panico della FIFO
TX sono di 16 parole; ogni trama consuma una parola, quindi la scrittura di cadenza di ogni posizione si blocca
finché la FIFO contiene 16 parole — una posizione per trama. Con `envelope_output=pwm` il canale PWM 1 svolge lo
stesso ruolo (mark/space, FIFO dal DMA, livello DREQ 8, clock da PLLD a frequenza delle posizioni × 2^`pwm_bits`).

Ogni avvio del DMA (attivazione, retune, riavvio dopo un errore del DMA) inizia con posizioni a riposo: il motore le
percorre finché la FIFO di cadenza raggiunge il suo livello DREQ, così nessuna posizione di dati viene riprodotta più
velocemente della frequenza delle posizioni. La FIFO viene svuotata e il trasmettitore riavviato a ogni avvio, e
fermato quando il DMA si ferma, il che silenzia un DAC d'inviluppo. La frequenza prodotta (sensore `sample_rate`)
differisce da quella nominale per la quantizzazione del divisore, meno di 1 ppm.

### Motore DMA e memoria

- **Memoria:** un'unica allocazione contigua VideoCore (`mem_alloc` con `MEM_FLAG_DIRECT` su BCM2836 e successivi,
  l'alias non in cache `0xC`, o `MEM_FLAG_L1_NONALLOCATING` su BCM2835; `HINT_PERMALOCK`), bloccata per ottenerne
  l'indirizzo di bus e mappata nel processo tramite `/dev/mem`. Prima i blocchi di controllo (allineati a 32 byte),
  poi le parole delle posizioni: `anello × (4·32 + 3·4)` = 140 byte per posizione (fino a `5·32 + 5·4` = 180 byte con `rf_dac` e un DAC
  d'inviluppo). Ogni risposta della mailbox viene
  verificata (risultato dell'ioctl, codice di risposta, bit di risposta del tag).
- **Blocchi di controllo:** `TI = NO_WIDE_BURSTS | WAIT_RESP`, più `D_DREQ | PERMAP(2)` (PCM TX) o `PERMAP(5)` (PWM)
  per il blocco di cadenza; un trasferimento di 4 byte ciascuno, con destinazione l'indirizzo di bus della periferica
  (`0x7E102220` PLLC_FRAC, `0x7E10002C` PADS_GPIO_0, `0x7E200000` GPFSEL0, `0x7E200008` GPFSEL2, `0x7E203004` FIFO
  PCM, `0x7E20C018` FIFO PWM).
- **Canale:** `CS = PRIORITY(7) | PANIC_PRIORITY(7) | DISDEBUG | ACTIVE`. Si usano solo i canali classici (0–14; 0–10
  su BCM2711, dove 11–14 sono canali DMA4 con un altro formato di blocco di controllo). I canali del kernel
  (`dma-channel-mask` o `brcm,dma-channel-mask` di ogni nodo `soc/dma@…` e `soc/dma-controller@…`) non vengono mai
  toccati; il predefinito è il primo di 7, 8, 9, 10, 6, 5 (BCM2711) o 14 … 5 che non sia del kernel né attivo. Se non
  si può leggere alcuna maschera, il canale non viene indovinato: occorre indicare `dma_channel`. Un canale esplicito
  attivo viene reimpostato solo con `dma_force=true`.

### Il thread alimentatore

L'alimentatore gira ogni 1 ms (`clock_nanosleep` su una scadenza assoluta di `CLOCK_MONOTONIC`), con priorità
`SCHED_FIFO` `rt_priority` e affinità di CPU opzionale. Le posizioni sono tracciate come **contatori assoluti a
64 bit** (`DmaRingTracker`), perché il motore riporta la sua posizione solo modulo la dimensione dell'anello:

- `readPos` avanza della distanza modulare dalla lettura precedente; quando il tempo trascorso × frequenza supera
  mezzo anello, vengono aggiunti i giri completi che una distanza modulare non può mostrare
  (`giri = round((atteso − delta) / anello)`).
- `fill = writePos − readPos`; `fill ≤ 0` significa che il motore ha superato lo scrittore: un **overrun** (`late`).
  Allora tutto l'anello tranne la posizione in esecuzione viene sovrascritto con campioni a riposo e la scrittura
  riparte a una distanza di guardia (`max(32, frequenza/500)`, ≤ ¼ dell'anello) davanti al motore.
- A ogni ciclo **cancella** le posizioni completate non ancora riscritte con dati più recenti (le sovrascrive con il
  riposo), così se l'alimentatore si blocca il motore riproduce silenzio, mai il giro precedente.
- Sposta i campioni dalla FIFO all'anello fino a un **riempimento obiettivo di ¾ dell'anello**, mai a meno di
  2 posizioni da quella in esecuzione.
- Quando la FIFO è vuota e il riempimento scende sotto `low = max(anello/8, frequenza/200, anticipo dell'inviluppo + guardia + 16)`, riempie con posizioni a
  riposo; se era in corso un burst senza `END_BURST`, viene contato un **underflow**.
- Se la posizione del DMA non è valida o il canale non è più `ACTIVE` per 5 letture consecutive, il motore viene
  considerato fermo: l'evento viene registrato con `DMA_CS` / `DMA_DEBUG`, `dma_errors` viene incrementato e lo stream
  riparte con i campioni non riprodotti.

**Avvio e pre-roll:** dopo l'attivazione l'alimentatore riempie l'anello con il DMA fermo. Il DMA parte dalla
posizione 0 quando ci sono `preroll_ms` di segnale in coda, quando viene segnalata la fine del burst, quando la FIFO è
piena o quando il primo campione in coda attende da `preroll_ms` (così anche i burst brevi escono).

**Retune:** il thread di controllo pubblica la richiesta con un numero di sequenza e attende fino a 3 s che
l'alimentatore la prenda (e poi che la completi). L'alimentatore registra la posizione esatta del motore, ferma il
DMA, **porta GPIO4 in ingresso e ripristina i pad** prima di toccare il PLL, copia le posizioni non riprodotte
dell'anello (i campioni I/Q originali sono conservati in un array ombra) in un buffer di ripetizione, riprogramma PLLC
e il clock PCM, reimposta l'anello e riprende: il buffer di ripetizione viene inviato prima della FIFO. Una richiesta
che il PLL non può soddisfare lascia in vigore la configurazione precedente.

**Rilevamento dello svuotamento:** `WaitDrained()` confronta il contatore dei campioni inseriti nella FIFO con un
contatore dei campioni consumati che l'alimentatore pubblica solo *dopo* che i campioni sono nell'anello e la
posizione di fine dati è aggiornata, così un burst non viene mai dichiarato inviato mentre gli ultimi campioni sono in
transito tra la FIFO e l'anello.

### La FIFO

`SpscFifo` è una coda limitata lock-free a singolo produttore e singolo consumatore con contatori di testa e coda a
64 bit (acquire/release), così il livello di riempimento è sempre `head − tail`. La memoria viene allocata per
`fifo_ms` alla frequenza di campionamento massima; la capacità utilizzabile è `max(frequenza·fifo_ms/1000, 1024)` e
viene aggiornata a ogni retune. Un produttore bloccato attende su una variabile di condizione a intervalli di 2 ms e
viene risvegliato dall'alimentatore quando libera spazio.

### Chiusura, ripristino dopo i crash e sicurezza

- **Chiusura normale** (`deactivateStream` / `closeStream` / distruzione del dispositivo): si attende la fine
  dell'alimentatore, si reimposta il DMA, GPIO4 (e GPIO20) tornano alla funzione originale (ingresso se era l'uscita di
  clock), si ripristinano i pad, i pin del DAC d'inviluppo tornano alla funzione originale, si disattiva il blocco PCM e
  se ne ripristina il clock, si ripristina il blocco PWM, si ripristina l'albero dei clock (prima
  PLLC, attendendone l'aggancio, poi i suoi canali, i clock VPU ed EMMC e GPCLK0), si libera la memoria VideoCore e si
  svuota il file di lock.
- **`exit()`:** un gestore `atexit` esegue un arresto di emergenza (reset del DMA, portante spenta, GPCLK0 fermo) senza
  invalidare gli oggetti, così un distruttore successivo completa ancora la chiusura.
- **Segnali** (`signal_cleanup=true`): gestori per SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGFPE, SIGILL e
  SIGABRT. Quando la disposizione precedente era l'azione predefinita (il processo sta per morire) eseguono scritture
  di registri sicure nei gestori di segnale (reset del DMA, GPIO4 / GPIO20 in ingresso, pad, GPCLK0 spento, DAC d'inviluppo fermo), ripristinano
  l'azione precedente e rilanciano il segnale; i segnali ignorati e i gestori dell'applicazione passano invariati.

#### Ripristino dopo un crash

Finché esiste un trasmettitore, `/run/soapy_rpitx/soapy_rpitx.lock` (tmpfs, non sopravvive mai a un riavvio;
directory privata) è tenuto con un `flock()` esclusivo e contiene un registro `chiave=valore` di tutto ciò che il
processo possiede: PID, canale DMA e handle della memoria VideoCore (scritti appena la memoria è allocata), valore
originale dei pad e funzioni di GPIO4 e degli altri pin usati, clock PCM originale o stato del PWM, e l'albero dei clock
(salvato e registrato prima della sua prima modifica). Il kernel rilascia il lock quando il processo muore, anche con `kill -9`. Il processo successivo che ottiene
il lock e trova un registro non vuoto sa che il proprietario è morto senza ripulire ed esegue il ripristino: reset del
canale DMA, GPIO4 e pad ripristinati, GPCLK0 fermo, PCM disattivato e il suo clock ripristinato, albero dei clock
ripristinato, PWM ripristinato e memoria GPU del processo morto liberata (gli handle VideoCore sono globali per il
firmware). Prima il registro viene verificato: un canale DMA che non sia un canale classico del SoC o che appartenga
al kernel, pin fuori da GPIO 0–27 o valori di clock non validi lo fanno **scartare** (segnalato, non si applica nulla).
`rpitx_reset` fa lo stesso su richiesta. Un processo creato con `fork()` senza `exec()` condivide il lock: finché quel
figlio è vivo, il processo successivo viene informato che il lock è tenuto da un figlio del processo terminato.

#### Controlli dei conflitti all'apertura

- un altro processo detiene il lock → "the transmitter is in use by process N";
- GPIO4 (e GPIO20, i pin PCM 18/19/21 o il pin PWM quando usati) reclamato da un qualsiasi utilizzatore sul chip GPIO
  del SoC (`pinctrl-bcm2…`, tramite le informazioni di linea del dispositivo a caratteri GPIO);
- `soc/i2s@7e203000` abilitato nel device tree (salvo con `envelope_output=pwm`); con `envelope_output=pwm`,
  `soc/pwm@7e20c000` o il nodo dell'audio analogico abilitati;
- la directory o il file di lock non privati dell'utente che esegue il driver;
- il canale DMA richiesto appartiene al kernel, oppure è attivo senza `dma_force`;
- un avviso quando GPCLK0 è già in funzione (un altro programma potrebbe usarlo).

### Igiene del modulo

Il modulo SoapySDR è collegato con visibilità nascosta e `--no-undefined`: i simboli della librpitx integrata non
vengono esportati, quindi non può entrare in conflitto con un'altra librpitx nello stesso processo. librpitx non
chiama mai `exit()`; ogni guasto hardware diventa una `std::runtime_error` che l'applicazione riceve da SoapySDR.

---

## Risoluzione dei problemi

| Sintomo | Causa e rimedio |
|---------|-----------------|
| `No devices found` | `SoapySDRUtil --info` deve elencare `rpitx` (directory dei moduli); eseguire come root; la famiglia Pi 5 non è supportata |
| "/dev/mem and the VideoCore mailbox are not accessible" | Eseguire come root (o dare al binario accesso a entrambi i dispositivi) |
| "the transmitter is in use by process N" | Un altro programma possiede il trasmettitore; chiuderlo |
| "DMA channel N is reserved for the Linux kernel" / "is active" | Scegliere un altro `dma_channel` o lasciare -1; `dma_force=true` solo per un canale notoriamente libero |
| "GPIO4 is claimed by …" | Rimuovere l'overlay che usa il pin (es. `dtoverlay=w1-gpio`) e riavviare |
| "the PCM/I2S controller is enabled" | Rimuovere l'overlay della scheda audio / `dtparam=i2s=on` e riavviare |
| "the PWM block is enabled" | `envelope_output=pwm`: impostare `dtparam=audio=off`, rimuovere gli overlay `pwm` e riavviare |
| "the kernel DMA channel mask cannot be read" | Indicare in `dma_channel` un canale fuori dalla maschera del kernel (`xxd /proc/device-tree/soc/dma*/*dma-channel-mask`) |
| "the lock directory … must be … mode 0700" | `sudo rm -rf /run/soapy_rpitx` (viene ricreata) |
| "in use by a child process of terminated process N" | Un figlio creato con fork ha conservato il lock; terminarlo |
| `envelope_clip` aumenta | L'inviluppo supera il fondo scala: abbassare `iq_scale` o il livello dell'applicazione |
| `envelope_late` aumenta | L'anello si è quasi svuotato durante un burst: aumentare `preroll_ms` / `fifo_ms` |
| "VideoCore memory allocation failed" | Ridurre `ring_size`, aumentare `gpu_mem` o eseguire `sudo rpitx_reset` per liberare la memoria lasciata da un crash |
| `underflows` cresce | L'applicazione è troppo lenta: aumentare `preroll_ms` e `fifo_ms`, scrivere blocchi più grandi |
| `late` cresce | L'alimentatore è stato ritardato: mantenere `rt_priority` > 0, vincolarlo (`cpu=3`), valutare `isolcpus=3` e il governor `performance` |
| `dma_errors` cresce | Qualcos'altro ha reimpostato o usato il canale DMA; scegliere un altro canale |
| "crosses a PLL integer step" | Le deviazioni oltre il gradino vengono tagliate: spostare leggermente la frequenza o ridurre la frequenza di campionamento |
| "PLLC did not report lock" | Frequenze molto alte possono non agganciare su tutti i chip; abbassare la frequenza o usare `harmonic` |
| Console seriale / I2C / SPI disturbate durante la trasmissione | Previsto: il clock della VPU è su PLLA mentre lo stream è aperto (`gpu_freq=250` lo rende prevedibile) |
| Portante bloccata dopo un crash | `sudo rpitx_reset` (o aprire qualsiasi stream): il registro di sessione viene ripristinato |
| Nessuna portante in SSB con il bridge HPSDR | Microfono muto; vedere le note del bridge sopra |

---

## Test

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

I test girano su qualsiasi host Linux; `tests/mock_hw.cpp` sostituisce la mailbox, `/dev/mem`, le interrogazioni al
device tree e il motore DMA (un thread che interpreta la catena di blocchi di controllo, scrive i registri simulati e
modella la FIFO di cadenza PCM / PWM con il suo livello DREQ, registrando a ogni trama i registri in vigore e la parola
che esce dalla FIFO).

| Test | Copre |
|------|-------|
| `rpitx_unit_tests` | Ordine della FIFO con concorrenza e limite di capacità; contabilità dell'anello contro un motore simulato (regime stabile, brevi blocchi, giri persi, doppio blocco, mancanza di dati dall'host); stima di frequenza del DSP, soglia e continuità di fase attraverso lo zero; modello di ampiezza, tabelle, medie del sigma-delta, ritardo dell'inviluppo e allineamento inviluppo/fase, corrispondenza del codice EER |
| `rpitx_devicetree_tests` | Base delle periferiche, limite dei canali DMA classici e maschera DMA del kernel con la nomenclatura vecchia e attuale del device tree |
| `rpitx_transmitter_tests` | Alimentatore, pre-roll, svuotamento, retune con portante spenta e ripetizione dell'anello, arresto di emergenza seguito da distruzione, riavvio del DMA, isteresi dei pad, capacità della FIFO rispetto alla frequenza, errori del costruttore (frequenza irraggiungibile, allocazione rifiutata, anello corto), selezione del canale DMA (e nessuna ipotesi senza la maschera del kernel), lock di sessione e ripristino dopo i crash (pin, PWM), directory di lock privata, collegamento simbolico e registri non plausibili, registrazione anticipata dell'allocazione, gestori di segnale, avvio cadenzato del DMA, ritardo dell'inviluppo, sigma-delta, correzione AM/PM, RF-DAC, posizioni per campione, uscita d'inviluppo PCM e PWM con allineamento inviluppo/fase, fine del burst prima di un burst breve |
| `rpitx_driver_tests` | Dispositivo SoapySDR: impostazioni e intervalli, analisi rigorosa dei numeri, impostazioni di ampiezza, correzione di frequenza, argomenti dello stream (compresa l'uscita d'inviluppo), scalatura CF32/CS16/CS8, svuotamento con `END_BURST`, `closeStream()` durante uno svuotamento, stato dello stream, sensori |

La CI (`.github/workflows/ci.yml`) esegue i test in una compilazione normale, con AddressSanitizer +
UndefinedBehaviorSanitizer e con ThreadSanitizer (`tests/tsan.supp` sopprime solo gli accessi ai registri simulati), e
controlla `clang-format`, `shellcheck` su `rpi-cross.sh` e `pyflakes` sugli strumenti Python.

---

## Struttura del progetto

```
src/
  SoapyRPITX.hpp                 classe del dispositivo SoapySDR e wrapper dello stream
  SoapyRPITX_Registration.cpp    rilevamento della scheda, enumerazione, registrazione in SoapySDR
  SoapyRPITX_Settings.cpp        identificazione, impostazioni, antenna, guadagno, frequenza, frequenza di campionamento
  SoapyRPITX_Streaming.cpp       configurazione dello stream, conversione dei formati, scrittura / stato / burst
  SoapySDR_Sensors.cpp           sensori
  SoapyRPITX_IQ_dmaSync.*        trasmettitore: anello DMA, thread alimentatore, modulazione, retune, chiusura
  SoapyRPITX_Buffers.hpp         SpscFifo e DmaRingTracker (indipendenti dall'hardware)
  SoapyRPITX_Amplitude.hpp       stati di ampiezza, sigma-delta, ritardo dell'inviluppo, corrispondenza EER (indipendente dall'hardware)
  SoapyRPITX_Session.*           file di lock, registro dei crash e ripristino, controllo dei conflitti
librpitx/src/                    librpitx integrata (dma, gpio/clock/PCM/PWM/pad, mailbox, dsp, device tree)
tools/
  rpitx_reset.cpp                strumento di ripristino
  rpitx_tone_test.py             trasmettitore di prova (tono / WAV)
  rpitx_amp_calibrate.py         calibrazione di ampiezza, AM/PM, RF-DAC, EER e ritardo dell'inviluppo
  hpsdr_rpitx_bridge.py          emulazione di radio openHPSDR Protocol 1
tests/                           test unitari, del device tree, del trasmettitore e del driver, simulazione hardware, soppressioni TSan
rpi-cross.sh                     helper per la compilazione incrociata (host Fedora / derivato di Debian)
```

---

## Licenza e crediti

GPL-3.0 — vedere [LICENSE](LICENSE).

- Copyright © 2023 Emiliano Augusto Gonzalez (hiperion), <https://github.com/hiperiondev/SoapySDR_rpitx>.
- Basato su [librpitx](https://github.com/F5OEO/librpitx), Copyright © 2018 Evariste Courjaud F5OEO; la copia integrata
  e le sue differenze rispetto all'originale sono descritte in [librpitx/README.md](librpitx/README.md).
- Struttura del dispositivo ispirata a [SoapyPlutoSDR](https://github.com/pothosware/SoapyPlutoSDR).
- Ricerca del divisore del PLL adattata da [PiFmRds](https://github.com/SaucySoliton/PiFmRds) (fork di SaucySoliton);
  helper degli angoli da [RadiantBee](https://github.com/f4gkr/RadiantBee) di F4GKR.
