// SPDX-License-Identifier: GPL-2.0-only
/*
 * Misc. glue code for the PCSX port
 *
 * Copyright (C) 2024 Paul Cercueil <paul@crapouillou.net>
 */

#include <frontend/plugin_lib.h>
#include <libpcsxcore/psxcounters.h>
#include <libpcsxcore/gpu.h>
#include <psemu_plugin_defs.h>

#include <arch/timer.h>
#include <dc/sq.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <dc/vmu_fb.h>

#include <stdint.h>
#include <sys/time.h>

#include "emu.h"
#include "vmu.h"

#define MAX_LAG_FRAMES 3

#define tvdiff(tv, tv_old) \
	((tv.tv_sec - tv_old.tv_sec) * 1000000 + tv.tv_usec - tv_old.tv_usec)

/* PVR texture size in pixels */
#define TEX_WIDTH  1024
#define TEX_HEIGHT 512

static unsigned int frames;
static uint64_t timer_ms;

static pvr_ptr_t pvram;
static uint32_t *pvram_sq;

static float screen_fw, screen_fh;
static unsigned int screen_w, screen_h, screen_bpp;

unsigned short in_keystate[8];

int in_type[8] = {
   PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
   PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
   PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
   PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE
};

static int dc_vout_open(void)
{
	pvram = pvr_mem_malloc(TEX_WIDTH * TEX_HEIGHT * 2);

	assert(!!pvram);
	assert(!((unsigned int)pvram & 0x1f));

	pvram_sq = (uint32_t *)(((uintptr_t)pvram & 0xffffff) | PVR_TA_TEX_MEM);

	return 0;
}

static void dc_vout_close(void)
{
	pvr_mem_free(pvram);
}

static void dc_vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
	screen_w = raw_w;
	screen_h = raw_h;
	screen_bpp = bpp;

	screen_fw = 320.0f / (float)raw_w;
	screen_fh = 240.0f / (float)raw_h;

	if (bpp == 15)
		vid_set_mode(DM_640x480, PM_RGB555);
	else
		vid_set_mode(DM_640x480, PM_RGB565);
}

static inline void copy15(const uint16_t *vram, int stride, int w, int h)
{
	const uint32_t *vram32 = (const uint32_t *)vram;
	uint32_t pixels, r, g, b;
	uint32_t *line, *dest = (uint32_t *)pvram_sq;
	unsigned int x, y, i;

	for (y = 0; y < h; y++) {
		line = SQ_MASK_DEST(dest);

		sq_lock(dest);

		for (x = 0; x < w; x += 16) {
			for (i = 0; i < 8; i++) {
				pixels = *vram32++;

				b = (pixels >> 10) & 0x001f001f;
				g = pixels & 0x03e003e0;
				r = (pixels & 0x001f001f) << 10;

				line[i] = r | g | b;
			}

			sq_flush(line);
			line += 8;
		}

		vram32 += (stride - w) / 2;
		dest += TEX_WIDTH / 2;

		sq_unlock();
	}
}

static inline uint16_t rgb_24_to_16(uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint16_t)r & 0xf8) << 8
		| ((uint16_t)g & 0xfc) << 3
		| (uint16_t)b >> 3;
}

static inline void copy24(const uint16_t *vram, int stride, int w, int h)
{
	const uint32_t *vram32 = (const uint32_t *)vram;
	uint32_t *line, *dest = (uint32_t *)pvram_sq;
	uint32_t w0, w1, w2;
	unsigned int x, y, i;
	uint16_t px0, px1;

	for (y = 0; y < h; y++) {
		sq_lock(dest);

		line = SQ_MASK_DEST(dest);

		for (x = 0; x < w; x += 16) {
			for (i = 0; i < 8; i += 2) {
				w0 = *vram32++; /* BGRB */
				w1 = *vram32++; /* GRBG */
				w2 = *vram32++; /* RBGR */

				px0 = rgb_24_to_16(w0, w0 >> 8, w0 >> 16);
				px1 = rgb_24_to_16(w0 >> 24, w1, w1 >> 8);
				line[i] = (uint32_t)px1 << 16 | px0;

				px0 = rgb_24_to_16(w1 >> 16, w1 >> 24, w2);
				px1 = rgb_24_to_16(w2 >> 8, w2 >> 16, w2 >> 24);
				line[i + 1] = (uint32_t)px1 << 16 | px0;
			}

			sq_flush(line);
			line += 8;
		}

		sq_unlock();

		vram32 += (stride * 2 - w * 3) / 4;
		dest += TEX_WIDTH / 2;
	}
}

