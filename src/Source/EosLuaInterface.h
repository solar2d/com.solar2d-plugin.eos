// --------------------------------------------------------------------------------
//
// EosLuaInterface.h
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// --------------------------------------------------------------------------------

#pragma once

#include "eos_init.h"

extern "C"
{
	struct lua_State;
}

bool InitializeSDK(lua_State* luaStatePointer, EOS_InitializeOptions& options);
int OnGetAuthIdToken(lua_State* luaStatePointer);
int OnAddEventListener(lua_State* luaStatePointer);
int OnRemoveEventListener(lua_State* luaStatePointer);
int OnLoadProducts(lua_State* luaStatePointer);
int OnPurchaseProduct(lua_State* luaStatePointer);
int OnRestorePurchases(lua_State* luaStatePointer);
int OnFinishTransaction(lua_State* luaStatePointer);
bool OnIsLoggedOn(lua_State* luaStatePointer);
bool OnLoginWithAccountPortal(lua_State* luaStatePointer);
bool OnLogout(lua_State* luaStatePointer);
