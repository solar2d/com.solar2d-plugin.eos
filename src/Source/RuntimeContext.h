// ----------------------------------------------------------------------------
// 
// RuntimeContext.h
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// ----------------------------------------------------------------------------

#pragma once

#include "BaseEosCallResultHandler.h"
#include "DispatchEventTask.h"
#include "LuaEventDispatcher.h"
#include "LuaMethodCallback.h"
#include "EosCallResultHandler.h"
#include <functional>
#include <memory>
#include <queue>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>
#include "eos_sdk.h"

// Forward declarations.
extern "C"
{
	struct lua_State;
}


/**
  Manages the plugin's event handling and current state between 1 Corona runtime and Steam.

  Automatically polls for and dispatches global Steam events, such as "LoginResponse_t", to Lua.
  Provides easy handling of Steam's CCallResult async operation via this class' AddEventHandlerFor() method.
  Also ensures that Steam events are only dispatched to Lua while the Corona runtime is running (ie: not suspended).
 */
class RuntimeContext
{
	public:
		/**
		  Instance of this type are passed as arguments to the
		  RuntimeContext::EventHandlerSettings::QueuingEventTaskCallback callback.
		 */
		struct QueuingEventTaskCallbackArguments
		{
			/**
			  Pointer to the task object that is about to be queued for execution.
			  Stores a copy of the information received from the Steam CCallResult operation.
			  This object is mutable and can be modified by the callback if needed.
			 */
			BaseDispatchCallResultEventTask* TaskPointer;

			/**
			  Callback can set this true to prevent this task from being queued, which in turn
			  prevents a Lua event from being dispatched.
			 */
			bool IsCanceled;
		};


		/**
		  Struct to be passed to a RuntimeContext's AddEventHandlerFor() method.
		  Sets up a Steam CCallResult async listener and then passes the received steam data to Lua as an
		  event to the given Lua function.
		 */
		struct EventHandlerSettings
		{
			/** Lua state that the "LuaFunctionStackIndex" field indexes. */
			lua_State* LuaStatePointer;

			/** Index to the Lua function that will receive a Lua event providing Steam's CCallResult data. */
			int LuaFunctionStackIndex;

			/**
			  Handle returned by Steam's C/C++ API used to receive the data of an async operation
			  via Steam's CCallResult class.
			 */
			// SteamAPICall_t SteamCallResultHandle;

			/**
			  Optional callback which will be invoked after a new BaseDispatchCallResultEventTask derived class
			  has been configured with the CCallResult's received information, but before the task has been
			  queued for dispatching its event data to Lua.

			  This is the caller's opportunity to modify the task's information or prevent it from being
			  queued by setting the argument's "IsCanceled" field to true.
			 */
			std::function<void(RuntimeContext::QueuingEventTaskCallbackArguments&)> QueuingEventTaskCallback;
		};


		/**
		  Creates a new Corona runtime context bound to the given Lua state.
		  Sets up a private Lua event dispatcher and listens for Lua runtime events such as "enterFrame".
		  @param luaStatePointer Pointer to a Lua state to bind this context to.

		                         Cannot be null or else an exception will be thrown.
		 */
		RuntimeContext(lua_State* luaStatePointer);

		/** Removes Lua listeners and frees allocated memory. */
		virtual ~RuntimeContext();


		/**
		  Gets a pointer to the Lua state that this Corona runtime context belongs to.
		  This will never return a Lua state belonging to a coroutine.
		  @return Returns a pointer to the Lua state that this Corona runtime context belongs to.
		 */
		lua_State* GetMainLuaState() const;

		/**
		  Gets the main event dispatcher that the plugin's Lua addEventListener() and removeEventListener() functions
		  are expected to be bound to. This runtime context will automatically dispatch global Steam events to
		  Lua listeners added to this dispatcher.
		  @return Returns a pointer to plugin's main event dispatcher for global Steam events.
		 */
		std::shared_ptr<LuaEventDispatcher> GetLuaEventDispatcher() const;


		/** Handle for Auth interface */
		EOS_HAuth fAuthHandle;

		/** Handle for Platform interface*/
		EOS_PlatformHandle* fPlatformHandle;

		/** Handle for logged in account*/
		EOS_EpicAccountId fAccountId;




		template<class TSteamResultType, class TDispatchEventTask>
		/**
		  Sets up a Steam CCallResult handler used to receive the result from a Steam async operation and
		  dispatch its data as a Lua event to the given Lua function.

		  This is a templatized method.
		  * The 1st template type must be set to the Steam result struct type, such as "NumberOfCurrentPlayers_t".
		  * The 2nd template type must be set to a "BaseDispatchEventTask" derived class,
		    such as the "DispatchNumberOfCurrentPlayersEventTask" class.
		  @param settings Provides a handle returned by a Steam async C/C++ function call used to listen for the result
		                  and an index to a Lua function to receive the result as a Lua event table.
		  @return Returns true if a CCallResult handler was successfully set up.

		          Returns false if the given "settings" argument contains invalid values. Both the Steam API handle
		          and Lua listener index must be assigned or else this method will fail.
		 */
		bool AddEventHandlerFor(const RuntimeContext::EventHandlerSettings& settings);

