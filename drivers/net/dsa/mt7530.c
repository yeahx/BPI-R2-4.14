// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mediatek MT7530 DSA Switch driver
 * Copyright (C) 2017 Sean Wang <sean.wang@mediatek.com>
 */
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phylink.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>
#include <net/dsa.h>

#include "mt7530.h"

/* String, offset, and register size in bytes if different from 4 bytes */
static const struct mt7530_mib_desc mt7530_mib[] = {
	MIB_DESC(1, 0x00, "TxDrop"),
	MIB_DESC(1, 0x04, "TxCrcErr"),
	MIB_DESC(1, 0x08, "TxUnicast"),
	MIB_DESC(1, 0x0c, "TxMulticast"),
	MIB_DESC(1, 0x10, "TxBroadcast"),
	MIB_DESC(1, 0x14, "TxCollision"),
	MIB_DESC(1, 0x18, "TxSingleCollision"),
	MIB_DESC(1, 0x1c, "TxMultipleCollision"),
	MIB_DESC(1, 0x20, "TxDeferred"),
	MIB_DESC(1, 0x24, "TxLateCollision"),
	MIB_DESC(1, 0x28, "TxExcessiveCollistion"),
	MIB_DESC(1, 0x2c, "TxPause"),
	MIB_DESC(1, 0x30, "TxPktSz64"),
	MIB_DESC(1, 0x34, "TxPktSz65To127"),
	MIB_DESC(1, 0x38, "TxPktSz128To255"),
	MIB_DESC(1, 0x3c, "TxPktSz256To511"),
	MIB_DESC(1, 0x40, "TxPktSz512To1023"),
	MIB_DESC(1, 0x44, "Tx1024ToMax"),
	MIB_DESC(2, 0x48, "TxBytes"),
	MIB_DESC(1, 0x60, "RxDrop"),
	MIB_DESC(1, 0x64, "RxFiltering"),
	MIB_DESC(1, 0x6c, "RxMulticast"),
	MIB_DESC(1, 0x70, "RxBroadcast"),
	MIB_DESC(1, 0x74, "RxAlignErr"),
	MIB_DESC(1, 0x78, "RxCrcErr"),
	MIB_DESC(1, 0x7c, "RxUnderSizeErr"),
	MIB_DESC(1, 0x80, "RxFragErr"),
	MIB_DESC(1, 0x84, "RxOverSzErr"),
	MIB_DESC(1, 0x88, "RxJabberErr"),
	MIB_DESC(1, 0x8c, "RxPause"),
	MIB_DESC(1, 0x90, "RxPktSz64"),
	MIB_DESC(1, 0x94, "RxPktSz65To127"),
	MIB_DESC(1, 0x98, "RxPktSz128To255"),
	MIB_DESC(1, 0x9c, "RxPktSz256To511"),
	MIB_DESC(1, 0xa0, "RxPktSz512To1023"),
	MIB_DESC(1, 0xa4, "RxPktSz1024ToMax"),
	MIB_DESC(2, 0xa8, "RxBytes"),
	MIB_DESC(1, 0xb0, "RxCtrlDrop"),
	MIB_DESC(1, 0xb4, "RxIngressDrop"),
	MIB_DESC(1, 0xb8, "RxArlDrop"),
};

static int
core_read_mmd_indirect(struct mt7530_priv *priv, int prtad, int devad)
{
	struct mii_bus *bus = priv->bus;
	int value, ret;

	/* Write the desired MMD Devad */
	ret = bus->write(bus, 0, MII_MMD_CTRL, devad);
	if (ret < 0)
		goto err;

	/* Write the desired MMD register address */
	ret = bus->write(bus, 0, MII_MMD_DATA, prtad);
	if (ret < 0)
		goto err;

	/* Select the Function : DATA with no post increment */
	ret = bus->write(bus, 0, MII_MMD_CTRL, (devad | MII_MMD_CTRL_NOINCR));
	if (ret < 0)
		goto err;

	/* Read the content of the MMD's selected register */
	value = bus->read(bus, 0, MII_MMD_DATA);

	return value;
err:
	dev_err(&bus->dev,  "failed to read mmd register\n");

	return ret;
}

static int
core_write_mmd_indirect(struct mt7530_priv *priv, int prtad,
			int devad, u32 data)
{
	struct mii_bus *bus = priv->bus;
	int ret;

	/* Write the desired MMD Devad */
	ret = bus->write(bus, 0, MII_MMD_CTRL, devad);
	if (ret < 0)
		goto err;

	/* Write the desired MMD register address */
	ret = bus->write(bus, 0, MII_MMD_DATA, prtad);
	if (ret < 0)
		goto err;

	/* Select the Function : DATA with no post increment */
	ret = bus->write(bus, 0, MII_MMD_CTRL, (devad | MII_MMD_CTRL_NOINCR));
	if (ret < 0)
		goto err;

	/* Write the data into MMD's selected register */
	ret = bus->write(bus, 0, MII_MMD_DATA, data);
err:
	if (ret < 0)
		dev_err(&bus->dev,
			"failed to write mmd register\n");
	return ret;
}

static void
core_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	core_write_mmd_indirect(priv, reg, MDIO_MMD_VEND2, val);

	mutex_unlock(&bus->mdio_lock);
}

static void
core_rmw(struct mt7530_priv *priv, u32 reg, u32 mask, u32 set)
{
	struct mii_bus *bus = priv->bus;
	u32 val;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	val = core_read_mmd_indirect(priv, reg, MDIO_MMD_VEND2);
	val &= ~mask;
	val |= set;
	core_write_mmd_indirect(priv, reg, MDIO_MMD_VEND2, val);

	mutex_unlock(&bus->mdio_lock);
}

static void
core_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	core_rmw(priv, reg, 0, val);
}

static void
core_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	core_rmw(priv, reg, val, 0);
}

static int
mt7530_mii_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;
	u16 page, r, lo, hi;
	int ret;

	page = (reg >> 6) & 0x3ff;
	r  = (reg >> 2) & 0xf;
	lo = val & 0xffff;
	hi = val >> 16;

	/* MT7530 uses 31 as the pseudo port */
	ret = bus->write(bus, 0x1f, 0x1f, page);
	if (ret < 0)
		goto err;

	ret = bus->write(bus, 0x1f, r,  lo);
	if (ret < 0)
		goto err;

	ret = bus->write(bus, 0x1f, 0x10, hi);
err:
	if (ret < 0)
		dev_err(&bus->dev,
			"failed to write mt7530 register\n");
	return ret;
}

static u32
mt7530_mii_read(struct mt7530_priv *priv, u32 reg)
{
	struct mii_bus *bus = priv->bus;
	u16 page, r, lo, hi;
	int ret;

	page = (reg >> 6) & 0x3ff;
	r = (reg >> 2) & 0xf;

	/* MT7530 uses 31 as the pseudo port */
	ret = bus->write(bus, 0x1f, 0x1f, page);
	if (ret < 0) {
		dev_err(&bus->dev,
			"failed to read mt7530 register\n");
		return ret;
	}

	lo = bus->read(bus, 0x1f, r);
	hi = bus->read(bus, 0x1f, 0x10);

	return (hi << 16) | (lo & 0xffff);
}

static void
mt7530_write(struct mt7530_priv *priv, u32 reg, u32 val)
{
	struct mii_bus *bus = priv->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	mt7530_mii_write(priv, reg, val);

	mutex_unlock(&bus->mdio_lock);
}

static u32
_mt7530_unlocked_read(struct mt7530_dummy_poll *p)
{
	return mt7530_mii_read(p->priv, p->reg);
}

static u32
_mt7530_read(struct mt7530_dummy_poll *p)
{
	struct mii_bus		*bus = p->priv->bus;
	u32 val;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	val = mt7530_mii_read(p->priv, p->reg);

	mutex_unlock(&bus->mdio_lock);

	return val;
}

static u32
mt7530_read(struct mt7530_priv *priv, u32 reg)
{
	struct mt7530_dummy_poll p;

	INIT_MT7530_DUMMY_POLL(&p, priv, reg);
	return _mt7530_read(&p);
}

static void
mt7530_rmw(struct mt7530_priv *priv, u32 reg,
	   u32 mask, u32 set)
{
	struct mii_bus *bus = priv->bus;
	u32 val;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	val = mt7530_mii_read(priv, reg);
	val &= ~mask;
	val |= set;
	mt7530_mii_write(priv, reg, val);

	mutex_unlock(&bus->mdio_lock);
}

static void
mt7530_set(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_rmw(priv, reg, 0, val);
}

static void
mt7530_clear(struct mt7530_priv *priv, u32 reg, u32 val)
{
	mt7530_rmw(priv, reg, val, 0);
}

