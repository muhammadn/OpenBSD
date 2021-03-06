/*	$OpenBSD: virtio.c,v 1.11 2016/04/04 17:13:54 stefan Exp $	*/

/*
 * Copyright (c) 2015 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
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

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>
#include <dev/pci/virtioreg.h>
#include <dev/pci/vioblkreg.h>
#include <machine/vmmvar.h>
#include <machine/param.h>
#include "pci.h"
#include "vmd.h"
#include "virtio.h"
#include "loadfile.h"

extern char *__progname;

struct viornd_dev viornd;
struct vioblk_dev *vioblk;
struct vionet_dev *vionet;

int nr_vionet;

#define MAXPHYS	(64 * 1024)	/* max raw I/O transfer size */

#define VIRTIO_NET_F_MAC	(1<<5)


const char *
vioblk_cmd_name(uint32_t type)
{
	switch (type) {
	case VIRTIO_BLK_T_IN: return "read";
	case VIRTIO_BLK_T_OUT: return "write";
	case VIRTIO_BLK_T_SCSI_CMD: return "scsi read";
	case VIRTIO_BLK_T_SCSI_CMD_OUT: return "scsi write";
	case VIRTIO_BLK_T_FLUSH: return "flush";
	case VIRTIO_BLK_T_FLUSH_OUT: return "flush out";
	case VIRTIO_BLK_T_GET_ID: return "get id";
	default: return "unknown";
	}
}

static void
dump_descriptor_chain(struct vring_desc *desc, int16_t dxx)
{
	log_debug("descriptor chain @ %d", dxx);
	do {
		log_debug("desc @%d addr/len/flags/next = 0x%llx / 0x%x "
		    "/ 0x%x / 0x%x",
		    dxx,
		    desc[dxx].addr,
		    desc[dxx].len,
		    desc[dxx].flags,
		    desc[dxx].next);
		dxx = desc[dxx].next;
	} while (desc[dxx].flags & VRING_DESC_F_NEXT);

	log_debug("desc @%d addr/len/flags/next = 0x%llx / 0x%x / 0x%x "
	    "/ 0x%x",
	    dxx,
	    desc[dxx].addr,
	    desc[dxx].len,
	    desc[dxx].flags,
	    desc[dxx].next);
}

static const char *
virtio_reg_name(uint8_t reg)
{
	switch (reg) {
	case VIRTIO_CONFIG_DEVICE_FEATURES: return "device feature";
	case VIRTIO_CONFIG_GUEST_FEATURES: return "guest feature";
	case VIRTIO_CONFIG_QUEUE_ADDRESS: return "queue address";
	case VIRTIO_CONFIG_QUEUE_SIZE: return "queue size";
	case VIRTIO_CONFIG_QUEUE_SELECT: return "queue select";
	case VIRTIO_CONFIG_QUEUE_NOTIFY: return "queue notify";
	case VIRTIO_CONFIG_DEVICE_STATUS: return "device status";
	case VIRTIO_CONFIG_ISR_STATUS: return "isr status";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI: return "device config 0";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4: return "device config 1";
	case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 8: return "device config 2";
	default: return "unknown";
	}
}

uint32_t
vring_size(uint32_t vq_size)
{
	uint32_t allocsize1, allocsize2;

	/* allocsize1: descriptor table + avail ring + pad */
	allocsize1 = VIRTQUEUE_ALIGN(sizeof(struct vring_desc) * vq_size
	    + sizeof(uint16_t) * (2 + vq_size));
	/* allocsize2: used ring + pad */
	allocsize2 = VIRTQUEUE_ALIGN(sizeof(uint16_t) * 2
	    + sizeof(struct vring_used_elem) * vq_size);

	return allocsize1 + allocsize2;
}

/* Update queue select */
void
viornd_update_qs(void)
{
	/* Invalid queue? */
	if (viornd.cfg.queue_select > 0)
		return;

	/* Update queue address/size based on queue select */
	viornd.cfg.queue_address = viornd.vq[viornd.cfg.queue_select].qa;
	viornd.cfg.queue_size = viornd.vq[viornd.cfg.queue_select].qs;
}

/* Update queue address */
void
viornd_update_qa(void)
{
	/* Invalid queue? */
	if (viornd.cfg.queue_select > 0)
		return;

	viornd.vq[viornd.cfg.queue_select].qa = viornd.cfg.queue_address;
}

