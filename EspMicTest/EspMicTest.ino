/*
 * EspMicTest - spraakgestuurde 8x8 ledmatrix op een ESP32-C3 SuperMini
 *
 *   INMP441        SuperMini
 *   VDD      ->    3V3        (NIET 5V, de INMP441 is 1.8-3.3 V)
 *   GND      ->    GND
 *   L/R      ->    GND        linkerkanaal, niet zwevend laten
 *   SCK      ->    GPIO10     bit clock (autodetect probeert ook GPIO4)
 *   WS       ->    GPIO5      word select / LRCL
 *   SD       ->    GPIO6      data uit de mic
 *
 *   8x8 WS2812B    DIN -> GPIO3, 5V + GND (zie README voor stroom)
 *   drukknop       GPIO7 <-> GND (interne pull-up)
 *
 * Board  : esp32:esp32:esp32c3 met USB CDC aan en huge_app-partitie
 *
 * Gedrag:
 *   - Knop indrukken: een eventueel lopend MicroPython-script wordt
 *     afgebroken, er verschijnt een ademende rode cirkel op de matrix en
 *     de audio streamt live (chunked) naar Groq Whisper.
 *   - Knop loslaten: opname sluit af, transcript -> LLM -> gegenereerde
 *     MicroPython-code draait op de ingebedde interpreter. Oneindige
 *     animatielussen zijn prima: de volgende knopdruk breekt ze af.
 *
 * De microfoon draait op een eigen FreeRTOS-task die continu in een
 * ringbuffer schrijft; zo gaat er niets verloren tijdens de TLS-handshake.
 *
 * Seriele commando's (115200 baud):
 *   h help, i info, m monitor aan/uit, r stats reset, +/- gain,
 *   s slot L/R, c readLen, f sample rate, d raw dump, w 3 s wav-dump,
 *   t spraakflow met vaste 5 s (testen zonder knop), p demo-script,
 *   P guard-test, x lopend script afbreken, W wifi
 *
 * Wifi-gegevens en de Groq API-key staan in secrets.h.
 */

#include <Arduino.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <math.h>
#include "secrets.h"
#include "font5x7.h"

// MicroPython draait op de loop-task; de standaard 8 kB stack is te krap
// voor de VM (zie ook mp_stack_set_limit in embed_api.c).
SET_LOOP_TASK_STACK_SIZE(20 * 1024);

// Brug naar de MicroPython embed-port (embed_api.c)
extern "C" void embed_run(const char *code, uint32_t budget_ms);

// ---------------------------------------------------------------- configuratie

static const uint32_t SERIAL_BAUD    = 115200;
static const uint32_t SAMPLE_RATE    = 16000;
// De mic-bitclock wordt bij het opstarten automatisch gezocht op deze pinnen
// (in volgorde): zo maakt het niet uit of de SCK-draad op 10 of nog op 4 zit.
static const int8_t   BCLK_CANDIDATES[] = {10, 4};
static int8_t         micBclkPin     = 10;   // actieve BCLK-pin (autodetect)
static const int8_t   PIN_I2S_WS     = 5;    // INMP441 WS
static const int8_t   PIN_I2S_DIN    = 6;    // INMP441 SD
static const int      PIN_MATRIX     = 3;    // WS2812B DIN
static const int      PIN_BUTTON     = 7;    // knop naar GND, interne pull-up
static const int      PIN_LED        = 8;    // onboard blauwe LED, active-low
static const uint32_t MONITOR_PERIOD = 250;  // ms tussen monitorregels
static const float    TEST_SECONDS   = 5.0f; // opnameduur van het 't'-commando
static const uint32_t MAX_RECORD_MS  = 45000;  // mu-law: ~45 s past in de 1 MB flash-partitie

// 0 = rijen recht achter elkaar (row-major), 1 = zigzag (serpentine).
// Loopt het 'p'-demodiagonaaltje zichtbaar krom, zet dan 1.
#define MATRIX_SERPENTINE 0
static const uint8_t MATRIX_BRIGHTNESS_DEFAULT = 40;

static const int CHUNK = 480;  // buffergrootte; readLen bepaalt hoeveel we echt lezen

// De ESP_I2S-standaardconfig gebruikt dma_frame_num = 240. Lees je een aantal
// samples dat daar geen deler/veelvoud van is, dan is het laatste sample van
// elke 3e leesbeurt 8 bits naar links verschoven - een spike van honderden keren
// de buurwaarde. Met 240, 480 of 120 is dat weg. Gemeten: 58 spikes per 3 s bij
// readLen 256, exact 0 bij 240/480/120.
static const int DMA_FRAME_NUM = 240;

// Gain als schuifafstand van de 24-bits waarde naar 16 bits.
// shift 8 = 1x (0 dB), elke stap omlaag is +6 dB. Default 4 = 16x = +24 dB.
static const int SHIFT_MIN = 0;
static const int SHIFT_MAX = 12;
static const int SHIFT_DEF = 4;

static const float RECORD_SECONDS = 3.0f;   // duur van de 'w' base64-dump

// ---------------------------------------------------------------- toestand

I2SClass i2s;

static bool              micOk      = false;
static int               micShift   = SHIFT_DEF;
static int8_t            slotMask   = I2S_STD_SLOT_LEFT;
static bool              monitorOn  = false;  // 'm' zet de meetregels aan
static volatile uint32_t sampleRate = SAMPLE_RATE;
static volatile int      readLen    = DMA_FRAME_NUM;
static volatile bool     micReconfig = false;

static volatile uint32_t totalSamples = 0;
static volatile uint32_t i2sStalls    = 0;
static volatile uint32_t micRestarts  = 0;

static int32_t rawBuf[CHUNK];
static int16_t pcmBuf[CHUNK];

// raw-dump op verzoek van het 'd'-commando (micTask vult, main print)
static volatile bool rawDumpReq = false;
static int32_t       rawDumpBuf[8];

// ---------------------------------------------------------------- opname-ring
// Gevuld door micTask zodra captureOn staat; leeggelezen door recordToFile().
// Sinds de opname naar flash gaat hoeft deze ring geen netwerkstalls meer te
// overbruggen, alleen nog de schrijflatentie van LittleFS: 4096 samples =
// 8 kB = 256 ms is ruim zat. Klein houden is hier geen detail maar cruciaal -
// de ring staat in .bss en gaat dus rechtstreeks van de heap af, en de
// TLS-handshake heeft ~50 kB vrij nodig. Met de oude 32 kB-ring sneuvelde de
// upload op "BIGNUM - Memory allocation failed". Overloop zie je als drops.

#define RING_SAMPLES 4096
static int16_t           ring[RING_SAMPLES];
static volatile uint32_t ringHead  = 0;
static volatile uint32_t ringTail  = 0;
static volatile uint32_t ringDrops = 0;
static volatile bool     captureOn = false;

static uint32_t captureRead(int16_t *dst, uint32_t maxN) {
  uint32_t avail = ringHead - ringTail;
  if (avail == 0) return 0;
  if (avail > maxN) avail = maxN;
  const uint32_t tail = ringTail;
  for (uint32_t i = 0; i < avail; i++) {
    dst[i] = ring[(tail + i) & (RING_SAMPLES - 1)];
  }
  ringTail = tail + avail;
  return avail;
}

// ---------------------------------------------------------------- statistieken

static uint64_t          statSumSq   = 0;
static int32_t           statSum     = 0;
static volatile uint32_t statCount   = 0;
static int16_t           statPeak    = 0;
static uint32_t          statPeak24  = 0;
static uint32_t          statClipped = 0;

static void statsReset() {
  statSumSq   = 0;
  statSum     = 0;
  statCount   = 0;
  statPeak    = 0;
  statPeak24  = 0;
  statClipped = 0;
}

// ---------------------------------------------------------------- matrix
// Ook de MicroPython-API (embed_api.c) komt hier binnen via de hal_*-functies.

static Adafruit_NeoPixel matrix(64, PIN_MATRIX, NEO_GRB + NEO_KHZ800);

static inline uint16_t matrixIndex(int x, int y) {
#if MATRIX_SERPENTINE
  return (y & 1) ? (uint16_t)(y * 8 + (7 - x)) : (uint16_t)(y * 8 + x);
#else
  return (uint16_t)(y * 8 + x);
#endif
}

extern "C" void hal_matrix_set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  matrix.setPixelColor(matrixIndex(x, y), r, g, b);
}

extern "C" void hal_matrix_fill(uint8_t r, uint8_t g, uint8_t b) {
  matrix.fill(matrix.Color(r, g, b));
}

extern "C" void hal_matrix_show(void) {
  matrix.show();
}

extern "C" void hal_matrix_brightness(uint8_t n) {
  matrix.setBrightness(n);
}

static void matrixClearShow() {
  matrix.clear();
  matrix.show();
}

// ---------------------------------------------------------------- iconen & font
// Kant-en-klare 8x8-symbolen en een 5x7-font (font5x7.h), zodat het LLM
// niet elk figuur pixel voor pixel hoeft te ontwerpen. Rij-bitmaps,
// bit 7 = linkerkolom (x=0).

struct MatrixIcon {
  const char* name;
  uint8_t rows[8];
};

static const MatrixIcon ICONS[] = {
  { "heart",       {0x66,0xFF,0xFF,0xFF,0x7E,0x3C,0x18,0x00} },
  { "smiley",      {0x3C,0x42,0xA5,0x81,0xA5,0x99,0x42,0x3C} },
  { "sad",         {0x3C,0x42,0xA5,0x81,0x99,0xA5,0x42,0x3C} },
  { "arrow_up",    {0x18,0x3C,0x7E,0xFF,0x18,0x18,0x18,0x18} },
  { "arrow_down",  {0x18,0x18,0x18,0x18,0xFF,0x7E,0x3C,0x18} },
  { "arrow_left",  {0x10,0x30,0x7F,0xFF,0x7F,0x30,0x10,0x00} },
  { "arrow_right", {0x08,0x0C,0xFE,0xFF,0xFE,0x0C,0x08,0x00} },
  { "check",       {0x00,0x01,0x03,0x86,0xCC,0x78,0x30,0x00} },
  { "cross",       {0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3} },
  { "star",        {0x18,0x18,0xFF,0x7E,0x3C,0x66,0xC3,0x00} },
  { "sun",         {0x18,0x99,0x5A,0x3C,0x3C,0x5A,0x99,0x18} },
  { "square",      {0xFF,0x81,0x81,0x81,0x81,0x81,0x81,0xFF} },
  { "circle",      {0x3C,0x42,0x81,0x81,0x81,0x81,0x42,0x3C} },
  { "diamond",     {0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x3C,0x18} },
};

