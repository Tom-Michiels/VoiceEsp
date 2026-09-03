#pragma once

// Kopieer dit bestand naar secrets.h en vul je eigen gegevens in.
// secrets.h staat in .gitignore en hoort niet in de repo.

// Wifi-netwerken waar de ESP32 op probeert in te loggen, in volgorde.
// De ESP32-C3 kan alleen 2,4 GHz. Lukt geen van beide, dan start het bord
// een eigen access point (zie WIFI_AP_*).
// Volgorde = voorkeur; zet een reserve-hotspot achteraan.
#define WIFI_NET_COUNT 3
static const char* WIFI_SSIDS[WIFI_NET_COUNT]  = { "netwerk1", "netwerk2", "hotspot" };
static const char* WIFI_PASSES[WIFI_NET_COUNT] = { "wachtwoord1", "wachtwoord2", "wachtwoord3" };

// Fallback-AP als er geen bekend netwerk gevonden wordt. Let op: zonder
// internet werkt de spraak->code-keten niet; het AP is er zodat het bord
// bereikbaar/zichtbaar blijft.
#define WIFI_AP_SSID "VoiceEsp"
#define WIFI_AP_PASS "kies-een-ap-wachtwoord"   // minimaal 8 tekens

// Groq API-key voor Whisper. Staat in platte tekst in secrets.h - deel dat
// bestand niet en zet het niet in een publieke repo. Aanmaken/intrekken kan op
// https://console.groq.com/keys
#define GROQ_API_KEY "gsk_..."

// Laat GROQ_LANGUAGE leeg om Whisper de taal te laten raden, of zet er een
// ISO-code in ("nl", "en") - dat scheelt merkbaar in nauwkeurigheid.
#define GROQ_MODEL    "whisper-large-v3-turbo"
#define GROQ_LANGUAGE "nl"

// Doel voor de LAN-streamtest (toets 'T' in de monitor): het adres van je eigen
// machine die daar met bv. `nc -l 8000` op luistert.
#define LAN_TEST_HOST "192.168.1.10"
#define LAN_TEST_PORT 8000

// Chat-model dat de transcriptie omzet in MicroPython-code.
// Vergeleken met dezelfde systeemprompt en zin ("teken een geel hartje"):
//   llama-3.3-70b-versatile : 8 losse pixels, een kruisje - onbruikbaar
//   qwen/qwen3.6-27b        : correcte code, maar 1308 tokens <think>-blok
//                             ervoor en ~4x trager (3,5 s vs 0,9 s)
//   openai/gpt-oss-120b     : correcte code, 175 tokens, geen <think>  <-- keuze
// De firmware strip <think>-blokken weg, dus een reasoning-model kan wel.
#define GROQ_LLM_MODEL "openai/gpt-oss-120b"
