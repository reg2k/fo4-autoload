#include <shlobj.h>
#include <vector>
#include <algorithm>

#include "f4se/PluginAPI.h"
#include "f4se/GameAPI.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/SafeWrite.h"
#include "f4se_common/BranchTrampoline.h"

#include "f4se/ScaleformValue.h"
#include "f4se/ScaleformCallbacks.h"

#include "xbyak/xbyak.h"

#include "utils.h"
#include "Config.h"
#include "rva/RVA.h"

#define INI_LOCATION "./Data/F4SE/autoload.ini"

IDebugLog gLog;
PluginHandle g_pluginHandle = kPluginHandle_Invalid;

F4SEScaleformInterface	*g_scaleform = NULL;
F4SEMessagingInterface  *g_messaging = NULL;

//--------------------
// Addresses [5]
//--------------------

typedef void(*_ExecuteCommand)(const char* str);
RVA<_ExecuteCommand> ExecuteCommand({
    { RUNTIME_VERSION_1_10_64, 0x0125B320 },
    { RUNTIME_VERSION_1_10_40, 0x0125AE40 },
    { RUNTIME_VERSION_1_10_26, 0x012594F0 },
    { RUNTIME_VERSION_1_10_20, 0x01259430 }
}, "40 53 55 56 57 41 54 48 81 EC 40 08 00 00");

RVA<int> AlwaysActive_Check({
    { RUNTIME_VERSION_1_10_64, 0x00D36C38 },
    { RUNTIME_VERSION_1_10_40, 0x00D36758 },
    { RUNTIME_VERSION_1_10_26, 0x00D34E08 },
    { RUNTIME_VERSION_1_10_20, 0x00D34D98 }
}, "75 2D 40 84 FF");

RVA<uintptr_t> ModsLoaded_Check({
    { RUNTIME_VERSION_1_10_64, 0x00CED622 },
    { RUNTIME_VERSION_1_10_40, 0x00CED142 },
    { RUNTIME_VERSION_1_10_26, 0x00CEB7F2 },
    { RUNTIME_VERSION_1_10_20, 0x00CEB782 }
}, "0F 84 ? ? ? ? 48 8D 4D 80 33 D2");

RVA<uintptr_t> MissingPlugins_Check({
    { RUNTIME_VERSION_1_10_64, 0x00CDE986 },
    { RUNTIME_VERSION_1_10_40, 0x00CDE4A6 },
    { RUNTIME_VERSION_1_10_26, 0x00CDCB56 },
    { RUNTIME_VERSION_1_10_20, 0x00CDCAE6 }
}, "75 25 40 84 F6");

RVA<uintptr_t> UI_OpenOrCloseMenu_HookTarget({
    { RUNTIME_VERSION_1_10_64, 0x0204CA3F },
    { RUNTIME_VERSION_1_10_40, 0x0204CBAF },
    { RUNTIME_VERSION_1_10_26, 0x0204B24F },
    { RUNTIME_VERSION_1_10_20, 0x0204B18F }
}, "E8 ? ? ? ? EB 03 49 8B C4 48 8D 4D 08");

typedef void(*_UI_OpenOrCloseMenu_Internal)(void* unk1, BSFixedString* menuName, unsigned int menuAction);	// 1 = open, 3 = close
_UI_OpenOrCloseMenu_Internal UI_OpenOrCloseMenu_Original = nullptr;

// State
bool loadDone = false;
bool postLoadActionDone = false;
bool loadComplete = false;	    // set to true when kMessage_PostLoadGame is received.
bool shiftKeyPressed = false;	// set to true if user has the shift key pressed during the key checks.

// User Settings
int autoloadMode;
bool loadSaves;
bool loadNamedSaves;
bool loadAutosaves;
bool loadQuicksaves;
bool loadExitsaves;
char fileToLoad[MAX_PATH];
char savePathSpecified[MAX_PATH];
char postLoadCommand[1024];
bool holdShiftToEnable;
bool focusOnLoad;
bool flashOnLoad;

