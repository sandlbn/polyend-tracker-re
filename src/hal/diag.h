// OpenTracker -- device boot diagnostics over USB CDC, independent of the main
// loop (docs/HARDWARE_TESTING.md "Diagnostic log"):
//  * diagLog()/diagLogf() append timestamped lines to a RAM ring (diag_log.h);
//  * a low-priority software interrupt (same priority as the USB ISR, pended from
//    SysTick every 10 ms and after each log line) pushes pending bytes as raw CDC
//    packets (usb_malloc/usb_tx, never waits) while the host holds DTR;
//  * any byte received on the CDC port replays the boot log, except lines
//    starting with '@': [I] those are remote-control commands (core/remote),
//    read by the main loop with diagRemoteRead();
//  * SysTick watchdog: no stage change and no diagBeat() for 10 s -> "STUCK" line
//    with the PC of the interrupted context;
//  * HardFault/MemManage/BusFault/UsageFault: registers + SCB fault status +
//    stage + the last 40 log lines, re-sent every second while USB is serviced.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ot {
namespace hal {

// First call in main(): installs the fault, SysTick and drain vectors.
void diagInit();
void diagLog(const char* text);
void diagLogf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Starts a numbered bring-up stage (logged; reported by the watchdog and faults).
void diagStage(const char* name);
uint32_t diagStageNumber();
// Main-loop heartbeat for the stuck detector.
void diagBeat();
// Enables/disables the 10 s stuck detector (on after diagInit()).
void diagWatchdog(bool on);
// True while a host terminal holds the CDC port open (configured + DTR), i.e.
// while log lines are actually being sent (app telemetry gates on this).
bool diagTerminalOpen();
// Stop copying into the replay buffer (after bring-up; the ring keeps streaming).
void diagFreezeBoot();
// [I] Received remote-control bytes ('@' lines, line ends included); returns
// the count copied (0 = none). Main loop only.
size_t diagRemoteRead(char* out, size_t cap);
// [I] The receive ring for those lines (SDRAM, handed over once the SDRAM is
// up; the SRAM stack headroom has no room for it). Until then they are dropped.
void diagRemoteBuffer(char* buf, uint32_t size);
// [I] Bytes the log ring can take before unsent data is overwritten.
uint32_t diagTxFree();

}  // namespace hal
}  // namespace ot
