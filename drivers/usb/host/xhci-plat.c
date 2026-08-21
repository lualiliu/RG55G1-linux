// SPDX-License-Identifier: GPL-2.0
/*
 * xhci-plat.c - xHCI host controller driver platform Bus Glue.
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - https://www.ti.com
 * Author: Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * A lot of code borrowed from the Linux xHCI driver.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/usb/phy.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/usb/of.h>
#include <linux/reset.h>
#include <linux/usb/xhci-sideband.h>
#include <linux/workqueue.h>

#include "xhci.h"
#include "xhci-plat.h"
#include "xhci-mvebu.h"

static struct hc_driver __read_mostly xhci_plat_hc_driver;

/* RG55G1: poll root-hub PORTSC when GIC events may be silent. */
struct rg55g1_xhci_poll {
	struct delayed_work work;
	struct xhci_hcd *xhci;
	u32 last_u2;
	u32 last_u3;
	u32 ticks;
	bool did_reset;
	/* After first CCS, never PORT_RESET / link-poke from poll again. */
	bool ever_connected;
	/* After CCS drop (PC/charger unplug), stay hands-off for hub_wq. */
	unsigned long quiet_until;
};

/* PORTSC RW1C change bits — writing 1 clears; must mask when setting PP. */
#define RG55_PORT_RWC	PORT_CHANGE_MASK

