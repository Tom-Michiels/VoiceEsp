# VoiceEsp — microfoontests

Twee losse opstellingen met elk een eigen sketch en helper-script:

| Bord | Microfoon | Sketch | Script |
|---|---|---|---|
| ESP32-C3 SuperMini | INMP441 (I2S) + Whisper/LLM via Groq + MicroPython | `EspMicTest/` | `./esp.sh` |
| XIAO nRF52840 Sense | MSM261D3526H1CPM (PDM, on-board) | `XiaoSenseTest/` | `./xiao.sh` |

Beide dumpen audio in hetzelfde base64-formaat, dus `tools/record_wav.py` werkt
voor allebei.

`arduino-cli` staat niet meer in de PATH; de scripts vallen daarom terug op de
kopie die in `/Applications/Arduino IDE.app` meegeleverd is (versie 1.2.0).

---

# ESP32-C3 SuperMini: spraakgestuurde 8x8 ledmatrix

Knop indrukken → rood pulserende middenpixels → inspreken wat de matrix moet
doen → knop loslaten → Whisper + LLM maken er MicroPython van dat meteen draait.
Oneindige animaties zijn prima; de volgende knopdruk breekt ze af en luistert
opnieuw.

## Bedrading

| Onderdeel | Pin | |
|---|---|---|
| INMP441 VDD | 3V3 | **niet** 5V, de INMP441 is 1,8–3,3 V |
| INMP441 GND | GND | |
| INMP441 L/R | GND | linkerkanaal (`I2S_STD_SLOT_LEFT`), niet zwevend laten |
| INMP441 SCK | GPIO10 | bit clock; autodetect probeert ook GPIO4 |
| INMP441 WS | GPIO5 | word select / LRCL |
| INMP441 SD | GPIO6 | data uit de mic |
| WS2812B DIN | GPIO3 | 8×8-matrix, 64 leds |
| WS2812B 5V/GND | 5V + GND | zie stroomwaarschuwing hieronder |
| drukknop | GPIO7 ↔ GND | interne pull-up, actief-laag |

De I2S-pinnen zijn vrij gekozen (de C3 heeft een GPIO-matrix). Vermijd GPIO2/8/9
(strapping, 8 zit ook op de blauwe LED), GPIO18/19 (native USB) en GPIO20/21
(UART0).

**Stroom:** 64 WS2812B's vol wit trekken ~3,8 A — dat levert USB niet. De
standaardhelderheid staat daarom op 40/255. Wil je fel, geef de matrix dan een
eigen 5 V-voeding en verbind alle GND's. Klassiekers die helpen: ~330–470 Ω in
serie met DIN en een elco van 470–1000 µF over de matrixvoeding. De datalijn is
3,3 V waar de matrix officieel ≥ 3,5 V wil; met een korte draad werkt dat
vrijwel altijd, anders is een 74AHCT125 de nette fix.

**Matrix-layout:** de sketch gaat uit van row-major (rij na rij, elke rij zelfde
richting). Loopt het diagonaaltje van het `p`-demoscript zichtbaar krom, zet dan
`MATRIX_SERPENTINE` op 1.

## Bouwen en draaien

```bash
./esp.sh build                  # compileren
./esp.sh upload                 # compileren + flashen
./esp.sh monitor                # live meetwaarden
./esp.sh record                 # 3 s opnemen naar recording-esp.wav
```

FQBN is `esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app` — **USB CDC
On Boot moet aan**, anders komt `Serial` niet over de USB-poort naar buiten, en
de 3 MB-app-partitie (geen OTA) is nodig sinds MicroPython meegelinkt wordt
(1,28 MB past niet meer in de standaard 1,3 MB-partitie samen met groei).
Core: `esp32:esp32` 3.3.0; `ESP_I2S` zit daarin, ArduinoJson 7.4.2 en de
gegenereerde `MicroPythonEmbed`-library staan in `~/Documents/Arduino/libraries`.

Commando's in de monitor:

