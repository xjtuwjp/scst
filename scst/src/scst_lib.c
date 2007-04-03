/*
 *  scst_lib.c
 *  
 *  Copyright (C) 2004-2006 Vladislav Bolkhovitin <vst@vlnb.net>
 *                 and Leonid Stoljar
 *  
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/cdrom.h>
#include <asm/unistd.h>
#include <asm/string.h>

#ifdef SCST_HIGHMEM
#include <linux/highmem.h>
#endif

#include "scst_debug.h"
#include "scsi_tgt.h"
#include "scst_priv.h"
#include "scst_mem.h"

#include "scst_cdbprobe.h"

static void scst_free_tgt_dev(struct scst_tgt_dev *tgt_dev);

void scst_set_cmd_error_status(struct scst_cmd *cmd, int status)
{
	TRACE_ENTRY();

	cmd->status = status;
	cmd->host_status = DID_OK;

	cmd->data_direction = SCST_DATA_NONE;
	cmd->tgt_resp_flags = SCST_TSC_FLAG_STATUS;
	cmd->resp_data_len = 0;

	TRACE_EXIT();
	return;
}

void scst_set_cmd_error(struct scst_cmd *cmd, int key, int asc, int ascq)
{
	TRACE_ENTRY();

	scst_set_cmd_error_status(cmd, SAM_STAT_CHECK_CONDITION);
	scst_set_sense(cmd->sense_buffer, sizeof(cmd->sense_buffer),
		key, asc, ascq);
	TRACE_BUFFER("Sense set", cmd->sense_buffer, sizeof(cmd->sense_buffer));

	TRACE_EXIT();
	return;
}

void scst_set_cmd_error_sense(struct scst_cmd *cmd, uint8_t *sense, 
	unsigned int len)
{
	TRACE_ENTRY();

	scst_set_cmd_error_status(cmd, SAM_STAT_CHECK_CONDITION);

	memset(cmd->sense_buffer, 0, sizeof(cmd->sense_buffer));
	memcpy(cmd->sense_buffer, sense, min((unsigned long)len, 
		(unsigned long)sizeof(cmd->sense_buffer)));
	TRACE_BUFFER("Sense set", cmd->sense_buffer, sizeof(cmd->sense_buffer));

	TRACE_EXIT();
	return;
}

void scst_set_busy(struct scst_cmd *cmd)
{
	int c = cmd->sess->sess_cmd_count;

	TRACE_ENTRY();

	if ((c <= 1) || (cmd->sess->init_phase != SCST_SESS_IPH_READY))
	{
		scst_set_cmd_error_status(cmd, SAM_STAT_BUSY);
		TRACE_MGMT_DBG("Sending BUSY status to initiator %s "
			"(cmds count %d, queue_type %x, sess->init_phase %d)",
			cmd->sess->initiator_name, c,
			cmd->queue_type, cmd->sess->init_phase);
	} else {
		scst_set_cmd_error_status(cmd, SAM_STAT_TASK_SET_FULL);
		TRACE_MGMT_DBG("Sending QUEUE_FULL status to initiator %s "
			"(cmds count %d, queue_type %x, sess->init_phase %d)",
			cmd->sess->initiator_name, c,
			cmd->queue_type, cmd->sess->init_phase);
	}

	TRACE_EXIT();
	return;
}

void scst_set_resp_data_len(struct scst_cmd *cmd, int resp_data_len)
{
	int i, l;

	TRACE_ENTRY();

	scst_check_restore_sg_buff(cmd);
	cmd->resp_data_len = resp_data_len;

	if (resp_data_len == cmd->bufflen)
		goto out;

	l = 0;
	for(i = 0; i < cmd->sg_cnt; i++) {
		l += cmd->sg[i].length;
		if (l >= resp_data_len) {
			int left = resp_data_len - (l - cmd->sg[i].length);
			TRACE(TRACE_SG, "cmd %p (tag %d), "
				"resp_data_len %d, i %d, cmd->sg[i].length %d, "
				"left %d", cmd, cmd->tag, resp_data_len, i,
				cmd->sg[i].length, left);
			cmd->orig_sg_cnt = cmd->sg_cnt;
			cmd->orig_sg_entry = i;
			cmd->orig_entry_len = cmd->sg[i].length;
			cmd->sg_cnt = (left > 0) ? i+1 : i;
			cmd->sg[i].length = left;
			cmd->sg_buff_modified = 1;
			break;
		}
	}

out:
	TRACE_EXIT();
	return;
}

struct scst_device *scst_alloc_device(int gfp_mask)
{
	struct scst_device *dev;

	TRACE_ENTRY();

	dev = kzalloc(sizeof(*dev), gfp_mask);
	if (dev == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s",
		      "Allocation of scst_device failed");
		goto out;
	}

	spin_lock_init(&dev->dev_lock);
	atomic_set(&dev->on_dev_count, 0);
	INIT_LIST_HEAD(&dev->blocked_cmd_list);
	INIT_LIST_HEAD(&dev->dev_tgt_dev_list);
	INIT_LIST_HEAD(&dev->dev_acg_dev_list);
	init_waitqueue_head(&dev->on_dev_waitQ);
	dev->dev_double_ua_possible = 1;
	dev->dev_serialized = 1;

out:
	TRACE_EXIT_HRES(dev);
	return dev;
}

void scst_free_device(struct scst_device *dev)
{
	TRACE_ENTRY();

#ifdef EXTRACHECKS
	if (!list_empty(&dev->dev_tgt_dev_list) || 
	    !list_empty(&dev->dev_acg_dev_list))
	{
		PRINT_ERROR_PR("%s: dev_tgt_dev_list or dev_acg_dev_list "
			"is not empty!", __FUNCTION__);
		sBUG();
	}
#endif

	kfree(dev);

	TRACE_EXIT();
	return;
}

struct scst_acg_dev *scst_alloc_acg_dev(struct scst_acg *acg,
	struct scst_device *dev, lun_t lun)
{
	struct scst_acg_dev *res;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	res = kmem_cache_alloc(scst_acgd_cachep, GFP_KERNEL);
#else
	res = kmem_cache_zalloc(scst_acgd_cachep, GFP_KERNEL);
#endif
	if (res == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s", "Allocation of scst_acg_dev failed");
		goto out;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	memset(res, 0, sizeof(*res));
#endif

	res->dev = dev;
	res->acg = acg;
	res->lun = lun;
	
out:
	TRACE_EXIT_HRES(res);
	return res;
}

/* The activity supposed to be suspended and scst_mutex held */
void scst_free_acg_dev(struct scst_acg_dev *acg_dev)
{
	TRACE_ENTRY();
	
	TRACE_DBG("Removing acg_dev %p from acg_dev_list and dev_acg_dev_list", 
		acg_dev);
	list_del(&acg_dev->acg_dev_list_entry);
	list_del(&acg_dev->dev_acg_dev_list_entry);
	
	kmem_cache_free(scst_acgd_cachep, acg_dev);
	
	TRACE_EXIT();
	return;
}

/* The activity supposed to be suspended and scst_mutex held */
struct scst_acg *scst_alloc_add_acg(const char *acg_name)
{
	struct scst_acg *acg;

	TRACE_ENTRY();

	acg = kzalloc(sizeof(*acg), GFP_KERNEL);
	if (acg == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s", "Allocation of acg failed");
		goto out;
	}

	INIT_LIST_HEAD(&acg->acg_dev_list);
	INIT_LIST_HEAD(&acg->acg_sess_list);
	INIT_LIST_HEAD(&acg->acn_list);
	acg->acg_name = acg_name;
	
	TRACE_DBG("Adding acg %s to scst_acg_list", acg_name);
	list_add_tail(&acg->scst_acg_list_entry, &scst_acg_list);
	
out:
	TRACE_EXIT_HRES(acg);
	return acg;
}

/* The activity supposed to be suspended and scst_mutex held */
int scst_destroy_acg(struct scst_acg *acg)
{
	struct scst_acn *n, *nn;
	struct scst_acg_dev *acg_dev, *acg_dev_tmp;
	int res = 0;

	TRACE_ENTRY();

	if (!list_empty(&acg->acg_sess_list)) {
		PRINT_ERROR_PR("%s: acg_sess_list is not empty!", __FUNCTION__);
		res = -EBUSY;
		goto out;
	}

	TRACE_DBG("Removing acg %s from scst_acg_list", acg->acg_name);
	list_del(&acg->scst_acg_list_entry);
	
	/* Freeing acg_devs */
	list_for_each_entry_safe(acg_dev, acg_dev_tmp, &acg->acg_dev_list, 
		acg_dev_list_entry)
	{
		struct scst_tgt_dev *tgt_dev, *tt;
		list_for_each_entry_safe(tgt_dev, tt,
			 &acg_dev->dev->dev_tgt_dev_list,
			 dev_tgt_dev_list_entry)
		{
			if (tgt_dev->acg_dev == acg_dev)
				scst_free_tgt_dev(tgt_dev);
		}
		scst_free_acg_dev(acg_dev);
	}

	/* Freeing names */
	list_for_each_entry_safe(n, nn, &acg->acn_list, 
		acn_list_entry)
	{
		list_del(&n->acn_list_entry);
		kfree(n->name);
		kfree(n);
	}
	INIT_LIST_HEAD(&acg->acn_list);

	kfree(acg);
out:
	TRACE_EXIT_RES(res);
	return res;
}

static int scst_create_tgt_threads(struct scst_tgt_dev *tgt_dev)
{
	int res = 0;
	static atomic_t major = ATOMIC_INIT(0);
	char nm[12];

	TRACE_ENTRY();

	if ( !tgt_dev->dev->handler->dedicated_thread)
		goto out;

	spin_lock_init(&tgt_dev->cmd_lists.cmd_list_lock);
	INIT_LIST_HEAD(&tgt_dev->cmd_lists.active_cmd_list);
	init_waitqueue_head(&tgt_dev->cmd_lists.cmd_list_waitQ);

	/* 
	 * Only ONE thread must be run here, otherwise the commands could
	 * be executed out of order !!
	 */

	strncpy(nm, tgt_dev->dev->handler->name, ARRAY_SIZE(nm)-1);
	nm[ARRAY_SIZE(nm)-1] = '\0';
	tgt_dev->thread = kthread_run(scst_cmd_thread, &tgt_dev->cmd_lists,
		"%sd%d", nm, atomic_inc_return(&major));
	if (IS_ERR(tgt_dev->thread)) {
		res = PTR_ERR(tgt_dev->thread);
		PRINT_ERROR_PR("kthread_create() failed: %d", res);
		tgt_dev->thread = NULL;
		goto out;
	}

	down(&scst_suspend_mutex);
	list_add_tail(&tgt_dev->cmd_lists.lists_list_entry,
		&scst_cmd_lists_list);
	up(&scst_suspend_mutex);

	tgt_dev->p_cmd_lists = &tgt_dev->cmd_lists;

out:
	TRACE_EXIT();
	return res;
}

static void scst_stop_tgt_threads(struct scst_tgt_dev *tgt_dev)
{
	int rc;

	TRACE_ENTRY();

	if (tgt_dev->thread == NULL)
		goto out;

	rc = kthread_stop(tgt_dev->thread);
	if (rc < 0) {
		TRACE_MGMT_DBG("kthread_stop() failed: %d", rc);
	}

	if (tgt_dev->p_cmd_lists == &tgt_dev->cmd_lists) {
		down(&scst_suspend_mutex);
		list_del(&tgt_dev->cmd_lists.lists_list_entry);
		up(&scst_suspend_mutex);
	}

out:
	TRACE_EXIT();
	return;
}

/*
 * No spin locks supposed to be held, scst_mutex - held.
 * The activity is suspended.
 */
static struct scst_tgt_dev *scst_alloc_add_tgt_dev(struct scst_session *sess,
	struct scst_acg_dev *acg_dev)
{
	struct scst_tgt_dev *tgt_dev;
	struct scst_device *dev = acg_dev->dev;
	int rc, i;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	tgt_dev = kmem_cache_alloc(scst_tgtd_cachep, GFP_KERNEL);
#else
	tgt_dev = kmem_cache_zalloc(scst_tgtd_cachep, GFP_KERNEL);
#endif
	if (tgt_dev == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s",
		      "Allocation of scst_tgt_dev failed");
		goto out;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	memset(tgt_dev, 0, sizeof(*tgt_dev));
#endif

	tgt_dev->dev = acg_dev->dev;
	tgt_dev->lun = acg_dev->lun;
	tgt_dev->acg_dev = acg_dev;
	tgt_dev->sess = sess;
	atomic_set(&tgt_dev->cmd_count, 0);

	tgt_dev->p_cmd_lists = &scst_main_cmd_lists;
	
	if (dev->scsi_dev != NULL) {
		TRACE(TRACE_DEBUG, "host=%d, channel=%d, id=%d, lun=%d, "
		      "SCST lun=%Ld", dev->scsi_dev->host->host_no, 
		      dev->scsi_dev->channel, dev->scsi_dev->id, 
		      dev->scsi_dev->lun, (uint64_t)tgt_dev->lun);
	}
	else {
		TRACE_MGMT_DBG("Virtual device SCST lun=%Ld", 
		      (uint64_t)tgt_dev->lun);
	}

