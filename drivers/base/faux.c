// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (c) 2025 The Linux Foundation
 *
 * A "simple" faux bus that allows devices to be created and added
 * automatically to it.  This is to be used whenever you need to create a
 * device that is not associated with any "real" system resources, and do
 * not want to have to deal with a bus/driver binding logic.  It is
 * intended to be very simple, with only a create and a destroy function
 * available.
 */
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/container_of.h>
#include <linux/device/faux.h>
#include "base.h"

/*
 * Internal wrapper structure so we can hold a pointer to the
 * faux_device_ops for this device.
 */
struct faux_object {
	struct faux_device faux_dev;
	const struct faux_device_ops *faux_ops;
	const struct attribute_group **groups;
};
#define to_faux_object(dev) container_of_const(dev, struct faux_object, faux_dev.dev)

static struct device *faux_bus_root;

static int faux_match(struct device *dev, const struct device_driver *drv)
{
	/* Match always succeeds, we only have one driver */
	return 1;
}

static int faux_probe(struct device *dev)
{
	struct faux_object *faux_obj = to_faux_object(dev);
	struct faux_device *faux_dev = &faux_obj->faux_dev;
	const struct faux_device_ops *faux_ops = faux_obj->faux_ops;
	int ret;

	if (faux_ops && faux_ops->probe) {
		ret = faux_ops->probe(faux_dev);
		if (ret)
			return ret;
	}

	/*
	 * Add groups after the probe succeeds to ensure resources are
	 * initialized correctly
	 */
	ret = device_add_groups(dev, faux_obj->groups);
	if (ret && faux_ops && faux_ops->remove)
		faux_ops->remove(faux_dev);

	return ret;
}

static void faux_remove(struct device *dev)
{
	struct faux_object *faux_obj = to_faux_object(dev);
	struct faux_device *faux_dev = &faux_obj->faux_dev;
	const struct faux_device_ops *faux_ops = faux_obj->faux_ops;

	device_remove_groups(dev, faux_obj->groups);

	if (faux_ops && faux_ops->remove)
		faux_ops->remove(faux_dev);
}

static const struct bus_type faux_bus_type = {
	.name		= "faux",
	.match		= faux_match,
	.probe		= faux_probe,
	.remove		= faux_remove,
};

static struct device_driver faux_driver = {
	.name		= "faux_driver",
	.bus		= &faux_bus_type,
	.probe_type	= PROBE_FORCE_SYNCHRONOUS,
	.suppress_bind_attrs = true,
};

static void faux_device_release(struct device *dev)
{
	struct faux_object *faux_obj = to_faux_object(dev);

	kfree(faux_obj);
}

/**
 * faux_device_create_with_groups - Create and register with the driver
 *		core a faux device and populate the device with an initial
 *		set of sysfs attributes.
 * @name:	The name of the device we are adding, must be unique for
 *		all faux devices.
 * @parent:	Pointer to a potential parent struct device.  If set to
 *		NULL, the device will be created in the "root" of the faux
 *		device tree in sysfs.
 * @faux_ops:	struct faux_device_ops that the new device will call back
 *		into, can be NULL.
 * @groups:	The set of sysfs attributes that will be created for this
 *		device when it is registered with the driver core.
 *
 * Create a new faux device and register it in the driver core properly.
 * If present, callbacks in @faux_ops will be called with the device that
 * for the caller to do something with at the proper time given the
 * device's lifecycle.
 *
 * Note, when this function is called, the functions specified in struct
 * faux_ops can be called before the function returns, so be prepared for
 * everything to be properly initialized before that point in time.  If the
 * probe callback (if one is present) does NOT succeed, the creation of the
 * device will fail and NULL will be returned.
 *
 * Return:
 * * NULL if an error happened with creating the device
 * * pointer to a valid struct faux_device that is registered with sysfs
 */
