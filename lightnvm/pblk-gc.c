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
 * pblk-gc.c - pblk's garbage collector
 */

#include "pblk.h"
#include <linux/delay.h>

static void pblk_gc_free_gc_rq(struct pblk_gc_rq *gc_rq)
{
	if (gc_rq->data)
		vfree(gc_rq->data);
	kfree(gc_rq);
}

static int pblk_gc_write(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_gc_rq *gc_rq, *tgc_rq;
	LIST_HEAD(w_list);

	spin_lock(&gc->w_lock);
	if (list_empty(&gc->w_list)) {
		spin_unlock(&gc->w_lock);
		return 1;
	}

	//printk("ocssd[%s]: pblk_gc_writer_ts\n", __func__);
	list_cut_position(&w_list, &gc->w_list, gc->w_list.prev);
	gc->w_entries = 0;
	spin_unlock(&gc->w_lock);

	list_for_each_entry_safe(gc_rq, tgc_rq, &w_list, list) {
		pblk_write_gc_to_cache(pblk, gc_rq);
		list_del(&gc_rq->list);
		kref_put(&gc_rq->line->ref, pblk_line_put);
		pblk_gc_free_gc_rq(gc_rq);
	}

	return 0;
}

static void pblk_gc_writer_kick(struct pblk_gc *gc)
{
	wake_up_process(gc->gc_writer_ts);//func pblk_gc_writer_ts
}

static void pblk_put_line_back(struct pblk *pblk, struct pblk_line *line)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct list_head *move_list;

	spin_lock(&line->lock);
	WARN_ON(line->state != PBLK_LINESTATE_GC);
	//printk("ocssd[%s]: line_id=%d, line_state=%d->%d(%s)\n", __func__, line->id, line->state, PBLK_LINESTATE_CLOSED, "LINE_CLOSE");
	line->state = PBLK_LINESTATE_CLOSED;
	move_list = pblk_line_gc_list(pblk, line);
	spin_unlock(&line->lock);

	if (move_list) {
		spin_lock(&l_mg->gc_lock);
		list_add_tail(&line->list, move_list);
		spin_unlock(&l_mg->gc_lock);
	}
}

//bookmark: switch
static void pblk_gc_line_ws(struct work_struct *work)
{
	struct pblk_line_ws *gc_rq_ws = container_of(work, struct pblk_line_ws, ws);
	struct pblk *pblk = gc_rq_ws->pblk;
	struct nvm_tgt_dev *dev = pblk->dev;
	struct nvm_geo *geo = &dev->geo;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line = gc_rq_ws->line;
	struct pblk_gc_rq *gc_rq = gc_rq_ws->priv;
	int ret;

	up(&gc->gc_sem);

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	gc_rq->data = vmalloc(gc_rq->nr_secs * geo->csecs);
#else
	gc_rq->data = vmalloc(array_size(gc_rq->nr_secs, geo->csecs));
#endif
	if (!gc_rq->data) {
		pr_err("pblk: could not GC line:%d (%d/%d)\n",
					line->id, *line->vsc, gc_rq->nr_secs);
		goto out;
	}

	/* Read from GC victim block */
	//printk("ocssd[%s]: line=%d, nr_valid_sectors=%d, nr_secs=%d\n", __func__, line->id, *line->vsc, gc_rq->nr_secs);
	ret = pblk_submit_read_gc(pblk, gc_rq);
	if (ret) {
		pr_err("pblk: failed GC read in line:%d (err:%d)\n",
								line->id, ret);
		goto out;
	}

	if (!gc_rq->secs_to_gc)
		goto out;

retry:
	spin_lock(&gc->w_lock);
	if (gc->w_entries >= PBLK_GC_RQ_QD) {
		spin_unlock(&gc->w_lock);
		pblk_gc_writer_kick(&pblk->gc);
		usleep_range(128, 256);
		goto retry;
	}
	gc->w_entries++;
	list_add_tail(&gc_rq->list, &gc->w_list);
	spin_unlock(&gc->w_lock);

	//printk("ocssd[%s]: wake_up_process gc_writer_ts\n", __func__);
	pblk_gc_writer_kick(&pblk->gc);

	kfree(gc_rq_ws);
	return;

out:
	pblk_gc_free_gc_rq(gc_rq);
	kref_put(&line->ref, pblk_line_put);
	kfree(gc_rq_ws);
}

