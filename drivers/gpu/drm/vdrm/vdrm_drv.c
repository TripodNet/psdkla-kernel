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

#include <linux/of.h>
#include <linux/sort.h>
#include <linux/sys_soc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_panel.h>

#include "vdrm_drv.h"

#define DRIVER_NAME		MODULE_NAME
#define DRIVER_DESC		"V DRM"
#define DRIVER_DATE		"20191105"
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

struct vdrm_plane_info {
	struct drm_plane *plane;
	struct vdrm_ctrl_device *vdev;

	struct list_head link;
};

struct vdrm_crtc_info {
	struct drm_crtc *crtc;
	struct drm_connector *connector;
	struct drm_plane *plane;
	struct drm_encoder *encoder;
	struct vdrm_ctrl_device *vdev;

	struct list_head link;
};

/*
 * mode config funcs
 */

static void vdrm_atomic_wait_for_completion(struct drm_device *dev,
					    struct drm_atomic_state *old_state)
{
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;
	int ret;

	debug("%s\n", __func__);

	for_each_new_crtc_in_state(old_state, crtc, new_crtc_state, i) {
		if (!new_crtc_state->active)
			continue;

		ret = vdrm_crtc_wait_pending(crtc);

		if (!ret)
			dev_warn(dev->dev,
				 "atomic complete timeout (pipe %u)!\n", i);
	}
}

static void vdrm_atomic_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;

	debug("%s\n", __func__);

	/* Apply the atomic update. */
	drm_atomic_helper_commit_modeset_disables(dev, old_state);
	drm_atomic_helper_commit_modeset_enables(dev, old_state);
	vdrm_atomic_wait_for_completion(dev, old_state);

	drm_atomic_helper_commit_planes(dev, old_state, 0);
	drm_atomic_helper_commit_hw_done(old_state);

	/*
	 * Wait for completion of the page flips to ensure that old buffers
	 * can't be touched by the hardware anymore before cleaning up planes.
	 */
	vdrm_atomic_wait_for_completion(dev, old_state);

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

static const struct drm_mode_config_helper_funcs vdrm_mode_config_helper_funcs = {
	.atomic_commit_tail = vdrm_atomic_commit_tail,
};

static const struct drm_mode_config_funcs vdrm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.output_poll_changed = drm_fb_helper_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Global/shared object state funcs */

/*
 * This is a helper that returns the private state currently in operation.
 * Note that this would return the "old_state" if called in the atomic check
 * path, and the "new_state" after the atomic swap has been done.
 */
struct vdrm_global_state *
vdrm_get_existing_global_state(struct vdrm_drm_private *priv)
{
	debug("%s\n", __func__);

	return to_vdrm_global_state(priv->glob_obj.state);
}

/*
 * This acquires the modeset lock set aside for global state, creates
 * a new duplicated private object state.
 */
struct vdrm_global_state *__must_check
vdrm_get_global_state(struct drm_atomic_state *s)
{
	struct vdrm_drm_private *priv = s->dev->dev_private;
	struct drm_private_state *priv_state;

	debug("%s\n", __func__);

	priv_state = drm_atomic_get_private_obj_state(s, &priv->glob_obj);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_vdrm_global_state(priv_state);
}

