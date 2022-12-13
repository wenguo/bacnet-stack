/**
 * @file
 * @brief Samble BACNet/SC hub.
 * @author Mikhail Antropov
 * @date December 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bacnet/config.h"
#include "bacnet/apdu.h"
#include "bacnet/iam.h"
#include "bacnet/version.h"
#include "bacnet/basic/services.h"
#include "bacnet/datalink/dlenv.h"
#include "bacnet/basic/sys/filename.h"
#include "bacnet/datalink/datalink.h"
/* include the device object */
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/bacfile.h"
#if defined(BAC_UCI)
#include "bacnet/basic/ucix/ucix.h"
#endif /* defined(BAC_UCI) */
#include "bacnet/basic/object/netport.h"
#include "bacnet/basic/object/sc_netport.h"
#include "bacnet/datalink/bsc/bsc-datalink.h"
#include "bacnet/datalink/bsc/bsc-event.h"

static uint8_t *Ca_Certificate = NULL;
static uint8_t *Certificate = NULL;
static uint8_t *Key = NULL;
static char *PrimaryUrl = "wss://127.0.0.1:9999";
static char *FailoverUrl = "wss://127.0.0.1:9999";

#define SC_NETPORT_BACFILE_START_INDEX    0

/* (Doxygen note: The next two lines pull all the following Javadoc
 *  into the ServerDemo module.) */
/** @addtogroup SCServerDemo */
/*@{*/

#ifndef BACDL_BSC
#error "BACDL_BSC must de defined"
#endif
#ifndef BACFILE
#error "BACFILE must de defined"
#endif

/* current version of the BACnet stack */
static const char *BACnet_Version = BACNET_VERSION_TEXT;

/** Initialize the handlers we will utilize.
 * @see Device_Init, apdu_set_unconfirmed_handler, apdu_set_confirmed_handler
 */
static void Init_Service_Handlers(void)
{
    Device_Init(NULL);
   /* set the handler for all the services we don't implement
       It is required to send the proper reject message... */
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    /* we must implement read property - it's required! */
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    /* handle the reply (request) coming back */
    //apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, my_i_am_handler);
}

static void print_usage(const char *filename)
{
    printf("Usage: %s port ca-cert cert key [device-instance [device-name]]\n", filename);
    printf("       [--version][--help]\n");
}

static void print_help(const char *filename)
{
    printf("Simulate a BACnet/SC HUB device\n"
           "port: Local port\n"
           "ca-cert: Filename of CA certificate\n"
           "cert: Filename of device certificate\n"
           "key: Filename of device certificate key\n"
           "device-instance: BACnet Device Object Instance number that you are\n"
           "trying simulate.\n"
           "device-name: The Device object-name is the text name for the device.\n"
           "\nExample:\n");
    printf("To simulate Device 123 on port #50000, use following command:\n"
           "%s 50000 ca_cert.pem cert.pem key.pem 123\n",
        filename);
    printf("To simulate Device 123 named Fred on port #50000, use following command:\n"
           "%s 50000 ca_cert.pem cert.pem key.pem 123 Fred\n",
        filename);
}

static uint32_t read_file(char *filename, uint8_t **buff)
{
    uint32_t size = 0;
    FILE *pFile;

    pFile = fopen(filename, "rb");
    if (pFile) {
        fseek(pFile, 0L, SEEK_END);
        size = ftell(pFile);
        fseek(pFile, 0, SEEK_SET);

        *buff = (uint8_t *)malloc(size);
        if (*buff != NULL) {
            if (fread(*buff, size, 1, pFile) == 0) {
                size = 0;
            }
        }
        fclose(pFile);
    }
    return *buff ? size : 0;
}

static bool init_bsc(uint16_t port, char *filename_ca_cert, char *filename_cert,
    char *filename_key)
{
    uint32_t instance = 1;
    uint32_t size;

    Network_Port_Object_Instance_Number_Set(0, instance);

    size = read_file(filename_ca_cert, &Ca_Certificate);
    Network_Port_Issuer_Certificate_File_Set_From_Memory(instance, 0,
        Ca_Certificate, size, SC_NETPORT_BACFILE_START_INDEX);

    size = read_file(filename_cert, &Certificate);
    Network_Port_Operational_Certificate_File_Set_From_Memory(instance,
        Certificate, size, SC_NETPORT_BACFILE_START_INDEX + 1);

    size = read_file(filename_key, &Key);
    Network_Port_Certificate_Key_File_Set_From_Memory(instance,
        Key, size, SC_NETPORT_BACFILE_START_INDEX + 2);

    Network_Port_SC_Primary_Hub_URI_Set(instance, PrimaryUrl);
    Network_Port_SC_Failover_Hub_URI_Set(instance, FailoverUrl);

    Network_Port_SC_Direct_Connect_Initiate_Enable_Set(instance, false);
    Network_Port_SC_Direct_Connect_Accept_Enable_Set(instance,  true);
    // TODO: get this param from command line
    Network_Port_SC_Direct_Server_Port_Set(instance, 9999);
    Network_Port_SC_Hub_Function_Enable_Set(instance, true);
    Network_Port_SC_Hub_Server_Port_Set(instance, port);

    return true;
}

