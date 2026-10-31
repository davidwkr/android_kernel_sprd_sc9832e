/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Goodix fingerprint HAL interface (ioctl numbers, structures, netlink
 * events). The definitions mirror the public GPL Goodix driver (see the
 * provenance note in gf_common.c); this header was written for this tree
 * because the upstream header carries no license notice.
 */
#ifndef __GF_COMMON_H
#define __GF_COMMON_H

#include <linux/types.h>
#include <linux/compat.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spi/spi.h>
#include <linux/wakelock.h>

#define GF_IOC_MAGIC	'g'

enum gf_key_event {
	GF_KEY_NONE = 0,
	GF_KEY_HOME,
	GF_KEY_POWER,
	GF_KEY_MENU,
	GF_KEY_BACK,
	GF_KEY_CAMERA,
};

enum gf_nav_event {
	GF_NAV_NONE = 0,
	GF_NAV_FINGER_UP,
	GF_NAV_FINGER_DOWN,
	GF_NAV_UP,
	GF_NAV_DOWN,
	GF_NAV_LEFT,
	GF_NAV_RIGHT,
	GF_NAV_CLICK,
	GF_NAV_HEAVY,
	GF_NAV_LONG_PRESS,
	GF_NAV_DOUBLE_CLICK,
};
typedef enum gf_nav_event gf_nav_event_t;

enum gf_netlink_cmd {
	GF_NETLINK_TEST = 0,
	GF_NETLINK_IRQ = 1,
	GF_NETLINK_SCREEN_OFF,
	GF_NETLINK_SCREEN_ON,
};

struct gf_key {
	enum gf_key_event key;
	uint32_t value;		/* key down = 1, key up = 0 */
};

struct gf_ioc_transfer {
	u8 cmd;
	u8 reserved;
	u16 addr;
	u32 len;
	u8 *buf;
};

struct gf_ioc_transfer_raw {
	u32 len;
	u8 *read_buf;
	u8 *write_buf;
	uint32_t high_time;
	uint32_t low_time;
};

struct gf_ioc_chip_info {
	u8 vendor_id;
	u8 mode;
	u8 operation;
	u8 reserved[5];
};

enum gf_spi_cpol { GF_SPI_CPOL_0, GF_SPI_CPOL_1 };
enum gf_spi_cpha { GF_SPI_CPHA_0, GF_SPI_CPHA_1 };

typedef struct {
	unsigned int cs_setuptime;
	unsigned int cs_holdtime;
	unsigned int cs_idletime;
	unsigned int speed_hz;
	unsigned int duty_cycle;
	enum gf_spi_cpol cpol;
	enum gf_spi_cpol cpha;
} gf_spi_cfg_t;

#define GF_IOC_INIT		_IOR(GF_IOC_MAGIC, 0, u8)
#define GF_IOC_EXIT		_IO(GF_IOC_MAGIC, 1)
#define GF_IOC_RESET		_IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ	_IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ	_IO(GF_IOC_MAGIC, 4)
#define GF_IOC_ENABLE_SPI_CLK	_IOW(GF_IOC_MAGIC, 5, uint32_t)
#define GF_IOC_DISABLE_SPI_CLK	_IO(GF_IOC_MAGIC, 6)
#define GF_IOC_ENABLE_POWER	_IO(GF_IOC_MAGIC, 7)
#define GF_IOC_DISABLE_POWER	_IO(GF_IOC_MAGIC, 8)
#define GF_IOC_INPUT_KEY_EVENT	_IOW(GF_IOC_MAGIC, 9, struct gf_key)
#define GF_IOC_ENTER_SLEEP_MODE	_IO(GF_IOC_MAGIC, 10)
#define GF_IOC_GET_FW_INFO	_IOR(GF_IOC_MAGIC, 11, u8)
#define GF_IOC_REMOVE		_IO(GF_IOC_MAGIC, 12)
#define GF_IOC_CHIP_INFO	_IOW(GF_IOC_MAGIC, 13, struct gf_ioc_chip_info)
#define GF_IOC_NAV_EVENT	_IOW(GF_IOC_MAGIC, 14, gf_nav_event_t)
#define GF_IOC_TRANSFER_CMD	_IOWR(GF_IOC_MAGIC, 15, struct gf_ioc_transfer)
#define GF_IOC_TRANSFER_RAW_CMD	_IOWR(GF_IOC_MAGIC, 16, struct gf_ioc_transfer_raw)
#define GF_IOC_SPI_INIT_CFG_CMD	_IOW(GF_IOC_MAGIC, 17, gf_spi_cfg_t)

#ifdef CONFIG_COMPAT
/* 32-bit userspace layout of the structures that hold pointers */
struct gf_ioc_transfer_compat {
	u8 cmd;
	u8 reserved;
	u16 addr;
	u32 len;
	compat_uptr_t buf;
};

struct gf_ioc_transfer_raw_compat {
	u32 len;
	compat_uptr_t read_buf;
	compat_uptr_t write_buf;
	uint32_t high_time;
	uint32_t low_time;
};
#define GF_IOC_TRANSFER_RAW_CMD_COMPAT \
	_IOWR(GF_IOC_MAGIC, 16, struct gf_ioc_transfer_raw_compat)
#endif

struct gf_device {
	struct miscdevice misc;
	struct kref ref;
	bool dead;
	struct spi_device *spi;
	struct input_dev *input;
	struct sock *nl_sk;
	struct notifier_block fb_nb;
	struct wake_lock wake_lock;
	struct mutex lock;		/* ioctl/open/release serialisation */
	u8 *spi_buffer;
	unsigned int bufsiz;
	int reset_gpio;
	int irq_gpio;
	int pwr_gpio;
	int irq;
	bool irq_enabled;
	bool fb_registered;
	bool system_status;
	u8 vendor_id;
};

int gf_spi_read_bytes(struct gf_device *gf, u16 addr, u32 len, u8 *rx);
int gf_spi_write_bytes(struct gf_device *gf, u16 addr, u32 len, const u8 *tx);
int gf_spi_transfer_raw(struct gf_device *gf, const u8 *tx, u8 *rx, u32 len);
void gf_spi_setup_conf(struct gf_device *gf, u32 speed);

#endif /* __GF_COMMON_H */
