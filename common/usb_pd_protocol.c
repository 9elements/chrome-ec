/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "board.h"
#include "common.h"
#include "console.h"
#include "crc.h"
#include "gpio.h"
#include "hooks.h"
#include "registers.h"
#include "task.h"
#include "timer.h"
#include "util.h"
#include "usb_pd.h"
#include "usb_pd_config.h"

#ifdef CONFIG_COMMON_RUNTIME
#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ## args)

/* dump full packet on RX error */
static int debug_dump;
#else
#define CPRINTF(format, args...)
const int debug_dump;
#endif

/* Control Message type */
enum {
	/* 0 Reserved */
	PD_CTRL_GOOD_CRC = 1,
	PD_CTRL_GOTO_MIN = 2,
	PD_CTRL_ACCEPT = 3,
	PD_CTRL_REJECT = 4,
	PD_CTRL_PING = 5,
	PD_CTRL_PS_RDY = 6,
	PD_CTRL_GET_SOURCE_CAP = 7,
	PD_CTRL_GET_SINK_CAP = 8,
	PD_CTRL_PROTOCOL_ERR = 9,
	PD_CTRL_SWAP = 10,
	/* 11 Reserved */
	PD_CTRL_WAIT = 12,
	PD_CTRL_SOFT_RESET = 13,
	/* 14-15 Reserved */
};

/* Data message type */
enum {
	/* 0 Reserved */
	PD_DATA_SOURCE_CAP = 1,
	PD_DATA_REQUEST = 2,
	PD_DATA_BIST = 3,
	PD_DATA_SINK_CAP = 4,
	/* 5-14 Reserved */
	PD_DATA_VENDOR_DEF = 15,
};

/* Protocol revision */
#define PD_REV10 0

/* Port role */
#define PD_ROLE_SINK   0
#define PD_ROLE_SOURCE 1

/* build message header */
#define PD_HEADER(type, role, id, cnt) \
	((type) | (PD_REV10 << 6) | \
	 ((role) << 8) | ((id) << 9) | ((cnt) << 12))

#define PD_HEADER_CNT(header)  (((header) >> 12) & 7)
#define PD_HEADER_TYPE(header) ((header) & 0xF)
#define PD_HEADER_ID(header)   (((header) >> 9) & 7)

/* Encode 5 bits using Biphase Mark Coding */
#define BMC(x)   ((x &  1 ? 0x001 : 0x3FF) \
		^ (x &  2 ? 0x004 : 0x3FC) \
		^ (x &  4 ? 0x010 : 0x3F0) \
		^ (x &  8 ? 0x040 : 0x3C0) \
		^ (x & 16 ? 0x100 : 0x300))

/* 4b/5b + Bimark Phase encoding */
static const uint16_t bmc4b5b[] = {
/* 0 = 0000 */ BMC(0x1E) /* 11110 */,
/* 1 = 0001 */ BMC(0x09) /* 01001 */,
/* 2 = 0010 */ BMC(0x14) /* 10100 */,
/* 3 = 0011 */ BMC(0x15) /* 10101 */,
/* 4 = 0100 */ BMC(0x0A) /* 01010 */,
/* 5 = 0101 */ BMC(0x0B) /* 01011 */,
/* 6 = 0110 */ BMC(0x0E) /* 01110 */,
/* 7 = 0111 */ BMC(0x0F) /* 01111 */,
/* 8 = 1000 */ BMC(0x12) /* 10010 */,
/* 9 = 1001 */ BMC(0x13) /* 10011 */,
/* A = 1010 */ BMC(0x16) /* 10110 */,
/* B = 1011 */ BMC(0x17) /* 10111 */,
/* C = 1100 */ BMC(0x1A) /* 11010 */,
/* D = 1101 */ BMC(0x1B) /* 11011 */,
/* E = 1110 */ BMC(0x1C) /* 11100 */,
/* F = 1111 */ BMC(0x1D) /* 11101 */,
/* Sync-1      K-code       11000 Startsynch #1 */
/* Sync-2      K-code       10001 Startsynch #2 */
/* RST-1       K-code       00111 Hard Reset #1 */
/* RST-2       K-code       11001 Hard Reset #2 */
/* EOP         K-code       01101 EOP End Of Packet */
/* Reserved    Error        00000 */
/* Reserved    Error        00001 */
/* Reserved    Error        00010 */
/* Reserved    Error        00011 */
/* Reserved    Error        00100 */
/* Reserved    Error        00101 */
/* Reserved    Error        00110 */
/* Reserved    Error        01000 */
/* Reserved    Error        01100 */
/* Reserved    Error        10000 */
/* Reserved    Error        11111 */
};
#define PD_SYNC1 0x18
#define PD_SYNC2 0x11
#define PD_RST1  0x07
#define PD_RST2  0x19
#define PD_EOP   0x0D

