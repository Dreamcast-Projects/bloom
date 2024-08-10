// SPDX-License-Identifier: GPL-2.0-only
/*
 * PowerVR powered hardware renderer - gpulib interface
 *
 * Copyright (C) 2024 Paul Cercueil <paul@crapouillou.net>
 */

#include <dc/pvr.h>
#include <gpulib/gpu.h>
#include <gpulib/gpu_timing.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_WIDTH 1024
#define FRAME_HEIGHT 512

#define DEBUG 0

#if DEBUG
#  define pvr_printf(...) printf(__VA_ARGS__)
#else
#  define pvr_printf(...)
#endif

extern float screen_fw, screen_fh;
extern uint32_t pvr_dr_state;

union PacketBuffer {
	uint32_t U4[16];
	uint16_t U2[32];
	uint8_t  U1[64];
};

struct pvr_renderer {
	uint32_t gp1;

	uint16_t draw_x1;
	uint16_t draw_y1;
	uint16_t draw_x2;
	uint16_t draw_y2;

	int16_t draw_dx;
	int16_t draw_dy;

	uint32_t set_mask :1;
	uint32_t check_mask :1;
};

static struct pvr_renderer pvr;

int renderer_init(void)
{
	pvr_printf("PVR renderer init\n");

	gpu.vram = aligned_alloc(32, 1024 * 1024);

	memset(&pvr, 0, sizeof(pvr));
	pvr.gp1 = 0x14802000;

	return 0;
}

void renderer_finish(void)
{
	free(gpu.vram);
}

void renderer_sync_ecmds(uint32_t *ecmds)
{
	int dummy;
	do_cmd_list(&ecmds[1], 6, &dummy, &dummy, &dummy);
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
}

void renderer_flush_queues(void)
{
}

void renderer_sync(void)
{
}

void renderer_notify_res_change(void)
{
}

void renderer_notify_scanout_change(int x, int y)
{
}

void renderer_notify_update_lace(int updated)
{
}

void renderer_set_config(const struct rearmed_cbs *cbs)
{
}

static void cmd_clear_image(union PacketBuffer *pbuffer)
{
	int32_t x0, y0, w0, h0;
	x0 = pbuffer->U2[2] & 0x3ff;
	y0 = pbuffer->U2[3] & 0x1ff;
	w0 = ((pbuffer->U2[4] - 1) & 0x3ff) + 1;
	h0 = ((pbuffer->U2[5] - 1) & 0x1ff) + 1;

	/* horizontal position / size work in 16-pixel blocks */
	x0 = (x0 + 0xe) & 0xf;
	w0 = (w0 + 0xe) & 0xf;

	/* TODO: Invalidate anything in the framebuffer, texture and palette
	 * caches that are covered by this rectangle */
}

static void * pvr_dr_get(void)
{
	sq_lock((void *)PVR_TA_INPUT);
	return pvr_dr_target(pvr_dr_state);
}

static void pvr_dr_put(void *addr)
{
	pvr_dr_commit(addr);
	sq_unlock();
}

static inline float x_to_pvr(int16_t x)
{
	return (float)(x + pvr.draw_dx - pvr.draw_x1) * screen_fw;
}

static inline float y_to_pvr(int16_t y)
{
	return (float)(y + pvr.draw_dy - pvr.draw_y1) * screen_fh;
}

static void draw_line(int16_t x0, int16_t y0, uint32_t color0,
		      int16_t x1, int16_t y1, uint32_t color1)
{
	unsigned int i, up = y1 < y0;
	int16_t xcoords[6] = {
		x0, x0, x0 + 1, x1, x1 + 1, x1 + 1,
	};
	int16_t ycoords[6] = {
		y0 + up, y0 + !up, y0 + up, y1 + !up, y1 + up, y1 + !up,
	};
	pvr_vertex_t *v;

	for (i = 0; i < 6; i++) {
		v = pvr_dr_get();

		*v = (pvr_vertex_t){
			.flags = (i == 5) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
			.argb = (i < 3) ? color0 : color1,
			.x = x_to_pvr(xcoords[i]),
			.y = y_to_pvr(ycoords[i]),
			.z = 1.0f,
		};

		pvr_dr_put(v);
	}
}

