/* -----------------------------------------------------------------------------
 * Copyright (c) 2013-2014 ARM Ltd.
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software. Permission is granted to anyone to use this
 * software for any purpose, including commercial applications, and to alter
 * it and redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software in
 *    a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 *
 * $Date:        16. October 2014
 * $Revision:    V2.04
 *
 * Driver:       Driver_USBH1
 * Configured:   via RTE_Device.h configuration file
 * Project:      USB High/Full-Speed Host Driver for ST STM32F4xx
 * --------------------------------------------------------------------------
 * Use the following configuration settings in the middleware component
 * to connect to this driver.
 *
 *   Configuration Setting                Value
 *   ---------------------                -----
 *   Connect to hardware via Driver_USBH# = 1
 *   USB Host controller interface        = Custom
 * -------------------------------------------------------------------------- */

/* History:
 *  Version 2.04
 *    - Fixed support for Transfers larger then FIFO size
 *  Version 2.03
 *    - Limited transfer size to maximum FIFO size
 *    - Forced unaligned access for FIFO interactions
 *  Version 2.02
 *    - Fixed interrupt IN endpoint handling
 *    - Improved bulk endpoint retransfer
 *    - Changed default FIFO settings
 *  Version 2.01
 *    - Update for USB Host CMSIS Driver API v2.01
 *  Version 2.00
 *    - Initial release for USB Host CMSIS Driver API v2.0
 */

#include <stdint.h>
#include <string.h>
#include "cmsis_os.h"

#include "OTG_HS_STM32F4xx.h"

#include "Driver_USBH.h"

extern uint8_t otg_hs_role;
extern uint8_t otg_hs_state;

extern void OTG_HS_PinsConfigure   (uint8_t pins_mask);
extern void OTG_HS_PinsUnconfigure (uint8_t pins_mask);
extern void OTG_HS_PinVbusOnOff    (bool state);

#define OTG                         OTG_HS
#define OTG_MAX_CH                  16


/* USBH Driver ****************************************************************/

#define ARM_USBH_DRIVER_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(2,3)

/* Driver Version */
static const ARM_DRIVER_VERSION usbh_driver_version = { ARM_USBH_API_VERSION, ARM_USBH_DRIVER_VERSION };

/* Driver Capabilities */
static const ARM_USBH_CAPABILITIES usbh_driver_capabilities = {
  0x0001, /* Root HUB available Ports Mask   */
  false,  /* Automatic SPLIT packet handling */
  true,   /* Signal Connect event            */
  true,   /* Signal Disconnect event         */
  true    /* Signal Overcurrent event        */
};

/* FIFO sizes in bytes (total available memory for FIFOs is 4 kB) */
#define RX_FIFO_SIZE          2048      /* RxFIFO depth is half of max 1.25 kB*/
#define TX_FIFO_SIZE_NON_PERI 1024      /* Non-periodic Tx FIFO size          */
#define TX_FIFO_SIZE_PERI     1024      /* Periodic Tx FIFO size              */

/* Local structure definitions */
typedef struct _endpoint_info_t {
  uint8_t   type;
  uint8_t   speed;
  uint16_t  max_packet_size;
  uint16_t  interval_reload;
} endpoint_info_t;

typedef struct _transfer_info_t {
  uint32_t  packet;
  uint8_t  *data;
  uint32_t  num;
  uint32_t  num_transferred;
  uint32_t  num_transferring;
  uint16_t  interval;
  struct {
    uint8_t active      :  1;
    uint8_t in_progress :  1;
  } status;
  uint8_t   event;
} transfer_info_t;

/* Local variables and structures */
static volatile uint32_t *OTG_DFIFO[OTG_MAX_CH] = { OTG_HS_DFIFO0,  OTG_HS_DFIFO1,  OTG_HS_DFIFO2,  OTG_HS_DFIFO3,  OTG_HS_DFIFO4,  OTG_HS_DFIFO5,  OTG_HS_DFIFO6,  OTG_HS_DFIFO7,
                                                    OTG_HS_DFIFO8,  OTG_HS_DFIFO9,  OTG_HS_DFIFO10, OTG_HS_DFIFO11, OTG_HS_DFIFO12, OTG_HS_DFIFO13, OTG_HS_DFIFO14, OTG_HS_DFIFO15  };

static ARM_USBH_SignalPortEvent_t signal_port_event;
static ARM_USBH_SignalPipeEvent_t signal_pipe_event;

static endpoint_info_t endpoint_info[OTG_MAX_CH];
static transfer_info_t transfer_info[OTG_MAX_CH];

static bool port_reset = false;

/* Function Prototypes */
static int32_t ARM_USBH_PipeTransferAbort (ARM_USBH_PIPE_HANDLE pipe_hndl);


/* Local Functions */

/**
  \fn          uint32_t ARM_USBH_CH_GetIndexFromAddress (OTG_HS_HC *ptr_ch)
  \brief       Get the Index of Channel from it's Address.
  \param[in]   ptr_ch   Pointer to the Channel
  \return      Index of the Channel
*/
__INLINE static uint32_t ARM_USBH_CH_GetIndexFromAddress (OTG_HS_HC *ptr_ch) {
  return (ptr_ch - (OTG_HS_HC *)(&(OTG->HCCHAR0)));
}

/**
  \fn          OTG_HS_HC *ARM_USBH_CH_GetAddressFromIndex (uint32_t index)
  \brief       Get the Channel Address from it's Index.
  \param[in]   index    Index of the Channel
  \return      Address of the Channel
*/
__INLINE static OTG_HS_HC *ARM_USBH_CH_GetAddressFromIndex (uint32_t index) {
  return ((OTG_HS_HC *)(&(OTG->HCCHAR0)) + index);
}

/**
  \fn          void *ARM_USBH_CH_FindFree (void)
  \brief       Find a free Channel.
  \return      Pointer to the first free Channel (0 = no free Channel is available)
*/
__INLINE static void *ARM_USBH_CH_FindFree (void) {
  OTG_HS_HC *ptr_ch;
  uint32_t   i;

  ptr_ch = (OTG_HS_HC *)(&(OTG->HCCHAR0));

  for (i = 0; i < OTG_MAX_CH; i++) {
    if (!(ptr_ch->HCCHAR & 0x3FFFFFFF)) return ptr_ch;
    ptr_ch++;
  }

  return 0;
}

/**
  \fn          bool ARM_USBH_CH_Disable (OTG_HS_HC *ptr_ch)
  \brief       Disable the Channel.
  \param[in]   ptr_ch   Pointer to the Channel
  \return      true = success, false = fail
*/
__INLINE static bool ARM_USBH_CH_Disable (OTG_HS_HC *ptr_ch) {
  int i;

  if (!ptr_ch) return false;

  ptr_ch->HCTSIZ &= ~OTG_HS_HCISIZx_DOPING;

  if (ptr_ch->HCINT & OTG_HS_HCINTx_CHH) return true;

  if (ptr_ch->HCCHAR & OTG_HS_HCCHARx_CHENA) {
    ptr_ch->HCINTMSK = 0;
    osDelay(1);
    if (ptr_ch->HCINT & OTG_HS_HCINTx_NAK) {
      ptr_ch->HCINT  =  0x7BB;
      return true;
    }
    ptr_ch->HCINT  =  0x7BB;
    ptr_ch->HCCHAR =  ptr_ch->HCCHAR | OTG_HS_HCCHARx_CHENA | OTG_HS_HCCHARx_CHDIS;
    for (i =0 ; i < 1000; i++) {
      if (ptr_ch->HCINT & OTG_HS_HCINTx_CHH) {
        ptr_ch->HCINT = 0x7BB;
        return true;
      }
    }
    return false;
  }

  return true;
}

