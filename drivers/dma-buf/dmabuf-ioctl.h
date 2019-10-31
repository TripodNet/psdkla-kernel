#ifndef UAPI_DMABUF_IOCTL_H
#define UAPI_DMABUF_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

enum dmabuf_flush_type {
	CACHE_INVALIDATE,
	CACHE_WRITEBACK
};

struct dmabuf_vmem_export {
	unsigned long vaddr;
	unsigned long size;
	int fd;
};

struct dmabuf_cache_flush {
	int fd;
	enum dmabuf_flush_type type;
};

#define DMABUF_CODE	0xDB
#define DMABUF_BASE	0xD0

#define DBUFIOC_EXPORT_VIRTMEM	_IOWR(DMABUF_CODE, DMABUF_BASE + 0, struct dmabuf_vmem_export)
#define DBUFIOC_CACHE_SYNC	_IOWR(DMABUF_CODE, DMABUF_BASE + 0, struct dmabuf_cache_flush)

#endif /* UAPI_DMABUF_IOCTL_H */
