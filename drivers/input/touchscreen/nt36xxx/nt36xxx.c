/*
 * Copyright (C) 2010 - 2018 Novatek, Inc.
 * Copyright (C) 2019 XiaoMi, Inc.
 *
 * $Revision: 47247 $
 * $Date: 2019-07-10 10:41:36 +0800 (Wed, 10 Jul 2019) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/proc_fs.h>
#include <linux/input/mt.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/regulator/consumer.h>
#include <linux/hwinfo.h>
#include <linux/moduleparam.h>
#include <linux/cpu.h>

#ifdef CONFIG_DRM
#include <linux/msm_drm_notify.h>
#include <drm/drm_panel.h>
#include <linux/fb.h>
#endif

#include "nt36xxx.h"

#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
#include "../xiaomi/xiaomi_touch.h"
#endif

#if NVT_TOUCH_EXT_PROC
extern int32_t nvt_extra_proc_init(void);
extern void nvt_extra_proc_deinit(void);
#endif

#if NVT_TOUCH_MP
extern int32_t nvt_mp_proc_init(void);
extern void nvt_mp_proc_deinit(void);
#endif

#define SUPER_RESOLUTION_FACOTR             8

struct nvt_ts_data *ts;

static int nvt_fw = 0;
module_param(nvt_fw, int, 0644);

#if BOOT_UPDATE_FIRMWARE
static struct workqueue_struct *nvt_fwu_wq;
extern void Boot_Update_Firmware(struct work_struct *work);
#endif

#if defined(CONFIG_DRM)
static int nvt_drm_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#endif

#define PROC_SYMLINK_PATH "touchpanel"

#if TOUCH_KEY_NUM > 0
const uint16_t touch_key_array[TOUCH_KEY_NUM] = {
	KEY_BACK,
	KEY_HOME,
	KEY_MENU
};
#endif

#if WAKEUP_GESTURE
const uint16_t gesture_key_array[] = {
	KEY_POWER,   //GESTURE_WORD_C
	KEY_POWER,   //GESTURE_WORD_W
	KEY_POWER,   //GESTURE_WORD_V
	KEY_WAKEUP,  //GESTURE_DOUBLE_CLICK
	KEY_POWER,   //GESTURE_WORD_Z
	KEY_POWER,   //GESTURE_WORD_M
	KEY_POWER,   //GESTURE_WORD_O
	KEY_POWER,   //GESTURE_WORD_e
	KEY_POWER,   //GESTURE_WORD_S
	KEY_POWER,   //GESTURE_SLIDE_UP
	KEY_POWER,   //GESTURE_SLIDE_DOWN
	KEY_POWER,   //GESTURE_SLIDE_LEFT
	KEY_POWER,   //GESTURE_SLIDE_RIGHT
};
#endif

static uint8_t bTouchIsAwake = 0;

/*******************************************************
Description:
	Novatek touchscreen irq enable/disable function.

return:
	n.a.
*******************************************************/
static void nvt_irq_enable(bool enable)
{
	struct irq_desc *desc;

	if (enable) {
		if (!ts->irq_enabled) {
			enable_irq(ts->client->irq);
			ts->irq_enabled = true;
		}
	} else {
		if (ts->irq_enabled) {
			disable_irq(ts->client->irq);
			ts->irq_enabled = false;
		}
	}

	desc = irq_to_desc(ts->client->irq);
	NVT_LOG("enable=%d, desc->depth=%d\n", enable, desc->depth);
}

