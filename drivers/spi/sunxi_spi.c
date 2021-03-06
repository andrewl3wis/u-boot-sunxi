/*
 * (C) Copyright 2017 Whitebox Systems / Northend Systems B.V.
 * Stephan van Schaik <stephan@whiteboxsystems.nl>
 * Merlijn Wajer <merlijn@whiteboxsystems.nl>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <spi.h>

#include <asm/bitops.h>
#include <asm/gpio.h>
#include <asm/io.h>

#include <asm/arch/clock.h>
#include <asm/arch/spi.h>

#define SUNXI_SPI_MAX_RATE (24 * 1000 * 1000)
#define SUNXI_SPI_MIN_RATE (3 * 1000)

struct sunxi_spi_platdata {
	struct sunxi_spi_regs *regs;
	unsigned int activate_delay_us;
	unsigned int deactivate_delay_us;
	uint32_t freq;
};

struct sunxi_spi_priv {
	struct sunxi_spi_regs *regs;
	unsigned int max_freq;
	unsigned int last_transaction_us;
};

DECLARE_GLOBAL_DATA_PTR;

static void sunxi_spi_setup_pinmux(unsigned int pin_function)
{
	unsigned int pin;

	for (pin = SUNXI_GPC(0); pin <= SUNXI_GPC(2); pin++)
		sunxi_gpio_set_cfgpin(pin, pin_function);

	if (IS_ENABLED(CONFIG_MACH_SUN4I) || IS_ENABLED(CONFIG_MACH_SUN7I)) {
		sunxi_gpio_set_cfgpin(SUNXI_GPC(23), pin_function);
	} else {
		sunxi_gpio_set_cfgpin(SUNXI_GPC(3), pin_function);
	}
}

static void sunxi_spi_enable_clock(struct udevice *bus)
{
	struct sunxi_ccm_reg * const ccm =
		(struct sunxi_ccm_reg * const)SUNXI_CCM_BASE;

	if (IS_ENABLED(CONFIG_SUNXI_GEN_SUN6I))
		setbits_le32(&ccm->ahb_reset0_cfg,
			     (1 << AHB_RESET_OFFSET_SPI0));

	setbits_le32(&ccm->ahb_gate0, (1 << AHB_GATE_OFFSET_SPI0));
	writel((1 << 31), &ccm->spi0_clk_cfg);
}

static void sunxi_spi_disable_clock(void)
{
	struct sunxi_ccm_reg * const ccm =
		(struct sunxi_ccm_reg * const)SUNXI_CCM_BASE;

	writel(0, &ccm->spi0_clk_cfg);
	clrbits_le32(&ccm->ahb_gate0, (1 << AHB_GATE_OFFSET_SPI0));
}

static void sunxi_spi_cs_activate(struct udevice *dev, unsigned int cs)
{
	struct udevice *bus = dev->parent;
	struct sunxi_spi_platdata *plat = dev_get_platdata(bus);
	struct sunxi_spi_priv *priv = dev_get_priv(bus);
	uint32_t reg;

	/* If it is too soon to perform another transaction, wait. */
	if (plat->deactivate_delay_us && priv->last_transaction_us) {
		unsigned int delay_us;

		delay_us = timer_get_us() - priv->last_transaction_us;

		if (delay_us < plat->deactivate_delay_us)
			udelay(plat->deactivate_delay_us - delay_us);
	}

	debug("%s: activate cs: %u, bus: '%s'\n", __func__, cs, bus->name);

	reg = readl(&priv->regs->xfer_ctl);
	reg &= ~(SUNXI_SPI_CTL_CS_MASK | SUNXI_SPI_CTL_CS_LEVEL);
	reg |= SUNXI_SPI_CTL_CS(cs);
	writel(reg, &priv->regs->xfer_ctl);

	if (plat->activate_delay_us)
		udelay(plat->activate_delay_us);
}