		/**
		  Fetches an active RuntimeContext instance that belongs to the given Lua state.
		  @param luaStatePointer Lua state that was passed to a RuntimeContext instance's constructor.
		  @return Returns a pointer to a RuntimeContext that belongs to the given Lua state.

		          Returns null if there is no RuntimeContext belonging to the given Lua state, or if there
		          was one, then it was already delete.
		 */
		static RuntimeContext* GetInstanceBy(lua_State* luaStatePointer);

		/**
		  Fetches the number of RuntimeContext instances currently active in the application.
		  @return Returns the number of instances current active. Returns zero if all instances have been destroyed.
		 */
		static int GetInstanceCount();

		/** Set up global Steam event handlers via their macros. */
		void OnLoginResponse(const EOS_Auth_LoginCallbackInfo* Data);

	private:
		/** Copy constructor deleted to prevent it from being called. */
		RuntimeContext(const RuntimeContext&) = delete;

		/** Method deleted to prevent the copy operator from being used. */
		void operator=(const RuntimeContext&) = delete;

		/**
		  Called when a Lua "enterFrame" event has been dispatched.
		  @param luaStatePointer Pointer to the Lua state that dispatched the event.
		  @return Returns the number of return values pushed to Lua. Returns 0 if no return values were pushed.
		 */
		int OnCoronaEnterFrame(lua_State* luatStatePointer);

		template<class TSteamResultType, class TDispatchEventTask>
		/**
		  To be called by this class' global EOS event handler methods.
		  Pushes the given EOS event data to the queue to be dispatched to Lua later once this runtime
		  context verifies that Corona is currently running (ie: not suspended).

		  This is a templatized method.
		  The 1st template type must be set to the Steam event struct type, such as "LoginResponse_t".
		  The 2nd template type must be set to a "BaseDispatchEventTask" derived class,
		  such as the "DispatchLoginResponseEventTask" class.
		  @param eventDataPointer Pointer to the Steam event data received. Can be null.
		 */
		void OnHandleGlobalEosEvent(TSteamResultType* eventDataPointer);

		template<class TSteamResultType, class TDispatchEventTask>
		/**
		  To be called by this class' global steam event handler methods whose Steam event structure contains
		  an "m_nGameID" field. Will ignore the given Steam event if it
		  belongs to another game/app by comparing its game ID with this application's Steam app ID.

		  If the given event belongs to this app, then this method pushes the event data to the queue to be
		  dispatched to Lua later once the runtime context verifies thta Corona is running (ie: not suspended).

		  This is a templatized method.
		  The 1st template type must be set to the Steam event struct type, such as "LoginResponse_t".
		  The 2nd template type must be set to a "BaseDispatchEventTask" derived class,
		  such as the "DispatchLoginResponseEventTask" class.
		  @param eventDataPointer Pointer to the Steam event data received. Can be null.
		 */
		void OnHandleGlobalEosEventWithGameId(TSteamResultType* eventDataPointer);

		/**
		  The main event dispatcher that the plugin's Lua addEventListener() and removeEventListener() functions
		  are bound to. Used to dispatch global steam events such as "LoginResponse_t".
		 */
		std::shared_ptr<LuaEventDispatcher> fLuaEventDispatcherPointer;

		/** Lua "enterFrame" listener. */
		LuaMethodCallback<RuntimeContext> fLuaEnterFrameCallback;

		/**
		  Queue of task objects used to dispatch various Steam related events to Lua.
		  Native Steam event callbacks are expected to push their event data to this queue to be dispatched
		  by this context later and only while the Corona runtime is running (ie: not suspended).
		 */
		std::queue<std::shared_ptr<BaseDispatchEventTask>> fDispatchEventTaskQueue;

		/**
		  Pool of re-usable Steam CCallResult handlers used to receive data from Steam's async API and
		  queue the results to the "fDispatchEventTaskQueue" to be dispatched as a Lua event later.
		 */
		std::vector<BaseEosCallResultHandler*> fEosCallResultHandlerPool;

		/** set of auth ID tokens to be destroyed **/
//		std::set<EOS_Auth_IdToken> fAuthIdTokens;

		/** Set true if we need to force Corona to render on the next "enterFrame" event. */
		bool fWasRenderRequested;
};