static void rg55g1_xhci_poll_fn(struct work_struct *work)
{
	struct rg55g1_xhci_poll *p =
		container_of(to_delayed_work(work), struct rg55g1_xhci_poll, work);
	struct xhci_hcd *xhci = p->xhci;
	struct usb_hcd *hcd;
	u32 u2 = 0, u3 = 0;
	char msg[48];
	bool was_conn, ccs, quiet;
	extern void rg55g1_status(const char *msg, u32 color);
	extern bool rg55g1_usb_loose_supplies;

	if (!rg55g1_usb_loose_supplies || !xhci)
		return;

	hcd = xhci->main_hcd;
	p->ticks++;

	was_conn = !!(p->last_u2 & PORT_CONNECT);
	if (hcd && xhci->usb2_rhub.ports && xhci->usb2_rhub.num_ports)
		u2 = xhci_portsc_readl(xhci->usb2_rhub.ports[0]);
	ccs = !!(u2 & PORT_CONNECT);

	if (ccs)
		p->ever_connected = true;

	/*
	 * PC/charger → unplug → keyboard: hub_wq is tearing down the previous
	 * "device" while we used to keep draining + poll_rh + rewriting
	 * HS_PHY_CTRL/PORTSC every 200ms — that hard-locks the controller.
	 * After CCS edges, deliver one kick then stay quiet so disconnect /
	 * first enumerate can finish without the poll worker fighting them.
	 */
	{
		bool edge_kick = false;
		bool allow;

		if (was_conn && !ccs) {
			p->quiet_until = jiffies + msecs_to_jiffies(1500);
			edge_kick = true;
		} else if (ccs && !was_conn) {
			p->quiet_until = jiffies + msecs_to_jiffies(500);
			edge_kick = true;
		}

		quiet = time_before(jiffies, p->quiet_until);
		allow = !quiet || edge_kick;

		/*
		 * Drain only when idle (trylock): never race cmd-wait drain
		 * during ENABLE_SLOT after keyboard plug.
		 */
		if (allow && hcd)
			(void)xhci_rg55_drain_irq_if_idle(hcd);

		/*
		 * PHY keepalive — do not rewrite sessvld/VBUS every tick
		 * (especially across PC↔keyboard). Only ensure UTMI clock
		 * and !SUSPEND when not in quiet.
		 */
	{
		static void __iomem *dwc_regs;
		static void __iomem *qscratch;
		static void __iomem *hsphy;
		static void __iomem *eud;
		u32 cfg = 0, hs = 0, utmi = 0, ecsr = 0;

		if (!dwc_regs)
			dwc_regs = ioremap(0x0a600000, 0xd800);
		if (!qscratch)
			qscratch = ioremap(0x0a6f8800, 0x400);
		if (!hsphy)
			hsphy = ioremap(0x088e3000, 0x200);
		if (!eud)
			eud = ioremap(0x088e0000, 0x2000);

		if (allow && dwc_regs) {
			cfg = readl(dwc_regs + 0xc200); /* GUSB2PHYCFG(0) */
			if (cfg & (BIT(6) | BIT(8))) {
				cfg &= ~(BIT(6) | BIT(8));
				writel(cfg, dwc_regs + 0xc200);
				cfg = readl(dwc_regs + 0xc200);
			}
		} else if (dwc_regs) {
			cfg = readl(dwc_regs + 0xc200);
		}

		if (qscratch) {
			hs = readl(qscratch + 0x10); /* HS_PHY_CTRL */
			if (allow) {
				u32 want = hs | BIT(21); /* UTMI_CLK_EN */

				want &= ~BIT(23);	 /* !USB2_SUSPEND */
				/* Leave UTMI_OTG_VBUS_VALID / SW_SESSVLD_SEL */
				if (want != hs) {
					writel(want, qscratch + 0x10);
					hs = readl(qscratch + 0x10);
				}
			}
		}

		if (allow && hsphy) {
			u32 c0, c2;
			static void __iomem *gcc;
			static bool phy_kicked;

			if (!gcc)
				gcc = ioremap(0x00100000, 0xa0000);
			if (gcc && !phy_kicked) {
				writel(1, gcc + 0x9c00c);
				writel(1, gcc + 0x9c010);
				writel(0, gcc + 0x22000); /* QUSB2PHY BCR deassert */
				phy_kicked = true;
			}

			utmi = readl(hsphy + 0x3c);
			c0 = readl(hsphy + 0x54);
			if (!(utmi & BIT(0)) || (c0 & BIT(2))) {
				writel(c0 & ~BIT(2), hsphy + 0x54);
				c2 = readl(hsphy + 0x64);
				writel(c2 | BIT(2) | BIT(3), hsphy + 0x64);
				writel(BIT(0), hsphy + 0x3c); /* SLEEPM */
				utmi = readl(hsphy + 0x3c);
				c0 = readl(hsphy + 0x54);
			}
			utmi = ((c0 & 0xffff) << 16) | (utmi & 0xffff);
		} else if (hsphy) {
			utmi = readl(hsphy + 0x3c);
		}

		if (eud)
			ecsr = readl(eud + 0x1014); /* CSR_EUD_EN */

		if (hcd && xhci->usb2_rhub.ports && xhci->usb2_rhub.num_ports) {
			struct xhci_port *port = xhci->usb2_rhub.ports[0];
			u32 pls;
			u32 change;

			/* Re-read after possible drain */
			u2 = xhci_portsc_readl(port);
			pls = u2 & PORT_PLS_MASK;
			ccs = !!(u2 & PORT_CONNECT);
			if (ccs)
				p->ever_connected = true;

			if (allow && !(u2 & PORT_POWER)) {
				u32 tmp = xhci_port_state_to_neutral(u2) |
					  PORT_POWER;

				xhci_portsc_writel(port, tmp);
				u2 = xhci_portsc_readl(port);
				pls = u2 & PORT_PLS_MASK;
			}

			/*
			 * Initial bring-up only. Never PORT_RESET after a PC
			 * or keyboard has already connected once.
			 */
			if (allow && !p->ever_connected && !ccs) {
				if ((u2 & PORT_POWER) &&
				    (pls == XDEV_DISABLED ||
				     pls == XDEV_INACTIVE ||
				     pls == XDEV_COMP_MODE)) {
					u32 tmp = xhci_port_state_to_neutral(u2);

					tmp &= ~PORT_PLS_MASK;
					tmp |= PORT_POWER | PORT_LINK_STROBE |
					       XDEV_RXDETECT;
					xhci_portsc_writel(port, tmp);
					u2 = xhci_portsc_readl(port);
				}

				if (!p->did_reset && (u2 & PORT_POWER) &&
				    (p->ticks == 15 ||
				     (p->ticks > 15 && (p->ticks % 100) == 0))) {
					u32 tmp = xhci_port_state_to_neutral(u2) |
						  PORT_POWER | PORT_RESET;

					xhci_portsc_writel(port, tmp);
					p->did_reset = true;
					u2 = xhci_portsc_readl(port);
				}
			}

			/*
			 * Kick hub only on status change — periodic poll_rh
			 * during PC→keyboard races hub_wq and hard-locks.
			 */
			change = (u2 ^ p->last_u2) &
				 (PORT_CONNECT | PORT_CHANGE_MASK);
			if (allow && change) {
				set_bit(HCD_FLAG_POLL_RH, &hcd->flags);
				usb_hcd_poll_rh_status(hcd);
			}
		}

		if (allow && xhci->shared_hcd && xhci->usb3_rhub.ports &&
		    xhci->usb3_rhub.num_ports) {
			u3 = xhci_portsc_readl(xhci->usb3_rhub.ports[0]);
			if ((u3 ^ p->last_u3) &
			    (PORT_CONNECT | PORT_CHANGE_MASK)) {
				set_bit(HCD_FLAG_POLL_RH,
					&xhci->shared_hcd->flags);
				usb_hcd_poll_rh_status(xhci->shared_hcd);
			}
		}

		/* Rotate: U2 portsc / HS_PHY_CTRL+GUSB2 / EUD+UTMI */
		switch (p->ticks % 3) {
		case 0:
			snprintf(msg, sizeof(msg), "%sU2:c%dp%de%dL%x",
				 quiet && !edge_kick ? "Q" : "",
				 !!(u2 & PORT_CONNECT),
				 !!(u2 & PORT_POWER),
				 !!(u2 & PORT_PE),
				 (u2 & PORT_PLS_MASK) >> 5);
			break;
		case 1:
			snprintf(msg, sizeof(msg), "H%08x G%08x", hs, cfg);
			break;
		default:
			snprintf(msg, sizeof(msg), "E%08x P%08x", ecsr, utmi);
			break;
		}
		rg55g1_status(msg, (u2 & PORT_CONNECT) ? 0x0000ff00 : 0x00ffff00);
		p->last_u2 = u2;
		p->last_u3 = u3;
	}
	}

	schedule_delayed_work(&p->work, msecs_to_jiffies(200));
}

