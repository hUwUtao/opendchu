// SPDX-License-Identifier: 0BSD

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/math64.h>
#include "dchu.h"

struct dchu_hwmon_ctx {
    struct dchu *core;
    struct device *hwdev;
    u8 fan_mode; /* last set mode */
};

/* Module parameters to handle inverse tach period vs RPM */
static bool invert = true; /* default assume period -> RPM */
module_param(invert, bool, 0644);
MODULE_PARM_DESC(invert, "Interpret 16-bit value as tach period (inverse of RPM)");

/* Match UI math: 60/(5.565217e-05 * raw) * 2 => tach_hz = 2 * (1/5.565217e-05) ≈ 35940 Hz */
static unsigned int tach_hz = 35940;
module_param(tach_hz, uint, 0644);
MODULE_PARM_DESC(tach_hz, "EC tach base clock in Hz (used when invert=1)");

static unsigned int ppr = 1; /* pulses per revolution to mirror UI's extra *2 */
module_param(ppr, uint, 0644);
MODULE_PARM_DESC(ppr, "Fan pulses per revolution (used when invert=1)");

/* Endianness of the 16-bit raw value at offsets (2,3), (4,5), (6,7) */
static bool le = true; /* default little-endian: (hi<<8)|lo with hi at index */
module_param(le, bool, 0644);
MODULE_PARM_DESC(le, "Raw 16-bit word endianness (little-endian if true)");

static inline u16 dchu_get16(const u8 *b, int hi)
{
    /* bytes at [hi] (MSB) and [hi+1] (LSB) in the parse table */
    return le ? (u16)(((u16)b[hi] << 8) | b[hi + 1])
              : (u16)(((u16)b[hi + 1] << 8) | b[hi]);
}

static inline long dchu_to_rpm(u16 raw)
{
    if (!invert)
        return (long)raw;
    if (!raw || !tach_hz || !ppr)
        return 0;
    return (long)DIV_ROUND_CLOSEST_ULL((u64)tach_hz * 60ULL,
                                       (u64)ppr * (u64)raw);
}

/* Helper: call _DSM and return buffer for given function id */
static int dchu_get_dsm_buf(struct dchu *core, u64 function, u8 **buf, u32 *len, union acpi_object **holder)
{
    union acpi_object *obj;
    int ret;

    ret = dchu_call_dsm(core, function, NULL, 0, &obj);
    if (ret)
        return ret;

    if (!obj || obj->type != ACPI_TYPE_BUFFER)
        goto bad;

    if (obj->buffer.length < 32)
        goto bad;

    *buf = obj->buffer.pointer;
    *len = obj->buffer.length;
    *holder = obj; /* caller must kfree(*holder) */
    return 0;

bad:
    kfree(obj);
    return -EIO;
}

static int dchu_read(struct device *dev, enum hwmon_sensor_types type,
                     u32 attr, int channel, long *val)
{
    /* Only used by legacy fan1_input sysfs helper below */
    if (type == hwmon_fan && attr == hwmon_fan_input && channel == 0) {
        struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
        u8 *buf; u32 len; union acpi_object *hold;
        int ret = dchu_get_dsm_buf(ctx->core, 12 /* FAN package */, &buf, &len, &hold);
        if (ret)
            return ret;
        /* CPU fan raw at [2],[3] */
        *val = dchu_to_rpm(dchu_get16(buf, 2));
        kfree(hold);
        return 0;
    }

    return -EOPNOTSUPP;
}

/* (obsolete) hwmon info approach kept for reference was removed */

/* Legacy sysfs path using groups to avoid strict hwmon_info checks */
static ssize_t fan1_input_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    long val;
    int ret = dchu_read(dev, hwmon_fan, hwmon_fan_input, 0, &val);
    if (ret)
        return ret;
    return sysfs_emit(buf, "%ld\n", val);
}
static DEVICE_ATTR_RO(fan1_input);

/* Additional attributes following the parse table (package 12) */
static ssize_t fan2_input_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long rpm; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    rpm = dchu_to_rpm(dchu_get16(b, 4));
    kfree(h);
    return sysfs_emit(buf, "%ld\n", rpm);
}
static DEVICE_ATTR_RO(fan2_input);

static ssize_t fan3_input_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long rpm; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    rpm = dchu_to_rpm(dchu_get16(b, 6));
    kfree(h);
    return sysfs_emit(buf, "%ld\n", rpm);
}
static DEVICE_ATTR_RO(fan3_input);

/* Debug: dump raw buffer in hex to aid interpretation */
static ssize_t fan_buf_show(struct device *dev,
                            struct device_attribute *attr, char *out)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; int ret; ssize_t pos = 0; u32 i;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    for (i = 0; i < l && pos < PAGE_SIZE - 4; i++)
        pos += scnprintf(out + pos, PAGE_SIZE - pos, "%02x%s", b[i],
                         (i + 1 < l) ? " " : "");
    pos += scnprintf(out + pos, PAGE_SIZE - pos, "\n");
    kfree(h);
    return pos;
}
static DEVICE_ATTR_RO(fan_buf);

/* duty in package appears 0..100; expose as pwmX (0..255) */
static ssize_t pwm1_show(struct device *dev,
                         struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long pwm; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    pwm = (long)((b[16] * 255 + 50) / 100);
    if (pwm > 255)
        pwm = 255;
    kfree(h);
    return sysfs_emit(buf, "%ld\n", pwm);
}
static DEVICE_ATTR_RO(pwm1);