int
viornd_notifyq(void)
{
	uint64_t q_gpa;
	uint32_t vr_sz;
	int ret;
	char *buf, *rnd_data;
	struct vring_desc *desc;
	struct vring_avail *avail;
	struct vring_used *used;

	ret = 0;

	/* Invalid queue? */
	if (viornd.cfg.queue_notify > 0)
		return (0);

	vr_sz = vring_size(VIORND_QUEUE_SIZE);
	q_gpa = viornd.vq[viornd.cfg.queue_notify].qa;
	q_gpa = q_gpa * VIRTIO_PAGE_SIZE;

	buf = malloc(vr_sz);
	if (buf == NULL) {
		log_warn("malloc error getting viornd ring");
		return (0);
	}

	memset(buf, 0, vr_sz);

	if (read_mem(q_gpa, buf, vr_sz)) {
		free(buf);
		return (0);
	}

	desc = (struct vring_desc *)(buf);
	avail = (struct vring_avail *)(buf +
	    viornd.vq[viornd.cfg.queue_notify].vq_availoffset);
	used = (struct vring_used *)(buf +
	    viornd.vq[viornd.cfg.queue_notify].vq_usedoffset);

	/* XXX sanity check len here */
	rnd_data = malloc(desc[avail->ring[avail->idx]].len);

	if (rnd_data != NULL) {
		arc4random_buf(rnd_data, desc[avail->ring[avail->idx]].len);
		if (write_mem(desc[avail->ring[avail->idx]].addr,
		    rnd_data, desc[avail->ring[avail->idx]].len)) {
			log_warnx("viornd: can't write random data @ "
			    "0x%llx",
			    desc[avail->ring[avail->idx]].addr);
		} else {
			/* ret == 1 -> interrupt needed */
			/* XXX check VIRTIO_F_NO_INTR */
			ret = 1;
			viornd.cfg.isr_status = 1;
			used->ring[used->idx].id = avail->ring[avail->idx];
			used->ring[used->idx].len =
			    desc[avail->ring[avail->idx]].len;
			used->idx++;

			if (write_mem(q_gpa, buf, vr_sz)) {
				log_warnx("viornd: error writing vio ring");
			}
		}
		free(rnd_data);
	}

	free(buf);

	return (ret);
}

int
virtio_rnd_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *unused)
{
	*intr = 0xFF;

	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			viornd.cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_ADDRESS:
			viornd.cfg.queue_address = *data;
			viornd_update_qa();
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			viornd.cfg.queue_select = *data;
			viornd_update_qs();
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			viornd.cfg.queue_notify = *data;
			if (viornd_notifyq())
				*intr = 1;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			viornd.cfg.device_status = *data;
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = viornd.cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = viornd.cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_ADDRESS:
			*data = viornd.cfg.queue_address;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			*data = viornd.cfg.queue_size;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = viornd.cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = viornd.cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			*data = viornd.cfg.device_status;
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = viornd.cfg.isr_status;
			break;
		}
	}
	return (0);
}

void
vioblk_update_qa(struct vioblk_dev *dev)
{
	/* Invalid queue? */
	if (dev->cfg.queue_select > 0)
		return;

	dev->vq[dev->cfg.queue_select].qa = dev->cfg.queue_address;
}

void
vioblk_update_qs(struct vioblk_dev *dev)
{
	/* Invalid queue? */
	if (dev->cfg.queue_select > 0)
		return;

	/* Update queue address/size based on queue select */
	dev->cfg.queue_address = dev->vq[dev->cfg.queue_select].qa;
	dev->cfg.queue_size = dev->vq[dev->cfg.queue_select].qs;
}

static char *
vioblk_do_read(struct vioblk_dev *dev, off_t sector, ssize_t sz)
{
	char *buf;

	buf = malloc(sz);
	if (buf == NULL) {
		log_warn("malloc errror vioblk read");
		return (NULL);
	}

	if (lseek(dev->fd, sector * VIRTIO_BLK_SECTOR_SIZE,
	    SEEK_SET) == -1) {
		log_warn("seek error in vioblk read");
		free(buf);
		return (NULL);
	}

	if (read(dev->fd, buf, sz) != sz) {
		log_warn("vioblk read error");
		free(buf);
		return (NULL);
	}

	return buf;
}

static int
vioblk_do_write(struct vioblk_dev *dev, off_t sector, char *buf, ssize_t sz)
{
	if (lseek(dev->fd, sector * VIRTIO_BLK_SECTOR_SIZE,
	    SEEK_SET) == -1) {
		log_warn("seek error in vioblk write");
		return (1);
	}

	if (write(dev->fd, buf, sz) != sz) {
		log_warn("vioblk write error");
		return (1);
	}

	return (0);
}

/*
 * XXX this function needs a cleanup block, lots of free(blah); return (0)
 *     in various cases, ds should be set to VIRTIO_BLK_S_IOERR, if we can
 * XXX cant trust ring data from VM, be extra cautious.
 */
