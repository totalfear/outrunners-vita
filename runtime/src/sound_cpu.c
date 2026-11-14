/**
 *    Name:        sound_cpu.c
 *    Description: runs the decompiled sound program on the board's time: the Z80's timing model
 *    Version:     0.1.0
 *    Created:     13-10-2026
 *    Modified:    21-10-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    The sound program is C (src/sound/, tools/decompile_sound.py) and keeps the Z80's time itself: every
 *    basic block of the original starts with z80_block(cycles). The rules, the same tools/core/system32/reference/sound
 *    keeps for MAME's Z80 so that the two compare sample for sample:
 *      - the Z80's time counts in its own cycles and moves on by each block's cycles (tools/z80/timing.py),
 *        the extra cycles of a condition's longer way, and 21 for every round of a block move but its
 *        last; the board's time is 2.5 board cycles a Z80 cycle (20 MHz against 8), rounded down;
 *      - when a block starts, the YM3438 timers due by then run out, and an interrupt is taken if the
 *        line is up and the Z80 lets interrupts in (not right after an EI that ended the block before):
 *        13 cycles and one count of the refresh register, then sound_irq(); after it returns the same is
 *        looked at again;
 *      - the refresh register counts the opcode fetches: every block's (tools/z80/timing.py fetches), two
 *        for every round of a block move but its last, and one for an interrupt; it starts at 0;
 *      - everything a block reads and writes happens at the time it starts;
 *      - the board runs the program whenever its time moves on (s32_board.time_passes): from its reset
 *        once the I/O chip lets the Z80 run, then the main loop again and again until the Z80's time is
 *        past the board's (the loop's top is where it waits, sound_symbols.txt's "loop" line); while the
 *        Z80 is held in reset, only the timers run.
 *
 *    The program starts some five thousand blocks a frame and a timer runs out 58 times, so a block's
 *    start does not ask the chips: z80_block() (sound/support.h) only counts while the Z80's time is
 *    before the next event, the first cycle at which a timer is out or an interrupt may be taken
 *    (next_event), and comes here at or past it (z80_block_event). Whatever can bring an event nearer
 *    sets it to now: EI, the interrupt line, a write to the YM3438, the board's time moving on. The time
 *    the chips take a block's accesses at (s32_board.sound_cycles) is set where the block reaches them.
 */
#include "sound/support.h"
#include "sound/chips.h"

#include <stdio.h>

#define INTERRUPT_CYCLES 13
#define NO_EVENT UINT64_MAX
#define LOOK_NOW 0          /* an event that every clock is past: the next block's start looks */

_Static_assert(S32_CPU_CLOCK * 2u == S32_SOUND_CLOCK * 5u, "board_time: the clocks are no longer 5/2");

/* the board's time when the Z80 has run 'cycles' */
static uint64_t board_time(uint64_t cycles)
{
	/* 20 MHz over 8 MHz is exactly 5/2, so this is a multiply and a shift and not a 64-bit divide (a
	   processor with no divide instruction, the Vita's, calls a function for one). Exact while cycles * 5
	   fits in 64 bits, which at the Z80's 133,333 cycles a frame is a few hundred thousand years. */
	return (cycles * 5u) >> 1;
}

/* the first Z80 cycle at or after the board's 'time' */
static uint64_t z80_time(uint64_t time)
{
	return (time * S32_SOUND_CLOCK + S32_CPU_CLOCK - 1) / S32_CPU_CLOCK;
}

/* the sound program's ways in (sound_symbols.txt) */
void sound_reset(void);
void sound_irq(void);
void sound_main_loop(void);

/* the Z80's time, where the program's blocks count it themselves (sound/support.h) */
sound_z80_time sound_z80 = { 0, LOOK_NOW, 0, 0, 0 };

static struct
{
	bool interrupts;        /* IFF1 */
	bool after_ei;          /* EI ended the block before */
	bool line;              /* the board's interrupt line to the Z80 */
	bool held;              /* in reset */
	bool running;           /* past the reset code: the main loop runs */
} z80 = { false, false, false, true, false };

/* The first cycle at which a block's start has something to do, for a block that starts now with the
   timers due run out and no interrupt taken: the next block's if an interrupt only waits for the Z80 to
   let it in after an EI; else the first whose board time a YM3438 timer runs out by, which nothing but
   the timers themselves and the program's writes to the chip move (port_out). An interrupt needs the
   line or the Z80's enable to change first, and both changes ask for a look (LOOK_NOW). */
static uint64_t next_event(void)
{
	const uint64_t deadline = sound_chips_next_timer();
	uint64_t ahead;

	if (z80.interrupts && z80.line)
		return LOOK_NOW;
	if (deadline == UINT64_MAX)
		return NO_EVENT;

	/* the timer is out at cycle c when deadline <= floor(c x 5 / 2), which is 2 x deadline <= 5 x c: in
	   fifths of a cycle from here, 32 bits unless a timer is hours away, and so with no 64-bit divide */
	if (2 * deadline <= 5 * sound_z80.clock)
		return LOOK_NOW;
	ahead = 2 * deadline - 5 * sound_z80.clock;
	if (ahead <= UINT32_MAX - 4)
		return sound_z80.clock + ((uint32_t)ahead + 4) / 5;
	return sound_z80.clock + (ahead + 4) / 5;
}