// Zet de gezette bits in de tekenbuffer; laat de rest van de buffer staan
// (matrix_clear() vooraf is aan de aanroeper).
static void drawRows(const uint8_t rows[8], uint8_t r, uint8_t g, uint8_t b) {
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 8; x++) {
      if (rows[y] & (0x80 >> x)) {
        matrix.setPixelColor(matrixIndex(x, y), r, g, b);
      }
    }
  }
}

extern "C" void hal_matrix_bitmap(const uint8_t rows[8], uint8_t r, uint8_t g, uint8_t b) {
  drawRows(rows, r, g, b);
}

// 5x7-teken, gecentreerd (kolommen 1..5, rijen 0..6). Font: bit 0 = bovenste rij.
extern "C" void hal_matrix_char(char c, uint8_t r, uint8_t g, uint8_t b) {
  if (c < 32 || c > 126) c = '?';
  const unsigned char* glyph = &FONT5X7[(c - 32) * 5];
  for (int col = 0; col < 5; col++) {
    for (int y = 0; y < 7; y++) {
      if (glyph[col] & (1 << y)) {
        matrix.setPixelColor(matrixIndex(col + 1, y), r, g, b);
      }
    }
  }
}

extern "C" int hal_matrix_icon(const char* name, uint8_t r, uint8_t g, uint8_t b) {
  for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++) {
    if (strcasecmp(name, ICONS[i].name) == 0) {
      drawRows(ICONS[i].rows, r, g, b);
      return 1;
    }
  }
  hal_matrix_char('?', r, g, b);   // onbekende naam: vraagteken
  return 0;
}

extern "C" int hal_abort_requested(void);   // gedefinieerd verderop

// Scrollt de tekst een keer voorbij (~60 ms per kolom) en showt zelf.
// De knop of 'x' breekt de scroll af.
extern "C" void hal_matrix_text(const char* s, uint8_t r, uint8_t g, uint8_t b) {
  const int wtot = (int)strlen(s) * 6;
  for (int off = -8; off <= wtot; off++) {
    matrix.clear();
    for (int x = 0; x < 8; x++) {
      const int vx = off + x;
      if (vx < 0 || vx >= wtot) continue;
      const int col = vx % 6;
      if (col == 5) continue;               // spatiekolom tussen tekens
      char c = s[vx / 6];
      if (c < 32 || c > 126) c = '?';
      const unsigned char bits = FONT5X7[(c - 32) * 5 + col];
      for (int y = 0; y < 7; y++) {
        if (bits & (1 << y)) matrix.setPixelColor(matrixIndex(x, y), r, g, b);
      }
    }
    matrix.show();
    delay(60);
    if (hal_abort_requested()) return;
  }
}

// Pulserende vier middelste pixels (rood) tijdens de opname; ~30 fps,
// 1,5 s per pulsslag. Bewust maar 4 pixels: zuinig op de voeding terwijl
// de wifi-zender vol staat te werken.
static void breatheReset() {
  matrix.clear();
}

static void breatheUpdate() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 33) return;
  last = now;

  const float phase = (float)(now % 1500) / 1500.0f;
  const uint8_t v = (uint8_t)(15.0f + 110.0f * (0.5f - 0.5f * cosf(2.0f * PI * phase)));

  matrix.clear();
  matrix.setPixelColor(matrixIndex(3, 3), v, 0, 0);
  matrix.setPixelColor(matrixIndex(4, 3), v, 0, 0);
  matrix.setPixelColor(matrixIndex(3, 4), v, 0, 0);
  matrix.setPixelColor(matrixIndex(4, 4), v, 0, 0);
  matrix.show();
}

// Knippert een rood pixel in de rechterbovenhoek zolang er geen
// internetverbinding is. Wordt alleen vanuit loop() bijgewerkt, dus terwijl
// een Python-script of een opname het scherm gebruikt blijft het met rust.
static void wifiIndicator() {
  static uint32_t last = 0;
  static bool aan = false;
  const bool verbonden = (WiFi.status() == WL_CONNECTED);

  if (verbonden) {
    if (aan) {                      // indicator opruimen zodra er verbinding is
      matrix.setPixelColor(matrixIndex(7, 0), 0, 0, 0);
      matrix.show();
      aan = false;
    }
    return;
  }
  if (millis() - last < 700) return;
  last = millis();
  aan = !aan;
  matrix.setPixelColor(matrixIndex(7, 0), aan ? 70 : 0, 0, 0);
  matrix.show();
}

// ---------------------------------------------------------------- knop + status-LED

static bool buttonDown() {
  return digitalRead(PIN_BUTTON) == LOW;
}

// A/B-schakelaar ('A'): opname-animatie tijdens het streamen aan/uit, om te
// kunnen meten of matrix.show() de TLS-upload stoort.
static bool recAnim = true;

static void statusLed(bool on) {
  ledcWrite(PIN_LED, on ? 0 : 255);   // active-low
}

// ---------------------------------------------------------------- hal voor MicroPython

extern "C" uint32_t hal_millis(void)          { return millis(); }
extern "C" void     hal_delay_ms(uint32_t ms) { delay(ms); }
extern "C" void     hal_serial_write(const char *s, size_t len) {
  Serial.write((const uint8_t*)s, len);
}

