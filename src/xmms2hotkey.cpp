/*
 * xmms2hotkey.cpp - client for controlling XMMS2 via hotkeys
 *
 * Copyright (C) 2009-2011 Adam Nielsen <a.nielsen@shikadi.net>
 * XMMS2 is Copyright (C) 2003-2011 XMMS2 Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include <xmmsclient/xmmsclient++.h>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <signal.h>

#include <errno.h>

#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/program_options.hpp>
namespace po = boost::program_options;

#ifdef USE_X11
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XF86keysym.h>
#endif // USE_X11

#ifdef USE_EVDEV
#include <fcntl.h>
#include <linux/input.h>
#endif // USE_EVDEV

#define PROGNAME "[xmms2hotkey] "

// Helper functions for triggering actions that require multiple calls to
// the XMMS2 daemon.  (Since each hotkey event will only call one function.)
namespace Xmms2Hotkey {
	void playpause(const Xmms::Playback *p)
	{
		switch (p->getStatus()) {
			case Xmms::Playback::PLAYING:
				p->pause();
				break;
			case Xmms::Playback::STOPPED:
			case Xmms::Playback::PAUSED:
				p->start();
				break;
		}
		return;
	}
	void setVol(const Xmms::Playback *p, int iDelta, const std::string& k, const Xmms::Dict::Variant& v)
	{
		int iVol = boost::get<int>(v) + iDelta;
		if (iVol > 100) iVol = 100;
		else if (iVol < 0) iVol = 0;
		p->volumeSet(k, iVol);
	}
	void volChange(const Xmms::Playback *p, int iDelta)
	{
		Xmms::Dict vol = p->volumeGet();
		vol.each(boost::bind(&setVol, p, iDelta, _1, _2));
		return;
	}
	void skipTrack(const Xmms::Client *p, int iDelta)
	{
		p->playlist.setNextRel(iDelta);
		p->playback.tickle();
		return;
	}
}

struct config {
	int iSeekDelta;
	int iVolDelta;
	bool bShowEvents;
} config;

struct hotkey;

typedef std::vector<struct hotkey> VC_HOTKEYS;
typedef std::map<std::string, VC_HOTKEYS> MP_KEYDEFS;

typedef enum { HK_X11_MOUSE = 1, HK_X11_KEYBOARD = 2, HK_EVDEV } HK_TYPE;

struct hotkey {
	int hkiType;
	int iKey; // HK_X11_MOUSE: button number, HK_X11_KEYBOARD: keycode, HK_EVDEV: evdev code/keycode
	int iModifier; // Shift, Ctrl, etc.  Uses default X11 modifier flags.
	boost::function<void()> fnAction; // Function to call when hotkey is pressed
	VC_HOTKEYS vcSubActions;
};

typedef struct hotkey HOTKEY;

VC_HOTKEYS vcHotkeys;
VC_HOTKEYS vcActiveHotkeys;

typedef struct {
	int iIndex;
	std::string strId;           // "evdev0" etc.
	std::string strDevicePath;   // "/dev/input/event1" etc.
} EVDEV_INFO;

class EUndefinedKey: virtual public std::exception {
	private:
		std::string strMsg;
	public:
		EUndefinedKey(std::string strKey, std::string strEvent)
			throw () :
				strMsg(std::string("Tried to associate undefined key \"")
						+ strKey + "\" with action \"" + strEvent + "\"")
		{
		}
		virtual ~EUndefinedKey()
			throw ()
		{
		}
		virtual const char *what() const
			throw ()
		{
			return this->strMsg.c_str();
		}
};

class EBindFailed: virtual public std::exception {
	private:
		std::string strMsg;
	public:
		EBindFailed(std::string strDevice, std::string strReason)
			throw () :
				strMsg(std::string("Unable to open ") + strDevice + ": " + strReason)
		{
		}
		virtual ~EBindFailed()
			throw ()
		{
		}
		virtual const char *what() const
			throw ()
		{
			return this->strMsg.c_str();
		}
};

// Search the vector for a matching keycode (or button) and modifier.  Returns
// iterator to matching structure, or end() on failure.
VC_HOTKEYS::iterator searchHotkeyVector(VC_HOTKEYS &vcHotkeys,
	int hkiType, int iKey, int iModifier)
{
	for (VC_HOTKEYS::iterator i = vcHotkeys.begin(); i != vcHotkeys.end(); i++) {
		if ((i->hkiType == hkiType) && (i->iKey == iKey) &&
			(
				(i->iModifier == -1) ||
				(i->iModifier == iModifier))
		) {
			return i;
		}
	}
	return vcHotkeys.end();
}

void processKeypress(int hkiType, int iKey, int iModifier)
{
//	printf("key: %d, state: %d, devtype: %d\n", iKey, iModifier, hkiType);
	VC_HOTKEYS::iterator itHK = searchHotkeyVector(vcHotkeys, hkiType,
		iKey, iModifier);
	if (itHK != vcHotkeys.end()) {
		// This keypress matched a primary hotkey
		if (itHK->fnAction) {
			std::cout << PROGNAME "Matched " << iKey << ", triggering action" << std::endl;
			try {
				itHK->fnAction(); // trigger the action, if one has been specified
			} catch (Xmms::result_error& e) {
				std::cerr << PROGNAME "Unable to trigger hotkey action: " << e.what() << std::endl;
			}
		}

		// If this is the only primary hotkey pressed, we're done.  If another
		// hotkey has been pressed, fall through to the subkey code below.  This
		// will let primary hotkeys also be used as subkeys, which allows
		// confusing setups like play=f1+f2, stop=f2+f1.
		if (vcActiveHotkeys.size() == 0) {
			// Flag this hotkey as a currently active one (add to 'active' vector)
			vcActiveHotkeys.push_back(*itHK);
			return;
		}
	}

	// If we're here, a subkey was pressed while one of the actual hotkeys is being held down
	for (VC_HOTKEYS::iterator i = vcActiveHotkeys.begin(); i != vcActiveHotkeys.end(); i++) {
		VC_HOTKEYS::iterator itHK = searchHotkeyVector(i->vcSubActions, hkiType, iKey, iModifier);
		if (itHK != i->vcSubActions.end()) {
			if (itHK->fnAction) {
				std::cout << PROGNAME "Matched subkey " << iKey << ", triggering action" << std::endl;
				try {
					itHK->fnAction(); // trigger the action, if one has been specified
				} catch (Xmms::result_error& e) {
					std::cerr << PROGNAME "Unable to trigger hotkey action: " << e.what() << std::endl;
				}
			}
			return; // matched, don't continue and add as a hotkey if it's a main key
			//break; // won't check for additional matches
		}
	}

	if (itHK != vcHotkeys.end()) {
		// Now we've processed any subkeys *without* the primary hotkey being
		// flagged active, time to flag it active for any subsequent keys.

		VC_HOTKEYS::iterator itActHK = searchHotkeyVector(vcActiveHotkeys,
			hkiType, iKey, iModifier);
		// ...but only if it's not already in the list of activated hotkeys
		if (itActHK == vcHotkeys.end()) {
			// Flag this hotkey as a currently active one (add to 'active' vector)
			vcActiveHotkeys.push_back(*itHK);
		}
	}
	/*for (VC_HOTKEYS::iterator i = vcActiveHotkeys.begin(); i != vcActiveHotkeys.end(); i++) {
		std::cout << "active hotkeys: " << i->iKey << "\n";
	}*/
	return;
}

