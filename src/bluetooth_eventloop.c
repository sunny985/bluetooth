#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <stdlib.h>
#include <dbus/dbus.h>

#include "bluetooth_eventloop.h"
#include "bluetooth_common.h"

/************************** bluetooth eventloop *********************************/
#define EVENT_LOOP_EXIT 1
#define EVENT_LOOP_ADD  2
#define EVENT_LOOP_REMOVE 3
#define EVENT_LOOP_WAKEUP 4

typedef struct event_loop_native_data_t {
    DBusConnection *conn;
    const char *adapter;

    /* protects the thread */
    pthread_mutex_t thread_mutex;
    pthread_t thread;
    /* our comms socket */
    /* mem for the list of sockets to listen to */
    struct pollfd *pollData;
    int pollMemberCount;
    int pollDataSize;
    /* mem for matching set of dbus watch ptrs */
    DBusWatch **watchData;
    /* pair of sockets for event loop control, Reader and Writer */
    int controlFdR;
    int controlFdW;
    /* flag to indicate if the event loop thread is running */
    int running;
}tBluetoothEvent;

static tBluetoothEvent * g_bluetooth_evt = NULL;

static void tearDownEventLoop(tBluetoothEvent *nat);
static DBusHandlerResult event_filter(DBusConnection *conn, DBusMessage *msg,
                                      void *data);
static int setUpEventLoop(tBluetoothEvent *nat);
static int register_agent(tBluetoothEvent * nat,
                          const char *agent_path, const char *capabilities);
static void unregister_agent(tBluetoothEvent * nat,
                          const char *agent_path);

static unsigned int unix_events_to_dbus_flags(short events) {
    return (events & DBUS_WATCH_READABLE ? POLLIN : 0) |
         (events & DBUS_WATCH_WRITABLE ? POLLOUT : 0) |
         (events & DBUS_WATCH_ERROR ? POLLERR : 0) |
         (events & DBUS_WATCH_HANGUP ? POLLHUP : 0);
}

static short dbus_flags_to_unix_events(unsigned int flags) {
    return (flags & POLLIN ? DBUS_WATCH_READABLE : 0) |
         (flags & POLLOUT ? DBUS_WATCH_WRITABLE : 0) |
         (flags & POLLERR ? DBUS_WATCH_ERROR : 0) |
         (flags & POLLHUP ? DBUS_WATCH_HANGUP : 0);
}

dbus_bool_t dbusAddWatch(DBusWatch *watch, void *data) {
    tBluetoothEvent * nat = (tBluetoothEvent *)data;

    if (dbus_watch_get_enabled(watch)) {
        // note that we can't just send the watch and inspect it later
        // because we may get a removeWatch call before this data is reacted
        // to by our eventloop and remove this watch..  reading the add first
        // and then inspecting the recently deceased watch would be bad.
        char control = EVENT_LOOP_ADD;
        write(nat->controlFdW, &control, sizeof(char));

        /*remove a warning, if you want to see the warning ,open the mark*/
        int fd = dbus_watch_get_fd(watch);
        //int fd = dbus_watch_get_unix_fd(watch);
        write(nat->controlFdW, &fd, sizeof(int));

        unsigned int flags = dbus_watch_get_flags(watch);
        write(nat->controlFdW, &flags, sizeof(unsigned int));

        write(nat->controlFdW, &watch, sizeof(DBusWatch*));
    }
    return 1;
}

void dbusRemoveWatch(DBusWatch *watch, void *data) {
    tBluetoothEvent * nat = (tBluetoothEvent *)data;

    char control = EVENT_LOOP_REMOVE;
    write(nat->controlFdW, &control, sizeof(char));

    /*remove a warning, if you want to see the warning ,open the mark*/
    int fd = dbus_watch_get_fd(watch);
    //int fd = dbus_watch_get_unix_fd(watch);
    write(nat->controlFdW, &fd, sizeof(int));

    unsigned int flags = dbus_watch_get_flags(watch);
    write(nat->controlFdW, &flags, sizeof(unsigned int));
}

