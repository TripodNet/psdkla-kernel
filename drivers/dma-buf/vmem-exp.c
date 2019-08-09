/*
 * DMAbuf Exporter driver for virtual memory.
 *
 * Copyright(C) 2016 Texas Instruments. All rights reserved.
 * Author: Nikhil Devshatwar <nikhil.nd@ti.com>
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

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/dma-buf.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include "dmabuf-ioctl.h"

#define VMEMEXP_NAME		"vmemexp"

struct vme_ctx {
	struct device *dev;
	struct list_head list;
	spinlock_t lock;
};

struct vmem_exp_buf {
	struct device			*dev;
	struct list_head		node;
	void				*priv;

	unsigned long 			vaddr;
	unsigned long 			size;
	unsigned long 			offset;

	struct sg_table			sgt;
	unsigned int			num_pages;
	struct page			**pages;
	atomic_t			refcount;
	enum dma_data_direction		dma_dir;

	struct dma_buf			*dbuf;
	int				dmafd;
};

/* Register a new class and only one device */
static int vmemexp_major;
static struct class *vmemexp_class;
static struct device *vmemexp_dev;

static void vmem_exp_vm_open(struct vm_area_struct *vma)
{
	struct vmem_exp_buf *buf = vma->vm_private_data;

	printk(KERN_ALERT "%s: %p, refcount: %d, vma: %08lx-%08lx\n",
	__func__, buf, atomic_read(&buf->refcount), vma->vm_start,
	vma->vm_end);

	atomic_inc(&buf->refcount);
}

static void vmem_exp_vm_close(struct vm_area_struct *vma)
{
	struct vmem_exp_buf *buf = vma->vm_private_data;

	printk(KERN_ALERT "%s: %p, refcount: %d, vma: %08lx-%08lx\n",
	__func__, buf, atomic_read(&buf->refcount), vma->vm_start,
	vma->vm_end);

	atomic_dec(&buf->refcount);
}

const struct vm_operations_struct vmem_exp_vm_ops = {
	.open = vmem_exp_vm_open,
	.close = vmem_exp_vm_close,
};

static struct sg_table *vmem_exp_dmabuf_ops_map(
	struct dma_buf_attachment *db_attach, enum dma_data_direction dma_dir)
{
	struct vmem_exp_buf *buf = db_attach->dmabuf->priv;

	if (buf == NULL)
		return NULL;
	return &buf->sgt;
}

static void vmem_exp_dmabuf_ops_unmap(struct dma_buf_attachment *db_attach,
	struct sg_table *sgt, enum dma_data_direction dma_dir)
{
}

static void vmem_exp_dmabuf_ops_release(struct dma_buf *dbuf)
{
	struct vmem_exp_buf *buf = dbuf->priv;
	struct sg_table *sgt = &buf->sgt;
	int i = buf->num_pages;

	if (atomic_dec_and_test(&buf->refcount)) {
		dma_unmap_sg_attrs(buf->dev, sgt->sgl, sgt->orig_nents,
				buf->dma_dir, DMA_ATTR_SKIP_CPU_SYNC);

		while (--i >= 0) {
			if (buf->dma_dir == DMA_FROM_DEVICE) {
				set_page_dirty_lock(buf->pages[i]);
				__free_page(buf->pages[i]);
			}
		}
		sg_free_table(&buf->sgt);
		kfree(buf->pages);
		kfree(buf);
	}
}

static void *vmem_exp_dmabuf_ops_vmap(struct dma_buf *dbuf)
{
printk(KERN_ALERT "%s: kernel access not supported yet\n", __func__);
	return NULL;
}
static void *vmem_exp_dmabuf_ops_kmap(struct dma_buf *dbuf, unsigned long pgnum)
{
	printk(KERN_ALERT "%s: kernel access not supported yet\n", __func__);
	return NULL;
}