static __le64 *get_lba_list_from_emeta(struct pblk *pblk, struct pblk_line *line)
{
	struct line_emeta *emeta_buf;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	unsigned int lba_list_size = lm->emeta_len[2];
	__le64 *lba_list;
	int ret;

	emeta_buf = pblk_malloc(lm->emeta_len[0], l_mg->emeta_alloc_type, GFP_KERNEL);
	if (!emeta_buf)
		return NULL;

	ret = pblk_line_read_emeta(pblk, line, emeta_buf);
	if (ret) {
		if( ret == -EIO ) {
			printk("ocssd[%s]: read emeta line=%d failed\n", __func__, line->id);
			pblk_scan_l2p_from_oob(pblk, line, emeta_to_lbas(pblk, emeta_buf));
			goto next;
		} else {
			pr_err("pblk: line %d read emeta failed (%d)\n", line->id, ret);
			pblk_mfree(emeta_buf, l_mg->emeta_alloc_type);
			return NULL;
		}
	}

	/* If this read fails, it means that emeta is corrupted.
	 * For now, leave the line untouched.
	 * TODO: Implement a recovery routine that scans and moves
	 * all sectors on the line.
	 */

	ret = pblk_recov_check_emeta(pblk, emeta_buf);
	if (ret) {
		printk("ocssd[%s]: check crc line=%d end_meta failed\n", __func__, line->id);
		pblk_scan_l2p_from_oob(pblk, line, emeta_to_lbas(pblk, emeta_buf));
	}
next:
	lba_list = pblk_malloc(lba_list_size, l_mg->emeta_alloc_type, GFP_KERNEL);
	if (lba_list)
		memcpy(lba_list, emeta_to_lbas(pblk, emeta_buf), lba_list_size);

	pblk_mfree(emeta_buf, l_mg->emeta_alloc_type);

	return lba_list;
}

static void pblk_gc_line_prepare_ws(struct work_struct *work)
{
	struct pblk_line_ws *line_ws = container_of(work, struct pblk_line_ws,
									ws);
	struct pblk *pblk = line_ws->pblk;
	struct pblk_line *line = line_ws->line;
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_line_meta *lm = &pblk->lm;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line_ws *gc_rq_ws;
	struct pblk_gc_rq *gc_rq;
	__le64 *lba_list;
	unsigned long *invalid_bitmap;
	int sec_left, nr_secs, bit;
	int total_sec_per_line;
	int max_write_pgs;

	if(true == pblk_line_is_slc(pblk, line->id)) {
		total_sec_per_line = lm->sec_per_line/NAND_TLC_STEP;
		max_write_pgs = pblk->max_write_pgs/NAND_TLC_STEP;
	} else {
		total_sec_per_line = lm->sec_per_line;
		max_write_pgs = pblk->max_write_pgs;
	}

	invalid_bitmap = kmalloc(lm->sec_bitmap_len, GFP_KERNEL);
	if (!invalid_bitmap)
		goto fail_free_ws;

	if (line->w_err_gc->has_write_err) {
		lba_list = line->w_err_gc->lba_list;
		line->w_err_gc->lba_list = NULL;
	} else {
		lba_list = get_lba_list_from_emeta(pblk, line);
		if (!lba_list) {
			pr_err("pblk: could not interpret emeta (line %d)\n",
					line->id);
			goto fail_free_invalid_bitmap;
		}
	}

	spin_lock(&line->lock);
	bitmap_copy(invalid_bitmap, line->invalid_bitmap, total_sec_per_line);
	sec_left = pblk_line_vsc(line);
	spin_unlock(&line->lock);

	if (sec_left < 0) {
		pr_err("pblk: corrupted GC line (%d)\n", line->id);
		goto fail_free_lba_list;
	}

	bit = -1;
next_rq:
	gc_rq = kmalloc(sizeof(struct pblk_gc_rq), GFP_KERNEL);
	if (!gc_rq)
		goto fail_free_lba_list;

	nr_secs = 0;
	do { //bookmark: 从这里选择要gc的sectors
		bit = find_next_zero_bit(invalid_bitmap, total_sec_per_line, bit + 1);
		if (bit > line->emeta_ssec)
			break;

		gc_rq->paddr_list[nr_secs] = bit;
		gc_rq->lba_list[nr_secs++] = le64_to_cpu(lba_list[bit]);
	} while (nr_secs < pblk->max_write_pgs);

	if (unlikely(!nr_secs)) {
		kfree(gc_rq);
		goto out;
	}

	gc_rq->nr_secs = nr_secs;
	gc_rq->line = line;

	gc_rq_ws = kmalloc(sizeof(struct pblk_line_ws), GFP_KERNEL);
	if (!gc_rq_ws)
		goto fail_free_gc_rq;

	gc_rq_ws->pblk = pblk;
	gc_rq_ws->line = line;
	gc_rq_ws->priv = gc_rq;

	/* The write GC path can be much slower than the read GC one due to
	 * the budget imposed by the rate-limiter. Balance in case that we get
	 * back pressure from the write GC path.
	 */
	while (down_timeout(&gc->gc_sem, msecs_to_jiffies(30000)))
		io_schedule();

	kref_get(&line->ref);
	//printk("ocssd[%s]: sec_left=%d, nr_secs=%d, line_id=%d, kref_get=%d\n", __func__, sec_left, nr_secs, line->id, atomic_read(&line->ref.refcount));

	//bookmark: backend run gc_read
	//pblk_gc_line_ws(&gc_rq_ws->ws);
	INIT_WORK(&gc_rq_ws->ws, pblk_gc_line_ws);
	queue_work(gc->gc_line_reader_wq, &gc_rq_ws->ws);

	sec_left -= nr_secs;
	if (sec_left > 0)
		goto next_rq;

out:
	pblk_mfree(lba_list, l_mg->emeta_alloc_type);
	kfree(line_ws);
	kfree(invalid_bitmap);

	kref_put(&line->ref, pblk_line_put);
	//printk("ocssd[%s]: line=%d kref_put=%d\n", __func__, line->id, atomic_read(&line->ref.refcount));
	atomic_dec(&gc->read_inflight_gc);

	return;

fail_free_gc_rq:
	kfree(gc_rq);
fail_free_lba_list:
	pblk_mfree(lba_list, l_mg->emeta_alloc_type);
fail_free_invalid_bitmap:
	kfree(invalid_bitmap);
fail_free_ws:
	kfree(line_ws);

	pblk_put_line_back(pblk, line);
	kref_put(&line->ref, pblk_line_put);
	atomic_dec(&gc->read_inflight_gc);

	pr_err("pblk: Failed to GC line %d\n", line->id);
}

