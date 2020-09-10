//
//  Black80211Control_ioctl.cpp
//  Black80211
//
//  Created by Roman Peshkov on 30/06/2018.
//  Copyright © 2018 Roman Peshkov. All rights reserved.
//

#include "Black80211Control.hpp"
#include "ieee80211_ioctl.h"
#include "Black80211Interface.hpp"

const char *fake_hw_version = "Hardware 1.0";

#define kIOMessageNetworkChanged iokit_vendor_specific_msg(1)
#define kIOMessageScanComplete iokit_vendor_specific_msg(2)

IOReturn Black80211Control::message(UInt32 type, IOService *provider, void *argument) {
	IOLog("%s: %d TYPE=%d\n", __PRETTY_FUNCTION__, __LINE__, type);
	switch (type) {
		case kIOMessageNetworkChanged:
			fInterface->postMessage(APPLE80211_M_SSID_CHANGED);
			//self->fInterface->postMessage(APPLE80211_M_LINK_CHANGED);
			return kIOReturnSuccess;
		case kIOMessageScanComplete:
			if (requestedScanning) {
				requestedScanning = false;
				networkIndex = 0;
				ScanResult *oldScanResult = scan_result;
				scan_result = fProvider->getScanResult();
				IOLog("Black80211: Scanning complete, found %lu networks\n", scan_result->count);
				for (int i = 0; i < scan_result->count; i++)
					IOLog("%.*s: %d\n", APPLE80211_MAX_SSID_LEN + 1, scan_result->networks[i].essid, scan_result->networks[i].channel);
				// Try to send but don't deadlock
				fCommandGate->attemptAction([](OSObject *target, void* arg0, void* arg1, void* arg2, void* arg3) {
					ScanResult* oldScanResult = (ScanResult*)arg1;
					OSSafeReleaseNULL(oldScanResult);
					((IO80211Interface*)arg0)->postMessage(APPLE80211_M_SCAN_DONE);
					return kIOReturnSuccess;
				}, fInterface, oldScanResult);
			}
			return kIOReturnSuccess;
	}
	IOLog("%s: unsupported\n", __PRETTY_FUNCTION__);
	return kIOReturnUnsupported;
}

//
// MARK: 1 - SSID
//

IOReturn Black80211Control::getSSID(IO80211Interface *interface,
                                    struct apple80211_ssid_data *sd) {
    
    bzero(sd, sizeof(*sd));
    sd->version = APPLE80211_VERSION;
	if (fProvider->getState() != APPLE80211_S_RUN)
		return 6;

	fProvider->getESSID(sd->ssid_bytes, &sd->ssid_len);

    return kIOReturnSuccess;
}

IOReturn Black80211Control::setSSID(IO80211Interface *interface,
                                    struct apple80211_ssid_data *sd) {
	IOLog("Black80211: setSSID is not supported\n");
    //fInterface->postMessage(APPLE80211_M_SSID_CHANGED);
    return kIOReturnSuccess;
}

//
// MARK: 2 - AUTH_TYPE
//

IOReturn Black80211Control::getAUTH_TYPE(IO80211Interface *interface,
                                         struct apple80211_authtype_data *ad) {
    ad->version = APPLE80211_VERSION;
	ad->authtype_lower = authtype_lower;
	ad->authtype_upper = authtype_upper;
    return kIOReturnSuccess;
}

IOReturn Black80211Control::setAUTH_TYPE(IO80211Interface *interface,
                                         struct apple80211_authtype_data *ad) {
	authtype_lower = ad->authtype_lower;
	authtype_upper = ad->authtype_upper;
    return kIOReturnSuccess;
}


static u_int32_t channelFlags(int channel) {
	if (channel <= 14)
		return APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_ACTIVE;
	else if (channel >= 52 && channel <= 140)
		return APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_40MHZ | APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_DFS;
	else
		return APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_40MHZ | APPLE80211_C_FLAG_ACTIVE;
}

//
// MARK: 3 - CIPHER_KEY
//

IOReturn Black80211Control::getCIPHER_KEY(IO80211Interface *interface,
                                         struct apple80211_key *key) {
	*key = cipher_key;
    return kIOReturnSuccess;
}

const char* hexdump(uint8_t *buf, size_t len) {
	ssize_t str_len = len * 3 + 1;
	char *str = (char*)IOMalloc(str_len);
	if (!str)
		return nullptr;
	for (size_t i = 0; i < len; i++)
		snprintf(str + 3 * i, (len - i) * 3, "%02x ", buf[i]);
	str[MAX(str_len - 2, 0)] = 0;
	return str;
}