static void dc_vout_flip(const void *vram, int stride, int bgr24,
			 int x, int y, int w, int h, int dims_changed)
{
	uint64_t new_timer;
	pvr_poly_cxt_t cxt;
	pvr_poly_hdr_t hdr;
	pvr_vertex_t vert;
	float ymin, ymax, xmin, xmax;

	if (!vram)
		return;

	assert(!((unsigned int)vram & 0x3));

	if (bgr24)
		copy24(vram, stride, w, h);
	else
		copy15(vram, stride, w, h);

	ymin = 240.0f - (float)(y + h) * screen_fh;
	ymax = 480.0f - ymin;

	xmin = 320.0f - (float)(x + w) * screen_fw;
	xmax = 640.0f - xmin;

	pvr_wait_ready();
	pvr_scene_begin();
	pvr_list_begin(PVR_LIST_OP_POLY);

	pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
			 PVR_TXRFMT_NONTWIDDLED | (bgr24 ? PVR_TXRFMT_RGB565 : PVR_TXRFMT_ARGB1555),
			 TEX_WIDTH, TEX_HEIGHT, pvram, PVR_FILTER_NONE);

	pvr_poly_compile(&hdr, &cxt);
	pvr_prim(&hdr, sizeof(hdr));

	vert.argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
	vert.oargb = 0;
	vert.flags = PVR_CMD_VERTEX;

	vert.x = xmin;
	vert.y = ymin;
	vert.z = 1.0f;
	vert.u = 0.0f;
	vert.v = 0.0f;
	pvr_prim(&vert, sizeof(vert));

	vert.x = xmax;
	vert.y = ymin;
	vert.z = 1.0f;
	vert.u = (float)w / (float)TEX_WIDTH;
	vert.v = 0.0f;
	pvr_prim(&vert, sizeof(vert));

	vert.x = xmin;
	vert.y = ymax;
	vert.z = 1.0f;
	vert.u = 0.0f;
	vert.v = (float)h / (float)TEX_HEIGHT;
	pvr_prim(&vert, sizeof(vert));

	vert.x = xmax;
	vert.y = ymax;
	vert.z = 1.0f;
	vert.u = (float)w / (float)TEX_WIDTH;
	vert.v = (float)h / (float)TEX_HEIGHT;
	vert.flags = PVR_CMD_VERTEX_EOL;
	pvr_prim(&vert, sizeof(vert));

	pvr_list_finish();
	pvr_scene_finish();

	new_timer = timer_ms_gettime64();

	frames++;

	if (timer_ms == 0) {
		timer_ms = new_timer;
		return;
	}

	if (new_timer > (timer_ms + 1000)) {
		vmu_print_info((float)frames, screen_w, screen_h, screen_bpp);

		timer_ms = new_timer;
		frames = 0;
	}
}

static struct rearmed_cbs dc_rearmed_cbs = {
	.pl_vout_open		= dc_vout_open,
	.pl_vout_close		= dc_vout_close,
	.pl_vout_set_mode	= dc_vout_set_mode,
	.pl_vout_flip		= dc_vout_flip,

	.gpu_hcnt		= (unsigned int *)&hSyncCount,
	.gpu_frame_count	= (unsigned int *)&frame_counter,
	.gpu_state_change	= gpu_state_change,

	.gpu_unai = {
		.lighting = 1,
		.blending = 1,
	},
};

void plugin_call_rearmed_cbs(void)
{
	extern void *hGPUDriver;
	void (*rearmed_set_cbs)(const struct rearmed_cbs *cbs);

	rearmed_set_cbs = SysLoadSym(hGPUDriver, "GPUrearmedCallbacks");
	if (rearmed_set_cbs != NULL)
		rearmed_set_cbs(&dc_rearmed_cbs);
}

static void emu_attach_cont_cb(maple_device_t *dev)
{
	printf("Hot-plugged a controller in port %u\n", dev->port);
	in_type[dev->port] = PSE_PAD_TYPE_STANDARD;
}

static void emu_detach_cont_cb(maple_device_t *dev)
{
	printf("Unplugged a controller in port %u\n", dev->port);
	in_type[dev->port] = PSE_PAD_TYPE_NONE;
}