static int pblk_gc_line(struct pblk *pblk, struct pblk_line *line)
{
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line_ws *line_ws;

	//printk("ocssd[%s]: line_id=%d being reclaimed for GC\n", __func__, line->id);

	line_ws = kmalloc(sizeof(struct pblk_line_ws), GFP_KERNEL);
	if (!line_ws)
		return -ENOMEM;

	line_ws->pblk = pblk;
	line_ws->line = line;

	atomic_inc(&gc->pipeline_gc);
	INIT_WORK(&line_ws->ws, pblk_gc_line_prepare_ws);
	queue_work(gc->gc_reader_wq, &line_ws->ws);

	return 0;
}

static void pblk_gc_reader_kick(struct pblk_gc *gc)
{
	wake_up_process(gc->gc_reader_ts);//func pblk_gc_reader_ts
}

static void pblk_gc_kick(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

	pblk_gc_writer_kick(gc);
	pblk_gc_reader_kick(gc);

	/* If we're shutting down GC, let's not start it up again */
	if (gc->gc_enabled) {
		//printk("ocssd[%s]: wake_up_process gc_ts\n", __func__);
		wake_up_process(gc->gc_ts);
		mod_timer(&gc->gc_timer, jiffies + msecs_to_jiffies(GC_TIME_MSECS));
	}
}

static int pblk_gc_read(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line;

	spin_lock(&gc->r_lock);
	if (list_empty(&gc->r_list)) {
		spin_unlock(&gc->r_lock);
		return 1;
	}

	line = list_first_entry(&gc->r_list, struct pblk_line, list);
	//printk("ocssd[%s]: pblk_gc_reader_ts, line_id=%d\n", __func__, line->id);
	list_del(&line->list);
	spin_unlock(&gc->r_lock);

	pblk_gc_kick(pblk);

	if (pblk_gc_line(pblk, line))
		pr_err("pblk: failed to GC line %d\n", line->id);

	return 0;
}

//bookmark: gc选择读取 有效数据最小/垃圾最多 的line
static struct pblk_line *pblk_gc_get_victim_line(struct pblk *pblk,
						 struct list_head *group_list)
{
	struct pblk_line *line, *victim;
	int line_vsc, victim_vsc;

