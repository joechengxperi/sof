/* 
 * BSD 3 Clause - See LICENCE file for details.
 *
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 * DW DMA driver.
 *
 * DW DMA IP comes in several flavours each with different capabilities and
 * with register and bit changes between falvours.
 *
 * This driver API will only be called by 3 clients in reef :-
 *
 * 1. Host audio component. This component represents the ALSA PCM device
 *    and involves copying data to/from the host ALSA audio buffer to/from the
 *    the DSP buffer.
 *
 * 2. DAI audio component. This component represents physical DAIs and involves
 *    copying data to/from the DSP buffers to/from the DAI FIFOs.
 *
 * 3. IPC Layer. Some IPC needs DMA to copy audio buffer page table information
 *    from the host DRAM into DSP DRAM. This page table information is then
 *    used to construct the DMA configuration for the host client 1 above. 
 */

#include <reef/debug.h>
#include <reef/reef.h>
#include <reef/dma.h>
#include <reef/dw-dma.h>
#include <reef/io.h>
#include <reef/stream.h>
#include <reef/timer.h>
#include <reef/alloc.h>
#include <reef/interrupt.h>
#include <reef/work.h>
#include <reef/lock.h>
#include <reef/trace.h>
#include <platform/dma.h>
#include <platform/platform.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* channel registers */
#define DW_MAX_CHAN			8
#define DW_CH_SIZE			0x58
#define BYT_CHAN_OFFSET(chan) \
	(DW_CH_SIZE * chan)

#define DW_SAR(chan)	\
	(0x0000 + BYT_CHAN_OFFSET(chan))
#define DW_DAR(chan) \
	(0x0008 + BYT_CHAN_OFFSET(chan))
#define DW_LLP(chan) \
	(0x0010 + BYT_CHAN_OFFSET(chan))
#define DW_CTRL_LOW(chan) \
	(0x0018 + BYT_CHAN_OFFSET(chan))
#define DW_CTRL_HIGH(chan) \
	(0x001C + BYT_CHAN_OFFSET(chan))
#define DW_CFG_LOW(chan) \
	(0x0040 + BYT_CHAN_OFFSET(chan))
#define DW_CFG_HIGH(chan) \
	(0x0044 + BYT_CHAN_OFFSET(chan))

/* registers */
#define DW_STATUS_TFR			0x02E8
#define DW_STATUS_BLOCK			0x02F0
#define DW_STATUS_SRC_TRAN		0x02F8
#define DW_STATUS_DST_TRAN		0x0300
#define DW_STATUS_ERR			0x0308
#define DW_RAW_TFR			0x02C0
#define DW_RAW_BLOCK			0x02C8
#define DW_RAW_SRC_TRAN                        0x02D0
#define DW_RAW_DST_TRAN                        0x02D8
#define DW_RAW_ERR			0x02E0
#define DW_MASK_TFR			0x0310
#define DW_MASK_BLOCK			0x0318
#define DW_MASK_SRC_TRAN		0x0320
#define DW_MASK_DST_TRAN		0x0328
#define DW_MASK_ERR			0x0330
#define DW_CLEAR_TFR			0x0338
#define DW_CLEAR_BLOCK			0x0340
#define DW_CLEAR_SRC_TRAN		0x0348
#define DW_CLEAR_DST_TRAN		0x0350
#define DW_CLEAR_ERR			0x0358
#define DW_INTR_STATUS			0x0360
#define DW_DMA_CFG			0x0398
#define DW_DMA_CHAN_EN			0x03A0
#define DW_FIFO_PARTI0_LO		0x0400
#define DW_FIFO_PART0_HI		0x0404
#define DW_FIFO_PART1_LO		0x0408
#define DW_FIFO_PART1_HI		0x040C
#define DW_CH_SAI_ERR			0x0410

/* channel bits */
#define INT_MASK(chan)			(0x100 << chan)
#define INT_UNMASK(chan)		(0x101 << chan)
#define CHAN_ENABLE(chan)		(0x101 << chan)
#define CHAN_DISABLE(chan)		(0x100 << chan)

#define DW_CFG_CH_SUSPEND		0x100
#define DW_CFG_CH_DRAIN			0x400
#define DW_CFG_CH_FIFO_EMPTY		0x200

/* data for each DMA channel */
struct dma_chan_data {
	uint8_t status;
	uint8_t reserved[3];
	struct dw_lli2 *lli;
	int desc_count;
	uint32_t cfg_lo;
	uint32_t cfg_hi;