long PAD__init(long flags) {
        maple_device_t *dev;
	unsigned int i;

	maple_attach_callback(MAPLE_FUNC_CONTROLLER, emu_attach_cont_cb);
	maple_detach_callback(MAPLE_FUNC_CONTROLLER, emu_detach_cont_cb);

	for (i = 0; i < 4; i++) {
		dev = maple_enum_dev(i, 0);
		if (dev) {
			printf("Found a controller in port %u\n", dev->port);
			in_type[dev->port] = PSE_PAD_TYPE_STANDARD;
		}
	}

	return PSE_PAD_ERR_SUCCESS;
}

long PAD__shutdown(void) {
	maple_attach_callback(MAPLE_FUNC_CONTROLLER, NULL);
	maple_detach_callback(MAPLE_FUNC_CONTROLLER, NULL);

	return PSE_PAD_ERR_SUCCESS;
}

long PAD__open(void)
{
	return PSE_PAD_ERR_SUCCESS;
}

long PAD__close(void) {
	return PSE_PAD_ERR_SUCCESS;
}

typedef union {
	u16 All;
	struct {
		unsigned SQUARE_BUTTON    : 1;
		unsigned CROSS_BUTTON     : 1;
		unsigned CIRCLE_BUTTON    : 1;
		unsigned TRIANGLE_BUTTON  : 1;
		unsigned R1_BUTTON        : 1;
		unsigned L1_BUTTON        : 1;
		unsigned R2_BUTTON        : 1;
		unsigned L2_BUTTON        : 1;
		unsigned L_DPAD           : 1;
		unsigned D_DPAD           : 1;
		unsigned R_DPAD           : 1;
		unsigned U_DPAD           : 1;
		unsigned START_BUTTON     : 1;
		unsigned L3_BUTTON        : 1;
		unsigned R3_BUTTON        : 1;
		unsigned SELECT_BUTTON    : 1;
	};
} psx_buttons_t;

long PAD1__readPort1(PadDataS *pad) {
        maple_device_t *dev;
	cont_state_t *state;
	uint16_t buttons = 0;

	pad->controllerType = in_type[pad->requestPadIndex];
	if (pad->controllerType == PSE_PAD_TYPE_NONE)
		return 0;

	dev = maple_enum_dev(pad->requestPadIndex, 0);
	if (!dev)
		return 0;

	if (!(dev->info.functions & MAPLE_FUNC_CONTROLLER))
		return 0;

	state = (cont_state_t *)maple_dev_status(dev);
	if (state->buttons & CONT_Z)
		buttons |= DKEY_SELECT;
	if (state->buttons & CONT_DPAD2_LEFT)
		buttons |= DKEY_L3;
	if (state->buttons & CONT_DPAD2_DOWN)
		buttons |= DKEY_R3;
	if (state->buttons & CONT_START)
		buttons |= DKEY_START;
	if (state->buttons & CONT_DPAD_UP)
		buttons |= DKEY_UP;
	if (state->buttons & CONT_DPAD_RIGHT)
		buttons |= DKEY_RIGHT;
	if (state->buttons & CONT_DPAD_DOWN)
		buttons |= DKEY_DOWN;
	if (state->buttons & CONT_DPAD_LEFT)
		buttons |= DKEY_LEFT;
	if (state->buttons & CONT_C)
		buttons |= DKEY_L2;
	if (state->buttons & CONT_D)
		buttons |= DKEY_R2;
	if (state->ltrig > 128)
		buttons |= DKEY_L1;
	if (state->rtrig > 128)
		buttons |= DKEY_R1;
	if (state->buttons & CONT_A)
		buttons |= DKEY_CROSS;
	if (state->buttons & CONT_B)
		buttons |= DKEY_CIRCLE;
	if (state->buttons & CONT_X)
		buttons |= DKEY_SQUARE;
	if (state->buttons & CONT_Y)
		buttons |= DKEY_TRIANGLE;

	pad->buttonStatus = ~buttons;

	return 0;
}

long PAD2__readPort2(PadDataS *pad) {
	return PAD1__readPort1(pad);
}

void plat_trigger_vibrate(int pad, int low, int high) {
}

void pl_frame_limit(void)
{
}

void pl_gun_byte2(int port, unsigned char byte)
{
}