int
vioblk_notifyq(struct vioblk_dev *dev)
{
	uint64_t q_gpa;
	uint32_t vr_sz; //, osz;
	uint16_t idx, cmd_desc_idx, secdata_desc_idx, ds_desc_idx; //, dxx;
	uint8_t ds;
	int ret;
	off_t secbias;
	char *vr, *secdata;
	struct vring_desc *desc, *cmd_desc, *secdata_desc, *ds_desc;
	struct vring_avail *avail;
	struct vring_used *used;
	struct virtio_blk_req_hdr cmd;

	ret = 0;

	/* Invalid queue? */
	if (dev->cfg.queue_notify > 0)
		return (0);

	vr_sz = vring_size(VIOBLK_QUEUE_SIZE);
	q_gpa = dev->vq[dev->cfg.queue_notify].qa;
	q_gpa = q_gpa * VIRTIO_PAGE_SIZE;

	vr = malloc(vr_sz);
	if (vr == NULL) {
		log_warn("malloc error getting vioblk ring");
		return (0);
	}

	memset(vr, 0, vr_sz);

	if (read_mem(q_gpa, vr, vr_sz)) {
		log_warnx("error reading gpa 0x%llx", q_gpa);
		free(vr);
		return (0);
	}

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr +
	    dev->vq[dev->cfg.queue_notify].vq_availoffset);
	used = (struct vring_used *)(vr +
	    dev->vq[dev->cfg.queue_notify].vq_usedoffset);


	idx = dev->vq[dev->cfg.queue_notify].last_avail & VIOBLK_QUEUE_MASK;

	if ((avail->idx & VIOBLK_QUEUE_MASK) == idx) {
		log_warnx("vioblk queue notify - nothing to do?");
		free(vr);
		return (0);
	}

	cmd_desc_idx = avail->ring[idx] & VIOBLK_QUEUE_MASK;
	cmd_desc = &desc[cmd_desc_idx];

	if ((cmd_desc->flags & VRING_DESC_F_NEXT) == 0) {
		log_warnx("unchained vioblk cmd descriptor received "
		    "(idx %d)", cmd_desc_idx);
		free(vr);
		return (0);
	}

	/* Read command from descriptor ring */
	if (read_mem(cmd_desc->addr, &cmd, cmd_desc->len)) {
		log_warnx("vioblk: command read_mem error @ 0x%llx",
		    cmd_desc->addr);
		free(vr);
		return (0);
	}

	switch (cmd.type) {
	case VIRTIO_BLK_T_IN:
		/* first descriptor */
		secdata_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
		secdata_desc = &desc[secdata_desc_idx];

		if ((secdata_desc->flags & VRING_DESC_F_NEXT) == 0) {
			log_warnx("unchained vioblk data descriptor "
			    "received (idx %d)", cmd_desc_idx);
			free(vr);
			return (0);
		}

		secbias = 0;
		do {
			/* read the data (use current data descriptor) */
			/*
			 * XXX waste to malloc secdata in vioblk_do_read
			 * and free it here over and over
			 */
			secdata = vioblk_do_read(dev, cmd.sector + secbias,
			    (ssize_t)secdata_desc->len);
			if (secdata == NULL) {
				log_warnx("vioblk: block read error, "
				    "sector %lld", cmd.sector);
				free(vr);
				return (0);
			}

			if (write_mem(secdata_desc->addr, secdata,
			    secdata_desc->len)) {
				log_warnx("can't write sector "
				    "data to gpa @ 0x%llx",
				    secdata_desc->addr);
				dump_descriptor_chain(desc, cmd_desc_idx);
				free(vr);
				free(secdata);
				return (0);
			}

			free(secdata);

			secbias += (secdata_desc->len / VIRTIO_BLK_SECTOR_SIZE);
			secdata_desc_idx = secdata_desc->next &
			    VIOBLK_QUEUE_MASK;
			secdata_desc = &desc[secdata_desc_idx];
		} while (secdata_desc->flags & VRING_DESC_F_NEXT);

		ds_desc_idx = secdata_desc_idx;
		ds_desc = secdata_desc;

		ds = VIRTIO_BLK_S_OK;
		if (write_mem(ds_desc->addr, &ds, ds_desc->len)) {
			log_warnx("can't write device status data @ "
			    "0x%llx", ds_desc->addr);
			dump_descriptor_chain(desc, cmd_desc_idx);
			free(vr);
			return (0);
		}


		ret = 1;
		dev->cfg.isr_status = 1;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].id = cmd_desc_idx;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].len = cmd_desc->len;
		used->idx++;

		dev->vq[dev->cfg.queue_notify].last_avail = avail->idx &
		    VIOBLK_QUEUE_MASK;

		if (write_mem(q_gpa, vr, vr_sz)) {
			log_warnx("vioblk: error writing vio ring");
		}
		break;
	case VIRTIO_BLK_T_OUT:
		secdata_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
		secdata_desc = &desc[secdata_desc_idx];

		if ((secdata_desc->flags & VRING_DESC_F_NEXT) == 0) {
			log_warnx("wr vioblk: unchained vioblk data "
			    "descriptor received (idx %d)", cmd_desc_idx);
			free(vr);
			return (0);
		}

		secdata = malloc(MAXPHYS);
		if (secdata == NULL) {
			log_warn("wr vioblk: malloc error, len %d",
			    secdata_desc->len);
			free(vr);
			return (0);
		}

		secbias = 0;
		do {
			if (read_mem(secdata_desc->addr, secdata,
			    secdata_desc->len)) {
				log_warnx("wr vioblk: can't read "
				    "sector data @ 0x%llx",
				    secdata_desc->addr);
				dump_descriptor_chain(desc, cmd_desc_idx);
				free(vr);
				free(secdata);
				return (0);
			}

			if (vioblk_do_write(dev, cmd.sector + secbias,
			    secdata, (ssize_t)secdata_desc->len)) {
				log_warnx("wr vioblk: disk write error");
				free(vr);
				free(secdata);
				return (0);
			}

			secbias += secdata_desc->len / VIRTIO_BLK_SECTOR_SIZE;

			secdata_desc_idx = secdata_desc->next &
			    VIOBLK_QUEUE_MASK;
			secdata_desc = &desc[secdata_desc_idx];
		} while (secdata_desc->flags & VRING_DESC_F_NEXT);

		free(secdata);

		ds_desc_idx = secdata_desc_idx;
		ds_desc = secdata_desc;

		ds = VIRTIO_BLK_S_OK;
		if (write_mem(ds_desc->addr, &ds, ds_desc->len)) {
			log_warnx("wr vioblk: can't write device status "
			    "data @ 0x%llx", ds_desc->addr);
			dump_descriptor_chain(desc, cmd_desc_idx);
			free(vr);
			return (0);
		}

		ret = 1;
		dev->cfg.isr_status = 1;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].id = cmd_desc_idx;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].len = cmd_desc->len;
		used->idx++;

		dev->vq[dev->cfg.queue_notify].last_avail = avail->idx &
		    VIOBLK_QUEUE_MASK;
		if (write_mem(q_gpa, vr, vr_sz))
			log_warnx("wr vioblk: error writing vio ring");
		break;
	case VIRTIO_BLK_T_FLUSH:
	case VIRTIO_BLK_T_FLUSH_OUT:
		ds_desc_idx = cmd_desc->next & VIOBLK_QUEUE_MASK;
		ds_desc = &desc[ds_desc_idx];

		ds = VIRTIO_BLK_S_OK;
		if (write_mem(ds_desc->addr, &ds, ds_desc->len)) {
			log_warnx("fl vioblk: can't write device status "
			    "data @ 0x%llx", ds_desc->addr);
			dump_descriptor_chain(desc, cmd_desc_idx);
			free(vr);
			return (0);
		}

		ret = 1;
		dev->cfg.isr_status = 1;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].id = cmd_desc_idx;
		used->ring[used->idx & VIOBLK_QUEUE_MASK].len = cmd_desc->len;
		used->idx++;

		dev->vq[dev->cfg.queue_notify].last_avail = avail->idx &
		    VIOBLK_QUEUE_MASK;
		if (write_mem(q_gpa, vr, vr_sz)) {
			log_warnx("fl vioblk: error writing vio ring");
		}
		break;
	}

	free(vr);

	return (ret);
}

