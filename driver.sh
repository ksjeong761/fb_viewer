insmod fpga_push_switch_driver.ko
insmod fpga_text_lcd_driver.ko
mknod /dev/fpga_push_switch c 265 0
mknod /dev/fpga_text_lcd c 263 0