	void (*cb)(void *data, uint32_t type);	/* client callback function */
	void *cb_data;		/* client callback data */
};

/* private data for DW DMA engine */
struct dma_pdata {
	struct dma_chan_data chan[DW_MAX_CHAN];
	struct work work;
	spinlock_t lock;
};

static inline void dw_write(struct dma *dma, uint32_t reg, uint32_t value)
{
	io_reg_write(dma_base(dma) + reg, value);
}

static inline uint32_t dw_read(struct dma *dma, uint32_t reg)
{
	return io_reg_read(dma_base(dma) + reg);
}

/* allocate next free DMA channel */
static int dw_dma_channel_get(struct dma *dma)
{
	struct dma_pdata *p = dma_get_drvdata(dma);
	int i;
	
	/* find first free non draining channel */
	for (i = 0; i < DW_MAX_CHAN; i++) {

		/* dont use any channels that are still draining */
		if (p->chan[i].status == DMA_STATUS_DRAINING)
			continue;

		/* use channel if it's free */
		/* TODO: may need read Channel Enable register to choose a
		free/disabled channel */
		if (dw_read(dma, DW_DMA_CHAN_EN) & (0x1 << i))
			continue;

		if (p->chan[i].status == DMA_STATUS_FREE) {
			p->chan[i].status = DMA_STATUS_IDLE;

			/* write interrupt clear registers for the channel:
			ClearTfr, ClearBlock, ClearSrcTran, ClearDstTran, ClearErr*/
			dw_write(dma, DW_CLEAR_TFR, 0x1 << i);
			dw_write(dma, DW_CLEAR_BLOCK, 0x1 << i);
			dw_write(dma, DW_CLEAR_SRC_TRAN, 0x1 << i);
			dw_write(dma, DW_CLEAR_DST_TRAN, 0x1 << i);
			dw_write(dma, DW_CLEAR_ERR, 0x1 << i);

			/* TODO: do we need read back Interrupt Raw Status and Interrupt
			Status registers to confirm that all interrupts have been cleared? */
			return i;
		}
	}

	/* DMAC has no free channels */
	return -ENODEV;
}

static void dw_dma_channel_put(struct dma *dma, int channel)
{
	struct dma_pdata *p = dma_get_drvdata(dma);

	dw_write(dma, DW_DMA_CHAN_EN, CHAN_DISABLE(channel));

	// TODO: disable/reset any other channel config.
	/* free the lli allocated by set_config*/
	if (p->chan[channel].lli)
		rfree(RZONE_MODULE, RMOD_SYS, p->chan[channel].lli);

	// TODO: fix status since it may still be draining
	p->chan[channel].status = DMA_STATUS_FREE;
	p->chan[channel].cb = NULL;

}

static int dw_dma_start(struct dma *dma, int channel)
{
	struct dma_pdata *p = dma_get_drvdata(dma);

	/* write interrupt clear registers for the channel:
	ClearTfr, ClearBlock, ClearErr*/
	dw_write(dma, DW_CLEAR_TFR, 0x1 << channel);
	dw_write(dma, DW_CLEAR_BLOCK, 0x1 << channel);
	dw_write(dma, DW_CLEAR_ERR, 0x1 << channel);
	dw_write(dma, DW_CLEAR_SRC_TRAN, 0x1 << channel);
	dw_write(dma, DW_CLEAR_DST_TRAN, 0x1 << channel);

	/* write SARn, DARn */
	dw_write(dma, DW_SAR(channel), p->chan[channel].lli->sar);
	dw_write(dma, DW_DAR(channel), p->chan[channel].lli->dar);
	dw_write(dma, DW_LLP(channel), p->chan[channel].lli->llp);

	/* program CTLn and CFGn*/
	dw_write(dma, DW_CTRL_LOW(channel), p->chan[channel].lli->ctrl_lo);
	dw_write(dma, DW_CTRL_HIGH(channel), p->chan[channel].lli->ctrl_hi);

	/* TODO: get correct values for these - left at defaults for the moment */
	//dw_write(dma, DW_CFG_LOW(channel), p->chan[channel].cfg_lo);
	//dw_write(dma, DW_CFG_HIGH(channel), p->chan[channel].cfg_hi);

	p->chan[channel].status = DMA_STATUS_RUNNING;
#if 0
dbg_val_at(dw_read(dma, DW_SAR(channel)), 20);
dbg_val_at(dw_read(dma, DW_DAR(channel)), 21);
dbg_val_at(dw_read(dma, DW_LLP(channel)), 22);
dbg_val_at(dw_read(dma, DW_CTRL_LOW(channel)), 23);
dbg_val_at(dw_read(dma, DW_CTRL_HIGH(channel)), 24);
dbg_val_at(dw_read(dma, DW_CFG_LOW(channel)), 25);
dbg_val_at(dw_read(dma, DW_CFG_HIGH(channel)), 26);
#endif



	/* unmask all kinds of interrupts for this channels */
	dw_write(dma, DW_MASK_TFR, INT_UNMASK(channel));
	dw_write(dma, DW_MASK_BLOCK, INT_UNMASK(channel));
	dw_write(dma, DW_MASK_ERR, INT_UNMASK(channel));

//dbg_val_at(dw_read(dma, DW_MASK_TFR), 27);
//dbg_val_at(dw_read(dma, DW_MASK_BLOCK), 28);
//dbg_val_at(dw_read(dma, DW_DMA_CFG), 18);

	/* enable the channel */
	dw_write(dma, DW_DMA_CHAN_EN, CHAN_ENABLE(channel));
dbg_val_at(dw_read(dma, DW_DMA_CHAN_EN), 19);
	return 0;
}

