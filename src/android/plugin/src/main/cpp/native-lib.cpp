#include <jni.h>
#include <string>
#include <set>
#include <queue>
#include <memory>
#include <mutex>
#include <eos_init.h>
#include <eos_sdk.h>
#include <eos_auth.h>
#include <eos_connect.h>
#include <eos_logging.h>
#include <eos_auth_types.h>
#include <eos_userinfo.h>
#include "Android/eos_android.h"
#include "EosLuaInterface.h"
#include "LuaEventDispatcher.h"
#include "DispatchEventTask.h"
#include "PluginConfigLuaSettings.h"
#include "CoronaLua.h"

extern "C"
{
#	include "lua.h"
}

bool IsSDKInitialized = false;
EOS_HPlatform PlatformHandle = nullptr;
JNIEnv *LocalENV = nullptr;
jclass GlobalRefLuaLoaderClass = nullptr;
jobject GlobalRefLuaLoaderInstance = nullptr;

EOS_EpicAccountId LocalUserId = nullptr;
static EOS_UserInfo *LocalUserInfo = nullptr;
static EOS_NotificationId NotifyLoginStatusChangedId = EOS_INVALID_NOTIFICATIONID;
static jobject GlobalRefActivity = nullptr;

// Persistent auth race-condition guards:
// When loginWithAccountPortal() is called while persistent auth is still in-flight,
// we defer the account portal login. If persistent auth succeeds, the loginResponse
// event is dispatched and no Chrome is needed. If persistent auth fails, the deferred
// account portal login is started automatically.
static bool sPersistentAuthInProgress = false;
static bool sPendingAccountPortalLogin = false;

// ---------------------------------------------------------------------------------
// Android Event Dispatch Infrastructure
// ---------------------------------------------------------------------------------
// On Android there is no RuntimeContext. Instead we use a global LuaEventDispatcher
// and an event task queue, processed during enterFrame, to bridge EOS callbacks to Lua.

std::shared_ptr<LuaEventDispatcher> sAndroidLuaEventDispatcher;
static std::queue<std::shared_ptr<BaseDispatchEventTask>> sAndroidEventQueue;
static std::mutex sEventQueueMutex;

/** Accessor for EosLuaInterface.cpp Android stubs */
std::shared_ptr<LuaEventDispatcher>& GetAndroidLuaEventDispatcher() {
    return sAndroidLuaEventDispatcher;
}

void DeletePersistentAuth();
void LoginPersistentAuth();
void AddNotifyLoginStatusChanged();

/** Call Java showtext method to display log in Android view */
void OS_LOG(const char *Text) {
    if (Text) {
        jmethodID MethodID = LocalENV->GetMethodID(GlobalRefLuaLoaderClass, "ShowText", "(Ljava/lang/String;)V");
        LocalENV->CallVoidMethod(GlobalRefLuaLoaderInstance, MethodID, LocalENV->NewStringUTF(Text));
    }
}

// Get native Lua state pointer from Java LuaState object
lua_State* GetLuaStatePointer(JNIEnv *env, jobject luaStateObj) {
    jclass luaStateClass = env->GetObjectClass(luaStateObj);
    jfieldID luaStatePointerField = env->GetFieldID(luaStateClass, "luaState", "J"); // Assuming LuaState stores pointer in a long field
    return (lua_State*) env->GetLongField(luaStateObj, luaStatePointerField);
}

/** Call Java UIButtonHandler method to hide/show correct button */
void LoginStateChanged(bool loggedIn) {
    CoronaLog("[EOS_DEBUG] native-lib: LoginStateChanged(%s)", loggedIn ? "true" : "false");
    jmethodID MethodID = LocalENV->GetMethodID(GlobalRefLuaLoaderClass, "LoginStateChanged", "(Z)V");
    LocalENV->CallVoidMethod(GlobalRefLuaLoaderInstance, MethodID, loggedIn);
}

void LoginInProgress() {
    CoronaLog("[EOS_DEBUG] native-lib: LoginInProgress()");
    jmethodID MethodID = LocalENV->GetMethodID(GlobalRefLuaLoaderClass, "LoginInProgress", "()V");
    LocalENV->CallVoidMethod(GlobalRefLuaLoaderInstance, MethodID);
}

