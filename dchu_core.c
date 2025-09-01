// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mfd/core.h>
#include "dchu.h"

static const u8 dchu_uuid_def[16] = {
    0xE4,0x24,0xF2,0x93, 0xDC,0xFB, 0xBF,0x4B,
    0xAD,0xD6,0xDB,0x71, 0xBD,0xC0,0xAF,0xAD
};

static struct platform_device *dchu_parent;
static struct dchu *dchu_core;

int dchu_call_dsm(struct dchu *core, u64 function,
                  const u8 *payload, u32 payload_len,
                  union acpi_object **out_obj)
{
    union acpi_object args[4];
    struct acpi_object_list input;
    struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
    acpi_status status;
    union acpi_object *obj;

    if (!core || !core->handle)
        return -ENODEV;

    args[0].type = ACPI_TYPE_BUFFER;
    args[0].buffer.length = sizeof(core->uuid);
    args[0].buffer.pointer = core->uuid;

    args[1].type = ACPI_TYPE_INTEGER;
    args[1].integer.value = core->rev;

    args[2].type = ACPI_TYPE_INTEGER;
    args[2].integer.value = function;

    if (payload && payload_len) {
        union acpi_object elem;
        elem.type = ACPI_TYPE_BUFFER;
        elem.buffer.length = payload_len;
        elem.buffer.pointer = (u8 *)payload;
        args[3].type = ACPI_TYPE_PACKAGE;
        args[3].package.count = 1;
        args[3].package.elements = &elem;
        input.count = 4;
        input.pointer = args;
        status = acpi_evaluate_object(core->handle, "_DSM", &input, &output);
    } else {
        args[3].type = ACPI_TYPE_PACKAGE;
        args[3].package.count = 0;
        args[3].package.elements = NULL;
        input.count = 4;
        input.pointer = args;
        status = acpi_evaluate_object(core->handle, "_DSM", &input, &output);
    }

    if (ACPI_FAILURE(status))
        return -EIO;

    obj = output.pointer;
    if (!obj)
        return -EIO;

    if (out_obj) {
        *out_obj = obj; /* caller must kfree() */
        return 0;
    }

    kfree(obj);
    return 0;
}
EXPORT_SYMBOL_GPL(dchu_call_dsm);

static int __init dchu_core_init(void)
{
    struct acpi_device *adev;
    int ret;

    /* Require ACPI HID CLV0001 */
    adev = acpi_dev_get_first_match_dev("CLV0001", NULL, -1);
    if (!adev) {
        pr_info("dchu-core: ACPI HID CLV0001 not present\n");
        return -ENODEV;
    }

    dchu_core = kzalloc(sizeof(*dchu_core), GFP_KERNEL);
    if (!dchu_core) {
        ret = -ENOMEM;
        goto put_adev;
    }
    dchu_core->dev = &adev->dev;
    dchu_core->handle = adev->handle;
    memcpy(dchu_core->uuid, dchu_uuid_def, sizeof(dchu_core->uuid));
    dchu_core->rev = 1;

    /* Parent platform device for MFD children */
    dchu_parent = platform_device_alloc("dchu", PLATFORM_DEVID_NONE);
    if (!dchu_parent) {
        ret = -ENOMEM;
        goto free_core;
    }
    ACPI_COMPANION_SET(&dchu_parent->dev, adev);

    ret = platform_device_add(dchu_parent);
    if (ret)
        goto put_parent;

    /* Create children: dchu-hwmon and dchu-leds */
    {
        struct dchu_cell_pdata pdata1 = { .core = dchu_core };
        struct dchu_cell_pdata pdata2 = { .core = dchu_core };
        struct mfd_cell cells[2] = { 0 };

        cells[0].name = "dchu-hwmon";
        cells[0].platform_data = &pdata1;
        cells[0].pdata_size = sizeof(pdata1);

        cells[1].name = "dchu-leds";
        cells[1].platform_data = &pdata2;
        cells[1].pdata_size = sizeof(pdata2);

        ret = mfd_add_devices(&dchu_parent->dev, 0, cells, ARRAY_SIZE(cells),
                              NULL, 0, NULL);
        if (ret)
            goto del_parent;
    }

    acpi_dev_put(adev);
    pr_info("dchu-core: registered with MFD children\n");
    return 0;

del_parent:
    platform_device_del(dchu_parent);
put_parent:
    platform_device_put(dchu_parent);
    dchu_parent = NULL;
free_core:
    kfree(dchu_core);
    dchu_core = NULL;
put_adev:
    acpi_dev_put(adev);
    return ret;
}

static void __exit dchu_core_exit(void)
{
    if (dchu_parent) {
        mfd_remove_devices(&dchu_parent->dev);
        platform_device_unregister(dchu_parent);
        dchu_parent = NULL;
    }
    kfree(dchu_core);
    dchu_core = NULL;
    pr_info("dchu-core: unloaded\n");
}

module_init(dchu_core_init);
module_exit(dchu_core_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Insyde DCHU protocol implementation");
MODULE_AUTHOR("stdpi <iam@stdpi.work>");