static const uint8_t dec4b5b[] = {
/* Error    */ 0x10 /* 00000 */,
/* Error    */ 0x10 /* 00001 */,
/* Error    */ 0x10 /* 00010 */,
/* Error    */ 0x10 /* 00011 */,
/* Error    */ 0x10 /* 00100 */,
/* Error    */ 0x10 /* 00101 */,
/* Error    */ 0x10 /* 00110 */,
/* RST-1    */ 0x13 /* 00111 K-code: Hard Reset #1 */,
/* Error    */ 0x10 /* 01000 */,
/* 1 = 0001 */ 0x01 /* 01001 */,
/* 4 = 0100 */ 0x04 /* 01010 */,
/* 5 = 0101 */ 0x05 /* 01011 */,
/* Error    */ 0x10 /* 01100 */,
/* EOP      */ 0x15 /* 01101 K-code: EOP End Of Packet */,
/* 6 = 0110 */ 0x06 /* 01110 */,
/* 7 = 0111 */ 0x07 /* 01111 */,
/* Error    */ 0x10 /* 10000 */,
/* Sync-2   */ 0x12 /* 10001 K-code: Startsynch #2 */,
/* 8 = 1000 */ 0x08 /* 10010 */,
/* 9 = 1001 */ 0x09 /* 10011 */,
/* 2 = 0010 */ 0x02 /* 10100 */,
/* 3 = 0011 */ 0x03 /* 10101 */,
/* A = 1010 */ 0x0A /* 10110 */,
/* B = 1011 */ 0x0B /* 10111 */,
/* Sync-1   */ 0x11 /* 11000 K-code: Startsynch #1 */,
/* RST-2    */ 0x14 /* 11001 K-code: Hard Reset #2 */,
/* C = 1100 */ 0x0C /* 11010 */,
/* D = 1101 */ 0x0D /* 11011 */,
/* E = 1110 */ 0x0E /* 11100 */,
/* F = 1111 */ 0x0F /* 11101 */,
/* 0 = 0000 */ 0x00 /* 11110 */,
/* Error    */ 0x10 /* 11111 */,
};

/* Start of Packet sequence : three Sync-1 K-codes, then one Sync-2 K-code */
#define PD_SOP (PD_SYNC1 | (PD_SYNC1<<5) | (PD_SYNC1<<10) | (PD_SYNC2<<15))

/* Hard Reset sequence : three RST-1 K-codes, then one RST-2 K-code */
#define PD_HARD_RESET (PD_RST1 | (PD_RST1 << 5) |\
		      (PD_RST1 << 10) | (PD_RST2 << 15))

/* PD counter definitions */
#define PD_MESSAGE_ID_COUNT 7
#define PD_RETRY_COUNT 2
#define PD_HARD_RESET_COUNT 2
#define PD_CAPS_COUNT 50

/* Timers */
#define PD_T_SEND_SOURCE_CAP 1500000 /* us (between 1s and 2s) */
#define PD_T_GET_SOURCE_CAP  1500000 /* us (between 1s and 2s) */
#define PD_T_SOURCE_ACTIVITY   45000 /* us (between 40ms and 50ms) */

