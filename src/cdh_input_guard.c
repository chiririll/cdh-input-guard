// SPDX-License-Identifier: GPL-2.0
/*
 * cdh_input_guard - keep virtual input devices away from the host.
 *
 * A virtual input device is one with no parent device (what uinput creates)
 * or, unless uhid=0, one below the uhid misc device. The module registers two
 * kinds of input handler for them:
 *
 *  1. Detach handlers. connect() disconnects every other handler from the
 *     device except the ones named in "keep" (default evdev, joydev), so keys
 *     injected by a container cannot reach the VT console, SysRq, rfkill,
 *     /dev/input/mice and so on. connect() then returns -ENODEV: no handle,
 *     no state.
 *
 *     Handlers attach in registration order, and connect() can only detach
 *     handles that already exist. A handler registered after us (sysrq when
 *     kernel.sysrq goes from 0 to non-zero, a handler module loaded later)
 *     would attach behind our back. So every sweep_ms a work item registers
 *     the second detach handler and then unregisters the first one, and the
 *     next time the other way round. Registering puts the handler at the tail
 *     of the handler list and runs connect() for every existing device.
 *     One of the two is always registered, so there is no gap.
 *
 *  2. The owner handler. connect() records the cgroup of the process that
 *     created the device (connect() runs inside the UI_DEV_CREATE ioctl) as
 *     the root-only sysfs attribute "cdh_owner_cgroup" on the input device,
 *     then sends a "change" uevent on the input device and its children so
 *     udev can act on it. Its handle is never opened and is never
 *     re-registered, so the owner survives the sweeps.
 *
 * Deliberately has no MODULE_DEVICE_TABLE: that would make udev autoload the
 * module for every input device.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cgroup.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/kernfs.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define GUARD_MAX_KEEP		8
#define GUARD_OWNER_LEN		512
#define GUARD_SWEEP_MIN_MS	100

static char *keep[GUARD_MAX_KEEP] = { "evdev", "joydev" };
static unsigned int keep_count = 2;
module_param_array(keep, charp, &keep_count, 0444);
MODULE_PARM_DESC(keep,
		 "Input handlers that stay attached to virtual devices; all others are disconnected (default: evdev,joydev)");

static bool uhid = true;
module_param(uhid, bool, 0444);
MODULE_PARM_DESC(uhid, "Also guard input devices created through uhid (default: Y)");

static unsigned int sweep_ms = 1000;
module_param(sweep_ms, uint, 0444);
MODULE_PARM_DESC(sweep_ms,
		 "Interval for disconnecting handlers that registered after this module, 0 to disable (default: 1000)");

struct guard_handle {
	struct input_handle handle;
	bool has_owner;
	char owner[GUARD_OWNER_LEN];
};

static struct input_handler guard_owner_handler;

/*
 * The task running input_register_handler() from module init. connect() calls
 * made by it are for devices that existed before the module loaded, whose
 * creator is unknown.
 */
static struct task_struct *guard_loader;

static ssize_t cdh_owner_cgroup_show(struct device *d,
				     struct device_attribute *attr, char *buf)
{
	struct input_dev *dev = to_input_dev(d);
	struct input_handle *handle;
	ssize_t ret = -ENODEV;

	/*
	 * dev->h_list is an RCU list. The attribute is removed in
	 * guard_owner_disconnect() before our handle is unregistered and
	 * freed, and device_remove_file() waits for running show() calls.
	 */
	rcu_read_lock();
	list_for_each_entry_rcu(handle, &dev->h_list, d_node) {
		if (handle->handler == &guard_owner_handler) {
			struct guard_handle *gh =
				container_of(handle, struct guard_handle, handle);

			ret = sysfs_emit(buf, "%s\n", gh->owner);
			break;
		}
	}
	rcu_read_unlock();

	return ret;
}
/* Root only: the path names the container and its internal cgroup layout. */
static DEVICE_ATTR_ADMIN_RO(cdh_owner_cgroup);

/*
 * No parent: uinput (and the odd platform driver; guarding those too is the
 * safe side). Topmost ancestor is the uhid misc device: uhid.
 */
static bool guard_match(struct input_handler *handler, struct input_dev *dev)
{
	struct device *top = &dev->dev;

	if (!top->parent)
		return true;
	if (!uhid)
		return false;

	while (top->parent)
		top = top->parent;

	return top->class && !strcmp(top->class->name, "misc") &&
	       !strcmp(dev_name(top), "uhid");
}

static bool guard_should_keep(const struct input_handler *handler)
{
	unsigned int i;

	if (handler == &guard_owner_handler || !handler->disconnect)
		return true;
	if (!handler->name)
		return false;

	for (i = 0; i < keep_count; i++)
		if (keep[i] && !strcmp(handler->name, keep[i]))
			return true;

	return false;
}

/*
 * connect() is called with input_mutex held and dev->mutex free. Every change
 * to dev->h_list goes through input_register_handle()/input_unregister_handle(),
 * which are only called from connect()/disconnect() under input_mutex, so the
 * list cannot change under us except by the disconnect() we call. The _safe
 * iterator handles that one removal. This is the same call, in the same
 * locking context, that __input_unregister_device() makes for every handle.
 */
static int guard_detach_connect(struct input_handler *handler,
				struct input_dev *dev,
				const struct input_device_id *id)
{
	struct input_handle *handle, *next;

	list_for_each_entry_safe(handle, next, &dev->h_list, d_node) {
		struct input_handler *other = handle->handler;

		if (guard_should_keep(other))
			continue;

		/* dev->name is chosen by the container: debug level only. */
		pr_debug("%s (%s): disconnecting handler %s\n",
			 dev_name(&dev->dev), dev->name ?: "unnamed",
			 other->name ?: "unnamed");
		other->disconnect(handle);
	}

	return -ENODEV;
}

