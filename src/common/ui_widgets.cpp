#include "ui_widgets.h"

lv_obj_t *create_menu_button(lv_obj_t *parent, const char *text, int width, int height) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(0x9FBAD0), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_y(button, 3, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(button, lv_color_hex(0x0B1117), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x22313F), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(0xD5E7F7), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button, 2, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_ofs_y(button, 1, LV_STATE_PRESSED);
  lv_obj_set_style_translate_y(button, 2, LV_STATE_PRESSED);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF8F1DC), LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}

lv_obj_t *create_small_button(lv_obj_t *parent, const char *text, int width, int height) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x34495E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(0x9FBAD0), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 4, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_y(button, 2, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(button, lv_color_hex(0x0B1117), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x22313F), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(0xD5E7F7), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button, 1, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_ofs_y(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_translate_y(button, 1, LV_STATE_PRESSED);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0xF8F1DC), LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}