	spin_lock_init(&tgt_dev->tgt_dev_lock);
	INIT_LIST_HEAD(&tgt_dev->UA_list);
	spin_lock_init(&tgt_dev->thr_data_lock);
	INIT_LIST_HEAD(&tgt_dev->thr_data_list);
	spin_lock_init(&tgt_dev->sn_lock);
	INIT_LIST_HEAD(&tgt_dev->deferred_cmd_list);
	INIT_LIST_HEAD(&tgt_dev->skipped_sn_list);
	INIT_LIST_HEAD(&tgt_dev->hq_cmd_list);
	tgt_dev->expected_sn = 1;
	tgt_dev->num_free_sn_slots = ARRAY_SIZE(tgt_dev->sn_slots);
	tgt_dev->cur_sn_slot = &tgt_dev->sn_slots[0];
	for(i = 0; i < ARRAY_SIZE(tgt_dev->sn_slots); i++)
		atomic_set(&tgt_dev->sn_slots[i], 0);

	if (dev->handler->parse_atomic && 
	    sess->tgt->tgtt->preprocessing_done_atomic) {
	    	if (sess->tgt->tgtt->rdy_to_xfer_atomic)
			__set_bit(SCST_TGT_DEV_AFTER_INIT_WR_ATOMIC,
				&tgt_dev->tgt_dev_flags);
		if (dev->handler->exec_atomic)
			__set_bit(SCST_TGT_DEV_AFTER_INIT_OTH_ATOMIC,
				&tgt_dev->tgt_dev_flags);
	}
	if (dev->handler->exec_atomic) {
		if (sess->tgt->tgtt->rdy_to_xfer_atomic)
			__set_bit(SCST_TGT_DEV_AFTER_RESTART_WR_ATOMIC,
				&tgt_dev->tgt_dev_flags);
		__set_bit(SCST_TGT_DEV_AFTER_RESTART_OTH_ATOMIC,
				&tgt_dev->tgt_dev_flags);
		__set_bit(SCST_TGT_DEV_AFTER_RX_DATA_ATOMIC,
			&tgt_dev->tgt_dev_flags);
	}
	if (dev->handler->dev_done_atomic && 
	    sess->tgt->tgtt->xmit_response_atomic) {
		__set_bit(SCST_TGT_DEV_AFTER_EXEC_ATOMIC,
			&tgt_dev->tgt_dev_flags);
	}

	spin_lock_bh(&scst_temp_UA_lock);
	scst_set_sense(scst_temp_UA, sizeof(scst_temp_UA),
		SCST_LOAD_SENSE(scst_sense_reset_UA));
	scst_alloc_set_UA(tgt_dev, scst_temp_UA, sizeof(scst_temp_UA));
	spin_unlock_bh(&scst_temp_UA_lock);

	tm_dbg_init_tgt_dev(tgt_dev, acg_dev);

	rc = scst_create_tgt_threads(tgt_dev);
	if (rc != 0)
		goto out_free;

	if (dev->handler && dev->handler->attach_tgt) {
		TRACE_DBG("Calling dev handler's attach_tgt(%p)",
		      tgt_dev);
		rc = dev->handler->attach_tgt(tgt_dev);
		TRACE_DBG("%s", "Dev handler's attach_tgt() returned");
		if (rc != 0) {
			PRINT_ERROR_PR("Device handler's %s attach_tgt() "
			    "failed: %d", dev->handler->name, rc);
			goto out_stop_free;
		}
	}
	
	list_add_tail(&tgt_dev->dev_tgt_dev_list_entry, &dev->dev_tgt_dev_list);
	if (dev->dev_reserved)
		__set_bit(SCST_TGT_DEV_RESERVED, &tgt_dev->tgt_dev_flags);

	list_add_tail(&tgt_dev->sess_tgt_dev_list_entry,
		&sess->sess_tgt_dev_list);

out:
	TRACE_EXIT();
	return tgt_dev;

out_stop_free:
	scst_stop_tgt_threads(tgt_dev);

out_free:
	kmem_cache_free(scst_tgtd_cachep, tgt_dev);
	tgt_dev = NULL;
	goto out;
}

static void scst_send_release(struct scst_tgt_dev *tgt_dev);

/* 
 * No locks supposed to be held, scst_mutex - held.
 * The activity is suspended.
 */
void scst_reset_tgt_dev(struct scst_tgt_dev *tgt_dev, int nexus_loss)
{
	struct scst_device *dev = tgt_dev->dev;

	if (dev->dev_reserved &&
	    !test_bit(SCST_TGT_DEV_RESERVED, &tgt_dev->tgt_dev_flags)) 
	{
		/* This is one who holds the reservation */
		struct scst_tgt_dev *tgt_dev_tmp;
		list_for_each_entry(tgt_dev_tmp, &dev->dev_tgt_dev_list,
				    dev_tgt_dev_list_entry) 
		{
			clear_bit(SCST_TGT_DEV_RESERVED,
				    &tgt_dev_tmp->tgt_dev_flags);
		}
		dev->dev_reserved = 0;

		scst_send_release(tgt_dev);
	}

	spin_lock_bh(&scst_temp_UA_lock);
	if (nexus_loss) {
		scst_set_sense(scst_temp_UA, sizeof(scst_temp_UA),
			SCST_LOAD_SENSE(scst_sense_nexus_loss_UA));
	} else {
		scst_set_sense(scst_temp_UA, sizeof(scst_temp_UA),
			SCST_LOAD_SENSE(scst_sense_reset_UA));
	}
	scst_check_set_UA(tgt_dev, scst_temp_UA, sizeof(scst_temp_UA));
	spin_unlock_bh(&scst_temp_UA_lock);
}

/* 
 * No locks supposed to be held, scst_mutex - held.
 * The activity is suspended.
 */
static void scst_free_tgt_dev(struct scst_tgt_dev *tgt_dev)
{
	struct scst_device *dev = tgt_dev->dev;

	TRACE_ENTRY();

	tm_dbg_deinit_tgt_dev(tgt_dev);

	list_del(&tgt_dev->dev_tgt_dev_list_entry);
	list_del(&tgt_dev->sess_tgt_dev_list_entry);

	scst_reset_tgt_dev(tgt_dev, 0);
	scst_free_all_UA(tgt_dev);

	if (dev->handler && dev->handler->detach_tgt) {
		TRACE_DBG("Calling dev handler's detach_tgt(%p)",
		      tgt_dev);
		dev->handler->detach_tgt(tgt_dev);
		TRACE_DBG("%s", "Dev handler's detach_tgt() returned");
	}

	scst_stop_tgt_threads(tgt_dev);

	kmem_cache_free(scst_tgtd_cachep, tgt_dev);

	TRACE_EXIT();
	return;
}

/* The activity supposed to be suspended and scst_mutex held */
int scst_sess_alloc_tgt_devs(struct scst_session *sess)
{
	int res = 0;
	struct scst_acg_dev *acg_dev;
	struct scst_tgt_dev *tgt_dev;

	TRACE_ENTRY();

	INIT_LIST_HEAD(&sess->sess_tgt_dev_list);
	list_for_each_entry(acg_dev, &sess->acg->acg_dev_list, 
		acg_dev_list_entry)
	{
		tgt_dev = scst_alloc_add_tgt_dev(sess, acg_dev);
		if (tgt_dev == NULL) {
			res = -ENOMEM;
			goto out_free;
		}
	}

out:
	TRACE_EXIT();
	return res;

out_free:
	scst_sess_free_tgt_devs(sess);
	goto out;
}

/* scst_mutex supposed to be held and activity suspended */
void scst_sess_free_tgt_devs(struct scst_session *sess)
{
	struct scst_tgt_dev *tgt_dev, *t;

	TRACE_ENTRY();
	
	/* The session is going down, no users, so no locks */
	list_for_each_entry_safe(tgt_dev, t, &sess->sess_tgt_dev_list,
				 sess_tgt_dev_list_entry) 
	{
		scst_free_tgt_dev(tgt_dev);
	}
	INIT_LIST_HEAD(&sess->sess_tgt_dev_list);

	TRACE_EXIT();
	return;
}

/* The activity supposed to be suspended and scst_mutex held */
int scst_acg_add_dev(struct scst_acg *acg, struct scst_device *dev, lun_t lun,
	int read_only)
{
	int res = 0;
	struct scst_acg_dev *acg_dev;
	struct scst_tgt_dev *tgt_dev;
	struct scst_session *sess;
	LIST_HEAD(tmp_tgt_dev_list);
	
	TRACE_ENTRY();
	
	INIT_LIST_HEAD(&tmp_tgt_dev_list);
	
#ifdef EXTRACHECKS
	list_for_each_entry(acg_dev, &acg->acg_dev_list, acg_dev_list_entry) {
		if (acg_dev->dev == dev) {
			PRINT_ERROR_PR("Device is already in group %s", 
				acg->acg_name);
			res = -EINVAL;
			goto out;
		}
	}
#endif
	
	acg_dev = scst_alloc_acg_dev(acg, dev, lun);
	if (acg_dev == NULL) {
		res = -ENOMEM;
		goto out;
	}
	acg_dev->rd_only_flag = read_only;

	TRACE_DBG("Adding acg_dev %p to acg_dev_list and dev_acg_dev_list", 
		acg_dev);
	list_add_tail(&acg_dev->acg_dev_list_entry, &acg->acg_dev_list);
	list_add_tail(&acg_dev->dev_acg_dev_list_entry, &dev->dev_acg_dev_list);
	
	list_for_each_entry(sess, &acg->acg_sess_list, acg_sess_list_entry) 
	{
		tgt_dev = scst_alloc_add_tgt_dev(sess, acg_dev);
		if (tgt_dev == NULL) {
			res = -ENOMEM;
			goto out_free;
		}
		list_add_tail(&tgt_dev->extra_tgt_dev_list_entry,
			      &tmp_tgt_dev_list);
	}

out:
	TRACE_EXIT_RES(res);
	return res;

out_free:
	list_for_each_entry(tgt_dev, &tmp_tgt_dev_list,
			 extra_tgt_dev_list_entry) 
	{
		scst_free_tgt_dev(tgt_dev);
	}
	scst_free_acg_dev(acg_dev);
	goto out;
}

/* The activity supposed to be suspended and scst_mutex held */
int scst_acg_remove_dev(struct scst_acg *acg, struct scst_device *dev)
{
	int res = 0;
	struct scst_acg_dev *acg_dev = NULL, *a;
	struct scst_tgt_dev *tgt_dev, *tt;
	
	TRACE_ENTRY();
	
	list_for_each_entry(a, &acg->acg_dev_list, acg_dev_list_entry) {
		if (a->dev == dev) {
			acg_dev = a;
			break;
		}
	}
	
	if (acg_dev == NULL) {
		PRINT_ERROR_PR("Device is not found in group %s", acg->acg_name);
		res = -EINVAL;
		goto out;
	}

	list_for_each_entry_safe(tgt_dev, tt, &dev->dev_tgt_dev_list,
		 dev_tgt_dev_list_entry) 
	{
		if (tgt_dev->acg_dev == acg_dev)
			scst_free_tgt_dev(tgt_dev);
	}
	scst_free_acg_dev(acg_dev);

out:	
	TRACE_EXIT_RES(res);
	return res;
}

/* scst_mutex supposed to be held */
int scst_acg_add_name(struct scst_acg *acg, const char *name)
{
	int res = 0;
	struct scst_acn *n;
	int len;
	char *nm;
	
	TRACE_ENTRY();

	list_for_each_entry(n, &acg->acn_list, acn_list_entry) 
	{
		if (strcmp(n->name, name) == 0) {
			PRINT_ERROR_PR("Name %s already exists in access "
				"control group %s", name, acg->acg_name);
			res = -EINVAL;
			goto out;
		}
	}
	
	n = kmalloc(sizeof(*n), GFP_KERNEL);
	if (n == NULL) {
		PRINT_ERROR_PR("%s", "Unable to allocate scst_acn");
		res = -ENOMEM;
		goto out;
	}
	
	len = strlen(name);
	nm = kmalloc(len + 1, GFP_KERNEL);
	if (nm == NULL) {
		PRINT_ERROR_PR("%s", "Unable to allocate scst_acn->name");
		res = -ENOMEM;
		goto out_free;
	}
	
	strcpy(nm, name);
	n->name = nm;
	
	list_add_tail(&n->acn_list_entry, &acg->acn_list);

out:
	TRACE_EXIT_RES(res);
	return res;

out_free:
	kfree(n);
	goto out;
}

/* scst_mutex supposed to be held */
int scst_acg_remove_name(struct scst_acg *acg, const char *name)
{
	int res = -EINVAL;
	struct scst_acn *n;
	
	TRACE_ENTRY();
	
	list_for_each_entry(n, &acg->acn_list, acn_list_entry)
	{
		if (strcmp(n->name, name) == 0) {
			list_del(&n->acn_list_entry);
		        kfree(n->name);
		        kfree(n);
			res = 0;
			break;
		}
	}
	
	if (res != 0) {
		PRINT_ERROR_PR("Unable to find name %s in access control "
			"group %s", name, acg->acg_name);
	}

	TRACE_EXIT_RES(res);
	return res;
}

