/*
 * Copyright (C) 2016 CNEX Labs
 * Initial release: Javier Gonzalez <javier@cnexlabs.com>
 *                  Matias Bjorling <matias@cnexlabs.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * pblk-rl.c - pblk's rate limiter for user I/O
 *
 */

#include "pblk.h"

static void pblk_rl_kick_u_timer(struct pblk_rl *rl)
{
	mod_timer(&rl->u_timer, jiffies + msecs_to_jiffies(5000));
}

int pblk_rl_is_limit(struct pblk_rl *rl)
{
	int rb_space;

	rb_space = atomic_read(&rl->rb_space);

	return (rb_space == 0);
}

int pblk_rl_user_may_insert(struct pblk_rl *rl, int nr_entries)
{
	int rb_user_cnt = atomic_read(&rl->rb_user_cnt);
	int rb_space = atomic_read(&rl->rb_space);

	if (unlikely(rb_space >= 0) && (rb_space - nr_entries < 0))
		return NVM_IO_ERR;

	if (rb_user_cnt >= rl->rb_user_max)
		return NVM_IO_REQUEUE;

	return NVM_IO_OK;
}

void pblk_rl_inserted(struct pblk_rl *rl, int nr_entries)
{
	int rb_space = atomic_read(&rl->rb_space);

	if (unlikely(rb_space >= 0))
		atomic_sub(nr_entries, &rl->rb_space);
}

int pblk_rl_gc_may_insert(struct pblk_rl *rl, int nr_entries)
{
	int rb_gc_cnt = atomic_read(&rl->rb_gc_cnt);
	int rb_user_active;

	/* If there is no user I/O let GC take over space on the write buffer */
	rb_user_active = READ_ONCE(rl->rb_user_active);
	return (!(rb_gc_cnt >= rl->rb_gc_max && rb_user_active));
}

void pblk_rl_user_in(struct pblk_rl *rl, int nr_entries)
{
	atomic_add(nr_entries, &rl->rb_user_cnt);

	/* Release user I/O state. Protect from GC */
	smp_store_release(&rl->rb_user_active, 1);
	pblk_rl_kick_u_timer(rl);
}

void pblk_rl_werr_line_in(struct pblk_rl *rl)
{
	atomic_inc(&rl->werr_lines);
	printk("ocssd[%s]: write error lines=%d\n", __func__, atomic_read(&rl->werr_lines));
}

void pblk_rl_werr_line_out(struct pblk_rl *rl)
{
	atomic_dec(&rl->werr_lines);
	printk("ocssd[%s]: write error lines=%d\n", __func__, atomic_read(&rl->werr_lines));
}

void pblk_rl_gc_in(struct pblk_rl *rl, int nr_entries)
{
	atomic_add(nr_entries, &rl->rb_gc_cnt);
}

void pblk_rl_out(struct pblk_rl *rl, int nr_user, int nr_gc)
{
	atomic_sub(nr_user, &rl->rb_user_cnt);
	atomic_sub(nr_gc, &rl->rb_gc_cnt);
}

unsigned long pblk_rl_nr_free_blks(struct pblk_rl *rl)
{
	return atomic_read(&rl->free_blocks);
}

unsigned long pblk_rl_nr_user_free_blks(struct pblk_rl *rl)
{
	return atomic_read(&rl->free_user_blocks);
}