IOReturn Black80211Control::setCIPHER_KEY(IO80211Interface *interface,
                                         struct apple80211_key *key) {
	const char* keydump = hexdump(key->key, key->key_len);
	const char* rscdump = hexdump(key->key_rsc, key->key_rsc_len);
	const char* eadump = hexdump(key->key_ea.octet, APPLE80211_ADDR_LEN);
	if (keydump && rscdump && eadump)
		IOLog("Set key request: len=%d cipher_type=%d flags=%d index=%d key=%s rsc_len=%d rsc=%s ea=%s\n",
			  key->key_len, key->key_cipher_type, key->key_flags, key->key_index, keydump, key->key_rsc_len, rscdump, eadump);
	else
		IOLog("Set key request, but failed to allocate memory for hexdump\n");
	
	if (keydump)
		IOFree((void*)keydump, 3 * key->key_len + 1);
	if (rscdump)
		IOFree((void*)rscdump, 3 * key->key_rsc_len + 1);
	if (eadump)
		IOFree((void*)eadump, 3 * APPLE80211_ADDR_LEN + 1);
	
	cipher_key = *key;
	switch (key->key_cipher_type) {
		case APPLE80211_CIPHER_NONE:
			IOLog("Setting NONE key is not supported\n");
			break;
		case APPLE80211_CIPHER_WEP_40:
		case APPLE80211_CIPHER_WEP_104:
			IOLog("Setting WEP key %d is not supported\n", key->key_index);
			break;
		case APPLE80211_CIPHER_TKIP:
		case APPLE80211_CIPHER_AES_OCB:
		case APPLE80211_CIPHER_AES_CCM:
			switch (key->key_flags) {
				case 4: // PTK
					fProvider->setPTK(key->key, key->key_len);
					fInterface->postMessage(APPLE80211_M_RSN_HANDSHAKE_DONE);
					break;
				case 0: // GTK
					fProvider->setGTK(key->key, key->key_len, key->key_index, key->key_rsc);
					fInterface->postMessage(APPLE80211_M_RSN_HANDSHAKE_DONE);
					break;
			}
			break;
		case APPLE80211_CIPHER_PMK:
			IOLog("Setting WPA PMK is not supported\n");
			break;
		case APPLE80211_CIPHER_PMKSA:
			fProvider->setPMKSA(key->key, key->key_len);
			break;
	}
	//fInterface->postMessage(APPLE80211_M_CIPHER_KEY_CHANGED);
    return kIOReturnSuccess;
}


//
// MARK: 4 - CHANNEL
//

IOReturn Black80211Control::getCHANNEL(IO80211Interface *interface,
                                       struct apple80211_channel_data *cd) {
    memset(cd, 0, sizeof(apple80211_channel_data));
	if (fProvider->getState() != APPLE80211_S_RUN)
		return 6;

	cd->version = APPLE80211_VERSION;
    cd->channel.version = APPLE80211_VERSION;
	cd->channel.channel = fProvider->getChannel();
	cd->channel.flags = channelFlags(cd->channel.channel);
    return kIOReturnSuccess;
}

//
// MARK: 6 - PROTMODE
//

IOReturn Black80211Control::getPROTMODE(IO80211Interface *interface, struct apple80211_protmode_data *pd) {
	pd->version = APPLE80211_VERSION;
	pd->protmode = APPLE80211_PROTMODE_AUTO;
	pd->threshold = 0;
	return kIOReturnSuccess;
}


//
// MARK: 7 - TXPOWER
//

IOReturn Black80211Control::getTXPOWER(IO80211Interface *interface,
                                       struct apple80211_txpower_data *txd) {

    txd->version = APPLE80211_VERSION;
    txd->txpower = 100;
    txd->txpower_unit = APPLE80211_UNIT_PERCENT;
    return kIOReturnSuccess;
}

//
// MARK: 8 - RATE
//

IOReturn Black80211Control::getRATE(IO80211Interface *interface, struct apple80211_rate_data *rd) {
	if (fProvider->getState() != APPLE80211_S_RUN)
		return 6;

    rd->version = APPLE80211_VERSION;
    rd->num_radios = 1;
    rd->rate[0] = fProvider->getRate();
    return kIOReturnSuccess;
}

//
// MARK: 9 - BSSID
//

