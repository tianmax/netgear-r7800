/*
 * aq_phy.c: AQ105 Phy driver
 *
 * Copyright (c) 2015 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/if_ether.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/debugfs.h>
#include <linux/aq_phy.h>

/* Driver private data structure */
struct aq_priv {
	struct phy_device *phydev;	/* Pointer to PHY device */
	struct device *dev;		/* Pointer to struct device */
	struct dentry *aq_top_dentry;	/* Top dentry for AQ105 PHY Driver */
	struct dentry *aq_write_dentry;	/* write-reg file dentry for
					   AQ105 PHY Driver */
	struct dentry *aq_read_dentry;	/* read-reg file dentry for
					   AQ105 PHY Driver */
	struct dentry *aq_stats_dentry;	/* Statistics file dentry for
					   AQ105 PHY Driver */
	struct aq_stats stats;		/* AQ PHY stats block */
	uint32_t phy_addr;		/* AQ105 PHY Address */
	uint32_t reg_addr;		/* Previous register address */
	uint16_t reg_val;		/* Hold the value of the
					   previous register read */
};

/* Check for a valid range of PHY register */
static bool aq_phy_check_valid_reg(unsigned int reg_addr)
{
	bool ret = false;
	uint8_t mmd;

	if (reg_addr < 0x10000)
		return false;

	if ((reg_addr & 0xffff) > 0xfc02)
		return false;

	mmd = (reg_addr & 0x1f0000) >> 16;

	switch (mmd) {
	case MDIO_MMD_PMAPMD:
	case MDIO_MMD_PCS:
	case MDIO_MMD_PHYXS:
	case MDIO_MMD_AN:
	case MDIO_MMD_C22EXT:
	case MDIO_MMD_VEND1:
		ret = true;
		break;
	default:
		ret = false;
	}

	return ret;
}

/* Read a statistics register address */
static void aq_phy_read_stats_regs(struct phy_device *phydev,
				struct aq_stats_reg *st_reg)
{
	uint16_t lsw, msw;
	msw =  phy_read(phydev, MII_ADDR_C45 | st_reg->regaddr_msw);
	lsw =  phy_read(phydev, MII_ADDR_C45 | st_reg->regaddr_lsw);
	st_reg->regval = ((msw << 16) | lsw);
}

