/*
 * byd.c --- Driver for BYD BTP-10463
 *
 * Copyright (C) 2015, Tai Chi Minh Ralph Eastwood
 * Copyright (C) 2015, Martin Wimpress
 * Copyright (C) 2015, Richard Pospesel
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Protocol of BYD Touch Pad reverse-engineered.
 * Datasheet: http://bydit.com/userfiles/file/BTP10463-XXX.pdf
 *
 */
#include <linux/types.h>
#include <linux/kernel.h>

#include <linux/input.h>
#include <linux/serio.h>
#include <linux/slab.h>
#include <linux/libps2.h>

#include "psmouse.h"
#include "byd.h"

#define BYD_MODEL_ID_LEN        2
#define BYD_CMD_PAIR(c)		((1 << 12) | (c))
#define BYD_CMD_PAIR_R(r,c)	((1 << 12) | (r << 8) | (c))

/* BYD pad constants */

/* 
 * true device resolution is unknown, however experiments show the
 * resolution is about 111 units/mm
 * absolute coordinate packets are in the range 0-255 for both X and y
 * we pick ABS_X/ABS_Y dimensions which are multiples of 256 and in 
 * the right ballpark given the touchpad's physical dimensions and estimate resolution
 * per spec sheet, device active area dimensions are 101.6 x 60.1 mm
 */

#define BYD_CONST_PAD_WIDTH                     11264
#define BYD_CONST_PAD_HEIGHT                    6656
#define BYD_CONST_PAD_RESOLUTION                111


/* BYD commands reverse engineered from windows driver */

/* 
 * swipe gesture from off-pad to on-pad
 *  0 : disable
 *  1 : enable
 */
#define BYD_CMD_SET_OFFSCREEN_SWIPE             0xcc
/*
 * tap and drag delay time
 *  0 : disable
 *  1 - 8 : least to most delay
 */
#define BYD_CMD_SET_TAP_DRAG_DELAY_TIME         0xcf
/*
 * physical buttons function mapping
 *  0 : enable
 *  4 : normal
 *  5 : left button custom command
 *  6 : right button custom command
 *  8 : disable
 */
#define BYD_CMD_SET_PHYSICAL_BUTTONS            0xd0
/*
 * absolute mode (1 byte X/Y resolution)
 *  0 : disable
 *  2 : enable
 */
#define BYD_CMD_SET_ABSOLUTE_MODE               0xd1
/*
 * two finger scrolling
 *  1 : vertical
 *  2 : horizontal
 *  3 : vertical + horizontal
 *  4 : disable
 */
#define BYD_CMD_SET_TWO_FINGER_SCROLL           0xd2
/*
 * handedness
 *  1 : right handed
 *  2 : left handed
 */
#define BYD_CMD_SET_HANDEDNESS                  0xd3
/*
 * tap to click
 *  1 : enable
 *  2 : disable
 */
#define BYD_CMD_SET_TAP                         0xd4
/*
 * tap and drag
 *  1 : tap and hold to drag
 *  2 : tap and hold to drag + lock
 *  3 : disable
 */
#define BYD_CMD_SET_TAP_DRAG                    0xd5
/*
 * touch sensitivity
 *  1 - 7 : least to most sensitive
 */
#define BYD_CMD_SET_TOUCH_SENSITIVITY           0xd6
/*
 * one finger scrolling
 *  1 : vertical
 *  2 : horizontal
 *  3 : vertical + horizontal
 *  4 : disable
 */
#define BYD_CMD_SET_ONE_FINGER_SCROLL           0xd7
/*
 * one finger scrolling function
 *  1 : free scrolling
 *  2 : edge motion
 *  3 : free scrolling + edge motion
 *  4 : disable
 */
#define BYD_CMD_SET_ONE_FINGER_SCROLL_FUNC      0xd8
/*
 * sliding speed
 *  1 - 5 : slowest to fastest
 */
#define BYD_CMD_SET_SLIDING_SPEED               0xda
/*
 * edge motion
 *  1 : disable
 *  2 : enable when dragging
 *  3 : enable when dragging and pointing
 */
#define BYD_CMD_SET_EDGE_MOTION                 0xdb
/*
 * left edge region size
 *  0 - 7 : smallest to largest width
 */
