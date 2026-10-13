#ifndef __SETTINGS_MENU_H__
#define __SETTINGS_MENU_H__

#include "so_util.h"

// Extra pages in the in-game Settings menu that edit config.ini (see
// settings_menu.c). config.settings_menu.
void apply_settings_menu(so_module *mod); // before so_finalize()

#endif
