// SPDX-License-Identifier: GPL-2.0+
/*
 * Provides user-space access to the SSAM EC via the /dev/surface/aggregator
 * misc device. Intended for debugging and development.
 *
 * Copyright (C) 2020 Maximilian Luz <luzmaximilian@gmail.com>
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../../include/uapi/linux/surface_aggregator/cdev.h"
#include "../../include/linux/surface_aggregator/controller.h"

#define SSAM_CDEV_DEVICE_NAME	"surface_aggregator_cdev"

struct ssam_cdev {
	struct ssam_controller *ctrl;
	struct miscdevice mdev;
};

static int ssam_cdev_device_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;

	filp->private_data = container_of(mdev, struct ssam_cdev, mdev);
	return nonseekable_open(inode, filp);
}

static long ssam_cdev_request(struct file *file, unsigned long arg)
{
	struct ssam_cdev *cdev = file->private_data;
	struct ssam_cdev_request __user *r;
	struct ssam_cdev_request rqst;
	struct ssam_request spec;
	struct ssam_response rsp;
	const void __user *plddata;
	void __user *rspdata;
	int status = 0, ret = 0, tmp;

	r = (struct ssam_cdev_request __user *)arg;
	ret = copy_struct_from_user(&rqst, sizeof(rqst), r, sizeof(*r));
	if (ret)
		goto out;

	plddata = u64_to_user_ptr(rqst.payload.data);
	rspdata = u64_to_user_ptr(rqst.response.data);

	// setup basic request fields
	spec.target_category = rqst.target_category;
	spec.target_id = rqst.target_id;
	spec.command_id = rqst.command_id;
	spec.instance_id = rqst.instance_id;
	spec.flags = rqst.flags;
	spec.length = rqst.payload.length;
	spec.payload = NULL;

	rsp.capacity = rqst.response.length;
	rsp.length = 0;
	rsp.pointer = NULL;

	// get request payload from user-space
	if (spec.length) {
		if (!plddata) {
			ret = -EINVAL;
			goto out;
		}

		spec.payload = kzalloc(spec.length, GFP_KERNEL);
		if (!spec.payload) {
			status = -ENOMEM;
			ret = -EFAULT;
			goto out;
		}

		if (copy_from_user((void *)spec.payload, plddata, spec.length)) {
			ret = -EFAULT;
			goto out;
		}
	}

	// allocate response buffer
	if (rsp.capacity) {
		if (!rspdata) {
			ret = -EINVAL;
			goto out;
		}

		rsp.pointer = kzalloc(rsp.capacity, GFP_KERNEL);
		if (!rsp.pointer) {
			status = -ENOMEM;
			ret = -EFAULT;
			goto out;
		}
	}

	// perform request
	status = ssam_request_sync(cdev->ctrl, &spec, &rsp);
	if (status)
		goto out;

	// copy response to user-space
	if (rsp.length && copy_to_user(rspdata, rsp.pointer, rsp.length))
		ret = -EFAULT;

out:
	// always try to set response-length and status
	tmp = put_user(rsp.length, &r->response.length);
	if (tmp)
		ret = tmp;

	tmp = put_user(status, &r->status);
	if (tmp)
		ret = tmp;

	// cleanup
	kfree(spec.payload);
	kfree(rsp.pointer);

	return ret;
}

static long ssam_cdev_device_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	switch (cmd) {
	case SSAM_CDEV_REQUEST:
		return ssam_cdev_request(file, arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations ssam_controller_fops = {
	.owner          = THIS_MODULE,
	.open           = ssam_cdev_device_open,
	.unlocked_ioctl = ssam_cdev_device_ioctl,
	.compat_ioctl   = ssam_cdev_device_ioctl,
	.llseek         = noop_llseek,
};

static int ssam_dbg_device_probe(struct platform_device *pdev)
{
	struct ssam_controller *ctrl;
	struct ssam_cdev *cdev;
	int status;

	status = ssam_client_bind(&pdev->dev, &ctrl);
	if (status)
		return status == -ENXIO ? -EPROBE_DEFER : status;

	cdev = devm_kzalloc(&pdev->dev, sizeof(*cdev), GFP_KERNEL);
	if (!cdev)
		return -ENOMEM;

	cdev->ctrl = ctrl;
	cdev->mdev.parent   = &pdev->dev;
	cdev->mdev.minor    = MISC_DYNAMIC_MINOR;
	cdev->mdev.name     = "surface_aggregator";
	cdev->mdev.nodename = "surface/aggregator";
	cdev->mdev.fops     = &ssam_controller_fops;

	platform_set_drvdata(pdev, cdev);
	return misc_register(&cdev->mdev);
}

static int ssam_dbg_device_remove(struct platform_device *pdev)
{
	struct ssam_cdev *cdev = platform_get_drvdata(pdev);

	misc_deregister(&cdev->mdev);
	return 0;
}

static struct platform_device *ssam_cdev_device;

static struct platform_driver ssam_cdev_driver = {
	.probe = ssam_dbg_device_probe,
	.remove = ssam_dbg_device_remove,
	.driver = {
		.name = SSAM_CDEV_DEVICE_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static int __init ssam_debug_init(void)
{
	int status;

	ssam_cdev_device = platform_device_alloc(SSAM_CDEV_DEVICE_NAME,
						 PLATFORM_DEVID_NONE);
	if (!ssam_cdev_device)
		return -ENOMEM;

	status = platform_device_add(ssam_cdev_device);
	if (status)
		goto err_device;

	status = platform_driver_register(&ssam_cdev_driver);
	if (status)
		goto err_driver;

	return 0;

err_driver:
	platform_device_del(ssam_cdev_device);
err_device:
	platform_device_put(ssam_cdev_device);
	return status;
}
module_init(ssam_debug_init);

static void __exit ssam_debug_exit(void)
{
	platform_driver_unregister(&ssam_cdev_driver);
	platform_device_unregister(ssam_cdev_device);
}
module_exit(ssam_debug_exit);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("User-space interface for Surface System Aggregator Module");
MODULE_LICENSE("GPL");
