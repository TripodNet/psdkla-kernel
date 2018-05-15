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

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_mode.h>
#include <drm/drm_plane_helper.h>
#include <linux/math64.h>

#include "vdrm_drv.h"

#define to_vdrm_crtc_state(x) container_of(x, struct vdrm_crtc_state, base)

struct vdrm_crtc_state {
	/* Must be first. */
	struct drm_crtc_state base;
};

#define to_vdrm_crtc(x) container_of(x, struct vdrm_crtc, base)

struct vdrm_crtc {
	struct drm_crtc base;

	struct drm_plane *plane;
	u32 id;

	spinlock_t irq_lock;
	bool irq_requested;
	bool wait_for_disable;
	struct completion framedone_completion;

	bool pending;
	wait_queue_head_t pending_wait;
	bool enabled;
	struct drm_pending_vblank_event *event;

	u32 fps;
	struct hrtimer vsync_timer;
	uint64_t nsec_to_vsync;

	struct list_head updates;

	struct device_node *device_node;

	unsigned int num_pends;
};

struct drm_plane *vdrm_crtc_get_primary_plane(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	return vdrm_crtc->plane;
}

bool vdrm_crtc_for_device_node(struct drm_crtc *crtc, struct device_node *np)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	if(vdrm_crtc->device_node == np)
		return true;

	return false;
}

static bool vdrm_crtc_is_pending(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	pending = vdrm_crtc->pending;
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

	return pending;
}

int vdrm_crtc_wait_pending(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	/*
	 * Timeout is set to a "sufficiently" high value, which should cover
	 * a single frame refresh even on slower displays.
	 */
	return wait_event_timeout(vdrm_crtc->pending_wait,
				  !vdrm_crtc_is_pending(crtc),
				  msecs_to_jiffies(250));
}

void vdrm_crtc_signal(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	spin_lock_irq(&vdrm_crtc->irq_lock);
	vdrm_crtc->num_pends--;
	spin_unlock_irq(&vdrm_crtc->irq_lock);
}

static void vdrm_crtc_publish(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	struct vdrm_plane_update *update, *temp;
	unsigned long flags;

	debug("%s\n", __func__);

	spin_lock_irqsave(&vdrm_crtc->irq_lock, flags);
	list_for_each_entry_safe(update, temp, &vdrm_crtc->updates, link) {
		/*
		 * You might have a concern that how is it not racy
		 * as vdrm_plane_publish is also called by consumer_install()
		 *
		 * Actually, it is intended to be racy! If the consumer gets
		 * installed and then there is no crtc IOCTLs for a long time,
		 * consumer might not get a buffer ... so, call plane_publish()
		 * immediately after consumer_install(), but do not expect a
		 * callback (just the state buffer, does not block vblank
		 * events)
		 *
		 * If, at the same time a CRTC IOCTL happens, it wipes off
		 * the stale buffer and writes the latest one.
		 */
		if(!vdrm_plane_publish(update->plane, update, crtc, true))
			vdrm_crtc->num_pends++;
		list_del(&update->link);
		kfree(update);
	}
	spin_unlock_irqrestore(&vdrm_crtc->irq_lock, flags);
}

void vdrm_crtc_add_disable(struct drm_crtc *crtc, struct drm_plane *plane)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	struct vdrm_plane_update *update, *search, *temp;

	debug("%s\n", __func__);

	update = kzalloc(sizeof(*update), GFP_KERNEL);
	if(WARN_ON(!update))
		return;

	update->plane = plane;
	update->active = false;

	spin_lock_irq(&vdrm_crtc->irq_lock);
	list_for_each_entry_safe(search, temp, &vdrm_crtc->updates, link) {
		if(search->plane == plane) {
			list_del(&search->link);
			kfree(search);
		}
	}
	list_add_tail(&update->link, &vdrm_crtc->updates);
	spin_unlock_irq(&vdrm_crtc->irq_lock);
}

void vdrm_crtc_add_enable(struct drm_crtc *crtc, struct drm_plane *plane)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	struct vdrm_plane_update *update, *search, *temp;
	struct drm_plane_state *state;

	debug("%s\n", __func__);

	if(WARN_ON(!plane->state))
		return;

	state = plane->state;

	if(WARN_ON(!state->fb))
		return;

	update = kzalloc(sizeof(*update), GFP_KERNEL);
	if(WARN_ON(!update))
		return;

	update->plane = plane;
	update->active = true;
	update->fourcc = state->fb->format->format;
	update->stride = state->fb->pitches[0];
	update->pos_x = state->crtc_x >> 16;
	update->pos_y = state->crtc_y >> 16;
	update->out_width = state->crtc_w >> 16;
	update->out_height = state->crtc_h >> 16;
	update->width = state->src_w >> 16;
	update->height = state->src_h >> 16;
	update->addr = vdrm_plane_get_fb_paddr(state->fb, state->src_x >> 16,
			state->src_y >> 16, false);
	if(update->fourcc == DRM_FORMAT_NV12)
		update->uv_addr = vdrm_plane_get_fb_paddr(state->fb,
				state->src_x >> 16, state->src_y >> 16, true);

	spin_lock_irq(&vdrm_crtc->irq_lock);
	list_for_each_entry_safe(search, temp, &vdrm_crtc->updates, link) {
		if(search->plane == plane) {
			list_del(&search->link);
			kfree(search);
		}
	}
	list_add_tail(&update->link, &vdrm_crtc->updates);
	spin_unlock_irq(&vdrm_crtc->irq_lock);
}