/* Port role at startup */
#ifdef CONFIG_USB_PD_DUAL_ROLE
#define PD_ROLE_DEFAULT PD_ROLE_SINK
#else
#define PD_ROLE_DEFAULT PD_ROLE_SOURCE
#endif

/* current port role */
static uint8_t pd_role = PD_ROLE_DEFAULT;
/* 3-bit rolling message ID counter */
static uint8_t pd_message_id;
/* Port polarity : 0 => CC1 is CC line, 1 => CC2 is CC line */
static uint8_t pd_polarity;

static enum {
	PD_STATE_DISABLED,
#ifdef CONFIG_USB_PD_DUAL_ROLE
	PD_STATE_SNK_DISCONNECTED,
	PD_STATE_SNK_DISCOVERY,
	PD_STATE_SNK_TRANSITION,
	PD_STATE_SNK_READY,
#endif /* CONFIG_USB_PD_DUAL_ROLE */

	PD_STATE_SRC_DISCONNECTED,
	PD_STATE_SRC_DISCOVERY,
	PD_STATE_SRC_NEGOCIATE,
	PD_STATE_SRC_ACCEPTED,
	PD_STATE_SRC_TRANSITION,
	PD_STATE_SRC_READY,

	PD_STATE_HARD_RESET,
	PD_STATE_BIST,
} pd_task_state = PD_DEFAULT_STATE;

/* increment message ID counter */
static void inc_id(void)
{
	pd_message_id = (pd_message_id + 1) & PD_MESSAGE_ID_COUNT;
}

static inline int encode_short(void *ctxt, int off, uint16_t val16)
{
	off = pd_write_sym(ctxt, off, bmc4b5b[(val16 >> 0) & 0xF]);
	off = pd_write_sym(ctxt, off, bmc4b5b[(val16 >> 4) & 0xF]);
	off = pd_write_sym(ctxt, off, bmc4b5b[(val16 >> 8) & 0xF]);
	return pd_write_sym(ctxt, off, bmc4b5b[(val16 >> 12) & 0xF]);
}

static inline int encode_word(void *ctxt, int off, uint32_t val32)
{
	off = encode_short(ctxt, off, (val32 >> 0) & 0xFFFF);
	return encode_short(ctxt, off, (val32 >> 16) & 0xFFFF);
}

/* prepare a 4b/5b-encoded PD message to send */
static int prepare_message(void *ctxt, uint16_t header, uint8_t cnt,
			   const uint32_t *data)
{
	int off, i;
	crc32_init();
	/* 64-bit preamble */
	off = pd_write_preamble(ctxt);
	/* Start Of Packet: 3x Sync-1 + 1x Sync-2 */
	off = pd_write_sym(ctxt, off, BMC(PD_SYNC1));
	off = pd_write_sym(ctxt, off, BMC(PD_SYNC1));
	off = pd_write_sym(ctxt, off, BMC(PD_SYNC1));
	off = pd_write_sym(ctxt, off, BMC(PD_SYNC2));
	/* header */
	off = encode_short(ctxt, off, header);
	crc32_hash16(header);
	/* data payload */
	for (i = 0; i < cnt; i++) {
		off = encode_word(ctxt, off, data[i]);
		crc32_hash32(data[i]);
	}
	/* CRC */
	off = encode_word(ctxt, off, crc32_result());
	/* End Of Packet */
	off = pd_write_sym(ctxt, off, BMC(PD_EOP));
	/* Ensure that we have a final edge */
	return pd_write_last_edge(ctxt, off);
}

static int analyze_rx(uint32_t *payload);

