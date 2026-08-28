// SPDX-License-Identifier: GPL-2.0-only
/*
 * RG55G1: mirror kernel log to a USB serial adapter (CH340G -> ttyUSB0).
 *
 * Uses the usb-serial port layer (same approach as drivers/usb/serial/console.c):
 * tty_kopen + tty->ops->write does not work until userspace has opened the
 * port; we must call serial->type->open() and serial->type->write() directly.
 */

#include <linux/console.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/kfifo.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>

#include "rg55g1_bringup.h"

#if IS_ENABLED(CONFIG_USB_SERIAL)
int __init usb_serial_init(void);
#endif
#if IS_ENABLED(CONFIG_USB_SERIAL_CH341)
int __init ch341_init(void);
#endif

bool rg55g1_log_usb = true;
EXPORT_SYMBOL_GPL(rg55g1_log_usb);

static struct task_struct *rg55g1_log_task;
static struct usb_serial_port *rg55g1_log_port;
static struct notifier_block rg55g1_log_usb_nb;
static atomic_t rg55g1_log_usb_seen;
static bool rg55g1_log_console_registered;

static const struct tty_operations rg55g1_log_fake_tty_ops;

static void rg55g1_log_register_console(void);
static void rg55g1_log_dump_ringbuffer(void);
static bool rg55g1_log_ensure_port(void);
static int rg55g1_log_port_setup(struct usb_serial_port *port);
static struct usb_serial_port *rg55g1_log_find_port(void);

static void rg55g1_log_wait_tx(struct usb_serial_port *port)
{
	unsigned int wait_ms;

	if (!port)
		return;

	for (wait_ms = 0; wait_ms < 30000; wait_ms += 10) {
		unsigned long flags;
		unsigned int pending;

		spin_lock_irqsave(&port->lock, flags);
		pending = kfifo_len(&port->write_fifo) + port->tx_bytes;
		spin_unlock_irqrestore(&port->lock, flags);

		if (!pending)
			return;

		msleep(10);
	}
}

/*
 * Push bytes to CH340. @wait=false for console->write (atomic, no sleep).
 * @wait=true for kthread/init bulk dump (may msleep while TX drains).
 */
static void rg55g1_log_port_xmit(const char *buf, unsigned int count, bool wait)
{
	struct usb_serial_port *port = rg55g1_log_port;
	struct usb_serial *serial;
	unsigned int i, lf, wrote;
	int ret;

	if (!port || !count)
		return;

	serial = port->serial;
	if (!serial || serial->disconnected ||
	    serial->dev->state == USB_STATE_NOTATTACHED)
		return;

	while (count) {
		for (i = 0, lf = 0; i < count; i++) {
			if (buf[i] == '\n') {
				lf = 1;
				i++;
				break;
			}
		}

		wrote = 0;
		while (wrote < i) {
			ret = serial->type->write(NULL, port, buf + wrote, i - wrote);
			if (ret < 0)
				return;
			if (!ret) {
				if (!wait)
					return;
				rg55g1_log_wait_tx(port);
				continue;
			}
			wrote += ret;
		}

		if (lf) {
			const unsigned char cr = '\r';

			ret = serial->type->write(NULL, port, &cr, 1);
			if (ret <= 0 && wait)
				rg55g1_log_wait_tx(port);
		}

		buf += i;
		count -= i;
	}

	if (wait)
		rg55g1_log_wait_tx(port);
}

static void rg55g1_log_port_write_sync(const char *buf, unsigned int count)
{
	rg55g1_log_port_xmit(buf, count, true);
}

static void rg55g1_log_port_write(const char *buf, unsigned int count)
{
	if (!rg55g1_log_port || !rg55g1_log_port->port.console)
		return;

	rg55g1_log_port_xmit(buf, count, false);
}

static bool rg55g1_log_ensure_port(void)
{
	struct usb_serial_port *port;
	int ret, i;

	if (rg55g1_log_port)
		return true;

	for (i = 0; i < 120; i++) {
		port = rg55g1_log_find_port();
		if (port) {
			ret = rg55g1_log_port_setup(port);
			if (!ret)
				return true;
			pr_emerg("rg55g1: log export port setup failed: %d\n", ret);
		}
		msleep(100);
	}

	return false;
}

