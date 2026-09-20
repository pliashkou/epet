#pragma once
#include "epet_ui.h"

/* The built-in sub-programs, owned by the core module. Use them as
 * templates: a page owns its state, draws its own screen, and returns false
 * from update() when it wants to close. */
epet_page_t *const *epet_pages_builtin(uint8_t *n_out);

/* Convenience for tests and standalone use: install the core module and
 * register its pages into `ui`. Normal code installs modules instead. */
void epet_pages_register_builtin(epet_ui_t *ui);
