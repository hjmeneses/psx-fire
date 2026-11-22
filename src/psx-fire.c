/*
PSX Fire
Copyright (C) 2026 hjmeneses

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * The fire from the PlayStation port of DOOM, as described by Fabien Sanglard
 * ("How DOOM fire was done"):
 *   - every cell holds a heat value 0..36, an index into a 37-colour palette;
 *   - the bottom row is the source (36 while burning);
 *   - each step, every cell copies its heat to the cell above, shifted sideways
 *     by a random amount and cooled by 0 or 1;
 *   - turning it off makes each source cell lose 0 or 1 per step, so the
 *     flames die down unevenly.
 *
 * The simulation runs on the CPU on one byte per cell. The bytes are uploaded
 * as an R8 texture and the effect maps them through the palette on the GPU,
 * so transparency and smooth scaling cost nothing on the CPU side.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>

#include <math.h>
#include <string.h>

#define MAX_HEAT 36
#define PAL_SIZE (MAX_HEAT + 1)
#define MAX_STEPS_PER_TICK 4
#define PREHEAT_STEPS 90

static const uint8_t PAL_RGB[PAL_SIZE * 3] = {
	0x07, 0x07, 0x07, 0x1F, 0x07, 0x07, 0x2F, 0x0F, 0x07, 0x47, 0x0F, 0x07, 0x57, 0x17, 0x07, 0x67, 0x1F, 0x07,
	0x77, 0x1F, 0x07, 0x8F, 0x27, 0x07, 0x9F, 0x2F, 0x07, 0xAF, 0x3F, 0x07, 0xBF, 0x47, 0x07, 0xC7, 0x47, 0x07,
	0xDF, 0x4F, 0x07, 0xDF, 0x57, 0x07, 0xDF, 0x57, 0x07, 0xD7, 0x5F, 0x07, 0xD7, 0x5F, 0x07, 0xD7, 0x67, 0x0F,
	0xCF, 0x6F, 0x0F, 0xCF, 0x77, 0x0F, 0xCF, 0x7F, 0x0F, 0xCF, 0x87, 0x17, 0xC7, 0x87, 0x17, 0xC7, 0x8F, 0x17,
	0xC7, 0x97, 0x1F, 0xBF, 0x9F, 0x1F, 0xBF, 0x9F, 0x1F, 0xBF, 0xA7, 0x27, 0xBF, 0xA7, 0x27, 0xBF, 0xAF, 0x2F,
	0xB7, 0xAF, 0x2F, 0xB7, 0xB7, 0x2F, 0xB7, 0xB7, 0x37, 0xCF, 0xCF, 0x6F, 0xDF, 0xDF, 0x9F, 0xEF, 0xEF, 0xC7,
	0xFF, 0xFF, 0xFF,
};

struct psx_fire {
	obs_source_t *source;
	pthread_mutex_t mutex;

	/* settings */
	uint32_t width, height; /* output size in pixels */
	uint32_t pixel;         /* output pixels per simulation cell */
	int wind;               /* -3..3, 0 = original */
	double cooling;         /* 0.3..2, 1 = original */
	double fps;             /* simulation steps per second */
	int fade;               /* heat values below this fade to transparent */
	bool preheat;
	bool auto_ignite;
	bool smooth;
	bool classic_order; /* column-major traversal, exactly like the PSX code */

	/* simulation */
	uint32_t sw, sh;
	uint8_t *heat;
	bool burning;
	bool initialized;
	double accum;
	uint64_t rng;
	bool dirty;
	bool palette_dirty;

	/* graphics (only touched from the graphics thread) */
	gs_effect_t *effect;
	gs_eparam_t *p_image, *p_palette;
	gs_texture_t *tex;
	gs_texture_t *pal_tex;
	uint32_t tex_w, tex_h;

	obs_hotkey_id hk_toggle;
	obs_hotkey_id hk_reignite;
};

/* ------------------------------------------------------------------------ */
/* Simulation                                                               */

