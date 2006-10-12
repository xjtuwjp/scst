/*
 *  scst_changer.c
 *  
 *  Copyright (C) 2004-2006 Vladislav Bolkhovitin <vst@vlnb.net>
 *                 and Leonid Stoljar
 *
 *  SCSI medium changer (type 8) dev handler
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

#define LOG_PREFIX      "dev_changer"
#include "scst_debug.h"
#include "scsi_tgt.h"
#include "scst_dev_handler.h"

#include "scst_debug.c"

#define CHANGER_NAME	"dev_changer"

#define CHANGER_TYPE {		\
  name:     CHANGER_NAME,   	\
  type:     TYPE_MEDIUM_CHANGER,\
  parse_atomic:     1,      	\
/*  dev_done_atomic:  1,*/      \
  attach:   changer_attach, 	\
/*  detach:   changer_detach,*/ \
  parse:    changer_parse,  	\
/*  dev_done: changer_done*/    \
}

#define CHANGER_RETRIES       2
#define CHANGER_TIMEOUT      (3 * HZ)
#define CHANGER_LONG_TIMEOUT (14000 * HZ)
#define READ_CAP_LEN          8

int changer_attach(struct scst_device *);
void changer_detach(struct scst_device *);
int changer_parse(struct scst_cmd *, const struct scst_info_cdb *);
int changer_done(struct scst_cmd *);

#if defined(DEBUG) || defined(TRACING)
unsigned long trace_flag = SCST_DEFAULT_DEV_LOG_FLAGS;
#endif

static struct scst_dev_type changer_devtype = CHANGER_TYPE;

/**************************************************************
 *  Function:  changer_attach
 *
 *  Argument:  
 *
 *  Returns :  1 if attached, error code otherwise
 *
 *  Description:  
 *************************************************************/
int changer_attach(struct scst_device *dev)
{
	int res = 0;
	int retries;

	TRACE_ENTRY();

	if (dev->scsi_dev == NULL ||
	    dev->scsi_dev->type != dev->handler->type) {
		PRINT_ERROR_PR("%s", "SCSI device not define or illegal type");
		res = -ENODEV;
		goto out;
	}

	/*
	 * If the device is offline, don't try to read capacity or any
	 * of the other stuff
	 */
	if (dev->scsi_dev->sdev_state == SDEV_OFFLINE)
	{
		TRACE_DBG("%s", "Device is offline");
		res = -ENODEV;
		goto out;
	}

	retries = SCST_DEV_UA_RETRIES;
	do {
		TRACE_DBG("%s", "Doing TEST_UNIT_READY");
		res = scsi_test_unit_ready(dev->scsi_dev, CHANGER_TIMEOUT, 
					   CHANGER_RETRIES);
		TRACE_DBG("TEST_UNIT_READY done: %x", res);
	} while ((--retries > 0) && res);
	if (res) 
		res = -ENODEV;

out:
	TRACE_EXIT_HRES(res);
	return res;
}

/************************************************************
 *  Function:  changer_detach
 *
 *  Argument: 
 *
 *  Returns :  None
 *
 *  Description:  Called to detach this device type driver
 ************************************************************/
void changer_detach(struct scst_device *dev)
{
	TRACE_ENTRY();

	TRACE_EXIT();
	return;
}

/********************************************************************
 *  Function:  changer_parse
 *
 *  Argument:  
 *
 *  Returns :  The state of the command
 *
 *  Description:  This does the parsing of the command
 *
 *  Note:  Not all states are allowed on return
 ********************************************************************/
int changer_parse(struct scst_cmd *cmd, const struct scst_info_cdb *info_cdb)
{
	int res = SCST_CMD_STATE_DEFAULT;

	TRACE_ENTRY();

	/*
	 * SCST sets good defaults for cmd->data_direction and cmd->bufflen
	 * based on info_cdb, therefore change them only if necessary
	 */

	if (info_cdb->flags & SCST_LONG_TIMEOUT) {
		cmd->timeout = CHANGER_LONG_TIMEOUT;
	} else {
		cmd->timeout = CHANGER_TIMEOUT;
	}

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

/********************************************************************
 *  Function:  changer_done
 *
 *  Argument:  
 *
 *  Returns :  
 *
 *  Description:  This is the completion routine for the command,
 *                it is used to extract any necessary information
 *                about a command. 
 ********************************************************************/
int changer_done(struct scst_cmd *cmd)
{
	int res = SCST_CMD_STATE_DEFAULT;

	TRACE_ENTRY();

	if (unlikely(cmd->sg == NULL))
		goto out;

	/*
	 * SCST sets good defaults for cmd->tgt_resp_flags and cmd->resp_data_len
	 * based on cmd->masked_status and cmd->data_direction, therefore change
	 * them only if necessary
	 */

#if 0
	switch (cmd->cdb[0]) {
	default:
		/* It's all good */
		break;
	}
#endif

out:
	TRACE_EXIT();
	return res;
}

static int __init changer_init(void)
{
	int res = 0;

	TRACE_ENTRY();
	
	changer_devtype.module = THIS_MODULE;
	if (scst_register_dev_driver(&changer_devtype) < 0) {
		res = -ENODEV;
		goto out;
	}
	
	res = scst_dev_handler_build_std_proc(&changer_devtype);
	if (res != 0)
		goto out_err;

out:
	TRACE_EXIT_RES(res);
	return res;

out_err:
	scst_unregister_dev_driver(&changer_devtype);
	goto out;
}

static void __exit changer_exit(void)
{
	TRACE_ENTRY();
	scst_dev_handler_destroy_std_proc(&changer_devtype);
	scst_unregister_dev_driver(&changer_devtype);
	TRACE_EXIT();
	return;
}

module_init(changer_init);
module_exit(changer_exit);

MODULE_LICENSE("GPL");
