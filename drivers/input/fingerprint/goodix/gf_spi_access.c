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
 * per the header above. Reworked: only the raw transfer and setup helpers
 * are kept (the register read/write helpers served an ioctl that upstream
 * compiles out).
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