static void send_hard_reset(void *ctxt)
{
	int off;

	/* 64-bit preamble */
	off = pd_write_preamble(ctxt);
	/* Hard-Reset: 3x RST-1 + 1x RST-2 */
	off = pd_write_sym(ctxt, off, BMC(PD_RST1));
	off = pd_write_sym(ctxt, off, BMC(PD_RST1));
	off = pd_write_sym(ctxt, off, BMC(PD_RST1));
	off = pd_write_sym(ctxt, off, BMC(PD_RST2));
	/* Ensure that we have a final edge */
	off = pd_write_last_edge(ctxt, off);
	/* Transmit the packet */
	pd_start_tx(ctxt, off);
	pd_tx_done();
}

static int send_validate_message(void *ctxt, uint16_t header, uint8_t cnt,
				 const uint32_t *data)
{
	int r;
	static uint32_t payload[7];

	/* retry 3 times if we are not getting a valid answer */
	for (r = 0; r <= PD_RETRY_COUNT; r++) {
		int bit_len;
		uint16_t head;
		/* write the encoded packet in the transmission buffer */
		bit_len = prepare_message(ctxt, header, cnt, data);
		/* Transmit the packet */
		pd_start_tx(ctxt, bit_len);
		pd_tx_done();
		/* starting waiting for GoodCrc */
		pd_rx_start();
		/* read the incoming packet if any */
		head = analyze_rx(payload);
		pd_rx_complete();
		if (head > 0) { /* we got a good packet, analyze it */
			int type = PD_HEADER_TYPE(head);
			int nb = PD_HEADER_CNT(head);
			uint8_t id = PD_HEADER_ID(head);
			if (type == PD_CTRL_GOOD_CRC && nb == 0 &&
			   id == pd_message_id) {
				/* got the GoodCRC we were expecting */
				inc_id();
				return bit_len;
			} else {
				CPRINTF("ERR ACK/%d %04x\n", id, head);
			}
		}
	}
	/* we failed all the re-transmissions */
	/* TODO: try HardReset */
	CPRINTF("TX NO ACK %04x/%d\n", header, cnt);
	return -1;
}

static int send_control(void *ctxt, int type)
{
	int bit_len;
	uint16_t header = PD_HEADER(type, pd_role, pd_message_id, 0);

	bit_len = send_validate_message(ctxt, header, 0, NULL);

	CPRINTF("CTRL[%d]>%d\n", type, bit_len);

	return bit_len;
}

static void send_goodcrc(void *ctxt, int id)
{
	uint16_t header = PD_HEADER(PD_CTRL_GOOD_CRC, pd_role, id, 0);
	int bit_len = prepare_message(ctxt, header, 0, NULL);

	pd_start_tx(ctxt, bit_len);
	pd_tx_done();
}

static int send_source_cap(void *ctxt)
{
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_SOURCE_CAP, pd_role, pd_message_id,
				    pd_src_pdo_cnt);

	bit_len = send_validate_message(ctxt, header, pd_src_pdo_cnt,
					pd_src_pdo);
	CPRINTF("srcCAP>%d\n", bit_len);

	return bit_len;
}

#ifdef CONFIG_USB_PD_DUAL_ROLE
static void send_sink_cap(void *ctxt)
{
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_SINK_CAP, pd_role, pd_message_id,
				    pd_snk_pdo_cnt);

	bit_len = send_validate_message(ctxt, header, pd_snk_pdo_cnt,
					pd_snk_pdo);
	CPRINTF("snkCAP>%d\n", bit_len);
}

static void send_request(void *ctxt, uint32_t rdo)
{
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_REQUEST, pd_role, pd_message_id, 1);

	bit_len = send_validate_message(ctxt, header, 1, &rdo);
	CPRINTF("REQ%d>\n", bit_len);
}
#endif /* CONFIG_USB_PD_DUAL_ROLE */

static int send_bist(void *ctxt)
{
	uint32_t bdo = BDO(BDO_MODE_TRANSMIT, 0);
	int bit_len;
	uint16_t header = PD_HEADER(PD_DATA_BIST, pd_role, pd_message_id, 1);

	bit_len = send_validate_message(ctxt, header, 1, &bdo);
	CPRINTF("BIST>%d\n", bit_len);

	return bit_len;
}

