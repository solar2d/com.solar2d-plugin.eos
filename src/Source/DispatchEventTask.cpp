// --------------------------------------------------------------------------------
// 
// DispatchEventTask.cpp
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// --------------------------------------------------------------------------------

#include "DispatchEventTask.h"
#include "CoronaLua.h"
#include <sstream>
#include <string>

//---------------------------------------------------------------------------------
// BaseDispatchEventTask Class Members
//---------------------------------------------------------------------------------

BaseDispatchEventTask::BaseDispatchEventTask()
{
}

BaseDispatchEventTask::~BaseDispatchEventTask()
{
}

std::shared_ptr<LuaEventDispatcher> BaseDispatchEventTask::GetLuaEventDispatcher() const
{
	return fLuaEventDispatcherPointer;
}

void BaseDispatchEventTask::SetLuaEventDispatcher(const std::shared_ptr<LuaEventDispatcher>& dispatcherPointer)
{
	fLuaEventDispatcherPointer = dispatcherPointer;
}

bool BaseDispatchEventTask::Execute()
{
	// Do not continue if not assigned a Lua event dispatcher.
	if (!fLuaEventDispatcherPointer)
	{
		return false;
	}

	// Fetch the Lua state the event dispatcher belongs to.
	auto luaStatePointer = fLuaEventDispatcherPointer->GetLuaState();
	if (!luaStatePointer)
	{
		return false;
	}

	// Push the derived class' event table to the top of the Lua stack.
	bool wasPushed = PushLuaEventTableTo(luaStatePointer);
	if (!wasPushed)
	{
		return false;
	}

	// Dispatch the event to all subscribed Lua listeners.
	bool wasDispatched = fLuaEventDispatcherPointer->DispatchEventWithoutResult(luaStatePointer, -1);

	// Pop the event table pushed above from the Lua stack.
	// Note: The DispatchEventWithoutResult() method above does not pop off this table.
	lua_pop(luaStatePointer, 1);

	// Return true if the event was successfully dispatched to Lua.
	return wasDispatched;
}


//---------------------------------------------------------------------------------
// BaseDispatchCallResultEventTask Class Members
//---------------------------------------------------------------------------------

BaseDispatchCallResultEventTask::BaseDispatchCallResultEventTask()
:	fHadIOFailure(false)
{
}

BaseDispatchCallResultEventTask::~BaseDispatchCallResultEventTask()
{
}

bool BaseDispatchCallResultEventTask::HadIOFailure() const
{
	return fHadIOFailure;
}

void BaseDispatchCallResultEventTask::SetHadIOFailure(bool value)
{
	fHadIOFailure = value;
}

//---------------------------------------------------------------------------------
// DispatchLoginResponseEventTask Class Members
//---------------------------------------------------------------------------------

const char DispatchLoginResponseEventTask::kLuaEventName[] = "loginResponse";

DispatchLoginResponseEventTask::DispatchLoginResponseEventTask()
: fResult(EOS_EResult::EOS_UnexpectedError)
{
}

DispatchLoginResponseEventTask::~DispatchLoginResponseEventTask()
{
}

void DispatchLoginResponseEventTask::AcquireEventDataFrom(const EOS_Auth_LoginCallbackInfo& eosEventData)
{
	fResult = eosEventData.ResultCode;
}

const char* DispatchLoginResponseEventTask::GetLuaEventName() const
{
	return kLuaEventName;
}

bool DispatchLoginResponseEventTask::PushLuaEventTableTo(lua_State* luaStatePointer) const
{
	// Validate.
	if (!luaStatePointer)
	{
		return false;
	}

	// Push the event data to Lua.
	CoronaLuaNewEvent(luaStatePointer, kLuaEventName);
	lua_pushboolean(luaStatePointer, fResult != EOS_EResult::EOS_Success);
	lua_setfield(luaStatePointer, -2, "isError");
	lua_pushinteger(luaStatePointer, 1); //fResult JOCHEM - TODO
	lua_setfield(luaStatePointer, -2, "resultCode");
	return true;
}