void processKeyrelease(int hkiType, int iKey, int iModifier)
{
	VC_HOTKEYS::iterator itHK = searchHotkeyVector(vcActiveHotkeys, hkiType, iKey, iModifier);
	if (itHK != vcActiveHotkeys.end()) {
		vcActiveHotkeys.erase(itHK);
	}
	/*for (VC_HOTKEYS::iterator i = vcActiveHotkeys.begin(); i != vcActiveHotkeys.end(); i++) {
		std::cout << "active hotkeys: " << i->iKey << "\n";
	}*/
	return;
}

#ifdef USE_X11
struct bindX11 {
	Display *d;

	bindX11(const char *cDisplay)
	{
		this->d = XOpenDisplay(cDisplay);
		if (!this->d) throw EBindFailed(cDisplay, "Unable to open X11 display.");
		std::cout << PROGNAME "Opened X11 display " << (cDisplay ? cDisplay : "<default>") << std::endl;
	}

	// Thread entrypoint
	void operator()()
	{
		Window w = RootWindow(this->d, DefaultScreen(this->d));

		// Grab all the hotkeys
		for (VC_HOTKEYS::iterator i = vcHotkeys.begin(); i != vcHotkeys.end(); i++) {
			// X11 uses AnyModifier to ignore modifiers, instead of -1
			int iXModifier;
			if (i->iModifier == -1) iXModifier = AnyModifier;
			else iXModifier = i->iModifier;

			switch (i->hkiType) {
				case HK_X11_MOUSE:
					std::cout << PROGNAME "Grabbing X11 button " << i->iKey << std::endl;
					XGrabButton(this->d, i->iKey, iXModifier, w, true, ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None);
					// TODO: Handle BadAccess (hotkey already in use)
					break;
				case HK_X11_KEYBOARD:
					std::cout << PROGNAME "Grabbing X11 key " << i->iKey << std::endl;
					XGrabKey(this->d, i->iKey, iXModifier, w, true, GrabModeAsync, GrabModeAsync);
					// TODO: Handle BadAccess (hotkey already in use)
					break;
			}
		}

		XEvent xev;
		while (XMaskEvent(this->d, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask, &xev) == 0) {
			// TODO: Perhaps monitor "grab lost" events as a good way of resetting the internal state, e.g.
			// if we lose focus or something without receiving the keyup event.)
			switch (xev.type) {
				case KeyPress:
					processKeypress(HK_X11_KEYBOARD, xev.xkey.keycode, xev.xkey.state);
					break;
				case ButtonPress:
					processKeypress(HK_X11_MOUSE, xev.xbutton.button, xev.xbutton.state);
					break;
				case KeyRelease:
					processKeyrelease(HK_X11_KEYBOARD, xev.xkey.keycode, xev.xkey.state);
					break;
				case ButtonRelease:
					processKeyrelease(HK_X11_MOUSE, xev.xbutton.button, xev.xbutton.state);
					break;
			}
		}

	}
};
#endif // USE_X11

