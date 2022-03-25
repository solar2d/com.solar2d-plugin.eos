// --------------------------------------------------------------------------------
// 
// EosLuaInterface.cpp
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// --------------------------------------------------------------------------------

#include "CoronaLua.h"
#include "CoronaMacros.h"
#include "DispatchEventTask.h"
#include "LuaEventDispatcher.h"
#include "PluginConfigLuaSettings.h"
#include "RuntimeContext.h"
#include <cmath>
#include <sstream>
#include <stdint.h>
#include <string>
#include <thread>
extern "C"
{
#	include "lua.h"
#	include "lauxlib.h"
}
#include "eos_sdk.h"
#include "eos_ui.h"
#include "eos_logging.h"

//---------------------------------------------------------------------------------
// Constants
//---------------------------------------------------------------------------------


//---------------------------------------------------------------------------------
// Private Static Variables
//---------------------------------------------------------------------------------

/**
  Gets the thread ID that all plugin instances are currently running in.
  This member is only applicable if at least 1 plugin instance exists.
  Intended to avoid multiple plugin instances from being loaded at the same time on different threads.
 */
static std::thread::id sMainThreadId;

//---------------------------------------------------------------------------------
// Private Static Functions
//---------------------------------------------------------------------------------
/**
  Pushes the EOS plugin table to the top of the Lua stack.
  @param luaStatePointer Pointer to the Lua state to push the plugin table to.
  @return Returns true if the plugin table was successfully pushed to the top of the Lua stack.

          Returns false if failed to load the plugin table. In this case, nil will be pushed to
		  the top of the Lua stack, unless given a null Lua state pointer argument.
 */
bool PushPluginTableTo(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return false;
	}

	// Fetch the current Lua stack count.
	int previousLuaStackCount = lua_gettop(luaStatePointer);

	// Call the Lua require() function to push this plugin's table to the top of the stack.
	bool wasSuccessful = false;
	lua_getglobal(luaStatePointer, "require");
	if (lua_isfunction(luaStatePointer, -1))
	{
		lua_pushstring(luaStatePointer, "plugin.eos");
		CoronaLuaDoCall(luaStatePointer, 1, 1);
		if (lua_istable(luaStatePointer, -1))
		{
			wasSuccessful = true;
		}
		else
		{
			lua_pop(luaStatePointer, 1);
		}
	}
	else
	{
		lua_pop(luaStatePointer, 1);
	}

	// Pop off remaing items above, leaving the plugin's table at the top of the stack.
	// If we've failed to load the plugin, then push nil instead.
	if (wasSuccessful)
	{
		lua_insert(luaStatePointer, previousLuaStackCount + 1);
		lua_settop(luaStatePointer, previousLuaStackCount + 1);
	}
	else
	{
		lua_settop(luaStatePointer, previousLuaStackCount);
		lua_pushnil(luaStatePointer);
	}
	return wasSuccessful;
}

/**
  Determines if the given Lua state is running under the Corona Simulator.
  @param luaStatePointer Pointer to the Lua state to check.
  @return Returns true if the given Lua state is running under the Corona Simulator.

          Returns false if running under a real device/desktop application or if given a null pointer.
 */
bool IsRunningInCoronaSimulator(lua_State* luaStatePointer)
{
	bool isSimulator = false;
	lua_getglobal(luaStatePointer, "system");
	if (lua_istable(luaStatePointer, -1))
	{
		lua_getfield(luaStatePointer, -1, "getInfo");
		if (lua_isfunction(luaStatePointer, -1))
		{
			lua_pushstring(luaStatePointer, "environment");
			int callResultCode = CoronaLuaDoCall(luaStatePointer, 1, 1);
			if (!callResultCode && (lua_type(luaStatePointer, -1) == LUA_TSTRING))
			{
				isSimulator = (strcmp(lua_tostring(luaStatePointer, -1), "simulator") == 0);
			}
		}
		lua_pop(luaStatePointer, 1);
	}
	lua_pop(luaStatePointer, 1);
	return isSimulator;
}