static void sunxi_spi_cs_deactivate(struct udevice *dev, unsigned int cs)
{
	struct udevice *bus = dev->parent;
	struct sunxi_spi_platdata *plat = dev_get_platdata(bus);
	struct sunxi_spi_priv *priv = dev_get_priv(bus);
	uint32_t reg;

	debug("%s: deactivate cs: %u, bus: '%s'\n", __func__, cs, bus->name);
	
	reg = readl(&priv->regs->xfer_ctl);
	reg &= ~SUNXI_SPI_CTL_CS_MASK;
	reg |= SUNXI_SPI_CTL_CS_LEVEL;
	writel(reg, &priv->regs->xfer_ctl);

	/* 
	 * Remember the time of this transaction so that we can honour the bus
	 * delay.
	 */
	if (plat->deactivate_delay_us)
		priv->last_transaction_us = timer_get_us();
}

static int sunxi_spi_ofdata_to_platdata(struct udevice *bus)
{
	struct sunxi_spi_platdata *plat = dev_get_platdata(bus);
	const void *blob = gd->fdt_blob;
	int node = dev_of_offset(bus);

	plat->regs = (struct sunxi_spi_regs *)devfdt_get_addr(bus);
	plat->activate_delay_us = fdtdec_get_int(
		blob, node, "spi-activate_delay", 0);
	plat->deactivate_delay_us = fdtdec_get_int(
		blob, node, "spi-deactivate-delay", 0);

	debug("%s: regs=%p, activate-delay=%u, deactivate-delay=%u\n",
		__func__, plat->regs, plat->activate_delay_us,
		plat->deactivate_delay_us);

	return 0;
}

static int sunxi_spi_probe(struct udevice *bus)
{
	struct sunxi_spi_platdata *plat = dev_get_platdata(bus);
	struct sunxi_spi_priv *priv = dev_get_priv(bus);

	debug("%s: probe\n", __func__);

	priv->regs = plat->regs;
	priv->last_transaction_us = timer_get_us();

	return 0;
}

static int sunxi_spi_claim_bus(struct udevice *dev)
{
	struct udevice *bus = dev->parent;
	struct sunxi_spi_priv *priv = dev_get_priv(bus);
	unsigned int pin_function = SUNXI_GPC_SPI0;

	debug("%s: claiming bus\n", __func__);

	if (IS_ENABLED(CONFIG_MACH_SUN50I))
		pin_function = SUN50I_GPC_SPI0;

	sunxi_spi_setup_pinmux(pin_function);
	sunxi_spi_enable_clock(bus);
	setbits_le32(&priv->regs->glb_ctl, SUNXI_SPI_CTL_MASTER |
		SUNXI_SPI_CTL_ENABLE | SUNXI_SPI_CTL_TP | SUNXI_SPI_CTL_SRST);
	setbits_le32(&priv->regs->xfer_ctl, SUNXI_SPI_CTL_CS_MANUAL |
		SUNXI_SPI_CTL_CS_LEVEL);
	setbits_le32(&priv->regs->fifo_ctl, SUNXI_SPI_CTL_RF_RST |
		SUNXI_SPI_CTL_TF_RST);

	if (IS_ENABLED(CONFIG_GEN_SUN6I))
		while (readl(&priv->regs->glb_ctl) & SUNXI_SPI_CTL_SRST)
			;

	return 0;
}

static int sunxi_spi_release_bus(struct udevice *dev)
{
	struct udevice *bus = dev->parent;
	struct sunxi_spi_priv *priv = dev_get_priv(bus);

	debug("%s: releasing bus\n", __func__);

	clrbits_le32(&priv->regs->glb_ctl, SUNXI_SPI_CTL_ENABLE);
	sunxi_spi_disable_clock();

	return 0;
}