| | |
|---|---|
| `h` | help |
| `i` | zelftest: chip, I2S-pinnen, sample-teller, stalls |
| `m` | monitorregels aan/uit |
| `r` | statistieken resetten |
| `+` / `-` | gain ±6 dB (shift 0–12, standaard 4 = +24 dB) |
| `s` | slot LEFT/RIGHT wisselen |
| `c` | leesblok wisselen (240/480/120 glitchvrij, 256/64 niet) |
| `f` | sample rate wisselen (8/16/32/48 kHz) |
| `d` | 8 ruwe 32-bits samples in hex |
| `w` | 3 s audio opnemen en als base64 dumpen |
| `t` | spraakflow met vaste 5 s opname (testen zonder knop) |
| `p` | vast MicroPython-demoscript draaien (diagonalen over de matrix) |
| `P` | guard-test: oneindige lus, moet na 3 s afbreken |
| `x` | lopend Python-script afbreken (de knop doet dat ook) |
| `W` | wifi opnieuw verbinden (ook vanuit AP-modus) |
| `S` | wifi-netwerken scannen |
| `D` | wifi verbreken (test van indicator + zelfherstel) |
| `M` | geheugentest: MemoryError + RecursionError opvangen |

Verder: `T` = LAN-streamtest (chunked zonder TLS naar een host op het LAN,
debughulp voor wifi-doorvoer) en `A` = opname-animatie aan/uit (A/B-debug).

De knop is het echte bedieningsorgaan: indrukken breekt een lopend script af en
start het luisteren, loslaten sluit de opname af. Op de matrix zie je de fase:
**rood pulserende middenpixels** = er wordt geluisterd (praat nu); **geel
stipje** = uploaden en wachten op het transcript; **rood kruis** = mislukt,
probeer opnieuw. Is er **geen wifi**, dan knippert er een **rood pixel in de
rechterbovenhoek**; die verdwijnt zodra de verbinding er is. Het bord zoekt
elke 30 s vanzelf opnieuw — ook als een bestaande verbinding wegvalt.

De opname gaat **naar flash** (LittleFS, max 45 s), als **µ-law** (zie
hieronder), en wordt pas na het loslaten geüpload met bekende `Content-Length`
— in het tempo dat het netwerk aankan, met automatisch een tweede poging bij
een fout. Er is dus geen real-time verbinding nodig tijdens het spreken: een
haperend netwerk kost alleen wachttijd, nooit audio. De RAM-ring (8 kB, 256 ms)
dempt alleen nog de schrijflatentie van de flash.

De meetregels van de microfoon staan standaard **uit**; `m` zet ze aan.

### µ-law-compressie

De audio wordt als **G.711 µ-law** weggeschreven: 2:1 compressie voor een
handvol integer-bewerkingen per sample, dus verwaarloosbaar op de C3. WAV
ondersteunt het native (format tag 7, 58-byte header met `fact`-chunk) en
Whisper transcribeert het even goed als PCM16 — vooraf getest met dezelfde
zin via beide formaten. Winst: halve upload, halve kans dat een wankele
verbinding het begeeft, en dubbel zoveel opnametijd in flash.

**Opstarten duurt ~1,5 s tot de knop reageert.** Het verbinden met wifi gebeurt
op een achtergrondtask, want dat kan tot 30 s duren (3 netwerken x 10 s) en de
knop mag daar niet op wachten. Dat kan omdat opnemen naar flash geen netwerk
nodig heeft: druk je de knop terwijl de verbinding nog opgezet wordt, dan neemt
het bord gewoon op en wacht het pas bij het *uploaden* tot de verbinding er is
(max 35 s, daarna een rood kruis).

**Wifi:** het bord **scant eerst** en probeert alleen netwerken die echt in de
lucht hangen — dat scheelt 10 s per afwezig netwerk (van boot tot verbonden nu
~4 s in plaats van ~25 s). Volgorde: eerst het via het webportaal ingestelde
netwerk (NVS), dan de lijst uit `secrets.h` op voorkeursvolgorde — zet je
reserve-hotspot dus achteraan.
Lukt niets, dan start het een eigen AP (`WIFI_AP_SSID`/`WIFI_AP_PASS` uit
`secrets.h`) met een
**configuratieportaal op http://192.168.4.1**: netwerken scannen, SSID +
wachtwoord opslaan (herstart daarna) en de Groq API-key aanpassen. Het portaal
draait ook gewoon op het STA-adres als het bord verbonden is.

### Wat de upload betrouwbaar maakte

De TLS-upload werkte thuis meteen, maar viel elders steeds na een paar kB stil.
Vijf dingen bleken nodig — de eerste drie uit de streaming-fase, de laatste twee
kwamen pas boven water na de overstap naar flash-buffering:

1. **`WiFi.setSleep(false)`** — modem-sleep staat in Arduino-ESP32 standaard
   aan en drukte de doorvoer tot 18 kB/s (gemeten met `T`), onder de 32 kB/s
   die live audio nodig heeft. Zonder sleep: exact realtime, 0 drops.
