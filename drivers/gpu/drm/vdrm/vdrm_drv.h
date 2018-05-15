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

#ifndef __VDRM_DRV_H__
#define __VDRM_DRV_H__

#include <linux/module.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem.h>

#include "vdrm_controller.h"
#include "vdrm_connector.h"
#include "vdrm_plane.h"
#include "vdrm_crtc.h"

#define debug(fmt, ...)

#define MODULE_NAME     "vdrm"

/*
 * Global private object state for tracking resources that are shared across
 * multiple kms objects (planes/crtcs/etc).
 */
#define to_vdrm_global_state(x) container_of(x, struct vdrm_global_state, base)
struct vdrm_global_state {
	struct drm_private_state base;

	struct drm_atomic_state *state;
};

struct vdrm_drm_private {
	struct drm_device *ddev;
	struct device *dev;

	/*
	 * Global private object state, Do not access directly, use
	 * vdrm_global_get_state()
	 */
	struct drm_modeset_lock glob_obj_lock;
	struct drm_private_obj glob_obj;

	struct list_head plane_infos;
	struct list_head crtc_infos;

	u32 num_pipes;

	struct workqueue_struct *wq;

	/* lock for obj_list below */
	struct mutex list_lock;

	/* list of GEM objects: */
	struct list_head obj_list;

	/* irq handling: */
	spinlock_t wait_lock;		/* protects the wait_list */
	struct list_head wait_list;	/* list of omap_irq_wait */
	u32 irq_mask;			/* enabled irqs in addition to wait_list */
};


int vdrm_debugfs_init(struct drm_minor *minor);
struct vdrm_global_state *__must_check
vdrm_get_global_state(struct drm_atomic_state *s);
struct vdrm_global_state *
vdrm_get_existing_global_state(struct vdrm_drm_private *priv);

#endif /* __VDRM_DRV_H__ */
