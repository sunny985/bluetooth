#include <stdlib.h>

#include "bluetooth_service.h"
#include "bluetooth_common.h"

static DBusConnection * g_dbus_conn = NULL;
extern DBusHandlerResult agent_event_filter(DBusConnection *conn,
                                            DBusMessage *msg,
                                            void *data);

// This function is called when the adapter is enabled.
static int setupRemoteAgent(DBusConnection *conn) {
    // Register agent for remote devices. 
    const char *device_agent_path = REMOTE_AGENT_PATH;
    static const DBusObjectPathVTable agent_vtable = {
                NULL, agent_event_filter, NULL, NULL, NULL, NULL };

    if (!dbus_connection_register_object_path(conn, device_agent_path,
                                            &agent_vtable, NULL)) {
        printf("%s: Can't register object path %s for remote device agent!",
                             __FUNCTION__, device_agent_path);
        return -1;
    }
    return 0;
}

static int tearDownRemoteAgent(DBusConnection *conn) {

    const char *device_agent_path = REMOTE_AGENT_PATH;
    dbus_connection_unregister_object_path (conn, device_agent_path);
    return 0;
}

int initServices(){
    g_dbus_conn = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
    if(!g_dbus_conn) return -1;
    setupRemoteAgent(g_dbus_conn);
    return 0;
}

int destoryServices(){
    if(g_dbus_conn){
        tearDownRemoteAgent(g_dbus_conn);
        dbus_connection_unref(g_dbus_conn);
        g_dbus_conn = NULL;
    }
    return 0;
}

/*
* start bluetooth discovery
*/
static int _startDiscovery(DBusConnection *conn){
    DBusMessage *msg = NULL;
    DBusMessage *reply = NULL;
    DBusError err;
    int ret = -1;
    if (!conn) return ret;

    dbus_error_init(&err);
    /* Compose the command */
    msg = dbus_message_new_method_call(BLUEZ_DBUS_BASE_IFC,
                                       ADAPTER_PATH,
                                       ADAPTER_IFC, "StartDiscovery");

    if (msg == NULL) {
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        }
        goto done;
    }

    /* Send the command. */
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    if (dbus_error_is_set(&err)) {
         LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
         goto done;
    }
    ret = 0;
done:
    if (reply) dbus_message_unref(reply);
    if (msg) dbus_message_unref(msg);
    return ret;
}

static int _stopDiscovery(DBusConnection *conn){
    DBusMessage *msg = NULL;
    DBusMessage *reply = NULL;
    DBusError err;
    int ret = -1;

    dbus_error_init(&err);
    if (!conn) return ret;
    /* Compose the command */
    msg = dbus_message_new_method_call(BLUEZ_DBUS_BASE_IFC,
                                       ADAPTER_PATH,
                                       ADAPTER_IFC, "StopDiscovery");
    if (msg == NULL) {
        if (dbus_error_is_set(&err))
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        goto done;
    }

    /* Send the command. */
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    if (dbus_error_is_set(&err)) {
        if(strncmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.NotAuthorized",
                   strlen(BLUEZ_DBUS_BASE_IFC ".Error.NotAuthorized")) == 0) {
            // hcid sends this if there is no active discovery to cancel
            printf("%s: There was no active discovery to cancel", __FUNCTION__);
            dbus_error_free(&err);
        } else {
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        }
        goto done;
    }

    ret = 0;
done:
    if (msg) dbus_message_unref(msg);
    if (reply) dbus_message_unref(reply);
    return ret;
}

void onStartPairDeviceResult(DBusMessage *msg, void *user, void *n) {
    int result = BOND_RESULT_SUCCESS;
    const char *address = (const char *)user;
    DBusError err;
    dbus_error_init(&err);

    if (dbus_set_error_from_message(&err, msg)) {
        if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationFailed")) {
            // Pins did not match, or remote device did not respond to pin
            // request in time
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_FAILED;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationRejected")) {
            // We rejected pairing, or the remote side rejected pairing. This
            // happens if either side presses 'cancel' at the pairing dialog.
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_REJECTED;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationCanceled")) {
            // Not sure if this happens
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_CANCELED;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.ConnectionAttemptFailed")) {
            // Other device is not responding at all
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_REMOTE_DEVICE_DOWN;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AlreadyExists")) {
            // already bonded
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_SUCCESS;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.InProgress") &&
                   !strcmp(err.message, "Bonding in progress")) {
            printf("... error = %s (%s)\n", err.name, err.message);
            goto done;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.InProgress") &&
                   !strcmp(err.message, "Discover in progress")) {
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_DISCOVERY_IN_PROGRESS;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.RepeatedAttempts")) {
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_REPEATED_ATTEMPTS;
        } else if (!strcmp(err.name, BLUEZ_DBUS_BASE_IFC ".Error.AuthenticationTimeout")) {
            printf("... error = %s (%s)\n", err.name, err.message);
            result = BOND_RESULT_AUTH_TIMEOUT;
        } else {
            printf("%s: D-Bus error: %s (%s)\n", __FUNCTION__, err.name, err.message);
            result = BOND_RESULT_ERROR;
        }
        LOG_AND_FREE_DBUS_ERROR(&err);
    }
	printf("user <%s> result <%d>\n", address, result);