static int
mt7531_ind_mmd_phy_read(struct mt7530_priv *priv, int port, int devad,
			int regnum)
{
	struct mii_bus *bus = priv->bus;
	struct mt7530_dummy_poll p;
	u32 reg, val;
	int ret;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MDIO_CL45_ADDR | MDIO_PHY_ADDR(port) | MDIO_DEV_ADDR(devad) |
	      regnum;
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MDIO_CL45_READ | MDIO_PHY_ADDR(port) | MDIO_DEV_ADDR(devad);
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	ret = val & MDIO_RW_DATA_MASK;
out:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int
mt7531_ind_mmd_phy_write(struct mt7530_priv *priv, int port, int devad,
			 int regnum, u32 data)
{
	struct mii_bus *bus = priv->bus;
	struct mt7530_dummy_poll p;
	u32 val, reg;
	int ret;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MDIO_CL45_ADDR | MDIO_PHY_ADDR(port) | MDIO_DEV_ADDR(devad) |
	      regnum;
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MDIO_CL45_WRITE | MDIO_PHY_ADDR(port) | MDIO_DEV_ADDR(devad) |
	      data;
	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

out:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int
mt7530_fdb_cmd(struct mt7530_priv *priv, enum mt7530_fdb_cmd cmd, u32 *rsp)
{
	u32 val;
	int ret;
	struct mt7530_dummy_poll p;

	/* Set the command operating upon the MAC address entries */
	val = ATC_BUSY | ATC_MAT(0) | cmd;
	mt7530_write(priv, MT7530_ATC, val);

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7530_ATC);
	ret = readx_poll_timeout(_mt7530_read, &p, val,
				 !(val & ATC_BUSY), 20, 20000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	/* Additional sanity for read command if the specified
	 * entry is invalid
	 */
	val = mt7530_read(priv, MT7530_ATC);
	if ((cmd == MT7530_FDB_READ) && (val & ATC_INVALID))
		return -EINVAL;

	if (rsp)
		*rsp = val;

	return 0;
}

static void
mt7530_fdb_read(struct mt7530_priv *priv, struct mt7530_fdb *fdb)
{
	u32 reg[3];
	int i;

	/* Read from ARL table into an array */
	for (i = 0; i < 3; i++) {
		reg[i] = mt7530_read(priv, MT7530_TSRA1 + (i * 4));

		dev_dbg(priv->dev, "%s(%d) reg[%d]=0x%x\n",
			__func__, __LINE__, i, reg[i]);
	}

	fdb->vid = (reg[1] >> CVID) & CVID_MASK;
	fdb->aging = (reg[2] >> AGE_TIMER) & AGE_TIMER_MASK;
	fdb->port_mask = (reg[2] >> PORT_MAP) & PORT_MAP_MASK;
	fdb->mac[0] = (reg[0] >> MAC_BYTE_0) & MAC_BYTE_MASK;
	fdb->mac[1] = (reg[0] >> MAC_BYTE_1) & MAC_BYTE_MASK;
	fdb->mac[2] = (reg[0] >> MAC_BYTE_2) & MAC_BYTE_MASK;
	fdb->mac[3] = (reg[0] >> MAC_BYTE_3) & MAC_BYTE_MASK;
	fdb->mac[4] = (reg[1] >> MAC_BYTE_4) & MAC_BYTE_MASK;
	fdb->mac[5] = (reg[1] >> MAC_BYTE_5) & MAC_BYTE_MASK;
	fdb->noarp = ((reg[2] >> ENT_STATUS) & ENT_STATUS_MASK) == STATIC_ENT;
}

static void
mt7530_fdb_write(struct mt7530_priv *priv, u16 vid,
		 u8 port_mask, const u8 *mac,
		 u8 aging, u8 type)
{
	u32 reg[3] = { 0 };
	int i;

	reg[1] |= vid & CVID_MASK;
	reg[2] |= (aging & AGE_TIMER_MASK) << AGE_TIMER;
	reg[2] |= (port_mask & PORT_MAP_MASK) << PORT_MAP;
	/* STATIC_ENT indicate that entry is static wouldn't
	 * be aged out and STATIC_EMP specified as erasing an
	 * entry
	 */
	reg[2] |= (type & ENT_STATUS_MASK) << ENT_STATUS;
	reg[1] |= mac[5] << MAC_BYTE_5;
	reg[1] |= mac[4] << MAC_BYTE_4;
	reg[0] |= mac[3] << MAC_BYTE_3;
	reg[0] |= mac[2] << MAC_BYTE_2;
	reg[0] |= mac[1] << MAC_BYTE_1;
	reg[0] |= mac[0] << MAC_BYTE_0;

	/* Write array into the ARL table */
	for (i = 0; i < 3; i++)
		mt7530_write(priv, MT7530_ATA1 + (i * 4), reg[i]);
}

static int
mt7530_pad_clk_setup(struct dsa_switch *ds, phy_interface_t mode)
{
	struct mt7530_priv *priv = ds->priv;
	u32 ncpo1, ssc_delta, trgint, i, xtal;

	xtal = mt7530_read(priv, MT7530_MHWTRAP) & HWTRAP_XTAL_MASK;

	if (xtal == HWTRAP_XTAL_20MHZ) {
		dev_err(priv->dev,
			"%s: MT7530 with a 20MHz XTAL is not supported!\n",
			__func__);
		return -EINVAL;
	}

	switch (mode) {
	case PHY_INTERFACE_MODE_RGMII:
		trgint = 0;
		/* PLL frequency: 125MHz */
		ncpo1 = 0x0c80;
		break;
	case PHY_INTERFACE_MODE_TRGMII:
		trgint = 1;
		if (priv->id == ID_MT7621) {
			/* PLL frequency: 150MHz: 1.2GBit */
			if (xtal == HWTRAP_XTAL_40MHZ)
				ncpo1 = 0x0780;
			if (xtal == HWTRAP_XTAL_25MHZ)
				ncpo1 = 0x0a00;
		} else { /* PLL frequency: 250MHz: 2.0Gbit */
			if (xtal == HWTRAP_XTAL_40MHZ)
				ncpo1 = 0x0c80;
			if (xtal == HWTRAP_XTAL_25MHZ)
				ncpo1 = 0x1400;
		}
		break;
	default:
		dev_err(priv->dev, "xMII mode %d not supported\n", mode);
		return -EINVAL;
	}

	if (xtal == HWTRAP_XTAL_25MHZ)
		ssc_delta = 0x57;
	else
		ssc_delta = 0x87;

	mt7530_rmw(priv, MT7530_P6ECR, P6_INTF_MODE_MASK,
		   P6_INTF_MODE(trgint));

	/* Lower Tx Driving for TRGMII path */
	for (i = 0 ; i < NUM_TRGMII_CTRL ; i++)
		mt7530_write(priv, MT7530_TRGMII_TD_ODT(i),
			     TD_DM_DRVP(8) | TD_DM_DRVN(8));

	/* Setup core clock for MT7530 */
	if (!trgint) {
		/* Disable MT7530 core clock */
		core_clear(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);

		/* Disable PLL, since phy_device has not yet been created
		 * provided for phy_[read,write]_mmd_indirect is called, we
		 * provide our own core_write_mmd_indirect to complete this
		 * function.
		 */
		core_write_mmd_indirect(priv,
					CORE_GSWPLL_GRP1,
					MDIO_MMD_VEND2,
					0);

		/* Set core clock into 500Mhz */
		core_write(priv, CORE_GSWPLL_GRP2,
			   RG_GSWPLL_POSDIV_500M(1) |
			   RG_GSWPLL_FBKDIV_500M(25));

		/* Enable PLL */
		core_write(priv, CORE_GSWPLL_GRP1,
			   RG_GSWPLL_EN_PRE |
			   RG_GSWPLL_POSDIV_200M(2) |
			   RG_GSWPLL_FBKDIV_200M(32));

		/* Enable MT7530 core clock */
		core_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);
	}

	/* Setup the MT7530 TRGMII Tx Clock */
	core_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_GSWCK_EN);
	core_write(priv, CORE_PLL_GROUP5, RG_LCDDS_PCW_NCPO1(ncpo1));
	core_write(priv, CORE_PLL_GROUP6, RG_LCDDS_PCW_NCPO0(0));
	core_write(priv, CORE_PLL_GROUP10, RG_LCDDS_SSC_DELTA(ssc_delta));
	core_write(priv, CORE_PLL_GROUP11, RG_LCDDS_SSC_DELTA1(ssc_delta));
	core_write(priv, CORE_PLL_GROUP4,
		   RG_SYSPLL_DDSFBK_EN | RG_SYSPLL_BIAS_EN |
		   RG_SYSPLL_BIAS_LPF_EN);
	core_write(priv, CORE_PLL_GROUP2,
		   RG_SYSPLL_EN_NORMAL | RG_SYSPLL_VODEN |
		   RG_SYSPLL_POSDIV(1));
	core_write(priv, CORE_PLL_GROUP7,
		   RG_LCDDS_PCW_NCPO_CHG | RG_LCCDS_C(3) |
		   RG_LCDDS_PWDB | RG_LCDDS_ISO_EN);
	core_set(priv, CORE_TRGMII_GSW_CLK_CG,
		 REG_GSWCK_EN | REG_TRGMIICK_EN);

	if (!trgint)
		for (i = 0 ; i < NUM_TRGMII_CTRL; i++)
			mt7530_rmw(priv, MT7530_TRGMII_RD(i),
				   RD_TAP_MASK, RD_TAP(16));
	return 0;
}

static void
mt7530_mib_reset(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_write(priv, MT7530_MIB_CCR, CCR_MIB_FLUSH);
	mt7530_write(priv, MT7530_MIB_CCR, CCR_MIB_ACTIVATE);
}

static void
mt7530_port_set_status(struct mt7530_priv *priv, int port, int enable)
{
	u32 mask = PMCR_TX_EN | PMCR_RX_EN | PMCR_FORCE_LNK;

	if (enable)
		mt7530_set(priv, MT7530_PMCR_P(port), mask);
	else
		mt7530_clear(priv, MT7530_PMCR_P(port), mask);
}

static int mt7530_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	struct mt7530_priv *priv = ds->priv;

	return mdiobus_read_nested(priv->bus, port, regnum);
}