#if (NUMS_SLC_LINE > 0) && (NUMS_SLC_LINE < 1478)
//bootmark: slc gc和user io的流量控制机制
void pblk_rl_update_slc_rates(struct pblk_rl *rl)
{
	struct pblk *pblk = container_of(rl, struct pblk, rl);
	int max = rl->rb_budget;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	static int cnt = 0;
	int inflight_io = atomic_read(&pblk->inflight_io);
	int inflight_reads = atomic_long_read(&pblk->inflight_reads);

	if(l_mg->nr_free_slc_lines == 0 && inflight_io == 0 && inflight_reads == 0) {
		cnt ++;
		printk("ocssd[%s]: add time cnt=%d\n", __func__, cnt);
	} else {
		cnt = 0;
		//printk("ocssd[%s]: ret cnt=%d, inflight_io=%d, inflight_reads=%ld\n", __func__, cnt, inflight_io, inflight_reads);
	}
	/**
	 * 1. user io is idle 30 s
	 * 2. free slc lines is empty
	 */
	if(cnt > 30 && l_mg->nr_free_slc_lines == 0 && pblk->gc.gc_slc == 0) 
	{
		//rl->rb_user_max = 0;
		//rl->rb_gc_max = max;
		rl->rb_gc_max = 1 << rl->rb_windows_pw;
		rl->rb_user_max = max - rl->rb_gc_max;
		rl->rb_state = PBLK_RL_SLC;
		pblk_gc_slc_start(pblk);
		printk("ocssd[%s]: time_cnt=%d, nr_free_slc_lines=%d, start_slc_gc\n", __func__, cnt, l_mg->nr_free_slc_lines);
	} else if(l_mg->nr_free_slc_lines < NUMS_SLC_LINE && pblk->gc.gc_slc == 1) {
		//rl->rb_user_max = 0;
		//rl->rb_gc_max = max;
		rl->rb_state = PBLK_RL_SLC;
		//printk("ocssd[%s]: rb_state=%d, nr_free_slc_lines=%d, continue_slc_gc\n", __func__, rl->rb_state, l_mg->nr_free_slc_lines);
	} else if(l_mg->nr_free_slc_lines == NUMS_SLC_LINE && pblk->gc.gc_slc == 1) {
		rl->rb_user_max = max;
		rl->rb_gc_max = 0;
		rl->rb_state = PBLK_RL_OFF;
		pblk_gc_slc_stop(pblk);
		printk("ocssd[%s]: rb_state=%d, nr_free_slc_lines=%d, close_slc_gc\n", __func__, rl->rb_state, l_mg->nr_free_slc_lines);
	}
}
#endif

//bootmark: normal gc和user io的流量控制机制
static void __pblk_rl_update_rates(struct pblk_rl *rl, unsigned long free_blocks)
{
	struct pblk *pblk = container_of(rl, struct pblk, rl);
	int max = rl->rb_budget;
	int werr_gc_needed = atomic_read(&rl->werr_lines);

	if(pblk->gc.gc_slc == 1)
		return;

	if (free_blocks >= rl->high) {
		if (werr_gc_needed) {
			/* Allocate a small budget for recovering
			 * lines with write errors
			 */
			rl->rb_gc_max = 1 << rl->rb_windows_pw;
			rl->rb_user_max = max - rl->rb_gc_max;
			rl->rb_state = PBLK_RL_WERR;
		} else {
			rl->rb_user_max = max;
			rl->rb_gc_max = 0;
			rl->rb_state = PBLK_RL_OFF;
		}
	} else if (free_blocks < rl->high) {
		int shift = rl->high_pw - rl->rb_windows_pw;
		int user_windows = free_blocks >> shift;
		int user_max = user_windows << PBLK_MAX_REQ_ADDRS_PW;

		rl->rb_user_max = user_max;
		rl->rb_gc_max = max - user_max;

		if (free_blocks <= rl->rsv_blocks) {
			rl->rb_user_max = 0;
			rl->rb_gc_max = max;
		}

		/* In the worst case, we will need to GC lines in the low list
		 * (high valid sector count). If there are lines to GC on high
		 * or mid lists, these will be prioritized
		 */
		rl->rb_state = PBLK_RL_LOW;
	}

	if (rl->rb_state != PBLK_RL_OFF) {
		//printk("ocssd[%s]: free_blocks=%ld, rb_state=%d start_gc\n", __func__, free_blocks, rl->rb_state);
		pblk_gc_should_start(pblk);
	} else {
		pblk_gc_should_stop(pblk);
	}
}