/**
  \fn          bool ARM_USBH_CH_TransferEnqueue (OTG_HS_HC *ptr_ch,
                                                 uint32_t   packet,
                                                 uint8_t   *data,
                                                 uint32_t   num)
  \brief       Enqueue the Transfer on a Channel.
  \param[in]   ptr_ch   Pointer to the Channel
  \param[in]   packet   Packet information
  \param[in]   data     Pointer to buffer with data to send or for data to receive
  \param[in]   num      Number of data bytes to transfer
  \return      true = success, false = fail
*/
static bool ARM_USBH_CH_TransferEnqueue (OTG_HS_HC *ptr_ch, uint32_t packet, uint8_t *data, uint32_t num) {
  uint32_t           hcchar;
  uint32_t           hctsiz;
  uint32_t           hcintmsk;
  uint32_t           mpsiz;
  uint32_t           ch_idx;
  uint8_t           *ptr_src;
  volatile uint32_t *ptr_dest;
  uint32_t           cnt;

  if (!ptr_ch)                          return false;
  if (!data && num)                     return false;
  if (!(OTG->HPRT & OTG_HS_HPRT_PCSTS)) return false;

  hcchar   = ptr_ch->HCCHAR;                      /* Read channel characterist*/
  hctsiz   = ptr_ch->HCTSIZ;                      /* Read channel size info   */
  hcintmsk = 0;
  cnt      = 0;
  ch_idx   = ARM_USBH_CH_GetIndexFromAddress (ptr_ch);

  /* Prepare transfer                                                         */
                                                  /* Prepare HCCHAR register  */
  hcchar &=          OTG_HS_HCCHARx_ODDFRM   |    /* Keep ODDFRM              */
                     OTG_HS_HCCHARx_DAD_MSK  |    /* Keep DAD                 */
                     OTG_HS_HCCHARx_MC_MSK   |    /* Keep MC                  */
                     OTG_HS_HCCHARx_EPTYP_MSK|    /* Keep EPTYP               */
                     OTG_HS_HCCHARx_LSDEV    |    /* Keep LSDEV               */
                     OTG_HS_HCCHARx_EPNUM_MSK|    /* Keep EPNUM               */
                     OTG_HS_HCCHARx_MPSIZ_MSK;    /* Keep MPSIZ               */
  if (packet & ARM_USBH_PACKET_PING) {
    hcchar       &= ~OTG_HS_HCCHARx_EPDIR;
    hcintmsk      =  OTG_HS_HCINTMSKx_TXERRM |
                     OTG_HS_HCINTMSKx_ACKM   |
                     OTG_HS_HCINTMSKx_NAKM   |
                     OTG_HS_HCINTMSKx_STALLM |
                     OTG_HS_HCINTMSKx_XFRCM  ;
    cnt           = 0;

    hcchar       &= ~OTG_HS_HCCHARx_CHDIS;
    hcchar       |=  OTG_HS_HCCHARx_CHENA;

                                                    /* Prepare HCTSIZ register  */
    hctsiz       &=  OTG_HS_HCTSIZx_DPID_MSK;       /* Keep DPID                */
    hctsiz       |=  OTG_HS_HCISIZx_DOPING;
  } else {
    switch (packet & (3 << ARM_USBH_PACKET_TOKEN_Pos)) {
      case ARM_USBH_PACKET_IN:
        hcchar   |=  OTG_HS_HCCHARx_EPDIR;
        hcintmsk  =  OTG_HS_HCINTMSKx_DTERRM |
                     OTG_HS_HCINTMSKx_BBERRM |
                     OTG_HS_HCINTMSKx_TXERRM |
                     OTG_HS_HCINTMSKx_ACKM   |
                     OTG_HS_HCINTMSKx_NAKM   |
                     OTG_HS_HCINTMSKx_STALLM |
                     OTG_HS_HCINTMSKx_XFRCM  ;
        break;
      case ARM_USBH_PACKET_OUT:
        hcchar   &= ~OTG_HS_HCCHARx_EPDIR;
        hcintmsk  =  OTG_HS_HCINTMSKx_TXERRM |
                     OTG_HS_HCINTMSKx_NYET   |
                     OTG_HS_HCINTMSKx_ACKM   |
                     OTG_HS_HCINTMSKx_NAKM   |
                     OTG_HS_HCINTMSKx_STALLM |
                     OTG_HS_HCINTMSKx_XFRCM  ;
        cnt       = (num + 3) / 4;
        break;
      case ARM_USBH_PACKET_SETUP:
        hcchar   &= ~OTG_HS_HCCHARx_EPDIR;
        hcintmsk  =  OTG_HS_HCINTMSKx_TXERRM |
                     OTG_HS_HCINTMSKx_XFRCM  ;
        hctsiz   &= ~OTG_HS_HCTSIZx_DPID_MSK;
        hctsiz   |=  OTG_HS_HCTSIZx_DPID_MDATA;
        cnt       = (num + 3) / 4;
        break;
    }

    switch (endpoint_info[ch_idx].type) {
      case ARM_USB_ENDPOINT_CONTROL:
      case ARM_USB_ENDPOINT_BULK:
        break;
      case ARM_USB_ENDPOINT_ISOCHRONOUS:
      case ARM_USB_ENDPOINT_INTERRUPT:
        if (OTG->HFNUM & 1)
          hcchar &= ~OTG_HS_HCCHARx_ODDFRM;
        else
          hcchar |=  OTG_HS_HCCHARx_ODDFRM;
        break;
    }
    hcchar       &= ~OTG_HS_HCCHARx_CHDIS;
    hcchar       |=  OTG_HS_HCCHARx_CHENA;

                                                  /* Prepare HCTSIZ register  */
    hctsiz       &=  OTG_HS_HCTSIZx_DPID_MSK;     /* Keep DPID                */
    switch (packet & ARM_USBH_PACKET_DATA_Msk) {
      case ARM_USBH_PACKET_DATA0:
        hctsiz   &= ~OTG_HS_HCTSIZx_DPID_MSK;
        hctsiz   |=  OTG_HS_HCTSIZx_DPID_DATA0;
        break;
      case ARM_USBH_PACKET_DATA1:
        hctsiz   &= ~OTG_HS_HCTSIZx_DPID_MSK;
        hctsiz   |=  OTG_HS_HCTSIZx_DPID_DATA1;
        break;
      default:
        break;
    }
  
    mpsiz = hcchar & 0x7FF;                       /* Maximum packet size      */
    if (num) {                                    /* Normal packet            */
      hctsiz |= ((num+mpsiz-1) / mpsiz) << 19;    /* Prepare PKTCNT field     */
      hctsiz |= ( num                 ) <<  0;    /* Prepare XFRSIZ field     */
    } else {                                      /* Zero length packet       */
      hctsiz |= ( 1                   ) << 19;    /* Prepare PKTCNT field     */
      hctsiz |= ( 0                   ) <<  0;    /* Prepare XFRSIZ field     */
    }
  }

  if (cnt) {
    ptr_src  = data;
    ptr_dest = OTG_DFIFO[ch_idx];
  }

  ptr_ch->HCTSIZ   = hctsiz;                      /* Write ch transfer size   */
  ptr_ch->HCCHAR   = hcchar;                      /* Write ch characteristics */
  while (cnt--) {                                 /* Load data                */
    *ptr_dest = *((__packed uint32_t *)ptr_src);
    ptr_src  += 4;
  }
  ptr_ch->HCINTMSK = hcintmsk;                    /* Enable channel interrupts*/

  return true;
}