/*******************************************************
Description:
	Novatek touchscreen set index/page/addr address.

return:
	Executive outcomes. 0---succeed. -5---access fail.
*******************************************************/
int32_t nvt_set_page(uint16_t i2c_addr, uint32_t addr)
{
	uint8_t buf[4] = {0};

	buf[0] = 0xFF;	//set index/page/addr command
	buf[1] = (addr >> 16) & 0xFF;
	buf[2] = (addr >> 8) & 0xFF;

	return CTP_I2C_WRITE(ts->client, i2c_addr, buf, 3);
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU then into idle mode
    function.

return:
	n.a.
*******************************************************/
void nvt_sw_reset_idle(void)
{
	uint8_t buf[4]={0};

	//---write i2c cmds to reset idle---
	buf[0]=0x00;
	buf[1]=0xA5;
	CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

	usleep_range(15000, 16000);
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU (boot) function.

return:
	n.a.
*******************************************************/
void nvt_bootloader_reset(void)
{
	uint8_t buf[8] = {0};

	NVT_LOG("start\n");

	//---write i2c cmds to reset---
	buf[0] = 0x00;
	buf[1] = 0x69;
	CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

	// need 35ms delay after bootloader reset
	msleep(35);

	NVT_LOG("end\n");
}

/*******************************************************
Description:
	Novatek touchscreen clear FW status function.

return:
	Executive outcomes. 0---succeed. -1---fail.
*******************************************************/
int32_t nvt_clear_fw_status(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 20;

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		nvt_set_page(I2C_FW_Address, ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE);

		//---clear fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0x00;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

		//---read fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0xFF;
		CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

		if (buf[1] == 0x00)
			break;

		usleep_range(10000, 11000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW status function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_status(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 50;

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		nvt_set_page(I2C_FW_Address, ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE);

		//---read fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0x00;
		CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

		if ((buf[1] & 0xF0) == 0xA0)
			break;

		usleep_range(10000, 11000);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW reset state function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_reset_state(RST_COMPLETE_STATE check_reset_state)
{
	uint8_t buf[8] = {0};
	int32_t ret = 0;
	int32_t retry = 0;

	while (1) {
		usleep_range(10000, 11000);

		//---read reset state---
		buf[0] = EVENT_MAP_RESET_COMPLETE;
		buf[1] = 0x00;
		CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 6);

		if ((buf[1] >= check_reset_state) && (buf[1] <= RESET_STATE_MAX)) {
			ret = 0;
			break;
		}

		retry++;
		if(unlikely(retry > 100)) {
			NVT_ERR("error, retry=%d, buf[1]=0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X\n",
				retry, buf[1], buf[2], buf[3], buf[4], buf[5]);
			ret = -1;
			break;
		}
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get novatek project id information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
int32_t nvt_read_pid(void)
{
	uint8_t buf[3] = {0};
	int32_t ret = 0;

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(I2C_FW_Address, ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_PROJECTID);

	//---read project id---
	buf[0] = EVENT_MAP_PROJECTID;
	buf[1] = 0x00;
	buf[2] = 0x00;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 3);

	ts->nvt_pid = (buf[2] << 8) + buf[1];

	NVT_LOG("PID=%04X\n", ts->nvt_pid);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get firmware related information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
int32_t nvt_get_fw_info(void)
{
	uint8_t buf[64] = {0};
	uint32_t retry_count = 0;
	int32_t ret = 0;

info_retry:
	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(I2C_FW_Address, ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_FWINFO);

	//---read fw info---
	buf[0] = EVENT_MAP_FWINFO;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 17);
	ts->fw_ver = buf[1];
	ts->x_num = buf[3];
	ts->y_num = buf[4];
	ts->abs_x_max = (uint16_t)((buf[5] << 8) | buf[6]);
	ts->abs_y_max = (uint16_t)((buf[7] << 8) | buf[8]);
	ts->max_button_num = buf[11];

	if (nvt_fw) {
		if (ts->fw_ver > 90 ) {
			ts->fw_ver = 92;
		} else {
			ts->fw_ver = 35;
		}
	} else {
		if (ts->fw_ver > 90 ) {
			ts->fw_ver = 91;
		} else {
			ts->fw_ver = 34;
		}
	}

	ts->x_num = 18;
	ts->y_num = 34;

	//---clear x_num, y_num if fw info is broken---
	if ((buf[1] + buf[2]) != 0xFF) {
		NVT_ERR("FW info is broken! fw_ver=0x%02X, ~fw_ver=0x%02X\n", buf[1], buf[2]);
		ts->fw_ver = 0;
		ts->x_num = 18;
		ts->y_num = 32;
		ts->abs_x_max = TOUCH_DEFAULT_MAX_WIDTH;
		ts->abs_y_max = TOUCH_DEFAULT_MAX_HEIGHT;
		ts->max_button_num = TOUCH_KEY_NUM;

		if(retry_count < 3) {
			retry_count++;
			NVT_ERR("retry_count=%d\n", retry_count);
			goto info_retry;
		} else {
			NVT_ERR("Set default fw_ver=%d, x_num=%d, y_num=%d, "
					"abs_x_max=%d, abs_y_max=%d, max_button_num=%d!\n",
					ts->fw_ver, ts->x_num, ts->y_num,
					ts->abs_x_max, ts->abs_y_max, ts->max_button_num);
			ret = -1;
		}
	} else {
		ret = 0;
	}

	//---Get Novatek PID---
	nvt_read_pid();

	return ret;
}

/*******************************************************
  Create Device Node (Proc Entry)
*******************************************************/
#if NVT_TOUCH_PROC
static struct proc_dir_entry *NVT_proc_entry;
#define DEVICE_NAME	"NVTflash"

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash read function.

return:
	Executive outcomes. 2---succeed. -5,-14---failed.
*******************************************************/
static ssize_t nvt_flash_read(struct file *file, char __user *buff, size_t count, loff_t *offp)
{
	uint8_t str[68] = {0};
	int32_t ret = -1;
	int32_t retries = 0;
	int8_t i2c_wr = 0;

	if (count > sizeof(str)) {
		NVT_ERR("error count=%zu\n", count);
		return -EFAULT;
	}

	if (copy_from_user(str, buff, count)) {
		NVT_ERR("copy from user error\n");
		return -EFAULT;
	}

	i2c_wr = str[0] >> 7;

	if (i2c_wr == 0) {	//I2C write
		while (retries < 20) {

			ret = CTP_I2C_WRITE(ts->client, (str[0] & 0x7F), &str[2], str[1]);
			if (ret == 1)
				break;
			else
				NVT_ERR("error, retries=%d, ret=%d\n", retries, ret);

			retries++;
		}

		if (unlikely(retries == 20)) {
			NVT_ERR("error, ret = %d\n", ret);
			return -EIO;
		}

		return ret;
	} else if (i2c_wr == 1) {	//I2C read
		while (retries < 20) {
			ret = CTP_I2C_READ(ts->client, (str[0] & 0x7F), &str[2], str[1]);
			if (ret == 2)
				break;
			else
				NVT_ERR("error, retries=%d, ret=%d\n", retries, ret);

			retries++;
		}

		// copy buff to user if i2c transfer
		if (retries < 20) {
			if (copy_to_user(buff, str, count))
				return -EFAULT;
		}

		if (unlikely(retries == 20)) {
			NVT_ERR("error, ret = %d\n", ret);
			return -EIO;
		}

		return ret;
	} else {
		NVT_ERR("Call error, str[0]=%d\n", str[0]);
		return -EFAULT;
	}
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash open function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
static int32_t nvt_flash_open(struct inode *inode, struct file *file)
{
	struct nvt_flash_data *dev;

	dev = kmalloc(sizeof(struct nvt_flash_data), GFP_KERNEL);
	if (dev == NULL) {
		NVT_ERR("Failed to allocate memory for nvt flash data\n");
		return -ENOMEM;
	}

	rwlock_init(&dev->lock);
	file->private_data = dev;

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash close function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_flash_close(struct inode *inode, struct file *file)
{
	struct nvt_flash_data *dev = file->private_data;

	if (dev)
		kfree(dev);

	return 0;
}

static const struct file_operations nvt_flash_fops = {
	.owner = THIS_MODULE,
	.open = nvt_flash_open,
	.release = nvt_flash_close,
	.read = nvt_flash_read,
};

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash initial function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
static int32_t nvt_flash_proc_init(void)
{
	NVT_proc_entry = proc_create(DEVICE_NAME, 0444, NULL,&nvt_flash_fops);
	if (NVT_proc_entry == NULL) {
		NVT_ERR("Failed!\n");
		return -ENOMEM;
	} else {
		NVT_LOG("Succeeded!\n");
	}

	NVT_LOG("============================================================\n");
	NVT_LOG("Create /proc/%s\n", DEVICE_NAME);
	NVT_LOG("============================================================\n");

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash deinitial function.

return:
	n.a.
*******************************************************/
static void nvt_flash_proc_deinit(void)
{
	if (NVT_proc_entry != NULL) {
		remove_proc_entry(DEVICE_NAME, NULL);
		NVT_proc_entry = NULL;
		NVT_LOG("Removed /proc/%s\n", DEVICE_NAME);
	}
}
#endif

#if WAKEUP_GESTURE
#define GESTURE_WORD_C          12
#define GESTURE_WORD_W          13
#define GESTURE_WORD_V          14
#define GESTURE_DOUBLE_CLICK    15
#define GESTURE_WORD_Z          16
#define GESTURE_WORD_M          17
#define GESTURE_WORD_O          18
#define GESTURE_WORD_e          19
#define GESTURE_WORD_S          20
#define GESTURE_SLIDE_UP        21
#define GESTURE_SLIDE_DOWN      22
#define GESTURE_SLIDE_LEFT      23
#define GESTURE_SLIDE_RIGHT     24
/* customized gesture id */
#define DATA_PROTOCOL           30

/* function page definition */
#define FUNCPAGE_GESTURE         1

/*******************************************************
Description:
	Novatek touchscreen wake up gesture key report function.

return:
	n.a.
*******************************************************/
void nvt_ts_wakeup_gesture_report(uint8_t gesture_id, uint8_t *data)
{
	uint32_t keycode = 0;
	uint8_t func_type = data[2];
	uint8_t func_id = data[3];

	/* support fw specifal data protocol */
	if ((gesture_id == DATA_PROTOCOL) && (func_type == FUNCPAGE_GESTURE)) {
		gesture_id = func_id;
	} else if (gesture_id > DATA_PROTOCOL) {
		NVT_ERR("gesture_id %d is invalid, func_type=%d, func_id=%d\n", gesture_id, func_type, func_id);
		return;
	}

	NVT_LOG("gesture_id = %d\n", gesture_id);

	switch (gesture_id) {
		case GESTURE_WORD_C:
			NVT_LOG("Gesture : Word-C.\n");
			keycode = gesture_key_array[0];
			break;
		case GESTURE_WORD_W:
			NVT_LOG("Gesture : Word-W.\n");
			keycode = gesture_key_array[1];
			break;
		case GESTURE_WORD_V:
			NVT_LOG("Gesture : Word-V.\n");
			keycode = gesture_key_array[2];
			break;
		case GESTURE_DOUBLE_CLICK:
			NVT_LOG("Gesture : Double Click.\n");
			keycode = gesture_key_array[3];
			break;
		case GESTURE_WORD_Z:
			NVT_LOG("Gesture : Word-Z.\n");
			keycode = gesture_key_array[4];
			break;
		case GESTURE_WORD_M:
			NVT_LOG("Gesture : Word-M.\n");
			keycode = gesture_key_array[5];
			break;
		case GESTURE_WORD_O:
			NVT_LOG("Gesture : Word-O.\n");
			keycode = gesture_key_array[6];
			break;
		case GESTURE_WORD_e:
			NVT_LOG("Gesture : Word-e.\n");
			keycode = gesture_key_array[7];
			break;
		case GESTURE_WORD_S:
			NVT_LOG("Gesture : Word-S.\n");
			keycode = gesture_key_array[8];
			break;
		case GESTURE_SLIDE_UP:
			NVT_LOG("Gesture : Slide UP.\n");
			keycode = gesture_key_array[9];
			break;
		case GESTURE_SLIDE_DOWN:
			NVT_LOG("Gesture : Slide DOWN.\n");
			keycode = gesture_key_array[10];
			break;
		case GESTURE_SLIDE_LEFT:
			NVT_LOG("Gesture : Slide LEFT.\n");
			keycode = gesture_key_array[11];
			break;
		case GESTURE_SLIDE_RIGHT:
			NVT_LOG("Gesture : Slide RIGHT.\n");
			keycode = gesture_key_array[12];
			break;
		default:
			break;
	}

	if (keycode > 0) {
		input_report_key(ts->input_dev, keycode, 1);
		input_sync(ts->input_dev);
		input_report_key(ts->input_dev, keycode, 0);
		input_sync(ts->input_dev);
	}
}
#endif

/*******************************************************
Description:
	Novatek touchscreen parse device tree function.

return:
	n.a.
*******************************************************/
#ifdef CONFIG_OF
static int nvt_parse_dt(struct device *dev)
{
	struct device_node *mi, *np = dev->of_node;
	struct nvt_config_info *config_info;
	int retval;
	u32 temp_val;

#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = of_get_named_gpio_flags(np, "novatek,reset-gpio", 0, &ts->reset_flags);
	NVT_LOG("novatek,reset-gpio=%d\n", ts->reset_gpio);
#endif
	ts->irq_gpio = of_get_named_gpio_flags(np, "novatek,irq-gpio", 0, &ts->irq_flags);
	NVT_LOG("novatek,irq-gpio=%d\n", ts->irq_gpio);

	ts->reset_tddi = of_get_named_gpio_flags(np, "novatek,reset-tddi", 0, NULL);
	NVT_LOG("novatek,reset-tddi=%d\n", ts->reset_tddi);

	retval = of_property_read_string(np, "novatek,vddio-reg-name", &ts->vddio_reg_name);
	if (retval < 0) {
		NVT_LOG("Unable to read VDDIO Regulator, rc:%d\n", retval);
		return retval;
	}

	retval = of_property_read_string(np, "novatek,lab-reg-name", &ts->lab_reg_name);
	if (retval < 0) {
		NVT_LOG("Unable to read LAB Regulator, rc:%d\n", retval);
		return retval;
	}

	retval = of_property_read_string(np, "novatek,ibb-reg-name", &ts->ibb_reg_name);
	if (retval < 0) {
		NVT_LOG("Unable to read IBB Regulator, rc:%d\n", retval);
		return retval;
	}

	retval = of_property_read_u32(np, "novatek,config-array-size",
				 (u32 *) &ts->config_array_size);
	if (retval) {
		NVT_LOG("Unable to get array size\n");
		return retval;
	}

	ts->config_array = devm_kzalloc(dev, ts->config_array_size *
					   sizeof(struct nvt_config_info),
					   GFP_KERNEL);

	if (!ts->config_array) {
		NVT_LOG("Unable to allocate memory\n");
		return -ENOMEM;
	}

	config_info = ts->config_array;
	for_each_child_of_node(np, mi) {
		retval = of_property_read_u32(mi,
				"novatek,tp-vendor", &temp_val);
		if (retval) {
			NVT_LOG("Unable to read tp vendor\n");
		} else {
			config_info->tp_vendor = (u8) temp_val;
			NVT_LOG("tp vendor: %u", config_info->tp_vendor);
		}

		retval = of_property_read_u32(mi,
				"novatek,hw-version", &temp_val);
		if (retval) {
			NVT_LOG("Unable to read tp hw version\n");
		} else {
			config_info->tp_hw_version = (u8) temp_val;
			NVT_LOG("tp hw version: %u", config_info->tp_hw_version);
		}

		retval = of_property_read_string(mi, "novatek,fw-name",
						 &config_info->nvt_cfg_name);

		if (retval && (retval != -EINVAL))
			NVT_LOG("Unable to read cfg name\n");
		else
			NVT_LOG("fw_name: %s", config_info->nvt_cfg_name);

		retval = of_property_read_string(mi, "novatek,fw-name-n",
						 &config_info->nvt_cfg_name_n);

		if (retval && (retval != -EINVAL))
			NVT_LOG("Unable to read cfg name\n");
		else
			NVT_LOG("fw_name: %s", config_info->nvt_cfg_name_n);

		retval = of_property_read_string(mi, "novatek,limit-name",
						 &config_info->nvt_limit_name);
		if (retval && (retval != -EINVAL))
			NVT_LOG("Unable to read limit name\n");
		else
			NVT_LOG("limit_name: %s", config_info->nvt_limit_name);

		config_info++;
	}

	return 0;
}
#else
static int nvt_parse_dt(struct device *dev)
{
#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = NVTTOUCH_RST_PIN;
#endif
	ts->irq_gpio = NVTTOUCH_INT_PIN;

	return 0;
}
#endif

static const char *nvt_get_config(struct nvt_ts_data *ts)
{
	int i;

	for (i = 0; i < ts->config_array_size; i++) {
		if ((ts->lockdown_info[0] ==
		     ts->config_array[i].tp_vendor) &&
		     (ts->lockdown_info[3] ==
		     ts->config_array[i].tp_hw_version))
			break;
	}

	if (i >= ts->config_array_size) {
		NVT_LOG("can't find right config\n");
		return BOOT_UPDATE_FIRMWARE_NAME;
	}

	if (nvt_fw)
		ts->config_array[i].nvt_cfg_set = ts->config_array[i].nvt_cfg_name_n;
	else
		ts->config_array[i].nvt_cfg_set = ts->config_array[i].nvt_cfg_name;

	NVT_LOG("Choose config %d: %s", i,
		 ts->config_array[i].nvt_cfg_set);
	ts->current_index = i;
	return ts->config_array[i].nvt_cfg_set;
}

static int nvt_get_reg(struct nvt_ts_data *ts, bool get)
{
	int retval;

	if (!get) {
		retval = 0;
		goto regulator_put;
	}

	if ((ts->vddio_reg_name != NULL) && (*ts->vddio_reg_name != 0)) {
		ts->vddio_reg = regulator_get(&ts->client->dev,
				ts->vddio_reg_name);
		if (IS_ERR(ts->vddio_reg)) {
			NVT_ERR("Failed to get power regulator\n");
			retval = PTR_ERR(ts->vddio_reg);
			goto regulator_put;
		}
	}

	if ((ts->lab_reg_name != NULL) && (*ts->lab_reg_name != 0)) {
		ts->lab_reg = regulator_get(&ts->client->dev,
				ts->lab_reg_name);
		if (IS_ERR(ts->lab_reg)) {
			NVT_ERR("Failed to get lab regulator\n");
			retval = PTR_ERR(ts->lab_reg);
			goto regulator_put;
		}
	}

	if ((ts->ibb_reg_name != NULL) && (*ts->ibb_reg_name != 0)) {
		ts->ibb_reg = regulator_get(&ts->client->dev,
				ts->ibb_reg_name);
		if (IS_ERR(ts->ibb_reg)) {
			NVT_ERR("Failed to get ibb regulator\n");
			retval = PTR_ERR(ts->ibb_reg);
			goto regulator_put;
		}
	}

	return 0;

regulator_put:
	if (ts->vddio_reg) {
		regulator_put(ts->vddio_reg);
		ts->vddio_reg = NULL;
	}
	if (ts->lab_reg) {
		regulator_put(ts->lab_reg);
		ts->lab_reg = NULL;
	}
	if (ts->ibb_reg) {
		regulator_put(ts->ibb_reg);
		ts->ibb_reg = NULL;
	}

	return retval;
}

static int nvt_enable_reg(struct nvt_ts_data *ts, bool enable)
{
	int retval;

	if (!enable) {
		retval = 0;
		goto disable_ibb_reg;
	}

	if (ts->vddio_reg) {
		retval = regulator_enable(ts->vddio_reg);
		if (retval < 0) {
			NVT_ERR("Failed to enable vddio regulator\n");
			goto exit;
		}
	}

	if (ts->lab_reg && ts->lab_reg) {
		retval = regulator_enable(ts->lab_reg);
		if (retval < 0) {
			NVT_ERR("Failed to enable lab regulator\n");
			goto disable_vddio_reg;
		}
	}

	if (ts->ibb_reg) {
		retval = regulator_enable(ts->ibb_reg);
		if (retval < 0) {
			NVT_ERR("Failed to enable ibb regulator\n");
			goto disable_lab_reg;
		}
	}

	return 0;

disable_ibb_reg:
	if (ts->ibb_reg)
		regulator_disable(ts->ibb_reg);

disable_lab_reg:
	if (ts->lab_reg)
		regulator_disable(ts->lab_reg);

disable_vddio_reg:
	if (ts->vddio_reg)
		regulator_disable(ts->vddio_reg);

exit:
	return retval;
}

/*******************************************************
Description:
	Novatek touchscreen config and request gpio

return:
	Executive outcomes. 0---succeed. not 0---failed.
*******************************************************/
static int nvt_gpio_config(struct nvt_ts_data *ts)
{
	int32_t ret = 0;

#if NVT_TOUCH_SUPPORT_HW_RST
	/* request RST-pin (Output/High) */
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = gpio_request_one(ts->reset_gpio, GPIOF_OUT_INIT_HIGH, "NVT-tp-rst");
		if (ret) {
			NVT_ERR("Failed to request NVT-tp-rst GPIO\n");
			goto err_request_reset_gpio;
		}
	}
#endif

	/* request INT-pin (Input) */
	if (gpio_is_valid(ts->irq_gpio)) {
		ret = gpio_request_one(ts->irq_gpio, GPIOF_IN, "NVT-int");
		if (ret) {
			NVT_ERR("Failed to request NVT-int GPIO\n");
			goto err_request_irq_gpio;
		}
	}

	return ret;

err_request_irq_gpio:
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_free(ts->reset_gpio);
err_request_reset_gpio:
#endif
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen deconfig gpio

return:
	n.a.
*******************************************************/
static void nvt_gpio_deconfig(struct nvt_ts_data *ts)
{
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
#if NVT_TOUCH_SUPPORT_HW_RST
	if (gpio_is_valid(ts->reset_gpio))
		gpio_free(ts->reset_gpio);
#endif
}

static uint8_t nvt_fw_recovery(uint8_t *point_data)
{
	uint8_t i = 0;
	uint8_t detected = true;

	/* check pattern */
	for (i=1 ; i<7 ; i++) {
		if (point_data[i] != 0x77) {
			detected = false;
			break;
		}
	}

	return detected;
}

#if WAKEUP_GESTURE
#define EVENT_START				0
#define EVENT_SENSITIVE_MODE_OFF		0
#define EVENT_SENSITIVE_MODE_ON			1
#define EVENT_STYLUS_MODE_OFF			2
#define EVENT_STYLUS_MODE_ON			3
#define EVENT_WAKEUP_MODE_OFF			4
#define EVENT_WAKEUP_MODE_ON			5
#define EVENT_COVER_MODE_OFF			6
#define EVENT_COVER_MODE_ON			7
#define EVENT_SLIDE_FOR_VOLUME			8
#define EVENT_DOUBLE_TAP_FOR_VOLUME		9
#define EVENT_SINGLE_TAP_FOR_VOLUME		10
#define EVENT_LONG_SINGLE_TAP_FOR_VOLUME	11
#define EVENT_PALM_OFF				12
#define EVENT_PALM_ON				13
#define EVENT_END				13
/*******************************************************
Description:
	Xiaomi modeswitch work function.

return:
	n.a.
*******************************************************/
static void mi_switch_mode_work(struct work_struct *work)
{
	struct mi_mode_switch *ms = container_of(
			work, struct mi_mode_switch, switch_mode_work
	);
	struct nvt_ts_data *data = ms->nvt_data;
	unsigned char value = ms->mode;

	if (value >= EVENT_WAKEUP_MODE_OFF &&
		value <= EVENT_WAKEUP_MODE_ON)
		data->gesture_enabled = value - EVENT_WAKEUP_MODE_OFF;
	else
		NVT_ERR("Does not support touch mode %d\n", value);

	if (ms != NULL) {
		kfree(ms);
		ms = NULL;
	}
}

/*******************************************************
Description:
	Xiaomi input events function.

return:
	n.a.
*******************************************************/
static int mi_input_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	struct nvt_ts_data *data = input_get_drvdata(dev);
	struct mi_mode_switch *ms;

	if (type == EV_SYN && code == SYN_CONFIG) {
		NVT_LOG("set input event value = %d\n", value);

		if (value >= EVENT_START && value <= EVENT_END) {
			ms = kmalloc(sizeof(struct mi_mode_switch), GFP_ATOMIC);

			if (ms != NULL) {
				ms->nvt_data = data;
				ms->mode = (unsigned char)value;
				INIT_WORK(&ms->switch_mode_work, mi_switch_mode_work);
				schedule_work(&ms->switch_mode_work);
			} else {
				NVT_ERR("Modeswitch memory allocation failed\n");
				return -ENOMEM;
			}
		} else {
			NVT_ERR("Invalid event value\n");
			return -EINVAL;
		}
	}

	return 0;
}
#endif

#define POINT_DATA_LEN 65
/*******************************************************
Description:
	Novatek touchscreen work function.

return:
	n.a.
*******************************************************/
static irqreturn_t nvt_ts_work_func(int irq, void *data)
{
	int32_t ret = -1;
	uint8_t point_data[POINT_DATA_LEN + 1] = {0};
	uint32_t position = 0;
	uint32_t input_x = 0;
	uint32_t input_y = 0;
	uint8_t input_id = 0;
#if MT_PROTOCOL_B
	uint8_t press_id[TOUCH_MAX_FINGER_NUM] = {0};
#endif /* MT_PROTOCOL_B */
	int32_t i = 0;
	int32_t finger_cnt = 0;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };

	pm_qos_update_request(&ts->pm_touch_req, 100);
	pm_qos_update_request(&ts->pm_spi_req, 100);

#if WAKEUP_GESTURE
	if (unlikely(bTouchIsAwake == 0)) {
		pm_wakeup_event(&ts->input_dev->dev, 5000);
	}
#endif

	sched_setscheduler(current, SCHED_FIFO, &param);

	mutex_lock(&ts->lock);

	ret = CTP_I2C_READ(ts->client, I2C_FW_Address, point_data, POINT_DATA_LEN + 1);
	if (unlikely(ret < 0)) {
		NVT_ERR("CTP_I2C_READ failed.(%d)\n", ret);
		goto XFER_ERROR;
	}

	if (nvt_fw_recovery(point_data)) {
		goto XFER_ERROR;
	}

#if WAKEUP_GESTURE
	if (unlikely(bTouchIsAwake == 0)) {
		input_id = (uint8_t)(point_data[1] >> 3);
		nvt_ts_wakeup_gesture_report(input_id, point_data);
		nvt_irq_enable(true);
		goto XFER_ERROR;
	}
#endif

	for (i = 0; i < ts->max_touch_num; i++) {
		position = 1 + 6 * i;
		input_id = (uint8_t) (point_data[position] >> 3);

		if ((input_id == 0) || (input_id > ts->max_touch_num))
			continue;

		if (likely(((point_data[position] & 0x07) == 0x01) || ((point_data[position] & 0x07) == 0x02))) {	//finger down (enter & moving)
			input_x = (uint32_t) (point_data[position + 1] << 4) + (uint32_t) (point_data[position + 3] >> 4);
			input_y = (uint32_t) (point_data[position + 2] << 4) + (uint32_t) (point_data[position + 3] & 0x0F);

#if MT_PROTOCOL_B
			press_id[input_id - 1] = 1;
			input_mt_slot(ts->input_dev, input_id - 1);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
#else /* MT_PROTOCOL_B */
			input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, input_id - 1);
			input_report_key(ts->input_dev, BTN_TOUCH, 1);
#endif /* MT_PROTOCOL_B */

			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, input_x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, input_y);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, TOUCH_FORCE_NUM);

#if !(MT_PROTOCOL_B) /* MT_PROTOCOL_B */
			input_mt_sync(ts->input_dev);
#endif /* MT_PROTOCOL_B */

			finger_cnt++;
		}
	}

	for (i = 0; i < ts->max_touch_num; i++) {
		if (likely(press_id[i] != 1)) {
			input_mt_slot(ts->input_dev, i);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
		}
	}

	input_report_key(ts->input_dev, BTN_TOUCH, (finger_cnt > 0));

	input_sync(ts->input_dev);

#if TOUCH_KEY_NUM > 0
	if (point_data[61] == 0xF8) {
		for (i = 0; i < ts->max_button_num; i++) {
			input_report_key(ts->input_dev, touch_key_array[i], ((point_data[62] >> i) & 0x01));
		}
	} else {
		for (i = 0; i < ts->max_button_num; i++) {
			input_report_key(ts->input_dev, touch_key_array[i], 0);
		}
	}
#endif

XFER_ERROR:

	pm_qos_update_request(&ts->pm_spi_req, PM_QOS_DEFAULT_VALUE);
	pm_qos_update_request(&ts->pm_touch_req, PM_QOS_DEFAULT_VALUE);

	input_sync(ts->input_dev);

	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

/*******************************************************
Description:
	Novatek touchscreen check and stop crc reboot loop.

return:
	n.a.
*******************************************************/
void nvt_stop_crc_reboot(void)
{
	uint8_t buf[8] = {0};
	int32_t retry = 0;

	//read dummy buffer to check CRC fail reboot is happening or not

	//---change I2C index to prevent geting 0xFF, but not 0xFC---
	nvt_set_page(I2C_BLDR_Address, 0x1F64E);

	//---read to check if buf is 0xFC which means IC is in CRC reboot ---
	buf[0] = 0x4E;
	CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 4);

	if ((buf[1] == 0xFC) ||
		((buf[1] == 0xFF) && (buf[2] == 0xFF) && (buf[3] == 0xFF))) {

		//IC is in CRC fail reboot loop, needs to be stopped!
		for (retry = 5; retry > 0; retry--) {

			//---write i2c cmds to reset idle : 1st---
			buf[0]=0x00;
			buf[1]=0xA5;
			CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

			//---write i2c cmds to reset idle : 2rd---
			buf[0]=0x00;
			buf[1]=0xA5;
			CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);
			msleep(1);

			//---clear CRC_ERR_FLAG---
			nvt_set_page(I2C_BLDR_Address, 0x3F135);

			buf[0] = 0x35;
			buf[1] = 0xA5;
			CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 2);

			//---check CRC_ERR_FLAG---
			nvt_set_page(I2C_BLDR_Address, 0x3F135);

			buf[0] = 0x35;
			buf[1] = 0x00;
			CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 2);

			if (buf[1] == 0xA5)
				break;
		}
		if (retry == 0)
			NVT_ERR("CRC auto reboot is not able to be stopped! buf[1]=0x%02X\n", buf[1]);
	}

	return;
}

