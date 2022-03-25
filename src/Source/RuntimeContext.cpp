// ----------------------------------------------------------------------------
// 
// RuntimeContext.cpp
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// ----------------------------------------------------------------------------

#include "RuntimeContext.h"
#include "CoronaLua.h"
#include "DispatchEventTask.h"
#include "EosCallResultHandler.h"
#include <exception>
#include <memory>
#include <unordered_set>
extern "C"
{
#	include "lua.h"
}


/** Stores a collection of all RuntimeContext instances that currently exist in the application. */
static std::unordered_set<RuntimeContext*> sRuntimeContextCollection;


RuntimeContext::RuntimeContext(lua_State* luaStatePointer)
:	fLuaEnterFrameCallback(this, &RuntimeContext::OnCoronaEnterFrame, luaStatePointer),
	fWasRenderRequested(false)
{
	// Validate.
	if (!luaStatePointer)
	{
		throw std::exception();
	}

	// If the given Lua state belongs to a coroutine, then use the main Lua state instead.
	{
		auto mainLuaStatePointer = CoronaLuaGetCoronaThread(luaStatePointer);
		if (mainLuaStatePointer && (mainLuaStatePointer != luaStatePointer))
		{
			luaStatePointer = mainLuaStatePointer;
		}
	}

	// Create a Lua EventDispatcher object.
	// Used to dispatch global events to listeners
	fLuaEventDispatcherPointer = std::make_shared<LuaEventDispatcher>(luaStatePointer);

	// Add Corona runtime event listeners.
	fLuaEnterFrameCallback.AddToRuntimeEventListeners("enterFrame");

	// Add this class instance to the global collection.
	sRuntimeContextCollection.insert(this);
}

RuntimeContext::~RuntimeContext()
{
	// Remove our Corona runtime event listeners.
	fLuaEnterFrameCallback.RemoveFromRuntimeEventListeners("enterFrame");

	// Delete our pool of Steam call result handlers.
	for (auto nextHandlerPointer : fEosCallResultHandlerPool)
	{
		if (nextHandlerPointer)
		{
			delete nextHandlerPointer;
		}
	}

	// for (auto authIdToken : fAuthIdTokens)
	// {
		// EOS_Auth_IdToken_Release(&authIdToken); // JOCHEM TODO
	// }

	// Remove this class instance from the global collection.
	sRuntimeContextCollection.erase(this);
}

lua_State* RuntimeContext::GetMainLuaState() const
{
	if (fLuaEventDispatcherPointer)
	{
		return fLuaEventDispatcherPointer->GetLuaState();
	}
	return nullptr;
}

void RuntimeContext::AddAuthIdToken(EOS_Auth_IdToken authIdToken)
{
	// fAuthIdTokens.insert(authIdToken); // JOCHEM TODO
}

std::shared_ptr<LuaEventDispatcher> RuntimeContext::GetLuaEventDispatcher() const
{
	return fLuaEventDispatcherPointer;
}

EOS_PlatformHandle* RuntimeContext::GetPlatformHandle() const
{
	return fPlatformHandle;
}

void RuntimeContext::SetPlatformHandle(EOS_PlatformHandle* platformHandle)
{
	 fPlatformHandle = platformHandle;
}

RuntimeContext* RuntimeContext::GetInstanceBy(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return nullptr;
	}

	// If the given Lua state belongs to a coroutine, then use the main Lua state instead.
	{
		auto mainLuaStatePointer = CoronaLuaGetCoronaThread(luaStatePointer);
		if (mainLuaStatePointer && (mainLuaStatePointer != luaStatePointer))
		{
			luaStatePointer = mainLuaStatePointer;
		}
	}

	// Return the first runtime context instance belonging to the given Lua state.
	for (auto&& runtimePointer : sRuntimeContextCollection)
	{
		if (runtimePointer && runtimePointer->GetMainLuaState() == luaStatePointer)
		{
			return runtimePointer;
		}
	}
	return nullptr;
}

int RuntimeContext::GetInstanceCount()
{
	return (int)sRuntimeContextCollection.size();
}

