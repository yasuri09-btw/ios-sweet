#pragma once
#include <stdint.h>

#define FB_BASE		0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W		1080
#define FB_H		2160	/* bonito: 1080x2160 sofef00/nt37700f */
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
void con_scale(int s);		/* glyph scale (default 3; 2 = 64 cols x 66 rows) */
void con_printf(const char *fmt, ...);	/* one screen line per '\n' */
void con_set_stride(uint32_t bytes);	/* panel row pitch after a stride change */
void con_home(void);			/* next line at the top, no clear */