/* USB Host Driver Functions */

/**
  \fn          ARM_DRIVER_VERSION ARM_USBH_GetVersion (void)
  \brief       Get driver version.
  \return      \ref ARM_DRIVER_VERSION
*/
static ARM_DRIVER_VERSION ARM_USBH_GetVersion (void) { return usbh_driver_version; }

/**
  \fn          ARM_USBH_CAPABILITIES ARM_USBH_GetCapabilities (void)
  \brief       Get driver capabilities.
  \return      \ref ARM_USBH_CAPABILITIES
*/
static ARM_USBH_CAPABILITIES ARM_USBH_GetCapabilities (void) { return usbh_driver_capabilities; }

/**
  \fn          int32_t ARM_USBH_Initialize (ARM_USBH_SignalPortEvent_t cb_port_event,
                                            ARM_USBH_SignalPipeEvent_t cb_pipe_event)
  \brief       Initialize USB Host Interface.
  \param[in]   cb_port_event  Pointer to \ref ARM_USBH_SignalPortEvent
  \param[in]   cb_pipe_event  Pointer to \ref ARM_USBH_SignalPipeEvent
  \return      \ref execution_status
*/
static int32_t ARM_USBH_Initialize (ARM_USBH_SignalPortEvent_t cb_port_event, ARM_USBH_SignalPipeEvent_t cb_pipe_event) {
  int32_t tout;

  if (otg_hs_state & OTG_HS_USBH_DRIVER_INITIALIZED) return ARM_DRIVER_OK;
  if (otg_hs_state)                                  return ARM_DRIVER_ERROR;

  signal_port_event = cb_port_event;
  signal_pipe_event = cb_pipe_event;

  port_reset             = false;

  memset(endpoint_info, 0, sizeof(endpoint_info));
  memset(transfer_info, 0, sizeof(transfer_info));

  otg_hs_role     = ARM_USB_ROLE_HOST;
  OTG_HS_PinsConfigure (ARM_USB_PIN_DP | ARM_USB_PIN_DM | ARM_USB_PIN_OC | ARM_USB_PIN_VBUS);

  RCC->AHB1ENR   |=  RCC_AHB1ENR_OTGHSEN;           /* OTG HS clock enable    */
  RCC->AHB1RSTR  |=  RCC_AHB1ENR_OTGHSEN;           /* Reset OTG HS clock     */
  osDelay(1);                                       /* Wait 1 ms              */
  RCC->AHB1RSTR  &= ~RCC_AHB1ENR_OTGHSEN;

#ifdef MX_USB_OTG_HS_ULPI_D7_Pin
  /* External ULPI High-speed PHY */
  RCC->AHB1ENR   |=  RCC_AHB1ENR_OTGHSULPIEN;       /* OTG HS ULPI clock en   */
  OTG->GUSBCFG   &= ~OTG_HS_GUSBCFG_PHSEL;          /* High-spd trnscvr       */
  OTG->GUSBCFG   |=  OTG_HS_GUSBCFG_PTCI      |     /* Ind. pass through      */
                     OTG_HS_GUSBCFG_PCCI      |     /* Ind. complement        */
                     OTG_HS_GUSBCFG_ULPIEVBUSI|     /* ULPI ext Vbus ind      */
                     OTG_HS_GUSBCFG_ULPIEVBUSD;     /* ULPI ext Vbus drv      */
#else
  /* On-chip Full-speed PHY */
  OTG->GUSBCFG   |=  OTG_HS_GUSBCFG_PHSEL  |        /* Full-speed transceiver */
                     OTG_HS_GUSBCFG_PHYLPCS;        /* 48 MHz external clock  */
  OTG->GCCFG     &= ~OTG_HS_GCCFG_VBUSBSEN;         /* Disable VBUS sens of B */
  OTG->GCCFG     &= ~OTG_HS_GCCFG_VBUSASEN;         /* Disable VBUS sens of A */
  OTG->GCCFG     |=  OTG_HS_GCCFG_NOVBUSSENS;       /* No VBUS sensing        */
#endif

  OTG->GAHBCFG   &= ~OTG_HS_GAHBCFG_GINT;           /* Disable interrupts     */

  /* Wait until AHB Master state machine is in the idle condition             */
  for (tout = 1000; tout >= 0; tout--) {            /* Wait max 1 second      */
    if (OTG->GRSTCTL & OTG_HS_GRSTCTL_AHBIDL) break;
    if (!tout) return ARM_DRIVER_ERROR;
    osDelay (1);
  }
  OTG->GRSTCTL |=  OTG_HS_GRSTCTL_CSRST;            /* Core soft reset        */
  for (tout = 1000; tout >= 0; tout--) {            /* Wait max 1 second      */
    if (!(OTG->GRSTCTL & OTG_HS_GRSTCTL_CSRST)) break;
    if (!tout) return ARM_DRIVER_ERROR;
    osDelay (1);
  }
  osDelay (20);

  if (!(OTG->GUSBCFG & OTG_HS_GUSBCFG_FHMOD)) {
    OTG->GUSBCFG |=  OTG_HS_GUSBCFG_FHMOD;          /* Force host mode        */
    osDelay (50);
  }

  /* Core initialization                                                      */
  /* Rx FIFO setting */
  OTG->GRXFSIZ   = (RX_FIFO_SIZE/4);
  /* Non-periodic Tx FIFO setting */
  OTG->GNPTXFSIZ = ((TX_FIFO_SIZE_NON_PERI/4)<<16) |  (RX_FIFO_SIZE/4);
  /* Periodic Tx FIFO setting */
  OTG->HPTXFSIZ  = ((TX_FIFO_SIZE_PERI    /4)<<16) | ((RX_FIFO_SIZE+TX_FIFO_SIZE_NON_PERI)/4);

  OTG->GINTMSK  |=  OTG_HS_GINTMSK_DISCINT|         /* En disconn int         */
                    OTG_HS_GINTMSK_HCIM   |         /* En host ch int         */
                    OTG_HS_GINTMSK_PRTIM  |         /* En host prt int        */
                    OTG_HS_GINTMSK_RXFLVLM|         /* Enable RXFIFO int      */
                    OTG_HS_GINTMSK_SOFM   ;         /* Enable SOF int         */
  OTG->HAINTMSK  =  0x0000FFFF;                     /* En all ch ints         */
  OTG->GINTSTS   =  0xFFFFFFFF;                     /* Clear interrupts       */

  NVIC_SetPriority (OTG_HS_IRQn, 0);                /* OTG int highest prio   */

  otg_hs_state |= OTG_HS_USBH_DRIVER_INITIALIZED;

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_Uninitialize (void)
  \brief       De-initialize USB Host Interface.
  \return      \ref execution_status
*/
static int32_t ARM_USBH_Uninitialize (void) {

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_INITIALIZED)) return ARM_DRIVER_OK;
  if (  otg_hs_state & OTG_HS_USBH_DRIVER_POWERED     ) return ARM_DRIVER_ERROR;

  RCC->AHB1RSTR  |=  RCC_AHB1ENR_OTGHSEN;           /* Reset OTG HS clock     */
  osDelay(1);                                       /* Wait 1 ms              */
  RCC->AHB1RSTR  &= ~RCC_AHB1ENR_OTGHSEN;
  RCC->AHB1ENR   &= ~RCC_AHB1ENR_OTGHSEN;           /* OTG HS clock disable   */

  OTG_HS_PinsUnconfigure (ARM_USB_PIN_DP | ARM_USB_PIN_DM | ARM_USB_PIN_OC | ARM_USB_PIN_VBUS);
  otg_hs_role     =  ARM_USB_ROLE_NONE;