IOReturn Black80211Control::getBSSID(IO80211Interface *interface,
                                     struct apple80211_bssid_data *bd) {
    
    bd->version = APPLE80211_VERSION;
	if (fProvider->getState() < APPLE80211_S_AUTH)
		return 6;

	fProvider->getBSSID(bd->bssid.octet);
    return kIOReturnSuccess;
}

//
// MARK: 10 - SCAN_REQ
//
IOReturn Black80211Control::setSCAN_REQ(IO80211Interface *interface,
										struct apple80211_scan_data *sd) {
	if (requestedScanning && fProvider->isScanning()) {
		IOLog("Error: already scanning\n");
		return iokit_family_err(sub_iokit_wlan, 0x446);
		/*IOLog("Reporting previous scan result\n");
		networkIndex = 0;
		scan_result = itlwm_get_scan_result(fItlWm);
		interface->postMessage(APPLE80211_M_SCAN_DONE);
		return kIOReturnSuccess;*/
	}

	requestedScanning = true;

	requestIsMulti = false;
	memcpy(&request, sd, sizeof(struct apple80211_scan_data));

	networkIndex = 0;

	IOLog("Black80211. Scan requested. Type: %u\n"
		  "BSS Type: %u\n"
		  "PHY Mode: %u\n"
		  "Dwell time: %u\n"
		  "Rest time: %u\n"
		  "Num channels: %u\n",
		  sd->scan_type,
		  sd->bss_type,
		  sd->phy_mode,
		  sd->dwell_time,
		  sd->rest_time,
		  sd->num_channels);

	IOLog("SSID: %.*s\n", sd->ssid_len + 1, sd->ssid);

	uint8_t *b = sd->bssid.octet;
	IOLog("BSSID: %02x:%02x:%02x:%02x:%02x:%02x\n", b[0], b[1], b[2], b[3], b[4], b[5]);

	IOLog("Channels:\n");
	uint8_t channels[255];
	for (int i = 0; i < sd->num_channels; i++) {
		IOLog("%d\n", sd->channels[i].channel);
		channels[i] = sd->channels[i].channel;
	}
	
	if (sd->scan_type == APPLE80211_SCAN_TYPE_FAST ||
		(fProvider->getState() != APPLE80211_S_RUN && sd->num_channels != 0 && sd->channels[0].channel != 1)) {
	  IOLog("Reporting previous scan result\n");
	  networkIndex = 0;
	  //scan_result = itlwm_get_scan_result(fItlWm);
		fTimerEventSource->setAction(&Black80211Control::postScanningDoneMessage);
		fTimerEventSource->setTimeoutUS(1);
	  return kIOReturnSuccess;
	}

	IOLog("Begin scanning\n");
	requestedScanning = true;

	IOReturn status = fProvider->bgscan(channels, sd->num_channels, (const char*)sd->ssid, sd->ssid_len);
	if (status != kIOReturnSuccess)
		requestedScanning = false;
	return status;
}

void Black80211Control::postScanningDoneMessage(OSObject* self,  ...) {
	Black80211Control* controller = (Black80211Control*)self;
	controller->fInterface->postMessage(APPLE80211_M_SCAN_DONE);
}

IOReturn Black80211Control::setSCAN_REQ_MULTIPLE(
IO80211Interface *interface, struct apple80211_scan_multiple_data *sd) {
	struct apple80211_scan_data s1d;
	s1d.version = sd->version;
	s1d.bss_type = APPLE80211_AP_MODE_ANY;
	memset(s1d.bssid.octet, 0, ETHER_ADDR_LEN);
	s1d.ssid_len = 0;
	s1d.scan_type = sd->scan_type;
	s1d.phy_mode = sd->phy_mode;
	s1d.dwell_time = sd->dwell_time;
	s1d.rest_time = sd->rest_time;
	s1d.num_channels = MIN(APPLE80211_MAX_CHANNELS, sd->num_channels);
	memcpy(s1d.channels, sd->channels, s1d.num_channels * sizeof(apple80211_channel));
	return setSCAN_REQ(interface, &s1d);
}


//
// MARK: 11 - SCAN_RESULT
//

