#ifndef BLUETOOTH_EVENTLOOP_H
#define BLUETOOTH_EVENTLOOP_H

int initializeBluetoothEvent();
int startEventLoop();
void stopEventLoop();
void cleanupBluetoothEvent();

#endif
