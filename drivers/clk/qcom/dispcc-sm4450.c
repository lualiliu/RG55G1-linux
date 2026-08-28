// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

extern bool rg55g1_preserve_abl_display;

#include <dt-bindings/clock/qcom,sm4450-dispcc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-pll.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "clk-regmap-divider.h"
#include "common.h"
#include "gdsc.h"
#include "reset.h"

enum {
	DT_BI_TCXO,
	DT_BI_TCXO_AO,
	DT_AHB_CLK,
	DT_SLEEP_CLK,

	DT_DSI0_PHY_PLL_OUT_BYTECLK,
	DT_DSI0_PHY_PLL_OUT_DSICLK,
};

enum {
	P_BI_TCXO,
	P_DISP_CC_PLL0_OUT_MAIN,
	P_DISP_CC_PLL1_OUT_EVEN,
	P_DISP_CC_PLL1_OUT_MAIN,
	P_DSI0_PHY_PLL_OUT_BYTECLK,
	P_DSI0_PHY_PLL_OUT_DSICLK,
	P_SLEEP_CLK,
};

static const struct pll_vco lucid_evo_vco[] = {
	{ 249600000, 2020000000, 0 },
};

/*
 * Stock rpmhclk often has no xo parent → DT bi_tcxo rate 0 →
 * "disp_cc_pll0: Rounded rate 0". Always parent PLLs/RCGs from a local
 * 19.2 MHz fixed clock so MDP set_rate works regardless of DT.
 */
static struct clk_fixed_rate disp_cc_bi_tcxo_fixed = {
	.fixed_rate = 19200000,
	.hw.init = &(struct clk_init_data) {
		.name = "disp_cc_bi_tcxo_fixed",
		.ops = &clk_fixed_rate_ops,
	},
};

static struct clk_hw *disp_cc_sm4450_hws[] = {
	&disp_cc_bi_tcxo_fixed.hw,
};

/* 600.0 MHz Configuration */
static const struct alpha_pll_config disp_cc_pll0_config = {
	.l = 0x1f,
	.alpha = 0x4000,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x32aa299c,
	.user_ctl_val = 0x00000000,
	.user_ctl_hi_val = 0x00000805,
};

static struct clk_alpha_pll disp_cc_pll0 = {
	.offset = 0x0,
	.vco_table = lucid_evo_vco,
	.num_vco = ARRAY_SIZE(lucid_evo_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_EVO],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_pll0",
			.parent_hws = (const struct clk_hw *[]) {
				&disp_cc_bi_tcxo_fixed.hw,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_evo_ops,
		},
	},
};

static struct clk_alpha_pll disp_cc_pll1 = {
	.offset = 0x1000,
	.vco_table = lucid_evo_vco,
	.num_vco = ARRAY_SIZE(lucid_evo_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_EVO],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_pll1",
			.parent_hws = (const struct clk_hw *[]) {
				&disp_cc_bi_tcxo_fixed.hw,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_evo_ops,
		},
	},
};

static const struct parent_map disp_cc_parent_map_0[] = {
	{ P_BI_TCXO, 0 },
	{ P_DSI0_PHY_PLL_OUT_DSICLK, 1 },
	{ P_DSI0_PHY_PLL_OUT_BYTECLK, 2 },
};

static const struct clk_parent_data disp_cc_parent_data_0[] = {
	{ .hw = &disp_cc_bi_tcxo_fixed.hw },
	{ .index = DT_DSI0_PHY_PLL_OUT_DSICLK },
	{ .index = DT_DSI0_PHY_PLL_OUT_BYTECLK },
};

static const struct parent_map disp_cc_parent_map_1[] = {
	{ P_BI_TCXO, 0 },
	{ P_DISP_CC_PLL0_OUT_MAIN, 1 },
	{ P_DISP_CC_PLL1_OUT_MAIN, 4 },
	{ P_DISP_CC_PLL1_OUT_EVEN, 6 },
};