int
virtio_blk_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *cookie)
{
	struct vioblk_dev *dev = (struct vioblk_dev *)cookie;

	*intr = 0xFF;

	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			dev->cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_ADDRESS:
			dev->cfg.queue_address = *data;
			vioblk_update_qa(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			dev->cfg.queue_select = *data;
			vioblk_update_qs(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			dev->cfg.queue_notify = *data;
			if (vioblk_notifyq(dev))
				*intr = 1;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			dev->cfg.device_status = *data;
			break;
		default:
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4:
			*data = (uint32_t)(dev->sz >> 32);
			break;
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI:
			*data = (uint32_t)(dev->sz);
			break;
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = dev->cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = dev->cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_ADDRESS:
			*data = dev->cfg.queue_address;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			*data = dev->cfg.queue_size;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = dev->cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = dev->cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			*data = dev->cfg.device_status;
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = dev->cfg.isr_status;
			break;
		}
	}
	return (0);
}

int
virtio_net_io(int dir, uint16_t reg, uint32_t *data, uint8_t *intr,
    void *cookie)
{
	struct vionet_dev *dev = (struct vionet_dev *)cookie;

	*intr = 0xFF;

	if (dir == 0) {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_FEATURES:
		case VIRTIO_CONFIG_QUEUE_SIZE:
		case VIRTIO_CONFIG_ISR_STATUS:
			log_warnx("%s: illegal write %x to %s",
			    __progname, *data, virtio_reg_name(reg));
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			dev->cfg.guest_feature = *data;
			break;
		case VIRTIO_CONFIG_QUEUE_ADDRESS:
			dev->cfg.queue_address = *data;
			vionet_update_qa(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			dev->cfg.queue_select = *data;
			vionet_update_qs(dev);
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			dev->cfg.queue_notify = *data;
			if (vionet_notifyq(dev))
				*intr = 1;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			dev->cfg.device_status = *data;
			break;
		default:
			break;
		}
	} else {
		switch (reg) {
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 1:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 2:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 3:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 4:
		case VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI + 5:
			*data = dev->mac[reg -
			    VIRTIO_CONFIG_DEVICE_CONFIG_NOMSI];
			break;
		case VIRTIO_CONFIG_DEVICE_FEATURES:
			*data = dev->cfg.device_feature;
			break;
		case VIRTIO_CONFIG_GUEST_FEATURES:
			*data = dev->cfg.guest_feature;
			break;
		case VIRTIO_CONFIG_QUEUE_ADDRESS:
			*data = dev->cfg.queue_address;
			break;
		case VIRTIO_CONFIG_QUEUE_SIZE:
			*data = dev->cfg.queue_size;
			break;
		case VIRTIO_CONFIG_QUEUE_SELECT:
			*data = dev->cfg.queue_select;
			break;
		case VIRTIO_CONFIG_QUEUE_NOTIFY:
			*data = dev->cfg.queue_notify;
			break;
		case VIRTIO_CONFIG_DEVICE_STATUS:
			*data = dev->cfg.device_status;
			break;
		case VIRTIO_CONFIG_ISR_STATUS:
			*data = dev->cfg.isr_status;
			break;
		}
	}
	return (0);
}