/*******************************************************
Description:
	Xiaomi touchscreen work function.
return:
	n.a.
*******************************************************/
/* 2019.12.6 longcheer taocheng add (xiaomi game mode) start */
/*function description*/
#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE

static struct xiaomi_touch_interface xiaomi_touch_interfaces;

int32_t nvt_xiaomi_read_reg(uint8_t *read_buf)
{
	uint8_t buf[8] = {0};
	int32_t ret = 0;
	msleep(35);

	mutex_lock(&ts->reg_lock);

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(I2C_BLDR_Address, ts->mmap->EVENT_BUF_ADDR | 0x5C);

	// read reg_addr:0x21C5C
	buf[0] = 0x5C;
	buf[1] = 0x00;

	ret = CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

	*read_buf = ((buf[1] >> 2) & 0xFF);         // 0x21C5C 的內容會讀取放在 buf[1]
	NVT_LOG("read_buf = %d\n", *read_buf);

	mutex_unlock(&ts->reg_lock);

	return ret;

}


int32_t nvt_xiaomi_write_reg(uint8_t write_buf_high, uint8_t write_buf_low)
{

	int32_t ret = 0;
	uint8_t buf[8] = {0};

	NVT_LOG("write_buf[1] = 0x%x, write_buf[2] = 0x%x,\n", write_buf_high, write_buf_low);
	mutex_lock(&ts->reg_lock);

	//---set xdata index to EVENT BUF ADDR---
	nvt_set_page(I2C_BLDR_Address, ts->mmap->EVENT_BUF_ADDR | 0X50);

	// Write 0x7D@offset 0x50, 0x51@offset 0x51
	buf[0] = EVENT_MAP_HOST_CMD;    // write from 0x50
	buf[1] = write_buf_high;    //write into 0x50
	buf[2] = write_buf_low;		//write info 0x51

	NVT_LOG("buf[0] = 0x%x, buf[1] = 0x%x, buf[2] = 0x%x\n", buf[0], buf[1], buf[2]);
	ret = CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 3);

	mutex_unlock(&ts->reg_lock);

	return ret;

}