void doLoadGame() {
	_MESSAGE("autoload start");

	// Execute the pre-load command
	char preLoadCommand[1024];
	GetPrivateProfileString("Autoload", "sPreLoadCommand", NULL, preLoadCommand, 1024, INI_LOCATION);
	if (preLoadCommand && strlen(preLoadCommand) > 0) {
		_MESSAGE("Executing pre-load command: %s", preLoadCommand);
		ExecuteCommand(preLoadCommand);
	}

	// Load the specified savefile.
	GetPrivateProfileString("Autoload", "sFileToLoad", NULL, fileToLoad, MAX_PATH, INI_LOCATION);

	if (!fileToLoad || strlen(fileToLoad) == 0) {
		char documentsFolder[MAX_PATH];
		SHGetFolderPath(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, documentsFolder);

		char savesDirectory[MAX_PATH];
		GetPrivateProfileString("Autoload", "sSavesPath", NULL, savePathSpecified, MAX_PATH, INI_LOCATION);
		if (!savePathSpecified || strlen(savePathSpecified) == 0) {
			snprintf(savesDirectory, MAX_PATH, "%s%s", documentsFolder, "\\My Games\\Fallout4\\Saves\\*.fos");
		} else {
			// There may be a more elegant solution here--I have never used snprintf()
			if (strchr(savePathSpecified, ' ')) {
				snprintf(savesDirectory, MAX_PATH, "\"%s\"", savePathSpecified);
			} else {
				snprintf(savesDirectory, MAX_PATH, "%s", savePathSpecified);
			}
		}

		_MESSAGE("save directory: %s", savesDirectory);

		HANDLE hFind;
		WIN32_FIND_DATA data;
		std::vector<WIN32_FIND_DATA> savefiles;

		hFind = FindFirstFile(savesDirectory, &data);
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				//_MESSAGE("%s\n", data.cFileName);
				if (strstr(data.cFileName, "Autosave") == data.cFileName) {
					if (!loadAutosaves) continue;
				} else if (strstr(data.cFileName, "Quicksave") == data.cFileName) {
					if (!loadQuicksaves) continue;
				} else if (strstr(data.cFileName, "Exitsave") == data.cFileName) {
					if (!loadExitsaves) continue;
				} else if (strstr(data.cFileName, "Save") == data.cFileName) {
					if (!loadSaves) continue;
				} else {
					if (!loadNamedSaves) continue;
				}
				savefiles.push_back(data);
			} while (FindNextFile(hFind, &data));
			FindClose(hFind);
		}

		std::sort(savefiles.begin(), savefiles.end(), [](const WIN32_FIND_DATA& lhs, const WIN32_FIND_DATA& rhs)
		{
			return (CompareFileTime(&lhs.ftLastWriteTime, &rhs.ftLastWriteTime) > 0);
		});

		if (savefiles.size() > 0) {
			_MESSAGE("latest save: %s", savefiles[0].cFileName);

			// Exclude file extension (.fos)
			strncpy_s(fileToLoad, savefiles[0].cFileName, strlen(savefiles[0].cFileName) - 4);
		} else {
			_WARNING("No saves matching criteria were found.");
		}

    } else {
        _MESSAGE("Loading specified file.");
    }

	if (fileToLoad && strlen(fileToLoad) > 0) {
		char cmd[MAX_PATH];
		if (strchr(fileToLoad, ' ')) {
			snprintf(cmd, MAX_PATH, "load \"%s\"", fileToLoad);
		} else {
			snprintf(cmd, MAX_PATH, "load %s", fileToLoad);
		}

		_MESSAGE("%s", cmd);

		ExecuteCommand(cmd);
	} else {
		_WARNING("Warning: sFileToLoad is empty. Game will not be loaded.");
	}
}

