/*
 * omap_plane.h -- OMAP DRM Plane
 *
 * Copyright (C) 2011 Texas Instruments
 * Author: Rob Clark <rob@ti.com>
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

#ifndef __VDRM_PLANE_H__
#define __VDRM_PLANE_H__

#include <linux/types.h>

enum drm_plane_type;

struct drm_device;
struct drm_mode_object;
struct drm_plane;

struct vdrm_plane_update;

dma_addr_t vdrm_plane_get_fb_paddr(struct drm_framebuffer *fb, u32 x, u32 y,
		bool is_uv);
void vdrm_plane_attach_vctrl_dev(struct drm_plane *plane,
		struct vdrm_ctrl_device *vdev);

void vdrm_plane_install_consumer(struct drm_plane *plane);
void vdrm_plane_uninstall_consumer(struct drm_plane *plane);
int vdrm_plane_publish(struct drm_plane *plane, struct vdrm_plane_update *u,
		struct drm_crtc *crtc, bool need_cb);

struct drm_plane *vdrm_plane_init(struct drm_device *dev,
		u32 possible_crtcs, enum drm_plane_type type,
		struct device_node *np);
void vdrm_plane_fini(struct drm_device *dev, struct drm_plane *plane);

#endif /* __VDRM_PLANE_H__ */