IOReturn Black80211Control::getSCAN_RESULT(IO80211Interface *interface,
                                           struct apple80211_scan_result **sr) {
	requestedScanning = false;

	if (sr == nullptr) {
		IOLog("Error: out var is null\n");
		return 0x16;
	}

	if (scan_result == nullptr) {
		IOLog("Error: scan result is not ready\n");
		return 0x10;
	}

	if (scan_result->count == 0) {
		IOLog("Error: no networks found\n");
		return 0x10;
	}

	for ( ; networkIndex < scan_result->count; networkIndex++) {
		auto *network = &scan_result->networks[networkIndex];

		bool requested = true;
		// itlwm reports current network even when its channel is not requested
		if (requestIsMulti && multiRequest.num_channels > 0) {
			requested = false;
			for (int i = 0; i < multiRequest.num_channels; i++)
				if (multiRequest.channels[i].channel == network->channel) {
					requested = true;
					break;
				}
		}
		else if (!requestIsMulti && request.num_channels > 0) {
			requested = false;
			for (int i = 0; i < request.num_channels; i++)
				if (request.channels[i].channel == network->channel) {
					requested = true;
					break;
				}
		}

		if (!requested)
			continue;

		if (requestIsMulti && multiRequest.ssid_count > 0) {
			requested = false;
			for (int i = 0; i < multiRequest.ssid_count; i++) {
				size_t ssid_len = strnlen((const char*)network->essid, 32);
				if (ssid_len != multiRequest.ssids[i].ssid_len)
					continue;

				if (!strncmp((const char*)multiRequest.ssids[i].ssid_bytes, (const char*)network->essid, ssid_len)) {
					requested = true;
					break;
				}
			}
		}
		else if (!requestIsMulti && request.ssid_len > 0) {
			requested = strnlen((const char*)network->essid, 32) == request.ssid_len && !strncmp((const char*)network->essid, (const char*)request.ssid, 32);
		}

		if (!requested)
			continue;

		struct apple80211_scan_result* result =
			(struct apple80211_scan_result*)IOMalloc(sizeof(struct apple80211_scan_result));

		bzero(result, sizeof(*result));

		result->version = APPLE80211_VERSION;

		result->asr_channel.version = APPLE80211_VERSION;
		result->asr_channel.channel = network->channel;
		result->asr_channel.flags = channelFlags(result->asr_channel.channel);
		result->asr_noise = fProvider->getNoise();
		result->asr_rssi = network->rssi;
		result->asr_beacon_int = network->beacon_interval;

		result->asr_cap = network->capabilities;

		result->asr_age = 0;

		memcpy(result->asr_bssid, network->bssid, APPLE80211_ADDR_LEN);

		result->asr_nrates = 1;
		result->asr_rates[0] = 300;

		IOLog("SSID: %s, channel %d\n", network->essid, network->channel);
		strncpy((char*)result->asr_ssid, (const char*)network->essid, APPLE80211_MAX_SSID_LEN);
		result->asr_ssid_len = strnlen((const char*)network->essid, APPLE80211_MAX_SSID_LEN);

		if (network->rsn_ie == nullptr) {
			result->asr_ie_len = 0;
			result->asr_ie_data = nullptr;
		}
		else {
			/*
			result->asr_ie_len = 2 + network->rsn_ie[1];
			 */
			result->asr_ie_len = network->ie_len;
			result->asr_ie_data = IOMalloc(result->asr_ie_len);
			memcpy(result->asr_ie_data, network->rsn_ie, result->asr_ie_len);
			const char* s = hexdump((uint8_t*)result->asr_ie_data, result->asr_ie_len);
			if (s) {
				IOLog("RSN IE: %s\n", s);
				IOFree((void*)s, 3 * result->asr_ie_len + 1);
			}
		}

		*sr = result;

		networkIndex++;
		return kIOReturnSuccess;
	}

	IOLog("Reported all results\n");
	return 5;
}