struct scst_cmd *scst_create_prepare_internal_cmd(
	struct scst_cmd *orig_cmd, int bufsize)
{
	struct scst_cmd *res;
	int gfp_mask = scst_cmd_atomic(orig_cmd) ? GFP_ATOMIC : GFP_KERNEL;

	TRACE_ENTRY();

	res = scst_alloc_cmd(gfp_mask);
	if (unlikely(res == NULL)) {
		goto out;
	}

	res->cmd_lists = orig_cmd->cmd_lists;
	res->sess = orig_cmd->sess;
	res->state = SCST_CMD_STATE_SEND_TO_MIDLEV;
	res->atomic = scst_cmd_atomic(orig_cmd);
	res->internal = 1;
	res->tgtt = orig_cmd->tgtt;
	res->tgt = orig_cmd->tgt;
	res->dev = orig_cmd->dev;
	res->tgt_dev = orig_cmd->tgt_dev;
	res->lun = orig_cmd->lun;
	res->queue_type = SCST_CMD_QUEUE_HEAD_OF_QUEUE;
	res->data_direction = SCST_DATA_UNKNOWN;
	res->orig_cmd = orig_cmd;

	res->bufflen = bufsize;
	if (bufsize > 0) {
		if (scst_alloc_space(res) != 0)
			PRINT_ERROR_PR("Unable to create buffer (size %d) for "
				"internal cmd", bufsize);
			goto out_free_res;
	}

out:
	TRACE_EXIT_HRES((unsigned long)res);
	return res;

out_free_res:
	scst_destroy_cmd(res);
	res = NULL;
	goto out;
}

void scst_free_internal_cmd(struct scst_cmd *cmd)
{
	TRACE_ENTRY();

	scst_cmd_put(cmd);

	TRACE_EXIT();
	return;
}

int scst_prepare_request_sense(struct scst_cmd *orig_cmd)
{
	int res = SCST_CMD_STATE_RES_CONT_NEXT;
#define sbuf_size 252
	static const unsigned char request_sense[6] =
	    { REQUEST_SENSE, 0, 0, 0, sbuf_size, 0 };
	struct scst_cmd *rs_cmd;

	TRACE_ENTRY();

	rs_cmd = scst_create_prepare_internal_cmd(orig_cmd, sbuf_size);
	if (rs_cmd != 0)
		goto out_error;

	memcpy(rs_cmd->cdb, request_sense, sizeof(request_sense));
	rs_cmd->cdb_len = sizeof(request_sense);
	rs_cmd->data_direction = SCST_DATA_READ;

	TRACE(TRACE_RETRY, "Adding REQUEST SENSE cmd %p to head of active "
		"cmd list ", rs_cmd);
	spin_lock_irq(&rs_cmd->cmd_lists->cmd_list_lock);
	list_add(&rs_cmd->cmd_list_entry, &rs_cmd->cmd_lists->active_cmd_list);
	spin_unlock_irq(&rs_cmd->cmd_lists->cmd_list_lock);

out:
	TRACE_EXIT_RES(res);
	return res;

out_error:
	res = -1;
	goto out;
#undef sbuf_size
}

struct scst_cmd *scst_complete_request_sense(struct scst_cmd *cmd)
{
	struct scst_cmd *orig_cmd = cmd->orig_cmd;
	uint8_t *buf;
	int len;

	TRACE_ENTRY();

	sBUG_ON(orig_cmd);

	len = scst_get_buf_first(cmd, &buf);

	if ((cmd->status == 0) && SCST_SENSE_VALID(buf) &&
	    (!SCST_NO_SENSE(buf))) 
	{
		TRACE_BUFF_FLAG(TRACE_SCSI, "REQUEST SENSE returned", 
			buf, len);
		memcpy(orig_cmd->sense_buffer, buf,
			(sizeof(orig_cmd->sense_buffer) > len) ?
				len : sizeof(orig_cmd->sense_buffer));
	} else {
		PRINT_ERROR_PR("%s", "Unable to get the sense via "
			"REQUEST SENSE, returning HARDWARE ERROR");
		scst_set_cmd_error(cmd, SCST_LOAD_SENSE(scst_sense_hardw_error));
	}

	scst_put_buf(cmd, buf);

	scst_free_internal_cmd(cmd);

	TRACE_EXIT_HRES((unsigned long)orig_cmd);
	return orig_cmd;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
static void scst_req_done(struct scsi_cmnd *scsi_cmd)
{
	struct scsi_request *req;

	TRACE_ENTRY();

	if (scsi_cmd && (req = scsi_cmd->sc_request)) {
		if (req) {
			if (req->sr_bufflen)
				kfree(req->sr_buffer);
			scsi_release_request(req);
		}
	}

	TRACE_EXIT();
	return;
}

static void scst_send_release(struct scst_tgt_dev *tgt_dev)
{
	struct scsi_request *req;
	struct scsi_device *scsi_dev;
	uint8_t cdb[6];

	TRACE_ENTRY();
	
	if (tgt_dev->dev->scsi_dev == NULL)
		goto out;

	scsi_dev = tgt_dev->dev->scsi_dev;

	req = scsi_allocate_request(scsi_dev, GFP_KERNEL);
	if (req == NULL) {
		PRINT_ERROR_PR("Allocation of scsi_request failed: unable "
			    "to RELEASE device %d:%d:%d:%d",
			    scsi_dev->host->host_no, scsi_dev->channel,
			    scsi_dev->id, scsi_dev->lun);
		goto out;
	}

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = RELEASE;
	cdb[1] = (scsi_dev->scsi_level <= SCSI_2) ?
	    ((scsi_dev->lun << 5) & 0xe0) : 0;
	memcpy(req->sr_cmnd, cdb, sizeof(cdb));
	req->sr_cmd_len = sizeof(cdb);
	req->sr_data_direction = SCST_DATA_NONE;
	req->sr_use_sg = 0;
	req->sr_bufflen = 0;
	req->sr_buffer = NULL;
	req->sr_request->rq_disk = tgt_dev->dev->rq_disk;
	req->sr_sense_buffer[0] = 0;

	TRACE(TRACE_DEBUG | TRACE_SCSI, "Sending RELEASE req %p to SCSI "
		"mid-level", req);
	scst_do_req(req, req->sr_cmnd, (void *)req->sr_buffer, req->sr_bufflen,
		    scst_req_done, SCST_DEFAULT_TIMEOUT, 3);

out:
	TRACE_EXIT();
	return;
}
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18) */
static void scst_send_release(struct scst_tgt_dev *tgt_dev)
{
	struct scsi_device *scsi_dev;
	unsigned char cdb[6];
	unsigned char sense[SCSI_SENSE_BUFFERSIZE];
	int rc;

	TRACE_ENTRY();
	
	if (tgt_dev->dev->scsi_dev == NULL)
		goto out;

	scsi_dev = tgt_dev->dev->scsi_dev;

	memset(cdb, 0, sizeof(cdb));
	cdb[0] = RELEASE;
	cdb[1] = (scsi_dev->scsi_level <= SCSI_2) ?
	    ((scsi_dev->lun << 5) & 0xe0) : 0;

	TRACE(TRACE_DEBUG | TRACE_SCSI, "%s", "Sending RELEASE req to SCSI "
		"mid-level");
	rc = scsi_execute(scsi_dev, cdb, SCST_DATA_NONE, NULL, 0,
			sense, SCST_DEFAULT_TIMEOUT,
			3, GFP_KERNEL);
	if (rc) {
		PRINT_INFO_PR("scsi_execute() failed: %d", rc);
		goto out;
	}

out:
	TRACE_EXIT();
	return;
}
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18) */

struct scst_session *scst_alloc_session(struct scst_tgt *tgt, int gfp_mask,
	const char *initiator_name)
{
	struct scst_session *sess;
	int len;
	char *nm;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	sess = kmem_cache_alloc(scst_sess_cachep, gfp_mask);
#else
	sess = kmem_cache_zalloc(scst_sess_cachep, gfp_mask);
#endif
	if (sess == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s",
		      "Allocation of scst_session failed");
		goto out;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	memset(sess, 0, sizeof(*sess));
#endif

	sess->init_phase = SCST_SESS_IPH_INITING;
	atomic_set(&sess->refcnt, 0);
	INIT_LIST_HEAD(&sess->sess_tgt_dev_list);
	spin_lock_init(&sess->sess_list_lock);
	INIT_LIST_HEAD(&sess->search_cmd_list);
	sess->tgt = tgt;
	INIT_LIST_HEAD(&sess->init_deferred_cmd_list);
	INIT_LIST_HEAD(&sess->init_deferred_mcmd_list);

	len = strlen(initiator_name);
	nm = kmalloc(len + 1, gfp_mask);
	if (nm == NULL) {
		PRINT_ERROR_PR("%s", "Unable to allocate sess->initiator_name");
		goto out_free;
	}
	
	strcpy(nm, initiator_name);
	sess->initiator_name = nm;
	
out:
	TRACE_EXIT();
	return sess;

out_free:
	kmem_cache_free(scst_sess_cachep, sess);
	sess = NULL;
	goto out;
}

void scst_free_session(struct scst_session *sess)
{
	TRACE_ENTRY();

	scst_suspend_activity();
	down(&scst_mutex);

	TRACE_DBG("Removing sess %p from the list", sess);
	list_del(&sess->sess_list_entry);
	TRACE_DBG("Removing session %p from acg %s", sess, sess->acg->acg_name);
	list_del(&sess->acg_sess_list_entry);

	scst_sess_free_tgt_devs(sess);

	wake_up_all(&sess->tgt->unreg_waitQ);

	up(&scst_mutex);
	scst_resume_activity();

	kfree(sess->initiator_name);
	kmem_cache_free(scst_sess_cachep, sess);

	TRACE_EXIT();
	return;
}

void scst_free_session_callback(struct scst_session *sess)
{
	struct completion *c;

	TRACE_ENTRY();

	TRACE_DBG("Freeing session %p", sess);

	c = sess->shutdown_compl;

	if (sess->unreg_done_fn) {
		TRACE_DBG("Calling unreg_done_fn(%p)", sess);
		sess->unreg_done_fn(sess);
		TRACE_DBG("%s", "unreg_done_fn() returned");
	}
	scst_free_session(sess);

	if (c)
		complete_all(c);

	TRACE_EXIT();
	return;
}

void scst_sched_session_free(struct scst_session *sess)
{
	unsigned long flags;

	TRACE_ENTRY();

	spin_lock_irqsave(&scst_mgmt_lock, flags);
	TRACE_DBG("Adding sess %p to scst_sess_mgmt_list", sess);
	list_add_tail(&sess->sess_mgmt_list_entry, &scst_sess_mgmt_list);
	spin_unlock_irqrestore(&scst_mgmt_lock, flags);
	
	wake_up(&scst_mgmt_waitQ);

	TRACE_EXIT();
	return;
}

struct scst_cmd *scst_alloc_cmd(int gfp_mask)
{
	struct scst_cmd *cmd;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	cmd = kmem_cache_alloc(scst_cmd_cachep, gfp_mask);
#else
	cmd = kmem_cache_zalloc(scst_cmd_cachep, gfp_mask);
#endif
	if (cmd == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s", "Allocation of scst_cmd failed");
		goto out;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,17)
	memset(cmd, 0, sizeof(*cmd));
#endif

	atomic_set(&cmd->cmd_ref, 1);
	cmd->cmd_lists = &scst_main_cmd_lists;
	cmd->queue_type = SCST_CMD_QUEUE_SIMPLE;
	cmd->timeout = SCST_DEFAULT_TIMEOUT;
	cmd->retries = 1;
	cmd->data_len = -1;
	cmd->tgt_resp_flags = SCST_TSC_FLAG_STATUS;
	cmd->resp_data_len = -1;

out:
	TRACE_EXIT();
	return cmd;
}

void scst_destroy_put_cmd(struct scst_cmd *cmd)
{
	scst_sess_put(cmd->sess);

	/* At this point tgt_dev can be dead, but the pointer remains not-NULL */
	if (likely(cmd->tgt_dev != NULL))
		__scst_put();

	scst_destroy_cmd(cmd);
	return;
}

