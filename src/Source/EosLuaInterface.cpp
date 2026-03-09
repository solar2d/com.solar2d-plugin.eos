// --------------------------------------------------------------------------------
// 
// EosLuaInterface.cpp
// Copyright (c) 2016 Corona Labs Inc. All rights reserved.
// This software may be modified and distributed under the terms
// of the MIT license.  See the LICENSE file for details.
//
// --------------------------------------------------------------------------------

#include "CoronaLua.h"
#include "CoronaMacros.h"
#include "DispatchEventTask.h"
#include "LuaEventDispatcher.h"
#include "PluginConfigLuaSettings.h"
#include "RuntimeContext.h"
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <stdint.h>

#include <string>
#include <thread>
extern "C"
{
#	include "lua.h"
#	include "lauxlib.h"
}
#include "eos_sdk.h"
#include "eos_ui.h"
#include "eos_logging.h"
#include "eos_auth.h"
#include "eos_ecom.h"
#include "PlatformCommandLine.h"

#if ALLOW_RESERVED_PLATFORM_OPTIONS
#include "ReservedPlatformOptions.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <windef.h>
#include <winbase.h>
#include "Windows/eos_Windows.h"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
// iOS: Provide the EOS_IOS_Auth_CredentialsOptions struct for AccountPortal login.
// We define a layout-compatible struct here to avoid importing UIKit in this .cpp file.
// The actual eos_ios.h requires <UIKit/UIKit.h> which needs Objective-C(++) compilation.
#include "WebAuthContextProvider.h"
#pragma pack(push, 8)
struct EOS_IOS_Auth_CredentialsOptions_Compat {
	int32_t ApiVersion;
	void* PresentationContextProviding;
	void* CreateBackgroundSnapshotView;
	void* CreateBackgroundSnapshotViewContext;
};
#pragma pack(pop)
#endif // TARGET_OS_IPHONE
#endif // __APPLE__

//---------------------------------------------------------------------------------
// Constants
//---------------------------------------------------------------------------------


//---------------------------------------------------------------------------------
// Private Static Variables
//---------------------------------------------------------------------------------

/**
  Gets the thread ID that all plugin instances are currently running in.
  This member is only applicable if at least 1 plugin instance exists.
  Intended to avoid multiple plugin instances from being loaded at the same time on different threads.
 */
static std::thread::id sMainThreadId;

//---------------------------------------------------------------------------------
// Private Static Functions
//---------------------------------------------------------------------------------
/**
  Determines if the given Lua state is running under the Corona Simulator.
  @param luaStatePointer Pointer to the Lua state to check.
  @return Returns true if the given Lua state is running under the Corona Simulator.

          Returns false if running under a real device/desktop application or if given a null pointer.
 */
bool IsRunningInCoronaSimulator(lua_State* luaStatePointer)
{
	bool isSimulator = false;
	lua_getglobal(luaStatePointer, "system");
	if (lua_istable(luaStatePointer, -1))
	{
		lua_getfield(luaStatePointer, -1, "getInfo");
		if (lua_isfunction(luaStatePointer, -1))
		{
			lua_pushstring(luaStatePointer, "environment");
			int callResultCode = CoronaLuaDoCall(luaStatePointer, 1, 1);
			if (!callResultCode && (lua_type(luaStatePointer, -1) == LUA_TSTRING))
			{
				isSimulator = (strcmp(lua_tostring(luaStatePointer, -1), "simulator") == 0);
			}
		}
		lua_pop(luaStatePointer, 1);
	}
	lua_pop(luaStatePointer, 1);
	return isSimulator;
}

/**
* Callback function to use for EOS SDK log messages
*
* @param InMsg - A structure representing data for a log message
*/
void EOS_CALL onEOSLogMessageReceived(const EOS_LogMessage* InMsg)
{
	if (InMsg->Level != EOS_ELogLevel::EOS_LOG_Off)
	{
		if (InMsg->Level == EOS_ELogLevel::EOS_LOG_Error || InMsg->Level == EOS_ELogLevel::EOS_LOG_Fatal)
		{
			CoronaLog("ERROR: [EOS SDK] %s", InMsg->Message);	
		}
		else if (InMsg->Level == EOS_ELogLevel::EOS_LOG_Warning)
		{
			CoronaLog("WARNING: [EOS SDK] %s", InMsg->Message);	
		}
		else
		{
			CoronaLog("[EOS SDK] %s", InMsg->Message);	
		}
	}
}

void EOS_CALL onLoginCallback(const EOS_Auth_LoginCallbackInfo* Data)
{
	RuntimeContext* contextPointer = (RuntimeContext*)Data->ClientData;
	if (Data->ResultCode == EOS_EResult::EOS_Success)
	{
		contextPointer->fAccountId = Data->SelectedAccountId;
	}
	
	if (EOS_EResult_IsOperationComplete(Data->ResultCode))
	{
		contextPointer->OnLoginResponse(Data);
	}
}

//---------------------------------------------------------------------------------
// Lua API Handlers
//---------------------------------------------------------------------------------
#ifndef ANDROID
/** UserInfo eos.getAuthIdToken() */
int OnGetAuthIdToken(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch this plugin's runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	EOS_Auth_CopyIdTokenOptions CopyTokenOptions = { 0 };
	CopyTokenOptions.ApiVersion = EOS_AUTH_COPYUSERAUTHTOKEN_API_LATEST;
	CopyTokenOptions.AccountId = contextPointer->fAccountId;

	EOS_Auth_IdToken* outIdToken;
	if (EOS_Auth_CopyIdToken(contextPointer->fAuthHandle, &CopyTokenOptions, &outIdToken) == EOS_EResult::EOS_Success)
	{
		lua_pushstring(luaStatePointer, outIdToken->JsonWebToken);
		EOS_Auth_IdToken_Release(outIdToken);
		return 1;
	}
	else
	{
		CoronaLog("WARNING: [EOS SDK] User Auth Token is invalid");
		return 0;
	}
}
#endif // !ANDROID