void
vionet_update_qa(struct vionet_dev *dev)
{
	/* Invalid queue? */
	if (dev->cfg.queue_select > 1)
		return;

	dev->vq[dev->cfg.queue_select].qa = dev->cfg.queue_address;
}

void
vionet_update_qs(struct vionet_dev *dev)
{
	/* Invalid queue? */
	if (dev->cfg.queue_select > 1)
		return;

	/* Update queue address/size based on queue select */
	dev->cfg.queue_address = dev->vq[dev->cfg.queue_select].qa;
	dev->cfg.queue_size = dev->vq[dev->cfg.queue_select].qs;
}

int
vionet_enq_rx(struct vionet_dev *dev, char *pkt, ssize_t sz, int *spc)
{
	uint64_t q_gpa;
	uint32_t vr_sz;
	uint16_t idx, pkt_desc_idx, hdr_desc_idx;
	int ret;
	char *vr;
	struct vring_desc *desc, *pkt_desc, *hdr_desc;
	struct vring_avail *avail;
	struct vring_used *used;

	ret = 0;

	vr_sz = vring_size(VIONET_QUEUE_SIZE);
	q_gpa = dev->vq[0].qa;
	q_gpa = q_gpa * VIRTIO_PAGE_SIZE;

	vr = malloc(vr_sz);
	if (vr == NULL) {
		log_warn("rx enq: malloc error getting vionet ring");
		return (0);
	}

	memset(vr, 0, vr_sz);

	if (read_mem(q_gpa, vr, vr_sz)) {
		log_warnx("rx enq: error reading gpa 0x%llx", q_gpa);
		free(vr);
		return (0);
	}

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr +
	    dev->vq[0].vq_availoffset);
	used = (struct vring_used *)(vr +
	    dev->vq[0].vq_usedoffset);

	idx = dev->vq[0].last_avail & VIONET_QUEUE_MASK;

	if ((avail->idx & VIONET_QUEUE_MASK) == idx) {
		log_warnx("vionet queue notify - no space, dropping packet");
		free(vr);
		return (0);
	}

	hdr_desc_idx = avail->ring[idx] & VIONET_QUEUE_MASK;
	hdr_desc = &desc[hdr_desc_idx];

	pkt_desc_idx = hdr_desc->next & VIONET_QUEUE_MASK;
	pkt_desc = &desc[pkt_desc_idx];

	/* must be not readable */
	if ((pkt_desc->flags & VRING_DESC_F_WRITE) == 0) {
		log_warnx("unexpected readable rx descriptor %d",
		    pkt_desc_idx);
		free(vr);
		return (0);
	}

	/* Write packet to descriptor ring */
	if (write_mem(pkt_desc->addr, pkt, sz)) {
		log_warnx("vionet: rx enq packet write_mem error @ "
		    "0x%llx", pkt_desc->addr);
		free(vr);
		return (0);
	}

	ret = 1;
	dev->cfg.isr_status = 1;
	used->ring[used->idx & VIONET_QUEUE_MASK].id = hdr_desc_idx;
	used->ring[used->idx & VIONET_QUEUE_MASK].len = hdr_desc->len + sz;
	used->idx++;
	dev->vq[0].last_avail = (dev->vq[0].last_avail + 1);
	*spc = avail->idx - dev->vq[0].last_avail;
	if (write_mem(q_gpa, vr, vr_sz)) {
		log_warnx("vionet: error writing vio ring");
	}

	free(vr);

	return (ret);
}