/* A debug-fs file read op to print the AQ PHY stats */
static ssize_t aq_phy_read_stats(struct file *fp, char __user *ubuf,
				 size_t sz, loff_t *ppos)
{
	struct aq_priv *priv = (struct aq_priv *)fp->private_data;
	struct aq_stats *st;
	int bytes_read;
	int size_wr;
	int size_al = AQ_PHY_STATS_MAX_STR_LENGTH * AQ_PHY_STATS_MAX_OUTPUT_LINE;
	uint16_t reg_value;
	uint32_t reg_addr;
	char *lbuf;

	if (!priv)
		return -EFAULT;

	/* Check if PHY is out of reset and FW has loaded successfully */
	reg_addr = MII_ADDR_C45 | MDIO_MMD_PMAPMD << 16 |
			AQ_PHY_PMA_STANDARD_CTRL_1_REG;

	reg_value = phy_read(priv->phydev, reg_addr);
	if (reg_value & AQ_PHY_PMA_STANDARD_CTRL_1_MASK) {
		pr_err("aq105 PHY is not ready\n");
		return -EIO;
	}

	lbuf = kzalloc(size_al, GFP_KERNEL);
	if (unlikely(lbuf == NULL)) {
		dev_dbg(priv->dev, "%s: Could not allocate memory "
				   "for local statistics buffer", __func__);
		return -EIO;
	}

	st = &priv->stats;

	size_wr = scnprintf(lbuf, size_al, "Link Status: %u\n",
						priv->phydev->link);

	size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
			"Link Speed: %u\n", priv->phydev->speed);

	aq_phy_read_stats_regs(priv->phydev, &st->line_tx_good);
	size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
		"Line Side TX Good: %u\n", st->line_tx_good.regval);

	aq_phy_read_stats_regs(priv->phydev, &st->line_tx_bad);
	size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
		"Line Side TX Bad: %u\n", st->line_tx_bad.regval);

	aq_phy_read_stats_regs(priv->phydev, &st->line_rx_good);
	size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
		"Line Side RX Good: %u\n", st->line_rx_good.regval);

	aq_phy_read_stats_regs(priv->phydev, &st->line_rx_bad);
	size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
		"Line Side RX Bad: %u\n", st->line_rx_bad.regval);

	if ((priv->phydev->speed == SPEED_1000) ||
		(priv->phydev->speed == SPEED_100)) {
		aq_phy_read_stats_regs(priv->phydev, &st->sys_tx_good);
		size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
			"System Side TX Good: %u\n", st->sys_tx_good.regval);

		aq_phy_read_stats_regs(priv->phydev, &st->sys_tx_bad);
		size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
			"System Side TX Bad: %u\n", st->sys_tx_bad.regval);

		aq_phy_read_stats_regs(priv->phydev, &st->sys_rx_good);
		size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
			"System Side RX Good: %u\n", st->sys_rx_good.regval);

		aq_phy_read_stats_regs(priv->phydev, &st->sys_rx_bad);
		size_wr += scnprintf(lbuf + size_wr, size_al - size_wr,
			"System Side RX Bad: %u\n", st->sys_rx_bad.regval);
	}

	bytes_read = simple_read_from_buffer(ubuf, sz, ppos,
						lbuf, strlen(lbuf));
	kfree(lbuf);
	return bytes_read;
}

/* read from debug-fs file */
static ssize_t aq_phy_read_reg_get(struct file *fp, char __user *ubuf,
				   size_t sz, loff_t *ppos)
{
	struct aq_priv *priv = (struct aq_priv *)fp->private_data;
	char lbuf[40];
	int bytes_read;

	if (!priv)
		return -EFAULT;

	snprintf(lbuf, sizeof(lbuf), "0x%x: 0x%x\n",
					priv->reg_addr,
					priv->reg_val);

	bytes_read = simple_read_from_buffer(ubuf, sz, ppos,
						lbuf, strlen(lbuf));

	dev_dbg(priv->dev, "%s: lbuf %s, bytes_read %d\n",
				__func__, lbuf, bytes_read);

	return bytes_read;
}

/* Write into the file and read back the PHY register */
static ssize_t aq_phy_read_reg_set(struct file *fp, const char __user *ubuf,
				   size_t sz, loff_t *ppos)
{
	struct aq_priv *priv = (struct aq_priv *)fp->private_data;
	char lbuf[32];
	size_t lbuf_size;
	char *curr_ptr = lbuf;
	unsigned long reg_addr = 0;
	uint16_t reg_value;
	uint32_t byte_cnt = 0;
	bool is_reabable = false;

	if (!priv)
		return -EFAULT;

	lbuf_size = min(sz, (sizeof(lbuf) - 1));

	dev_dbg(priv->dev, "PHY ADDR:DEVID 0x%x:0x%x, user buf size  %d\n",
		priv->phy_addr, priv->phydev->c45_ids.device_ids[1], lbuf_size);

	if (copy_from_user(lbuf, ubuf, lbuf_size)) {
		dev_dbg(priv->dev, "%s: failed in copy_from_user\n", __func__);
		return -EFAULT;
	}

	lbuf[lbuf_size] = 0;

	while (*curr_ptr == ' ') {
		curr_ptr++;
		byte_cnt++;
	}

	if (byte_cnt >= (lbuf_size - 1))
		return -EINVAL;

	if (strict_strtoul(curr_ptr, 16, &reg_addr)) {
		dev_dbg(priv->dev, "%s: Invalid register\n", __func__);
		return -EINVAL;
	}

	/* Read into PHY reg and store value in previous reg read */
	is_reabable = aq_phy_check_valid_reg(reg_addr);
	if (is_reabable) {
		priv->reg_addr = reg_addr;
		reg_addr = MII_ADDR_C45 | reg_addr;
		reg_value = phy_read(priv->phydev, reg_addr);
		priv->reg_val = reg_value;
		dev_dbg(priv->dev, "%s: reg = 0x%lx, value = 0x%x\n",
						__func__, reg_addr, reg_value);
	} else {
		return -EINVAL;
	}

	return lbuf_size;
}