int RuntimeContext::OnCoronaEnterFrame(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	if (fPlatformHandle)
	{
		EOS_Platform_Tick(fPlatformHandle);
	}

	// Dispatch all queued events received from the above SteamAPI_RunCallbacks() call to Lua.
	while (fDispatchEventTaskQueue.size() > 0)
	{
		auto dispatchEventTaskPointer = fDispatchEventTaskQueue.front();
		fDispatchEventTaskQueue.pop();
		if (dispatchEventTaskPointer)
		{
			dispatchEventTaskPointer->Execute();
		}
	}

	// // If Steam's overlay needs to be rendered, then force Corona to render the next frame.
	// // We need to do this because Steam renders its overlay by hooking into the OpenGL/Direct3D rendering process.
	// auto steamUtilsPointer = SteamUtils();
	// bool isSteamShowingOverlay = (steamUtilsPointer && steamUtilsPointer->BOverlayNeedsPresent());
	// if (isSteamShowingOverlay || fWasRenderRequested)
	// {
	// 	// We can force Corona to render the next frame by toggling the Lua stage object's visibility state.
	// 	lua_getglobal(luaStatePointer, "display");
	// 	if (!lua_isnil(luaStatePointer, -1))
	// 	{
	// 		lua_getfield(luaStatePointer, -1, "currentStage");
	// 		{
	// 			lua_getfield(luaStatePointer, -1, "isVisible");
	// 			if (lua_type(luaStatePointer, -1) == LUA_TBOOLEAN)
	// 			{
	// 				bool isVisible = lua_toboolean(luaStatePointer, -1) ? true : false;
	// 				lua_pushboolean(luaStatePointer, !isVisible ? 1 : 0);
	// 				lua_setfield(luaStatePointer, -3, "isVisible");
	// 				lua_pushboolean(luaStatePointer, isVisible ? 1 : 0);
	// 				lua_setfield(luaStatePointer, -3, "isVisible");
	// 			}
	// 			lua_pop(luaStatePointer, 1);
	// 		}
	// 		lua_pop(luaStatePointer, 1);
	// 	}
	// 	lua_pop(luaStatePointer, 1);
	// }
	// {
	// 	// We must always force Corona to render 1 more time while the Steam overlay is shown.
	// 	// This is needed to erase the last rendered frame of a Steam fade-out animation.
	// 	fWasRenderRequested = isSteamShowingOverlay;
	// }

	return 0;
}

template<class TSteamResultType, class TDispatchEventTask>
void RuntimeContext::OnHandleGlobalEosEvent(TSteamResultType* eventDataPointer)
{
	// Triggers a compiler error if template type "TDispatchEventTask" does not derive from "BaseDispatchEventTask".
	static_assert(
			std::is_base_of<BaseDispatchEventTask, TDispatchEventTask>::value,
			"OnReceivedGlobalEosEvent<TSteamResultType, TDispatchEventTask>() method's 'TDispatchEventTask' type"
			" must be set to a class type derived from the 'BaseDispatchEventTask' class.");

	// Validate.
	if (!eventDataPointer)
	{
		return;
	}

	// Create and configure the event dispatcher task.
	auto taskPointer = new TDispatchEventTask();
	if (!taskPointer)
	{
		return;
	}
	taskPointer->SetLuaEventDispatcher(fLuaEventDispatcherPointer);
	taskPointer->AcquireEventDataFrom(*eventDataPointer);

	// Special handling of particular Epic events goes here if we had any.

	// Queue the received Steam event data to be dispatched to Lua later.
	// This ensures that Lua events are only dispatched while Corona is running (ie: not suspended).
	fDispatchEventTaskQueue.push(std::shared_ptr<BaseDispatchEventTask>(taskPointer));
}

template<class TSteamResultType, class TDispatchEventTask>
void RuntimeContext::OnHandleGlobalEosEventWithGameId(TSteamResultType* eventDataPointer)
{
	// Validate.
	if (!eventDataPointer)
	{
		return;
	}

	// Ignore the given event if it belongs to another application.
	// Note: The below if-check works for "m_nGameID" fields that are of type uint64 and CGameID.
	// auto steamUtilsPointer = SteamUtils();
	// if (steamUtilsPointer)
	// {
	// 	if (CGameID(steamUtilsPointer->GetAppID()) != CGameID(eventDataPointer->m_nGameID))
	// 	{
	// 		return;
	// 	}
	// }

	// Handle the given event.
	OnHandleGlobalEosEvent<TSteamResultType, TDispatchEventTask>(eventDataPointer);
}

// void RuntimeContext::OnLoginResponse(LoginResponse_t* eventDataPointer)
// {
// 	OnHandleGlobalEosEvent<LoginResponse_t, DispatchLoginResponseEventTask>(eventDataPointer);
// }