static int vmem_exp_dmabuf_ops_mmap(struct dma_buf *dbuf,
	struct vm_area_struct *vma)
{
	struct vmem_exp_buf *buf = dbuf->priv;
	unsigned long uaddr = vma->vm_start;
	unsigned long usize = vma->vm_end - vma->vm_start;
	int i = 0;

printk(KERN_ALERT "%s: debug dbuf %p\n", __func__, dbuf);
	if (!buf) {
		printk(KERN_ALERT "No memory to map\n");
		return -EINVAL;
	}

	do {
		int ret;

		ret = vm_insert_page(vma, uaddr, buf->pages[i++]);
		if (ret) {
			printk(KERN_ALERT "Remapping memory, error: %d\n", ret);
			return ret;
		}

		uaddr += PAGE_SIZE;
		usize -= PAGE_SIZE;
	} while (usize > 0);


	/*
	 * Use common vm_area operations to track buffer refcount.
	 */
	vma->vm_private_data	= buf;
	vma->vm_ops		= &vmem_exp_vm_ops;

	vma->vm_ops->open(vma);

	return 0;
}

static struct dma_buf_ops vmem_exp_dmabuf_ops = {
	.map_dma_buf = vmem_exp_dmabuf_ops_map,
	.unmap_dma_buf = vmem_exp_dmabuf_ops_unmap,
	.vmap = vmem_exp_dmabuf_ops_vmap,
	.map = vmem_exp_dmabuf_ops_kmap,
	.mmap = vmem_exp_dmabuf_ops_mmap,
	.release = vmem_exp_dmabuf_ops_release,
};

/* Perform a page table walkthrough and get ref of all the pages
 * - Minimal memory allocation - Keep it fast
 * - Directly create sgt from vaddr
 */
static int sg_alloc_from_vaddr(struct sg_table *sgt, struct page ***pagesptr,
	unsigned long vaddr, unsigned long size, unsigned long offset)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long addr, pfn;
	int first, last, i = 0, nr, locked = 0, ret = 0;
	struct page **pages;

	vaddr &= PAGE_MASK;
	/* Allocate a scatterlist for maximum fragmentation */
	first = vaddr >> PAGE_SHIFT;
	last = (vaddr + size - 1) >> PAGE_SHIFT;
	nr = last - first + 1;

	pages = kzalloc(nr * sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL)
		return -ENOMEM;

	if (sg_alloc_table(sgt, nr, GFP_KERNEL)) {
		printk(KERN_ALERT "Failed to allocated sgt for %d pages\n", nr);
		goto out;
	}

	down_read(&mm->mmap_sem);
	locked = 1;

	/* Check only for the first vma found
	 * Do not handle segmented VMA regions
	 */
	vma = find_vma_intersection(mm, vaddr, vaddr + 1);
	if (!vma) {
		printk(KERN_ALERT "Faulty vaddr %lx passed to export\n", vaddr);
		ret = -EFAULT;
		goto out;
	}

	if ((vma->vm_flags & (VM_IO | VM_PFNMAP))) {
		/* Pages are mapped - walk, get pfn -> page*/

		addr = vaddr;
		for (i = 0; i < nr; i++) {
			ret = follow_pfn(vma, addr, &pfn);
			if (ret)
				break;
			pages[i] = pfn_to_page(pfn);
			addr += PAGE_SIZE;
		}
		if (i < nr) {
			printk(KERN_ALERT "WALK: Failed to get the page for vaddr %lx\n", addr);
			goto out;
		}

	} else {
		/* Get user pages directly */

		ret = get_user_pages_locked(vaddr, nr,
			0, pages, &locked);
		if (ret != nr) {
			printk(KERN_ALERT "Got ref for %d user pages, expected %d\n",
				ret, nr);
			goto out;
		}
	}

	ret = sg_alloc_table_from_pages(sgt, pages, nr, offset, size, 0);
	if (ret) {
		printk(KERN_ALERT "Could not create SG table\n");
		goto out;
	}

	if (locked)
		up_read(&mm->mmap_sem);
	*pagesptr = pages;
	return nr;