int
vionet_process_rx(void)
{
	int i, num_enq, spc, hasdata;
	ssize_t sz;
	char *buf;
	struct pollfd pfd;

	num_enq = 0;
	buf = malloc(PAGE_SIZE);
	for (i = 0 ; i < nr_vionet; i++) {
		if (!vionet[i].rx_added)
			continue;

		spc = 1;
		hasdata = 1;
		memset(buf, 0, PAGE_SIZE);
		memset(&pfd, 0, sizeof(struct pollfd));
		pfd.fd = vionet[i].fd;
		pfd.events = POLLIN;
		while (spc && hasdata) {
			hasdata = poll(&pfd, 1, 0);
			if (hasdata == 1) {
				sz = read(vionet[i].fd, buf, PAGE_SIZE);
				if (sz != 0) {
					num_enq += vionet_enq_rx(&vionet[i],
					    buf, sz, &spc);
				} else if (sz == 0) {
					log_debug("process_rx: no data");
					hasdata = 0;
				}
			}
		}
	}

	free(buf);

	/*
	 * XXX returns the number of packets enqueued across all vionet, which
	 * may not be right for VMs with more than one vionet.
	 */
	return (num_enq);
}

void
vionet_notify_rx(struct vionet_dev *dev)
{
	uint64_t q_gpa;
	uint32_t vr_sz;
	uint16_t idx, pkt_desc_idx;
	char *vr;
	struct vring_desc *desc, *pkt_desc;
	struct vring_avail *avail;
	struct vring_used *used;

	vr_sz = vring_size(VIONET_QUEUE_SIZE);
	q_gpa = dev->vq[dev->cfg.queue_notify].qa;
	q_gpa = q_gpa * VIRTIO_PAGE_SIZE;

	vr = malloc(vr_sz);
	if (vr == NULL) {
		log_warn("malloc error getting vionet ring");
		return;
	}

	memset(vr, 0, vr_sz);

	if (read_mem(q_gpa, vr, vr_sz)) {
		log_warnx("error reading gpa 0x%llx", q_gpa);
		free(vr);
		return;
	}

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr +
	    dev->vq[dev->cfg.queue_notify].vq_availoffset);
	used = (struct vring_used *)(vr +
	    dev->vq[dev->cfg.queue_notify].vq_usedoffset);

	idx = dev->vq[dev->cfg.queue_notify].last_avail & VIONET_QUEUE_MASK;
	pkt_desc_idx = avail->ring[idx] & VIONET_QUEUE_MASK;
	pkt_desc = &desc[pkt_desc_idx];

	dev->rx_added = 1;

	free(vr);
}


/*
 * XXX cant trust ring data from VM, be extra cautious.
 * XXX advertise link status to guest
 */
