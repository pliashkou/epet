#pragma once
#include <stdbool.h>
#include "epet_link.h"

/* Firmware update over the same link as modules.
 *
 *   host: #FW <length>        dev: #FREADY | #ERR <why>
 *   host: #FD <seq> <base64>  dev: #FA <next-seq> <bytes>  | #ERR seq <n>
 *   host: #FDONE              dev: #FOK  (then reboots) | #ERR <why>
 *   host: #FABORT             dev: #OK
 *
 * Same sequencing and retry as a module transfer, because the same link
 * drops lines. The image is written straight into the spare app slot, so
 * nothing large is buffered in RAM. */

void ota_init(void);
bool ota_command(void *ctx, const char *cmd, char *arg);
bool ota_in_progress(void);
int  ota_progress(void);      /* 0..100 */
const char *ota_running_slot(void);
