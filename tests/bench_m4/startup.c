// Minimal Cortex-M4 startup for tests/audio_bench.cpp under QEMU mps2-an386
// (semihosting I/O via librdimon).
#include <stdint.h>
#include <string.h>

extern uint32_t __data_load, __data_start, __data_end, __bss_start__, __bss_end__, __stack_top;
extern void __libc_init_array(void);
extern void initialise_monitor_handles(void);
extern int main(void);
extern void exit(int);

void Reset_Handler(void) {
    // FPU on (CPACR CP10/CP11 full access) before any float code.
    *(volatile uint32_t*)0xE000ED88 |= (0xFu << 20);
    __asm volatile("dsb\n isb");
    memcpy(&__data_start, &__data_load, (uint32_t)((char*)&__data_end - (char*)&__data_start));
    memset(&__bss_start__, 0, (uint32_t)((char*)&__bss_end__ - (char*)&__bss_start__));
    initialise_monitor_handles();
    __libc_init_array();
    exit(main());
    for (;;) {}
}

// GCC 13 (Ubuntu arm-none-eabi 13.2) emits __cxa_atexit(…, &__dso_handle)
// for static objects; -nostartfiles drops crtbegin, which defines it.
void* __dso_handle = 0;

void _init(void) {}
void _fini(void) {}

static void Fault_Handler(void) {
    for (;;) {}
}

__attribute__((section(".vectors"), used)) const void* const gVectors[16] = {
    &__stack_top, (const void*)Reset_Handler, (const void*)Fault_Handler, (const void*)Fault_Handler,
    (const void*)Fault_Handler, (const void*)Fault_Handler, (const void*)Fault_Handler, 0, 0, 0, 0,
    (const void*)Fault_Handler, (const void*)Fault_Handler, 0, (const void*)Fault_Handler,
    (const void*)Fault_Handler,
};