/* Debug-fs function to write a PHY register */
static ssize_t aq_phy_write_reg_set(struct file *fp, const char __user *ubuf,
				    size_t sz, loff_t *ppos)
{
	struct aq_priv *priv = (struct aq_priv *)fp->private_data;
	char lbuf[32];
	size_t lbuf_size;
	char *curr_ptr = lbuf;
	unsigned long reg_addr = 0;
	unsigned long reg_value = 0;
	uint32_t check_16bit_boundary = 0xffff0000;
	uint32_t byte_cnt = 0;
	bool is_writeable = false;

	if (!priv)
		return -EFAULT;

	lbuf_size = min(sz, (sizeof(lbuf) - 1));

	dev_dbg(priv->dev, "PHY ADDR 0x%x, user buf size  %d\n",
					priv->phy_addr, lbuf_size);

	if (copy_from_user(lbuf, ubuf, lbuf_size)) {
		dev_dbg(priv->dev, "%s: failed in copy_from_user\n", __func__);
		return -EFAULT;
	}

	lbuf[lbuf_size] = 0;

	while (*curr_ptr == ' ') {
		curr_ptr++;
		byte_cnt++;
	}

	if (byte_cnt >= (lbuf_size - 1))
		return -EINVAL;

	reg_addr = simple_strtoul(curr_ptr, &curr_ptr, 16);

	while (*curr_ptr == ' ')
		curr_ptr++;

	if (byte_cnt >= (lbuf_size - 1))
		return -EINVAL;

	if (strict_strtoul(curr_ptr, 16, &reg_value)) {
		dev_dbg(priv->dev, "%s: Invalid reg value\n", __func__);
		return -EINVAL;
	}

	 /*
	  * Check for 16BIT register value boundary,
	  * if it cross 16 Bit return error
	  */
	if (check_16bit_boundary & reg_value) {
		dev_dbg(priv->dev, "%s: Invalid reg value\n", __func__);
		return -EINVAL;
	}

	 /* Check for a valid Range of register and write into Phy dev */
	is_writeable = aq_phy_check_valid_reg(reg_addr);
	if (is_writeable) {
		reg_addr = MII_ADDR_C45 | reg_addr;
		dev_dbg(priv->dev, "%s: Reg val 0x%lx, Data 0x%lx\n",
						__func__, reg_addr, reg_value);
		phy_write(priv->phydev, reg_addr, reg_value);
	} else {
		return -EINVAL;
	}

	return lbuf_size;
}

static const struct file_operations aq_phy_read_reg_ops = {
	.open = simple_open,
	.read = aq_phy_read_reg_get,
	.write = aq_phy_read_reg_set,
	.llseek = no_llseek,
};

static const struct file_operations aq_phy_write_reg_ops = {
	.open = simple_open,
	.write = aq_phy_write_reg_set,
	.llseek = no_llseek,
};

static const struct file_operations aq_phy_stats_ops = {
	.open = simple_open,
	.read = aq_phy_read_stats,
	.llseek = no_llseek,
};

