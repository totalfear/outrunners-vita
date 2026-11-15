/**
 *    Name:        video.c
 *    Description: Sega System Multi 32 video: the tilemaps, text, bitmap and backdrop layers, the sprite
 *                 frame buffers and the two mixers, drawing each frame of both screens from the board's
 *                 memories
 *    Version:     0.1.0
 *    Created:     13-10-2026
 *    Modified:    14-11-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    Behavior follows MAME's sega/segas32_v.cpp (Aaron Giles) pixel for pixel, as it draws the Multi 32
 *    (screen_update_multi32_left and _right): tools/core/system32/reference/video builds that file, and
 *    tools/check_scenarios.py --video compares its frames with these. MAME's terms are in
 *    tools/core/system32/reference/mame/LICENSE.
 *
 *    How a frame is made: every layer draws a palette offset per pixel (0 is transparent) into a buffer
 *    of its own, through its clip windows. The sprites were drawn before, into the hidden one of two
 *    frame buffers, when the board sent the list. The mixer then takes, pixel by pixel, the topmost
 *    layer that is not transparent in the order its priority registers give, maybe blends it with the
 *    one below, and looks the color up in the palette.
 *
 *    The two screens share the video RAM, so their tilemaps, text, bitmap and backdrop are the same; each
 *    has a mixer and a palette of its own, and sprites of its own: a sprite goes to one screen's frame
 *    buffers or the other's, as bit 11 of its fourth word says.
 *
 *    Video RAM holds the tilemap pages (32 x 16 cells of a 16x16 tile, 0x400 bytes each), the text cells
 *    and their graphics, the bitmap layer, the line tables, and the registers at 0x31FF00.
 */
#include "s32/video.h"
#include "s32/workers.h"

#include "s32/machine.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH   S32_VIDEO_MAX_WIDTH
#define HEIGHT  S32_VIDEO_HEIGHT

/* A function kept out of the loops that call it now and then, so that they stay small */
#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE
#endif

/* The layers in the mixer's order: its registers come in this order */
enum
{
	LAYER_TEXT,
	LAYER_NBG0,
	LAYER_NBG1,
	LAYER_NBG2,
	LAYER_NBG3,
	LAYER_BITMAP,
	LAYER_SPRITES,
	LAYER_BACKDROP,
	LAYERS
};

/* A rectangle with inclusive edges (the clip windows keep one past their right and bottom edges) */
typedef struct area
{
	int left;
	int right;
	int top;
	int bottom;
} area;

/* What a layer drew: a palette offset per pixel, the rows where everything is transparent, and the rows
   where nothing is (which hide the rows of the layers under this one: draw_layers) */
typedef struct layer
{
	uint16_t pixels[HEIGHT][WIDTH];
	bool empty[HEIGHT];
	bool solid[HEIGHT];
} layer;

/* Where the clip windows turn a layer on and off along each row: lists of columns that start at the
   left edge and end one past the right edge, one for each set of windows (0-31) and one for each row
   whose windows come from the line table (32 + the row) */
typedef struct clip_runs
{
	uint8_t list_of_row[256];
	uint16_t lists[256][16];
} clip_runs;

/* The layers and the mix are one pass over the rows: each draw_ function works its setup out and adds
   its row function here, the mix leaves its own setup, and s32_video_draw_frame runs them all over each
   row in turn (draw_and_mix_rows). A layer's pixels are then still in cache when the mix reads them,
   instead of making a 1.27 MB round trip through memory, and the workers meet at one barrier instead of
   seven. */
#define PIPELINE_MOST 8
#define ROWS_AT_A_TIME 8    /* what a thread takes of the pass at a time: few, so that none waits long for the last */

static struct
{
	int count;
	s32_rows_work work[PIPELINE_MOST];
	void *context[PIPELINE_MOST];
	const void *mix;    /* the screen's mixing (mix), for mix_rows after the layers */
} pipeline;

static void pipeline_add(s32_rows_work work, void *context)
{
	if (pipeline.count < PIPELINE_MOST)
	{
		pipeline.work[pipeline.count] = work;
		pipeline.context[pipeline.count] = context;
		pipeline.count++;
	}
}

/* The source of one row of a layer: its pixels from screen column 'from' to the one before 'to', written at
   their columns of 'pixels'. Hands back how many of them are transparent. One call a run of columns, not one
   a pixel: the call through the pointer, and whatever the source can work out once, are not paid 320 times. */
typedef int (*span_source)(const void *row, uint16_t *pixels, int from, int to);

/* A layer in the mixer's list */
typedef struct mix_layer
{
	uint16_t palette_base;
	uint16_t sprite_blend;      /* sprite groups this layer blends with */
	uint8_t blend;              /* layers this one blends with */
	uint8_t index;              /* LAYER_ */
	uint8_t priority;           /* the register's priority << 3 | the layer's own rank */
	uint8_t shift;              /* of the pixel, before the palette base is added */
	uint8_t offsets;            /* color offsets 0 or 1, or none (2) */
} mix_layer;

/* What one sprite draws with */
typedef struct sprite_drawing
{
	uint16_t *row;
	uint8_t *extra_row;         /* the extra palette's marks of the row (s32_video_use_extra_palette) */
	uint8_t extra;              /* this sprite's marks: EXTRA_COLOURS, EXTRA_GHOST */
	const area *inside;         /* the clip it draws in */
	const area *outside;        /* the clip it leaves alone, on the rows it covers */
	bool outside_row;
	bool indirect;              /* pens go through a table of 16 colors */
	bool shadow;                /* darkens what is under it instead */
	bool bits8;
	int color;
	int transparent_bits;       /* an indirect color with all these bits set is transparent */
	uint16_t table[16];
} sprite_drawing;

static struct
{
	s32_graphics graphics;
	uint8_t *image_pens;                    /* s32_video_init's: the tiles' pens taken out of the image */
	const uint8_t **image_tiles;
	const uint8_t *sprite_rom;              /* s32_video_init's image of the sprite chips */
	uint32_t sprite_rom_words;

	area visible;                           /* 320, 416 or WIDE's x 224 */
	int wide_side;                          /* s32_video_set_wide_side: WIDE's columns more at each side, or 0 */
	int side;                               /* this frame's: WIDE's side (wide_side), else 0 */
	int sprite_side;                        /* the sprite list's: every sprite that much to the right */
	bool extra_used;                        /* s32_video_use_extra_palette */
	int screens_shown;                      /* s32_video_show_screens: bit 0 the left, bit 1 the right */
	uint16_t extra_palette[S32_SCREENS][S32_PALETTE_ENTRIES];
	bool extra_set[S32_SCREENS][S32_PALETTE_ENTRIES];    /* the entries s32_video_extra_colour set */
	uint8_t extra_pixels[S32_SCREENS][2][HEIGHT][WIDTH];    /* the sprite buffers' pixels' marks */
	layer layers[LAYERS];                   /* the sprites' entry is unused: they have their own buffers */
	uint16_t sprites[S32_SCREENS][2][HEIGHT][WIDTH];    /* each screen's frame buffers: one is shown, the
	                                                       list draws into the other */
	int shown_sprites;
	uint32_t frame[S32_SCREENS][HEIGHT][WIDTH];
	clip_runs runs;
} video;

static const uint16_t transparent_row[WIDTH] = { 0 };
static uint16_t backdrop_colors[HEIGHT];    /* draw_backdrop's: the backdrop's one color on each row */
static bool half_columns;    /* s32_video_set_half_columns: every other column worked out, the next its copy */

/* A sprite pixel's marks (the extra palette's plane) */
#define EXTRA_COLOURS 1    /* its colour from the extra palette, where one is set */
#define EXTRA_GHOST   2    /* half its colour, half the colour under it */

/* ---- the board's memories ---- */

/* What the video reads of the board: copies, taken when the work is asked for. s32_video_draw_frame_begin
   takes the video RAM, the mixers, the palettes of the screens it mixes and the bits beside them;
   s32_video_sprites the sprite RAM and its controls. With s32_video_set_overlap the work is done on the
   workers' threads while the game runs on and writes the board's own, so nothing that draws reads s32_board:
   what it needs of the board is here, under the board's names */
static struct
{
	uint8_t video_ram[S32_VIDEO_RAM_SIZE];
	uint16_t palette[S32_SCREENS][S32_PALETTE_ENTRIES];
	uint16_t mixer[S32_SCREENS][S32_MIXER_REGISTERS];
	uint8_t tilebank_external;
	uint8_t sprite_flips;                   /* the sprite controls' third byte, as the frame's mix takes it */
	bool display_enabled[S32_SCREENS];      /* s32_display_enabled */
	uint8_t sprite_ram[S32_SPRITE_RAM_SIZE];
	uint8_t sprite_control_latched[8];      /* as the sprite list takes them */
} seen;

static bool bit(uint32_t value, int number)
{
	return (value >> number) & 1;
}

static int sign_extend(uint32_t value, int bits)
{
	const uint32_t sign = 1u << (bits - 1);
	return (int)((value & ((sign << 1) - 1)) ^ sign) - (int)sign;
}

/* x >> count, rounding down for negative numbers too */
static int shift_down(int value, int count)
{
	return value >= 0 ? value >> count : ~(~value >> count);
}

/* The video RAM and the sprite RAM are 16-bit words in the host's order (s32/machine.h): a word as it is,
   a byte at S32_BYTE_LANE from its address */
static uint16_t video_word(uint32_t index)
{
	uint16_t word;
	memcpy(&word, &seen.video_ram[(index & 0xffff) * 2], sizeof(word));
	return word;
}

static uint8_t video_byte(uint32_t address)
{
	return seen.video_ram[(address ^ S32_BYTE_LANE) & (S32_VIDEO_RAM_SIZE - 1)];
}

/* A register of the tilemap chip, by its offset from 0x31FF00 */
static uint16_t video_register(uint32_t offset)
{
	return video_word((0x1ff00 + offset) / 2);
}

/* A register of a screen's mixer, by its offset in the mixer's window */
static uint16_t mixer_register(int screen, uint32_t offset)
{
	return seen.mixer[screen][offset / 2];
}

static uint16_t sprite_word(uint32_t index)
{
	uint16_t word;
	memcpy(&word, &seen.sprite_ram[(index & 0xffff) * 2], sizeof(word));
	return word;
}

