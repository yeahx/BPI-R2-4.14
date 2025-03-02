// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 */
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/etherdevice.h>
#include "mt76.h"

static int
mt76_get_of_file(struct mt76_dev *dev, int len)
{
	char path[64]="";
	struct file *fp;
	loff_t pos=0;
	int ret;

	ret = snprintf(path,sizeof(path),"/lib/firmware/mediatek/%s_rf.bin",dev->dev->driver->name);
	if(ret < 0)
		return -EINVAL;
	dev_info(dev->dev,"Load firmware : %s\n",path);
	fp = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(fp)) {
		dev_info(dev->dev,"Load firmware Failed : %s\n",path);
		return -ENOENT;
	}
	ret = kernel_read(fp, dev->eeprom.data, len, &pos);
	if(ret < len){
		dev_info(dev->dev,"Load firmware ERR, count %d byte (%d)\n",ret,len);
		ret = -ENOENT;
	}else{
		dev_info(dev->dev,"Load firmware OK, count %d byte\n",ret);
		ret = 0;
	}
	filp_close(fp, 0);
	return ret;
}

static int
mt76_get_of_eeprom(struct mt76_dev *dev, int len)
{
#if defined(CONFIG_OF) && defined(CONFIG_MTD)
	struct device_node *np = dev->dev->of_node;
	struct mtd_info *mtd;
	const __be32 *list;
	const char *part;
	phandle phandle;
	int offset = 0;
	int size;
	size_t retlen;
	int ret;

	if (!np)
		return -ENOENT;

	list = of_get_property(np, "mediatek,mtd-eeprom", &size);
	if (!list)
		return -ENOENT;

	phandle = be32_to_cpup(list++);
	if (!phandle)
		return -ENOENT;

	np = of_find_node_by_phandle(phandle);
	if (!np)
		return -EINVAL;

	part = of_get_property(np, "label", NULL);
	if (!part)
		part = np->name;

	mtd = get_mtd_device_nm(part);
	if (IS_ERR(mtd)) {
		ret =  PTR_ERR(mtd);
		goto out_put_node;
	}

	if (size <= sizeof(*list)) {
		ret = -EINVAL;
		goto out_put_node;
	}

	offset = be32_to_cpup(list);
	ret = mtd_read(mtd, offset, len, &retlen, dev->eeprom.data);
	put_mtd_device(mtd);
	if (ret)
		goto out_put_node;

	if (retlen < len) {
		ret = -EINVAL;
		goto out_put_node;
	}

out_put_node:
	of_node_put(np);
	return ret;
#else
	return -ENOENT;
#endif
}

void
mt76_eeprom_override(struct mt76_dev *dev)
{
#ifdef CONFIG_OF
	struct device_node *np = dev->dev->of_node;
	const u8 *mac;

	if (!np)
		return;

	mac = of_get_mac_address(np);
	if (!IS_ERR_OR_NULL(mac))
		memcpy(dev->macaddr, mac, ETH_ALEN);
#endif

	if (!is_valid_ether_addr(dev->macaddr)) {
		eth_random_addr(dev->macaddr);
		dev_info(dev->dev,
			 "Invalid MAC address, using random address %pM\n",
			 dev->macaddr);
	}
}
EXPORT_SYMBOL_GPL(mt76_eeprom_override);

int
mt76_eeprom_init(struct mt76_dev *dev, int len)
{
	dev->eeprom.size = len;
	dev->eeprom.data = devm_kzalloc(dev->dev, len, GFP_KERNEL);
	if (!dev->eeprom.data)
		return -ENOMEM;

	return (!mt76_get_of_file(dev, len)) || (!mt76_get_of_eeprom(dev, len));//mtd priority mt76
}
EXPORT_SYMBOL_GPL(mt76_eeprom_init);