/* Create debug-fs aq-phy dir and files */
static int aq_phy_init_debugfs_entries(struct aq_priv *priv)
{
	priv->aq_top_dentry = debugfs_create_dir("aq-phy", NULL);
	if (priv->aq_top_dentry == NULL) {
		dev_dbg(priv->dev, "Failed to create aq-phy "
						"directory in debugfs\n");
		return -1;
	}

	priv->aq_write_dentry = debugfs_create_file("write-reg", 0400,
						priv->aq_top_dentry,
						priv, &aq_phy_write_reg_ops);

	if (unlikely(priv->aq_write_dentry == NULL)) {
		dev_dbg(priv->dev, "Failed to create "
				"aq-phy/write-reg file in debugfs\n");
		debugfs_remove_recursive(priv->aq_top_dentry);
		return -1;
	}

	priv->aq_read_dentry = debugfs_create_file("read-reg", 0400,
						priv->aq_top_dentry,
						priv, &aq_phy_read_reg_ops);

	if (unlikely(priv->aq_read_dentry == NULL)) {
		dev_dbg(priv->dev, "Failed to create "
				"aq-phy/read-reg file in debugfs\n");
		debugfs_remove_recursive(priv->aq_top_dentry);
		return -1;
	}

	priv->aq_stats_dentry = debugfs_create_file("stats", 0400,
						priv->aq_top_dentry,
						priv, &aq_phy_stats_ops);

	if (unlikely(priv->aq_stats_dentry == NULL)) {
		dev_dbg(priv->dev, "Failed to create "
				"aq-phy/stats file in debugfs\n");
		debugfs_remove_recursive(priv->aq_top_dentry);
		return -1;
	}

	priv->reg_val = 0;
	priv->reg_addr = 0;
	return 0;
}

/* Initialize all AQ PHY stats register address */
static void aq_phy_init_stats(struct aq_stats *st)
{
	st->line_tx_good.regaddr_lsw = AQ_LINE_SIDE_TX_GOOD_REG_LSW;
	st->line_tx_good.regaddr_msw = AQ_LINE_SIDE_TX_GOOD_REG_MSW;
	st->line_tx_bad.regaddr_lsw = AQ_LINE_SIDE_TX_BAD_REG_LSW;
	st->line_tx_bad.regaddr_msw = AQ_LINE_SIDE_TX_BAD_REG_MSW;

	st->line_rx_good.regaddr_lsw = AQ_LINE_SIDE_RX_GOOD_REG_LSW;
	st->line_rx_good.regaddr_msw = AQ_LINE_SIDE_RX_GOOD_REG_MSW;
	st->line_rx_bad.regaddr_lsw = AQ_LINE_SIDE_RX_BAD_REG_LSW;
	st->line_rx_bad.regaddr_msw = AQ_LINE_SIDE_RX_BAD_REG_MSW;

	st->sys_tx_good.regaddr_lsw = AQ_SYS_SIDE_TX_GOOD_REG_LSW;
	st->sys_tx_good.regaddr_msw = AQ_SYS_SIDE_TX_GOOD_REG_MSW;
	st->sys_tx_bad.regaddr_lsw = AQ_SYS_SIDE_TX_BAD_REG_LSW;
	st->sys_tx_bad.regaddr_msw = AQ_SYS_SIDE_TX_BAD_REG_MSW;

	st->sys_rx_good.regaddr_lsw = AQ_SYS_SIDE_RX_GOOD_REG_LSW;
	st->sys_rx_good.regaddr_msw = AQ_SYS_SIDE_RX_GOOD_REG_MSW;
	st->sys_rx_bad.regaddr_lsw = AQ_SYS_SIDE_RX_BAD_REG_LSW;
	st->sys_rx_bad.regaddr_msw = AQ_SYS_SIDE_RX_BAD_REG_MSW;
}