static int mt7530_phy_write(struct dsa_switch *ds, int port, int regnum,
			    u16 val)
{
	struct mt7530_priv *priv = ds->priv;

	return mdiobus_write_nested(priv->bus, port, regnum, val);
}

static int
mt7531_ind_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	struct mt7530_priv *priv = ds->priv;
	struct mii_bus *bus = priv->bus;
	struct mt7530_dummy_poll p;
	int ret;
	u32 val;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	val = MDIO_CL22_READ | MDIO_PHY_ADDR(port) | MDIO_REG_ADDR(regnum);

	mt7530_mii_write(priv, MT7531_PHY_IAC, val | PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, val,
				 !(val & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	ret = val & MDIO_RW_DATA_MASK;
out:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static int
mt7531_ind_phy_write(struct dsa_switch *ds, int port, int regnum,
		     u16 data)
{
	struct mt7530_priv *priv = ds->priv;
	struct mii_bus *bus = priv->bus;
	struct mt7530_dummy_poll p;
	int ret;
	u32 reg;

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7531_PHY_IAC);

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, reg,
				 !(reg & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

	reg = MDIO_CL22_WRITE | MDIO_PHY_ADDR(port) | MDIO_REG_ADDR(regnum) |
	      data;

	mt7530_mii_write(priv, MT7531_PHY_IAC, reg | PHY_ACS_ST);

	ret = readx_poll_timeout(_mt7530_unlocked_read, &p, reg,
				 !(reg & PHY_ACS_ST), 20, 100000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		goto out;
	}

out:
	mutex_unlock(&bus->mdio_lock);

	return ret;
}

static void
mt7530_get_strings(struct dsa_switch *ds, int port, u32 stringset,
		   uint8_t *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(mt7530_mib); i++)
		strncpy(data + i * ETH_GSTRING_LEN, mt7530_mib[i].name,
			ETH_GSTRING_LEN);
}

static void
mt7530_get_ethtool_stats(struct dsa_switch *ds, int port,
			 uint64_t *data)
{
	struct mt7530_priv *priv = ds->priv;
	const struct mt7530_mib_desc *mib;
	u32 reg, i;
	u64 hi;

	for (i = 0; i < ARRAY_SIZE(mt7530_mib); i++) {
		mib = &mt7530_mib[i];
		reg = MT7530_PORT_MIB_COUNTER(port) + mib->offset;

		data[i] = mt7530_read(priv, reg);
		if (mib->size == 2) {
			hi = mt7530_read(priv, reg + 4);
			data[i] |= hi << 32;
		}
	}
}

static int
mt7530_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(mt7530_mib);
}

static void mt7530_setup_port5(struct dsa_switch *ds, phy_interface_t interface)
{
	struct mt7530_priv *priv = ds->priv;
	u8 tx_delay = 0;
	int val;

	mutex_lock(&priv->reg_mutex);

	val = mt7530_read(priv, MT7530_MHWTRAP);

	val |= MHWTRAP_MANUAL | MHWTRAP_P5_MAC_SEL | MHWTRAP_P5_DIS;
	val &= ~MHWTRAP_P5_RGMII_MODE & ~MHWTRAP_PHY0_SEL;

	switch (priv->p5_intf_sel) {
	case P5_INTF_SEL_PHY_P0:
		/* MT7530_P5_MODE_GPHY_P0: 2nd GMAC -> P5 -> P0 */
		val |= MHWTRAP_PHY0_SEL;
		/* fall through */
	case P5_INTF_SEL_PHY_P4:
		/* MT7530_P5_MODE_GPHY_P4: 2nd GMAC -> P5 -> P4 */
		val &= ~MHWTRAP_P5_MAC_SEL & ~MHWTRAP_P5_DIS;

		/* Setup the MAC by default for the cpu port */
		mt7530_write(priv, MT7530_PMCR_P(5), 0x56300);
		break;
	case P5_INTF_SEL_GMAC5:
		/* MT7530_P5_MODE_GMAC: P5 -> External phy or 2nd GMAC */
		val &= ~MHWTRAP_P5_DIS;
		break;
	case P5_DISABLED:
		interface = PHY_INTERFACE_MODE_NA;
		break;
	default:
		dev_err(ds->dev, "Unsupported p5_intf_sel %d\n",
			priv->p5_intf_sel);
		goto unlock_exit;
	}

	/* Setup RGMII settings */
	if (phy_interface_mode_is_rgmii(interface)) {
		val |= MHWTRAP_P5_RGMII_MODE;

		/* P5 RGMII RX Clock Control: delay setting for 1000M */
		mt7530_write(priv, MT7530_P5RGMIIRXCR, CSR_RGMII_EDGE_ALIGN);

		/* Don't set delay in DSA mode */
		if (!dsa_is_dsa_port(priv->ds, 5) &&
		    (interface == PHY_INTERFACE_MODE_RGMII_TXID ||
		     interface == PHY_INTERFACE_MODE_RGMII_ID))
			tx_delay = 4; /* n * 0.5 ns */

		/* P5 RGMII TX Clock Control: delay x */
		mt7530_write(priv, MT7530_P5RGMIITXCR,
			     CSR_RGMII_TXC_CFG(0x10 + tx_delay));

		/* reduce P5 RGMII Tx driving, 8mA */
		mt7530_write(priv, MT7530_IO_DRV_CR,
			     P5_IO_CLK_DRV(1) | P5_IO_DATA_DRV(1));
	}

	mt7530_write(priv, MT7530_MHWTRAP, val);

	dev_dbg(ds->dev, "Setup P5, HWTRAP=0x%x, intf_sel=%s, phy-mode=%s\n",
		val, p5_intf_modes(priv->p5_intf_sel), phy_modes(interface));

	priv->p5_interface = interface;

unlock_exit:
	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_cpu_port_enable(struct mt7530_priv *priv,
		       int port)
{
	/* Enable Mediatek header mode on the cpu port */
	mt7530_write(priv, MT7530_PVC_P(port),
		     PORT_SPEC_TAG);

	/* Unknown multicast frame forwarding to the cpu port */
	mt7530_rmw(priv, MT7530_MFC, UNM_FFP_MASK, UNM_FFP(BIT(port)));

	/* Set CPU port number */
	if (priv->id == ID_MT7621)
		mt7530_rmw(priv, MT7530_MFC, CPU_MASK, CPU_EN | CPU_PORT(port));

	/* CPU port gets connected to all user ports of
	 * the switch
	 */
	mt7530_write(priv, MT7530_PCR_P(port),
		     PCR_MATRIX(dsa_user_ports(priv->ds)));

	return 0;
}

static int
mt7530_port_enable(struct dsa_switch *ds, int port,
		   struct phy_device *phy)
{
	struct mt7530_priv *priv = ds->priv;

	if (!dsa_is_user_port(ds, port))
		return 0;

	mutex_lock(&priv->reg_mutex);

	/* Allow the user port gets connected to the cpu port and also
	 * restore the port matrix if the port is the member of a certain
	 * bridge.
	 */
	priv->ports[port].pm |= PCR_MATRIX(BIT(MT7530_CPU_PORT));
	priv->ports[port].enable = true;
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_MATRIX_MASK,
		   priv->ports[port].pm);
	mt7530_port_set_status(priv, port, 0);

	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static void
mt7530_port_disable(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	if (!dsa_is_user_port(ds, port))
		return;

	mutex_lock(&priv->reg_mutex);

	/* Clear up all port matrix which could be restored in the next
	 * enablement for the port.
	 */
	priv->ports[port].enable = false;
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_MATRIX_MASK,
		   PCR_MATRIX_CLR);
	mt7530_port_set_status(priv, port, 0);

	mutex_unlock(&priv->reg_mutex);
}

static void
mt7530_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
	struct mt7530_priv *priv = ds->priv;
	u32 stp_state;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = MT7530_STP_DISABLED;
		break;
	case BR_STATE_BLOCKING:
		stp_state = MT7530_STP_BLOCKING;
		break;
	case BR_STATE_LISTENING:
		stp_state = MT7530_STP_LISTENING;
		break;
	case BR_STATE_LEARNING:
		stp_state = MT7530_STP_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = MT7530_STP_FORWARDING;
		break;
	}

	mt7530_rmw(priv, MT7530_SSP_P(port), FID_PST_MASK, stp_state);
}

static int
mt7530_port_bridge_join(struct dsa_switch *ds, int port,
			struct net_device *bridge)
{
	struct mt7530_priv *priv = ds->priv;
	u32 port_bitmap = BIT(MT7530_CPU_PORT);
	int i;

	mutex_lock(&priv->reg_mutex);

	for (i = 0; i < MT7530_NUM_PORTS; i++) {
		/* Add this port to the port matrix of the other ports in the
		 * same bridge. If the port is disabled, port matrix is kept
		 * and not being setup until the port becomes enabled.
		 */
		if (dsa_is_user_port(ds, i) && i != port) {
			if (dsa_to_port(ds, i)->bridge_dev != bridge)
				continue;
			if (priv->ports[i].enable)
				mt7530_set(priv, MT7530_PCR_P(i),
					   PCR_MATRIX(BIT(port)));
			priv->ports[i].pm |= PCR_MATRIX(BIT(port));

			port_bitmap |= BIT(i);
		}
	}

