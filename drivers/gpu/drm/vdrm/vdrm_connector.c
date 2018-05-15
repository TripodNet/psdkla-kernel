#include <linux/of.h>

#include <drm/drm_atomic_helper.h>

#include "vdrm_drv.h"

struct vdrm_timings
{
	u16 x_res;
	u16 y_res;
	u32 pixelclock;
	u16 hsw;
	u16 hfp;
	u16 hbp;
	u16 vsw;
	u16 vfp;
	u16 vbp;
};

struct vdrm_connector {
	struct drm_connector base;
	struct vdrm_timings timings;
};

#define to_vdrm_connector(x) container_of(x, struct vdrm_connector, base)

void vdrm_connector_fini(struct drm_connector *connector)
{
	struct vdrm_connector *conn = to_vdrm_connector(connector);

	debug("%s\n", __func__);

	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(conn);
}

static const struct drm_connector_funcs vdrm_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vdrm_connector_fini,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void copy_timings_vdrm_to_drm(struct drm_display_mode *mode,
		struct vdrm_timings *timings)
{
	debug("%s\n", __func__);

	mode->clock = timings->pixelclock / 1000;

	mode->hdisplay = timings->x_res;
	mode->hsync_start = mode->hdisplay + timings->hfp;
	mode->hsync_end = mode->hsync_start + timings->hsw;
	mode->htotal = mode->hsync_end + timings->hbp;

	mode->vdisplay = timings->y_res;
	mode->vsync_start = mode->vdisplay + timings->vfp;
	mode->vsync_end = mode->vsync_start + timings->vsw;
	mode->vtotal = mode->vsync_end + timings->vbp;

	mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
}

static int vdrm_conn_get_modes(struct drm_connector *connector)
{
	struct vdrm_connector *conn = to_vdrm_connector(connector);
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;

	debug("%s\n", __func__);

	mode = drm_mode_create(dev);
	copy_timings_vdrm_to_drm(mode, &conn->timings);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs vdrm_conn_helper_funcs = {
	.get_modes    = vdrm_conn_get_modes,
};

struct drm_connector *vdrm_connector_init(struct drm_device *dev, struct drm_encoder *encoder,
		struct device_node *np)
{
	struct drm_connector *connector = NULL;
	struct vdrm_connector *vdrm_connector;
	uint32_t x, y, fps;
	uint32_t total_x = 0, total_y = 0;

	debug("%s\n", __func__);

	vdrm_connector = kzalloc(sizeof(struct vdrm_connector), GFP_KERNEL);
	if (!vdrm_connector)
		return NULL;

	if(of_property_read_u32(np, "x-res", &x)) {
		kfree(vdrm_connector);
		return NULL;
	}

	if(of_property_read_u32(np, "y-res", &y)) {
		kfree(vdrm_connector);
		return NULL;
	}

	if(of_property_read_u32(np, "refresh", &fps)) {
		kfree(vdrm_connector);
		return NULL;
	}

	vdrm_connector->timings.x_res = x;
	total_x += vdrm_connector->timings.x_res;
	vdrm_connector->timings.hfp = ((vdrm_connector->timings.x_res/100 > 0) ? (vdrm_connector->timings.x_res/100) : 1);
	total_x += vdrm_connector->timings.hfp;
	vdrm_connector->timings.hbp = ((vdrm_connector->timings.hfp/2 > 0) ? (vdrm_connector->timings.hfp/2) : 1);
	total_x += vdrm_connector->timings.hbp;
	vdrm_connector->timings.hsw = ((vdrm_connector->timings.hbp/2 > 0) ? (vdrm_connector->timings.hbp/2) : 1);
	total_x += vdrm_connector->timings.hsw;

	vdrm_connector->timings.y_res = y;
	total_y += vdrm_connector->timings.y_res;
	vdrm_connector->timings.vfp = ((vdrm_connector->timings.y_res/100 > 0) ? (vdrm_connector->timings.y_res/100) : 1);
	total_y += vdrm_connector->timings.vfp;
	vdrm_connector->timings.vbp = ((vdrm_connector->timings.vfp/2 > 0) ? (vdrm_connector->timings.vfp/2) : 1);
	total_y += vdrm_connector->timings.vbp;
	vdrm_connector->timings.vsw = ((vdrm_connector->timings.vbp/2 > 0) ? (vdrm_connector->timings.vbp/2) : 1);
	total_y += vdrm_connector->timings.vsw;

	vdrm_connector->timings.pixelclock = (fps * total_x * total_y);


	connector = &vdrm_connector->base;

	if(drm_connector_init(dev, connector, &vdrm_connector_funcs,
				DRM_MODE_CONNECTOR_VIRTUAL)) {
		kfree(vdrm_connector);
		return NULL;
	}

	drm_connector_helper_add(connector, &vdrm_conn_helper_funcs);

	if(drm_connector_register(connector)) {
		drm_connector_cleanup(connector);
		kfree(connector);
		return NULL;
	}

	if(drm_connector_attach_encoder(connector, encoder))
	{
		drm_connector_unregister(connector);
		drm_connector_cleanup(connector);
		kfree(connector);
		return NULL;
	}

	return connector;
}