static void nvt_init_touchmode_data(void)
{
	int i;

	NVT_LOG("%s,ENTER\n", __func__);
	/* Touch Game Mode Switch */
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_MAX_VALUE] = 1;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][GET_CUR_VALUE] = 0;

	/* Active Mode */
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_MAX_VALUE] = 1;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Active_MODE][GET_CUR_VALUE] = 0;

	/* sensivity */
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_MAX_VALUE] = 50;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_MIN_VALUE] = 35;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][GET_CUR_VALUE] = 0;

	/*  Tolerance */
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_MAX_VALUE] = 255;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_MIN_VALUE] = 64;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][GET_CUR_VALUE] = 0;
	/* edge filter orientation*/
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_MAX_VALUE] = 3;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][GET_CUR_VALUE] = 0;

	/* edge filter area*/
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_MAX_VALUE] = 3;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_DEF_VALUE] = 2;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][GET_CUR_VALUE] = 0;

	/* Resist RF interference*/
	xiaomi_touch_interfaces.touch_mode[Touch_Resist_RF][GET_MAX_VALUE] = 1;
	xiaomi_touch_interfaces.touch_mode[Touch_Resist_RF][GET_MIN_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Resist_RF][GET_DEF_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Resist_RF][SET_CUR_VALUE] = 0;
	xiaomi_touch_interfaces.touch_mode[Touch_Resist_RF][GET_CUR_VALUE] = 0;

	for (i = 0; i < Touch_Mode_NUM; i++) {
		NVT_LOG("mode:%d, set cur:%d, get cur:%d, def:%d min:%d max:%d\n",
				i,
				xiaomi_touch_interfaces.touch_mode[i][SET_CUR_VALUE],
				xiaomi_touch_interfaces.touch_mode[i][GET_CUR_VALUE],
				xiaomi_touch_interfaces.touch_mode[i][GET_DEF_VALUE],
				xiaomi_touch_interfaces.touch_mode[i][GET_MIN_VALUE],
				xiaomi_touch_interfaces.touch_mode[i][GET_MAX_VALUE]);
	}

	return;
}