// Knop ingedrukt of 'x' getypt: lopend Python-script afbreken. De knop wordt
// bewust NIET geconsumeerd, zodat loop() hem daarna ziet en de opname start.
extern "C" int hal_abort_requested(void) {
  if (buttonDown()) return 1;
  if (Serial.available() && Serial.peek() == 'x') {
    Serial.read();
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------- hoogdoorlaat
// De INMP441 heeft een flinke DC-offset. Eenpolig hoogdoorlaatfilter met
// a = 1015/1024 geeft een kantelpunt van ~22 Hz bij 16 kHz.

static int32_t hpPrevX = 0;
static int64_t hpPrevY = 0;

static inline int32_t hpFilter(int32_t x) {
  int64_t y = (int64_t)(x - hpPrevX) + ((hpPrevY * 1015) >> 10);
  if (y >  (int64_t)1 << 30) y =  (int64_t)1 << 30;
  if (y < -((int64_t)1 << 30)) y = -((int64_t)1 << 30);
  hpPrevX = x;
  hpPrevY = y;
  return (int32_t)y;
}

static void hpReset() {
  hpPrevX = 0;
  hpPrevY = 0;
}

// ---------------------------------------------------------------- base64
// Zelfde streaming-encoder als in XiaoSenseTest, zodat tools/record_wav.py
// ongewijzigd blijft werken.

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t  b64Carry[2];
static uint8_t  b64CarryLen = 0;
static char     b64Line[328];
static uint16_t b64LineLen = 0;

static void b64Reset() {
  b64CarryLen = 0;
  b64LineLen  = 0;
}

static void b64EmitTriplet(uint8_t a, uint8_t b, uint8_t c) {
  b64Line[b64LineLen++] = B64[a >> 2];
  b64Line[b64LineLen++] = B64[((a & 0x03) << 4) | (b >> 4)];
  b64Line[b64LineLen++] = B64[((b & 0x0F) << 2) | (c >> 6)];
  b64Line[b64LineLen++] = B64[c & 0x3F];
  if (b64LineLen >= 320) {
    Serial.write((const uint8_t*)b64Line, b64LineLen);
    Serial.println();
    b64LineLen = 0;
  }
}

static void b64Write(const uint8_t* data, size_t len) {
  size_t i = 0;
  while (b64CarryLen > 0 && b64CarryLen < 3 && i < len) {
    b64Carry[b64CarryLen++] = data[i++];
    if (b64CarryLen == 3) {
      b64EmitTriplet(b64Carry[0], b64Carry[1], b64Carry[2]);
      b64CarryLen = 0;
    }
  }
  while (len - i >= 3) {
    b64EmitTriplet(data[i], data[i + 1], data[i + 2]);
    i += 3;
  }
  while (i < len) b64Carry[b64CarryLen++] = data[i++];
}

static void b64Flush() {
  if (b64CarryLen == 1) {
    uint8_t a = b64Carry[0];
    b64Line[b64LineLen++] = B64[a >> 2];
    b64Line[b64LineLen++] = B64[(a & 0x03) << 4];
    b64Line[b64LineLen++] = '=';
    b64Line[b64LineLen++] = '=';
  } else if (b64CarryLen == 2) {
    uint8_t a = b64Carry[0], b = b64Carry[1];
    b64Line[b64LineLen++] = B64[a >> 2];
    b64Line[b64LineLen++] = B64[((a & 0x03) << 4) | (b >> 4)];
    b64Line[b64LineLen++] = B64[(b & 0x0F) << 2];
    b64Line[b64LineLen++] = '=';
  }
  b64CarryLen = 0;
  if (b64LineLen > 0) {
    Serial.write((const uint8_t*)b64Line, b64LineLen);
    Serial.println();
    b64LineLen = 0;
  }
}

// ---------------------------------------------------------------- I2S + micTask

static bool micBegin() {
  i2s.end();
  i2s.setPins(micBclkPin, PIN_I2S_WS, -1 /* geen dout */, PIN_I2S_DIN);
  // 32-bits slot: de INMP441 stuurt 24 bits MSB-first en vult de rest met nullen.
  bool ok = i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_32BIT,
                      I2S_SLOT_MODE_MONO, slotMask);
  hpReset();
  return ok;
}

// Leest een blok, converteert naar 16 bits en werkt de statistieken bij.
// Geeft het aantal samples terug (0 = niets binnengekomen).
static uint32_t micRead() {
  size_t bytes = i2s.readBytes((char*)rawBuf, readLen * sizeof(int32_t));
  if (bytes == 0) {
    i2sStalls++;
    return 0;
  }
  const uint32_t n = bytes / sizeof(int32_t);

  for (uint32_t i = 0; i < n; i++) {
    int32_t s24 = rawBuf[i] >> 8;
    uint32_t a24 = (s24 < 0) ? (uint32_t)(-(int64_t)s24) : (uint32_t)s24;
    if (a24 > statPeak24) statPeak24 = a24;

    int32_t v = hpFilter(s24) >> micShift;
    if (v >  32767) { v =  32767; statClipped++; }
    if (v < -32768) { v = -32768; statClipped++; }

    int16_t s = (int16_t)v;
    pcmBuf[i] = s;

    statSum   += s;
    statSumSq += (uint64_t)((int32_t)s * (int32_t)s);
    int16_t abs16 = (s < 0) ? (int16_t)(-(int32_t)s) : s;
    if (abs16 > statPeak) statPeak = abs16;
    statCount = statCount + 1;
  }
  totalSamples += n;
  return n;
}

// Draait continu op een eigen task (prioriteit boven de loop-task), zodat de
// DMA ook tijdens de TLS-handshake en het draaien van Python leeggelezen
// wordt. Alle i2s-aanroepen gebeuren vanaf hier.
// Probeert de BCLK-kandidaten tot er echte data uit de mic komt. Zonder klok
// levert de INMP441 (vrijwel) louter nullen; met klok is er altijd ruis/DC
// op bijna elk sample. Streng criterium, want overspraak op een zwevende pin
// kan losse bitjes zetten en de mic heeft ~100 ms opstarttijd - een eerdere,
// te gretige versie koos daardoor na een herstart soms de verkeerde pin.
static void micAutodetect() {
  for (size_t k = 0; k < sizeof(BCLK_CANDIDATES); k++) {
    micBclkPin = BCLK_CANDIDATES[k];
    micOk = micBegin();
    if (!micOk) continue;
    vTaskDelay(pdMS_TO_TICKS(150));   // INMP441-opstarttijd afwachten
    micRead(); micRead();             // eerste blokken weggooien
    uint32_t nonzero = 0, totaal = 0, piek = 0;
    for (int blok = 0; blok < 8; blok++) {
      uint32_t n = micRead();
      for (uint32_t i = 0; i < n; i++) {
        if (rawBuf[i] != 0) nonzero++;
        int32_t s24 = rawBuf[i] >> 8;
        uint32_t a = (s24 < 0) ? (uint32_t)(-(int64_t)s24) : (uint32_t)s24;
        if (a > piek) piek = a;
      }
      totaal += n;
    }
    Serial.print("mic : SCK=GPIO"); Serial.print(micBclkPin);
    Serial.print(": "); Serial.print(nonzero); Serial.print("/"); Serial.print(totaal);
    Serial.print(" samples actief, piek "); Serial.println(piek);
    if (totaal > 0 && nonzero * 2 >= totaal && piek > 200) {
      Serial.print("mic : data gevonden met SCK op GPIO");
      Serial.println(micBclkPin);
      return;
    }
  }
  Serial.println("mic : GEEN echte data op SCK=GPIO10 of GPIO4 - zit de SCK-draad goed?");
  micBclkPin = BCLK_CANDIDATES[0];
  micOk = micBegin();
}

static void micTaskFn(void *arg) {
  (void)arg;
  micAutodetect();
  for (;;) {
    if (micReconfig) {
      micOk = micBegin();
      micReconfig = false;
      if (!micOk) Serial.println("!! I2S init mislukt - controleer de pinnen");
    }
    if (!micOk) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    uint32_t n = micRead();
    if (n == 0) {
      micOk = micBegin();
      micRestarts = micRestarts + 1;
      continue;
    }

    if (rawDumpReq) {
      memcpy(rawDumpBuf, rawBuf, sizeof(rawDumpBuf));
      rawDumpReq = false;
    }

    if (captureOn) {
      uint32_t head = ringHead;
      const uint32_t tail = ringTail;
      for (uint32_t i = 0; i < n; i++) {
        if (head - tail >= RING_SAMPLES) {
          ringDrops = ringDrops + (n - i);
          break;
        }
        ring[head & (RING_SAMPLES - 1)] = pcmBuf[i];
        head++;
      }
      ringHead = head;
    }
  }
}

// ---------------------------------------------------------------- instellingen (NVS)
// Via het webportaal ingestelde waarden overleven een herstart en gaan voor
// op de compile-time defaults uit secrets.h.

static Preferences prefs;
static String cfgSsid;    // extra wifi-netwerk uit het portaal
static String cfgPass;
static String groqKey;    // Groq API-key (NVS-override of secrets.h)

static void settingsLoad() {
  prefs.begin("voiceesp", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  groqKey = prefs.getString("gkey", GROQ_API_KEY);
}

// ---------------------------------------------------------------- wifi

static bool wifiApMode = false;

// Probeert de bekende netwerken in volgorde; lukt geen ervan, dan start een
// eigen access point. force=true (het 'W'-commando) probeert ook vanuit
// AP-modus opnieuw een echt netwerk; de spraakflow doet dat niet, anders
// wacht je bij elke knopdruk 2x 10 s als er geen netwerk is.
static bool wifiEnsure(bool force = false) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wifiApMode && !force) {
    Serial.println("wifi: AP-modus actief, geen internet - spraak->code kan niet");
    Serial.println("      (druk 'W' om opnieuw een netwerk te zoeken)");
    return false;
  }

  wifiApMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();   // reset een eventueel hangende eerdere poging
  delay(100);

  // Eerst kijken wat er werkelijk in de lucht hangt. Scheelt 10 s per netwerk
  // dat er niet is - blind proberen kostte tot 30 s voor het bord bruikbaar was.
  Serial.print("wifi: scannen... ");
  const int gezien = WiFi.scanNetworks();
  Serial.print(gezien < 0 ? 0 : gezien); Serial.println(" netwerken");

  auto zichtbaar = [&](const char* ssid, int32_t& rssi) -> bool {
    for (int k = 0; k < gezien; k++) {
      if (WiFi.SSID(k) == ssid) { rssi = WiFi.RSSI(k); return true; }
    }
    return false;
  };

  // eerst het via het portaal ingestelde netwerk, dan de lijst uit secrets.h
  // (die staat op voorkeursvolgorde; reserve-hotspot achteraan)
  const int extra = cfgSsid.length() ? 1 : 0;
  for (int n = 0; n < extra + WIFI_NET_COUNT; n++) {
    const char* ssid = (n < extra) ? cfgSsid.c_str() : WIFI_SSIDS[n - extra];
    const char* pass = (n < extra) ? cfgPass.c_str() : WIFI_PASSES[n - extra];

    // Alleen overslaan als de scan het echt gedaan heeft (gezien > 0);
    // mislukte de scan, dan toch maar blind proberen.
    int32_t rssi = 0;
    if (gezien > 0 && !zichtbaar(ssid, rssi)) {
      Serial.print("wifi: "); Serial.print(ssid); Serial.println(" niet in de lucht, overslaan");
      continue;
    }
    // Een nog lopende (mislukte) poging blokkeert de volgende begin() met
    // "sta is connecting, cannot set config" - radio echt resetten dus.
    WiFi.disconnect(true);
    delay(300);
    WiFi.mode(WIFI_STA);
    delay(100);
    Serial.print("wifi: verbinden met "); Serial.print(ssid);
    if (rssi) { Serial.print(" ("); Serial.print(rssi); Serial.print(" dBm)"); }
    WiFi.begin(ssid, pass);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
      delay(250);
      Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.scanDelete();
      // Modem-sleep staat in Arduino-ESP32 standaard AAN en drukt de
      // doorvoer tot ver onder de 32 kB/s die live audio nodig heeft
      // (gemeten: 18 kB/s met, ruim voldoende zonder). Uitzetten kost
      // ~60 mA extra maar maakt de stream betrouwbaar.
      WiFi.setSleep(false);
      // 15 dBm: compromis tussen bereik en de matige antenne-afstemming
      // van de SuperMini. (De eerdere 8,5 dBm was een verworpen hypothese;
      // de echte streamproblemen bleken modem-sleep en path-MTU.)
      WiFi.setTxPower(WIFI_POWER_15dBm);
      Serial.print(" ok, ip="); Serial.print(WiFi.localIP());
      Serial.print(" rssi="); Serial.print(WiFi.RSSI());
      Serial.println(" dBm, txpower=15 dBm");
      return true;
    }
    Serial.println(" niet gelukt");
    WiFi.disconnect();
    delay(100);
  }

  WiFi.scanDelete();
  Serial.print("wifi: geen bekend netwerk - eigen AP starten: ");
  Serial.print(WIFI_AP_SSID);
  // AP_STA zodat het portaal vanuit AP-modus nog netwerken kan scannen
  WiFi.mode(WIFI_AP_STA);
  if (WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS)) {
    wifiApMode = true;
    Serial.print("  ip="); Serial.println(WiFi.softAPIP());
    Serial.println("      configuratie: verbind met dit AP en open http://192.168.4.1");
    Serial.println("      (geen internet: spraak->code werkt pas weer op een echt netwerk)");
  } else {
    Serial.println("  MISLUKT");
  }
  return false;
}

// ---------------------------------------------------------------- wifi op de achtergrond
// wifiEnsure() kan tot 30 s duren (3 netwerken x 10 s). Dat mag setup() en
// loop() niet blokkeren, anders reageert de knop al die tijd niet. Opnemen
// heeft toch geen netwerk nodig - dat is pas bij het uploaden nodig.

static volatile bool wifiBusy = false;

static void wifiTaskFn(void *arg) {
  (void)arg;
  wifiEnsure(true);
  wifiBusy = false;
  vTaskDelete(NULL);
}

