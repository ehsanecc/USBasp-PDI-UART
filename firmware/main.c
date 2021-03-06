/*
 * USBasp - USB in-circuit programmer for Atmel AVR controllers
 *
 * Thomas Fischl <tfischl@gmx.de>
 *
 * License........: GNU GPL v2 (see Readme.txt)
 * Target.........: ATMega8 at 12 MHz
 * Creation Date..: 2005-02-20
 * Last change....: 2009-02-28
 *
 * PC2 SCK speed option.
 * GND  -> slow (8khz SCK),
 * open -> software set speed (default is 375kHz SCK)
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>

#include "usbasp.h"
#include "usbdrv/usbdrv.h"
#include "isp.h"
#include "clock.h"
#include "tpi.h"
#include "tpi_defs.h"
#include "uart.h"
#include "pdi.h"
#include <string.h>

#include <util/delay.h> //after clock.h

static uchar replyBuffer[8];

static uchar prog_state = PROG_STATE_IDLE;
static uchar prog_sck = USBASP_ISP_SCK_AUTO;

static uchar prog_address_newmode = 0;
static unsigned long prog_address;
static unsigned int prog_nbytes = 0;
static unsigned int prog_pagesize;
static uchar prog_blockflags;
static uchar prog_pagecounter;

static uchar prog_buf[128];
static uchar prog_buf_pos;

usbMsgLen_t usbFunctionSetup(uchar data[8]) {

	usbMsgLen_t len = 0;

	if (data[1] == USBASP_FUNC_CONNECT) {
		uart_disable(); // make it not interefere.

		/* set SCK speed */
		if ((PINC & (1 << PC2)) == 0) {
			ispSetSCKOption(USBASP_ISP_SCK_8);
		} else {
			ispSetSCKOption(prog_sck);
		}

		/* set compatibility mode of address delivering */
		prog_address_newmode = 0;

		ledRedOn();
		ispConnect();

	} else if (data[1] == USBASP_FUNC_DISCONNECT) {
		ispDisconnect();
		ledRedOff();

	} else if (data[1] == USBASP_FUNC_TRANSMIT) {
		replyBuffer[0] = ispTransmit(data[2]);
		replyBuffer[1] = ispTransmit(data[3]);
		replyBuffer[2] = ispTransmit(data[4]);
		replyBuffer[3] = ispTransmit(data[5]);
		len = 4;

	} else if (data[1] == USBASP_FUNC_READFLASH) {

		if (!prog_address_newmode)
			prog_address = (data[3] << 8) | data[2];

		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_READFLASH;
		len = USB_NO_MSG; /* multiple in */

	} else if (data[1] == USBASP_FUNC_READEEPROM) {

		if (!prog_address_newmode)
			prog_address = (data[3] << 8) | data[2];

		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_READEEPROM;
		len = USB_NO_MSG; /* multiple in */

	} else if (data[1] == USBASP_FUNC_ENABLEPROG) {
		replyBuffer[0] = ispEnterProgrammingMode();
		len = 1;

	} else if (data[1] == USBASP_FUNC_WRITEFLASH) {

		if (!prog_address_newmode)
			prog_address = (data[3] << 8) | data[2];

		prog_pagesize = data[4];
		prog_blockflags = data[5] & 0x0F;
		prog_pagesize += (((unsigned int) data[5] & 0xF0) << 4);
		if (prog_blockflags & PROG_BLOCKFLAG_FIRST) {
			prog_pagecounter = prog_pagesize;
		}
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_WRITEFLASH;
		len = USB_NO_MSG; /* multiple out */

	} else if (data[1] == USBASP_FUNC_WRITEEEPROM) {

		if (!prog_address_newmode)
			prog_address = (data[3] << 8) | data[2];

		prog_pagesize = 0;
		prog_blockflags = 0;
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_WRITEEEPROM;
		len = USB_NO_MSG; /* multiple out */

	} else if (data[1] == USBASP_FUNC_SETLONGADDRESS) {

		/* set new mode of address delivering (ignore address delivered in commands) */
		prog_address_newmode = 1;
		/* set new address */
		prog_address = *((unsigned long*) &data[2]);

	} else if (data[1] == USBASP_FUNC_SETISPSCK) {

		/* set sck option */
		prog_sck = data[2];
		replyBuffer[0] = 0;
		len = 1;

	} else if (data[1] == USBASP_FUNC_TPI_CONNECT) {
		uart_disable(); // make it not interefere.
		tpi_dly_cnt = data[2] | (data[3] << 8);

		/* RST high */
		ISP_OUT |= (1 << ISP_RST);
		ISP_DDR |= (1 << ISP_RST);

		clockWait(3);

		/* RST low */
		ISP_OUT &= ~(1 << ISP_RST);
		ledRedOn();

		clockWait(16);
		tpi_init();
	
	} else if (data[1] == USBASP_FUNC_TPI_DISCONNECT) {

		tpi_send_byte(TPI_OP_SSTCS(TPISR));
		tpi_send_byte(0);

		clockWait(10);

		/* pulse RST */
		ISP_OUT |= (1 << ISP_RST);
		clockWait(5);
		ISP_OUT &= ~(1 << ISP_RST);
		clockWait(5);

		/* set all ISP pins inputs */
		ISP_DDR &= ~((1 << ISP_RST) | (1 << ISP_SCK) | (1 << ISP_MOSI));
		/* switch pullups off */
		ISP_OUT &= ~((1 << ISP_RST) | (1 << ISP_SCK) | (1 << ISP_MOSI));

		ledRedOff();
	
	} else if (data[1] == USBASP_FUNC_TPI_RAWREAD) {
		replyBuffer[0] = tpi_recv_byte();
		len = 1;
	
	} else if (data[1] == USBASP_FUNC_TPI_RAWWRITE) {
		tpi_send_byte(data[2]);
	
	} else if (data[1] == USBASP_FUNC_TPI_READBLOCK) {
		prog_address = (data[3] << 8) | data[2];
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_TPI_READ;
		len = USB_NO_MSG; /* multiple in */
	
	} else if (data[1] == USBASP_FUNC_TPI_WRITEBLOCK) {
		prog_address = (data[3] << 8) | data[2];
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_TPI_WRITE;
		len = USB_NO_MSG; /* multiple out */
	
	}  else if (data[1] == USBASP_FUNC_PDI_CONNECT)
		{
		if ((replyBuffer[0]=pdiInit())==PDI_STATUS_OK)
			ledRedOn();
		len=1;
		}
	else if (data[1] == USBASP_FUNC_PDI_DISCONNECT)
		{
		ledRedOff();
		pdiCleanup(data[2]);
		}
	else if (data[1] == USBASP_FUNC_PDI_SEND)
		{
		prog_nbytes = (data[7] << 8) | data[6];
		prog_blockflags = data[2];
		prog_state = PROG_STATE_PDI_SEND;
		prog_buf_pos = 0;
		len = 0xff;
		}
	else if (data[1] == USBASP_FUNC_PDI_READ)
		{
		memmove(&prog_address,data+2,4);
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_PDI_READ;
		len = 0xff;
		}
	// UART from now on:
	else if(data[1]==USBASP_FUNC_UART_CONFIG){
		uart_config(/*baud*/(data[3]<<8)|data[2], /*par*/ data[4] & USBASP_UART_PARITY_MASK, 
			/*stop*/data[4] & USBASP_UART_STOP_MASK, /*bytes*/data[4] & USBASP_UART_BYTES_MASK);
	}
	else if(data[1]==USBASP_FUNC_UART_FLUSHTX) {
		uart_flush_tx();
	}
	else if(data[1]==USBASP_FUNC_UART_FLUSHRX) {
		uart_flush_rx();
	}
	else if(data[1]==USBASP_FUNC_UART_DISABLE) {
		uart_disable();
	}
	else if(data[1]==USBASP_FUNC_UART_TX){
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_UART_TX;
		len=USB_NO_MSG; // multiple out
	}
	else if(data[1]==USBASP_FUNC_UART_RX){
		prog_nbytes = (data[7] << 8) | data[6];
		prog_state = PROG_STATE_UART_RX;
		len=USB_NO_MSG; // multiple in
	}
	else if(data[1]==USBASP_FUNC_UART_TX_FREE){
		uint16_t places=uart_tx_freeplaces();
		replyBuffer[0]=places>>8;
		replyBuffer[1]=places&0xFF;
		len=2;
	}
	else if (data[1] == USBASP_FUNC_GETCAPABILITIES) {
		replyBuffer[0] = USBASP_CAP_0_TPI|USBASP_CAP_6_UART|USBASP_CAP_0_PDI;
		replyBuffer[1] = 0;
		replyBuffer[2] = 0;
		replyBuffer[3] = 0;
		len = 4;
	}

	usbMsgPtr = replyBuffer;

	return len;
}