static void rg55g1_log_dump_ringbuffer(void)
{
	struct kmsg_dump_iter iter = {};
	char *line;
	size_t len;

	line = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!line)
		return;

	kmsg_dump_rewind(&iter);
	while (kmsg_dump_get_line(&iter, false, line, PAGE_SIZE, &len) &&
	       len > 0)
		rg55g1_log_port_write_sync(line, len);

	kfree(line);
}

void rg55g1_log_export_flush(void)
{
	static const char banner[] =
		"\n*** rg55g1 full dmesg -> ttyUSB (115200 8N1) ***\n";

	if (!rg55g1_log_usb)
		return;

	if (!rg55g1_log_ensure_port()) {
		pr_emerg("rg55g1: log flush: no ttyUSB\n");
		return;
	}

	if (!rg55g1_log_console_registered)
		pr_emerg("rg55g1: flushing full dmesg to ttyUSB%d\n",
			 rg55g1_log_port->minor);
	rg55g1_log_port_write_sync(banner, sizeof(banner) - 1);
	rg55g1_log_dump_ringbuffer();
	if (!rg55g1_log_console_registered)
		rg55g1_log_register_console();
	{
		static const char done[] =
			"\n*** rg55g1 dmesg flush done, console live ***\n";

		rg55g1_log_port_write_sync(done, sizeof(done) - 1);
	}
}
EXPORT_SYMBOL_GPL(rg55g1_log_export_flush);

static void rg55g1_log_console_write(struct console *co, const char *s,
				       unsigned int count)
{
	rg55g1_log_port_write(s, count);
}

static struct console rg55g1_log_console = {
	.name = "usbser",
	.write = rg55g1_log_console_write,
	/*
	 * CON_ENABLED: register even without console=usbser on cmdline
	 * (register_console rejects unknown consoles otherwise).
	 * History is exported via kmsg_dump; omit CON_PRINTBUFFER.
	 */
	.flags = CON_ANYTIME | CON_ENABLED,
	.index = -1,
};

static int rg55g1_log_port_setup(struct usb_serial_port *port)
{
	struct usb_serial *serial = port->serial;
	struct tty_struct *tty = NULL;
	struct ktermios dummy;
	int ret;

	ret = usb_autopm_get_interface(serial->interface);
	if (ret)
		goto out_unlock;

	tty_port_tty_set(&port->port, NULL);

	++port->port.count;
	if (!tty_port_initialized(&port->port)) {
		if (serial->type->set_termios) {
			tty = kzalloc(sizeof(*tty), GFP_KERNEL);
			if (!tty) {
				ret = -ENOMEM;
				goto reset_open_count;
			}

			kref_init(&tty->kref);
			tty->driver = usb_serial_tty_driver;
			tty->index = port->minor;
			init_ldsem(&tty->ldisc_sem);
			spin_lock_init(&tty->files_lock);
			INIT_LIST_HEAD(&tty->tty_files);
			kref_get(&tty->driver->kref);
			__module_get(tty->driver->owner);
			tty->ops = (struct tty_operations *)&rg55g1_log_fake_tty_ops;
			tty_init_termios(tty);
			tty_port_tty_set(&port->port, tty);
		}

		ret = serial->type->open(tty, port);
		if (ret)
			goto fail;

		if (serial->type->set_termios && tty) {
			tty->termios.c_cflag = CREAD | HUPCL | CLOCAL | CS8;
			tty_termios_encode_baud_rate(&tty->termios, 115200, 115200);
			memset(&dummy, 0, sizeof(dummy));
			serial->type->set_termios(tty, port, &dummy);
			tty_port_tty_set(&port->port, NULL);
			tty_kref_put(tty);
			tty = NULL;
		}

		tty_port_set_initialized(&port->port, true);
	}

	--port->port.count;
	port->port.console = 1;
	rg55g1_log_port = port;

	mutex_unlock(&serial->disc_mutex);
	return 0;

fail:
	if (tty) {
		tty_port_tty_set(&port->port, NULL);
		tty_kref_put(tty);
	}
reset_open_count:
	port->port.count = 0;
	usb_autopm_put_interface(serial->interface);
out_unlock:
	mutex_unlock(&serial->disc_mutex);
	usb_serial_put(serial);
	return ret;
}

static struct usb_serial_port *rg55g1_log_find_port(void)
{
	struct usb_serial_port *port;
	unsigned int minor;

