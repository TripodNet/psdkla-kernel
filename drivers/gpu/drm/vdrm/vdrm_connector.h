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

#ifndef __VDRM_CONNECTOR_H__
#define __VDRM_CONNECTOR_H__

struct drm_connector *vdrm_connector_init(struct drm_device *dev, struct drm_encoder *encoder,
		struct device_node *np);
void vdrm_connector_fini(struct drm_connector *connector);

#endif /*__VDRM_CONNECTOR_H__*/