void vdrm_crtc_vblank_irq(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	bool pending;

	spin_lock(&crtc->dev->event_lock);
	/* Send the vblank event if one has been requested. */
	if (vdrm_crtc->event) {
		drm_crtc_send_vblank_event(crtc, vdrm_crtc->event);
		vdrm_crtc->event = NULL;
	}

	pending = vdrm_crtc->pending;
	vdrm_crtc->pending = false;

	if(vdrm_crtc->wait_for_disable) {
		vdrm_crtc->wait_for_disable = false;
		complete(&vdrm_crtc->framedone_completion);
	}

	spin_unlock(&crtc->dev->event_lock);

	if (pending)
		drm_crtc_vblank_put(crtc);

	wake_up(&vdrm_crtc->pending_wait);
}

static enum hrtimer_restart vsync_timer(struct hrtimer *timer)
{
	struct vdrm_crtc *vdrm_crtc = container_of(timer, struct vdrm_crtc, vsync_timer);
	struct drm_crtc *crtc = &vdrm_crtc->base;
	struct drm_device *dev = crtc->dev;

	spin_lock(&vdrm_crtc->irq_lock);
	if(vdrm_crtc->irq_requested && !vdrm_crtc->num_pends) {
		spin_unlock(&vdrm_crtc->irq_lock);

		drm_handle_vblank(dev, vdrm_crtc->id);
		vdrm_crtc_vblank_irq(crtc);
	} else
		spin_unlock(&vdrm_crtc->irq_lock);

	hrtimer_forward_now(timer, ns_to_ktime(vdrm_crtc->nsec_to_vsync));
	return HRTIMER_RESTART;
}

int vdrm_irq_enable_vblank(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	unsigned long flags;

	debug("%s\n", __func__);

	spin_lock_irqsave(&vdrm_crtc->irq_lock, flags);
	vdrm_crtc->irq_requested = true;
	spin_unlock_irqrestore(&vdrm_crtc->irq_lock, flags);

	return 0;
}

void vdrm_irq_disable_vblank(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	unsigned long flags;

	debug("%s\n", __func__);

	spin_lock_irqsave(&vdrm_crtc->irq_lock, flags);
	vdrm_crtc->irq_requested = false;
	spin_unlock_irqrestore(&vdrm_crtc->irq_lock, flags);
}

static void vdrm_crtc_destroy(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	drm_crtc_cleanup(crtc);
	kfree(vdrm_crtc);
}

static void vdrm_crtc_arm_event(struct drm_crtc *crtc)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	WARN_ON(vdrm_crtc->pending);
	vdrm_crtc->pending = true;

	if (crtc->state->event) {
		vdrm_crtc->event = crtc->state->event;
		crtc->state->event = NULL;
	}
}

static void vdrm_crtc_atomic_enable(struct drm_crtc *crtc,
				    struct drm_crtc_state *old_state)
{
	int ret;
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	if (WARN_ON(vdrm_crtc->enabled))
		return;

	vdrm_crtc->enabled = true;

	spin_lock_irq(&crtc->dev->event_lock);
	/*
	 * In most realistic usecases, a flush is called
	 * after enable, and the list will be empty in enable(),
	 * full in flush(), and ->enbaled bit will be set in
	 * flush().
	 * No worries there! the pipe contents are published
	 * in flush()
	 *
	 * In some cases, flush() is called before enable(),
	 * and therefore, even though the list is populated
	 * in flush(), the function returns because ->enabled()
	 * bit is unset.
	 * In that scenario, call publish() again in enable() and
	 * it will drain the buffers
	 */
	vdrm_crtc_publish(crtc);
	drm_crtc_vblank_on(crtc);
	ret = drm_crtc_vblank_get(crtc);
	WARN_ON(ret != 0);

	vdrm_crtc_arm_event(crtc);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void vdrm_crtc_atomic_disable(struct drm_crtc *crtc,
				     struct drm_crtc_state *old_state)
{
	int ret;
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);

	debug("%s\n", __func__);

	if (WARN_ON(!vdrm_crtc->enabled))
		return;

	spin_lock_irq(&crtc->dev->event_lock);

	ret = drm_crtc_vblank_get(crtc);
	WARN_ON(ret != 0);

	vdrm_crtc_arm_event(crtc);

	reinit_completion(&vdrm_crtc->framedone_completion);
	vdrm_crtc->wait_for_disable = true;

	spin_unlock_irq(&crtc->dev->event_lock);

	if (!wait_for_completion_timeout(&vdrm_crtc->framedone_completion,
					 msecs_to_jiffies(500)))
		dev_err(crtc->dev->dev, "Timeout waiting for framedone on crtc %d",
			vdrm_crtc->id);