static void wifiStartBackground() {
  if (wifiBusy) return;
  wifiBusy = true;
  xTaskCreate(wifiTaskFn, "wifi", 8192, NULL, 1, NULL);
}

// Wacht tot een lopende achtergrondpoging klaar is en probeer daarna zelf
// nog een keer. Alleen aanroepen vanuit de loop-task (niet vanuit wifiTask).
static bool wifiReady(uint32_t maxMs) {
  const uint32_t t0 = millis();
  while (wifiBusy && millis() - t0 < maxMs) delay(50);
  if (WiFi.status() == WL_CONNECTED) return true;
  return wifiBusy ? false : wifiEnsure(true);
}

// ---------------------------------------------------------------- webportaal
// Bereikbaar op het STA-adres en, in AP-modus, op http://192.168.4.1.
// Hier stel je een extra wifi-netwerk en de Groq API-key in (NVS).

static WebServer portal(80);

static void portalRoot() {
  String h =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>VoiceEsp</title>"
    "<style>body{font-family:sans-serif;max-width:26em;margin:2em auto;padding:0 1em}"
    "input{width:100%;margin:.2em 0 .8em;padding:.4em}button{padding:.5em 1.2em}</style>"
    "<h2>VoiceEsp</h2><p>Status: ";
  if (WiFi.status() == WL_CONNECTED) {
    h += "verbonden met <b>" + WiFi.SSID() + "</b> (" + WiFi.localIP().toString() + ")";
  } else if (wifiApMode) {
    h += "AP-modus, geen internet";
  } else {
    h += "niet verbonden";
  }
  h += "</p><h3>Wifi instellen</h3>";

  int n = wifiBusy ? 0 : WiFi.scanNetworks();   // geen scan tijdens een lopende poging
  if (n > 0) {
    h += "<p>Gevonden netwerken:</p><ul>";
    for (int k = 0; k < n && k < 12; k++) {
      h += "<li><a href='#' onclick=\"document.getElementsByName('ssid')[0].value='" +
           WiFi.SSID(k) + "';return false\">" + WiFi.SSID(k) + "</a> (" +
           String(WiFi.RSSI(k)) + " dBm)</li>";
    }
    h += "</ul>";
  }
  WiFi.scanDelete();

  h += "<form method=post action=/save>"
       "SSID<br><input name=ssid value='" + cfgSsid + "'>"
       "Wachtwoord<br><input name=pass type=password>"
       "<button>Opslaan en herstarten</button></form>"
       "<h3>Groq API-key</h3>"
       "<form method=post action=/key>"
       "<input name=gkey placeholder='gsk_...'>"
       "<button>Opslaan</button></form>"
       "<p style='color:#666'>De key staat nu " +
       String(groqKey == GROQ_API_KEY ? "op de compile-time default." : "in NVS (portaal).") +
       "</p>";
  portal.send(200, "text/html", h);
}

static void portalSave() {
  cfgSsid = portal.arg("ssid");
  cfgPass = portal.arg("pass");
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  portal.send(200, "text/html",
              "<meta charset=utf-8>Opgeslagen - het bord herstart en probeert \"" +
              cfgSsid + "\".");
  delay(500);
  ESP.restart();
}

static void portalKey() {
  String k = portal.arg("gkey");
  k.trim();
  if (k.length() > 10) {
    groqKey = k;
    prefs.putString("gkey", groqKey);
    portal.send(200, "text/html", "<meta charset=utf-8>API-key opgeslagen.");
  } else {
    portal.send(400, "text/html", "<meta charset=utf-8>Dat lijkt geen geldige key.");
  }
}

static void portalBegin() {
  portal.on("/", portalRoot);
  portal.on("/save", HTTP_POST, portalSave);
  portal.on("/key", HTTP_POST, portalKey);
  portal.begin();
}

// ---------------------------------------------------------------- WAV-header

static void put32(uint8_t* p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put16(uint8_t* p, uint16_t v) { p[0]=v; p[1]=v>>8; }

// 44-byte canonieke PCM WAV-header, mono 16 bits
static void wavHeader(uint8_t* h, uint32_t rate, uint32_t dataBytes) {
  memcpy(h + 0,  "RIFF", 4);
  put32(h + 4,   36 + dataBytes);
  memcpy(h + 8,  "WAVEfmt ", 8);
  put32(h + 16,  16);
  put16(h + 20,  1);           // PCM
  put16(h + 22,  1);           // mono
  put32(h + 24,  rate);
  put32(h + 28,  rate * 2);
  put16(h + 32,  2);
  put16(h + 34,  16);
  memcpy(h + 36, "data", 4);
  put32(h + 40,  dataBytes);
}

// ---------------------------------------------------------------- mu-law (G.711)
// 2:1 compressie voor een handvol bewerkingen per sample - verwaarloosbaar op
// de C3, en WAV ondersteunt het native (format tag 7). Getest: Groq/Whisper
// transcribeert mu-law even goed als PCM16. Halveert de upload en daarmee de
// kans dat een wankele verbinding het begeeft.

static inline uint8_t linear2ulaw(int16_t sample) {
  const int32_t BIAS = 0x84, CLIP = 32635;
  int32_t s = sample;
  int32_t sign = (s < 0) ? 0x80 : 0;
  if (s < 0) s = -s;
  if (s > CLIP) s = CLIP;
  s += BIAS;
  int32_t exponent = 7;
  for (int32_t mask = 0x4000; exponent > 0 && !(s & mask); mask >>= 1) exponent--;
  int32_t mantissa = (s >> (exponent + 3)) & 0x0F;
  return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

// 58-byte mu-law WAV-header (fmt-chunk van 18 bytes + fact-chunk)
static const uint32_t ULAW_HDR_LEN = 58;

static void wavHeaderUlaw(uint8_t* h, uint32_t rate, uint32_t dataBytes) {
  memcpy(h + 0,  "RIFF", 4);
  put32(h + 4,   50 + dataBytes);
  memcpy(h + 8,  "WAVEfmt ", 8);
  put32(h + 16,  18);          // fmt-chunk lengte (met cbSize)
  put16(h + 20,  7);           // WAVE_FORMAT_MULAW
  put16(h + 22,  1);           // mono
  put32(h + 24,  rate);
  put32(h + 28,  rate);        // byte rate = 1 byte per sample
  put16(h + 32,  1);           // block align
  put16(h + 34,  8);           // bits per sample
  put16(h + 36,  0);           // cbSize
  memcpy(h + 38, "fact", 4);
  put32(h + 42,  4);
  put32(h + 46,  dataBytes);   // aantal samples
  memcpy(h + 50, "data", 4);
  put32(h + 54,  dataBytes);
}

// ---------------------------------------------------------------- HTTP-helpers

// Leest het HTTP-antwoord uit. Groq stuurt chunked terug, dus die codering
// halen we er hier af.
// maxBody begrenst de respons zodat een uitzonderlijk groot antwoord het
// geheugen niet opeet; overschrijding wordt gemeld, nooit stil afgekapt.
static bool httpBodyTeGroot = false;

static String httpReadBody(NetworkClientSecure& c, int& status, uint32_t maxBody = 48000) {
  const uint32_t deadline = millis() + 20000;
  status = 0;

  while (!c.available() && millis() < deadline) delay(10);

  String line = c.readStringUntil('\n');
  int sp = line.indexOf(' ');
  if (sp > 0) status = line.substring(sp + 1, sp + 4).toInt();

  bool chunked = false;
  while (millis() < deadline) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    String low = h; low.toLowerCase();
    if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) chunked = true;
  }

  String body;
  httpBodyTeGroot = false;
  body.reserve(2048);     // scheelt herhaald hergroeien tijdens het lezen
  if (chunked) {
    while (millis() < deadline) {
      String sz = c.readStringUntil('\n');
      sz.trim();
      if (sz.length() == 0) continue;
      long n = strtol(sz.c_str(), nullptr, 16);
      if (n <= 0) break;
      for (long i = 0; i < n && millis() < deadline; i++) {
        while (!c.available() && c.connected() && millis() < deadline) delay(1);
        int ch = c.read();
        if (ch < 0) break;
        if (body.length() >= maxBody) { httpBodyTeGroot = true; break; }
        body += (char)ch;
      }
      if (httpBodyTeGroot) break;
      c.readStringUntil('\n');
    }
  } else {
    while ((c.connected() || c.available()) && millis() < deadline) {
      if (c.available()) body += (char)c.read();
      else delay(5);
    }
  }
  body.trim();
  return body;
}

// ---------------------------------------------------------------- Groq Whisper

// Opnemen naar flash (LittleFS), daarna uploaden met bekende lengte.
// Real-time streaming is zo niet nodig: een trage of haperende verbinding
// kost alleen wachttijd, nooit audio. De RAM-ring (1 s) dempt alleen nog de
// schrijflatentie van de flash, en een mislukte upload krijgt automatisch
// een tweede poging - de opname staat immers veilig in flash (~25 s past
// in de 1 MB-partitie).

static const char* REC_PATH = "/opname.pcm";