static int nvt_set_cur_value(int nvt_mode, int nvt_value)
{

	uint8_t nvt_game_value[2] = {0};
	uint8_t temp_value = 0;
	uint8_t reg_value = 0;
	uint8_t ret = 0;

	if (nvt_mode >= Touch_Mode_NUM && nvt_mode < 0) {
		NVT_ERR("%s, nvt mode is error:%d", __func__, nvt_mode);
		return -EINVAL;
	} else if (xiaomi_touch_interfaces.touch_mode[nvt_mode][SET_CUR_VALUE] >
			xiaomi_touch_interfaces.touch_mode[nvt_mode][GET_MAX_VALUE]) {

		xiaomi_touch_interfaces.touch_mode[nvt_mode][SET_CUR_VALUE] =
			xiaomi_touch_interfaces.touch_mode[nvt_mode][GET_MAX_VALUE];

	} else if (xiaomi_touch_interfaces.touch_mode[nvt_mode][SET_CUR_VALUE] <
			xiaomi_touch_interfaces.touch_mode[nvt_mode][GET_MIN_VALUE]) {

		xiaomi_touch_interfaces.touch_mode[nvt_mode][SET_CUR_VALUE] =
			xiaomi_touch_interfaces.touch_mode[nvt_mode][GET_MIN_VALUE];
	}

	xiaomi_touch_interfaces.touch_mode[nvt_mode][SET_CUR_VALUE] = nvt_value;

	NVT_LOG("%s,nvt_mode:%d,nvt_vlue:%d", __func__, nvt_mode, nvt_value);

	switch (nvt_mode) {
	case Touch_Game_Mode:
		break;
	case Touch_Active_MODE:
		break;
	case Touch_UP_THRESHOLD:
		/* 0,1,2 = default,no hover,strong hover reject*/
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_UP_THRESHOLD][SET_CUR_VALUE];
		if (temp_value >= 0 && temp_value < 35)
			reg_value = 3;
		else if (temp_value > 35 && temp_value <= 40)
			reg_value = 0;
		else if (temp_value > 40 && temp_value <= 45)
			reg_value = 1;
		else if (temp_value > 45 && temp_value <= 50)
			reg_value = 2;
		else
			reg_value = 3;

		nvt_game_value[0] = 0x71;
		nvt_game_value[1] = reg_value;
		break;
	case Touch_Tolerance:
		/* jitter 0,1,2,3,4,5 = default,weakest,weak,mediea,strong,strongest*/
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Tolerance][SET_CUR_VALUE];
		if (temp_value >= 0 && temp_value <= 80)
			reg_value = 0;
		else if (temp_value > 80 && temp_value <= 150)
			reg_value = 1;
		else if (temp_value > 150 && temp_value <= 255)
			reg_value = 2;

		nvt_game_value[0] = 0x70;
		nvt_game_value[1] = reg_value;
		break;
	case Touch_Edge_Filter:
		/* filter 0,1,2,3,4,5,6,7,8 = default,1,2,3,4,5,6,7,8 level*/
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Edge_Filter][SET_CUR_VALUE];
		reg_value = temp_value;

		nvt_game_value[0] = 0x72;
		nvt_game_value[1] = reg_value;
		break;
	case Touch_Panel_Orientation:
		/* 0,1,2,3 = 0, 90, 180,270 */
            	/*
		temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Panel_Orientation][SET_CUR_VALUE];
		if (temp_value == 0 || temp_value == 2) {
			nvt_game_value[0] = 0xBA;
			nvt_game_value[1] = 0x00;
		} else if (temp_value == 3) {
			nvt_game_value[0] = 0xBB;
			nvt_game_value[1] = 0x00;
		} else if (temp_value == 1) {
			nvt_game_value[0] = 0xBC;
			nvt_game_value[1] = 0x00;
		}
                */
		break;
	case Touch_Resist_RF:
			temp_value = xiaomi_touch_interfaces.touch_mode[Touch_Resist_RF][SET_CUR_VALUE];
			if (temp_value == 0) {
				nvt_game_value[0] = 0x76;
			} else if (temp_value == 1) {
				nvt_game_value[0] = 0x75;
			}
			nvt_game_value[1] = 0;
			break;
	default:
		/* Don't support */
		break;

	};

	NVT_LOG("0000 mode:%d, value:%d,temp_value:%d, game value:0x%x,0x%x",
			nvt_mode, nvt_value, temp_value, nvt_game_value[0], nvt_game_value[1]);

	xiaomi_touch_interfaces.touch_mode[nvt_mode][GET_CUR_VALUE] =
		xiaomi_touch_interfaces.touch_mode[nvt_mode][SET_CUR_VALUE];
	if (xiaomi_touch_interfaces.touch_mode[Touch_Game_Mode][SET_CUR_VALUE]) {

		ret = nvt_xiaomi_write_reg(nvt_game_value[0], nvt_game_value[1]);
		if (ret < 0) {
			NVT_ERR("change game mode fail");
		}
	}
	return 0;
}

static int nvt_get_mode_value(int mode, int value_type)
{
	int value = -1;

	if (mode < Touch_Mode_NUM && mode >= 0)
		value = xiaomi_touch_interfaces.touch_mode[mode][value_type];
	else
		NVT_ERR("%s, don't support\n", __func__);

	return value;
}

static int nvt_get_mode_all(int mode, int *value)
{
	if (mode < Touch_Mode_NUM && mode >= 0) {
		value[0] = xiaomi_touch_interfaces.touch_mode[mode][GET_CUR_VALUE];
		value[1] = xiaomi_touch_interfaces.touch_mode[mode][GET_DEF_VALUE];
		value[2] = xiaomi_touch_interfaces.touch_mode[mode][GET_MIN_VALUE];
		value[3] = xiaomi_touch_interfaces.touch_mode[mode][GET_MAX_VALUE];
	} else {
		NVT_ERR("%s, don't support\n",  __func__);
	}
	NVT_LOG("%s, mode:%d, value:%d:%d:%d:%d\n", __func__, mode, value[0],
			value[1], value[2], value[3]);

	return 0;
}

