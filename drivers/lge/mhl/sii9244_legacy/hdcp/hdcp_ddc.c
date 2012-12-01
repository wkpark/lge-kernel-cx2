/*
 * hdcp_ddc.c
 *
 * HDCP support functions for TI OMAP processors.
 *
 * Copyright (C) 2011 Texas Instruments
 *
 * Authors:	Sujeet Baranwal <s-baranwal@ti.com>
 *		Fabrice Olivero <f-olivero@ti.com>	
 *
 * Use of this software is controlled by the terms and conditions found
 * in the license agreement under which this software has been supplied.
 *
 */

#include <linux/delay.h>
#include "hdcp.h"
#include "hdcp_ddc.h"

/*-----------------------------------------------------------------------------
 * Function: hdcp_suspend_resume_auto_ri
 *-----------------------------------------------------------------------------
 */
static int hdcp_suspend_resume_auto_ri(enum ri_suspend_resume state)
{
	static u8 OldRiStat, OldRiCommand;
	u8 TimeOut = 10;

	/* Suspend Auto Ri in order to allow FW access MDDC bus.
	 * Poll 0x72:0x26[0] for MDDC bus availability or timeout
	 */

	DBG("hdcp_suspend_resume_auto_ri() state=%s", state == AUTO_RI_SUSPEND ? "SUSPEND" : "RESUME");

    // wooho47.jung@lge.com 2011.11.05
    // DEL : no need delay
	//msleep(10);  // by Joshua(must be set for delay of DDC)

	if (state == AUTO_RI_SUSPEND) {
		/* Save original Auto Ri state */
		OldRiCommand = RD_FIELD_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
					   HDMI_IP_CORE_SYSTEM__RI_CMD, 0, 0);

		/* Disable Auto Ri */
		hdcp_lib_auto_ri_check(false);

		/* Wait for HW to release MDDC bus */
		/* TODO: while loop / timeout to be enhanced */
		while (--TimeOut) {							
			if (!RD_FIELD_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
					 HDMI_IP_CORE_SYSTEM__RI_STAT, 0, 0))
				break;
		}

		/* MDDC bus not relinquished */
		if (!TimeOut) {
			printk(KERN_ERR "HDCP: Suspending Auto Ri failed !\n");
			return -HDCP_DDC_ERROR;
		}

		OldRiStat = RD_FIELD_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
					HDMI_IP_CORE_SYSTEM__RI_STAT, 0, 0);
	}
	else {
		/* If Auto Ri was enabled before it was suspended */
	        if ((OldRiStat) && (OldRiCommand))
			/* Re-enable Auto Ri */
			hdcp_lib_auto_ri_check(false);
	}

	return HDCP_OK;
}


/*-----------------------------------------------------------------------------
 * Function: hdcp_start_ddc_transfer
 *-----------------------------------------------------------------------------
 */
static int hdcp_start_ddc_transfer(mddc_type *mddc_cmd, u8 operation)
{
	u8 *cmd = (u8 *)mddc_cmd;
    struct timeval t0, t1, t2;
    u32 time_elapsed_ms = 0;
    u32 i, size;
	unsigned long flags;
	
#ifdef _9032_AUTO_RI_
	if (hdcp_suspend_resume_auto_ri(AUTO_RI_SUSPEND))
		return -HDCP_DDC_ERROR;
#endif

	spin_lock_irqsave(&hdcp.spinlock, flags);

	/* Abort Master DDC operation and Clear FIFO pointer */
	WR_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_CMD, MASTER_CMD_CLEAR_FIFO);

	/* Read to flush */
	RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_CMD);

	/* Sending DDC header, it'll clear DDC Status register too */
	for(i = 0; i < 7; i++) {
		WR_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
			  HDMI_IP_CORE_SYSTEM__DDC_ADDR + i * sizeof(uint32_t),
			  cmd[i]);

		/* Read to flush */
		RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
			  HDMI_IP_CORE_SYSTEM__DDC_ADDR +
			  i * sizeof(uint32_t));
	}

	spin_unlock_irqrestore(&hdcp.spinlock, flags);

	do_gettimeofday(&t1);
	memcpy(&t0, &t1, sizeof(t0));

	i = 0;
	size = mddc_cmd->nbytes_lsb + (mddc_cmd->nbytes_msb << 8);

	while ((i < size) && (hdcp.pending_disable == 0)) {
		if (operation == DDC_WRITE) {
			/* Write data to DDC FIFO as long as it is NOT full */
			if(RD_FIELD_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
				       HDMI_IP_CORE_SYSTEM__DDC_STATUS, 3, 3) == 0) {
				WR_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
					  HDMI_IP_CORE_SYSTEM__DDC_DATA, mddc_cmd->pdata[i++]);
				do_gettimeofday(&t1);
			}
		}
		else if (operation == DDC_READ) {
			/* Read from DDC FIFO as long as it is NOT empty */
			if(RD_FIELD_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
				       HDMI_IP_CORE_SYSTEM__DDC_STATUS, 2, 2) == 0) {
				mddc_cmd->pdata[i++] = RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
								 HDMI_IP_CORE_SYSTEM__DDC_DATA);
				do_gettimeofday(&t1);
			}
		}

		do_gettimeofday(&t2);
		time_elapsed_ms = (t2.tv_sec - t1.tv_sec) * 1000 +
				  (t2.tv_usec - t1.tv_usec) / 1000;

		if(time_elapsed_ms > HDCP_DDC_TIMEOUT) {
			DBG_ERROR("DDC timeout - no data during %d ms - "
			    "status=%02x %u",
					HDCP_DDC_TIMEOUT,
					RD_REG_32(hdcp.hdmi_wp_base_addr +
					  HDMI_IP_CORE_SYSTEM,
					  HDMI_IP_CORE_SYSTEM__DDC_STATUS),
					jiffies_to_msecs(jiffies));
			goto ddc_error;
		}

	} //end of while

	if (hdcp.pending_disable)
		goto ddc_abort;

	while ((RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
			   HDMI_IP_CORE_SYSTEM__DDC_STATUS) != 0x4) &&
	       (hdcp.pending_disable == 0)) {
		do_gettimeofday(&t2);
		time_elapsed_ms = (t2.tv_sec - t1.tv_sec) * 1000 +
				  (t2.tv_usec - t1.tv_usec) / 1000;
        
		if(time_elapsed_ms > HDCP_DDC_TIMEOUT) {
			DBG("DDC timeout - FIFO not getting empty - "
			    "status=%02x",
				RD_REG_32(hdcp.hdmi_wp_base_addr +
					  HDMI_IP_CORE_SYSTEM,
					  HDMI_IP_CORE_SYSTEM__DDC_STATUS));
			goto ddc_error;
		}

		
	}

	if (hdcp.pending_disable)
		goto ddc_abort;