int do_cmd_list(uint32_t *list, int list_len,
		int *cycles_sum_out, int *cycles_last, int *last_cmd)
{
	int cpu_cycles_sum = 0, cpu_cycles = *cycles_last;
	uint32_t cmd = 0, len;
	uint32_t *list_start = list;
	uint32_t *list_end = list + list_len;
	union PacketBuffer pbuffer;
	unsigned int i;

	for (; list < list_end; list += 1 + len)
	{
		cmd = *list >> 24;
		len = cmd_lengths[cmd];
		if (list + 1 + len > list_end) {
			cmd = -1;
			break;
		}

		for (i = 0; i <= len; i++)
			pbuffer.U4[i] = list[i];

		switch (cmd) {
		case 0x02:
			cmd_clear_image(&pbuffer);
			gput_sum(cpu_cycles_sum, cpu_cycles,
				 gput_fill(pbuffer.U2[4] & 0x3ff,
					   pbuffer.U2[5] & 0x1ff));
			break;

		case 0xe1:
			/* Set texture page */
			pvr.gp1 = (pvr.gp1 & ~0x7ff) | (pbuffer.U4[0] & 0x7ff);
			break;

		case 0xe2:
			/* TODO: Set texture window */
			break;

		case 0xe3:
			/* Set top-left corner of drawing area */
			pvr.draw_x1 = pbuffer.U4[0] & 0x3ff;
			pvr.draw_y1 = (pbuffer.U4[0] >> 10) & 0x1ff;
			pvr_printf("Set top-left corner to %ux%u\n",
			       pvr.draw_x1, pvr.draw_y1);
			break;

		case 0xe4:
			/* Set top-left corner of drawing area */
			pvr.draw_x2 = pbuffer.U4[0] & 0x3ff;
			pvr.draw_y2 = (pbuffer.U4[0] >> 10) & 0x1ff;
			pvr_printf("Set bottom-right corner to %ux%u\n",
			       pvr.draw_x2, pvr.draw_y2);
			break;

		case 0xe5:
			/* Set drawing offsets */
			pvr.draw_dx = ((int32_t)pbuffer.U4[0] << 21) >> 21;
			pvr.draw_dy = ((int32_t)pbuffer.U4[0] << 10) >> 21;
			pvr_printf("Set drawing offsets to %dx%d\n",
			       pvr.draw_dx, pvr.draw_dy);
			break;

		case 0xe6:
			/* VRAM mask settings */
			pvr.set_mask = pbuffer.U4[0] & 0x1;
			pvr.check_mask = (pbuffer.U4[0] & 0x2) >> 1;
			break;

		case 0x01:
		case 0x80 ... 0x9f:
		case 0xa0 ... 0xbf:
		case 0xc0 ... 0xdf:
			/* VRAM access commands */
			break;

		case 0x00:
			/* NOP */
			break;

		case 0x20:
		case 0x28:
		case 0x30:
		case 0x38: {
			/* Monochrome/shaded non-textured polygon */
			pvr_poly_cxt_t cxt;
			pvr_poly_hdr_t *hdr;
			pvr_vertex_t *v;
			bool multicolor = cmd & 0x10;
			bool poly4 = cmd & 0x8;
			uint32_t val, *buf = pbuffer.U4;
			unsigned int i, nb = 3 + !!poly4;
			uint32_t color = 0;
			int16_t x, y;

			pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);

			cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
			cxt.gen.culling = PVR_CULLING_NONE;

			hdr = pvr_dr_get();
			pvr_poly_compile(hdr, &cxt);
			pvr_dr_put(hdr);

			for (i = 0; i < nb; i++) {
				if (i == 0 || multicolor) {
					/* BGR->RGB swap */
					color = __builtin_bswap32(*buf++) >> 8;
					pvr_printf("Render polygon color 0x%x\n", color);
				}

				val = *buf++;
				x = (int16_t)val;
				y = (int16_t)(val >> 16);

				v = pvr_dr_get();

				*v = (pvr_vertex_t){
					.flags = (i == nb - 1) ?
						PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
					.argb = color,
					.x = x_to_pvr(x),
					.y = y_to_pvr(y),
					.z = 1.0f,
				};

				pvr_dr_put(v);
			}
			break;
		}

		case 0x21 ... 0x27:
		case 0x29 ... 0x2f:
		case 0x31 ... 0x37:
		case 0x39 ... 0x3f:
			pvr_printf("Render polygon (0x%x)\n", cmd);
			break;

		case 0x40:
		case 0x50:
			/* Monochrome/shaded line */
			pvr_poly_cxt_t cxt;
			pvr_poly_hdr_t *hdr;
			bool multicolor = cmd & 0x10;
			uint32_t val, *buf = pbuffer.U4;
			unsigned int i, nb = 2;
			uint32_t oldcolor, color;
			int16_t x, y, oldx, oldy;

			pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);

			cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
			cxt.gen.culling = PVR_CULLING_NONE;

			hdr = pvr_dr_get();
			pvr_poly_compile(hdr, &cxt);
			pvr_dr_put(hdr);

			/* BGR->RGB swap */
			color = __builtin_bswap32(*buf++) >> 8;
			oldcolor = color;

			val = *buf++;
			oldx = (int16_t)val;
			oldy = (int16_t)(val >> 16);

			for (i = 0; i < nb - 1; i++) {
				if (multicolor)
					color = __builtin_bswap32(*buf++) >> 8;

				val = *buf++;
				x = (int16_t)val;
				y = (int16_t)(val >> 16);

				if (oldx > x)
					draw_line(x, y, color, oldx, oldy, oldcolor);
				else
					draw_line(oldx, oldy, oldcolor, x, y, color);

				oldx = x;
				oldy = y;
				oldcolor = color;
			}
			break;

		case 0x41 ... 0x4f:
		case 0x51 ... 0x5a:
			pvr_printf("Render line (0x%x)\n", cmd);
			break;

		case 0x60: {
			/* Monochrome rectangle */
			pvr_poly_cxt_t cxt;
			pvr_poly_hdr_t *hdr;
			pvr_vertex_t *v;
			int16_t x[2], y[2];
			unsigned int i;
			uint32_t color;

			pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);

			cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
			cxt.gen.culling = PVR_CULLING_NONE;

			hdr = pvr_dr_get();
			pvr_poly_compile(hdr, &cxt);
			pvr_dr_put(hdr);

			/* BGR->RGB swap */
			color = __builtin_bswap32(pbuffer.U4[0]) >> 8;

			x[0] = (int16_t)pbuffer.U4[1];
			y[0] = (int16_t)(pbuffer.U4[1] >> 16);
			x[1] = x[0] + (int16_t)pbuffer.U4[2];
			y[1] = y[0] + (int16_t)(pbuffer.U4[2] >> 16);

			for (i = 0; i < 4; i++) {
				v = pvr_dr_get();

				*v = (pvr_vertex_t){
					.flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX,
					.argb = color,
					.x = x_to_pvr(x[!!(i & 0x1)]),
					.y = y_to_pvr(y[!!(i & 0x2)]),
					.z = 1.0f,
				};

				pvr_dr_put(v);
			}
			break;
		}

		case 0x61 ... 0x7f:
			pvr_printf("Render rectangle (0x%x)\n", cmd);
			break;

		default:
			pvr_printf("Unhandled GPU CMD: 0x%x\n", cmd);
			break;
		}
	}

	gpu.ex_regs[1] &= ~0x1ff;
	gpu.ex_regs[1] |= pvr.gp1 & 0x1ff;

	*cycles_sum_out += cpu_cycles_sum;
	*cycles_last = cpu_cycles;
	*last_cmd = cmd;
	return list - list_start;
}
