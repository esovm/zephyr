/*
 * Copyright (c) 2018-2019 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME eth_e1000
#define LOG_LEVEL CONFIG_ETHERNET_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <sys/types.h>
#include <zephyr.h>
#include <net/ethernet.h>
#include <ethernet/eth_stats.h>
#include <drivers/pcie/pcie.h>
#include "eth_e1000_priv.h"

#if defined(CONFIG_ETH_E1000_VERBOSE_DEBUG)
#define hexdump(_buf, _len, fmt, args...)				\
({									\
	const size_t STR_SIZE = 80;					\
	char _str[STR_SIZE];						\
									\
	snprintk(_str, STR_SIZE, "%s: " fmt, __func__, ## args);	\
									\
	LOG_HEXDUMP_DBG(_buf, _len, log_strdup(_str));			\
})
#else
#define hexdump(args...)
#endif

static const char *e1000_reg_to_string(enum e1000_reg_t r)
{
#define _(_x)	case _x: return #_x
	switch (r) {
	_(CTRL);
	_(ICR);
	_(ICS);
	_(IMS);
	_(RCTL);
	_(TCTL);
	_(RDBAL);
	_(RDBAH);
	_(RDLEN);
	_(RDH);
	_(RDT);
	_(TDBAL);
	_(TDBAH);
	_(TDLEN);
	_(TDH);
	_(TDT);
	_(RAL);
	_(RAH);
	}
#undef _
	LOG_ERR("Unsupported register: 0x%x", r);
	k_oops();
	return NULL;
}

static enum ethernet_hw_caps e1000_caps(struct device *dev)
{
	return  ETHERNET_LINK_10BASE_T | ETHERNET_LINK_100BASE_T | \
		ETHERNET_LINK_1000BASE_T;
}

static int e1000_tx(struct e1000_dev *dev, void *buf, size_t len)
{
	hexdump(buf, len, "%zu byte(s)", len);

	dev->tx.addr = POINTER_TO_INT(buf);
	dev->tx.len = len;
	dev->tx.cmd = TDESC_EOP | TDESC_RS;

	iow32(dev, TDT, 1);

	while (!(dev->tx.sta)) {
		k_yield();
	}

	LOG_DBG("tx.sta: 0x%02hx", dev->tx.sta);

	return (dev->tx.sta & TDESC_STA_DD) ? 0 : -EIO;
}

static int e1000_send(struct device *device, struct net_pkt *pkt)
{
	struct e1000_dev *dev = device->driver_data;
	size_t len = net_pkt_get_len(pkt);

	if (net_pkt_read(pkt, dev->txb, len)) {
		return -EIO;
	}

	return e1000_tx(dev, dev->txb, len);
}

static struct net_pkt *e1000_rx(struct e1000_dev *dev)
{
	struct net_pkt *pkt = NULL;
	void *buf;
	ssize_t len;

	LOG_DBG("rx.sta: 0x%02hx", dev->rx.sta);

	if (!(dev->rx.sta & RDESC_STA_DD)) {
		LOG_ERR("RX descriptor not ready");
		goto out;
	}

	buf = INT_TO_POINTER((u32_t)dev->rx.addr);
	len = dev->rx.len - 4;

	if (len <= 0) {
		LOG_ERR("Invalid RX descriptor length: %hu", dev->rx.len);
		goto out;
	}

	hexdump(buf, len, "%zd byte(s)", len);

	pkt = net_pkt_rx_alloc_with_buffer(dev->iface, len, AF_UNSPEC, 0,
					   K_NO_WAIT);
	if (!pkt) {
		LOG_ERR("Out of buffers");
		goto out;
	}

	if (net_pkt_write(pkt, buf, len)) {
		LOG_ERR("Out of memory for received frame");
		net_pkt_unref(pkt);
		pkt = NULL;
	}

out:
	return pkt;
}

static void e1000_isr(struct device *device)
{
	struct e1000_dev *dev = device->driver_data;
	u32_t icr = ior32(dev, ICR); /* Cleared upon read */

	icr &= ~(ICR_TXDW | ICR_TXQE);

	if (icr & ICR_RXO) {
		struct net_pkt *pkt = e1000_rx(dev);

		icr &= ~ICR_RXO;

		if (pkt) {
			net_recv_data(dev->iface, pkt);
		} else {
			eth_stats_update_errors_rx(dev->iface);
		}
	}

	if (icr) {
		LOG_ERR("Unhandled interrupt, ICR: 0x%x", icr);
	}
}