// Fase 1: opnemen. Rood pulserende middenpixels = er wordt geluisterd.
// Geeft het aantal opgenomen bytes terug (0 = mislukt).
static uint32_t recordToFile(float fixedSeconds) {
  File f = LittleFS.open(REC_PATH, "w");
  if (!f) {
    Serial.println("groq: kan opnamebestand niet openen");
    return 0;
  }

  ringTail  = ringHead;
  ringDrops = 0;
  captureOn = true;
  statusLed(true);
  breatheReset();
  breatheUpdate();          // meteen zichtbaar, niet pas na het eerste blok
  Serial.println(fixedSeconds > 0 ? ">>> SPREEK NU <<<"
                                  : ">>> SPREEK (knop vasthouden, loslaten = klaar) <<<");

  static int16_t out[480];
  static uint8_t ulaw[480];
  uint32_t geschreven = 0;
  const uint32_t t0 = millis();
  bool ok = true;

  while (ok) {
    const uint32_t elapsed = millis() - t0;
    const bool doorgaan = (fixedSeconds > 0)
                            ? (elapsed < (uint32_t)(fixedSeconds * 1000.0f))
                            : buttonDown();
    if (!doorgaan || elapsed > MAX_RECORD_MS) break;

    uint32_t n = captureRead(out, 480);
    if (n > 0) {
      for (uint32_t i = 0; i < n; i++) ulaw[i] = linear2ulaw(out[i]);
      ok = f.write(ulaw, n) == n;
      geschreven += n;
    } else {
      delay(4);
    }
    if (recAnim) breatheUpdate();
  }
  captureOn = false;

  uint32_t n;
  while (ok && (n = captureRead(out, 480)) > 0) {
    for (uint32_t i = 0; i < n; i++) ulaw[i] = linear2ulaw(out[i]);
    ok = f.write(ulaw, n) == n;
    geschreven += n;
  }
  f.close();
  statusLed(false);

  Serial.print("groq: "); Serial.print(geschreven); Serial.print(" bytes mu-law (");
  Serial.print(geschreven / 16); Serial.print(" ms) opgenomen naar flash");
  if (ringDrops) { Serial.print(", drops="); Serial.print(ringDrops); }
  Serial.println(ok ? "" : " - SCHRIJVEN MISLUKT");
  statsReset();
  return ok ? geschreven : 0;
}

// Fase 2: uploaden met Content-Length en een retry. Geel stipje = bezig.
static String uploadRecording() {
  if (!wifiReady(35000)) {
    Serial.println("groq: geen wifi - upload overgeslagen");
    matrix.clear();
    hal_matrix_icon("cross", 80, 0, 0);
    matrix.show();
    return String();
  }
  const char* BOUNDARY = "----EspMicTestBoundary7f3a";

  String pre;
  pre.reserve(512);
  auto field = [&](const char* name, const char* value) {
    pre += "--"; pre += BOUNDARY; pre += "\r\n";
    pre += "Content-Disposition: form-data; name=\""; pre += name; pre += "\"\r\n\r\n";
    pre += value; pre += "\r\n";
  };
  field("model", GROQ_MODEL);
  field("response_format", "text");
  if (strlen(GROQ_LANGUAGE) > 0) field("language", GROQ_LANGUAGE);
  pre += "--"; pre += BOUNDARY; pre += "\r\n";
  pre += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  pre += "Content-Type: audio/wav\r\n\r\n";

  String post = "\r\n--";
  post += BOUNDARY;
  post += "--\r\n";

  File f = LittleFS.open(REC_PATH, "r");
  if (!f || f.size() == 0) return String();
  const uint32_t pcmBytes = f.size();     // mu-law: 1 byte per sample
  const uint32_t contentLength = pre.length() + ULAW_HDR_LEN + pcmBytes + post.length();

  // geel stipje: uploaden / wachten op het transcript
  matrix.clear();
  matrix.setPixelColor(matrixIndex(3, 3), 60, 45, 0);
  matrix.setPixelColor(matrixIndex(4, 3), 60, 45, 0);
  matrix.setPixelColor(matrixIndex(3, 4), 60, 45, 0);
  matrix.setPixelColor(matrixIndex(4, 4), 60, 45, 0);
  matrix.show();

  String body;
  int status = 0;

  for (int poging = 1; poging <= 2 && status != 200; poging++) {
    NetworkClientSecure client;
    client.setInsecure();          // geen certificaatcontrole, zie README
    // Ruim: de opname staat in flash, dus wachten kost geen audio. Deze
    // waarde is ook de socket_timeout van de TLS-schrijflus - te kort en
    // een volle verzendbuffer laat de upload sneuvelen (was 4 s: dood na
    // ~4 kB, precies de LWIP-verzendbuffer).
    client.setTimeout(20000);

    Serial.print("groq: upload poging "); Serial.print(poging); Serial.print("... ");
    const uint32_t t0 = millis();
    if (!client.connect("api.groq.com", 443)) {
      char err[128] = {0};
      int ec = client.lastError(err, sizeof(err));
      Serial.print("TLS MISLUKT ("); Serial.print(ec); Serial.print(" ");
      Serial.print(err); Serial.println(")");
      continue;
    }
    client.setNoDelay(true);       // kleine segmenten, zie README (path-MTU)
    Serial.print("(heap na TLS: "); Serial.print(ESP.getFreeHeap()); Serial.print(") ");

    client.print("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
    client.print("Host: api.groq.com\r\n");
    client.print("Authorization: Bearer "); client.print(groqKey); client.print("\r\n");
    client.print("Content-Type: multipart/form-data; boundary=");
    client.print(BOUNDARY); client.print("\r\n");
    client.print("Content-Length: "); client.print(contentLength); client.print("\r\n");
    client.print("Connection: close\r\n\r\n");
    client.print(pre);

    uint8_t hdr[ULAW_HDR_LEN];
    wavHeaderUlaw(hdr, sampleRate, pcmBytes);
    bool ok = client.write(hdr, sizeof(hdr)) == sizeof(hdr);

    f.seek(0);
    static uint8_t buf[700];       // < pad-MTU, elk blok een eigen segment
    uint32_t verstuurd = 0;
    while (ok && verstuurd < pcmBytes) {
      size_t nb = f.read(buf, sizeof(buf));
      if (nb == 0) break;
      ok = client.write(buf, nb) == nb;
      verstuurd += nb;
    }
    if (!ok || verstuurd != pcmBytes) {
      char err[128] = {0};
      int ec = client.lastError(err, sizeof(err));
      Serial.print("verbinding weg na "); Serial.print(verstuurd);
      Serial.print("/"); Serial.print(pcmBytes);
      Serial.print(" bytes (err "); Serial.print(ec); Serial.print(" ");
      Serial.print(err); Serial.print(", heap "); Serial.print(ESP.getFreeHeap());
      Serial.println(")");
      client.stop();
      delay(500);          // socket echt laten opruimen voor de retry
      continue;
    }
    client.print(post);
    Serial.print(millis() - t0); Serial.println(" ms, wachten op antwoord...");

    const uint32_t tResp = millis();
    body = httpReadBody(client, status);
    client.stop();
    Serial.print("groq: HTTP "); Serial.print(status);
    Serial.print(", "); Serial.print(millis() - tResp); Serial.println(" ms");
    if (status != 200) { Serial.print("FOUT : "); Serial.println(body); }
  }
  f.close();

  if (status != 200) {
    matrix.clear();
    hal_matrix_icon("cross", 80, 0, 0);   // rood kruis: opnieuw proberen
    matrix.show();
    return String();
  }
  matrixClearShow();
  return body;
}

// Opnemen + uploaden; geeft de transcriptie terug ("" bij fout).
static String transcribeStream(float fixedSeconds) {
  // Bewust geen wifi-check vooraf: opnemen kan zonder netwerk, en zo reageert
  // de knop meteen ook als de verbinding nog opgezet wordt.
  if (recordToFile(fixedSeconds) == 0) {
    matrixClearShow();
    return String();
  }
  return uploadRecording();
}

// ---------------------------------------------------------------- Groq LLM

// De context die het LLM meekrijgt: transcriptie in, MicroPython uit.
// De API hieronder moet exact overeenkomen met wat embed_api.c registreert.
static const char* LLM_SYSTEM =
  "Je zet gesproken instructies om in MicroPython-code voor een ESP32-C3 met "
  "een 8x8 WS2812-ledmatrix. De gebruikerstekst is een transcriptie van spraak "
  "(meestal Nederlands) en kan herkenningsfouten bevatten; interpreteer "
  "welwillend wat er bedoeld wordt. "
  "Antwoord met UITSLUITEND uitvoerbare MicroPython-code: geen markdown, geen "
  "```-blokken, geen uitleg. "
  "Er is ALLEEN deze API beschikbaar; imports bestaan niet en zijn verboden: "
  "matrix_set(x, y, r, g, b) zet een pixel in de tekenbuffer (x 0..7 van links "
  "naar rechts, y 0..7 van boven naar onder, kleuren 0..255); "
  "matrix_fill(r, g, b); matrix_clear(); "
  "matrix_icon(naam, r, g, b) tekent een kant-en-klaar 8x8-symbool in de "
  "buffer, naam uit: heart, smiley, sad, arrow_up, arrow_down, arrow_left, "
  "arrow_right, check, cross, star, sun, square, circle, diamond - gebruik "
  "dit waar mogelijk in plaats van zelf pixels te zetten; "
  "matrix_char(teken, r, g, b) tekent een letter/cijfer/leesteken (5x7, "
  "gecentreerd) in de buffer; "
  "matrix_text(tekst, r, g, b) scrollt een tekst een keer voorbij en showt "
  "zelf (blokkeert tot klaar) - gebruik dit voor woorden of zinnen; "
  "matrix_bitmap(rijen, r, g, b) tekent een eigen figuur: rijen is een lijst "
  "van 8 getallen, een per rij van boven naar onder, bit 7 = linkerkolom "
  "(bijvoorbeeld 0b00111100 voor 4 pixels in het midden); "
  "matrix_show() stuurt de tekenbuffer naar de leds en is verplicht om iets "
  "zichtbaar te maken (alleen matrix_text doet dat zelf); "
  "matrix_brightness(n) met n 0..255 (staat standaard op 40, hoger dan 80 "
  "alleen als de gebruiker er expliciet om vraagt); "
  "sleep_ms(ms); millis(); print(...). "
  "Daarnaast bestaan time (sleep, sleep_ms, ticks_ms) en random (randint, "
  "randrange, choice, shuffle, random) al als globale objecten - gebruik ze "
  "zonder import; imports zijn en blijven verboden. "
  "De matrix is de enige uitvoer. Animaties met een oneindige lus (while True) "
  "zijn prima en zelfs gewenst als de gebruiker iets blijvends of bewegends "
  "vraagt; roep dan in elke lus-iteratie sleep_ms(20) of meer aan. Het "
  "programma wordt automatisch afgebroken zodra de gebruiker de knop indrukt; "
  "daar hoef je niets voor te programmeren. "
  "Vraagt de gebruiker iets dat niet met de matrix kan, doe dan wat het "
  "dichtst in de buurt komt en print een korte melding. "
  "Schrijf een VOLLEDIGE, werkende implementatie - geen schets of vereenvoudigde "
  "versie. Een animatie moet er ook echt uitzien zoals gevraagd: vraagt iemand "
  "iets als Tetris, dan vallen blokken van boven naar beneden, blijven ze liggen "
  "op de bodem of bovenop eerder gevallen blokken (hou een speelveld bij van "
  "8x8), en gebruik verschillende tetromino-vormen - niet alleen losse "
  "vierkantjes. Volle rijen mogen verdwijnen. Neem gerust 100+ regels als dat "
  "nodig is; er is ruimte zat. Alleen geen commentaarregels of uitleg. "
  "Het resultaat moet er op het scherm altijd zo professioneel en volledig "
  "mogelijk uitzien: heldere, goed onderscheidbare kleuren, vloeiende beweging "
  "(20 tot 150 ms per stap), niets dat halverwege blijft hangen, en het beeld "
  "vult de 8x8 zinvol. Doe het volledig af zoals de gebruiker het zich zou "
  "voorstellen, ook als dat meer code kost. "
  "De animatie moet blijven doorlopen: bereikt een simulatie of spel een "
  "eindtoestand (game over, alles vol, uitgestorven), begin dan automatisch "
  "opnieuw in plaats van te stoppen. Varieer waar het kan - bijvoorbeeld de "
  "horizontale startpositie - zodat het beeld niet steeds hetzelfde is.";

// Stuurt de instructie naar het chat-model en geeft de gegenereerde
// MicroPython-code terug (leeg bij een fout).
static String llmGenerateCode(const String& instruction) {
  if (!wifiEnsure()) return String();

  JsonDocument req;
  req["model"] = GROQ_LLM_MODEL;
  req["temperature"] = 0.2;
  // De echte grens is niet het model (gpt-oss-120b kan 65536 uit) en ook niet
  // het RAM van de C3 (~70 kB vrij tijdens deze stap, respons van 6 kB kostte
  // 1,6 kB), maar de TPM-limiet van het Groq-account: de free tier staat 8000
  // tokens per MINUUT toe, en prompt + max_tokens samen tellen daarin mee.
  // Met 8000 gaf dat meteen HTTP 413. 5000 laat ~200 regels code toe en houdt
  // marge voor de systeemprompt.
  req["max_tokens"] = 5000;
  JsonArray msgs = req["messages"].to<JsonArray>();
  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = LLM_SYSTEM;
  JsonObject usr = msgs.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = instruction;

  String body;
  serializeJson(req, body);

  NetworkClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);   // ms! (core 3.x; 2.x nam hier seconden)

  Serial.print("llm : verbinden... ");
  uint32_t t0 = millis();
  if (!client.connect("api.groq.com", 443)) {
    Serial.println("MISLUKT (TLS)");
    return String();
  }
  Serial.print(millis() - t0); Serial.println(" ms");

  client.print("POST /openai/v1/chat/completions HTTP/1.1\r\n");
  client.print("Host: api.groq.com\r\n");
  client.print("Authorization: Bearer "); client.print(groqKey); client.print("\r\n");
  client.print("Content-Type: application/json\r\n");
  client.print("Content-Length: "); client.print(body.length()); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  int status = 0;
  t0 = millis();
  String resp = httpReadBody(client, status);
  client.stop();
  Serial.print("llm : HTTP "); Serial.print(status);
  Serial.print(", "); Serial.print(millis() - t0); Serial.println(" ms");

  if (status != 200) {
    Serial.print("FOUT : "); Serial.println(resp);
    return String();
  }

  Serial.print("llm : respons "); Serial.print(resp.length());
  Serial.print(" bytes, heap "); Serial.print(ESP.getFreeHeap());

  // Filter: het JsonDocument houdt alleen content + finish_reason vast in
  // plaats van het hele antwoord. Scheelt bij grote programma's kilobytes.
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["choices"][0]["finish_reason"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, resp,
                                             DeserializationOption::Filter(filter));
  Serial.print(" -> na parse "); Serial.println(ESP.getFreeHeap());
  if (err) {
    Serial.print("FOUT : JSON: "); Serial.println(err.c_str());
    return String();
  }
  const char* content = doc["choices"][0]["message"]["content"];
  if (content == nullptr) {
    Serial.println("FOUT : geen content in het antwoord");
    return String();
  }

  // Loopt het model tegen max_tokens, dan is de code middenin afgekapt en
  // levert uitvoeren alleen een syntaxfout op een zwart scherm op. Dat is
  // precies wat er misging bij "maak een tetris-animatie".
  if (httpBodyTeGroot) {
    Serial.println("FOUT : antwoord groter dan de buffer - vraag iets eenvoudigers");
    return String();
  }

  const char* reden = doc["choices"][0]["finish_reason"];
  if (reden != nullptr && strcmp(reden, "stop") != 0) {
    Serial.print("FOUT : antwoord afgekapt (finish_reason="); Serial.print(reden);
    Serial.println("), vraag iets eenvoudigers of verhoog max_tokens");
    return String();
  }

  String code = content;
  code.trim();

  // Reasoning-modellen (qwen3.6, deepseek-r1, ...) zetten hun gedachtegang
  // in een <think>...</think>-blok voor de code. Dat moet eraf, anders voert
  // MicroPython de redenering uit. Ook een niet-afgesloten blok afvangen:
  // dan is er geen bruikbare code en is een lege String het juiste antwoord.
  int think = code.indexOf("<think>");
  if (think >= 0) {
    int eind = code.indexOf("</think>", think);
    if (eind < 0) {
      Serial.println("FOUT : <think> zonder afsluiting - antwoord afgekapt?");
      return String();
    }
    code = code.substring(0, think) + code.substring(eind + 8);
    code.trim();
  }

  // Imports bestaan niet in deze sandbox; de API is globaal en time/random
  // komen uit de prelude. Het model schrijft ze toch af en toe, en dan zou
  // het script meteen op een ImportError sneuvelen. Dus eruit.
  if (code.indexOf("\nimport ") >= 0 || code.startsWith("import ") ||
      code.indexOf("\nfrom ") >= 0 || code.startsWith("from ")) {
    String schoon;
    schoon.reserve(code.length());
    int i = 0, weg = 0;
    while (i < (int)code.length()) {
      int eol = code.indexOf('\n', i);
      if (eol < 0) eol = code.length();
      String regel = code.substring(i, eol);
      String kop = regel; kop.trim();
      if (kop.startsWith("import ") || kop.startsWith("from ")) weg++;
      else { schoon += regel; schoon += '\n'; }
      i = eol + 1;
    }
    Serial.print("llm : "); Serial.print(weg);
    Serial.println(" import-regel(s) verwijderd (de API is globaal)");
    code = schoon;
    code.trim();
  }

  // Mocht het model toch fences meesturen: eerste regel en slotfence eraf.
  if (code.startsWith("```")) {
    int nl = code.indexOf('\n');
    code = (nl >= 0) ? code.substring(nl + 1) : String();
    int fence = code.lastIndexOf("```");
    if (fence >= 0) code = code.substring(0, fence);
    code.trim();
  }
  return code;
}