#ifndef ANDROID
/** bool eos.setNotificationPosition(positionName) */
int OnSetNotificationPosition(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the required position name argument.
	if (lua_type(luaStatePointer, 1) != LUA_TSTRING)
	{
		CoronaLuaError(luaStatePointer, "Given argument is not of type string.");
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}
	const char* positionName = lua_tostring(luaStatePointer, 1);
	if (!positionName)
	{
		positionName = "";
	}

	// Convert the position name to its equivalent Steam enum constant.
	EOS_UI_ENotificationLocation positionId;
	if (!strcmp(positionName, "topLeft"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_TopLeft;
	}
	else if (!strcmp(positionName, "topRight"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_TopRight;
	}
	else if (!strcmp(positionName, "bottomLeft"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_BottomLeft;
	}
	else if (!strcmp(positionName, "bottomRight"))
	{
		positionId = EOS_UI_ENotificationLocation::EOS_UNL_BottomRight;
	}
	else
	{
		CoronaLuaError(luaStatePointer, "Given unknown position name '%s'", positionName);
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}

	// Fetch the runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	auto eosPlatformHandle = contextPointer->fPlatformHandle;
	if (eosPlatformHandle)
	{
		// Change EOS's notification position with given setting.
		EOS_HUI ExternalUIHandle = EOS_Platform_GetUIInterface(eosPlatformHandle);

		EOS_UI_SetDisplayPreferenceOptions Options = {};
		Options.ApiVersion = EOS_UI_SETDISPLAYPREFERENCE_API_LATEST;
		Options.NotificationLocation = positionId;

		const EOS_EResult Result = EOS_UI_SetDisplayPreference(ExternalUIHandle, &Options);
		if (Result == EOS_EResult::EOS_Success)
		{
			lua_pushboolean(luaStatePointer, 1);
			return 1;
		}
		else
		{
			lua_pushboolean(luaStatePointer, 0);
			return 1;
		}
	}
	else
	{
		lua_pushboolean(luaStatePointer, 0);
		return 1;
	}
}

/** eos.addEventListener(eventName, listener) */
int OnAddEventListener(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the global Steam event name to listen to.
	const char* eventName = nullptr;
	if (lua_type(luaStatePointer, 1) == LUA_TSTRING)
	{
		eventName = lua_tostring(luaStatePointer, 1);
	}
	if (!eventName || ('\0' == eventName[0]))
	{
		CoronaLuaError(luaStatePointer, "1st argument must be set to an event name.");
		return 0;
	}

	// Determine if the 2nd argument references a Lua listener function/table.
	if (!CoronaLuaIsListener(luaStatePointer, 2, eventName))
	{
		CoronaLuaError(luaStatePointer, "2nd argument must be set to a listener.");
		return 0;
	}

	// Fetch the runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	// Add the given listener for the global Steam event.
	auto luaEventDispatcherPointer = contextPointer->GetLuaEventDispatcher();
	if (luaEventDispatcherPointer)
	{
		luaEventDispatcherPointer->AddEventListener(luaStatePointer, eventName, 2);
	}
	return 0;
}

/** eos.removeEventListener(eventName, listener) */
int OnRemoveEventListener(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the global EOS event name to stop listening to.
	const char* eventName = nullptr;
	if (lua_type(luaStatePointer, 1) == LUA_TSTRING)
	{
		eventName = lua_tostring(luaStatePointer, 1);
	}
	if (!eventName || ('\0' == eventName[0]))
	{
		CoronaLuaError(luaStatePointer, "1st argument must be set to an event name.");
		return 0;
	}

	// Determine if the 2nd argument references a Lua listener function/table.
	if (!CoronaLuaIsListener(luaStatePointer, 2, eventName))
	{
		CoronaLuaError(luaStatePointer, "2nd argument must be set to a listener.");
		return 0;
	}

	// Fetch the runtime context associated with the calling Lua state.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (!contextPointer)
	{
		return 0;
	}

	// Remove the given listener from the global EOS event.
	auto luaEventDispatcherPointer = contextPointer->GetLuaEventDispatcher();
	if (luaEventDispatcherPointer)
	{
		luaEventDispatcherPointer->RemoveEventListener(luaStatePointer, eventName, 2);
	}
	return 0;
}

/** Called when a property field is being read from the plugin's Lua table. */
int OnAccessingField(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the field name being accessed.
	if (lua_type(luaStatePointer, 2) != LUA_TSTRING)
	{
		return 0;
	}
	auto fieldName = lua_tostring(luaStatePointer, 2);
	if (!fieldName)
	{
		return 0;
	}

	// Attempt to fetch the requested field value.
	int resultCount = 0;
	if (!strcmp(fieldName, "canLoadProducts"))
	{
		// Epic store always supports loading products
		lua_pushboolean(luaStatePointer, 1);
		resultCount = 1;
	}
	else if (!strcmp(fieldName, "isActive"))
	{
		// Plugin is always active once loaded
		lua_pushboolean(luaStatePointer, 1);
		resultCount = 1;
	}
	else
	{
		// Unknown field - return nil silently (don't error, game may probe for optional fields)
		lua_pushnil(luaStatePointer);
		resultCount = 1;
	}

	// Return the number of value pushed to Lua as return values.
	return resultCount;
}

/** Called when a property field is being written to in the plugin's Lua table. */
int OnAssigningField(lua_State* luaStatePointer)
{
	// Writing to fields is not currently supported.
	return 0;
}

/**
  Called when the Lua plugin table is being destroyed.
  Expected to happen when the Lua runtime is being terminated.

  Performs finaly cleanup and terminates connection with the EOS client.
 */
int OnFinalizing(lua_State* luaStatePointer)
{
	// Delete this plugin's runtime context from memory.
	auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
	if (contextPointer)
	{
		delete contextPointer;
	}

	return 0;
}


//---------------------------------------------------------------------------------
// iOS/Desktop IAP Functions (using RuntimeContext)
//---------------------------------------------------------------------------------

// Map from product identifier (catalog item ID) → offer ID
// Populated during OnQueryOffersComplete, used by OnPurchaseProduct to look up
// the correct offer ID for EOS_Ecom_Checkout.
static std::map<std::string, std::string> sCatalogItemToOfferIdMap;

/** Helper: Format price from EOS Ecom offer data into a localized price string */
static std::string FormatOfferPrice(const EOS_Ecom_CatalogOffer* offer)
{
    if (!offer || offer->PriceResult != EOS_EResult::EOS_Success)
    {
        return "N/A";
    }

    uint64_t price = offer->CurrentPrice64;
    uint32_t decimalPoint = offer->DecimalPoint;
    std::string currencyCode = offer->CurrencyCode ? offer->CurrencyCode : "";

    std::string priceStr;
    if (decimalPoint > 0)
    {
        uint64_t divisor = 1;
        for (uint32_t d = 0; d < decimalPoint; d++) divisor *= 10;
        uint64_t whole = price / divisor;
        uint64_t frac = price % divisor;

        std::ostringstream oss;
        oss << whole << ".";
        std::string fracStr = std::to_string(frac);
        while (fracStr.length() < decimalPoint) fracStr = "0" + fracStr;
        oss << fracStr;
        priceStr = oss.str();
    }
    else
    {
        priceStr = std::to_string(price);
    }

    if (currencyCode == "USD") return "$" + priceStr;
    else if (currencyCode == "EUR") return "\xE2\x82\xAC" + priceStr;
    else if (currencyCode == "GBP") return "\xC2\xA3" + priceStr;
    else if (currencyCode == "JPY") return "\xC2\xA5" + priceStr;
    else if (currencyCode == "CAD") return "CA$" + priceStr;
    else if (currencyCode == "AUD") return "A$" + priceStr;
    else if (currencyCode == "BRL") return "R$" + priceStr;
    else if (currencyCode == "MXN") return "MX$" + priceStr;
    else if (!currencyCode.empty()) return priceStr + " " + currencyCode;
    else return priceStr;
}

/** Context passed through EOS_Ecom_QueryOffers callback to carry requested offer IDs */
struct QueryOffersContext {
    std::vector<std::string> requestedOfferIds;
};

/** EOS Ecom QueryOffers completion callback */
static void EOS_CALL OnQueryOffersComplete(const EOS_Ecom_QueryOffersCallbackInfo* Data)
{
    auto context = static_cast<QueryOffersContext*>(Data->ClientData);
    auto* rtContext = RuntimeContext::GetFirstInstance();
    if (!rtContext)
    {
        CoronaLog("ERROR: [EOS SDK] OnQueryOffersComplete: No RuntimeContext available");
        delete context;
        return;
    }

    auto dispatcher = rtContext->GetLuaEventDispatcher();
    auto task = std::make_shared<DispatchLoadProductsEventTask>();
    task->SetLuaEventDispatcher(dispatcher);

    if (Data->ResultCode != EOS_EResult::EOS_Success)
    {
        CoronaLog("[EOS_DEBUG] OnQueryOffersComplete: FAILED result=%s", EOS_EResult_ToString(Data->ResultCode));
        task->SetIsError(true);
        task->SetErrorString(EOS_EResult_ToString(Data->ResultCode));
    }
    else
    {
        EOS_HEcom EcomHandle = EOS_Platform_GetEcomInterface(rtContext->fPlatformHandle);

        EOS_Ecom_GetOfferCountOptions CountOptions = {};
        CountOptions.ApiVersion = EOS_ECOM_GETOFFERCOUNT_API_LATEST;
        CountOptions.LocalUserId = rtContext->fAccountId;
        uint32_t offerCount = EOS_Ecom_GetOfferCount(EcomHandle, &CountOptions);

        CoronaLog("[EOS_DEBUG] OnQueryOffersComplete: SUCCESS, %u offers in catalog, %d requested",
                  offerCount, (int)context->requestedOfferIds.size());

        std::set<std::string> requestedSet(context->requestedOfferIds.begin(),
                                            context->requestedOfferIds.end());
        std::set<std::string> matchedSet;

        int matchedCount = 0;
        for (uint32_t i = 0; i < offerCount; i++)
        {
            EOS_Ecom_CopyOfferByIndexOptions CopyOptions = {};
            CopyOptions.ApiVersion = EOS_ECOM_COPYOFFERBYINDEX_API_LATEST;
            CopyOptions.LocalUserId = rtContext->fAccountId;
            CopyOptions.OfferIndex = i;

            EOS_Ecom_CatalogOffer* offer = nullptr;
            EOS_EResult copyResult = EOS_Ecom_CopyOfferByIndex(EcomHandle, &CopyOptions, &offer);
            if (copyResult != EOS_EResult::EOS_Success || !offer)
            {
                CoronaLog("[EOS_DEBUG]   offer[%u]: CopyOfferByIndex failed: %s", i, EOS_EResult_ToString(copyResult));
                continue;
            }

            std::string offerId = offer->Id ? offer->Id : "";
            std::string offerTitle = offer->TitleText ? offer->TitleText : "(null)";

            CoronaLog("[EOS_DEBUG]   catalog offer[%u]: id='%s' title='%s' available=%d price=%s",
                      i, offerId.c_str(), offerTitle.c_str(),
                      (int)offer->bAvailableForPurchase,
                      FormatOfferPrice(offer).c_str());

            // Enumerate catalog items within this offer
            EOS_Ecom_GetOfferItemCountOptions ItemCountOptions = {};
            ItemCountOptions.ApiVersion = EOS_ECOM_GETOFFERITEMCOUNT_API_LATEST;
            ItemCountOptions.LocalUserId = rtContext->fAccountId;
            ItemCountOptions.OfferId = offer->Id;
            uint32_t itemCount = EOS_Ecom_GetOfferItemCount(EcomHandle, &ItemCountOptions);

            CoronaLog("[EOS_DEBUG]     offer '%s' contains %u catalog items:", offerId.c_str(), itemCount);

            for (uint32_t j = 0; j < itemCount; j++)
            {
                EOS_Ecom_CopyOfferItemByIndexOptions ItemCopyOptions = {};
                ItemCopyOptions.ApiVersion = EOS_ECOM_COPYOFFERITEMBYINDEX_API_LATEST;
                ItemCopyOptions.LocalUserId = rtContext->fAccountId;
                ItemCopyOptions.OfferId = offer->Id;
                ItemCopyOptions.ItemIndex = j;

                EOS_Ecom_CatalogItem* item = nullptr;
                EOS_EResult itemResult = EOS_Ecom_CopyOfferItemByIndex(EcomHandle, &ItemCopyOptions, &item);
                if (itemResult == EOS_EResult::EOS_Success && item)
                {
                    std::string itemId = item->Id ? item->Id : "";
                    CoronaLog("[EOS_DEBUG]       item[%u]: id='%s' title='%s'",
                              j, itemId.c_str(),
                              item->TitleText ? item->TitleText : "(null)");

                    if (!itemId.empty() && !offerId.empty())
                    {
                        sCatalogItemToOfferIdMap[itemId] = offerId;
                    }

                    if (!requestedSet.empty() && requestedSet.count(itemId) > 0 && matchedSet.count(itemId) == 0)
                    {
                        ProductInfo product;
                        product.productIdentifier = itemId;
                        product.title = item->TitleText ? item->TitleText : offerTitle;
                        product.description = item->DescriptionText ? item->DescriptionText : "";
                        product.localizedPrice = FormatOfferPrice(offer);

                        CoronaLog("[EOS_DEBUG]       MATCHED catalog item by ID: id='%s' title='%s' price='%s'",
                                  product.productIdentifier.c_str(),
                                  product.title.c_str(),
                                  product.localizedPrice.c_str());

                        task->AddProduct(product);
                        matchedSet.insert(itemId);
                        matchedCount++;
                    }

                    EOS_Ecom_CatalogItem_Release(item);
                }
            }

            // Also check if the offer ID itself was requested
            bool isOfferRequested = false;
            if (requestedSet.empty())
            {
                isOfferRequested = true;
            }
            else if (requestedSet.count(offerId) > 0 && matchedSet.count(offerId) == 0)
            {
                isOfferRequested = true;
            }

            if (isOfferRequested)
            {
                ProductInfo product;
                product.productIdentifier = offerId;
                product.title = offer->TitleText ? offer->TitleText : "";
                product.description = offer->DescriptionText ? offer->DescriptionText : "";
                product.localizedPrice = FormatOfferPrice(offer);

                CoronaLog("[EOS_DEBUG]   MATCHED offer by ID: id='%s' title='%s' price='%s'",
                          product.productIdentifier.c_str(),
                          product.title.c_str(),
                          product.localizedPrice.c_str());

                task->AddProduct(product);
                matchedSet.insert(offerId);
                matchedCount++;
            }

            EOS_Ecom_CatalogOffer_Release(offer);
        }

        CoronaLog("[EOS_DEBUG] OnQueryOffersComplete: returning %d matched products to Lua",
                  matchedCount);
    }

    rtContext->QueueEventTask(task);
    delete context;
}

/** Flag to prevent double-login attempts when OnInit is called multiple times */
static bool sLoginInProgress = false;

/** eos.init() - iOS implementation: triggers EOS Auth login */
int OnInit(lua_State* luaStatePointer)
{
    auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));

    if (!contextPointer || !contextPointer->fPlatformHandle)
    {
        return 0;
    }

    if (contextPointer->fAccountId)
    {
        return 0;
    }

    if (sLoginInProgress)
    {
        return 0;
    }

    sLoginInProgress = true;

    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(contextPointer->fPlatformHandle);
    if (!AuthHandle)
    {
        sLoginInProgress = false;
        return 0;
    }

    // Try PersistentAuth first
    EOS_Auth_LoginOptions LoginOptions = {};
    LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;

    EOS_Auth_Credentials Credentials = {};
    Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
    Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_PersistentAuth;
    Credentials.Id = nullptr;
    Credentials.Token = nullptr;
    LoginOptions.Credentials = &Credentials;

    EOS_Auth_Login(AuthHandle, &LoginOptions, contextPointer, [](const EOS_Auth_LoginCallbackInfo* Data) {
        auto* ctx = static_cast<RuntimeContext*>(Data->ClientData);
        if (!ctx) return;

        if (Data->ResultCode == EOS_EResult::EOS_Success)
        {
            ctx->fAccountId = Data->LocalUserId;
            ctx->fAuthHandle = EOS_Platform_GetAuthInterface(ctx->fPlatformHandle);
            sLoginInProgress = false;
            ctx->OnLoginResponse(Data);
        }
        else
        {
            // PersistentAuth failed (no cached credentials), fall back to AccountPortal

            EOS_HAuth AuthHandle2 = EOS_Platform_GetAuthInterface(ctx->fPlatformHandle);
            EOS_Auth_LoginOptions LoginOptions2 = {};
            LoginOptions2.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
            EOS_Auth_Credentials Credentials2 = {};
            Credentials2.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
            Credentials2.Type = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
            Credentials2.Id = nullptr;
            Credentials2.Token = nullptr;

#if defined(__APPLE__) && TARGET_OS_IPHONE
            // iOS requires EOS_IOS_Auth_CredentialsOptions with a presentation context
            // for the ASWebAuthenticationSession used by AccountPortal login.
            EOS_IOS_Auth_CredentialsOptions_Compat iosOpts2 = {};
            iosOpts2.ApiVersion = 2; // EOS_IOS_AUTH_CREDENTIALSOPTIONS_API_LATEST
            iosOpts2.PresentationContextProviding = CreateWebAuthContextProvider();
            iosOpts2.CreateBackgroundSnapshotView = nullptr;
            iosOpts2.CreateBackgroundSnapshotViewContext = nullptr;
            Credentials2.SystemAuthCredentialsOptions = &iosOpts2;
#endif

            LoginOptions2.Credentials = &Credentials2;

            EOS_Auth_Login(AuthHandle2, &LoginOptions2, ctx, [](const EOS_Auth_LoginCallbackInfo* Data2) {
                auto* ctx2 = static_cast<RuntimeContext*>(Data2->ClientData);
                if (!ctx2) return;

                if (Data2->ResultCode == EOS_EResult::EOS_Success)
                {
                    ctx2->fAccountId = Data2->LocalUserId;
                    ctx2->fAuthHandle = EOS_Platform_GetAuthInterface(ctx2->fPlatformHandle);
                }

                sLoginInProgress = false;
                ctx2->OnLoginResponse(Data2);
            });
        }
    });

    return 0;
}

/** eos.loadProducts(identifiers, listener) - iOS implementation using EOS Ecom API */
int OnLoadProducts(lua_State* luaStatePointer)
{
    auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
    if (!contextPointer || !contextPointer->fPlatformHandle)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoadProducts: PlatformHandle is null");
        if (contextPointer)
        {
            auto task = std::make_shared<DispatchLoadProductsEventTask>();
            task->SetLuaEventDispatcher(contextPointer->GetLuaEventDispatcher());
            task->SetIsError(true);
            task->SetErrorString("Platform not initialized");
            contextPointer->QueueEventTask(task);
        }
        return 0;
    }

    if (!contextPointer->fAccountId)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoadProducts: not logged in");
        auto task = std::make_shared<DispatchLoadProductsEventTask>();
        task->SetLuaEventDispatcher(contextPointer->GetLuaEventDispatcher());
        task->SetIsError(true);
        task->SetErrorString("Not logged in");
        contextPointer->QueueEventTask(task);
        return 0;
    }

    auto context = new QueryOffersContext();
    if (lua_istable(luaStatePointer, 1))
    {
        int tableLen = (int)lua_objlen(luaStatePointer, 1);
        CoronaLog("[EOS_DEBUG] OnLoadProducts: reading %d product identifiers from Lua", tableLen);
        for (int i = 1; i <= tableLen; i++)
        {
            lua_rawgeti(luaStatePointer, 1, i);
            if (lua_isstring(luaStatePointer, -1))
            {
                const char* id = lua_tostring(luaStatePointer, -1);
                if (id)
                {
                    context->requestedOfferIds.push_back(id);
                    CoronaLog("[EOS_DEBUG]   identifier[%d] = %s", i, id);
                }
            }
            lua_pop(luaStatePointer, 1);
        }
    }

    EOS_HEcom EcomHandle = EOS_Platform_GetEcomInterface(contextPointer->fPlatformHandle);
    if (!EcomHandle)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoadProducts: Could not get Ecom interface");
        auto task = std::make_shared<DispatchLoadProductsEventTask>();
        task->SetLuaEventDispatcher(contextPointer->GetLuaEventDispatcher());
        task->SetIsError(true);
        task->SetErrorString("Could not get Ecom interface");
        contextPointer->QueueEventTask(task);
        delete context;
        return 0;
    }

    EOS_Ecom_QueryOffersOptions QueryOptions = {};
    QueryOptions.ApiVersion = EOS_ECOM_QUERYOFFERS_API_LATEST;
    QueryOptions.LocalUserId = contextPointer->fAccountId;
    QueryOptions.OverrideCatalogNamespace = nullptr;

    CoronaLog("[EOS_DEBUG] OnLoadProducts: calling EOS_Ecom_QueryOffers");
    EOS_Ecom_QueryOffers(EcomHandle, &QueryOptions, context, OnQueryOffersComplete);

    return 0;
}

