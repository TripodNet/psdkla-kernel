#ifndef _V_CONTROLLER_IF_H_
#define _V_CONTROLLER_IF_H_

#if defined(__KERNEL__) || defined(__linux__)

#include <linux/types.h>
#include <asm/ioctl.h>

#else /* One of the BSDs */

#include <sys/ioccom.h>
#include <sys/types.h>
typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;

#endif

#define V_CTRL_NUM_GEMS_PER_BUFFER			2

struct v_ctrl_event {
	__u32 type;
	__u32 length;
};

#define V_CTRL_EVENT_TYPE_NEW_BUFFER		0x1

struct v_ctrl_event_new_buffer {
	struct v_ctrl_event base;

	__u32 valid;

	__u32 v_ctrl_buf_id;
	__u32 drm_format;
	__u32 stride;
	__u32 width;
	__u32 height;

	__u32 dst_x;
	__u32 dst_y;
	__u32 dst_w;
	__u32 dst_h;
};

struct v_ctrl_provider_buf_to_paddr_array {
	__u32 v_ctrl_buf_id;
	__u32 num_paddrs;
	__u32 paddrs[V_CTRL_NUM_GEMS_PER_BUFFER];
};

struct v_ctrl_provider_buf_submit_done {
	__u32 v_ctrl_buf_id;
};

#define V_CTRL_IOCTL_BASE			'd'
#define V_CTRL_IO(nr)				_IO(V_CTRL_IOCTL_BASE,nr)
#define V_CTRL_IOR(nr,type)			_IOR(V_CTRL_IOCTL_BASE,nr,type)
#define V_CTRL_IOW(nr,type)			_IOW(V_CTRL_IOCTL_BASE,nr,type)
#define V_CTRL_IOWR(nr,type)			_IOWR(V_CTRL_IOCTL_BASE,nr,type)

#define V_CTRL_PROVIDER_BUF_TO_PADDR_ARRAY	0x0
#define V_CTRL_PROVIDER_BUF_SUBMIT_DONE		0x1

#define V_CTRL_IOCTL_PROVIDER_BUF_TO_PADDR_ARRAY	V_CTRL_IOWR(V_CTRL_PROVIDER_BUF_TO_PADDR_ARRAY, struct v_ctrl_provider_buf_to_paddr_array)
#define V_CTRL_IOCTL_PROVIDER_BUF_SUBMIT_DONE		V_CTRL_IOWR(V_CTRL_PROVIDER_BUF_SUBMIT_DONE, struct v_ctrl_provider_buf_submit_done)
#endif