/*
 * Wait for DMA drain completion using delayed work. This allows the stream
 * IPC to return immediately without blocking the host. This work is called 
 * by the general system timer.
 */
static uint32_t dw_dma_fifo_work(void *data)
{
	struct dma *dma = (struct dma *)data;
	struct dma_pdata *p = dma_get_drvdata(dma);
	int i, schedule;
	uint32_t cfg;

	/* check any draining channels */
	for (i = 0; i < DW_MAX_CHAN; i++) {

		/* only check channels that are still draining */
		if (p->chan[i].status != DMA_STATUS_DRAINING)
			continue;

		/* check for FIFO empty */
		cfg = dw_read(dma, DW_CFG_LOW(i));
		if (cfg & DW_CFG_CH_FIFO_EMPTY) {

			/* disable channel */
			io_reg_update_bits(dma_base(dma) + DW_DMA_CHAN_EN,
				CHAN_DISABLE(i), CHAN_DISABLE(i));
			p->chan[i].status = DMA_STATUS_IDLE;
		} else
			schedule = 1;
	}

	/* still waiting on more FIFOs to drain ? */
	if (schedule)
		return 1;	/* reschedule this work in 1 msec */
	else
		return 0;
}

static int dw_dma_stop(struct dma *dma, int channel)
{
	struct dma_pdata *p = dma_get_drvdata(dma);

	/* suspend the channel */
	io_reg_update_bits(dma_base(dma) + DW_CFG_LOW(channel),
		DW_CFG_CH_SUSPEND, DW_CFG_CH_SUSPEND);

	p->chan[channel].status = DMA_STATUS_DRAINING;
	
	/* FIFO cleanup done by general purpose timer */
	work_schedule_default(&p->work, 1);
	return 0;
}

static int dw_dma_drain(struct dma *dma, int channel)
{
	struct dma_pdata *p = dma_get_drvdata(dma);

	/* suspend the channel */
	io_reg_update_bits(dma_base(dma) + DW_CFG_LOW(channel),
		DW_CFG_CH_SUSPEND | DW_CFG_CH_DRAIN,
		DW_CFG_CH_SUSPEND | DW_CFG_CH_DRAIN);

	p->chan[channel].status = DMA_STATUS_DRAINING;

	/* FIFO cleanup done by general purpose timer */
	work_schedule_default(&p->work, 1);
	return 0;
}

/* fill in "status" with current DMA channel state and position */
static int dw_dma_status(struct dma *dma, int channel,
	struct dma_chan_status *status)
{
	
	return 0;
}
//static int l = 0;
/* set the DMA channel configuration, source/target address, buffer sizes */
static int dw_dma_set_config(struct dma *dma, int channel,
	struct dma_sg_config *config)
{
	struct dma_pdata *p = dma_get_drvdata(dma);
	struct list_head *plist;
	struct dma_sg_elem *sg_elem;
	struct dw_lli2 *lli_desc, *lli_desc_head, *lli_desc_tail;

