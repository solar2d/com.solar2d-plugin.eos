// // ----------------------------------------------------------------------------
// // 
// // EosCallResultHandler.h
// // Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// // This software may be modified and distributed under the terms
// // of the MIT license.  See the LICENSE file for details.
// //
// // ----------------------------------------------------------------------------

// #pragma once

// #include "BaseEosCallResultHandler.h"
// #include <functional>
// #include "eos_sdk.h"

// template<class TSteamResult>
// /**
//   Wraps a Steam CCallResult object, receiving an async Steam operation's result data and then
//   passing it to a given callback.
//  */
// class EosCallResultHandler : public BaseEosCallResultHandler
// {
// 	public:
// 		/** Creates a new Steam CCallResult handler. */
// 		EosCallResultHandler()
// 		{
// 			fCallback = nullptr;
// 		}

// 		/** Unregisters the Steam CCallResult event handler and disposes of this object. */
// 		virtual ~EosCallResultHandler()
// 		{
// 		}

// 		/**
// 		  Determines if the Handle() method was called and this handler is currently waiting for a result
// 		  to be received by an async Steam operation.
// 		  @return Returns true if this handler is still waiting for a result from Steam.

// 		          Returns false if Handle() has not been called yet or it has and the Steam result has already
// 				  been received by its assigned callback. This means that this object is now available to
// 				  handle another Steam async operation.
// 		 */
// 		virtual bool IsWaitingForResult() const override
// 		{
// 			return fCallResult.IsActive();
// 		}

// 		/** Aborts the last Handle() operation, unregistering its Steam result listener and assigned callback. */
// 		virtual void Abort() override
// 		{
// 			fCallback = nullptr;
// 			fCallResult.Cancel();
// 		}

// 		/**
// 		  Starts listening for result data for the given async Steam operation.
// 		  @param callResultHandle Handle returned by Steam's C/C++ async API.
// 		  @param callback The callback to be invoked by this handler when Steam's result data has been received.
// 		 */
// 		void Handle(SteamAPICall_t callResultHandle, const std::function<void(TSteamResult*, bool)>& callback)
// 		{
// 			fCallback = callback;
// 			fCallResult.Set(callResultHandle, this, &EosCallResultHandler::OnReceived);
// 		}

// 	private:
// 		/** Copy constructor deleted to prevent it from being called. */
// 		EosCallResultHandler(const EosCallResultHandler<TSteamResult>&) = delete;

// 		/** Copy operator deleted to prevent it from being called. */
// 		void operator=(const EosCallResultHandler<TSteamResult>&) = delete;

// 		/**
// 		  Called by Steam's CCallResult object when the result data has been received.
// 		  Can only be invoked after a call to the SteamAPI_RunCallbacks() function, which is the responsiblity
// 		  of the creator of this result handler object.
// 		  @param resultPointer Pointer to Steam's result data.
// 		  @param hadIOFailure Set true if there was an I/O failure during the operation. False if succeeded.
// 		 */
// 		void OnReceived(TSteamResult* resultPointer, bool hadIOFailure)
// 		{
// 			// Do not continue if this handler was not assigned a callback.
// 			if (!fCallback)
// 			{
// 				return;
// 			}

// 			// Copy the handler's assigned callback before invoking it below, keeping its captures alive on the stack.
// 			// This is in case the Handle() method gets called by the callback, replacing the current callback.
// 			auto callback = fCallback;
// 			fCallback = nullptr;

// 			// Pass the received result data to this handler's assigned callback.
// 			callback(resultPointer, hadIOFailure);
// 		}


// 		/** The Steam CCallResult object used to receive the async operation's result. */
// 		CCallResult<EosCallResultHandler<TSteamResult>, TSteamResult> fCallResult;

// 		/** Callback to be invoked by this handler, passing the received result data from the CCallResult object. */
// 		std::function<void(TSteamResult*, bool)> fCallback;
// };
