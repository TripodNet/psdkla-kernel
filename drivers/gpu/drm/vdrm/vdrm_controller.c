#include <linux/idr.h>
#include <linux/miscdevice.h>

#include "vdrm_drv.h"

#include <uapi/vdrm_controller/v_controller_if.h>

struct vdrm_ctrl_pending_event {
	struct v_ctrl_event *event;
	struct vdrm_ctrl_device *dev;
	struct drm_crtc *crtc;
	bool need_cb;
	int id;
	int num_planes;
	dma_addr_t plane_addrs[V_CTRL_NUM_GEMS_PER_BUFFER];
};

struct vdrm_ctrl_device {
	struct miscdevice misc;
	struct drm_device *drmdev;
	struct drm_plane *plane;

	spinlock_t event_lock;
	wait_queue_head_t event_wait;

	struct vdrm_ctrl_pending_event *event;
	struct vdrm_ctrl_pending_event *pending_event;

	atomic_t open_count;
};

typedef int vdrm_ctrl_ioctl_t(struct vdrm_ctrl_device *dev, void *data);

struct vdrm_ctrl_ioctl_desc {
	unsigned int cmd;
	vdrm_ctrl_ioctl_t *func;
	const char *name;
};

#define VDRMCTRL_IOCTL_DEF(ioctl, _func)	\
	[_IOC_NR(ioctl)] = {		\
		.cmd = ioctl,			\
		.func = _func,			\
		.name = #ioctl			\
	}

static DEFINE_IDR(vdrm_ctrl_buf_idr);

/*
 * This is called from crtc_flush code (need_cb = true), or vsync (need_cb = false)
 * dev->event is populated here and any readers are woken up.
 * The expectation is:
 *     if close(fp) is called before read, dev->event will be there, and will be
 *     returned by release
 *     if read is called the dev->event = NULL, dev->pending_event = dev->event, so if
 *     close is called after read but before submit IOCTL, dev->pending_event will be
 *     released
 *     if read and submit ioctl is called, IOCTL will release dev->pending_event
 *
 * If called from vsync (need_cb = false), the signalling to CRTC does not happen
 */
int vdrm_ctrl_publish(struct vdrm_ctrl_device *dev, struct vdrm_plane_update *u,
		struct drm_crtc *crtc, bool need_cb)
{
	int ret;
	struct vdrm_ctrl_pending_event *e;
	struct v_ctrl_event_new_buffer *event;
	unsigned long flags;

	debug("%s\n", __func__);

	e = kzalloc(sizeof(*e), GFP_ATOMIC);
	if (e == NULL)
		return -ENOMEM;

	event = kzalloc(sizeof(*event), GFP_ATOMIC);
	if(!event) {
		ret = -ENOMEM;
		goto no_event;
	}

	e->dev = dev;
	e->crtc = crtc;
	e->need_cb = need_cb;

	if(u->active) {
		event->valid = 1;
		event->drm_format = u->fourcc;
		event->width = u->width;
		event->height = u->height;
		event->stride = u->stride;
		event->dst_x = u->pos_x;
		event->dst_y = u->pos_y;
		event->dst_w = u->out_width;
		event->dst_h = u->out_height;
		if(u->uv_addr) {
			e->plane_addrs[0] = u->addr;
			e->plane_addrs[1] = u->uv_addr;
			e->num_planes = 2;
		} else {
			e->plane_addrs[0] = u->addr;
			e->num_planes = 1;
		}
	} else {
		event->valid = 0;
	}

	event->base.type = V_CTRL_EVENT_TYPE_NEW_BUFFER;
	event->base.length = sizeof(*event);

	e->event = &event->base;

	spin_lock_irqsave(&dev->event_lock, flags);

	if(WARN_ON(dev->event && dev->event->need_cb)) {
		ret = -EBUSY;
		goto err_unlock;
	}

	ret = idr_alloc(&vdrm_ctrl_buf_idr, e, 0, 0, GFP_ATOMIC);
	if(ret < 0)
		goto err_unlock;

	e->id = event->v_ctrl_buf_id = ret;

	dev->event = e;
	wake_up(&dev->event_wait);

	spin_unlock_irqrestore(&dev->event_lock, flags);

	return 0;

err_unlock:
	spin_unlock_irqrestore(&dev->event_lock, flags);
	kfree(event);
no_event:
	kfree(e);
	return ret;

}

static int vdrm_ctrl_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *misc = filp->private_data;
	struct vdrm_ctrl_device *dev = container_of(misc, struct vdrm_ctrl_device, misc);

	debug("%s\n", __func__);

	if(!atomic_add_unless(&dev->open_count, 1, 1))
		return -EBUSY;

	vdrm_plane_install_consumer(dev->plane);

	return 0;
}