	/* Add the all other ports to this port matrix. */
	if (priv->ports[port].enable)
		mt7530_rmw(priv, MT7530_PCR_P(port),
			   PCR_MATRIX_MASK, PCR_MATRIX(port_bitmap));
	priv->ports[port].pm |= PCR_MATRIX(port_bitmap);

	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static void
mt7530_port_set_vlan_unaware(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;
	bool all_user_ports_removed = true;
	int i;

	/* When a port is removed from the bridge, the port would be set up
	 * back to the default as is at initial boot which is a VLAN-unaware
	 * port.
	 */
	mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
		   MT7530_PORT_MATRIX_MODE);
	mt7530_rmw(priv, MT7530_PVC_P(port), VLAN_ATTR_MASK | PVC_EG_TAG_MASK,
		   VLAN_ATTR(MT7530_VLAN_TRANSPARENT) |
		   PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT));

	for (i = 0; i < MT7530_NUM_PORTS; i++) {
		if (dsa_is_user_port(ds, i) &&
		    dsa_port_is_vlan_filtering(&ds->ports[i])) {
			all_user_ports_removed = false;
			break;
		}
	}

	/* CPU port also does the same thing until all user ports belonging to
	 * the CPU port get out of VLAN filtering mode.
	 */
	if (all_user_ports_removed) {
		mt7530_write(priv, MT7530_PCR_P(MT7530_CPU_PORT),
			     PCR_MATRIX(dsa_user_ports(priv->ds)));
		mt7530_write(priv, MT7530_PVC_P(MT7530_CPU_PORT), PORT_SPEC_TAG
			     | PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT));
	}
}

static void
mt7530_port_set_vlan_aware(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	/* Trapped into security mode allows packet forwarding through VLAN
	 * table lookup. CPU port is set to fallback mode to let untagged
	 * frames pass through.
	 */
	if (dsa_is_cpu_port(ds, port))
		mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
			   MT7530_PORT_FALLBACK_MODE);
	else
		mt7530_rmw(priv, MT7530_PCR_P(port), PCR_PORT_VLAN_MASK,
			   MT7530_PORT_SECURITY_MODE);

	/* Set the port as a user port which is to be able to recognize VID
	 * from incoming packets before fetching entry within the VLAN table.
	 */
	mt7530_rmw(priv, MT7530_PVC_P(port), VLAN_ATTR_MASK | PVC_EG_TAG_MASK,
		   VLAN_ATTR(MT7530_VLAN_USER) |
		   PVC_EG_TAG(MT7530_VLAN_EG_DISABLED));
}

static void
mt7530_port_bridge_leave(struct dsa_switch *ds, int port,
			 struct net_device *bridge)
{
	struct mt7530_priv *priv = ds->priv;
	int i;

	mutex_lock(&priv->reg_mutex);

	for (i = 0; i < MT7530_NUM_PORTS; i++) {
		/* Remove this port from the port matrix of the other ports
		 * in the same bridge. If the port is disabled, port matrix
		 * is kept and not being setup until the port becomes enabled.
		 * And the other port's port matrix cannot be broken when the
		 * other port is still a VLAN-aware port.
		 */
		if (dsa_is_user_port(ds, i) && i != port &&
		   !dsa_port_is_vlan_filtering(&ds->ports[i])) {
			if (dsa_to_port(ds, i)->bridge_dev != bridge)
				continue;
			if (priv->ports[i].enable)
				mt7530_clear(priv, MT7530_PCR_P(i),
					     PCR_MATRIX(BIT(port)));
			priv->ports[i].pm &= ~PCR_MATRIX(BIT(port));
		}
	}

	/* Set the cpu port to be the only one in the port matrix of
	 * this port.
	 */
	if (priv->ports[port].enable)
		mt7530_rmw(priv, MT7530_PCR_P(port), PCR_MATRIX_MASK,
			   PCR_MATRIX(BIT(MT7530_CPU_PORT)));
	priv->ports[port].pm = PCR_MATRIX(BIT(MT7530_CPU_PORT));

	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_port_fdb_add(struct dsa_switch *ds, int port,
		    const unsigned char *addr, u16 vid)
{
	struct mt7530_priv *priv = ds->priv;
	int ret;
	u8 port_mask = BIT(port);

	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, vid, port_mask, addr, -1, STATIC_ENT);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, NULL);
	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_port_fdb_del(struct dsa_switch *ds, int port,
		    const unsigned char *addr, u16 vid)
{
	struct mt7530_priv *priv = ds->priv;
	int ret;
	u8 port_mask = BIT(port);

	mutex_lock(&priv->reg_mutex);
	mt7530_fdb_write(priv, vid, port_mask, addr, -1, STATIC_EMP);
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_WRITE, NULL);
	mutex_unlock(&priv->reg_mutex);

	return ret;
}

