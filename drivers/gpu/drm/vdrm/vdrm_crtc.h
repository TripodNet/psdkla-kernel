/*
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
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

#ifndef __VDRM_CRTC_H__
#define __VDRM_CRTC_H__

struct vdrm_plane_update {
	struct drm_plane *plane;
	bool active;

	u32 fourcc;
	u32 stride;
	u32 pos_x;
	u32 pos_y;
	u32 out_width;
	u32 out_height;
	u32 width;
	u32 height;
	dma_addr_t addr;
	dma_addr_t uv_addr;

	struct list_head link;
};

int vdrm_crtc_wait_pending(struct drm_crtc *crtc);
bool vdrm_crtc_for_device_node(struct drm_crtc *crtc, struct device_node *np);
struct drm_plane *vdrm_crtc_get_primary_plane(struct drm_crtc *crtc);

void vdrm_crtc_signal(struct drm_crtc *crtc);

void vdrm_crtc_add_disable(struct drm_crtc *crtc, struct drm_plane *plane);
void vdrm_crtc_add_enable(struct drm_crtc *crtc, struct drm_plane *plane);

struct drm_crtc *vdrm_crtc_init(struct drm_device *dev,
		u32 id,	struct drm_plane *plane,
		struct device_node *np);
void vdrm_crtc_fini(struct drm_device *dev, struct drm_crtc *crtc);

#endif /*__VDRM_CRTC_H__*/