/* Path of current's cgroup v2 cgroup, relative to the hierarchy root. */
static bool guard_record_owner(struct guard_handle *gh)
{
	struct cgroup *cgrp;
	int len;

	rcu_read_lock();
	cgrp = task_dfl_cgroup(current);
	len = kernfs_path(cgrp->kn, gh->owner, sizeof(gh->owner));
	rcu_read_unlock();

	if (len < 0 || len >= (int)sizeof(gh->owner)) {
		pr_warn_ratelimited("cannot record owner cgroup (%d)\n", len);
		gh->owner[0] = '\0';
		return false;
	}

	return true;
}

static int guard_notify_child(struct device *child, void *data)
{
	kobject_uevent(&child->kobj, KOBJ_CHANGE);
	return 0;
}

/*
 * The "add" uevents of inputN and of evdev's eventN were sent before the
 * attribute existed, so announce it with "change" on both.
 */
static void guard_notify(struct input_dev *dev)
{
	kobject_uevent(&dev->dev.kobj, KOBJ_CHANGE);
	device_for_each_child(&dev->dev, NULL, guard_notify_child);
}

static int guard_owner_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct guard_handle *gh;
	int error;

	gh = kzalloc(sizeof(*gh), GFP_KERNEL);
	if (!gh)
		return -ENOMEM;

	gh->handle.dev = dev;
	gh->handle.handler = handler;
	gh->handle.name = KBUILD_MODNAME;

	/*
	 * No owner for devices that existed before the module loaded, nor for
	 * devices registered from a kernel thread (uhid adds its device from
	 * a workqueue). Without the attribute nobody is given the device.
	 */
	if (current != READ_ONCE(guard_loader) && !(current->flags & PF_KTHREAD))
		gh->has_owner = guard_record_owner(gh);

	/*
	 * The handle is never opened: it receives no events and exists only
	 * so that disconnect() is called when the device goes away.
	 */
	error = input_register_handle(&gh->handle);
	if (error) {
		kfree(gh);
		return error;
	}

	if (gh->has_owner) {
		error = device_create_file(&dev->dev, &dev_attr_cdh_owner_cgroup);
		if (error) {
			pr_warn_ratelimited("%s: cannot create owner attribute (%d)\n",
					    dev_name(&dev->dev), error);
			gh->has_owner = false;
		} else {
			pr_debug("%s (%s): owner %s\n", dev_name(&dev->dev),
				 dev->name ?: "unnamed", gh->owner);
			guard_notify(dev);
		}
	}

	return 0;
}

static void guard_owner_disconnect(struct input_handle *handle)
{
	struct guard_handle *gh = container_of(handle, struct guard_handle, handle);

	if (gh->has_owner)
		device_remove_file(&handle->dev->dev, &dev_attr_cdh_owner_cgroup);

	input_unregister_handle(handle);
	kfree(gh);
}

/*
 * Match everything; guard_match() narrows it to virtual devices. The input
 * core sets EV_SYN on every device, and this is how evdev matches all of them.
 * An entry with flags == 0 ends the table: since 6.17 that is true even when
 * driver_info is set, so { .driver_info = 1 } matches nothing.
 */
static const struct input_device_id guard_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_SYN) },
	},
	{ },
};

static struct input_handler guard_owner_handler = {
	.match		= guard_match,
	.connect	= guard_owner_connect,
	.disconnect	= guard_owner_disconnect,
	.name		= KBUILD_MODNAME,
	.id_table	= guard_ids,
};

/* No disconnect(): connect() never registers a handle. */
static struct input_handler guard_detach_handlers[2] = {
	[0 ... 1] = {
		.match		= guard_match,
		.connect	= guard_detach_connect,
		.name		= KBUILD_MODNAME "_detach",
		.id_table	= guard_ids,
	},
};

/* Index of the registered detach handler. Only init, exit and the work use it. */
static unsigned int guard_active;

static void guard_sweep(struct work_struct *work);
static DECLARE_DELAYED_WORK(guard_sweep_work, guard_sweep);

static void guard_schedule_sweep(void)
{
	schedule_delayed_work(&guard_sweep_work,
			      msecs_to_jiffies(max(sweep_ms, GUARD_SWEEP_MIN_MS)));
}

/* Register the idle detach handler at the tail, then drop the other one. */
static void guard_sweep(struct work_struct *work)
{
	unsigned int idle = !guard_active;

	if (!input_register_handler(&guard_detach_handlers[idle])) {
		input_unregister_handler(&guard_detach_handlers[guard_active]);
		guard_active = idle;
	}

	guard_schedule_sweep();
}

static int __init guard_init(void)
{
	int error;

	WRITE_ONCE(guard_loader, current);
	error = input_register_handler(&guard_owner_handler);
	WRITE_ONCE(guard_loader, NULL);
	if (error)
		return error;

	error = input_register_handler(&guard_detach_handlers[guard_active]);
	if (error) {
		input_unregister_handler(&guard_owner_handler);
		return error;
	}

	if (sweep_ms)
		guard_schedule_sweep();

	pr_info("loaded\n");
	return 0;
}

static void __exit guard_exit(void)
{
	cancel_delayed_work_sync(&guard_sweep_work);
	input_unregister_handler(&guard_detach_handlers[guard_active]);
	input_unregister_handler(&guard_owner_handler);
	pr_info("unloaded; existing devices stay disconnected, new ones are not guarded\n");
}

module_init(guard_init);
module_exit(guard_exit);

MODULE_DESCRIPTION("Keep virtual input devices away from the host and record their owner cgroup");
MODULE_LICENSE("GPL");