/* No locks supposed to be held */
void scst_free_cmd(struct scst_cmd *cmd)
{
	int destroy = 1;

	TRACE_ENTRY();

	if (unlikely(test_bit(SCST_CMD_ABORTED, &cmd->cmd_flags))) {
		TRACE_MGMT_DBG("Freeing aborted cmd %p (scst_cmd_count %d)",
			cmd, atomic_read(&scst_cmd_count));
	}

	sBUG_ON(cmd->blocking);

#if defined(EXTRACHECKS) && (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18))
	if (cmd->scsi_req) {
		PRINT_ERROR_PR("%s: %s", __FUNCTION__, "Cmd with unfreed "
			"scsi_req!");
		scst_release_request(cmd);
	}
#endif

	if (likely(cmd->tgt_dev != NULL))
		atomic_dec(&cmd->tgt_dev->cmd_count);

	/* 
	 * cmd->mgmt_cmnd can't being changed here, since for that it either
	 * must be on search_cmd_list, or cmd_ref must be taken. Both are
	 * false here.
	 */
	if (unlikely(cmd->mgmt_cmnd))
		scst_complete_cmd_mgmt(cmd, cmd->mgmt_cmnd);

	if (unlikely(cmd->internal)) {
		if (cmd->bufflen > 0)
			scst_release_space(cmd);
		scst_destroy_cmd(cmd);
		goto out;
	}

	if (cmd->tgtt->on_free_cmd != NULL) {
		TRACE_DBG("Calling target's on_free_cmd(%p)", cmd);
		cmd->tgtt->on_free_cmd(cmd);
		TRACE_DBG("%s", "Target's on_free_cmd() returned");
	}

	if (likely(cmd->dev != NULL)) {
		struct scst_dev_type *handler = cmd->dev->handler;
		if (handler->on_free_cmd != NULL) {
			TRACE_DBG("Calling dev handler %s on_free_cmd(%p)",
			      handler->name, cmd);
			handler->on_free_cmd(cmd);
			TRACE_DBG("Dev handler %s on_free_cmd() returned",
				handler->name);
		}
	}

	scst_release_space(cmd);

	if (likely(cmd->tgt_dev != NULL)) {
#ifdef EXTRACHECKS
		if (unlikely(!cmd->sent_to_midlev)) {
			PRINT_ERROR_PR("Finishing not executed cmd %p (opcode "
			     "%d, target %s, lun %Ld, sn %d, expected_sn %d)",
			     cmd, cmd->cdb[0], cmd->tgtt->name, (uint64_t)cmd->lun,
			     cmd->sn, cmd->tgt_dev->expected_sn);
			scst_unblock_deferred(cmd->tgt_dev, cmd);
		}
#endif

		if (unlikely(cmd->out_of_sn)) {
			TRACE_SN("Out of SN cmd %p (tag %d, sn %d), "
				"destroy=%d", cmd, cmd->tag, cmd->sn, destroy);
			destroy = test_and_set_bit(SCST_CMD_CAN_BE_DESTROYED,
					&cmd->cmd_flags);
		}
	}

	if (likely(destroy))
		scst_destroy_put_cmd(cmd);

out:
	TRACE_EXIT();
	return;
}

/* No locks supposed to be held. */
void scst_check_retries(struct scst_tgt *tgt)
{
	int need_wake_up = 0;

	TRACE_ENTRY();

	/* 
	 * We don't worry about overflow of finished_cmds, because we check 
	 * only for its change 
	 */
	atomic_inc(&tgt->finished_cmds);
	smp_mb__after_atomic_inc();
	if (unlikely(tgt->retry_cmds > 0)) 
	{
		struct scst_cmd *c, *tc;
		unsigned long flags;

		TRACE(TRACE_RETRY, "Checking retry cmd list (retry_cmds %d)",
		      tgt->retry_cmds);

		spin_lock_irqsave(&tgt->tgt_lock, flags);
		list_for_each_entry_safe(c, tc, &tgt->retry_cmd_list,
				cmd_list_entry)
		{
			tgt->retry_cmds--;

			TRACE(TRACE_RETRY, "Moving retry cmd %p to head of active "
				"cmd list (retry_cmds left %d)", c, tgt->retry_cmds);
			spin_lock(&c->cmd_lists->cmd_list_lock);
			list_move(&c->cmd_list_entry, &c->cmd_lists->active_cmd_list);
			wake_up(&c->cmd_lists->cmd_list_waitQ);
			spin_unlock(&c->cmd_lists->cmd_list_lock);

			need_wake_up++;
			if (need_wake_up >= 2) /* "slow start" */
				break; 
		}
		spin_unlock_irqrestore(&tgt->tgt_lock, flags);
	}

	TRACE_EXIT();
	return;
}

void scst_tgt_retry_timer_fn(unsigned long arg)
{
	struct scst_tgt *tgt = (struct scst_tgt*)arg;
	unsigned long flags;

	TRACE(TRACE_RETRY, "Retry timer expired (retry_cmds %d)",
		tgt->retry_cmds);

	spin_lock_irqsave(&tgt->tgt_lock, flags);
	tgt->retry_timer_active = 0;
	spin_unlock_irqrestore(&tgt->tgt_lock, flags);

	scst_check_retries(tgt);

	TRACE_EXIT();
	return;
}

struct scst_mgmt_cmd *scst_alloc_mgmt_cmd(int gfp_mask)
{
	struct scst_mgmt_cmd *mcmd;

	TRACE_ENTRY();

	mcmd = mempool_alloc(scst_mgmt_mempool, gfp_mask);
	if (mcmd == NULL) {
		PRINT_ERROR_PR("%s", "Allocation of management command "
			"failed, some commands and their data could leak");
		goto out;
	}
	memset(mcmd, 0, sizeof(*mcmd));

out:
	TRACE_EXIT();
	return mcmd;
}