int
vionet_notifyq(struct vionet_dev *dev)
{
	uint64_t q_gpa;
	uint32_t vr_sz;
	uint16_t idx, pkt_desc_idx, hdr_desc_idx, dxx;
	size_t pktsz;
	int ret, num_enq, ofs;
	char *vr, *pkt;
	struct vring_desc *desc, *pkt_desc, *hdr_desc;
	struct vring_avail *avail;
	struct vring_used *used;

	vr = pkt = NULL;
	ret = 0;

	/* Invalid queue? */
	if (dev->cfg.queue_notify != 1) {
		vionet_notify_rx(dev);
		goto out;
	}

	vr_sz = vring_size(VIONET_QUEUE_SIZE);
	q_gpa = dev->vq[dev->cfg.queue_notify].qa;
	q_gpa = q_gpa * VIRTIO_PAGE_SIZE;

	vr = malloc(vr_sz);
	if (vr == NULL) {
		log_warn("malloc error getting vionet ring");
		goto out;
	}

	memset(vr, 0, vr_sz);

	if (read_mem(q_gpa, vr, vr_sz)) {
		log_warnx("error reading gpa 0x%llx", q_gpa);
		goto out;
	}

	/* Compute offsets in ring of descriptors, avail ring, and used ring */
	desc = (struct vring_desc *)(vr);
	avail = (struct vring_avail *)(vr +
	    dev->vq[dev->cfg.queue_notify].vq_availoffset);
	used = (struct vring_used *)(vr +
	    dev->vq[dev->cfg.queue_notify].vq_usedoffset);

	num_enq = 0;

	idx = dev->vq[dev->cfg.queue_notify].last_avail & VIONET_QUEUE_MASK;

	if ((avail->idx & VIONET_QUEUE_MASK) == idx) {
		log_warnx("vionet tx queue notify - nothing to do?");
		goto out;
	}

	while ((avail->idx & VIONET_QUEUE_MASK) != idx) {
		hdr_desc_idx = avail->ring[idx] & VIONET_QUEUE_MASK;
		hdr_desc = &desc[hdr_desc_idx];
		pktsz = 0;

		dxx = hdr_desc_idx;
		do {
			pktsz += desc[dxx].len;
			dxx = desc[dxx].next;
		} while (desc[dxx].flags & VRING_DESC_F_NEXT);

		pktsz += desc[dxx].len;

		/* Remove virtio header descriptor len */
		pktsz -= hdr_desc->len;

		/*
		 * XXX check sanity pktsz
		 * XXX too long and  > PAGE_SIZE checks
		 *     (PAGE_SIZE can be relaxed to 16384 later)
		 */
		pkt = malloc(pktsz);
		if (pkt == NULL) {
			log_warn("malloc error alloc packet buf");
			goto out;
		}

		ofs = 0;
		pkt_desc_idx = hdr_desc->next & VIONET_QUEUE_MASK;
		pkt_desc = &desc[pkt_desc_idx];

		while (pkt_desc->flags & VRING_DESC_F_NEXT) {
			/* must be not writable */
			if (pkt_desc->flags & VRING_DESC_F_WRITE) {
				log_warnx("unexpected writable tx desc "
				    "%d", pkt_desc_idx);
				goto out;
			}

			/* Read packet from descriptor ring */
			if (read_mem(pkt_desc->addr, pkt + ofs,
			    pkt_desc->len)) {
				log_warnx("vionet: packet read_mem error "
				    "@ 0x%llx", pkt_desc->addr);
				goto out;
			}

			ofs += pkt_desc->len;
			pkt_desc_idx = pkt_desc->next & VIONET_QUEUE_MASK;
			pkt_desc = &desc[pkt_desc_idx];
		}

		/* Now handle tail descriptor - must be not writable */
		if (pkt_desc->flags & VRING_DESC_F_WRITE) {
			log_warnx("unexpected writable tx descriptor %d",
			    pkt_desc_idx);
			goto out;
		}

		/* Read packet from descriptor ring */
		if (read_mem(pkt_desc->addr, pkt + ofs,
		    pkt_desc->len)) {
			log_warnx("vionet: packet read_mem error @ "
			    "0x%llx", pkt_desc->addr);
			goto out;
		}

		/* XXX signed vs unsigned here, funky cast */
		if (write(dev->fd, pkt, pktsz) != (int)pktsz) {
			log_warnx("vionet: tx failed writing to tap: "
			    "%d", errno);
			goto out;
		}

		ret = 1;
		dev->cfg.isr_status = 1;
		used->ring[used->idx & VIONET_QUEUE_MASK].id = hdr_desc_idx;
		used->ring[used->idx & VIONET_QUEUE_MASK].len = hdr_desc->len;
		used->idx++;

		dev->vq[dev->cfg.queue_notify].last_avail =
		    (dev->vq[dev->cfg.queue_notify].last_avail + 1);
		num_enq++;

		idx = dev->vq[dev->cfg.queue_notify].last_avail &
		    VIONET_QUEUE_MASK;
	}

	if (write_mem(q_gpa, vr, vr_sz)) {
		log_warnx("vionet: tx error writing vio ring");
	}

out:
	free(vr);
	free(pkt);

	return (ret);
}