static int sunxi_spi_xfer(struct udevice *dev, unsigned int bitlen,
	const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct sunxi_spi_priv *priv = dev_get_priv(bus);
	struct dm_spi_slave_platdata *slave_plat = dev_get_parent_platdata(dev);
	const char *tx_buf = dout;
	char *rx_buf = din;
	size_t len = bitlen / 8;
	size_t i, nbytes;
	char byte;

	if (bitlen % 8) {
		debug("%s: non byte-aligned SPI transfer.\n", __func__);
		return -1;
	}

	if (flags & SPI_XFER_BEGIN)
		sunxi_spi_cs_activate(dev, slave_plat->cs);

	while (len) {
		nbytes = min(len, (size_t)64 - 1);

		writel(SUNXI_SPI_BURST_CNT(nbytes), &priv->regs->burst_cnt);

		if (!tx_buf) {
			if (IS_ENABLED(CONFIG_SUNXI_GEN_SUN6I))
				writel(SUNXI_SPI_BURST_CNT(0),
					&priv->regs->burst_ctl);
			writel(0, &priv->regs->xmit_cnt);
		} else {
			if (IS_ENABLED(CONFIG_SUNXI_GEN_SUN6I))
				writel(SUNXI_SPI_BURST_CNT(nbytes),
					&priv->regs->burst_ctl);
			writel(SUNXI_SPI_XMIT_CNT(nbytes),
				&priv->regs->xmit_cnt);

			for (i = 0; i < nbytes; ++i) {
				byte = tx_buf ? *tx_buf++ : 0;
				writeb(byte, &priv->regs->tx_data);
			}
		}
		
		setbits_le32(&priv->regs->xfer_ctl, SUNXI_SPI_CTL_XCH);

		while (((readl(&priv->regs->fifo_sta) &
			SUNXI_SPI_FIFO_RF_CNT_MASK) >>
			SUNXI_SPI_FIFO_RF_CNT_BITS) < nbytes)
				;

		for (i = 0; i < nbytes; ++i) {
			byte = readb(&priv->regs->rx_data);

			if (rx_buf)
				*rx_buf++ = byte;
		}

		len -= nbytes;
	}

	if (flags & SPI_XFER_END)
		sunxi_spi_cs_deactivate(dev, slave_plat->cs);

	return 0;
}

static int sunxi_spi_set_speed(struct udevice *bus, uint speed)
{
	struct sunxi_spi_priv *priv = dev_get_priv(bus);
	unsigned int div;
	uint32_t reg;

	speed = min(speed, (unsigned int)SUNXI_SPI_MAX_RATE);
	speed = max((unsigned int)SUNXI_SPI_MIN_RATE, speed);

	div = SUNXI_SPI_MAX_RATE / (2 * speed);

	if (div <= (SUNXI_SPI_CLK_CTL_CDR2_MASK + 1)) {
		if (div > 0)
			div--;

		reg = SUNXI_SPI_CLK_CTL_CDR2(div) | SUNXI_SPI_CLK_CTL_DRS;
	} else {
		div = __ilog2(SUNXI_SPI_MAX_RATE) - __ilog2(speed);
		reg = SUNXI_SPI_CLK_CTL_CDR1(div);
	}

	writel(reg, &priv->regs->clk_ctl);

	debug("%s: speed=%u\n", __func__, speed);

	return 0;
}

static int sunxi_spi_set_mode(struct udevice *bus, uint mode)
{
	struct sunxi_spi_priv *priv = dev_get_priv(bus);
	uint32_t reg;

	reg = readl(&priv->regs->xfer_ctl);
	reg &= ~(SUNXI_SPI_CTL_CPOL | SUNXI_SPI_CTL_CPHA |
		SUNXI_SPI_CTL_CS_ACTIVE_LOW);

	if (mode & SPI_CPOL)
		reg |= SUNXI_SPI_CTL_CPOL;

	if (mode & SPI_CPHA)
		reg |= SUNXI_SPI_CTL_CPHA;

	if (!(mode & SPI_CS_HIGH))
		reg |= SUNXI_SPI_CTL_CS_ACTIVE_LOW;

	writel(reg, &priv->regs->xfer_ctl);

	debug("%s: mode=%d\n", __func__, mode);

	return 0;
}

static const struct dm_spi_ops sunxi_spi_ops = {
	.claim_bus	= sunxi_spi_claim_bus,
	.release_bus	= sunxi_spi_release_bus,
	.xfer		= sunxi_spi_xfer,
	.set_speed	= sunxi_spi_set_speed,
	.set_mode	= sunxi_spi_set_mode,
};

static const struct udevice_id sunxi_spi_ids[] = {
	{ .compatible = "allwinner,sun4i-a10-spi" },
	{ .compatible = "allwinner,sun6i-a31-spi" },
	{ }
};

U_BOOT_DRIVER(sunxi_spi) = {
	.name	= "sunxi_spi",
	.id	= UCLASS_SPI,
	.of_match = sunxi_spi_ids,
	.ops	= &sunxi_spi_ops,
	.ofdata_to_platdata = sunxi_spi_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct sunxi_spi_platdata),
	.priv_auto_alloc_size = sizeof(struct sunxi_spi_priv),
	.probe	= sunxi_spi_probe,
};