/**
* Callback function to use for EOS SDK log messages
*
* @param InMsg - A structure representing data for a log message
*/
void EOS_CALL onEOSLogMessageReceived(const EOS_LogMessage* InMsg)
{
	if (InMsg->Level != EOS_ELogLevel::EOS_LOG_Off)
	{
		if (InMsg->Level == EOS_ELogLevel::EOS_LOG_Error || InMsg->Level == EOS_ELogLevel::EOS_LOG_Fatal)
		{
			CoronaLog("ERROR: [EOS SDK] %s", InMsg->Message);	
		}
		else if (InMsg->Level == EOS_ELogLevel::EOS_LOG_Warning)
		{
			CoronaLog("WARNING: [EOS SDK] %s", InMsg->Message);	
		}
		else
		{
			CoronaLog("[EOS SDK] %s", InMsg->Message);	
		}
	}
}

void CheckAutoLogin()
{


	// SteamManager starts login automatically
#ifndef EOS_STEAM_ENABLED
	// Check for Launcher commandline params to log in via Exchange Code
	// if (FCommandLine::Get().HasParam(CommandLineConstants::EpicPortal) &&
	// 	FCommandLine::Get().HasParam(CommandLineConstants::LauncherAuthType) &&
	// 	FCommandLine::Get().HasParam(CommandLineConstants::LauncherAuthPassword))
	// {
	// 	if (FCommandLine::Get().GetParamValue(CommandLineConstants::LauncherAuthType) == L"exchangecode")
	// 	{
	// 		std::wstring LauncherExchangeCode = FCommandLine::Get().GetParamValue(CommandLineConstants::LauncherAuthPassword);
	// 		if (!LauncherExchangeCode.empty())
	// 		{
	// 			// Auto login with Exchange Code
	// 			FGameEvent Event(EGameEventType::StartUserLogin, (int)ELoginMode::ExchangeCode, LauncherExchangeCode);
	// 			FGame::Get().OnGameEvent(Event);
	// 			return;
	// 		}
	// 	}
	// }
#endif
}

//---------------------------------------------------------------------------------
// Lua API Handlers
//---------------------------------------------------------------------------------
/** UserInfo eos.getAuthIdToken() */
int OnGetAuthIdToken(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch this plugin's runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}

	// EOS_Auth_IdToken* UserAuthIdToken = nullptr;

	// EOS_Auth_CopyIdTokenOptions CopyTokenOptions = { 0 };
	// CopyTokenOptions.ApiVersion = EOS_AUTH_COPYUSERAUTHTOKEN_API_LATEST;
	// CopyTokenOptions.AccountId = nullptr;

	// if (EOS_Auth_CopyIdToken(AuthHandle, &CopyTokenOptions, UserId, &UserAuthToken) == EOS_EResult::EOS_Success)
	// {
	// 	EOS_Auth_Token_Release(UserAuthToken);
	// }
	// else
	// {
	// 	CoronaLog("WARNING: [EOS SDK] User Auth Token is invalid");
	// }

	return 1;
}

