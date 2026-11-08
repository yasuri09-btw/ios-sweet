/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

static int SCALE = 3;	/* con_scale() */
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}

void con_scale(int s)
{
	SCALE = s;
}

/* Minimal printf: %s %c %d %u %x %lx %llx %lu %ld %%, optional 0-pad width for x. */
static int fmt_num(char *o, uint64_t v, int base, int neg, int width)
{
	char t[24];
	int n = 0, k = 0;
	do {
		t[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (n < width)
		t[n++] = '0';
	if (neg)
		o[k++] = '-';
	while (n)
		o[k++] = t[--n];
	return k;
}

void con_vprintf(const char *f, __builtin_va_list ap)
{
	char line[96];
	int n = 0;
	for (; *f && n < (int)sizeof(line) - 24; f++) {
		if (*f == '\n') {
			line[n] = 0;
			put_str(0, line, WHITE);
			row++;
			n = 0;
			continue;
		}
		if (*f != '%') {
			line[n++] = *f;
			continue;
		}
		f++;
		int width = 0, lng = 0;
		while (*f >= '0' && *f <= '9')
			width = width * 10 + (*f++ - '0');
		while (*f == 'l') {
			lng++;
			f++;
		}
		switch (*f) {
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s && n < (int)sizeof(line) - 24)
				line[n++] = *s++;
			break;
		}
		case 'c':
			line[n++] = (char)__builtin_va_arg(ap, int);
			break;
		case 'd': {
			int64_t v = lng ? __builtin_va_arg(ap, int64_t) : __builtin_va_arg(ap, int);
			n += fmt_num(line + n, v < 0 ? (uint64_t)-v : (uint64_t)v, 10, v < 0, width);
			break;
		}
		case 'u':
			n += fmt_num(line + n, lng ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, unsigned), 10, 0, width);
			break;
		case 'x':
			n += fmt_num(line + n, lng ? __builtin_va_arg(ap, uint64_t) : __builtin_va_arg(ap, unsigned), 16, 0, width);
			break;
		default:
			line[n++] = *f;
		}
	}
	if (n) {
		line[n] = 0;
		put_str(0, line, WHITE);
		row++;
	}
}

void con_printf(const char *f, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, f);
	con_vprintf(f, ap);
	__builtin_va_end(ap);
}
