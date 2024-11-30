// SPDX-License-Identifier: GPL-2.0
/*
 * Focusrite Control Protocol Driver for ALSA
 *
 * FCP is the vendor-specific protocol used by Focusrite Scarlett 2nd
 * Gen, 3rd Gen, 4th Gen, Clarett USB, Clarett+, and Vocaster series.
 *
 * This driver provides a hwdep interface for userspace to send FCP
 * commands and receive notifications which is sufficient to implement
 * all user-space controls except the Level Meter (which is volatile
 * and thus requires a kernel-space control).
 *
 * Copyright (c) 2024 by Geoffrey D. Bennett <g at b4.vu>
 */

#include <linux/slab.h>
#include <linux/usb.h>

#include <sound/control.h>
#include <sound/hwdep.h>

#include <uapi/sound/fcp.h>

#include "usbaudio.h"
#include "mixer.h"
#include "helper.h"

#include "fcp.h"

struct fcp_data {
	struct usb_mixer_interface *mixer;

	struct mutex mutex;         /* serialise access to the device */
	struct completion cmd_done; /* wait for command completion */
	struct file *file;          /* hwdep file */

	/* notify waiting to send to *file */
	struct {
		wait_queue_head_t queue;
		u32               event;
		spinlock_t        lock;
	} notify;

	__u8  bInterfaceNumber;
	__u8  bEndpointAddress;
	__u16 wMaxPacketSize;
	__u8  bInterval;

	u16 seq;

	u8    num_meter_slots;
	s16  *meter_level_map;
	char *meter_labels;
	int   meter_labels_size;
	struct snd_kcontrol *meter_ctl;
};

/*** USB Interactions ***/

/* FCP Command ACK notification bit */
#define FCP_NOTIFY_ACK 1

/* Vendor-specific USB control requests */
#define FCP_USB_REQ_STEP0  0
#define FCP_USB_REQ_CMD_TX 2
#define FCP_USB_REQ_CMD_RX 3

/* Focusrite Control Protocol opcodes that the kernel side needs to
 * know about
 */
#define FCP_USB_INIT_1    0x00000000
#define FCP_USB_REBOOT    0x00000003
#define FCP_USB_GET_METER 0x00001001

#define FCP_USB_METER_LEVELS_GET_MAGIC 1

/* FCP command request/response format */
struct fcp_usb_packet {
	__le32 opcode;
	__le16 size;
	__le16 seq;
	__le32 error;
	__le32 pad;
	u8 data[];
};

static void fcp_fill_request_header(struct fcp_data *private,
				    struct fcp_usb_packet *req,
				    u32 opcode, u16 req_size)
{
	/* sequence must go up by 1 for each request */
	u16 seq = private->seq++;

	req->opcode = cpu_to_le32(opcode);
	req->size = cpu_to_le16(req_size);
	req->seq = cpu_to_le16(seq);
	req->error = 0;
	req->pad = 0;
}

static int fcp_usb_tx(struct usb_device *dev, int interface,
		      void *buf, u16 size)
{
	return snd_usb_ctl_msg(dev, usb_sndctrlpipe(dev, 0),
			FCP_USB_REQ_CMD_TX,
			USB_RECIP_INTERFACE | USB_TYPE_CLASS | USB_DIR_OUT,
			0, interface, buf, size);
}

static int fcp_usb_rx(struct usb_device *dev, int interface,
		      void *buf, u16 size)
{
	return snd_usb_ctl_msg(dev, usb_rcvctrlpipe(dev, 0),
			FCP_USB_REQ_CMD_RX,
			USB_RECIP_INTERFACE | USB_TYPE_CLASS | USB_DIR_IN,
			0, interface, buf, size);
}