void doPostLoadAction() {
	_MESSAGE("load complete");

	GetPrivateProfileString("Autoload", "sPostLoadCommand", NULL, postLoadCommand, sizeof(postLoadCommand), INI_LOCATION);
	if (postLoadCommand && strlen(postLoadCommand) > 0) {
		_MESSAGE("Running post-load command: %s", postLoadCommand);
		ExecuteCommand(postLoadCommand);
	}

	// Sleep until we're activated
	/*HWND window = FindWindow("Fallout4", NULL);
	char windowName[100];
	if (GetForegroundWindow() != window) {
		_MESSAGE("going to sleep");
	}
	while (GetForegroundWindow() != window) {
		GetWindowText(GetForegroundWindow(), windowName, 100);
		_MESSAGE("foreground window: %s", windowName);
		Sleep(100);
	}
	_MESSAGE("awoken");*/

}

// Checks for the shift key state to determine whether to skip (or enable) autoloading.
// We check for the shift key state twice - once on launch (console window), and again when MainMenu is opened.
// This gives the user more time to decide.
void checkShiftKeyState() {
	short shiftKeyState = GetAsyncKeyState(VK_SHIFT);
	if ((1 << 15) & shiftKeyState) {
		// Shift is pressed.
		if (!holdShiftToEnable) {
			_MESSAGE("Shift is held down - skipping autoload.");
			loadDone = true;
			loadComplete = true;
		} else {
			// holdShiftToEnable is ON, and shift is explicitly being held down - make this setting sticky.
			_MESSAGE("Shift is held down - enabling autoload.");
			shiftKeyPressed = true;
			loadDone = false;
			loadComplete = false;
		}
	} else {
		// Shift is not pressed.
		if (holdShiftToEnable && !shiftKeyPressed) {
			//_MESSAGE("Shift is not held down - skipping autoload.");
			loadDone = true;
			loadComplete = true;
		}
	}
}

bool RegisterScaleform(GFxMovieView * view, GFxValue * f4se_root)
{
	
	GFxMovieRoot *movieRoot = view->movieRoot;
	GFxValue      currentSWFPath;
	movieRoot->GetVariable(&currentSWFPath, "root.loaderInfo.url");

	//_MESSAGE("The current SWF is: %s.", currentSWFPath.GetString());
	
	if (strcmp(currentSWFPath.GetString(), "Interface/MainMenu.swf") == 0) {
		checkShiftKeyState();
		if (!loadDone) {
			loadDone = true;

			// Stop window flash
			/*HWND window = FindWindow(NULL, "Fallout4");
			FLASHWINFO flashInfo;
			flashInfo.cbSize = sizeof(flashInfo);
			flashInfo.hwnd = window;
			flashInfo.dwFlags = FLASHW_STOP;
			flashInfo.uCount = 0;
			flashInfo.dwTimeout = 0;
			::FlashWindowEx(&flashInfo);*/

			doLoadGame();
		}
	}

	return true;
}

void UI_OpenOrCloseMenu_Hook(void* unk1, BSFixedString* menuName, unsigned int menuAction) {
	const char* menuNameStr = menuName->c_str();

	if (!postLoadActionDone && loadComplete && loadDone) {
		_MESSAGE("Menu %s, action %d", menuNameStr, menuAction);
		if (!strcmp(menuNameStr, "FaderMenu") && menuAction == 3) {
			postLoadActionDone = true;
			doPostLoadAction();
		}
	}

	UI_OpenOrCloseMenu_Original(unk1, menuName, menuAction);
}