static struct drm_private_state *
vdrm_global_duplicate_state(struct drm_private_obj *obj)
{
	struct vdrm_global_state *state;

	debug("%s\n", __func__);

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void vdrm_global_destroy_state(struct drm_private_obj *obj,
				      struct drm_private_state *state)
{
	struct vdrm_global_state *vdrm_state = to_vdrm_global_state(state);

	debug("%s\n", __func__);

	kfree(vdrm_state);
}

static const struct drm_private_state_funcs vdrm_global_state_funcs = {
	.atomic_duplicate_state = vdrm_global_duplicate_state,
	.atomic_destroy_state = vdrm_global_destroy_state,
};

static int vdrm_global_obj_init(struct drm_device *dev)
{
	struct vdrm_drm_private *priv = dev->dev_private;
	struct vdrm_global_state *state;

	debug("%s\n", __func__);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	drm_atomic_private_obj_init(dev, &priv->glob_obj, &state->base,
				    &vdrm_global_state_funcs);
	return 0;
}

static void vdrm_global_obj_fini(struct vdrm_drm_private *priv)
{
	debug("%s\n", __func__);

	drm_atomic_private_obj_fini(&priv->glob_obj);
}

static void vdrm_encoder_destroy(struct drm_encoder *encoder)
{
	debug("%s\n", __func__);

	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static const struct drm_encoder_funcs vdrm_encoder_funcs = {
	.destroy = vdrm_encoder_destroy,
};

static void vdrm_plane_info_delete_all(struct drm_device *dev)
{
	struct vdrm_plane_info *plane_info, *temp;
	struct vdrm_drm_private *priv = dev->dev_private;

	debug("%s\n", __func__);

	list_for_each_entry_safe(plane_info, temp, &priv->plane_infos, link) {
		list_del(&plane_info->link);
		vdrm_controller_delete_device(plane_info->vdev);
		vdrm_plane_fini(dev, plane_info->plane);
		kfree(plane_info);
	}
}

static void vdrm_crtc_info_delete_all(struct drm_device *dev)
{
	struct vdrm_crtc_info *crtc_info, *temp;
	struct vdrm_drm_private *priv = dev->dev_private;

	debug("%s\n", __func__);

	list_for_each_entry_safe(crtc_info, temp, &priv->crtc_infos, link) {
		list_del(&crtc_info->link);
		vdrm_controller_delete_device(crtc_info->vdev);
		vdrm_crtc_fini(dev, crtc_info->crtc);
		vdrm_plane_fini(dev, crtc_info->plane);
		drm_encoder_cleanup(crtc_info->encoder);
		vdrm_connector_fini(crtc_info->connector);
		kfree(crtc_info);
	}
}

static bool vdrm_create_new_crtc(struct drm_device *dev, uint32_t id, struct device_node *np)
{
	int ret;
	struct vdrm_drm_private *priv = dev->dev_private;
	struct vdrm_crtc_info *crtc_info;

	debug("%s\n", __func__);

	crtc_info = kzalloc(sizeof(*crtc_info), GFP_KERNEL);
	if(!crtc_info)
		return false;

	crtc_info->encoder = kzalloc(sizeof(*crtc_info->encoder), GFP_KERNEL);
	if(!crtc_info->encoder)
		goto encoder_alloc_fail;

	ret = drm_encoder_init(dev, crtc_info->encoder, &vdrm_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret)
		goto encoder_fail;

	crtc_info->encoder->possible_crtcs = 1 << id;

	crtc_info->connector = vdrm_connector_init(dev, crtc_info->encoder, np);
	if (!crtc_info->connector)
		goto connector_fail;

	crtc_info->plane = vdrm_plane_init(dev, (1 << id), DRM_PLANE_TYPE_PRIMARY, np);
	if (!crtc_info->plane)
		goto plane_fail;

	crtc_info->crtc = vdrm_crtc_init(dev, id, crtc_info->plane, np);
	if (!crtc_info->crtc)
		goto crtc_fail;

	crtc_info->vdev = vdrm_controller_create_crtc_device(dev, crtc_info->crtc);
	if(!crtc_info->vdev)
		goto vdev_fail;

	vdrm_plane_attach_vctrl_dev(crtc_info->plane, crtc_info->vdev);
	list_add_tail(&crtc_info->link, &priv->crtc_infos);

	return true;

vdev_fail:
	vdrm_crtc_fini(dev, crtc_info->crtc);
crtc_fail:
	vdrm_plane_fini(dev, crtc_info->plane);
plane_fail:
	vdrm_connector_fini(crtc_info->connector);
connector_fail:
	drm_encoder_cleanup(crtc_info->encoder);
encoder_fail:
	kfree(crtc_info->encoder);
encoder_alloc_fail:
	kfree(crtc_info);
	return false;
}

static int vdrm_modeset_init(struct drm_device *dev)
{
	int i;
	struct vdrm_drm_private *priv = dev->dev_private;
	struct device_node *np = dev->dev->of_node;
	struct device_node *child = NULL;

	debug("%s\n", __func__);

	drm_mode_config_init(dev);

	INIT_LIST_HEAD(&priv->crtc_infos);
	INIT_LIST_HEAD(&priv->plane_infos);

	i = 0;
	while((child = of_get_next_child(np, child)) != NULL) {
		if(of_device_is_compatible(child, "ti,dra7-vdrm-crtc")) {
			if(!vdrm_create_new_crtc(dev, i, child)) {
				of_node_put(child);
				goto out_fail;
			}
			i++;
			priv->num_pipes++;
		}
	}

	child = NULL;
	while((child = of_get_next_child(np, child)) != NULL) {
		if(of_device_is_compatible(child, "ti,dra7-vdrm-plane")) {
			struct of_phandle_args crtcs;
			int rc;
			int count = 0;
			uint32_t crtc_mask = 0;
			struct vdrm_plane_info *plane_info;
			struct drm_plane *plane;
			struct drm_crtc *crtc;
			struct vdrm_ctrl_device *vdev;

			plane_info = kzalloc(sizeof(*plane_info), GFP_KERNEL);
			if(!plane_info) {
				of_node_put(child);
				goto plane_out_fail;
			}

			do {
				rc = of_parse_phandle_with_args(child, "supported-crtcs", NULL, count, &crtcs);
				if(rc)
					break;
				count++;
				drm_for_each_crtc(crtc, dev) {
					if(vdrm_crtc_for_device_node(crtc, crtcs.np))
						crtc_mask |= (1 << drm_crtc_index(crtc));
				}
				of_node_put(crtcs.np);
			} while(1);

			if(!count) {
				of_node_put(child);
				kfree(plane_info);
				goto plane_out_fail;
			}

			plane = vdrm_plane_init(dev, crtc_mask, DRM_PLANE_TYPE_OVERLAY, child);
			if(!plane) {
				of_node_put(child);
				kfree(plane_info);
				goto plane_out_fail;
			}

			vdev = vdrm_controller_create_plane_device(dev, plane);
			if(!vdev) {
				vdrm_plane_fini(dev, plane);
				of_node_put(child);
				kfree(plane_info);
				goto plane_out_fail;
			}

			vdrm_plane_attach_vctrl_dev(plane, vdev);
			plane_info->plane = plane;
			plane_info->vdev = vdev;

			list_add_tail(&plane_info->link, &priv->plane_infos);
		}
	}

	dev->mode_config.min_width = 16;
	dev->mode_config.min_height = 16;
	dev->mode_config.max_width = 2048;
	dev->mode_config.max_height = 2048;

	dev->mode_config.funcs = &vdrm_mode_config_funcs;

	drm_mode_config_reset(dev);

	dev->irq_enabled = true;

	return 0;

plane_out_fail:
	vdrm_plane_info_delete_all(dev);
out_fail:
	vdrm_crtc_info_delete_all(dev);
	drm_mode_config_cleanup(dev);
	return -ENOMEM;

}

static void vdrm_modeset_fini(struct drm_device *dev, bool shutdown)
{
	debug("%s\n", __func__);

	dev->irq_enabled = false;
	vdrm_plane_info_delete_all(dev);
	vdrm_crtc_info_delete_all(dev);
	if(shutdown)
		drm_atomic_helper_shutdown(dev);
	drm_mode_config_cleanup(dev);
}

/*
 * drm driver funcs
 */

DEFINE_DRM_GEM_CMA_FOPS(vdrmdriver_fops);

static struct drm_driver vdrm_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM  | DRIVER_PRIME |
		DRIVER_ATOMIC,
	.gem_free_object_unlocked = drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import	= drm_gem_prime_import,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,
	.dumb_create		= drm_gem_cma_dumb_create,
	.fops                   = &vdrmdriver_fops,
	.name                   = DRIVER_NAME,
	.desc                   = DRIVER_DESC,
	.date                   = DRIVER_DATE,
	.major                  = DRIVER_MAJOR,
	.minor                  = DRIVER_MINOR,
};

static int vdrm_init(struct vdrm_drm_private *priv, struct device *dev)
{
	struct drm_device *ddev;
	int ret;
	struct vdrm_crtc_info *crtc_info;

	debug("%s\n", __func__);

	/* Allocate and initialize the DRM device. */
	ddev = drm_dev_alloc(&vdrm_drm_driver, dev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	priv->ddev = ddev;
	ddev->dev_private = priv;

	priv->dev = dev;

	priv->wq = alloc_ordered_workqueue("vdrm", 0);

	mutex_init(&priv->list_lock);
	INIT_LIST_HEAD(&priv->obj_list);

	ret = vdrm_modeset_init(ddev);
	if (ret) {
		dev_err(priv->dev, "vdrm_modeset_init failed: ret=%d\n", ret);
		goto err_free;
	}

	ret = vdrm_global_obj_init(ddev);
	if (ret)
		goto err_vdrm_modeset_fini;


	/* Initialize vblank handling, start with all CRTCs disabled. */
	ret = drm_vblank_init(ddev, priv->num_pipes);
	if (ret) {
		dev_err(priv->dev, "could not init vblank\n");
		goto err_global_object_fini;
	}

	list_for_each_entry(crtc_info, &priv->crtc_infos, link)
		drm_crtc_vblank_off(crtc_info->crtc);

	drm_kms_helper_poll_init(ddev);

	/*
	 * Register the DRM device with the core and the connectors with
	 * sysfs.
	 */
	ret = drm_dev_register(ddev, 0);
	if (ret)
		goto err_cleanup_helpers;

	drm_fbdev_generic_setup(ddev, 32);

	return 0;

err_cleanup_helpers:
	drm_kms_helper_poll_fini(ddev);
err_global_object_fini:
	vdrm_global_obj_fini(priv);
err_vdrm_modeset_fini:
	vdrm_modeset_fini(ddev, false);
err_free:
	destroy_workqueue(priv->wq);
	drm_dev_put(ddev);
	return ret;
}

static void vdrm_cleanup(struct vdrm_drm_private *priv)
{
	struct drm_device *ddev = priv->ddev;

	debug("%s\n", __func__);

	drm_dev_unregister(ddev);

	drm_kms_helper_poll_fini(ddev);

	vdrm_global_obj_fini(priv);

	vdrm_modeset_fini(ddev, true);

	destroy_workqueue(priv->wq);

	drm_dev_put(ddev);
}

static int pdev_probe(struct platform_device *pdev)
{
	struct vdrm_drm_private *priv;
	int ret;

	debug("%s\n", __func__);

	/* Allocate and initialize the driver private structure. */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	ret = vdrm_init(priv, &pdev->dev);
	if (ret < 0)
		kfree(priv);

	return ret;
}

static int pdev_remove(struct platform_device *pdev)
{
	struct vdrm_drm_private *priv = platform_get_drvdata(pdev);

	debug("%s\n", __func__);

	vdrm_cleanup(priv);
	kfree(priv);

	return 0;
}

static const struct of_device_id v_drm_of_match[] = {
	{ .compatible = "ti,dra7-vdrm", },
	{},
};

static struct platform_driver pdev = {
	.driver = {
		.name = "vdrm",
		.of_match_table = v_drm_of_match,
	},
	.probe = pdev_probe,
	.remove = pdev_remove,
};

static struct platform_driver * const drivers[] = {
	&pdev,
};

static int __init vdrm_drm_init(void)
{
	debug("%s\n", __func__);

	return platform_register_drivers(drivers, ARRAY_SIZE(drivers));
}

static void __exit vdrm_drm_fini(void)
{
	debug("%s\n", __func__);

	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
}

/* need late_initcall() so we load after dss_driver's are loaded */
late_initcall(vdrm_drm_init);
module_exit(vdrm_drm_fini);

MODULE_AUTHOR("Subhajit Paul <subhajit_paul@ti.com>");
MODULE_DESCRIPTION("Virtual DRM Display Driver");
MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_LICENSE("GPL v2");
