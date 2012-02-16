/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2011 Code Aurora Forum
 * Copyright (C) 2012 fredvj
 *
 * Not a Contribution. Notifications and licenses are retained for attribution purposes only
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/wireless.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define LOG_TAG "SoftapController"
#include <cutils/log.h>

#include "SoftapController.h"

#include "hardware_legacy/wifi.h"
#include "libwpa_client/wpa_ctrl.h"

#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"

#ifdef HAVE_LIBC_SYSTEM_PROPERTIES
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#endif


static const char HOSTAPD_NAME[]	= "hostapd";
static const char HOSTAPD_PROP_NAME[]	= "init.svc.hostapd";

#define WIFI_AP_INTERFACE       "softap.0"
#define WIFI_AP_FOLDER		"/data/hostapd"
#define WIFI_AP_CONTROL		"/data/hostapd/softap.0"
#define WIFI_AP_CONFIG_FILE	"/data/hostapd/hostapd.conf"
#define WIFI_AP_CONFIG_TEMPLATE	"/system/etc/wifi/softap/hostapd.conf"
#define WIFI_AP_ACCEPT_LIST	"/data/hostapd/hostapd.accept"
#define WIFI_AP_DENY_LIST	"/data/hostapd/hostapd.deny"
#define WIFI_AP_MAX_LINE	256


int fexists(const char *filename) {
    struct stat status;

    if(stat(filename, &status) == 0) {
	LOGD("fexists: \"%s\" found", filename);
	return 1;
    }

    LOGE("fexists: \"%s\" not found", filename);

  return 0;
}


int wifi_start_hostapd()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 100;

    // Check whether hostapd is already running

    property_get(HOSTAPD_PROP_NAME, supp_status, NULL);

    if(strcmp(supp_status, "running") == 0 && fexists(WIFI_AP_CONTROL)) {
	LOGD("Helper: hostapd is already running ?!");
        return 0;
    }

    // Delete old control file

    unlink(WIFI_AP_CONTROL);

    // Try to start the hostapd service

    LOGD("Helper: Sending hostapd ctl.start");
    property_set("ctl.start", HOSTAPD_NAME);
    sched_yield();

    while(count-- > 0) {
        if(property_get(HOSTAPD_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "running") == 0) {
		usleep(500000);

		if(fexists(WIFI_AP_CONTROL)) {
			LOGD("Helper: hostapd is running");
			return 0;
		}
		else {
			LOGE("Helper: hostapd accepted the start signal, but socket does not exist. Demon probably died. See logs.");
			return -1;
		}
	    }
        }
	LOGD("Helper: Countdown waiting for hostapd: %d", count);

        usleep(200000);
    }
    LOGE("Helper: hostapd failed to start within 20 seconds");

    return -1;
}

int wifi_stop_hostapd()
{
    char supp_status[PROPERTY_VALUE_MAX] = {'\0'};
    int count = 30;

    /* Check whether hostapd already stopped */

    if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)
        && strcmp(supp_status, "stopped") == 0) {
	LOGD("Helper: hostapd service is already stopped ?!");
        return 0;
    }

    LOGD("Helper: Sending hostapd ctl.stop");
    property_set("ctl.stop", HOSTAPD_NAME);
    sched_yield();

    while (count-- > 0) {
        if (property_get(HOSTAPD_PROP_NAME, supp_status, NULL)) {
            if (strcmp(supp_status, "stopped") == 0) {
		LOGD("Helper: hostapd successfully stopped");
                return 0;
	    }
        }
	LOGD("Helper: Countdown waiting for hostapd: %d", count);

        usleep(200000);
    }
    LOGE("Helper: hostapd failed to stop within 6 seconds");

    return -1;
}