// Event Handling
void onF4SEMessage(F4SEMessagingInterface::Message* msg) {
	if (!loadComplete) _MESSAGE("game event: type %d and len %d", msg->type, msg->dataLen);

	switch (msg->type) {
		case F4SEMessagingInterface::kMessage_GameLoaded:
		{
			// Make sure if the game starts in the background that it doesn't just suspend and not do anything.
			if (!loadComplete) {
                SafeWrite8(AlwaysActive_Check.GetUIntPtr(), 0xEB);    // JNZ -> JMP
			}
            
			break;
		}

		case F4SEMessagingInterface::kMessage_PreLoadGame:
		{
			loadComplete = false;

            SafeWrite8(AlwaysActive_Check.GetUIntPtr(), 0x75);    // restore JNZ

			// Set savefile in INI if shift is held down
			short shiftKeyState = GetAsyncKeyState(VK_SHIFT);
			if ((1 << 15) & shiftKeyState) {
				char* savefile = (char*)(msg->data);
				if (msg->dataLen > 0 && savefile) {
					WritePrivateProfileString("Autoload", "sFileToLoad", savefile, INI_LOCATION);
					_MESSAGE("sFileToLoad updated: %s", savefile);
				}
			}

			break;
		}

		case F4SEMessagingInterface::kMessage_PostLoadGame:
			loadComplete = true;
			bool success = (msg->data != 0);
			_MESSAGE("Game loaded: %d", success);
			if (success) {
				// See if we're in the foreground
				if (GetForegroundWindow() == FindWindow(NULL, "Fallout4")) {
					_MESSAGE("Fallout 4 in foreground.");
				} else {
					_MESSAGE("Fallout 4 not in foreground");
				}

				if (flashOnLoad || focusOnLoad) {
					HWND window = FindWindow(NULL, "Fallout4");

					// Flash window if desired.
					if (flashOnLoad) {
						FLASHWINFO flashInfo;
						flashInfo.cbSize = sizeof(flashInfo);
						flashInfo.hwnd = window;
						flashInfo.dwFlags = FLASHW_TRAY;
						flashInfo.uCount = 3;
						flashInfo.dwTimeout = 0;
						::FlashWindowEx(&flashInfo);
					}

					// Bring game to front if desired.
					if (focusOnLoad) {
						_MESSAGE("Bringing game to front.");
						HWND hCurWnd = ::GetForegroundWindow();
						DWORD dwMyID = ::GetCurrentThreadId();
						DWORD dwCurID = ::GetWindowThreadProcessId(hCurWnd, NULL);
						::AttachThreadInput(dwCurID, dwMyID, TRUE);
						::SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
						::SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
						::SetForegroundWindow(window);
						::AttachThreadInput(dwCurID, dwMyID, FALSE);
						::SetFocus(window);
						::SetActiveWindow(window);
						// ...yeah. thanks microsoft.
					}
				}

				
			}
			break;
	}
}

