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

#ifndef __VDRM_CONTROLLER_H__
#define __VDRM_CONTROLLER_H__

struct vdrm_plane_update;
struct vdrm_ctrl_device;

int vdrm_ctrl_publish(struct vdrm_ctrl_device *dev, struct vdrm_plane_update *u,
		struct drm_crtc *crtc, bool need_cb);

struct vdrm_ctrl_device *vdrm_controller_create_crtc_device(struct drm_device *dev,
		struct drm_crtc *crtc);
struct vdrm_ctrl_device *vdrm_controller_create_plane_device(struct drm_device *dev,
		struct drm_plane *plane);
void vdrm_controller_delete_device(struct vdrm_ctrl_device *dev);

#endif /*__VDRM_CONTROLLER_H__*/


