/* ST Microelectronics IIS2MDC 3-axis magnetometer sensor
 *
 * Copyright (c) 2020 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Datasheet:
 * https://www.st.com/resource/en/datasheet/iis2mdc.pdf
 */

#include <string.h>
#include "iis2mdc.h"
#include <logging/log.h>

#ifdef DT_ST_IIS2MDC_BUS_SPI

#define IIS2MDC_SPI_READ		(1 << 7)

#define LOG_LEVEL CONFIG_SENSOR_LOG_LEVEL
LOG_MODULE_DECLARE(IIS2MDC);

static int iis2mdc_spi_read(struct device *dev, u8_t reg_addr,
			    u8_t *value, u8_t len)
{
	struct iis2mdc_data *data = dev->driver_data;
	const struct iis2mdc_config *cfg = dev->config->config_info;
	const struct spi_config *spi_cfg = &cfg->spi_conf;
	u8_t buffer_tx[2] = { reg_addr | IIS2MDC_SPI_READ, 0 };
	const struct spi_buf tx_buf = {
			.buf = buffer_tx,
			.len = 2,
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1
	};
	const struct spi_buf rx_buf[2] = {
		{
			.buf = NULL,
			.len = 1,
		},
		{
			.buf = value,
			.len = len,
		}
	};
	const struct spi_buf_set rx = {
		.buffers = rx_buf,
		.count = 2
	};


	if (len > 64) {
		return -EIO;
	}

	if (spi_transceive(data->bus, spi_cfg, &tx, &rx)) {
		return -EIO;
	}

	return 0;
}

static int iis2mdc_spi_write(struct device *dev, u8_t reg_addr,
			     u8_t *value, u8_t len)
{
	struct iis2mdc_data *data = dev->driver_data;
	const struct iis2mdc_config *cfg = dev->config->config_info;
	const struct spi_config *spi_cfg = &cfg->spi_conf;
	u8_t buffer_tx[1] = { reg_addr & ~IIS2MDC_SPI_READ };
	const struct spi_buf tx_buf[2] = {
		{
			.buf = buffer_tx,
			.len = 1,
		},
		{
			.buf = value,
			.len = len,
		}
	};
	const struct spi_buf_set tx = {
		.buffers = tx_buf,
		.count = 2
	};


	if (len > 64) {
		return -EIO;
	}

	if (spi_write(data->bus, spi_cfg, &tx)) {
		return -EIO;
	}

	return 0;
}

int iis2mdc_spi_init(struct device *dev)
{
	struct iis2mdc_data *data = dev->driver_data;

	data->ctx_spi.read_reg = (stmdev_read_ptr) iis2mdc_spi_read;
	data->ctx_spi.write_reg = (stmdev_write_ptr) iis2mdc_spi_write;

	data->ctx = &data->ctx_spi;
	data->ctx->handle = dev;

#if defined(DT_INST_0_ST_IIS2MDC_CS_GPIOS_CONTROLLER)
	const struct iis2mdc_config *cfg = dev->config->config_info;

	/* handle SPI CS thru GPIO if it is the case */
	data->cs_ctrl.gpio_dev = device_get_binding(cfg->gpio_cs_port);
	if (!data->cs_ctrl.gpio_dev) {
		LOG_ERR("Unable to get GPIO SPI CS device");
		return -ENODEV;
	}

	data->cs_ctrl.gpio_pin = cfg->cs_gpio;
	data->cs_ctrl.delay = 0;

	LOG_DBG("SPI GPIO CS configured on %s:%u",
		cfg->gpio_cs_port, cfg->cs_gpio);
#endif

#if CONFIG_IIS2MDC_SPI_FULL_DUPLEX
	/* Set SPI 4wires */
	if (iis2mdc_spi_mode_set(data->ctx, IIS2MDC_SPI_4_WIRE) < 0) {
		return -EIO;
	}
#endif

	return 0;
}
#endif /* DT_ST_IIS2MDC_BUS_SPI */