void dbusToggleWatch(DBusWatch *watch, void *data) {
    if (dbus_watch_get_enabled(watch)) {
        dbusAddWatch(watch, data);
    } else {
        dbusRemoveWatch(watch, data);
    }
}

void dbusWakeup(void *data) {
    tBluetoothEvent * nat = (tBluetoothEvent *)data;

    char control = EVENT_LOOP_WAKEUP;
    write(nat->controlFdW, &control, sizeof(char));
}

static void handleWatchAdd(tBluetoothEvent *nat) {
    DBusWatch *watch;
    int newFD,y;
    unsigned int flags;

    read(nat->controlFdR, &newFD, sizeof(int));
    read(nat->controlFdR, &flags, sizeof(unsigned int));
    read(nat->controlFdR, &watch, sizeof(DBusWatch *));
    short events = dbus_flags_to_unix_events(flags);
    for (y = 0; y<nat->pollMemberCount; y++) {
        if ((nat->pollData[y].fd == newFD) &&
                (nat->pollData[y].events == events)) {
            printf("DBusWatch duplicate add");
            return;
        }
    }
    if (nat->pollMemberCount == nat->pollDataSize) {
        printf("Bluetooth EventLoop poll struct growing");
        struct pollfd *temp = (struct pollfd *)malloc(
                sizeof(struct pollfd) * (nat->pollMemberCount+1));
        if (!temp) {
            return;
        }
        memcpy(temp, nat->pollData, sizeof(struct pollfd) *
                nat->pollMemberCount);
        free(nat->pollData);
        nat->pollData = temp;
        DBusWatch **temp2 = (DBusWatch **)malloc(sizeof(DBusWatch *) *
                (nat->pollMemberCount+1));
        if (!temp2) {
            return;
        }
        memcpy(temp2, nat->watchData, sizeof(DBusWatch *) *
                nat->pollMemberCount);
        free(nat->watchData);
        nat->watchData = temp2;
        nat->pollDataSize++;
    }
    nat->pollData[nat->pollMemberCount].fd = newFD;
    nat->pollData[nat->pollMemberCount].revents = 0;
    nat->pollData[nat->pollMemberCount].events = events;
    nat->watchData[nat->pollMemberCount] = watch;
    nat->pollMemberCount++;
}

static void handleWatchRemove(tBluetoothEvent *nat) {
    int removeFD,y;
    unsigned int flags;

    read(nat->controlFdR, &removeFD, sizeof(int));
    read(nat->controlFdR, &flags, sizeof(unsigned int));
    short events = dbus_flags_to_unix_events(flags);

    for (y = 0; y < nat->pollMemberCount; y++) {
        if ((nat->pollData[y].fd == removeFD) &&
                (nat->pollData[y].events == events)) {
            int newCount = --nat->pollMemberCount;
            // copy the last live member over this one
            nat->pollData[y].fd = nat->pollData[newCount].fd;
            nat->pollData[y].events = nat->pollData[newCount].events;
            nat->pollData[y].revents = nat->pollData[newCount].revents;
            nat->watchData[y] = nat->watchData[newCount];
            return;
        }
    }
    printf("WatchRemove given with unknown watch");
}

