/* Goodix's GF516/GF318/GF516M/GF318M/GF518M/GF3118M/GF5118M
 *  fingerprint sensor linux driver for REE and factory mode
 *
 * 2010 - 2015 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


/*
 * Provenance: derived from drivers/input/fingerprint/gf3626/gf_spi_access.c
 * of Supernote-Ratta/kernel_a6x_a5x (commit bdc77c244cd3), GPL-2.0-or-later
 * per the header above. Reworked: length checks against the bounce buffer,
 * error propagation, no per-call allocation.
 */

#include <linux/spi/spi.h>
#include <linux/string.h>

#include "gf_common.h"

void gf_spi_setup_conf(struct gf_device *gf, u32 speed)
{
	/* upstream maps every speed index to 2.4 MHz */
	gf->spi->mode = SPI_MODE_0;
	gf->spi->max_speed_hz = 2400000;
	gf->spi->bits_per_word = 8;

	if (spi_setup(gf->spi))
		dev_err(&gf->spi->dev, "failed to set up spi\n");
}

/* caller holds gf->lock; the bounce buffer is gf->bufsiz bytes */
int gf_spi_read_bytes(struct gf_device *gf, u16 addr, u32 len, u8 *rx)
{
	struct spi_transfer xfer[2] = {};
	struct spi_message msg;
	u8 *buf = gf->spi_buffer;
	int ret;

	if (len == 0 || len > gf->bufsiz - 8)
		return -EINVAL;

	buf[0] = 0xF0;
	buf[1] = addr >> 8;
	buf[2] = addr & 0xFF;
	xfer[0].tx_buf = buf;
	xfer[0].len = 3;
	xfer[0].delay_usecs = 5;
	spi_message_init(&msg);
	spi_message_add_tail(&xfer[0], &msg);
	ret = spi_sync(gf->spi, &msg);
	if (ret)
		return ret;

	buf[4] = 0xF1;
	memset(buf + 5, 0, len);
	xfer[1].tx_buf = buf + 4;
	xfer[1].rx_buf = buf + 4;
	xfer[1].len = len + 1;
	xfer[1].delay_usecs = 5;
	spi_message_init(&msg);
	spi_message_add_tail(&xfer[1], &msg);
	ret = spi_sync(gf->spi, &msg);
	if (ret)
		return ret;

	memcpy(rx, buf + 5, len);
	return 0;
}

int gf_spi_write_bytes(struct gf_device *gf, u16 addr, u32 len, const u8 *tx)
{
	struct spi_transfer xfer = {};
	struct spi_message msg;
	u8 *buf = gf->spi_buffer;

	if (len == 0 || len > gf->bufsiz - 8)
		return -EINVAL;

	buf[0] = 0xF0;
	buf[1] = addr >> 8;
	buf[2] = addr & 0xFF;
	memcpy(buf + 3, tx, len);
	xfer.tx_buf = buf;
	xfer.len = len + 3;
	xfer.delay_usecs = 5;
	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	return spi_sync(gf->spi, &msg);
}

int gf_spi_transfer_raw(struct gf_device *gf, const u8 *tx, u8 *rx, u32 len)
{
	struct spi_transfer xfer = {};
	struct spi_message msg;

	xfer.tx_buf = tx;
	xfer.rx_buf = rx;
	xfer.len = len;
	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	return spi_sync(gf->spi, &msg);
}
