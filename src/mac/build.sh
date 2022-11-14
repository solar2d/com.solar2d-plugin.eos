#!/bin/bash

set -e

path=`dirname $0`

(
	cd "$path"
	rm -rf build out
	xcodebuild -project "Plugin.xcodeproj" -configuration Release clean

	xcodebuild -project "Plugin.xcodeproj" -configuration Release

	OUTPUT="out"
	mkdir -p "$OUTPUT"
	cp ~/Library/Application\ Support/Corona/Simulator/Plugins/plugin_eos.dylib "$OUTPUT"
	# lipo ~/Library/Application\ Support/Corona/Simulator/Plugins/libeos_api.dylib -thin x86_64  -output "$OUTPUT/libeos_api.dylib"
	cp "$path"/../Dependencies/Epic/redistributable_bin/libEOSSDK-Mac-Shipping.dylib "$OUTPUT"
)