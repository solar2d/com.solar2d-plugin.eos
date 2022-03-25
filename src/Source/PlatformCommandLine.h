// ----------------------------------------------------------------------------
// 
// PlatformCommandLine.h
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// ----------------------------------------------------------------------------

#pragma once
#include <string>
#include <vector>
#include <map>

class PlatformCommandLine
{
	public:
		static const std::vector<std::string>& GetCommandLine();
		static const std::map<std::string, std::string>& GommandLineMap();
};