/** An example of obtaining the display name for the user currently logged into the EOS Auth Interface */
std::string GetLoggedInDisplayName() {
    if (PlatformHandle == nullptr) {
        return "";
    }

    EOS_HUserInfo UserInfoHandle = EOS_Platform_GetUserInfoInterface(PlatformHandle);

    /** Release any data returned to us from a previous call to GetLoggedInDisplayName */
    if (LocalUserInfo != nullptr) {
        EOS_UserInfo_Release(LocalUserInfo);
        LocalUserInfo = nullptr;
    }

    EOS_UserInfo_CopyUserInfoOptions CopyUserInfoOptions = {};
    CopyUserInfoOptions.ApiVersion = EOS_USERINFO_COPYUSERINFO_API_LATEST;
    CopyUserInfoOptions.LocalUserId = LocalUserId;
    CopyUserInfoOptions.TargetUserId = LocalUserId;

    EOS_EResult ResultCode = EOS_UserInfo_CopyUserInfo(UserInfoHandle, &CopyUserInfoOptions, &LocalUserInfo);
    bool bSuccessful = ResultCode == EOS_EResult::EOS_Success;
    return std::string(bSuccessful ? LocalUserInfo->DisplayName : "");
}

extern "C"
JNIEXPORT jstring JNICALL
Java_plugin_eos_LuaLoader_GetUsername(JNIEnv *env, jobject thiz) {
    return LocalENV->NewStringUTF(GetLoggedInDisplayName().c_str());
}

// ---------------------------------------------------------------------------------
// Helper: Queue a loginResponse event for dispatch to Lua during next enterFrame
// ---------------------------------------------------------------------------------
static void QueueLoginResponseEvent(const EOS_Auth_LoginCallbackInfo *Data) {
    if (!sAndroidLuaEventDispatcher) {
        CoronaLog("[EOS_DEBUG] native-lib: QueueLoginResponseEvent - no event dispatcher, skipping");
        return;
    }
    CoronaLog("[EOS_DEBUG] native-lib: QueueLoginResponseEvent result=%s", EOS_EResult_ToString(Data->ResultCode));
    auto taskPointer = new DispatchLoginResponseEventTask();
    taskPointer->SetLuaEventDispatcher(sAndroidLuaEventDispatcher);
    taskPointer->AcquireEventDataFrom(Data);
    {
        std::lock_guard<std::mutex> lock(sEventQueueMutex);
        sAndroidEventQueue.push(std::shared_ptr<BaseDispatchEventTask>(taskPointer));
    }
}

/** Helper: Queue any event task for dispatch to Lua during next enterFrame.
 *  Called from EosLuaInterface.cpp for loadProducts, purchase, etc. */
void QueueAndroidEvent(std::shared_ptr<BaseDispatchEventTask> task) {
    std::lock_guard<std::mutex> lock(sEventQueueMutex);
    sAndroidEventQueue.push(task);
}

/** Callback to handle login status changes */
void EOS_CALL AuthNotifyLoginStatusChangedCb(const EOS_Auth_LoginStatusChangedCallbackInfo *Data) {
    CoronaLog("[EOS_DEBUG] native-lib: AuthNotifyLoginStatusChangedCb prev=%d current=%d",
              (int)Data->PrevStatus, (int)Data->CurrentStatus);
    if (Data->CurrentStatus == EOS_ELoginStatus::EOS_LS_LoggedIn) {
        LoginStateChanged(true);
    } else if (Data->CurrentStatus == EOS_ELoginStatus::EOS_LS_NotLoggedIn) {
        DeletePersistentAuth();
        LoginStateChanged(false);
    }
}

/** Callback to handle result of attempting a login using the web account portal */
void EOS_CALL AuthLoginCb(const EOS_Auth_LoginCallbackInfo *Data) {
    CoronaLog("[EOS_DEBUG] native-lib: AuthLoginCb result=%s isComplete=%s",
              EOS_EResult_ToString(Data->ResultCode),
              EOS_EResult_IsOperationComplete(Data->ResultCode) ? "true" : "false");

    if (!EOS_EResult_IsOperationComplete(Data->ResultCode)) {
        return;
    }

    std::string result = std::string("Login Result: ") + EOS_EResult_ToString(Data->ResultCode);
    OS_LOG(result.c_str());
    bool bSuccessful = Data->ResultCode == EOS_EResult::EOS_Success;
    if (bSuccessful) {
        LocalUserId = Data->LocalUserId;
        std::string DisplayName = std::string("DisplayName= ") + GetLoggedInDisplayName();
        OS_LOG(DisplayName.c_str());
        CoronaLog("[EOS_DEBUG] native-lib: AuthLoginCb SUCCESS, user=%s", DisplayName.c_str());
    }
    LoginStateChanged(bSuccessful);

    // Queue loginResponse event for dispatch to Lua
    QueueLoginResponseEvent(Data);
}