//
// MARK: 12 - CARD_CAPABILITIES
//
IOReturn Black80211Control::getCARD_CAPABILITIES(
    IO80211Interface *interface, struct apple80211_capability_data *cd) {
	bzero(cd, sizeof(struct apple80211_capability_data));
	cd->version = APPLE80211_VERSION;

	int capabilities[] = {
		APPLE80211_CAP_WEP,
		APPLE80211_CAP_TKIP,
		APPLE80211_CAP_AES_CCM,
		//APPLE80211_CAP_IBSS,
		//APPLE80211_CAP_PMGT,
		//APPLE80211_CAP_HOSTAP,
		APPLE80211_CAP_SHSLOT,
		APPLE80211_CAP_SHPREAMBLE,
		//APPLE80211_CAP_MONITOR,
		APPLE80211_CAP_TKIPMIC,
		APPLE80211_CAP_WPA1,
		APPLE80211_CAP_WPA2
	};

	uint64_t capabilities_value = 0;
	for (int i = 0; i < sizeof(capabilities) / sizeof(int); i++)
		capabilities_value |= (1 << capabilities[i]);

	cd->capabilities[0] = capabilities_value & 0xff;
	cd->capabilities[1] = (capabilities_value >> 8) & 0xff;
	
	//Setting all bits enables AirDrop, Handoff, Auto Unlock. Of course, they don't work
	//memset(cd->capabilities, 0xff, sizeof(cd->capabilities));

	// Some bits of these two bytes enable multiple scanning requests. It's not what we currently want
	//cd->capabilities[2] |= 0xC0;
	//cd->capabilities[6] |= 0x84;
	//memset(cd->capabilities, 0xff, 8);

  /// ic->ic_caps =
  /// IEEE80211_C_WEP |        /* WEP */
  /// IEEE80211_C_RSN |        /* WPA/RSN */
  /// IEEE80211_C_SCANALL |    /* device scans all channels at once */
  /// IEEE80211_C_SCANALLBAND |    /* device scans all bands at once */
  /// IEEE80211_C_SHSLOT |    /* short slot time supported */
  /// IEEE80211_C_SHPREAMBLE;    /* short preamble supported */

  // cd->capabilities[5] |= 4;

	// cd->capabilities[2] |= 0xC0;
	// cd->capabilities[6] |= 0x84;

  /* Airport-related flags
  cd->capabilities[3] |= 8;
  cd->capabilities[6] |= 1;
  cd->capabilities[5] |= 0x80;
  */

  /*
   Needs further documentation,
   as setting all of these flags enables AirDrop + IO80211VirtualInterface

  cd->capabilities[2] = 3;
  cd->capabilities[2] |= 0x13;
  cd->capabilities[2] |= 0x20;
  cd->capabilities[2] |= 0x28;
  cd->capabilities[2] |= 4;
  cd->capabilities[5] |= 8;
  cd->capabilities[3] |= 2;
  cd->capabilities[4] |= 1;
  cd->capabilities[6] |= 8;

  cd->capabilities[3] |= 0x23;
  cd->capabilities[2] |= 0x80;
  cd->capabilities[5] |= 4;
  cd->capabilities[2] |= 0xC0;
  cd->capabilities[6] |= 0x84;
  cd->capabilities[3] |= 8;
  cd->capabilities[6] |= 1;
  cd->capabilities[5] |= 0x80;

  */

  return kIOReturnSuccess;
}

//
// MARK: 13 - STATE
//

IOReturn Black80211Control::getSTATE(IO80211Interface *interface,
                                     struct apple80211_state_data *sd) {
    sd->version = APPLE80211_VERSION;
    sd->state = fProvider->getState();
    return kIOReturnSuccess;
}

IOReturn Black80211Control::setSTATE(IO80211Interface *interface,
                                     struct apple80211_state_data *sd) {
    //IOLog("Black80211: Setting state: %u\n", sd->state);
    //state = sd->state;
    return kIOReturnSuccess;
}

//
// MARK: 14 - PHY_MODE
//

IOReturn Black80211Control::getPHY_MODE(IO80211Interface *interface,
                                        struct apple80211_phymode_data *pd) {
    pd->version = APPLE80211_VERSION;
    pd->phy_mode = fProvider->getSupportedPHYModes();
    pd->active_phy_mode = fProvider->getPHYMode();
    return kIOReturnSuccess;
}

//
// MARK: 15 - OP_MODE
//

IOReturn Black80211Control::getOP_MODE(IO80211Interface *interface,
                                       struct apple80211_opmode_data *od) {
    od->version = APPLE80211_VERSION;
    od->op_mode = fProvider->getOpMode();
    return kIOReturnSuccess;
}

//
// MARK: 16 - RSSI
//

IOReturn Black80211Control::getRSSI(IO80211Interface *interface,
                                    struct apple80211_rssi_data *rd) {
    // iwm_get_signal_strength
	// iwm_calc_rssi
	// iwm_rxmq_get_signal_strength
    bzero(rd, sizeof(*rd));
    rd->version = APPLE80211_VERSION;
    rd->num_radios = 1;
    rd->rssi[0] = fProvider->getRSSI();
    rd->aggregate_rssi = rd->rssi[0];
    rd->rssi_unit = APPLE80211_UNIT_DBM;
    return kIOReturnSuccess;
}