static int nvt_reset_mode(int mode)
{
	int i = 0;

	NVT_LOG("enter reset mode\n");

	if (mode < Touch_Mode_NUM && mode > 0) {
		xiaomi_touch_interfaces.touch_mode[mode][SET_CUR_VALUE] =
			xiaomi_touch_interfaces.touch_mode[mode][GET_DEF_VALUE];
		nvt_set_cur_value(mode, xiaomi_touch_interfaces.touch_mode[mode][SET_CUR_VALUE]);
	} else if (mode == 0) {
		for (i = Touch_Mode_NUM-1; i >= 0; i--) {
			xiaomi_touch_interfaces.touch_mode[i][SET_CUR_VALUE] =
				xiaomi_touch_interfaces.touch_mode[i][GET_DEF_VALUE];
			nvt_set_cur_value(i, xiaomi_touch_interfaces.touch_mode[mode][SET_CUR_VALUE]);
		}
	} else {
		NVT_ERR("%s, don't support\n",  __func__);
	}

	NVT_ERR("%s, mode:%d\n",  __func__, mode);

	return 0;
}

static int nvt_get_touch_super_resolution_factor(void)
{
	NVT_LOG("current super resolution factor is: %d", SUPER_RESOLUTION_FACOTR);
	return SUPER_RESOLUTION_FACOTR;
}

/* 2019.12.16 longcheer taocheng add (xiaomi game mode) end */
#endif

/*******************************************************
Description:
	Novatek touchscreen check chip version trim function.

return:
	Executive outcomes. 0---NVT IC. -1---not NVT IC.
*******************************************************/
static int8_t nvt_ts_check_chip_ver_trim(void)
{
	uint8_t buf[8] = {0};
	int32_t retry = 0;
	int32_t list = 0;
	int32_t i = 0;
	int32_t found_nvt_chip = 0;
	int32_t ret = -1;

	nvt_bootloader_reset(); // NOT in retry loop

	//---Check for 5 times---
	for (retry = 5; retry > 0; retry--) {
		nvt_sw_reset_idle();

		buf[0] = 0x00;
		buf[1] = 0x35;
		CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);
		usleep_range(10000, 11000);

		nvt_set_page(I2C_BLDR_Address, 0x1F64E);

		buf[0] = 0x4E;
		buf[1] = 0x00;
		buf[2] = 0x00;
		buf[3] = 0x00;
		buf[4] = 0x00;
		buf[5] = 0x00;
		buf[6] = 0x00;
		CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 7);
		NVT_LOG("buf[1]=0x%02X, buf[2]=0x%02X, buf[3]=0x%02X, buf[4]=0x%02X, buf[5]=0x%02X, buf[6]=0x%02X\n",
			buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

		//---Stop CRC check to prevent IC auto reboot---
		if ((buf[1] == 0xFC) ||
			((buf[1] == 0xFF) && (buf[2] == 0xFF) && (buf[3] == 0xFF))) {
			nvt_stop_crc_reboot();
			continue;
		}

		// compare read chip id on supported list
		for (list = 0; list < (sizeof(trim_id_table) / sizeof(struct nvt_ts_trim_id_table)); list++) {
			found_nvt_chip = 0;

			// compare each byte
			for (i = 0; i < NVT_ID_BYTE_MAX; i++) {
				if (trim_id_table[list].mask[i]) {
					if (buf[i + 1] != trim_id_table[list].id[i])
						break;
				}
			}

			if (i == NVT_ID_BYTE_MAX) {
				found_nvt_chip = 1;
			}

			if (found_nvt_chip) {
				NVT_LOG("This is NVT touch IC\n");
				ts->mmap = trim_id_table[list].mmap;
				ts->carrier_system = trim_id_table[list].hwinfo->carrier_system;
				ret = 0;
				goto out;
			} else {
				ts->mmap = NULL;
				ret = -1;
			}
		}

		usleep_range(10000, 11000);
	}

out:
	return ret;
}

/*******************************************************
Description:
	Xiaomi pinctrl init function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int nvt_pinctrl_init(struct nvt_ts_data *nvt_data)
{
	int retval = 0;
	/* Get pinctrl if target uses pinctrl */
	nvt_data->ts_pinctrl = devm_pinctrl_get(&nvt_data->client->dev);

	if (IS_ERR_OR_NULL(nvt_data->ts_pinctrl)) {
		retval = PTR_ERR(nvt_data->ts_pinctrl);
		NVT_ERR("Target does not use pinctrl %d\n", retval);
		goto err_pinctrl_get;
	}

	nvt_data->pinctrl_state_active
		= pinctrl_lookup_state(nvt_data->ts_pinctrl, PINCTRL_STATE_ACTIVE);

	if (IS_ERR_OR_NULL(nvt_data->pinctrl_state_active)) {
		retval = PTR_ERR(nvt_data->pinctrl_state_active);
		NVT_ERR("Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_ACTIVE, retval);
		goto err_pinctrl_lookup;
	}

	nvt_data->pinctrl_state_suspend
		= pinctrl_lookup_state(nvt_data->ts_pinctrl, PINCTRL_STATE_SUSPEND);

	if (IS_ERR_OR_NULL(nvt_data->pinctrl_state_suspend)) {
		retval = PTR_ERR(nvt_data->pinctrl_state_suspend);
		NVT_ERR("Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_SUSPEND, retval);
		goto err_pinctrl_lookup;
	}

	return 0;
err_pinctrl_lookup:
	devm_pinctrl_put(nvt_data->ts_pinctrl);
err_pinctrl_get:
	nvt_data->ts_pinctrl = NULL;
	return retval;
}

static ssize_t nvt_panel_color_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%c\n", ts->lockdown_info[2]);
}

static ssize_t nvt_panel_vendor_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%c\n", ts->lockdown_info[6]);
}

static ssize_t nvt_panel_display_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%c\n", ts->lockdown_info[1]);
}

static ssize_t nvt_panel_gesture_enable_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
        const char c = ts->gesture_enabled ? '1' : '0';
        return sprintf(buf, "%c\n", c);
}

static ssize_t nvt_panel_gesture_enable_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t count)
{
	int i;

	if (sscanf(buf, "%u", &i) == 1 && i < 2) {
		ts->gesture_enabled = i;
		return count;
	} else {
		dev_dbg(dev, "enable_dt2w write error\n");
		return -EINVAL;
	}
}

static DEVICE_ATTR(panel_vendor, (S_IRUGO), nvt_panel_vendor_show, NULL);
static DEVICE_ATTR(panel_color, (S_IRUGO), nvt_panel_color_show, NULL);
static DEVICE_ATTR(panel_display, (S_IRUGO), nvt_panel_display_show, NULL);
static DEVICE_ATTR(gesture_enable, S_IWUSR | S_IRUSR,
		nvt_panel_gesture_enable_show, nvt_panel_gesture_enable_store);

static struct attribute *nvt_attr_group[] = {
	&dev_attr_panel_vendor.attr,
	&dev_attr_panel_color.attr,
	&dev_attr_panel_display.attr,
	&dev_attr_gesture_enable.attr,
	NULL,
};

static ssize_t novatek_input_symlink(struct nvt_ts_data *ts) {
	char *driver_path;
	int ret = 0;
	if (ts->input_proc) {
		proc_remove(ts->input_proc);
		ts->input_proc = NULL;
	}
	driver_path = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!driver_path) {
		pr_err("%s: failed to allocate memory\n", __func__);
		return -ENOMEM;
	}

	sprintf(driver_path, "/sys%s",
			kobject_get_path(&ts->client->dev.kobj, GFP_KERNEL));

	pr_err("%s: driver_path=%s\n", __func__, driver_path);

	ts->input_proc = proc_symlink(PROC_SYMLINK_PATH, NULL, driver_path);

	if (!ts->input_proc) {
		ret = -ENOMEM;
	}
	kfree(driver_path);

	return ret;
}
/*
int oos_input_symlink(void) {
    static struct proc_dir_entry *tp_oos;
	int ret = 0;

	tp_oos = proc_symlink("tp_gesture", NULL, "touchpanel/gesture_enable");
	if (!tp_oos) {
		ret = -ENOMEM;
	}
    return ret;
}*/