#ifdef MX_USB_OTG_HS_ULPI_D7_Pin
  /* External ULPI High-speed PHY */
  RCC->AHB1ENR  &= ~RCC_AHB1ENR_OTGHSULPIEN;        /* OTG HS ULPI clock dis  */
#endif

  otg_hs_state &= ~OTG_HS_USBH_DRIVER_INITIALIZED;

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PowerControl (ARM_POWER_STATE state)
  \brief       Control USB Host Interface Power.
  \param[in]   state  Power state
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PowerControl (ARM_POWER_STATE state) {

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_INITIALIZED)) return ARM_DRIVER_ERROR;

  switch (state) {
    case ARM_POWER_OFF:
      if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_OK;
      otg_hs_state  &= ~OTG_HS_USBH_DRIVER_POWERED;
      NVIC_DisableIRQ   (OTG_HS_IRQn);              /* Disable OTG interrupt  */
      OTG->GAHBCFG  &= ~OTG_HS_GAHBCFG_GINT;        /* Disable interrupts     */
      OTG->GCCFG    &= ~OTG_HS_GCCFG_PWRDWN;        /* Enable power down      */
      break;

    case ARM_POWER_LOW:
      return ARM_DRIVER_ERROR_UNSUPPORTED;

    case ARM_POWER_FULL:
      if (  otg_hs_state & OTG_HS_USBH_DRIVER_POWERED ) return ARM_DRIVER_OK;
      OTG->GCCFG    |=  OTG_HS_GCCFG_PWRDWN;        /* Disable power down     */
      otg_hs_state  |=  OTG_HS_USBH_DRIVER_POWERED;
      OTG->GINTSTS   =  0xFFFFFFFF;                 /* Clear interrupts       */
      NVIC_EnableIRQ   (OTG_HS_IRQn);               /* Enable OTG interrupt   */
      OTG->GAHBCFG  |=  OTG_HS_GAHBCFG_GINT;        /* Enable interrupts      */
      break;

    default:
      return ARM_DRIVER_ERROR;
  }

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PortVbusOnOff (uint8_t port, bool vbus)
  \brief       Root HUB Port VBUS on/off.
  \param[in]   port  Root HUB Port Number
  \param[in]   vbus
                - \b false VBUS off
                - \b true  VBUS on
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PortVbusOnOff (uint8_t port, bool vbus) {

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (port)                                         return ARM_DRIVER_ERROR_PARAMETER;

  if (vbus) {                                       /* VBUS power on          */
    OTG->GAHBCFG &= ~OTG_HS_GAHBCFG_GINT;           /* Disable interrupts     */
    OTG->HPRT    |=  OTG_HS_HPRT_PPWR;              /* Port power on          */
    OTG_HS_PinVbusOnOff (true);                     /* VBUS pin on            */
    OTG->GINTSTS  =  0xFFFFFFFF;                    /* Clear interrupts       */
    OTG->GAHBCFG |=  OTG_HS_GAHBCFG_GINT;           /* Enable interrupts      */
  } else {                                          /* VBUS power off         */
    OTG_HS_PinVbusOnOff (false);                    /* VBUS pin off           */
    OTG->HPRT    &= ~OTG_HS_HPRT_PPWR;              /* Port power off         */
  }

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PortReset (uint8_t port)
  \brief       Do Root HUB Port Reset.
  \param[in]   port  Root HUB Port Number
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PortReset (uint8_t port) {
  uint32_t hprt;
  uint32_t hcfg;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (port)                                         return ARM_DRIVER_ERROR_PARAMETER;
  if (!(OTG->HPRT & OTG_HS_HPRT_PCSTS))             return ARM_DRIVER_ERROR;

  hcfg = OTG->HCFG;
  hprt = OTG->HPRT;
  switch ((hprt >> 17) & 3) {
    case 0:                             /* High-speed detected                */
    case 1:                             /* Full-speed detected                */
      if (OTG->HFIR != 48000) OTG->HFIR = 48000;
      if ((hcfg & 3) != 1) {
        OTG->HCFG = (hcfg & ~OTG_HS_HCFG_FSLSPCS(3)) | OTG_HS_HCFG_FSLSPCS(1);
      }
      break;
    case 2:                             /* Low-speed detected                 */
      if (OTG->HFIR != 6000) OTG->HFIR = 6000;
      if ((hcfg & 3) != 2) {
        OTG->HCFG = (hcfg & ~OTG_HS_HCFG_FSLSPCS(3)) | OTG_HS_HCFG_FSLSPCS(2);
      }
      break;
    case 3:
      break;
  }

  if (!(OTG->HPRT & OTG_HS_HPRT_PCSTS)) return ARM_DRIVER_ERROR;

  port_reset = true;
  hprt  =  OTG->HPRT;
  hprt &= ~OTG_HS_HPRT_PENA;            /* Disable port                       */
  hprt |=  OTG_HS_HPRT_PRST;            /* Port reset                         */
  OTG->HPRT = hprt;
  osDelay (50);
  hprt &= ~OTG_HS_HPRT_PRST;            /* Clear port reset                   */
  OTG->HPRT = hprt;
  osDelay (50);
  if (port_reset) {
    port_reset = false;
    return ARM_DRIVER_ERROR;
  }

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PortSuspend (uint8_t port)
  \brief       Suspend Root HUB Port (stop generating SOFs).
  \param[in]   port  Root HUB Port Number
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PortSuspend (uint8_t port) {

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (port)                                         return ARM_DRIVER_ERROR_PARAMETER;

  OTG->HPRT |=  OTG_HS_HPRT_PSUSP;      /* Port suspend                       */

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PortResume (uint8_t port)
  \brief       Resume Root HUB Port (start generating SOFs).
  \param[in]   port  Root HUB Port Number
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PortResume (uint8_t port) {

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (port)                                         return ARM_DRIVER_ERROR;

  OTG->HPRT |=  OTG_HS_HPRT_PRES;       /* Port resume                        */

  return ARM_DRIVER_OK;
}

/**
  \fn          ARM_USBH_PORT_STATE ARM_USBH_PortGetState (uint8_t port)
  \brief       Get current Root HUB Port State.
  \param[in]   port  Root HUB Port Number
  \return      Port State \ref ARM_USBH_PORT_STATE
*/
static ARM_USBH_PORT_STATE ARM_USBH_PortGetState (uint8_t port) {
  ARM_USBH_PORT_STATE port_state = { 0 };
  uint32_t hprt;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return port_state;

  if (port) return port_state;

  hprt = OTG->HPRT;

  port_state.connected = (hprt & OTG_HS_HPRT_PCSTS) != 0;
  switch ((hprt & OTG_HS_HPRT_PSPD_MSK) >> OTG_HS_HPRT_PSPD_POS) {
    case 0:                             /* High speed                         */
     port_state.speed = ARM_USB_SPEED_HIGH;
     break;
    case 1:                             /* Full speed                         */
     port_state.speed = ARM_USB_SPEED_FULL;
     break;
    case 2:                             /* Low speed                          */
     port_state.speed = ARM_USB_SPEED_LOW;
     break;
    default:
     break;
  }

  return port_state;
}

/**
  \fn          ARM_USBH_PIPE_HANDLE ARM_USBH_PipeCreate (uint8_t  dev_addr,
                                                         uint8_t  dev_speed,
                                                         uint8_t  hub_addr,
                                                         uint8_t  hub_port,
                                                         uint8_t  ep_addr,
                                                         uint8_t  ep_type,
                                                         uint16_t ep_max_packet_size,
                                                         uint8_t  ep_interval)
  \brief       Create Pipe in System.
  \param[in]   dev_addr   Device Address
  \param[in]   dev_speed  Device Speed
  \param[in]   hub_addr   Hub Address
  \param[in]   hub_port   Hub Port
  \param[in]   ep_addr    Endpoint Address
                - ep_addr.0..3: Address
                - ep_addr.7:    Direction
  \param[in]   ep_type    Endpoint Type (ARM_USB_ENDPOINT_xxx)
  \param[in]   ep_max_packet_size Endpoint Maximum Packet Size
  \param[in]   ep_interval        Endpoint Polling Interval
  \return      Pipe Handle \ref ARM_USBH_PIPE_HANDLE
*/
static ARM_USBH_PIPE_HANDLE ARM_USBH_PipeCreate (uint8_t dev_addr, uint8_t dev_speed, uint8_t hub_addr, uint8_t hub_port, uint8_t ep_addr, uint8_t ep_type, uint16_t ep_max_packet_size, uint8_t  ep_interval) {
  OTG_HS_HC *ptr_ch;
  uint32_t   ch_idx;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return NULL;

  ptr_ch = (OTG_HS_HC *)(ARM_USBH_CH_FindFree ());            /* Find free Ch */
  if (!ptr_ch) return NULL;                                   /* If no free   */

  ch_idx = ARM_USBH_CH_GetIndexFromAddress (ptr_ch);

  /* Fill in all fields of Endpoint Descriptor                                */
  ptr_ch->HCCHAR = OTG_HS_HCCHARx_MPSIZ   (ep_max_packet_size)             |
                   OTG_HS_HCCHARx_EPNUM   (ep_addr)                        |
                   OTG_HS_HCCHARx_EPDIR * (!((ep_addr >> 7) & 0x0001))     |
                   OTG_HS_HCCHARx_LSDEV * (dev_speed == ARM_USB_SPEED_LOW) |
                   OTG_HS_HCCHARx_EPTYP   (ep_type)                        |
                   OTG_HS_HCCHARx_DAD     (dev_addr);

  endpoint_info[ch_idx].speed           = dev_speed;
  endpoint_info[ch_idx].max_packet_size = ep_max_packet_size;
  endpoint_info[ch_idx].type            = ep_type;
  switch (ep_type) {
    case ARM_USB_ENDPOINT_CONTROL:
    case ARM_USB_ENDPOINT_BULK:
      break;
    case ARM_USB_ENDPOINT_ISOCHRONOUS:
    case ARM_USB_ENDPOINT_INTERRUPT:
      if (dev_speed == ARM_USB_SPEED_HIGH) {
        if ((ep_interval > 0) && (ep_interval <= 16)) {
          endpoint_info[ch_idx].interval_reload = 1 << (ep_interval - 1);
        }
      } else if ((dev_speed == ARM_USB_SPEED_FULL) || (dev_speed == ARM_USB_SPEED_LOW)) {
        if (ep_interval > 0) {
          endpoint_info[ch_idx].interval_reload = ep_interval;
        }
      }
      ptr_ch->HCCHAR |= OTG_HS_HCCHARx_MC((((ep_max_packet_size >> 11) + 1) & 3));
      break;
  }

  return ((ARM_USBH_EP_HANDLE)ptr_ch);
}

/**
  \fn          int32_t ARM_USBH_PipeModify (ARM_USBH_PIPE_HANDLE pipe_hndl,
                                            uint8_t              dev_addr,
                                            uint8_t              dev_speed,
                                            uint8_t              hub_addr,
                                            uint8_t              hub_port,
                                            uint16_t             ep_max_packet_size)
  \brief       Modify Pipe in System.
  \param[in]   pipe_hndl  Pipe Handle
  \param[in]   dev_addr   Device Address
  \param[in]   dev_speed  Device Speed
  \param[in]   hub_addr   Hub Address
  \param[in]   hub_port   Hub Port
  \param[in]   ep_max_packet_size Endpoint Maximum Packet Size
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PipeModify (ARM_USBH_PIPE_HANDLE pipe_hndl, uint8_t dev_addr, uint8_t dev_speed, uint8_t hub_addr, uint8_t hub_port, uint16_t ep_max_packet_size) {
  OTG_HS_HC *ptr_ch;
  uint32_t   ch_idx;
  uint32_t   hcchar;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (!pipe_hndl)                                   return ARM_DRIVER_ERROR_PARAMETER;

  ptr_ch = (OTG_HS_HC *)(pipe_hndl);
  ch_idx = ARM_USBH_CH_GetIndexFromAddress (ptr_ch);

  if (ARM_USBH_PipeTransferAbort (pipe_hndl) != ARM_DRIVER_OK) return ARM_DRIVER_ERROR;

  /* Fill in all fields of Endpoint Descriptor                                */
  hcchar  = ptr_ch->HCCHAR;
  hcchar &= (~OTG_HS_HCCHARx_MPSIZ_MSK) &   /* Clear maximum packet size field*/
            (~OTG_HS_HCCHARx_LSDEV    ) &   /* Clear device speed bit         */
            (~OTG_HS_HCCHARx_DAD_MSK  ) ;   /* Clear device address field     */
  hcchar |=   OTG_HS_HCCHARx_MPSIZ   (ep_max_packet_size)              |
            ( OTG_HS_HCCHARx_LSDEV * (dev_speed == ARM_USB_SPEED_LOW)) |
            ( OTG_HS_HCCHARx_DAD     (dev_addr))                       ;
  ptr_ch->HCCHAR = hcchar;              /* Update modified fields             */

  endpoint_info[ch_idx].speed           = dev_speed;
  endpoint_info[ch_idx].max_packet_size = ep_max_packet_size;

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PipeDelete (ARM_USBH_PIPE_HANDLE pipe_hndl)
  \brief       Delete Pipe from System.
  \param[in]   pipe_hndl  Pipe Handle
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PipeDelete (ARM_USBH_PIPE_HANDLE pipe_hndl) {
  OTG_HS_HC *ptr_ch;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (!pipe_hndl)                                   return ARM_DRIVER_ERROR_PARAMETER;

  if (ARM_USBH_PipeTransferAbort (pipe_hndl) != ARM_DRIVER_OK) return ARM_DRIVER_ERROR;

  ptr_ch           = (OTG_HS_HC *)(pipe_hndl);
  ptr_ch->HCCHAR   = 0;
  ptr_ch->HCINT    = 0;
  ptr_ch->HCINTMSK = 0;
  ptr_ch->HCTSIZ   = 0;

  memset(&endpoint_info[ARM_USBH_CH_GetIndexFromAddress (ptr_ch)], 0, sizeof(endpoint_info));

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PipeReset (ARM_USBH_PIPE_HANDLE pipe_hndl)
  \brief       Reset Pipe.
  \param[in]   pipe_hndl  Pipe Handle
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PipeReset (ARM_USBH_PIPE_HANDLE pipe_hndl) {
  OTG_HS_HC *ptr_ch;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (!pipe_hndl)                                   return ARM_DRIVER_ERROR_PARAMETER;

  if (ARM_USBH_PipeTransferAbort (pipe_hndl) != ARM_DRIVER_OK) return ARM_DRIVER_ERROR;

  ptr_ch           = (OTG_HS_HC *)(pipe_hndl);
  ptr_ch->HCINT    = 0;
  ptr_ch->HCINTMSK = 0;
  ptr_ch->HCTSIZ   = 0;

  return ARM_DRIVER_OK;
}

/**
  \fn          int32_t ARM_USBH_PipeTransfer (ARM_USBH_PIPE_HANDLE pipe_hndl,
                                              uint32_t             packet,
                                              uint8_t             *data,
                                              uint32_t             num)
  \brief       Transfer packets through USB Pipe.
  \param[in]   pipe_hndl  Pipe Handle
  \param[in]   packet     Packet information
  \param[in]   data       Pointer to buffer with data to send or for data to receive
  \param[in]   num        Number of data bytes to transfer
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PipeTransfer (ARM_USBH_PIPE_HANDLE pipe_hndl, uint32_t packet, uint8_t *data, uint32_t num) {
  uint32_t max_num_to_transfer;
  uint32_t ch_idx;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (!pipe_hndl)                                   return ARM_DRIVER_ERROR_PARAMETER;
  if (!(OTG->HPRT & OTG_HS_HPRT_PCSTS))             return ARM_DRIVER_ERROR;

  ch_idx = ARM_USBH_CH_GetIndexFromAddress ((OTG_HS_HC *)(pipe_hndl));

  if (transfer_info[ch_idx].status.active)          return ARM_DRIVER_ERROR_BUSY;

  memset(&transfer_info[ch_idx], 0, sizeof(transfer_info_t));

  max_num_to_transfer = num;
  if (((packet & ARM_USBH_PACKET_TOKEN_Msk) == ARM_USBH_PACKET_SETUP) ||
      ((packet & ARM_USBH_PACKET_TOKEN_Msk) == ARM_USBH_PACKET_OUT) )  {
    switch (endpoint_info[ch_idx].type) {
      case ARM_USB_ENDPOINT_CONTROL:
      case ARM_USB_ENDPOINT_BULK:
        if (max_num_to_transfer > TX_FIFO_SIZE_NON_PERI)
          max_num_to_transfer = TX_FIFO_SIZE_NON_PERI;
        break;
      case ARM_USB_ENDPOINT_ISOCHRONOUS:
      case ARM_USB_ENDPOINT_INTERRUPT:
        if (max_num_to_transfer > TX_FIFO_SIZE_PERI)
          max_num_to_transfer = TX_FIFO_SIZE_PERI;
        break;
    }
  }

  transfer_info[ch_idx].packet             = packet;
  transfer_info[ch_idx].data               = data;
  transfer_info[ch_idx].num                = num;
  transfer_info[ch_idx].num_transferring   = max_num_to_transfer;
  transfer_info[ch_idx].num_transferred    = 0;
  transfer_info[ch_idx].interval           = endpoint_info[ch_idx].interval_reload;
  transfer_info[ch_idx].event              = 0;
  transfer_info[ch_idx].status.in_progress = 1;
  transfer_info[ch_idx].status.active      = 1;

  ARM_USBH_CH_TransferEnqueue ((OTG_HS_HC *)pipe_hndl, packet, data, max_num_to_transfer);

  return ARM_DRIVER_OK;
}

/**
  \fn          uint32_t ARM_USBH_PipeTransferGetResult (ARM_USBH_PIPE_HANDLE pipe_hndl)
  \brief       Get result of USB Pipe transfer.
  \param[in]   pipe_hndl  Pipe Handle
  \return      number of successfully transfered data bytes
*/
static uint32_t ARM_USBH_PipeTransferGetResult (ARM_USBH_PIPE_HANDLE pipe_hndl) {

  if (!pipe_hndl) return 0;

  return (transfer_info[ARM_USBH_CH_GetIndexFromAddress((OTG_HS_HC *)pipe_hndl)].num_transferred);
}

/**
  \fn          int32_t ARM_USBH_PipeTransferAbort (ARM_USBH_PIPE_HANDLE pipe_hndl)
  \brief       Abort current USB Pipe transfer.
  \param[in]   pipe_hndl  Pipe Handle
  \return      \ref execution_status
*/
static int32_t ARM_USBH_PipeTransferAbort (ARM_USBH_PIPE_HANDLE pipe_hndl) {
  uint32_t ch_idx;

  if (!(otg_hs_state & OTG_HS_USBH_DRIVER_POWERED)) return ARM_DRIVER_ERROR;

  if (!pipe_hndl)                                   return ARM_DRIVER_ERROR_PARAMETER;

  ch_idx = ARM_USBH_CH_GetIndexFromAddress ((OTG_HS_HC *)pipe_hndl);

  if (transfer_info[ch_idx].status.active) {
    transfer_info[ch_idx].status.active = 0;
    if (!ARM_USBH_CH_Disable((OTG_HS_HC *)(pipe_hndl))) return ARM_DRIVER_ERROR;
  }

  return ARM_DRIVER_OK;
}

/**
  \fn          uint16_t ARM_USBH_GetFrameNumber (void)
  \brief       Get current USB Frame Number.
  \return      Frame Number
*/
static uint16_t ARM_USBH_GetFrameNumber (void) {

  return ((OTG->HFNUM >> 3) & 0x7FF);
}

/**
  \fn          void USBH_HS_IRQ (uint32_t gintsts)
  \brief       USB Host Interrupt Routine (IRQ).
*/
void USBH_HS_IRQ (uint32_t gintsts) {
  OTG_HS_HC         *ptr_ch;
  uint8_t           *ptr_data;
  volatile uint32_t *dfifo;
  uint32_t           hprt, haint, hcint, pktcnt, xfrsiz, mpsiz, hcchar, hcchar_upd, transferred;
  uint32_t           grxsts, bcnt, ch, dat, len, len_rest;
  uint32_t           max_num_to_transfer;
  uint8_t            signal;

  if (gintsts & OTG_HS_GINTSTS_HPRTINT) {         /* If host port interrupt   */
    hprt = OTG->HPRT;
    OTG->HPRT = hprt & (~OTG_HS_HPRT_PENA);       /* Leave PENA bit           */
    if (hprt  & OTG_HS_HPRT_PCDET) {              /* Port connect detected    */
      if (!port_reset) {                          /* If port not under reset  */
        signal_port_event(0, ARM_USBH_EVENT_CONNECT);
      }
    }
    if (hprt & OTG_HS_HPRT_PENCHNG) {             /* If port enable changed   */
      if (hprt & OTG_HS_HPRT_PENA) {              /* If device connected      */
        if (port_reset) {
          port_reset = false;
          signal_port_event(0, ARM_USBH_EVENT_RESET);
        }
      }
    }
  }
  if (gintsts & OTG_HS_GINTSTS_DISCINT) {         /* If device disconnected   */
    OTG->GINTSTS = OTG_HS_GINTSTS_DISCINT;        /* Clear disconnect int     */
    if (!port_reset) {                            /* Ignore discon under reset*/
      for (ch = 0; ch < OTG_MAX_CH; ch++) {
        if (transfer_info[ch].status.active) {
          transfer_info[ch].status.active = 0;
        }
      }
      signal_port_event(0, ARM_USBH_EVENT_DISCONNECT);
    }
  }
                                                  /* Handle reception int     */
  if (gintsts & OTG_HS_GINTSTS_RXFLVL) {          /* If RXFIFO non-empty int  */
    OTG->GINTMSK &= ~OTG_HS_GINTMSK_RXFLVLM;
    grxsts = OTG->GRXSTSR;
    if (((grxsts >> 17) & 0x0F) == 0x02){         /* If PKTSTS = 0x02         */
      grxsts     = (OTG->GRXSTSP);
      ch         = (grxsts >> 0) & 0x00F;
      bcnt       = (grxsts >> 4) & 0x7FF;
      dfifo      = OTG_DFIFO[ch];
      ptr_data   = transfer_info[ch].data + transfer_info[ch].num_transferred;
      len        = bcnt / 4;                      /* Received number of 32-bit*/
      len_rest   = bcnt & 3;                      /* Number of bytes left     */
      while (len--) {
        *((__packed uint32_t *)ptr_data) = *dfifo;
        ptr_data += 4;
      }
      if (len_rest) {
        dat = *((__packed uint32_t *)dfifo);
        while (len_rest--) {
          *ptr_data++ = dat;
          dat >>= 8;
        }
      }
      transfer_info[ch].num_transferred += bcnt;
    } else {                                      /* If PKTSTS != 0x02        */
      grxsts     = OTG->GRXSTSP;
    }
    OTG->GINTMSK |= OTG_HS_GINTMSK_RXFLVLM;
  }
                                                  /* Handle host ctrl int     */
  if (gintsts & OTG_HS_GINTSTS_HCINT) {           /* If host channel interrupt*/
    haint = OTG->HAINT;
    for (ch = 0; ch < OTG_MAX_CH; ch++) {
      if (!haint) break;
      if (haint & (1 << ch)) {                    /* If channels interrupt act*/
        haint     &= ~(1 << ch);
        signal     =   0;
        ptr_ch     =  (OTG_HS_HC *)(&OTG->HCCHAR0) + ch;
        hcint      =   ptr_ch->HCINT & ptr_ch->HCINTMSK;
        hcchar     =   ptr_ch->HCCHAR;
        hcchar_upd =   0;
        if (hcint & OTG_HS_HCINTx_NYET) {         /* If NYET received         */
          pktcnt = (ptr_ch->HCTSIZ >> 19) & 0x3FF;
          xfrsiz = (ptr_ch->HCTSIZ >>  0) & 0x7FFFF;
          mpsiz  = (ptr_ch->HCCHAR >>  0) & 0x7FF;
          if (xfrsiz >= mpsiz)
            transferred = (((transfer_info[ch].num_transferring + mpsiz - 1) / mpsiz) - pktcnt) * mpsiz;
          else
            transferred = xfrsiz;
          if (transfer_info[ch].num_transferring != transferred) {
            // Transfer is not done, else XFRC follows
            transfer_info[ch].num_transferred += transferred;
            transfer_info[ch].packet |= ARM_USBH_PACKET_PING;     /* Do Ping  */
            goto halt_ch;
          }
        }
        if (hcint & OTG_HS_HCINTx_XFRC) {         /* If data transfer finished*/
          if (ptr_ch->HCCHAR & (1 << 15)) {                 /* If endpoint IN */
            if (!transfer_info[ch].num_transferred)
              transfer_info[ch].num_transferred = transfer_info[ch].num;
            transfer_info[ch].event = ARM_USBH_EVENT_TRANSFER_COMPLETE;
          } else {                                          /* If endpoint OUT*/
            pktcnt = (ptr_ch->HCTSIZ >> 19) & 0x3FF;
            xfrsiz = (ptr_ch->HCTSIZ >>  0) & 0x7FFFF;
            mpsiz  = (ptr_ch->HCCHAR >>  0) & 0x7FF;
            if (xfrsiz >= mpsiz)
              transferred = (((transfer_info[ch].num_transferring + mpsiz - 1) / mpsiz) - pktcnt) * mpsiz;
            else
              transferred = xfrsiz;
            transfer_info[ch].num_transferred += transferred;
            if (transfer_info[ch].num_transferred == transfer_info[ch].num)
              transfer_info[ch].event = ARM_USBH_EVENT_TRANSFER_COMPLETE;
          }
          goto halt_ch;
        } else if (hcint & OTG_HS_HCINTx_STALL) { /* If STALL event           */
          transfer_info[ch].event = ARM_USBH_EVENT_HANDSHAKE_STALL;
        } else if ((hcint & OTG_HS_HCINTx_NAK)   ||         /* If NAK received*/
                   (hcint & OTG_HS_HCINTx_TXERR) ||         /* If TXERR rece  */
                   (hcint & OTG_HS_HCINTx_BBERR) ||         /* If BBERR rece  */
                   (hcint & OTG_HS_HCINTx_DTERR)) {         /* If DTERR rece  */
                                                  /* Update transfer info     */
          if (hcint & OTG_HS_HCINTx_NAK) {
            /* On NAK, NAK is not returned to middle layer but transfer is
               restarted from driver for remaining data                       */
            if (ptr_ch->HCCHAR & (1 << 15)) {               /* If endpoint IN */
              if (endpoint_info[ch].type == ARM_USB_ENDPOINT_INTERRUPT) {
                goto halt_ch;
              } else {
                hcchar_upd = hcchar | OTG_HS_HCCHARx_CHENA;
              }
            } else {                                        /* If endpoint OUT*/
              if (((OTG->HPRT & OTG_HS_HPRT_PSPD_MSK) >> OTG_HS_HPRT_PSPD_POS) == 2)  /* If HS */
                transfer_info[ch].packet |= ARM_USBH_PACKET_PING;   /* Do Ping  */
              pktcnt = (ptr_ch->HCTSIZ >> 19) & 0x3FF;
              xfrsiz = (ptr_ch->HCTSIZ >>  0) & 0x7FFFF;
              mpsiz  = (ptr_ch->HCCHAR >>  0) & 0x7FF;
              if (xfrsiz >= mpsiz)
                transferred = (((transfer_info[ch].num_transferring + mpsiz - 1) / mpsiz) - pktcnt) * mpsiz;
              else
                transferred = 0;
              transfer_info[ch].num_transferred += transferred;
              goto halt_ch;
            }
          } else {
            transfer_info[ch].event = ARM_USBH_EVENT_BUS_ERROR;
            goto halt_ch;
          }
        } else if (hcint & OTG_HS_HCINTx_CHH) {   /* If channel halted        */
                                                  /* Transfer is done here    */
          ptr_ch->HCINTMSK = 0;                   /* Mask all interrupts      */
          hcint = 0x7BB;                          /* Clear all interrupts     */
          transfer_info[ch].status.in_progress = 0;
          if (transfer_info[ch].event) {
            transfer_info[ch].status.active = 0;
            signal = 1;
          }
        } else if (hcint & OTG_HS_HCINTx_ACK) {             /* If ACK received*/
          /* On ACK, ACK is not an event that can be returned so when channel
             is halted it will be signaled to middle layer if transfer is
             completed otherwise transfer will be restarted for remaining
             data                                                             */
          if (ptr_ch->HCCHAR & (1 << 15)) {                 /* If endpoint IN */
            if ((transfer_info[ch].num != transfer_info[ch].num_transferred) &&   /* If all data was not transferred  */
                (transfer_info[ch].num_transferred != 0)                     &&   /* If zero-length packet was not received */
               ((transfer_info[ch].num_transferred % endpoint_info[ch].max_packet_size) == 0)) {  /* If short packet was not received */
              hcchar_upd = hcchar | OTG_HS_HCCHARx_CHENA;
            }
          } else {
            if (transfer_info[ch].packet & ARM_USBH_PACKET_PING) {
              transfer_info[ch].packet &= ~ARM_USBH_PACKET_PING;
              goto halt_ch;
            }
          }
        } else {
halt_ch:                                          /* Halt the channel         */
          ptr_ch->HCINTMSK = OTG_HS_HCINTx_CHH;
          hcchar_upd = hcchar | OTG_HS_HCCHARx_CHENA | OTG_HS_HCCHARx_CHDIS;
        }
        ptr_ch->HCINT = hcint;
        if (signal)     signal_pipe_event((ARM_USBH_EP_HANDLE)ptr_ch, transfer_info[ch].event);
        if (hcchar_upd) ptr_ch->HCCHAR = hcchar_upd;
      }
      ptr_ch++;
    }
  }

  /* Handle periodic transfer timings                                         */
  if (gintsts & OTG_HS_GINTSTS_SOF) {             /* If start of frame int    */
    OTG->GINTSTS = OTG_HS_GINTSTS_SOF;            /* Clear SOF interrupt      */
    for (ch = 0; ch < OTG_MAX_CH; ch++) {
      /* If interrupt transfer is active handle period (interval)             */
      if ((endpoint_info[ch].type == ARM_USB_ENDPOINT_INTERRUPT) && (transfer_info[ch].status.active && transfer_info[ch].interval))
        --transfer_info[ch].interval;
    }
  }

  /* Handle restarts of unfinished transfers (due to NAK or ACK)              */
  for (ch = 0; ch < OTG_MAX_CH; ch++) {
    if (transfer_info[ch].status.active && !transfer_info[ch].status.in_progress) {
      /* Restart periodic transfer if not in progress and interval expired    */
      if (endpoint_info[ch].type == ARM_USB_ENDPOINT_INTERRUPT) {
        if (!transfer_info[ch].interval) {
          transfer_info[ch].interval = endpoint_info[ch].interval_reload;
        } else {
          continue;
        }
      }

      /* Restart transfer (periodic or non-periodic)                          */
      if (((transfer_info[ch].packet & ARM_USBH_PACKET_TOKEN_Msk) == ARM_USBH_PACKET_SETUP) ||
          ((transfer_info[ch].packet & ARM_USBH_PACKET_TOKEN_Msk) == ARM_USBH_PACKET_OUT) )  {
        /* SETUP or OUT transfer */
        max_num_to_transfer = transfer_info[ch].num - transfer_info[ch].num_transferred;
        switch (endpoint_info[ch].type) {
          case ARM_USB_ENDPOINT_CONTROL:
          case ARM_USB_ENDPOINT_BULK:
            if (max_num_to_transfer > TX_FIFO_SIZE_NON_PERI)
              max_num_to_transfer = TX_FIFO_SIZE_NON_PERI;
            break;
          case ARM_USB_ENDPOINT_ISOCHRONOUS:
          case ARM_USB_ENDPOINT_INTERRUPT:
            if (max_num_to_transfer > TX_FIFO_SIZE_PERI)
              max_num_to_transfer = TX_FIFO_SIZE_PERI;
            break;
        }
        transfer_info[ch].num_transferring   = max_num_to_transfer;
        transfer_info[ch].status.in_progress = 1;
        if (!ARM_USBH_CH_TransferEnqueue (ARM_USBH_CH_GetAddressFromIndex (ch), transfer_info[ch].packet, transfer_info[ch].data + transfer_info[ch].num_transferred, max_num_to_transfer)) {
          transfer_info[ch].status.active = 0;
        }
      } else {
        /* IN transfer */
        transfer_info[ch].status.in_progress = 1;
        if (!ARM_USBH_CH_TransferEnqueue (ARM_USBH_CH_GetAddressFromIndex (ch), transfer_info[ch].packet, transfer_info[ch].data + transfer_info[ch].num_transferred, transfer_info[ch].num - transfer_info[ch].num_transferred)) {
          transfer_info[ch].status.active = 0;
        }
      }
    }
  }
}

ARM_DRIVER_USBH Driver_USBH1 = {
  ARM_USBH_GetVersion,
  ARM_USBH_GetCapabilities,
  ARM_USBH_Initialize,
  ARM_USBH_Uninitialize,
  ARM_USBH_PowerControl,
  ARM_USBH_PortVbusOnOff,
  ARM_USBH_PortReset,
  ARM_USBH_PortSuspend,
  ARM_USBH_PortResume,
  ARM_USBH_PortGetState,
  ARM_USBH_PipeCreate,
  ARM_USBH_PipeModify,
  ARM_USBH_PipeDelete,
  ARM_USBH_PipeReset,
  ARM_USBH_PipeTransfer,
  ARM_USBH_PipeTransferGetResult,
  ARM_USBH_PipeTransferAbort,
  ARM_USBH_GetFrameNumber
};