static int
mt7530_port_fdb_dump(struct dsa_switch *ds, int port,
		     dsa_fdb_dump_cb_t *cb, void *data)
{
	struct mt7530_priv *priv = ds->priv;
	struct mt7530_fdb _fdb = { 0 };
	int cnt = MT7530_NUM_FDB_RECORDS;
	int ret = 0;
	u32 rsp = 0;

	mutex_lock(&priv->reg_mutex);

	ret = mt7530_fdb_cmd(priv, MT7530_FDB_START, &rsp);
	if (ret < 0)
		goto err;

	do {
		if (rsp & ATC_SRCH_HIT) {
			mt7530_fdb_read(priv, &_fdb);
			if (_fdb.port_mask & BIT(port)) {
				ret = cb(_fdb.mac, _fdb.vid, _fdb.noarp,
					 data);
				if (ret < 0)
					break;
			}
		}
	} while (--cnt &&
		 !(rsp & ATC_SRCH_END) &&
		 !mt7530_fdb_cmd(priv, MT7530_FDB_NEXT, &rsp));
err:
	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static int
mt7530_vlan_cmd(struct mt7530_priv *priv, enum mt7530_vlan_cmd cmd, u16 vid)
{
	struct mt7530_dummy_poll p;
	u32 val;
	int ret;

	val = VTCR_BUSY | VTCR_FUNC(cmd) | vid;
	mt7530_write(priv, MT7530_VTCR, val);

	INIT_MT7530_DUMMY_POLL(&p, priv, MT7530_VTCR);
	ret = readx_poll_timeout(_mt7530_read, &p, val,
				 !(val & VTCR_BUSY), 20, 20000);
	if (ret < 0) {
		dev_err(priv->dev, "poll timeout\n");
		return ret;
	}

	val = mt7530_read(priv, MT7530_VTCR);
	if (val & VTCR_INVALID) {
		dev_err(priv->dev, "read VTCR invalid\n");
		return -EINVAL;
	}

	return 0;
}

static int
mt7530_port_vlan_filtering(struct dsa_switch *ds, int port,
			   bool vlan_filtering)
{
	if (vlan_filtering) {
		/* The port is being kept as VLAN-unaware port when bridge is
		 * set up with vlan_filtering not being set, Otherwise, the
		 * port and the corresponding CPU port is required the setup
		 * for becoming a VLAN-aware port.
		 */
		mt7530_port_set_vlan_aware(ds, port);
		mt7530_port_set_vlan_aware(ds, MT7530_CPU_PORT);
	} else {
		mt7530_port_set_vlan_unaware(ds, port);
	}

	return 0;
}

static int
mt7530_port_vlan_prepare(struct dsa_switch *ds, int port,
			 const struct switchdev_obj_port_vlan *vlan)
{
	/* nothing needed */

	return 0;
}

static void
mt7530_hw_vlan_add(struct mt7530_priv *priv,
		   struct mt7530_hw_vlan_entry *entry)
{
	u8 new_members;
	u32 val;

	new_members = entry->old_members | BIT(entry->port) |
		      BIT(MT7530_CPU_PORT);

	/* Validate the entry with independent learning, create egress tag per
	 * VLAN and joining the port as one of the port members.
	 */
	val = IVL_MAC | VTAG_EN | PORT_MEM(new_members) | VLAN_VALID;
	mt7530_write(priv, MT7530_VAWD1, val);

	/* Decide whether adding tag or not for those outgoing packets from the
	 * port inside the VLAN.
	 */
	val = entry->untagged ? MT7530_VLAN_EGRESS_UNTAG :
				MT7530_VLAN_EGRESS_TAG;
	mt7530_rmw(priv, MT7530_VAWD2,
		   ETAG_CTRL_P_MASK(entry->port),
		   ETAG_CTRL_P(entry->port, val));

	/* CPU port is always taken as a tagged port for serving more than one
	 * VLANs across and also being applied with egress type stack mode for
	 * that VLAN tags would be appended after hardware special tag used as
	 * DSA tag.
	 */
	mt7530_rmw(priv, MT7530_VAWD2,
		   ETAG_CTRL_P_MASK(MT7530_CPU_PORT),
		   ETAG_CTRL_P(MT7530_CPU_PORT,
			       MT7530_VLAN_EGRESS_STACK));
}

static void
mt7530_hw_vlan_del(struct mt7530_priv *priv,
		   struct mt7530_hw_vlan_entry *entry)
{
	u8 new_members;
	u32 val;

	new_members = entry->old_members & ~BIT(entry->port);

	val = mt7530_read(priv, MT7530_VAWD1);
	if (!(val & VLAN_VALID)) {
		dev_err(priv->dev,
			"Cannot be deleted due to invalid entry\n");
		return;
	}

	/* If certain member apart from CPU port is still alive in the VLAN,
	 * the entry would be kept valid. Otherwise, the entry is got to be
	 * disabled.
	 */
	if (new_members && new_members != BIT(MT7530_CPU_PORT)) {
		val = IVL_MAC | VTAG_EN | PORT_MEM(new_members) |
		      VLAN_VALID;
		mt7530_write(priv, MT7530_VAWD1, val);
	} else {
		mt7530_write(priv, MT7530_VAWD1, 0);
		mt7530_write(priv, MT7530_VAWD2, 0);
	}
}

static void
mt7530_hw_vlan_update(struct mt7530_priv *priv, u16 vid,
		      struct mt7530_hw_vlan_entry *entry,
		      mt7530_vlan_op vlan_op)
{
	u32 val;

	/* Fetch entry */
	mt7530_vlan_cmd(priv, MT7530_VTCR_RD_VID, vid);

	val = mt7530_read(priv, MT7530_VAWD1);

	entry->old_members = (val >> PORT_MEM_SHFT) & PORT_MEM_MASK;

	/* Manipulate entry */
	vlan_op(priv, entry);

	/* Flush result to hardware */
	mt7530_vlan_cmd(priv, MT7530_VTCR_WR_VID, vid);
}

static void
mt7530_port_vlan_add(struct dsa_switch *ds, int port,
		     const struct switchdev_obj_port_vlan *vlan)
{
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	struct mt7530_hw_vlan_entry new_entry;
	struct mt7530_priv *priv = ds->priv;
	u16 vid;

	/* The port is kept as VLAN-unaware if bridge with vlan_filtering not
	 * being set.
	 */
	if (!dsa_port_is_vlan_filtering(&ds->ports[port]))
		return;

	mutex_lock(&priv->reg_mutex);

	for (vid = vlan->vid_begin; vid <= vlan->vid_end; ++vid) {
		mt7530_hw_vlan_entry_init(&new_entry, port, untagged);
		mt7530_hw_vlan_update(priv, vid, &new_entry,
				      mt7530_hw_vlan_add);
	}

	if (pvid) {
		mt7530_rmw(priv, MT7530_PPBV1_P(port), G0_PORT_VID_MASK,
			   G0_PORT_VID(vlan->vid_end));
		priv->ports[port].pvid = vlan->vid_end;
	}

	mutex_unlock(&priv->reg_mutex);
}

static int
mt7530_port_vlan_del(struct dsa_switch *ds, int port,
		     const struct switchdev_obj_port_vlan *vlan)
{
	struct mt7530_hw_vlan_entry target_entry;
	struct mt7530_priv *priv = ds->priv;
	u16 vid, pvid;

	/* The port is kept as VLAN-unaware if bridge with vlan_filtering not
	 * being set.
	 */
	if (!dsa_port_is_vlan_filtering(&ds->ports[port]))
		return 0;

	mutex_lock(&priv->reg_mutex);

	pvid = priv->ports[port].pvid;
	for (vid = vlan->vid_begin; vid <= vlan->vid_end; ++vid) {
		mt7530_hw_vlan_entry_init(&target_entry, port, 0);
		mt7530_hw_vlan_update(priv, vid, &target_entry,
				      mt7530_hw_vlan_del);

		/* PVID is being restored to the default whenever the PVID port
		 * is being removed from the VLAN.
		 */
		if (pvid == vid)
			pvid = G0_PORT_VID_DEF;
	}

	mt7530_rmw(priv, MT7530_PPBV1_P(port), G0_PORT_VID_MASK, pvid);
	priv->ports[port].pvid = pvid;

	mutex_unlock(&priv->reg_mutex);

	return 0;
}

static enum dsa_tag_protocol
mtk_get_tag_protocol(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	if (port != MT7530_CPU_PORT) {
		dev_warn(priv->dev,
			 "port not matched with tagging CPU port\n");
		return DSA_TAG_PROTO_NONE;
	} else {
		return DSA_TAG_PROTO_MTK;
	}
}

static int
mt7530_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	struct device_node *phy_node;
	struct device_node *mac_np;
	struct mt7530_dummy_poll p;
	phy_interface_t interface;
	struct device_node *dn;
	u32 id, val;
	int ret, i;

	/* The parent node of master netdev which holds the common system
	 * controller also is the container for two GMACs nodes representing
	 * as two netdev instances.
	 */
	dn = ds->ports[MT7530_CPU_PORT].master->dev.of_node->parent;

	if (priv->id == ID_MT7530) {
		regulator_set_voltage(priv->core_pwr, 1000000, 1000000);
		ret = regulator_enable(priv->core_pwr);
		if (ret < 0) {
			dev_err(priv->dev,
				"Failed to enable core power: %d\n", ret);
			return ret;
		}

		regulator_set_voltage(priv->io_pwr, 3300000, 3300000);
		ret = regulator_enable(priv->io_pwr);
		if (ret < 0) {
			dev_err(priv->dev, "Failed to enable io pwr: %d\n",
				ret);
			return ret;
		}
	}

	/* Reset whole chip through gpio pin or memory-mapped registers for
	 * different type of hardware
	 */
	if (priv->mcm) {
		reset_control_assert(priv->rstc);
		usleep_range(1000, 1100);
		reset_control_deassert(priv->rstc);
	} else {
		gpiod_set_value_cansleep(priv->reset, 0);
		usleep_range(1000, 1100);
		gpiod_set_value_cansleep(priv->reset, 1);
	}

	/* Waiting for MT7530 got to stable */
	INIT_MT7530_DUMMY_POLL(&p, priv, MT7530_HWTRAP);
	ret = readx_poll_timeout(_mt7530_read, &p, val, val != 0,
				 20, 1000000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	id = mt7530_read(priv, MT7530_CREV);
	id >>= CHIP_NAME_SHIFT;
	if (id != MT7530_ID) {
		dev_err(priv->dev, "chip %x can't be supported\n", id);
		return -ENODEV;
	}

	/* Reset the switch through internal reset */
	mt7530_write(priv, MT7530_SYS_CTRL,
		     SYS_CTRL_PHY_RST | SYS_CTRL_SW_RST |
		     SYS_CTRL_REG_RST);

	/* Enable Port 6 only; P5 as GMAC5 which currently is not supported */
	val = mt7530_read(priv, MT7530_MHWTRAP);
	val &= ~MHWTRAP_P6_DIS & ~MHWTRAP_PHY_ACCESS;
	val |= MHWTRAP_MANUAL;
	mt7530_write(priv, MT7530_MHWTRAP, val);

	priv->p6_interface = PHY_INTERFACE_MODE_NA;

	/* Enable and reset MIB counters */
	mt7530_mib_reset(ds);

	for (i = 0; i < MT7530_NUM_PORTS; i++) {
		/* Disable forwarding by default on all ports */
		mt7530_rmw(priv, MT7530_PCR_P(i), PCR_MATRIX_MASK,
			   PCR_MATRIX_CLR);

		if (dsa_is_cpu_port(ds, i))
			mt7530_cpu_port_enable(priv, i);
		else
			mt7530_port_disable(ds, i);

		/* Enable consistent egress tag */
		mt7530_rmw(priv, MT7530_PVC_P(i), PVC_EG_TAG_MASK,
			   PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT));
	}

	/* Setup port 5 */
	priv->p5_intf_sel = P5_DISABLED;
	interface = PHY_INTERFACE_MODE_NA;

	if (!dsa_is_unused_port(ds, 5)) {
		priv->p5_intf_sel = P5_INTF_SEL_GMAC5;
		interface = of_get_phy_mode(ds->ports[5].dn);
	} else {
		/* Scan the ethernet nodes. look for GMAC1, lookup used phy */
		for_each_child_of_node(dn, mac_np) {
			if (!of_device_is_compatible(mac_np,
						     "mediatek,eth-mac"))
				continue;

			ret = of_property_read_u32(mac_np, "reg", &id);
			if (ret < 0 || id != 1)
				continue;

			phy_node = of_parse_phandle(mac_np, "phy-handle", 0);
			if (!phy_node)
				continue;

			if (phy_node->parent == priv->dev->of_node->parent) {
				interface = of_get_phy_mode(mac_np);
				id = of_mdio_parse_addr(ds->dev, phy_node);
				if (id == 0)
					priv->p5_intf_sel = P5_INTF_SEL_PHY_P0;
				if (id == 4)
					priv->p5_intf_sel = P5_INTF_SEL_PHY_P4;
			}
			of_node_put(phy_node);
			break;
		}
	}

	mt7530_setup_port5(ds, interface);

	/* Flush the FDB table */
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_FLUSH, NULL);
	if (ret < 0)
		return ret;

	return 0;
}