static ssize_t pwm2_show(struct device *dev,
                         struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long pwm; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    pwm = (long)((b[19] * 255 + 50) / 100);
    if (pwm > 255)
        pwm = 255;
    kfree(h);
    return sysfs_emit(buf, "%ld\n", pwm);
}
static DEVICE_ATTR_RO(pwm2);

static ssize_t pwm3_show(struct device *dev,
                         struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long pwm; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    pwm = (long)((b[22] * 255 + 50) / 100);
    if (pwm > 255)
        pwm = 255;
    kfree(h);
    return sysfs_emit(buf, "%ld\n", pwm);
}
static DEVICE_ATTR_RO(pwm3);

/* temps in degrees C; expose in millidegrees */
static ssize_t temp1_input_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long t; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    /* NOTE: Original uses CalCPUTemp(TDP, b[18]); here we expose raw */
    t = (long)b[18] * 1000L;
    kfree(h);
    return sysfs_emit(buf, "%ld\n", t);
}
static DEVICE_ATTR_RO(temp1_input);

static ssize_t temp2_input_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long t; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    t = (long)b[21] * 1000L;
    kfree(h);
    return sysfs_emit(buf, "%ld\n", t);
}
static DEVICE_ATTR_RO(temp2_input);

static ssize_t temp3_input_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    u8 *b; u32 l; union acpi_object *h; long t; int ret;
    ret = dchu_get_dsm_buf(ctx->core, 12, &b, &l, &h);
    if (ret) return ret;
    t = (long)b[24] * 1000L;
    kfree(h);
    return sysfs_emit(buf, "%ld\n", t);
}
static DEVICE_ATTR_RO(temp3_input);

/* Fan mode high (aka turbo): write 0/1 via _DSM command 121 with 1-byte payload */
/* fan_mode_high removed */

static const char *dchu_mode_name(u8 mode)
{
    switch (mode) {
    case 0: return "auto";
    case 1: return "max";
    case 3: return "silent";
    case 5: return "maxq";
    case 6: return "custom";
    case 7: return "turbo";
    default: return "unknown";
    }
}

static int dchu_set_fan_mode(struct dchu_hwmon_ctx *ctx, u8 mode)
{
    u8 payload[4] = {0};
    payload[0] = mode; /* data */
    payload[3] = 1;    /* subcommand */
    return dchu_call_dsm(ctx->core, 121, payload, sizeof(payload), NULL);
}

static ssize_t fan_mode_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    return sysfs_emit(buf, "%u\n", ctx->fan_mode);
}

static ssize_t fan_mode_store(struct device *dev,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    unsigned long v; int ret; u8 mode;

    /* numeric? */
    if (!kstrtoul(buf, 0, &v)) {
        mode = (u8)v;
    } else {
        /* else expect a known name (lowercase) */
        if (sysfs_streq(buf, "auto")) mode = 0;
        else if (sysfs_streq(buf, "max")) mode = 1;
        else if (sysfs_streq(buf, "silent")) mode = 3;
        else if (sysfs_streq(buf, "maxq")) mode = 5;
        else if (sysfs_streq(buf, "custom")) mode = 6;
        else if (sysfs_streq(buf, "turbo")) mode = 7;
        else return -EINVAL;
    }

    switch (mode) {
    case 0: case 1: case 3: case 5: case 6: case 7: break;
    default: return -ERANGE;
    }

    ret = dchu_set_fan_mode(ctx, mode);
    if (ret)
        return ret;
    ctx->fan_mode = mode;
    return count;
}
static DEVICE_ATTR_RW(fan_mode);

static ssize_t fan_mode_name_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
    struct dchu_hwmon_ctx *ctx = dev_get_drvdata(dev->parent);
    return sysfs_emit(buf, "%s\n", dchu_mode_name(ctx->fan_mode));
}
static DEVICE_ATTR_RO(fan_mode_name);

static struct attribute *dchu_attrs[] = {
    &dev_attr_fan1_input.attr,
    &dev_attr_fan2_input.attr,
    &dev_attr_fan3_input.attr,
    &dev_attr_fan_buf.attr,
    &dev_attr_pwm1.attr,
    &dev_attr_pwm2.attr,
    &dev_attr_pwm3.attr,
    &dev_attr_temp1_input.attr,
    &dev_attr_temp2_input.attr,
    &dev_attr_temp3_input.attr,
    &dev_attr_fan_mode.attr,
    &dev_attr_fan_mode_name.attr,
    NULL,
};

static const struct attribute_group dchu_group = {
    .attrs = dchu_attrs,
};

static const struct attribute_group *dchu_groups[] = {
    &dchu_group,
    NULL,
};

static int dchu_hwmon_probe(struct platform_device *pdev)
{
    struct dchu_hwmon_ctx *ctx;
    struct dchu_cell_pdata *pdata = dev_get_platdata(&pdev->dev);

    if (!pdata || !pdata->core)
        return -ENODEV;

    ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->core = pdata->core;

    ctx->hwdev = devm_hwmon_device_register_with_groups(&pdev->dev, "dchu",
                                                        NULL, dchu_groups);
    if (IS_ERR(ctx->hwdev))
        return PTR_ERR(ctx->hwdev);

    platform_set_drvdata(pdev, ctx);
    dev_info(&pdev->dev, "dchu-hwmon initialized\n");
    return 0;
}

static void dchu_hwmon_remove(struct platform_device *pdev) { }

static struct platform_driver dchu_hwmon_driver = {
    .driver = {
        .name = "dchu-hwmon",
    },
    .probe = dchu_hwmon_probe,
    .remove = dchu_hwmon_remove,
};

module_platform_driver(dchu_hwmon_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("hwmon driver for Insyde DCHU");
MODULE_AUTHOR("stdpi <iam@stdpi.work>");