/* Send an FCP command and get the response */
static int fcp_usb(struct usb_mixer_interface *mixer, u32 opcode,
		   const void *req_data, u16 req_size,
		   void *resp_data, u16 resp_size)
{
	struct fcp_data *private = mixer->private_data;
	struct usb_device *dev = mixer->chip->dev;
	struct fcp_usb_packet *req, *resp = NULL;
	size_t req_buf_size = struct_size(req, data, req_size);
	size_t resp_buf_size = struct_size(resp, data, resp_size);
	int retries = 0;
	const int max_retries = 5;
	int err;

	req = kmalloc(req_buf_size, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto done;
	}

	resp = kmalloc(resp_buf_size, GFP_KERNEL);
	if (!resp) {
		err = -ENOMEM;
		goto done;
	}

	/* build request message */
	fcp_fill_request_header(private, req, opcode, req_size);
	if (req_size)
		memcpy(req->data, req_data, req_size);

	/* send the request and retry on EPROTO */
retry:
	err = fcp_usb_tx(dev, private->bInterfaceNumber, req, req_buf_size);
	if (err == -EPROTO && ++retries <= max_retries) {
		msleep(1 << (retries - 1));
		goto retry;
	}

	if (err != req_buf_size) {
		usb_audio_err(
			mixer->chip,
			"FCP request %08x failed: %d\n",
			opcode, err);
		err = -EINVAL;
		goto done;
	}

	if (!wait_for_completion_timeout(&private->cmd_done,
					 msecs_to_jiffies(1000))) {
		usb_audio_err(
			mixer->chip,
			"FCP request %08x timed out\n",
			opcode);

		err = -ETIMEDOUT;
		goto done;
	}

	/* send a second message to get the response */

	err = fcp_usb_rx(dev, private->bInterfaceNumber, resp, resp_buf_size);

	/* validate the response */

	if (err != resp_buf_size) {

		/* ESHUTDOWN and EPROTO are valid responses to a
		 * reboot request
		 */
		if (opcode == FCP_USB_REBOOT &&
		    (err == -ESHUTDOWN || err == -EPROTO)) {
			err = 0;
			goto done;
		}

		usb_audio_err(
			mixer->chip,
			"FCP response %08x failed: %d (expected %zu)\n",
			opcode, err, resp_buf_size);
		err = -EINVAL;
		goto done;
	}

	/* opcode/seq/size should match except when initialising
	 * seq sent = 1, response = 0
	 */
	if (resp->opcode != req->opcode ||
	    (resp->seq != req->seq &&
		(le16_to_cpu(req->seq) != 1 || resp->seq != 0)) ||
	    resp_size != le16_to_cpu(resp->size) ||
	    resp->error ||
	    resp->pad) {
		usb_audio_err(
			mixer->chip,
			"FCP response invalid; "
			    "opcode tx/rx %08x/%08x seq %d/%d size %d/%d "
			    "error %d pad %d\n",
			le32_to_cpu(req->opcode), le32_to_cpu(resp->opcode),
			le16_to_cpu(req->seq), le16_to_cpu(resp->seq),
			resp_size, le16_to_cpu(resp->size),
			le32_to_cpu(resp->error),
			le32_to_cpu(resp->pad));
		err = -EINVAL;
		goto done;
	}

	if (resp_data && resp_size > 0)
		memcpy(resp_data, resp->data, resp_size);

done:
	kfree(req);
	kfree(resp);
	return err;
}

/*** Control Functions ***/

/* helper function to create a new control */
static int fcp_add_new_ctl(struct usb_mixer_interface *mixer,
			   const struct snd_kcontrol_new *ncontrol,
			   int index, int channels, const char *name,
			   struct snd_kcontrol **kctl_return)
{
	struct snd_kcontrol *kctl;
	struct usb_mixer_elem_info *elem;
	int err;

	elem = kzalloc(sizeof(*elem), GFP_KERNEL);
	if (!elem)
		return -ENOMEM;

	/* We set USB_MIXER_BESPOKEN type, so that the core USB mixer code
	 * ignores them for resume and other operations.
	 * Also, the head.id field is set to 0, as we don't use this field.
	 */
	elem->head.mixer = mixer;
	elem->control = index;
	elem->head.id = 0;
	elem->channels = channels;
	elem->val_type = USB_MIXER_BESPOKEN;

	kctl = snd_ctl_new1(ncontrol, elem);
	if (!kctl) {
		kfree(elem);
		return -ENOMEM;
	}
	kctl->private_free = snd_usb_mixer_elem_free;

	strscpy(kctl->id.name, name, sizeof(kctl->id.name));

	err = snd_usb_mixer_add_control(&elem->head, kctl);
	if (err < 0)
		return err;