extern "C"
{

bool F4SEPlugin_Query(const F4SEInterface * f4se, PluginInfo * info)
{
	std::string logPath = "\\My Games\\Fallout4\\F4SE\\";
	logPath += PLUGIN_NAME_SHORT;
	logPath += ".log";
	gLog.OpenRelative(CSIDL_MYDOCUMENTS, logPath.c_str());

	_MESSAGE("%s v%s", PLUGIN_NAME_SHORT, PLUGIN_VERSION_STRING);
	_MESSAGE("%s query", PLUGIN_NAME_SHORT);

	// populate info structure
	info->infoVersion =	PluginInfo::kInfoVersion;
	info->name	  =		PLUGIN_NAME_SHORT;
	info->version =		PLUGIN_VERSION;

	// store plugin handle so we can identify ourselves later
	g_pluginHandle = f4se->GetPluginHandle();

	// Check user setting
	bool bEnable = !!GetPrivateProfileInt("Autoload", "bEnable", 0, INI_LOCATION);
	if (bEnable) {
		_MESSAGE("autoload is enabled.");
	} else {
		_MESSAGE("autoload is not enabled and will not be loaded.");
		return false;
	}

	// Check game version
	if (f4se->runtimeVersion != SUPPORTED_RUNTIME_VERSION) {
		char str[512];
		sprintf_s(str, sizeof(str), "Your game version: v%d.%d.%d.%d\nExpected version: v%d.%d.%d.%d\n%s will be disabled.",
			GET_EXE_VERSION_MAJOR(f4se->runtimeVersion),
			GET_EXE_VERSION_MINOR(f4se->runtimeVersion),
			GET_EXE_VERSION_BUILD(f4se->runtimeVersion),
			GET_EXE_VERSION_SUB(f4se->runtimeVersion),
			GET_EXE_VERSION_MAJOR(SUPPORTED_RUNTIME_VERSION),
			GET_EXE_VERSION_MINOR(SUPPORTED_RUNTIME_VERSION),
			GET_EXE_VERSION_BUILD(SUPPORTED_RUNTIME_VERSION),
			GET_EXE_VERSION_SUB(SUPPORTED_RUNTIME_VERSION),
			PLUGIN_NAME_LONG
		);

		MessageBox(NULL, str, PLUGIN_NAME_LONG, MB_OK | MB_ICONEXCLAMATION);
		return false;
	}

    if (f4se->runtimeVersion > SUPPORTED_RUNTIME_VERSION) {
        _MESSAGE("INFO: Newer game version (%08X) than target (%08X).", f4se->runtimeVersion, SUPPORTED_RUNTIME_VERSION);
    }

	// Get scaleform interface
	g_scaleform = (F4SEScaleformInterface *)f4se->QueryInterface(kInterface_Scaleform);

	// Get messaging interface
	g_messaging = (F4SEMessagingInterface *)f4se->QueryInterface(kInterface_Messaging);

	return true;
}

bool F4SEPlugin_Load(const F4SEInterface *f4se)
{
    _MESSAGE("%s load", PLUGIN_NAME_SHORT);

    RVAManager::UpdateAddresses(f4se->runtimeVersion);

	// Register Scaleform creation callbacks.
	g_scaleform->Register("autoload", RegisterScaleform);

	// Grab configuration
	autoloadMode	= GetPrivateProfileInt("Autoload", "iAutoloadMode", 1, INI_LOCATION);
	loadSaves		= !!GetPrivateProfileInt("Autoload", "bLoadSaves", 1, INI_LOCATION);
	loadNamedSaves	= !!GetPrivateProfileInt("Autoload", "bLoadNamedSaves", 1, INI_LOCATION);
	loadAutosaves	= !!GetPrivateProfileInt("Autoload", "bLoadAutosaves", 0, INI_LOCATION);
	loadQuicksaves	= !!GetPrivateProfileInt("Autoload", "bLoadQuicksaves", 0, INI_LOCATION);
	loadExitsaves	= !!GetPrivateProfileInt("Autoload", "bLoadExitsaves", 0, INI_LOCATION);

	holdShiftToEnable = !!GetPrivateProfileInt("Autoload", "bHoldShiftToAutoload", 0, INI_LOCATION);
	focusOnLoad		  = !!GetPrivateProfileInt("Autoload", "bFocusOnLoad", 0, INI_LOCATION);
	flashOnLoad		  = !!GetPrivateProfileInt("Autoload", "bFlashOnLoad", 0, INI_LOCATION);

	// Register for F4SE messages
	g_messaging->RegisterListener(g_pluginHandle, "F4SE", onF4SEMessage);

	if (!g_branchTrampoline.Create(1024 * 64)) {
		_ERROR("couldn't create branch trampoline. this is fatal. skipping remainder of init process.");
		return false;
	}

	// Install hook for menu open/close
	SInt32 rel32 = 0;
	ReadMemory(UI_OpenOrCloseMenu_HookTarget.GetUIntPtr() + 1, &rel32, sizeof(UInt32));
	UI_OpenOrCloseMenu_Original = reinterpret_cast<_UI_OpenOrCloseMenu_Internal>(UI_OpenOrCloseMenu_HookTarget.GetUIntPtr() + 5 + rel32);
	g_branchTrampoline.Write5Call(UI_OpenOrCloseMenu_HookTarget.GetUIntPtr(), (uintptr_t)UI_OpenOrCloseMenu_Hook);

	// Suppress load dialogs (if requested)
	if (GetPrivateProfileInt("Autoload", "bSuppressAchievementsWarning", 0, INI_LOCATION)) {
		unsigned char data[] = { 0x90, 0xE9 };
		SafeWriteBuf(ModsLoaded_Check.GetUIntPtr(), &data, sizeof(data));
	}

	// Suppress missing plugin warning (if requested)
	if (GetPrivateProfileInt("Autoload", "bSuppressMissingContentWarning", 0, INI_LOCATION)) {
		// 75 -> EB unconditional jump
		unsigned char data[] = { 0xEB };
		SafeWriteBuf(MissingPlugins_Check.GetUIntPtr(), &data, sizeof(data));
	}

	checkShiftKeyState();

	return true;
}

};