out:
	for(; i > 0; i-- ) {
		put_page(pages[i - 1]);
	}
	if (locked)
		up_read(&mm->mmap_sem);
	return ret;
}

static struct vmem_exp_buf *vmem_exp_export(struct device *dev,
	unsigned long vaddr, unsigned long size,
	enum dma_data_direction dma_dir)
{
	struct vmem_exp_buf *buf;
	struct sg_table *sgt;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int ret = 0;

	buf = kzalloc(sizeof *buf, GFP_KERNEL);
	if (!buf)
		return NULL;

	buf->dev = dev;
	buf->vaddr = vaddr;
	buf->size = size;
	buf->offset = vaddr & ~PAGE_MASK;

	sgt = &buf->sgt;

	ret = sg_alloc_from_vaddr(sgt, &buf->pages, vaddr, size, buf->offset);
	if (ret <= 0) {
		dev_err(dev, "sg_alloc_from_vaddr failed\n");
		goto fail;
	}

	ret = dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dma_dir, DMA_ATTR_SKIP_CPU_SYNC);
	if (!ret) {
		dev_err(dev, "failed to map scatterlist\n");
		goto fail;
	}

	exp_info.ops = &vmem_exp_dmabuf_ops;
	exp_info.size = buf->size;
	exp_info.flags = O_CLOEXEC | O_ACCMODE;
	exp_info.priv = buf;

	buf->dbuf = dma_buf_export(&exp_info);
	if (IS_ERR(buf->dbuf)) {
		dev_err(dev, "Failed to create DMA buf\n");
		goto fail;
	}

	atomic_inc(&buf->refcount);
	buf->dmafd = dma_buf_fd(buf->dbuf, O_CLOEXEC);

return buf;
fail:
	kfree(buf);
	return NULL;
}

struct vmem_exp_buf *find_exported_buf(struct vme_ctx *ctx,
		unsigned long vaddr, unsigned long size)
{
	struct vmem_exp_buf *buf, *temp;
	unsigned long flags;

	spin_lock_irqsave(&ctx->lock, flags);

	list_for_each_entry_safe(buf, temp, &ctx->list, node) {

		if (buf->vaddr == vaddr && buf->size == size) {

			dev_dbg(ctx->dev, "%s: Match found\n", __func__);
			spin_unlock_irqrestore(&ctx->lock, flags);
			return buf;;
		}
	}

	spin_unlock_irqrestore(&ctx->lock, flags);
	return NULL;
}

unsigned long vmem_export_single(struct vme_ctx *ctx,
	struct dmabuf_vmem_export *req)
{
	struct vmem_exp_buf *buf;
	unsigned long flags;

	req->fd = 0;
	req->vaddr &= PAGE_MASK;

	if (req->vaddr == 0 || req->size == 0)
		return -EINVAL;

	dev_dbg(ctx->dev, "%s: Export request %ld bytes vmem at 0x%0lx\n", __func__,
		req->size, req->vaddr);

	buf = find_exported_buf(ctx, req->vaddr, req->size);
	if (buf) {
		req->fd = buf->dmafd;
		dev_dbg(ctx->dev, "%s: Buffer already exported at dmafd %d\n",
			__func__, buf->dmafd);
		return 0;
	}

	buf = vmem_exp_export(ctx->dev, req->vaddr, req->size,
				DMA_BIDIRECTIONAL);
	if (!buf) {
		dev_err(ctx->dev, "vmem %0lx export failed\n", req->vaddr);
		return -EINVAL;
	}

	buf->priv = ctx;
	spin_lock_irqsave(&ctx->lock, flags);
	list_add_tail(&buf->node, &ctx->list);
	spin_unlock_irqrestore(&ctx->lock, flags);

