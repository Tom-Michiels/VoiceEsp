/*
 * embed_api.c - brug tussen de MicroPython embed-port en de Arduino-sketch.
 *
 * Dit bestand is bewust C (geen C++): de MicroPython-headers zijn C-headers.
 * De hardwarekant (hal_*) is geimplementeerd in EspMicTest.ino als extern "C".
 *
 * Python-API die hier geregistreerd wordt (als globals in __main__):
 *   matrix_set(x, y, r, g, b)   pixel in de buffer zetten (x,y 0..7; 0..255)
 *   matrix_fill(r, g, b)        hele buffer een kleur geven
 *   matrix_clear()              buffer zwart
 *   matrix_show()               buffer naar de leds sturen
 *   matrix_brightness(n)        globale helderheid 0..255
 *   sleep_ms(ms) / millis()
 *
 * De 8x8 WS2812-matrix is de enige uitvoer; de losse-LED-API is vervallen.
 *
 * Elke embed_run() krijgt een verse interpreter (init -> exec -> deinit),
 * zodat een vorig script geen rommel achterlaat in de globals.
 *
 * Afbreken: scripts mogen oneindig lopen (budget_ms = 0). embed_vm_hook()
 * wordt door de VM periodiek gepollt (MICROPY_VM_HOOK_* in mpconfigport.h)
 * en zet een KeyboardInterrupt klaar zodra de knop ingedrukt wordt, er een
 * 'x' op de seriele poort staat, of een eventueel budget verstreken is.
 * sleep_ms() controleert hetzelfde, zodat ook een script dat vooral slaapt
 * meteen reageert.
 */

#include <stdint.h>
#include <string.h>

#include <micropython_embed.h>   // wrapper in de MicroPythonEmbed-library
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/obj.h"

// ---- geleverd door de sketch (EspMicTest.ino, extern "C") ----
extern void     hal_matrix_set(int x, int y, uint8_t r, uint8_t g, uint8_t b);
extern void     hal_matrix_fill(uint8_t r, uint8_t g, uint8_t b);
extern void     hal_matrix_show(void);
extern void     hal_matrix_brightness(uint8_t n);
extern void     hal_matrix_bitmap(const uint8_t rows[8], uint8_t r, uint8_t g, uint8_t b);
extern void     hal_matrix_char(char c, uint8_t r, uint8_t g, uint8_t b);
extern int      hal_matrix_icon(const char *name, uint8_t r, uint8_t g, uint8_t b);
extern void     hal_matrix_text(const char *s, uint8_t r, uint8_t g, uint8_t b);
extern uint32_t hal_millis(void);
extern void     hal_delay_ms(uint32_t ms);
extern int      hal_abort_requested(void);   // knop ingedrukt of 'x' getypt
extern void     hal_serial_write(const char *s, size_t len);

// ---- MicroPython GC-heap ----
// Statisch (in .bss), zodat hij nooit concurreert met de Arduino-heap die de
// TLS-handshake nodig heeft. Elke kB hier is een kB minder voor TLS; op de C3
// is 64 kB gemeten het maximum met marge (bij 80 kB faalt de handshake met
// "SSL - Memory allocation failed"). Zie README voor de meetreeks.
//
// Op een ESP32-S3 met PSRAM vervalt die afweging: vervang de regel hieronder
// door een runtime-allocatie in PSRAM, dan mag de Python-heap megabytes zijn
// zonder dat het intern RAM voor TLS/wifi aantast:
//
//     static char *mp_heap; static const size_t MP_HEAP_LEN = 4*1024*1024;
//     ... in embed_run(): if (!mp_heap) mp_heap = (char*)ps_malloc(MP_HEAP_LEN);
//
// (Arduino-ESP32 3.3.0 heeft ps_malloc(); zet PSRAM aan in de FQBN.)
static char mp_heap[64 * 1024];

static int      embed_active   = 0;
static uint32_t embed_deadline = 0;   // 0 = geen tijdslimiet

// stdout van MicroPython (print() etc.) naar de USB-seriele poort.
// Vervangt port/mphalport.c, dat naar printf/UART0 zou schrijven.
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) {
    hal_serial_write(str, len);
}

// Vereist door micropython.kbd_intr(); wij onderbreken via de VM-hook
// hieronder, niet via een interrupt-teken, dus dit is een no-op.
void mp_hal_set_interrupt_char(int c) {
    (void)c;
}

// Door de VM gepollt via MICROPY_VM_HOOK_* (zie mpconfigport.h).
void embed_vm_hook(void) {
    if (!embed_active) {
        return;
    }
    if (hal_abort_requested() ||
        (embed_deadline != 0 && (int32_t)(hal_millis() - embed_deadline) >= 0)) {
        mp_sched_keyboard_interrupt();
    }
}