/* Read the current status of the PHY i.e. Link, Speed, Duplex */
static int
aq_phy_read_status(struct phy_device *phydev)
{
	uint16_t reg_value = 0;
	uint32_t reg_addr;

	/*
	 * Read the PMD Standard Signal Detect register to check
	 * valid Ethernet signals are present on the wire.
	 */
	reg_addr = MII_ADDR_C45 | MDIO_MMD_PMAPMD << 16 |
			AQ_PHY_PMD_SIGNAL_DETECT_REG;

	reg_value = phy_read(phydev, reg_addr);
	if (!(reg_value & AQ_PHY_PMD_SIGNAL_DETECT_MASK)) {
		phydev->link = 0;
		return 0;
	}

	/* Read the line side current link status register. */
	reg_addr = MII_ADDR_C45 | MDIO_MMD_PMAPMD << 16 |
		AQ_PHY_PMA_RX_LINK_CURRENT_STATUS_REG;

	reg_value = phy_read(phydev, reg_addr);
	if (!(reg_value & AQ_PHY_PMA_RX_LINK_CURRENT_STATUS_MASK)) {
		phydev->link = 0;
		return 0;
	}

	/*
	 * Find the connect rate. The rate the PHY connected
	 * or attempting to connect.
	 */
	reg_addr = MII_ADDR_C45 | MDIO_MMD_AN << 16 | AQ_PHY_LINK_REG;
	reg_value = phy_read(phydev, reg_addr);

	switch ((reg_value >> 1) & 0x7) {
	case AQ_PHY_LINK_SPEED_2500:
		phydev->speed = SPEED_2500;
		phydev->advertising = ADVERTISED_2500baseX_Full;
		break;
	case AQ_PHY_LINK_SPEED_1000:
		phydev->speed = SPEED_1000;
		phydev->advertising = ADVERTISED_1000baseT_Full;
		break;
	case AQ_PHY_LINK_SPEED_100:
		phydev->speed = SPEED_100;
		if (reg_value & AQ_PHY_LINK_DUPLEX_MASK)
			phydev->advertising = ADVERTISED_100baseT_Full;
		else
			phydev->advertising = ADVERTISED_100baseT_Half;
		break;
	case AQ_PHY_LINK_SPEED_5000:
	case AQ_PHY_LINK_SPEED_10000:
	default:
		dev_dbg(&phydev->dev, "%s: unknown speed\n", __func__);
		phydev->speed = SPEED_UNKNOWN;
		phydev->advertising = SPEED_UNKNOWN;
	}

	phydev->link = 1;
	phydev->duplex = reg_value & AQ_PHY_LINK_DUPLEX_MASK;
	phydev->adjust_link(phydev->attached_dev);
	return 0;
}

/* Function for configuration of auto-negotiation */
static int
aq_phy_config_aneg(struct phy_device *phydev)
{
	return 0;
}

/* Initialize the Speed, Link and Duplex */
static int
aq_phy_config_init(struct phy_device *phydev)
{
	phydev->speed = SPEED_UNKNOWN;
	phydev->link = 0;
	phydev->duplex = 1;
	phydev->autoneg = 1;
	phydev->supported =	SUPPORTED_100baseT_Half |
				SUPPORTED_100baseT_Full |
				SUPPORTED_1000baseT_Full |
				SUPPORTED_2500baseX_Full;

	phydev->advertising =	ADVERTISED_100baseT_Half |
				ADVERTISED_100baseT_Full |
				ADVERTISED_1000baseT_Full |
				ADVERTISED_2500baseX_Full;
	return 0;
}

/* PHY driver probe function */
static int
aq_phy_probe(struct phy_device *phydev)
{
	return 0;
}

/* PHY driver remove function */
static void
aq_phy_remove(struct phy_device *pdev)
{
	return;
}

/* Match the PHY device ID */
static int aq_phy_match_phy_device(struct phy_device *phydev)
{
	int found = 0;

	if (((phydev->c45_ids.device_ids[0] & AQ_DEVICE_ID_MASK)
			== AQ_DEVICE_ID) ||
	    ((phydev->c45_ids.device_ids[1] & AQ_DEVICE_ID_MASK)
			== AQ_DEVICE_ID) ||
	    ((phydev->c45_ids.device_ids[2] & AQ_DEVICE_ID_MASK)
			== AQ_DEVICE_ID) ||
	    ((phydev->c45_ids.device_ids[3] & AQ_DEVICE_ID_MASK)
			== AQ_DEVICE_ID)) {

		found = 1;
	}

	return found;
}