static int xhci_plat_setup(struct usb_hcd *hcd);
static int xhci_plat_start(struct usb_hcd *hcd);

static const struct xhci_driver_overrides xhci_plat_overrides __initconst = {
	.extra_priv_size = sizeof(struct xhci_plat_priv),
	.reset = xhci_plat_setup,
	.start = xhci_plat_start,
};

static void xhci_priv_plat_start(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (priv->plat_start)
		priv->plat_start(hcd);
}

static int xhci_priv_init_quirk(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->init_quirk)
		return 0;

	return priv->init_quirk(hcd);
}

static int xhci_priv_suspend_quirk(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->suspend_quirk)
		return 0;

	return priv->suspend_quirk(hcd);
}

static int xhci_priv_resume_quirk(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->resume_quirk)
		return 0;

	return priv->resume_quirk(hcd);
}

static int xhci_priv_post_resume_quirk(struct usb_hcd *hcd)
{
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (!priv->post_resume_quirk)
		return 0;

	return priv->post_resume_quirk(hcd);
}

static void xhci_plat_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	struct xhci_plat_priv *priv = xhci_to_priv(xhci);

	xhci->quirks |= priv->quirks;
}

/* called during probe() after chip reset completes */
static int xhci_plat_setup(struct usb_hcd *hcd)
{
	int ret;


	ret = xhci_priv_init_quirk(hcd);
	if (ret)
		return ret;

	return xhci_gen_setup(hcd, xhci_plat_quirks);
}