// ---------------------------------------------------------------- LAN-streamtest ('T')
// Chunked stream zonder TLS naar een luisterende host op het LAN: scheidt
// wifi-throughputproblemen van de TLS/Groq-route. Debughulp.

static void lanStreamTest(const char* host, uint16_t poort) {
  NetworkClient c;
  Serial.print("lantest: verbinden met "); Serial.print(host);
  Serial.print(":"); Serial.print(poort); Serial.print("... ");
  if (!c.connect(host, poort)) {
    Serial.println("MISLUKT (bereikbaar? client-isolation?)");
    return;
  }
  Serial.println("ok");
  c.setTimeout(15000);
  c.print("POST /test HTTP/1.1\r\nHost: lan\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");

  ringTail = ringHead;
  ringDrops = 0;
  captureOn = true;
  static int16_t out[480];
  uint32_t sent = 0;
  const uint32_t t0 = millis();
  bool ok = true;
  while (ok && millis() - t0 < 10000) {
    uint32_t n = captureRead(out, 480);
    if (n > 0) {
      char hdr[12];
      int h = snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned)(n * 2));
      ok = c.write((const uint8_t*)hdr, h) == (size_t)h &&
           c.write((const uint8_t*)out, n * 2) == n * 2 &&
           c.write((const uint8_t*)"\r\n", 2) == 2;
      sent += n * 2;
    } else {
      delay(4);
    }
  }
  captureOn = false;
  if (ok) c.print("0\r\n\r\n");
  Serial.print("lantest: "); Serial.print(sent); Serial.print(" bytes in ");
  Serial.print(millis() - t0); Serial.print(" ms, drops="); Serial.print(ringDrops);
  Serial.println(ok ? "" : " - AFGEBROKEN");
  c.stop();
}

// ---------------------------------------------------------------- spraak -> code -> uitvoeren

// fixedSeconds > 0: vaste opnameduur ('t'); anders tot de knop losgelaten wordt.
static void voiceCommandFlow(float fixedSeconds) {
  String text = transcribeStream(fixedSeconds);
  text.trim();
  if (text.length() == 0) {
    Serial.println("-> geen transcriptie, gestopt");
    return;
  }
  Serial.println();
  Serial.print("GEHOORD: \""); Serial.print(text); Serial.println("\"");

  // Bij stilte geeft Whisper "***" of "." terug - dan niets genereren.
  bool verstaanbaar = false;
  for (size_t i = 0; i < text.length(); i++) {
    if (isAlphaNumeric(text[i])) { verstaanbaar = true; break; }
  }
  if (!verstaanbaar) {
    Serial.println("-> niets verstaanbaars, geen code gegenereerd");
    return;
  }

  String code = llmGenerateCode(text);
  if (code.length() == 0) {
    Serial.println("-> geen code ontvangen, gestopt");
    return;
  }

  Serial.println();
  Serial.println("----- MicroPython ------------------------------");
  Serial.println(code);
  Serial.println("----- uitvoer (knop of 'x' = stoppen) ----------");
  uint32_t t0 = millis();
  embed_run(code.c_str(), 0);          // 0 = geen tijdslimiet
  Serial.print("----- gestopt na "); Serial.print(millis() - t0);
  Serial.println(" ms --------------------");
  Serial.println();
}