	if (kctl_return)
		*kctl_return = kctl;

	return 0;
}

/*** Level Meter Control ***/

static int fcp_meter_ctl_info(struct snd_kcontrol *kctl,
			      struct snd_ctl_elem_info *uinfo)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = elem->channels;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 4095;
	uinfo->value.integer.step = 1;
	return 0;
}

static int fcp_meter_ctl_get(struct snd_kcontrol *kctl,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct fcp_data *private = mixer->private_data;
	int num_meter_slots, resp_size;
	u32 *resp;
	int i, err = 0;

	struct {
		__le16 pad;
		__le16 num_meters;
		__le32 magic;
	} __packed req;

	mutex_lock(&private->mutex);

	num_meter_slots = private->num_meter_slots;
	resp_size = num_meter_slots * sizeof(u32);

	resp = kmalloc(resp_size, GFP_KERNEL);
	if (!resp) {
		err = -ENOMEM;
		goto done;
	}

	req.pad = 0;
	req.num_meters = cpu_to_le16(num_meter_slots);
	req.magic = cpu_to_le32(FCP_USB_METER_LEVELS_GET_MAGIC);
	err = fcp_usb(mixer, FCP_USB_GET_METER,
		      &req, sizeof(req), resp, resp_size);
	if (err < 0)
		goto done;

	/* copy & translate from meter_levels[] using meter_level_map[] */
	for (i = 0; i < elem->channels; i++) {
		int idx = private->meter_level_map[i];
		int value = idx < 0 ? 0 : le32_to_cpu(resp[idx]);

		ucontrol->value.integer.value[i] = value;
	}

done:
	mutex_unlock(&private->mutex);
	kfree(resp);
	return err;
}

static int fcp_meter_tlv_callback(struct snd_kcontrol *kctl,
				  int op_flag, unsigned int size,
				  unsigned int __user *tlv)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct fcp_data *private = mixer->private_data;

	if (op_flag == SNDRV_CTL_TLV_OP_READ) {
		if (private->meter_labels_size == 0)
			return 0;

		if (size > private->meter_labels_size)
			size = private->meter_labels_size;

		if (copy_to_user(tlv, private->meter_labels, size))
			return -EFAULT;

		return size;
	}

	if (op_flag == SNDRV_CTL_TLV_OP_WRITE) {
		kfree(private->meter_labels);
		private->meter_labels = NULL;
		private->meter_labels_size = 0;

		if (size == 0)
			return 0;

		if (size > 4096)
			return -EINVAL;

		private->meter_labels = kmalloc(size, GFP_KERNEL);
		if (!private->meter_labels)
			return -ENOMEM;

		if (copy_from_user(private->meter_labels, tlv, size)) {
			kfree(private->meter_labels);
			private->meter_labels = NULL;
			return -EFAULT;
		}

		private->meter_labels_size = size;
		return 0;
	}

	return -EINVAL;
}

static const struct snd_kcontrol_new fcp_meter_ctl = {
	.iface  = SNDRV_CTL_ELEM_IFACE_PCM,
	.access = SNDRV_CTL_ELEM_ACCESS_READ |
		  SNDRV_CTL_ELEM_ACCESS_VOLATILE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READWRITE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
	.info = fcp_meter_ctl_info,
	.get  = fcp_meter_ctl_get,
	.tlv  = { .c = fcp_meter_tlv_callback },
};

/*** Callbacks ***/

static void fcp_notify(struct urb *urb)
{
	struct usb_mixer_interface *mixer = urb->context;
	struct fcp_data *private = mixer->private_data;
	int len = urb->actual_length;
	int ustatus = urb->status;
	u32 data;

	if (ustatus != 0 || len != 8)
		goto requeue;

	data = le32_to_cpu(*(__le32 *)urb->transfer_buffer);

	/* Handle command acknowledgement */
	if (data & FCP_NOTIFY_ACK) {
		complete(&private->cmd_done);
		data &= ~FCP_NOTIFY_ACK;
	}

	if (data) {
		unsigned long flags;

		spin_lock_irqsave(&private->notify.lock, flags);
		private->notify.event |= data;
		spin_unlock_irqrestore(&private->notify.lock, flags);

		wake_up_interruptible(&private->notify.queue);
	}

requeue:
	if (ustatus != -ENOENT &&
	    ustatus != -ECONNRESET &&
	    ustatus != -ESHUTDOWN) {
		urb->dev = mixer->chip->dev;
		usb_submit_urb(urb, GFP_ATOMIC);
	} else {
		complete(&private->cmd_done);
	}
}

