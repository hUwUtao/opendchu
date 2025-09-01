// SPDX-License-Identifier: 0BSD
#ifndef _DCHU_H
#define _DCHU_H

#include <linux/acpi.h>

struct dchu {
    struct device *dev;        /* core parent device */
    acpi_handle handle;        /* ACPI handle for _DSM calls */
    u8 uuid[16];               /* _DSM UUID */
    u64 rev;                   /* _DSM revision */
};

struct dchu_cell_pdata {
    struct dchu *core;
};

int dchu_call_dsm(struct dchu *core, u64 function,
                  const u8 *payload, u32 payload_len,
                  union acpi_object **out_obj);

#endif /* _DCHU_H */