static uint32_t big_endian_word(const uint8_t *at)
{
	return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

/* The board as a frame is drawn from it */
static void see_frame(void)
{
	memcpy(seen.video_ram, s32_board.video_ram, sizeof(seen.video_ram));
	memcpy(seen.mixer, s32_board.mixer, sizeof(seen.mixer));
	for (int screen = 0; screen < S32_SCREENS; screen++)
	{
		seen.display_enabled[screen] = s32_display_enabled(screen);
		if (bit((uint32_t)video.screens_shown, screen) && seen.display_enabled[screen])
			memcpy(seen.palette[screen], s32_board.palette[screen], sizeof(seen.palette[screen]));
	}
	seen.tilebank_external = s32_board.tilebank_external;
	seen.sprite_flips = s32_board.sprite_control_latched[2];
}

/* The board as a sprite list is drawn from it */
static void see_sprites(void)
{
	memcpy(seen.sprite_ram, s32_board.sprite_ram, sizeof(seen.sprite_ram));
	memcpy(seen.sprite_control_latched, s32_board.sprite_control_latched, sizeof(seen.sprite_control_latched));
}

/* s32_video_set_overlap: the sprite lists and the frames are tasks for the workers' threads (s32_workers_queue),
   done in the order the board asked for them while the game runs on. Whoever changes what they draw with, or
   reads what they draw, finishes them first */
static bool overlap;
static bool work_queued;        /* tasks queued and not known to be done */
static unsigned frame_task;     /* the last frame's number among the tasks */
static unsigned sprites_task;   /* the last sprite list's */

static void finish_work(void)
{
	if (work_queued)
	{
		s32_workers_finish_all();
		work_queued = false;
	}
}

static void clip_area(area *target, const area *bounds)
{
	if (bounds->left > target->left)
		target->left = bounds->left;
	if (bounds->right < target->right)
		target->right = bounds->right;
	if (bounds->top > target->top)
		target->top = bounds->top;
	if (bounds->bottom < target->bottom)
		target->bottom = bounds->bottom;
}

/* ---- tiles and text cells ---- */

/* The 16 pens (0-15) of a row of a 16x16 tile */
static const uint8_t *tile_line(uint32_t tile, int line)
{
	const uint32_t count = video.graphics.tile_count;
	/* the board's set is 0x8000 tiles, a power of two: the wrap is an AND, not __aeabi_uidiv (an ARM with no
	   divide instruction calls it for the modulo, and this runs for every cell of every row). A set of any
	   other size keeps the modulo. */
	const uint32_t index = (count & (count - 1u)) == 0u ? (tile & (count - 1u)) : tile % count;

	return &video.graphics.tile_pens[index][line * 16];
}

/* The pens of row 'line' (0-15) of a cell of a tilemap page (32 x 16 cells, 512 x 256 pixels). A cell is
   the tile number (13 bits, the bank above them), its color (bits 4-12, over the number: CELL_COLOR has it
   where a pixel does) and its flips (bit 14 x, bit 15 y). */
#define CELL_COLOR  0x1ff0
#define CELL_FLIP_X 0x4000

static const uint8_t *cell_pens(uint16_t cell, int bank, int line)
{
	return tile_line((uint32_t)bank << 13 | (cell & 0x1fff), (cell & 0x8000) ? 15 - line : line);
}

/* A row of a tilemap that moves one source pixel a column: what the scrolled ones draw, and the zoomed
   ones at a zoom of 1. Two pages side by side, 1024 columns that wrap. */
typedef struct tile_row
{
	int pages[2];       /* left and right page of the row's half */
	int bank;
	int line;           /* the row within the pages */
	int start;          /* source column at screen column 0 */
	int step;           /* 1, or -1 flipped */
} tile_row;

/* Whether none of eight pens, taken as one number, is 0 */
static inline bool no_pen_missing(uint64_t pens)
{
	return ((pens - 0x0101010101010101ull) & ~pens & 0x8080808080808080ull) == 0;
}

/* Its columns a cell at a time: the map is read once for a cell's 16 pixels, not once a pixel; a row of a
   tile with nothing on it is cleared without being looked at pen by pen, and one with no gap in it (most
   of the others) is its pens with the cell's color, nothing to choose or to count */
static int tile_row_pixels(const void *source, uint16_t *pixels, int from, int to)
{
	const tile_row *row = (const tile_row *)source;
	const uint32_t cells = (uint32_t)(row->line >> 4) << 5;
	const int line = row->line & 15;
	const int step = row->step;
	uint32_t column = (uint32_t)(row->start + step * from) & 0x3ff;
	int transparent = 0;

	for (int x = from; x < to;)
	{
		const uint16_t cell = video_word((uint32_t)row->pages[column >> 9] << 9 | cells | ((column & 0x1ff) >> 4));
		const uint8_t *pens = cell_pens(cell, row->bank, line);
		const uint16_t color = cell & CELL_COLOR;
		const int inside = (int)(column & 15);
		int count = step > 0 ? 16 - inside : inside + 1;    /* the cell's columns from here on */
		uint64_t halves[2];

		if (count > to - x)
			count = to - x;
		memcpy(halves, pens, sizeof(halves));
		if ((halves[0] | halves[1]) == 0)
		{
			/* nothing on this row of the tile. (A whole cell's 32 bytes are cleared in place; a memset
			   of a size the compiler does not know is a call.) */
			if (count == 16)
				memset(&pixels[x], 0, 16 * sizeof(pixels[0]));
			else
				for (int index = 0; index < count; index++)
					pixels[x + index] = 0;
			transparent += count;
		}
		else if (count == 16 && no_pen_missing(halves[0]) && no_pen_missing(halves[1]))
		{
			/* a whole cell with no gap, its pens in their order or flipped (the cell is, or the row, not
			   both). (From a copy of the pens, which the pixels written cannot be: the loops are then
			   free to be done several pixels at once.) */
			uint8_t solid[16];

			memcpy(solid, pens, sizeof(solid));
			if (((cell & CELL_FLIP_X) != 0) == (step < 0))
				for (int index = 0; index < 16; index++)
					pixels[x + index] = (uint16_t)(color | solid[index]);
			else
				for (int index = 0; index < 16; index++)
					pixels[x + index] = (uint16_t)(color | solid[15 - index]);
		}
		else if (count == 16 && ((cell & CELL_FLIP_X) != 0) == (step < 0))
		{
			/* a whole cell, its pens in their order */
			for (int index = 0; index < 16; index++)
			{
				const int pen = pens[index];

				pixels[x + index] = pen ? (uint16_t)(color | pen) : 0;
				transparent += pen == 0;
			}
		}
		else if (count == 16)
		{
			/* a whole cell, flipped */
			for (int index = 0; index < 16; index++)
			{
				const int pen = pens[15 - index];

				pixels[x + index] = pen ? (uint16_t)(color | pen) : 0;
				transparent += pen == 0;
			}
		}
		else
		{
			/* part of a cell, at an edge: the cell's flip turns the way its pens are read */
			const int pen_step = (cell & CELL_FLIP_X) ? -step : step;
			int at = (cell & CELL_FLIP_X) ? 15 - inside : inside;

			for (int index = 0; index < count; index++, at += pen_step)
			{
				const int pen = pens[at];

				pixels[x + index] = pen ? (uint16_t)(color | pen) : 0;
				transparent += pen == 0;
			}
		}
		x += count;
		column = (column + (uint32_t)(step * count)) & 0x3ff;
	}
	return transparent;
}

/* The 8 pens of a row of a text cell, the leftmost in the top four bits. The text layer is 64 x 32 cells
   of 8x8, a 512 x 256 map; the cells' page and the bank of their graphics come from register 0x5C; the
   graphics are in video RAM too, 32 bytes a character, four bytes a row, the high nibble first. */
static uint32_t text_cell_pens(uint16_t control, uint16_t cell, int line)
{
	const uint32_t at = ((uint32_t)(control & 7) << 9 | (cell & 0x1ff)) * 32 + (uint32_t)line * 4;

	return (uint32_t)video_byte(at) << 24 | (uint32_t)video_byte(at + 1) << 16 | (uint32_t)video_byte(at + 2) << 8 |
		video_byte(at + 3);
}

/* Whether they are all 0: the same four bytes whatever the order the host keeps them in, read at once */
static bool text_cell_blank(uint16_t control, uint16_t cell, int line)
{
	const uint32_t at = ((uint32_t)(control & 7) << 9 | (cell & 0x1ff)) * 32 + (uint32_t)line * 4;
	uint32_t pens;

	memcpy(&pens, &seen.video_ram[at & (S32_VIDEO_RAM_SIZE - 1)], sizeof(pens));
	return pens == 0;
}

/* ---- clip windows ---- */

/* Window 0-4 (0x60 + 8 each: left, top, right, bottom), with one past its right and bottom edges */
static void window_of(int number, bool flip, area *window)
{
	const uint32_t at = 0x60 + 8 * (uint32_t)number;
	const int left = video_register(at) & 0x1ff;
	const int top = video_register(at + 2) & 0xff;
	const int right = (video_register(at + 4) & 0x1ff) + 1;
	const int bottom = (video_register(at + 6) & 0xff) + 1;

	if (!flip)
	{
		window->left = left + video.side;
		window->right = right + video.side;
		window->top = top;
		window->bottom = bottom;
		if (video.side && left == 0 && right >= 320)
		{
			window->left = 0;
			window->right = video.visible.right + 1;    /* (WIDE: the whole screen, the wider one) */
		}
	}
	else
	{
		window->left = video.visible.right + 1 - right;
		window->right = video.visible.right + 1 - left;
		window->top = video.visible.bottom + 1 - bottom;
		window->bottom = video.visible.bottom + 1 - top;
	}
}

static void sort_by_left(const area *windows, int *order)
{
	for (int index = 1; index < 5; index++)
	{
		const int key = order[index];
		int place = index - 1;

		while (place >= 0 && windows[order[place]].left > windows[key].left)
		{
			order[place + 1] = order[place];
			place--;
		}
		order[place + 1] = key;
	}
}

/* The list for a set of windows: taken from left to right, a window that starts inside the last run
   stretches it */
static void merge_windows(uint16_t *list, const area *windows, const int *order, int set, const area *edges)
{
	uint16_t *end = list;

	*end++ = (uint16_t)edges->left;
	for (int index = 0; index < 5; index++)
	{
		const area *window = &windows[order[index]];

		if (!bit((uint32_t)set, order[index]))
			continue;
		if (end != list + 1 && window->left <= end[-1])
		{
			if (window->right > end[-1])
				end[-1] = (uint16_t)window->right;
		}
		else
		{
			*end++ = (uint16_t)window->left;
			*end++ = (uint16_t)window->right;
		}
	}
	*end = (uint16_t)edges->right;
}

/* Windows 2 and 3 can take their columns from the line table instead, row by row (0xFFFF: none) */
static void line_window(int number, int line, bool flip, uint32_t table, const area *edges, area *window, int *crossing)
{
	const uint16_t left = video_word(table + 0x100 * (uint32_t)(number - 2) + (uint32_t)line);
	const uint16_t right = video_word(table + 0x200 + 0x100 * (uint32_t)(number - 2) + (uint32_t)line);

	if (left == 0xffff || right == 0xffff)
		*crossing &= ~(1 << number);
	else if (!flip)
	{
		window->left = (left & 0x1ff) + video.side;
		window->right = (right & 0x1ff) + 1 + video.side;
		if (video.side && (left & 0x1ff) == 0 && (right & 0x1ff) + 1 >= 320)
		{
			window->left = 0;
			window->right = video.visible.right + 1;
		}
	}
	else
	{
		window->left = video.visible.right + 1 - ((right & 0x1ff) + 1);
		window->right = video.visible.right + 1 - (left & 0x1ff);
	}
	clip_area(window, edges);
	if (window->left >= window->right)
		*crossing &= ~(1 << number);
}

/* A layer's runs: 'selected' picks windows 0-4. The first run of each row is drawn when the layer draws
   outside its windows (or has them off), and the runs alternate from there. */
static bool find_clip_runs(bool enabled, bool outside, int selected, clip_runs *runs)
{
	const bool flip = bit(video_register(0x00), 9);
	const uint16_t line_control = video_register(0x04);
	const bool line_windows = bit(line_control, 4) || bit(line_control, 5);
	const area edges = { video.visible.left, video.visible.right + 1, video.visible.top, video.visible.bottom + 1 };
	area windows[5];
	int order[5];

	memset(runs, 0, sizeof(*runs));
	runs->lists[0][0] = (uint16_t)edges.left;
	runs->lists[0][1] = (uint16_t)edges.right;
	if (!enabled)
		return true;

	for (int number = 0; number < 5; number++)
	{
		window_of(number, flip, &windows[number]);
		clip_area(&windows[number], &edges);
		order[number] = number;
	}
	sort_by_left(windows, order);
	for (int set = 1; set < 32; set++)
		if (set & selected)
			merge_windows(runs->lists[set], windows, order, set, &edges);

	for (int y = edges.top; y < edges.bottom; y++)
	{
		int crossing = 0;
		area row_windows[5];
		int row_order[5];

		for (int number = 0; number < 5; number++)
			if (bit((uint32_t)selected, number) && y >= windows[number].top && y < windows[number].bottom)
				crossing |= 1 << number;
		if (!line_windows)
		{
			runs->list_of_row[y] = (uint8_t)crossing;
			continue;
		}

		for (int number = 0; number < 5; number++)
		{
			row_windows[number] = windows[number];
			row_order[number] = number;
		}
		for (int number = 2; number <= 3; number++)
			if (bit(line_control, number + 2) && bit((uint32_t)selected, number))
				line_window(number, flip ? video.visible.bottom - y : y, flip, (uint32_t)(line_control >> 10) * 0x400,
					&edges, &row_windows[number], &crossing);
		sort_by_left(row_windows, row_order);
		merge_windows(runs->lists[32 + y], row_windows, row_order, crossing, &edges);
		runs->list_of_row[y] = (uint8_t)(32 + y);
	}
	return outside;
}

/* One row of a layer through its runs, which alternate between drawn and cleared starting with 'drawn'.
   Overlapping windows can make a run start before the last one ended: it is drawn again. */
static void draw_row(layer *out, int y, const uint16_t *runs, bool drawn, span_source span, const void *row)
{
	uint16_t *pixels = out->pixels[y];
	const int width = video.visible.right + 1 - video.visible.left;
	/* (one run, drawn, from edge to edge: the only rows looked at for having no gap) */
	const bool across = drawn && runs[0] <= video.visible.left && runs[1] > video.visible.right;
	int transparent = 0;

	if (!drawn && runs[1] > video.visible.right)
	{
		/* one run, cleared: the whole row most times, and an empty row's pixels are not written, as
		   nothing reads them (mix_rows takes transparent_row in their place) */
		transparent = runs[1] - runs[0];
		out->solid[y] = false;
		out->empty[y] = transparent == width;
		if (!out->empty[y])
			for (int x = runs[0]; x < runs[1] && x < WIDTH; x++)
				pixels[x] = 0;
		return;
	}
	for (;; runs++, drawn = !drawn)
	{
		const int from = runs[0];
		const int to = runs[1] < WIDTH ? runs[1] : WIDTH;

		if (drawn && half_columns && from < to)
		{
			/* every column shows the even one at or before it (a run's first too, wherever it starts).
			   Every layer samples the same columns, so they still line up */
			uint16_t whole[WIDTH];

			span(row, whole, from & ~1, to);
			for (int x = from; x < to; x++)
			{
				pixels[x] = whole[x & ~1];
				transparent += pixels[x] == 0;
			}
		}
		else if (drawn && from < to)
			transparent += span(row, pixels, from, to);
		else if (!drawn)
		{
			for (int x = from; x < to; x++)
				pixels[x] = 0;
			transparent += runs[1] - runs[0];
		}
		if (runs[1] > video.visible.right)
			break;
	}
	out->empty[y] = transparent == width;
	out->solid[y] = across && transparent == 0;
}

/* Whether a row of a layer is under a row with no gap of one of the layers 'under' (LAYER_ bits: the ones
   that hide it, drawn before it). It is left empty then, and not drawn: nothing of it would be seen. */
static bool hidden_row(layer *out, int under, int y)
{
	for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
		if (bit((uint32_t)under, index) && video.layers[index].solid[y])
		{
			out->empty[y] = true;
			out->solid[y] = false;
			return true;
		}
	return false;
}

/* ---- the layers ---- */

/* The four pages of tilemap 0-3 (upper left, upper right, lower left, lower right) and its tile bank */
static int pages_of(int number, int *pages)
{
	const uint16_t upper = video_register(0x40 + 4 * (uint32_t)number);
	const uint16_t lower = video_register(0x42 + 4 * (uint32_t)number);

	pages[0] = upper & 0x7f;
	pages[1] = (upper >> 8) & 0x7f;
	pages[2] = lower & 0x7f;
	pages[3] = (lower >> 8) & 0x7f;
	/* the I/O chip's port H gives each tilemap a bank of its own (System 32: one, with register 0x00) */
	return (seen.tilebank_external >> (2 * number)) & 3;
}

/* Register 0x00 flips tilemap 0-3 against the whole screen's flip (bit 9); bit 8 keeps them from
   flipping vertically */
static void flips_of(int number, bool *flip_x, bool *flip_y)
{
	const uint16_t control = video_register(0x00);
	const bool screen_flip = bit(control, 9);
	const bool layer_flip = bit(control, number);

	*flip_x = layer_flip ? !screen_flip : screen_flip;
	*flip_y = (layer_flip && !bit(control, 8)) ? !screen_flip : screen_flip;
}

typedef struct zoomed_row
{
	int pages[2];       /* left and right page of the row's half */
	int bank;
	int line;           /* the row within the pages */
	uint32_t x_start;   /* source column at the left edge, 12.20 */
	uint32_t x_step;
} zoomed_row;

/* Its columns, each from wherever the zoom puts it; the cell is read again only when the source moves on
   to another one */
static int zoomed_row_pixels(const void *source, uint16_t *pixels, int from, int to)
{
	const zoomed_row *row = (const zoomed_row *)source;
	const uint32_t cells = (uint32_t)(row->line >> 4) << 5;
	const int line = row->line & 15;
	uint32_t column = row->x_start + row->x_step * (uint32_t)(from - video.visible.left - video.side);
	uint32_t held = ~0u;        /* the cell the pens are of: bits 24-29 of a column, the page and the cell in it */
	const uint8_t *pens = NULL;
	uint16_t color = 0;
	uint32_t flip = 0;
	int transparent = 0;

	for (int x = from; x < to; x++, column += row->x_step)
	{
		const uint32_t at = (column >> 24) & 0x3f;
		int pen;

		if (at != held)
		{
			const uint16_t cell = video_word((uint32_t)row->pages[at >> 5] << 9 | cells | (at & 0x1f));

			pens = cell_pens(cell, row->bank, line);
			color = cell & CELL_COLOR;
			flip = (cell & CELL_FLIP_X) ? 15 : 0;
			held = at;
		}
		pen = pens[((column >> 20) & 15) ^ flip];
		pixels[x] = pen ? (uint16_t)(color | pen) : 0;
		transparent += pen == 0;
	}
	return transparent;
}

/* What a zoomed tilemap draws with, the same on every row (its rows on the workers' bands) */
typedef struct zoomed_layer
{
	int number;
	int under;          /* the layers that hide it (hidden_row) */
	int pages[4];
	int bank;
	bool first_drawn;
	uint32_t x_start;
	uint32_t x_step;
	uint32_t source_y;  /* at the top row, 12.20 */
	uint32_t y_step;
	clip_runs runs;    /* its own: the layers are set up together now, so they cannot share one */
} zoomed_layer;

static void draw_zoomed_rows(void *context, int first, int last)
{
	const zoomed_layer *zoomed = (const zoomed_layer *)context;

	for (int y = first; y <= last; y++)
	{
		const uint32_t source_y = zoomed->source_y + (uint32_t)(y - video.visible.top) * zoomed->y_step;
		const int half = (source_y >> 27) & 2;

		if (zoomed->under != 0 && hidden_row(&video.layers[LAYER_NBG0 + zoomed->number], zoomed->under, y))
			continue;
		if (zoomed->x_step == 1u << 20 || zoomed->x_step == 0u - (1u << 20))
		{
			/* a zoom of 1 (most of the time): the source moves a whole pixel a column, so the fraction of
			   its start never carries and the row is drawn as a scrolled one is, a cell at a time */
			const int step = zoomed->x_step == 1u << 20 ? 1 : -1;
			const tile_row row = {
				{ zoomed->pages[half], zoomed->pages[half + 1] }, zoomed->bank, (source_y >> 20) & 0xff,
				(int)(zoomed->x_start >> 20) - step * (video.visible.left + video.side), step
			};

			draw_row(&video.layers[LAYER_NBG0 + zoomed->number], y, zoomed->runs.lists[zoomed->runs.list_of_row[y]],
				zoomed->first_drawn, tile_row_pixels, &row);
		}
		else
		{
			const zoomed_row row = {
				{ zoomed->pages[half], zoomed->pages[half + 1] }, zoomed->bank, (source_y >> 20) & 0xff,
				zoomed->x_start, zoomed->x_step
			};

			draw_row(&video.layers[LAYER_NBG0 + zoomed->number], y, zoomed->runs.lists[zoomed->runs.list_of_row[y]],
				zoomed->first_drawn, zoomed_row_pixels, &row);
		}
	}
}

static zoomed_layer zoomed_kept[2];    /* the context outlives the setup now: the dispatch is later */

/* NBG0 and NBG1: scrolled (with a fraction) and zoomed about a center. The steps are screen pixels per
   source pixel (0x200 = 1); the source moves by their reciprocal, in 12.20. */
static void draw_zoomed(int number, int under)
{
	const uint32_t scroll = 0x10 + 8 * (uint32_t)number;
	const uint32_t center = 0x30 + 4 * (uint32_t)number;
	int pages[4];
	const int bank = pages_of(number, pages);
	bool flip_x;
	bool flip_y;
	int x_zoom = video_register(0x50 + 4 * (uint32_t)number) & 0xfff;
	int y_zoom = bit(video_register(0x00), 14) ? video_register(0x52 + 4 * (uint32_t)number) & 0xfff : x_zoom;
	uint32_t x_step;
	uint32_t y_step;
	uint32_t x_start;
	uint32_t source_y;
	zoomed_layer *const zoomed = &zoomed_kept[number];
	const bool first_drawn = find_clip_runs(bit(video_register(0x02), 11 + number), bit(video_register(0x02), 6 + number),
		(video_register(0x06) >> (4 * number)) & 0x0f, &zoomed->runs);

	flips_of(number, &flip_x, &flip_y);
	if (x_zoom < 0x80)
		x_zoom = 0x80;
	if (y_zoom < 0x80)
		y_zoom = 0x80;
	x_step = (0x200u << 20) / (uint32_t)x_zoom;
	y_step = (0x200u << 20) / (uint32_t)y_zoom;

	/* the scroll with its fraction, less the center (one bit more of it while zoomed) */
	x_start = (uint32_t)(video_register(scroll + 2) & 0x3ff) << 20;
	x_start += (uint32_t)(video_register(scroll) & 0xff00) << 4;
	source_y = (uint32_t)(video_register(scroll + 6) & 0x1ff) << 20;
	source_y += (uint32_t)(video_register(scroll + 4) & 0xfe00) << 4;
	x_start -= (uint32_t)sign_extend(video_register(center), x_zoom != 0x200 ? 10 : 9) * x_step;
	source_y -= (uint32_t)sign_extend(video_register(center + 2), y_zoom != 0x200 ? 10 : 9) * y_step;
	x_start += (uint32_t)video.visible.left * x_step;
	source_y += (uint32_t)video.visible.top * y_step;
	if (flip_y)
	{
		source_y += (uint32_t)(video.visible.bottom - 2 * video.visible.top) * y_step;
		y_step = 0u - y_step;
	}
	if (flip_x)
	{
		x_start += (uint32_t)(video.visible.right - 2 * video.side - 2 * video.visible.left) * x_step;
		x_step = 0u - x_step;
	}

	zoomed->number = number;
	zoomed->under = under;
	zoomed->pages[0] = pages[0];
	zoomed->pages[1] = pages[1];
	zoomed->pages[2] = pages[2];
	zoomed->pages[3] = pages[3];
	zoomed->bank = bank;
	zoomed->first_drawn = first_drawn;
	zoomed->x_start = x_start;
	zoomed->x_step = x_step;
	zoomed->source_y = source_y;
	zoomed->y_step = y_step;
	pipeline_add(draw_zoomed_rows, zoomed);
}

/* What a scrolled tilemap draws with, the same on every row */
typedef struct scrolled_layer
{
	int number;
	int under;          /* the layers that hide it (hidden_row) */
	uint32_t table;
	bool row_scroll;
	bool row_select;
	int x_scroll;
	int y_scroll;
	int pages[4];
	int bank;
	bool flip_x;
	bool flip_y;
	bool first_drawn;
	clip_runs runs;    /* its own: the layers are set up together now, so they cannot share one */
} scrolled_layer;

static void draw_scrolled_rows(void *context, int first, int last)
{
	const scrolled_layer *scrolled = (const scrolled_layer *)context;

	for (int y = first; y <= last; y++)
	{
		const int line = scrolled->flip_y ? video.visible.bottom - y : y;
		int source_y = scrolled->y_scroll + line;
		int x_start = (scrolled->flip_x ? video.visible.right - 2 * video.side : video.visible.left) + scrolled->x_scroll;
		tile_row row;

		if (scrolled->under != 0 && hidden_row(&video.layers[LAYER_NBG0 + scrolled->number], scrolled->under, y))
			continue;
		row.step = scrolled->flip_x ? -1 : 1;
		if (scrolled->row_scroll)
			x_start += video_word(scrolled->table + (uint32_t)line) & 0x3ff;
		if (scrolled->row_select)
			source_y = (scrolled->y_scroll + video_word(scrolled->table + 0x200 + (uint32_t)line)) & 0x1ff;
		/* (x_start is the source column at the left edge of the board's screen: from there to column 0) */
		row.start = x_start - row.step * (video.visible.left + video.side);
		row.pages[0] = scrolled->pages[(source_y >> 7) & 2];
		row.pages[1] = scrolled->pages[((source_y >> 7) & 2) + 1];
		row.bank = scrolled->bank;
		row.line = source_y & 0xff;
		draw_row(&video.layers[LAYER_NBG0 + scrolled->number], y, scrolled->runs.lists[scrolled->runs.list_of_row[y]],
			scrolled->first_drawn, tile_row_pixels, &row);
	}
}

/* NBG2 and NBG3: scrolled, and row by row the line table can add to the column (row scroll) or pick
   the source row (row select). Register 0x04 turns those on; its top bits place the table. */
static scrolled_layer scrolled_kept[2];    /* NBG2 and NBG3, kept until the single dispatch */

static void draw_scrolled(int number, int under)
{
	const uint16_t line_control = video_register(0x04);
	const bool line_windows = bit(line_control, number + 2);
	scrolled_layer *const scrolled = &scrolled_kept[number - 2];

	scrolled->number = number;
	scrolled->under = under;
	scrolled->table = (uint32_t)(line_control >> 10) * 0x400 + 0x100 * (uint32_t)(number - 2);
	scrolled->row_scroll = bit(line_control, number - 2) && !line_windows;
	scrolled->row_select = bit(line_control, number) && !line_windows;
	scrolled->x_scroll = (video_register(0x12 + 8 * (uint32_t)number) & 0x3ff) - (video_register(0x30 + 4 * (uint32_t)number) & 0x1ff);
	scrolled->y_scroll = video_register(0x16 + 8 * (uint32_t)number) & 0x1ff;
	scrolled->bank = pages_of(number, scrolled->pages);
	scrolled->first_drawn = find_clip_runs(bit(video_register(0x02), 11 + number), bit(video_register(0x02), 6 + number),
		(video_register(0x06) >> (4 * number)) & 0x0f, &scrolled->runs);
	flips_of(number, &scrolled->flip_x, &scrolled->flip_y);
	pipeline_add(draw_scrolled_rows, scrolled);
}

/* The text layer, with no scroll and no windows; the screen's flip turns the whole 512 x 256 map */
static void draw_text_rows(void *context, int first, int last)
{
	const bool flip = *(const bool *)context;
	const uint16_t control = video_register(0x5c);
	const uint32_t page = (uint32_t)((control >> 4) & 0x1f) << 11;
	/* the columns it draws: the screen's, or the board's 320 in the middle (WIDE: the sides clear), and
	   the map's columns they show, the first and the last */
	const int from = video.side ? video.side : video.visible.left;
	const int to = video.side ? video.side + 320 : video.visible.right + 1;
	const int source_low = flip ? 511 - (to - 1 - video.side) : from - video.side;
	const int source_high = flip ? 511 - (from - video.side) : to - 1 - video.side;

	for (int y = first; y <= last; y++)
	{
		layer *const text = &video.layers[LAYER_TEXT];
		uint16_t *pixels = text->pixels[y];
		const int source_y = flip ? 255 - y : y;
		const uint32_t cells = page | (uint32_t)(source_y >> 3) << 6;
		const int line = source_y & 7;
		bool blank = true;

		/* a row with nothing on it, as most are, is found out a cell at a time and not drawn: the mix
		   does not read an empty row (it takes transparent_row in its place) */
		for (int cell = source_low >> 3; cell <= source_high >> 3 && blank; cell++)
			blank = text_cell_blank(control, video_word(cells | (uint32_t)cell), line);
		text->empty[y] = blank;
		if (blank)
			continue;

		for (int x = video.visible.left; x < from; x++)
			pixels[x] = 0;
		for (int x = to; x <= video.visible.right; x++)
			pixels[x] = 0;
		for (int x = from; x < to;)
		{
			const int source_x = flip ? 511 - (x - video.side) : x - video.side;
			const int inside = source_x & 7;
			const uint16_t cell = video_word(cells | (uint32_t)(source_x >> 3));
			const uint32_t pens = text_cell_pens(control, cell, line);
			const uint16_t color = (uint16_t)(((cell >> 9) & 0x7f) << 4);
			int count = flip ? inside + 1 : 8 - inside;    /* the cell's columns from here on */

			if (count > to - x)
				count = to - x;
			for (int index = 0; index < count; index++)
			{
				const int pen = (pens >> (28 - 4 * (flip ? inside - index : inside + index))) & 0x0f;

				pixels[x + index] = pen ? (uint16_t)(color | pen) : 0;
			}
			x += count;
		}
		if (half_columns)
			for (int x = video.visible.left + 1; x <= video.visible.right; x++)
				if (x & 1)
					pixels[x] = pixels[x - 1];    /* (half the columns: an odd one shows the one before it) */
	}
}

static bool text_flip_kept;

static void draw_text(void)
{
	text_flip_kept = bit(video_register(0x00), 9);
	pipeline_add(draw_text_rows, &text_flip_kept);
}

typedef struct bitmap_row
{
	uint32_t start;     /* the row in video RAM: a byte offset (8 bits a pixel) or a word (4 bits) */
	int x_scroll;
	int color;
	bool bits8;
} bitmap_row;

static uint16_t bitmap_pixel(const bitmap_row *row, int x)
{
	const int column = (x - video.side + row->x_scroll) & 0x1ff;
	int pixel;

	if (row->bits8)
	{
		pixel = video_byte(row->start + (uint32_t)column) + row->color;
		return (pixel & 0xff) ? (uint16_t)pixel : 0;
	}
	pixel = ((video_word(row->start + (uint32_t)column / 4) >> (4 * (column & 3))) & 0x0f) + row->color;
	return (pixel & 0x0f) ? (uint16_t)pixel : 0;
}

static int bitmap_row_pixels(const void *source, uint16_t *pixels, int from, int to)
{
	const bitmap_row *row = (const bitmap_row *)source;
	int transparent = 0;

	for (int x = from; x < to; x++)
	{
		pixels[x] = bitmap_pixel(row, x);
		transparent += pixels[x] == 0;
	}
	return transparent;
}

/* What the bitmap layer draws with, the same on every row */
typedef struct bitmap_layer
{
	bool bits8;
	int under;          /* the layers that hide it (hidden_row) */
	int x_scroll;
	int y_scroll;
	int color;
	bool first_drawn;
	clip_runs runs;    /* its own: the layers are set up together now, so they cannot share one */
} bitmap_layer;

static void draw_bitmap_rows(void *context, int first, int last)
{
	const bitmap_layer *bitmap = (const bitmap_layer *)context;

	for (int y = first; y <= last; y++)
	{
		const bitmap_row row = {
			bitmap->bits8 ? 512 * (uint32_t)((y + bitmap->y_scroll) & 0xff) : 128 * (uint32_t)((y + bitmap->y_scroll) & 0x1ff),
			bitmap->x_scroll, bitmap->color, bitmap->bits8
		};

		if (bitmap->under != 0 && hidden_row(&video.layers[LAYER_BITMAP], bitmap->under, y))
			continue;
		draw_row(&video.layers[LAYER_BITMAP], y, bitmap->runs.lists[bitmap->runs.list_of_row[y]], bitmap->first_drawn,
			bitmap_row_pixels, &row);
	}
}

/* The bitmap layer: 512 pixels a row from the start of video RAM, 4 or 8 bits each (register 0x00
   bit 11), scrolled by registers 0x88 and 0x8A, its palette from 0x8C; window 4 clips it */
static void draw_bitmap(int under)
{
	static bitmap_layer bitmap_kept;
	bitmap_layer *const bitmap = &bitmap_kept;

	bitmap->under = under;
	bitmap->bits8 = bit(video_register(0x00), 11);
	bitmap->x_scroll = video_register(0x88) & 0x1ff;
	bitmap->y_scroll = video_register(0x8a) & 0x1ff;
	bitmap->color = (video_register(0x8c) << 4) & 0x1fff0 & ~((1 << (bitmap->bits8 ? 8 : 4)) - 1);
	bitmap->first_drawn = find_clip_runs(bit(video_register(0x02), 15), bit(video_register(0x02), 10), 0x10, &bitmap->runs);
	pipeline_add(draw_bitmap_rows, bitmap);
}

/* The backdrop: one color for the screen, or one per line (register 0x5E bit 15), wrapping at 511. A
   row's color is kept, not a row of pixels of it: the mix fills the backdrop's row with it only where a
   pixel takes the long way (mix_rows), instead of 143 KB being written every frame to be read here and
   there */
static void draw_backdrop(void)
{
	const uint16_t control = video_register(0x5e);
	const bool flip = bit(video_register(0x00), 9);

	for (int y = video.visible.top; y <= video.visible.bottom; y++)
	{
		const int color = (control & 0x1e00) + (bit(control, 15) ? (control + y) & 0x1ff : 0);

		backdrop_colors[flip ? video.visible.bottom - y : y] = (uint16_t)color;
	}
}

/* Every layer the registers leave on (0x02 and 0x8E turn them off, 0x00 bits 12-13 too for NBG2-3);
   the backdrop is always there. The layers drawn, as bits by LAYER_. */
s32_tile_observer s32_video_tile_observer = NULL;

/* Every cell of the pages tilemap 'number' shows, to s32_video_tile_observer */
static void observe_tilemap(int number)
{
	int pages[4];
	const int bank = pages_of(number, pages);

	for (int index = 0; index < 4; index++)
	{
		bool seen = false;

		for (int earlier = 0; earlier < index; earlier++)
			seen = seen || pages[earlier] == pages[index];
		if (seen)
			continue;
		for (uint32_t cell = 0; cell < 32 * 16; cell++)
		{
			const uint16_t word = video_word((uint32_t)pages[index] << 9 | cell);

			s32_video_tile_observer(number, (uint32_t)bank << 13 | (word & 0x1fff), (word >> 4) & 0x1ff);
		}
	}
}

/* Where a layer goes in a mixer's order, from its register there (0x20 + 2 a layer): the highest on top.
   The backdrop's is 8, under every layer that has a priority. */
static int layer_rank(uint16_t control, int index)
{
	return (control & 0x0f) << 3 | (6 - index);
}

/* Whether layer 'above' hides layer 'below' wherever it is not transparent: over it (which a layer with no
   priority never is), and blending with nothing, in the mixer of every screen shown that takes the lower
   one. (A sprite's place does not come into it: what is under a pixel of the upper layer is not seen,
   whether a sprite is over that pixel or lets it show.) */
static bool covers(int above, int below)
{
	for (int screen = 0; screen < S32_SCREENS; screen++)
	{
		const uint16_t upper = mixer_register(screen, 0x20 + 2 * (uint32_t)above);
		const uint16_t lower = mixer_register(screen, 0x20 + 2 * (uint32_t)below);

		if (!bit((uint32_t)video.screens_shown, screen) || !seen.display_enabled[screen] || (lower & 0x0f) == 0)
			continue;
		if (layer_rank(upper, above) < layer_rank(lower, below))
			return false;
		if (bit(mixer_register(screen, 0x4e), 11) && ((mixer_register(screen, 0x30 + 2 * (uint32_t)above) >> 6) & 0xff) != 0)
			return false;
	}
	return true;
}

/* The layers that are on, of those 'wanted' (LAYER_* bits): the ones the mixers of the screens shown take.
   They are drawn from the top one down, as the first screen shown has them, each knowing the ones drawn
   before it that hide it: a row under a row with no gap is not drawn (hidden_row). */
static int draw_layers(int wanted)
{
	const uint16_t control = video_register(0x00);
	const uint16_t off = video_register(0x02);
	const uint16_t more_off = video_register(0x8e);
	int drawn = 0;
	int order[LAYERS];
	int ranks[LAYERS];
	int count = 0;
	int first_screen = 0;

	if (bit((uint32_t)wanted, LAYER_NBG0) && !bit(off, 0) && !bit(more_off, 1))
		drawn |= 1 << LAYER_NBG0;
	if (bit((uint32_t)wanted, LAYER_NBG1) && !bit(off, 1) && !bit(more_off, 2))
		drawn |= 1 << LAYER_NBG1;
	if (bit((uint32_t)wanted, LAYER_NBG2) && !bit(off, 2) && !bit(more_off, 3) && !bit(control, 12))
		drawn |= 1 << LAYER_NBG2;
	if (bit((uint32_t)wanted, LAYER_NBG3) && !bit(off, 3) && !bit(more_off, 4) && !bit(control, 13))
		drawn |= 1 << LAYER_NBG3;
	if (bit((uint32_t)wanted, LAYER_TEXT) && !bit(off, 4) && !bit(more_off, 0))
		drawn |= 1 << LAYER_TEXT;
	if (bit((uint32_t)wanted, LAYER_BITMAP) && !bit(off, 5) && !bit(more_off, 5))
		drawn |= 1 << LAYER_BITMAP;
	if (s32_video_tile_observer != NULL)
		for (int number = 0; number < 4; number++)
			if (bit((uint32_t)drawn, LAYER_NBG0 + number))
				observe_tilemap(number);

	while (first_screen < S32_SCREENS - 1 &&
		!(bit((uint32_t)video.screens_shown, first_screen) && seen.display_enabled[first_screen]))
		first_screen++;
	for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
	{
		const int rank = layer_rank(mixer_register(first_screen, 0x20 + 2 * (uint32_t)index), index);
		int place = count;

		if (!bit((uint32_t)drawn, index))
			continue;
		for (; place > 0 && ranks[place - 1] < rank; place--)
		{
			order[place] = order[place - 1];
			ranks[place] = ranks[place - 1];
		}
		order[place] = index;
		ranks[place] = rank;
		count++;
	}
	for (int place = 0; place < count; place++)
	{
		const int index = order[place];
		int under = 0;

		for (int earlier = 0; earlier < place; earlier++)
			if (covers(order[earlier], index))
				under |= 1 << order[earlier];
		if (index == LAYER_TEXT)
			draw_text();
		else if (index == LAYER_BITMAP)
			draw_bitmap(under);
		else if (index <= LAYER_NBG1)
			draw_zoomed(index - LAYER_NBG0, under);
		else
			draw_scrolled(index - LAYER_NBG0, under);
	}
	draw_backdrop();
	return drawn;
}

/* ---- sprites ---- */

s32_sprite_observer s32_video_sprite_observer = NULL;

static void put_sprite_pixel(const sprite_drawing *sprite, int x, int pen, int skipped_pen)
{
	int pixel;

	if (x < sprite->inside->left || x > sprite->inside->right || pen == skipped_pen)
		return;
	if (sprite->outside_row && x >= sprite->outside->left && x <= sprite->outside->right)
		return;
	if (!sprite->indirect)
	{
		if (pen == 0)
			return;
		pixel = sprite->color | pen;
	}
	else
	{
		pixel = sprite->bits8 ? sprite->table[pen >> 4] | (pen & 0x0f) : sprite->table[pen];
		if ((pixel & sprite->transparent_bits) == sprite->transparent_bits)
			return;
	}
	if (!sprite->shadow)
	{
		sprite->row[x] = (uint16_t)pixel;
		if (sprite->extra_row)
			sprite->extra_row[x] = sprite->extra;
	}
	else
		sprite->row[x] &= 0x7fff;
}

/* A 32-bit word of sprite graphics: from the ROM bank, or from sprite RAM itself */
static uint32_t sprite_graphics(bool from_ram, uint32_t bank, uint32_t index)
{
	if (from_ram)
	{
		/* the bytes in their order in sprite RAM, each at S32_BYTE_LANE in its word */
		const uint8_t *at = &seen.sprite_ram[(index & 0x7fff) * 4];
		const uint8_t bytes[4] = { at[0 ^ S32_BYTE_LANE], at[1 ^ S32_BYTE_LANE], at[2 ^ S32_BYTE_LANE],
		                           at[3 ^ S32_BYTE_LANE] };
		return big_endian_word(bytes);
	}
	return video.graphics.sprite_word((bank << 20) | (index & 0xfffff));
}

/* One row of a sprite: words of 8 pens (4 bits) or 4 (8 bits), each pen repeated or skipped by the
   zoom. The first and last pen of a word are transparent at all ones (unless the sprite is opaque),
   and a word that ends with them ends the row. */
static void draw_sprite_row(const sprite_drawing *sprite, bool from_ram, uint32_t bank, uint32_t address,
	int x, int x_end, int x_delta, int x_zoom, int end_pen)
{
	const int bits = sprite->bits8 ? 8 : 4;
	const int pens = 32 / bits;
	const int pen_mask = (1 << bits) - 1;
	int x_accumulator = 0;

	while (x != x_end)
	{
		const uint32_t word = sprite_graphics(from_ram, bank, address++);
		int pen = 0;

		for (int index = 0; index < pens; index++)
		{
			const int skipped_pen = (index == 0 || index == pens - 1) ? end_pen : 0;

			pen = (int)(word >> (32 - bits * (index + 1))) & pen_mask;
			while (x_accumulator < 0x10000 && x != x_end)
			{
				put_sprite_pixel(sprite, x, pen, skipped_pen);
				x += x_delta;
				x_accumulator += x_zoom;
			}
			x_accumulator -= 0x10000;
		}
		if (end_pen != 0 && pen == pen_mask)
			break;
	}
}

/* A sprite entry (8 words): flags, source height and width, screen height (and bank bits), screen
   width, y, x, the graphics' offset in the bank, and the color. Hands back how many more entries it
   takes: an indirect table kept in the list takes two. */
static int draw_sprite(uint32_t entry, int x_offset, int y_offset, const area *inside, const area *outside)
{
	static const uint16_t transparent_bits[4][4] = {
		{ 0x7fff, 0x3fff, 0x1fff, 0x0fff },
		{ 0x3fff, 0x1fff, 0x0fff, 0x07ff },
		{ 0x3fff, 0x1fff, 0x0fff, 0x07ff },
		{ 0x1fff, 0x0fff, 0x07ff, 0x03ff }
	};
	const uint8_t *latched = seen.sprite_control_latched;
	const uint16_t flags = sprite_word(entry);
	const uint16_t size = sprite_word(entry + 1);
	const uint16_t height_word = sprite_word(entry + 2);
	const uint16_t width_word = sprite_word(entry + 3);
	const bool table_inline = bit(flags, 12);
	const bool from_ram = bit(flags, 10);
	const bool flip_y = bit(flags, 7);
	const bool flip_x = bit(flags, 6);
	const int source_height = size >> 8;
	const int height = height_word & 0x3ff;
	const int width = width_word & 0x3ff;
	/* the bank in bits 13 and 15 of the fourth word; bit 11 picks the screen */
	const uint32_t bank = ((width_word & 0x2000) >> 13 | (width_word & 0x8000) >> 14) % (video.graphics.sprite_banks ? video.graphics.sprite_banks : 1);
	const int screen = bit(width_word, 11);
	const int taken = (bit(flags, 13) && table_inline) ? 2 : 0;

	if (!bit((uint32_t)video.screens_shown, screen))
		return taken;    /* (a screen the port does not show: s32_video_show_screens) */
	sprite_drawing sprite;
	int source_width;
	int end_pen;
	int x;
	int y;
	int x_delta = 1;
	int y_delta = 1;
	int x_end;
	int y_end;
	int x_zoom;
	int y_zoom;
	int y_accumulator = 0;
	uint32_t address = sprite_word(entry + 6) | (uint32_t)(height_word & 0xf000) << 4;

	sprite.inside = inside;
	sprite.outside = outside;
	sprite.extra = (uint8_t)(!video.extra_used ? 0
		: ((size & S32_SPRITE_EXTRA_PALETTE) ? EXTRA_COLOURS : 0) | ((sprite_word(entry + 7) & S32_SPRITE_GHOST) ? EXTRA_GHOST : 0));
	sprite.extra_row = NULL;
	sprite.indirect = bit(flags, 13);
	sprite.shadow = bit(flags, 11) && bit(latched[5], 0);
	sprite.bits8 = bit(flags, 9);
	sprite.color = 0x8000 | (sprite_word(entry + 7) & (sprite.bits8 ? 0x7f00 : 0x7ff0));
	source_width = sprite.bits8 ? (size & 0x3f) : ((size >> 1) & 0x3f);
	end_pen = bit(flags, 8) ? 0 : (sprite.bits8 ? 0xff : 0x0f);
	if (source_width == 0 || source_height == 0 || width == 0 || height == 0)
		return taken;

	sprite.transparent_bits = transparent_bits[latched[4] & 3][latched[5] & 3];
	if (sprite.bits8)
		sprite.transparent_bits &= 0xfff0;
	if (sprite.indirect)
	{
		/* the table follows the entry, or is an entry of the list picked by the color word */
		const uint32_t table = table_inline ? entry + 8 : 8 * (uint32_t)(sprite_word(entry + 7) & 0x1fff);

		for (int index = 0; index < 16; index++)
			sprite.table[index] = (uint16_t)((sprite_word(table + (uint32_t)index) & (sprite.bits8 ? 0xfff0 : 0xffff)) |
				(bit(latched[5], 0) ? 0x8000 : 0));
	}

	if (s32_video_sprite_observer != NULL && !from_ram)
	{
		/* the pixel each pen puts in the buffer, as put_sprite_pixel works it out */
		uint16_t pixels[256];
		const int pens = sprite.bits8 ? 256 : 16;

		for (int pen = 0; pen < pens; pen++)
			pixels[pen] = (uint16_t)(!sprite.indirect ? (sprite.color | pen)
				: (sprite.bits8 ? sprite.table[pen >> 4] | (pen & 0x0f) : sprite.table[pen]));
		s32_video_sprite_observer(screen, bank << 20 | (address & 0xfffff), source_width, source_height, sprite.bits8,
			end_pen == 0, pixels);
	}

	x_zoom = (((sprite.bits8 ? 4 : 8) * source_width) << 16) / width;
	y_zoom = (source_height << 16) / height;

	/* place it: the list's offset, then its alignment (0 or 3 centered, 1 by the far edge, 2 by the near) */
	x = sign_extend(sprite_word(entry + 5), 12) + (bit(flags, 4) ? x_offset : 0) + video.sprite_side;
	y = sign_extend(sprite_word(entry + 4), 12) + (bit(flags, 5) ? y_offset : 0);
	switch (flags & 3)
	{
		case 0:
		case 3: x -= (width - 1) / 2; break;
		case 1: x -= width - 1; break;
		default: break;
	}
	switch ((flags >> 2) & 3)
	{
		case 0:
		case 3: y -= (height - 1) / 2; break;
		case 1: y -= height - 1; break;
		default: break;
	}
	if (flip_x)
	{
		x += width - 1;
		x_delta = -1;
	}
	if (flip_y)
	{
		y += height - 1;
		y_delta = -1;
	}
	x_end = x + x_delta * width;
	y_end = y + y_delta * height;

	/* rows end at the sprite's width, or at the clip. (MAME tests a flipped sprite's end against the clip's
	   right edge instead of its left one, so the sprite runs on to the left edge reading whatever follows
	   it in the ROM: an opaque one, which ignores end codes, leaves copies of the next frames there. The
	   reference, tools/core/system32/reference/mame/sega/segas32_v.cpp, has the same line fixed.) */
	if (x_delta > 0 && x_end > inside->right)
	{
		x_end = inside->right + 1;
		if (x >= x_end)
			return taken;
	}
	if (x_delta < 0 && x_end < inside->left)
	{
		x_end = inside->left - 1;
		if (x <= x_end)
			return taken;
	}

	for (int row = y; row != y_end; row += y_delta)
	{
		if (row >= inside->top && row <= inside->bottom)
		{
			sprite.row = video.sprites[screen][video.shown_sprites ^ 1][row];
			if (video.extra_used)
				sprite.extra_row = video.extra_pixels[screen][video.shown_sprites ^ 1][row];
			sprite.outside_row = row >= outside->top && row <= outside->bottom;
			draw_sprite_row(&sprite, from_ram, bank, address, x, x_end, x_delta, x_zoom, end_pen);
		}
		/* the zoom steps through the source rows */
		y_accumulator += y_zoom;
		address += (uint32_t)(source_width * (y_accumulator >> 16));
		y_accumulator &= 0xffff;
	}
	return taken;
}

/* The list in sprite RAM from its entry 'number', 16 bytes an entry, its command in the top two bits of the
   first word: draw a sprite, set the clips (bit 12 the one to draw in, bit 13 the one to leave alone), jump
   (and with bit 13, set the offset sprites can add), or end. The board's starts at 0. Only its rows from
   first_row to last_row are drawn: each thread walks the whole list for a band of them (draw_sprites) */
static void draw_sprite_list(int number, int first_row, int last_row)
{
	/* (WIDE: the board's screen of 320 in the middle of a wider one, the sprites placed that much to the right) */
	const bool board_wide = bit(seen.sprite_control_latched[6], 0);
	const bool wide = video.wide_side != 0 && !board_wide;
	const int side = video.sprite_side;
	const area screen = { 0, board_wide ? 415 : 319 + 2 * side, first_row, last_row };
	area inside = screen;
	area outside = { 0, -1, 0, -1 };
	int x_offset = 0;
	int y_offset = 0;

	for (int entries = 0; entries < 0x20000 / 16; entries++)
	{
		const uint32_t entry = 8 * (uint32_t)(number & 0x1fff);
		const uint16_t command = sprite_word(entry);

		switch (command >> 14)
		{
			case 0:
				number += 1 + draw_sprite(entry, x_offset, y_offset, &inside, &outside);
				break;
			case 1:
				if (bit(command, 12))
				{
					inside.left = sign_extend(sprite_word(entry + 2), 12);
					inside.right = sign_extend(sprite_word(entry + 3), 12);
					inside.top = sign_extend(command, 12);
					inside.bottom = sign_extend(sprite_word(entry + 1), 12);
					if (wide && inside.left <= 0 && inside.right >= 319)
					{
						inside.left = 0;
						inside.right = screen.right;    /* (the whole screen: the wide one) */
					}
					else
					{
						inside.left += side;
						inside.right += side;
					}
					clip_area(&inside, &screen);
				}
				if (bit(command, 13))
				{
					outside.left = sign_extend(sprite_word(entry + 6), 12) + side;
					outside.right = sign_extend(sprite_word(entry + 7), 12) + side;
					outside.top = sign_extend(sprite_word(entry + 4), 12);
					outside.bottom = sign_extend(sprite_word(entry + 5), 12);
				}
				number++;
				break;
			case 2:
				if (bit(command, 13))
				{
					y_offset = sign_extend(sprite_word(entry + 1), 12);
					x_offset = sign_extend(sprite_word(entry + 2), 12);
				}
				number = command & 0x1fff;
				break;
			default:
				return;
		}
	}
}

static void draw_sprite_list_rows(void *context, int first_row, int last_row)
{
	draw_sprite_list(*(const int *)context, first_row, last_row);
}

/* The list from its entry 'number', a band of its rows on each of the workers' threads; on this one alone
   while an observer watches the sprites (it sees them in the list's order) */
static void draw_sprites(int number)
{
	const bool wide = video.wide_side != 0 && !bit(seen.sprite_control_latched[6], 0);

	video.sprite_side = wide ? video.wide_side : 0;
	if (s32_video_sprite_observer != NULL)
		draw_sprite_list(number, 0, HEIGHT - 1);
	else
		s32_workers_rows(draw_sprite_list_rows, &number, 0, HEIGHT - 1);
}

/* What s32_video_sprites asks for, done */
static void sprites_now(bool erase, bool draw)
{
	if (erase)
		for (int screen = 0; screen < S32_SCREENS; screen++)
		{
			uint16_t *shown = &video.sprites[screen][video.shown_sprites][0][0];

			for (size_t index = 0; index < (size_t)HEIGHT * WIDTH; index++)
				shown[index] = 0xffff;
			if (video.extra_used)
				memset(video.extra_pixels[screen][video.shown_sprites], 0, sizeof(video.extra_pixels[screen][0]));
		}
	if (draw)
	{
		video.shown_sprites ^= 1;
		draw_sprites(0);
	}
}

/* ---- a port's sprites over the frame ---- */

#define OVERLAY_ENTRY 0x1F00    /* where the overlay's list goes in sprite RAM, for the time it is drawn */

static uint16_t overlay_saved[S32_SCREENS][HEIGHT][WIDTH];
static bool overlay_on;

void s32_video_overlay_sprites(const uint8_t *list, size_t size)
{
	static uint8_t sprite_ram_saved[S32_SPRITE_RAM_SIZE - OVERLAY_ENTRY * 16];
	uint8_t *place = &s32_board.sprite_ram[OVERLAY_ENTRY * 16];

	if (size > sizeof(sprite_ram_saved))
		size = sizeof(sprite_ram_saved);
	finish_work();
	if (!overlay_on)
		for (int screen = 0; screen < S32_SCREENS; screen++)
			memcpy(overlay_saved[screen], video.sprites[screen][video.shown_sprites], sizeof(overlay_saved[screen]));
	overlay_on = true;
	memcpy(sprite_ram_saved, place, size);
	memcpy(place, list, size);
	see_sprites();               /* (the list is drawn from the copy) */
	video.shown_sprites ^= 1;    /* (the list draws into the buffer not shown: this one) */
	draw_sprites(OVERLAY_ENTRY);
	video.shown_sprites ^= 1;
	memcpy(place, sprite_ram_saved, size);
}

void s32_video_overlay_end(void)
{
	if (!overlay_on)
		return;
	finish_work();
	overlay_on = false;
	for (int screen = 0; screen < S32_SCREENS; screen++)
		memcpy(video.sprites[screen][video.shown_sprites], overlay_saved[screen], sizeof(overlay_saved[screen]));
}

/* ---- the mixer ---- */

/* Which color offset a layer takes (0x3E bit 15 and the layer's bit there pick how its own flag counts) */
static uint8_t color_offsets(int screen, bool layer_bit, bool layer_flag)
{
	switch ((bit(mixer_register(screen, 0x3e), 15) ? 2 : 0) | (layer_bit ? 1 : 0))
	{
		case 1: return 2;
		case 2: return layer_flag ? 0 : 2;
		default: return layer_flag ? 0 : 1;
	}
}

/* The sprite groups a layer blends with: the group equal to a value, at most it, at least it, or all */
static uint16_t sprite_blend(int encoding)
{
	const int value = encoding & 0x0f;

	switch ((encoding >> 4) & 3)
	{
		case 0: return (uint16_t)(1 << value);
		case 1: return (uint16_t)((1 << value) | ((1 << value) - 1));
		case 2: return (uint16_t)(~((1 << value) - 1) & 0xffff);
		default: return 0xffff;
	}
}

typedef struct sprite_grouping
{
	uint8_t shift;      /* where the group is in a sprite pixel */
	uint8_t mask;
	uint8_t base;       /* the first group's priority register */
} sprite_grouping;

/* The sprite groupings register 0x4C picks */
static const sprite_grouping groupings[16] = {
	{ 14, 0x00, 0x01 }, { 14, 0x01, 0x02 }, { 13, 0x03, 0x04 }, { 12, 0x07, 0x08 },
	{ 14, 0x01, 0x00 }, { 13, 0x03, 0x00 }, { 12, 0x07, 0x00 }, { 11, 0x0f, 0x00 },
	{ 14, 0x01, 0x00 }, { 13, 0x03, 0x00 }, { 12, 0x07, 0x00 }, { 11, 0x0f, 0x00 },
	{ 13, 0x01, 0x00 }, { 12, 0x03, 0x00 }, { 11, 0x07, 0x00 }, { 10, 0x0f, 0x00 }
};

/* The first layer down the list that is not transparent there; the backdrop always is. A sprite pixel
   all ones lets the layers below show, and the shadow pen darkens them. */
static const mix_layer *top_layer(const mix_layer *entry, const uint16_t *const *rows, int x, int sprite_pixel,
	int shadow_mask, int sprite_bits, int shadow_pen, int *pixel, int *shadow)
{
	for (;; entry++)
	{
		if (entry->index != LAYER_SPRITES)
		{
			*pixel = rows[entry->index][x] & 0x1fff;
			if (*pixel != 0 || entry->index == LAYER_BACKDROP)
				return entry;
			continue;
		}
		*pixel = sprite_pixel;
		*shadow = ~sprite_pixel & shadow_mask;
		if ((sprite_pixel & 0x7fff) != 0x7fff)
		{
			*pixel = sprite_pixel & sprite_bits;
			if ((*pixel & 0x7ffe) != shadow_pen)
				return entry;
			*shadow = 1;
		}
	}
}

static uint16_t palette_color(int screen, const mix_layer *entry, int pixel)
{
	return seen.palette[screen][(entry->palette_base + ((pixel >> entry->shift) & 0xfff0) + (pixel & 0x0f)) & 0x3fff];
}

/* (the two below are asked by tools while the sprites are drawn and when a frame is: they answer with the
   board's own registers and palette as they are now, not with the copies a frame is drawn from) */
static uint16_t board_mixer_register(int screen, uint32_t offset)
{
	return s32_board.mixer[screen][offset / 2];
}

static uint16_t board_color(int screen, const mix_layer *entry, int pixel)
{
	return s32_board.palette[screen][(entry->palette_base + ((pixel >> entry->shift) & 0xfff0) + (pixel & 0x0f)) & 0x3fff];
}

uint16_t s32_video_tile_pen_color(int screen, int tilemap, uint16_t color, int pen)
{
	const uint16_t control = board_mixer_register(screen, 0x20 + 2 * (uint32_t)(LAYER_NBG0 + tilemap));
	mix_layer layer;

	layer.palette_base = (uint16_t)((control & 0x00f0) << 6);
	layer.shift = (control >> 8) & 3;
	return board_color(screen, &layer, ((color & 0x1ff) << 4 | (pen & 0x0f)) & 0x1fff);
}

uint16_t s32_video_sprite_pixel_color(int screen, uint16_t pixel)
{
	const uint16_t sprite_control = board_mixer_register(screen, 0x4c);
	const sprite_grouping grouping = groupings[sprite_control & 0x0f];
	const int group = (pixel >> grouping.shift) & grouping.mask;
	const uint16_t control = board_mixer_register(screen, 2 * (uint32_t)(grouping.base | group));
	mix_layer sprites;

	sprites.palette_base = (uint16_t)((((sprite_control & 3) != 3) ? control : sprite_control) & 0x00f0) << 6;
	sprites.shift = (control >> 8) & 3;
	return board_color(screen, &sprites, pixel & (((1 << grouping.shift) - 1) & 0x3fff));
}

/* What a screen's mixing reads, the same on every row (its rows on the workers' bands) */
typedef struct mixing
{
	int screen;
	int blend_factor;
	sprite_grouping grouping;
	int shadow_mask;
	int sprite_bits;
	int shadow_pen;
	uint8_t sprite_flips;
	int offsets[3][3];
	mix_layer orders[16][8];
	/* the same orders taken apart, for the rows' short way: the layers without the sprites, from the top
	   down to the backdrop; each group's sprites, and the layer they go in right above; and every channel
	   of a color with its offset added, darkened under a shadow or not, clamped and put where a frame's
	   pixel has it (by shadow, color offsets, channel and the palette's five bits) */
	mix_layer layers[8];
	int count;
	mix_layer sprites[16];
	uint8_t place[16];
	uint32_t channels[2][3][3][32];
} mixing;

#define SPRITES_UNSEEN 0xff    /* a group's place when it is under the backdrop */

/* A screen's frame from the layers and its sprites: registers 0x00-0x1E give each sprite group its priority and palette, 0x20-0x2C
   each layer, 0x30-0x3A what each blends with, 0x40-0x4A two color offsets, 0x4C the sprite grouping and
   shadows, 0x4E blending */
static void mix(int screen, int drawn)
{
	const uint16_t blend_control = mixer_register(screen, 0x4e);
	const bool blending = bit(blend_control, 11);
	const int blend_factor = (blend_control >> 8) & 7;
	const uint16_t sprite_control = mixer_register(screen, 0x4c);
	const sprite_grouping grouping = groupings[sprite_control & 0x0f];
	const int shadow_mask = bit(sprite_control, 2) ? 0x8000 : 0;
	const int sprite_bits = ((1 << grouping.shift) - 1) & 0x3fff;
	const int shadow_pen = 0x7ffe & sprite_bits;
	const uint8_t sprite_flips = seen.sprite_flips;
	static mixing setup;
	int (*const offsets)[3] = setup.offsets;
	mix_layer sorted[8];
	mix_layer (*const orders)[8] = setup.orders;
	int count = 0;

	for (int channel = 0; channel < 3; channel++)
	{
		offsets[0][channel] = sign_extend(mixer_register(screen, 0x40 + 2 * (uint32_t)channel), 6);
		offsets[1][channel] = sign_extend(mixer_register(screen, 0x46 + 2 * (uint32_t)channel), 6);
		offsets[2][channel] = 0;
	}

	/* the layers that are on and have a priority, then the backdrop, from the top down */
	for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
	{
		const uint16_t control = mixer_register(screen, 0x20 + 2 * (uint32_t)index);
		const uint16_t blend = mixer_register(screen, 0x30 + 2 * (uint32_t)index);

		if (!bit((uint32_t)drawn, index) || (control & 0x0f) == 0)
			continue;
		sorted[count].index = (uint8_t)index;
		sorted[count].priority = (uint8_t)layer_rank(control, index);
		sorted[count].palette_base = (uint16_t)((control & 0x00f0) << 6);
		sorted[count].shift = (control >> 8) & 3;
		sorted[count].blend = blending ? (uint8_t)((blend >> 6) & 0xff) : 0;
		sorted[count].sprite_blend = sprite_blend(blend & 0x3f);
		sorted[count].offsets = color_offsets(screen, bit(mixer_register(screen, 0x3e), index), bit(blend, 14));
		count++;
	}
	sorted[count].index = LAYER_BACKDROP;
	sorted[count].priority = 1 << 3;
	sorted[count].palette_base = (uint16_t)((mixer_register(screen, 0x2c) & 0x00f0) << 6);
	sorted[count].shift = (mixer_register(screen, 0x2c) >> 8) & 3;
	sorted[count].blend = 0;
	sorted[count].sprite_blend = 0;
	sorted[count].offsets = color_offsets(screen, bit(mixer_register(screen, 0x3e), 8), bit(mixer_register(screen, 0x3e), 14));
	count++;
	for (int index = 0; index < count; index++)
		for (int other = index + 1; other < count; other++)
			if (sorted[other].priority > sorted[index].priority)
			{
				const mix_layer swap = sorted[other];

				sorted[other] = sorted[index];
				sorted[index] = swap;
			}

	/* each sprite group goes in above the first layer of a lower priority */
	for (int group = 0; group <= grouping.mask; group++)
	{
		const uint16_t control = mixer_register(screen, 2 * (uint32_t)(grouping.base | group));
		const int priority = (control & 0x0f) << 3 | 7;
		int place = count;
		int copied = 0;
		mix_layer *sprites;

		for (int index = 0; index < count; index++)
		{
			if (priority > sorted[index].priority && place == count)
				place = copied++;
			orders[group][copied++] = sorted[index];
		}
		sprites = &orders[group][place];
		sprites->index = LAYER_SPRITES;
		sprites->priority = (uint8_t)priority;
		sprites->palette_base = (uint16_t)((((sprite_control & 3) != 3) ? control : sprite_control) & 0x00f0) << 6;
		sprites->shift = (control >> 8) & 3;
		sprites->blend = 0;
		sprites->sprite_blend = 0;
		sprites->offsets = color_offsets(screen, bit(mixer_register(screen, 0x3e), 6), bit(sprite_control, 15));
		setup.sprites[group] = *sprites;
		setup.place[group] = (uint8_t)place;
	}

	/* the short way's: the list ends at the backdrop, which nothing shows through, and sprites placed
	   under it are never seen */
	setup.count = 0;
	while (sorted[setup.count].index != LAYER_BACKDROP)
		setup.count++;
	setup.count++;
	for (int index = 0; index < setup.count; index++)
		setup.layers[index] = sorted[index];
	for (int group = 0; group <= grouping.mask; group++)
		if (setup.place[group] >= setup.count)
			setup.place[group] = SPRITES_UNSEEN;
	for (int set = 0; set < 3; set++)
		for (int channel = 0; channel < 3; channel++)
			for (int value = 0; value < 32; value++)
			{
				const int lit = value + offsets[set][channel];
				const int dark = shift_down(lit, 1);

				setup.channels[0][set][channel][value] = (uint32_t)(lit > 31 ? 31 : (lit > 0 ? lit : 0)) << (19 - 8 * channel);
				setup.channels[1][set][channel][value] = (uint32_t)(dark > 31 ? 31 : (dark > 0 ? dark : 0)) << (19 - 8 * channel);
			}

	setup.screen = screen;
	setup.blend_factor = blend_factor;
	setup.grouping = grouping;
	setup.shadow_mask = shadow_mask;
	setup.sprite_bits = sprite_bits;
	setup.shadow_pen = shadow_pen;
	setup.sprite_flips = sprite_flips;
	pipeline.mix = &setup;
}

/* One pixel the long way, down its sprite group's whole order: for the pixels the short way leaves (a top
   layer that blends, a port's marks on a sprite), with all the mixer can do to them */
static uint32_t mixed_pixel(const mixing *setup, const uint16_t *const *rows, const uint8_t *marks_row, int x, int sprite_x)
{
	const int screen = setup->screen;
	const int blend_factor = setup->blend_factor;
	const int (*const offsets)[3] = setup->offsets;
	const int sprite_pixel = rows[LAYER_SPRITES][sprite_x];
	const int group = (sprite_pixel >> setup->grouping.shift) & setup->grouping.mask;
	int shadow = 0;
	int pixel;
	const mix_layer *first = top_layer(setup->orders[group], rows, x, sprite_pixel, setup->shadow_mask, setup->sprite_bits,
		setup->shadow_pen, &pixel, &shadow);
	uint16_t color = palette_color(screen, first, pixel);
	const uint8_t marks = marks_row != NULL && first->index == LAYER_SPRITES ? marks_row[sprite_x] : 0;

	if (marks & EXTRA_COLOURS)
	{
		const int index = (first->palette_base + ((pixel >> first->shift) & 0xfff0) + (pixel & 0x0f)) & 0x3fff;

		if (video.extra_set[screen][index])
			color = video.extra_palette[screen][index];
	}
	int red = (color & 0x1f) + offsets[first->offsets][0];
	int green = ((color >> 5) & 0x1f) + offsets[first->offsets][1];
	int blue = ((color >> 10) & 0x1f) + offsets[first->offsets][2];

	if (first->blend != 0)
	{
		int below;
		const mix_layer *second = top_layer(first + 1, rows, x, sprite_pixel, setup->shadow_mask, setup->sprite_bits,
			setup->shadow_pen, &below, &shadow);

		if (bit(first->blend, second->index) && (second->index != LAYER_SPRITES || bit(first->sprite_blend, group)))
		{
			color = palette_color(screen, second, below);
			red = shift_down(red * (7 - blend_factor) + ((color & 0x1f) + offsets[second->offsets][0]) * (blend_factor + 1), 3);
			green = shift_down(green * (7 - blend_factor) + (((color >> 5) & 0x1f) + offsets[second->offsets][1]) * (blend_factor + 1), 3);
			blue = shift_down(blue * (7 - blend_factor) + (((color >> 10) & 0x1f) + offsets[second->offsets][2]) * (blend_factor + 1), 3);
		}
	}
	if (marks & EXTRA_GHOST)
	{
		/* a ghost: half its colour, half the colour of the layer under it */
		int below;
		int ignored_shadow = 0;
		const mix_layer *second = top_layer(first + 1, rows, x, sprite_pixel, setup->shadow_mask, setup->sprite_bits,
			setup->shadow_pen, &below, &ignored_shadow);
		const uint16_t under = palette_color(screen, second, below);

		red = (red + (under & 0x1f) + offsets[second->offsets][0]) / 2;
		green = (green + ((under >> 5) & 0x1f) + offsets[second->offsets][1]) / 2;
		blue = (blue + ((under >> 10) & 0x1f) + offsets[second->offsets][2]) / 2;
	}
	if (shadow)
	{
		red = shift_down(red, 1);
		green = shift_down(green, 1);
		blue = shift_down(blue, 1);
	}
	red = red > 31 ? 31 : (red > 0 ? red : 0);
	green = green > 31 ? 31 : (green > 0 ? green : 0);
	blue = blue > 31 ? 31 : (blue > 0 ? blue : 0);
	return (uint32_t)red << 19 | (uint32_t)green << 11 | (uint32_t)blue << 3;
}

#define SPRITES_SEEN 256    /* the sprite colors the rows keep, and their top layer's (powers of two) */
#define OVER_SEEN    128

/* A row's mixing: of the screen's layers, the ones with something on this row, from the top down to the
   backdrop, and where each sprite group goes among them. Most rows have one or two, so a pixel's top layer
   is found in a step or two instead of a walk down every layer the mixer lists. */
typedef struct row_mixing
{
	const mixing *setup;
	const uint16_t *pixels[8];      /* each layer's row */
	const mix_layer *layers[8];
	int tiles;                      /* how many they are, without the backdrop (the last) */
	int backdrop;                   /* its color on this row */
	uint8_t place[16];              /* how many of them are above a group's sprites, or SPRITES_UNSEEN */
	const mix_layer *sprites;       /* the groups' own entries */
	sprite_grouping grouping;
	int shadow_mask;
	int sprite_bits;
	int shadow_pen;
	/* the long way's: every layer's row by LAYER_, the sprites' marks if the row has any, and whether the
	   backdrop's row is filled yet */
	const uint16_t *rows[LAYERS];
	const uint8_t *marks_row;
	int y;
	bool backdrop_filled;
	bool kept;                      /* whether what follows is of an earlier row, or of none yet */
	/* a sprite's pixel over every layer of the row is a color of its own, whatever is under it: the same
	   few along a row and down the rows, so each is worked out once and kept here by the pixel it came
	   from, at seen_slot() of it. They are kept from a row to the next (down a worker's band of the
	   screen: draw_and_mix_rows) while the sprites' places among the layers stay as they were; not on a
	   row with a port's marks, where a pixel is more than its value. (0xffff, no sprite, is never kept or
	   looked for.) */
	uint16_t seen[SPRITES_SEEN];
	uint32_t seen_color[SPRITES_SEEN];
	/* and so is a pixel of the one layer of a row that has one (mix_open_row), where no sprite is: kept
	   by the layer's pixel at over_slot() of it, while the rows' layer is 'over_of'. A pixel of nothing
	   (0) is the backdrop's color, which is the row's own. (0xffff is no layer's pixel.) */
	const mix_layer *over_of;
	uint32_t backdrop_shown;
	uint16_t over[OVER_SEEN];
	uint32_t over_color[OVER_SEEN];
} row_mixing;

static inline unsigned seen_slot(unsigned sprite_pixel)
{
	return (sprite_pixel ^ (sprite_pixel >> 4)) & (SPRITES_SEEN - 1);
}

static inline unsigned over_slot(unsigned pixel)
{
	return (pixel ^ (pixel >> 5)) & (OVER_SEEN - 1);
}

/* top_layer's answer for a pixel, the short way: the same layer, pixel and shadow as walking the group's
   order with the row's empty layers left in it */
static inline const mix_layer *top_of_row(const row_mixing *row, int x, int sprite_pixel, int *pixel, int *shadow)
{
	int at = 0;

	if (sprite_pixel != 0xffff)
	{
		const int group = (sprite_pixel >> row->grouping.shift) & row->grouping.mask;
		const int above = row->place[group];

		if (above != SPRITES_UNSEEN)
		{
			for (; at < above; at++)
			{
				*pixel = row->pixels[at][x] & 0x1fff;
				if (*pixel != 0)
					return row->layers[at];
			}
			*shadow = ~sprite_pixel & row->shadow_mask;
			if ((sprite_pixel & 0x7fff) != 0x7fff)
			{
				*pixel = sprite_pixel & row->sprite_bits;
				if ((*pixel & 0x7ffe) != row->shadow_pen)
					return &row->sprites[group];
				*shadow = 1;
			}
		}
	}
	/* (no sprite there, or one that lets the layers under it show) */
	for (; at < row->tiles; at++)
	{
		*pixel = row->pixels[at][x] & 0x1fff;
		if (*pixel != 0)
			return row->layers[at];
	}
	*pixel = row->backdrop;
	return row->layers[at];
}

/* A color of the palette as a frame's pixel, through a layer's channels: each one's offset, shadow and
   clamp are in the tables */
static inline uint32_t frame_color(const uint32_t (*channels)[32], uint16_t color)
{
	return channels[0][color & 0x1f] | channels[1][(color >> 5) & 0x1f] | channels[2][(color >> 10) & 0x1f];
}

/* One pixel of a row, whatever is there: the rows' loops come here for the pixels they have no short
   answer for, and are kept small by this not being part of them */
static NOINLINE uint32_t mix_pixel(row_mixing *row, int x, const uint16_t *sprite_at)
{
	const mixing *setup = row->setup;
	const int sprite_pixel = *sprite_at;
	const int sprite_x = (int)(sprite_at - row->rows[LAYER_SPRITES]);
	int shadow = 0;
	int pixel;
	const mix_layer *first = top_of_row(row, x, sprite_pixel, &pixel, &shadow);
	uint32_t color;

	if (first->blend != 0 || (row->marks_row != NULL && first->index == LAYER_SPRITES && row->marks_row[sprite_x] != 0))
	{
		/* (the long way reads the backdrop's row: filled the first time one of this row's pixels takes it) */
		if (!row->backdrop_filled)
		{
			uint16_t *backdrop = video.layers[LAYER_BACKDROP].pixels[row->y];

			for (int column = video.visible.left; column <= video.visible.right; column++)
				backdrop[column] = backdrop_colors[row->y];
			row->backdrop_filled = true;
		}
		return mixed_pixel(setup, row->rows, row->marks_row, x, sprite_x);
	}
	color = frame_color(setup->channels[shadow != 0][first->offsets], palette_color(setup->screen, first, pixel));
	if (first->index == LAYER_SPRITES && row->marks_row == NULL &&
		row->place[(sprite_pixel >> row->grouping.shift) & row->grouping.mask] == 0)
	{
		row->seen[seen_slot((unsigned)sprite_pixel)] = (uint16_t)sprite_pixel;
		row->seen_color[seen_slot((unsigned)sprite_pixel)] = color;
	}
	return color;
}

/* The color of a pixel of the one layer of a row (or of none, 0: the backdrop's), worked out and kept */
static NOINLINE uint32_t see_over(row_mixing *row, unsigned pixel)
{
	const mixing *setup = row->setup;
	const mix_layer *layer = row->layers[0];
	const uint32_t color = pixel == 0 ? row->backdrop_shown :
		frame_color(setup->channels[0][layer->offsets], palette_color(setup->screen, layer, (int)pixel));

	row->over[over_slot(pixel)] = (uint16_t)pixel;
	row->over_color[over_slot(pixel)] = color;
	return color;
}

/* The columns 'from' to 'to', 'step' apart, of a row with one layer over the backdrop at most, and that one
   not blending: where there is no sprite, a pixel is that layer's color or the backdrop's, with no shadow
   and no marks. Most of a picture's pixels, and most of them a color already seen, the layer's or a
   sprite's: the loop has those in hand and leaves the rest to see_over and mix_pixel. */
static void mix_open_row(row_mixing *row, uint32_t *out, int from, int to, int step, const uint16_t *sprite_at, int sprite_step)
{
	const mixing *setup = row->setup;
	const mix_layer *const layer = row->layers[0];
	const uint16_t *const pixels = row->tiles == 1 ? row->pixels[0] : transparent_row;
	const uint16_t *const seen = row->seen;
	const uint32_t *const seen_color = row->seen_color;
	const uint16_t *const over = row->over;
	const uint32_t *const over_color = row->over_color;

	if (layer != row->over_of)
	{
		memset(row->over, 0xff, sizeof(row->over));
		row->over_of = layer;    /* (or the backdrop's entry, on a row with no layer: nothing but 0 is kept then) */
	}
	row->backdrop_shown = frame_color(setup->channels[0][row->layers[row->tiles]->offsets],
		palette_color(setup->screen, row->layers[row->tiles], row->backdrop));
	see_over(row, 0);
	for (int x = from; x <= to; x += step, sprite_at += sprite_step)
	{
		const unsigned sprite_pixel = *sprite_at;

		if (sprite_pixel == 0xffff)
		{
			const unsigned pixel = pixels[x] & 0x1fff;

			out[x] = over[over_slot(pixel)] == pixel ? over_color[over_slot(pixel)] : see_over(row, pixel);
		}
		else if (seen[seen_slot(sprite_pixel)] == sprite_pixel)
			out[x] = seen_color[seen_slot(sprite_pixel)];
		else
			out[x] = mix_pixel(row, x, sprite_at);
	}
}

/* The same columns of any other row: the sprites' colors already seen, and the rest a pixel at a time */
static void mix_layers_row(row_mixing *row, uint32_t *out, int from, int to, int step, const uint16_t *sprite_at, int sprite_step)
{
	const uint16_t *const seen = row->seen;
	const uint32_t *const seen_color = row->seen_color;

	for (int x = from; x <= to; x += step, sprite_at += sprite_step)
	{
		const unsigned sprite_pixel = *sprite_at;

		if (sprite_pixel != 0xffff && seen[seen_slot(sprite_pixel)] == sprite_pixel)
			out[x] = seen_color[seen_slot(sprite_pixel)];
		else
			out[x] = mix_pixel(row, x, sprite_at);
	}
}

/* Whether any of a row's marks is set: four at a time, as it is asked of every row */
static bool marked(const uint8_t *marks, int count)
{
	uint32_t any = 0;
	int index = 0;

	for (; index + 4 <= count; index += 4)
	{
		uint32_t four;

		memcpy(&four, &marks[index], sizeof(four));
		any |= four;
	}
	for (; index < count; index++)
		any |= marks[index];
	return any != 0;
}

/* The rows of a screen from its layers and sprites. 'row' is the caller's, the same for the rows it mixes
   one after another: it holds what a row leaves for the next ones. */
static void mix_rows(const mixing *setup, row_mixing *row, int first_row, int last_row)
{
	const int screen = setup->screen;
	const uint8_t sprite_flips = setup->sprite_flips;
	const int left = video.visible.left;
	const int right = video.visible.right;
	const bool half = half_columns;

	/* the sprite buffer is scanned flipped as the sprite controls say */
	for (int y = first_row; y <= last_row; y++)
	{
		const int sprite_y = bit(sprite_flips, 1) ? video.visible.bottom - (y - video.visible.top) : y;
		const uint8_t *const marks = video.extra_pixels[screen][video.shown_sprites][sprite_y];
		uint32_t *out = video.frame[screen][y];
		const uint16_t *sprite_at;
		const int sprite_dx = bit(sprite_flips, 0) ? -1 : 1;
		uint8_t above[9];           /* how many of the row's layers are above each of the screen's */
		uint8_t place[16];
		int kept = 0;
		int from = left;

		for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
			row->rows[index] = video.layers[index].empty[y] ? transparent_row : video.layers[index].pixels[y];
		row->rows[LAYER_SPRITES] = video.sprites[screen][video.shown_sprites][sprite_y];
		row->rows[LAYER_BACKDROP] = video.layers[LAYER_BACKDROP].pixels[y];
		/* (a row of the marks with none on it, as most are, is as good as no marks) */
		row->marks_row = video.extra_used && marked(&marks[left], right + 1 - left) ? marks : NULL;
		row->y = y;
		row->backdrop_filled = false;
		sprite_at = &row->rows[LAYER_SPRITES][bit(sprite_flips, 0) ? right : left];

		for (int index = 0; index < setup->count; index++)
		{
			const mix_layer *entry = &setup->layers[index];

			above[index] = (uint8_t)kept;
			if (row->rows[entry->index] == transparent_row)
				continue;
			row->layers[kept] = entry;
			row->pixels[kept] = row->rows[entry->index];
			kept++;
		}
		row->setup = setup;
		row->tiles = kept - 1;
		row->backdrop = backdrop_colors[y] & 0x1fff;
		for (int group = 0; group < 16; group++)
			place[group] = group > setup->grouping.mask || setup->place[group] == SPRITES_UNSEEN ? SPRITES_UNSEEN :
				above[setup->place[group]];
		/* (the sprites' colors seen are theirs over every layer: good for this row too if each group is
		   where it was among the row's layers) */
		if (!row->kept || row->marks_row != NULL || memcmp(place, row->place, sizeof(place)) != 0)
			memset(row->seen, 0xff, sizeof(row->seen));
		if (!row->kept)
			row->over_of = NULL;
		row->kept = true;
		memcpy(row->place, place, sizeof(place));
		row->sprites = setup->sprites;
		row->grouping = setup->grouping;
		row->shadow_mask = setup->shadow_mask;
		row->sprite_bits = setup->sprite_bits;
		row->shadow_pen = setup->shadow_pen;

		const bool open = row->tiles == 0 || (row->tiles == 1 && row->layers[0]->blend == 0);

		/* half the columns: the even ones are worked out (and the first, wherever it is), and each of the
		   others then shows the one before it */
		if (half && (left & 1))
		{
			out[left] = mix_pixel(row, left, sprite_at);
			from = left + 1;
			sprite_at += sprite_dx;
		}
		if (open)
			mix_open_row(row, out, from, right, half ? 2 : 1, sprite_at, half ? 2 * sprite_dx : sprite_dx);
		else
			mix_layers_row(row, out, from, right, half ? 2 : 1, sprite_at, half ? 2 * sprite_dx : sprite_dx);
		if (half)
			for (int x = (left + 1) | 1; x <= right; x += 2)
				out[x] = out[x - 1];
	}
}

/* A few rows through every step, in the order they were added, the layers and then the mix; and so on down
   the rows given, a worker's band of the screen. A few and not one, so that each step is called a quarter
   as often; not more, so that the rows are still in the cache when the mix comes to them (3 KB each or so). */
#define ROWS_TOGETHER 4

static void draw_and_mix_rows(void *context, int first, int last)
{
	row_mixing kept;    /* (this call's own, as its rows are: what the mix keeps from a row to the next) */

	(void)context;
	kept.kept = false;
	for (int y = first; y <= last; y += ROWS_TOGETHER)
	{
		const int end = y + ROWS_TOGETHER - 1 < last ? y + ROWS_TOGETHER - 1 : last;

		for (int step = 0; step < pipeline.count; step++)
			pipeline.work[step](pipeline.context[step], y, end);
		mix_rows((const mixing *)pipeline.mix, &kept, y, end);
	}
}

/* ---- the frame ---- */

static void frame_now(void)
{
	const bool board_wide = bit(video_register(0x00), 15);
	const int width = board_wide ? 416 : 320 + 2 * video.wide_side;

	video.side = board_wide ? 0 : video.wide_side;
	video.visible.left = 0;
	video.visible.right = width - 1;
	video.visible.top = 0;
	video.visible.bottom = HEIGHT - 1;

	/* each screen's I/O chip turns it on (CNT1); the layers they share are drawn once, those the mixers of
	   the screens shown take (a layer with no priority is not mixed); a screen the port does not show keeps
	   its last frame */
	int wanted = 0;
	int drawn = -1;

	for (int screen = 0; screen < S32_SCREENS; screen++)
		if (bit((uint32_t)video.screens_shown, screen) && seen.display_enabled[screen])
			for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
				if ((mixer_register(screen, 0x20 + 2 * (uint32_t)index) & 0x0f) != 0)
					wanted |= 1 << index;
	for (int screen = 0; screen < S32_SCREENS; screen++)
	{
		if (!bit((uint32_t)video.screens_shown, screen))
			continue;
		if (!seen.display_enabled[screen])
		{
			for (int y = 0; y < HEIGHT; y++)
				memset(video.frame[screen][y], 0, (size_t)width * sizeof(video.frame[screen][y][0]));
			continue;
		}
		pipeline.count = 0;
		if (drawn < 0)
			drawn = draw_layers(wanted);    /* (each layer adds its row function; the backdrop is drawn here) */
		mix(screen, drawn);                 /* (and the mix adds its own, after them) */
		/* The second screen shares the layers, so draw_layers is not called again: its pipeline holds the
		   mix alone, and it reads the pixels the first screen's pass left in the buffers. */
		s32_workers_rows_by(draw_and_mix_rows, NULL, video.visible.top, video.visible.bottom, ROWS_AT_A_TIME);
	}
}

/* ---- the work, now or on the workers' threads ---- */

/* What the board asks for is done by sprites_now and frame_now, from the copies see_sprites and see_frame took:
   at once, or with overlap as tasks of the workers' threads */

static struct
{
	bool erase;
	bool draw;
} sprites_asked;    /* the queued list's */

static void sprites_task_work(void *context)
{
	(void)context;
	sprites_now(sprites_asked.erase, sprites_asked.draw);
}

void s32_video_sprites(bool erase, bool draw)
{
	/* (an observer sees the sprites when the board sends them, in the list's order) */
	const bool later = overlap && s32_video_sprite_observer == NULL;

	if (!later)
		finish_work();
	else if (work_queued)
		s32_workers_finish(sprites_task);    /* (the list before this one: the copy it is drawn from is this one's) */
	if (draw)
		see_sprites();
	if (!later)
	{
		sprites_now(erase, draw);
		return;
	}
	/* after the frame being drawn, which shows the buffer this list is erased and drawn in: the tasks are done
	   in their order */
	sprites_asked.erase = erase;
	sprites_asked.draw = draw;
	sprites_task = s32_workers_queue(sprites_task_work, NULL);
	work_queued = true;
}

static void frame_task_work(void *context)
{
	(void)context;
	frame_now();
}

void s32_video_draw_frame_begin(void)
{
	/* the frame before this one and the sprites sent since are done first: this frame is drawn into the same
	   buffers, and shows those sprites */
	finish_work();
	see_frame();
	if (overlap && s32_video_tile_observer == NULL)
	{
		frame_task = s32_workers_queue(frame_task_work, NULL);
		work_queued = true;
	}
	else
		frame_now();    /* (an observer sees the tilemaps as the frame is asked for) */
}

void s32_video_draw_frame_end(void)
{
	if (work_queued)
		s32_workers_finish(frame_task);
}

void s32_video_draw_frame(void)
{
	s32_video_draw_frame_begin();
	s32_video_draw_frame_end();
}

void s32_video_set_overlap(bool on)
{
	if (on != overlap)
	{
		finish_work();
		overlap = on;
	}
}

void s32_video_show_screens(int screens)
{
	const int shown = screens & ((1 << S32_SCREENS) - 1);

	if (shown != video.screens_shown)
	{
		finish_work();
		video.screens_shown = shown;
	}
}

void s32_video_init_graphics(const s32_graphics *graphics)
{
	uint8_t *image_pens = video.image_pens;
	const uint8_t **image_tiles = video.image_tiles;
	const uint8_t *sprite_rom = video.sprite_rom;
	const uint32_t sprite_rom_words = video.sprite_rom_words;

	finish_work();
	memset(&video, 0, sizeof(video));
	video.screens_shown = (1 << S32_SCREENS) - 1;
	video.graphics = *graphics;
	video.image_pens = image_pens;
	video.image_tiles = image_tiles;
	video.sprite_rom = sprite_rom;
	video.sprite_rom_words = sprite_rom_words;
	video.visible.right = 319;
	video.visible.bottom = HEIGHT - 1;
	s32_board.sprites_due = s32_video_sprites;
	s32_board.frame_shown = s32_video_draw_frame;
}

/* A word of the sprite chips' image s32_video_init was given */
static uint32_t image_sprite_word(uint32_t place)
{
	return big_endian_word(&video.sprite_rom[(size_t)(place % video.sprite_rom_words) * 4]);
}

void s32_video_init(const uint8_t *tiles, size_t tiles_size, const uint8_t *sprites, size_t sprites_size)
{
	/* a tile's rows of 8 bytes: two pens a byte with the high nibble first, and the byte pairs of each half
	   of the row swapped */
	static const uint8_t byte_of_column[16] = { 0, 0, 2, 2, 1, 1, 3, 3, 4, 4, 6, 6, 5, 5, 7, 7 };
	const uint32_t count = (uint32_t)(tiles_size / 128);
	s32_graphics graphics;

	finish_work();
	free(video.image_pens);
	free((void *)video.image_tiles);
	video.image_pens = malloc((size_t)count * 256);
	video.image_tiles = malloc((size_t)count * sizeof(video.image_tiles[0]));
	if (!video.image_pens || !video.image_tiles)
		abort();
	for (uint32_t tile = 0; tile < count; tile++)
	{
		uint8_t *pens = &video.image_pens[(size_t)tile * 256];

		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++)
			{
				const uint8_t value = tiles[(size_t)tile * 128 + y * 8 + byte_of_column[x]];

				pens[y * 16 + x] = (x & 1) ? (value & 0x0f) : (value >> 4);
			}
		video.image_tiles[tile] = pens;
	}
	video.sprite_rom = sprites;
	video.sprite_rom_words = (uint32_t)(sprites_size / 4);
	graphics.tile_pens = video.image_tiles;
	graphics.tile_count = count;
	graphics.sprite_word = image_sprite_word;
	graphics.sprite_banks = video.sprite_rom_words >> 20;
	s32_video_init_graphics(&graphics);
}