// ---- de Python-functies ----

static uint8_t clamp_u8(mp_int_t v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// matrix_set(x, y, r, g, b) - buiten 0..7 wordt stil genegeerd, zodat
// animatiecode met randfouten geen exception regent.
static mp_obj_t py_matrix_set(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    mp_int_t x = mp_obj_get_int(args[0]);
    mp_int_t y = mp_obj_get_int(args[1]);
    if (x < 0 || x > 7 || y < 0 || y > 7) {
        return mp_const_none;
    }
    hal_matrix_set((int)x, (int)y,
                   clamp_u8(mp_obj_get_int(args[2])),
                   clamp_u8(mp_obj_get_int(args[3])),
                   clamp_u8(mp_obj_get_int(args[4])));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_matrix_set_obj, 5, 5, py_matrix_set);

static mp_obj_t py_matrix_fill(mp_obj_t r_in, mp_obj_t g_in, mp_obj_t b_in) {
    hal_matrix_fill(clamp_u8(mp_obj_get_int(r_in)),
                    clamp_u8(mp_obj_get_int(g_in)),
                    clamp_u8(mp_obj_get_int(b_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(py_matrix_fill_obj, py_matrix_fill);

static mp_obj_t py_matrix_clear(void) {
    hal_matrix_fill(0, 0, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_matrix_clear_obj, py_matrix_clear);

static mp_obj_t py_matrix_show(void) {
    hal_matrix_show();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_matrix_show_obj, py_matrix_show);

static mp_obj_t py_matrix_brightness(mp_obj_t n_in) {
    hal_matrix_brightness(clamp_u8(mp_obj_get_int(n_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_matrix_brightness_obj, py_matrix_brightness);

// matrix_icon(naam, r, g, b) - kant-en-klaar symbool in de buffer
static mp_obj_t py_matrix_icon(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    hal_matrix_icon(mp_obj_str_get_str(args[0]),
                    clamp_u8(mp_obj_get_int(args[1])),
                    clamp_u8(mp_obj_get_int(args[2])),
                    clamp_u8(mp_obj_get_int(args[3])));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_matrix_icon_obj, 4, 4, py_matrix_icon);

// matrix_char(teken, r, g, b) - een 5x7-teken, gecentreerd
static mp_obj_t py_matrix_char(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    const char *s = mp_obj_str_get_str(args[0]);
    hal_matrix_char(s[0] ? s[0] : '?',
                    clamp_u8(mp_obj_get_int(args[1])),
                    clamp_u8(mp_obj_get_int(args[2])),
                    clamp_u8(mp_obj_get_int(args[3])));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_matrix_char_obj, 4, 4, py_matrix_char);

// matrix_text(tekst, r, g, b) - scrollt de tekst en showt zelf (blokkeert)
static mp_obj_t py_matrix_text(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    hal_matrix_text(mp_obj_str_get_str(args[0]),
                    clamp_u8(mp_obj_get_int(args[1])),
                    clamp_u8(mp_obj_get_int(args[2])),
                    clamp_u8(mp_obj_get_int(args[3])));
    mp_handle_pending(true);   // knop tijdens de scroll -> KeyboardInterrupt nu
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_matrix_text_obj, 4, 4, py_matrix_text);

// matrix_bitmap(rijen, r, g, b) - 8 ints, bit 7 = linkerkolom
static mp_obj_t py_matrix_bitmap(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(args[0], &len, &items);
    uint8_t rows[8] = {0};
    for (size_t i = 0; i < 8 && i < len; i++) {
        mp_int_t v = mp_obj_get_int(items[i]);
        rows[i] = (uint8_t)(v & 0xFF);
    }
    hal_matrix_bitmap(rows,
                      clamp_u8(mp_obj_get_int(args[1])),
                      clamp_u8(mp_obj_get_int(args[2])),
                      clamp_u8(mp_obj_get_int(args[3])));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(py_matrix_bitmap_obj, 4, 4, py_matrix_bitmap);

static mp_obj_t py_sleep_ms(mp_obj_t ms_in) {
    mp_int_t ms = mp_obj_get_int(ms_in);
    if (ms < 0) ms = 0;
    const uint32_t end = hal_millis() + (uint32_t)ms;
    while ((int32_t)(end - hal_millis()) > 0) {
        hal_delay_ms(5);
        embed_vm_hook();
        mp_handle_pending(true);   // gooit de KeyboardInterrupt meteen
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_sleep_ms_obj, py_sleep_ms);

static mp_obj_t py_millis(void) {
    return mp_obj_new_int_from_uint(hal_millis());
}
static MP_DEFINE_CONST_FUN_OBJ_0(py_millis_obj, py_millis);

static void embed_register_api(void) {
    mp_store_global(qstr_from_str("matrix_set"),        MP_OBJ_FROM_PTR(&py_matrix_set_obj));
    mp_store_global(qstr_from_str("matrix_fill"),       MP_OBJ_FROM_PTR(&py_matrix_fill_obj));
    mp_store_global(qstr_from_str("matrix_clear"),      MP_OBJ_FROM_PTR(&py_matrix_clear_obj));
    mp_store_global(qstr_from_str("matrix_show"),       MP_OBJ_FROM_PTR(&py_matrix_show_obj));
    mp_store_global(qstr_from_str("matrix_brightness"), MP_OBJ_FROM_PTR(&py_matrix_brightness_obj));
    mp_store_global(qstr_from_str("matrix_icon"),       MP_OBJ_FROM_PTR(&py_matrix_icon_obj));
    mp_store_global(qstr_from_str("matrix_char"),       MP_OBJ_FROM_PTR(&py_matrix_char_obj));
    mp_store_global(qstr_from_str("matrix_text"),       MP_OBJ_FROM_PTR(&py_matrix_text_obj));
    mp_store_global(qstr_from_str("matrix_bitmap"),     MP_OBJ_FROM_PTR(&py_matrix_bitmap_obj));
    mp_store_global(qstr_from_str("sleep_ms"),          MP_OBJ_FROM_PTR(&py_sleep_ms_obj));
    mp_store_global(qstr_from_str("millis"),            MP_OBJ_FROM_PTR(&py_millis_obj));
}

// ---- prelude ----
// Het LLM grijpt uit gewoonte naar "time" en "random", hoe vaak de prompt ook
// zegt dat imports verboden zijn. In plaats van daartegen te vechten draaien
// we deze shims voor elk script. Ze lopen als aparte exec_str in dezelfde
// interpreter, zodat de regelnummers in tracebacks van de gebruikerscode
// blijven kloppen. random is een simpele LCG - genoeg voor animaties.
static const char PRELUDE[] =
    "class _T:\n"
    "    def sleep(self, s): sleep_ms(int(s * 1000))\n"
    "    def sleep_ms(self, m): sleep_ms(int(m))\n"
    "    def sleep_us(self, u): sleep_ms(int(u) // 1000)\n"
    "    def ticks_ms(self): return millis()\n"
    "    def ticks_diff(self, a, b): return a - b\n"
    "    def time(self): return millis() // 1000\n"
    "time = _T()\n"
    "utime = time\n"
    "class _R:\n"
    "    def __init__(self): self.s = (millis() * 2654435761) & 0x3FFFFFFF | 1\n"
    "    def _n(self):\n"
    "        self.s = (self.s * 1103515245 + 12345) & 0x3FFFFFFF\n"
    "        return self.s\n"
    "    def seed(self, x=None): self.s = ((int(x) if x else millis()) & 0x3FFFFFFF) | 1\n"
    "    def randint(self, a, b): return a + self._n() % (b - a + 1)\n"
    "    def randrange(self, a, b=None):\n"
    "        if b is None: a, b = 0, a\n"
    "        return a + self._n() % (b - a)\n"
    "    def choice(self, q): return q[self._n() % len(q)]\n"
    "    def random(self): return self._n() / 1073741824.0\n"
    "    def shuffle(self, q):\n"
    "        for i in range(len(q) - 1, 0, -1):\n"
    "            j = self._n() % (i + 1)\n"
    "            q[i], q[j] = q[j], q[i]\n"
    "random = _R()\n"
    "randint = random.randint\n"
    "randrange = random.randrange\n"
    "choice = random.choice\n"
    "shuffle = random.shuffle\n"
    "sleep = time.sleep\n"
    "ticks_ms = millis\n";

// ---- publieke ingang voor de sketch ----

// budget_ms = 0: geen tijdslimiet; het script stopt via de knop of 'x'.
void embed_run(const char *code, uint32_t budget_ms) {
    int stack_top;
    mp_embed_init(&mp_heap[0], sizeof(mp_heap), &stack_top);
    // De loop-task-stack is 20 kB (SET_LOOP_TASK_STACK_SIZE in de sketch);
    // ruim daaronder blijven zodat diepe recursie een RecursionError geeft
    // in plaats van een gecrashte FreeRTOS-task.
    mp_stack_set_limit(12 * 1024);
    embed_register_api();
    mp_embed_exec_str(PRELUDE);   // time/random-shims, zie boven

    embed_deadline = (budget_ms == 0) ? 0 : hal_millis() + budget_ms;
    embed_active = 1;

    mp_embed_exec_str(code);

    embed_active = 0;
    embed_deadline = 0;
    mp_embed_deinit();
}
