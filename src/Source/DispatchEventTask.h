// ----------------------------------------------------------------------------
// 
// DispatchEventTask.h
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// ----------------------------------------------------------------------------

#pragma once

#include "LuaEventDispatcher.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "eos_sdk.h"


// Forward declarations.
extern "C"
{
	struct lua_State;
}


/**
  Abstract class used to dispatch an event table to Lua.

  The intended usage is that a derived class' constructor would copy a Steam event's data structure and then
  the event task instance would be queued to a RuntimeContext. A RuntimeContext would then dispatch all queued
  event tasks to Lua via their Execute() methods only if the Corona runtime is currently running (ie: not suspended).
 */
class BaseDispatchEventTask
{
	public:
		BaseDispatchEventTask();
		virtual ~BaseDispatchEventTask();

		std::shared_ptr<LuaEventDispatcher> GetLuaEventDispatcher() const;
		void SetLuaEventDispatcher(const std::shared_ptr<LuaEventDispatcher>& dispatcherPointer);
		virtual const char* GetLuaEventName() const = 0;
		virtual bool PushLuaEventTableTo(lua_State* luaStatePointer) const = 0;
		bool Execute();

	private:
		std::shared_ptr<LuaEventDispatcher> fLuaEventDispatcherPointer;
};


/**
  Abstract class used to dispatch all Steam CCallResult related events to Lua.

  Provides SetHadIOFailure() and HadIOFailure() methods used to determine if there was a Steam I/O failure.
  The RuntimeContext::AddEventHandlerFor() method will automatically set this I/O failure flag.
  It is up to the derived class to call HadIOFailure() within the PushLuaEventTableTo() method to use it, if relevant.
 */
class BaseDispatchCallResultEventTask : public BaseDispatchEventTask
{
	public:
		BaseDispatchCallResultEventTask();
		virtual ~BaseDispatchCallResultEventTask();

		bool HadIOFailure() const;
		void SetHadIOFailure(bool value);

	private:
		bool fHadIOFailure;
};

/** Dispatches a Steam "EOS_Auth_LoginCallbackInfo" event and its data to Lua. */
class DispatchLoginResponseEventTask : public BaseDispatchEventTask
{
public:
	static const char kLuaEventName[];

	DispatchLoginResponseEventTask();
	virtual ~DispatchLoginResponseEventTask();

	void AcquireEventDataFrom(const EOS_Auth_LoginCallbackInfo* Data);
	virtual const char* GetLuaEventName() const;
	virtual bool PushLuaEventTableTo(lua_State* luaStatePointer) const;

private:
	EOS_EResult fResult;
	char fSelectedAccountID[EOS_EPICACCOUNTID_MAX_LENGTH + 1];
};


/** Holds data for a single product/offer to dispatch to Lua. */
struct ProductInfo
{
	std::string productIdentifier;
	std::string localizedPrice;
	std::string title;
	std::string description;
};

/** Holds data for a single transaction (purchase/restore result) to dispatch to Lua. */
struct TransactionInfo
{
	std::string productIdentifier;
	std::string state;   // "purchased", "cancelled", "failed", "restored", "restoreCompleted"
	std::string receipt;  // Transaction ID from EOS checkout
};

/** Dispatches a "loadProducts" event and product data array to Lua. */
class DispatchLoadProductsEventTask : public BaseDispatchEventTask
{
public:
	static const char kLuaEventName[];

	DispatchLoadProductsEventTask();
	virtual ~DispatchLoadProductsEventTask();

	void SetIsError(bool isError);
	void SetErrorString(const std::string& errorString);
	void AddProduct(const ProductInfo& product);

	virtual const char* GetLuaEventName() const;
	virtual bool PushLuaEventTableTo(lua_State* luaStatePointer) const;

private:
	bool fIsError;
	std::string fErrorString;
	std::vector<ProductInfo> fProducts;
};


/** Dispatches a "storeTransaction" event and transaction data array to Lua. */
class DispatchStoreTransactionEventTask : public BaseDispatchEventTask
{
public:
	static const char kLuaEventName[];

	DispatchStoreTransactionEventTask();
	virtual ~DispatchStoreTransactionEventTask();

	void AddTransaction(const TransactionInfo& transaction);

	virtual const char* GetLuaEventName() const;
	virtual bool PushLuaEventTableTo(lua_State* luaStatePointer) const;

private:
	std::vector<TransactionInfo> fTransactions;
};
