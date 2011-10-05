/* serial.c
 *
 * Serial routines for ATmega32
 * Modified to support the multiple UARTS of the Atmega2560
 *
 * Author: Austin Hendrix
*/

#include "serial.h"
#include <avr/io.h>
#include <avr/interrupt.h>


/* recieve circular fifo (10 bytes total 20% overhead) */
uint8_t rx_head[4]; /* points to next writeable byte */
volatile uint16_t rx_size[4]; /* number of byts in buffer */
uint8_t rx_buf[4][BUF_SZ];

/* send circular fifo (10 bytes total, 20% overhead) */
uint8_t tx_head[4]; /* next writeable byte */
volatile uint16_t tx_size[4]; /* number of bytes in buffer */

volatile uint8_t * ucsr[] = {&UCSR0A, &UCSR1A, &UCSR2A, &UCSR3A};
#define A 0
#define B 1
#define C 2

/* determine if there is data in the rx buffer */
uint8_t rx_ready(uint8_t port) {
   return rx_size[port] > 0;
}

/* get a byte from recieve buffer. block until data recieved */
uint8_t rx_byte(uint8_t port) {
   while(!rx_size[port]);

   //cli();
   ucsr[port][B] &= ~(1 << 7); /* disable receive interrupt */

   uint8_t res = rx_buf[port][(rx_head[port] - rx_size[port]) % BUF_SZ];
   rx_size[port]--;

   ucsr[port][B] |= (1 << 7); /* enable receive interrupt */
   //sei();
   return res;
}

uint8_t * tx_ptrs[4][PTR_SZ];
uint16_t tx_szs[4][PTR_SZ];
uint16_t tx_pos[4] = {0, 0, 0, 0};

/* Get the number of bytes remaining in this window */
uint16_t time_remain() {
   // TCNT0: interrupt counter at 16MHz / 64 = 250kHz
   // window is 1/20 sec = 12500 counts
   // at 115200, window is 576 bytes
   // 576 = 24 * 24 = 2^6 * 3^2
   // 250 = 2 * 5^3
   // bytes per timer tick: 576 / 250 = 288 / 125
   uint16_t r = 249 - TCNT0;
   r *= 288;
   r /= 125;
   return r;
}

/* get the number of bytes that are unaccounted for in this transmit window */
uint16_t window_remain(uint8_t port) {
   uint16_t count = 0;
   uint8_t i = 0;
   // disable interrupts; lock
   cli();

   // count remaining bytes
   for( i=0; i<tx_size[port]; i++ ) {
      // count down; the same way the serial routines do
      count += tx_szs[port][(tx_pos[port]-i) % PTR_SZ];
   }

   // re-enable interrupts
   sei();

   return time_remain() - count;
}

/* transmit an entire buffer
 * the bufsz will be set to 0 when transmit is complete */
void tx_buffer(uint8_t port, uint8_t * buf, uint16_t bufsz) {
   while(tx_size[port] >= PTR_SZ);

   ucsr[port][B] &= ~(1 << 5); /* diable send interrupt (locking) */

   tx_ptrs[port][tx_head[port]] = buf;
   tx_szs[port][tx_head[port]] = bufsz;
   tx_head[port]++;
   tx_head[port] %= PTR_SZ;
   tx_size[port]++;

   ucsr[port][B] |= (1 << 5); /* enable send interrupt */
}

/* priority tx: push to front of tx queue
 */
void priority_tx(uint8_t port, uint8_t * buf, uint16_t bufsz) {
   while(tx_size[port] >= PTR_SZ); // TODO: consider dropping this check

   ucsr[port][B] &= ~(1 << 5); /* diable send interrupt (locking) */

   uint8_t p = (tx_head[port] - tx_size[port] - 1) % PTR_SZ;
   // TODO: push onto other end of circular fifo
   tx_ptrs[port][p] = buf;
   tx_szs[port][p] = bufsz;
   tx_pos[port] = 0;
   //tx_head[port]++;
   //tx_head[port] %= PTR_SZ;
   tx_size[port]++;

   ucsr[port][B] |= (1 << 5); /* enable send interrupt */
}

volatile uint8_t * rxtx[] = {&DDRE, &DDRD, &DDRH, &DDRJ};
uint8_t rxbit[] = {0, 2, 0, 0};

volatile uint16_t * ubrr[] = {&UBRR0, &UBRR1, &UBRR2, &UBRR3};

/* initialize serial tx */
void serial_init_tx(uint8_t port) {
	ucsr[port][C] = 0x8E; /* no parity, 1 stop, 8bit */

   *ubrr[port] = 0x67; /* serial divisor 9600 baud */

	/* USART init code */
   ucsr[port][B] |= 0x08; /* RX and TX enable, interrupts */

   /* buffer init */
   tx_head[port] = 0;
   tx_size[port] = 0;

   /* tx pos init */
   tx_pos[port] = 0;
}

/* initialize serial rx */
void serial_init_rx(uint8_t port) {
   /* rx pin setup */
   /* set pin input */
   rxtx[port][0] &= ~(1 << rxbit[port]);
   /* enable input pin pull-up */
   rxtx[port][1] &= ~(1 << rxbit[port]);

	ucsr[port][C] = 0x8E; /* no parity, 1 stop, 8bit */

   *ubrr[port] = 0x67; /* serial divisor 9600 baud */

	/* USART init code */
   ucsr[port][B] |= 0x10; /* RX and TX enable, interrupts */

   /* buffer init */
   rx_head[port] = 0;
   rx_size[port] = 0;

	/* set the USART_RXC interrupt enable bit */
   ucsr[port][B] |= (1 << 7);
}

/* setup and enable serial interrupts */
void serial_init(uint8_t port)
{
   serial_stop(port);

   serial_init_rx(port);
   serial_init_tx(port);
}

void serial_baud(uint8_t port, uint32_t baud) {
   uint32_t ubr = 10000000; // ubr is 10x input clock
   ubr /= baud; // divide by baud rate
   // ubr is now 10x target value

   // add 5 (0.5) and divide by 10 to round properly
   ubr += 5; 
   ubr /= 10;

   // final subtraction
   ubr--;
   // FIXME: deal with baud rates that are too high here
   *ubrr[port] = ubr;
}

/* stops the serial interrupts */
void serial_stop(uint8_t port)
{
	/* clear both interrupt enable bits */
   ucsr[port][B] &= ~( (1 << 5) | (1 << 7));
}