void scst_free_mgmt_cmd(struct scst_mgmt_cmd *mcmd)
{
	unsigned long flags;

	TRACE_ENTRY();

	spin_lock_irqsave(&mcmd->sess->sess_list_lock, flags);
	mcmd->sess->sess_cmd_count--;
	spin_unlock_irqrestore(&mcmd->sess->sess_list_lock, flags);

	scst_sess_put(mcmd->sess);

	if (mcmd->mcmd_tgt_dev != NULL)
		__scst_put();

	mempool_free(mcmd, scst_mgmt_mempool);

	TRACE_EXIT();
	return;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
int scst_alloc_request(struct scst_cmd *cmd)
{
	int res = 0;
	struct scsi_request *req;
	int gm = scst_cmd_atomic(cmd) ? GFP_ATOMIC : GFP_KERNEL;

	TRACE_ENTRY();

	/* cmd->dev->scsi_dev must be non-NULL here */
	req = scsi_allocate_request(cmd->dev->scsi_dev, gm);
	if (req == NULL) {
		TRACE(TRACE_OUT_OF_MEM, "%s",
		      "Allocation of scsi_request failed");
		res = -ENOMEM;
		goto out;
	}

	cmd->scsi_req = req;

	memcpy(req->sr_cmnd, cmd->cdb, cmd->cdb_len);
	req->sr_cmd_len = cmd->cdb_len;
	req->sr_data_direction = cmd->data_direction;
	req->sr_use_sg = cmd->sg_cnt;
	req->sr_bufflen = cmd->bufflen;
	req->sr_buffer = cmd->sg;
	req->sr_request->rq_disk = cmd->dev->rq_disk;
	req->sr_sense_buffer[0] = 0;

	cmd->scsi_req->upper_private_data = cmd;

out:
	TRACE_EXIT();
	return res;
}

void scst_release_request(struct scst_cmd *cmd)
{
	scsi_release_request(cmd->scsi_req);
	cmd->scsi_req = NULL;
}
#endif

int scst_alloc_space(struct scst_cmd *cmd)
{
	int tgt_sg = cmd->tgt->sg_tablesize;
	int ini_sg;
	int gfp_mask;
	int res = -ENOMEM;
	int ini_unchecked_isa_dma, ini_use_clustering;
	int use_clustering = 0;
	struct sgv_pool *pool;
	int atomic = scst_cmd_atomic(cmd);

	TRACE_ENTRY();

	if (cmd->data_buf_alloced) {
		TRACE_MEM("%s", "data_buf_alloced set, returning");
		sBUG_ON(cmd->sg == NULL);
		res = 0;
		goto out;
	}

	gfp_mask = __GFP_NOWARN;
	gfp_mask |= (atomic ? GFP_ATOMIC : GFP_KERNEL);
	pool = &scst_sgv.norm;

	if (cmd->dev->scsi_dev != NULL) {
		ini_sg = cmd->dev->scsi_dev->host->sg_tablesize;
		ini_unchecked_isa_dma = 
			cmd->dev->scsi_dev->host->unchecked_isa_dma;
		ini_use_clustering = 
			(cmd->dev->scsi_dev->host->use_clustering == 
				ENABLE_CLUSTERING);
	}
	else {
		ini_sg = (1 << 15) /* infinite */;
		ini_unchecked_isa_dma = 0;
		ini_use_clustering = 0;
	}

	if ((cmd->tgtt->use_clustering || ini_use_clustering) && 
	    !cmd->tgtt->no_clustering)
	{
		TRACE_MEM("%s", "Use clustering");
		pool = &scst_sgv.norm_clust;
		use_clustering = 1;
	}

	if (cmd->tgtt->unchecked_isa_dma || ini_unchecked_isa_dma) {
		TRACE_MEM("%s", "Use ISA DMA memory");
		gfp_mask |= GFP_DMA;
		pool = &scst_sgv.dma;
	} else {
#ifdef SCST_HIGHMEM
		gfp_mask |= __GFP_HIGHMEM;
		pool = &scst_sgv.highmem;
#endif
	}

	if (cmd->no_sgv) {
		if (atomic)
			goto out;
		cmd->sg = scst_alloc(cmd->bufflen, gfp_mask, use_clustering,	
			&cmd->sg_cnt);
	} else {
		cmd->sg = sgv_pool_alloc(pool, cmd->bufflen, gfp_mask, atomic,
				&cmd->sg_cnt, &cmd->sgv);
	}
	if (cmd->sg == NULL)
		goto out;

	if (unlikely(cmd->sg_cnt > ini_sg)) {
		static int ll;
		if (ll < 10) {
			PRINT_INFO("Unable to complete command due to "
				"underlying device SG IO count limitation "
				"(requested %d, available %d)", cmd->sg_cnt,
				ini_sg);
			ll++;
		}
		goto out_sg_free;
	}

	if (unlikely(cmd->sg_cnt > tgt_sg)) {
		static int ll;
		if (ll < 10) {
			PRINT_INFO("Unable to complete command due to "
				"target device %s SG IO count limitation "
				"(requested %d, available %d)", cmd->tgtt->name,
				cmd->sg_cnt, tgt_sg);
			ll++;
		}
		goto out_sg_free;
	}
	
	res = 0;

out:
	TRACE_EXIT();
	return res;

out_sg_free:
	if (cmd->no_sgv)
		scst_free(cmd->sg, cmd->sg_cnt);
	else
		sgv_pool_free(cmd->sgv);
	cmd->sgv = NULL;
	cmd->sg = NULL;
	cmd->sg_cnt = 0;
	goto out;
}

void scst_release_space(struct scst_cmd *cmd)
{
	TRACE_ENTRY();

	if (cmd->data_buf_alloced) {
		TRACE_MEM("%s", "data_buf_alloced set, returning");
		goto out;
	}

	if (cmd->sgv) {
		scst_check_restore_sg_buff(cmd);
		sgv_pool_free(cmd->sgv);
	} else if (cmd->sg)
		scst_free(cmd->sg, cmd->sg_cnt);

	cmd->sgv = NULL;
	cmd->sg_cnt = 0;
	cmd->sg = NULL;
	cmd->bufflen = 0;
	cmd->data_len = 0;

out:
	TRACE_EXIT();
	return;
}

int __scst_get_buf(struct scst_cmd *cmd, uint8_t **buf)
{
	int res = 0;
	struct scatterlist *sg = cmd->sg;
	int i = cmd->get_sg_buf_entry_num;
	
	TRACE_ENTRY();
	
	*buf = NULL;
	
	if (i >= cmd->sg_cnt)
		goto out;
#ifdef SCST_HIGHMEM
	/* 
	 * HIGHMEM pages not merged (clustered), so if it's 
	 * not HIGHMEM page, kmap() is the same as page_address()
	 */
	if (scst_cmd_atomic(cmd)) {
		enum km_type km;
		if (in_softirq())
			km = KM_SOFTIRQ0;
		else
			km = KM_USER0;
		*buf = kmap_atomic(sg[i].page, km);
	} else
		*buf = kmap(sg[i].page);
#else
	*buf = page_address(sg[i].page);
#endif
	*buf += sg[i].offset;
	res = sg[i].length;
	cmd->get_sg_buf_entry_num++;
	
out:
	TRACE_EXIT_RES(res);
	return res;
}

static const int SCST_CDB_LENGTH[8] = { 6, 10, 10, -1, 16, 12, -1, -1 };

#define SCST_CDB_GROUP(opcode)   ((opcode >> 5) & 0x7)
#define SCST_GET_CDB_LEN(opcode) SCST_CDB_LENGTH[SCST_CDB_GROUP(opcode)]

int scst_get_cdb_len(const uint8_t *cdb)
{
	return SCST_GET_CDB_LEN(cdb[0]);
}

/* get_trans_len_x extract x bytes from cdb as length starting from off */

static uint32_t get_trans_len_1(const uint8_t *cdb, uint8_t off)
{
	u32 len;

	len = (u32)cdb[off];
	return len;
}

static uint32_t get_trans_len_2(const uint8_t *cdb, uint8_t off)
{
	const uint8_t *p = cdb + off;
	u32 len = 0;

	len |= ((u32)p[0]) << 8;
	len |= ((u32)p[1]);
	return len;
}

static uint32_t get_trans_len_3(const uint8_t *cdb, uint8_t off)
{
	const uint8_t *p = cdb + off;
	u32 len = 0;

	len |= ((u32)p[0]) << 16;
	len |= ((u32)p[1]) << 8;
	len |= ((u32)p[2]);
	return len;
}

static uint32_t get_trans_len_4(const uint8_t *cdb, uint8_t off)
{
	const uint8_t *p = cdb + off;
	u32 len = 0;

	len |= ((u32)p[0]) << 24;
	len |= ((u32)p[1]) << 16;
	len |= ((u32)p[2]) << 8;
	len |= ((u32)p[3]);
	return len;
}

/* for special commands */
static uint32_t get_trans_len_block_limit(const uint8_t *cdb, uint8_t off)
{
	return 6;
}

static uint32_t get_trans_len_read_capacity(const uint8_t *cdb, uint8_t off)
{
	return 8;
}

static uint32_t get_trans_len_single(const uint8_t *cdb, uint8_t off)
{
	return 1;
}

static uint32_t get_trans_len_none(const uint8_t *cdb, uint8_t off)
{
	return 0;
}

int scst_get_cdb_info(const uint8_t *cdb_p, int dev_type,
		      struct scst_info_cdb *info_p)
{
	int i, res = 0;
	uint8_t op;
	const struct scst_sdbops *ptr = NULL;

	TRACE_ENTRY();

	op = *cdb_p;	/* get clear opcode */

	TRACE_DBG("opcode=%02x, cdblen=%d bytes, tblsize=%zd, "
		"dev_type=%d", op, SCST_GET_CDB_LEN(op), SCST_CDB_TBL_SIZE,
		dev_type);

	i = scst_scsi_op_list[op];
	while (i < SCST_CDB_TBL_SIZE && scst_scsi_op_table[i].ops == op) {
		if (scst_scsi_op_table[i].devkey[dev_type] != SCST_CDB_NOTSUPP) {
			ptr = &scst_scsi_op_table[i];
			TRACE_DBG("op = 0x%02x+'%c%c%c%c%c%c%c%c%c%c'+<%s>", 
			      ptr->ops, ptr->devkey[0],	/* disk     */
			      ptr->devkey[1],	/* tape     */
			      ptr->devkey[2],	/* printer */
			      ptr->devkey[3],	/* cpu      */
			      ptr->devkey[4],	/* cdr      */
			      ptr->devkey[5],	/* cdrom    */
			      ptr->devkey[6],	/* scanner */
			      ptr->devkey[7],	/* worm     */
			      ptr->devkey[8],	/* changer */
			      ptr->devkey[9],	/* commdev */
			      ptr->op_name);
			TRACE_DBG("direction=%d flags=%d off=%d",
			      ptr->direction,
			      ptr->flags,
			      ptr->off);
			break;
		}
		i++;
	}

	if (ptr == NULL) {
		/* opcode not found or now not used !!! */
		TRACE(TRACE_SCSI, "Unknown opcode 0x%x for type %d", op,
		      dev_type);
		res = -1;
		goto out;
	}

	info_p->cdb_len = SCST_GET_CDB_LEN(op);
	info_p->op_name = ptr->op_name;
	info_p->direction = ptr->direction;
	info_p->flags = ptr->flags;
	info_p->transfer_len = (*ptr->get_trans_len)(cdb_p, ptr->off);

#ifdef EXTRACHECKS
	if (unlikely((info_p->transfer_len == 0) &&
		     (info_p->direction != SCST_DATA_NONE))) {
		TRACE_DBG("Warning! transfer_len 0, direction %d change on %d",
			info_p->direction, SCST_DATA_NONE);
		info_p->direction = SCST_DATA_NONE;
	}
#endif

out:
	TRACE_EXIT();
	return res;
}

/*
 * Routine to extract a lun number from an 8-byte LUN structure
 * in network byte order (BE).
 * (see SAM-2, Section 4.12.3 page 40)
 * Supports 2 types of lun unpacking: peripheral and logical unit.
 */
lun_t scst_unpack_lun(const uint8_t *lun, int len)
{
	lun_t res = (lun_t)-1;
	int address_method;

	TRACE_ENTRY();

	TRACE_BUFF_FLAG(TRACE_DEBUG, "Raw LUN", lun, len);

	if (len < 2) {
		PRINT_ERROR_PR("Illegal lun length %d, expected 2 bytes or "
			"more", len);
		goto out;
	}

	if (len > 2) {
		switch(len) {
		case 8:
		{
			if ((*((uint64_t*)lun) & 
			  __constant_cpu_to_be64(0x0000FFFFFFFFFFFFLL)) != 0)
				goto out_err;
			break;
		}
		case 4:
			if (*((uint16_t*)&lun[2]) != 0)
				goto out_err;
			break;
		case 6:
			if (*((uint32_t*)&lun[2]) != 0)
				goto out_err;
			break;
		default:
			goto out_err;
		}
	}

	address_method = (*lun) >> 6;	/* high 2 bits of byte 0 */
	switch (address_method) {
	case 0:	/* peripheral device addressing method */
#if 0 /* Looks like it's legal to use it as flat space addressing method as well */
		if (*lun) {
			PRINT_ERROR_PR("Illegal BUS INDENTIFIER in LUN "
			     "peripheral device addressing method 0x%02x, "
			     "expected 0", *lun);
			break;
		}
		res = *(lun + 1);
		break;
#else
		/* go through */
#endif

	case 1:	/* flat space addressing method */
		res = *(lun + 1) | (((*lun) & 0x3f) << 8);
		break;

	case 2:	/* logical unit addressing method */
		if (*lun & 0x3f) {
			PRINT_ERROR_PR("Illegal BUS NUMBER in LUN logical unit "
				    "addressing method 0x%02x, expected 0",
				    *lun & 0x3f);
			break;
		}
		if (*(lun + 1) & 0xe0) {
			PRINT_ERROR_PR("Illegal TARGET in LUN logical unit "
				    "addressing method 0x%02x, expected 0",
				    (*(lun + 1) & 0xf8) >> 5);
			break;
		}
		res = *(lun + 1) & 0x1f;
		break;

	case 3:	/* extended logical unit addressing method */
	default:
		PRINT_ERROR_PR("Unimplemented LUN addressing method %u",
			    address_method);
		break;
	}

out:
	TRACE_EXIT_RES((int)res);
	return res;

out_err:
	PRINT_ERROR_PR("%s", "Multi-level LUN unimplemented");
	goto out;
}

int scst_calc_block_shift(int sector_size)
{
	int block_shift = 0;
	int t = sector_size;

	if (sector_size == 0)
		sector_size = 512;

	while(1) {
		if ((t & 1) != 0)
			break;
		t >>= 1;
		block_shift++;
	}
	if (block_shift < 9) {
		PRINT_ERROR_PR("Wrong sector size %d", sector_size);
		block_shift = -1;
	} 

	TRACE_EXIT_RES(block_shift);
	return block_shift;
}

int scst_sbc_generic_parse(struct scst_cmd *cmd,
	struct scst_info_cdb *info_cdb, int block_shift)
{
	int res = 0;

	TRACE_ENTRY();
	
	/*
	 * SCST sets good defaults for cmd->data_direction and cmd->bufflen
	 * based on info_cdb, therefore change them only if necessary
	 */

	TRACE_DBG("op_name <%s> direct %d flags %d transfer_len %d",
	      info_cdb->op_name,
	      info_cdb->direction, info_cdb->flags, info_cdb->transfer_len);

	switch (cmd->cdb[0]) {
	case READ_CAPACITY:
		cmd->bufflen = READ_CAP_LEN;
		cmd->data_direction = SCST_DATA_READ;
		break;
	case SERVICE_ACTION_IN:
		if ((cmd->cdb[1] & 0x1f) == SAI_READ_CAPACITY_16) {
			cmd->bufflen = READ_CAP16_LEN;
			cmd->data_direction = SCST_DATA_READ;
		}
		break;
	case VERIFY_6:
	case VERIFY:
	case VERIFY_12:
	case VERIFY_16:
		if ((cmd->cdb[1] & BYTCHK) == 0) {
			cmd->data_len = 
			    info_cdb->transfer_len << block_shift;
			cmd->bufflen = 0;
			cmd->data_direction = SCST_DATA_NONE;
			info_cdb->flags &= ~SCST_TRANSFER_LEN_TYPE_FIXED;
		} else
			cmd->data_len = 0;
		break;
	default:
		/* It's all good */
		break;
	}

	if (info_cdb->flags & SCST_TRANSFER_LEN_TYPE_FIXED) {
		/* 
		 * No need for locks here, since *_detach() can not be
		 * called, when there are existing commands.
		 */
		cmd->bufflen = info_cdb->transfer_len << block_shift;
	}

	TRACE_DBG("res %d, bufflen %zd, data_len %zd, direct %d",
	      res, cmd->bufflen, cmd->data_len, cmd->data_direction);

	TRACE_EXIT_RES(res);
	return res;
}

int scst_cdrom_generic_parse(struct scst_cmd *cmd,
	struct scst_info_cdb *info_cdb, int block_shift)
{
	int res = 0;

	TRACE_ENTRY();

	/*
	 * SCST sets good defaults for cmd->data_direction and cmd->bufflen
	 * based on info_cdb, therefore change them only if necessary
	 */

	TRACE_DBG("op_name <%s> direct %d flags %d transfer_len %d lun %d(%d)",
	      info_cdb->op_name,
	      info_cdb->direction,
	      info_cdb->flags,
	      info_cdb->transfer_len, cmd->lun, (cmd->cdb[1] >> 5) & 7);

	cmd->cdb[1] &= 0x1f;

	switch (cmd->cdb[0]) {
	case READ_CAPACITY:
		cmd->bufflen = READ_CAP_LEN;
		cmd->data_direction = SCST_DATA_READ;
		break;
	case GPCMD_SET_STREAMING:
		cmd->bufflen = (((*(cmd->cdb + 9)) & 0xff) << 8) +
		    ((*(cmd->cdb + 10)) & 0xff);
		cmd->bufflen &= 0xffff;
		break;
	case GPCMD_READ_CD:
		cmd->bufflen = cmd->bufflen >> 8;
		break;
	case VERIFY_6:
	case VERIFY:
	case VERIFY_12:
	case VERIFY_16:
		if ((cmd->cdb[1] & BYTCHK) == 0) {
			cmd->data_len = 
			    info_cdb->transfer_len << block_shift;
			cmd->bufflen = 0;
			cmd->data_direction = SCST_DATA_NONE;
			info_cdb->flags &= ~SCST_TRANSFER_LEN_TYPE_FIXED;
		}
		break;
	default:
		/* It's all good */
		break;
	}

	if (info_cdb->flags & SCST_TRANSFER_LEN_TYPE_FIXED)
		cmd->bufflen = info_cdb->transfer_len << block_shift;

	TRACE_DBG("res %d bufflen %zd direct %d",
	      res, cmd->bufflen, cmd->data_direction);

	TRACE_EXIT();
	return res;
}

int scst_modisk_generic_parse(struct scst_cmd *cmd,
	struct scst_info_cdb *info_cdb, int block_shift)
{
	int res = 0;

	TRACE_ENTRY();

	/*
	 * SCST sets good defaults for cmd->data_direction and cmd->bufflen
	 * based on info_cdb, therefore change them only if necessary
	 */

	TRACE_DBG("op_name <%s> direct %d flags %d transfer_len %d lun %d(%d)",
	      info_cdb->op_name,
	      info_cdb->direction,
	      info_cdb->flags,
	      info_cdb->transfer_len, cmd->lun, (cmd->cdb[1] >> 5) & 7);

	cmd->cdb[1] &= 0x1f;

	switch (cmd->cdb[0]) {
	case READ_CAPACITY:
		cmd->bufflen = READ_CAP_LEN;
		cmd->data_direction = SCST_DATA_READ;
		break;
	case 0xB6 /* SET_STREAMING */ :
		cmd->bufflen = (((*(cmd->cdb + 9)) & 0xff) << 8) +
		    ((*(cmd->cdb + 10)) & 0xff);
		cmd->bufflen &= 0xffff;
		break;
	case 0xBE /* READ_CD */ :
		cmd->bufflen = cmd->bufflen >> 8;
		break;
	case VERIFY_6:
	case VERIFY:
	case VERIFY_12:
	case VERIFY_16:
		if ((cmd->cdb[1] & BYTCHK) == 0) {
			cmd->data_len = 
			    info_cdb->transfer_len << block_shift;
			cmd->bufflen = 0;
			cmd->data_direction = SCST_DATA_NONE;
			info_cdb->flags &= ~SCST_TRANSFER_LEN_TYPE_FIXED;
		}
		break;
	default:
		/* It's all good */
		break;
	}

	if (info_cdb->flags & SCST_TRANSFER_LEN_TYPE_FIXED)
		cmd->bufflen = info_cdb->transfer_len << block_shift;;

	TRACE_DBG("res %d bufflen %zd direct %d",
	      res, cmd->bufflen, cmd->data_direction);

	TRACE_EXIT_RES(res);
	return res;
}

int scst_tape_generic_parse(struct scst_cmd *cmd,
	struct scst_info_cdb *info_cdb, int block_size)
{
	int res = 0;

	TRACE_ENTRY();

	/*
	 * SCST sets good defaults for cmd->data_direction and cmd->bufflen
	 * based on info_cdb, therefore change them only if necessary
	 */

	TRACE_DBG("op_name <%s> direct %d flags %d transfer_len %d",
	      info_cdb->op_name,
	      info_cdb->direction, info_cdb->flags, info_cdb->transfer_len);

	if (cmd->cdb[0] == READ_POSITION) {
		int tclp = cmd->cdb[1] & TCLP_BIT;
		int long_bit = cmd->cdb[1] & LONG_BIT;
		int bt = cmd->cdb[1] & BT_BIT;

		if ((tclp == long_bit) && (!bt || !long_bit)) {
			cmd->bufflen =
			    tclp ? POSITION_LEN_LONG : POSITION_LEN_SHORT;
			cmd->data_direction = SCST_DATA_READ;
		} else {
			cmd->bufflen = 0;
			cmd->data_direction = SCST_DATA_NONE;
		}
	}

	if (info_cdb->flags & SCST_TRANSFER_LEN_TYPE_FIXED & cmd->cdb[1])
		cmd->bufflen = info_cdb->transfer_len * block_size;

	TRACE_EXIT_RES(res);
	return res;
}

int scst_null_parse(struct scst_cmd *cmd, struct scst_info_cdb *info_cdb)
{
	int res = 0;

	TRACE_ENTRY();

	/*
	 * SCST sets good defaults for cmd->data_direction and cmd->bufflen
	 * based on info_cdb, therefore change them only if necessary
	 */

	TRACE_DBG("op_name <%s> direct %d flags %d transfer_len %d",
	      info_cdb->op_name,
	      info_cdb->direction, info_cdb->flags, info_cdb->transfer_len);
#if 0
	switch (cmd->cdb[0]) {
	default:
		/* It's all good */
		break;
	}
#endif
	TRACE_DBG("res %d bufflen %zd direct %d",
	      res, cmd->bufflen, cmd->data_direction);

	TRACE_EXIT();
	return res;
}

/* Called under dev_lock and BH off */
void scst_process_reset(struct scst_device *dev,
	struct scst_session *originator, struct scst_cmd *exclude_cmd,
	struct scst_mgmt_cmd *mcmd)
{
	struct scst_tgt_dev *tgt_dev;
	struct scst_cmd *cmd, *tcmd;

	TRACE_ENTRY();

	/* Clear RESERVE'ation, if necessary */
	if (dev->dev_reserved) {
		/* Either scst_mutex held or exclude_cmd non-NULL */
		list_for_each_entry(tgt_dev, &dev->dev_tgt_dev_list,
				    dev_tgt_dev_list_entry) 
		{
			TRACE(TRACE_MGMT, "Clearing RESERVE'ation for tgt_dev "
				"lun %d", tgt_dev->lun);
			clear_bit(SCST_TGT_DEV_RESERVED,
				  &tgt_dev->tgt_dev_flags);
		}
		dev->dev_reserved = 0;
		/*
		 * There is no need to send RELEASE, since the device is going
		 * to be resetted
		 */
	}

	dev->dev_double_ua_possible = 1;
	dev->dev_serialized = 1;

	/* BH already off */
	spin_lock(&scst_temp_UA_lock);
	scst_set_sense(scst_temp_UA, sizeof(scst_temp_UA),
		SCST_LOAD_SENSE(scst_sense_reset_UA));
	__scst_process_UA(dev, exclude_cmd, scst_temp_UA, sizeof(scst_temp_UA),
		1);
	spin_unlock(&scst_temp_UA_lock);

	list_for_each_entry(tgt_dev, &dev->dev_tgt_dev_list, 
		dev_tgt_dev_list_entry) 
	{
		struct scst_session *sess = tgt_dev->sess;

		spin_lock_irq(&sess->sess_list_lock);

		TRACE_DBG("Searching in search cmd list (sess=%p)", sess);
		list_for_each_entry(cmd, &sess->search_cmd_list, 
				search_cmd_list_entry) {
			if (cmd == exclude_cmd)
				continue;
			if ((cmd->tgt_dev == tgt_dev) ||
			    ((cmd->tgt_dev == NULL) && 
			     (cmd->lun == tgt_dev->lun))) {
			        scst_abort_cmd(cmd, mcmd, 
			        	(tgt_dev->sess != originator), 0);
			}
		}
		spin_unlock_irq(&sess->sess_list_lock);
	}

	list_for_each_entry_safe(cmd, tcmd, &dev->blocked_cmd_list,
				blocked_cmd_list_entry) {
		if (test_bit(SCST_CMD_ABORTED, &cmd->cmd_flags)) {
			list_del(&cmd->blocked_cmd_list_entry);
			TRACE_MGMT_DBG("Adding aborted blocked cmd %p "
				"to active cmd list", cmd);
			spin_lock_irq(&cmd->cmd_lists->cmd_list_lock);
			list_add_tail(&cmd->cmd_list_entry,
				&cmd->cmd_lists->active_cmd_list);
			wake_up(&cmd->cmd_lists->cmd_list_waitQ);
			spin_unlock_irq(&cmd->cmd_lists->cmd_list_lock);
		}
	}

	TRACE_EXIT();
	return;
}

int scst_set_pending_UA(struct scst_cmd *cmd)
{
	int res = 0;
	struct scst_tgt_dev_UA *UA_entry;

	TRACE_ENTRY();

	TRACE(TRACE_MGMT, "Setting pending UA cmd %p", cmd);

	spin_lock_bh(&cmd->tgt_dev->tgt_dev_lock);

	/* UA list could be cleared behind us, so retest */
	if (list_empty(&cmd->tgt_dev->UA_list)) {
		TRACE_DBG("%s",
		      "SCST_TGT_DEV_UA_PENDING set, but UA_list empty");
		res = -1;
		goto out_unlock;
	}

	UA_entry = list_entry(cmd->tgt_dev->UA_list.next, typeof(*UA_entry),
			      UA_list_entry);

	TRACE_DBG("next %p UA_entry %p",
	      cmd->tgt_dev->UA_list.next, UA_entry);

	scst_set_cmd_error_sense(cmd, UA_entry->UA_sense_buffer,
		sizeof(UA_entry->UA_sense_buffer));

	cmd->ua_ignore = 1;

	list_del(&UA_entry->UA_list_entry);

	mempool_free(UA_entry, scst_ua_mempool);

	if (list_empty(&cmd->tgt_dev->UA_list)) {
		clear_bit(SCST_TGT_DEV_UA_PENDING,
			  &cmd->tgt_dev->tgt_dev_flags);
	}

	spin_unlock_bh(&cmd->tgt_dev->tgt_dev_lock);

out:
	TRACE_EXIT_RES(res);
	return res;

out_unlock:
	spin_unlock_bh(&cmd->tgt_dev->tgt_dev_lock);
	goto out;
}

/* Called under dev_lock, tgt_dev_lock and BH off */
void scst_alloc_set_UA(struct scst_tgt_dev *tgt_dev,
	const uint8_t *sense, int sense_len)
{
	struct scst_tgt_dev_UA *UA_entry = NULL;

	TRACE_ENTRY();

	UA_entry = mempool_alloc(scst_ua_mempool, GFP_ATOMIC);
	if (UA_entry == NULL) {
		PRINT_ERROR_PR("%s", "UNIT ATTENTION memory "
		     "allocation failed. The UNIT ATTENTION "
		     "on some sessions will be missed");
		goto out;
	}
	memset(UA_entry, 0, sizeof(*UA_entry));

	if (sense_len > sizeof(UA_entry->UA_sense_buffer))
		sense_len = sizeof(UA_entry->UA_sense_buffer);
	memcpy(UA_entry->UA_sense_buffer, sense, sense_len);
	set_bit(SCST_TGT_DEV_UA_PENDING, &tgt_dev->tgt_dev_flags);
	smp_mb__after_set_bit();
	list_add_tail(&UA_entry->UA_list_entry, &tgt_dev->UA_list);

out:
	TRACE_EXIT();
	return;
}

/* Called under dev_lock and BH off */
void scst_check_set_UA(struct scst_tgt_dev *tgt_dev,
	const uint8_t *sense, int sense_len)
{
	int skip_UA = 0;
	struct scst_tgt_dev_UA *UA_entry_tmp;

	TRACE_ENTRY();

	spin_lock(&tgt_dev->tgt_dev_lock);

	list_for_each_entry(UA_entry_tmp, &tgt_dev->UA_list,
			    UA_list_entry) 
	{
		if (sense[12] == UA_entry_tmp->UA_sense_buffer[12]) {
			skip_UA = 1;
			break;
		}
	}

	if (skip_UA == 0)
		scst_alloc_set_UA(tgt_dev, sense, sense_len);

	spin_unlock(&tgt_dev->tgt_dev_lock);

	TRACE_EXIT();
	return;
}

/* Called under dev_lock and BH off */
void __scst_process_UA(struct scst_device *dev,
	struct scst_cmd *exclude, const uint8_t *sense, int sense_len,
	int internal)
{
	struct scst_tgt_dev *tgt_dev, *exclude_tgt_dev = NULL;

	TRACE_ENTRY();

	TRACE(TRACE_MGMT, "Processing UA dev %p", dev);

	if (exclude != NULL)
		exclude_tgt_dev = exclude->tgt_dev;

	/* Check for reset UA */
	if (!internal && (sense[12] == SCST_SENSE_ASC_UA_RESET)) {
		scst_process_reset(dev, (exclude != NULL) ? exclude->sess : NULL,
			exclude, NULL);
	}

	list_for_each_entry(tgt_dev, &dev->dev_tgt_dev_list, 
				dev_tgt_dev_list_entry) {
		if (tgt_dev != exclude_tgt_dev)
			scst_check_set_UA(tgt_dev, sense, sense_len);
	}

	TRACE_EXIT();
	return;
}

/* Called under tgt_dev_lock or when tgt_dev is unused */
void scst_free_all_UA(struct scst_tgt_dev *tgt_dev)
{
	struct scst_tgt_dev_UA *UA_entry, *t;

	TRACE_ENTRY();

	list_for_each_entry_safe(UA_entry, t, &tgt_dev->UA_list, UA_list_entry) {
		TRACE_MGMT_DBG("Clearing UA for tgt_dev lun %d", 
			tgt_dev->lun);
		list_del(&UA_entry->UA_list_entry);
		kfree(UA_entry);
	}
	INIT_LIST_HEAD(&tgt_dev->UA_list);
	clear_bit(SCST_TGT_DEV_UA_PENDING, &tgt_dev->tgt_dev_flags);

	TRACE_EXIT();
	return;
}

/* sn_lock supposed to be held and IRQ off */
static inline int __scst_check_hq_cmd(struct scst_cmd *cmd)
{
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	struct scst_cmd *hq;
	int res;

	TRACE_ENTRY();

	/* According to SAM, the latest HQ cmd shall pass first */
	hq = list_entry(tgt_dev->hq_cmd_list.next, typeof(*hq),
			      sn_cmd_list_entry);
	if ((cmd == hq) && !test_bit(SCST_TGT_DEV_HQ_ACTIVE,
				&tgt_dev->tgt_dev_flags)) {
		TRACE_SN("Passing HQ cmd %p", cmd);
		res = 1;
		list_del(&cmd->sn_cmd_list_entry);
		set_bit(SCST_TGT_DEV_HQ_ACTIVE, &tgt_dev->tgt_dev_flags);
	} else {
		TRACE_SN("Defer HQ cmd %p", cmd);
		res = 0;
		cmd->hq_deferred = 1;
		tgt_dev->def_cmd_count++;
	}

	TRACE_EXIT_RES(res);
	return res;
}

int scst_check_hq_cmd(struct scst_cmd *cmd)
{
	struct scst_tgt_dev *tgt_dev = cmd->tgt_dev;
	int res;

	spin_lock_irq(&tgt_dev->sn_lock);
	res = __scst_check_hq_cmd(cmd);
	spin_unlock_irq(&tgt_dev->sn_lock);

	return res;
}

/* No locks */
struct scst_cmd *__scst_check_deferred_commands(struct scst_tgt_dev *tgt_dev)
{
	struct scst_cmd *res = NULL, *cmd, *t;
	int expected_sn = tgt_dev->expected_sn;

	spin_lock_irq(&tgt_dev->sn_lock);

	if (unlikely(test_bit(SCST_TGT_DEV_HQ_ACTIVE, &tgt_dev->tgt_dev_flags))) {
		if (!list_empty(&tgt_dev->hq_cmd_list)) {
			int rc;
			cmd = list_entry(tgt_dev->hq_cmd_list.next,
				typeof(*cmd), sn_cmd_list_entry);
			if (cmd->hq_deferred) {
				TRACE_SN("Releasing deferred HQ cmd %p", cmd);
				tgt_dev->def_cmd_count--;
				cmd->hq_deferred = 0;
				res = cmd;
				/* 
				 * Since __scst_check_hq_cmd() is inline, a lot
				 * of code should be optimized out
				 */
				clear_bit(SCST_TGT_DEV_HQ_ACTIVE,
					&tgt_dev->tgt_dev_flags);
				rc = __scst_check_hq_cmd(res);
				EXTRACHECKS_BUG_ON(rc != 1);
				goto out_unlock;
			}
		}
		TRACE_SN("Turning OFF hq_cmd_active (tgt_dev %p)",
			tgt_dev);
		clear_bit(SCST_TGT_DEV_HQ_ACTIVE, &tgt_dev->tgt_dev_flags);
	}

restart:
	list_for_each_entry_safe(cmd, t, &tgt_dev->deferred_cmd_list,
				sn_cmd_list_entry) {
		if (cmd->sn == expected_sn) {
			TRACE_SN("Deferred command %p (sn %d) found",
				cmd, cmd->sn);
			tgt_dev->def_cmd_count--;
			list_del(&cmd->sn_cmd_list_entry);
			if (res == NULL)
				res = cmd;
			else {
				spin_lock(&cmd->cmd_lists->cmd_list_lock);
				TRACE_SN("Adding cmd %p to active cmd list",
					cmd);
				list_add_tail(&cmd->cmd_list_entry,
					&cmd->cmd_lists->active_cmd_list);
				wake_up(&cmd->cmd_lists->cmd_list_waitQ);
				spin_unlock(&cmd->cmd_lists->cmd_list_lock);
			}
		}
	}
	if (res != NULL)
		goto out_unlock;

	list_for_each_entry(cmd, &tgt_dev->skipped_sn_list,
				sn_cmd_list_entry) {
		if (cmd->sn == expected_sn) {
			atomic_t *slot = cmd->sn_slot;
			/* 
			 * !! At this point any pointer in cmd, except !!
			 * !! sn_slot and sn_cmd_list_entry, could be	!!
			 * !! already destroyed				!!
			 */
			TRACE_SN("cmd %p (tag %d) with skipped sn %d found",
				cmd, cmd->tag, cmd->sn);
			tgt_dev->def_cmd_count--;
			list_del(&cmd->sn_cmd_list_entry);
			spin_unlock_irq(&tgt_dev->sn_lock);
			EXTRACHECKS_BUG_ON(cmd->head_of_queue);
			if (test_and_set_bit(SCST_CMD_CAN_BE_DESTROYED, 
					&cmd->cmd_flags)) {
				scst_destroy_put_cmd(cmd);
			}
			scst_inc_expected_sn(tgt_dev, slot);
			expected_sn = tgt_dev->expected_sn;
			spin_lock_irq(&tgt_dev->sn_lock);
			goto restart;
		}
	}

out_unlock:
	spin_unlock_irq(&tgt_dev->sn_lock);
	return res;
}

void scst_add_thr_data(struct scst_tgt_dev *tgt_dev,
	struct scst_thr_data_hdr *data,
	void (*free_fn) (struct scst_thr_data_hdr *data))
{
	data->pid = current->pid;
	atomic_set(&data->ref, 1);
	EXTRACHECKS_BUG_ON(free_fn == NULL);
	data->free_fn = free_fn;
	spin_lock(&tgt_dev->thr_data_lock);
	list_add_tail(&data->thr_data_list_entry, &tgt_dev->thr_data_list);
	spin_unlock(&tgt_dev->thr_data_lock);
}

void scst_del_all_thr_data(struct scst_tgt_dev *tgt_dev)
{
	spin_lock(&tgt_dev->thr_data_lock);
	while (!list_empty(&tgt_dev->thr_data_list)) {
		struct scst_thr_data_hdr *d = list_entry(
				tgt_dev->thr_data_list.next, typeof(*d),
				thr_data_list_entry);
		list_del(&d->thr_data_list_entry);
		spin_unlock(&tgt_dev->thr_data_lock);
		scst_thr_data_put(d);
		spin_lock(&tgt_dev->thr_data_lock);
	}
	spin_unlock(&tgt_dev->thr_data_lock);
	return;
}

void scst_dev_del_all_thr_data(struct scst_device *dev)
{
	struct scst_tgt_dev *tgt_dev;

	TRACE_ENTRY();

	/* 
	 * This is read-only function for dev->dev_tgt_dev_list, so
	 * suspending the activity isn't necessary.
	 */

	down(&scst_mutex);

	list_for_each_entry(tgt_dev, &dev->dev_tgt_dev_list,
				dev_tgt_dev_list_entry) {
		scst_del_all_thr_data(tgt_dev);
	}

	up(&scst_mutex);

	TRACE_EXIT();
	return;
}

struct scst_thr_data_hdr *scst_find_thr_data(struct scst_tgt_dev *tgt_dev)
{
	struct scst_thr_data_hdr *res = NULL, *d;

	spin_lock(&tgt_dev->thr_data_lock);
	list_for_each_entry(d, &tgt_dev->thr_data_list, thr_data_list_entry) {
		if (d->pid == current->pid) {
			res = d;
			scst_thr_data_get(res);
			break;
		}
	}
	spin_unlock(&tgt_dev->thr_data_lock);
	return res;
}

/* No locks */
int scst_inc_on_dev_cmd(struct scst_cmd *cmd)
{
	int res = 0;
	struct scst_device *dev = cmd->dev;

	sBUG_ON(cmd->blocking);

	atomic_inc(&dev->on_dev_count);

#ifdef STRICT_SERIALIZING
	spin_lock_bh(&dev->dev_lock);
	if (test_bit(SCST_CMD_ABORTED, &cmd->cmd_flags))
		goto out_unlock;
	if (dev->block_count > 0) {
		scst_dec_on_dev_cmd(cmd, 0);
		TRACE_MGMT_DBG("Delaying cmd %p due to blocking or serializing"
		      "(tag %d, dev %p)", cmd, cmd->tag, dev);
		list_add_tail(&cmd->blocked_cmd_list_entry,
			      &dev->blocked_cmd_list);
		res = 1;
	} else {
		__scst_block_dev(cmd->dev);
		cmd->blocking = 1;
	}
	spin_unlock_bh(&dev->dev_lock);
	goto out;
#else
repeat:
	if (unlikely(dev->block_count > 0)) {
		spin_lock_bh(&dev->dev_lock);
		if (test_bit(SCST_CMD_ABORTED, &cmd->cmd_flags))
			goto out_unlock;
		barrier(); /* to reread block_count */
		if (dev->block_count > 0) {
			scst_dec_on_dev_cmd(cmd, 0);
			TRACE_MGMT_DBG("Delaying cmd %p due to blocking or "
				"serializing (tag %d, dev %p)", cmd,
				cmd->tag, dev);
			list_add_tail(&cmd->blocked_cmd_list_entry,
				      &dev->blocked_cmd_list);
			res = 1;
			spin_unlock_bh(&dev->dev_lock);
			goto out;
		} else {
			TRACE_MGMT_DBG("%s", "Somebody unblocked the device, "
				"continuing");
		}
		spin_unlock_bh(&dev->dev_lock);
	}
	if (unlikely(cmd->dev->dev_serialized)) {
		spin_lock_bh(&dev->dev_lock);
		barrier(); /* to reread block_count */
		if (cmd->dev->block_count == 0) {
			TRACE_MGMT_DBG("cmd %p (tag %d), blocking further "
				"cmds due to serializing (dev %p)", cmd,
				cmd->tag, dev);
			__scst_block_dev(cmd->dev);
			cmd->blocking = 1;
		} else {
			spin_unlock_bh(&dev->dev_lock);
			goto repeat;
		}
		spin_unlock_bh(&dev->dev_lock);
	}
#endif

out:
	return res;

out_unlock:
	spin_unlock_bh(&dev->dev_lock);
	goto out;
}

/* Called under dev_lock */
void scst_unblock_cmds(struct scst_device *dev)
{
#ifdef STRICT_SERIALIZING
	struct scst_cmd *cmd, *t;
	unsigned long flags;

	TRACE_ENTRY();

	local_irq_save(flags);
	list_for_each_entry_safe(cmd, t, &dev->blocked_cmd_list,
				 blocked_cmd_list_entry) {
		int brk = 0;
		/* 
		 * Since only one cmd per time is being executed, expected_sn
		 * can't change behind us, if the corresponding cmd is in
		 * blocked_cmd_list, but we could be called before
		 * scst_inc_expected_sn().
		 */
		if (likely(!cmd->internal) && likely(!cmd->retry)) {
			int expected_sn;
			if (cmd->tgt_dev == NULL)
				sBUG();
			expected_sn = cmd->tgt_dev->expected_sn;
			if (cmd->sn == expected_sn)
				brk = 1;
			else if (cmd->sn != (expected_sn+1))
				continue;
		}
		    	
		list_del(&cmd->blocked_cmd_list_entry);
		TRACE_MGMT_DBG("Adding cmd %p to head of active cmd list", cmd);
		spin_lock(&cmd->cmd_lists->cmd_list_lock);
		list_add(&cmd->cmd_list_entry, &cmd->cmd_lists->active_cmd_list);
		wake_up(&cmd->cmd_lists->cmd_list_waitQ);
		spin_unlock(&cmd->cmd_lists->cmd_list_lock);
		if (brk)
			break;
	}
	local_irq_restore(flags);
#else /* STRICT_SERIALIZING */
	struct scst_cmd *cmd, *tcmd;
	unsigned long flags;

	TRACE_ENTRY();

	local_irq_save(flags);
	list_for_each_entry_safe(cmd, tcmd, &dev->blocked_cmd_list,
				 blocked_cmd_list_entry) {
		list_del(&cmd->blocked_cmd_list_entry);
		TRACE_MGMT_DBG("Adding blocked cmd %p to active cmd list", cmd);
		spin_lock(&cmd->cmd_lists->cmd_list_lock);
		if (unlikely(cmd->head_of_queue))
			list_add(&cmd->cmd_list_entry,
				&cmd->cmd_lists->active_cmd_list);
		else
			list_add_tail(&cmd->cmd_list_entry,
				&cmd->cmd_lists->active_cmd_list);
		wake_up(&cmd->cmd_lists->cmd_list_waitQ);
		spin_unlock(&cmd->cmd_lists->cmd_list_lock);
	}
	local_irq_restore(flags);
#endif /* STRICT_SERIALIZING */

	TRACE_EXIT();
	return;
}

static struct scst_cmd *__scst_unblock_deferred(
	struct scst_tgt_dev *tgt_dev, struct scst_cmd *out_of_sn_cmd)
{
	struct scst_cmd *res = NULL;

	if (out_of_sn_cmd->head_of_queue) {
		TRACE_SN("HQ out_of_sn_cmd %p", out_of_sn_cmd);
		spin_lock_irq(&out_of_sn_cmd->tgt_dev->sn_lock);
		list_del(&out_of_sn_cmd->sn_cmd_list_entry);
		spin_unlock_irq(&out_of_sn_cmd->tgt_dev->sn_lock);
		res = scst_check_deferred_commands(tgt_dev);
	} else if (out_of_sn_cmd->sn == tgt_dev->expected_sn) {
		scst_inc_expected_sn(tgt_dev, out_of_sn_cmd->sn_slot);
		res = scst_check_deferred_commands(tgt_dev);
	} else {
		out_of_sn_cmd->out_of_sn = 1;
		spin_lock_irq(&tgt_dev->sn_lock);
		tgt_dev->def_cmd_count++;
		list_add_tail(&out_of_sn_cmd->sn_cmd_list_entry,
			      &tgt_dev->skipped_sn_list);
		TRACE_SN("out_of_sn_cmd %p with sn %d added to skipped_sn_list "
			"(expected_sn %d)", out_of_sn_cmd, out_of_sn_cmd->sn,
			tgt_dev->expected_sn);
		spin_unlock_irq(&tgt_dev->sn_lock);
	}

	return res;
}

void scst_unblock_deferred(struct scst_tgt_dev *tgt_dev,
	struct scst_cmd *out_of_sn_cmd)
{
	struct scst_cmd *cmd;

	TRACE_ENTRY();

	if (out_of_sn_cmd->no_sn) {
		TRACE_SN("cmd %p with no_sn", out_of_sn_cmd);
		goto out;
	}

	cmd = __scst_unblock_deferred(tgt_dev, out_of_sn_cmd);
	if (cmd != NULL) {
		unsigned long flags;
		spin_lock_irqsave(&cmd->cmd_lists->cmd_list_lock, flags);
		TRACE_SN("cmd %p with sn %d added to the head of active cmd "
			"list", cmd, cmd->sn);
		list_add(&cmd->cmd_list_entry, &cmd->cmd_lists->active_cmd_list);
		wake_up(&cmd->cmd_lists->cmd_list_waitQ);
		spin_unlock_irqrestore(&cmd->cmd_lists->cmd_list_lock, flags);
	}

out:
	TRACE_EXIT();
	return;
}

void __init scst_scsi_op_list_init(void)
{
	int i;
	uint8_t op = 0xff;

	TRACE_ENTRY();

	for (i = 0; i < 256; i++)
		scst_scsi_op_list[i] = SCST_CDB_TBL_SIZE;

	for (i = 0; i < SCST_CDB_TBL_SIZE; i++) {
		if (scst_scsi_op_table[i].ops != op) {
			op = scst_scsi_op_table[i].ops;
			scst_scsi_op_list[op] = i;
		}
	}

	TRACE_EXIT();
	return;
}

#ifdef DEBUG
/* Original taken from the XFS code */
unsigned long scst_random(void)
{
	static int Inited;
	static unsigned long RandomValue;
	static spinlock_t lock = SPIN_LOCK_UNLOCKED;
	/* cycles pseudo-randomly through all values between 1 and 2^31 - 2 */
	register long rv;
	register long lo;
	register long hi;
	unsigned long flags;

	spin_lock_irqsave(&lock, flags);
	if (!Inited) {
		RandomValue = jiffies;
		Inited = 1;
	}
	rv = RandomValue;
	hi = rv / 127773;
	lo = rv % 127773;
	rv = 16807 * lo - 2836 * hi;
	if (rv <= 0) rv += 2147483647;
	RandomValue = rv;
	spin_unlock_irqrestore(&lock, flags);
	return rv;
}
#endif

#ifdef DEBUG_TM

#define TM_DBG_STATE_ABORT		0
#define TM_DBG_STATE_RESET		1
#define TM_DBG_STATE_OFFLINE		2

#define INIT_TM_DBG_STATE		TM_DBG_STATE_ABORT

static void tm_dbg_timer_fn(unsigned long arg);

static spinlock_t scst_tm_dbg_lock = SPIN_LOCK_UNLOCKED;
/* All serialized by scst_tm_dbg_lock */
struct
{
	unsigned int tm_dbg_active:1;
	unsigned int tm_dbg_release:1;
	unsigned int tm_dbg_blocked:1;
} tm_dbg_flags;
static LIST_HEAD(tm_dbg_delayed_cmd_list);
static int tm_dbg_delayed_cmds_count;
static int tm_dbg_passed_cmds_count;
static int tm_dbg_state;
static int tm_dbg_on_state_passes;
static DEFINE_TIMER(tm_dbg_timer, tm_dbg_timer_fn, 0, 0);
static wait_queue_head_t *tm_dbg_p_cmd_list_waitQ;

static const int tm_dbg_on_state_num_passes[] = { 5, 1, 0x7ffffff };

void tm_dbg_init_tgt_dev(struct scst_tgt_dev *tgt_dev,
	struct scst_acg_dev *acg_dev)
{
	if (((acg_dev->acg == scst_default_acg) || 
	     (acg_dev->acg == tgt_dev->sess->tgt->default_acg)) && 
	    (acg_dev->lun == 0)) {
	    	unsigned long flags;
	    	spin_lock_irqsave(&scst_tm_dbg_lock, flags);
	    	if (!tm_dbg_flags.tm_dbg_active) {
			/* Do TM debugging only for LUN 0 */
			tm_dbg_p_cmd_list_waitQ = 
				&tgt_dev->p_cmd_lists->cmd_list_waitQ;
			tm_dbg_state = INIT_TM_DBG_STATE;
			tm_dbg_on_state_passes =
				tm_dbg_on_state_num_passes[tm_dbg_state];
			__set_bit(SCST_TGT_DEV_UNDER_TM_DBG, &tgt_dev->tgt_dev_flags);
			PRINT_INFO("LUN 0 connected from initiator %s is under "
				"TM debugging", tgt_dev->sess->tgt->tgtt->name);
			tm_dbg_flags.tm_dbg_active = 1;
		}
		spin_unlock_irqrestore(&scst_tm_dbg_lock, flags);
	}
}

void tm_dbg_deinit_tgt_dev(struct scst_tgt_dev *tgt_dev)
{
	if (test_bit(SCST_TGT_DEV_UNDER_TM_DBG, &tgt_dev->tgt_dev_flags)) {
		unsigned long flags;
		del_timer_sync(&tm_dbg_timer);
		spin_lock_irqsave(&scst_tm_dbg_lock, flags);
		tm_dbg_p_cmd_list_waitQ = NULL;
		tm_dbg_flags.tm_dbg_active = 0;
		spin_unlock_irqrestore(&scst_tm_dbg_lock, flags);
	}
}

static void tm_dbg_timer_fn(unsigned long arg)
{
	TRACE_MGMT_DBG("%s", "delayed cmd timer expired");
	tm_dbg_flags.tm_dbg_release = 1;
	smp_mb();
	wake_up_all(tm_dbg_p_cmd_list_waitQ);
}

/* Called under scst_tm_dbg_lock and IRQs off */
static void tm_dbg_delay_cmd(struct scst_cmd *cmd)
{
	switch(tm_dbg_state) {
	case TM_DBG_STATE_ABORT:
		if (tm_dbg_delayed_cmds_count == 0) {
			unsigned long d = 58*HZ + (scst_random() % (4*HZ));
			TRACE_MGMT_DBG("STATE ABORT: delaying cmd %p (tag %d) "
				"for %ld.%ld seconds (%ld HZ), "
				"tm_dbg_on_state_passes=%d", cmd, cmd->tag,
				d/HZ, (d%HZ)*100/HZ, d,	tm_dbg_on_state_passes);
			mod_timer(&tm_dbg_timer, jiffies + d);
#if 0
			tm_dbg_flags.tm_dbg_blocked = 1;
#endif
		} else {
			TRACE_MGMT_DBG("Delaying another timed cmd %p "
				"(tag %d), delayed_cmds_count=%d, "
				"tm_dbg_on_state_passes=%d", cmd, cmd->tag,
				tm_dbg_delayed_cmds_count,
				tm_dbg_on_state_passes);
			if (tm_dbg_delayed_cmds_count == 2)
				tm_dbg_flags.tm_dbg_blocked = 0;
		}
		break;

	case TM_DBG_STATE_RESET:
	case TM_DBG_STATE_OFFLINE:
		TRACE_MGMT_DBG("STATE RESET/OFFLINE: delaying cmd %p "
			"(tag %d), delayed_cmds_count=%d, "
			"tm_dbg_on_state_passes=%d", cmd, cmd->tag,
			tm_dbg_delayed_cmds_count, tm_dbg_on_state_passes);
		tm_dbg_flags.tm_dbg_blocked = 1;
		break;

	default:
		sBUG();
	}
	/* IRQs already off */
	spin_lock(&cmd->cmd_lists->cmd_list_lock);
	list_add_tail(&cmd->cmd_list_entry, &tm_dbg_delayed_cmd_list);
	spin_unlock(&cmd->cmd_lists->cmd_list_lock);
	cmd->tm_dbg_delayed = 1;
	tm_dbg_delayed_cmds_count++;
	return;
}

/* No locks */
void tm_dbg_check_released_cmds(void)
{
	if (tm_dbg_flags.tm_dbg_release) {
		struct scst_cmd *cmd, *tc;
		spin_lock_irq(&scst_tm_dbg_lock);
		list_for_each_entry_safe_reverse(cmd, tc, 
				&tm_dbg_delayed_cmd_list, cmd_list_entry) {
			TRACE_MGMT_DBG("Releasing timed cmd %p (tag %d), "
				"delayed_cmds_count=%d", cmd, cmd->tag,
				tm_dbg_delayed_cmds_count);
			spin_lock(&cmd->cmd_lists->cmd_list_lock);
			list_move(&cmd->cmd_list_entry,
				&cmd->cmd_lists->active_cmd_list);
			spin_unlock(&cmd->cmd_lists->cmd_list_lock);
		}
		tm_dbg_flags.tm_dbg_release = 0;
		spin_unlock_irq(&scst_tm_dbg_lock);
	}
}

/* Called under scst_tm_dbg_lock */
static void tm_dbg_change_state(void)
{
	tm_dbg_flags.tm_dbg_blocked = 0;
	if (--tm_dbg_on_state_passes == 0) {
		switch(tm_dbg_state) {
		case TM_DBG_STATE_ABORT:
			TRACE_MGMT_DBG("%s", "Changing "
			    "tm_dbg_state to RESET");
			tm_dbg_state =
				TM_DBG_STATE_RESET;
			tm_dbg_flags.tm_dbg_blocked = 0;
			break;
		case TM_DBG_STATE_RESET:
		case TM_DBG_STATE_OFFLINE:
			if (TM_DBG_GO_OFFLINE) {
			    TRACE_MGMT_DBG("%s", "Changing "
				    "tm_dbg_state to OFFLINE");
			    tm_dbg_state =
				TM_DBG_STATE_OFFLINE;
			} else {
			    TRACE_MGMT_DBG("%s", "Changing "
				    "tm_dbg_state to ABORT");
			    tm_dbg_state =
				TM_DBG_STATE_ABORT;
			}
			break;
		default:
			sBUG();
		}
		tm_dbg_on_state_passes =
		    tm_dbg_on_state_num_passes[tm_dbg_state];
	}
		
	TRACE_MGMT_DBG("%s", "Deleting timer");
	del_timer(&tm_dbg_timer);
}

/* No locks */
int tm_dbg_check_cmd(struct scst_cmd *cmd)
{
	int res = 0;
	unsigned long flags;

	if (cmd->tm_dbg_immut)
		goto out;

	if (cmd->tm_dbg_delayed) {
		spin_lock_irqsave(&scst_tm_dbg_lock, flags);
		TRACE_MGMT_DBG("Processing delayed cmd %p (tag %d), "
			"delayed_cmds_count=%d", cmd, cmd->tag,
			tm_dbg_delayed_cmds_count);

		cmd->tm_dbg_immut = 1;
		tm_dbg_delayed_cmds_count--;
		if ((tm_dbg_delayed_cmds_count == 0) &&
		    (tm_dbg_state == TM_DBG_STATE_ABORT))
			tm_dbg_change_state();
		spin_unlock_irqrestore(&scst_tm_dbg_lock, flags);
	} else if (cmd->tgt_dev && test_bit(SCST_TGT_DEV_UNDER_TM_DBG,
					&cmd->tgt_dev->tgt_dev_flags)) {
		/* Delay 50th command */
		spin_lock_irqsave(&scst_tm_dbg_lock, flags);
		if (tm_dbg_flags.tm_dbg_blocked ||
		    (++tm_dbg_passed_cmds_count % 50) == 0) {
			tm_dbg_delay_cmd(cmd);
			res = 1;
		} else
			cmd->tm_dbg_immut = 1;
		spin_unlock_irqrestore(&scst_tm_dbg_lock, flags);
	}

out:
	return res;
}

/* No locks */
void tm_dbg_release_cmd(struct scst_cmd *cmd)
{
	struct scst_cmd *c;
	unsigned long flags;

	spin_lock_irqsave(&scst_tm_dbg_lock, flags);
	list_for_each_entry(c, &tm_dbg_delayed_cmd_list,
				cmd_list_entry) {
		if (c == cmd) {
			TRACE_MGMT_DBG("Abort request for "
				"delayed cmd %p (tag=%d), moving it to "
				"active cmd list (delayed_cmds_count=%d)",
				c, c->tag, tm_dbg_delayed_cmds_count);
			spin_lock(&cmd->cmd_lists->cmd_list_lock);
			list_move(&c->cmd_list_entry, 
				&c->cmd_lists->active_cmd_list);
			wake_up(&c->cmd_lists->cmd_list_waitQ);
			spin_unlock(&cmd->cmd_lists->cmd_list_lock);
			break;
		}
	}
	spin_unlock_irqrestore(&scst_tm_dbg_lock, flags);
}

/* No locks */
void tm_dbg_task_mgmt(const char *fn, int force)
{
	unsigned long flags;

	spin_lock_irqsave(&scst_tm_dbg_lock, flags);
	if ((tm_dbg_state != TM_DBG_STATE_OFFLINE) || force) {
		TRACE_MGMT_DBG("%s: freeing %d delayed cmds", fn,
			tm_dbg_delayed_cmds_count);
		tm_dbg_change_state();
		tm_dbg_flags.tm_dbg_release = 1;
		smp_mb();
		wake_up_all(tm_dbg_p_cmd_list_waitQ);
	} else {
		TRACE_MGMT_DBG("%s: while OFFLINE state, doing nothing", fn);
	}
	spin_unlock_irqrestore(&scst_tm_dbg_lock, flags);
}

int tm_dbg_is_release(void)
{
	return tm_dbg_flags.tm_dbg_release;
}
#endif /* DEBUG_TM */

#ifdef DEBUG_SN
void scst_check_debug_sn(struct scst_cmd *cmd)
{
	static spinlock_t lock = SPIN_LOCK_UNLOCKED;
	static int type;
	static int cnt;
	unsigned long flags;
	int old = cmd->queue_type;

	spin_lock_irqsave(&lock, flags);

	if (cnt == 0) {
		if ((scst_random() % 1000) == 500) {
			if ((scst_random() % 3) == 1)
				type = SCST_CMD_QUEUE_HEAD_OF_QUEUE;
			else
				type = SCST_CMD_QUEUE_ORDERED;
			do {
				cnt = scst_random() % 10;
			} while(cnt == 0);
		} else
			goto out_unlock;
	}

	cmd->queue_type = type;
	cnt--;

	if (((scst_random() % 1000) == 750))
		cmd->queue_type = SCST_CMD_QUEUE_ORDERED;
	else if (((scst_random() % 1000) == 751))
		cmd->queue_type = SCST_CMD_QUEUE_HEAD_OF_QUEUE;
	else if (((scst_random() % 1000) == 752))
		cmd->queue_type = SCST_CMD_QUEUE_SIMPLE;

	TRACE_SN("DbgSN changed cmd %p: %d/%d (cnt %d)", cmd, old,
		cmd->queue_type, cnt);

out_unlock:
	spin_unlock_irqrestore(&lock, flags);
	return;
}
#endif /* DEBUG_SN */