static inline uint32_t rnd(struct psx_fire *f)
{
	/* xorshift64* */
	uint64_t x = f->rng;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	f->rng = x;
	return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

/* Same distribution as the original Math.round(Math.random() * 3) & 3:
   0 and 3 with probability 1/6 each, 1 and 2 with probability 1/3 each. */
static inline int spread(struct psx_fire *f)
{
	static const int table[6] = {0, 1, 1, 2, 2, 3};
	return table[rnd(f) % 6];
}

static void set_source_row(struct psx_fire *f, uint8_t v)
{
	memset(f->heat + (size_t)(f->sh - 1) * f->sw, v, f->sw);
}

static void fire_step(struct psx_fire *f)
{
	const long W = (long)f->sw, H = (long)f->sh;
	uint8_t *F = f->heat;
	const long source_row = (H - 1) * W;

	if (!f->burning) {
		for (long x = 0; x < W; x++)
			if (F[source_row + x] > 0 && (rnd(f) & 1))
				F[source_row + x]--;
	}

	const bool classic = f->cooling == 1.0;
	const double p = fmin(f->cooling * 0.5, 1.0);
	const uint32_t threshold = p >= 1.0 ? UINT32_MAX : (uint32_t)(p * 4294967296.0);
	const int wind = f->wind;

	if (f->classic_order) {
		/* Column-major (x outer, y inner), as in the original code and the
		 * HTML/PowerShell versions. Writes that land in columns not yet
		 * processed get moved again in the same step, so some heat climbs
		 * more than one row per step, and leaving one edge re-enters on the
		 * other edge one row higher (flat-index arithmetic). */
		for (long x = 0; x < W; x++) {
			for (long y = 1; y < H; y++) {
				const long src = y * W + x;
				const uint8_t px = F[src];
				if (px == 0) {
					F[src - W] = 0;
					continue;
				}
				const int r = spread(f);
				const long dst = src - r + 1 + wind - W;
				if (dst < 0 || dst >= source_row)
					continue;
				const int dec = classic ? (r & 1) : (rnd(f) < threshold ? 1 : 0);
				F[dst] = (uint8_t)(px > dec ? px - dec : 0);
			}
		}
		return;
	}

	/* Row-major (y outer, x inner): every read is from row y and every write
	 * goes to row y - 1, so positive/negative wind are symmetric and memory
	 * access is sequential. Horizontal shift wraps cleanly within row y - 1. */
	for (long y = 1; y < H; y++) {
		const long src_row = y * W;
		const long dst_row = src_row - W;
		for (long x = 0; x < W; x++) {
			const uint8_t px = F[src_row + x];
			if (px == 0) {
				F[dst_row + x] = 0;
				continue;
			}
			const int r = spread(f);
			long nx = x - r + 1 + wind;
			if (nx < 0)
				nx += W;
			else if (nx >= W)
				nx -= W;
			const int dec = classic ? (r & 1) : (rnd(f) < threshold ? 1 : 0);
			F[dst_row + nx] = (uint8_t)(px > dec ? px - dec : 0);
		}
	}
}

/* Rebuilds the grid for the current size. Caller holds the mutex. */
static void alloc_sim(struct psx_fire *f)
{
	uint32_t pixel = f->pixel ? f->pixel : 1;
	f->sw = f->width / pixel;
	f->sh = f->height / pixel;
	if (f->sw < 8)
		f->sw = 8;
	if (f->sh < 4)
		f->sh = 4;

	bfree(f->heat);
	f->heat = bzalloc((size_t)f->sw * f->sh);
	set_source_row(f, f->burning ? MAX_HEAT : 0);
	if (f->burning && f->preheat)
		for (int i = 0; i < PREHEAT_STEPS; i++)
			fire_step(f);
	f->accum = 0.0;
	f->dirty = true;
}

static void fire_ignite_locked(struct psx_fire *f)
{
	memset(f->heat, 0, (size_t)f->sw * f->sh);
	f->burning = true;
	set_source_row(f, MAX_HEAT);
	f->dirty = true;
}

static void fire_ignite(struct psx_fire *f)
{
	pthread_mutex_lock(&f->mutex);
	fire_ignite_locked(f);
	pthread_mutex_unlock(&f->mutex);
}

static void fire_toggle(struct psx_fire *f)
{
	pthread_mutex_lock(&f->mutex);
	f->burning = !f->burning;
	if (f->burning)
		set_source_row(f, MAX_HEAT);
	pthread_mutex_unlock(&f->mutex);
}

/* ------------------------------------------------------------------------ */
/* Hotkeys and property buttons                                             */

static void hotkey_toggle(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		fire_toggle(data);
}

static void hotkey_reignite(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		fire_ignite(data);
}

static bool button_toggle(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (data) /* properties can be built without a source instance */
		fire_toggle(data);
	return false;
}

static bool button_reignite(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (data)
		fire_ignite(data);
	return false;
}

/* ------------------------------------------------------------------------ */
/* Source callbacks                                                         */

static const char *psx_fire_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PSXFire");
}