/** Context passed through EOS_Ecom_Checkout callback */
struct CheckoutContext {
    std::string productIdentifier;
    std::string offerId;
};

/** EOS Ecom Checkout completion callback */
static void EOS_CALL OnCheckoutComplete(const EOS_Ecom_CheckoutCallbackInfo* Data)
{
    auto* context = static_cast<CheckoutContext*>(Data->ClientData);
    auto* rtContext = RuntimeContext::GetFirstInstance();

    CoronaLog("[EOS_DEBUG] OnCheckoutComplete: result=%s productId='%s'",
              EOS_EResult_ToString(Data->ResultCode),
              context->productIdentifier.c_str());

    auto task = std::make_shared<DispatchStoreTransactionEventTask>();
    if (rtContext)
    {
        task->SetLuaEventDispatcher(rtContext->GetLuaEventDispatcher());
    }

    TransactionInfo txn;
    txn.productIdentifier = context->productIdentifier;

    if (Data->ResultCode == EOS_EResult::EOS_Success)
    {
        txn.state = "purchased";
        txn.receipt = Data->TransactionId ? Data->TransactionId : "";
        CoronaLog("[EOS_DEBUG] OnCheckoutComplete: purchase SUCCEEDED, transactionId='%s'", txn.receipt.c_str());
    }
    else if (Data->ResultCode == EOS_EResult::EOS_Canceled)
    {
        txn.state = "cancelled";
        CoronaLog("[EOS_DEBUG] OnCheckoutComplete: purchase CANCELLED");
    }
    else
    {
        txn.state = "failed";
        CoronaLog("[EOS_DEBUG] OnCheckoutComplete: purchase FAILED: %s", EOS_EResult_ToString(Data->ResultCode));
    }

    task->AddTransaction(txn);
    if (rtContext) rtContext->QueueEventTask(task);

    delete context;
}

