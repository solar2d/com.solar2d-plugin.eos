// ----------------------------------------------------------------------------
// 
// PluginConfigLuaSettings.h
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// ----------------------------------------------------------------------------

#pragma once

#include <string>
extern "C"
{
#	include "lua.h"
}


/**
  Provides an easy means of reading this plugin's settings from a "config.lua" file.

  Will ensure that the "config.lua" file is removed from the Lua package manager
  and the file's "application" Lua global is nil'ed out if not loaded before.
 */
class PluginConfigLuaSettings
{
	public:
		PluginConfigLuaSettings();
		virtual ~PluginConfigLuaSettings();

		const char* GetStringAppId() const;
		void SetStringAppId(const char* stringId);
		const char* GetStringClientId() const;
		void SetStringClientId(const char* stringId);
		const char* GetStringClientSecret() const;
		void SetStringClientSecret(const char* stringId);
		void Reset();
		bool LoadFrom(lua_State* luaStatePointer);

	private:
		std::string fStringAppId;
		std::string fStringClientId;
		std::string fStringClientSecret;
};