struct faux_device *faux_device_create_with_groups(const char *name,
						   struct device *parent,
						   const struct faux_device_ops *faux_ops,
						   const struct attribute_group **groups)
{
	struct faux_object *faux_obj;
	struct faux_device *faux_dev;
	struct device *dev;
	int ret;

	if (!faux_bus_root)
		return NULL;

	faux_obj = kzalloc_obj(*faux_obj);
	if (!faux_obj)
		return NULL;

	/* Save off the callbacks and groups so we can use them in the future */
	faux_obj->faux_ops = faux_ops;
	faux_obj->groups = groups;

	/* Initialize the device portion and register it with the driver core */
	faux_dev = &faux_obj->faux_dev;
	dev = &faux_dev->dev;

	device_initialize(dev);
	dev->release = faux_device_release;
	if (parent)
		dev->parent = parent;
	else
		dev->parent = faux_bus_root;
	dev->bus = &faux_bus_type;
	dev_set_name(dev, "%s", name);
	device_set_pm_not_required(dev);

	ret = device_add(dev);
	if (ret) {
		pr_err("%s: device_add for faux device '%s' failed with %d\n",
		       __func__, name, ret);
		put_device(dev);
		return NULL;
	}

	/*
	 * Verify that we did bind the driver to the device (i.e. probe worked),
	 * if not, let's fail the creation as trying to guess if probe was
	 * successful is almost impossible to determine by the caller.
	 */
	if (!dev->driver) {
		dev_dbg(dev, "probe did not succeed, tearing down the device\n");
		faux_device_destroy(faux_dev);
		faux_dev = NULL;
	}

	return faux_dev;
}
EXPORT_SYMBOL_GPL(faux_device_create_with_groups);

/**
 * faux_device_create - create and register with the driver core a faux device
 * @name:	The name of the device we are adding, must be unique for all
 *		faux devices.
 * @parent:	Pointer to a potential parent struct device.  If set to
 *		NULL, the device will be created in the "root" of the faux
 *		device tree in sysfs.
 * @faux_ops:	struct faux_device_ops that the new device will call back
 *		into, can be NULL.
 *
 * Create a new faux device and register it in the driver core properly.
 * If present, callbacks in @faux_ops will be called with the device that
 * for the caller to do something with at the proper time given the
 * device's lifecycle.
 *
 * Note, when this function is called, the functions specified in struct
 * faux_ops can be called before the function returns, so be prepared for
 * everything to be properly initialized before that point in time.
 *
 * Return:
 * * NULL if an error happened with creating the device
 * * pointer to a valid struct faux_device that is registered with sysfs
 */
struct faux_device *faux_device_create(const char *name,
				       struct device *parent,
				       const struct faux_device_ops *faux_ops)
{
	return faux_device_create_with_groups(name, parent, faux_ops, NULL);
}
EXPORT_SYMBOL_GPL(faux_device_create);

/**
 * faux_device_destroy - destroy a faux device
 * @faux_dev:	faux device to destroy
 *
 * Unregisters and cleans up a device that was created with a call to
 * faux_device_create()
 */
void faux_device_destroy(struct faux_device *faux_dev)
{
	struct device *dev = &faux_dev->dev;

	if (!faux_dev)
		return;

	device_del(dev);

	/* The final put_device() will clean up the memory we allocated for this device. */
	put_device(dev);
}
EXPORT_SYMBOL_GPL(faux_device_destroy);

/*
 * faux_bus_init() - 构造 faux 设备的根对象、总线类型和配套驱动。
 *
 * 【宏观位置】driver_init() 在 devices/buses/classes 核心根发布后调用。无入参，
 * 早期进程上下文可睡眠，入口不持锁。
 *
 * 【变量与 ownership】root 是 root_device_register() 返回的持有引用，只有全部
 * 注册成功后才发布到全局 faux_bus_root；ret 贯穿 bus/driver 注册错误。先注册
 * 根设备，再注册 bus_type，最后注册该总线的内建 faux_driver，保证任何对外
 * 可见的后续层都能找到完整父层。
 *
 * 返回：0 表示三层均已注册且全局变量接管 root 的长期引用；负 errno 可能是
 * 编码在 ERR_PTR(root) 中的根设备错误，也可能来自 bus_register()/
 * driver_register()。失败路径按 driver→bus→root 的反向取得顺序撤销，入口
 * 状态不变。
 */
int __init faux_bus_init(void)
{
	struct device *root;
	int ret;

	/* 阶段 1：创建 /sys/devices/faux 根设备；错误以 ERR_PTR 编码返回。 */
	root = root_device_register("faux");
	if (IS_ERR(root))
		return PTR_ERR(root);

	/* 阶段 2：发布 bus_type；此后设备和驱动可以开始引用该总线。 */
	ret = bus_register(&faux_bus_type);
	if (ret)
		goto err_deregister_root;

	/* 阶段 3：注册总线配套驱动；成功前 root 仍只保存在局部变量中。 */
	ret = driver_register(&faux_driver);
	if (ret)
		goto err_deregister_bus;

	/* 最终提交：全局指针接管 root，运行期 faux_device_create() 可以使用它。 */
	faux_bus_root = root;

	return 0;

	/* driver 注册失败时，先撤销已经可见的 bus，再释放 root 设备引用。 */
err_deregister_bus:
	bus_unregister(&faux_bus_type);
err_deregister_root:
	root_device_unregister(root);

	return ret;
}
