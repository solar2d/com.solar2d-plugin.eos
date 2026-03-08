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

void DispatchLoginResponseEventTask::AcquireEventDataFrom(const EOS_Auth_LoginCallbackInfo* eosEventData)
{
	fResult = eosEventData->ResultCode;
	int sz = 0;
	if(fResult == EOS_EResult::EOS_Success && eosEventData->SelectedAccountId) {
		sz = EOS_EPICACCOUNTID_MAX_LENGTH + 1;
		EOS_EpicAccountId_ToString(eosEventData->SelectedAccountId, fSelectedAccountID, &sz);
	}
	fSelectedAccountID[sz] = 0;
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

	if(fResult == EOS_EResult::EOS_Success) {
		lua_pushstring(luaStatePointer, fSelectedAccountID);
		lua_setfield(luaStatePointer, -2, "selectedAccountId");
	}

	lua_pushboolean(luaStatePointer, fResult != EOS_EResult::EOS_Success ? 1 : 0);
	lua_setfield(luaStatePointer, -2, "isError");
	lua_pushinteger(luaStatePointer, (int)fResult);
	lua_setfield(luaStatePointer, -2, "resultCode");
	return true;
}


//---------------------------------------------------------------------------------
// DispatchLoadProductsEventTask Class Members
//---------------------------------------------------------------------------------

const char DispatchLoadProductsEventTask::kLuaEventName[] = "loadProducts";

DispatchLoadProductsEventTask::DispatchLoadProductsEventTask()
:	fIsError(false)
{
}

DispatchLoadProductsEventTask::~DispatchLoadProductsEventTask()
{
}

void DispatchLoadProductsEventTask::SetIsError(bool isError)
{
	fIsError = isError;
}

void DispatchLoadProductsEventTask::SetErrorString(const std::string& errorString)
{
	fErrorString = errorString;
}

void DispatchLoadProductsEventTask::AddProduct(const ProductInfo& product)
{
	fProducts.push_back(product);
}

const char* DispatchLoadProductsEventTask::GetLuaEventName() const
{
	return kLuaEventName;
}

bool DispatchLoadProductsEventTask::PushLuaEventTableTo(lua_State* luaStatePointer) const
{
	// Validate.
	if (!luaStatePointer)
	{
		return false;
	}

	// Create the event table with name="loadProducts"
	CoronaLuaNewEvent(luaStatePointer, kLuaEventName);

	// Set isError field
	lua_pushboolean(luaStatePointer, fIsError ? 1 : 0);
	lua_setfield(luaStatePointer, -2, "isError");

	// Set errorString field if present
	if (!fErrorString.empty())
	{
		lua_pushstring(luaStatePointer, fErrorString.c_str());
		lua_setfield(luaStatePointer, -2, "errorString");
	}

	// Create the products array table
	lua_createtable(luaStatePointer, (int)fProducts.size(), 0);
	for (int i = 0; i < (int)fProducts.size(); i++)
	{
		// Create a table for each product: { productIdentifier, localizedPrice, title, description }
		lua_createtable(luaStatePointer, 0, 4);

		lua_pushstring(luaStatePointer, fProducts[i].productIdentifier.c_str());
		lua_setfield(luaStatePointer, -2, "productIdentifier");

		lua_pushstring(luaStatePointer, fProducts[i].localizedPrice.c_str());
		lua_setfield(luaStatePointer, -2, "localizedPrice");

		lua_pushstring(luaStatePointer, fProducts[i].title.c_str());
		lua_setfield(luaStatePointer, -2, "title");

		lua_pushstring(luaStatePointer, fProducts[i].description.c_str());
		lua_setfield(luaStatePointer, -2, "description");

		// Set into products array at index i+1 (Lua arrays are 1-based)
		lua_rawseti(luaStatePointer, -2, i + 1);
	}
	lua_setfield(luaStatePointer, -2, "products");

	return true;
}


//---------------------------------------------------------------------------------
// DispatchStoreTransactionEventTask Class Members
//---------------------------------------------------------------------------------

const char DispatchStoreTransactionEventTask::kLuaEventName[] = "storeTransaction";

DispatchStoreTransactionEventTask::DispatchStoreTransactionEventTask()
{
}

DispatchStoreTransactionEventTask::~DispatchStoreTransactionEventTask()
{
}

void DispatchStoreTransactionEventTask::AddTransaction(const TransactionInfo& transaction)
{
	fTransactions.push_back(transaction);
}

const char* DispatchStoreTransactionEventTask::GetLuaEventName() const
{
	return kLuaEventName;
}

bool DispatchStoreTransactionEventTask::PushLuaEventTableTo(lua_State* luaStatePointer) const
{
	// Validate.
	if (!luaStatePointer)
	{
		return false;
	}

	// Create the event table with name="storeTransaction"
	CoronaLuaNewEvent(luaStatePointer, kLuaEventName);

	// Create the transactions array table
	lua_createtable(luaStatePointer, (int)fTransactions.size(), 0);
	for (int i = 0; i < (int)fTransactions.size(); i++)
	{
		// Create a table for each transaction: { productIdentifier, state, receipt, isError }
		lua_createtable(luaStatePointer, 0, 4);

		lua_pushstring(luaStatePointer, fTransactions[i].productIdentifier.c_str());
		lua_setfield(luaStatePointer, -2, "productIdentifier");

		lua_pushstring(luaStatePointer, fTransactions[i].state.c_str());
		lua_setfield(luaStatePointer, -2, "state");

		if (!fTransactions[i].receipt.empty())
		{
			lua_pushstring(luaStatePointer, fTransactions[i].receipt.c_str());
			lua_setfield(luaStatePointer, -2, "receipt");
		}

		lua_pushboolean(luaStatePointer, 0);
		lua_setfield(luaStatePointer, -2, "isError");

		// Set into transactions array at index i+1 (Lua arrays are 1-based)
		lua_rawseti(luaStatePointer, -2, i + 1);
	}
	lua_setfield(luaStatePointer, -2, "transactions");

	return true;
}