2. **`client.setNoDelay(true)` + records van ~700 bytes** — het gastnetwerk
   bleek volle 1500-byte-pakketten richting internet te verliezen
   (path-MTU-blackhole): de handshake lukte, elke bulkupload stierf. Kleine
   TLS-records die elk meteen als eigen TCP-segment vertrekken omzeilen dat.
3. **`setTimeout()` is in core 3.x in milliseconden** (2.x nam seconden) —
   `setTimeout(15)` betekende dus 15 ms.
4. **Ruime `setTimeout` bij de upload (20 s).** Diezelfde waarde is de
   `socket_timeout` van de TLS-schrijflus in `ssl_client.cpp`: blijft de
   verzendbuffer langer vol, dan geeft `send_ssl_data()` op en sluit
   `write()` de verbinding ("Closing connection on failed write"). Met 4 s
   stierf de upload rond 4 kB — precies de LWIP-verzendbuffer. Snel falen was
   zinvol tijdens streamen (wachten kostte audio); met flash-buffering is
   geduld juist gratis.
5. **Genoeg vrije heap.** De TLS-handshake wil ~50 kB; LittleFS erbij duwde
   dat over de rand → `BIGNUM - Memory allocation failed`. De RAM-ring van
   32 kB was sinds de flash-buffering overbodig groot en staat in `.bss`,
   dus die gaat rechtstreeks van de heap af. Terug naar 8 kB gaf 24 kB lucht:
   vrije heap van 76 → 101 kB, en de upload lukt sindsdien in één poging.

Daarnaast staat het zendvermogen op 15 dBm. Let bij problemen vooral op de
rssi in `i`: rond −60 dBm gaat alles vlot, bij −80 dBm wordt het moeizaam.

### Voorbeeld van de uitvoer

```
=== ESP32-C3 + INMP441 + 8x8 WS2812B - zelftest ===
  Chip        : ESP32-C3 rev 4, 160 MHz
  Vrij heap   : 101344 bytes
  I2S pinnen  : BCLK=GPIO10  WS=GPIO5  DIN=GPIO6
  Matrix      : 8x8 WS2812B op GPIO3, row-major, helderheid 40
  Knop        : GPIO7 (pull-up), nu los
  Microfoon   : I2S actief, 16000 Hz mono 32-bit, slot=LEFT, readLen=240, shift=4 (24 dB)
  Wifi        : MijnNetwerk, ip=192.168.1.176, rssi=-62 dBm
```

En een volledige ronde (`m` aan geeft daarnaast de MIC-meetregels):

```
>>> SPREEK NU <<<
groq: 79920 bytes mu-law (4995 ms) opgenomen naar flash
groq: upload poging 1... 1250 ms, wachten op antwoord...
groq: HTTP 200, 240 ms

GEHOORD: "Toon een groen vinkje."
llm : HTTP 200, 473 ms

----- MicroPython ------------------------------
matrix_fill(0, 0, 0)
matrix_icon('check', 0, 255, 0)
matrix_show()
sleep_ms(1000)
----- gestopt na 1005 ms --------------------
```

De MIC-monitorregel (met `m`) ziet er zo uit:

```
MIC rms=71 peak=214 dc=-4 dBFS=-53.3 [##                  ] raw24peak=4304 (-65.8 dBFS) n=4096
```

`raw24peak` is de piek in de ongeschaalde 24-bits waarde (volle schaal =
8388608), dus onafhankelijk van de gain — dat is het getal om naar te kijken als
je wilt weten wat de microfoon zélf levert. `n` hoort 4096 te zijn per 250 ms bij
16 kHz, `stalls` hoort 0 te blijven.

Gemeten in een stille kamer met de standaardgain (+24 dB): rms ≈ 50–60, oftewel
−55 dBFS na de gain en ≈ −79 dBFS in de ruwe 24-bits waarde. Praten vlak bij de
microfoon tilt dat ruim boven −40 dBFS.

## De spike-bug in de uitlezing (opgelost)

De eerste versie las blokken van 256 samples en dat gaf een **glitch die er als
geluid uitzag**: elke 3e leesbeurt was het laatste sample precies 256× de
buurwaarde, dus 8 bits naar links verschoven. In een opname van 3 s waren dat 58
losse pieken tot −0,6 dBFS, terwijl het echte signaal op −55 dBFS zat. De
`record`-samenvatting noemde dat "gezond" — die pieken waren dus meetfouten, geen
geluid.

Het patroon schaalde exact mee met het leesblok en niet met de sample rate:

| readLen | spikes per 3 s | periode |
|---|---|---|
| 64 | 230 | elke 192 samples (3 × 64) |
| 128 | 122 | elke 384 samples (3 × 128) |
| 256 | 58 | elke 768 samples (3 × 256) |
| **120** | **0** | — |
| **240** | **0** | — |
| **480** | **0** | — |

De ESP_I2S-standaardconfig gebruikt `dma_frame_num = 240`. Lees je een aantal
samples dat daar geen deler of veelvoud van is, dan raakt het laatste woord van
een leesbeurt één byte uit de pas. Met `readLen = 240` is het volledig weg. Je
kunt het zelf terugzien met `c`: schakel naar 256, neem op, en de pieken staan er
weer.

## Aandachtspunten

* **De INMP441 zet 24 bits in een 32-bits slot.** Dus I2S openen met
  `I2S_DATA_BIT_WIDTH_32BIT` en daarna `raw >> 8` voor de echte waarde. Op
  16 bits configureren levert onzin op.
* **Er zit een flinke DC-offset op.** De sketch haalt die eruit met een eenpolig
  hoogdoorlaatfilter (a = 1015/1024, kantelpunt ≈ 22 Hz bij 16 kHz).
* **`i2s.readBytes()` blokkeert** tot het gevraagde blok binnen is, met de
  `Stream`-timeout van 1 s. Levert het 0 op, dan is er iets mis en herstart de
  sketch het I2S-kanaal.
* **Bedradingscheck met `d`:** alleen nullen betekent geen data (SD-draad,
  voeding of L/R nakijken); een constante waarde betekent dat de klok wel loopt
  maar de mic niet meepraat. Krijg je stilte terwijl de bedrading klopt, probeer
  dan `s` voor het andere slot.
* Met de standaard gain (+24 dB) tikt een harde klap tegen de clipgrens aan. Zie
  je `clip=` in de monitorregel, dan een stap terug met `-`.

## Spraak naar tekst met Whisper op Groq

`t` neemt 5 s op en zet die om in tekst. De audio gaat **streaming** het
TLS-kanaal in: de lengte is vooraf bekend uit de duur, dus de `Content-Length`
klopt zonder dat de hele opname eerst in RAM moet. Dat scheelt 160 kB — met een
buffer erbij zou het samen met de TLS-stack krap worden op de 320 kB van de C3.

Wifi-SSID, wachtwoord en de API-key staan in `EspMicTest/secrets.h`. Dat bestand
zit niet in de repo (zie `.gitignore`); begin met
`cp EspMicTest/secrets.example.h EspMicTest/secrets.h` en vul je eigen gegevens
in. Model en taal staan er ook in:

```c
#define GROQ_MODEL    "whisper-large-v3-turbo"
#define GROQ_LANGUAGE "nl"     // leeg laten = Whisper raadt de taal
```

Voorbeeld van een volledige ronde:

```
groq: verbinden... 574 ms
>>> SPREEK NU (5.0 s) <<<
groq: 160000 bytes audio in 5013 ms, wachten op antwoord...
groq: HTTP 200, 361 ms

TEKST: "De microfoon werkt en dit is een test van Wisper op grob."
```

Getest door de Mac de zin *"De microfoon werkt en dit is een test van Whisper op
Groq"* te laten uitspreken op ~1 m afstand. Alles klopte op de twee eigennamen na
— die haalt Whisper er niet uit bij een synthetische stem op laag volume.

De opname loopt precies real-time (160000 bytes in 5013 ms), dus de upload houdt
het bij en er gaan geen samples verloren.

Twee dingen om te weten:

* **`client.setInsecure()`** — het certificaat van `api.groq.com` wordt niet
  gecontroleerd. Prima voor een test op je eigen netwerk, maar voor iets dat
  buiten de deur draait hoor je de root-CA in te bakken.
* **De API-key staat in platte tekst in `secrets.h`.** Zet dat bestand niet in
  een publieke repo; intrekken kan op https://console.groq.com/keys.

## Spraak → MicroPython: de matrix bedienen met je stem

De keten draait volledig op het bord zelf:

1. audio wordt als µ-law naar **flash** opgenomen zolang de knop ingedrukt is
   en daarna naar **Whisper** geüpload (met retry) — geen real-time vereiste;
2. transcriptie → **chat-LLM op Groq** (`llama-3.3-70b-versatile`, instelbaar
   in `secrets.h`) met een systeemprompt die de matrix-API beschrijft, kale
   MicroPython eist en oneindige animatielussen expliciet toestaat (mits
   `sleep_ms(≥20)` per iteratie);