/* Submit a URB to receive notifications from the device */
static int fcp_init_notify(struct usb_mixer_interface *mixer)
{
	struct usb_device *dev = mixer->chip->dev;
	struct fcp_data *private = mixer->private_data;
	unsigned int pipe = usb_rcvintpipe(dev, private->bEndpointAddress);
	void *transfer_buffer;
	int err;

	/* Already set up */
	if (mixer->urb)
		return 0;

	if (usb_pipe_type_check(dev, pipe))
		return -EINVAL;

	mixer->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!mixer->urb)
		return -ENOMEM;

	transfer_buffer = kmalloc(private->wMaxPacketSize, GFP_KERNEL);
	if (!transfer_buffer) {
		usb_free_urb(mixer->urb);
		mixer->urb = NULL;
		return -ENOMEM;
	}

	usb_fill_int_urb(mixer->urb, dev, pipe,
			 transfer_buffer, private->wMaxPacketSize,
			 fcp_notify, mixer, private->bInterval);

	init_completion(&private->cmd_done);

	err = usb_submit_urb(mixer->urb, GFP_KERNEL);
	if (err) {
		usb_audio_err(mixer->chip,
			      "%s: usb_submit_urb failed: %d\n",
			      __func__, err);
		kfree(transfer_buffer);
		usb_free_urb(mixer->urb);
		mixer->urb = NULL;
	}

	return err;
}

/*** hwdep interface ***/

/* FCP initialisation step 0 */
static int fcp_ioctl_init(struct usb_mixer_interface *mixer, unsigned long arg)
{
	struct fcp_step0 step0;
	struct usb_device *dev = mixer->chip->dev;
	struct fcp_data *private = mixer->private_data;
	void *resp = NULL;
	int err;

	if (usb_pipe_type_check(dev, usb_sndctrlpipe(dev, 0)))
		return -EINVAL;

	if (copy_from_user(&step0, (void __user *)arg, sizeof(step0)))
		return -EFAULT;

	if (step0.size > 255)
		return -EINVAL;

	if (step0.size > 0) {
		resp = kmalloc(step0.size, GFP_KERNEL);
		if (!resp)
			return -ENOMEM;
	}

	err = snd_usb_ctl_msg(dev, usb_rcvctrlpipe(dev, 0),
		FCP_USB_REQ_STEP0,
		USB_RECIP_INTERFACE | USB_TYPE_CLASS | USB_DIR_IN,
		0, private->bInterfaceNumber, resp, step0.size);
	if (err < 0)
		goto done;

	if (copy_to_user((void __user *)arg, resp, step0.size)) {
		err = -EFAULT;
		goto done;
	}

	err = fcp_init_notify(mixer);

	private->seq = 0;

done:
	kfree(resp);
	return err;
}

/* Execute an FCP command specified by the user */
static int fcp_ioctl_cmd(struct usb_mixer_interface *mixer, unsigned long arg)
{
	struct fcp_cmd cmd;
	int err;
	void *req = NULL;
	void *resp = NULL;

	/* get cmd & req/resp buffers */
	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	/* validate request and response sizes */
	if (cmd.req_size > 4096 || cmd.resp_size > 4096)
		return -EINVAL;

	/* allocate request buffer, copy data from user */
	if (cmd.req_size > 0) {
		req = kmalloc(cmd.req_size, GFP_KERNEL);
		if (!req) {
			err = -ENOMEM;
			goto exit;
		}
		if (copy_from_user(req, cmd.req, cmd.req_size)) {
			err = -EFAULT;
			goto exit;
		}
	}

	/* allocate response buffer */
	if (cmd.resp_size > 0) {
		resp = kmalloc(cmd.resp_size, GFP_KERNEL);
		if (!resp) {
			err = -ENOMEM;
			goto exit;
		}
	}

	/* send request, get response */
	err = fcp_usb(mixer, cmd.opcode,
		      req, cmd.req_size,
		      resp, cmd.resp_size);
	if (err < 0)
		goto exit;

	/* copy response to user */
	if (cmd.resp_size > 0)
		if (copy_to_user(cmd.resp, resp, cmd.resp_size))
			err = -EFAULT;

exit:
	kfree(req);
	kfree(resp);

	return err;
}