const uint32_t *s32_video_frame(int screen, int *width)
{
	s32_video_draw_frame_end();
	if (width)
		*width = video.visible.right + 1;
	return &video.frame[screen & 1][0][0];
}

void s32_video_use_extra_palette(bool used)
{
	if (used != video.extra_used)
	{
		finish_work();
		video.extra_used = used;
	}
}

void s32_video_extra_colour(int screen, uint32_t index, uint16_t colour)
{
	/* (the game sets the same colours again and again: only a new one waits for the work queued) */
	if (video.extra_set[screen & 1][index & 0x3fff] && video.extra_palette[screen & 1][index & 0x3fff] == colour)
		return;
	finish_work();
	video.extra_palette[screen & 1][index & 0x3fff] = colour;
	video.extra_set[screen & 1][index & 0x3fff] = true;
}

void s32_video_set_wide_side(int side)
{
	side = side < 0 ? 0 : (side > S32_VIDEO_WIDE_SIDE_MOST ? S32_VIDEO_WIDE_SIDE_MOST : side);
	if (side != video.wide_side)
	{
		finish_work();
		video.wide_side = side;
	}
}

void s32_video_set_half_columns(bool on)
{
	if (on != half_columns)
	{
		finish_work();
		half_columns = on;
	}
}

int s32_video_side(void)
{
	return video.side;
}

uint64_t s32_video_frame_hash(void)
{
	uint64_t hash = 0xcbf29ce484222325ull;

	s32_video_draw_frame_end();
	for (int screen = 0; screen < S32_SCREENS; screen++)
		for (int y = 0; y < HEIGHT; y++)
			for (int x = 0; x <= video.visible.right; x++)
				for (int shift = 0; shift < 24; shift += 8)
					hash = (hash ^ ((video.frame[screen][y][x] >> shift) & 0xff)) * 0x100000001b3ull;
	return hash;
}