/** eos.purchase(productIdentifier) - iOS implementation */
int OnPurchaseProduct(lua_State* luaStatePointer)
{
    const char* productId = lua_tostring(luaStatePointer, 1);
    if (!productId || productId[0] == '\0')
    {
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: ERROR - no product identifier provided");
        return 0;
    }

    CoronaLog("[EOS_DEBUG] OnPurchaseProduct: called with productId='%s'", productId);

    auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
    if (!contextPointer || !contextPointer->fPlatformHandle || !contextPointer->fAccountId)
    {
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: ERROR - not logged in");
        if (contextPointer)
        {
            auto task = std::make_shared<DispatchStoreTransactionEventTask>();
            task->SetLuaEventDispatcher(contextPointer->GetLuaEventDispatcher());
            TransactionInfo txn;
            txn.productIdentifier = productId;
            txn.state = "failed";
            task->AddTransaction(txn);
            contextPointer->QueueEventTask(task);
        }
        return 0;
    }

    EOS_HEcom EcomHandle = EOS_Platform_GetEcomInterface(contextPointer->fPlatformHandle);

    std::string offerId = productId;
    auto it = sCatalogItemToOfferIdMap.find(productId);
    if (it != sCatalogItemToOfferIdMap.end())
    {
        offerId = it->second;
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: mapped catalog item '%s' -> offer '%s'", productId, offerId.c_str());
    }

    EOS_Ecom_CheckoutEntry entry = {};
    entry.ApiVersion = EOS_ECOM_CHECKOUTENTRY_API_LATEST;
    entry.OfferId = offerId.c_str();

    EOS_Ecom_CheckoutOptions options = {};
    options.ApiVersion = EOS_ECOM_CHECKOUT_API_LATEST;
    options.LocalUserId = contextPointer->fAccountId;
    options.OverrideCatalogNamespace = nullptr;
    options.EntryCount = 1;
    options.Entries = &entry;

    auto* checkoutCtx = new CheckoutContext();
    checkoutCtx->productIdentifier = productId;
    checkoutCtx->offerId = offerId;

    CoronaLog("[EOS_DEBUG] OnPurchaseProduct: calling EOS_Ecom_Checkout with offerId='%s'", offerId.c_str());
    EOS_Ecom_Checkout(EcomHandle, &options, checkoutCtx, OnCheckoutComplete);

    return 0;
}

int OnRestorePurchases(lua_State* luaStatePointer) { return 0; }
int OnFinishTransaction(lua_State* luaStatePointer) { return 0; }

/** eos.isLoggedOn() - returns boolean indicating whether user is authenticated */
int OnIsLoggedOn(lua_State* luaStatePointer)
{
    auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
    if (contextPointer && contextPointer->fPlatformHandle && contextPointer->fAccountId)
    {
        lua_pushboolean(luaStatePointer, 1);
    }
    else
    {
        lua_pushboolean(luaStatePointer, 0);
    }
    return 1;
}

/** eos.logout() - clears local auth state */
int OnLogout(lua_State* luaStatePointer)
{
    CoronaLog("[EOS_DEBUG] OnLogout: called");
    auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
    if (contextPointer && contextPointer->fPlatformHandle && contextPointer->fAuthHandle)
    {
        // Delete persistent auth token so next login requires fresh sign-in
        EOS_Auth_DeletePersistentAuthOptions opts = {};
        opts.ApiVersion = EOS_AUTH_DELETEPERSISTENTAUTH_API_LATEST;
        opts.RefreshToken = nullptr;
        EOS_Auth_DeletePersistentAuth(contextPointer->fAuthHandle, &opts, nullptr, nullptr);

        contextPointer->fAccountId = nullptr;
        sLoginInProgress = false;
    }
    return 0;
}

/** eos.loginWithAccountPortal() - iOS implementation */
int OnLoginWithAccountPortal(lua_State* luaStatePointer)
{
    auto contextPointer = (RuntimeContext*)lua_touserdata(luaStatePointer, lua_upvalueindex(1));
    if (!contextPointer || !contextPointer->fPlatformHandle)
    {
        return 0;
    }

    // If another login (e.g. from eos.init()) is already in progress, don't start a second
    // concurrent EOS_Auth_Login — the SDK doesn't support it and the second call will fail.
    // Just wait for the in-progress login to complete; it will dispatch loginResponse.
    if (sLoginInProgress)
    {
        return 0;
    }

    // If already logged in, immediately dispatch a success loginResponse event
    if (contextPointer->fAccountId)
    {
        auto task = std::make_shared<DispatchLoginResponseEventTask>();
        task->SetLuaEventDispatcher(contextPointer->GetLuaEventDispatcher());

        // Build a synthetic successful login callback info
        EOS_Auth_LoginCallbackInfo syntheticData = {};
        syntheticData.ResultCode = EOS_EResult::EOS_Success;
        syntheticData.LocalUserId = contextPointer->fAccountId;
        task->AcquireEventDataFrom(&syntheticData);

        contextPointer->QueueEventTask(task);
        return 0;
    }

    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(contextPointer->fPlatformHandle);
    if (!AuthHandle)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoginWithAccountPortal: Could not get Auth interface");
        return 0;
    }

    EOS_Auth_LoginOptions LoginOptions = {};
    LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;

    EOS_Auth_Credentials Credentials = {};
    Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
    Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
    Credentials.Id = nullptr;
    Credentials.Token = nullptr;

#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iOS requires EOS_IOS_Auth_CredentialsOptions with a presentation context
    // for the ASWebAuthenticationSession used by AccountPortal login.
    EOS_IOS_Auth_CredentialsOptions_Compat iosOpts = {};
    iosOpts.ApiVersion = 2; // EOS_IOS_AUTH_CREDENTIALSOPTIONS_API_LATEST
    iosOpts.PresentationContextProviding = CreateWebAuthContextProvider();
    iosOpts.CreateBackgroundSnapshotView = nullptr;
    iosOpts.CreateBackgroundSnapshotViewContext = nullptr;
    Credentials.SystemAuthCredentialsOptions = &iosOpts;
#endif

    LoginOptions.Credentials = &Credentials;

    EOS_Auth_Login(AuthHandle, &LoginOptions, contextPointer, [](const EOS_Auth_LoginCallbackInfo* Data) {
        auto* ctx = static_cast<RuntimeContext*>(Data->ClientData);
        if (!ctx) return;

        if (Data->ResultCode == EOS_EResult::EOS_Success)
        {
            CoronaLog("[EOS SDK] Login successful via AccountPortal");
            ShowDebugAlert(ctx, "EOS Debug", "loginWithAccountPortal callback: SUCCESS!");
            ctx->fAccountId = Data->LocalUserId;
            ctx->fAuthHandle = EOS_Platform_GetAuthInterface(ctx->fPlatformHandle);
        }
        else
        {
            const char* errStr = EOS_EResult_ToString(Data->ResultCode);
            CoronaLog("[EOS SDK] Login failed: %s", errStr);

            char alertMsg[256];
            snprintf(alertMsg, sizeof(alertMsg), "loginWithAccountPortal FAILED: %s", errStr);
            ShowDebugAlert(ctx, "EOS Error", alertMsg);
        }

        ctx->OnLoginResponse(Data);
    });

    return 0;
}


//---------------------------------------------------------------------------------
// Public Exports
//---------------------------------------------------------------------------------

/**
  Called when this plugin is being loaded from Lua via a require() function.
  Initializes itself with EOS and returns the plugin's Lua table.
 */