static int vdrm_ctrl_release(struct inode *inode, struct file *filp)
{
	struct miscdevice *misc = filp->private_data;
	struct vdrm_ctrl_device *dev = container_of(misc, struct vdrm_ctrl_device, misc);
	struct vdrm_ctrl_pending_event *e = NULL;

	debug("%s\n", __func__);

	vdrm_plane_uninstall_consumer(dev->plane);

	spin_lock_irq(&dev->event_lock);
	if(dev->event) {
		e = dev->event;
		dev->event = NULL;
	} else if(dev->pending_event) {
		e = dev->pending_event;
		dev->pending_event = NULL;
	}

	if(e)
		idr_remove(&vdrm_ctrl_buf_idr, e->id);
	spin_unlock_irq(&dev->event_lock);

	if(e) {
		if(e->need_cb)
			vdrm_crtc_signal(e->crtc);
		kfree(e->event);
		kfree(e);
	}

	WARN_ON(!atomic_add_unless(&dev->open_count, -1, 0));

	return 0;
}

static int vdrm_ctrl_buf_submit_done(struct vdrm_ctrl_device *dev, void *data)
{
	struct v_ctrl_provider_buf_submit_done *d = data;
	int ret = 0;
	struct vdrm_ctrl_pending_event *e;

	debug("%s\n", __func__);

	spin_lock_irq(&dev->event_lock);
	e = idr_remove(&vdrm_ctrl_buf_idr, d->v_ctrl_buf_id);
	if(!e) {
		spin_unlock_irq(&dev->event_lock);
		return -ENOENT;
	}

	dev->pending_event = NULL;

	spin_unlock_irq(&dev->event_lock);

	if(e->need_cb)
		vdrm_crtc_signal(e->crtc);
	kfree(e->event);
	kfree(e);

	return ret;
}

static int vdrm_ctrl_buf_to_paddr_array(struct vdrm_ctrl_device *dev, void *data)
{
	struct v_ctrl_provider_buf_to_paddr_array *d = data;
	int i, ret = 0;
	struct vdrm_ctrl_pending_event *e;

	debug("%s\n", __func__);

	spin_lock_irq(&dev->event_lock);
	e = idr_find(&vdrm_ctrl_buf_idr, d->v_ctrl_buf_id);
	if(!e) {
		spin_unlock_irq(&dev->event_lock);
		return -ENOENT;
	}

	if(!e->num_planes) {
		spin_unlock_irq(&dev->event_lock);
		return -EINVAL;
	}

	d->num_paddrs = e->num_planes;
	for(i = 0; i < e->num_planes; i++)
		d->paddrs[i] = (uint32_t)e->plane_addrs[i];

	spin_unlock_irq(&dev->event_lock);

	return ret;
}

static const struct vdrm_ctrl_ioctl_desc vdrm_ctrl_ioctls[] = {
	VDRMCTRL_IOCTL_DEF(V_CTRL_IOCTL_PROVIDER_BUF_TO_PADDR_ARRAY,  vdrm_ctrl_buf_to_paddr_array),
	VDRMCTRL_IOCTL_DEF(V_CTRL_IOCTL_PROVIDER_BUF_SUBMIT_DONE,  vdrm_ctrl_buf_submit_done),
};

#define VDRMCTRL_IOCTL_COUNT	ARRAY_SIZE(vdrm_ctrl_ioctls)

static long vdrm_ctrl_ioctl(struct file *filp,
	      unsigned int cmd, unsigned long arg)
{
	struct miscdevice *misc = filp->private_data;
	struct vdrm_ctrl_device *dev = container_of(misc, struct vdrm_ctrl_device, misc);
	const struct vdrm_ctrl_ioctl_desc *ioctl = NULL;
	unsigned int nr = _IOC_NR(cmd);
	unsigned int size;
	char *kdata = NULL;
	char __user *buffer = (char __user *)arg;
	vdrm_ctrl_ioctl_t *func;
	int retcode = -EINVAL;

	debug("%s\n", __func__);

	if(nr >= VDRMCTRL_IOCTL_COUNT)
		return -EINVAL;
	ioctl = &vdrm_ctrl_ioctls[nr];

	if(cmd != ioctl->cmd)
		return -EINVAL;

	size = _IOC_SIZE(ioctl->cmd);
	cmd = ioctl->cmd;
	func = ioctl->func;
	if (!func)
		return -EINVAL;

	if ((cmd & IOC_IN) && !access_ok(VERIFY_READ, buffer, size))
		return -EFAULT;

	if ((cmd & IOC_OUT) && !access_ok(VERIFY_WRITE, buffer, size))
		return -EFAULT;

	if (cmd & (IOC_IN | IOC_OUT)) {
		kdata = kzalloc(size, GFP_KERNEL);
		if (!kdata) {
			return -ENOMEM;
		}
	}

	if (cmd & IOC_IN) {
		if (copy_from_user(kdata, buffer, size) != 0) {
			retcode = -EFAULT;
			goto err_i1;
		}
	}

	retcode = func(dev, kdata);

	if (cmd & IOC_OUT) {
		if (copy_to_user(buffer, kdata, size) != 0)
			retcode = -EFAULT;
	}

err_i1:
	kfree(kdata);
	return retcode;
}