	req->fd = buf->dmafd;
	dev_dbg(ctx->dev, "vmem %0lx of size %ld exported as dmafd %d\n",
		buf->vaddr, buf->size, buf->dmafd);

	return 0;
}

static int vmemexp_open(struct inode *inode, struct file *filp)
{
	struct vme_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = vmemexp_dev;
	INIT_LIST_HEAD(&ctx->list);
	spin_lock_init(&ctx->lock);

	dev_dbg(ctx->dev, "Initialized new context %p\n", ctx);
	filp->private_data = ctx;
	return 0;
}

static int vmemexp_release(struct inode *inode, struct file *filp)
{
	struct vme_ctx *ctx =  (struct vme_ctx *)filp->private_data;
	struct vmem_exp_buf *buf, *temp;
	unsigned long flags;

	dev_dbg(ctx->dev, "%s", __func__);
	spin_lock_irqsave(&ctx->lock, flags);

	list_for_each_entry_safe(buf, temp, &ctx->list, node) {

		dev_dbg(ctx->dev, "%s: Unref dmafd %u\n", __func__, buf->dmafd);
		list_del(&buf->node);
	}

	spin_unlock_irqrestore(&ctx->lock, flags);

	dev_dbg(ctx->dev, "Closing context %p\n", ctx);
	kfree(ctx);
	return 0;
}

static long vmemexp_ioctl(struct file *filp,
	unsigned int cmd, unsigned long arg)
{
	struct vme_ctx *ctx =  (struct vme_ctx *)filp->private_data;
	struct dmabuf_vmem_export req;
	unsigned long ret;

	dev_dbg(ctx->dev, "%s cmd=%x", __func__, cmd);

	switch (cmd) {
	case DBUFIOC_EXPORT_VIRTMEM:

		ret = copy_from_user(&req, (void __user *)arg,
			_IOC_SIZE(cmd));
		if (ret)
			return ret;

		ret = vmem_export_single(ctx, &req);
		if (ret)
			return ret;

		ret = copy_to_user((void __user *)arg, &req,
				_IOC_SIZE(cmd));
		return ret;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vmemexp_fops = {
	.open		= vmemexp_open,
	.release	= vmemexp_release,
	.unlocked_ioctl	= vmemexp_ioctl,
};

static int __init vmemexp_init(void)
{
	vmemexp_major = register_chrdev(0, VMEMEXP_NAME, &vmemexp_fops);
	if (vmemexp_major < 0) {
		printk(KERN_ERR "Failed to register vmemexp device\n");
		return -ENODEV;
	}
	printk(KERN_INFO "vmemexp device MAJOR num = %d\n", vmemexp_major);

	vmemexp_class = class_create(THIS_MODULE, "vmemexp");
	if (IS_ERR(vmemexp_class)) {
		printk(KERN_ERR "Failed to create vmemexp class\n");
		return -EINVAL;
	}
	printk(KERN_INFO "vmemexp class registered\n");

	vmemexp_dev = device_create(vmemexp_class, NULL,
			MKDEV(vmemexp_major, 0),
			NULL, VMEMEXP_NAME);
	if (IS_ERR(vmemexp_dev)) {
		printk(KERN_ERR "Failed to create /dev/vmemexp device\n");
		return -ENODEV;
	}
	printk(KERN_INFO "/dev/vmemexp device registered\n");
	printk(KERN_INFO "ioctl DBUFIOC_EXPORT_VIRTMEM = %d\n",
		DBUFIOC_EXPORT_VIRTMEM);

	return 0;
}

static void __exit vmemexp_deinit(void)
{
	class_destroy(vmemexp_class);
	unregister_chrdev(vmemexp_major, VMEMEXP_NAME);
}

MODULE_DESCRIPTION("Exporter driver for virtual memory");
MODULE_AUTHOR("Nikhil Devshatwar <nikhil.nd@ti.com>");
MODULE_LICENSE("GPL");
module_init(vmemexp_init);
module_exit(vmemexp_deinit);