int wifi_configuration_file(void)
{
    FILE *scf, *stf;
    int c;

    // Check if we can open the config file

    scf = fopen (WIFI_AP_CONFIG_FILE, "r");

    if(scf != NULL) {
	// We are done. We can open the config file -> Folder exists, file exists

	fclose(scf);

	LOGD("Helper: Hostapd configuration file exists");

	return 1;
    }

    LOGD("Helper: Trying to create hostapd config file from template");

    // If the folder for our configuration files does not exist, create it

    mkdir(WIFI_AP_FOLDER, 0770);

    // Create empty accept list

    scf = fopen(WIFI_AP_ACCEPT_LIST, "w");

    if(scf == NULL) {
	LOGE("Helper: Failed to create \"%s\"", WIFI_AP_ACCEPT_LIST);

	return 0;
    }

    fprintf(scf, "# Empty accept list\n\n");

    fclose(scf);

    // Create empty deny list

    scf = fopen(WIFI_AP_DENY_LIST, "w");

    if(scf == NULL) {
	LOGE("Helper: Failed to create \"%s\"", WIFI_AP_DENY_LIST);

	return 0;
    }

    fprintf(scf, "# Empty deny list\n\n");

    fclose(scf);

    // Open the template

    stf = fopen(WIFI_AP_CONFIG_TEMPLATE, "r");

    // If we cannot open the template, all we can do is fail

    if(stf == NULL) {
	LOGE("Helper: Failed to open configuration template \"%s\" for hostapd", WIFI_AP_CONFIG_TEMPLATE);

	return 0;
    }

    // Create the configuration file

    scf = fopen(WIFI_AP_CONFIG_FILE, "w");

    // If we cannot create the configuration, fail as well

    if(scf == NULL) {
	LOGE("Helper: Failed to create configuration file \"%s\" for hostapd", WIFI_AP_CONFIG_FILE);

	return 0;
    }

    // Loop to copy template file

    while((c = fgetc(stf)) != EOF) fputc(c, scf);

    // Close the template

    fclose(stf);

    // Close the newly created config file

    fclose(scf);

    // Happy ending

    return 1;
}


int wifi_configuration_change(char *SSID, char *Security, char *Key)
{
    FILE *scf, *stf;
    char line[WIFI_AP_MAX_LINE];
    int bSecure = 1;

    // Open the template

    stf = fopen(WIFI_AP_CONFIG_TEMPLATE, "r");

    // If we cannot open the template, all we can do is fail

    if(stf == NULL) {
	LOGE("Helper: Failed to open configuration template \"%s\" for hostapd", WIFI_AP_CONFIG_TEMPLATE);

	return 0;
    }

    // Create the configuration file

    scf = fopen(WIFI_AP_CONFIG_FILE, "w");

    // If we cannot create the configuration, fail as well

    if(scf == NULL) {
	LOGE("Helper: Failed to create configuration file \"%s\" for hostapd", WIFI_AP_CONFIG_FILE);

	return 0;
    }

    // Check if the user configured an open network

    if(strcmp(Security, "open") == 0) bSecure = 0;

    // Loop to copy the template, replacing values on the fly

    while(fgets(line, sizeof(line), stf) != NULL) {
	// ssid=

	if(strstr(line, "ssid=") != NULL) {
		// Make sure we do not kill other lines containing the "ssid" substring

		if(strstr(line, "ignore_broadcast_ssid=") == NULL &&
		   strstr(line, "bssid=") == NULL) {
			snprintf(line, sizeof(line), "ssid=%s\n", SSID);
		}
	}

	// wpa=

	if(strstr(line, "wpa=") != NULL) {
		if(bSecure)
		 snprintf(line, sizeof(line), "wpa=2\n");
		else
		 snprintf(line, sizeof(line), "# wpa=2\n");
	}

	// wpa_passphrase=

	if(strstr(line, "wpa_passphrase=") != NULL) {
		if(bSecure)
		 snprintf(line, sizeof(line), "wpa_passphrase=%s\n", Key);
		else
		 snprintf(line, sizeof(line), "# wpa_passphrase=this_is_stupid\n");
	}

	// rsn_pairwise=

	if(strstr(line, "rsn_pairwise=") != NULL) {
		if(bSecure)
		 snprintf(line, sizeof(line), "rsn_pairwise=TKIP CCMP\n");
		else
		 snprintf(line, sizeof(line), "# rsn_pairwise=TKIP CCMP\n");
	}

	// Write back the changed or unchanged line

	fputs(line, scf);
    }

    // Close the template

    fclose(stf);

    // Close the newly created config file

    fclose(scf);

    // Happy ending

    return 1;
}