static void psx_fire_update(void *data, obs_data_t *settings)
{
	struct psx_fire *f = data;

	int64_t raw_w = obs_data_get_int(settings, "width");
	int64_t raw_h = obs_data_get_int(settings, "height");
	int64_t raw_p = obs_data_get_int(settings, "pixel");
	uint32_t width = (uint32_t)(raw_w < 16 ? 16 : (raw_w > 7680 ? 7680 : raw_w));
	uint32_t height = (uint32_t)(raw_h < 16 ? 16 : (raw_h > 4320 ? 4320 : raw_h));
	uint32_t pixel = (uint32_t)(raw_p < 1 ? 1 : (raw_p > 16 ? 16 : raw_p));
	int fade = (int)obs_data_get_int(settings, "fade");
	if (fade < 0)
		fade = 0;
	else if (fade > MAX_HEAT)
		fade = MAX_HEAT;
	int wind = (int)obs_data_get_int(settings, "wind");
	if (wind < -3)
		wind = -3;
	else if (wind > 3)
		wind = 3;

	pthread_mutex_lock(&f->mutex);

	f->wind = wind;
	f->cooling = round(obs_data_get_double(settings, "cooling") * 10.0) / 10.0;
	if (f->cooling < 0.1)
		f->cooling = 0.1;
	f->fps = (double)obs_data_get_int(settings, "fps");
	if (f->fps < 1.0)
		f->fps = 1.0;
	f->preheat = obs_data_get_bool(settings, "preheat");
	f->auto_ignite = obs_data_get_bool(settings, "auto_ignite");
	f->smooth = obs_data_get_bool(settings, "smooth");
	f->classic_order = obs_data_get_bool(settings, "classic_order");

	if (fade != f->fade) {
		f->fade = fade;
		f->palette_dirty = true;
	}

	if (!f->initialized) {
		f->burning = obs_data_get_bool(settings, "start_on");
		f->initialized = true;
		f->width = width;
		f->height = height;
		f->pixel = pixel;
		alloc_sim(f);
	} else if (width != f->width || height != f->height || pixel != f->pixel) {
		f->width = width;
		f->height = height;
		f->pixel = pixel;
		alloc_sim(f);
	}

	pthread_mutex_unlock(&f->mutex);
}

static void *psx_fire_create(obs_data_t *settings, obs_source_t *source)
{
	struct psx_fire *f = bzalloc(sizeof(*f));
	f->source = source;
	f->fade = -1; /* forces the palette to be built on the first update */
	f->rng = os_gettime_ns() | 1;
	pthread_mutex_init_value(&f->mutex);
	if (pthread_mutex_init(&f->mutex, NULL) != 0) {
		bfree(f);
		return NULL;
	}

	char *path = obs_module_file("effects/psx-fire.effect");
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, NULL);
	if (f->effect) {
		f->p_image = gs_effect_get_param_by_name(f->effect, "image");
		f->p_palette = gs_effect_get_param_by_name(f->effect, "palette");
	}
	obs_leave_graphics();
	if (!f->effect)
		obs_log(LOG_ERROR, "could not load effect file '%s'", path ? path : "(null)");
	bfree(path);

	psx_fire_update(f, settings);

	f->hk_toggle = obs_hotkey_register_source(source, "PSXFire.Toggle", obs_module_text("HotkeyToggle"),
						  hotkey_toggle, f);
	f->hk_reignite = obs_hotkey_register_source(source, "PSXFire.Reignite", obs_module_text("HotkeyReignite"),
						    hotkey_reignite, f);
	return f;
}

static void psx_fire_destroy(void *data)
{
	struct psx_fire *f = data;

	obs_enter_graphics();
	gs_texture_destroy(f->tex);
	gs_texture_destroy(f->pal_tex);
	gs_effect_destroy(f->effect);
	obs_leave_graphics();

	pthread_mutex_destroy(&f->mutex);
	bfree(f->heat);
	bfree(f);
}

static void psx_fire_activate(void *data)
{
	struct psx_fire *f = data;
	pthread_mutex_lock(&f->mutex);
	if (f->auto_ignite)
		fire_ignite_locked(f);
	pthread_mutex_unlock(&f->mutex);
}

static void psx_fire_tick(void *data, float seconds)
{
	struct psx_fire *f = data;

	/* Nothing to simulate while the source is not on screen anywhere */
	if (!obs_source_showing(f->source))
		return;

	pthread_mutex_lock(&f->mutex);
	const double dt = 1.0 / f->fps;
	f->accum += seconds;
	int steps = 0;
	while (f->accum >= dt && steps < MAX_STEPS_PER_TICK) {
		fire_step(f);
		f->accum -= dt;
		steps++;
	}
	if (steps)
		f->dirty = true;
	if (f->accum > dt) /* fell behind (e.g. a hitch): do not try to catch up */
		f->accum = 0.0;
	pthread_mutex_unlock(&f->mutex);
}

static void build_palette(uint8_t *out, int fade)
{
	for (int i = 0; i < PAL_SIZE; i++) {
		out[i * 4 + 0] = PAL_RGB[i * 3 + 0];
		out[i * 4 + 1] = PAL_RGB[i * 3 + 1];
		out[i * 4 + 2] = PAL_RGB[i * 3 + 2];
		out[i * 4 + 3] = (fade > 0 && i < fade) ? (uint8_t)lround(255.0 * i / fade) : 255;
	}
}

