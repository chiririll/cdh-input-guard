# cdh_input_guard

A host kernel module for Cloud Desktop Host (cloud-desktop-host), plus one host udev rule. Together they stop input from containers reaching the host: keys sent through uinput can no longer switch VTs, trigger Ctrl+Alt+Del or SysRq, toggle rfkill, or make logind power off the host. The module also records which container created each virtual input device, so udev can expose the device only inside that container.

Design and phases: `cloud-desktop-host/docs/07_input_isolation_kmod_plan.md`.

**Status: phase 1 prototype.** It builds against kernel 7.0. It has been loaded once, but that build matched no devices, so the detach and owner code has not run yet. Test it only in a throwaway VM running the same kernel as production. A bug here panics the whole host.

## How it works

A virtual input device is one with no parent device, which is what uinput creates, or one below the uhid misc device. The module registers two kinds of input handler for them.

**Detach handlers.** `connect()` calls `disconnect()` on every other handler attached to the device, except the ones named in the `keep` parameter (default `evdev,joydev`). `/dev/input/event*` and `js*` keep working; `kbd`, `sysrq`, `rfkill`, `mousedev`, `leds` and anything else are gone. `connect()` then returns `-ENODEV`, so these handlers hold no handle and no state.

Handlers attach in registration order, and `connect()` can only detach handles that already exist. A handler registered after the module (sysrq when `kernel.sysrq` goes from 0 to non-zero, `input-leds` autoloaded for the first keyboard with LEDs) would attach afterwards. So there are two identical detach handlers, and every `sweep_ms` (default 1000) a work item registers the idle one and then unregisters the other. Registering puts the handler at the tail of the handler list and runs `connect()` for every existing device. One of the two is always registered.

**Owner handler.** `connect()` runs inside the `UI_DEV_CREATE` ioctl, so `current` is the process that created the device. The module stores that process's cgroup v2 path, relative to the hierarchy root (for example `/lxc/101/ns/...`), and exposes it as `/sys/devices/virtual/input/inputN/cdh_owner_cgroup`, readable by root only. It then sends a `change` uevent on `inputN` and its children (`eventN`, `jsN`, ...). Its handle is never opened and exists so that `disconnect()` runs when the device goes away.

A device gets no owner attribute if it existed when the module loaded, or if a kernel thread registered it. uhid adds its device from a workqueue, so uhid devices are detached but never have an owner.

**udev rule** `udev/71-cdh-virtual-input.rules`. evdev stays attached, so host userspace can still read the devices. The stock `70-power-switch.rules` tags every event node that has keys with `power-switch`, and logind acts on `KEY_POWER`, `KEY_SLEEP`, `KEY_SUSPEND` and the lid switch of tagged nodes. The rule removes the tag from virtual devices and adds the tag `cdh-virtual-input`. The second tag is required: see the findings below. The rule does not depend on the module.

## Parameters

All are read-only after load.

| Parameter | Default | Meaning |
|---|---|---|
| `keep` | `evdev,joydev` | Handlers that stay attached. Everything else is disconnected. |
| `uhid` | `Y` | Also guard devices created through uhid. Set `uhid=0` if the host console uses a Bluetooth LE keyboard (BlueZ creates those through uhid). |
| `sweep_ms` | `1000` | Sweep interval, minimum 100. `0` disables sweeps. |

## Build

In the test VM, install headers for the exact running kernel (`proxmox-headers-$(uname -r)`), then run:

```
make            # build/cdh_input_guard.ko + build/uinput_kbd + build/uhid_kbd
make KVER=...   # build for another installed kernel
make clean      # rm -rf build/
```

Layout: module sources in `src/`, tests in `tests/`, the udev rule in `udev/`, and all build output in `build/`. The `MO=` option (kernel 6.13+) keeps kbuild's files out of `src/`.

## Install the udev rule

```
cp udev/71-cdh-virtual-input.rules /etc/udev/rules.d/
udevadm control --reload
```

It applies to devices created after the reload.

## Test

```
sudo tests/check.sh
```

The script compares a baseline run, without the module and without the udev rule, against a run with both. It covers handlers, evdev delivery, VT switching, SysRq, the owner attribute and its mode, whether systemd-logind opens the event node, a device that existed before loading, a handler registered after the module, a uhid keyboard, create/destroy and load/unload loops, and kernel warnings in dmesg. It temporarily sets `kernel.sysrq=1` and puts the udev rule in `/run/udev/rules.d`.

Three checks stay manual, in the VM only. The script unloads the module and removes its temporary udev rule when it exits, so load the module (`insmod build/cdh_input_guard.ko`) and install the udev rule first. Without them these commands do type on the console, reboot and power off.