3. de gegenereerde code → **MicroPython-interpreter in de firmware**, zonder
   tijdslimiet — de volgende knopdruk (of `x`) breekt hem af.

### De Python-API (de matrix is de enige uitvoer)

| functie | doet |
|---|---|
| `matrix_set(x, y, r, g, b)` | pixel in de tekenbuffer; x 0–7 van links, y 0–7 van boven; buiten bereik wordt stil genegeerd |
| `matrix_fill(r, g, b)` / `matrix_clear()` | hele buffer |
| `matrix_icon(naam, r, g, b)` | kant-en-klaar 8×8-symbool: heart, smiley, sad, arrow_up/down/left/right, check, cross, star, sun, square, circle, diamond; onbekende naam tekent een `?` |
| `matrix_char(teken, r, g, b)` | één 5×7-teken, gecentreerd (font uit Adafruit-GFX) |
| `matrix_text(tekst, r, g, b)` | scrollt de tekst één keer voorbij; showt zelf, blokkeert tot klaar; knop breekt af |
| `matrix_bitmap(rijen, r, g, b)` | eigen figuur: lijst van 8 ints, bit 7 = linkerkolom |
| `matrix_show()` | buffer naar de leds (verplicht om iets te zien; alleen `matrix_text` showt zelf) |
| `matrix_brightness(n)` | globale helderheid 0–255, standaard 40 |
| `sleep_ms(ms)` / `millis()` | tijd |
| `print(...)` | naar de USB-seriële poort |

De icon/char/text/bitmap-helpers bestaan zodat het LLM figuren en letters niet
zelf pixel voor pixel hoeft te ontwerpen (llama-3.3 maakte van "een hartje" een
kruisje van 8 pixels; sindsdien staat het model op `openai/gpt-oss-120b` én
zijn er helpers). Een mislukte upload of Whisper-fout toont een **rood kruis**
op de matrix: gewoon opnieuw proberen.

De losse-LED-API (`led_on` enz.) is vervallen; de blauwe onboard LED doet nog
wel dienst als opname-indicator, maar hoort niet meer bij de Python-API.

Meer functionaliteit later = functie toevoegen in `embed_api.c` + regel in de
systeemprompt (`LLM_SYSTEM` in de sketch).

### Architectuur: microfoon op een eigen task

De microfoon draait op een eigen FreeRTOS-task (prioriteit 3, boven de
loop-task) die continu de I2S-DMA leegleest en de monitorstatistieken bijwerkt.
Tijdens een opname schrijft hij door in een ringbuffer van 32 kB (~1 s) waar de
uploadlus uit leest. Alle I2S-aanroepen gebeuren op die task; commando's als
`s`/`f` zetten alleen een reconfig-vlag. Zo blijft de DMA ook leeg tijdens
TLS-werk en lopende Python-scripts.

De ring is bewust niet groter: de TLS-handshake heeft ~50 kB vrije heap nodig
(mbedtls-bignum) en met een 64 kB-ring bleef daar te weinig van over — dat gaf
letterlijk `BIGNUM - Memory allocation failed`. Vandaar ook de twee fasen bij
de knop: eerst verbinden, dan pas opnemen, zodat de ring de handshake nooit
hoeft te overbruggen.

### MicroPython in de firmware

De echte MicroPython (embed-port) is meegelinkt als Arduino-library
`MicroPythonEmbed`, gegenereerd met:

```bash
./tools/build_micropython_lib.sh
```

(kloont de repo shallow naar `~/.cache/micropython-src`, genereert de boom met
`tools/mpconfigport.h` en patcht twee dingen: `port/mphalport.c` eruit omdat de
sketch stdout zelf naar USB-CDC stuurt, en `__assert_func` eruit omdat newlib
die al heeft). Na een wijziging in `tools/mpconfigport.h` gewoon opnieuw draaien
— let op: features die qstr's toevoegen (zoals `MICROPY_PY_SYS_PLATFORM`)
vereisen zo'n regeneratie, de headers in de library zijn gegenereerd.

Kerncijfers: 48 kB GC-heap (statisch), loop-task-stack naar 20 kB
(`SET_LOOP_TASK_STACK_SIZE`), `MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES` + float,
geen filesystem/`open()`/externe imports. Elke run krijgt een verse interpreter,
dus scripts laten geen globals achter voor de volgende.

**Geheugen — kan een gek script het bord slopen?** Nee, getest met `M`. De
Python-heap is een **statische 64 kB** (`mp_heap` in `embed_api.c`, dus in
`.bss`); statisch is hier bewust, zo kan hij nooit concurreren met de
Arduino-heap die de TLS-handshake nodig heeft. Loopt een gegenereerd script
eruit, dan krijg je een gewone Python-exception en draait het bord door:

