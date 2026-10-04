/**
  ******************************************************************************
  * @file    iol_main.c
  * @author  Sanjana S , Calixto Firmware Team
  * @version V1.0.0
  * @date    02-Feb-2026
  * @brief   This file provides functions to manage the following
  *          breif the functionalities here:
  *           - 1.
  *
  * <h2><center>&copy; COPYRIGHT 2026 Calixto Systems Pvt Ltd</center></h2>
  ******************************************************************************
  */
/* Includes ------------------------------------------------------------------*/
#include "iol-master/iol_main.h"
#include "iol-master/iol_cce4511.h"
#include <iol-master/iol_cce4511_pl.h>


#include "iol-master/iol_pl.h"
#include "iol-master/iol_dl.h"
#include "iol-master/iol_sm.h"
#include "iol-master/iol_mw.h"

#include <errno.h>
#include <stdlib.h> /* calloc */
#include <string.h>

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "iol-master/osal_spi.h"
#include "iol-master/cce4511_conf.h"


/* ---------------------------------------------------------------------------
 * CCE4511 hardware resources - one entry per chip
 *
 * Chip N is described by the devicetree node labelled "cce4511_N"
 * (SPI child node with irq-gpios). Each chip has its own SPI device and its
 * own interrupt line; a chip provides IOL_PORTS_PER_CHIP IO-Link ports.
 * ---------------------------------------------------------------------------*/
#define IOL_NUM_CHIPS       CONFIG_IOL_MASTER_NUM_CHIPS
#define IOL_PORTS_PER_CHIP  CCE4511_NUM_CHANNELS

BUILD_ASSERT (IOL_NUM_CHIPS <= IOL_SPI_MAX_DEVICES,
              "IOL_MASTER_NUM_CHIPS exceeds IOL_SPI_MAX_DEVICES");
BUILD_ASSERT (IOLINK_NUM_PORTS <= (IOL_NUM_CHIPS * IOL_PORTS_PER_CHIP),
              "IOL_MASTER_NUM_CHANNELS needs more chips: 4 ports per "
              "CCE4511 (raise IOL_MASTER_NUM_CHIPS)");

struct iol_chip
{
   const char *                name;   /* SPI registration key + log tag   */
   const struct spi_dt_spec    spi;
   const struct gpio_dt_spec   irq;
   struct gpio_callback        cb;
   bool                        irq_armed;
   iolink_hw_drv_t * volatile  hw;     /* NULL until the chip is up        */
};

#define IOL_CHIP_NODE(n) DT_NODELABEL (cce4511_##n)

#define IOL_CHIP_ENTRY(n)                                                   \
   {                                                                        \
      .name = "cce4511_" #n,                                                \
      .spi  = SPI_DT_SPEC_GET (IOL_CHIP_NODE (n), CCE4511_SPI_OPERATION, 0),\
      .irq  = GPIO_DT_SPEC_GET (IOL_CHIP_NODE (n), irq_gpios),              \
   }

