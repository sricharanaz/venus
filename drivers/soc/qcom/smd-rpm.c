/*
 * Copyright (c) 2015, Sony Mobile Communications AB.
 * Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include <linux/soc/qcom/smd.h>
#include <soc/qcom/glink.h>
#include <linux/soc/qcom/smd-rpm.h>

#define RPM_REQUEST_TIMEOUT     (5 * HZ)

#define SMD_RPM		BIT(0)
#define GLINK_RPM	BIT(1)

/**
 * struct qcom_smd_rpm - state of the rpm device driver
 * @smd_channel:	reference to the smd channel
 * @ack:		completion for acks
 * @lock:		mutual exclusion around the send/complete pair
 * @ack_status:		result of the rpm request
 */
struct qcom_smd_rpm {
	struct device *dev;
	struct qcom_smd_channel *smd_channel;
	void *glink_channel;
	struct completion ack;
	struct mutex lock;
	int ack_status;
	u8 flag;
};

/**
 * struct qcom_rpm_header - header for all rpm requests and responses
 * @service_type:	identifier of the service
 * @length:		length of the payload
 */
struct qcom_rpm_header {
	__le32 service_type;
	__le32 length;
};

/**
 * struct qcom_rpm_request - request message to the rpm
 * @msg_id:	identifier of the outgoing message
 * @flags:	active/sleep state flags
 * @type:	resource type
 * @id:		resource id
 * @data_len:	length of the payload following this header
 */
struct qcom_rpm_request {
	__le32 msg_id;
	__le32 flags;
	__le32 type;
	__le32 id;
	__le32 data_len;
};

/**
 * struct qcom_rpm_message - response message from the rpm
 * @msg_type:	indicator of the type of message
 * @length:	the size of this message, including the message header
 * @msg_id:	message id
 * @message:	textual message from the rpm
 *
 * Multiple of these messages can be stacked in an rpm message.
 */
struct qcom_rpm_message {
	__le32 msg_type;
	__le32 length;
	union {
		__le32 msg_id;
		u8 message[0];
	};
};

#define RPM_SERVICE_TYPE_REQUEST	0x00716572 /* "req\0" */

#define RPM_MSG_TYPE_ERR		0x00727265 /* "err\0" */
#define RPM_MSG_TYPE_MSG_ID		0x2367736d /* "msg#" */

/**
 * qcom_rpm_smd_write - write @buf to @type:@id
 * @rpm:	rpm handle
 * @type:	resource type
 * @id:		resource identifier
 * @buf:	the data to be written
 * @count:	number of bytes in @buf
 */