uchar usbFunctionRead(uchar *data, uchar len) {

	uchar i;

	if (prog_state == PROG_STATE_TPI_READ) {
		/* fill packet TPI mode */
		tpi_read_block(prog_address, data, len);
		prog_address += len;
		return len;
	}
	else if (prog_state == PROG_STATE_UART_RX) {
		for(uint8_t rd=0; rd<len; rd++){
			if(!uart_getc(data+rd)) {
				len=rd; // Emptied whole buffer.
				break;
			}
			ledRedOn();
		}
		if(len<8){
			prog_state=PROG_STATE_IDLE;
		}
		ledRedOff();
		return len; // Whole data buffer written.
	}
	else if (prog_state == PROG_STATE_PDI_READ) {
		pdiDisableTimerClock();
		pdiSendIdle();
		if (pdi_nvmbusy)
			pdiWaitNVM();
		uchar ret=pdiReadBlock(prog_address, data, len);
		pdiEnableTimerClock();
		if (ret!=PDI_STATUS_OK)
			return 0;
		prog_address += len;
		return len;
	}
	else if (prog_state == PROG_STATE_READFLASH || prog_state == PROG_STATE_READEEPROM) {
		/* fill packet ISP mode */
		for (i = 0; i < len; i++) {
			if (prog_state == PROG_STATE_READFLASH) {
				data[i] = ispReadFlash(prog_address);
			} else {
				data[i] = ispReadEEPROM(prog_address);
			}
			prog_address++;
		}

		/* last packet? */
		if (len < 8) {
			prog_state = PROG_STATE_IDLE;
		}
	}
	else {
		return 0xFF;
	}

	return len;
}