static int xhci_plat_start(struct usb_hcd *hcd)
{
	xhci_priv_plat_start(hcd);
	return xhci_run(hcd);
}

#ifdef CONFIG_OF
static const struct xhci_plat_priv xhci_plat_marvell_armada = {
	.init_quirk = xhci_mvebu_mbus_init_quirk,
};

static const struct xhci_plat_priv xhci_plat_marvell_armada3700 = {
	.quirks = XHCI_RESET_ON_RESUME,
};

static const struct xhci_plat_priv xhci_plat_brcm = {
	.quirks = XHCI_RESET_ON_RESUME | XHCI_SUSPEND_RESUME_CLKS,
};

static const struct of_device_id usb_xhci_of_match[] = {
	{
		.compatible = "generic-xhci",
	}, {
		.compatible = "xhci-platform",
	}, {
		.compatible = "marvell,armada-375-xhci",
		.data = &xhci_plat_marvell_armada,
	}, {
		.compatible = "marvell,armada-380-xhci",
		.data = &xhci_plat_marvell_armada,
	}, {
		.compatible = "marvell,armada3700-xhci",
		.data = &xhci_plat_marvell_armada3700,
	}, {
		.compatible = "brcm,xhci-brcm-v2",
		.data = &xhci_plat_brcm,
	}, {
		.compatible = "brcm,bcm2711-xhci",
		.data = &xhci_plat_brcm,
	}, {
		.compatible = "brcm,bcm7445-xhci",
		.data = &xhci_plat_brcm,
	},
	{},
};
MODULE_DEVICE_TABLE(of, usb_xhci_of_match);
#endif

