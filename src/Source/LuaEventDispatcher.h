// ----------------------------------------------------------------------------
// 
// LuaEventDispatcher.h
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// ----------------------------------------------------------------------------

#pragma once


// Forward declarations.
extern "C"
{
	struct lua_State;
}


/**
  Creates a Lua "EventDispatcher" object and provides easy access to its addEventListener(), removeEventListener(),
  and dispatchEvent() Lua functions via this object's methods in C++.
 */
class LuaEventDispatcher
{
	private:
		/** Copy constructor made private to prevent it from being called. */
		LuaEventDispatcher(const LuaEventDispatcher&);

	public:
		/**
		  Creates a new Lua "EventDispatcher" object via a call to Corona's system.newEventDispatcher() Lua function.
		  @param luaStatePointer Pointer to the Lua state to create the EventDispatcher object on.
		 */
		LuaEventDispatcher(lua_State* luaStatePointer);

		/** Removes this instance's reference to the Lua "EventDispatcher" object. */
		virtual ~LuaEventDispatcher();


		/**
		  Gets a pointer to the Lua state that the Lua "EventDispatcher" object was created in.
		  @return Returns a pointer to the Lua state that the Lua "EventDispatcher" object was created in.
		  
		          Returns null if the constructor was given a null pointer.
		 */
		lua_State* GetLuaState() const;

		/**
		  Calls the Lua EventDispatcher object's addEventListener() Lua function with the given arguments.
		  @param luaStatePointer Pointer to the Lua state that the "luaListenerStackIndex" argument references.
		                         Must be the same Lua state that the EventDispatcher Lua object was created in
		                         or an associated coroutine's Lua state.
		  @param eventName Name of the event to add a listener for.
		  @param luaListenerStackIndex Index to the Lua function or table that to be registered as a listener.
		  @return Returns true if the listener was successfully added to the event dispatcher.

		          Returns false if given invalid arguments or if this object's constructor was unable to create
		          an event dispatcher in Lua.
		 */
		bool AddEventListener(lua_State* luaStatePointer, const char* eventName, int luaListenerStackIndex);

		/**
		  Calls the Lua EventDispatcher object's removeEventListener() Lua function with the given arguments.
		  @param luaStatePointer Pointer to the Lua state that the "luaListenerStackIndex" argument references.
		                         Must be the same Lua state that the EventDispatcher Lua object was created in
		                         or an associated coroutine's Lua state.
		  @param eventName Name of the event to remove a listener from.
		  @param luaListenerStackIndex Index to the Lua function or table that was registered as a listener.
		  @return Returns true if the listener was successfully removed from the event dispatcher.

		          Returns false if given invalid arguments or if this object's constructor was unable to create
		          an event dispatcher in Lua.
		 */
		bool RemoveEventListener(lua_State* luaStatePointer, const char* eventName, int luaListenerStackIndex);

		/**
		  Calls the Lua EventDispatcher object's dispatchEvent() Lua function.

		  Dispatches a simple event table containing only 1 field, the event name.

		  1 Lua return value will be pushed to the top of the Lua stack if successfully called.
		  It is the caller's responsibilty to pop the return value from the Lua stack.
		  @param luaStatePointer The Lua state to dispatch the event on.
		                         Must be the same Lua state that the EventDispatcher Lua object was created in
		                         or an associated coroutine's Lua state.
		  @param eventName The unique name of the event to be dispatched to Lua.
		  @return Returns true if the Lua dispatchEvent() function was successfully invoked and 1 Lua return value
		          was pushed to the top of the stack. It is the caller's responsibility to pop this return value
		          from the Lua stack.

		          Returns false if given invalid arguments or if this object's constructor was unable to create
		          an event dispatcher in Lua. A Lua return value will not be pushed onto the stack in this case.
		 */
		bool DispatchEventWithResult(lua_State* luaStatePointer, const char* eventName);

		/**
		  Calls the Lua EventDispatcher object's dispatchEvent() Lua function.

		  Dispatches the given Lua event table, which is not popped from the stack after calling this function.
		  This allows Lua listeners to provide feedback to the caller by assigning values to the event table's fields.
		
		  1 Lua return value will be pushed to the top of the Lua stack if successfully called.
		  It is the caller's responsibilty to pop the return value from the Lua stack.
		  @param luaStatePointer The Lua state to dispatch the event on.
		                         Must be the same Lua state that the EventDispatcher Lua object was created in
		                         or an associated coroutine's Lua state.
		  @param luaEventTableStackIndex Index to the Lua event table to be dispatched.
		  @return Returns true if the Lua dispatchEvent() function was successfully invoked and 1 Lua return value
		          was pushed to the top of the stack. It is the caller's responsibility to pop this return value
		          from the Lua stack.

		          Returns false if given invalid arguments or if this object's constructor was unable to create
		          an event dispatcher in Lua. A Lua return value will not be pushed onto the stack in this case.
		 */
		bool DispatchEventWithResult(lua_State* luaStatePointer, int luaEventTableStackIndex);

		/**
		  Calls the Lua EventDispatcher object's dispatchEvent() Lua function.

		  Dispatches a simple event table containing only 1 field, the event name.

		  A Lua listener's return value is ignored and is not pushed to the top of the Lua stack.
		  @param luaStatePointer The Lua state to dispatch the event on.
		                         Must be the same Lua state that the EventDispatcher Lua object was created in
		                         or an associated coroutine's Lua state.
		  @param eventName The unique name of the event to be dispatched to Lua.
		  @return Returns true if the Lua dispatchEvent() function was successfully invoked.

		          Returns false if given invalid arguments or if this object's constructor was unable to create
		          an event dispatcher in Lua.
		 */
		bool DispatchEventWithoutResult(lua_State* luaStatePointer, const char* eventName);

		/**
		  Calls the Lua EventDispatcher object's dispatchEvent() Lua function.

		  Dispatches the given Lua event table, which is not popped from the stack after calling this function.
		  This allows Lua listeners to provide feedback to the caller by assigning values to the event table's fields.
		
		  A Lua listener's return value is ignored and is not pushed to the top of the Lua stack.
		  @param luaStatePointer The Lua state to dispatch the event on.
		                         Must be the same Lua state that the EventDispatcher Lua object was created in
		                         or an associated coroutine's Lua state.
		  @param luaEventTableStackIndex Index to the Lua event table to be dispatched.
		  @return Returns true if the Lua dispatchEvent() function was successfully invoked.

		          Returns false if given invalid arguments or if this object's constructor was unable to create
		          an event dispatcher in Lua.
		 */
		bool DispatchEventWithoutResult(lua_State* luaStatePointer, int luaEventTableStackIndex);

	private:
		/** Copy operator made private to prevent it from being called. */
		void operator=(const LuaEventDispatcher&) {}


		/** The Lua state that the Lua EventDispatcher object was created */
		lua_State* fLuaStatePointer;

		/**
		  Unique ID to the Lua EventDispatcher object stored in the Lua registry.
		  Set to LUA_NOREF if no longer stored under the registry.
		 */
		int fLuaRegistryReferenceId;
};
