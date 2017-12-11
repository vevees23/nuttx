/****************************************************************************
 * arch/arm/src/lpc54xx/lpc54_i2c_master.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/wdog.h>
#include <nuttx/semaphore.h>
#include <nuttx/i2c/i2c_master.h>

#include <nuttx/irq.h>
#include <arch/board/board.h>

#include "chip.h"
#include "up_arch.h"
#include "up_internal.h"

#include "chip/lpc54_pinmux.h"
#include "chip/lpc54_syscon.h"
#include "chip/lpc54_flexcomm.h"
#include "chip/lpc54_i2c.h"
#include "lpc54_config.h"
#include "lpc54_i2c_master.h"

#include <arch/board/board.h>

#ifdef HAVE_SPI_MASTER_DEVICE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Private Data
 ****************************************************************************/

struct lpc54_i2cdev_s
{
  struct i2c_master_s dev;     /* Generic I2C device */
  uintptr_t        base;       /* Base address of Flexcomm registers */
  uint16_t         irq;        /* Flexcomm IRQ number */
  uint32_t         fclock;     /* Flexcomm function clock frequency */

  sem_t            exclsem;    /* Only one thread can access at a time */
  sem_t            waitsem;    /* Supports wait for state machine completion */
  volatile uint8_t state;      /* State of state machine */
  WDOG_ID          timeout;    /* watchdog to timeout when bus hung */
  uint32_t         frequency;  /* Current I2C frequency */

  struct i2c_msg_s *msgs;      /* remaining transfers - first one is in progress */
  unsigned int     nmsg;       /* number of transfer remaining */