```
MemoryError netjes gevangen na 76 kB      (bij een testheap van 80 kB)
en de interpreter leeft nog
RecursionError netjes gevangen: maximum recursion depth exceeded
en de interpreter leeft nog
heap voor=68772 na=68772
```

De Arduino-heap is voor en na exact gelijk, want elke `embed_run()` doet
init → exec → deinit op diezelfde pool: er lekt niets tussen scripts door,
hoe vaak je ook een opdracht geeft. Diepe recursie wordt gevangen door
`MICROPY_STACK_CHECK` met een limiet van 12 kB, ruim onder de 20 kB
loop-task-stack, zodat je een `RecursionError` krijgt in plaats van een
gecrashte FreeRTOS-task.

### Grote programma's: vijf grenzen, één voor één geraakt

Bij *"maak een tetris-achtige animatie"* gebeurde er eerst helemaal niets. Wat
volgde was een reeks limieten die elk apart moesten worden opgelost:

1. **`max_tokens` stond op 1200** — een gok uit de tijd dat de LLM-stap nog voor
   de losse LED was. `finish_reason: length`, code **middenin een expressie
   afgekapt**, syntaxfout, zwart scherm — en géén melding. De firmware
   controleert nu `finish_reason` en weigert afgekapte code te draaien.
2. **`OverflowError: long int not supported in this build`** — `MICROPY_LONGINT_IMPL`
   stond uit, dus elk getal boven 2³⁰ knalde. Nu op `MPZ` (willekeurige
   precisie, zoals CPython). Vereist wel het regenereren van de library.
3. **`ImportError: no module named 'time'`** — het model schrijft `import time`
   en `from random import choice`, hoe vaak de prompt ook zegt dat imports
   verboden zijn. Oplossing: niet vechten maar leveren. Een **prelude** definieert
   `time` (sleep, sleep_ms, ticks_ms) en `random` (randint, randrange, choice,
   shuffle) plus de losse namen, en de firmware haalt `import`-regels weg. De
   prelude draait als aparte `exec_str` in dezelfde interpreter, zodat de
   regelnummers in tracebacks van de gebruikerscode blijven kloppen.
4. **De prompt maakte het model lui.** Na de afkap-bug had ik er *"hou de code
   compact, korte namen, geheugen is beperkt"* in gezet — met als resultaat
   blokjes die niet bleven liggen en alleen vierkantjes. Die instructie is
   vervangen door het omgekeerde: een volledige implementatie, blokken die
   stapelen, echte tetromino-vormen, visueel zo professioneel mogelijk, en
   animaties die blijven doorlopen (bij game over automatisch herstarten).
5. **`HTTP 413: Limit 8000, Requested 8885`** — en dit bleek de échte
   bovengrens. Niet het model (gpt-oss-120b kan 65536 tokens uit) en niet het
   RAM van de C3 (tijdens de LLM-stap is ~70 kB vrij; een respons van 6 kB kostte
   met het JSON-filter maar 1,6 kB), maar de **TPM-limiet van het Groq-account**:
   de free tier staat 8000 tokens per *minuut* toe, en prompt + `max_tokens`
   tellen daar samen in mee.

`max_tokens` staat daarom nu op **5000** — goed voor ~200 regels code, met marge
voor de systeemprompt. Wil je groter, dan is een hogere Groq-tier nodig, niet
ander hardware. Snel achter elkaar twee opdrachten geven kan de TPM-limiet nog
steeds raken; dat geeft nu een nette melding in plaats van een JSON-brok.

Terugkerende les: elke limiet die stil kan falen hoort een expliciete controle
te hebben. Dezelfde klasse fout als de `<think>`-blokken.

### Hoe groot mag die heap? (gemeten)

De Python-heap en de TLS-handshake delen hetzelfde RAM, dus dit is een
afweging — hier uitgemeten in plaats van gegokt:

| `mp_heap` | vrij na TLS | upload |
|---|---|---|
| 48 kB | ~70 kB | werkt |
| **64 kB** | **37,9 kB** | **werkt (3/3, stabiel)** |
| 80 kB | — | **faalt**: `SSL - Memory allocation failed` |

64 kB is de instelling: 76 kB uitdeelbaar was er niet meer bij, maar de
handshake hield consistent 37,9 kB over (37928 / 37920 / 37920 over drie
uploads). Bij 80 kB haalt de handshake het niet meer.