CORONA_EXPORT int luaopen_plugin_eos(lua_State* luaStatePointer)
{
	// Validate.
	if (!luaStatePointer)
	{
		return 0;
	}

	// If this plugin instance is being loaded while another one already exists, then make sure that they're
	// both running on the same thread to avoid race conditions since EOS's event handlers are global.
	// Note: This can only happen if multiple Corona runtimes are running at the same time.
	if (RuntimeContext::GetInstanceCount() > 0)
	{
		if (std::this_thread::get_id() != sMainThreadId)
		{
			luaL_error(luaStatePointer, "Cannot load another instance of 'plugin.eos' from another thread.");
			return 0;
		}
	}
	else
	{
		sMainThreadId = std::this_thread::get_id();
	}

	// Create a new runtime context used to receive EOS's event and dispatch them to Lua.
	// Also used to ensure that the EOS overlay is rendered when requested on Windows.
	auto contextPointer = new RuntimeContext(luaStatePointer);
	if (!contextPointer)
	{
		return 0;
	}

	// Push this plugin's Lua table and all of its functions to the top of the Lua stack.
	// Note: The RuntimeContext pointer is pushed as an upvalue to all of these functions via luaL_openlib().
	{
		const struct luaL_Reg luaFunctions[] =
		{
			{ "getAuthIdToken", OnGetAuthIdToken },
			{ "setNotificationPosition", OnSetNotificationPosition },
			{ "addEventListener", OnAddEventListener },
			{ "removeEventListener", OnRemoveEventListener },
			{ "init", OnInit },
			{ "isLoggedOn", OnIsLoggedOn },
			{ "logout", OnLogout },
			{ "loadProducts", OnLoadProducts },
			{ "purchase", OnPurchaseProduct },
			{ "consumePurchase", OnFinishTransaction },
			{ "finishTransaction", OnFinishTransaction },
			{ "restore", OnRestorePurchases },
			{ "loginWithAccountPortal", OnLoginWithAccountPortal },
			{ nullptr, nullptr }
		};
		lua_createtable(luaStatePointer, 0, 0);
		lua_pushlightuserdata(luaStatePointer, contextPointer);
		luaL_openlib(luaStatePointer, nullptr, luaFunctions, 1);
	}

	// Add a Lua finalizer to the plugin's Lua table and to the Lua registry.
	// Note: Lua 5.1 tables do not support the "__gc" metatable field, but Lua light-userdata types do.
	{
		// Create a Lua metatable used to receive the finalize event.
		const struct luaL_Reg luaFunctions[] =
		{
			{ "__gc", OnFinalizing },
			{ nullptr, nullptr }
		};
		luaL_newmetatable(luaStatePointer, "plugin.eos.__gc");
		lua_pushlightuserdata(luaStatePointer, contextPointer);
		luaL_openlib(luaStatePointer, nullptr, luaFunctions, 1);
		lua_pop(luaStatePointer, 1);

		// Add the finalizer metable to the Lua registry.
		CoronaLuaPushUserdata(luaStatePointer, nullptr, "plugin.eos.__gc");
		int luaReferenceKey = luaL_ref(luaStatePointer, LUA_REGISTRYINDEX);

		// Add the finalizer metatable to the plugin's Lua table as an undocumented "__gc" field.
		// Note that a developer can overwrite this field, which is why we add it to the registry above too.
		lua_rawgeti(luaStatePointer, LUA_REGISTRYINDEX, luaReferenceKey);
		lua_setfield(luaStatePointer, -2, "__gc");
	}

	// Wrap the plugin's Lua table in a metatable used to provide readable/writable property fields.
	{
		const struct luaL_Reg luaFunctions[] =
		{
			{ "__index", OnAccessingField },
			{ "__newindex", OnAssigningField },
			{ nullptr, nullptr }
		};
		luaL_newmetatable(luaStatePointer, "plugin.eos");
		lua_pushlightuserdata(luaStatePointer, contextPointer);
		luaL_openlib(luaStatePointer, nullptr, luaFunctions, 1);
		lua_setmetatable(luaStatePointer, -2);
	}

	// Fetch the EOS properties from the "config.lua" file.
	PluginConfigLuaSettings configLuaSettings;
	configLuaSettings.LoadFrom(luaStatePointer);

	// Initialize our connection with EOS if this is the first plugin instance.
	// Note: This avoid initializing twice in case multiple plugin instances exist at the same time.
	if (RuntimeContext::GetInstanceCount() == 1)
	{
		// Init EOS SDK
		EOS_InitializeOptions SDKOptions = {};
		SDKOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
		SDKOptions.AllocateMemoryFunction = nullptr;
		SDKOptions.ReallocateMemoryFunction = nullptr;
		SDKOptions.ReleaseMemoryFunction = nullptr;
		SDKOptions.ProductName = "Coromon"; // JOCHEM - TODO
		SDKOptions.ProductVersion = "1.0.12"; // JOCHEM - TODO
		SDKOptions.Reserved = nullptr;
		SDKOptions.SystemInitializeOptions = nullptr;
		SDKOptions.OverrideThreadAffinity = nullptr;

		EOS_EResult InitResult = EOS_Initialize(&SDKOptions);
		if (InitResult == EOS_EResult::EOS_InvalidParameters)
		{
			CoronaLuaError(luaStatePointer, "[EOS SDK] Init Failed! Invalid Parameters");
			return 0;
		}
		else if (InitResult == EOS_EResult::EOS_AlreadyConfigured) // TODO: Apparently this happens the first time the simulator reloads, should probably prevent reaching this state though
		{
			CoronaLog("WARNING: [EOS SDK] Init Failed! Already Configured");
			return 1;
		}

		CoronaLog("[EOS SDK] Initialized. Setting Logging Callback ...");
		EOS_EResult SetLogCallbackResult = EOS_Logging_SetCallback(&onEOSLogMessageReceived);
		if (SetLogCallbackResult != EOS_EResult::EOS_Success)
		{
			CoronaLog("WARNING: [EOS SDK] Set Logging Callback Failed!");
		}
		else
		{
			CoronaLog("[EOS SDK] Logging Callback Set");
		}

		// Create platform instance
		EOS_Platform_Options PlatformOptions = {};
		PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
		PlatformOptions.bIsServer = false;
		PlatformOptions.EncryptionKey = configLuaSettings.GetStringEncryptionKey();
		PlatformOptions.OverrideCountryCode = nullptr;
		PlatformOptions.OverrideLocaleCode = nullptr;
		PlatformOptions.Flags = EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D9 | EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D10 | EOS_PF_WINDOWS_ENABLE_OVERLAY_OPENGL; // Enable overlay support for D3D9/10 and OpenGL. This sample uses D3D11 or SDL.
		// PlatformOptions.CacheDirectory = FUtils::GetTempDirectory();

		PlatformOptions.ProductId = configLuaSettings.GetStringProductId();
		PlatformOptions.SandboxId = configLuaSettings.GetStringSandboxId();
		PlatformOptions.DeploymentId = configLuaSettings.GetStringDeploymentId();
		PlatformOptions.ClientCredentials.ClientId = configLuaSettings.GetStringClientId();
		PlatformOptions.ClientCredentials.ClientSecret = configLuaSettings.GetStringClientSecret();

	#ifdef _WIN32
		EOS_Platform_RTCOptions RtcOptions = { 0 };
		RtcOptions.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;

        wchar_t CurDir[MAX_PATH + 1] = {};
        ::GetCurrentDirectoryW(MAX_PATH + 1u, CurDir);
        std::wstring BasePath = std::wstring(CurDir);
        std::string XAudio29DllPath;
        XAudio29DllPath.append("/xaudio2_9redist.dll");

        EOS_Windows_RTCOptions WindowsRtcOptions = { 0 };
        WindowsRtcOptions.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
        WindowsRtcOptions.XAudio29DllPath = XAudio29DllPath.c_str();
        RtcOptions.PlatformSpecificOptions = &WindowsRtcOptions;

		PlatformOptions.RTCOptions = &RtcOptions;
	#endif // _WIN32

	#if ALLOW_RESERVED_PLATFORM_OPTIONS
		SetReservedPlatformOptions(PlatformOptions);
	#else
		PlatformOptions.Reserved = NULL;
	#endif // ALLOW_RESERVED_PLATFORM_OPTIONS

		EOS_HPlatform platformHandle = EOS_Platform_Create(&PlatformOptions);
		if (!platformHandle) {
			CoronaLuaError(luaStatePointer, "Failed to initialize connection with Epic client.");
		}
		contextPointer->fPlatformHandle = platformHandle;
	}

	#ifndef EOS_STEAM_ENABLED
	auto launcherAuthTypeLaunchArg = CMDLine::Map().find("AUTH_TYPE");
	auto launcherAuthPasswordLaunchArg = CMDLine::Map().find("AUTH_PASSWORD");
	if (contextPointer->fPlatformHandle && launcherAuthTypeLaunchArg != CMDLine::End() && launcherAuthPasswordLaunchArg != CMDLine::End()) {
		std::string launcherAuthType = launcherAuthTypeLaunchArg->second;
		if (launcherAuthType == "exchangecode") {
			std::string launcherAuthPassword = launcherAuthPasswordLaunchArg->second;
			if (!launcherAuthPassword.empty())
			{
				contextPointer->fAuthHandle = EOS_Platform_GetAuthInterface(contextPointer->fPlatformHandle);

				EOS_Auth_Credentials Credentials = {};
				Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;

				EOS_Auth_LoginOptions LoginOptions;
				memset(&LoginOptions, 0, sizeof(LoginOptions));
				LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
				LoginOptions.ScopeFlags |= EOS_EAuthScopeFlags::EOS_AS_NoFlags;

				Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_ExchangeCode;
				Credentials.Token = launcherAuthPassword.c_str();
				LoginOptions.Credentials = &Credentials;

				EOS_Auth_Login(contextPointer->fAuthHandle, &LoginOptions, contextPointer, onLoginCallback);
			}
		}
	}
	#endif

	// We're returning 1 Lua plugin table.
	return 1;
}
#endif // !ANDROID