static void handle_vdm_request(void *ctxt, int cnt, uint32_t *payload)
{
	CPRINTF("Unhandled VDM VID %04x CMD %04x\n", payload[0] >> 16,
		payload[0] & 0xFFFF);
}

static void handle_data_request(void *ctxt, uint16_t head, uint32_t *payload)
{
	int type = PD_HEADER_TYPE(head);
	int cnt = PD_HEADER_CNT(head);

	switch (type) {
#ifdef CONFIG_USB_PD_DUAL_ROLE
	case PD_DATA_SOURCE_CAP:
		if ((pd_task_state == PD_STATE_SNK_DISCOVERY)
			|| (pd_task_state == PD_STATE_SNK_TRANSITION)) {
			uint32_t rdo;
			int res;
			/* we were waiting for them, let's process them */
			res = pd_choose_voltage(cnt, payload, &rdo);
			if (res >= 0) {
				send_request(ctxt, rdo);
				pd_task_state = PD_STATE_SNK_TRANSITION;
			}
		}
		break;
#endif /* CONFIG_USB_PD_DUAL_ROLE */
	case PD_DATA_REQUEST:
		if ((pd_role == PD_ROLE_SOURCE) && (cnt == 1))
			if (!pd_request_voltage(payload[0])) {
				send_control(ctxt, PD_CTRL_ACCEPT);
				pd_task_state = PD_STATE_SRC_ACCEPTED;
				return;
			}
		/* the message was incorrect or cannot be satisfied */
		send_control(ctxt, PD_CTRL_REJECT);
		break;
	case PD_DATA_BIST:
		CPRINTF("BIST not supported\n");
		break;
	case PD_DATA_SINK_CAP:
		break;
	case PD_DATA_VENDOR_DEF:
		handle_vdm_request(ctxt, cnt, payload);
		break;
	default:
		CPRINTF("Unhandled data message type %d\n", type);
	}
}

static void handle_ctrl_request(void *ctxt, uint16_t head, uint32_t *payload)
{
	int type = PD_HEADER_TYPE(head);

	switch (type) {
	case PD_CTRL_GOOD_CRC:
		/* should not get it */
		break;
	case PD_CTRL_PING:
		/* Nothing else to do */
		break;
	case PD_CTRL_GET_SOURCE_CAP:
		send_source_cap(ctxt);
		break;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	case PD_CTRL_GET_SINK_CAP:
		send_sink_cap(ctxt);
		break;
	case PD_CTRL_GOTO_MIN:
		break;
	case PD_CTRL_PS_RDY:
		if (pd_role == PD_ROLE_SINK)
			pd_task_state = PD_STATE_SNK_READY;
		break;
#endif /* CONFIG_USB_PD_DUAL_ROLE */
	case PD_CTRL_ACCEPT:
		break;
	case PD_CTRL_REJECT:
		break;
	case PD_CTRL_PROTOCOL_ERR:
	case PD_CTRL_SWAP:
	case PD_CTRL_WAIT:
	case PD_CTRL_SOFT_RESET:
	default:
		CPRINTF("Unhandled ctrl message type %d\n", type);
	}
}

static void handle_request(void *ctxt, uint16_t head, uint32_t *payload)
{
	int cnt = PD_HEADER_CNT(head);
	int p;

	if (PD_HEADER_TYPE(head) != 1 || cnt)
		send_goodcrc(ctxt, PD_HEADER_ID(head));

	/* dump received packet content */
	CPRINTF("RECV %04x/%d ", head, cnt);
	for (p = 0; p < cnt; p++)
		CPRINTF("[%d]%08x ", p, payload[p]);
	CPRINTF("\n");

	if (cnt)
		handle_data_request(ctxt, head, payload);
	else
		handle_ctrl_request(ctxt, head, payload);
}