static int mt7531_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;
	struct mt7530_dummy_poll p;
	u32 val, id;
	int ret, i;

	/* Reset whole chip through gpio pin or memory-mapped registers for
	 * different type of hardware
	 */
	if (priv->mcm) {
		reset_control_assert(priv->rstc);
		usleep_range(1000, 1100);
		reset_control_deassert(priv->rstc);
	} else {
		gpiod_set_value_cansleep(priv->reset, 0);
		usleep_range(1000, 1100);
		gpiod_set_value_cansleep(priv->reset, 1);
	}

	/* Waiting for MT7530 got to stable */
	INIT_MT7530_DUMMY_POLL(&p, priv, MT7530_HWTRAP);
	ret = readx_poll_timeout(_mt7530_read, &p, val, val != 0,
				 20, 1000000);
	if (ret < 0) {
		dev_err(priv->dev, "reset timeout\n");
		return ret;
	}

	id = mt7530_read(priv, MT7531_CREV);
	id >>= CHIP_NAME_SHIFT;

	if (id != MT7531_ID) {
		dev_err(priv->dev, "chip %x can't be supported\n", id);
		return -ENODEV;
	}

	/* Reset the switch through internal reset */
	mt7530_write(priv, MT7530_SYS_CTRL,
		     SYS_CTRL_PHY_RST | SYS_CTRL_SW_RST |
		     SYS_CTRL_REG_RST);

	priv->p6_interface = PHY_INTERFACE_MODE_NA;

	/* Enable PHY power, since phy_device has not yet been created
	 * provided for phy_[read,write]_mmd_indirect is called, we provide
	 * our own mt7531_ind_mmd_phy_[read,write] to complete this
	 * function.
	 */
	val = mt7531_ind_mmd_phy_read(priv, 0, PHY_DEV1F,
				      MT7531_PHY_DEV1F_REG_403);
	val |= MT7531_PHY_EN_BYPASS_MODE;
	val &= ~MT7531_PHY_POWER_OFF;
	mt7531_ind_mmd_phy_write(priv, 0, PHY_DEV1F,
				 MT7531_PHY_DEV1F_REG_403, val);

	/* Enable and reset MIB counters */
	mt7530_mib_reset(ds);

	mt7530_clear(priv, MT7530_MFC, UNU_FFP_MASK);

	for (i = 0; i < MT7530_NUM_PORTS; i++) {
		/* Disable forwarding by default on all ports */
		mt7530_rmw(priv, MT7530_PCR_P(i), PCR_MATRIX_MASK,
			   PCR_MATRIX_CLR);

		if (dsa_is_cpu_port(ds, i))
			mt7530_cpu_port_enable(priv, i);
		else
			mt7530_port_disable(ds, i);
	}

	/* Flush the FDB table */
	ret = mt7530_fdb_cmd(priv, MT7530_FDB_FLUSH, NULL);
	if (ret < 0)
		return ret;

	return 0;
}

static bool mt7530_phy_supported(struct dsa_switch *ds, int port,
				 const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	switch (port) {
	case 0: /* Internal phy */
	case 1:
	case 2:
	case 3:
	case 4:
		if (state->interface != PHY_INTERFACE_MODE_GMII)
			goto unsupported;
		break;
	case 5: /* 2nd cpu port with phy of port 0 or 4 / external phy */
		if (!phy_interface_mode_is_rgmii(state->interface) &&
		    state->interface != PHY_INTERFACE_MODE_MII &&
		    state->interface != PHY_INTERFACE_MODE_GMII)
			goto unsupported;
		break;
	case 6: /* 1st cpu port */
		if (state->interface != PHY_INTERFACE_MODE_RGMII &&
		    state->interface != PHY_INTERFACE_MODE_TRGMII)
			goto unsupported;
		break;
	default:
		dev_err(priv->dev, "%s: unsupported port: %i\n", __func__,
			port);
		goto unsupported;
	}

	return true;

unsupported:
	return false;
}

static bool mt7531_dual_sgmii_supported(struct mt7530_priv *priv)
{
	u32 val;

	val = mt7530_read(priv, MT7531_TOP_SIG_SR);
	return ((val & PAD_DUAL_SGMII_EN) != 0);
}

static bool mt7531_phy_supported(struct dsa_switch *ds, int port,
				 const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	switch (port) {
	case 0: /* Internal phy */
	case 1:
	case 2:
	case 3:
	case 4:
		if (state->interface != PHY_INTERFACE_MODE_GMII)
			goto unsupported;
		break;
	case 5: /* 2nd cpu port supports either rgmii or sgmii/8023z */
		if (!mt7531_dual_sgmii_supported(priv))
			return phy_interface_mode_is_rgmii(state->interface);
		/* fall through */
	case 6: /* 1st cpu port supports sgmii/8023z only */
		if (state->interface != PHY_INTERFACE_MODE_SGMII &&
		    !phy_interface_mode_is_8023z(state->interface))
			goto unsupported;
		break;
	default:
		dev_err(priv->dev, "%s: unsupported port: %i\n", __func__,
			port);
		goto unsupported;
	}

	return true;

unsupported:
	return false;
}

static bool mt753x_phy_supported(struct dsa_switch *ds, int port,
				 const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	return priv->info->phy_supported(ds, port, state);
}

static int
mt7530_pad_setup(struct dsa_switch *ds, const struct phylink_link_state *state)
{
	//struct mt7530_priv *priv = ds->priv;

	/* Setup TX circuit incluing relevant PAD and driving */
	mt7530_pad_clk_setup(ds, state->interface);

	return 0;
}

static int
mt7531_pad_setup(struct dsa_switch *ds, const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;
	u32 xtal, val;

	if (mt7531_dual_sgmii_supported(priv))
		return 0;

	xtal = mt7530_read(priv, MT7531_HWTRAP) & HWTRAP_XTAL_FSEL_MASK;

	switch (xtal) {
	case HWTRAP_XTAL_FSEL_25MHZ:
		/* Step 1 : Disable MT7531 COREPLL */
		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val &= ~EN_COREPLL;
		mt7530_write(priv, MT7531_PLLGP_EN, val);

		/* Step 2: switch to XTAL output */
		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val |= SW_CLKSW;
		mt7530_write(priv, MT7531_PLLGP_EN, val);

		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_EN;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		/* Step 3: disable PLLGP and enable program PLLGP */
		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val |= SW_PLLGP;
		mt7530_write(priv, MT7531_PLLGP_EN, val);

		/* Step 4: program COREPLL output frequency to 500MHz */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_POSDIV_M;
		val |= 2 << RG_COREPLL_POSDIV_S;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);
		usleep_range(25, 35);

		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_SDM_PCW_M;
		val |= 0x140000 << RG_COREPLL_SDM_PCW_S;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		/* Set feedback divide ratio update signal to high */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val |= RG_COREPLL_SDM_PCW_CHG;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);
		/* Wait for at least 16 XTAL clocks */
		usleep_range(10, 20);

		/* Step 5: set feedback divide ratio update signal to low */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_SDM_PCW_CHG;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		/* Enable 325M clock for SGMII */
		mt7530_write(priv, MT7531_ANA_PLLGP_CR5, 0xad0000);

		/* Enable 250SSC clock for RGMII */
		mt7530_write(priv, MT7531_ANA_PLLGP_CR2, 0x4f40000);

		/* Step 6: Enable MT7531 PLL */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val |= RG_COREPLL_EN;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val |= EN_COREPLL;
		mt7530_write(priv, MT7531_PLLGP_EN, val);
		usleep_range(25, 35);
		break;
	case HWTRAP_XTAL_FSEL_40MHZ:
		/* Step 1 : Disable MT7531 COREPLL */
		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val &= ~EN_COREPLL;
		mt7530_write(priv, MT7531_PLLGP_EN, val);

		/* Step 2: switch to XTAL output */
		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val |= SW_CLKSW;
		mt7530_write(priv, MT7531_PLLGP_EN, val);

		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_EN;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		/* Step 3: disable PLLGP and enable program PLLGP */
		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val |= SW_PLLGP;
		mt7530_write(priv, MT7531_PLLGP_EN, val);

		/* Step 4: program COREPLL output frequency to 500MHz */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_POSDIV_M;
		val |= 2 << RG_COREPLL_POSDIV_S;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);
		usleep_range(25, 35);

		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_SDM_PCW_M;
		val |= 0x190000 << RG_COREPLL_SDM_PCW_S;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		/* Set feedback divide ratio update signal to high */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val |= RG_COREPLL_SDM_PCW_CHG;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);
		/* Wait for at least 16 XTAL clocks */
		usleep_range(10, 20);

		/* Step 5: set feedback divide ratio update signal to low */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val &= ~RG_COREPLL_SDM_PCW_CHG;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		/* Enable 325M clock for SGMII */
		mt7530_write(priv, MT7531_ANA_PLLGP_CR5, 0xad0000);

		/* Enable 250SSC clock for RGMII */
		mt7530_write(priv, MT7531_ANA_PLLGP_CR2, 0x4f40000);

		/* Step 6: Enable MT7531 PLL */
		val = mt7530_read(priv, MT7531_PLLGP_CR0);
		val |= RG_COREPLL_EN;
		mt7530_write(priv, MT7531_PLLGP_CR0, val);

		val = mt7530_read(priv, MT7531_PLLGP_EN);
		val |= EN_COREPLL;
		mt7530_write(priv, MT7531_PLLGP_EN, val);
		usleep_range(25, 35);
		break;
	}

	return 0;
}

static int
mt753x_pad_setup(struct dsa_switch *ds, const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	return priv->info->pad_setup(ds, state);
}

static int
mt7530_mac_setup(struct dsa_switch *ds, int port, unsigned int mode,
		 const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	/* Only need to setup port5. */
	if (port != 5)
		return 0;

	mt7530_setup_port5(priv->ds, state->interface);

	return 0;
}

static int mt7531_rgmii_setup(struct mt7530_priv *priv, u32 port)
{
	u32 val;

	if (port != 5) {
		dev_err(priv->dev, "RGMII mode is not available for port %d\n",
			port);
		return -EINVAL;
	}

	val = mt7530_read(priv, MT7531_CLKGEN_CTRL);
	val |= GP_CLK_EN;
	val &= ~GP_MODE_MASK;
	val |= GP_MODE(MT7531_GP_MODE_RGMII);
	val |= TXCLK_NO_REVERSE;
	val |= RXCLK_NO_DELAY;
	val &= ~CLK_SKEW_IN_MASK;
	val |= CLK_SKEW_IN(MT7531_CLK_SKEW_NO_CHG);
	val &= ~CLK_SKEW_OUT_MASK;
	val |= CLK_SKEW_OUT(MT7531_CLK_SKEW_NO_CHG);
	mt7530_write(priv, MT7531_CLKGEN_CTRL, val);

	return 0;
}