#define BYD_CMD_SET_LEFT_EDGE_REGION            0xdc
/* 
 * top edge region size
 *  0 - 9 : smallest to largest height
 */
#define BYD_CMD_SET_TOP_EDGE_REGION             0xdd
/*
 * disregard palm press as clicks
 *  1 - 6 : smallest to largest
 */
#define BYD_CMD_SET_PALM_CHECK                  0xde
/* right edge region size
 *  0 - 7 : smallest to largest width
 */
#define BYD_CMD_SET_RIGHT_EDGE_REGION           0xdf
/*
 * bottom edge region size
 *  0 - 9 : smallest to largest height
 */
#define BYD_CMD_SET_BOTTOM_EDGE_REGION          0xe1
/*
 * multitouch gestures
 *  1 : enable
 *  2 : disable
 */
#define BYD_CMD_SET_MULTITOUCH                  0xe3
/*
 * edge motion speed
 *  0 : control with finger pressure
 *  1 - 9 : slowest to fastest
 */
#define BYD_CMD_SET_EDGE_MOTION_SPEED           0xe4
/*
 * two finger scolling funtion
 *  0 : free scrolling
 *  1 : free scrolling (with momentum)
 *  2 : edge motion
 *  3 : free scrolling (with momentum) + edge motion
 *  4 : disable
 */
#define BYD_CMD_SET_TWO_FINGER_SCROLL_FUNC      0xe5

/* BYD Packets */

#define BYD_PKT_RELATIVE                        0x00
#define BYD_PKT_ABSOLUTE                        0xf8
#define BYD_PKT_PINCH_IN                        0xd8
#define BYD_PKT_PINCH_OUT                       0x28
#define BYD_PKT_ROTATE_CLOCKWISE                0x29 
#define BYD_PKT_ROTATE_ANTICLOCKWISE            0xd7
#define BYD_PKT_TWO_FINGER_SCROLL_RIGHT         0x2a
#define BYD_PKT_TWO_FINGER_SCROLL_DOWN          0x2b
#define BYD_PKT_TWO_FINGER_SCROLL_UP            0xd5
#define BYD_PKT_TWO_FINGER_SCROLL_LEFT          0xd6
#define BYD_PKT_THREE_FINGER_SWIPE_RIGHT        0x2c
#define BYD_PKT_THREE_FINGER_SWIPE_DOWN         0x2d
#define BYD_PKT_THREE_FINGER_SWIPE_UP           0xd3
#define BYD_PKT_THREE_FINGER_SWIPE_LEFT         0xd4
#define BYD_PKT_FOUR_FINGER_DOWN                0x33
#define BYD_PKT_FOUR_FINGER_UP                  0xcd
#define BYD_PKT_REGION_SCROLL_RIGHT             0x35
#define BYD_PKT_REGION_SCROLL_DOWN              0x36
#define BYD_PKT_REGION_SCROLL_UP                0xca
#define BYD_PKT_REGION_SCROLL_LEFT              0xcb
#define BYD_PKT_RIGHT_CORNER_CLICK              0xd2
#define BYD_PKT_LEFT_CORNER_CLICK               0x2e
#define BYD_PKT_LEFT_AND_RIGHT_CORNER_CLICK     0x2f
#define BYD_PKT_ONTO_PAD_SWIPE_RIGHT            0x37
#define BYD_PKT_ONTO_PAD_SWIPE_DOWN             0x30
#define BYD_PKT_ONTO_PAD_SWIPE_UP               0xd0
#define BYD_PKT_ONTO_PAD_SWIPE_LEFT             0xc9

struct byd_init_command_pair {
	uint8_t command;
	uint8_t  value;
};

static const struct byd_init_command_pair init_commands[] = {
	{BYD_CMD_SET_HANDEDNESS, 0x01},
	{BYD_CMD_SET_PHYSICAL_BUTTONS, 0x04},
	{BYD_CMD_SET_TAP, 0x02},
	{BYD_CMD_SET_ONE_FINGER_SCROLL, 0x04},
	{BYD_CMD_SET_EDGE_MOTION, 0x01},
	{BYD_CMD_SET_PALM_CHECK, 0x00},
	{BYD_CMD_SET_MULTITOUCH, 0x01},
	{BYD_CMD_SET_TWO_FINGER_SCROLL, 0x03},
	{BYD_CMD_SET_TWO_FINGER_SCROLL_FUNC, 0x00},
	{BYD_CMD_SET_LEFT_EDGE_REGION, 0x00},
	{BYD_CMD_SET_TOP_EDGE_REGION, 0x00},
	{BYD_CMD_SET_RIGHT_EDGE_REGION, 0x0},
	{BYD_CMD_SET_BOTTOM_EDGE_REGION, 0x00},
	{BYD_CMD_SET_ABSOLUTE_MODE, 0x02},
};