static inline int decode_short(void *ctxt, int off, uint16_t *val16)
{
	uint32_t w;
	int end;

	end = pd_dequeue_bits(ctxt, off, 20, &w);

#if 0 /* DEBUG */
	CPRINTS("%d-%d: %05x %x:%x:%x:%x\n",
		off, end, w,
		dec4b5b[(w >> 15) & 0x1f], dec4b5b[(w >> 10) & 0x1f],
		dec4b5b[(w >>  5) & 0x1f], dec4b5b[(w >>  0) & 0x1f]);
#endif
	*val16 = dec4b5b[w & 0x1f] |
		(dec4b5b[(w >>  5) & 0x1f] << 4) |
		(dec4b5b[(w >> 10) & 0x1f] << 8) |
		(dec4b5b[(w >> 15) & 0x1f] << 12);
	return end;
}

static inline int decode_word(void *ctxt, int off, uint32_t *val32)
{
	off = decode_short(ctxt, off, (uint16_t *)val32);
	return decode_short(ctxt, off, ((uint16_t *)val32 + 1));
}

static int analyze_rx(uint32_t *payload)
{
	int bit;
	char *msg = "---";
	uint32_t val = 0;
	uint16_t header;
	uint32_t pcrc, ccrc;
	int p, cnt;
	/* uint32_t eop; */
	void *ctxt;

	crc32_init();
	ctxt = pd_init_dequeue();

	/* Detect preamble */
	bit = pd_find_preamble(ctxt);
	if (bit < 0) {
		msg = "Preamble";
		goto packet_err;
	}

	/* Find the Start Of Packet sequence */
	while (bit > 0) {
		bit = pd_dequeue_bits(ctxt, bit, 20, &val);
		if (val == PD_SOP)
			break;
		/* TODO: detect SOP with 1 error code */
		/* TODO: detect Hard reset */
	}
	if (bit < 0) {
		msg = "SOP";
		goto packet_err;
	}

	/* read header */
	bit = decode_short(ctxt, bit, &header);
	crc32_hash16(header);
	cnt = PD_HEADER_CNT(header);

	/* read payload data */
	for (p = 0; p < cnt && bit > 0; p++) {
		bit = decode_word(ctxt, bit, payload+p);
		crc32_hash32(payload[p]);
	}
	if (bit < 0) {
		msg = "len";
		goto packet_err;
	}

	/* check transmitted CRC */
	bit = decode_word(ctxt, bit, &pcrc);
	ccrc = crc32_result();
	if (bit < 0 || pcrc != ccrc) {
		msg = "CRC";
		if (pcrc != ccrc)
			bit = PD_ERR_CRC;
		/* DEBUG */CPRINTF("CRC %08x <> %08x\n", pcrc, crc32_result());
		goto packet_err;
	}

	/* check End Of Packet */
	/* SKIP EOP for now
	bit = pd_dequeue_bits(ctxt, bit, 5, &eop);
	if (bit < 0 || eop != PD_EOP) {
		msg = "EOP";
		goto packet_err;
	}
	*/

	return header;
packet_err:
	if (debug_dump)
		pd_dump_packet(ctxt, msg);
	else
		CPRINTF("RX ERR (%d)\n", bit);
	return bit;
}

static void execute_hard_reset(void)
{
	pd_message_id = 0;
#ifdef CONFIG_USB_PD_DUAL_ROLE
	pd_task_state = pd_role == PD_ROLE_SINK ? PD_STATE_SNK_DISCONNECTED
						: PD_STATE_SRC_DISCONNECTED;
#else
	pd_task_state = PD_STATE_SRC_DISCONNECTED;
#endif
	pd_power_supply_reset();
	CPRINTF("HARD RESET!\n");
}