static struct phy_driver aq_phy_driver = {
	.phy_id		= AQ_DEVICE_ID,
	.name		= "AQ105",
	.phy_id_mask	= AQ_DEVICE_ID_MASK,
	.features	= PHY_BASIC_FEATURES,
	.probe		= aq_phy_probe,
	.remove		= aq_phy_remove,
	.config_init	= &aq_phy_config_init,
	.config_aneg	= &aq_phy_config_aneg,
	.read_status	= &aq_phy_read_status,
	.match_phy_device = aq_phy_match_phy_device,
	.driver		= { .owner = THIS_MODULE },
};

/* Platform driver probe function */
static int __devinit aq_driver_probe(struct platform_device *pdev)
{
	struct aq_phy_platform_data *aq_pdata;
	struct aq_priv *priv;
	struct mii_bus *miibus;
	struct phy_device *phydev;
	struct device *dev;
	uint8_t busid[MII_BUS_ID_SIZE];
	uint8_t phy_id[MII_BUS_ID_SIZE + 3];
	int ret;

	dev_dbg(&pdev->dev, "aq_driver_probe\n");

	aq_pdata = (struct aq_phy_platform_data *)pdev->dev.platform_data;
	if (!aq_pdata)
		return -EIO;

	priv = vzalloc(sizeof(struct aq_priv));
	if (priv == NULL) {
		dev_dbg(&pdev->dev, "%s: Could not allocate "
			"private memory for driver\n", __func__);
		return -EIO;
	}

	/* Register the AQ_PHY PHY Driver */
	ret = phy_driver_register(&aq_phy_driver);
	if (ret) {
		dev_dbg(&pdev->dev, "PHY driver register fail\n");
		vfree(priv);
		return -EIO;
	}

	/* Get MII BUS pointer */
	snprintf(busid, MII_BUS_ID_SIZE, "%s.%d",
			aq_pdata->mdio_bus_name,
			aq_pdata->mdio_bus_id);

	dev = bus_find_device_by_name(&platform_bus_type, NULL,	busid);
	if (!dev) {
		dev_dbg(&pdev->dev, "mdio bus '%s' get FAIL\n", busid);
		phy_driver_unregister(&aq_phy_driver);
		vfree(priv);
		return -EIO;
	}

	miibus = dev_get_drvdata(dev);
	if (!miibus) {
		dev_dbg(&pdev->dev, " mdio bus '%s' get FAIL\n", busid);
		phy_driver_unregister(&aq_phy_driver);
		vfree(priv);
		return -EIO;
	}

	dev_dbg(&pdev->dev, "mdio bus '%s' OK.\n", miibus->id);

	/* Find the PHY Device and Attach with PHY Driver */
	phydev = get_phy_device(miibus, aq_pdata->phy_addr, true);
	if (IS_ERR(phydev) || phydev == NULL) {
		dev_dbg(&pdev->dev, " No phy dev at address 0x%x.\n",
						aq_pdata->phy_addr);

		phy_driver_unregister(&aq_phy_driver);
		vfree(priv);
		return -EIO;
	}

	ret = phy_device_register(phydev);
	if (ret) {
		phy_device_free(phydev);
		phy_driver_unregister(&aq_phy_driver);
		dev_dbg(&pdev->dev, "PHY device register fail.\n");
		vfree(priv);
		return -EIO;
	}

	/* create a phyid using MDIO bus id and MDIO bus address of phy */
	snprintf(phy_id, MII_BUS_ID_SIZE + 3,
				PHY_ID_FMT,
				miibus->id,
				aq_pdata->phy_addr);

	priv->phydev = phydev;
	priv->phy_addr = aq_pdata->phy_addr;

	/*
	 * Search the list of PHY devices on the mdio bus for the
	 * PHY with the requested name
	 */
	dev = bus_find_device_by_name(&mdio_bus_type, NULL, phy_id);
	if (!dev) {
		dev_dbg(&pdev->dev, "PHY %s not found\n", phy_id);
		phy_driver_unregister(&aq_phy_driver);
		device_unregister(&phydev->dev);
		miibus->phy_map[priv->phy_addr] = NULL;
		vfree(priv);
		return -ENODEV;
	}

	priv->dev = dev;

	/* Initialize the debug-fs entries */
	ret = aq_phy_init_debugfs_entries(priv);
	if (ret < 0) {
		phy_driver_unregister(&aq_phy_driver);
		device_unregister(&phydev->dev);
		miibus->phy_map[priv->phy_addr] = NULL;
		vfree(priv);
		return ret;
	}

	/* Initialize AQ Phy Stats Registers addresses */
	aq_phy_init_stats(&priv->stats);

	phydev->dev.platform_data = (void *)priv;

	pr_notice("AQ PHY Device registered\n");
	return 0;
}