	/* get number of SG elems - TODO: add this count to struct */
	p->chan[channel].desc_count = 0;
	list_for_each(plist, &config->elem_list) {
		p->chan[channel].desc_count++;
	}

	/* allocate descriptors for channel */
	p->chan[channel].lli = rmalloc(RZONE_MODULE, RMOD_SYS,
		sizeof(struct dw_lli2) * p->chan[channel].desc_count);
	if (!p->chan[channel].lli)
		return -ENOMEM;

	bzero(p->chan[channel].lli, sizeof(struct dw_lli2) *
		p->chan[channel].desc_count);
	lli_desc = lli_desc_head = p->chan[channel].lli;
	lli_desc_tail = p->chan[channel].lli + p->chan[channel].desc_count - 1;

	/* SSTATARn/DSTATARn for write back*/

	/* fill in lli for the elem in the list */
	list_for_each(plist, &config->elem_list) {

		sg_elem = container_of(plist, struct dma_sg_elem, list);

		lli_desc->ctrl_hi &= ~DWC_CTLH_DONE; /* clear the done bit */

		/* write CTL_LOn for the first lli */
		// TODO: optimise the burst size.
		lli_desc->ctrl_lo |= DWC_CTLL_FC(config->direction); /* config the transfer type */
		lli_desc->ctrl_lo |= DWC_CTLL_SRC_WIDTH(2); /* config the src/dest tr width */
		lli_desc->ctrl_lo |= DWC_CTLL_DST_WIDTH(2); /* config the src/dest tr width */
		lli_desc->ctrl_lo |= DWC_CTLL_SRC_MSIZE(3); /* config the src/dest tr width */
		lli_desc->ctrl_lo |= DWC_CTLL_DST_MSIZE(3); /* config the src/dest tr width */
		lli_desc->ctrl_lo |= DWC_CTLL_INT_EN; /* enable interrupt */

		/* config the SINC and DINC field of CTL_LOn, SRC/DST_PER filed of CFGn */
		switch (config->direction) {
		case DMA_DIR_MEM_TO_MEM:
			lli_desc->ctrl_lo |= DWC_CTLL_SRC_INC | DWC_CTLL_DST_INC;
			break;
		case DMA_DIR_MEM_TO_DEV:
			lli_desc->ctrl_lo |= DWC_CTLL_SRC_INC | DWC_CTLL_DST_FIX;
			//lli_desc->cfg_hi |= DWC_CFGH_DST_PER(0);//peripheral id
			break;
		case DMA_DIR_DEV_TO_MEM:
			lli_desc->ctrl_lo |= DWC_CTLL_SRC_FIX | DWC_CTLL_DST_INC;
			//lli_desc->cfg_hi |= DWC_CFGH_SRC_PER(0);//peripheral id
			break;
		case DMA_DIR_DEV_TO_DEV:
			lli_desc->ctrl_lo |= DWC_CTLL_SRC_FIX | DWC_CTLL_DST_FIX;
			//lli_desc->cfg_hi |= DWC_CFGH_SRC_PER(0) | DWC_CFGH_DST_PER(0);//peripheral id
			break;
		default:
			break;
		}

		/* set source and destination adddresses */
		lli_desc->sar = (uint32_t)sg_elem->src;
		lli_desc->dar = (uint32_t)sg_elem->dest;

		/* set transfer size of element */
		lli_desc->ctrl_hi |= sg_elem->size & DWC_CTLH_BLOCK_TS_MASK;

		/* set next descriptor in list */
		lli_desc->llp = (uint32_t)(lli_desc + 1);
		lli_desc->ctrl_lo |= DWC_CTLL_LLP_S_EN | DWC_CTLL_LLP_D_EN;

		/* next descriptor */
		lli_desc++;
	}

	/* end of list or cyclic buffer ? */
	if (config->cyclic) {
		lli_desc_tail->llp = (uint32_t)lli_desc_head;
	} else {
		lli_desc_tail->llp = 0;
		lli_desc_tail->ctrl_lo &= ~(DWC_CTLL_LLP_S_EN | DWC_CTLL_LLP_D_EN);
		//lli_desc->cfg_lo &= ~(DWC_CFGL_RELOAD_SAR | DWC_CFGL_RELOAD_DAR);//how about cfg_lo.reload_src/dst?
	}

	return 0;
}