void
virtio_init(struct vm_create_params *vcp, int *child_disks, int *child_taps)
{
	uint8_t id;
	uint8_t i;
	off_t sz;

	/* Virtio entropy device */
	if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
	    PCI_PRODUCT_QUMRANET_VIO_RNG, PCI_CLASS_SYSTEM,
	    PCI_SUBCLASS_SYSTEM_MISC,
	    PCI_VENDOR_OPENBSD,
	    PCI_PRODUCT_VIRTIO_ENTROPY, 1, NULL)) {
		log_warnx("%s: can't add PCI virtio rng device",
		    __progname);
		return;
	}

	if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, virtio_rnd_io, NULL)) {
		log_warnx("%s: can't add bar for virtio rng device",
		    __progname);
		return;
	}

	memset(&viornd, 0, sizeof(viornd));
	viornd.vq[0].qs = VIORND_QUEUE_SIZE;
	viornd.vq[0].vq_availoffset = sizeof(struct vring_desc) *
	    VIORND_QUEUE_SIZE;
	viornd.vq[0].vq_usedoffset = VIRTQUEUE_ALIGN(
	    sizeof(struct vring_desc) * VIORND_QUEUE_SIZE
	    + sizeof(uint16_t) * (2 + VIORND_QUEUE_SIZE));


	vioblk = malloc(sizeof(struct vioblk_dev) * vcp->vcp_ndisks);
	if (vioblk == NULL) {
		log_warn("%s: malloc failure allocating vioblks",
		    __progname);
		return;
	}

	memset(vioblk, 0, sizeof(struct vioblk_dev) * vcp->vcp_ndisks);

	/* One virtio block device for each disk defined in vcp */
	for (i = 0; i < vcp->vcp_ndisks; i++) {
		if ((sz = lseek(child_disks[i], 0, SEEK_END)) == -1)
			continue;

		if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
		    PCI_PRODUCT_QUMRANET_VIO_BLOCK, PCI_CLASS_MASS_STORAGE,
		    PCI_SUBCLASS_MASS_STORAGE_SCSI,
		    PCI_VENDOR_OPENBSD,
		    PCI_PRODUCT_VIRTIO_BLOCK, 1, NULL)) {
			log_warnx("%s: can't add PCI virtio block "
			    "device", __progname);
			return;
		}
		if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, virtio_blk_io,
		    &vioblk[i])) {
			log_warnx("%s: can't add bar for virtio block "
			    "device", __progname);
			return;
		}
		vioblk[i].vq[0].qs = VIOBLK_QUEUE_SIZE;
		vioblk[i].vq[0].vq_availoffset = sizeof(struct vring_desc) *
		    VIORND_QUEUE_SIZE;
		vioblk[i].vq[0].vq_usedoffset = VIRTQUEUE_ALIGN(
		    sizeof(struct vring_desc) * VIOBLK_QUEUE_SIZE
		    + sizeof(uint16_t) * (2 + VIOBLK_QUEUE_SIZE));
		vioblk[i].vq[0].last_avail = 0;
		vioblk[i].fd = child_disks[i];
		vioblk[i].sz = sz / 512;
	}

	vionet = malloc(sizeof(struct vionet_dev) * vcp->vcp_nnics);
	if (vionet == NULL) {
		log_warn("%s: malloc failure allocating vionets",
		    __progname);
		return;
	}

	memset(vionet, 0, sizeof(struct vionet_dev) * vcp->vcp_nnics);

	nr_vionet = vcp->vcp_nnics;
	/* Virtio network */
	for (i = 0; i < vcp->vcp_nnics; i++) {
		if (pci_add_device(&id, PCI_VENDOR_QUMRANET,
		    PCI_PRODUCT_QUMRANET_VIO_NET, PCI_CLASS_SYSTEM,
		    PCI_SUBCLASS_SYSTEM_MISC,
		    PCI_VENDOR_OPENBSD,
		    PCI_PRODUCT_VIRTIO_NETWORK, 1, NULL)) {
			log_warnx("%s: can't add PCI virtio net device",
			    __progname);
			return;
		}

		if (pci_add_bar(id, PCI_MAPREG_TYPE_IO, virtio_net_io,
		    &vionet[i])) {
			log_warnx("%s: can't add bar for virtio net "
			    "device", __progname);
			return;
		}

		vionet[i].vq[0].qs = VIONET_QUEUE_SIZE;
		vionet[i].vq[0].vq_availoffset = sizeof(struct vring_desc) *
		    VIONET_QUEUE_SIZE;
		vionet[i].vq[0].vq_usedoffset = VIRTQUEUE_ALIGN(
		    sizeof(struct vring_desc) * VIONET_QUEUE_SIZE
		    + sizeof(uint16_t) * (2 + VIONET_QUEUE_SIZE));
		vionet[i].vq[0].last_avail = 0;
		vionet[i].vq[1].qs = VIONET_QUEUE_SIZE;
		vionet[i].vq[1].vq_availoffset = sizeof(struct vring_desc) *
		    VIONET_QUEUE_SIZE;
		vionet[i].vq[1].vq_usedoffset = VIRTQUEUE_ALIGN(
		    sizeof(struct vring_desc) * VIONET_QUEUE_SIZE
		    + sizeof(uint16_t) * (2 + VIONET_QUEUE_SIZE));
		vionet[i].vq[1].last_avail = 0;
		vionet[i].fd = child_taps[i];

#if 0
		/* User defined MAC */
		vionet[i].cfg.device_feature = VIRTIO_NET_F_MAC;
		bcopy(&vcp->vcp_macs[i], &vionet[i].mac, 6);
#endif
	}
}
