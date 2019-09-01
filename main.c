#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include "bluetooth_eventloop.h"
#include "bluetooth_service.h"
#include "bluetooth_common.h"

#define DEFAULT_DEVICE_PATH "/org/bluez/hci0/dev_0C_12_62_24_15_E1"

static int terminate = 0;

static void sig_term(int sig) {
    terminate = 1;
}

int main (void) {
    int ret = 0;
    struct sigaction sa;
    char cmd[64];

    ret = initializeBluetoothEvent();
    if (ret < 0) {
        printf("Failed to initialize bluetooth eventloop\n");
        return -1;
    }
    ret = startEventLoop();
    if (ret < 0) {
        printf("Failed to start bluetooth eventloop\n");
        goto exit;
    }
    ret = initServices();
    if (ret < 0) {
        printf("Failed to init bluetooth service\n");
        goto exit;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_NOCLDSTOP;
    sa.sa_handler = sig_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    sleep(3);
    //addProfile("/foo/bar/profile0", "0000110b-0000-1000-8000-00805f9b34fb", "a2dp_sink", 0);
    //addProfile("/foo/bar/profile1", "0000110a-0000-1000-8000-00805f9b34fb", "a2dp_souce", 0);
    //addProfile("/foo/bar/profile2", "0000110e-0000-1000-8000-00805f9b34fb", "avrcp_control", 0);
    //addProfile("/foo/bar/profile3", "0000110c-0000-1000-8000-00805f9b34fb", "avrcp_control", 0);

    
    while (!terminate) {
        memset(cmd, 0, sizeof(cmd));
        scanf("%s", cmd);
        if (strstr(cmd, "start_scan")) {
            startDiscovery();
        } else if (strstr(cmd, "stop_scan")) {
            stopDiscovery();
        } if (strstr(cmd, "pair")) {
            startPaireDevice(DEFAULT_DEVICE_PATH);
        } else if (strstr(cmd, "cancle_pair")) {

        } else if (strstr(cmd, "connect_all")) {
            connectDevice(DEFAULT_DEVICE_PATH);
        } else if (strstr(cmd, "hfp-ag")) {
            connectProfile(DEFAULT_DEVICE_PATH, "hfp-ag");
        } else if (strstr(cmd, "audio-sink")) {
            //connectProfile(DEFAULT_DEVICE_PATH, "0000110b-0000-1000-8000-00805f9b34fb");//a2dp sink
            //connectProfile(DEFAULT_DEVICE_PATH, "0000110e-0000-1000-8000-00805f9b34fb");//avrcp remote
            connectProfile(DEFAULT_DEVICE_PATH, "0000110a-0000-1000-8000-00805f9b34fb");//a2dp souce
            connectProfile(DEFAULT_DEVICE_PATH, "0000110c-0000-1000-8000-00805f9b34fb");//avrcp control
        } else if (strstr(cmd, "Play")) {
            mediaPlayerControl("dev_50_8F_4C_E5_04_D9", "Play");
        } else if (strstr(cmd, "Pause")) {
            mediaPlayerControl("dev_50_8F_4C_E5_04_D9", "Pause");
        }
    }

exit:
    destoryServices();
    stopEventLoop();
    cleanupBluetoothEvent();
}