static int mt7531_sgmii_setup_mode_force(struct mt7530_priv *priv, u32 port,
					 const struct phylink_link_state *state)
{
	u32 val;

	if (port != 5 && port != 6)
		return -EINVAL;

	val = mt7530_read(priv, MT7531_QPHY_PWR_STATE_CTRL(port));
	val |= MT7531_SGMII_PHYA_PWD;
	mt7530_write(priv, MT7531_QPHY_PWR_STATE_CTRL(port), val);

	val = mt7530_read(priv, MT7531_PHYA_CTRL_SIGNAL3(port));
	val &= ~MT7531_RG_TPHY_SPEED_MASK;
	if (state->interface == PHY_INTERFACE_MODE_2500BASEX)
		val |= MT7531_RG_TPHY_SPEED_3_125G;
	mt7530_write(priv, MT7531_PHYA_CTRL_SIGNAL3(port), val);

	val = mt7530_read(priv, MT7531_PCS_CONTROL_1(port));
	val &= ~MT7531_SGMII_AN_ENABLE;
	mt7530_write(priv, MT7531_PCS_CONTROL_1(port), val);

	val = mt7530_read(priv, MT7531_SGMII_MODE(port));
	val &= ~MT7531_SGMII_IF_MODE_MASK;

	switch (state->speed) {
	case SPEED_10:
		val |= MT7531_SGMII_FORCE_SPEED_10;
		break;
	case SPEED_100:
		val |= MT7531_SGMII_FORCE_SPEED_100;
		break;
	case SPEED_2500:
	case SPEED_1000:
		val |= MT7531_SGMII_FORCE_SPEED_1000;
		break;
	};

	val &= ~MT7531_SGMII_FORCE_DUPLEX;
	/* For sgmii force mode, 0 is full duplex and 1 is half duplex */
	if (state->duplex == DUPLEX_HALF)
		val |= MT7531_SGMII_FORCE_DUPLEX;

	mt7530_write(priv, MT7531_SGMII_MODE(port), val);

	val = mt7530_read(priv, MT7531_QPHY_PWR_STATE_CTRL(port));
	val &= ~MT7531_SGMII_PHYA_PWD;
	mt7530_write(priv, MT7531_QPHY_PWR_STATE_CTRL(port), val);

	return 0;
}

static int mt7531_sgmii_setup_mode_an(struct mt7530_priv *priv, int port,
				      const struct phylink_link_state *state)
{
	u32 val;

	if (port != 5 && port != 6)
		return -EINVAL;

	val = mt7530_read(priv, MT7531_QPHY_PWR_STATE_CTRL(port));
	val |= MT7531_SGMII_PHYA_PWD;
	mt7530_write(priv, MT7531_QPHY_PWR_STATE_CTRL(port), val);

	switch (state->speed) {
	case SPEED_10:
	case SPEED_100:
	case SPEED_1000:
		val = mt7530_read(priv, MT7531_PHYA_CTRL_SIGNAL3(port));
		val &= ~MT7531_RG_TPHY_SPEED_MASK;
		mt7530_write(priv, MT7531_PHYA_CTRL_SIGNAL3(port), val);
		break;
	default:
		dev_info(priv->dev, "invalid SGMII speed idx %d for port %d\n",
			 state->speed, port);

		return -EINVAL;
	}

	val = mt7530_read(priv, MT7531_SGMII_MODE(port));
	val |= MT7531_SGMII_REMOTE_FAULT_DIS;
	mt7530_write(priv, MT7531_SGMII_MODE(port), val);

	val = mt7530_read(priv, MT7531_PCS_CONTROL_1(port));
	val |= MT7531_SGMII_AN_RESTART;
	mt7530_write(priv, MT7531_PCS_CONTROL_1(port), val);

	val = mt7530_read(priv, MT7531_QPHY_PWR_STATE_CTRL(port));
	val &= ~MT7531_SGMII_PHYA_PWD;
	mt7530_write(priv, MT7531_QPHY_PWR_STATE_CTRL(port), val);

	return 0;
}

static int
mt7531_mac_setup(struct dsa_switch *ds, int port, unsigned int mode,
		 const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	if (port < 5 || port >= MT7530_NUM_PORTS) {
		dev_err(priv->dev, "port %d is not a MAC port\n", port);
		return -EINVAL;
	}

	switch (state->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		return mt7531_rgmii_setup(priv, port);
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		return mt7531_sgmii_setup_mode_force(priv, port, state);
	case PHY_INTERFACE_MODE_SGMII:
		return mt7531_sgmii_setup_mode_an(priv, port, state);
	default:
		return -EINVAL;
	}
}

static int mt753x_mac_setup(struct dsa_switch *ds, int port, unsigned int mode,
			    const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;

	return priv->info->mac_setup(ds, port, mode, state);
}

static void mt753x_phylink_mac_config(struct dsa_switch *ds, int port,
				      unsigned int mode,
				      const struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;
	u32 mcr_cur, mcr_new;

	if (!mt753x_phy_supported(ds, port, state))
		return;

	switch (port) {
	case 0: /* Internal phy */
	case 1:
	case 2:
	case 3:
	case 4:
		if (state->interface != PHY_INTERFACE_MODE_GMII)
			return;
		break;
	case 5: /* 2nd cpu port with phy of port 0 or 4 / external phy */
		if (priv->p5_interface == state->interface)
			break;

		if (mt753x_mac_setup(ds, port, mode, state) < 0)
			goto unsupported;

		priv->p5_interface = state->interface;

		break;
	case 6: /* 1st cpu port */
		if (priv->p6_interface == state->interface)
			break;

		mt753x_pad_setup(ds, state);

		if (mt753x_mac_setup(ds, port, mode, state) < 0)
			goto unsupported;

		priv->p6_interface = state->interface;
		break;
	default:
unsupported:
		return;
	}

	if (phylink_autoneg_inband(mode) &&
	    state->interface != PHY_INTERFACE_MODE_SGMII) {
		dev_err(ds->dev, "%s: in-band negotiation unsupported\n",
			__func__);
		return;
	}

	mcr_cur = mt7530_read(priv, MT7530_PMCR_P(port));
	mcr_new = mcr_cur;
	mcr_new &= ~(PMCR_FORCE_SPEED_1000 | PMCR_FORCE_SPEED_100 |
		     PMCR_FORCE_FDX | PMCR_TX_FC_EN | PMCR_RX_FC_EN);
	mcr_new |= PMCR_IFG_XMIT(1) | PMCR_MAC_MODE | PMCR_BACKOFF_EN |
		   PMCR_BACKPR_EN | PMCR_FORCE_MODE_ID(priv->id) |
		   PMCR_FORCE_LNK;

	/* Are we connected to external phy */
	if (port == 5 && dsa_is_user_port(ds, 5))
		mcr_new |= PMCR_EXT_PHY;

	switch (state->speed) {
	case SPEED_2500:
	case SPEED_1000:
		mcr_new |= PMCR_FORCE_SPEED_1000;
		break;
	case SPEED_100:
		mcr_new |= PMCR_FORCE_SPEED_100;
		break;
	}
	if (state->duplex == DUPLEX_FULL) {
		mcr_new |= PMCR_FORCE_FDX;
		if (state->pause & MLO_PAUSE_TX)
			mcr_new |= PMCR_TX_FC_EN;
		if (state->pause & MLO_PAUSE_RX)
			mcr_new |= PMCR_RX_FC_EN;
	}

	if (mcr_new != mcr_cur)
		mt7530_write(priv, MT7530_PMCR_P(port), mcr_new);
}

void mt7531_sgmii_restart_an(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;
	u32 val;

	val = mt7530_read(priv, MT7531_PCS_CONTROL_1(port));
	val |= MT7531_SGMII_AN_RESTART;
	mt7530_write(priv, MT7531_PCS_CONTROL_1(port), val);
}

static void
mt753x_phylink_mac_an_restart(struct dsa_switch *ds, int port)
{
	struct mt7530_priv *priv = ds->priv;

	if (!priv->info->port_an_restart)
		return;

	priv->info->port_an_restart(ds, port);
}

static void mt7530_phylink_mac_link_down(struct dsa_switch *ds, int port,
					 unsigned int mode,
					 phy_interface_t interface)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_port_set_status(priv, port, 0);
}

static void mt7530_phylink_mac_link_up(struct dsa_switch *ds, int port,
				       unsigned int mode,
				       phy_interface_t interface,
				       struct phy_device *phydev)
{
	struct mt7530_priv *priv = ds->priv;

	mt7530_port_set_status(priv, port, 1);
}