// Bewust een oneindige lus: controleert dat de VM-hook zo'n script na het
// budget afbreekt met een KeyboardInterrupt in plaats van het bord te hangen.
static const char GUARD_PY[] =
  "print('guard-test: while True, budget 3 s')\n"
  "while True:\n"
  "    matrix_fill(30, 0, 0)\n"
  "    matrix_show()\n"
  "    sleep_ms(50)\n"
  "    matrix_clear()\n"
  "    matrix_show()\n"
  "    sleep_ms(50)\n";

// Geheugentests: uitputten van de 48 kB GC-heap en oneindige recursie.
// Beide horen een nette Python-exception te geven (MemoryError /
// RecursionError) waarna het bord gewoon doordraait - het LLM kan immers
// van alles genereren.
static const char MEM_PY[] =
  "print('geheugentest: lijst laten groeien tot de heap op is')\n"
  "try:\n"
  "    blokken = []\n"
  "    while True:\n"
  "        blokken.append(bytearray(1024))\n"
  "except MemoryError:\n"
  "    print('MemoryError netjes gevangen na', len(blokken), 'kB')\n"
  "blokken = None\n"
  "print('en de interpreter leeft nog')\n";

static const char RECUR_PY[] =
  "print('recursietest')\n"
  "def diep(n):\n"
  "    return diep(n + 1)\n"
  "try:\n"
  "    diep(0)\n"
  "except RuntimeError as e:\n"
  "    print('RecursionError netjes gevangen:', e)\n"
  "print('en de interpreter leeft nog')\n";

// Vast testscript om interpreter en matrix los van het LLM te controleren:
// twee diagonalen die pixel voor pixel over het scherm lopen.
static const char DEMO_PY[] =
  "print('MicroPython draait; millis =', millis())\n"
  "matrix_brightness(40)\n"
  "for naam in ('heart', 'smiley', 'star'):\n"
  "    matrix_clear()\n"
  "    matrix_icon(naam, 90, 30, 0)\n"
  "    matrix_show()\n"
  "    sleep_ms(600)\n"
  "matrix_clear()\n"
  "matrix_char('A', 0, 90, 0)\n"
  "matrix_show()\n"
  "sleep_ms(600)\n"
  "matrix_text('OK', 0, 60, 90)\n"
  "for i in range(8):\n"
  "    matrix_clear()\n"
  "    matrix_set(i, i, 0, 90, 0)\n"
  "    matrix_set(7 - i, i, 0, 0, 90)\n"
  "    matrix_show()\n"
  "    sleep_ms(120)\n"
  "matrix_clear()\n"
  "matrix_show()\n"
  "print('demo klaar')\n";

// ---------------------------------------------------------------- opname ('w')

static void recordAudio() {
  const uint32_t target = (uint32_t)(RECORD_SECONDS * sampleRate);
  static int16_t buf[480];

  Serial.println();
  Serial.print("#WAV-BEGIN rate="); Serial.print(sampleRate);
  Serial.print(" bits=16 ch=1 gain="); Serial.print(micShift);
  Serial.print(" samples="); Serial.println(target);

  b64Reset();
  ringTail  = ringHead;
  ringDrops = 0;
  captureOn = true;

  uint32_t written = 0;
  const uint32_t deadline = millis() + (uint32_t)(RECORD_SECONDS * 1000) + 4000;

  while (written < target && millis() < deadline) {
    uint32_t n = captureRead(buf, min((uint32_t)480, target - written));
    if (n == 0) {
      delay(4);
      continue;
    }
    b64Write((const uint8_t*)buf, n * sizeof(int16_t));
    written += n;
  }
  captureOn = false;
  b64Flush();

  Serial.print("#WAV-END samples="); Serial.print(written);
  Serial.print(" drops="); Serial.println(ringDrops);
  Serial.println();
  statsReset();
}

// ---------------------------------------------------------------- uitvoer

static void printBar(float dbfs) {
  int filled = (int)((dbfs + 60.0f) / 60.0f * 20.0f + 0.5f);
  if (filled < 0)  filled = 0;
  if (filled > 20) filled = 20;
  Serial.print('[');
  for (int i = 0; i < 20; i++) Serial.print(i < filled ? '#' : ' ');
  Serial.print(']');
}

static void printInfo() {
  Serial.println();
  Serial.println("=== ESP32-C3 + INMP441 + 8x8 WS2812B - zelftest ===");
  Serial.print("  Sketch      : "); Serial.print(__DATE__); Serial.print(' '); Serial.println(__TIME__);
  Serial.print("  Chip        : "); Serial.print(ESP.getChipModel());
  Serial.print(" rev "); Serial.print(ESP.getChipRevision());
  Serial.print(", "); Serial.print(getCpuFrequencyMhz()); Serial.println(" MHz");
  Serial.print("  Uptime      : "); Serial.print(millis() / 1000); Serial.println(" s");
  Serial.print("  Vrij heap   : "); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");

  Serial.print("  I2S pinnen  : BCLK=GPIO"); Serial.print(micBclkPin);
  Serial.print("  WS=GPIO");   Serial.print(PIN_I2S_WS);
  Serial.print("  DIN=GPIO");  Serial.println(PIN_I2S_DIN);
  Serial.print("  Matrix      : 8x8 WS2812B op GPIO"); Serial.print(PIN_MATRIX);
  Serial.print(", "); Serial.print(MATRIX_SERPENTINE ? "serpentine" : "row-major");
  Serial.print(", helderheid "); Serial.println(MATRIX_BRIGHTNESS_DEFAULT);
  Serial.print("  Knop        : GPIO"); Serial.print(PIN_BUTTON);
  Serial.print(" (pull-up), nu "); Serial.println(buttonDown() ? "INGEDRUKT" : "los");

  Serial.print("  Microfoon   : ");
  Serial.print(micOk ? "I2S actief" : "I2S NIET gestart");
  Serial.print(", "); Serial.print(sampleRate); Serial.print(" Hz mono 32-bit, slot=");
  Serial.print(slotMask == I2S_STD_SLOT_LEFT ? "LEFT" : "RIGHT");
  Serial.print(", readLen="); Serial.print(readLen);
  Serial.print(", shift="); Serial.print(micShift);
  Serial.print(" ("); Serial.print((8 - micShift) * 6); Serial.println(" dB)");
  Serial.print("                samples="); Serial.print(totalSamples);
  Serial.print(", stalls="); Serial.print(i2sStalls);
  Serial.print(", restarts="); Serial.println(micRestarts);

  Serial.print("  Wifi        : ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(WiFi.SSID()); Serial.print(", ip="); Serial.print(WiFi.localIP());
    Serial.print(", rssi="); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  } else if (wifiApMode) {
    Serial.print("AP-modus \""); Serial.print(WIFI_AP_SSID);
    Serial.print("\", ip="); Serial.print(WiFi.softAPIP());
    Serial.println(" (geen internet; 'W' zoekt opnieuw)");
  } else {
    Serial.println("niet verbonden (druk 'W')");
  }
  Serial.print("  Whisper     : "); Serial.print(GROQ_MODEL);
  Serial.print(", taal="); Serial.print(strlen(GROQ_LANGUAGE) ? GROQ_LANGUAGE : "auto");
  Serial.println(", opname naar flash, daarna upload met retry");
  Serial.print("  LLM         : "); Serial.println(GROQ_LLM_MODEL);

  Serial.println("  Commando's  : h=help i=info m=monitor r=reset +/-=gain s=slot");
  Serial.println("                c=readLen f=rate d=dump w=opname t=spraak p=demo W=wifi");
  Serial.println();
}

static void printHelp() {
  Serial.println();
  Serial.println("  h  deze help");
  Serial.println("  i  info / zelftest");
  Serial.println("  m  monitor aan/uit");
  Serial.println("  r  statistieken resetten");
  Serial.println("  +  gain +6 dB");
  Serial.println("  -  gain -6 dB");
  Serial.println("  s  slot LEFT/RIGHT wisselen");
  Serial.println("  c  readLen wisselen (240/480/120 glitchvrij, 256/64 niet)");
  Serial.println("  f  sample rate wisselen (8/16/32/48 kHz)");
  Serial.println("  d  8 ruwe 32-bits samples in hex");
  Serial.println("  w  3 s audio opnemen -> base64 dump");
  Serial.println("  t  spraakflow met vaste 5 s opname (testen zonder knop)");
  Serial.println("  p  vast MicroPython-demoscript draaien");
  Serial.println("  P  guard-test: oneindige lus, moet na 3 s afbreken");
  Serial.println("  M  geheugentest: MemoryError + RecursionError opvangen");
  Serial.println("  x  lopend Python-script afbreken (de knop doet dat ook)");
  Serial.println("  W  wifi opnieuw verbinden (ook vanuit AP-modus)");
  Serial.println("  S  wifi-netwerken scannen");
  Serial.println("  D  wifi verbreken (test van indicator + zelfherstel)");
  Serial.println();
}