int xhci_plat_probe(struct platform_device *pdev, struct device *sysdev, const struct xhci_plat_priv *priv_match)
{
	const struct hc_driver	*driver;
	struct device		*tmpdev;
	struct xhci_hcd		*xhci;
	struct resource         *res;
	struct usb_hcd		*hcd, *usb3_hcd;
	int			ret;
	int			irq;
	struct xhci_plat_priv	*priv = NULL;
	const struct of_device_id *of_match;

	if (usb_disabled())
		return -ENODEV;

	driver = &xhci_plat_hc_driver;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	if (!sysdev)
		sysdev = &pdev->dev;

	{
		extern bool rg55g1_usb_loose_supplies;

		/*
		 * RG55G1: CRCR stays CMD_RING_RUNNING with no completions when
		 * cmd/event rings land above 4GB (USB DMA path is effectively
		 * 32-bit). Force 32-bit DMA before any ring allocation.
		 */
		if (rg55g1_usb_loose_supplies)
			ret = dma_set_mask_and_coherent(sysdev, DMA_BIT_MASK(32));
		else
			ret = dma_set_mask_and_coherent(sysdev, DMA_BIT_MASK(64));
		if (ret)
			return ret;
	}

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	hcd = __usb_create_hcd(driver, sysdev, &pdev->dev,
			       dev_name(&pdev->dev), NULL);
	if (!hcd) {
		ret = -ENOMEM;
		goto disable_runtime;
	}

	hcd->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hcd->regs)) {
		ret = PTR_ERR(hcd->regs);
		goto put_hcd;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	xhci = hcd_to_xhci(hcd);

	xhci->allow_single_roothub = 1;

	{
		extern bool rg55g1_usb_loose_supplies;

		if (rg55g1_usb_loose_supplies) {
			xhci->quirks |= XHCI_NO_64BIT_SUPPORT;
			pr_emerg("rg55g1: xhci force 32-bit DMA / NO_64BIT\n");
		}
	}

	/*
	 * Not all platforms have clks so it is not an error if the
	 * clock do not exist.
	 */
	xhci->reg_clk = devm_clk_get_optional(&pdev->dev, "reg");
	if (IS_ERR(xhci->reg_clk)) {
		ret = PTR_ERR(xhci->reg_clk);
		goto put_hcd;
	}

	xhci->clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(xhci->clk)) {
		ret = PTR_ERR(xhci->clk);
		goto put_hcd;
	}

	xhci->reset = devm_reset_control_array_get_optional_shared(&pdev->dev);
	if (IS_ERR(xhci->reset)) {
		ret = PTR_ERR(xhci->reset);
		goto put_hcd;
	}

	ret = reset_control_deassert(xhci->reset);
	if (ret)
		goto put_hcd;

	ret = clk_prepare_enable(xhci->reg_clk);
	if (ret)
		goto err_reset;

	ret = clk_prepare_enable(xhci->clk);
	if (ret)
		goto disable_reg_clk;

	if (priv_match) {
		priv = hcd_to_xhci_priv(hcd);
		/* Just copy data for now */
		*priv = *priv_match;
	}

	device_set_wakeup_capable(&pdev->dev, true);

	xhci->main_hcd = hcd;

	/* imod_interval is the interrupt moderation value in nanoseconds. */
	xhci->imod_interval = 40000;

	/* Iterate over all parent nodes for finding quirks */
	for (tmpdev = &pdev->dev; tmpdev; tmpdev = tmpdev->parent) {

		if (device_property_read_bool(tmpdev, "usb2-lpm-disable"))
			xhci->quirks |= XHCI_HW_LPM_DISABLE;

		if (device_property_read_bool(tmpdev, "usb3-lpm-capable"))
			xhci->quirks |= XHCI_LPM_SUPPORT;

		if (device_property_read_bool(tmpdev, "quirk-broken-port-ped"))
			xhci->quirks |= XHCI_BROKEN_PORT_PED;

		if (device_property_read_bool(tmpdev, "xhci-sg-trb-cache-size-quirk"))
			xhci->quirks |= XHCI_SG_TRB_CACHE_SIZE_QUIRK;

		if (device_property_read_bool(tmpdev, "write-64-hi-lo-quirk"))
			xhci->quirks |= XHCI_WRITE_64_HI_LO;

		if (device_property_read_bool(tmpdev, "xhci-missing-cas-quirk"))
			xhci->quirks |= XHCI_MISSING_CAS;

		if (device_property_read_bool(tmpdev, "xhci-skip-phy-init-quirk"))
			xhci->quirks |= XHCI_SKIP_PHY_INIT;

		device_property_read_u32(tmpdev, "imod-interval-ns",
					 &xhci->imod_interval);
		device_property_read_u16(tmpdev, "num-hc-interrupters",
					 &xhci->max_interrupters);
	}

	/*
	 * Drivers such as dwc3 manages PHYs themself (and rely on driver name
	 * matching for the xhci platform device).
	 */
	of_match = of_match_device(pdev->dev.driver->of_match_table, &pdev->dev);
	if (of_match) {
		hcd->usb_phy = devm_usb_get_phy_by_phandle(sysdev, "usb-phy", 0);
		if (IS_ERR(hcd->usb_phy)) {
			ret = PTR_ERR(hcd->usb_phy);
			if (ret == -EPROBE_DEFER)
				goto disable_clk;
			hcd->usb_phy = NULL;
		} else {
			ret = usb_phy_init(hcd->usb_phy);
			if (ret)
				goto disable_clk;
		}
	}

	hcd->tpl_support = of_usb_host_tpl_support(sysdev->of_node);

	if ((priv && (priv->quirks & XHCI_SKIP_PHY_INIT)) ||
	    (xhci->quirks & XHCI_SKIP_PHY_INIT))
		hcd->skip_phy_initialization = 1;

	if (priv && (priv->quirks & XHCI_SG_TRB_CACHE_SIZE_QUIRK))
		xhci->quirks |= XHCI_SG_TRB_CACHE_SIZE_QUIRK;

	{
		u32 cap, usbsts;
		void __iomem *base = hcd->regs;
		u8 cap_len;

		cap = readl(base);
		cap_len = cap & 0xff;
		usbsts = (cap_len >= 0x20 && cap_len < 0xe0) ?
			 readl(base + cap_len + 0x04) : 0xdeadbeef;
		pr_emerg("rg55g1: xhci pre-add CAP=0x%08x USBSTS=0x%08x\n",
			 cap, usbsts);
	}

	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto disable_usb_phy;

	if (!xhci_has_one_roothub(xhci)) {
		xhci->shared_hcd = __usb_create_hcd(driver, sysdev, &pdev->dev,
						    dev_name(&pdev->dev), hcd);
		if (!xhci->shared_hcd) {
			ret = -ENOMEM;
			goto dealloc_usb2_hcd;
		}

		if (of_match) {
			xhci->shared_hcd->usb_phy = devm_usb_get_phy_by_phandle(sysdev,
										"usb-phy", 1);
			if (IS_ERR(xhci->shared_hcd->usb_phy)) {
				xhci->shared_hcd->usb_phy = NULL;
			} else {
				ret = usb_phy_init(xhci->shared_hcd->usb_phy);
				if (ret)
					dev_err(sysdev, "%s init usb3phy fail (ret=%d)\n",
						__func__, ret);
			}
		}

		xhci->shared_hcd->tpl_support = hcd->tpl_support;
	}

	usb3_hcd = xhci_get_usb3_hcd(xhci);
	if (usb3_hcd && HCC_MAX_PSA(xhci->hcc_params) >= 4 &&
	    !(xhci->quirks & XHCI_BROKEN_STREAMS))
		usb3_hcd->can_do_streams = 1;

	if (xhci->shared_hcd) {
		xhci->shared_hcd->rsrc_start = hcd->rsrc_start;
		xhci->shared_hcd->rsrc_len = hcd->rsrc_len;
		ret = usb_add_hcd(xhci->shared_hcd, irq, IRQF_SHARED);
		if (ret)
			goto put_usb3_hcd;
	}

	device_enable_async_suspend(&pdev->dev);
	pm_runtime_put_noidle(&pdev->dev);

	/*
	 * Prevent runtime pm from being on as default, users should enable
	 * runtime pm using power/control in sysfs.
	 */
	pm_runtime_forbid(&pdev->dev);

	{
		extern void rg55g1_status(const char *msg, u32 color);
		extern bool rg55g1_usb_loose_supplies;

		pr_emerg("rg55g1: xhci_plat_probe OK irq=%d\n", irq);
		rg55g1_status("XHCI-OK", 0x0000ff00);

		if (rg55g1_usb_loose_supplies) {
			struct rg55g1_xhci_poll *poll;

			set_bit(HCD_FLAG_POLL_RH, &hcd->flags);
			if (xhci->shared_hcd)
				set_bit(HCD_FLAG_POLL_RH,
					&xhci->shared_hcd->flags);
			/*
			 * Do NOT poll/enumerate synchronously here — that can
			 * run ENABLE_SLOT on the probe path and stall boot.
			 * Delayed work will kick hub + drain events.
			 */
			poll = devm_kzalloc(&pdev->dev, sizeof(*poll),
					    GFP_KERNEL);
			if (poll) {
				poll->xhci = xhci;
				INIT_DELAYED_WORK(&poll->work,
						  rg55g1_xhci_poll_fn);
				schedule_delayed_work(&poll->work,
						      msecs_to_jiffies(100));
			}
		}
	}

	return 0;