/* Set the Level Meter map and add the control */
static int fcp_ioctl_set_meter_map(struct usb_mixer_interface *mixer,
				   unsigned long arg)
{
	struct fcp_meter_map map;
	struct fcp_data *private = mixer->private_data;
	s16 *new_map;
	int i, err;

	if (copy_from_user(&map, (void __user *)arg, sizeof(map)))
		return -EFAULT;

	if (map.map_size > 255)
		return -EINVAL;

	new_map = kmalloc_array(map.map_size, sizeof(s16), GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	if (copy_from_user(new_map, map.map, map.map_size * sizeof(s16))) {
		err = -EFAULT;
		goto fail;
	}

	for (i = 0; i < map.map_size; i++) {
		if (new_map[i] < -1 || new_map[i] >= map.meter_slots) {
			err = -EINVAL;
			goto fail;
		}
	}

	if (!private->meter_level_map ||
	    private->meter_ctl->count != map.map_size) {
		if (private->meter_ctl) {
			snd_ctl_remove(mixer->chip->card, private->meter_ctl);
			private->meter_ctl = NULL;
		}
		err = fcp_add_new_ctl(mixer, &fcp_meter_ctl, 0, map.map_size,
				      "Level Meter", &private->meter_ctl);
		if (err < 0) {
			kfree(private->meter_level_map);
			private->meter_level_map = NULL;
			private->num_meter_slots = 0;
			private->meter_labels_size = 0;
			private->meter_labels = NULL;
			goto fail;
		}
	}

	kfree(private->meter_level_map);
	private->meter_level_map = new_map;
	private->num_meter_slots = map.meter_slots;

	return 0;

fail:
	kfree(new_map);
	return err;
}

static int fcp_hwdep_open(struct snd_hwdep *hw, struct file *file)
{
	struct usb_mixer_interface *mixer = hw->private_data;
	struct fcp_data *private = mixer->private_data;

	private->file = file;

	return 0;
}

static int fcp_hwdep_ioctl(struct snd_hwdep *hw, struct file *file,
			   unsigned int cmd, unsigned long arg)
{
	struct usb_mixer_interface *mixer = hw->private_data;
	struct fcp_data *private = mixer->private_data;
	int err;

	mutex_lock(&private->mutex);

	switch (cmd) {

	case FCP_IOCTL_PVERSION:
		err = put_user(FCP_HWDEP_VERSION,
			       (int __user *)arg) ? -EFAULT : 0;
		break;

	case FCP_IOCTL_INIT:
		err = fcp_ioctl_init(mixer, arg);
		break;

	case FCP_IOCTL_CMD:
		err = fcp_ioctl_cmd(mixer, arg);
		break;

	case FCP_IOCTL_SET_METER_MAP:
		err = fcp_ioctl_set_meter_map(mixer, arg);
		break;

	default:
		err = -ENOIOCTLCMD;
	}

	mutex_unlock(&private->mutex);

	return err;
}

static ssize_t fcp_hwdep_read(struct snd_hwdep *hw, char __user *buf,
			      ssize_t count, loff_t *offset)
{
	struct usb_mixer_interface *mixer = hw->private_data;
	struct fcp_data *private = mixer->private_data;
	unsigned long flags;
	ssize_t ret = 0;
	u32 event;

	if (count < sizeof(event))
		return -EINVAL;

	ret = wait_event_interruptible(private->notify.queue,
				       private->notify.event);
	if (ret)
		return ret;

	spin_lock_irqsave(&private->notify.lock, flags);
	event = private->notify.event;
	private->notify.event = 0;
	spin_unlock_irqrestore(&private->notify.lock, flags);

	if (copy_to_user(buf, &event, sizeof(event)))
		return -EFAULT;

	return sizeof(event);
}

static unsigned int fcp_hwdep_poll(struct snd_hwdep *hw,
				   struct file *file,
				   poll_table *wait)
{
	struct usb_mixer_interface *mixer = hw->private_data;
	struct fcp_data *private = mixer->private_data;
	unsigned int mask = 0;

	poll_wait(file, &private->notify.queue, wait);

	if (private->notify.event)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static int fcp_hwdep_release(struct snd_hwdep *hw, struct file *file)
{
	struct usb_mixer_interface *mixer = hw->private_data;
	struct fcp_data *private = mixer->private_data;

	if (!private)
		return 0;

	private->file = NULL;

	return 0;
}

static int fcp_hwdep_init(struct usb_mixer_interface *mixer)
{
	struct snd_hwdep *hw;
	int err;

	err = snd_hwdep_new(mixer->chip->card, "Focusrite Control", 0, &hw);
	if (err < 0)
		return err;

	hw->private_data = mixer;
	hw->exclusive = 1;
	hw->ops.open = fcp_hwdep_open;
	hw->ops.ioctl = fcp_hwdep_ioctl;
	hw->ops.read = fcp_hwdep_read;
	hw->ops.poll = fcp_hwdep_poll;
	hw->ops.release = fcp_hwdep_release;

	return 0;
}

/*** Cleanup ***/

static void fcp_private_free(struct usb_mixer_interface *mixer)
{
	struct fcp_data *private = mixer->private_data;

	if (mixer->urb) {
		usb_kill_urb(mixer->urb);
		kfree(mixer->urb->transfer_buffer);
		usb_free_urb(mixer->urb);
		mixer->urb = NULL;
	}

	kfree(private->meter_level_map);
	kfree(private->meter_labels);
	kfree(private);
	mixer->private_data = NULL;
}

/*** Initialisation ***/

static int fcp_init_private(struct usb_mixer_interface *mixer)
{
	struct fcp_data *private =
		kzalloc(sizeof(struct fcp_data), GFP_KERNEL);

	if (!private)
		return -ENOMEM;

	mutex_init(&private->mutex);
	init_waitqueue_head(&private->notify.queue);
	spin_lock_init(&private->notify.lock);

	mixer->private_data = private;
	mixer->private_free = fcp_private_free;

	private->mixer = mixer;

	return 0;
}

/* Look through the interface descriptors for the Focusrite Control
 * interface (bInterfaceClass = 255 Vendor Specific Class) and set
 * bInterfaceNumber, bEndpointAddress, wMaxPacketSize, and bInterval
 * in private
 */
static int fcp_find_fc_interface(struct usb_mixer_interface *mixer)
{
	struct snd_usb_audio *chip = mixer->chip;
	struct fcp_data *private = mixer->private_data;
	struct usb_host_config *config = chip->dev->actconfig;
	int i;

	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		struct usb_interface *intf = config->interface[i];
		struct usb_interface_descriptor *desc =
			&intf->altsetting[0].desc;
		struct usb_endpoint_descriptor *epd;

		if (desc->bInterfaceClass != 255)
			continue;

		epd = get_endpoint(intf->altsetting, 0);
		private->bInterfaceNumber = desc->bInterfaceNumber;
		private->bEndpointAddress = epd->bEndpointAddress &
			USB_ENDPOINT_NUMBER_MASK;
		private->wMaxPacketSize = le16_to_cpu(epd->wMaxPacketSize);
		private->bInterval = epd->bInterval;
		return 0;
	}

	usb_audio_err(chip, "Focusrite vendor-specific interface not found\n");
	return -EINVAL;
}

int snd_fcp_init(struct usb_mixer_interface *mixer)
{
	struct snd_usb_audio *chip = mixer->chip;
	int err;

	/* only use UAC_VERSION_2 */
	if (!mixer->protocol)
		return 0;

	err = fcp_init_private(mixer);
	if (err < 0)
		return err;

	err = fcp_find_fc_interface(mixer);
	if (err < 0)
		return err;

	err = fcp_hwdep_init(mixer);
	if (err < 0)
		return err;

	usb_audio_info(chip,
		"Focusrite Control Protocol Driver ready (pid=0x%04x); "
		"report any issues to "
		"https://github.com/geoffreybennett/fcp-control/issues",
		USB_ID_PRODUCT(chip->usb_id));

	return err;
}