SoftapController::SoftapController() {
    mPid = 0;
    mSock = socket(AF_INET, SOCK_DGRAM, 0);
    if(mSock < 0)
        LOGE("Failed to open socket");
    memset(mIface, 0, sizeof(mIface));
}

SoftapController::~SoftapController() {
    if(mSock >= 0) close(mSock);
}

int SoftapController::getPrivFuncNum(char *iface, const char *fname) {
    struct iwreq wrq;
    struct iw_priv_args *priv_ptr;
    int i, ret;

    strncpy(wrq.ifr_name, iface, sizeof(wrq.ifr_name));
    wrq.u.data.pointer = mBuf;
    wrq.u.data.length = sizeof(mBuf) / sizeof(struct iw_priv_args);
    wrq.u.data.flags = 0;
    if ((ret = ioctl(mSock, SIOCGIWPRIV, &wrq)) < 0) {
        LOGE("SIOCGIPRIV failed: %d", ret);
        return ret;
    }
    priv_ptr = (struct iw_priv_args *)wrq.u.data.pointer;
    for(i=0;(i < wrq.u.data.length);i++) {
        if (strcmp(priv_ptr[i].name, fname) == 0)
            return priv_ptr[i].cmd;
    }
    return -1;
}

int SoftapController::startDriver(char *iface) {
	LOGD("Softap driver start - Qualcomm NOOP");
	return 0;
}

int SoftapController::stopDriver(char *iface) {
	LOGD("Softap driver stop - Qualcomm NOOP");
	return 0;
}

int SoftapController::startSoftap() {
	LOGD("SoftapController::startSoftap()");

	if(!wifi_configuration_file()) {
		LOGE("startSoftap: Pre flight check for configuration file failed");

		return -1;
	}

	if(wifi_start_hostapd() == 0) {
		mPid = 23;

		return 0;
	}

	LOGE("startSoftap: Failed to start hostapd");

	return -1;
}

int SoftapController::stopSoftap() {
	LOGD("SoftapController::stopSoftap()");

	if(wifi_stop_hostapd() == 0) {
		mPid = 0;

		return 0;
	}

	LOGE("stopSoftap: Failed to stop hostapd");

	return -1;
}

bool SoftapController::isSoftapStarted() {
    LOGD("SoftapController::isSoftapStarted() - mPid = %d", mPid);

    return (mPid != 0 ? true : false);
}

int SoftapController::addParam(int pos, const char *cmd, const char *arg)
{
    if (pos < 0)
        return pos;
    if ((unsigned)(pos + strlen(cmd) + strlen(arg) + 1) >= sizeof(mBuf)) {
        LOGE("Command line is too big");
        return -1;
    }
    pos += sprintf(&mBuf[pos], "%s=%s,", cmd, arg);
    return pos;
}

/*
 * Arguments:
 *      argv[2] - wlan interface
 *      argv[3] - softap interface
 *      argv[4] - SSID
 *	argv[5] - Security
 *	argv[6] - Key
 *	argv[7] - Channel
 *	argv[8] - Preamble
 *	argv[9] - Max SCB
 */

int SoftapController::setSoftap(int argc, char *argv[]) {
	int i;

	LOGD("SoftapController::setSoftap - Qualcomm");

	for(i=0; i<argc; i++) {
		LOGD("SoftapController::setSoftap - argv[%d] := %s", i, argv[i]);
	}

	// First make sure we have a configuration file to work on

	if(!wifi_configuration_file()) {
		LOGE("setSoftap: Configuration file preflight check failed");

		return -1;
	}

	// Try to write the new configuration to the Qualcomm hostapd file

	if(!wifi_configuration_change(argv[4], argv[5], argv[6])) {
		LOGE("setSoftap: Failed to write configuration to file");

		return -1;
	}

	// If the hostapd is running, log a warning

	if(isSoftapStarted()) {
		LOGW("setSoftap: Configuration changed while hostapd is running. Using old config until next start.");
	}

	// The configuration has been written

	return 0;
}

/*
 * Arguments:
 *	argv[2] - interface name
 *	argv[3] - AP or STA
 */

int SoftapController::fwReloadSoftap(int argc, char *argv[])
{
	LOGD("SoftapController::fwReloadSoftap - Qualcomm NOOP");
	return 0;
}