static void psx_fire_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct psx_fire *f = data;
	if (!f->effect)
		return;

	pthread_mutex_lock(&f->mutex);
	if (!f->tex || f->tex_w != f->sw || f->tex_h != f->sh) {
		gs_texture_destroy(f->tex);
		f->tex = gs_texture_create(f->sw, f->sh, GS_R8, 1, NULL, GS_DYNAMIC);
		f->tex_w = f->sw;
		f->tex_h = f->sh;
		f->dirty = true;
	}
	if (f->tex && f->dirty) {
		gs_texture_set_image(f->tex, f->heat, f->sw, false);
		f->dirty = false;
	}
	if (!f->pal_tex || f->palette_dirty) {
		uint8_t pal[PAL_SIZE * 4];
		build_palette(pal, f->fade);
		if (!f->pal_tex)
			f->pal_tex = gs_texture_create(PAL_SIZE, 1, GS_RGBA, 1, NULL, GS_DYNAMIC);
		if (f->pal_tex)
			gs_texture_set_image(f->pal_tex, pal, PAL_SIZE * 4, false);
		f->palette_dirty = false;
	}
	const uint32_t w = f->width, h = f->height;
	const bool smooth = f->smooth;
	pthread_mutex_unlock(&f->mutex);

	if (!f->tex || !f->pal_tex)
		return;

	gs_effect_set_texture(f->p_image, f->tex);
	gs_effect_set_texture(f->p_palette, f->pal_tex);
	while (gs_effect_loop(f->effect, smooth ? "DrawSmooth" : "Draw"))
		gs_draw_sprite(f->tex, 0, w, h);
}

static uint32_t psx_fire_get_width(void *data)
{
	struct psx_fire *f = data;
	pthread_mutex_lock(&f->mutex);
	uint32_t w = f->width;
	pthread_mutex_unlock(&f->mutex);
	return w;
}

static uint32_t psx_fire_get_height(void *data)
{
	struct psx_fire *f = data;
	pthread_mutex_lock(&f->mutex);
	uint32_t h = f->height;
	pthread_mutex_unlock(&f->mutex);
	return h;
}

static void psx_fire_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 480);
	obs_data_set_default_int(settings, "pixel", 6);
	obs_data_set_default_int(settings, "wind", 0);
	obs_data_set_default_double(settings, "cooling", 1.0);
	obs_data_set_default_int(settings, "fps", 30);
	obs_data_set_default_int(settings, "fade", 6);
	obs_data_set_default_bool(settings, "start_on", true);
	obs_data_set_default_bool(settings, "preheat", true);
	obs_data_set_default_bool(settings, "auto_ignite", false);
	obs_data_set_default_bool(settings, "smooth", false);
	obs_data_set_default_bool(settings, "classic_order", false);
}

static obs_properties_t *psx_fire_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "width", obs_module_text("Width"), 16, 7680, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 16, 4320, 1);
	obs_properties_add_int_slider(props, "pixel", obs_module_text("PixelSize"), 1, 16, 1);
	obs_properties_add_int_slider(props, "wind", obs_module_text("Wind"), -3, 3, 1);
	obs_properties_add_float_slider(props, "cooling", obs_module_text("Cooling"), 0.3, 2.0, 0.1);
	obs_properties_add_int_slider(props, "fps", obs_module_text("FPS"), 10, 60, 1);
	obs_property_t *fade = obs_properties_add_int_slider(props, "fade", obs_module_text("Fade"), 0, 36, 1);
	obs_property_set_long_description(fade, obs_module_text("Fade.Description"));
	obs_properties_add_bool(props, "start_on", obs_module_text("StartOn"));
	obs_properties_add_bool(props, "preheat", obs_module_text("Preheat"));
	obs_properties_add_bool(props, "auto_ignite", obs_module_text("AutoIgnite"));
	obs_properties_add_bool(props, "smooth", obs_module_text("Smooth"));
	obs_property_t *classic_order =
		obs_properties_add_bool(props, "classic_order", obs_module_text("ClassicOrder"));
	obs_property_set_long_description(classic_order, obs_module_text("ClassicOrder.Description"));
	obs_properties_add_button2(props, "toggle", obs_module_text("Toggle"), button_toggle, data);
	obs_properties_add_button2(props, "reignite", obs_module_text("Reignite"), button_reignite, data);

	return props;
}

struct obs_source_info psx_fire_source = {
	.id = "psx_fire_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = psx_fire_get_name,
	.create = psx_fire_create,
	.destroy = psx_fire_destroy,
	.update = psx_fire_update,
	.get_defaults = psx_fire_defaults,
	.get_properties = psx_fire_properties,
	.activate = psx_fire_activate,
	.video_tick = psx_fire_tick,
	.video_render = psx_fire_render,
	.get_width = psx_fire_get_width,
	.get_height = psx_fire_get_height,
};