struct byd_model_info {
	char name[16];
	char id[BYD_MODEL_ID_LEN];
};


static struct byd_model_info byd_model_data[] = {
	{ "BTP10463", { 0x03, 0x64 } }
};

struct byd_data
{
	struct timer_list timer;
	int32_t abs_x;
	int32_t abs_y;
	uint32_t last_touch_time;
	int16_t rel_x            : 9;
	int16_t rel_y            : 9;
	uint8_t button_left      : 1;
	uint8_t button_right     : 1;
	uint8_t touch            : 1;
	int8_t vertical_scroll   : 2;
	int8_t horizontal_scroll : 2;
};

static void byd_report_input(struct psmouse *psmouse)
{
	struct byd_data *priv = (struct byd_data *)psmouse->private;
	struct input_dev *dev = psmouse->dev;

	input_report_abs(dev, ABS_X, priv->abs_x);
	input_report_abs(dev, ABS_Y, priv->abs_y);
	input_report_key(dev, BTN_LEFT, priv->button_left);
	input_report_key(dev, BTN_RIGHT, priv->button_right);
	input_report_key(dev, BTN_TOUCH, priv->touch);
	input_report_key(dev, BTN_0, priv->vertical_scroll == 1 ? 1 : 0);
	input_report_key(dev, BTN_1, priv->vertical_scroll == -1 ? 1 : 0);
	input_report_key(dev, BTN_2, priv->horizontal_scroll == 1 ? 1 : 0);
	input_report_key(dev, BTN_3, priv->horizontal_scroll == -1 ? 1 : 0);

	input_report_key(dev, BTN_TOOL_FINGER, 1);
	
	input_sync(dev);
}

static void byd_clear_touch(unsigned long data)
{
	struct psmouse *psmouse = (struct psmouse *)data;
	struct byd_data *priv = psmouse->private;

	serio_pause_rx(psmouse->ps2dev.serio);

	priv->touch = 0;

	byd_report_input(psmouse);

	serio_continue_rx(psmouse->ps2dev.serio);
}

static psmouse_ret_t byd_process_byte(struct psmouse *psmouse)
{
	struct byd_data *priv = psmouse->private;
	unsigned char *packet = psmouse->packet;
	uint32_t now_msecs = jiffies_to_msecs(jiffies);


	if (psmouse->pktcnt < psmouse->pktsize)
		return PSMOUSE_GOOD_DATA;

#ifdef DEBUG
	psmouse_dbg(psmouse, "process: packet = %x %x %x %x\n",
			packet[0], packet[1], packet[2], packet[3]);
#endif

	printk("packet: %u %02x, %02x, %02x, %02x\n",
			jiffies_to_msecs(jiffies), packet[0], packet[1], packet[2], packet[3]);

	switch(packet[3])
	{
		case BYD_PKT_ABSOLUTE:
			/* on first touch, use the absolute packet to determine our start location */
			if(priv->touch == 0) {
				priv->button_left = packet[0] & 1;
				priv->button_right = (packet[0] >> 1) & 1;
				priv->abs_x = packet[1] * (BYD_CONST_PAD_WIDTH / 256);
				priv->abs_y = (255 - packet[2]) * (BYD_CONST_PAD_HEIGHT / 256);

				/* needed to detect tap when edge scrolling */
				if(now_msecs - priv->last_touch_time > 64) {
					priv->touch = 1;
				}
			}
			break;
		case BYD_PKT_RELATIVE:
			priv->button_left = packet[0] & 1;
			priv->button_right = (packet[0] >> 1) & 1;
			priv->rel_x = packet[1] ? (int) packet[1] - (int) ((packet[0] << 4) & 0x100) : 0;
			priv->rel_y = packet[2] ? (int) ((packet[0] << 3) & 0x100) - (int) packet[2] : 0;

			/* 
			 * experiments show relative mouse packets come in increments of 
			 * 1 unit / 11 msecs (regardless of time delta between relative packets)
			 */
			priv->abs_x += priv->rel_x * 11;
			priv->abs_y += priv->rel_y * 11;

			priv->touch = 1;
			break;
		/* 
		 * communicate two-finger scroll events as 
		 * scroll button press/release
		 */	
		case BYD_PKT_TWO_FINGER_SCROLL_UP:
			priv->vertical_scroll = 1;
			byd_report_input(psmouse);
			priv->vertical_scroll = 0;
			break;
		case BYD_PKT_TWO_FINGER_SCROLL_DOWN:
			priv->vertical_scroll = -1;
			byd_report_input(psmouse);
			priv->vertical_scroll = 0;
			break;
		case BYD_PKT_TWO_FINGER_SCROLL_RIGHT:
			priv->horizontal_scroll = -1;
			byd_report_input(psmouse);
			priv->horizontal_scroll = 0;
			break;
		case BYD_PKT_TWO_FINGER_SCROLL_LEFT:
			priv->horizontal_scroll = 1;
			byd_report_input(psmouse);
			priv->horizontal_scroll = 0;
	}

	byd_report_input(psmouse);
	
	/* reset time since last touch */
	if(priv->touch == 1) {
		priv->last_touch_time = now_msecs;
		mod_timer(&priv->timer, jiffies + msecs_to_jiffies(32));
	}

	return PSMOUSE_FULL_PACKET;
}