done:
    if(user) free(user);
}


static int _startPaireDevice(DBusConnection *conn, const char *device_path) {
    int len = strlen(device_path) + 1;
    char * context_path = (char*)calloc(len,sizeof(char));
    const char *capabilities = "DisplayYesNo";
    printf("dev_paht = %s\n", device_path);
    //const char *agent_path = REMOTE_AGENT_PATH;
    snprintf(context_path,len,"%s",device_path);
    int ret = dbus_func_args_async(conn, (int)5000,
                                        onStartPairDeviceResult, // callback
                                        context_path,
                                        NULL,
                                        device_path,
                                        DEVICE_IFC,
                                        "Pair",
                                        DBUS_TYPE_INVALID);

    return ret ? 0 : -1;
}

static int _connectDevice(DBusConnection *conn, const char *device_path) {
    DBusMessage *msg = NULL;
    DBusMessage *reply = NULL;
    DBusError err;
    int ret = -1;
    if (!conn) return ret;

    dbus_error_init(&err);
    /* Compose the command */
    msg = dbus_message_new_method_call(BLUEZ_DBUS_BASE_IFC,
                                       device_path,
                                       DEVICE_IFC, "Connect");

    if (msg == NULL) {
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        }
        goto done;
    }

    /* Send the command. */
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    if (dbus_error_is_set(&err)) {
         LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
         goto done;
    }
    ret = 0;
done:
    if (reply) dbus_message_unref(reply);
    if (msg) dbus_message_unref(msg);
    return ret;
}

static int _connectProfile(DBusConnection *conn, const char *device_path, char *profile) {
    DBusMessage *msg = NULL;
    DBusMessage *reply = NULL;
    DBusError err;
    int ret = -1;
    if (!conn) return ret;

    dbus_error_init(&err);
    /* Compose the command */
    msg = dbus_message_new_method_call(BLUEZ_DBUS_BASE_IFC,
                                       device_path,
                                       DEVICE_IFC, "ConnectProfile");

    if (msg == NULL) {
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        }
        goto done;
    }

    dbus_message_append_args(msg, DBUS_TYPE_STRING, &profile, DBUS_TYPE_INVALID);

    /* Send the command. */
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    if (dbus_error_is_set(&err)) {
         LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
         goto done;
    }
    ret = 0;
done:
    if (reply) dbus_message_unref(reply);
    if (msg) dbus_message_unref(msg);
    return ret;
}

/* "/org/bluez", "org.bluez.ProfileManager1", RegisterProfile */
static int _addProfile(DBusConnection *conn, char *path, char *uuid, char *name, int auto_connect) {
    DBusMessage *msg = NULL;
    DBusMessage *reply = NULL;
    DBusError err;
    int ret = -1;
    if (!conn) return ret;

    dbus_error_init(&err);
    /* Compose the command */
    msg = dbus_message_new_method_call(BLUEZ_DBUS_BASE_IFC,
                                       BLUEZ_DBUS_BASE_PATH,
                                       PROFILE_MANAGER_IFC, "RegisterProfile");

    if (msg == NULL) {
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
        }
        goto done;
    }

    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &uuid, DBUS_TYPE_INVALID);

    /* append arguments */
    append_dict_args(msg,
                     "Name", DBUS_TYPE_STRING, &name,
                     "AutoConnect", DBUS_TYPE_BOOLEAN, &auto_connect,
                     DBUS_TYPE_INVALID);


    /* Send the command. */
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    if (dbus_error_is_set(&err)) {
         LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
         goto done;
    }
    ret = 0;
done:
    if (reply) dbus_message_unref(reply);
    if (msg) dbus_message_unref(msg);
    return ret;
}

static int _registerMedia(DBusConnection *conn) {
    
}

/*************************************** adapter methods *************************/
int startDiscovery(){
    return _startDiscovery(g_dbus_conn);
}

int stopDiscovery() {
    return _stopDiscovery(g_dbus_conn);
}

int SetDiscoveryFilter() {

}

int removeDevice() {

}

/***************************************** device methods ***************************/
int startPaireDevice(const char *device_path) {
    return _startPaireDevice(g_dbus_conn, device_path);
}

int cacleParingDevice() {

}

/* connect any profiles the remote device 
   supports that can be connected to 
*/
int connectDevice(const char *device_path) {
    return _connectDevice(g_dbus_conn, device_path);
}

int disconnectDevice() {

}

/* This method connects a specific profile of this device
*/
int connectProfile(const char *device_path, char *profile) {
    return _connectProfile(g_dbus_conn, device_path, profile);
}

int disconnectProfile() {

}

/********************************** profile manager ****************************/
int addProfile(char *path, char *uuid, char *name, int auto_connect) {
    return _addProfile(g_dbus_conn, path, uuid, name, auto_connect);
}

/************************************ media *************************************/
static int registerMedia() {
    return _registerMedia(g_dbus_conn);
}