	victim = list_first_entry(group_list, struct pblk_line, list);
	if(pblk->gc.gc_slc != 1) {
		list_for_each_entry(line, group_list, list) {
			line_vsc = le32_to_cpu(*line->vsc);
			victim_vsc = le32_to_cpu(*victim->vsc);
			if (line_vsc < victim_vsc)
				victim = line;
		}
	}

	return victim;
}

static bool pblk_gc_should_run(struct pblk_gc *gc, struct pblk_rl *rl)
{
	unsigned int nr_blocks_free, nr_blocks_need;
	unsigned int werr_lines = atomic_read(&rl->werr_lines);
	bool rlt;

	nr_blocks_need = pblk_rl_high_thrs(rl);
	nr_blocks_free = pblk_rl_nr_free_blks(rl);

	/* This is not critical, no need to take lock here */
	rlt = (werr_lines > 0) || (gc->gc_slc) || ((gc->gc_active) && (nr_blocks_need > nr_blocks_free));
	//printk("ocssd[%s]: write_error_lines=%d, gc_active=%d, nr_blocks_need=%d, nr_blocks_free=%d\n", __func__, werr_lines, gc->gc_active, nr_blocks_need, nr_blocks_free);
	//if(rlt == true) {
		//printk("ocssd[%s]: {write_error_lines=%d}, {nr_blocks_need=%d,nr_blocks_free=%d}, {gc_slc=%d} \n", __func__, werr_lines, nr_blocks_need, nr_blocks_free, gc->gc_slc);
	//}
	return rlt;
}

void pblk_gc_free_full_lines(struct pblk *pblk)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line;

	do {
		spin_lock(&l_mg->gc_lock);
		if (list_empty(&l_mg->gc_full_list)) {
			spin_unlock(&l_mg->gc_lock);
			return;
		}

		line = list_first_entry(&l_mg->gc_full_list, struct pblk_line, list);
		//printk("ocssd[%s]: pblk_line_put line_id=%d\n", __func__, line->id);

		spin_lock(&line->lock);
		WARN_ON(line->state != PBLK_LINESTATE_CLOSED);
		line->state = PBLK_LINESTATE_GC;
		//printk("ocssd[%s]: line_id=%d, line_state=%d(PBLK_LINESTATE_GC), call pblk_line_put\n", __func__, line->id, line->state);
		spin_unlock(&line->lock);

		list_del(&line->list);
		spin_unlock(&l_mg->gc_lock);

		atomic_inc(&gc->pipeline_gc);
		kref_put(&line->ref, pblk_line_put);
	} while (1);
}

/*
 * Lines with no valid sectors will be returned to the free list immediately. If
 * GC is activated - either because the free block count is under the determined
 * threshold, or because it is being forced from user space - only lines with a
 * high count of invalid sectors will be recycled.
 */