/* Unregister the PHY device and PHY driver. */
static int __devexit aq_driver_remove(struct platform_device *pdev)
{
	struct aq_phy_platform_data *aq_pdata;
	struct aq_priv *priv;
	struct mii_bus *miibus;
	struct phy_device *phydev;
	struct device *dev;
	uint8_t busid[MII_BUS_ID_SIZE];
	uint8_t phy_id[MII_BUS_ID_SIZE + 3];

	aq_pdata = (struct aq_phy_platform_data *)pdev->dev.platform_data;
	if (!aq_pdata)
		return -EIO;

	/* Get MII BUS pointer */
	snprintf(busid, MII_BUS_ID_SIZE, "%s.%d",
			aq_pdata->mdio_bus_name,
			aq_pdata->mdio_bus_id);

	dev = bus_find_device_by_name(&platform_bus_type, NULL,	busid);
	if (!dev)
		return -EIO;

	miibus = dev_get_drvdata(dev);
	if (!miibus)
		return -EIO;

	/* create a phyid using MDIO bus id and MDIO bus address of phy */
	snprintf(phy_id, MII_BUS_ID_SIZE + 3,
				PHY_ID_FMT,
				miibus->id,
				aq_pdata->phy_addr);

	dev = bus_find_device_by_name(&mdio_bus_type, NULL, phy_id);
	if (!dev)
		return -ENODEV;

	phydev = to_phy_device(dev);
	if (!phydev)
		return -ENODEV;

	priv = (struct aq_priv *)phydev->dev.platform_data;
	if (!priv)
		return -EIO;

	phy_driver_unregister(&aq_phy_driver);
	device_unregister(&phydev->dev);
	miibus->phy_map[priv->phy_addr] = NULL;

	/* Remove debugfs tree */
	if (likely(priv->aq_top_dentry != NULL))
		debugfs_remove_recursive(priv->aq_top_dentry);

	/* Free the driver private data */
	vfree(priv);

	dev_dbg(&pdev->dev, "%s: Unregistered AQ PHY device\n", __func__);
	return 0;
}

static struct platform_driver aq_driver = {
	.probe = aq_driver_probe,
	.remove = __devexit_p(aq_driver_remove),
	.driver		= {
		.name	= "aq-phy",
		.owner	= THIS_MODULE,
	},
};

/* Platform driver module init function */
static int __init aq_driver_init(void)
{
	int ret;
	ret = platform_driver_register(&aq_driver);
	if (ret < 0)
		pr_debug("platform_driver_register fail.\n");

	return ret;
}
module_init(aq_driver_init);

/* Platform driver module exit function */
static void __exit aq_driver_exit(void)
{
	platform_driver_unregister(&aq_driver);
}
module_exit(aq_driver_exit);

MODULE_LICENSE("Dual BSD/GPL");