void pblk_rl_update_rates(struct pblk_rl *rl)
{
	__pblk_rl_update_rates(rl, pblk_rl_nr_user_free_blks(rl));
}

void pblk_rl_free_lines_inc(struct pblk_rl *rl, struct pblk_line *line)
{
	int blk_in_line = atomic_read(&line->blk_in_line);
	int free_blocks;

	atomic_add(blk_in_line, &rl->free_blocks);
	//printk("ocssd[%s]: total_free_blocks=%d, blk_in_line=%d\n", __func__, atomic_read(&rl->free_blocks), blk_in_line);
	free_blocks = atomic_add_return(blk_in_line, &rl->free_user_blocks);

	__pblk_rl_update_rates(rl, free_blocks);
}

//bookmark: 执行一次，剩余的块数减刚用掉的块
void pblk_rl_free_lines_dec(struct pblk_rl *rl, struct pblk_line *line,
			    bool used)
{
	int blk_in_line = atomic_read(&line->blk_in_line);
	int free_blocks;

	atomic_sub(blk_in_line, &rl->free_blocks);
	//printk("ocssd[%s]: line_id=%d, total_free_blocks=%d, blk_in_line=%d\n", __func__, line->id, atomic_read(&rl->free_blocks), blk_in_line);

	if (used)
		free_blocks = atomic_sub_return(blk_in_line,
							&rl->free_user_blocks);
	else
		free_blocks = atomic_read(&rl->free_user_blocks);

	__pblk_rl_update_rates(rl, free_blocks);
}

int pblk_rl_high_thrs(struct pblk_rl *rl)
{
	return rl->high;
}

int pblk_rl_max_io(struct pblk_rl *rl)
{
	return rl->rb_max_io;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
static void pblk_rl_u_timer(unsigned long _arg)
{
	struct pblk_rl *rl = (struct pblk_rl *)_arg;

	/* Release user I/O state. Protect from GC */
	smp_store_release(&rl->rb_user_active, 0);
}
#else
static void pblk_rl_u_timer(struct timer_list *t)
{
	struct pblk_rl *rl = from_timer(rl, t, u_timer);

	/* Release user I/O state. Protect from GC */
	smp_store_release(&rl->rb_user_active, 0);
}
#endif

void pblk_rl_free(struct pblk_rl *rl)
{
	del_timer(&rl->u_timer);
}

void pblk_rl_init(struct pblk_rl *rl, int budget)
{
	struct pblk *pblk = container_of(rl, struct pblk, rl);
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	int min_blocks = lm->blk_per_line * PBLK_GC_RSV_LINE;
	int sec_meta, blk_meta;

	unsigned int rb_windows;

	/* Consider sectors used for metadata */
	sec_meta = (lm->smeta_sec + lm->emeta_sec[0]) * l_mg->nr_free_lines;
	blk_meta = DIV_ROUND_UP(sec_meta, geo->clba);

	rl->high = pblk->op_blks - blk_meta - lm->blk_per_line;
	rl->high_pw = get_count_order(rl->high);

	rl->rsv_blocks = min_blocks;

	/* This will always be a power-of-2 */
	rb_windows = budget / PBLK_MAX_REQ_ADDRS;
	rl->rb_windows_pw = get_count_order(rb_windows);

	/* To start with, all buffer is available to user I/O writers */
	rl->rb_budget = budget;
	rl->rb_user_max = budget;
	rl->rb_max_io = budget >> 1;
	rl->rb_gc_max = 0;
	rl->rb_state = PBLK_RL_HIGH;

	atomic_set(&rl->rb_user_cnt, 0);
	atomic_set(&rl->rb_gc_cnt, 0);
	atomic_set(&rl->rb_space, -1);
	atomic_set(&rl->werr_lines, 0);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	setup_timer(&rl->u_timer, pblk_rl_u_timer, (unsigned long)rl);
#else
	timer_setup(&rl->u_timer, pblk_rl_u_timer, 0);
#endif

	rl->rb_user_active = 0;
	rl->rb_gc_active = 0;
}