void pd_task(void)
{
	int head;
	void *ctxt = pd_hw_init();
	uint32_t payload[7];
	int timeout = 10000;
	uint32_t evt;
	int cc1_volt, cc2_volt;
	int res;

	/* Ensure the power supply is in the default state */
	pd_power_supply_reset();

	while (1) {
		/* monitor for incoming packet */
		pd_rx_enable_monitoring();
		/* wait for next event/packet or timeout expiration */
		evt = task_wait_event(timeout);
		/* incoming packet ? */
		if (evt & PD_EVENT_RX) {
			head = analyze_rx(payload);
			pd_rx_complete();
			if (head > 0)
				handle_request(ctxt, head, payload);
			else if (head == PD_ERR_HARD_RESET)
				execute_hard_reset();
		}
		timeout = -1;
		switch (pd_task_state) {
		case PD_STATE_DISABLED:
			/* Nothing to do */
			break;
		case PD_STATE_SRC_DISCONNECTED:
			/* Vnc monitoring */
			cc1_volt = adc_read_channel(ADC_CH_CC1_PD);
			cc2_volt = adc_read_channel(ADC_CH_CC2_PD);
			if ((cc1_volt < PD_SRC_VNC) ||
			    (cc2_volt < PD_SRC_VNC)) {
				pd_polarity = !(cc1_volt < PD_SRC_VNC);
				pd_task_state = PD_STATE_SRC_DISCOVERY;
			}
			timeout = 10000;
			break;
		case PD_STATE_SRC_DISCOVERY:
			/* Query capabilites of the other side */
			res = send_source_cap(ctxt);
			/* packet was acked => PD capable device) */
			if (res >= 0) {
				pd_task_state = PD_STATE_SRC_NEGOCIATE;
			} else { /* failed, retry later */
				timeout = PD_T_SEND_SOURCE_CAP;
			}
			break;
		case PD_STATE_SRC_NEGOCIATE:
			/* wait for a "Request" message */
			break;
		case PD_STATE_SRC_ACCEPTED:
			/* Accept sent, wait for the end of transition */
			timeout = PD_POWER_SUPPLY_TRANSITION_DELAY;
			pd_task_state = PD_STATE_SRC_TRANSITION;
			break;
		case PD_STATE_SRC_TRANSITION:
			res = pd_set_power_supply_ready();
			/* TODO error fallback */
			/* the voltage output is good, notify the source */
			res = send_control(ctxt, PD_CTRL_PS_RDY);
			if (res >= 0) {
				timeout =  PD_T_SEND_SOURCE_CAP;
				/* it'a time to ping regularly the sink */
				pd_task_state = PD_STATE_SRC_READY;
			}
			/* TODO error fallback */
			break;
		case PD_STATE_SRC_READY:
			/* Verify that the sink is alive */
			res = send_control(ctxt, PD_CTRL_PING);
			if (res < 0) {
				/* The sink died ... TODO */
				pd_task_state = PD_STATE_SRC_DISCOVERY;
				timeout = PD_T_SEND_SOURCE_CAP;
			} else { /* schedule next keep-alive */
				timeout = PD_T_SOURCE_ACTIVITY;
			}
			break;
#ifdef CONFIG_USB_PD_DUAL_ROLE
		case PD_STATE_SNK_DISCONNECTED:
			/* Source connection monitoring */
			cc1_volt = adc_read_channel(ADC_CH_CC1_PD);
			cc2_volt = adc_read_channel(ADC_CH_CC2_PD);
			if ((cc1_volt > PD_SNK_VA) ||
			    (cc2_volt > PD_SNK_VA)) {
				pd_polarity = !(cc1_volt > PD_SNK_VA);
				pd_task_state = PD_STATE_SNK_DISCOVERY;
			}
			timeout = 10000;
			break;
		case PD_STATE_SNK_DISCOVERY:
			res = send_control(ctxt, PD_CTRL_GET_SOURCE_CAP);
			/* packet was acked => PD capable device) */
			if (res >= 0) {
				pd_task_state = PD_STATE_SNK_TRANSITION;
			} else { /* failed, retry later */
				timeout = PD_T_GET_SOURCE_CAP;
			}
			break;
		case PD_STATE_SNK_TRANSITION:
			break;
		case PD_STATE_SNK_READY:
			/* we have power and we are happy */
			break;
#endif /* CONFIG_USB_PD_DUAL_ROLE */
		case PD_STATE_HARD_RESET:
			send_hard_reset(ctxt);
			/* reset our own state machine */
			execute_hard_reset();
			break;
		case PD_STATE_BIST:
			send_bist(ctxt);
			pd_task_state = PD_STATE_DISABLED;
			break;
		}
	}
}