int byd_init(struct psmouse *psmouse)
{
	struct byd_data *priv;
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[4];
	int cmd, error = 0;
	int i = 0;

	/* it needs to be initialised like an intellimouse to get 4-byte packets */
	psmouse_reset(psmouse);
	param[0] = 200;
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRATE);
	param[0] = 100;
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRATE);
	param[0] =  80;
	ps2_command(ps2dev, param, PSMOUSE_CMD_SETRATE);
	ps2_command(ps2dev, param, PSMOUSE_CMD_GETID);

	if (param[0] != 3)
		return -1;

#ifdef DEBUG
	psmouse_dbg(psmouse, "detect: init sequence\n");
#endif

	/* activate the mouse to initialise it */
	psmouse_activate(psmouse);

	/* enter command mode */
	param[0] = 0x00;
	if (ps2_command(ps2dev, param, BYD_CMD_PAIR(0xe2))) {
		error = -EIO;
		goto init_fail;
	}
#ifdef DEBUG
	psmouse_dbg(psmouse, "detect: entered command mode\n");
#endif

	/* send second identification command */
	param[0] = 0x02;
	if (ps2_command(ps2dev, param, BYD_CMD_PAIR(0xe0))) {
		error = -EIO;
		goto init_fail;
	}

	param[0] = 0x01;
	if (ps2_command(ps2dev, param, BYD_CMD_PAIR_R(4, 0xe0))) {
		error = -EIO;
		goto init_fail;
	}

#ifdef DEBUG
	psmouse_dbg(psmouse, "detect: magic %x %x %x %x\n",
			param[0], param[1], param[2], param[3]);
#endif

	/* magic identifier the vendor driver reads */
	if (param[0] != 0x08 || param[1] != 0x01 ||
	    param[2] != 0x01 || param[3] != 0x31) {
		psmouse_err(psmouse, "unknown magic, expected: 08 01 01 31\n");
		error = -EINVAL;
		goto init_fail;
	}

	/*
	 * send the byd vendor commands
	 * these appear to be pairs of (command, param)
	 */
	for(i = 0; i < ARRAY_SIZE(init_commands); i++) {
		param[0] = init_commands[i].value;
		cmd = BYD_CMD_PAIR(init_commands[i].command);
		if(ps2_command(ps2dev, param, cmd)) {
			error = -EIO;
			goto init_fail;
		}
	}

	/* confirm/finalize the above vender command table */
	param[0] = 0x00;
	if (ps2_command(ps2dev, param, BYD_CMD_PAIR(0xe0))) {
		error = -EIO;
		goto init_fail;
	}

	/* exit command mode */
	param[0] = 0x01;
	if (ps2_command(ps2dev, param, BYD_CMD_PAIR(0xe2))) {
		error = -ENOMEM;
		goto init_fail;
	}

	/* alloc space for byd_data */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if(!priv) {
		error = -ENOMEM;
		goto init_fail;
	}

	/* init struct and timer */
	memset(priv, 0x00, sizeof(*priv));
	/* signal touch end after not receiving movement packets for 32 ms */
	setup_timer(&priv->timer, byd_clear_touch, (unsigned long)psmouse);
	psmouse->private = priv;