static void *eventLoopMain(void *ptr) {
    int i = 0;
    tBluetoothEvent *nat = (tBluetoothEvent *)ptr;

    dbus_connection_set_watch_functions(nat->conn, dbusAddWatch,
            dbusRemoveWatch, dbusToggleWatch, ptr, NULL);
    dbus_connection_set_wakeup_main_function(nat->conn, dbusWakeup, ptr, NULL);
    nat->running = 1;

    while (1) {
        for (i = 0; i < nat->pollMemberCount; i++) {
            if (!nat->pollData[i].revents) {
                continue;
            }
            if (nat->pollData[i].fd == nat->controlFdR) {
                char data;
                while (recv(nat->controlFdR, &data, sizeof(char), MSG_DONTWAIT)
                        != -1) {
                    switch (data) {
                    case EVENT_LOOP_EXIT:
                    {
                        dbus_connection_set_watch_functions(nat->conn,
                                NULL, NULL, NULL, NULL, NULL);
                        tearDownEventLoop(nat);
                        int fd = nat->controlFdR;
                        nat->controlFdR = 0;
                        close(fd);
                        return NULL;
                    }
                    case EVENT_LOOP_ADD:
                    {
                        handleWatchAdd(nat);
                        break;
                    }
                    case EVENT_LOOP_REMOVE:
                    {
                        handleWatchRemove(nat);
                        break;
                    }
                    case EVENT_LOOP_WAKEUP:
                    {
                        // noop
                        break;
                    }
                    }
                }
            } else {
                short events = nat->pollData[i].revents;
                unsigned int flags = unix_events_to_dbus_flags(events);
                dbus_watch_handle(nat->watchData[i], flags);
                nat->pollData[i].revents = 0;
                // can only do one - it may have caused a 'remove'
                break;
            }
        }
        while (dbus_connection_dispatch(nat->conn) ==
                DBUS_DISPATCH_DATA_REMAINS) {
        }
        poll(nat->pollData, nat->pollMemberCount, -1);
    }
}


static void tearDownEventLoop(tBluetoothEvent *nat){
    if (nat != NULL && nat->conn != NULL) {
        DBusError err;
        dbus_error_init(&err);
        const char * agent_path = LOCAL_AGENT_PATH;
        unregister_agent(nat, agent_path);

        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Device1'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }

        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Adapter1'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }

        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".MediaControl1'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }

        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='org.freedesktop.DBus.ObjectManager'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }

        dbus_bus_remove_match(nat->conn,
                "type='signal',interface='org.freedesktop.DBus'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }

        dbus_connection_remove_filter(nat->conn, event_filter, nat);
    }
}

int initializeBluetoothEvent(){
    g_bluetooth_evt = (tBluetoothEvent *)calloc(1, sizeof(tBluetoothEvent));
    tBluetoothEvent *nat = g_bluetooth_evt;
    if (NULL == nat) {
        printf("%s: out of memory!", __FUNCTION__);
        return -1;
    }
    memset(nat, 0, sizeof(tBluetoothEvent));
    pthread_mutex_init(&(nat->thread_mutex), NULL);

    {
        DBusError err;
        dbus_error_init(&err);
        dbus_threads_init_default();
        nat->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
        if (dbus_error_is_set(&err)){
            printf("%s: Could not get onto the system bus!", __FUNCTION__);
            dbus_error_free(&err);
            return -1;
        }
        dbus_connection_set_exit_on_disconnect(nat->conn, FALSE);
        printf("event dbus <0x%x>\n", nat->conn);
    }
    return 0;
}

void cleanupBluetoothEvent() {
    tBluetoothEvent *nat = g_bluetooth_evt;
    if (nat) {
        pthread_mutex_destroy(&(nat->thread_mutex));
        free(nat);
        g_bluetooth_evt = NULL;
    }
}