put_usb3_hcd:
	usb_put_hcd(xhci->shared_hcd);

dealloc_usb2_hcd:
	usb_remove_hcd(hcd);

disable_usb_phy:
	usb_phy_shutdown(hcd->usb_phy);

disable_clk:
	clk_disable_unprepare(xhci->clk);

disable_reg_clk:
	clk_disable_unprepare(xhci->reg_clk);

err_reset:
	reset_control_assert(xhci->reset);

put_hcd:
	usb_put_hcd(hcd);

disable_runtime:
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(xhci_plat_probe);

static int xhci_generic_plat_probe(struct platform_device *pdev)
{
	const struct xhci_plat_priv *priv_match;
	struct device *sysdev;
	int ret;

	/*
	 * sysdev must point to a device that is known to the system firmware
	 * or PCI hardware. We handle these three cases here:
	 * 1. xhci_plat comes from firmware
	 * 2. xhci_plat is child of a device from firmware (dwc3-plat)
	 * 3. xhci_plat is grandchild of a pci device (dwc3-pci)
	 */
	for (sysdev = &pdev->dev; sysdev; sysdev = sysdev->parent) {
		if (is_of_node(sysdev->fwnode) ||
			is_acpi_device_node(sysdev->fwnode))
			break;
		else if (dev_is_pci(sysdev))
			break;
	}

	if (!sysdev)
		sysdev = &pdev->dev;

	if (WARN_ON(!sysdev->dma_mask)) {
		extern bool rg55g1_usb_loose_supplies;

		/* Platform did not initialize dma_mask */
		ret = dma_coerce_mask_and_coherent(sysdev,
			rg55g1_usb_loose_supplies ? DMA_BIT_MASK(32)
						  : DMA_BIT_MASK(64));
		if (ret)
			return ret;
	}

	if (pdev->dev.of_node)
		priv_match = of_device_get_match_data(&pdev->dev);
	else
		priv_match = dev_get_platdata(&pdev->dev);

	return xhci_plat_probe(pdev, sysdev, priv_match);
}

