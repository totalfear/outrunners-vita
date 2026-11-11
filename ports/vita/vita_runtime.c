/* Name:        vita_runtime.c
 * Description: what the PlayStation Vita build sets before main: the heap, the threads' stacks, the clocks
 * Status:      first draft: the sizes are guesses, to be tuned on a Vita (the size of the assets pack decides
 *              the heap; the video's workers and the game's tasks decide the stacks)
 */
#ifdef __vita__

#include <psp2/power.h>

/* read by VitaSDK's C library at start-up: the memory malloc gives out (the Vita gives an app about 256 MB) */
int _newlib_heap_size_user = 128 * 1024 * 1024;

/* read by VitaSDK's pthreads: the stack of every thread made without one of its own */
unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

/* The Vita starts an app at 333 MHz; the software video and the sound chips want all it has (444 MHz, as the
 * emulators on it set) */
__attribute__((constructor)) static void orunners_vita_clocks(void)
{
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);
}

#endif
