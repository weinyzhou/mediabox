#ifndef __MB_UI_MENU_H__
#define __MB_UI_MENU_H__


struct mb_ui_menu;


int
mb_ui_menu_additem(struct mb_ui_menu *inst, char *name, void *data);

int
mb_ui_menu_showdialog(struct mb_ui_menu *inst);


struct mb_ui_menu*
mb_ui_menu_new(struct mbv_window *window);


void
mb_ui_menu_destroy(struct mb_ui_menu *inst);

#endif