static void pblk_gc_run(struct pblk *pblk)
{
	struct pblk_line_mgmt *l_mg = &pblk->l_mg;
	struct pblk_gc *gc = &pblk->gc;
	struct pblk_line *line;
	struct list_head *group_list;
	bool run_gc;
	int read_inflight_gc, gc_group = 0, prev_group = 0;

	pblk_gc_free_full_lines(pblk);

	run_gc = pblk_gc_should_run(&pblk->gc, &pblk->rl);
	if (!run_gc || (atomic_read(&gc->read_inflight_gc) >= PBLK_GC_L_QD))
		return;

next_gc_group:
	group_list = l_mg->gc_lists[gc_group++];

	do {
		spin_lock(&l_mg->gc_lock);
		if (list_empty(group_list)) {
			spin_unlock(&l_mg->gc_lock);
			break;
		}

		if(pblk->rl.rb_state == PBLK_RL_SLC && gc->gc_slc !=1)
			break;

		line = pblk_gc_get_victim_line(pblk, group_list);
		//printk("ocssd[%s]: victim_line_id=%d, gc_slc=%d, gc_group=%d\n", __func__, line->id, pblk->gc.gc_slc, gc_group);

		spin_lock(&line->lock);
		WARN_ON(line->state != PBLK_LINESTATE_CLOSED);
		//printk("ocssd[%s]: victim_line_id=%d, line_state=%d->%d(%s)\n", __func__, line->id, line->state, PBLK_LINESTATE_GC, "LINE_GC");
		line->state = PBLK_LINESTATE_GC;
		spin_unlock(&line->lock);

		list_del(&line->list);
		spin_unlock(&l_mg->gc_lock);

		spin_lock(&gc->r_lock);
		list_add_tail(&line->list, &gc->r_list);
		spin_unlock(&gc->r_lock);

		read_inflight_gc = atomic_inc_return(&gc->read_inflight_gc);
		//printk("ocssd[%s]: wake_up_process gc_reader_ts\n", __func__);
		pblk_gc_reader_kick(gc);

		prev_group = 1;

		/* No need to queue up more GC lines than we can handle */
		run_gc = pblk_gc_should_run(&pblk->gc, &pblk->rl);
		if (!run_gc || read_inflight_gc >= PBLK_GC_L_QD) {
			break;
		}
	} while (1);

	if (!prev_group && pblk->rl.rb_state > gc_group && gc_group < PBLK_GC_NR_LISTS)
		goto next_gc_group;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
static void pblk_gc_timer(unsigned long _arg)
{
	struct pblk *pblk = (struct pblk *)_arg;

	pblk_gc_kick(pblk);
#if (NUMS_SLC_LINE > 0) && (NUMS_SLC_LINE < 1478)
	pblk_rl_update_slc_rates(&pblk->rl);
#endif
}
#else
static void pblk_gc_timer(struct timer_list *t)
{
	struct pblk *pblk = from_timer(pblk, t, gc.gc_timer);

	pblk_gc_kick(pblk);
#if (NUMS_SLC_LINE > 0) && (NUMS_SLC_LINE < 1478)
	pblk_rl_update_slc_rates(&pblk->rl);
#endif
}
#endif

static int pblk_gc_ts(void *data)
{
	struct pblk *pblk = data;

	while (!kthread_should_stop()) {
		pblk_gc_run(pblk);
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

	return 0;
}

static int pblk_gc_writer_ts(void *data)
{
	struct pblk *pblk = data;

	while (!kthread_should_stop()) {
		if (!pblk_gc_write(pblk))
			continue;
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

	return 0;
}

static int pblk_gc_reader_ts(void *data)
{
	struct pblk *pblk = data;
	struct pblk_gc *gc = &pblk->gc;

	while (!kthread_should_stop()) {
		if (!pblk_gc_read(pblk))
			continue;
		set_current_state(TASK_INTERRUPTIBLE);
		io_schedule();
	}

#ifdef CONFIG_NVM_DEBUG
	pr_info("pblk: flushing gc pipeline, %d lines left\n",
			atomic_read(&gc->pipeline_gc));
#endif

	do {
		if (!atomic_read(&gc->pipeline_gc))
			break;

		schedule();

		pr_info("ocssd[%s]:  %d lines left\n", __func__, atomic_read(&gc->pipeline_gc));
		mdelay(1000);
	} while (1);

	return 0;
}

static void pblk_gc_start(struct pblk *pblk)
{
	pblk->gc.gc_active = 1;
	pr_debug("pblk: gc start\n");
}

void pblk_gc_should_start(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

	if (gc->gc_enabled && !gc->gc_active) {
		pblk_gc_start(pblk);
		pblk_gc_kick(pblk);
	}
}

void pblk_gc_slc_start(struct pblk *pblk)
{
	pblk->gc.gc_slc = 1;
}

void pblk_gc_slc_stop(struct pblk *pblk)
{
	pblk->gc.gc_slc = 0;
}

void pblk_gc_should_stop(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;

	if (gc->gc_active && !gc->gc_forced)
		gc->gc_active = 0;
}

void pblk_gc_should_kick(struct pblk *pblk)
{
	pblk_rl_update_rates(&pblk->rl);
}

void pblk_gc_sysfs_state_show(struct pblk *pblk, int *gc_enabled,
			      int *gc_active)
{
	struct pblk_gc *gc = &pblk->gc;

	spin_lock(&gc->lock);
	*gc_enabled = gc->gc_enabled;
	*gc_active = gc->gc_active;
	spin_unlock(&gc->lock);
}

int pblk_gc_sysfs_force(struct pblk *pblk, int force)
{
	struct pblk_gc *gc = &pblk->gc;

	if (force < 0 || force > 1)
		return -EINVAL;

	spin_lock(&gc->lock);
	gc->gc_forced = force;

	if (force)
		gc->gc_enabled = 1;
	else
		gc->gc_enabled = 0;
	spin_unlock(&gc->lock);

	pblk_gc_should_start(pblk);

	return 0;
}

int pblk_gc_init(struct pblk *pblk)
{
	struct pblk_gc *gc = &pblk->gc;
	int ret;

	printk("ocssd[%s]: kthread create: gc_ts\n", __func__);
	gc->gc_ts = kthread_create(pblk_gc_ts, pblk, "pblk-gc-ts");
	if (IS_ERR(gc->gc_ts)) {
		pr_err("pblk: could not allocate GC main kthread\n");
		return PTR_ERR(gc->gc_ts);
	}

	printk("ocssd[%s]: kthread create: gc_writer_ts\n", __func__);
	gc->gc_writer_ts = kthread_create(pblk_gc_writer_ts, pblk, "pblk-gc-writer-ts");
	if (IS_ERR(gc->gc_writer_ts)) {
		pr_err("pblk: could not allocate GC writer kthread\n");
		ret = PTR_ERR(gc->gc_writer_ts);
		goto fail_free_main_kthread;
	}

	printk("ocssd[%s]: kthread create: gc_reader_ts\n", __func__);
	gc->gc_reader_ts = kthread_create(pblk_gc_reader_ts, pblk, "pblk-gc-reader-ts");
	if (IS_ERR(gc->gc_reader_ts)) {
		pr_err("pblk: could not allocate GC reader kthread\n");
		ret = PTR_ERR(gc->gc_reader_ts);
		goto fail_free_writer_kthread;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	setup_timer(&gc->gc_timer, pblk_gc_timer, (unsigned long)pblk);
#else
	timer_setup(&gc->gc_timer, pblk_gc_timer, 0);
#endif
	mod_timer(&gc->gc_timer, jiffies + msecs_to_jiffies(GC_TIME_MSECS));
	printk("ocssd[%s]: gc_timer timer=%dms\n", __func__, GC_TIME_MSECS);

	gc->gc_active = 0;
	gc->gc_forced = 0;
	gc->gc_enabled = 1;
	gc->gc_slc = 0;
	gc->w_entries = 0;
	atomic_set(&gc->read_inflight_gc, 0);
	atomic_set(&gc->pipeline_gc, 0);

	/* Workqueue that reads valid sectors from a line and submit them to the
	 * GC writer to be recycled.
	 */
	gc->gc_line_reader_wq = alloc_workqueue("pblk-gc-line-reader-wq",
			WQ_MEM_RECLAIM | WQ_UNBOUND, PBLK_GC_MAX_READERS);
	if (!gc->gc_line_reader_wq) {
		pr_err("pblk: could not allocate GC line reader workqueue\n");
		ret = -ENOMEM;
		goto fail_free_reader_kthread;
	}

	/* Workqueue that prepare lines for GC */
	gc->gc_reader_wq = alloc_workqueue("pblk-gc-line_wq",
					WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!gc->gc_reader_wq) {
		pr_err("pblk: could not allocate GC reader workqueue\n");
		ret = -ENOMEM;
		goto fail_free_reader_line_wq;
	}

	spin_lock_init(&gc->lock);
	spin_lock_init(&gc->w_lock);
	spin_lock_init(&gc->r_lock);

	sema_init(&gc->gc_sem, PBLK_GC_RQ_QD);

	INIT_LIST_HEAD(&gc->w_list);
	INIT_LIST_HEAD(&gc->r_list);

	return 0;

fail_free_reader_line_wq:
	destroy_workqueue(gc->gc_line_reader_wq);
fail_free_reader_kthread:
	kthread_stop(gc->gc_reader_ts);
fail_free_writer_kthread:
	kthread_stop(gc->gc_writer_ts);
fail_free_main_kthread:
	kthread_stop(gc->gc_ts);

	return ret;
}

void pblk_gc_exit(struct pblk *pblk, bool graceful)
{
	struct pblk_gc *gc = &pblk->gc;

	printk("ocssd[%s]: exit\n", __func__);
	gc->gc_enabled = 0;
	del_timer_sync(&gc->gc_timer);
	gc->gc_active = 0;

	if (gc->gc_ts) {
		printk("ocssd[%s]: stop gc_ts\n", __func__);
		kthread_stop(gc->gc_ts);
	}

	if (gc->gc_reader_ts) {
		printk("ocssd[%s]: stop gc_reader_ts\n", __func__);
		kthread_stop(gc->gc_reader_ts);
	}

	if (graceful) {
		flush_workqueue(gc->gc_reader_wq);
		flush_workqueue(gc->gc_line_reader_wq);
	}

	destroy_workqueue(gc->gc_reader_wq);
	destroy_workqueue(gc->gc_line_reader_wq);

	if (gc->gc_writer_ts) {
		printk("ocssd[%s]: stop gc_writer_ts\n", __func__);
		kthread_stop(gc->gc_writer_ts);
	}
}