### Meer heap? ESP32-S3 met PSRAM

Op de C3 is 64 kB het plafond, want alles zit in dezelfde ~320 kB intern RAM.
Op een **ESP32-S3 met PSRAM** vervalt die afweging volledig: de Python-heap
hoeft dan niet in `.bss` maar kan met `ps_malloc()` in het externe RAM (2–16 MB),
waardoor het interne RAM helemaal vrij blijft voor wifi en TLS. De wijziging is
klein — één statische array vervangen door een runtime-allocatie (zie het
commentaar bij `mp_heap` in `embed_api.c`) — plus PSRAM aanzetten in de FQBN
(`PSRAM=enabled` voor QSPI, `PSRAM=opi` voor octal). `ps_malloc()` zit gewoon
in Arduino-ESP32 3.3.0.

Twee kanttekeningen: PSRAM is trager dan intern SRAM (extern, via cache), dus
een GC-sweep over een héél grote heap kost meer tijd — voor ledanimaties
irrelevant. En de S3 heeft sowieso meer intern RAM (512 kB) en twee cores, dus
de microfoon-task en het netwerk kunnen elk hun eigen core krijgen.

**Afbreken:** scripts draaien zonder tijdslimiet — oneindige animaties zijn
juist de bedoeling. Een VM-hook (`MICROPY_VM_HOOK_*` in `mpconfigport.h`) pollt
tijdens het draaien en zet een `KeyboardInterrupt` klaar zodra de knop
ingedrukt wordt of er een `x` op de seriële poort staat; `sleep_ms()`
controleert hetzelfde. Getest met `P` (bewuste `while True:` met een
testbudget van 3 s): breekt na 3006 ms af met een nette traceback, waarna het
bord gewoon doordraait.

---

# XIAO nRF52840 Sense — IMU + microfoon test

Testsketch voor de Seeed Studio XIAO nRF52840 **Sense**: leest de LSM6DS3TR-C IMU
en de PDM-microfoon uit en toont beide live over de seriële poort. Getest en werkend
op het aangesloten bord (poort `/dev/cu.usbmodem83201`).

## Specificaties van het bord

| | |
|---|---|
| MCU | Nordic nRF52840, ARM Cortex-M4F @ 64 MHz |
| Geheugen | 1 MB flash + 256 KB RAM, plus 2 MB QSPI-flash on-board |
| Radio | Bluetooth LE + Mesh, NFC, ingebouwde antenne + u.FL-connector |
| IMU (alleen Sense) | LSM6DS3TR-C, 6-assig (accel + gyro + temp) |
| Microfoon (alleen Sense) | MSM261D3526H1CPM, PDM digitaal, mono |
| Batterij | BQ25101 lader, 50 mA of 100 mA (schakelbaar via P0.13), meting op P0.31 |
| I/O | 11 digitale / 6 analoge pinnen, UART, I2C, SPI, SWD |
| Afmeting | 21 × 17,8 mm, standby < 5 µA |

### Interne pinbezetting (uit de mbed-variant van de board-package)

| Functie | nRF-pin | Arduino-pin |
|---|---|---|
| IMU I2C SDA (`Wire1`) | P0.07 | 17 |
| IMU I2C SCL (`Wire1`) | P0.27 | 16 |
| IMU voeding | P1.08 | 15 (`PIN_LSM6DS3TR_C_POWER`) |
| IMU INT1 | P0.11 | 18 (`PIN_LSM6DS3TR_C_INT1`) |
| PDM DATA | P0.16 | 21 (`PIN_PDM_DIN`) |
| PDM CLK | P1.00 | 20 (`PIN_PDM_CLK`) |
| PDM voeding | P1.10 | 19 (`PIN_PDM_PWR`) |
| LED rood / groen / blauw | P0.26 / P0.30 / P0.06 | 12 / 13 / 14 (active-low) |
| Externe I2C (`Wire`) | P0.04 / P0.05 | D4 / D5 |

**Belangrijk:** de IMU zit op de *interne* bus `Wire1`, niet op `Wire` (D4/D5).
De Seeed-LSM6DS3-library `#define`t `Wire` voor dit board zelf naar `Wire1`, dus
`LSM6DS3 imu(I2C_MODE, 0x6A)` werkt zonder dat je iets hoeft door te geven — maar
een eigen I2C-scan op `Wire` vindt de IMU dus níet.

## Geïnstalleerde tools

