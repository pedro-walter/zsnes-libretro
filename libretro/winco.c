/* Windows coroutine primitives for the emulator thread (fibers). */
#ifdef _WIN32
#include <windows.h>
static LPVOID main_fib, emu_fib;
static void (*entry_fn)(void);
static VOID CALLBACK fiber_proc(LPVOID p) { (void)p; entry_fn(); }
int winco_init(void (*entry)(void), unsigned stack)
{
    if (!main_fib) { main_fib = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL); if (!main_fib) return 0; }
    if (emu_fib) DeleteFiber(emu_fib);
    entry_fn = entry;
    emu_fib = CreateFiber(stack, fiber_proc, NULL);
    return emu_fib != NULL;
}
void winco_free(void) { if (emu_fib) { DeleteFiber(emu_fib); emu_fib = NULL; } }
void winco_to_main(void) { SwitchToFiber(main_fib); }
void winco_to_emu(void) { SwitchToFiber(emu_fib); }
#endif