static ssize_t vdrm_ctrl_read(struct file *filp, char __user *buffer,
		 size_t count, loff_t *offset)
{
	struct miscdevice *misc = filp->private_data;
	struct vdrm_ctrl_device *dev = container_of(misc, struct vdrm_ctrl_device, misc);
	ssize_t ret = 0;
	struct vdrm_ctrl_pending_event *e;

	debug("%s\n", __func__);

	if (!access_ok(VERIFY_WRITE, buffer, count))
		return -EFAULT;

	spin_lock_irq(&dev->event_lock);
	if (!dev->event && filp->f_flags & O_NONBLOCK) {
		ret = -EAGAIN;
		goto out;
	}

	spin_unlock_irq(&dev->event_lock);
	ret = wait_event_interruptible(dev->event_wait, dev->event);
	spin_lock_irq(&dev->event_lock);
	if (ret < 0)
		goto out;

	e = dev->event;
	if (e->event->length > count) {
		ret = -EMSGSIZE;
		goto out;
	}

	if (__copy_to_user_inatomic(buffer, e->event, e->event->length)) {
		ret = -EFAULT;
		goto out;
	}

	ret = e->event->length;
	dev->pending_event = e;
	dev->event = NULL;

out:
	spin_unlock_irq(&dev->event_lock);

	return ret;
}

static unsigned int vdrm_ctrl_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct miscdevice *misc = filp->private_data;
	struct vdrm_ctrl_device *dev = container_of(misc, struct vdrm_ctrl_device, misc);
	unsigned int mask = 0;

	debug("%s\n", __func__);

	poll_wait(filp, &dev->event_wait, wait);

	if (!dev->event)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static const struct file_operations vdrm_ctrl_fops = {
	.owner = THIS_MODULE,
	.open = vdrm_ctrl_open,
	.release = vdrm_ctrl_release,
	.unlocked_ioctl = vdrm_ctrl_ioctl,
	.read = vdrm_ctrl_read,
	.poll = vdrm_ctrl_poll,
	.llseek = no_llseek,
};

static struct vdrm_ctrl_device *vdrm_controller_create_device(struct drm_device *dev,
		int id, bool is_crtc)
{
	int ret;
	struct vdrm_ctrl_device *priv;
	struct miscdevice *misc;

	debug("%s\n", __func__);

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if(!priv)
		return ERR_PTR(-ENOMEM);

	priv->drmdev = dev;
	atomic_set(&priv->open_count, 0);

	spin_lock_init(&priv->event_lock);
	init_waitqueue_head(&priv->event_wait);

	misc = &priv->misc;
	misc->minor = MISC_DYNAMIC_MINOR;
	misc->name = kasprintf(GFP_KERNEL, "vdrm-controller-%u-%s-%u",
			dev->driver->minor, is_crtc ? "crtc" : "plane", id);
	misc->fops = &vdrm_ctrl_fops;

	if(!misc->name) {
		ret = -ENOMEM;
		goto err_misc_alloc;
	}

	ret = misc_register(misc);
	if(ret)
		goto err_misc;

	return priv;

err_misc:
	kfree(misc->name);
err_misc_alloc:
	kfree(priv);
	return ERR_PTR(ret);
}

struct vdrm_ctrl_device *vdrm_controller_create_crtc_device(struct drm_device *dev,
		struct drm_crtc *crtc)
{
	struct vdrm_ctrl_device *vdev;

	debug("%s\n", __func__);

	vdev = vdrm_controller_create_device(dev, crtc->base.id, true);
	if(IS_ERR_OR_NULL(vdev))
		return NULL;

	vdev->plane = vdrm_crtc_get_primary_plane(crtc);

	return vdev;
}

struct vdrm_ctrl_device *vdrm_controller_create_plane_device(struct drm_device *dev,
		struct drm_plane *plane)
{
	struct vdrm_ctrl_device *vdev;

	debug("%s\n", __func__);

	vdev = vdrm_controller_create_device(dev, plane->base.id, false);
	if(IS_ERR_OR_NULL(vdev))
		return NULL;

	vdev->plane = plane;

	return vdev;
}


void vdrm_controller_delete_device(struct vdrm_ctrl_device *dev)
{
	struct miscdevice *misc = &dev->misc;

	debug("%s\n", __func__);

	misc_deregister(misc);
	kfree(misc->name);
	kfree(dev);
}


