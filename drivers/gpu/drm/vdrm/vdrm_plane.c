/*
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Rob Clark <rob.clark@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>

#include "vdrm_drv.h"

/*
 * plane funcs
 */

#define VDRM_PLANE_MAX_FORMATS (32)

struct vdrm_plane {
	struct drm_plane base;

	uint32_t nformats;
	uint32_t formats[VDRM_PLANE_MAX_FORMATS];

	struct vdrm_ctrl_device *vdev;
	atomic_t active;
};

#define to_vdrm_plane(x) container_of(x, struct vdrm_plane, base)

int vdrm_plane_publish(struct drm_plane *plane, struct vdrm_plane_update *u,
		struct drm_crtc *crtc, bool need_cb)
{
	struct vdrm_plane *vdrm_plane = to_vdrm_plane(plane);

	debug("%s\n", __func__);

	if(atomic_read(&vdrm_plane->active))
		return vdrm_ctrl_publish(vdrm_plane->vdev, u, crtc, need_cb);

	return -1;
}

void vdrm_plane_install_consumer(struct drm_plane *plane)
{
	struct vdrm_plane *vdrm_plane = to_vdrm_plane(plane);

	debug("%s\n", __func__);

	WARN_ON(!atomic_add_unless(&vdrm_plane->active, 1, 1));

	if(plane->state && plane->state->crtc) {
		struct vdrm_plane_update *u = kzalloc(sizeof(*u), GFP_KERNEL);
		struct drm_plane_state *state = plane->state;

		u->plane = plane;
		u->active = true;
		u->fourcc = state->fb->format->format;
		u->stride = state->fb->pitches[0];
		u->pos_x = state->crtc_x >> 16;
		u->pos_y = state->crtc_y >> 16;
		u->out_width = state->crtc_w >> 16;
		u->out_height = state->crtc_h >> 16;
		u->width = state->src_w >> 16;
		u->height = state->src_h >> 16;
		u->addr = vdrm_plane_get_fb_paddr(state->fb, state->src_x >> 16,
				state->src_y >> 16, false);
		if(u->fourcc == DRM_FORMAT_NV12)
			u->uv_addr = vdrm_plane_get_fb_paddr(state->fb,
					state->src_x >> 16, state->src_y >> 16, true);

		vdrm_plane_publish(plane, u, NULL, false);
		kfree(u);
	}
}

void vdrm_plane_uninstall_consumer(struct drm_plane *plane)
{
	struct vdrm_plane *vdrm_plane = to_vdrm_plane(plane);

	debug("%s\n", __func__);

	WARN_ON(!atomic_add_unless(&vdrm_plane->active, -1, 0));
}


void vdrm_plane_attach_vctrl_dev(struct drm_plane *plane,
		struct vdrm_ctrl_device *vdev)
{
	struct vdrm_plane *vdrm_plane = to_vdrm_plane(plane);

	debug("%s\n", __func__);

	vdrm_plane->vdev = vdev;
}

dma_addr_t vdrm_plane_get_fb_paddr(struct drm_framebuffer *fb, u32 x, u32 y,
		bool is_uv)
{
	struct drm_gem_cma_object *gem;

	debug("%s\n", __func__);

	if(!is_uv) {
		gem = drm_fb_cma_get_gem_obj(fb, 0);
		return gem->paddr + fb->offsets[0] + x * fb->format->cpp[0] +
			y * fb->pitches[0];
	} else {
		gem = drm_fb_cma_get_gem_obj(fb, 1);
		return gem->paddr + fb->offsets[1] +
			(x * fb->format->cpp[1] / fb->format->hsub) +
			(y * fb->pitches[1] / fb->format->vsub);
	}
}

static int vdrm_plane_atomic_check(struct drm_plane *plane,
				    struct drm_plane_state *state)
{
	int ret;
	struct drm_crtc_state *crtc_state;

	debug("%s\n", __func__);

	if (!state->crtc) {
		/*
		 * The visible field is not reset by the DRM core but only
		 * updated by drm_plane_helper_check_state(), set it manually.
		 */
		state->visible = false;
		return 0;
	}

	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  0,
						  INT_MAX,
						  true, true);
	if (ret < 0)
		return ret;

	if (!state->visible)
		return 0;

	return 0;

}

static void vdrm_plane_atomic_update(struct drm_plane *plane,
				      struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;

	debug("%s\n", __func__);

	if (WARN_ON(!state->crtc))
		return;

	if (!state->visible)
		vdrm_crtc_add_disable(state->crtc, plane);
	else
		vdrm_crtc_add_enable(state->crtc, plane);
}

static void vdrm_plane_atomic_disable(struct drm_plane *plane,
				       struct drm_plane_state *old_state)
{
	debug("%s\n", __func__);

	if (WARN_ON(!old_state->crtc))
		return;

	vdrm_crtc_add_disable(old_state->crtc, plane);
}

static void vdrm_plane_destroy(struct drm_plane *plane)
{
	struct vdrm_plane *vdrm_plane = to_vdrm_plane(plane);

	debug("%s\n", __func__);

	drm_plane_cleanup(plane);
	kfree(vdrm_plane);
}

static const struct drm_plane_helper_funcs vdrm_plane_helper_funcs = {
	.atomic_check = vdrm_plane_atomic_check,
	.atomic_update = vdrm_plane_atomic_update,
	.atomic_disable = vdrm_plane_atomic_disable,
};

static const struct drm_plane_funcs vdrm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = drm_atomic_helper_plane_reset,
	.destroy = vdrm_plane_destroy,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* initialize plane */
struct drm_plane *vdrm_plane_init(struct drm_device *dev,
		u32 possible_crtcs,
		enum drm_plane_type type, struct device_node *np)
{
	struct vdrm_plane *vdrm_plane;
	int ret;

	debug("%s\n", __func__);

	vdrm_plane = kzalloc(sizeof(*vdrm_plane), GFP_KERNEL);
	if (!vdrm_plane)
		return ERR_PTR(-ENOMEM);

	atomic_set(&vdrm_plane->active, 0);

	ret = of_property_count_elems_of_size(np, "supported-formats", sizeof(uint32_t));
	if(ret <= 0 || ret > ARRAY_SIZE(vdrm_plane->formats))
		goto error;

	vdrm_plane->nformats = ret;
	ret = of_property_read_u32_array(np, "supported-formats",
			vdrm_plane->formats, vdrm_plane->nformats);
	if(ret)
		goto error;

	ret = drm_universal_plane_init(dev, &vdrm_plane->base,
				       possible_crtcs,
				       &vdrm_plane_funcs,
				       vdrm_plane->formats, vdrm_plane->nformats,
				       NULL, type, NULL);
	if (ret < 0)
		goto error;

	drm_plane_helper_add(&vdrm_plane->base, &vdrm_plane_helper_funcs);

	return &vdrm_plane->base;

error:
	kfree(vdrm_plane);
	return NULL;
}

void vdrm_plane_fini(struct drm_device *dev, struct drm_plane *plane)
{
	debug("%s\n", __func__);

	vdrm_plane_destroy(plane);
}