//
// MARK: 17 - NOISE
//

IOReturn Black80211Control::getNOISE(IO80211Interface *interface,
                                     struct apple80211_noise_data *nd) {
    // iwm_get_noise
    bzero(nd, sizeof(*nd));
    nd->version = APPLE80211_VERSION;
    nd->num_radios = 1;
    nd->noise[0] = fProvider->getNoise();
    nd->aggregate_noise = nd->noise[0];
    nd->noise_unit = APPLE80211_UNIT_DBM;
    return kIOReturnSuccess;
}

//
// MARK: 18 - INT_MIT
//
IOReturn Black80211Control::getINT_MIT(IO80211Interface* interface,
                                       struct apple80211_intmit_data* imd) {
    imd->version = APPLE80211_VERSION;
    imd->int_mit = APPLE80211_INT_MIT_AUTO;
    return kIOReturnSuccess;
}


//
// MARK: 19 - POWER
//

IOReturn Black80211Control::getPOWER(IO80211Interface *interface,
                                     struct apple80211_power_data *pd) {
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 1;
    pd->power_state[0] = powerState;
    
    return kIOReturnSuccess;
}


IOReturn Black80211Control::setPOWER(IO80211Interface *interface,
                                     struct apple80211_power_data *pd) {
	requestedScanning = false;
    if (pd->num_radios > 0) {
		if (pd->power_state[0] != powerState) {
			if (pd->power_state[0])
				enable(interface);
			else
				disable(interface);
			powerState = pd->power_state[0];
		}
    }
    //fInterface->postMessage(APPLE80211_M_POWER_CHANGED, NULL, 0);
    
    return kIOReturnSuccess;
}

//
// MARK: 20 - ASSOCIATE
//

IOReturn Black80211Control::setASSOCIATE(IO80211Interface *interface,
                                         struct apple80211_assoc_data *ad) {
	uint8_t *b = ad->ad_bssid.octet;
    IOLog("Black80211::setAssociate %s, bssid %02x:%02x:%02x:%02x:%02x:%02x auth type %x:%x\n", ad->ad_ssid, b[0], b[1], b[2], b[3], b[4], b[5], ad->ad_auth_upper, ad->ad_auth_lower);

	setDISASSOCIATE(interface);

	apple80211_authtype_data authtype_data;
	authtype_data.version = APPLE80211_VERSION;
	authtype_data.authtype_lower = ad->ad_auth_lower;
	authtype_data.authtype_upper = ad->ad_auth_upper;
	setAUTH_TYPE(interface, &authtype_data);
	
	apple80211_rsn_ie_data rsn_ie_data;
	rsn_ie_data.version = APPLE80211_VERSION;
	rsn_ie_data.len = ad->ad_rsn_ie[1] + 2;
	memcpy(rsn_ie_data.ie, ad->ad_rsn_ie, rsn_ie_data.len);
	setRSN_IE(interface, &rsn_ie_data);
	
//	apple80211_ssid_data ssid_data;
//	ssid_data.version = APPLE80211_VERSION;
//	ssid_data.ssid_len = ad->ad_ssid_len;
//	memcpy(ssid_data.ssid_bytes, ad->ad_ssid, APPLE80211_MAX_SSID_LEN);
//	setSSID(interface, &ssid_data);

//	setCIPHER_KEY(interface, &ad->ad_key);
	fProvider->associate(ad->ad_ssid, ad->ad_ssid_len, ad->ad_bssid, ad->ad_auth_lower, ad->ad_auth_upper, ad->ad_key.key, ad->ad_key.key_len, ad->ad_key.key_index);

    fInterface->postMessage(APPLE80211_M_SSID_CHANGED);
    fInterface->setLinkState(IO80211LinkState::kIO80211NetworkLinkUp, 0);

	// setAP_MODE(interface, ap_mode);
	// don't set BSSID – it is set by the driver

	return kIOReturnSuccess;
}

//
// MARK: 22 - DISASSOCIATE
//

IOReturn Black80211Control::setDISASSOCIATE(IO80211Interface *interface) {
    IOLog("Black80211::disassociate\n");
	requestedScanning = false;
	fProvider->disassociate();
    fInterface->postMessage(APPLE80211_M_SSID_CHANGED);
	//fInterface->setLinkState(IO80211LinkState::kIO80211NetworkLinkDown, 0);
	return kIOReturnSuccess;
}