/** Callback to handle result of attempting a login with stored secure credentials */
void EOS_CALL PersistentAuthLoginCb(const EOS_Auth_LoginCallbackInfo *Data) {
    CoronaLog("[EOS_DEBUG] native-lib: PersistentAuthLoginCb result=%s isComplete=%s",
              EOS_EResult_ToString(Data->ResultCode),
              EOS_EResult_IsOperationComplete(Data->ResultCode) ? "true" : "false");

    if (!EOS_EResult_IsOperationComplete(Data->ResultCode)) {
        return;
    }

    std::string result = std::string(
            "LoginPersistentAuth: Result=") + EOS_EResult_ToString(Data->ResultCode);
    OS_LOG(result.c_str());
    bool bSuccessful = Data->ResultCode == EOS_EResult::EOS_Success;
    if (bSuccessful) {
        LocalUserId = Data->LocalUserId;
        std::string DisplayName = std::string("DisplayName= ") + GetLoggedInDisplayName();
        OS_LOG(DisplayName.c_str());
        CoronaLog("[EOS_DEBUG] native-lib: PersistentAuthLoginCb SUCCESS, user=%s", DisplayName.c_str());
    } else {
        // Check the specific error if we fail to complete a persistent login attempt, as we may need to flush any stored secure credentials
        switch (Data->ResultCode) {
            case EOS_EResult::EOS_Canceled:
            case EOS_EResult::EOS_AlreadyPending:
            case EOS_EResult::EOS_TooManyRequests:
            case EOS_EResult::EOS_TimedOut:
            case EOS_EResult::EOS_ServiceFailure:
            case EOS_EResult::EOS_NotFound:
                OS_LOG("LoginPersistentAuth: Login Failed");
                CoronaLog("[EOS_DEBUG] native-lib: PersistentAuth failed (non-fatal): %s", EOS_EResult_ToString(Data->ResultCode));
                break;
            default:
                OS_LOG("LoginPersistentAuth: Delete persistent auth");
                CoronaLog("[EOS_DEBUG] native-lib: PersistentAuth failed, deleting credentials");
                DeletePersistentAuth();
                break;
        }
    }

    /** Update native UI */
    LoginStateChanged(bSuccessful);

    // Clear the persistent auth in-progress flag
    sPersistentAuthInProgress = false;

    // Only dispatch loginResponse to Lua on SUCCESS.
    // On failure, persistent auth is an internal mechanism - the user hasn't
    // explicitly requested a login, so we should NOT dispatch an error to Lua.
    // The user will later call loginWithAccountPortal() which has its own callback.
    if (bSuccessful) {
        CoronaLog("[EOS_DEBUG] native-lib: PersistentAuth succeeded, dispatching loginResponse to Lua");
        QueueLoginResponseEvent(Data);
        // If loginWithAccountPortal was called while persistent auth was in-flight,
        // the loginResponse we just queued will satisfy it — no need for Chrome.
        if (sPendingAccountPortalLogin) {
            CoronaLog("[EOS_DEBUG] native-lib: Clearing deferred accountPortal login (persistent auth succeeded)");
            sPendingAccountPortalLogin = false;
        }
    } else {
        CoronaLog("[EOS_DEBUG] native-lib: PersistentAuth failed, NOT dispatching error to Lua (internal mechanism)");
        // If loginWithAccountPortal was called while persistent auth was in-flight,
        // persistent auth failed, so now actually start the account portal (Chrome) flow.
        if (sPendingAccountPortalLogin) {
            CoronaLog("[EOS_DEBUG] native-lib: PersistentAuth failed, starting deferred AccountPortal login now");
            sPendingAccountPortalLogin = false;

            EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
            EOS_Auth_Credentials Credentials = {};
            Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
            Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
            Credentials.Id = nullptr;
            Credentials.Token = nullptr;

            EOS_Auth_LoginOptions LoginOptions = {};
            LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
            LoginOptions.Credentials = &Credentials;
            LoginOptions.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile;

            LoginInProgress();
            EOS_Auth_Login(AuthHandle, &LoginOptions, nullptr, AuthLoginCb);
            CoronaLog("[EOS_DEBUG] native-lib: Deferred AccountPortal EOS_Auth_Login initiated");
        }
    }
}

/** Callback to handle result of attempting to delete any secure credentials on the device */
void EOS_CALL AuthDeletePersistentAuthCb(const EOS_Auth_DeletePersistentAuthCallbackInfo *Data) {
    std::string result = std::string("Delete PersistentAuth: Result=") + EOS_EResult_ToString(Data->ResultCode);
    OS_LOG(result.c_str());

    bool bSuccessful = Data->ResultCode == EOS_EResult::EOS_Success;
    if (bSuccessful) {
        LocalUserId = nullptr;
        OS_LOG("Delete successful");
    }
}