void xhci_plat_remove(struct platform_device *dev)
{
	struct usb_hcd	*hcd = platform_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	struct clk *clk = xhci->clk;
	struct clk *reg_clk = xhci->reg_clk;
	struct usb_hcd *shared_hcd = xhci->shared_hcd;

	xhci->xhc_state |= XHCI_STATE_REMOVING;
	pm_runtime_get_sync(&dev->dev);

	if (shared_hcd) {
		usb_remove_hcd(shared_hcd);
		xhci->shared_hcd = NULL;
	}

	usb_phy_shutdown(hcd->usb_phy);

	usb_remove_hcd(hcd);

	if (shared_hcd)
		usb_put_hcd(shared_hcd);

	clk_disable_unprepare(clk);
	clk_disable_unprepare(reg_clk);
	reset_control_assert(xhci->reset);
	usb_put_hcd(hcd);

	pm_runtime_disable(&dev->dev);
	pm_runtime_put_noidle(&dev->dev);
	pm_runtime_set_suspended(&dev->dev);
}
EXPORT_SYMBOL_GPL(xhci_plat_remove);

static int xhci_plat_suspend_common(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	int ret;

	if (pm_runtime_suspended(dev))
		pm_runtime_resume(dev);

	ret = xhci_priv_suspend_quirk(hcd);
	if (ret)
		return ret;
	/*
	 * xhci_suspend() needs `do_wakeup` to know whether host is allowed
	 * to do wakeup during suspend.
	 */
	ret = xhci_suspend(xhci, device_may_wakeup(dev));
	if (ret)
		return ret;

	if (!device_may_wakeup(dev) && (xhci->quirks & XHCI_SUSPEND_RESUME_CLKS)) {
		clk_disable_unprepare(xhci->clk);
		clk_disable_unprepare(xhci->reg_clk);
	}

	return 0;
}

static int xhci_plat_suspend(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (xhci_sideband_check(hcd)) {
		priv->sideband_at_suspend = 1;
		dev_dbg(dev, "sideband instance active, skip suspend.\n");
		return 0;
	}

	return xhci_plat_suspend_common(dev);
}

static int xhci_plat_freeze(struct device *dev)
{
	return xhci_plat_suspend_common(dev);
}

