#ifndef BLUETOOTH_SERVICE_H
#define BLUETOOTH_SERVICE_H

int initServices();
int destoryServices();
int startDiscovery();
int stopDiscovery();
int startPaireDevice(const char * device_path);
int connectDevice(const char *device_path);

#endif