int startEventLoop(){
    int result = -1;

    tBluetoothEvent *nat = g_bluetooth_evt;

    pthread_mutex_lock(&(nat->thread_mutex));

    nat->running = 0;

    if (nat->pollData) {
        printf("trying to start EventLoop a second time!");
        pthread_mutex_unlock( &(nat->thread_mutex) );
        return result;
    }

    nat->pollData = (struct pollfd *)malloc(sizeof(struct pollfd) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    if (!nat->pollData) {
        printf("out of memory error starting EventLoop!");
        goto done;
    }

    nat->watchData = (DBusWatch **)malloc(sizeof(DBusWatch *) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    if (!nat->watchData) {
        printf("out of memory error starting EventLoop!");
        goto done;
    }

    memset(nat->pollData, 0, sizeof(struct pollfd) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    memset(nat->watchData, 0, sizeof(DBusWatch *) *
            DEFAULT_INITIAL_POLLFD_COUNT);
    nat->pollDataSize = DEFAULT_INITIAL_POLLFD_COUNT;
    nat->pollMemberCount = 1;

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, &(nat->controlFdR))) {
        printf("Error getting BT control socket");
        goto done;
    }
    nat->pollData[0].fd = nat->controlFdR;
    nat->pollData[0].events = POLLIN;

    if (setUpEventLoop(nat) < 0) {
        printf("failure setting up Event Loop!");
        goto done;
    }

    pthread_create(&(nat->thread), NULL, eventLoopMain, nat);
    result = 0;

done:
    if (-1 == result) {
        if (nat->controlFdW) {
            close(nat->controlFdW);
            nat->controlFdW = 0;
        }
        if (nat->controlFdR) {
            close(nat->controlFdR);
            nat->controlFdR = 0;
        }
        if (nat->pollData) free(nat->pollData);
        nat->pollData = NULL;
        if (nat->watchData) free(nat->watchData);
        nat->watchData = NULL;
        nat->pollDataSize = 0;
        nat->pollMemberCount = 0;
    }

    pthread_mutex_unlock(&(nat->thread_mutex));

    return result;
}

void stopEventLoop(){
    tBluetoothEvent *nat = g_bluetooth_evt;

    pthread_mutex_lock(&(nat->thread_mutex));
    if (nat->pollData) {
        char data = EVENT_LOOP_EXIT;
        write(nat->controlFdW, &data, sizeof(char));
        void *ret;
        pthread_join(nat->thread, &ret);

        free(nat->pollData);
        nat->pollData = NULL;
        free(nat->watchData);
        nat->watchData = NULL;
        nat->pollDataSize = 0;
        nat->pollMemberCount = 0;

        int fd = nat->controlFdW;
        nat->controlFdW = 0;
        close(fd);
    }
    nat->running = 0;
    pthread_mutex_unlock(&(nat->thread_mutex));
}

static int setUpEventLoop(tBluetoothEvent *nat){
    DBusError err;
    const char *agent_path = LOCAL_AGENT_PATH;
    const char *capabilities = "DisplayYesNo";
    if(nat != NULL && nat->conn != NULL){
        dbus_error_init(&err);
        if (register_agent(nat, agent_path, capabilities) < 0) {
            dbus_connection_unregister_object_path (nat->conn, agent_path);
            return -1;
        } 

        // Add a filter for all incoming messages
        if (!dbus_connection_add_filter(nat->conn, event_filter, nat, NULL)){
            return -1;
        }

        // Set which messages will be processed by this dbus connection
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='org.freedesktop.DBus'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return -1;
        }

/*        dbus_bus_add_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Manager'",
                &err);*/
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='org.freedesktop.DBus.ObjectManager'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return -1;
        }
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Adapter1'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return -1;
        }
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".Device1'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return -1;
        }
        dbus_bus_add_match(nat->conn,
                "type='signal',interface='"BLUEZ_DBUS_BASE_IFC".MediaControl1'",
                &err);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
            return -1;
        }
    }
    return 0;
}

static int interface_added(DBusMessage *msg) {
	const char *path;
    DBusMessageIter iter = { 0 }, subiter = { 0 };
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_iter_init(msg, &iter))
        goto failure;
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
		goto failure;

	dbus_message_iter_get_basic(&iter, &path);
    printf("path = %s\n", path);

	/* a{sa{sv}} */
	dbus_message_iter_next(&iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
		dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_DICT_ENTRY)
		goto failure;
	dbus_message_iter_recurse(&iter, &subiter);

	while (dbus_message_iter_get_arg_type(&subiter) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter value, entry;
		const char *key;

		dbus_message_iter_recurse(&subiter, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		printf("interface_added key <%s>\n", key);
		dbus_message_iter_next(&entry);
		//dbus_message_iter_recurse(&entry, &value);

		//if (parse_ext_opt(ext, key, &value) < 0)
		//	error("Invalid value for profile option %s", key);

		dbus_message_iter_next(&subiter);
	}
	
    return 0;
failure:
    LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, msg);
    return -1;
}