#ifdef USE_EVDEV
struct bindEvdev {
	int devHandle;
	int iDevice;
	std::string strDevName;

	bindEvdev(int iDevice, std::string strDevName) :
		iDevice(iDevice),
		strDevName(strDevName)
	{
		this->devHandle = open(strDevName.c_str(), O_RDONLY);
		if (this->devHandle < 0) throw EBindFailed(strDevName, strerror(errno));
	}

	void operator()()
	{
		#define NUM_EVENTS 64  // maximum number of events to retrieve in one call
		struct input_event events[NUM_EVENTS];
		int iState = 0;
		for (;;) {
			if (this->devHandle == -1) {
				// Device was closed/lost, but not yet reopened
				this->devHandle = open(this->strDevName.c_str(), O_RDONLY);
				if (this->devHandle < 0) {
					// Device doesn't exist yet
					sleep(1);
					continue;
				} else {
					std::cerr << PROGNAME "Successfully reopened device "
						<< this->strDevName << std::endl;
				}
			}

			size_t iNumBytes = read(this->devHandle, events, sizeof(struct input_event) * NUM_EVENTS);

			if (iNumBytes == (size_t)-1) {
				// If we've been interrupted, exit the thread
				if (errno == EINTR) break;

				if (errno == ENODEV) {
					// Device has been removed
					std::cerr << PROGNAME "Lost device " << this->strDevName << std::endl;
					close(this->devHandle);
					this->devHandle = -1;
					continue;
				}

				std::cerr << PROGNAME "Error reading from " << this->strDevName
					<< ": " << strerror(errno)
					<< " - not monitoring this device any more." << std::endl;
				break;

			} else if (iNumBytes < sizeof(struct input_event)) {
				std::cerr << PROGNAME "Short read from evdev device "
					<< this->strDevName
					<< " (only an incomplete event was returned, ignoring)" << std::endl;
				continue;
			}
			for (size_t i = 0; i < iNumBytes / sizeof(struct input_event); i++) {

				// Massive hack to make mousewheel events appear as keypresses
				if (events[i].type == EV_REL) {

					// Ignore normal mouse movements (for performance reasons)
					if ((events[i].code == REL_X) || (events[i].code == REL_Y)) continue;

					events[i].code *= 2;
					events[i].code += 0x1000; // larger than KEY_MAX
					if (events[i].value > 0) events[i].code++; // scrolling down

					if (::config.bShowEvents) {
						std::cout << PROGNAME "Key " << events[i].code << " pressed." << std::endl;
					}

					// Can't hold these "buttons" down, so do a quick press then release
					processKeypress(HK_EVDEV + this->iDevice, events[i].code, iState);
					processKeyrelease(HK_EVDEV + this->iDevice, events[i].code, iState);

				} else if (events[i].type == EV_KEY) { // key event
					if ((::config.bShowEvents) &&
						((events[i].value == 1) || (events[i].value == 2)))
					{
						std::cout << PROGNAME "Key " << events[i].code << " pressed." << std::endl;
					}

					switch (events[i].value) {
						case 0: // key release
							processKeyrelease(HK_EVDEV + this->iDevice, events[i].code, iState);
							break;
						case 1: // key press
						case 2: // autorepeat
							processKeypress(HK_EVDEV + this->iDevice, events[i].code, iState);
							break;
					}
					// Because evdev doesn't handle the keyboard state, we need to do this ourselves
					if (events[i].value < 2) { // only handle 0 (keyrelease) and 1 (keypress)
						switch (events[i].code) {
							case KEY_LEFTSHIFT:
							case KEY_RIGHTSHIFT: // 1
								iState = (iState &~ 1) | (events[i].value & 1);
								break;
							case KEY_LEFTCTRL:
							case KEY_RIGHTCTRL:
								iState = (iState &~ (1<<2)) | ((events[i].value & 1) << 2);
								break;
							case KEY_LEFTALT:
							case KEY_RIGHTALT:
								iState = (iState &~ (1<<3)) | ((events[i].value & 1) << 3);
								break;
						}
					}
				}
//printf("type: %d, code: %d, value %d\n", events[i].type, events[i].code, events[i].value);
			}
		}
		close(this->devHandle);
		return;
	}
};
#endif // USE_EVDEV