void pd_rx_event(void)
{
	task_set_event(TASK_ID_PD, PD_EVENT_RX, 0);
}

#ifdef CONFIG_COMMON_RUNTIME
static int command_pd(int argc, char **argv)
{
	if (argc < 2)
		return EC_ERROR_PARAM1;

	if (!strcasecmp(argv[1], "tx")) {
		pd_task_state = PD_STATE_SNK_DISCOVERY;
		task_wake(TASK_ID_PD);
	} else if (!strcasecmp(argv[1], "rx")) {
		pd_rx_event();
	} else if (!strcasecmp(argv[1], "bist")) {
		pd_task_state = PD_STATE_BIST;
		task_wake(TASK_ID_PD);
	} else if (!strcasecmp(argv[1], "charger")) {
		pd_role = PD_ROLE_SOURCE;
		pd_set_host_mode(1);
		pd_task_state = PD_STATE_SRC_DISCONNECTED;
		task_wake(TASK_ID_PD);
	} else if (!strncasecmp(argv[1], "dev", 3)) {
		if (argc >= 3) {
			unsigned max_volt;
			char *e;

			max_volt = strtoi(argv[2], &e, 10);
			pd_set_max_voltage(max_volt * 1000);
		}

		pd_role = PD_ROLE_SINK;
		pd_set_host_mode(0);
		pd_task_state = PD_STATE_SNK_DISCONNECTED;
		task_wake(TASK_ID_PD);
	} else if (!strcasecmp(argv[1], "clock")) {
		int freq;
		char *e;

		if (argc < 3)
			return EC_ERROR_PARAM2;

		freq = strtoi(argv[2], &e, 10);
		if (*e)
			return EC_ERROR_PARAM2;
		pd_set_clock(freq);
		ccprintf("set TX frequency to %d Hz\n", freq);
	} else if (!strcasecmp(argv[1], "dump")) {
		debug_dump = !debug_dump;
	} else if (!strncasecmp(argv[1], "hard", 4)) {
		pd_task_state = PD_STATE_HARD_RESET;
		task_wake(TASK_ID_PD);
	} else if (!strncasecmp(argv[1], "ping", 4)) {
		pd_role = PD_ROLE_SOURCE;
		pd_set_host_mode(1);
		pd_task_state = PD_STATE_SRC_READY;
		task_wake(TASK_ID_PD);
	} else if (!strncasecmp(argv[1], "state", 5)) {
		const char * const state_names[] = {
			"DISABLED",
			"SNK_DISCONNECTED", "SNK_DISCOVERY", "SNK_TRANSITION",
			"SNK_READY",
			"SRC_DISCONNECTED", "SRC_DISCOVERY", "SRC_NEGOCIATE",
			"SRC_READY",
			"HARD_RESET", "BIST",
		};
		ccprintf("Role: %s Polarity: CC%d State: %s\n",
			pd_role == PD_ROLE_SOURCE ? "SRC" : "SNK",
			pd_polarity + 1, state_names[pd_task_state]);
	} else {
		return EC_ERROR_PARAM1;
	}

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(pd, command_pd,
			"[rx|tx|hardreset|clock|connect]",
			"USB PD",
			NULL);
#endif /* CONFIG_COMMON_RUNTIME */
