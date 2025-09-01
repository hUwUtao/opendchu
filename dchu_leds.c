// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "dchu.h"

struct dchu_leds_ctx {
    struct dchu *core;
    struct led_classdev cdev;
    struct mutex lock;
    u8 last_level;
};

static enum led_brightness dchu_led_get(struct led_classdev *cdev)
{
    struct dchu_leds_ctx *ctx = container_of(cdev, struct dchu_leds_ctx, cdev);
    union acpi_object *obj = NULL;
    int ret;
    enum led_brightness b = 0;

    mutex_lock(&ctx->lock);
    u8 payload[1] = { 0 };
    ret = dchu_call_dsm(ctx->core, 61, payload, sizeof(payload), &obj);
    if (!ret && obj && obj->type == ACPI_TYPE_INTEGER) {
        u64 v = obj->integer.value & 0xff; /* 1 byte casted to int */
        if (v > cdev->max_brightness)
            v = cdev->max_brightness;
        b = (enum led_brightness)v;
    } else {
        /* Fallback to last set value if GET is unsupported */
        b = ctx->last_level;
    }
    kfree(obj);
    mutex_unlock(&ctx->lock);
    return b;
}

static int dchu_led_set(struct led_classdev *cdev, enum led_brightness value)
{
    struct dchu_leds_ctx *ctx = container_of(cdev, struct dchu_leds_ctx, cdev);
    u8 payload[4] = { 0, 0, 0, 0 };
    int ret;

    if (value > cdev->max_brightness)
        value = cdev->max_brightness;

    payload[0] = (u8)value; /* 0..5 */

    mutex_lock(&ctx->lock);
    ret = dchu_call_dsm(ctx->core, 39, payload, sizeof(payload), NULL);
    mutex_unlock(&ctx->lock);
    if (!ret)
        ctx->last_level = payload[0];
    return ret;
}

/* Debug helpers on parent platform device for raw access */
static ssize_t raw_status_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct dchu_leds_ctx *ctx = platform_get_drvdata(to_platform_device(dev));
    union acpi_object *obj = NULL; int ret; ssize_t n = 0; u32 i;
    mutex_lock(&ctx->lock);
    ret = dchu_call_dsm(ctx->core, 61, NULL, 0, &obj);
    if (!ret && obj) {
        if (obj->type == ACPI_TYPE_INTEGER) {
            n = scnprintf(buf, PAGE_SIZE, "int %llu\n", obj->integer.value);
        } else if (obj->type == ACPI_TYPE_BUFFER) {
            n += scnprintf(buf + n, PAGE_SIZE - n, "buf %u ", obj->buffer.length);
            for (i = 0; i < obj->buffer.length && n < PAGE_SIZE - 4; i++)
                n += scnprintf(buf + n, PAGE_SIZE - n, "%02x%s",
                               obj->buffer.pointer[i], i + 1 < obj->buffer.length ? " " : "\n");
        } else {
            n = scnprintf(buf, PAGE_SIZE, "type %d\n", obj->type);
        }
    } else {
        n = scnprintf(buf, PAGE_SIZE, "err %d\n", ret);
    }
    kfree(obj);
    mutex_unlock(&ctx->lock);
    return n;
}
static DEVICE_ATTR_RO(raw_status);

static ssize_t raw_set_store(struct device *dev,
                             struct device_attribute *attr, const char *buf, size_t count)
{
    struct dchu_leds_ctx *ctx = platform_get_drvdata(to_platform_device(dev));
    unsigned long v; int ret; u8 payload[4] = {0};
    if (kstrtoul(buf, 0, &v))
        return -EINVAL;
    if (v > 255)
        v = 255;
    payload[0] = (u8)v;
    mutex_lock(&ctx->lock);
    ret = dchu_call_dsm(ctx->core, 31, payload, sizeof(payload), NULL);
    mutex_unlock(&ctx->lock);
    return ret ? ret : count;
}
static DEVICE_ATTR_WO(raw_set);

static int dchu_leds_probe(struct platform_device *pdev)
{
    struct dchu_cell_pdata *pdata = dev_get_platdata(&pdev->dev);
    struct dchu_leds_ctx *ctx;
    int ret;

    if (!pdata || !pdata->core)
        return -ENODEV;

    ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;
    ctx->core = pdata->core;
    mutex_init(&ctx->lock);

    ctx->cdev.name = "dchu::kbd_backlight";
    ctx->cdev.max_brightness = 5;
    ctx->cdev.brightness_set_blocking = dchu_led_set;
    ctx->cdev.brightness_get = dchu_led_get;
    ctx->last_level = 0;

    ret = devm_led_classdev_register(&pdev->dev, &ctx->cdev);
    if (ret)
        return ret;

    /* Register debug attrs on platform device */
    ret = device_create_file(&pdev->dev, &dev_attr_raw_status);
    if (ret)
        return ret;
    ret = device_create_file(&pdev->dev, &dev_attr_raw_set);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, ctx);
    dev_info(&pdev->dev, "dchu-leds initialized\n");
    return 0;
}

static void dchu_leds_remove(struct platform_device *pdev)
{
    device_remove_file(&pdev->dev, &dev_attr_raw_status);
    device_remove_file(&pdev->dev, &dev_attr_raw_set);
}

static struct platform_driver dchu_leds_driver = {
    .driver = {
        .name = "dchu-leds",
    },
    .probe = dchu_leds_probe,
    .remove = dchu_leds_remove,
};

module_platform_driver(dchu_leds_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Insyde DCHU keyboard light");
MODULE_AUTHOR("stdpi <iam@stdpi.work>");