//
// MARK: 27 - SUPPORTED_CHANNELS
//

IOReturn Black80211Control::getSUPPORTED_CHANNELS(IO80211Interface *interface,
                                                  struct apple80211_sup_channel_data *ad) {
    ad->version = APPLE80211_VERSION;
	channel_desc channels[APPLE80211_MAX_CHANNELS];
	fProvider->getSupportedChannels(ad->num_channels, channels);
	ad->num_channels = MIN(APPLE80211_MAX_CHANNELS, ad->num_channels);
	for (int i = 0; i < ad->num_channels; i++) {
		auto& apple_channel = ad->supported_channels[i];
		auto& ieee_channel = channels[i];
		apple_channel.version = APPLE80211_VERSION;
		apple_channel.channel = ieee_channel.channel_num;
		apple_channel.flags = ieee_channel.channel_flags;
	}
    return kIOReturnSuccess;
}

//
// MARK: 28 - LOCALE
//

IOReturn Black80211Control::getLOCALE(IO80211Interface *interface,
                                      struct apple80211_locale_data *ld) {
    ld->version = APPLE80211_VERSION;
    ld->locale  = APPLE80211_LOCALE_FCC;
    
    return kIOReturnSuccess;
}

//
// MARK: 29 - DEAUTH
//

IOReturn Black80211Control::getDEAUTH(IO80211Interface *interface, struct apple80211_deauth_data *dd) {
	dd->version = APPLE80211_VERSION;
	int state = fProvider->getState();
	//if (state == APPLE80211_S_RUN)
	//	dd->deauth_reason = APPLE80211_RESULT_SUCCESS;
	//else
	//	dd->deauth_reason = APPLE80211_RESULT_UNAVAILABLE;
	dd->deauth_reason = 0;
	fProvider->getBSSID(dd->deauth_ea.octet);
	IOLog("Deauth reason: %d, state: %d\n", dd->deauth_reason, state);
	return kIOReturnSuccess;
}

//
// MARK: 37 - TX_ANTENNA
//
IOReturn Black80211Control::getTX_ANTENNA(IO80211Interface *interface,
                                          apple80211_antenna_data *ad) {
    ad->version = APPLE80211_VERSION;
    ad->num_radios = 1;
    ad->antenna_index[0] = 1;
    return kIOReturnSuccess;
}

//
// MARK: 39 - ANTENNA_DIVERSITY
//

IOReturn Black80211Control::getANTENNA_DIVERSITY(IO80211Interface *interface,
                                                 apple80211_antenna_data *ad) {
    ad->version = APPLE80211_VERSION;
    ad->num_radios = 1;
    ad->antenna_index[0] = 1;
    return kIOReturnSuccess;
}

//
// MARK: 43 - DRIVER_VERSION
//

IOReturn Black80211Control::getDRIVER_VERSION(IO80211Interface *interface,
                                              struct apple80211_version_data *hv) {
    hv->version = APPLE80211_VERSION;
	fProvider->getFirmwareVersion(hv->string, hv->string_len);
    return kIOReturnSuccess;
}

//
// MARK: 44 - HARDWARE_VERSION
//

IOReturn Black80211Control::getHARDWARE_VERSION(IO80211Interface *interface,
                                                struct apple80211_version_data *hv) {
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fake_hw_version, sizeof(hv->string));
    hv->string_len = strlen(fake_hw_version);
    return kIOReturnSuccess;
}

//
// MARK: 46 - RSN_IE
//
IOReturn Black80211Control::getRSN_IE(IO80211Interface *interface,
                                                struct apple80211_rsn_ie_data *rsn_ie_data) {
    rsn_ie_data->version = APPLE80211_VERSION;
	fProvider->getRSNIE(rsn_ie_data->len, rsn_ie_data->ie);
	const char *s = hexdump(rsn_ie_data->ie, rsn_ie_data->len);
	if (s) {
		IOLog("Black80211::getRSN_IE: %s\n", s);
		IOFree((void*)s, 3 * rsn_ie_data->len + 1);
	}
	else {
		IOLog("Black80211::getRSN_IE: empty\n");		
	}
    return kIOReturnSuccess;
}