  uint16_t         wrcnt;      /* number of bytes sent to tx fifo */
  uint16_t         rdcnt;      /* number of bytes read from rx fifo */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int  lpc54_i2c_start(struct lpc54_i2cdev_s *priv);
static void lpc54_i2c_stop(struct lpc54_i2cdev_s *priv);
static int  lpc54_i2c_interrupt(int irq, FAR void *context, FAR void *arg);
static void lpc54_i2c_timeout(int argc, uint32_t arg, ...);
static void lpc54_i2c_setfrequency(struct lpc54_i2cdev_s *priv,
              uint32_t frequency);
static int  lpc54_i2c_transfer(FAR struct i2c_master_s *dev,
              FAR struct i2c_msg_s *msgs, int count);
#ifdef CONFIG_I2C_RESET
static int lpc54_i2c_reset(FAR struct i2c_master_s * dev);
#endif

/****************************************************************************
 * I2C device operations
 ****************************************************************************/

struct i2c_ops_s lpc54_i2c_ops =
{
  .transfer = lpc54_i2c_transfer
#ifdef CONFIG_I2C_RESET
  , .reset  = lpc54_i2c_reset
#endif
};

#ifdef CONFIG_LPC54_I2C0_MASTER
static struct lpc54_i2cdev_s g_i2c0_dev;
#endif
#ifdef CONFIG_LPC54_I2C1_MASTER
static struct lpc54_i2cdev_s g_i2c1_dev;
#endif
#ifdef CONFIG_LPC54_I2C2_MASTER
static struct lpc54_i2cdev_s g_i2c2_dev;
#endif
#ifdef CONFIG_LPC54_I2C3_MASTER
static struct lpc54_i2cdev_s g_i2c3_dev;
#endif
#ifdef CONFIG_LPC54_I2C4_MASTER
static struct lpc54_i2cdev_s g_i2c4_dev;
#endif
#ifdef CONFIG_LPC54_I2C5_MASTER
static struct lpc54_i2cdev_s g_i2c5_dev;
#endif
#ifdef CONFIG_LPC54_I2C6_MASTER
static struct lpc54_i2cdev_s g_i2c6_dev;
#endif
#ifdef CONFIG_LPC54_I2C7_MASTER
static struct lpc54_i2cdev_s g_i2c7_dev;
#endif
#ifdef CONFIG_LPC54_I2C8_MASTER
static struct lpc54_i2cdev_s g_i2c8_dev;
#endif
#ifdef CONFIG_LPC54_I2C9_MASTER
static struct lpc54_i2cdev_s g_i2c9_dev;
#endif

/****************************************************************************
 * Name: lpc54_i2c_setfrequency
 *
 * Description:
 *   Set the frequency for the next transfer
 *
 ****************************************************************************/

static void lpc54_i2c_setfrequency(struct lpc54_i2cdev_s *priv,
                                   uint32_t frequency)
{
  /* Has the I2C frequency changed? */

  if (frequency != priv->frequency)
    {
      /* Yes.. instantiate the new I2C frequency */
#warning Missing logic

      priv->frequency = frequency;
    }
}

/****************************************************************************
 * Name: lpc54_i2c_start
 *
 * Description:
 *   Perform a I2C transfer start
 *
 ****************************************************************************/

static int lpc54_i2c_start(struct lpc54_i2cdev_s *priv)
{
#warning Missing logic
  return priv->nmsg;
}

/****************************************************************************
 * Name: lpc54_i2c_stop
 *
 * Description:
 *   Perform a I2C transfer stop
 *
 ****************************************************************************/

static void lpc54_i2c_stop(struct lpc54_i2cdev_s *priv)
{
#warning Missing logic
  nxsem_post(&priv->waitsem);
}

/****************************************************************************
 * Name: lpc54_i2c_timeout
 *
 * Description:
 *   Watchdog timer for timeout of I2C operation
 *
 ****************************************************************************/

static void lpc54_i2c_timeout(int argc, uint32_t arg, ...)
{
  struct lpc54_i2cdev_s *priv = (struct lpc54_i2cdev_s *)arg;

  irqstate_t flags = enter_critical_section();
  priv->state = 0xff;
  nxsem_post(&priv->waitsem);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: lpc32_i2c_nextmsg
 *
 * Description:
 *   Setup for the next message.
 *
 ****************************************************************************/

void lpc32_i2c_nextmsg(struct lpc54_i2cdev_s *priv)
{
  priv->nmsg--;

  if (priv->nmsg > 0)
    {
      priv->msgs++;
#warning Missing logic
    }
  else
    {
      lpc54_i2c_stop(priv);
    }
}

/****************************************************************************
 * Name: lpc54_i2c_interrupt
 *
 * Description:
 *   The I2C Interrupt Handler
 *
 ****************************************************************************/

static int lpc54_i2c_interrupt(int irq, FAR void *context, FAR void *arg)
{
  struct lpc54_i2cdev_s *priv = (struct lpc54_i2cdev_s *)arg;
  struct i2c_msg_s *msg;
  uint32_t state;

  DEBUGASSERT(priv != NULL);

  state = getreg32(priv->base + LPC54_I2C_STAT_OFFSET);
  msg  = priv->msgs;
#warning Missing logic

  priv->state = state;
  switch (state)
    {
#warning Missing logic
    default:
      lpc54_i2c_stop(priv);
      break;
    }

#warning Missing logic
  return OK;
}

/****************************************************************************
 * Name: lpc54_i2c_transfer
 *
 * Description:
 *   Perform a sequence of I2C transfers
 *
 ****************************************************************************/

static int lpc54_i2c_transfer(FAR struct i2c_master_s *dev,
                              FAR struct i2c_msg_s *msgs, int count)
{
  struct lpc54_i2cdev_s *priv = (struct lpc54_i2cdev_s *)dev;
  int ret;

  DEBUGASSERT(dev != NULL);

  /* Get exclusive access to the I2C bus */

  nxsem_wait(&priv->exclsem);

  /* Set up for the transfer */

  priv->wrcnt = 0;
  priv->rdcnt = 0;
  priv->msgs  = msgs;
  priv->nmsg  = count;

  /* Configure the I2C frequency.
   * REVISIT: Note that the frequency is set only on the first message.
   * This could be extended to support different transfer frequencies for
   * each message segment.
   */

  lpc54_i2c_setfrequency(priv, msgs->frequency);

  /* Perform the transfer */

  ret = lpc54_i2c_start(priv);

  nxsem_post(&priv->exclsem);
  return ret;
}

/************************************************************************************
 * Name: lpc54_i2c_reset
 *
 * Description:
 *   Perform an I2C bus reset in an attempt to break loose stuck I2C devices.
 *
 * Input Parameters:
 *   dev   - Device-specific state data
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ************************************************************************************/

#ifdef CONFIG_I2C_RESET
static int lpc54_i2c_reset(FAR struct i2c_master_s * dev)
{
#warning Missing logic
  return OK;
}
#endif /* CONFIG_I2C_RESET */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: lpc54_i2cbus_initialize
 *
 * Description:
 *   Initialise an I2C device
 *
 ****************************************************************************/

struct i2c_master_s *lpc54_i2cbus_initialize(int port)
{
  struct lpc54_i2cdev_s *priv;
  irqstate_t flags;
  uint32_t regval;
  uint32_t deffreq;

  flags = enter_critical_section();

  /* Configure the requestin I2C peripheral */
  /* NOTE:  The basic FLEXCOMM initialization was performed in
   * lpc54_lowputc.c.
   */

#ifdef CONFIG_LPC54_I2C0_MASTER
  if (port == 0)
    {
      /* Attach 12 MHz clock to FLEXCOMM0 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM0, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM0 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM0_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c0_dev;
      priv->base     = LPC54_FLEXCOMM0_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM0;
      priv->fclock   = BOARD_FLEXCOMM0_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C0_SCL);
      lpc54_gpio_config(GPIO_I2C0_SDA);

      /* Set up the FLEXCOMM0 function clock */

      putreg32(BOARD_FLEXCOMM0_CLKSEL, LPC54_SYSCON_FCLKSEL0);

      /* Set the default I2C frequency */

      deffreq = I2C0_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C1_MASTER
  if (port == 1)
    {
      /* Attach 12 MHz clock to FLEXCOMM1 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM1, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM1 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM1_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c1_dev;
      priv->base     = LPC54_FLEXCOMM1_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM1;
      priv->fclock   = BOARD_FLEXCOMM1_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C1_SCL);
      lpc54_gpio_config(GPIO_I2C1_SDA);

      /* Set up the FLEXCOMM1 function clock */

      putreg32(BOARD_FLEXCOMM1_CLKSEL, LPC54_SYSCON_FCLKSEL1);

      /* Set the default I2C frequency */

      deffreq = I2C1_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C2_MASTER
  if (port == 2)
    {
      /* Attach 12 MHz clock to FLEXCOMM2 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM2, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM2 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM2_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c2_dev;
      priv->base     = LPC54_FLEXCOMM2_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM2;
      priv->fclock   = BOARD_FLEXCOMM2_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C2_SCL);
      lpc54_gpio_config(GPIO_I2C2_SDA);

      /* Set up the FLEXCOMM2 function clock */

      putreg32(BOARD_FLEXCOMM2_CLKSEL, LPC54_SYSCON_FCLKSEL2);

      /* Set the default I2C frequency */

      deffreq = I2C2_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C3_MASTER
  if (port == 3)
    {
      /* Attach 12 MHz clock to FLEXCOMM3 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM3, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM3 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM3_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c3_dev;
      priv->base     = LPC54_FLEXCOMM3_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM3;
      priv->fclock   = BOARD_FLEXCOMM3_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C3_SCL);
      lpc54_gpio_config(GPIO_I2C3_SDA);

      /* Set up the FLEXCOMM3 function clock */

      putreg32(BOARD_FLEXCOMM3_CLKSEL, LPC54_SYSCON_FCLKSEL3);

      /* Set the default I2C frequency */

      deffreq = I2C3_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C4_MASTER
  if (port == 4)
    {
      /* Attach 12 MHz clock to FLEXCOMM4 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM4, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM4 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM4_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c4_dev;
      priv->base     = LPC54_FLEXCOMM4_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM4;
      priv->fclock   = BOARD_FLEXCOMM4_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C4_SCL);
      lpc54_gpio_config(GPIO_I2C4_SDA);

      /* Set up the FLEXCOMM4 function clock */

      putreg32(BOARD_FLEXCOMM4_CLKSEL, LPC54_SYSCON_FCLKSEL4);

      /* Set the default I2C frequency */

      deffreq = I2C4_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C5_MASTER
  if (port == 5)
    {
      /* Attach 12 MHz clock to FLEXCOMM5 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM5, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM5 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM5_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c5_dev;
      priv->base     = LPC54_FLEXCOMM5_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM5;
      priv->fclock   = BOARD_FLEXCOMM5_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C5_SCL);
      lpc54_gpio_config(GPIO_I2C5_SDA);

      /* Set up the FLEXCOMM5 function clock */

      putreg32(BOARD_FLEXCOMM5_CLKSEL, LPC54_SYSCON_FCLKSEL5);

      /* Set the default I2C frequency */

      deffreq = I2C5_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C6_MASTER
  if (port == 6)
    {
      /* Attach 12 MHz clock to FLEXCOMM6 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM6, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM6 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM6_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c6_dev;
      priv->base     = LPC54_FLEXCOMM6_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM6;
      priv->fclock   = BOARD_FLEXCOMM6_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C6_SCL);
      lpc54_gpio_config(GPIO_I2C6_SDA);

      /* Set up the FLEXCOMM6 function clock */

      putreg32(BOARD_FLEXCOMM6_CLKSEL, LPC54_SYSCON_FCLKSEL6);

      /* Set the default I2C frequency */

      deffreq = I2C6_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C7_MASTER
  if (port == 7)
    {
      /* Attach 12 MHz clock to FLEXCOMM7 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM7, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM7 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM7_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c7_dev;
      priv->base     = LPC54_FLEXCOMM7_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM7;
      priv->fclock   = BOARD_FLEXCOMM7_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C7_SCL);
      lpc54_gpio_config(GPIO_I2C7_SDA);

      /* Set up the FLEXCOMM7 function clock */

      putreg32(BOARD_FLEXCOMM7_CLKSEL, LPC54_SYSCON_FCLKSEL7);

      /* Set the default I2C frequency */

      deffreq = I2C7_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C8_MASTER
  if (port == 8)
    {
      /* Attach 12 MHz clock to FLEXCOMM8 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM8, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM8 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM8_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c8_dev;
      priv->base     = LPC54_FLEXCOMM8_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM8;
      priv->fclock   = BOARD_FLEXCOMM8_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C8_SCL);
      lpc54_gpio_config(GPIO_I2C8_SDA);

      /* Set up the FLEXCOMM8 function clock */

      putreg32(BOARD_FLEXCOMM8_CLKSEL, LPC54_SYSCON_FCLKSEL8);

      /* Set the default I2C frequency */

      deffreq = I2C8_DEFAULT_FREQUENCY;
    }
  else
#endif
#ifdef CONFIG_LPC54_I2C9_MASTER
  if (port == 9)
    {
      /* Attach 12 MHz clock to FLEXCOMM9 */

      putreg32(SYSCON_AHBCLKCTRL1_FLEXCOMM9, LPC54_SYSCON_AHBCLKCTRLSET1);

      /* Set FLEXCOMM9 to the I2C peripheral, locking that configuration
       * in place.
       */

      putreg32(FLEXCOMM_PSELID_PERSEL_I2C | FLEXCOMM_PSELID_LOCK,
               LPC54_FLEXCOMM9_PSELID);

      /* Initialize the state structure */

      priv           = &g_i2c9_dev;
      priv->base     = LPC54_FLEXCOMM9_BASE;
      priv->irq      = LPC54_IRQ_FLEXCOMM9;
      priv->fclock   = BOARD_FLEXCOMM9_FCLK;

      /* Configure I2C pins (defined in board.h) */

      lpc54_gpio_config(GPIO_I2C9_SCL);
      lpc54_gpio_config(GPIO_I2C9_SDA);

      /* Set up the FLEXCOMM9 function clock */

      putreg32(BOARD_FLEXCOMM9_CLKSEL, LPC54_SYSCON_FCLKSEL9);

      /* Set the default I2C frequency */

      deffreq = I2C9_DEFAULT_FREQUENCY;
    }
  else
#endif
    {
      return NULL;
    }

  leave_critical_section(flags);

  /* Enable the I2C peripheral and configure master  mode */
#Missing logic

  /* Set the default I2C frequency */

  lpc54_i2c_setfrequency(priv, deffreq);

  /* Initialize semaphores */

  nxsem_init(&priv->exclsem, 0, 1);
  nxsem_init(&priv->waitsem, 0, 0);

  /* The waitsem semaphore is used for signaling and, hence, should not have
   * priority inheritance enabled.
   */

  nxsem_setprotocol(&priv->waitsem, SEM_PRIO_NONE);

  /* Allocate a watchdog timer */

  priv->timeout = wd_create();
  DEBUGASSERT(priv->timeout != 0);

  /* Attach Interrupt Handler */

  irq_attach(priv->irq, lpc54_i2c_interrupt, priv);

  /* Enable Interrupt Handler */

  up_enable_irq(priv->irq);

  /* Install our operations */

  priv->dev.ops = &lpc54_i2c_ops;
  return &priv->dev;
}

/****************************************************************************
 * Name: lpc54_i2cbus_uninitialize
 *
 * Description:
 *   Uninitialise an I2C device
 *
 ****************************************************************************/

int lpc54_i2cbus_uninitialize(FAR struct i2c_master_s * dev)
{
  struct lpc54_i2cdev_s *priv = (struct lpc54_i2cdev_s *) dev;

  /* Disable I2C interrupts */
#warning Missing logic

  /* Disable the I2C peripheral */
 #warning Missing logic

  /* Disable the Flexcomm interface at the NVIC and detach the interrupt. */

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);
  return OK;
}

#endif /* HAVE_SPI_MASTER_DEVICE */