#ifdef DEBUG
	psmouse_dbg(psmouse, "detect: exit command mode\n");
#endif

	return 0;

init_fail:
	psmouse_deactivate(psmouse);
	return error;
}

static void byd_disconnect(struct psmouse *psmouse)
{
	if (psmouse->private) {	
		struct byd_data *priv = psmouse->private;
		del_timer(&priv->timer);
		kfree(psmouse->private);
		psmouse->private = NULL;
	}
}

static int byd_reconnect(struct psmouse *psmouse)
{
	if (byd_detect(psmouse, 0))
		return -1;

	if (byd_init(psmouse))
		return -1;

	return 0;
}

int byd_detect(struct psmouse *psmouse, bool set_properties)
{
	struct ps2dev *ps2dev = &psmouse->ps2dev;
	unsigned char param[4];
	int i;

	/* reset the mouse */
	psmouse_reset(psmouse);

	/* magic knock - identify the mouse (as per. the datasheet) */
	param[0] = 0x03;
	if (ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES) ||
	    ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES) ||
	    ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES) ||
	    ps2_command(ps2dev, param, PSMOUSE_CMD_SETRES) ||
	    ps2_command(ps2dev, param, PSMOUSE_CMD_GETINFO))
		return -EIO;

#ifdef DEBUG
	psmouse_dbg(psmouse, "detect: model id: %x %x %x\n",
			param[0], param[1], param[2]);
#endif

	/*
	 * match the device - the first byte, param[0], appears to be set
	 * to some unknown value based on the state of the mouse and cannot
	 * be used for identification after suspend.
	 */
	for (i = 0; i < ARRAY_SIZE(byd_model_data); i++) {
		if (!memcmp(param + 1, &byd_model_data[i].id,
				 BYD_MODEL_ID_LEN))
			break;
	}

	/* no match found */
	if (i == ARRAY_SIZE(byd_model_data)) {
#ifdef DEBUG		
		psmouse_dbg(psmouse, "detect: no match found\n");
#endif		
		return -EINVAL;
	} else {
#ifdef DEBUG		
		psmouse_dbg(psmouse, "detect: matched %s\n",
				byd_model_data[i].name);
#endif		
	}

	if (set_properties) {
		struct input_dev *dev = psmouse->dev;

		__set_bit(INPUT_PROP_POINTER, dev->propbit);

		/* touchpad */
 		__set_bit(BTN_TOUCH, dev->keybit);
		__set_bit(BTN_TOOL_FINGER, dev->keybit);

		/* buttons */
		__set_bit(BTN_LEFT, dev->keybit);
		__set_bit(BTN_RIGHT, dev->keybit);
		__clear_bit(BTN_MIDDLE, dev->keybit);

		/* two-finger scroll gesture */
		__set_bit(BTN_0, dev->keybit);
		__set_bit(BTN_1, dev->keybit);
		__set_bit(BTN_2, dev->keybit);
		__set_bit(BTN_3, dev->keybit);

		/* absolute position */
		__set_bit(EV_ABS, dev->evbit);

		input_set_abs_params(dev, ABS_X, 0, BYD_CONST_PAD_WIDTH, 0, 0);
		input_set_abs_params(dev, ABS_Y, 0, BYD_CONST_PAD_HEIGHT, 0, 0);
		input_abs_set_res(dev, ABS_X, BYD_CONST_PAD_RESOLUTION);
		input_abs_set_res(dev, ABS_Y, BYD_CONST_PAD_RESOLUTION);

		/* no relative support */
		__clear_bit(EV_REL, dev->evbit);
		__clear_bit(REL_X, dev->relbit);
		__clear_bit(REL_Y, dev->relbit);

		psmouse->vendor = "BYD";
		psmouse->name = "TouchPad";
		psmouse->protocol_handler = byd_process_byte;
		psmouse->pktsize = 4;
		psmouse->private = NULL;
		psmouse->disconnect = byd_disconnect;
		psmouse->reconnect = byd_reconnect;
	}

	return 0;
}
