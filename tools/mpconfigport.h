// Configuratie voor de MicroPython embed-port in EspMicTest.
// Wordt zowel bij het genereren (qstr's) als bij het compileren gebruikt;
// het bestand in de Arduino-library moet identiek zijn aan deze kopie.

#include <port/mpconfigport_common.h>

#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_PY_GC                           (1)
#define MICROPY_PY_BUILTINS_FLOAT               (1)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)

// setjmp-nlr: portabel, geen arch-specifieke assembly nodig op riscv32
#define MICROPY_NLR_SETJMP                      (1)

// Bescherming tegen door de LLM gegenereerde recursie en oneindige lussen
#define MICROPY_STACK_CHECK                     (1)
#define MICROPY_KBD_EXCEPTION                   (1)
#define MICROPY_ENABLE_SOURCE_LINE              (1)
#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_DETAILED)

// VM-hook: pollt periodiek embed_vm_hook(), die bij een verstreken deadline
// of een 'x' op de seriele poort een KeyboardInterrupt klaarzet.
#define MICROPY_VM_HOOK_COUNT (256)
#define MICROPY_VM_HOOK_INIT static uint32_t vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL \
    if (--vm_hook_divisor == 0) { \
        vm_hook_divisor = MICROPY_VM_HOOK_COUNT; \
        extern void embed_vm_hook(void); \
        embed_vm_hook(); \
    }
#define MICROPY_VM_HOOK_LOOP MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN MICROPY_VM_HOOK_POLL

// sys.platform-string (MICROPY_PY_SYS staat aan op CORE_FEATURES-niveau)
#define MICROPY_PY_SYS_PLATFORM                 "embed"

// Verwacht door modmicropython.c (kbd_intr) zodra MICROPY_KBD_EXCEPTION
// aanstaat; de embed-port declareert hem nergens. De sketch levert een no-op.
#ifndef __ASSEMBLER__
void mp_hal_set_interrupt_char(int c);
#endif

// Geen bestandssysteem in de embed-port: geen open() en geen imports van
// bestanden (ingebouwde modules zoals gc blijven gewoon werken).
#define MICROPY_PY_IO                           (0)
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (0)

// Grote gehele getallen. Zonder dit geeft elk getal boven 2^30 meteen een
// OverflowError ("long int not supported in this build") - en door het LLM
// gegenereerde code rekent daar niet op. MPZ = willekeurige precisie, net
// als CPython.
#define MICROPY_LONGINT_IMPL (MICROPY_LONGINT_IMPL_MPZ)