static const struct clk_parent_data disp_cc_parent_data_1[] = {
	{ .hw = &disp_cc_bi_tcxo_fixed.hw },
	{ .hw = &disp_cc_pll0.clkr.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
};

static const struct parent_map disp_cc_parent_map_2[] = {
	{ P_BI_TCXO, 0 },
};

static const struct clk_parent_data disp_cc_parent_data_2[] = {
	{ .hw = &disp_cc_bi_tcxo_fixed.hw },
};

static const struct clk_parent_data disp_cc_parent_data_2_ao[] = {
	{ .hw = &disp_cc_bi_tcxo_fixed.hw },
};

static const struct parent_map disp_cc_parent_map_3[] = {
	{ P_BI_TCXO, 0 },
	{ P_DISP_CC_PLL1_OUT_MAIN, 4 },
	{ P_DISP_CC_PLL1_OUT_EVEN, 6 },
};

static const struct clk_parent_data disp_cc_parent_data_3[] = {
	{ .hw = &disp_cc_bi_tcxo_fixed.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
};

static const struct parent_map disp_cc_parent_map_4[] = {
	{ P_BI_TCXO, 0 },
	{ P_DSI0_PHY_PLL_OUT_BYTECLK, 2 },
};

static const struct clk_parent_data disp_cc_parent_data_4[] = {
	{ .hw = &disp_cc_bi_tcxo_fixed.hw },
	{ .index = DT_DSI0_PHY_PLL_OUT_BYTECLK },
};

static const struct parent_map disp_cc_parent_map_5[] = {
	{ P_SLEEP_CLK, 0 },
};

static const struct clk_parent_data disp_cc_parent_data_5[] = {
	{ .index = DT_SLEEP_CLK },
};

static const struct freq_tbl ftbl_disp_cc_mdss_ahb_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	F(37500000, P_DISP_CC_PLL1_OUT_MAIN, 16, 0, 0),
	F(75000000, P_DISP_CC_PLL1_OUT_MAIN, 8, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_mdss_ahb_clk_src = {
	.cmd_rcgr = 0x82a4,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_3,
	.freq_tbl = ftbl_disp_cc_mdss_ahb_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_ahb_clk_src",
		.parent_data = disp_cc_parent_data_3,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_3),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_disp_cc_mdss_byte0_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_mdss_byte0_clk_src = {
	.cmd_rcgr = 0x80f8,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_0,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_byte0_clk_src",
		.parent_data = disp_cc_parent_data_0,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_0),
		/* Parent (DSI PHY PLL) must be enabled for RCG update */
		.flags = CLK_SET_RATE_PARENT | CLK_OPS_PARENT_ENABLE,
		.ops = &clk_byte2_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_esc0_clk_src = {
	.cmd_rcgr = 0x8114,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_4,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_esc0_clk_src",
		.parent_data = disp_cc_parent_data_4,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_4),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_disp_cc_mdss_mdp_clk_src[] = {
	F(200000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(325000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(380000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(506000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(608000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_mdss_mdp_clk_src = {
	.cmd_rcgr = 0x80b0,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_1,
	.freq_tbl = ftbl_disp_cc_mdss_mdp_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_mdp_clk_src",
		.parent_data = disp_cc_parent_data_1,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_1),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_pclk0_clk_src = {
	.cmd_rcgr = 0x8098,
	.mnd_width = 8,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_0,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_pclk0_clk_src",
		.parent_data = disp_cc_parent_data_0,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT | CLK_OPS_PARENT_ENABLE,
		.ops = &clk_pixel_ops,
	},
};

static const struct freq_tbl ftbl_disp_cc_mdss_rot_clk_src[] = {
	F(200000000, P_DISP_CC_PLL1_OUT_MAIN, 3, 0, 0),
	F(300000000, P_DISP_CC_PLL1_OUT_MAIN, 2, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_mdss_rot_clk_src = {
	.cmd_rcgr = 0x80c8,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_1,
	.freq_tbl = ftbl_disp_cc_mdss_rot_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_rot_clk_src",
		.parent_data = disp_cc_parent_data_1,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_1),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_vsync_clk_src = {
	.cmd_rcgr = 0x80e0,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_2,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_vsync_clk_src",
		.parent_data = disp_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_2),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_disp_cc_sleep_clk_src[] = {
	F(32000, P_SLEEP_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_sleep_clk_src = {
	.cmd_rcgr = 0xe058,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_5,
	.freq_tbl = ftbl_disp_cc_sleep_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_sleep_clk_src",
		.parent_data = disp_cc_parent_data_5,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_5),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_rcg2 disp_cc_xo_clk_src = {
	.cmd_rcgr = 0xe03c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_2,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_xo_clk_src",
		.parent_data = disp_cc_parent_data_2_ao,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_2_ao),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_regmap_div disp_cc_mdss_byte0_div_clk_src = {
	.reg = 0x8110,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_byte0_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&disp_cc_mdss_byte0_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_branch disp_cc_mdss_ahb1_clk = {
	.halt_reg = 0xa020,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa020,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_ahb1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_ahb_clk = {
	.halt_reg = 0x8094,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8094,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_ahb_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_byte0_clk = {
	.halt_reg = 0x8024,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8024,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_byte0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_byte0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_byte0_intf_clk = {
	.halt_reg = 0x8028,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8028,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_byte0_intf_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_byte0_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_esc0_clk = {
	.halt_reg = 0x802c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x802c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_esc0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_esc0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_mdp1_clk = {
	.halt_reg = 0xa004,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa004,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_mdp1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_mdp_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_mdp_clk = {
	.halt_reg = 0x8008,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8008,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_mdp_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_mdp_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_mdp_lut1_clk = {
	.halt_reg = 0xa014,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa014,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_mdp_lut1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_mdp_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_mdp_lut_clk = {
	.halt_reg = 0x8018,
	.halt_check = BRANCH_HALT_VOTED,
	.clkr = {
		.enable_reg = 0x8018,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_mdp_lut_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_mdp_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_non_gdsc_ahb_clk = {
	.halt_reg = 0xc004,
	.halt_check = BRANCH_HALT_VOTED,
	.clkr = {
		.enable_reg = 0xc004,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_non_gdsc_ahb_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_pclk0_clk = {
	.halt_reg = 0x8004,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8004,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_pclk0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_pclk0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_rot1_clk = {
	.halt_reg = 0xa00c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa00c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_rot1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_rot_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_rot_clk = {
	.halt_reg = 0x8010,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8010,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_rot_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_rot_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_rscc_ahb_clk = {
	.halt_reg = 0xc00c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xc00c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_rscc_ahb_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_ahb_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_rscc_vsync_clk = {
	.halt_reg = 0xc008,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xc008,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_rscc_vsync_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_vsync_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_vsync1_clk = {
	.halt_reg = 0xa01c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xa01c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_vsync1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_vsync_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_vsync_clk = {
	.halt_reg = 0x8020,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8020,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_vsync_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_vsync_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct gdsc disp_cc_mdss_core_gdsc = {
	.gdscr = 0x9000,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0xf,
	.pd = {
		.name = "disp_cc_mdss_core_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = HW_CTRL | POLL_CFG_GDSCR | RETAIN_FF_ENABLE,
};

static struct gdsc disp_cc_mdss_core_int2_gdsc = {
	.gdscr = 0xb000,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0xf,
	.pd = {
		.name = "disp_cc_mdss_core_int2_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = HW_CTRL | POLL_CFG_GDSCR | RETAIN_FF_ENABLE,
};

static struct clk_regmap *disp_cc_sm4450_clocks[] = {
	[DISP_CC_MDSS_AHB1_CLK] = &disp_cc_mdss_ahb1_clk.clkr,
	[DISP_CC_MDSS_AHB_CLK] = &disp_cc_mdss_ahb_clk.clkr,
	[DISP_CC_MDSS_AHB_CLK_SRC] = &disp_cc_mdss_ahb_clk_src.clkr,
	[DISP_CC_MDSS_BYTE0_CLK] = &disp_cc_mdss_byte0_clk.clkr,
	[DISP_CC_MDSS_BYTE0_CLK_SRC] = &disp_cc_mdss_byte0_clk_src.clkr,
	[DISP_CC_MDSS_BYTE0_DIV_CLK_SRC] = &disp_cc_mdss_byte0_div_clk_src.clkr,
	[DISP_CC_MDSS_BYTE0_INTF_CLK] = &disp_cc_mdss_byte0_intf_clk.clkr,
	[DISP_CC_MDSS_ESC0_CLK] = &disp_cc_mdss_esc0_clk.clkr,
	[DISP_CC_MDSS_ESC0_CLK_SRC] = &disp_cc_mdss_esc0_clk_src.clkr,
	[DISP_CC_MDSS_MDP1_CLK] = &disp_cc_mdss_mdp1_clk.clkr,
	[DISP_CC_MDSS_MDP_CLK] = &disp_cc_mdss_mdp_clk.clkr,
	[DISP_CC_MDSS_MDP_CLK_SRC] = &disp_cc_mdss_mdp_clk_src.clkr,
	[DISP_CC_MDSS_MDP_LUT1_CLK] = &disp_cc_mdss_mdp_lut1_clk.clkr,
	[DISP_CC_MDSS_MDP_LUT_CLK] = &disp_cc_mdss_mdp_lut_clk.clkr,
	[DISP_CC_MDSS_NON_GDSC_AHB_CLK] = &disp_cc_mdss_non_gdsc_ahb_clk.clkr,
	[DISP_CC_MDSS_PCLK0_CLK] = &disp_cc_mdss_pclk0_clk.clkr,
	[DISP_CC_MDSS_PCLK0_CLK_SRC] = &disp_cc_mdss_pclk0_clk_src.clkr,
	[DISP_CC_MDSS_ROT1_CLK] = &disp_cc_mdss_rot1_clk.clkr,
	[DISP_CC_MDSS_ROT_CLK] = &disp_cc_mdss_rot_clk.clkr,
	[DISP_CC_MDSS_ROT_CLK_SRC] = &disp_cc_mdss_rot_clk_src.clkr,
	[DISP_CC_MDSS_RSCC_AHB_CLK] = &disp_cc_mdss_rscc_ahb_clk.clkr,
	[DISP_CC_MDSS_RSCC_VSYNC_CLK] = &disp_cc_mdss_rscc_vsync_clk.clkr,
	[DISP_CC_MDSS_VSYNC1_CLK] = &disp_cc_mdss_vsync1_clk.clkr,
	[DISP_CC_MDSS_VSYNC_CLK] = &disp_cc_mdss_vsync_clk.clkr,
	[DISP_CC_MDSS_VSYNC_CLK_SRC] = &disp_cc_mdss_vsync_clk_src.clkr,
	[DISP_CC_PLL0] = &disp_cc_pll0.clkr,
	[DISP_CC_PLL1] = &disp_cc_pll1.clkr,
	[DISP_CC_SLEEP_CLK_SRC] = &disp_cc_sleep_clk_src.clkr,
	[DISP_CC_XO_CLK_SRC] = &disp_cc_xo_clk_src.clkr,
};

static struct gdsc *disp_cc_sm4450_gdscs[] = {
	[DISP_CC_MDSS_CORE_GDSC] = &disp_cc_mdss_core_gdsc,
	[DISP_CC_MDSS_CORE_INT2_GDSC] = &disp_cc_mdss_core_int2_gdsc,
};

static const struct qcom_reset_map disp_cc_sm4450_resets[] = {
	[DISP_CC_MDSS_CORE_BCR] = { 0x8000 },
	[DISP_CC_MDSS_CORE_INT2_BCR] = { 0xa000 },
	[DISP_CC_MDSS_RSCC_BCR] = { 0xc000 },
};

static const struct regmap_config disp_cc_sm4450_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x11008,
	.fast_io = true,
};

static const struct qcom_cc_desc disp_cc_sm4450_desc = {
	.config = &disp_cc_sm4450_regmap_config,
	.clks = disp_cc_sm4450_clocks,
	.num_clks = ARRAY_SIZE(disp_cc_sm4450_clocks),
	.clk_hws = disp_cc_sm4450_hws,
	.num_clk_hws = ARRAY_SIZE(disp_cc_sm4450_hws),
	.resets = disp_cc_sm4450_resets,
	.num_resets = ARRAY_SIZE(disp_cc_sm4450_resets),
	.gdscs = disp_cc_sm4450_gdscs,
	.num_gdscs = ARRAY_SIZE(disp_cc_sm4450_gdscs),
};

static const struct of_device_id disp_cc_sm4450_match_table[] = {
	{ .compatible = "qcom,sm4450-dispcc" },
	{ .compatible = "qcom,ravelin-dispcc" },
	{ }
};
MODULE_DEVICE_TABLE(of, disp_cc_sm4450_match_table);

/*
 * Bootloader continuous splash keeps INTF timing running. Stock SDE places
 * INTF at mdss@ae00000 + 0x36000 = 0xae36000 (MDP@ae01000 catalog offset
 * 0x35000). Stop the timing engine and drain one frame before RCG/PLL
 * registration so Linux can take over with a clean modeset (ROCKNIX
 * WIP_SM4450 approach).
 */
static void disp_cc_sm4450_quiesce_splash(void)
{
	void __iomem *intf;
	u32 en;

	intf = ioremap(0xae36000, SZ_4);
	if (!intf)
		return;

	en = readl(intf);
	if (en) {
		pr_emerg("dispcc-sm4450: quiesce splash INTF_TIMING_ENGINE_EN=0x%x\n",
			 en);
		writel(0, intf); /* INTF_TIMING_ENGINE_EN */
		msleep(50);
	}
	iounmap(intf);

	/*
	 * Stop ABL keep-alive for DSI/DPU/fbdev modeset, but leave
	 * abl_panel_ready set — keep ABL PHY + skip FT7131M DCS re-init.
	 * Host timing is still reprogrammed after quiesce so DPU INTF matches.
	 */
	rg55g1_preserve_abl_display = false;
}

static int disp_cc_sm4450_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	struct clk *pll0;
	int ret;

	disp_cc_sm4450_quiesce_splash();

	regmap = qcom_cc_map(pdev, &disp_cc_sm4450_desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/*
	 * Configure lucid PLLs so Linux clk_set_rate(MDP/…) works after the
	 * splash timing engine has been stopped above.
	 */
	clk_lucid_evo_pll_configure(&disp_cc_pll0, regmap, &disp_cc_pll0_config);
	clk_lucid_evo_pll_configure(&disp_cc_pll1, regmap, &disp_cc_pll0_config);

	/* Keep some clocks always enabled */
	qcom_branch_set_clk_en(regmap, 0xe070); /* DISP_CC_SLEEP_CLK */
	qcom_branch_set_clk_en(regmap, 0xe054); /* DISP_CC_XO_CLK */

	ret = qcom_cc_really_probe(&pdev->dev, &disp_cc_sm4450_desc, regmap);
	if (ret)
		return ret;

	pll0 = clk_hw_get_clk(&disp_cc_pll0.clkr.hw, "dispcc");
	if (!IS_ERR(pll0)) {
		(void)clk_set_rate(pll0, 600000000);
		pr_emerg("dispcc-sm4450: bi_tcxo_fixed=19200000 pll0 rate=%lu\n",
			 clk_get_rate(pll0));
		clk_put(pll0);
	}

	return 0;
}

static struct platform_driver disp_cc_sm4450_driver = {
	.probe = disp_cc_sm4450_probe,
	.driver = {
		.name = "dispcc-sm4450",
		.of_match_table = disp_cc_sm4450_match_table,
	},
};

static bool dispcc_sm4450_registered;

int rg55g1_dispcc_driver_register(void)
{
	int ret;

	if (dispcc_sm4450_registered)
		return 0;

	ret = platform_driver_register(&disp_cc_sm4450_driver);
	if (!ret)
		dispcc_sm4450_registered = true;
	return ret;
}

static int __init disp_cc_sm4450_init(void)
{
	return rg55g1_dispcc_driver_register();
}
module_init(disp_cc_sm4450_init);

MODULE_DESCRIPTION("QTI DISPCC SM4450 Driver");
MODULE_LICENSE("GPL");
