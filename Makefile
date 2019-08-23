CROSS=/home/sun/share/prj/rk3308-bsp/buildroot/output/firefly_rk3308_release/host/bin/aarch64-rockchip-linux-gnu-
ROOT=/home/sun/share/prj/rk3308-bsp/buildroot/output/firefly_rk3308_release/host/aarch64-rockchip-linux-gnu/sysroot
CC=$(CROSS)gcc -I$(ROOT)/usr/include -I$(ROOT)/usr/include/dbus-1.0 -I$(ROOT)/usr/lib/dbus-1.0/include -L$(ROOT)/usr/lib -L$(ROOT)/lib64 -lbluetooth -ldbus-1 -lpthread
#OBJ=modeset-double-buffered
OBJ=dbus_bt_test
$(OBJ):bluetooth_eventloop.c bluetooth_common.c  bluetooth_service.c main.c
	$(CC) bluetooth_eventloop.c bluetooth_common.c  bluetooth_service.c main.c -o dbus_bt_test