/*
 * Embedded kmbugcheck.sys blob.  This stub ships empty; the CI regenerates this
 * file from the WDK-built driver on the x64 job (scripts/gen-sys-header.ps1).
 * An empty blob means km_driver_launch() reports the kernel path unavailable.
 */
#pragma once
#include <stddef.h>

static const unsigned char kmbugcheck_sys[] = { 0 };
static const unsigned int kmbugcheck_sys_len = 0;