#define PCI_VENDOR_ID_INTEL	0x8086
#define PCI_DEVICE_ID_I82540EM	0x100e

int e1000_probe(struct device *device)
{
	const pcie_bdf_t bdf = PCIE_BDF(0, 3, 0);
	struct e1000_dev *dev = device->driver_data;
	int retval = -ENODEV;

	if (pcie_probe(bdf, PCIE_ID(PCI_VENDOR_ID_INTEL,
			    PCI_DEVICE_ID_I82540EM))) {
		dev->address = pcie_get_mbar(bdf, 0);
		pcie_set_cmd(bdf, PCIE_CONF_CMDSTAT_MEM |
				  PCIE_CONF_CMDSTAT_MASTER, true);
		retval = 0;
	}

	return retval;
}

static struct device DEVICE_NAME_GET(eth_e1000);

static void e1000_init(struct net_if *iface)
{
	struct e1000_dev *dev = net_if_get_device(iface)->driver_data;
	u32_t ral, rah;

	dev->iface = iface;

	/* Setup TX descriptor */

	iow32(dev, TDBAL, (u32_t) &dev->tx);
	iow32(dev, TDBAH, 0);
	iow32(dev, TDLEN, 1*16);

	iow32(dev, TDH, 0);
	iow32(dev, TDT, 0);

	iow32(dev, TCTL, TCTL_EN);

	/* Setup RX descriptor */

	dev->rx.addr = POINTER_TO_INT(dev->rxb);
	dev->rx.len = sizeof(dev->rxb);

	iow32(dev, RDBAL, (u32_t) &dev->rx);
	iow32(dev, RDBAH, 0);
	iow32(dev, RDLEN, 1*16);

	iow32(dev, RDH, 0);
	iow32(dev, RDT, 1);

	iow32(dev, IMS, IMS_RXO);

	ral = ior32(dev, RAL);
	rah = ior32(dev, RAH);

	memcpy(dev->mac, &ral, 4);
	memcpy(dev->mac + 4, &rah, 2);

	ethernet_init(iface);

	net_if_set_link_addr(iface, dev->mac, sizeof(dev->mac),
				NET_LINK_ETHERNET);

	IRQ_CONNECT(DT_INST_0_INTEL_E1000_IRQ_0,
			DT_INST_0_INTEL_E1000_IRQ_0_PRIORITY,
			e1000_isr, DEVICE_GET(eth_e1000),
			DT_INST_0_INTEL_E1000_IRQ_0_SENSE);

	irq_enable(DT_INST_0_INTEL_E1000_IRQ_0);

	iow32(dev, CTRL, CTRL_SLU); /* Set link up */

	iow32(dev, RCTL, RCTL_EN | RCTL_MPE);

	LOG_DBG("done");
}

static struct e1000_dev e1000_dev;

static const struct ethernet_api e1000_api = {
	.iface_api.init		= e1000_init,
	.get_capabilities	= e1000_caps,
	.send			= e1000_send,
};

NET_DEVICE_INIT(eth_e1000,
		"ETH_0",
		e1000_probe,
		device_pm_control_nop,
		&e1000_dev,
		NULL,
		CONFIG_ETH_INIT_PRIORITY,
		&e1000_api,
		ETHERNET_L2,
		NET_L2_GET_CTX_TYPE(ETHERNET_L2),
		NET_ETH_MTU);