uchar usbFunctionWrite(uchar *data, uchar len) {

	uchar retVal = 0;
	uchar i;

	if (prog_state == PROG_STATE_TPI_WRITE) {
		tpi_write_block(prog_address, data, len);
		prog_address += len;
		prog_nbytes -= len;
		if(prog_nbytes <= 0)
		{
			prog_state = PROG_STATE_IDLE;
			return 1;
		}
		return 0;
	}
	else if (prog_state == PROG_STATE_UART_TX) {
		if(len) {
			ledRedOn();
			uart_putsn(data, len);
			// This function should succeed, since computer should
			// request correct number of bytes. If request is bad,
			// return anything.
			ledRedOff();
		}

		prog_nbytes-=len;
		if(prog_nbytes<=0){
			prog_state=PROG_STATE_IDLE;
			return 1;
		}
		return 0;
	}
	else if (prog_state == PROG_STATE_PDI_SEND) {
		memmove(&prog_buf[prog_buf_pos],data,len);
		prog_buf_pos += len;
		prog_nbytes -= len;
		if (prog_nbytes==0)
			{
			pdiDisableTimerClock();
			pdiSendIdle();
			if ((prog_blockflags & USBASP_PDI_WAIT_BUSY) && pdi_nvmbusy)
				pdiWaitNVM();
			pdiSendBytes(prog_buf,prog_buf_pos);
			if (prog_blockflags & USBASP_PDI_MARK_BUSY)
				pdi_nvmbusy=1;
			pdiEnableTimerClock();
			prog_state = PROG_STATE_IDLE;
			return 1;
			}
		return 0;
	}
	else if (prog_state == PROG_STATE_WRITEFLASH || prog_state == PROG_STATE_WRITEEEPROM) {
		for (i = 0; i < len; i++) {
			if (prog_state == PROG_STATE_WRITEFLASH) {
				/* Flash */

				if (prog_pagesize == 0) {
					/* not paged */
					ispWriteFlash(prog_address, data[i], 1);
				} else {
					/* paged */
					ispWriteFlash(prog_address, data[i], 0);
					prog_pagecounter--;
					if (prog_pagecounter == 0) {
						ispFlushPage(prog_address, data[i]);
						prog_pagecounter = prog_pagesize;
					}
				}

			} else {
				/* EEPROM */
				ispWriteEEPROM(prog_address, data[i]);
			}

			prog_nbytes--;

			if (prog_nbytes == 0) {
				prog_state = PROG_STATE_IDLE;
				if ((prog_blockflags & PROG_BLOCKFLAG_LAST) && (prog_pagecounter
						!= prog_pagesize)) {

					/* last block and page flush pending, so flush it now */
					ispFlushPage(prog_address, data[i]);
				}

				retVal = 1; // Need to return 1 when no more data is to be received
			}

			prog_address++;
		}
	}
	else {
		return 0xFF;
	}

	return retVal;
}

int main(void) {
	uchar i, j;

	/* no pullups on USB and ISP pins */
	PORTD = 0;
	PORTB = 0;
	/* all outputs except PD2 = INT0 */
	DDRD = ~(1 << 2);

	PORTD|=(1<<0); // pullup on Rx pin.
	DDRD&=~(1<<0); // Rx as input too.

	/* output SE0 for USB reset */
	DDRB = ~0;
	j = 0;
	/* USB Reset by device only required on Watchdog Reset */
	while (--j) {
		i = 0;
		/* delay >10ms for USB reset */
		while (--i)
			;
	}
	/* all USB and ISP pins inputs */
	DDRB = 0;

	/* all inputs except PC0, PC1 */
	DDRC = 0x03;
	PORTC = 0xfe;

	/* init timer */
	clockInit();

	/* main event loop */
	usbInit();
	sei();
	for (;;) {
		usbPoll();
	}
	return 0;
}