	for (minor = 0; minor < 4; minor++) {
		port = usb_serial_port_get_by_minor(minor);
		if (port)
			return port;
	}

	return NULL;
}

static void rg55g1_log_register_console(void)
{
	if (rg55g1_log_console_registered || !rg55g1_log_port)
		return;

	console_lock();
	register_console(&rg55g1_log_console);
	console_unlock();
	rg55g1_log_console_registered = true;
}

static bool rg55g1_log_ch340_device(struct usb_device *udev)
{
	if (!udev)
		return false;

	return udev->descriptor.idVendor == 0x1a86 &&
	       (udev->descriptor.idProduct == 0x7523 ||
		udev->descriptor.idProduct == 0x5523);
}

static int rg55g1_log_usb_notify(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct usb_device *udev = data;

	if (action == USB_DEVICE_ADD && rg55g1_log_ch340_device(udev))
		atomic_set(&rg55g1_log_usb_seen, 1);
	return NOTIFY_OK;
}

int __init rg55g1_usb_serial_bringup(void)
{
	static bool done;
	int ret = 0;

	if (done)
		return 0;

#if IS_ENABLED(CONFIG_USB_SERIAL)
	ret = usb_serial_init();
	if (ret) {
		pr_emerg("rg55g1: usb_serial_init failed: %d\n", ret);
		return ret;
	}
#endif
#if IS_ENABLED(CONFIG_USB_SERIAL_CH341)
	ret = ch341_init();
	if (ret) {
		pr_emerg("rg55g1: ch341_init failed: %d\n", ret);
		return ret;
	}
#endif

	done = true;
	pr_emerg("rg55g1: USB serial drivers registered (LV6 skip workaround)\n");
	return 0;
}
EXPORT_SYMBOL_GPL(rg55g1_usb_serial_bringup);

static int rg55g1_log_export_thread(void *unused)
{
	static const char ready[] =
		"\n*** rg55g1 ttyUSB log (115200) — full dump ~20s ***\n";

	if (!rg55g1_log_ensure_port()) {
		pr_emerg("rg55g1: log export: no ttyUSB (ch341=%d seen=%d)\n",
			 IS_ENABLED(CONFIG_USB_SERIAL_CH341),
			 atomic_read(&rg55g1_log_usb_seen));
		return 0;
	}

	rg55g1_log_port_write_sync(ready, sizeof(ready) - 1);
	rg55g1_log_register_console();
	rg55g1_log_port_write_sync(
		"rg55g1: ttyUSB live console ready\n", 35);

	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(1000));

	if (rg55g1_log_console_registered) {
		console_lock();
		unregister_console(&rg55g1_log_console);
		console_unlock();
		rg55g1_log_console_registered = false;
	}

	return 0;
}

void rg55g1_log_export_start(void)
{
	int ret;

	if (!rg55g1_log_usb || rg55g1_log_task)
		return;

#if !IS_ENABLED(CONFIG_USB_SERIAL)
	pr_emerg("rg55g1: log export disabled (CONFIG_USB_SERIAL=n)\n");
	return;
#endif

	pr_emerg("rg55g1: log export thread starting (ch341=%s)\n",
		 IS_ENABLED(CONFIG_USB_SERIAL_CH341) ? "y" : "n");

	if (!rg55g1_log_usb_nb.notifier_call) {
		rg55g1_log_usb_nb.notifier_call = rg55g1_log_usb_notify;
		usb_register_notify(&rg55g1_log_usb_nb);
	}

	rg55g1_log_task = kthread_run(rg55g1_log_export_thread, NULL,
				      "rg55g1-log");
	if (IS_ERR(rg55g1_log_task)) {
		ret = PTR_ERR(rg55g1_log_task);
		rg55g1_log_task = NULL;
		pr_emerg("rg55g1: log export kthread failed: %d\n", ret);
	}
}
EXPORT_SYMBOL_GPL(rg55g1_log_export_start);

static int __init rg55g1_log_usb_setup(char *str)
{
	if (!str || !*str)
		return 0;

	if (!strcmp(str, "0") || !strcmp(str, "off") || !strcmp(str, "no"))
		rg55g1_log_usb = false;
	else
		rg55g1_log_usb = true;
	return 0;
}
early_param("rg55g1.log_usb", rg55g1_log_usb_setup);