//---------------------------------------------------------------------------------
// Android JNI Bridge Functions
//---------------------------------------------------------------------------------
// These functions are called from native-lib.cpp on Android.
// On desktop, the equivalent functionality is handled directly in luaopen_plugin_eos.
#ifdef ANDROID
#include "EosLuaInterface.h"

bool InitializeSDK(lua_State* luaStatePointer, EOS_InitializeOptions& options)
{
	EOS_EResult InitResult = EOS_Initialize(&options);
	if (InitResult == EOS_EResult::EOS_InvalidParameters)
	{
		CoronaLog("ERROR: [EOS SDK] Init Failed! Invalid Parameters");
		return false;
	}
	else if (InitResult == EOS_EResult::EOS_AlreadyConfigured)
	{
		CoronaLog("WARNING: [EOS SDK] Already Configured");
		return true;
	}
	else if (InitResult != EOS_EResult::EOS_Success)
	{
		CoronaLog("ERROR: [EOS SDK] Init Failed! Result: %s", EOS_EResult_ToString(InitResult));
		return false;
	}

	CoronaLog("[EOS SDK] Initialized. Setting Logging Callback ...");
	EOS_EResult SetLogCallbackResult = EOS_Logging_SetCallback(&onEOSLogMessageReceived);
	if (SetLogCallbackResult != EOS_EResult::EOS_Success)
	{
		CoronaLog("WARNING: [EOS SDK] Set Logging Callback Failed!");
	}
	else
	{
		CoronaLog("[EOS SDK] Logging Callback Set");
		EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);
	}

	return true;
}

// Ecom functions for Android
// Forward declarations for helpers defined in native-lib.cpp
extern EOS_HPlatform PlatformHandle;
extern EOS_EpicAccountId LocalUserId;
extern std::shared_ptr<LuaEventDispatcher>& GetAndroidLuaEventDispatcher();
extern void QueueAndroidEvent(std::shared_ptr<BaseDispatchEventTask> task);

// Map from product identifier (catalog item ID) → offer ID
// Populated during OnQueryOffersComplete, used by OnPurchaseProduct to look up
// the correct offer ID for EOS_Ecom_Checkout (which requires offer IDs, not catalog item IDs).
static std::map<std::string, std::string> sCatalogItemToOfferIdMap;

/** Context passed through EOS_Ecom_QueryOffers callback to carry requested offer IDs */
struct QueryOffersContext {
    std::vector<std::string> requestedOfferIds;
};

/** Helper: Format price from EOS Ecom offer data into a localized price string */
static std::string FormatOfferPrice(const EOS_Ecom_CatalogOffer* offer)
{
    if (!offer || offer->PriceResult != EOS_EResult::EOS_Success)
    {
        return "N/A";
    }

    uint64_t price = offer->CurrentPrice64;
    uint32_t decimalPoint = offer->DecimalPoint;
    std::string currencyCode = offer->CurrencyCode ? offer->CurrencyCode : "";

    // Format price with decimal point
    // e.g. price=299, decimalPoint=2 → "2.99"
    std::string priceStr;
    if (decimalPoint > 0)
    {
        uint64_t divisor = 1;
        for (uint32_t d = 0; d < decimalPoint; d++) divisor *= 10;
        uint64_t whole = price / divisor;
        uint64_t frac = price % divisor;

        std::ostringstream oss;
        oss << whole << ".";
        // Pad fractional part with leading zeros
        std::string fracStr = std::to_string(frac);
        while (fracStr.length() < decimalPoint) fracStr = "0" + fracStr;
        oss << fracStr;
        priceStr = oss.str();
    }
    else
    {
        priceStr = std::to_string(price);
    }

    // Add currency symbol for common currencies
    if (currencyCode == "USD") return "$" + priceStr;
    else if (currencyCode == "EUR") return "\xE2\x82\xAC" + priceStr; // € in UTF-8
    else if (currencyCode == "GBP") return "\xC2\xA3" + priceStr;     // £ in UTF-8
    else if (currencyCode == "JPY") return "\xC2\xA5" + priceStr;     // ¥ in UTF-8
    else if (currencyCode == "CAD") return "CA$" + priceStr;
    else if (currencyCode == "AUD") return "A$" + priceStr;
    else if (currencyCode == "BRL") return "R$" + priceStr;
    else if (currencyCode == "MXN") return "MX$" + priceStr;
    else if (!currencyCode.empty()) return priceStr + " " + currencyCode;
    else return priceStr;
}

/** EOS Ecom QueryOffers completion callback - runs on EOS SDK thread */
static void EOS_CALL OnQueryOffersComplete(const EOS_Ecom_QueryOffersCallbackInfo* Data)
{
    auto context = static_cast<QueryOffersContext*>(Data->ClientData);
    auto& dispatcher = GetAndroidLuaEventDispatcher();

    auto task = std::make_shared<DispatchLoadProductsEventTask>();
    task->SetLuaEventDispatcher(dispatcher);

    if (Data->ResultCode != EOS_EResult::EOS_Success)
    {
        CoronaLog("[EOS_DEBUG] OnQueryOffersComplete: FAILED result=%s", EOS_EResult_ToString(Data->ResultCode));
        task->SetIsError(true);
        task->SetErrorString(EOS_EResult_ToString(Data->ResultCode));
    }
    else
    {
        EOS_HEcom EcomHandle = EOS_Platform_GetEcomInterface(PlatformHandle);

        EOS_Ecom_GetOfferCountOptions CountOptions = {};
        CountOptions.ApiVersion = EOS_ECOM_GETOFFERCOUNT_API_LATEST;
        CountOptions.LocalUserId = LocalUserId;
        uint32_t offerCount = EOS_Ecom_GetOfferCount(EcomHandle, &CountOptions);

        CoronaLog("[EOS_DEBUG] OnQueryOffersComplete: SUCCESS, %u offers in catalog, %d requested",
                  offerCount, (int)context->requestedOfferIds.size());

        // Build a set of requested IDs for fast lookup (and track which have been matched)
        std::set<std::string> requestedSet(context->requestedOfferIds.begin(),
                                            context->requestedOfferIds.end());
        std::set<std::string> matchedSet;

        int matchedCount = 0;
        for (uint32_t i = 0; i < offerCount; i++)
        {
            EOS_Ecom_CopyOfferByIndexOptions CopyOptions = {};
            CopyOptions.ApiVersion = EOS_ECOM_COPYOFFERBYINDEX_API_LATEST;
            CopyOptions.LocalUserId = LocalUserId;
            CopyOptions.OfferIndex = i;

            EOS_Ecom_CatalogOffer* offer = nullptr;
            EOS_EResult copyResult = EOS_Ecom_CopyOfferByIndex(EcomHandle, &CopyOptions, &offer);
            if (copyResult != EOS_EResult::EOS_Success || !offer)
            {
                CoronaLog("[EOS_DEBUG]   offer[%u]: CopyOfferByIndex failed: %s", i, EOS_EResult_ToString(copyResult));
                continue;
            }

            std::string offerId = offer->Id ? offer->Id : "";
            std::string offerTitle = offer->TitleText ? offer->TitleText : "(null)";

            // Log every offer we see for debugging
            CoronaLog("[EOS_DEBUG]   catalog offer[%u]: id='%s' title='%s' available=%d price=%s",
                      i, offerId.c_str(), offerTitle.c_str(),
                      (int)offer->bAvailableForPurchase,
                      FormatOfferPrice(offer).c_str());

            // Also enumerate catalog items within this offer for debugging and matching
            EOS_Ecom_GetOfferItemCountOptions ItemCountOptions = {};
            ItemCountOptions.ApiVersion = EOS_ECOM_GETOFFERITEMCOUNT_API_LATEST;
            ItemCountOptions.LocalUserId = LocalUserId;
            ItemCountOptions.OfferId = offer->Id;
            uint32_t itemCount = EOS_Ecom_GetOfferItemCount(EcomHandle, &ItemCountOptions);

            CoronaLog("[EOS_DEBUG]     offer '%s' contains %u catalog items:", offerId.c_str(), itemCount);

            for (uint32_t j = 0; j < itemCount; j++)
            {
                EOS_Ecom_CopyOfferItemByIndexOptions ItemCopyOptions = {};
                ItemCopyOptions.ApiVersion = EOS_ECOM_COPYOFFERITEMBYINDEX_API_LATEST;
                ItemCopyOptions.LocalUserId = LocalUserId;
                ItemCopyOptions.OfferId = offer->Id;
                ItemCopyOptions.ItemIndex = j;

                EOS_Ecom_CatalogItem* item = nullptr;
                EOS_EResult itemResult = EOS_Ecom_CopyOfferItemByIndex(EcomHandle, &ItemCopyOptions, &item);
                if (itemResult == EOS_EResult::EOS_Success && item)
                {
                    std::string itemId = item->Id ? item->Id : "";
                    CoronaLog("[EOS_DEBUG]       item[%u]: id='%s' title='%s' entitlement='%s' type=%d",
                              j, itemId.c_str(),
                              item->TitleText ? item->TitleText : "(null)",
                              item->EntitlementName ? item->EntitlementName : "(null)",
                              (int)item->ItemType);

                    // Always record the catalog item → offer ID mapping for purchase lookups
                    if (!itemId.empty() && !offerId.empty())
                    {
                        sCatalogItemToOfferIdMap[itemId] = offerId;
                    }

                    // Check if any requested ID matches this catalog item ID
                    if (!requestedSet.empty() && requestedSet.count(itemId) > 0 && matchedSet.count(itemId) == 0)
                    {
                        ProductInfo product;
                        product.productIdentifier = itemId; // Use the catalog item ID as the product identifier
                        product.title = item->TitleText ? item->TitleText : offerTitle;
                        product.description = item->DescriptionText ? item->DescriptionText : "";
                        product.localizedPrice = FormatOfferPrice(offer); // Use the parent offer's price

                        CoronaLog("[EOS_DEBUG]       MATCHED catalog item by ID: id='%s' title='%s' price='%s' (offerId='%s')",
                                  product.productIdentifier.c_str(),
                                  product.title.c_str(),
                                  product.localizedPrice.c_str(),
                                  offerId.c_str());

                        task->AddProduct(product);
                        matchedSet.insert(itemId);
                        matchedCount++;
                    }

                    EOS_Ecom_CatalogItem_Release(item);
                }
                else
                {
                    CoronaLog("[EOS_DEBUG]       item[%u]: CopyOfferItemByIndex failed: %s", j, EOS_EResult_ToString(itemResult));
                }
            }

            // Also check if the offer ID itself was requested (original matching logic)
            bool isOfferRequested = false;
            if (requestedSet.empty())
            {
                // No filter — return all offers
                isOfferRequested = true;
            }
            else if (requestedSet.count(offerId) > 0 && matchedSet.count(offerId) == 0)
            {
                isOfferRequested = true;
            }

            if (isOfferRequested)
            {
                ProductInfo product;
                product.productIdentifier = offerId;
                product.title = offer->TitleText ? offer->TitleText : "";
                product.description = offer->DescriptionText ? offer->DescriptionText : "";
                product.localizedPrice = FormatOfferPrice(offer);

                CoronaLog("[EOS_DEBUG]   MATCHED offer by ID: id='%s' title='%s' price='%s'",
                          product.productIdentifier.c_str(),
                          product.title.c_str(),
                          product.localizedPrice.c_str());

                task->AddProduct(product);
                matchedSet.insert(offerId);
                matchedCount++;
            }

            EOS_Ecom_CatalogOffer_Release(offer);
        }

        // Log any requested IDs that didn't match anything
        for (const auto& reqId : context->requestedOfferIds)
        {
            if (matchedSet.count(reqId) == 0)
            {
                CoronaLog("[EOS_DEBUG]   UNMATCHED requested ID: '%s' (not found as offer ID or catalog item ID)", reqId.c_str());
            }
        }

        CoronaLog("[EOS_DEBUG] OnQueryOffersComplete: returning %d matched products to Lua (from %u offers, %d requested)",
                  matchedCount, offerCount, (int)context->requestedOfferIds.size());
    }

    // Queue the loadProducts event for dispatch on the Corona thread via enterFrame
    QueueAndroidEvent(task);

    // Clean up the context
    delete context;
}