// ------------------------------------------------------------------------------------------
// Templatized class method defined below to prevent it from being inlined into calling code.
// ------------------------------------------------------------------------------------------

template<class TSteamResultType, class TDispatchEventTask>
bool RuntimeContext::AddEventHandlerFor(
	const RuntimeContext::EventHandlerSettings& settings)
{
	// Triggers a compiler error if "TDispatchEventTask" does not derive from "BaseDispatchCallResultEventTask".
	// static_assert(
	// 		std::is_base_of<BaseDispatchCallResultEventTask, TDispatchEventTask>::value,
	// 		"AddEventHandlerFor<TSteamResultType, TDispatchEventTask>() method's 'TDispatchEventTask' type "
	// 		"must be set to a class type derived from the 'BaseDispatchCallResultEventTask' class.");

	// // Validate arguments.
	// if (!settings.LuaStatePointer || !settings.LuaFunctionStackIndex)
	// {
	// 	return false;
	// }
	// // if (k_uAPICallInvalid == settings.SteamCallResultHandle)
	// // {
	// // 	return false;
	// // }
	
	// // Attempt to fetch an unused Steam CCallResult handler from the pool.
	// BaseEosCallResultHandler* handlerPointer = nullptr;
	// const std::type_info& handlerType = typeid(EosCallResultHandler<TSteamResultType>);
	// for (auto nextHandlerPointer : fEosCallResultHandlerPool)
	// {
	// 	if (nextHandlerPointer && (typeid(*nextHandlerPointer) == handlerType))
	// 	{
	// 		if (nextHandlerPointer->IsNotWaitingForResult())
	// 		{
	// 			handlerPointer = nextHandlerPointer;
	// 			break;
	// 		}
	// 	}
	// }

	// // If an unused Steam CCallResult handler was not found in the pool above, then create a new one.
	// if (!handlerPointer)
	// {
	// 	handlerPointer = new EosCallResultHandler<TSteamResultType>();
	// 	if (!handlerPointer)
	// 	{
	// 		return false;
	// 	}
	// 	fEosCallResultHandlerPool.push_back(handlerPointer);
	// }

	// // Set up a temporary Lua event dispatcher used to call the given Lua function when the operation completes.
	// auto luaEventDispatcherPointer = std::make_shared<LuaEventDispatcher>(settings.LuaStatePointer);
	// auto eventName = TDispatchEventTask::kLuaEventName;
	// luaEventDispatcherPointer->AddEventListener(
	// 		settings.LuaStatePointer, eventName, settings.LuaFunctionStackIndex);
	// auto queuingEventTaskCallback = settings.QueuingEventTaskCallback;
	// auto callback =
	// 		[this, luaEventDispatcherPointer, queuingEventTaskCallback]
	// 		(TSteamResultType* resultPointer, bool hadIOFailure)->void
	// {
	// 	// Validate.
	// 	if (!resultPointer)
	// 	{
	// 		return;
	// 	}

	// 	// Create and configure the event dispatcher task.
	// 	auto taskPointer = new TDispatchEventTask();
	// 	if (!taskPointer)
	// 	{
	// 		return;
	// 	}
	// 	auto sharedTaskPointer = std::shared_ptr<BaseDispatchEventTask>(taskPointer);
	// 	taskPointer->SetLuaEventDispatcher(luaEventDispatcherPointer);
	// 	taskPointer->SetHadIOFailure(hadIOFailure);
	// 	taskPointer->AcquireEventDataFrom(*resultPointer);

	// 	// Notify the caller of AddEventHandlerFor() about this new task, if a callback was configured.
	// 	if (queuingEventTaskCallback)
	// 	{
	// 		// Invoke the given callback.
	// 		QueuingEventTaskCallbackArguments callbackArguments;
	// 		callbackArguments.TaskPointer = taskPointer;
	// 		callbackArguments.IsCanceled = false;
	// 		queuingEventTaskCallback(callbackArguments);

	// 		// Do not queue the event dispatcher task if canceled by the callback.
	// 		// Note: The shared pointer which wraps the task pointer will auto delete it for us.
	// 		if (callbackArguments.IsCanceled)
	// 		{
	// 			return;
	// 		}
	// 	}

	// 	// Queue the received Steam event data to be dispatched to Lua later.
	// 	// This ensures that Lua events are only dispatched while Corona is running (ie: not suspended).
	// 	fDispatchEventTaskQueue.push(sharedTaskPointer);
	// };

	// // Set up the Steam CCallResult handler to start listening for the async Steam result.
	// // Will invoke the above callback when the async operation ends, pushing the result to the event queue.
	// auto concreteHandlerPointer = (EosCallResultHandler<TSteamResultType>*)handlerPointer;
	// // concreteHandlerPointer->Handle(settings.SteamCallResultHandle, callback);
	// return true;
}