static void mt753x_phylink_validate(struct dsa_switch *ds, int port,
				    unsigned long *supported,
				    struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };

	if (state->interface != PHY_INTERFACE_MODE_NA &&
	    !mt753x_phy_supported(ds, port, state)) {
		linkmode_zero(supported);
		return;
	}

	phylink_set_port_modes(mask);
	phylink_set(mask, Autoneg);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_TRGMII:
		phylink_set(mask, 1000baseT_Full);
		break;
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		phylink_set(mask, 1000baseX_Full);
		phylink_set(mask, 2500baseX_Full);
		break;
	case PHY_INTERFACE_MODE_SGMII:
		phylink_set(mask, 1000baseT_Full);
		phylink_set(mask, 1000baseX_Full);
		/* fall through */
	default:
		phylink_set(mask, 10baseT_Half);
		phylink_set(mask, 10baseT_Full);
		phylink_set(mask, 100baseT_Half);
		phylink_set(mask, 100baseT_Full);

		if (state->interface != PHY_INTERFACE_MODE_MII) {
			/* This switch only supports 1G full-duplex. */
			phylink_set(mask, 1000baseT_Full);
			if (port == 5)
				phylink_set(mask, 1000baseX_Full);
		}
		break;
	}

	phylink_set(mask, Pause);
	phylink_set(mask, Asym_Pause);

	linkmode_and(supported, supported, mask);
	linkmode_and(state->advertising, state->advertising, mask);
}

static int
mt7530_phylink_mac_link_state(struct dsa_switch *ds, int port,
			      struct phylink_link_state *state)
{
	struct mt7530_priv *priv = ds->priv;
	u32 pmsr;

	if (port < 0 || port >= MT7530_NUM_PORTS)
		return -EINVAL;

	pmsr = mt7530_read(priv, MT7530_PMSR_P(port));

	state->link = (pmsr & PMSR_LINK);
	state->an_complete = state->link;
	state->duplex = !!(pmsr & PMSR_DPX);

	switch (pmsr & PMSR_SPEED_MASK) {
	case PMSR_SPEED_10:
		state->speed = SPEED_10;
		break;
	case PMSR_SPEED_100:
		state->speed = SPEED_100;
		break;
	case PMSR_SPEED_1000:
		state->speed = SPEED_1000;
		break;
	default:
		state->speed = SPEED_UNKNOWN;
		break;
	}

	state->pause &= ~(MLO_PAUSE_RX | MLO_PAUSE_TX);
	if (pmsr & PMSR_RX_FC)
		state->pause |= MLO_PAUSE_RX;
	if (pmsr & PMSR_TX_FC)
		state->pause |= MLO_PAUSE_TX;

	return 1;
}

static int
mt753x_setup(struct dsa_switch *ds)
{
	struct mt7530_priv *priv = ds->priv;

	return priv->info->setup(ds);
}

static int
mt753x_phy_read(struct dsa_switch *ds, int port, int regnum)
{
	struct mt7530_priv *priv = ds->priv;

	return priv->info->phy_read(ds, port, regnum);
}

static int
mt753x_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val)
{
	struct mt7530_priv *priv = ds->priv;

	return priv->info->phy_write(ds, port, regnum, val);
}

static const struct dsa_switch_ops mt7530_switch_ops = {
	.get_tag_protocol	= mtk_get_tag_protocol,
	.setup			= mt753x_setup,
	.get_strings		= mt7530_get_strings,
	.phy_read		= mt753x_phy_read,
	.phy_write		= mt753x_phy_write,
	.get_ethtool_stats	= mt7530_get_ethtool_stats,
	.get_sset_count		= mt7530_get_sset_count,
	.port_enable		= mt7530_port_enable,
	.port_disable		= mt7530_port_disable,
	.port_stp_state_set	= mt7530_stp_state_set,
	.port_bridge_join	= mt7530_port_bridge_join,
	.port_bridge_leave	= mt7530_port_bridge_leave,
	.port_fdb_add		= mt7530_port_fdb_add,
	.port_fdb_del		= mt7530_port_fdb_del,
	.port_fdb_dump		= mt7530_port_fdb_dump,
	.port_vlan_filtering	= mt7530_port_vlan_filtering,
	.port_vlan_prepare	= mt7530_port_vlan_prepare,
	.port_vlan_add		= mt7530_port_vlan_add,
	.port_vlan_del		= mt7530_port_vlan_del,
	.phylink_validate	= mt753x_phylink_validate,
	.phylink_mac_link_state	= mt7530_phylink_mac_link_state,
	.phylink_mac_config	= mt753x_phylink_mac_config,
	.phylink_mac_an_restart	= mt753x_phylink_mac_an_restart,
	.phylink_mac_link_down	= mt7530_phylink_mac_link_down,
	.phylink_mac_link_up	= mt7530_phylink_mac_link_up,
};

static const struct mt753x_info mt753x_table[] = {
	[ID_MT7621] = {
		.id = ID_MT7621,
		.setup = mt7530_setup,
		.phy_read = mt7530_phy_read,
		.phy_write = mt7530_phy_write,
		.phy_supported = mt7530_phy_supported,
		.pad_setup = mt7530_pad_setup,
		.mac_setup = mt7530_mac_setup,
	},
	[ID_MT7530] = {
		.id = ID_MT7530,
		.setup = mt7530_setup,
		.phy_read = mt7530_phy_read,
		.phy_write = mt7530_phy_write,
		.phy_supported = mt7530_phy_supported,
		.pad_setup = mt7530_pad_setup,
		.mac_setup = mt7530_mac_setup,
	},
	[ID_MT7531] = {
		.id = ID_MT7531,
		.setup = mt7531_setup,
		.phy_read = mt7531_ind_phy_read,
		.phy_write = mt7531_ind_phy_write,
		.phy_supported = mt7531_phy_supported,
		.pad_setup = mt7531_pad_setup,
		.mac_setup = mt7531_mac_setup,
		.port_an_restart = mt7531_sgmii_restart_an,
	},
};

static const struct of_device_id mt7530_of_match[] = {
	{ .compatible = "mediatek,mt7621", .data = &mt753x_table[ID_MT7621], },
	{ .compatible = "mediatek,mt7530", .data = &mt753x_table[ID_MT7530], },
	{ .compatible = "mediatek,mt7531", .data = &mt753x_table[ID_MT7531], },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mt7530_of_match);

static int
mt7530_probe(struct mdio_device *mdiodev)
{
	struct mt7530_priv *priv;
	struct device_node *dn;

	dn = mdiodev->dev.of_node;

	priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ds = dsa_switch_alloc(&mdiodev->dev, DSA_MAX_PORTS);
	if (!priv->ds)
		return -ENOMEM;

	/* Use medatek,mcm property to distinguish hardware type that would
	 * casues a little bit differences on power-on sequence.
	 */
	priv->mcm = of_property_read_bool(dn, "mediatek,mcm");
	if (priv->mcm) {
		dev_info(&mdiodev->dev, "MT7530 adapts as multi-chip module\n");

		priv->rstc = devm_reset_control_get(&mdiodev->dev, "mcm");
		if (IS_ERR(priv->rstc)) {
			dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
			return PTR_ERR(priv->rstc);
		}
	}

	/* Get the hardware identifier from the devicetree node.
	 * We will need it for some of the clock and regulator setup.
	 */
	priv->info = of_device_get_match_data(&mdiodev->dev);
	if (!priv->info)
		return -EINVAL;

	/* Sanity check if these required device operstaions are filled
	 * properly.
	 */
	if (!priv->info->setup || !priv->info->phy_read ||
	    !priv->info->phy_write || !priv->info->phy_supported ||
	    !priv->info->pad_setup || !priv->info->mac_setup)
		return -EINVAL;

	priv->id = priv->info->id;

	if (priv->id == ID_MT7530) {
		priv->core_pwr = devm_regulator_get(&mdiodev->dev, "core");
		if (IS_ERR(priv->core_pwr))
			return PTR_ERR(priv->core_pwr);

		priv->io_pwr = devm_regulator_get(&mdiodev->dev, "io");
		if (IS_ERR(priv->io_pwr))
			return PTR_ERR(priv->io_pwr);
	}

	/* Not MCM that indicates switch works as the remote standalone
	 * integrated circuit so the GPIO pin would be used to complete
	 * the reset, otherwise memory-mapped register accessing used
	 * through syscon provides in the case of MCM.
	 */
	if (!priv->mcm) {
		priv->reset = devm_gpiod_get_optional(&mdiodev->dev, "reset",
						      GPIOD_OUT_LOW);
		if (IS_ERR(priv->reset)) {
			dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
			return PTR_ERR(priv->reset);
		}
	}

	priv->bus = mdiodev->bus;
	priv->dev = &mdiodev->dev;
	priv->ds->priv = priv;
	priv->ds->ops = &mt7530_switch_ops;
	mutex_init(&priv->reg_mutex);
	dev_set_drvdata(&mdiodev->dev, priv);

	return dsa_register_switch(priv->ds);
}

static void
mt7530_remove(struct mdio_device *mdiodev)
{
	struct mt7530_priv *priv = dev_get_drvdata(&mdiodev->dev);
	int ret = 0;

	ret = regulator_disable(priv->core_pwr);
	if (ret < 0)
		dev_err(priv->dev,
			"Failed to disable core power: %d\n", ret);

	ret = regulator_disable(priv->io_pwr);
	if (ret < 0)
		dev_err(priv->dev, "Failed to disable io pwr: %d\n",
			ret);

	dsa_unregister_switch(priv->ds);
	mutex_destroy(&priv->reg_mutex);
}

static struct mdio_driver mt7530_mdio_driver = {
	.probe  = mt7530_probe,
	.remove = mt7530_remove,
	.mdiodrv.driver = {
		.name = "mt7530",
		.of_match_table = mt7530_of_match,
	},
};

mdio_module_driver(mt7530_mdio_driver);

MODULE_AUTHOR("Sean Wang <sean.wang@mediatek.com>");
MODULE_DESCRIPTION("Driver for Mediatek MT7530 Switch");
MODULE_LICENSE("GPL");