int qcom_rpm_smd_write(struct qcom_smd_rpm *rpm,
		       int state,
		       u32 type, u32 id,
		       void *buf,
		       size_t count)
{
	static unsigned msg_id = 1;
	int left;
	int ret;
	struct {
		struct qcom_rpm_header hdr;
		struct qcom_rpm_request req;
		u8 payload[];
	} *pkt;
	size_t size = sizeof(*pkt) + count;

	/* SMD packets to the RPM may not exceed 256 bytes */
	if (WARN_ON(size >= 256))
		return -EINVAL;

	pkt = kmalloc(size, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	mutex_lock(&rpm->lock);

	pkt->hdr.service_type = cpu_to_le32(RPM_SERVICE_TYPE_REQUEST);
	pkt->hdr.length = cpu_to_le32(sizeof(struct qcom_rpm_request) + count);

	pkt->req.msg_id = cpu_to_le32(msg_id++);
	pkt->req.flags = cpu_to_le32(state);
	pkt->req.type = cpu_to_le32(type);
	pkt->req.id = cpu_to_le32(id);
	pkt->req.data_len = cpu_to_le32(count);
	memcpy(pkt->payload, buf, count);

	if (rpm->flag & SMD_RPM) {
		ret = qcom_smd_send(rpm->smd_channel, pkt, size);
	} else {
		ret = glink_tx(rpm->glink_channel, pkt, pkt, size,
				GLINK_TX_SINGLE_THREADED);
	}
	if (ret)
		goto out;

	left = wait_for_completion_timeout(&rpm->ack, RPM_REQUEST_TIMEOUT);
	if (!left)
		ret = -ETIMEDOUT;
	else
		ret = rpm->ack_status;

out:
	kfree(pkt);
	mutex_unlock(&rpm->lock);
	return ret;
}
EXPORT_SYMBOL(qcom_rpm_smd_write);

static int qcom_ipc_rpm_callback(struct qcom_smd_rpm *rpm,
				 const void *data,
				 size_t count)
{
	const struct qcom_rpm_header *hdr = data;
	size_t hdr_length = le32_to_cpu(hdr->length);
	const struct qcom_rpm_message *msg;
	const u8 *buf = data + sizeof(struct qcom_rpm_header);
	const u8 *end = buf + hdr_length;
	char msgbuf[32];
	int status = 0;
	u32 len, msg_length;

	if (le32_to_cpu(hdr->service_type) != RPM_SERVICE_TYPE_REQUEST ||
	    hdr_length < sizeof(struct qcom_rpm_message)) {
		dev_err(rpm->dev, "invalid request\n");
		return 0;
	}

	while (buf < end) {
		msg = (struct qcom_rpm_message *)buf;
		msg_length = le32_to_cpu(msg->length);

		switch (le32_to_cpu(msg->msg_type)) {
		case RPM_MSG_TYPE_MSG_ID:
			printk("\n qcom_ipc_rpm_callback success");
			break;
		case RPM_MSG_TYPE_ERR:
			printk("\n qcom_ipc_rpm_callback failure");
			len = min_t(u32, ALIGN(msg_length, 4), sizeof(msgbuf));
			memcpy_fromio(msgbuf, msg->message, len);
			msgbuf[len - 1] = 0;

			if (!strcmp(msgbuf, "resource does not exist"))
				status = -ENXIO;
			else
				status = -EINVAL;
			break;
		}

		buf = PTR_ALIGN(buf + 2 * sizeof(u32) + msg_length, 4);
	}

	rpm->ack_status = status;

	complete(&rpm->ack);
	return 0;
}

static int qcom_smd_rpm_callback(void *dev,
				 const void *data,
				 size_t count)
{
	struct qcom_smd_rpm *rpm = qcom_smd_get_drvdata(dev);
	return qcom_ipc_rpm_callback(rpm, data, count);
}

static int qcom_glink_rpm_callback(void *dev,
				   const void *data,
				   size_t count)
{
	struct qcom_smd_rpm *rpm = qcom_glink_get_drvdata(dev);
	return qcom_ipc_rpm_callback(rpm, data, count);
}

static int qcom_ipc_rpm_probe(struct qcom_smd_device *idev)
{
	struct qcom_smd_rpm *rpm;

	rpm = devm_kzalloc(&idev->dev, sizeof(*rpm), GFP_KERNEL);
	if (!rpm)
		return -ENOMEM;

	if (of_device_is_compatible(idev->dev.of_node, "qcom,rpm-msm8996")) {
		rpm->flag |= GLINK_RPM;
		rpm->glink_channel = idev->channel;
		qcom_glink_set_drvdata(rpm->glink_channel, rpm);
	} else { /* default behaviour */
		rpm->flag |= SMD_RPM;
		rpm->smd_channel = idev->channel;
		qcom_smd_set_drvdata(rpm->smd_channel, rpm);
	}

	mutex_init(&rpm->lock);
	init_completion(&rpm->ack);

	rpm->dev = &idev->dev;
	dev_set_drvdata(&idev->dev, rpm);

	return of_platform_populate(idev->dev.of_node, NULL, NULL, &idev->dev);
}

static void qcom_ipc_rpm_remove(struct qcom_smd_device *idev)
{
	of_platform_depopulate(&idev->dev);
}

static const struct of_device_id qcom_smd_rpm_of_match[] = {
	{ .compatible = "qcom,rpm-apq8084" },
	{ .compatible = "qcom,rpm-msm8916" },
	{ .compatible = "qcom,rpm-msm8974" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_smd_rpm_of_match);

static const struct of_device_id qcom_glink_rpm_of_match[] = {
	{ .compatible = "qcom,rpm-msm8996" },
	{}
};

static struct qcom_smd_driver qcom_smd_rpm_driver = {
	.probe = qcom_ipc_rpm_probe,
	.remove = qcom_ipc_rpm_remove,
	.callback = qcom_smd_rpm_callback,
	.driver  = {
		.name  = "qcom_ipc_rpm",
		.owner = THIS_MODULE,
		.of_match_table = qcom_smd_rpm_of_match,
	},
};

static struct qcom_smd_driver qcom_glink_rpm_driver = {
	.probe = qcom_ipc_rpm_probe,
	.remove = qcom_ipc_rpm_remove,
	.callback = qcom_glink_rpm_callback,
	.driver = {
		.name = "qcom_ipc_rpm",
		.owner = THIS_MODULE,
		.of_match_table = qcom_glink_rpm_of_match,
	},
};

static int __init qcom_smd_rpm_init(void)
{
	return qcom_smd_driver_register(&qcom_smd_rpm_driver);
}
subsys_initcall(qcom_smd_rpm_init);

static int __init qcom_glink_rpm_init(void)
{
	return qcom_glink_driver_register(&qcom_glink_rpm_driver);
}
subsys_initcall(qcom_glink_rpm_init);

static void __exit qcom_ipc_rpm_exit(void)
{
	qcom_smd_driver_unregister(&qcom_smd_rpm_driver);
	qcom_glink_driver_unregister(&qcom_glink_rpm_driver);
}
module_exit(qcom_ipc_rpm_exit);

MODULE_AUTHOR("Bjorn Andersson <bjorn.andersson@sonymobile.com>");
MODULE_DESCRIPTION("Qualcomm SMD/GLINK backed RPM driver");
MODULE_LICENSE("GPL v2");