/** bool eos.setNotificationPosition(positionName) */
int OnSetNotificationPosition(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the required position name argument.
	if (lua_type(luaStatePointer, 1) != LUA_TSTRING)
	{
		CoronaLuaError(luaStatePointer, "Given argument is not of type string.");
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}
	const char* positionName = lua_tostring(luaStatePointer, 1);
	if (!positionName)
	{
		positionName = "";
	}

	// Convert the position name to its equivalent Steam enum constant.
	EOS_UI_ENotificationLocation positionId;
	if (!strcmp(positionName, "topLeft"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_TopLeft;
	}
	else if (!strcmp(positionName, "topRight"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_TopRight;
	}
	else if (!strcmp(positionName, "bottomLeft"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_BottomLeft;
	}
	else if (!strcmp(positionName, "bottomRight"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_BottomRight;
	}
	else
	{
		CoronaLuaError(luaStatePointer, "Given unknown position name '%s'", positionName);
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}

	// Fetch the runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	auto eosPlatformHandle = contextPointer->GetPlatformHandle();
	if (eosPlatformHandle)
	{
		// Change EOS's notification position with given setting.
		EOS_HUI ExternalUIHandle = EOS_Platform_GetUIInterface(eosPlatformHandle);

		EOS_UI_SetDisplayPreferenceOptions Options = {};
		Options.ApiVersion = EOS_UI_SETDISPLAYPREFERENCE_API_LATEST;
		Options.NotificationLocation = positionId;

		const EOS_EResult Result = EOS_UI_SetDisplayPreference(ExternalUIHandle, &Options);
		if (Result == EOS_EResult::EOS_Success)
		{
			lua_pushboolean(luaStatePointer, 1);
			return 1;
		}
		else
		{
			lua_pushboolean(luaStatePointer, 0);
			return 1;
		}
	}
	else
	{
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}
}

/** eos.addEventListener(eventName, listener) */
int OnAddEventListener(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the global Steam event name to listen to.
	const char* eventName = nullptr;
	if (lua_type(luaStatePointer, 1) == LUA_TSTRING)
	{
		eventName = lua_tostring(luaStatePointer, 1);
	}
	if (!eventName || ('\0' == eventName[0]))
	{
		CoronaLuaError(luaStatePointer, "1st argument must be set to an event name.");
		return 0;
	}

	// Determine if the 2nd argument references a Lua listener function/table.
	if (!CoronaLuaIsListener(luaStatePointer, 2, eventName))
	{
		CoronaLuaError(luaStatePointer, "2nd argument must be set to a listener.");
		return 0;
	}

	// Fetch the runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	// Add the given listener for the global Steam event.
	auto luaEventDispatcherPointer = contextPointer->GetLuaEventDispatcher();
	if (luaEventDispatcherPointer)
	{
		luaEventDispatcherPointer->AddEventListener(luaStatePointer, eventName, 2);
	}
	return 0;
}

/** eos.removeEventListener(eventName, listener) */
int OnRemoveEventListener(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the global EOS event name to stop listening to.
	const char* eventName = nullptr;
	if (lua_type(luaStatePointer, 1) == LUA_TSTRING)
	{
		eventName = lua_tostring(luaStatePointer, 1);
	}
	if (!eventName || ('\0' == eventName[0]))
	{
		CoronaLuaError(luaStatePointer, "1st argument must be set to an event name.");
		return 0;
	}

	// Determine if the 2nd argument references a Lua listener function/table.
	if (!CoronaLuaIsListener(luaStatePointer, 2, eventName))
	{
		CoronaLuaError(luaStatePointer, "2nd argument must be set to a listener.");
		return 0;
	}

	// Fetch the runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	// Remove the given listener from the global EOS event.
	auto luaEventDispatcherPointer = contextPointer->GetLuaEventDispatcher();
	if (luaEventDispatcherPointer)
	{
		luaEventDispatcherPointer->RemoveEventListener(luaStatePointer, eventName, 2);
	}
	return 0;
}

/** Called when a property field is being read from the plugin's Lua table. */
int OnAccessingField(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the field name being accessed.
	if (lua_type(luaStatePointer, 2) != LUA_TSTRING)
	{
		return 0;
	}
	auto fieldName = lua_tostring(luaStatePointer, 2);
	if (!fieldName)
	{
		return 0;
	}

	// Attempt to fetch the requested field value.
	int resultCount = 0;
	if (!strcmp(fieldName, "isLoggedOn"))
	{
		// Fetch the runtime context associated with the calling Lua state.
		auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
		if (!contextPointer)
		{
			return 0;
		}

		auto eosPlatformHandle = contextPointer->GetPlatformHandle();
		if (!eosPlatformHandle)
		{
			return 0;
		}

		lua_pushboolean(luaStatePointer, 1);
		resultCount = 1;
	}
	else
	{
		// Unknown field.
		CoronaLuaError(luaStatePointer, "Accessing unknown field: '%s'", fieldName);
	}

	// Return the number of value pushed to Lua as return values.
	return resultCount;
}

/** Called when a property field is being written to in the plugin's Lua table. */
int OnAssigningField(lua_State* luaStatePointer)
{
	// Writing to fields is not currently supported.
	return 0;
}

/**
  Called when the Lua plugin table is being destroyed.
  Expected to happen when the Lua runtime is being terminated.

  Performs finaly cleanup and terminates connection with the EOS client.
 */
int OnFinalizing(lua_State* luaStatePointer)
{
	// Delete this plugin's runtime context from memory.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (contextPointer)
	{
		delete contextPointer;
	}

	return 0;
}


//---------------------------------------------------------------------------------
// Public Exports
//---------------------------------------------------------------------------------

/**
  Called when this plugin is being loaded from Lua via a require() function.
  Initializes itself with EOS and returns the plugin's Lua table.
 */
CORONA_EXPORT int luaopen_plugin_eos(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// If this plugin instance is being loaded while another one already exists, then make sure that they're
	// both running on the same thread to avoid race conditions since EOS's event handlers are global.
	// Note: This can only happen if multiple Corona runtimes are running at the same time.
	if (RuntimeContext::GetInstanceCount() > 0)
	{
		if (std::this_thread::get_id() != sMainThreadId)
		{
			luaL_error(luaStatePointer, "Cannot load another instance of 'plugin.eos' from another thread.");
			return 0;
		}
	}
	else
	{
		sMainThreadId = std::this_thread::get_id();
	}

	// Create a new runtime context used to receive EOS's event and dispatch them to Lua.
	// Also used to ensure that the EOS overlay is rendered when requested on Windows.
	auto contextPointer = new RuntimeContext(luaStatePointer);
	if (!contextPointer)
	{
		return 0;
	}

	// Push this plugin's Lua table and all of its functions to the top of the Lua stack.
	// Note: The RuntimeContext pointer is pushed as an upvalue to all of these functions via luaL_openlib().
	{
		const struct luaL_Reg luaFunctions[] =
		{
			{ "getAuthIdToken", OnGetAuthIdToken },
			{ "setNotificationPosition", OnSetNotificationPosition },
			{ "addEventListener", OnAddEventListener },
			{ "removeEventListener", OnRemoveEventListener },
			{ nullptr, nullptr }
		};
		lua_createtable(luaStatePointer, 0, 0);
		lua_pushlightuserdata(luaStatePointer, contextPointer);
		luaL_openlib(luaStatePointer, nullptr, luaFunctions, 1);
	}

	// Add a Lua finalizer to the plugin's Lua table and to the Lua registry.
	// Note: Lua 5.1 tables do not support the "__gc" metatable field, but Lua light-userdata types do.
	{
		// Create a Lua metatable used to receive the finalize event.
		const struct luaL_Reg luaFunctions[] =
		{
			{ "__gc", OnFinalizing },
			{ nullptr, nullptr }
		};
		luaL_newmetatable(luaStatePointer, "plugin.eos.__gc");
		lua_pushlightuserdata(luaStatePointer, contextPointer);
		luaL_openlib(luaStatePointer, nullptr, luaFunctions, 1);
		lua_pop(luaStatePointer, 1);

		// Add the finalizer metable to the Lua registry.
		CoronaLuaPushUserdata(luaStatePointer, nullptr, "plugin.eos.__gc");
		int luaReferenceKey = luaL_ref(luaStatePointer, LUA_REGISTRYINDEX);

		// Add the finalizer metatable to the plugin's Lua table as an undocumented "__gc" field.
		// Note that a developer can overwrite this field, which is why we add it to the registry above too.
		lua_rawgeti(luaStatePointer, LUA_REGISTRYINDEX, luaReferenceKey);
		lua_setfield(luaStatePointer, -2, "__gc");
	}

	// Wrap the plugin's Lua table in a metatable used to provide readable/writable property fields.
	{
		const struct luaL_Reg luaFunctions[] =
		{
			{ "__index", OnAccessingField },
			{ "__newindex", OnAssigningField },
			{ nullptr, nullptr }
		};
		luaL_newmetatable(luaStatePointer, "plugin.eos");
		lua_pushlightuserdata(luaStatePointer, contextPointer);
		luaL_openlib(luaStatePointer, nullptr, luaFunctions, 1);
		lua_setmetatable(luaStatePointer, -2);
	}

	// // Acquire and handle the EOS app ID.
	// {
	// 	// First, check if a EOS app ID has already been assigned to this application.
	// 	// This can happen when:
	// 	// - The Corona project has been run more than once in the same app, such as via the Corona Simulator.
	// 	// - This app was launch via the EOS client, which happens with deployed EOS apps.
	// 	std::string currentStringId;
	// 	if (currentStringId == "0")
	// 	{
	// 		// Ignore an app ID of zero, which is an invalid ID.
	// 		// This can happen when launching an app from the Steam client that was not installed by Steam.
	// 		// Steam also allows us to switch an app ID of zero to a working/real app ID, which we may do down below.
	// 		currentStringId.erase();
	// 	}

	// Fetch the EOS app ID configured in the "config.lua" file.
	// std::string configStringId;
	PluginConfigLuaSettings configLuaSettings;
	configLuaSettings.LoadFrom(luaStatePointer);
	// 	if (configLuaSettings.GetStringAppId())
	// 	{
	// 		configStringId = configLuaSettings.GetStringAppId();
	// 	}
	// }

	// Initialize our connection with EOS if this is the first plugin instance.
	// Note: This avoid initializing twice in case multiple plugin instances exist at the same time.
	if (RuntimeContext::GetInstanceCount() == 1)
	{
		// Init EOS SDK
		EOS_InitializeOptions SDKOptions = {};
		SDKOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
		SDKOptions.AllocateMemoryFunction = nullptr;
		SDKOptions.ReallocateMemoryFunction = nullptr;
		SDKOptions.ReleaseMemoryFunction = nullptr;
		SDKOptions.ProductName = "JOCHEM - TODO";
		SDKOptions.ProductVersion = "1.0"; // JOCHEM - TODO
		SDKOptions.Reserved = nullptr;
		SDKOptions.SystemInitializeOptions = nullptr;
		SDKOptions.OverrideThreadAffinity = nullptr;

		EOS_EResult InitResult = EOS_Initialize(&SDKOptions);
		if (InitResult != EOS_EResult::EOS_Success)
		{
			CoronaLog("WARNING: [EOS SDK] Init Failed!");
			return 0;
		}

		CoronaLog("[EOS SDK] Initialized. Setting Logging Callback ...");
		EOS_EResult SetLogCallbackResult = EOS_Logging_SetCallback(&onEOSLogMessageReceived);
		if (SetLogCallbackResult != EOS_EResult::EOS_Success)
		{
			CoronaLog("WARNING: [EOS SDK] Set Logging Callback Failed!");
		}
		else
		{
			CoronaLog("[EOS SDK] Logging Callback Set");
			// EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Verbose);
		}

		// Create platform instance
		EOS_Platform_Options PlatformOptions = {};
		PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
		PlatformOptions.bIsServer = false;
		PlatformOptions.EncryptionKey = "JOCHEM - TODO";
		PlatformOptions.OverrideCountryCode = nullptr;
		PlatformOptions.OverrideLocaleCode = nullptr;
		PlatformOptions.Flags = EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D9 | EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D10 | EOS_PF_WINDOWS_ENABLE_OVERLAY_OPENGL; // Enable overlay support for D3D9/10 and OpenGL. This sample uses D3D11 or SDL.
		// PlatformOptions.CacheDirectory = FUtils::GetTempDirectory();

		PlatformOptions.ProductId = "JOCHEM - TODO";
		PlatformOptions.SandboxId = "JOCHEM - TODO";
		PlatformOptions.DeploymentId = "JOCHEM - TODO";
		PlatformOptions.ClientCredentials.ClientId = configLuaSettings.GetStringClientId();
		PlatformOptions.ClientCredentials.ClientSecret = configLuaSettings.GetStringClientSecret();

		EOS_Platform_RTCOptions RtcOptions = { 0 };
		RtcOptions.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;

	#ifdef _WIN32
		// Get absolute path for xaudio2_9redist.dll file
	#ifdef DXTK
		wchar_t CurDir[MAX_PATH + 1] = {};
		::GetCurrentDirectoryW(MAX_PATH + 1u, CurDir);
		std::wstring BasePath = std::wstring(CurDir);
		std::string XAudio29DllPath = FStringUtils::Narrow(BasePath);
	#endif
	#ifdef EOS_DEMO_SDL
		std::string XAudio29DllPath = SDL_GetBasePath();
	#endif // EOS_DEMO_SDL
		XAudio29DllPath.append("/xaudio2_9redist.dll");

		EOS_Windows_RTCOptions WindowsRtcOptions = { 0 };
		WindowsRtcOptions.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
		WindowsRtcOptions.XAudio29DllPath = XAudio29DllPath.c_str();
		RtcOptions.PlatformSpecificOptions = &WindowsRtcOptions;
	#else
		RtcOptions.PlatformSpecificOptions = NULL;
	#endif // _WIN32

		PlatformOptions.RTCOptions = &RtcOptions;

	#if ALLOW_RESERVED_PLATFORM_OPTIONS
		SetReservedPlatformOptions(PlatformOptions);
	#else
		PlatformOptions.Reserved = NULL;
	#endif // ALLOW_RESERVED_PLATFORM_OPTIONS

		EOS_HPlatform platformHandle = EOS_Platform_Create(&PlatformOptions);
		if (!platformHandle) {
			CoronaLuaError(luaStatePointer, "Failed to initialize connection with Epic client.");
		}
		contextPointer->SetPlatformHandle(platformHandle);
	}

	// We're returning 1 Lua plugin table.
	return 1;
}