// Callback function used when loading the list of hotkeys from the config file
void loadHotkey(int hkiType, int iKey, int iModifier, int iSubkey, boost::function<void()> fnAction)
{
	VC_HOTKEYS::iterator itHKmain = searchHotkeyVector(::vcHotkeys, hkiType, iKey, iModifier);
	if (itHKmain != ::vcHotkeys.end()) {
		// Found this main hotkey
		if (iSubkey != 0) {
			// New subaction to add
			HOTKEY hk;
			hk.hkiType = hkiType;
			hk.iKey = iSubkey;
			hk.iModifier = iModifier;
			hk.fnAction = fnAction;
			itHKmain->vcSubActions.push_back(hk);
			std::cerr << PROGNAME "Added subkey " << iSubkey << " under existing parent hotkey " << iKey << "+" << iModifier << std::endl;
		} else {
			// Duplicate hotkey (could mean a subkey was added before the original hotkey)
			if (itHKmain->fnAction) {
				std::cerr << PROGNAME "Warning: Cannot assign same hotkey to multiple actions." << std::endl;
			} else {
				// This global hotkey has no action assigned, so it was probably added when a subkey appeared first and needed
				// a parent to hang from.  Update the existing parent with the new action.
				itHKmain->fnAction = fnAction;
				std::cerr << PROGNAME "Added action to existing normal/parent hotkey " << iKey << "+" << iModifier << std::endl;
				// No need to update the key or modifier, because we've already done a search and matched against those values
				// to end up here.
			}
		}
	} else {
		// This key doesn't exist yet
		if (iSubkey != 0) {
			// This is a subkey, but its parent hasn't been added yet (and may never be if it won't have an action assigned
			// to it.)  Add a dummy/blank parent.
			HOTKEY hk;
			hk.hkiType = hkiType;
			hk.iKey = iKey;
			hk.iModifier = iModifier;
			hk.fnAction = NULL;
				HOTKEY hkSub;
				hkSub.hkiType = hkiType;
				hkSub.iKey = iSubkey;
				hkSub.iModifier = iModifier;
				hkSub.fnAction = fnAction;
				hk.vcSubActions.push_back(hkSub);
			::vcHotkeys.push_back(hk);
			std::cerr << PROGNAME "Added subkey " << iSubkey << " under new parent hotkey " << iKey << "+" << iModifier << std::endl;
		} else {
			// This is a normal hotkey that hasn't yet been added to the main list.  This will be the most common case.
			HOTKEY hk;
			hk.hkiType = hkiType;
			hk.iKey = iKey;
			hk.iModifier = iModifier;
			hk.fnAction = fnAction;
			::vcHotkeys.push_back(hk);
			std::cerr << PROGNAME "Added normal/parent hotkey " << iKey << "+" << iModifier << std::endl;
		}
	}
	return;
}