/** Main function of server demo.
 *
 * @see Device_Set_Object_Instance_Number, dlenv_init, Send_I_Am,
 *      datalink_receive, npdu_handler,
 *      dcc_timer_seconds, datalink_maintenance_timer,
 *      Load_Control_State_Machine_Handler, handler_cov_task,
 *      tsm_timer_milliseconds
 *
 * @param argc [in] Arg count.
 * @param argv [in] Takes one argument: the Device Instance #.
 * @return 0 on success.
 */
int main(int argc, char *argv[])
{
#if defined(BAC_UCI)
    int uciId = 0;
    struct uci_context *ctx;
#endif
    int argi = 0;
    const char *filename = NULL;

    uint16_t port = 0;
    char *filename_ca_cert = NULL;
    char *filename_cert = NULL;
    char *filename_key = NULL;

    filename = filename_remove_path(argv[0]);
    argi = 1;
    if ((argc < 2) || (strcmp(argv[argi], "--help") == 0)) {
        print_usage(filename);
        print_help(filename);
        return 0;
    }
    if (strcmp(argv[argi], "--version") == 0) {
        printf("%s %s\n", filename, BACNET_VERSION_TEXT);
        printf("Copyright (C) 2022 by Steve Karg and others.\n"
               "This is free software; see the source for copying "
               "conditions.\n"
               "There is NO warranty; not even for MERCHANTABILITY or\n"
               "FITNESS FOR A PARTICULAR PURPOSE.\n");
        return 0;
    }
    port = strtol(argv[argi], NULL, 0);
    if (++argi < argc) {
        filename_ca_cert = argv[argi];
    }
    if (++argi < argc) {
        filename_cert = argv[argi];
    }
    if (++argi < argc) {
        filename_key = argv[argi];
    }

#if defined(BAC_UCI)
    ctx = ucix_init("bacnet_dev");
    if (!ctx)
        fprintf(stderr, "Failed to load config file bacnet_dev\n");
    uciId = ucix_get_option_int(ctx, "bacnet_dev", "0", "Id", 0);
    if (uciId != 0) {
        Device_Set_Object_Instance_Number(uciId);
    } else {
#endif /* defined(BAC_UCI) */
        /* allow the device ID to be set */
        if (++argi < argc) {
            Device_Set_Object_Instance_Number(strtol(argv[argi], NULL, 0));
        }

#if defined(BAC_UCI)
    }
    ucix_cleanup(ctx);
#endif /* defined(BAC_UCI) */

    printf("BACnet SC Hub Demo\n"
           "BACnet Stack Version %s\n"
           "BACnet Device ID: %u\n"
           "Max APDU: %d\n",
        BACnet_Version, Device_Object_Instance_Number(), MAX_APDU);
    /* load any static address bindings to show up
       in our device bindings list */
    address_init();
    Init_Service_Handlers();
#if defined(BAC_UCI)
    const char *uciname;
    ctx = ucix_init("bacnet_dev");
    if (!ctx)
        fprintf(stderr, "Failed to load config file bacnet_dev\n");
    uciname = ucix_get_option(ctx, "bacnet_dev", "0", "Name");
    if (uciname != 0) {
        Device_Object_Name_ANSI_Init(uciname);
    } else {
#endif /* defined(BAC_UCI) */
        if (++argi < argc) {
            Device_Object_Name_ANSI_Init(argv[argi]);
        }
#if defined(BAC_UCI)
    }
    ucix_cleanup(ctx);
#endif /* defined(BAC_UCI) */
    BACNET_CHARACTER_STRING DeviceName;
    if (Device_Object_Name(Device_Object_Instance_Number(),&DeviceName)) {
        printf("BACnet Device Name: %s\n", DeviceName.value);
    }

    if (!init_bsc(port, filename_ca_cert, filename_cert, filename_key)) {
        goto exit;
    }

    dlenv_init();
    atexit(datalink_cleanup);

    /* broadcast an I-Am on startup */
    //Send_I_Am(&Handler_Transmit_Buffer[0]);
    /* loop forever */
    for (;;) {
        /* input */
        bsc_wait(1);
        /* output */

        /* blink LEDs, Turn on or off outputs, etc */
    }

exit:
    free(Ca_Certificate);
    free(Certificate);
    free(Key);

    return 0;
}

/* @} */

/* End group SCServerDemo */
