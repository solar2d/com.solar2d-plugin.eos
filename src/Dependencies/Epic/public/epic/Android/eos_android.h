// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "eos_types.h"

#pragma pack(push, 8)

/** The most recent version of the EOS_Android_InitializeOptions structure. */
#define EOS_ANDROID_INITIALIZEOPTIONS_API_LATEST 2

/**
 * Options for initializing the Epic Online Services SDK on Android.
 *
 * Pass this structure to EOS_InitializeOptions::SystemInitializeOptions when initializing the SDK on Android.
 */
EOS_STRUCT(EOS_Android_InitializeOptions, (
	/** API Version: Set this to EOS_ANDROID_INITIALIZEOPTIONS_API_LATEST. */
	int32_t ApiVersion;
	/** Reserved for future use, must be set to NULL. */
	void* Reserved;
	/** Optional path to the application's internal storage directory. */
	const char* OptionalInternalDirectory;
	/** Optional path to the application's external storage directory. */
	const char* OptionalExternalDirectory;
));

#pragma pack(pop)