/*******************************************************
Description:
	Novatek touchscreen driver probe function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int32_t nvt_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int32_t ret = 0;
#if ((TOUCH_KEY_NUM > 0) || WAKEUP_GESTURE)
	int32_t retry = 0;
#endif

	NVT_LOG("start\n");

	ts = kzalloc(sizeof(struct nvt_ts_data), GFP_KERNEL);
	if (ts == NULL) {
		NVT_ERR("failed to allocated memory for nvt ts data\n");
		return -ENOMEM;
	}

	ts->client = client;
	ts->input_proc = NULL;
	i2c_set_clientdata(client, ts);

	//---parse dts---
	nvt_parse_dt(&client->dev);

	//---request and config pinctrls---
	ret = nvt_pinctrl_init(ts);
	if (!ret && ts->ts_pinctrl) {
		ret = pinctrl_select_state(ts->ts_pinctrl, ts->pinctrl_state_active);

		if (ret < 0) {
			NVT_ERR("Failed to select %s pinstate %d\n",
				PINCTRL_STATE_ACTIVE, ret);
		}
	} else {
		NVT_ERR("Failed to init pinctrl\n");
	}

	//---request and config GPIOs---
	ret = nvt_gpio_config(ts);
	if (ret) {
		NVT_ERR("gpio config error!\n");
		goto err_gpio_config_failed;
	}

	//---check i2c func.---
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		NVT_ERR("i2c_check_functionality failed. (no I2C_FUNC_I2C)\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	mutex_init(&ts->lock);
	mutex_init(&ts->xbuf_lock);
	mutex_init(&ts->reg_lock);

	// need 10ms delay after POR(power on reset)
	usleep_range(10000, 11000);

	//---check chip version trim---
	ret = nvt_ts_check_chip_ver_trim();
	if (ret) {
		NVT_ERR("chip is not identified\n");
		ret = -EINVAL;
		goto err_chipvertrim_failed;
	}

	nvt_bootloader_reset();
	nvt_check_fw_reset_state(RESET_STATE_INIT);
	nvt_get_fw_info();

	//---allocate input device---
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		NVT_ERR("allocate input device failed\n");
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	ts->max_touch_num = TOUCH_MAX_FINGER_NUM;

#if TOUCH_KEY_NUM > 0
	ts->max_button_num = TOUCH_KEY_NUM;
#endif

	ts->int_trigger_type = INT_TRIGGER_TYPE;


	//---set input device info.---
	ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) ;
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	ts->input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

#if MT_PROTOCOL_B
	input_mt_init_slots(ts->input_dev, ts->max_touch_num, 0);
#endif

	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, TOUCH_FORCE_NUM, 0, 0);    //pressure = TOUCH_FORCE_NUM

#if TOUCH_MAX_FINGER_NUM > 1
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);    //area = 255

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
#if MT_PROTOCOL_B
	// no need to set ABS_MT_TRACKING_ID, input_mt_init_slots() already set it
#else
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, ts->max_touch_num, 0, 0);
#endif //MT_PROTOCOL_B
#endif //TOUCH_MAX_FINGER_NUM > 1

#if TOUCH_KEY_NUM > 0
	for (retry = 0; retry < ts->max_button_num; retry++) {
		input_set_capability(ts->input_dev, EV_KEY, touch_key_array[retry]);
	}
#endif

#if WAKEUP_GESTURE
	for (retry = 0; retry < ARRAY_SIZE(gesture_key_array); retry++) {
			input_set_capability(ts->input_dev, EV_KEY, gesture_key_array[retry]);
	}
#endif

	sprintf(ts->phys, "input/ts");
	ts->input_dev->name = NVT_TS_NAME;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_I2C;
#if WAKEUP_GESTURE
	ts->input_dev->event = mi_input_event;
	input_set_drvdata(ts->input_dev, ts);
#endif

	//---register input device---
	ret = input_register_device(ts->input_dev);
	if (ret) {
		NVT_ERR("register input device (%s) failed. ret=%d\n", ts->input_dev->name, ret);
		goto err_input_register_device_failed;
	}

	//--- request regulator---
	nvt_get_reg(ts, true);

	/* we should enable the reg for lpwg mode */
	nvt_enable_reg(ts, true);

	//---set int-pin & request irq---
	client->irq = gpio_to_irq(ts->irq_gpio);
	if (client->irq) {
		NVT_LOG("int_trigger_type=%d\n", ts->int_trigger_type);
		ts->irq_enabled = true;
		ret = request_threaded_irq(client->irq, NULL, nvt_ts_work_func,
				ts->int_trigger_type | IRQF_ONESHOT, NVT_I2C_NAME, ts);
		if (ret != 0) {
			NVT_ERR("request irq failed. ret=%d\n", ret);
			goto err_int_request_failed;
		} else {
			irq_set_affinity(ts->client->irq, cpu_perf_mask);
			nvt_irq_enable(false);
			NVT_LOG("request irq %d succeed\n", client->irq);
		}

		ts->pm_spi_req.type = PM_QOS_REQ_AFFINE_IRQ;
		ts->pm_spi_req.irq = ts->client->irq;
		pm_qos_add_request(&ts->pm_spi_req, PM_QOS_CPU_DMA_LATENCY,
			PM_QOS_DEFAULT_VALUE);

		ts->pm_touch_req.type = PM_QOS_REQ_AFFINE_IRQ;
		ts->pm_touch_req.irq = client->irq;
		pm_qos_add_request(&ts->pm_touch_req, PM_QOS_CPU_DMA_LATENCY,
				PM_QOS_DEFAULT_VALUE);
	}

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 1);
#endif

	update_hardware_info(TYPE_TOUCH, 5);

	ret = nvt_get_lockdown_info(ts->lockdown_info);

	if (ret < 0)
		NVT_ERR("can't get lockdown info");
	else
		update_hardware_info(TYPE_TP_MAKER, ts->lockdown_info[0] - 0x30);

	ts->fw_name = nvt_get_config(ts);

#if BOOT_UPDATE_FIRMWARE
	nvt_fwu_wq = alloc_workqueue("nvt_fwu_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!nvt_fwu_wq) {
		NVT_ERR("nvt_fwu_wq create workqueue failed\n");
		ret = -ENOMEM;
		goto err_create_nvt_fwu_wq_failed;
	}
	INIT_DELAYED_WORK(&ts->nvt_fwu_work, Boot_Update_Firmware);
	// please make sure boot update start after display reset(RESX) sequence
	queue_delayed_work(nvt_fwu_wq, &ts->nvt_fwu_work, msecs_to_jiffies(14000));
#endif

	/*---set device node---*/
#if NVT_TOUCH_PROC
	ret = nvt_flash_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt flash proc init failed. ret=%d\n", ret);
		goto err_flash_proc_init_failed;
	}
#endif

	ts->attrs.attrs = nvt_attr_group;
	ret = sysfs_create_group(&client->dev.kobj, &ts->attrs);
	if (ret) {
		NVT_ERR("Cannot create sysfs structure!\n");
	}

	ret = novatek_input_symlink(ts);
	if (ret < 0) {
		NVT_ERR("Failed to symlink input device!\n");
	}
/*	ret = oos_input_symlink();
	if (ret < 0) {
		NVT_ERR("Failed to symlink oos touch gesture!\n");
	}*/

#if NVT_TOUCH_EXT_PROC
	ret = nvt_extra_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt extra proc init failed. ret=%d\n", ret);
		goto err_extra_proc_init_failed;
	}
#endif

#if NVT_TOUCH_MP
	ret = nvt_mp_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt mp proc init failed. ret=%d\n", ret);
		goto err_mp_proc_init_failed;
	}
#endif

#if defined(CONFIG_DRM)
	ts->notifier.notifier_call = nvt_drm_notifier_callback;
	ret = msm_drm_register_client(&ts->notifier);
	if (ret) {
		NVT_ERR("register drm_notifier failed. ret=%d\n", ret);
		goto err_register_drm_notif_failed;
	}
#endif

	/* 2019.12.16 longcheer taocheng add (xiaomi game mode) start */
	/*function description*/
	if (ts->nvt_tp_class == NULL) {
#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
		ts->nvt_tp_class = get_xiaomi_touch_class();
#endif
		if (ts->nvt_tp_class) {
			ts->nvt_touch_dev = device_create(ts->nvt_tp_class, NULL, 0x38, ts, "tp_dev");
			if (IS_ERR(ts->nvt_touch_dev)) {
				NVT_ERR("Failed to create device !\n");
				goto err_class_create;
			}
			dev_set_drvdata(ts->nvt_touch_dev, ts);
#ifdef CONFIG_TOUCHSCREEN_XIAOMI_TOUCHFEATURE
			memset(&xiaomi_touch_interfaces, 0x00, sizeof(struct xiaomi_touch_interface));
			xiaomi_touch_interfaces.getModeValue = nvt_get_mode_value;
			xiaomi_touch_interfaces.setModeValue = nvt_set_cur_value;
			xiaomi_touch_interfaces.resetMode = nvt_reset_mode;
			xiaomi_touch_interfaces.getModeAll = nvt_get_mode_all;
			xiaomi_touch_interfaces.get_touch_super_resolution_factor = nvt_get_touch_super_resolution_factor;
			nvt_init_touchmode_data();
			xiaomitouch_register_modedata(&xiaomi_touch_interfaces);
#endif
		}
	}
	/* 2019.12.16 longcheer taocheng add (xiaomi game mode) end */

	bTouchIsAwake = 1;
	NVT_LOG("end\n");

	nvt_irq_enable(true);

	return 0;

//2019.12.16 longcheer taocheng add (xiaomi game mode)
err_class_create:
	class_destroy(ts->nvt_tp_class);
	ts->nvt_tp_class = NULL;

#if defined(CONFIG_DRM)
err_register_drm_notif_failed:
	if (msm_drm_unregister_client(&ts->notifier))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#endif