/** eos.loadProducts(identifiers, listener) - Android implementation using EOS Ecom API */
int OnLoadProducts(lua_State* luaStatePointer)
{
    CoronaLog("[EOS_DEBUG] OnLoadProducts: called");

    if (!PlatformHandle)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoadProducts: PlatformHandle is null");
        // Dispatch error event immediately
        auto& dispatcher = GetAndroidLuaEventDispatcher();
        if (dispatcher)
        {
            auto task = std::make_shared<DispatchLoadProductsEventTask>();
            task->SetLuaEventDispatcher(dispatcher);
            task->SetIsError(true);
            task->SetErrorString("Platform not initialized");
            QueueAndroidEvent(task);
        }
        return 0;
    }

    if (!LocalUserId)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoadProducts: LocalUserId is null (not logged in)");
        auto& dispatcher = GetAndroidLuaEventDispatcher();
        if (dispatcher)
        {
            auto task = std::make_shared<DispatchLoadProductsEventTask>();
            task->SetLuaEventDispatcher(dispatcher);
            task->SetIsError(true);
            task->SetErrorString("Not logged in");
            QueueAndroidEvent(task);
        }
        return 0;
    }

    // Read product identifiers from Lua stack arg 1 (table of strings)
    auto context = new QueryOffersContext();
    if (lua_istable(luaStatePointer, 1))
    {
        int tableLen = (int)lua_objlen(luaStatePointer, 1);
        CoronaLog("[EOS_DEBUG] OnLoadProducts: reading %d product identifiers from Lua", tableLen);
        for (int i = 1; i <= tableLen; i++)
        {
            lua_rawgeti(luaStatePointer, 1, i);
            if (lua_isstring(luaStatePointer, -1))
            {
                const char* id = lua_tostring(luaStatePointer, -1);
                if (id)
                {
                    context->requestedOfferIds.push_back(id);
                    CoronaLog("[EOS_DEBUG]   identifier[%d] = %s", i, id);
                }
            }
            lua_pop(luaStatePointer, 1);
        }
    }
    else
    {
        CoronaLog("[EOS_DEBUG] OnLoadProducts: arg 1 is not a table (type=%d), will query all offers",
                  lua_type(luaStatePointer, 1));
    }

    // Get the Ecom interface
    EOS_HEcom EcomHandle = EOS_Platform_GetEcomInterface(PlatformHandle);
    if (!EcomHandle)
    {
        CoronaLog("ERROR: [EOS SDK] OnLoadProducts: Could not get Ecom interface");
        auto& dispatcher = GetAndroidLuaEventDispatcher();
        if (dispatcher)
        {
            auto task = std::make_shared<DispatchLoadProductsEventTask>();
            task->SetLuaEventDispatcher(dispatcher);
            task->SetIsError(true);
            task->SetErrorString("Could not get Ecom interface");
            QueueAndroidEvent(task);
        }
        delete context;
        return 0;
    }

    // Query all offers from the EOS catalog (async)
    EOS_Ecom_QueryOffersOptions QueryOptions = {};
    QueryOptions.ApiVersion = EOS_ECOM_QUERYOFFERS_API_LATEST;
    QueryOptions.LocalUserId = LocalUserId;
    QueryOptions.OverrideCatalogNamespace = nullptr;

    CoronaLog("[EOS_DEBUG] OnLoadProducts: calling EOS_Ecom_QueryOffers with %d requested IDs...",
              (int)context->requestedOfferIds.size());
    EOS_Ecom_QueryOffers(EcomHandle, &QueryOptions, context, OnQueryOffersComplete);

    return 0;
}

// ---------------------------------------------------------------------------------
// EOS Ecom Checkout (Purchase) Implementation
// ---------------------------------------------------------------------------------

/** Context passed through EOS_Ecom_Checkout callback */
struct CheckoutContext {
    std::string productIdentifier;  // The original product ID passed from Lua
    std::string offerId;            // The offer ID used for checkout
};

/** EOS Ecom Checkout completion callback - runs on EOS SDK thread */
static void EOS_CALL OnCheckoutComplete(const EOS_Ecom_CheckoutCallbackInfo* Data)
{
    auto* context = static_cast<CheckoutContext*>(Data->ClientData);

    CoronaLog("[EOS_DEBUG] OnCheckoutComplete: result=%s transactionId='%s' productId='%s' offerId='%s'",
              EOS_EResult_ToString(Data->ResultCode),
              Data->TransactionId ? Data->TransactionId : "(null)",
              context->productIdentifier.c_str(),
              context->offerId.c_str());

    auto task = std::make_shared<DispatchStoreTransactionEventTask>();
    auto& dispatcher = GetAndroidLuaEventDispatcher();
    if (dispatcher)
    {
        task->SetLuaEventDispatcher(dispatcher);
    }

    TransactionInfo txn;
    txn.productIdentifier = context->productIdentifier;

    if (Data->ResultCode == EOS_EResult::EOS_Success)
    {
        txn.state = "purchased";
        txn.receipt = Data->TransactionId ? Data->TransactionId : "";
        CoronaLog("[EOS_DEBUG] OnCheckoutComplete: purchase SUCCEEDED, transactionId='%s'", txn.receipt.c_str());
    }
    else if (Data->ResultCode == EOS_EResult::EOS_Canceled)
    {
        txn.state = "cancelled";
        CoronaLog("[EOS_DEBUG] OnCheckoutComplete: purchase CANCELLED");
    }
    else
    {
        txn.state = "failed";
        CoronaLog("[EOS_DEBUG] OnCheckoutComplete: purchase FAILED: %s", EOS_EResult_ToString(Data->ResultCode));
    }

    task->AddTransaction(txn);
    QueueAndroidEvent(task);

    delete context;
}