	vdrm_crtc->enabled = false;

	drm_crtc_vblank_off(crtc);
}

static enum drm_mode_status vdrm_crtc_mode_valid(struct drm_crtc *crtc,
					const struct drm_display_mode *mode)
{
	debug("%s\n", __func__);

	return MODE_OK;
}

static void vdrm_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	debug("%s\n", __func__);
}

static int vdrm_crtc_atomic_check(struct drm_crtc *crtc,
				struct drm_crtc_state *state)
{
	debug("%s\n", __func__);

	return 0;
}

static void vdrm_crtc_atomic_begin(struct drm_crtc *crtc,
				   struct drm_crtc_state *old_crtc_state)
{
	debug("%s\n", __func__);
}

static void vdrm_crtc_atomic_flush(struct drm_crtc *crtc,
				   struct drm_crtc_state *old_crtc_state)
{
	struct vdrm_crtc *vdrm_crtc = to_vdrm_crtc(crtc);
	int ret;
	unsigned long flags;

	debug("%s\n", __func__);

	/* Only flush the CRTC if it is currently enabled. */
	if (!vdrm_crtc->enabled)
		return;


	ret = drm_crtc_vblank_get(crtc);
	WARN_ON(ret != 0);

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	vdrm_crtc_publish(crtc);
	vdrm_crtc_arm_event(crtc);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void vdrm_crtc_reset(struct drm_crtc *crtc)
{
	debug("%s\n", __func__);

	if (crtc->state)
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

	kfree(crtc->state);
	crtc->state = kzalloc(sizeof(struct vdrm_crtc_state), GFP_KERNEL);

	if (crtc->state)
		crtc->state->crtc = crtc;
}

static struct drm_crtc_state *
vdrm_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct vdrm_crtc_state *state, *current_state;

	debug("%s\n", __func__);

	if (WARN_ON(!crtc->state))
		return NULL;

	current_state = to_vdrm_crtc_state(crtc->state);

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);

	return &state->base;
}

static const struct drm_crtc_funcs vdrm_crtc_funcs = {
	.reset = vdrm_crtc_reset,
	.set_config = drm_atomic_helper_set_config,
	.destroy = vdrm_crtc_destroy,
	.page_flip = drm_atomic_helper_page_flip,
	.gamma_set = drm_atomic_helper_legacy_gamma_set,
	.atomic_duplicate_state = vdrm_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = vdrm_irq_enable_vblank,
	.disable_vblank = vdrm_irq_disable_vblank,
};

static const struct drm_crtc_helper_funcs vdrm_crtc_helper_funcs = {
	.mode_set_nofb = vdrm_crtc_mode_set_nofb,
	.atomic_check = vdrm_crtc_atomic_check,
	.atomic_begin = vdrm_crtc_atomic_begin,
	.atomic_flush = vdrm_crtc_atomic_flush,
	.atomic_enable = vdrm_crtc_atomic_enable,
	.atomic_disable = vdrm_crtc_atomic_disable,
	.mode_valid = vdrm_crtc_mode_valid,
};

/* initialize crtc */
struct drm_crtc *vdrm_crtc_init(struct drm_device *dev,
		u32 id,	struct drm_plane *plane,
		struct device_node *np)
{
	struct drm_crtc *crtc = NULL;
	struct vdrm_crtc *vdrm_crtc;
	struct hrtimer *timer;	
	int ret;

	debug("%s\n", __func__);

	vdrm_crtc = kzalloc(sizeof(*vdrm_crtc), GFP_KERNEL);
	if (!vdrm_crtc)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&vdrm_crtc->irq_lock);
	vdrm_crtc->id = id;
	vdrm_crtc->plane = plane;
	init_completion(&vdrm_crtc->framedone_completion);

	INIT_LIST_HEAD(&vdrm_crtc->updates);

	if(of_property_read_u32(np, "refresh", &vdrm_crtc->fps)) {
		kfree(vdrm_crtc);
		return NULL;
	}

	vdrm_crtc->nsec_to_vsync = div64_u64(1000000000ull,
			(uint64_t)vdrm_crtc->fps);

	init_waitqueue_head(&vdrm_crtc->pending_wait);

	timer = &vdrm_crtc->vsync_timer;
	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = vsync_timer;

	crtc = &vdrm_crtc->base;
	ret = drm_crtc_init_with_planes(dev, crtc, plane, NULL,
					&vdrm_crtc_funcs, NULL);
	if (ret < 0) {
		kfree(vdrm_crtc);
		return ERR_PTR(ret);
	}

	drm_crtc_helper_add(crtc, &vdrm_crtc_helper_funcs);

	hrtimer_start(timer, ns_to_ktime(vdrm_crtc->nsec_to_vsync), HRTIMER_MODE_REL);

	return crtc;
}

void vdrm_crtc_fini(struct drm_device *dev, struct drm_crtc *crtc)
{
	debug("%s\n", __func__);

	vdrm_crtc_destroy(crtc);
}