IOReturn Black80211Control::setRSN_IE(IO80211Interface *interface,
                                                struct apple80211_rsn_ie_data *rsn_ie_data) {
	const char *s = hexdump(rsn_ie_data->ie, rsn_ie_data->len);
	if (s) {
		IOLog("set RSN IE: %s\n", s);
		IOFree((void*)s, 3 * rsn_ie_data->len + 1);
	}
	fProvider->setRSN_IE(rsn_ie_data->ie);
    return kIOReturnSuccess;
}

//
// MARK: 48 - AP_IE_LIST
//
IOReturn Black80211Control::getAP_IE_LIST(IO80211Interface *interface,
                                                struct apple80211_ap_ie_data *ap_ie_data) {
	fProvider->getAP_IE_LIST(ap_ie_data->len, ap_ie_data->ie_data);
	const char *s = hexdump(ap_ie_data->ie_data, ap_ie_data->len);
	if (s) {
		IOLog("IE list: %s\n", s);
		IOFree((void*)s, 3 * ap_ie_data->len + 1);
	}
    return kIOReturnSuccess;
}


//
// MARK: 50 - ASSOCIATION_STATUS
//
IOReturn Black80211Control::getASSOCIATION_STATUS(IO80211Interface *interface,
												  struct apple80211_assoc_status_data *sd) {
	sd->version = APPLE80211_VERSION;
	int state = fProvider->getState();
	//if (state == APPLE80211_S_RUN)
	//	sd->status = APPLE80211_RESULT_SUCCESS;
	//else
	//	sd->status = APPLE80211_RESULT_UNAVAILABLE;
	sd->status = 0;
	IOLog("getASSOCIATION_STATUS: state %d, assoc status %d\n", state, sd->status);
	return kIOReturnSuccess;
}

//
// MARK: 51 - COUNTRY_CODE
//

IOReturn Black80211Control::getCOUNTRY_CODE(IO80211Interface *interface,
                                            struct apple80211_country_code_data *cd) {
    cd->version = APPLE80211_VERSION;
	fProvider->getCountryCode((char*)cd->cc);
    return kIOReturnSuccess;
}

//
// MARK: 57 - MCS
//
IOReturn Black80211Control::getMCS(IO80211Interface* interface, struct apple80211_mcs_data* md) {
    md->version = APPLE80211_VERSION;
    md->index = fProvider->getMCS();
    return kIOReturnSuccess;
}

IOReturn Black80211Control::getROAM_THRESH(IO80211Interface* interface, struct apple80211_roam_threshold_data* md) {
    md->threshold = 1000;
    md->count = 0;
    return kIOReturnSuccess;
}

IOReturn Black80211Control::getRADIO_INFO(IO80211Interface* interface, struct apple80211_radio_info_data* md)
{
    md->version = 1;
    md->count = 1;
    return kIOReturnSuccess;
}

//
// MARK: 90 - SCANCACHE_CLEAR
//

IOReturn Black80211Control::setSCANCACHE_CLEAR(IO80211Interface *interface) {
	networkIndex = 0;
	OSSafeReleaseNULL(scan_result);
	return kIOReturnSuccess;
}

//
// MARK: 156 - LINK_CHANGED_EVENT_DATA
//

IOReturn Black80211Control::getLINK_CHANGED_EVENT_DATA(IO80211Interface *interface, struct apple80211_link_changed_event_data *ed) {
	if (ed == nullptr)
		return 16;

	bzero(ed, sizeof(apple80211_link_changed_event_data));
	ed->isLinkDown = fProvider->getState() != APPLE80211_S_RUN;
	ed->rssi = fProvider->getRSSI();
	if (ed->isLinkDown) {
		ed->voluntary = true;
		ed->reason = APPLE80211_LINK_DOWN_REASON_DEAUTH;
	}
	IOLog("Link down: %d, reason: %d\n", ed->isLinkDown, ed->reason);
	return kIOReturnSuccess;
}

//
// MARK: 254 - HW_SUPPORTED_CHANNELS
//

IOReturn Black80211Control::getHW_SUPPORTED_CHANNELS(IO80211Interface *interface,
                                                  struct apple80211_sup_channel_data *ad) {
	return getSUPPORTED_CHANNELS(interface, ad);
}

//
// MARK: 353 - NSS
//

IOReturn Black80211Control::getNSS(IO80211Interface *interface,
                                                  struct apple80211_nss_data *ad) {
	if (ad == nullptr)
		return 0x16;

	if (fProvider->getState() < APPLE80211_S_AUTH)
		return 6;

	ad->version = APPLE80211_VERSION;	
	ad->nss = 1;
	return kIOReturnSuccess;
}