- Run `build/uinput_kbd -t -w 3000` while tty1 shows a login prompt. The text must not appear.
- Run `build/uinput_kbd -d` with `ctrl-alt-del.target` unmasked. The VM must not reboot.
- Run `build/uinput_kbd -p` with the udev rule installed. The VM must not power off.

For the run that counts, use a debug kernel in the VM with `CONFIG_PROVE_LOCKING`, `CONFIG_DEBUG_ATOMIC_SLEEP`, `CONFIG_KASAN`, `CONFIG_DEBUG_LIST`, so lockdep and KASAN can catch locking and memory bugs in the detach path.

The module logs device names only at debug level, because the container chooses them. To see them: `echo 'module cdh_input_guard +p' > /sys/kernel/debug/dynamic_debug/control`.

## Findings from reading the v7.0 source (confirm by running)

- **Calling another handler's `disconnect()` inside `connect()`.** `input_register_device()` and `input_register_handler()` call `connect()` with `input_mutex` held and `dev->mutex` free. `__input_unregister_device()` calls `disconnect()` for every handle in that same context, so every handler has to cope with it. `kbd_disconnect()`, `sysrq_disconnect()` and `rfkill_disconnect()` take only `dev->mutex`, plus `cancel_work_sync()` and timer shutdown for sysrq.
- **Matching every device.** `input_match_device()` stops at the first id table entry with `flags == 0`. Until 6.17 an entry with only `driver_info` set matched everything; since 6.17 it ends the table and the handler attaches to nothing, without any error. The module matches on `EV_SYN`, which the input core sets on every device, as evdev does. (Confirmed by running: the first build used `{ .driver_info = 1 }` and loaded but did nothing.)
- **`TAG-=` alone does not keep logind away.** logind opens event nodes whose current tags include `power-switch`. When `TAG-=` removes the last tag of a device, udev (systemd 252 to 259, by source and by `udevadm test`) leaves the old `CURRENT_TAGS=:power-switch:` property in the event it sends, although the udev database is correct. logind then opens the node, and `uinput_kbd -p` powered the VM off with the first version of the rule. With a second tag on the device udev sends the correct list, so the rule adds `cdh-virtual-input`. `udevadm info` does not show the problem; the test looks at logind's open files instead.
- **Handler order.** `input_register_device()` attaches handlers in `input_handler_list` order, and new handlers go to the tail.
- **sysrq is only registered while `kernel.sysrq != 0`.** `sysrq_toggle_support()` unregisters it at 0 and re-registers it, at the tail, when it becomes non-zero.
- **uinput devices have no parent** (`drivers/input/misc/uinput.c`). **uhid devices** have the HID device as parent, which has the `uhid` misc device as parent, and are added by `uhid_device_add_worker()` on a workqueue.
- **A handler with no `event`, `events` or `filter`** is accepted; its handles get a no-op event function.

## Limitations

- **A late handler is attached for up to `sweep_ms`.** For an existing device, from the moment the handler registers until the next sweep. For new devices, only until that sweep has moved the detach handler back to the tail. With `sweep_ms=0`, keep `kernel.sysrq=0` and load no input handler modules after this one.
- **Other parentless devices are guarded too.** Some platform drivers (WMI hotkeys, for example) register input devices with no parent. They lose `kbd` and get an owner attribute naming whoever loaded the driver. The module cannot tell them from uinput devices.
- **Host userspace still sees the devices through evdev.** The udev rule covers logind. A graphical session on the host would still take the devices as seat input.
- **Unloading does not re-attach handlers** to existing devices, and new devices are not guarded until the module is loaded again.
- **The module must load at boot**, before any container creates devices. Otherwise those devices have no owner.
- **The owner is the creator's cgroup**, which assumes the device is created by a process inside the container, not by a host-side helper.
- **Device names stay visible** through `/sys` and `/proc/bus/input/devices`. Events and owner paths do not.

## For phase 2 (udev)

The owner attribute appears after the `add` uevents. The udev rule must act on `ACTION=="change"` (and `add`, for the case where `eventN` or `jsN` was created after the attribute) and match `ATTRS{cdh_owner_cgroup}=="?*"` on `event*`/`js*` nodes.

Treat the attribute as untrusted. Below `/lxc/<ctid>/` the container names its own cgroups, and a name may contain spaces, quotes, glob characters or a segment like `lxc/102` (only a newline is impossible). So the helper must:

- Read the attribute from sysfs itself. Do not pass it through `$attr{cdh_owner_cgroup}` in `RUN+=`; udev splits the substituted text on spaces.
- Take the container id only from a match of `^/lxc/([0-9]+)/` at the start of the path, and ignore the device if that does not match (owner `/`, a host service, a platform driver).