#define IOL_CHIP_NODE_CHECK(n)                                              \
   BUILD_ASSERT (DT_NODE_EXISTS (IOL_CHIP_NODE (n)),                        \
                 "devicetree node label cce4511_" #n " is missing")

IOL_CHIP_NODE_CHECK (0);
#if IOL_NUM_CHIPS > 1
IOL_CHIP_NODE_CHECK (1);
#endif
#if IOL_NUM_CHIPS > 2
IOL_CHIP_NODE_CHECK (2);
#endif
#if IOL_NUM_CHIPS > 3
IOL_CHIP_NODE_CHECK (3);
#endif

static struct iol_chip s_chips[IOL_NUM_CHIPS] = {
   IOL_CHIP_ENTRY (0),
#if IOL_NUM_CHIPS > 1
   IOL_CHIP_ENTRY (1),
#endif
#if IOL_NUM_CHIPS > 2
   IOL_CHIP_ENTRY (2),
#endif
#if IOL_NUM_CHIPS > 3
   IOL_CHIP_ENTRY (3),
#endif
};

static void gpio_isr_handler (
   const struct device * port,
   struct gpio_callback * cb,
   gpio_port_pins_t       pins)
{
   ARG_UNUSED (port);
   ARG_UNUSED (pins);

   /* Each chip has its own callback object, so the chip is recovered from it. */
   struct iol_chip * chip = CONTAINER_OF (cb, struct iol_chip, cb);
   iolink_hw_drv_t * hw   = chip->hw;

   if (hw != NULL)
   {
      iolink_cce4511_isr ((void *)hw);
   }
}

static int iol_chip_hw_init (struct iol_chip * chip)
{
   if (!spi_is_ready_dt (&chip->spi))
   {
      IOL_LOG_ERR ("%s: SPI bus not ready - check DTS overlay\n", chip->name);
      return -ENODEV;
   }

   int rc = iol_spi_register_device (chip->name, &chip->spi);
   if (rc != 0)
   {
      IOL_LOG_ERR ("%s: SPI registration failed: %d\n", chip->name, rc);
      return rc;
   }

   if (!gpio_is_ready_dt (&chip->irq))
   {
      IOL_LOG_ERR ("%s: IRQ GPIO not ready - check DTS irq-gpios\n",
                   chip->name);
      return -ENODEV;
   }

   rc = gpio_pin_configure_dt (&chip->irq, GPIO_INPUT);
   if (rc != 0)
   {
      IOL_LOG_ERR ("%s: gpio_pin_configure_dt(IRQ) failed: %d\n",
                   chip->name, rc);
      return rc;
   }

   rc = gpio_pin_interrupt_configure_dt (&chip->irq, GPIO_INT_EDGE_TO_ACTIVE);
   if (rc != 0)
   {
      IOL_LOG_ERR ("%s: gpio_pin_interrupt_configure_dt failed: %d\n",
                   chip->name, rc);
      return rc;
   }

   gpio_init_callback (&chip->cb, gpio_isr_handler, BIT (chip->irq.pin));

   rc = gpio_add_callback (chip->irq.port, &chip->cb);
   if (rc != 0)
   {
      IOL_LOG_ERR ("%s: gpio_add_callback failed: %d\n", chip->name, rc);
      return rc;
   }

   chip->irq_armed = true;

   IOL_LOG_DBG ("%s HW init done - IRQ GPIO pin %d\n",
                chip->name, chip->irq.pin);
   return 0;
}

static void iol_chip_hw_deinit (struct iol_chip * chip)
{
   if (chip->irq_armed)
   {
      gpio_pin_interrupt_configure_dt (&chip->irq, GPIO_INT_DISABLE);
      gpio_remove_callback (chip->irq.port, &chip->cb);
      chip->irq_armed = false;
   }
}

/* Tear down every chip that is up (used on Master_init failure paths). */
static void iol_chips_shutdown (void)
{
   for (uint8_t c = 0; c < IOL_NUM_CHIPS; c++)
   {
      struct iol_chip * chip = &s_chips[c];
      iolink_hw_drv_t * hw   = chip->hw;

      iol_chip_hw_deinit (chip);
      chip->hw = NULL;

      if ((hw != NULL) && (hw->ops != NULL) && (hw->ops->pl_deinit != NULL))
      {
         hw->ops->pl_deinit (hw);
      }
   }
}

enum { MASTER_RW_DONE_BIT = 0x1 };
enum { MASTER_RW_TIMEOUT_MS = 5000 };

typedef struct
{
   bool in_use;
   os_event_t * ev;
   iolink_error_t res;
   uint8_t * out_buf;
   uint8_t * out_len;
} master_read_ctx_t;

typedef struct
{
   bool in_use;
   os_event_t * ev;
   iolink_error_t res;
} master_write_ctx_t;

static master_read_ctx_t s_master_read_ctx[IOLINK_NUM_PORTS];
static master_write_ctx_t s_master_write_ctx[IOLINK_NUM_PORTS];

static void master_read_cnf_cb (
   iolink_port_t * cb_port,
   uint8_t cb_len,
   const uint8_t * cb_data,
   iolink_error_t errortype)
{
   uint8_t pnum = iolink_get_portnumber (cb_port);
   if ((pnum == 0) || (pnum > IOLINK_NUM_PORTS))
   {
      return;
   }

   master_read_ctx_t * ctx = &s_master_read_ctx[pnum - 1];
   ctx->res = errortype;

   if ((errortype == IOLINK_ERROR_NONE) && (cb_data != NULL) &&
       (ctx->out_buf != NULL) && (ctx->out_len != NULL))
   {
      memcpy (ctx->out_buf, cb_data, cb_len);
      *(ctx->out_len) = cb_len;
   }
   else if (ctx->out_len != NULL)
   {
      *(ctx->out_len) = 0;
   }

   if (ctx->ev)
   {
      os_event_set (ctx->ev, MASTER_RW_DONE_BIT);
   }
}

static void master_write_cnf_cb (
   iolink_port_t * cb_port,
   iolink_error_t errortype)
{
   uint8_t pnum = iolink_get_portnumber (cb_port);
   if ((pnum == 0) || (pnum > IOLINK_NUM_PORTS))
   {
      return;
   }

   master_write_ctx_t * ctx = &s_master_write_ctx[pnum - 1];
   ctx->res = errortype;

   if (ctx->ev)
   {
      os_event_set (ctx->ev, MASTER_RW_DONE_BIT);
   }
}

/**
 * @file
 * @brief Handler
 *
 */

#define IOLINK_MASTER_JOB_CNT     100
#define IOLINK_MASTER_JOB_API_CNT 50

typedef struct iolink_port
{
   iolink_m_t * master;
   uint8_t portnumber;
   iolink_pl_port_t pl;
   iolink_dl_t dl;
   iolink_sm_port_t sm;
   iolink_mw_port_t mw;
   iolink_port_info_t port_info;
} iolink_port_t;


typedef struct iolink_m
{
   bool has_exited;
   os_thread_t  *thread;
   os_mbox_t * mbox;           /* Mailbox for job submission */
   os_mbox_t  *mbox_avail;     /* Mailbox for available jobs */
   os_mbox_t  *mbox_api_avail; /* Mailbox for available API (external) jobs */
   iolink_job_t job[IOLINK_MASTER_JOB_CNT];
   iolink_job_t job_api[IOLINK_MASTER_JOB_API_CNT];

   void * cb_arg; /* Callback opaque argument */

   /* data callback */
   void (*user_cb) (
      uint8_t portnumber,
      void * arg,
	  IOL_data_ind data_type,
      uint8_t data_len,
      const uint8_t * data);

   uint8_t port_cnt;
   struct iolink_port ports[];
} iolink_m_t;

iolink_m_t * the_master = NULL;

static iolink_transmission_rate_t mhmode_to_transmission_rate (
   iolink_mhmode_t mhmode)
{
   iolink_transmission_rate_t res = IOLINK_TRANSMISSION_RATE_NOT_DETECTED;

   switch (mhmode)
   {
   case IOLINK_MHMODE_COM1:
      res = IOLINK_TRANSMISSION_RATE_COM1;
      break;
   case IOLINK_MHMODE_COM2:
      res = IOLINK_TRANSMISSION_RATE_COM2;
      break;
   case IOLINK_MHMODE_COM3:
      res = IOLINK_TRANSMISSION_RATE_COM3;
      break;
   default:
      break;
   }

   return res;
}

// static iolink_error_t portnumber_to_iolinkport (
//    uint8_t portnumber,
//    iolink_port_t ** port)
// {
//    uint8_t port_index = portnumber - 1;

//    *port = NULL;

//    if (the_master == NULL)
//    {
//       return IOLINK_ERROR_STATE_INVALID;
//    }

//    if ((portnumber == 0) || (port_index >= the_master->port_cnt))
//    {
//       return IOLINK_ERROR_PARAMETER_CONFLICT;
//    }

//    *port = &the_master->ports[port_index];

//    return IOLINK_ERROR_NONE;
// }

static void iolink_main (void * arg)
{
   iolink_m_t * master = arg;
   bool running = true;

   while (running)
   {
      iolink_job_t * job;

      CC_ASSERT (master->mbox_avail != NULL);
      os_mbox_fetch (master->mbox, (void **)&job, OS_WAIT_FOREVER);

      CC_ASSERT (job != NULL);

      switch (job->type)
      {
      case IOLINK_JOB_PD_EVENT:
         if (master->user_cb)
         {
            master->user_cb (
               iolink_get_portnumber (job->port),
               master->cb_arg,
			   PD_DATA_IND,
               job->pd_event.data_len,
               job->pd_event.data);
         }
         job->type     = IOLINK_JOB_NONE;
         job->callback = NULL;
         os_mbox_post (master->mbox_avail, job, 0);
         break;
      case IOLINK_JOB_OPERATE_EVENT:
          if (master->user_cb)
          {
              master->user_cb(
                  iolink_get_portnumber(job->port),
                  master->cb_arg,
                  PORT_OPERATE_IND,
                  0,
                  NULL);
          }
          job->type     = IOLINK_JOB_NONE;
          job->callback = NULL;
          os_mbox_post(master->mbox_avail, job, 0);
          break;
      case IOLINK_JOB_COMLOST_EVENT:
          if (master->user_cb)
          {
              master->user_cb(iolink_get_portnumber(job->port),
                              master->cb_arg, PORT_COMLOST_IND, 0, NULL);
          }
          job->type     = IOLINK_JOB_NONE;
          job->callback = NULL;
          os_mbox_post(master->mbox_avail, job, 0);
          break;
      case IOLINK_JOB_PL_ERROR_EVENT:
          if (master->user_cb)
          {
              master->user_cb(
                  iolink_get_portnumber(job->port),
                  master->cb_arg,
                  PL_ERR_IND,
                  (uint16_t)sizeof (job->pl_error.error_flags),
                  (const uint8_t *)&job->pl_error.error_flags);
          }
          job->type     = IOLINK_JOB_NONE;
          job->callback = NULL;
          os_mbox_post(master->mbox_avail, job, 0);
          break;
      case IOLINK_JOB_DEV_EVENT:
      {
         if (master->user_cb)
         {
            /*
               * Wire format sent to user_cb:
               *   byte 0        = event_cnt
               *   bytes 1..3    = entry 0: [event_qualifier, code_hi, code_lo]
               *   bytes 4..6    = entry 1  (if present)
               *   ...           up to 6 entries = 19 bytes max
               */
            uint8_t cnt = job->dev_event.event_cnt;
            uint8_t buf[1u + 6u * 3u];
            uint8_t idx = 0u;

            buf[idx++] = cnt;
            for (uint8_t i = 0u; i < cnt && i < 6u; i++)
            {
                  diag_entry_t * e = &job->dev_event.events[i];
                  buf[idx++] = e->event_qualifier;
                  buf[idx++] = (uint8_t)((uint16_t)e->event_code >> 8u);
                  buf[idx++] = (uint8_t)((uint16_t)e->event_code & 0xFFu);
            }

            master->user_cb(
                  iolink_get_portnumber(job->port),
                  master->cb_arg,
                  EVENT_DATA_IND,
                  idx,
                  buf);
         }
         job->type     = IOLINK_JOB_NONE;
         job->callback = NULL;
         os_mbox_post(master->mbox_avail, job, 0);
         break;
      }
      case IOLINK_JOB_SM_OPERATE_REQ:
      case IOLINK_JOB_SM_SET_PORT_CFG_REQ:
      case IOLINK_JOB_DL_MODE_IND:
      case IOLINK_JOB_DL_READ_CNF:
      case IOLINK_JOB_DL_WRITE_CNF:
      case IOLINK_JOB_DL_WRITE_DEVMODE_CNF:
      case IOLINK_JOB_DL_READPARAM_CNF:
      case IOLINK_JOB_DL_WRITEPARAM_CNF:
      case IOLINK_JOB_DL_ISDU_TRANS_CNF:
      case IOLINK_JOB_DL_PDINPUT_TRANS_IND:
      case IOLINK_JOB_MW_CONTROL_CNF:
      case IOLINK_JOB_MW_READ_REQ:
      case IOLINK_JOB_MW_READ_CNF:
      case IOLINK_JOB_MW_WRITE_REQ:
      case IOLINK_JOB_MW_WRITE_CNF:

      case IOLINK_JOB_SM_PORT_MODE_IND:
      case IOLINK_JOB_DL_EVENT_IND:
      case IOLINK_JOB_DL_CONTROL_IND:
      case IOLINK_JOB_DS_STARTUP:
      case IOLINK_JOB_DS_DELETE:
      case IOLINK_JOB_DS_INIT:
      case IOLINK_JOB_DS_UPLOAD:
      case IOLINK_JOB_DS_READY:
      case IOLINK_JOB_DS_CHANGE:
      case IOLINK_JOB_DS_FAULT:
      case IOLINK_JOB_OD_START:
      case IOLINK_JOB_OD_STOP:
      case IOLINK_JOB_MW_EVENT_RSP:
      case IOLINK_JOB_PERIODIC:
         if (job->callback)
         {
            job->callback (job);
         }
         job->type     = IOLINK_JOB_NONE;
         job->callback = NULL;
         os_mbox_post (master->mbox_avail, job, 0);
         break;
      case IOLINK_JOB_MW_ABORT:
      case IOLINK_JOB_MW_EVENT_REQ:

      case IOLINK_JOB_MW_PORTCONFIGURATION:
      case IOLINK_JOB_MW_READBACKPORTCONFIGURATION:
      case IOLINK_JOB_MW_PORTSTATUS:
      case IOLINK_JOB_MW_DEVICE_WRITE:
      case IOLINK_JOB_MW_DEVICE_READ:
      case IOLINK_JOB_MW_PARAM_READ:
      case IOLINK_JOB_MW_PARAM_WRITE:

         if (job->callback)
         {
            job->callback (job);
         }
         job->type     = IOLINK_JOB_NONE;
         job->callback = NULL;
         os_mbox_post (master->mbox_api_avail, job, 0);
         break;
      case IOLINK_JOB_EXIT:
         running = false;
         break;
      default:
         CC_ASSERT (0);
         break;
      }
   }

   master->has_exited = true;
}

/* Stack internal API */
iolink_job_t * iolink_fetch_avail_job (iolink_port_t * port)
{
   iolink_job_t * job;

   if (os_mbox_fetch (port->master->mbox_avail, (void **)&job, 0))
   {
	   IOL_LOG_ERR("iolink_fetch_avail_job: mbox exhausted on port %u!\n",
			   iolink_get_portnumber(port));
	   return NULL;
//      CC_ASSERT (0); // TODO: This is bad! How to continue?
   }

   job->port = port;

   return job;
}

iolink_job_t * iolink_fetch_avail_api_job (iolink_port_t * port)
{
   iolink_job_t * job;

   if (os_mbox_fetch (port->master->mbox_api_avail, (void **)&job, 0))
   {
//      CC_ASSERT (0); // TODO: This is fine, return busy
	      IOL_LOG_ERR (
	         "iolink_fetch_avail_api_job: mbox exhausted on port %u - returning busy\n",
	         iolink_get_portnumber (port));
	      return NULL;
   }

   job->port = port;

   return job;
}


void iolink_post_job_with_type_and_callback (
   iolink_port_t * port,
   iolink_job_t * job,
   iolink_job_type_t type,
   void (*callback) (struct iolink_job * job))
{
	if (job == NULL)
	{
		IOL_LOG_ERR (
				"iolink_post_job_with_type_and_callback: NULL job on port %u, dropping event type %d\n",
	         iolink_get_portnumber (port),
	         type);
	     	return;
	}
   job->type     = type;
   job->callback = callback;

   bool res = os_mbox_post (port->master->mbox, job, 0);
   if (res)
   {
      IOL_LOG_ERR (
         "iolink_post_job_with_type_and_callback: mbox post failed on port %u, type %d\n",
         iolink_get_portnumber (port),
         type);
      /* Return the job slot to the available pool so it is not lost */
      job->type     = IOLINK_JOB_NONE;
      job->callback = NULL;
      os_mbox_post (port->master->mbox_avail, job, 0);
   }
}


bool iolink_post_job_pd_event (
   iolink_port_t * port,
   uint32_t timeout,
   uint8_t data_len,
   const uint8_t * data)
{
   iolink_job_t * job;

   os_mbox_fetch (port->master->mbox_avail, (void **)&job, OS_WAIT_FOREVER);

   job->port              = port;
   job->type              = IOLINK_JOB_PD_EVENT;
   job->pd_event.data_len = data_len;
   job->pd_event.data     = data;

   uint8_t safe_len = (data_len <= IOLINK_PD_MAX_SIZE) ? data_len : IOLINK_PD_MAX_SIZE;
   memcpy (job->pd_event.data_buf, data, safe_len);
   job->pd_event.data = job->pd_event.data_buf;

   bool res = os_mbox_post (port->master->mbox, job, timeout);
   if(res){
	   IOL_LOG_ERR (
			   "iolink_post_job_pd_event: mbox post failed on port %u\n",
	         iolink_get_portnumber (port));
	      /* Return the job slot to the available pool so it is not lost */
	      job->type     = IOLINK_JOB_NONE;
	      job->callback = NULL;
	      os_mbox_post (port->master->mbox_avail, job, 0);
   }

   return res;
}

void iolink_post_job_operate_event(iolink_port_t * port)
{
    iolink_job_t * job;
    os_mbox_fetch(port->master->mbox_avail, (void **)&job, OS_WAIT_FOREVER);
    job->port     = port;
    job->type     = IOLINK_JOB_OPERATE_EVENT;
    job->callback = NULL;
    os_mbox_post(port->master->mbox, job, 0);
}

void iolink_post_job_comlost_event(iolink_port_t * port)
{
    iolink_job_t * job;
    os_mbox_fetch(port->master->mbox_avail, (void **)&job, OS_WAIT_FOREVER);
    job->port     = port;
    job->type     = IOLINK_JOB_COMLOST_EVENT;
    job->callback = NULL;
    os_mbox_post(port->master->mbox, job, 0);
}

void iolink_post_job_pl_error_event(iolink_port_t * port, uint16_t error_flags)
{
    iolink_job_t * job;
    os_mbox_fetch(port->master->mbox_avail, (void **)&job, OS_WAIT_FOREVER);
    job->port                  = port;
    job->type                  = IOLINK_JOB_PL_ERROR_EVENT;
    job->callback              = NULL;
    job->pl_error.error_flags  = error_flags;
    os_mbox_post(port->master->mbox, job, 0);
}

iolink_port_t * iolink_get_port (iolink_m_t * master, uint8_t portnumber)
{
   uint8_t port_index = portnumber - 1;

   if ((portnumber == 0) || (port_index >= master->port_cnt))
   {
      return NULL;
   }

   return &master->ports[port_index];
}

uint8_t iolink_get_portnumber (iolink_port_t * port)
{
   return port->portnumber;
}

uint8_t iolink_get_port_cnt (iolink_port_t * port)
{
   return port->master->port_cnt;
}

iolink_port_info_t * iolink_get_port_info (iolink_port_t * port)
{
   return &port->port_info;
}

const iolink_smp_parameterlist_t * iolink_get_paramlist (iolink_port_t * port)
{
   return &port->sm.real_paramlist;
}

iolink_transmission_rate_t iolink_get_transmission_rate (iolink_port_t * port)
{
   return mhmode_to_transmission_rate (port->sm.comrate);
}

iolink_mw_port_t * iolink_get_mw_ctx (iolink_port_t * port)
{
   return &port->mw;
}


iolink_dl_t * iolink_get_dl_ctx (iolink_port_t * port)
{
   return &port->dl;
}

iolink_ds_port_t * iolink_get_ds_ctx (iolink_port_t * port)
{
   /* DS layer not integrated in this project variant */
   (void)port;
   return NULL;
}



iolink_pl_port_t * iolink_get_pl_ctx (iolink_port_t * port)
{
   return &port->pl;
}

iolink_sm_port_t * iolink_get_sm_ctx (iolink_port_t * port)
{
   return &port->sm;
}


iolink_m_t * Master_init (const iolink_m_cfg_t * m_cfg)
{
	int i;

	uint8_t port_index;
	uint8_t chip_index;
	uint8_t chips_needed;
	uint8_t chips_up = 0;

   if (the_master != NULL)
   {
	   IOL_LOG_DBG ( "%s: the_master returned\r\n", __func__);
	   return the_master;
   }

   if ((m_cfg->port_cnt == 0) || (m_cfg->port_cnt > IOLINK_NUM_PORTS))
   {
      CC_ASSERT (m_cfg->port_cnt <= IOLINK_NUM_PORTS);
      return NULL;
   }

   /*
    * Port -> hardware mapping (fixed, in order):
    *    chip    = (port_index / IOL_PORTS_PER_CHIP)   -> devicetree cce4511_<chip>
    *    channel = (port_index % IOL_PORTS_PER_CHIP)   -> PL "arg" of the port
    * Only chips that actually carry a configured port are brought up.
    */
   chips_needed = (uint8_t)((m_cfg->port_cnt + IOL_PORTS_PER_CHIP - 1) /
                            IOL_PORTS_PER_CHIP);

   for (chip_index = 0; chip_index < chips_needed; chip_index++)
   {
      struct iol_chip * chip = &s_chips[chip_index];
      iolink_hw_drv_t * hw   = NULL;
      uint8_t           first = chip_index * IOL_PORTS_PER_CHIP;
      uint8_t           last  = first + IOL_PORTS_PER_CHIP;

      if (last > m_cfg->port_cnt)
      {
         last = m_cfg->port_cnt;
      }

      chip->hw = NULL;

      /* Stage 1: SPI device + IRQ GPIO (devicetree / pin setup). */
      if (iol_chip_hw_init (chip) == 0)
      {
         /* Stage 2: talk to the chip (reset, revision check, channel setup). */
         const iolink_cce4511_cfg_t cfg = {
            .spi_slave_name = chip->name,
         };

         hw = iolink_cce4511_init (&cfg);
         if (hw == NULL)
         {
            IOL_LOG_ERR ("%s: CCE4511 REG CONFIG FAIL - failed to open driver\r\n",
                         chip->name);
            iol_chip_hw_deinit (chip);
         }
      }

      chip->hw = hw;

      if (hw != NULL)
      {
         chips_up++;
         IOL_LOG_INF ("%s: CCE4511 ready - ports %u..%u\r\n",
                      chip->name, first + 1U, last);
      }
      else
      {
         IOL_LOG_ERR ("%s: NOT available - ports %u..%u disabled\r\n",
                      chip->name, first + 1U, last);

         /*
          * Tell the application, for BOTH failure stages (bad devicetree /
          * GPIO / SPI setup as well as no response from the chip). The
          * master/port/job-queue objects don't exist yet, so the normal
          * job-posting path can't be used; call user_cb directly, using the
          * same PL_ERR_IND mechanism the stack uses for runtime PHY errors.
          */
         if (m_cfg->user_cb != NULL)
         {
            uint16_t err_flags = IOL_PL_ERR_CHIP_NOT_DETECTED;

            for (port_index = first; port_index < last; port_index++)
            {
               m_cfg->user_cb (
                  port_index + 1,
                  m_cfg->cb_arg,
                  PL_ERR_IND,
                  (uint8_t)sizeof (err_flags),
                  (const uint8_t *)&err_flags);
            }
         }
      }
   }

   if (chips_up == 0)
   {
      IOL_LOG_ERR ("Master_init: no CCE4511 chip could be initialised\r\n");
      return NULL;
   }

   for (port_index = 0; port_index < m_cfg->port_cnt; port_index++)
   {
      iolink_port_cfg_t * pcfg = &m_cfg->port_cfgs[port_index];
      iolink_hw_drv_t *   hw   = s_chips[port_index / IOL_PORTS_PER_CHIP].hw;

      pcfg->drv = hw;
      pcfg->arg = (void *)(uintptr_t)(port_index % IOL_PORTS_PER_CHIP);

      if ((hw == NULL) && (pcfg->mode != NULL))
      {
         /* Chip missing: keep the master running on the remaining chips and
          * tell the application this port is unusable. */
         *pcfg->mode = iolink_mode_INACTIVE;
         IOL_LOG_WRN ("Master_init: port %u disabled (chip %u not available)\r\n",
                      port_index + 1, port_index / IOL_PORTS_PER_CHIP);
      }
   }

   size_t total =
       sizeof(iolink_m_t) +
       sizeof(iolink_port_t) * m_cfg->port_cnt;

   iolink_m_t * master = os_malloc(total);

   if (master != NULL)
   {
       memset(master, 0, total);
   }
   else
   {
	   IOL_LOG_DBG("Master Malloc failed\r\n");
	   iol_chips_shutdown ();
       return NULL;
   }

   master->has_exited = false;

   master->port_cnt = m_cfg->port_cnt;
   master->cb_arg   = m_cfg->cb_arg;
   master->user_cb    = m_cfg->user_cb;

   for (port_index = 0; port_index < master->port_cnt; port_index++)
   {
      const iolink_port_cfg_t * port_cfg = &m_cfg->port_cfgs[port_index];

      iolink_port_t * port = &(master->ports[port_index]);

      port->master     = master;
      port->portnumber = port_index + 1;

      iolink_pl_init (port, port_cfg->drv, port_cfg->arg);
      iolink_sm_init (port);
      iolink_mw_init (port);

      // iolink_pl_port_power_on(port);

      iolink_sm_port_t *sm = iolink_get_sm_ctx (port);
      sm->config_paramlist.cycletime = port_cfg->cyc_time;

   }

   master->mbox 		  = os_mbox_create (IOLINK_MASTER_JOB_CNT + IOLINK_MASTER_JOB_API_CNT);
   master->mbox_avail     = os_mbox_create (IOLINK_MASTER_JOB_CNT);
   master->mbox_api_avail = os_mbox_create (IOLINK_MASTER_JOB_API_CNT);

   for (i = 0; i < IOLINK_MASTER_JOB_CNT; i++)
   {
      master->job[i].type = IOLINK_JOB_NONE;
      os_mbox_post (master->mbox_avail, &master->job[i], 0);
   }

   for (i = 0; i < IOLINK_MASTER_JOB_API_CNT; i++)
   {
      master->job_api[i].type = IOLINK_JOB_NONE;
      os_mbox_post (master->mbox_api_avail, &master->job_api[i], 0);
   }

   CC_ASSERT (master->mbox_avail != NULL);

   master->thread = os_thread_create (
      "iolink_m_thread",
      CONFIG_IOL_MASTER_TASK_PRIORITY,
      NULL,
      (uint16_t)CONFIG_IOL_MASTER_STACK_SIZE,
      iolink_main,
      master);

   CC_ASSERT (master->thread != NULL);

   for (port_index = 0; port_index < master->port_cnt; port_index++)
   {
       if (m_cfg->port_cfgs[port_index].mode != iolink_mode_INACTIVE){

    	   iolink_dl_instantiate (
    		  &master->ports[port_index],
    	      CONFIG_IOL_DL_TASK_PRIORITY,
    	      CONFIG_IOL_DL_STACK_SIZE);
       }
   }

   the_master = master;

   return master;
}

void Master_deinit (iolink_m_t ** m)
{
   int i;
   iolink_m_t * master = *m;
   iolink_job_t job;

   if (master == NULL)
   {
      return;
   }

   /* Stop all chip interrupts first so no ISR runs while we tear down. */
   for (i = 0; i < IOL_NUM_CHIPS; i++) {
      iol_chip_hw_deinit(&s_chips[i]);
   }

   for (int i = 0; i < master->port_cnt; i++) {
      iolink_dl_stop(&master->ports[i], 2000U);
      iolink_mw_deinit(&master->ports[i]);
   }

   job.type = IOLINK_JOB_EXIT;
   if (os_mbox_post (master->mbox, &job, 0))
   {
      CC_ASSERT (0);
   }

   while (master->has_exited == false)
   {
      os_usleep (1 * 1000);
   }

   os_thread_destroy (master->thread);   /* <-- ADD THIS */
   master->thread = NULL;

   /*
    * One driver instance serves IOL_PORTS_PER_CHIP ports. Deinitialise each
    * chip exactly once (through its first port), then detach its sibling
    * ports so nothing keeps a pointer to the freed driver.
    */
   for (uint8_t c = 0; c < IOL_NUM_CHIPS; c++)
   {
      uint8_t first = c * IOL_PORTS_PER_CHIP;
      uint8_t last  = first + IOL_PORTS_PER_CHIP;

      if (first >= master->port_cnt)
      {
         break;
      }
      if (last > master->port_cnt)
      {
         last = master->port_cnt;
      }

      s_chips[c].hw = NULL;
      iolink_pl_deinit(&master->ports[first]);   /* no-op if chip never came up */

      for (uint8_t p = first; p < last; p++)
      {
         iolink_get_pl_ctx(&master->ports[p])->drv = NULL;
      }
   }

   for (i = 0; i < master->port_cnt; i++)
   {
      iolink_port_t * p = &(master->ports[i]);
      os_mutex_destroy (p->mw.mtx_pdin);
   }

   os_mbox_destroy (master->mbox);
   os_mbox_destroy (master->mbox_avail);
   os_mbox_destroy (master->mbox_api_avail);

   the_master = NULL;
   os_free (*m);
   *m = NULL;
}

iolink_error_t Master_setPortMode(
		uint8_t portnumber,
		PortMode mode){

	if(the_master == NULL){
		return IOLINK_ERROR_MASTER_INIT;
	}

	iolink_port_t * port = iolink_get_port(the_master, portnumber);

	if (port == NULL)
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	iolink_sm_port_t *sm = iolink_get_sm_ctx (port);
	iolink_mw_port_t *mw = iolink_get_mw_ctx(port);

	if (mode == PORT_ACTIVE)
	{
		if (mw->mw_state == MW_STATE_Inactive ||
		    mw->mw_state == MW_STATE_Recovery)
		{
			sm->config_paramlist.portnumber= portnumber;
			sm->config_paramlist.mode = IOLINK_SMTARGET_MODE_AUTOCOM;
			sm->config_paramlist.revisionid = USER_REV_ID;
			sm->config_paramlist.inspectionlevel = IOLINK_INSPECTIONLEVEL_NO_CHECK;
			sm->config_paramlist.vendorid = USER_VENDOR_ID;
			sm->config_paramlist.deviceid = USER_DEVICE_ID;
			sm->config_paramlist.functionid = USER_FUNC_ID;
			iolink_mw_event(port, MW_EVENT_Master_Port_Active);
		}
	}
	else
	{
		sm->config_paramlist.mode = IOLINK_SMTARGET_MODE_INACTIVE;
		iolink_mw_event(port, MW_EVENT_Master_Port_Inactive);
	}

	return IOLINK_ERROR_NONE;
}



iolink_error_t Master_readParameter(
		 uint8_t portnumber,
		 uint16_t index,
		 uint8_t subindex,
		 uint8_t *buf,
		 uint8_t *len){

	if (the_master == NULL)
	{
		return IOLINK_ERROR_MASTER_INIT;
	}

	if ((buf == NULL) || (len == NULL))
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	iolink_port_t * p = iolink_get_port(the_master, portnumber);
	if (p == NULL)
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	if ((portnumber == 0) || (portnumber > IOLINK_NUM_PORTS))
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	iolink_mw_port_t *mw = iolink_get_mw_ctx(p);

    if (mw->mw_state != MW_STATE_Operate)
    {
		return IOLINK_ERROR_SERVICE_TEMP_UNAVAILABLE;
    }

	master_read_ctx_t * ctx = &s_master_read_ctx[portnumber - 1];
	if (ctx->in_use)
	{
		return IOLINK_ERROR_SERVICE_TEMP_UNAVAILABLE;
	}

	ctx->in_use   = true;
	ctx->ev       = os_event_create();
	ctx->res      = IOLINK_ERROR_SERVICE_TEMP_UNAVAILABLE;
	ctx->out_buf  = buf;
	ctx->out_len  = len;
	*len = 0;

	mw->service.index = index;
	mw->service.subindex = subindex;
	mw->service.direction = IOLINK_RWDIRECTION_READ;

	iolink_error_t req_err = MW_Read_req(p, index, subindex, master_read_cnf_cb);
	if (req_err != IOLINK_ERROR_NONE)
	{
		os_event_destroy(ctx->ev);
		memset(ctx, 0, sizeof(*ctx));
		return req_err;
	}

	uint32_t v = 0;
	bool timed_out = os_event_wait(ctx->ev, MASTER_RW_DONE_BIT, &v, MASTER_RW_TIMEOUT_MS);
	if (!timed_out)
	{
		os_event_clr(ctx->ev, v);
	}

	iolink_error_t res = timed_out ? IOLINK_ERROR_NO_COMM : ctx->res;

	os_event_destroy(ctx->ev);
	memset(ctx, 0, sizeof(*ctx));
	return res;
}

iolink_error_t Master_writeParameter(
		 uint8_t portnumber,
		 uint16_t index,
		 uint8_t subindex,
		 const uint8_t *buf,
		 uint8_t len){

	/* Synchronous wrapper over MW_Write_req(). */

	if (the_master == NULL)
	{
		return IOLINK_ERROR_MASTER_INIT;
	}

	if ((buf == NULL) && (len != 0))
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	iolink_port_t * p = iolink_get_port(the_master, portnumber);
	if (p == NULL)
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	if ((portnumber == 0) || (portnumber > IOLINK_NUM_PORTS))
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	master_write_ctx_t * ctx = &s_master_write_ctx[portnumber - 1];
	if (ctx->in_use)
	{
		return IOLINK_ERROR_SERVICE_TEMP_UNAVAILABLE;
	}

	ctx->in_use = true;
	ctx->ev     = os_event_create();
	ctx->res    = IOLINK_ERROR_SERVICE_TEMP_UNAVAILABLE;

	iolink_error_t req_err = MW_Write_req(p, index, subindex, len, buf, master_write_cnf_cb);
	if (req_err != IOLINK_ERROR_NONE)
	{
		os_event_destroy(ctx->ev);
		memset(ctx, 0, sizeof(*ctx));
		return req_err;
	}

	uint32_t v = 0;
	bool timed_out = os_event_wait(ctx->ev, MASTER_RW_DONE_BIT, &v, MASTER_RW_TIMEOUT_MS);
	if (!timed_out)
	{
		os_event_clr(ctx->ev, v);
	}

	iolink_error_t res = timed_out ? IOLINK_ERROR_NO_COMM : ctx->res;

	os_event_destroy(ctx->ev);
	memset(ctx, 0, sizeof(*ctx));
	return res;
}


iolink_error_t Master_PDin (
   uint8_t    portnumber,
   bool     * pd_valid,
   uint8_t  * data,
   uint8_t  * len)
{
   if (the_master == NULL)
   {
      return IOLINK_ERROR_MASTER_INIT;
   }

   if ((data == NULL) || (len == NULL))
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   if ((portnumber == 0) || (portnumber > the_master->port_cnt))
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   iolink_port_t * port = iolink_get_port (the_master, portnumber);
   if (port == NULL)
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   iolink_mw_port_t * mw = iolink_get_mw_ctx (port);


   if (mw->mw_state != MW_STATE_Operate)
   {
      IOL_LOG_WRN (
         "Master_PDin: port %u MW state %u is not OPERATE\n",
         portnumber,
         mw->mw_state);
      return IOLINK_ERROR_STATE_CONFLICT;
   }

   if (*len > mw->pdin_data_len)
   {
      return IOLINK_ERROR_PDOUTLENGTH;
   }

   bool valid = iolink_dl_get_pd_valid_status (port);
   if (pd_valid != NULL)
   {
      *pd_valid = valid;
   }

   os_mutex_lock (mw->mtx_pdin);
   memcpy (data, mw->pdin_data, mw->pdin_data_len);
   *len = mw->pdin_data_len;
   os_mutex_unlock (mw->mtx_pdin);

   return IOLINK_ERROR_NONE;
}

iolink_error_t Master_PDout (
   uint8_t        portnumber,
   bool           pd_valid,
   const uint8_t * data,
   uint8_t        len)
{
   if (the_master == NULL)
   {
      return IOLINK_ERROR_MASTER_INIT;
   }

   /* data may be NULL only when pd_valid=false (just marking invalid) */
   if ((data == NULL) && (len > 0))
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   if (len > IOLINK_PD_MAX_SIZE)
   {
      return IOLINK_ERROR_PDOUTLENGTH;
   }

   if ((portnumber == 0) || (portnumber > the_master->port_cnt))
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   iolink_port_t * port = iolink_get_port (the_master, portnumber);
   if (port == NULL)
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   iolink_mw_port_t * mw = iolink_get_mw_ctx (port);
   if (mw->mw_state != MW_STATE_Operate)
   {
      IOL_LOG_WRN (
         "Master_PDout: port %u MW state %u is not OPERATE\n",
         portnumber,
         mw->mw_state);
      return IOLINK_ERROR_STATE_CONFLICT;
   }

   if ((data != NULL) && (len > 0))
   {
      iolink_error_t err = MW_SetOutput_req (port, (uint8_t *)data);
      if (err != IOLINK_ERROR_NONE)
      {
         IOL_LOG_ERR (
            "Master_PDout: port %u MW_SetOutput_req failed (err=%d)\n",
            portnumber,
            (int)err);
         return err;
      }
   }

   iolink_controlcode_t ctrl = pd_valid
      ? IOLINK_CONTROLCODE_PDOUTVALID
      : IOLINK_CONTROLCODE_PDOUTINVALID;

   iolink_error_t ctrl_err = MW_Control_req (port, ctrl);
   if (ctrl_err != IOLINK_ERROR_NONE)
   {
      IOL_LOG_WRN (
         "Master_PDout: port %u MW_Control_req(%s) failed (err=%d)\n",
         portnumber,
         pd_valid ? "PDOUTVALID" : "PDOUTINVALID",
         (int)ctrl_err);
   }

   return IOLINK_ERROR_NONE;
}


iolink_error_t Master_ConfigPDFilter (
   uint8_t portnumber,
   uint8_t extract_offset,
   uint8_t extract_len)
{
   if (the_master == NULL)
   {
      return IOLINK_ERROR_MASTER_INIT;
   }

   if ((portnumber == 0) || (portnumber > the_master->port_cnt))
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   if ((extract_len > 0) && ((uint16_t)extract_offset + extract_len > IOLINK_PD_MAX_SIZE))
   {
      IOL_LOG_WRN (
         "Master_ConfigPDFilter: port %u offset+len (%u+%u) exceeds PD_MAX_SIZE (%u)\n",
         portnumber,
         extract_offset,
         extract_len,
         IOLINK_PD_MAX_SIZE);
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   iolink_port_t * port = iolink_get_port (the_master, portnumber);
   if (port == NULL)
   {
      return IOLINK_ERROR_PARAMETER_CONFLICT;
   }

   MW_ConfigPDFilter (port, extract_offset, extract_len);
   return IOLINK_ERROR_NONE;
}


iolink_error_t Master_PortPowerOffOn(
    uint8_t portnumber,
    PortPowerMode powerMode,
    uint16_t PowerOffTime)
{
	if (the_master == NULL)
	{
		return IOLINK_ERROR_MASTER_INIT;
	}

	if ((portnumber == 0) || (portnumber > the_master->port_cnt))
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}

	iolink_port_t * port = iolink_get_port (the_master, portnumber);
	if (port == NULL)
	{
		return IOLINK_ERROR_PARAMETER_CONFLICT;
	}
    iolink_mw_port_t * mw = iolink_get_mw_ctx (port);

    mw->port_pwr_info.offSetTime = PowerOffTime;
    mw->port_pwr_info.powerMode  = powerMode;


    switch (powerMode)
    {
    	case SwitchPowerOn:
    		if (mw->mw_state == MW_STATE_Inactive ||
    				mw->mw_state == MW_STATE_Recovery)
    		{
    			iolink_dl_reset(port);
    			iolink_mw_event(port, MW_EVENT_power_on_req);
    		}
    		break;

        case SwitchPowerOff:
        	if (mw->mw_state != MW_STATE_Inactive)
        	{
        		iolink_dl_reset(port);
        		iolink_mw_event(port, MW_EVENT_power_off_req);
        	}
        	break;

        case OneTimeSwitchOff:
        	if (mw->mw_state != MW_STATE_Inactive)
        	{
        		iolink_dl_reset(port);
        		iolink_mw_event(port, MW_EVENT_power_one_time_off);
        	}
        	break;

        default:
            return IOLINK_ERROR_PARAMETER_CONFLICT;
    }

    return IOLINK_ERROR_NONE;
}
void iolink_post_job_dev_event(
        iolink_port_t * port,
        uint8_t         event_cnt,
        diag_entry_t    events[6])
{
    iolink_m_t * master = port->master;   /* full struct visible here */

    if ((master == NULL) || (master->user_cb == NULL) || (event_cnt == 0))
    {
        return;
    }

    iolink_job_t * job = iolink_fetch_avail_job(port);
    if (job == NULL)
    {
        IOL_LOG_ERR("iolink_post_job_dev_event: no job slot on port %u\n",
                    iolink_get_portnumber(port));
        return;
    }

    job->port                = port;
    job->type                = IOLINK_JOB_DEV_EVENT;
    job->callback            = NULL;

    uint8_t n = (event_cnt <= 6u) ? event_cnt : 6u;
    job->dev_event.event_cnt = n;
    memcpy(job->dev_event.events, events, n * sizeof(diag_entry_t));

    os_mbox_post(master->mbox, job, 0);
}
/************** (C) COPYRIGHT 2026 Calixto Systems Pvt Ltd *****END OF FILE****/