/* restore DMA conext after leaving D3 */
static int dw_dma_pm_context_restore(struct dma *dma)
{
	return 0;
}

/* store DMA conext after leaving D3 */
static int dw_dma_pm_context_store(struct dma *dma)
{
	return 0;
}

static void dw_dma_set_cb(struct dma *dma, int channel,
		void (*cb)(void *data, uint32_t type), void *data)
{
	struct dma_pdata *p = dma_get_drvdata(dma);

	p->chan[channel].cb = cb;
	p->chan[channel].cb_data = data;
}
static int k = 0;
/* this will probably be called at the end of every period copied */
static void dw_dma_irq_handler(void *data)
{
	struct dma *dma = (struct dma *)data;
	struct dma_pdata *p = dma_get_drvdata(dma);
	uint32_t status_tfr, status_block, status_err, status_intr, pisr, mask;
	int i;

	interrupt_disable(dma_irq(dma));
	
dbg_val_at(++k, 11);
	status_intr = dw_read(dma, DW_INTR_STATUS);
//dbg_val_at(status_intr, 16);
	if (!status_intr)
		goto out;

	/* get the source of our IRQ. */
	status_block = dw_read(dma, DW_STATUS_BLOCK);
	status_tfr = dw_read(dma, DW_STATUS_TFR);

//dbg_val_at(status_block, 13);
//dbg_val_at(status_tfr, 14);
//dbg_val_at(src, 16);
//dbg_val_at(dst, 17);

	/* TODO: handle errors, just clear them atm */
	status_err = dw_read(dma, DW_STATUS_ERR);
//dbg_val_at(status_err, 15);
	dw_write(dma, DW_CLEAR_ERR, status_err);

	for (i = 0; i < DW_MAX_CHAN; i++) {

		mask = 0x1 << i;

		/* end of a transfer */
		if (status_tfr & mask) {
			if (p->chan[i].cb)
				p->chan[i].cb(p->chan[i].cb_data,
					DMA_IRQ_TYPE_LLIST);
		}

		/* TODO: end of a block */
	}

	dw_write(dma, DW_CLEAR_TFR, status_tfr);
	dw_write(dma, DW_CLEAR_BLOCK, status_block);

out:

	/* we dont use the DSP IRQ clear as we only need to clear the ISR */
	pisr = shim_read(SHIM_PISR);
	pisr |= 0xff000000;
	shim_write(SHIM_PISR, pisr);

	interrupt_enable(dma_irq(dma));
}

static int dw_dma_setup(struct dma *dma)
{
	/*mask all kinds of interrupts for all 8 channels*/
	dw_write(dma, DW_MASK_TFR, 0x0000ff00);
	dw_write(dma, DW_MASK_BLOCK, 0x0000ff00);
	dw_write(dma, DW_MASK_SRC_TRAN, 0x0000ff00);
	dw_write(dma, DW_MASK_DST_TRAN, 0x0000ff00);
	dw_write(dma, DW_MASK_ERR, 0x0000ff00);

	/*enable dma cntrl*/
	dw_write(dma, DW_DMA_CFG, 1);

	return 0;
}

static int dw_dma_probe(struct dma *dma)
{
	struct dma_pdata *dw_pdata;

	/* allocate private data */
	dw_pdata = rmalloc(RZONE_DEV, RMOD_SYS, sizeof(*dw_pdata));
	bzero(dw_pdata, sizeof(*dw_pdata));
	dma_set_drvdata(dma, dw_pdata);

	spinlock_init(dw_pdata->lock);
	dw_dma_setup(dma);

	/* init work */
	work_init(&dw_pdata->work, dw_dma_fifo_work, dma);

	/* register our IRQ handler */
	interrupt_register(dma_irq(dma), dw_dma_irq_handler, dma);
	interrupt_enable(dma_irq(dma));

	return 0;
}

const struct dma_ops dw_dma_ops = {
	.channel_get	= dw_dma_channel_get,
	.channel_put	= dw_dma_channel_put,
	.start		= dw_dma_start,
	.stop		= dw_dma_stop,
	.drain		= dw_dma_drain,
	.status		= dw_dma_status,
	.set_config	= dw_dma_set_config,
	.set_cb		= dw_dma_set_cb,
	.pm_context_restore		= dw_dma_pm_context_restore,
	.pm_context_store		= dw_dma_pm_context_store,
	.probe		= dw_dma_probe,
};