static DBusHandlerResult event_filter(DBusConnection *conn, DBusMessage *msg,
                                      void *data){
    DBusError err;
    dbus_error_init(&err);
    const char *agent_path = LOCAL_AGENT_PATH;
    const char *capabilities = "DisplayYesNo";
    tBluetoothEvent *nat = g_bluetooth_evt;

    printf("event filter\n");
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
        printf("%s: not interested (not a signal).\n", __FUNCTION__);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_is_signal(msg,
                                      "org.bluez.Adapter1",
                                      "PropertyChanged")) {
        t_property_value_array str_array;
        memset(&str_array,0,sizeof(str_array));
        parse_adapter_property_change(msg, &str_array);
        print_property_value(&str_array);
        free_property_value(&str_array);
    }else if (dbus_message_is_signal(msg,
                                      "org.bluez.Device1",
                                      "PropertyChanged")) {
        t_property_value_array str_array;
        const char *remote_device_path;
        memset(&str_array,0,sizeof(str_array));
        parse_remote_device_property_change(msg,&str_array);
        print_property_value(&str_array);
        //remote_device_path = dbus_message_get_path(msg);
        //if (gDbusEvtHdl) gDbusEvtHdl(DEVICE_PROPERTY_CHANGE, remote_device_path, &str_array);
        free_property_value(&str_array);        
    }else if (dbus_message_is_signal(msg,
                                      "org.freedesktop.DBus.ObjectManager",
                                      "InterfacesAdded")) {
        
        printf("Interfaces added\n");
        interface_added(msg);
    } else if (dbus_message_is_signal(msg,
                                      "org.freedesktop.DBus.ObjectManager",
                                      "InterfacesRemoved")) {
        printf("Interfaces removed\n");
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}


#define AGENT_INTERFACE "org.bluez.Agent1"

static char * passkey_value = "0000";

DBusHandlerResult agent_event_filter(DBusConnection *conn,
                                     DBusMessage *msg, void *data){
    tBluetoothEvent *nat = g_bluetooth_evt;
    printf("%s\n", __FUNCTION__);
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        printf("%s: not interested (not a method call).", __FUNCTION__);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "Cancel")){
        printf("%s: Cancel\n", __FUNCTION__);
		DBusMessage *reply = dbus_message_new_method_return(msg);
		if (!reply) {
			printf("%s: Cannot create message reply\n", __FUNCTION__);
			goto failure;
		}
		dbus_connection_send(nat->conn, reply, NULL);
		dbus_message_unref(reply);
		goto success;
    }else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "Authorize")) {
        printf("%s: Authorize\n", __FUNCTION__);
    }else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "RequestPinCode")) {
        printf("%s: RequestPinCode\n", __FUNCTION__);
        
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) {
            printf("%s: Cannot create message reply\n", __FUNCTION__);
            goto failure;
        }
        dbus_message_append_args(reply, DBUS_TYPE_STRING, &passkey_value,
							DBUS_TYPE_INVALID);
        dbus_connection_send(nat->conn, reply, NULL);
        dbus_message_unref(reply);
        goto success;
    }else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "RequestPasskey")) {
        printf("%s: RequestPasskey\n", __FUNCTION__);
    }else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "DisplayPasskey")) {
        printf("%s: DisplayPasskey\n", __FUNCTION__);
    }else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "RequestConfirmation")) {
        printf("%s: RequestConfirmation\n", __FUNCTION__);
        char *object_path;
        uint32_t passkey;
        if (!dbus_message_get_args(msg, NULL,
                               DBUS_TYPE_OBJECT_PATH, &object_path,
                               DBUS_TYPE_UINT32, &passkey,
                               DBUS_TYPE_INVALID)) {
            printf("%s: Invalid arguments for RequestConfirmation() method\n", __FUNCTION__);
            goto failure;
        }
       printf("%s: RequestConfirmation passkey <%d>\n", __FUNCTION__, passkey);
    } else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "Release")) {
        printf("%s: Release\n");
        DBusMessage *reply = dbus_message_new_method_return(msg);
        if (!reply) {
            printf("%s: Cannot create message reply\n", __FUNCTION__);
            goto failure;
        }
        dbus_connection_send(nat->conn, reply, NULL);
        dbus_message_unref(reply);
        goto success;
    } else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "AuthorizeService")) {
        printf("%s: AuthorizeService\n", __FUNCTION__);
    } else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "RequestAuthorization")) {
        printf("%s: RequestAuthorization\n", __FUNCTION__);
    } else if (dbus_message_is_method_call(msg,
            AGENT_INTERFACE, "DisplayPinCode")) { 
        printf("%s: DisplayPinCode\n", __FUNCTION__);
    } else {
        printf("%s: unknown message\n", __FUNCTION__);
    }