void z80_block_event(void)
{
	uint64_t now;

	for (;;)
	{
		now = board_time(sound_z80.clock);
		sound_chips_timers_due(now);
		if (!z80.interrupts || z80.after_ei || !z80.line)
			break;
		z80.interrupts = false;
		sound_z80.clock += INTERRUPT_CYCLES;
		sound_z80.refresh++;
		sound_irq();
	}
	z80.after_ei = false;
	sound_z80.event = next_event();
}

uint8_t refresh_read(int fetches)
{
	return (uint8_t)((sound_z80.block_refresh + (uint32_t)fetches) & 0x7f);
}

bool interrupts_enabled(void)
{
	return z80.interrupts;
}

void interrupts_on(void)
{
	z80.interrupts = true;
	sound_z80.event = LOOK_NOW;
}

void interrupts_on_after_block(void)
{
	z80.interrupts = true;
	z80.after_ei = true;
	sound_z80.event = LOOK_NOW;
}

void interrupts_off(void)
{
	z80.interrupts = false;
}

/* What the running block reads and writes happens when it started: the chips make their samples up to
   then before a write (s32_board.sound_cycles), so every way to them goes through here first */
static void at_block_start(void)
{
	s32_board.sound_cycles = board_time(sound_z80.block_clock);
}

uint8_t z80_read8(uint16_t address)
{
	at_block_start();
	return s32_z80_read(address);
}

uint16_t z80_read16(uint16_t address)
{
	uint8_t low;

	at_block_start();
	low = s32_z80_read(address);
	return (uint16_t)(low | s32_z80_read((uint16_t)(address + 1)) << 8);
}

void z80_write8(uint16_t address, uint8_t value)
{
	at_block_start();
	s32_z80_write(address, value);
}

void z80_write16(uint16_t address, uint16_t value)
{
	at_block_start();
	s32_z80_write(address, (uint8_t)value);
	s32_z80_write((uint16_t)(address + 1), (uint8_t)(value >> 8));
}

uint8_t port_in(uint8_t port)
{
	at_block_start();
	return s32_z80_in(port);
}

void port_out(uint8_t port, uint8_t value)
{
	at_block_start();
	/* the MultiPCM's samples up to now come from the banks as they were */
	if ((port & 0xf0) == 0xb0)
		sound_chips_pcm_banks_changing();
	s32_z80_out(port, value);
	/* a write to the YM3438 may have started, stopped or moved a timer */
	if ((port & 0xf0) == 0x80)
		sound_z80.event = LOOK_NOW;
}

void move_block(uint16_t source, uint16_t destination, uint16_t count, bool upward)
{
	const uint32_t rounds = count ? count : 0x10000;

	at_block_start();
	for (uint32_t round = 0; round < rounds; round++)
	{
		s32_z80_write(destination, s32_z80_read(source));
		source = (uint16_t)(upward ? source + 1 : source - 1);
		destination = (uint16_t)(upward ? destination + 1 : destination - 1);
	}
	sound_z80.clock += 21 * (uint64_t)(rounds - 1);
	sound_z80.refresh += 2 * (rounds - 1);
}

void sound_invalid_jump(uint32_t address)
{
	if (s32_board.log)
	{
		char message[64];
		snprintf(message, sizeof(message), "sound program: a jump to %04X, past its table", (unsigned)address);
		s32_board.log(message);
	}
}

/* ---- the board's side ---- */

static void on_time_passes(uint64_t until)
{
	/* whatever the board did since the last time (the timers below, a chip set up again): the next block
	   looks for itself */
	sound_z80.event = LOOK_NOW;
	if (z80.held)
	{
		sound_chips_timers_due(until);
		{
			const uint64_t start = z80_time(until);    /* (once: this one is still a divide) */

			if (sound_z80.clock < start)
				sound_z80.clock = start;
		}
		return;
	}
	if (!z80.running)
	{
		z80.running = true;
		sound_reset();
	}
	while (board_time(sound_z80.clock) < until)
		sound_main_loop();
}

static void on_sound_irq(bool asserted, uint8_t vector)
{
	(void)vector;    /* interrupt mode 1: always 0038 */
	z80.line = asserted;
	sound_z80.event = LOOK_NOW;
}

static void on_sound_reset(bool held)
{
	if (held)
	{
		z80.running = false;
		z80.interrupts = false;
		z80.after_ei = false;
		sound_z80.refresh = 0;
	}
	z80.held = held;
	sound_z80.event = LOOK_NOW;
}

/* Hooks the sound program to the board; s32_board.sound_rom and the chips have to be set */
void sound_cpu_attach(void)
{
	/* the tables the program reads by name, where the chip (or the assets) has them */
	for (size_t index = 0; index < sound_table_count; index++)
		*sound_tables[index].data = s32_sound_rom_at(sound_tables[index].address);
	sound_z80.clock = z80_time(s32_board.cycles);
	sound_z80.event = LOOK_NOW;
	s32_board.time_passes = on_time_passes;
	s32_board.sound_irq_changed = on_sound_irq;
	s32_board.sound_reset_changed = on_sound_reset;
}