// LGE_CHANGE_S [jh.koo kibum.lee] 2011-09-08, for hdcp certification at P940
	RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_STATUS);
// LGE_CHANGE_E [jh.koo kibum.lee] 2011-09-08, for hdcp certification at P940

#ifdef _9032_AUTO_RI_
	/* Re-enable Auto Ri */
	if (hdcp_suspend_resume_auto_ri(AUTO_RI_RESUME))
		return -HDCP_DDC_ERROR;
#endif

	return HDCP_OK;
ddc_error:
	hdcp_ddc_abort();
	return -HDCP_DDC_ERROR;

ddc_abort:
    // wooho47.jung@lge.com 2011.11.05
    // ADD : apply Linux_27.G.5_HDCP patch for HDCP
    //hdcp_ddc_abort();
	RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_STATUS);
	DBG("DDC transfer aborted\n");

	return HDCP_OK;
}

/*-----------------------------------------------------------------------------
 * Function: hdcp_ddc_operation
 *-----------------------------------------------------------------------------
 */
static int hdcp_ddc_operation(u16 no_bytes, u8 addr, u8 * pdata, enum ddc_operation operation)
{
	mddc_type mddc;

	mddc.slaveAddr	= HDCPRX_SLV;
	mddc.offset	= 0;
	mddc.regAddr	= addr;
	mddc.nbytes_lsb	= no_bytes & 0xFF;
	mddc.nbytes_msb	= (no_bytes & 0x300) >> 8;
	mddc.dummy	= 0;
	mddc.pdata	= pdata;

	if (operation == DDC_READ) {
		mddc.cmd = MASTER_CMD_SEQ_RD;

	} else {
		mddc.cmd = MASTER_CMD_SEQ_WR;
	}

	DBG("DDC %s: offset=%02x len=%d", operation == DDC_READ ? "READ" : "WRITE", addr, no_bytes);

	return hdcp_start_ddc_transfer(&mddc, operation);
}

/*-----------------------------------------------------------------------------
 * Function: hdcp_ddc_read
 *-----------------------------------------------------------------------------
 */
int hdcp_ddc_read(u16 no_bytes, u8 addr, u8 *pdata)
{
        return hdcp_ddc_operation(no_bytes, addr, pdata, DDC_READ);
}

/*-----------------------------------------------------------------------------
 * Function: hdcp_ddc_write
 *-----------------------------------------------------------------------------
 */
int hdcp_ddc_write(u16 no_bytes, u8 addr, u8 *pdata)
{
        return hdcp_ddc_operation(no_bytes, addr, pdata, DDC_WRITE);
}
/*-----------------------------------------------------------------------------
 * Function: hdcp_ddc_abort
 *-----------------------------------------------------------------------------
 */
void hdcp_ddc_abort(void)
{
	unsigned long flags;

	/* In case of I2C_NO_ACK error, do not abort DDC to avoid
	 * DDC lockup
	 */
	if (RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		      HDMI_IP_CORE_SYSTEM__DDC_STATUS) & 0x20)
		return;

	spin_lock_irqsave(&hdcp.spinlock, flags);

	/* Abort Master DDC operation and Clear FIFO pointer */
	WR_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_CMD, MASTER_CMD_ABORT);

	/* Read to flush */
	RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_CMD);

	WR_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_CMD, MASTER_CMD_CLEAR_FIFO);

	/* Read to flush */
	RD_REG_32(hdcp.hdmi_wp_base_addr + HDMI_IP_CORE_SYSTEM,
		  HDMI_IP_CORE_SYSTEM__DDC_CMD);

	spin_unlock_irqrestore(&hdcp.spinlock, flags);
}
