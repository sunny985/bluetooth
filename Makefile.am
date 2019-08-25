bin_PROGRAMS    = dbus_bt
dbus_bt_SOURCES = main.c \
						src/bluetooth_common.c \
						src/bluetooth_eventloop.c \
						src/bluetooth_service.c


AM_CPPFLAGS = -I$(top_srcdir)/include
LIBS   = -lbluetooth -ldbus-1 -lpthread