| Tool | Versie | Waar |
|---|---|---|
| arduino-cli | 1.2.0 | meegeleverd met de Arduino IDE (niet meer via Homebrew) |
| Seeed nRF52 mbed-enabled Boards | 2.9.3 | `Seeeduino:mbed`, FQBN `Seeeduino:mbed:xiaonRF52840Sense` |
| Seeed Arduino LSM6DS3 | 2.0.7 | `~/Documents/Arduino/libraries` |
| PDM | meegeleverd met de core | — |
| adafruit-nrfutil | 0.5.3 | meegeleverd met de core, doet de DFU-upload |
| pyserial | 3.5 | in `.venv` |

De PDM-library zit al in de core, daar hoef je niets voor te installeren.

## Gebruik

```bash
./xiao.sh build                 # compileren
./xiao.sh upload                # compileren + flashen (poort wordt gedetecteerd)
./xiao.sh monitor               # live meetwaarden bekijken
./xiao.sh record                # 3 s opnemen naar recording.wav
./xiao.sh port                  # welke poort gedetecteerd wordt
```

Commando's in de monitor (typ het teken en druk enter):

| | |
|---|---|
| `h` | help |
| `i` | zelftest: I2C-scan van beide bussen, WHO_AM_I, PDM-status |
| `m` | monitorregels aan/uit |
| `r` | statistieken resetten |
| `+` / `-` | microfoon-gain ±5 (bereik 0–80, standaard 40) |
| `w` | 3 s audio opnemen en als base64 dumpen |

### Voorbeeld van de uitvoer

```
=== XIAO nRF52840 Sense - zelftest ===
  I2C scan:
    Wire1 (intern, IMU) : 0x6A(LSM6DS3TR-C)
    Wire  (extern D4/D5): (leeg)
  IMU         : WHO_AM_I=0x6A  (LSM6DS3TR-C, correct)  begin() OK
  Microfoon   : PDM actief, 16000 Hz mono, gain=40, samples=50176, drops=0, restarts=0

IMU a[g] -0.22 0.96 -0.06 |a|=0.99  g[dps] 0.1 -1.4 -0.2  T=26.6C || MIC rms=41 peak=191 dc=-3 dBFS=-58.0 [#     ] n=4096
```

`|a|` hoort ±1.00 te zijn als het bord stil ligt (dat is de zwaartekracht) —
draai het bord en de verdeling over x/y/z verschuift. Beweeg je het, dan lopen de
gyro-waarden op. `n` is het aantal audiosamples in het interval (≈4000 bij 16 kHz
en 250 ms); `drops` moet 0 blijven.

### Audio opnemen

`./xiao.sh record` stuurt `w`, vangt de base64-dump op en schrijft `recording.wav`,
met meteen een oordeel over het signaal:

```
Samples : 48000 (3.00 s @ 16000 Hz)
DC      : -0
RMS     : 32  (-60.3 dBFS)
Peak    : 315  (-40.3 dBFS)
Clipping: 0 samples
-> Ziet er gezond uit.
```

In een stille kamer zit de ruisvloer rond −60 dBFS; praten vlak bij het bord
brengt hem naar −25 à −15 dBFS. Te stil → gain omhoog met `+`, veel clipping →
omlaag met `-`.

## Aandachtspunten die uit het testen kwamen

* **De PDM-DMA valt stil bij overflow.** Leest je code de buffer niet snel genoeg
  leeg, dan roept de driver `nrf_pdm_disable()` aan en komt er nooit meer data.
  De sketch bewaakt daarom de sample-teller en herstart de PDM als er 500 ms lang
  niets binnenkwam.
* **`millis()` in de PDM-ISR is niet betrouwbaar** op deze core; de stall-detectie
  gebruikt daarom de sample-teller in `loop()` in plaats van een tijdstempel uit
  de interrupt.
* **De eerste ~0,5 s audio na het opstarten is rommel** (DC-instelling van de
  microfoon). Gooi die weg als je op het signaal gaat rekenen.
* De sketch drukt de DC-offset af; die hoort bij een gezonde opname rond 0 te
  liggen. Voor echt gebruik hoort er nog een hoogdoorlaatfilter op.

## Flashen als het bord niet reageert

Normaal doet `./xiao.sh upload` alles. Reageert het bord niet, druk dan **twee keer
snel op reset**: het bord meldt zich als USB-schijf `/Volumes/XIAO-SENSE` en de
rode LED pulseert. Je kunt dan een `.uf2` naar die schijf kopiëren, of gewoon
opnieuw `./xiao.sh upload` draaien — de bootloader accepteert ook dan de DFU-upload.