failure:
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
success:
    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable agent_vtable = {
    NULL, agent_event_filter, NULL, NULL, NULL, NULL
};

static int register_agent(tBluetoothEvent * nat,
                          const char *agent_path, const char *capabilities){
    DBusMessage *msg, *reply;
    DBusError err;

    if (!dbus_connection_register_object_path(nat->conn, agent_path,
            &agent_vtable, NULL)) {
        printf("%s: Can't register object path %s for agent!",
              __FUNCTION__, agent_path);
        return -1;
    }

    msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
          "org.bluez.AgentManager1", "RegisterAgent");
    if (!msg) {
        printf("%s: Can't allocate new method call for agent!",
              __FUNCTION__);
        return -1;
    }
    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &agent_path,
                             DBUS_TYPE_STRING, &capabilities,
                             DBUS_TYPE_INVALID);

    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(nat->conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (!reply) {
        printf("%s: Can't register agent!", __FUNCTION__);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        return -1;
    }

    msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
          "org.bluez.AgentManager1", "RequestDefaultAgent");
    if (!msg) {
        printf("%s: Can't allocate new method call for agent!",
              __FUNCTION__);
        return -1;
    }
    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &agent_path,
                             DBUS_TYPE_INVALID);

    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(nat->conn, msg, -1, &err);
    dbus_message_unref(msg);

    if (!reply) {
        printf("%s: Can't request default agent!", __FUNCTION__);
        if (dbus_error_is_set(&err)) {
            LOG_AND_FREE_DBUS_ERROR(&err);
        }
        return -1;
    }

    dbus_message_unref(reply);
    dbus_connection_flush(nat->conn);

    return 0;
}

static void unregister_agent(tBluetoothEvent * nat,
                          const char *agent_path){
    DBusMessage *msg, *reply;
    DBusError err;
    dbus_error_init(&err);

    msg = dbus_message_new_method_call("org.bluez",
                                       "org.bluez",
                                       "org.bluez.AgentManager1",
                                       "UnregisterAgent");
    if (msg != NULL) {
        dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &agent_path,
                                 DBUS_TYPE_INVALID);
        reply = dbus_connection_send_with_reply_and_block(nat->conn,
                                                      msg, -1, &err);

        if (!reply) {
            if (dbus_error_is_set(&err)) {
                LOG_AND_FREE_DBUS_ERROR(&err);
                dbus_error_free(&err);
            }
        } else {
            dbus_message_unref(reply);
        }
        dbus_message_unref(msg);
    } else {
        printf("%s: Can't create new method call!", __FUNCTION__);
    }
    dbus_connection_flush(nat->conn);
    dbus_connection_unregister_object_path(nat->conn, agent_path);
}