/** Callback to handle result of attempting a logout */
void EOS_CALL AuthLogoutCb(const EOS_Auth_LogoutCallbackInfo *Data) {
    bool bSuccessful = Data->ResultCode == EOS_EResult::EOS_Success;
    if (bSuccessful) {
        LocalUserId = nullptr;
        // Release any data returned to us from GetLoggedInDisplayName
        if (LocalUserInfo != nullptr) {
            EOS_UserInfo_Release(LocalUserInfo);
            LocalUserInfo = nullptr;
        }
        // Delete any stored secure credentials, now that we have logged out
        DeletePersistentAuth();
    }
}

/** Delete secure stored credentials on this device */
void DeletePersistentAuth() {
    if (PlatformHandle == nullptr) {
        CoronaLog("[EOS_DEBUG] native-lib: DeletePersistentAuth skipped - no platform handle");
        return;
    }
    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
    EOS_Auth_DeletePersistentAuthOptions DeletePersistentAuthOptions = {};
    DeletePersistentAuthOptions.ApiVersion = EOS_AUTH_DELETEPERSISTENTAUTH_API_LATEST;
    EOS_Auth_DeletePersistentAuth(AuthHandle, &DeletePersistentAuthOptions, nullptr, AuthDeletePersistentAuthCb);
}