#if NVT_TOUCH_MP
nvt_mp_proc_deinit();
err_mp_proc_init_failed:
#endif
#if NVT_TOUCH_EXT_PROC
nvt_extra_proc_deinit();
err_extra_proc_init_failed:
#endif
#if NVT_TOUCH_PROC
nvt_flash_proc_deinit();
err_flash_proc_init_failed:
#endif
#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
err_create_nvt_fwu_wq_failed:
#endif
#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 0);
#endif
	free_irq(client->irq, ts);
err_int_request_failed:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
err_input_register_device_failed:
	if (ts->input_dev) {
		input_free_device(ts->input_dev);
		ts->input_dev = NULL;
	}
err_input_dev_alloc_failed:
err_chipvertrim_failed:
	mutex_destroy(&ts->xbuf_lock);
	mutex_destroy(&ts->lock);
err_check_functionality_failed:
	nvt_gpio_deconfig(ts);
err_gpio_config_failed:
	i2c_set_clientdata(client, NULL);
	if (ts) {
		kfree(ts);
		ts = NULL;
	}
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen driver release function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_remove(struct i2c_client *client)
{
	NVT_LOG("Removing driver...\n");

#if defined(CONFIG_DRM)
	if (msm_drm_unregister_client(&ts->notifier))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#endif

	pm_qos_remove_request(&ts->pm_touch_req);
	pm_qos_remove_request(&ts->pm_spi_req);

#if NVT_TOUCH_MP
	nvt_mp_proc_deinit();
#endif
#if NVT_TOUCH_EXT_PROC
	nvt_extra_proc_deinit();
#endif
#if NVT_TOUCH_PROC
	nvt_flash_proc_deinit();
#endif

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
#endif

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 0);
#endif

	nvt_get_reg(ts, false);
	nvt_irq_enable(false);
	free_irq(client->irq, ts);

	mutex_destroy(&ts->xbuf_lock);
	mutex_destroy(&ts->lock);

	nvt_gpio_deconfig(ts);

	if (ts->input_dev) {
		input_unregister_device(ts->input_dev);
		ts->input_dev = NULL;
	}

	i2c_set_clientdata(client, NULL);

	if (ts) {
		kfree(ts);
		ts = NULL;
	}

	return 0;
}

static void nvt_ts_shutdown(struct i2c_client *client)
{
	NVT_LOG("Shutdown driver...\n");

	nvt_irq_enable(false);

#if defined(CONFIG_DRM)
	if (msm_drm_unregister_client(&ts->notifier))
		NVT_ERR("Error occurred while unregistering drm_notifier.\n");
#endif

#if NVT_TOUCH_MP
	nvt_mp_proc_deinit();
#endif
#if NVT_TOUCH_EXT_PROC
	nvt_extra_proc_deinit();
#endif
#if NVT_TOUCH_PROC
	nvt_flash_proc_deinit();
#endif

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq) {
		cancel_delayed_work_sync(&ts->nvt_fwu_work);
		destroy_workqueue(nvt_fwu_wq);
		nvt_fwu_wq = NULL;
	}
#endif

#if WAKEUP_GESTURE
	device_init_wakeup(&ts->input_dev->dev, 0);
#endif
}

/*******************************************************
Description:
	Novatek touchscreen driver suspend function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_suspend(struct device *dev)
{
	uint8_t buf[4] = {0};
#if MT_PROTOCOL_B
	uint32_t i = 0;
#endif

	if (!bTouchIsAwake) {
		NVT_LOG("Touch is already suspend\n");
		return 0;
	}

#if !WAKEUP_GESTURE
	nvt_irq_enable(false);
#endif

	mutex_lock(&ts->lock);

	NVT_LOG("start\n");

	bTouchIsAwake = 0;

#if WAKEUP_GESTURE
	//---write command to enter "wakeup gesture mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x13;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

	enable_irq_wake(ts->client->irq);

	NVT_LOG("Enabled touch wakeup gesture\n");

#else // WAKEUP_GESTURE
	//---write command to enter "deep sleep mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x11;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

	if (ts->ts_pinctrl) {
		ret = pinctrl_select_state(ts->ts_pinctrl,
			ts->pinctrl_state_suspend);

		if (ret < 0) {
			NVT_ERR("Failed to select %s pinstate %d\n",
				PINCTRL_STATE_ACTIVE, ret);
		}
	} else {
		NVT_ERR("Failed to init pinctrl\n");
	}
#endif // WAKEUP_GESTURE

	mutex_unlock(&ts->lock);

	/* release all touches */
#if MT_PROTOCOL_B
	for (i = 0; i < ts->max_touch_num; i++) {
		input_mt_slot(ts->input_dev, i);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
	}
#endif
	input_report_key(ts->input_dev, BTN_TOUCH, 0);
#if !MT_PROTOCOL_B
	input_mt_sync(ts->input_dev);
#endif
	input_sync(ts->input_dev);

	msleep(50);

	NVT_LOG("end\n");

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen driver resume function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_resume(struct device *dev)
{
	if (bTouchIsAwake) {
		NVT_LOG("Touch is already resume\n");
		return 0;
	}

	mutex_lock(&ts->lock);

	NVT_LOG("start\n");

	// please make sure display reset(RESX) sequence and mipi dsi cmds sent before this
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_set_value(ts->reset_gpio, 1);
#endif

	// need to uncomment the following code for NT36672, NT36772 IC due to no boot-load when RESX/TP_RESX
	nvt_bootloader_reset();
	if (nvt_check_fw_reset_state(RESET_STATE_REK)) {
		NVT_ERR("FW is not ready! Try to bootloader reset...\n");
		nvt_bootloader_reset();
		nvt_check_fw_reset_state(RESET_STATE_REK);
	}

#if !WAKEUP_GESTURE
	nvt_irq_enable(true);

	if (ts->ts_pinctrl) {
		ret = pinctrl_select_state(ts->ts_pinctrl,
			ts->pinctrl_state_active);

		if (ret < 0) {
			NVT_ERR("Failed to select %s pinstate %d\n",
				PINCTRL_STATE_ACTIVE, ret);
		}
	} else {
		NVT_ERR("Failed to init pinctrl\n");
	}
#endif

	bTouchIsAwake = 1;

	mutex_unlock(&ts->lock);

	NVT_LOG("end\n");

	return 0;
}

#if defined(CONFIG_DRM)
static int nvt_drm_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;
	struct nvt_ts_data *ts =
		container_of(self, struct nvt_ts_data, notifier);

	if (!evdata)
		return 0;

	if (evdata->data && ts) {
		blank = evdata->data;
		if (event == MSM_DRM_EARLY_EVENT_BLANK) {
			if (*blank == MSM_DRM_BLANK_POWERDOWN) {
				NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
				irq_set_affinity(ts->client->irq, cpumask_of(0));
#if WAKEUP_GESTURE
				if (ts->gesture_enabled) {
					nvt_enable_reg(ts, true);
					drm_panel_reset_skip_enable(true);
				}
#endif
				nvt_ts_suspend(&ts->client->dev);
			}
		} else if (event == MSM_DRM_EVENT_BLANK) {
			if (*blank == MSM_DRM_BLANK_UNBLANK) {
				NVT_LOG("event=%lu, *blank=%d\n", event, *blank);
				irq_set_affinity(ts->client->irq, cpu_perf_mask);
#if WAKEUP_GESTURE
				if (ts->gesture_enabled) {
					drm_panel_reset_skip_enable(false);
					nvt_enable_reg(ts, false);
				}
#endif
				nvt_ts_resume(&ts->client->dev);
			}
		}
	}

	return 0;
}
#endif

static const struct i2c_device_id nvt_ts_id[] = {
	{ NVT_I2C_NAME, 0 },
	{ }
};

#ifdef CONFIG_OF
static struct of_device_id nvt_match_table[] = {
	{ .compatible = "novatek,NVT-ts",},
	{ },
};
#endif

static struct i2c_driver nvt_i2c_driver = {
	.probe		= nvt_ts_probe,
	.remove		= nvt_ts_remove,
	.shutdown	= nvt_ts_shutdown,
	.id_table	= nvt_ts_id,
	.driver = {
		.name	= NVT_I2C_NAME,
#ifdef CONFIG_OF
		.of_match_table = nvt_match_table,
#endif
	},
};

/*******************************************************
Description:
	Driver Install function.

return:
	Executive Outcomes. 0---succeed. not 0---failed.
********************************************************/
static int32_t __init nvt_driver_init(void)
{
	int32_t ret = 0;

	NVT_LOG("start\n");
	//---add i2c driver---
	ret = i2c_add_driver(&nvt_i2c_driver);
	if (ret) {
		NVT_ERR("failed to add i2c driver");
		goto err_driver;
	}

	NVT_LOG("finished\n");

err_driver:
	return ret;
}

/*******************************************************
Description:
	Driver uninstall function.

return:
	n.a.
********************************************************/
static void __exit nvt_driver_exit(void)
{
	i2c_del_driver(&nvt_i2c_driver);
}

//late_initcall(nvt_driver_init);
module_init(nvt_driver_init);
module_exit(nvt_driver_exit);

MODULE_DESCRIPTION("Novatek Touchscreen Driver");
MODULE_LICENSE("GPL");