int OnPurchaseProduct(lua_State* luaStatePointer)
{
    // Arg 1: product identifier (string) — could be an offer ID or a catalog item ID
    const char* productId = lua_tostring(luaStatePointer, 1);
    if (!productId || productId[0] == '\0')
    {
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: ERROR - no product identifier provided");
        return 0;
    }

    CoronaLog("[EOS_DEBUG] OnPurchaseProduct: called with productId='%s'", productId);

    if (PlatformHandle == nullptr || LocalUserId == nullptr)
    {
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: ERROR - not logged in (PlatformHandle=%p, LocalUserId=%p)",
                  PlatformHandle, LocalUserId);
        // Dispatch a failed transaction event
        auto task = std::make_shared<DispatchStoreTransactionEventTask>();
        auto& dispatcher = GetAndroidLuaEventDispatcher();
        if (dispatcher) task->SetLuaEventDispatcher(dispatcher);
        TransactionInfo txn;
        txn.productIdentifier = productId;
        txn.state = "failed";
        task->AddTransaction(txn);
        QueueAndroidEvent(task);
        return 0;
    }

    EOS_HEcom EcomHandle = EOS_Platform_GetEcomInterface(PlatformHandle);

    // Determine the offer ID for checkout.
    // EOS_Ecom_Checkout requires an offer ID. If the product identifier is a
    // catalog item ID (matched via OnQueryOffersComplete), look up the corresponding offer ID.
    // Otherwise, assume productId is already an offer ID.
    std::string offerId = productId;
    auto it = sCatalogItemToOfferIdMap.find(productId);
    if (it != sCatalogItemToOfferIdMap.end())
    {
        offerId = it->second;
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: mapped catalog item '%s' → offer '%s'", productId, offerId.c_str());
    }
    else
    {
        CoronaLog("[EOS_DEBUG] OnPurchaseProduct: using productId as offer ID directly: '%s'", productId);
    }

    // Check if the offer is available for purchase (log warning if not)
    {
        EOS_Ecom_CopyOfferByIdOptions copyOpts = {};
        copyOpts.ApiVersion = EOS_ECOM_COPYOFFERBYID_API_LATEST;
        copyOpts.LocalUserId = LocalUserId;
        copyOpts.OfferId = offerId.c_str();
        EOS_Ecom_CatalogOffer* offer = nullptr;
        EOS_EResult copyRes = EOS_Ecom_CopyOfferById(EcomHandle, &copyOpts, &offer);
        if (copyRes == EOS_EResult::EOS_Success && offer)
        {
            CoronaLog("[EOS_DEBUG] OnPurchaseProduct: offer '%s' title='%s' available=%d price=%s",
                      offerId.c_str(),
                      offer->TitleText ? offer->TitleText : "(null)",
                      (int)offer->bAvailableForPurchase,
                      FormatOfferPrice(offer).c_str());
            if (!offer->bAvailableForPurchase)
            {
                CoronaLog("[EOS_DEBUG] WARNING: offer '%s' has bAvailableForPurchase=FALSE — "
                          "this usually means the offer has unmet prerequisites in the EOS catalog. "
                          "Check the EOS Developer Portal catalog configuration.", offerId.c_str());
            }
            EOS_Ecom_CatalogOffer_Release(offer);
        }
        else
        {
            CoronaLog("[EOS_DEBUG] OnPurchaseProduct: could not look up offer '%s': %s (proceeding anyway)",
                      offerId.c_str(), EOS_EResult_ToString(copyRes));
        }
    }

    // Create checkout entry
    EOS_Ecom_CheckoutEntry entry = {};
    entry.ApiVersion = EOS_ECOM_CHECKOUTENTRY_API_LATEST;
    entry.OfferId = offerId.c_str();

    // Create checkout options
    EOS_Ecom_CheckoutOptions options = {};
    options.ApiVersion = EOS_ECOM_CHECKOUT_API_LATEST;
    options.LocalUserId = LocalUserId;
    options.OverrideCatalogNamespace = nullptr;
    options.EntryCount = 1;
    options.Entries = &entry;

    auto* context = new CheckoutContext();
    context->productIdentifier = productId;
    context->offerId = offerId;

    CoronaLog("[EOS_DEBUG] OnPurchaseProduct: calling EOS_Ecom_Checkout with offerId='%s'", offerId.c_str());
    EOS_Ecom_Checkout(EcomHandle, &options, context, OnCheckoutComplete);

    return 0;
}

int OnRestorePurchases(lua_State* luaStatePointer) { return 0; }
int OnFinishTransaction(lua_State* luaStatePointer) { return 0; }

// Auth functions - on Android these are handled by the Java layer via
// direct JNI methods (CreatePlatform, LoginWithAccountPortal, Logout, etc.)
bool OnIsLoggedOn(lua_State* luaStatePointer) { return false; }
bool OnLoginWithAccountPortal(lua_State* luaStatePointer) { return false; }
bool OnLogout(lua_State* luaStatePointer) { return false; }

// Event listener functions - on Android, we use the global LuaEventDispatcher
// from native-lib.cpp instead of RuntimeContext (which doesn't exist on Android).
// This avoids the lua_upvalueindex(1) crash from JNI.
// Note: GetAndroidLuaEventDispatcher() is already declared above in the Ecom section.

/** eos.addEventListener(eventName, listener) - Android implementation */
int OnAddEventListener(lua_State* luaStatePointer)
{
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the event name from argument 1.
	const char* eventName = nullptr;
	if (lua_type(luaStatePointer, 1) == LUA_TSTRING)
	{
		eventName = lua_tostring(luaStatePointer, 1);
	}
	if (!eventName || ('\0' == eventName[0]))
	{
		CoronaLuaError(luaStatePointer, "1st argument must be set to an event name.");
		return 0;
	}

	// Validate the 2nd argument is a Lua listener function/table.
	if (!CoronaLuaIsListener(luaStatePointer, 2, eventName))
	{
		CoronaLuaError(luaStatePointer, "2nd argument must be set to a listener.");
		return 0;
	}

	// Add the listener using the global Android event dispatcher.
	auto& dispatcher = GetAndroidLuaEventDispatcher();
	if (dispatcher)
	{
		CoronaLog("[EOS_DEBUG] OnAddEventListener: Adding listener for event '%s'", eventName);
		dispatcher->AddEventListener(luaStatePointer, eventName, 2);
	}
	else
	{
		CoronaLog("WARNING: [EOS_DEBUG] OnAddEventListener: No event dispatcher available yet for event '%s'", eventName);
	}
	return 0;
}

/** eos.removeEventListener(eventName, listener) - Android implementation */
int OnRemoveEventListener(lua_State* luaStatePointer)
{
	if (!luaStatePointer)
	{
		return 0;
	}

	// Fetch the event name from argument 1.
	const char* eventName = nullptr;
	if (lua_type(luaStatePointer, 1) == LUA_TSTRING)
	{
		eventName = lua_tostring(luaStatePointer, 1);
	}
	if (!eventName || ('\0' == eventName[0]))
	{
		CoronaLuaError(luaStatePointer, "1st argument must be set to an event name.");
		return 0;
	}

	// Validate the 2nd argument is a Lua listener function/table.
	if (!CoronaLuaIsListener(luaStatePointer, 2, eventName))
	{
		CoronaLuaError(luaStatePointer, "2nd argument must be set to a listener.");
		return 0;
	}

	// Remove the listener using the global Android event dispatcher.
	auto& dispatcher = GetAndroidLuaEventDispatcher();
	if (dispatcher)
	{
		CoronaLog("[EOS_DEBUG] OnRemoveEventListener: Removing listener for event '%s'", eventName);
		dispatcher->RemoveEventListener(luaStatePointer, eventName, 2);
	}
	else
	{
		CoronaLog("WARNING: [EOS_DEBUG] OnRemoveEventListener: No event dispatcher available for event '%s'", eventName);
	}
	return 0;
}

// OnGetAuthIdToken - Android implementation using global PlatformHandle/LocalUserId
// instead of RuntimeContext (which doesn't exist on Android).
// Note: PlatformHandle and LocalUserId are already declared extern above in the Ecom section.

int OnGetAuthIdToken(lua_State* luaStatePointer)
{
	if (!luaStatePointer)
	{
		return 0;
	}

	if (!PlatformHandle || !LocalUserId)
	{
		CoronaLog("WARNING: [EOS SDK] Cannot get auth ID token - not logged in or platform not initialized");
		return 0;
	}

	EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
	if (!AuthHandle)
	{
		CoronaLog("WARNING: [EOS SDK] Cannot get auth interface");
		return 0;
	}

	EOS_Auth_CopyIdTokenOptions CopyTokenOptions = { 0 };
	CopyTokenOptions.ApiVersion = EOS_AUTH_COPYUSERAUTHTOKEN_API_LATEST;
	CopyTokenOptions.AccountId = LocalUserId;

	EOS_Auth_IdToken* outIdToken;
	if (EOS_Auth_CopyIdToken(AuthHandle, &CopyTokenOptions, &outIdToken) == EOS_EResult::EOS_Success)
	{
		lua_pushstring(luaStatePointer, outIdToken->JsonWebToken);
		EOS_Auth_IdToken_Release(outIdToken);
		return 1;
	}
	else
	{
		CoronaLog("WARNING: [EOS SDK] User Auth Token is invalid");
		return 0;
	}
}

#endif // ANDROID