// Ruwe samples via micTask opvragen; bedradingscheck:
//   allemaal 0x00000000 -> geen data: SD-draad, voeding of L/R nakijken
//   allemaal hetzelfde  -> klok loopt wel, mic praat niet mee
static void dumpRaw() {
  rawDumpReq = true;
  const uint32_t t0 = millis();
  while (rawDumpReq && millis() - t0 < 1000) delay(5);
  Serial.println();
  if (rawDumpReq) {
    rawDumpReq = false;
    Serial.println("  geen data uit de mic-task (I2S down?)");
    return;
  }
  Serial.println("  ruwe 32-bits samples:");
  for (int i = 0; i < 8; i++) {
    Serial.print("    0x");
    for (int b = 28; b >= 0; b -= 4) {
      Serial.print("0123456789ABCDEF"[(rawDumpBuf[i] >> b) & 0xF]);
    }
    Serial.print("  24-bit="); Serial.println(rawDumpBuf[i] >> 8);
  }
  Serial.println();
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  Serial.begin(SERIAL_BAUD);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1200) { delay(10); }

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledcAttach(PIN_LED, 5000, 8);
  statusLed(false);

  matrix.begin();
  matrix.setBrightness(MATRIX_BRIGHTNESS_DEFAULT);
  // Korte testflits zodat je bij het opstarten ziet dat de matrix werkt:
  // rood-groen-blauw in de linkerbovenhoek plus wit in het midden.
  matrix.clear();
  matrix.setPixelColor(matrixIndex(0, 0), 80, 0, 0);
  matrix.setPixelColor(matrixIndex(1, 0), 0, 80, 0);
  matrix.setPixelColor(matrixIndex(2, 0), 0, 0, 80);
  matrix.setPixelColor(matrixIndex(3, 3), 60, 60, 60);
  matrix.show();
  delay(350);
  matrixClearShow();

  // Mic op een eigen task, prioriteit boven de loop-task (1), zodat de
  // I2S-DMA ook tijdens TLS-werk en Python-scripts leeggelezen wordt.
  xTaskCreate(micTaskFn, "mic", 4096, NULL, 3, NULL);

  Serial.println();
  Serial.println("ESP32-C3 SuperMini - spraakgestuurde 8x8 matrix");
  Serial.println("Knop indrukken = luisteren; loslaten = code genereren en draaien.");

  settingsLoad();
  if (LittleFS.begin(true)) {   // opnamebuffer, zie recordToFile()
    Serial.print("flash: LittleFS ok, ");
    Serial.print((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
    Serial.println(" kB vrij voor opnames");
  } else {
    Serial.println("flash: LittleFS MISLUKT - opnemen gaat niet werken");
  }
  // De netwerkstack MOET vanuit de hoofdtask geinitialiseerd worden: doe je
  // dat vanuit de achtergrondtask, dan crasht het bord in een bootloop op
  // "assert failed: xQueueSemaphoreTake" (semafoor bestaat dan nog niet).
  // Alleen het trage begin()+wachten gaat naar de achtergrond.
  WiFi.mode(WIFI_STA);
  delay(50);
  wifiStartBackground();   // verbinden mag niet op de knop wachten
  printInfo();
}

void loop() {
  wifiIndicator();   // rood hoekpixel zolang er geen verbinding is

  // Portaal pas opstarten als er een interface is; portal.begin() voor de
  // verbinding gaf een race met de wifi-task.
  static bool portalGestart = false;
  if (!portalGestart && !wifiBusy && (WiFi.status() == WL_CONNECTED || wifiApMode)) {
    portalBegin();
    portalGestart = true;
    Serial.print("portaal: http://");
    Serial.println(wifiApMode ? WiFi.softAPIP() : WiFi.localIP());
  }
  if (portalGestart) portal.handleClient();

  // Zelfherstel: elke 30 s opnieuw zoeken zolang er geen verbinding is - dus
  // ook als een bestaande verbinding wegvalt (router herstart, buiten bereik),
  // niet alleen vanuit AP-modus. In AP-modus wachten we wel als er iemand op
  // het portaal zit, want een nieuwe poging onderbreekt dat.
  static uint32_t lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && !wifiBusy &&
      (!wifiApMode || WiFi.softAPgetStationNum() == 0) &&
      millis() - lastWifiRetry > 30000) {
    lastWifiRetry = millis();
    Serial.println("wifi: geen verbinding - opnieuw zoeken");
    wifiStartBackground();
  }

  // --- knop: script is al afgebroken door de VM-hook; nu de spraakflow
  if (buttonDown()) {
    delay(30);                        // debounce
    if (buttonDown()) {
      voiceCommandFlow(0.0f);         // 0 = opnemen tot de knop losgelaten wordt
    }
  }

  // --- seriele commando's
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'h': case '?': printHelp(); break;
      case 'i': printInfo(); break;
      case 'm':
        monitorOn = !monitorOn;
        Serial.println(monitorOn ? "monitor AAN" : "monitor UIT");
        break;
      case 'r':
        statsReset();
        i2sStalls = 0;
        Serial.println("statistieken gereset");
        break;
      case '+':
        micShift = max(SHIFT_MIN, micShift - 1);
        Serial.print("gain = "); Serial.print((8 - micShift) * 6); Serial.println(" dB");
        break;
      case '-':
        micShift = min(SHIFT_MAX, micShift + 1);
        Serial.print("gain = "); Serial.print((8 - micShift) * 6); Serial.println(" dB");
        break;
      case 's':
        slotMask = (slotMask == I2S_STD_SLOT_LEFT) ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
        micReconfig = true;
        Serial.print("slot = ");
        Serial.println(slotMask == I2S_STD_SLOT_LEFT ? "LEFT" : "RIGHT");
        break;
      case 'c': {
        // 240/480/120 zijn glitchvrij, 256 en 64 juist niet - zie DMA_FRAME_NUM
        static const int lens[] = {240, 480, 120, 256, 64};
        int idx = 0;
        for (int k = 0; k < 5; k++) if (lens[k] == readLen) idx = k;
        readLen = lens[(idx + 1) % 5];
        Serial.print("readLen = "); Serial.print(readLen);
        Serial.println((readLen % DMA_FRAME_NUM == 0 || DMA_FRAME_NUM % readLen == 0)
                       ? "  (glitchvrij)" : "  (LET OP: geeft spikes)");
        break;
      }
      case 'f': {
        static const uint32_t rates[] = {8000, 16000, 32000, 48000};
        int idx = 0;
        for (int k = 0; k < 4; k++) if (rates[k] == sampleRate) idx = k;
        sampleRate = rates[(idx + 1) % 4];
        micReconfig = true;
        Serial.print("sampleRate = "); Serial.println(sampleRate);
        break;
      }
      case 'd': dumpRaw(); break;
      case 'w': recordAudio(); break;
      case 't': voiceCommandFlow(TEST_SECONDS); break;
      case 'p':
        Serial.println("----- demo-script (knop of 'x' = afbreken) -----");
        embed_run(DEMO_PY, 0);
        Serial.println("----- demo klaar -----");
        break;
      case 'M': {
        Serial.println("----- geheugentest -----");
        uint32_t h0 = ESP.getFreeHeap();
        embed_run(MEM_PY, 15000);
        embed_run(RECUR_PY, 15000);
        Serial.print("heap voor="); Serial.print(h0);
        Serial.print(" na="); Serial.println(ESP.getFreeHeap());
        Serial.println("----- geheugentest klaar -----");
        break;
      }
      case 'P': {
        Serial.println("----- guard-test: oneindige lus -----");
        uint32_t t0 = millis();
        embed_run(GUARD_PY, 3000);
        matrixClearShow();
        Serial.print("----- afgebroken na "); Serial.print(millis() - t0);
        Serial.println(" ms -----");
        break;
      }
      case 'T': lanStreamTest(LAN_TEST_HOST, LAN_TEST_PORT); break;
      case 'A':
        recAnim = !recAnim;
        Serial.print("opname-animatie "); Serial.println(recAnim ? "AAN" : "UIT");
        break;
      case 'W': wifiStartBackground(); break;
      case 'D':   // test: verbinding verbreken (indicator + zelfherstel)
        Serial.println("wifi: verbinding verbroken (test)");
        WiFi.disconnect();
        break;
      case 'S': {
        Serial.println("wifi-scan...");
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();   // anders geeft een scan tijdens een connect-poging 0 resultaten
        delay(200);
        int n = WiFi.scanNetworks();
        for (int k = 0; k < n; k++) {
          Serial.print("  "); Serial.print(WiFi.SSID(k));
          Serial.print("  rssi="); Serial.print(WiFi.RSSI(k));
          Serial.print("  ch="); Serial.print(WiFi.channel(k));
          Serial.print("  "); Serial.println(WiFi.encryptionType(k) == WIFI_AUTH_OPEN ? "open" : "beveiligd");
        }
        if (n <= 0) Serial.println("  (geen netwerken gezien)");
        WiFi.scanDelete();
        break;
      }
      default: break;
    }
  }

  // --- monitorregel (statistieken worden door micTask bijgewerkt)
  static uint32_t lastPrint = 0;
  if (monitorOn && (millis() - lastPrint) >= MONITOR_PERIOD) {
    lastPrint = millis();

    float rms = 0, dbfs = -120.0f, dc = 0, db24 = -120.0f;
    uint32_t cnt = statCount;
    if (cnt > 0) {
      dc  = (float)statSum / (float)cnt;
      rms = sqrtf((float)((double)statSumSq / (double)cnt));
      if (rms > 0.5f) dbfs = 20.0f * log10f(rms / 32768.0f);
      if (statPeak24 > 0) db24 = 20.0f * log10f((float)statPeak24 / 8388608.0f);
    }

    Serial.print("MIC rms="); Serial.print(rms, 0);
    Serial.print(" peak=");   Serial.print(statPeak);
    Serial.print(" dc=");     Serial.print(dc, 0);
    Serial.print(" dBFS=");   Serial.print(dbfs, 1);
    Serial.print(' ');
    printBar(dbfs);
    Serial.print(" raw24peak="); Serial.print(statPeak24);
    Serial.print(" ("); Serial.print(db24, 1); Serial.print(" dBFS)");
    Serial.print(" n="); Serial.print(cnt);
    if (statClipped) { Serial.print(" clip="); Serial.print(statClipped); }
    if (i2sStalls)   { Serial.print(" stalls="); Serial.print(i2sStalls); }
    if (!micOk)      { Serial.print(" (I2S DOWN)"); }
    Serial.println();

    statsReset();
  }

  delay(2);
}