static int xhci_plat_resume_common(struct device *dev, bool power_lost)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	int ret;

	if (!device_may_wakeup(dev) && (xhci->quirks & XHCI_SUSPEND_RESUME_CLKS)) {
		ret = clk_prepare_enable(xhci->clk);
		if (ret)
			return ret;

		ret = clk_prepare_enable(xhci->reg_clk);
		if (ret) {
			clk_disable_unprepare(xhci->clk);
			return ret;
		}
	}

	ret = xhci_priv_resume_quirk(hcd);
	if (ret)
		goto disable_clks;

	ret = xhci_resume(xhci, power_lost || priv->power_lost, false);
	if (ret)
		goto disable_clks;

	ret = xhci_priv_post_resume_quirk(hcd);
	if (ret)
		goto disable_clks;

	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;

disable_clks:
	if (!device_may_wakeup(dev) && (xhci->quirks & XHCI_SUSPEND_RESUME_CLKS)) {
		clk_disable_unprepare(xhci->clk);
		clk_disable_unprepare(xhci->reg_clk);
	}

	return ret;
}

static int xhci_plat_resume(struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata(dev);
	struct xhci_plat_priv *priv = hcd_to_xhci_priv(hcd);

	if (priv->sideband_at_suspend) {
		priv->sideband_at_suspend = 0;
		dev_dbg(dev, "sideband instance active, skip resume.\n");
		return 0;
	}

	return xhci_plat_resume_common(dev, false);
}

static int xhci_plat_thaw(struct device *dev)
{
	return xhci_plat_resume_common(dev, false);
}

static int xhci_plat_restore(struct device *dev)
{
	return xhci_plat_resume_common(dev, true);
}

static int __maybe_unused xhci_plat_runtime_suspend(struct device *dev)
{
	struct usb_hcd  *hcd = dev_get_drvdata(dev);
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	int ret;

	ret = xhci_priv_suspend_quirk(hcd);
	if (ret)
		return ret;

	return xhci_suspend(xhci, true);
}

static int __maybe_unused xhci_plat_runtime_resume(struct device *dev)
{
	struct usb_hcd  *hcd = dev_get_drvdata(dev);
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);

	return xhci_resume(xhci, false, true);
}

const struct dev_pm_ops xhci_plat_pm_ops = {
	.suspend = pm_sleep_ptr(xhci_plat_suspend),
	.resume = pm_sleep_ptr(xhci_plat_resume),
	.freeze = pm_sleep_ptr(xhci_plat_freeze),
	.thaw = pm_sleep_ptr(xhci_plat_thaw),
	.poweroff = pm_sleep_ptr(xhci_plat_freeze),
	.restore = pm_sleep_ptr(xhci_plat_restore),

	SET_RUNTIME_PM_OPS(xhci_plat_runtime_suspend,
			   xhci_plat_runtime_resume,
			   NULL)
};
EXPORT_SYMBOL_GPL(xhci_plat_pm_ops);

#ifdef CONFIG_ACPI
static const struct acpi_device_id usb_xhci_acpi_match[] = {
	/* XHCI-compliant USB Controller */
	{ "PNP0D10", },
	{ "PNP0D15", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, usb_xhci_acpi_match);
#endif

static struct platform_driver usb_generic_xhci_driver = {
	.probe	= xhci_generic_plat_probe,
	.remove = xhci_plat_remove,
	.shutdown = usb_hcd_platform_shutdown,
	.driver	= {
		.name = "xhci-hcd",
		.pm = &xhci_plat_pm_ops,
		.of_match_table = of_match_ptr(usb_xhci_of_match),
		.acpi_match_table = ACPI_PTR(usb_xhci_acpi_match),
	},
};
MODULE_ALIAS("platform:xhci-hcd");

static int __init xhci_plat_init(void)
{
	xhci_init_driver(&xhci_plat_hc_driver, &xhci_plat_overrides);
	return platform_driver_register(&usb_generic_xhci_driver);
}
arch_initcall(xhci_plat_init); /* RG55G1: host controller without full LV6 */

static void __exit xhci_plat_exit(void)
{
	platform_driver_unregister(&usb_generic_xhci_driver);
}
module_exit(xhci_plat_exit);

MODULE_DESCRIPTION("xHCI Platform Host Controller Driver");
MODULE_LICENSE("GPL");