// Find a key in the list of key definitions
VC_HOTKEYS& findKeyDef(MP_KEYDEFS& mpKeyDefs, const std::string& strKey)
{
	for (MP_KEYDEFS::iterator i = mpKeyDefs.begin(); i != mpKeyDefs.end(); i++) {
		if (strKey.compare(i->first) == 0) {
			return i->second;
		}
	}
	throw std::exception();
}

void xmmsdc(Xmms::Client *client)
{
	if (!client->isConnected()) {
		try {
			client->connect(std::getenv("XMMS_PATH"));
		} catch (Xmms::connection_error& e) {
			std::cerr << PROGNAME "Could not reconnect to XMMS2 daemon: " << e.what() << std::endl;
		}
	}
	return;
}

int main(void)//int iArgC, char *cArgV[])
{
	// Defaults, overridden later by config file values (if any)
	::config.iSeekDelta = 5000;   // milliseconds
	::config.iVolDelta = 5;       // percent
	::config.bShowEvents = false; // print all keycodes (evdev only)

	// Connect to XMMS2
	Xmms::Client client("xmms2hotkey");
	try {
		client.connect(std::getenv("XMMS_PATH"));
	} catch (Xmms::connection_error& e) {
		std::cerr << PROGNAME "Connection error: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	client.setDisconnectCallback(boost::bind(xmmsdc, &client));

	std::string strConfigFilename = Xmms::getUserConfDir() + "/clients/xmms2hotkey.conf";
	std::cout << PROGNAME "Loading config from " << strConfigFilename << std::endl;

	std::vector<std::string> vcXDisplays;
	std::vector<EVDEV_INFO> vcEvDev;

	MP_KEYDEFS mpKeyDefs;

//	po::variables_map cfg;
	try {
		std::ifstream cfgStream(strConfigFilename.c_str());
		po::options_description optDummy;
		po::parsed_options pa = po::parse_config_file(cfgStream, optDummy, true);
		// Process all the devices first
		for (std::vector<po::option>::iterator i = pa.options.begin(); i != pa.options.end(); i++) {

			if (i->string_key.compare("listen.x11") == 0) {
				vcXDisplays.push_back(i->value[0]);

			} else if (i->string_key.compare(0, 12, "listen.evdev") == 0) {
				EVDEV_INFO evi;
				evi.iIndex = vcEvDev.size();
				evi.strId = i->string_key.substr(7);
				evi.strDevicePath = i->value[0];
				vcEvDev.push_back(evi);

			} else if (i->string_key.compare("main.seek_step") == 0) {
				::config.iSeekDelta = strtoul(i->value[0].c_str(), NULL, 10);

			} else if (i->string_key.compare("main.volume_step") == 0) {
				::config.iVolDelta = strtoul(i->value[0].c_str(), NULL, 10);

			} else if (i->string_key.compare("main.show_keycodes") == 0) {
				if (i->value[0].compare("true") == 0) ::config.bShowEvents = true;
			}
		}

		// Then process the key definitions
		for (std::vector<po::option>::iterator i = pa.options.begin(); i != pa.options.end(); i++) {
			if (i->string_key.compare(0, 4, "key.") == 0) {
				struct hotkey hkNew;

				int iNextDot = i->string_key.find_first_of('.', 4);
				std::string strKeyName = i->string_key.substr(4, iNextDot - 4);
				std::string strDevName = i->string_key.substr(iNextDot + 1);

				if (strDevName.compare("x11kb") == 0) {
					hkNew.hkiType = HK_X11_KEYBOARD;
				} else if (strDevName.compare("x11m") == 0) {
					hkNew.hkiType = HK_X11_MOUSE;
				} else if (strDevName.compare(0, 5, "evdev") == 0) {
					int iEvdevIndex = strtoul(strDevName.substr(5).c_str(), NULL, 10);
					hkNew.hkiType = HK_EVDEV + iEvdevIndex;
				} else {
					std::cerr << PROGNAME "Unknown device type \"" << strDevName << "\", ignoring." << std::endl;
					continue;
				}

				// program_options code hasn't yet combined repeated options into a
				// vector, but let's make sure.
				assert(i->value.size() == 1);

				//for (std::vector<std::string>::iterator j = i->value.begin(); j != i->value.end(); j++) {
				std::string& strValue = i->value[0];
				std::string::size_type iComma = strValue.find_first_of(',');
				//int iModifier, iKeycode;
				if (iComma == std::string::npos) {
					hkNew.iModifier = -1; // Will get changed to AnyModifier for X11
					hkNew.iKey = strtoul(strValue.c_str(), NULL, 10);
				} else {
					hkNew.iModifier = strtoul(strValue.substr(0, iComma).c_str(), NULL, 10);
					hkNew.iKey = strtoul(strValue.substr(iComma + 1).c_str(), NULL, 10);
				}

				/*std::cout << "STORE KEYDEF " << strKeyName << ": type " << hkNew.hkiType
					<< ", mod " << hkNew.iModifier << ", keycode " << hkNew.iKey << std::endl;*/
				MP_KEYDEFS::iterator itDef = mpKeyDefs.find(strKeyName);
				mpKeyDefs[strKeyName].push_back(hkNew);
			}
		}
		// Then process the event definitions
		for (std::vector<po::option>::iterator i = pa.options.begin(); i != pa.options.end(); i++) {
			if (i->string_key.compare(0, 7, "events.") == 0) {
				std::string strEvent = i->string_key.substr(7);
				boost::function<void()> fnAction;
				if (strEvent.compare("stop") == 0)
					fnAction = boost::bind(&Xmms::Playback::stop, &client.playback);
				else if (strEvent.compare("play") == 0)
					fnAction = boost::bind(&Xmms::Playback::start, &client.playback);
				else if (strEvent.compare("pause") == 0)
					fnAction = boost::bind(&Xmms::Playback::pause, &client.playback);

				else if (strEvent.compare("seekfwd") == 0)
					fnAction = boost::bind(&Xmms::Playback::seekMsRel, &client.playback, ::config.iSeekDelta);
				else if (strEvent.compare("seekback") == 0)
					fnAction = boost::bind(&Xmms::Playback::seekMsRel, &client.playback, -::config.iSeekDelta);

				else if (strEvent.compare("skipnext") == 0)
					fnAction = boost::bind(&Xmms2Hotkey::skipTrack, &client, 1);
				else if (strEvent.compare("skipprev") == 0)
					fnAction = boost::bind(&Xmms2Hotkey::skipTrack, &client, -1);

				else if (strEvent.compare("playpause") == 0)
					fnAction = boost::bind(&Xmms2Hotkey::playpause, &client.playback);
				else if (strEvent.compare("volup") == 0)
					fnAction = boost::bind(&Xmms2Hotkey::volChange, &client.playback, ::config.iVolDelta);
				else if (strEvent.compare("voldown") == 0)
					fnAction = boost::bind(&Xmms2Hotkey::volChange, &client.playback, -::config.iVolDelta);
				else {
					std::cerr << PROGNAME "Unknown action \"" << strEvent << "\", ignoring." << std::endl;
					continue;
				}
				std::string strKeys = i->value[0];
				std::string::size_type iPlusPos = strKeys.find_first_of('+');

				std::string strMainKey, strSubKey;
				if (iPlusPos != std::string::npos) {
					// There's a plus in here
					strMainKey = strKeys.substr(0, iPlusPos);
					strSubKey = strKeys.substr(iPlusPos + 1);
				} else {
					// Just one key
					strMainKey = strKeys;
				}

				// Find the key(s)
				VC_HOTKEYS vcGrabKeys;
				VC_HOTKEYS vcGrabSubKeys;
				try {
					vcGrabKeys = findKeyDef(mpKeyDefs, strMainKey);
					if (!strSubKey.empty()) vcGrabSubKeys = findKeyDef(mpKeyDefs, strSubKey);
				} catch (std::exception) {
					throw EUndefinedKey(strMainKey, strEvent);
				}

				// Found the key, grab all the necessary keycodes
				for (VC_HOTKEYS::iterator j = vcGrabKeys.begin(); j != vcGrabKeys.end(); j++) {
					/*int iModifier;
					// Map -1 to the appropriate "any" modifier for X11 only
					if ((j->iModifier == -1) && ((j->hkiType == HK_X11_KEYBOARD) || (j->hkiType == HK_X11_MOUSE))) {
						iModifier = AnyModifier;
					} else {
						iModifier = j->iModifier;
					}*/
					if (!strSubKey.empty()) {
						for (VC_HOTKEYS::iterator s = vcGrabSubKeys.begin(); s != vcGrabSubKeys.end(); s++) {
							if (s->hkiType == j->hkiType) {
								/*std::cout << "load " << j->hkiType << " key " << strMainKey << "-"
									<< j->iKey << "." << j->iModifier
									<< " sub " << strSubKey << "-" << s->iKey << "\n";*/
								loadHotkey(j->hkiType, j->iKey, j->iModifier, s->iKey, fnAction);
							}
						}
					} else {
						loadHotkey(j->hkiType, j->iKey, j->iModifier, 0, fnAction);
					}
				}
			}
		}

	} catch (std::exception& e) {
		std::cerr << PROGNAME "Error parsing configuration file: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	/*std::cout << "Dumping keydefs:\n";
	for (MP_KEYDEFS::iterator i = mpKeyDefs.begin(); i != mpKeyDefs.end(); i++) {
		std::cout << i->first << ": \n";
		for (VC_HOTKEYS::iterator j = i->second.begin(); j != i->second.end(); j++) {
			std::cout << "  " << j->iModifier << " / " << j->iKey << "\n";
		}
	}*/

	// Run each bind function (X11 display and evdev device) in a separate thread
	boost::thread_group threads;

#ifdef USE_EVDEV
	for (std::vector<EVDEV_INFO>::iterator i = vcEvDev.begin(); i != vcEvDev.end(); i++) {
		try {
			bindEvdev o(i->iIndex, i->strDevicePath);
			threads.create_thread(o);
		} catch (std::exception& e) {
			std::cerr << e.what() << std::endl;
		}
	}
#endif

#ifdef USE_X11
	for (std::vector<std::string>::iterator i = vcXDisplays.begin(); i != vcXDisplays.end(); i++) {
		try {
			if (i->compare("default") == 0) {
				bindX11 o(NULL);
				threads.create_thread(o);
			} else {
				bindX11 o(i->c_str());
				threads.create_thread(o);
			}
		} catch (std::exception& e) {
			std::cerr << e.what() << std::endl;
		}
	}
#endif

	// TODO: Figure out how to wait for Ctrl+C/SIGTERM and exit cleanly

	// Wait here until all the threads have terminated.
	std::cout << PROGNAME "Waiting for threads to terminate." << std::endl;
	threads.join_all();

	std::cout << PROGNAME "Exiting." << std::endl;
	return 0;
}