// ---------------------------------------------------------------------------------
// enterFrame callback: Ticks EOS SDK and dispatches queued events to Lua
// ---------------------------------------------------------------------------------
static int AndroidEnterFrame(lua_State* L) {
    // Tick EOS platform
    if (PlatformHandle != nullptr) {
        EOS_Platform_Tick(PlatformHandle);
    }

    // Process queued events - move to local queue under lock, then execute without lock
    std::queue<std::shared_ptr<BaseDispatchEventTask>> localQueue;
    {
        std::lock_guard<std::mutex> lock(sEventQueueMutex);
        std::swap(localQueue, sAndroidEventQueue);
    }
    while (!localQueue.empty()) {
        auto task = localQueue.front();
        localQueue.pop();
        if (task) {
            CoronaLog("[EOS_DEBUG] native-lib: enterFrame dispatching event '%s' to Lua", task->GetLuaEventName());
            task->Execute();
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------------
// Helper: Create platform from config.lua settings (mirrors desktop luaopen_plugin_eos)
// ---------------------------------------------------------------------------------
static bool CreatePlatformFromConfig(lua_State* L) {
    if (PlatformHandle != nullptr) {
        CoronaLog("[EOS_DEBUG] native-lib: Platform already created, skipping");
        return true;
    }

    // Read EOS settings from config.lua
    PluginConfigLuaSettings configLuaSettings;
    bool hasConfig = configLuaSettings.LoadFrom(L);
    if (!hasConfig) {
        CoronaLog("[EOS_DEBUG] native-lib: No application.eos table found in config.lua!");
        return false;
    }

    CoronaLog("[EOS_DEBUG] native-lib: Creating platform with config.lua settings");
    CoronaLog("[EOS_DEBUG]   productId=%s", configLuaSettings.GetStringProductId());
    CoronaLog("[EOS_DEBUG]   sandboxId=%s", configLuaSettings.GetStringSandboxId());
    CoronaLog("[EOS_DEBUG]   deploymentId=%s", configLuaSettings.GetStringDeploymentId());
    CoronaLog("[EOS_DEBUG]   clientId=%s", configLuaSettings.GetStringClientId());

    EOS_Platform_Options PlatformOptions{0};
    PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    PlatformOptions.ProductId = configLuaSettings.GetStringProductId();
    PlatformOptions.SandboxId = configLuaSettings.GetStringSandboxId();
    PlatformOptions.DeploymentId = configLuaSettings.GetStringDeploymentId();
    PlatformOptions.ClientCredentials.ClientId = configLuaSettings.GetStringClientId();
    PlatformOptions.ClientCredentials.ClientSecret = configLuaSettings.GetStringClientSecret();
    PlatformOptions.bIsServer = EOS_FALSE;
    PlatformOptions.Flags = 0;

    PlatformHandle = EOS_Platform_Create(&PlatformOptions);
    if (PlatformHandle == nullptr) {
        CoronaLog("ERROR: [EOS SDK] Platform creation failed!");
        return false;
    }

    CoronaLog("[EOS_DEBUG] native-lib: Platform creation successful!");

    // Register for login status changes and attempt persistent auth login
    AddNotifyLoginStatusChanged();
    CoronaLog("[EOS_DEBUG] native-lib: AddNotifyLoginStatusChanged done");
    LoginPersistentAuth();
    CoronaLog("[EOS_DEBUG] native-lib: LoginPersistentAuth initiated");

    return true;
}

// ---------------------------------------------------------------------------------
// Helper: Register enterFrame listener for EOS ticking
// ---------------------------------------------------------------------------------
static bool RegisterEnterFrameListener(lua_State* L) {
    CoronaLog("[EOS_DEBUG] native-lib: Registering enterFrame listener for EOS ticking");

    lua_getglobal(L, "Runtime");
    if (!lua_istable(L, -1)) {
        CoronaLog("ERROR: [EOS SDK] Could not find Runtime global for enterFrame listener");
        lua_pop(L, 1);
        return false;
    }

    lua_getfield(L, -1, "addEventListener");
    if (!lua_isfunction(L, -1)) {
        CoronaLog("ERROR: [EOS SDK] Runtime.addEventListener not found");
        lua_pop(L, 2);
        return false;
    }

    lua_pushvalue(L, -2);                // push Runtime as self
    lua_pushstring(L, "enterFrame");     // push event name
    lua_pushcfunction(L, AndroidEnterFrame); // push callback
    int callResult = CoronaLuaDoCall(L, 3, 0);
    if (callResult != 0) {
        CoronaLog("ERROR: [EOS SDK] Failed to register enterFrame listener, error=%d", callResult);
    } else {
        CoronaLog("[EOS_DEBUG] native-lib: enterFrame listener registered successfully");
    }

    lua_pop(L, 1); // pop Runtime
    return callResult == 0;
}

/** Initialize the EOS SDK for use before we call any other functions, normally during application launching
 *  We supply optional internal/external directory */
extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_nativeInitializeSDK(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj,
        jstring Path) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeInitializeSDK called, IsSDKInitialized=%s", IsSDKInitialized ? "true" : "false");

    lua_State* L = GetLuaStatePointer(env, luaStateObj);

    if (IsSDKInitialized) {
        // SDK previously initialized. Skip.
        OS_LOG("EOS_Initialize already initialized");
        CoronaLog("[EOS_DEBUG] native-lib: SDK already initialized, skipping");
        return true;
    }

    EOS_InitializeOptions SDKOptions = {0};
    SDKOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
    SDKOptions.ProductName = "Coromon";
    SDKOptions.ProductVersion = "1.3.6";

    const char *androidPath = env->GetStringUTFChars(Path, nullptr);
    static EOS_Android_InitializeOptions JNIOptions = {0};
    JNIOptions.ApiVersion = EOS_ANDROID_INITIALIZEOPTIONS_API_LATEST;
    JNIOptions.Reserved = nullptr;
    JNIOptions.OptionalInternalDirectory = androidPath;
    JNIOptions.OptionalExternalDirectory = androidPath;
    SDKOptions.SystemInitializeOptions = &JNIOptions;

    IsSDKInitialized = InitializeSDK(L, SDKOptions);
    if (!IsSDKInitialized) {
        CoronaLog("ERROR: [EOS SDK] InitializeSDK failed!");
        return false;
    }
    CoronaLog("[EOS_DEBUG] native-lib: SDK initialized successfully");

    // Create the Lua event dispatcher (for addEventListener/removeEventListener/dispatchEvent)
    if (!sAndroidLuaEventDispatcher) {
        CoronaLog("[EOS_DEBUG] native-lib: Creating LuaEventDispatcher");
        sAndroidLuaEventDispatcher = std::make_shared<LuaEventDispatcher>(L);
        CoronaLog("[EOS_DEBUG] native-lib: LuaEventDispatcher created");
    }

    // Create the EOS platform using config.lua settings
    CreatePlatformFromConfig(L);

    // Register enterFrame listener for EOS ticking + event dispatch
    RegisterEnterFrameListener(L);

    return true;
}

extern "C" JNIEXPORT jint JNICALL
Java_plugin_eos_LuaLoader_nativeLoadProducts(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeLoadProducts called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnLoadProducts(L);
}
extern "C" JNIEXPORT jint JNICALL
Java_plugin_eos_LuaLoader_nativePurchase(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativePurchase called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnPurchaseProduct(L);
}

extern "C" JNIEXPORT jint JNICALL
Java_plugin_eos_LuaLoader_nativeRestorePurchases(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeRestorePurchases called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnRestorePurchases(L);
}


extern "C" JNIEXPORT jint JNICALL
Java_plugin_eos_LuaLoader_nativeFinishTransaction(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeFinishTransaction called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnFinishTransaction(L);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_nativeIsLoggedOn(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    bool loggedOn = (PlatformHandle != nullptr && LocalUserId != nullptr);
    CoronaLog("[EOS_DEBUG] native-lib: nativeIsLoggedOn = %s (PlatformHandle=%p, LocalUserId=%p)",
              loggedOn ? "true" : "false", PlatformHandle, LocalUserId);
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    lua_pushboolean(L, loggedOn ? 1 : 0);
    return loggedOn;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_nativeLoginWithAccountPortal(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeLoginWithAccountPortal called");

    if (PlatformHandle == nullptr) {
        CoronaLog("ERROR: [EOS SDK] nativeLoginWithAccountPortal - PlatformHandle is null!");
        return false;
    }

    // If already logged in, immediately queue a success loginResponse instead of
    // opening the browser again. This prevents the double-login loop where
    // IAPStore:loadProducts() triggers another login after IAPShopScreen already logged in.
    if (LocalUserId != nullptr) {
        CoronaLog("[EOS_DEBUG] native-lib: Already logged in, skipping AccountPortal login and dispatching success");
        EOS_Auth_LoginCallbackInfo syntheticData = {};
        syntheticData.ResultCode = EOS_EResult::EOS_Success;
        syntheticData.SelectedAccountId = LocalUserId;
        syntheticData.LocalUserId = LocalUserId;
        QueueLoginResponseEvent(&syntheticData);
        CoronaLog("[EOS_DEBUG] native-lib: Queued synthetic success loginResponse");
        return true;
    }

    // If persistent auth is still in progress, defer. When persistent auth completes:
    // - If success: loginResponse will be dispatched automatically (no Chrome needed)
    // - If fail: the account portal login will be started from PersistentAuthLoginCb
    if (sPersistentAuthInProgress) {
        CoronaLog("[EOS_DEBUG] native-lib: PersistentAuth still in progress, deferring AccountPortal login");
        sPendingAccountPortalLogin = true;
        return true;
    }

    // Actually perform the login (persistent auth already failed or was never started)
    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);

    EOS_Auth_Credentials Credentials = {};
    Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
    Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
    Credentials.Id = nullptr;
    Credentials.Token = nullptr;

    EOS_Auth_LoginOptions LoginOptions = {};
    LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
    LoginOptions.Credentials = &Credentials;
    LoginOptions.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile;

    CoronaLog("[EOS_DEBUG] native-lib: Calling EOS_Auth_Login with AccountPortal...");
    LoginInProgress();
    EOS_Auth_Login(AuthHandle, &LoginOptions, nullptr, AuthLoginCb);
    CoronaLog("[EOS_DEBUG] native-lib: EOS_Auth_Login initiated");
    return true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_nativeAddEventListener(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeAddEventListener called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnAddEventListener(L);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_nativeRemoveEventListener(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeRemoveEventListener called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnRemoveEventListener(L);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_nativeLogout(
        JNIEnv *env,
        jobject /* this */,
        jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeLogout called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    if (PlatformHandle == nullptr) {
        CoronaLog("[EOS_DEBUG] native-lib: nativeLogout - no platform handle");
        return false;
    }
    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
    EOS_Auth_LogoutOptions LogoutOptions = {};
    LogoutOptions.ApiVersion = EOS_AUTH_LOGOUT_API_LATEST;
    LogoutOptions.LocalUserId = LocalUserId;
    EOS_Auth_Logout(AuthHandle, &LogoutOptions, nullptr, AuthLogoutCb);
    return true;
}

void AuthLogin(const EOS_Auth_LoginOptions &options, const EOS_Auth_OnLoginCallback delegate) {
    EOS_HAuth handle = EOS_Platform_GetAuthInterface(PlatformHandle);
    LoginInProgress();
    EOS_Auth_Login(handle, &options, nullptr, delegate);
}

/** Attempt a login to the EOS Auth Interface with any previously stored secure credentials (as a result of a previous session calling LoginWithAccountPortal successfully)
 *  If no credential exist then the result EOS_NotFound will be returned to indicate the we still need to login for the first time
 *  If credentials do exist they will be maintained across sessions until we call logout
 *  This should be called after createPlatform and before allowing the user any manual login options */
void LoginPersistentAuth() {
    OS_LOG("Performing Persistent login");
    CoronaLog("[EOS_DEBUG] native-lib: LoginPersistentAuth called");

    sPersistentAuthInProgress = true;

    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);

    EOS_Auth_Credentials Credentials = {};
    Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
    Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_PersistentAuth;
    Credentials.Id = nullptr;
    Credentials.Token = nullptr;

    EOS_Auth_LoginOptions LoginOptions = {};
    LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
    LoginOptions.Credentials = &Credentials;
    AuthLogin(LoginOptions, PersistentAuthLoginCb);
}

/** Register for updates that reflect changes in the users login status for the EOS Auth Interface */
void AddNotifyLoginStatusChanged() {
    if (NotifyLoginStatusChangedId != EOS_INVALID_NOTIFICATIONID) {
        return;
    }

    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
    EOS_Auth_AddNotifyLoginStatusChangedOptions LoginStatusChangedOptions = {0};
    LoginStatusChangedOptions.ApiVersion = EOS_AUTH_ADDNOTIFYLOGINSTATUSCHANGED_API_LATEST;
    NotifyLoginStatusChangedId = EOS_Auth_AddNotifyLoginStatusChanged(AuthHandle, &LoginStatusChangedOptions, nullptr,
                                                                      AuthNotifyLoginStatusChangedCb);
}

/** Shutdown the EOS SDK, normally during application termination
 *  This is also the safest way to release any created platforms we are tracking
 *  NOTE: initializeSDK and shutdownSDK must be called on the main thread */
void ShutdownSDK() {
    // Release any data returned to us from GetLoggedInDisplayName
    if (LocalUserInfo != nullptr) {
        EOS_UserInfo_Release(LocalUserInfo);
        LocalUserInfo = nullptr;
    }

    // Release the event dispatcher
    sAndroidLuaEventDispatcher.reset();

    EOS_Platform_Release(PlatformHandle);
    PlatformHandle = nullptr;

    EOS_Shutdown();
}

/** Unregister for login status updates for the EOS Auth Interface */
void RemoveNotifyLoginStatusChanged() {
    OS_LOG("RemoveNotifyLoginStatusChanged: Unregister");

    if (NotifyLoginStatusChangedId == EOS_INVALID_NOTIFICATIONID) {
        return;
    }

    if (PlatformHandle == nullptr) {
        return;
    }

    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
    EOS_Auth_RemoveNotifyLoginStatusChanged(AuthHandle, NotifyLoginStatusChangedId);
    NotifyLoginStatusChangedId = EOS_INVALID_NOTIFICATIONID;
}

/** Initialize the platform interface using the settings we have obtained from the Developer Portal
 *  This is our hub interface for gaining access to other systems */
extern "C" JNIEXPORT jboolean JNICALL
Java_plugin_eos_LuaLoader_CreatePlatform(
        JNIEnv *env,
        jobject /* this */, jstring ProductID, jstring SandboxID, jstring DeploymentID, jstring ClientID,
        jstring ClientSecret,
        jboolean IsServer, jint Flags) {
    CoronaLog("[EOS_DEBUG] native-lib: CreatePlatform (Java) called");
    if (PlatformHandle != nullptr) {
        // Platform previously created. Skip.
        OS_LOG("EOS Platform already created");
    } else {
        EOS_Platform_Options PlatformOptions{0};

        PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        PlatformOptions.ProductId = env->GetStringUTFChars(ProductID, nullptr);
        PlatformOptions.SandboxId = env->GetStringUTFChars(SandboxID, nullptr);
        PlatformOptions.DeploymentId = env->GetStringUTFChars(DeploymentID, nullptr);
        PlatformOptions.ClientCredentials.ClientId = env->GetStringUTFChars(ClientID, nullptr);
        PlatformOptions.ClientCredentials.ClientSecret = env->GetStringUTFChars(ClientSecret, nullptr);
        PlatformOptions.bIsServer = IsServer ? EOS_TRUE : EOS_FALSE;
        PlatformOptions.Flags = Flags;

        PlatformHandle = EOS_Platform_Create(&PlatformOptions);
        if (PlatformHandle == nullptr) {
            OS_LOG("EOS Platform creation failed");
            return false;
        }

        OS_LOG("EOS Platform creation successful");
    }

    AddNotifyLoginStatusChanged();
    LoginPersistentAuth();
    return true;
}

/** Attempt to logout of the EOS Auth Interface
 *  If any stored secure credentials exist on the device, they will also be removed */
extern "C" JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_Logout(
        JNIEnv *env,
        jobject /* this */) {
    if (PlatformHandle == nullptr) return;
    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);
    EOS_Auth_LogoutOptions LogoutOptions = {};
    LogoutOptions.ApiVersion = EOS_AUTH_LOGOUT_API_LATEST;
    LogoutOptions.LocalUserId = LocalUserId;
    EOS_Auth_Logout(AuthHandle, &LogoutOptions, nullptr, AuthLogoutCb);
}

extern "C"
JNIEXPORT jint JNICALL
Java_plugin_eos_LuaLoader_nativeGetAuthIdToken(JNIEnv *env, jobject thiz, jobject luaStateObj) {
    CoronaLog("[EOS_DEBUG] native-lib: nativeGetAuthIdToken called");
    lua_State* L = GetLuaStatePointer(env, luaStateObj);
    return OnGetAuthIdToken(L);
}


/** Attempt a login to the EOS Auth Interface using the web account portal */
extern "C" JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_LoginWithAccountPortal(
        JNIEnv *env,
        jobject /* this */) {
    CoronaLog("[EOS_DEBUG] native-lib: LoginWithAccountPortal (Java) called");
    if (PlatformHandle == nullptr) {
        CoronaLog("ERROR: [EOS SDK] LoginWithAccountPortal - PlatformHandle is null!");
        return;
    }
    EOS_HAuth AuthHandle = EOS_Platform_GetAuthInterface(PlatformHandle);

    EOS_Auth_Credentials Credentials = {};
    Credentials.ApiVersion = EOS_AUTH_CREDENTIALS_API_LATEST;
    Credentials.Type = EOS_ELoginCredentialType::EOS_LCT_AccountPortal;
    Credentials.Id = nullptr;
    Credentials.Token = nullptr;

    EOS_Auth_LoginOptions LoginOptions = {};
    LoginOptions.ApiVersion = EOS_AUTH_LOGIN_API_LATEST;
    LoginOptions.Credentials = &Credentials;
    LoginOptions.ScopeFlags = EOS_EAuthScopeFlags::EOS_AS_BasicProfile;
    AuthLogin(LoginOptions, AuthLoginCb);
}

/** Tick all active platforms so that they can update and processes any in-flight/incoming HTTP requests or services */
extern "C" JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_Tick(
        JNIEnv *env,
        jobject
        /* this */) {
    if (PlatformHandle != nullptr) {
        EOS_Platform_Tick(PlatformHandle);
    }
}

/** Suspend signals to the SDK that the application status will change to background */
extern "C"
JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_Suspend(JNIEnv *env, jobject thiz) {
    if (PlatformHandle != nullptr) {
        EOS_Platform_SetApplicationStatus(PlatformHandle, EOS_EApplicationStatus::EOS_AS_BackgroundSuspended);
    }
}

/** Resume signals to the SDK that the application status will change to foreground */
extern "C"
JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_Resume(JNIEnv *env, jobject thiz) {
    if (PlatformHandle != nullptr) {
        EOS_Platform_SetApplicationStatus(PlatformHandle, EOS_EApplicationStatus::EOS_AS_Foreground);
    }
}

void UpdateNetwork(EOS_ENetworkStatus status) {
    if (PlatformHandle != nullptr) {
        EOS_Platform_SetNetworkStatus(PlatformHandle, status);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_NetworkChanged(JNIEnv *env, jobject thiz, jboolean connected) {
    UpdateNetwork(connected ? EOS_ENetworkStatus::EOS_NS_Online : EOS_ENetworkStatus::EOS_NS_Disabled);
}

extern "C"
JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_NetworkDisabled(JNIEnv *env, jobject thiz) {
    UpdateNetwork(EOS_ENetworkStatus::EOS_NS_Disabled);
}

/** Store reference to LuaLoader instance */
extern "C"
JNIEXPORT void JNICALL
Java_plugin_eos_LuaLoader_PassLuaLoaderInstance(JNIEnv *env, jobject thiz) {
    GlobalRefLuaLoaderInstance = LocalENV->NewGlobalRef(thiz);
}

/** Called by load.library on Java side
    Stores LuaLoader class for accessing Java methods from JNI */
jint JNI_OnLoad(JavaVM *vm, void *Reserved) {
    if (vm->GetEnv(reinterpret_cast<void **>(&LocalENV), JNI_VERSION_1_6) != JNI_OK) {
        return -1;
    }
    jclass LuaLoader = LocalENV->FindClass("plugin/eos/LuaLoader");
    GlobalRefLuaLoaderClass = reinterpret_cast<jclass>(LocalENV->NewGlobalRef(LuaLoader));
    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM *vm, void *Reserved) {
    RemoveNotifyLoginStatusChanged();
    ShutdownSDK();
}
