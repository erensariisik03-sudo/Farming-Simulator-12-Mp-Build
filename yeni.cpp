#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <android/keycodes.h> 
#include <dlfcn.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <atomic> 
#include <thread>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <cerrno>
#include <math.h>

#include "Substrate.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

// Button Texture Data
#include "buton_texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define LOG_TAG "MultiplayerMod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define DISCOVERY_PORT 8888
#define TCP_SYNC_PORT 8889

// ========================================================================
// GLOBAL VARIABLES
// ========================================================================
JavaVM* g_GlobalJavaVM = nullptr; 

GLuint g_MultiplayerButtonTexture = 0;
bool g_ImGuiInitialized = false;
bool g_TextureLoaded = false;
bool g_IsMultiplayerMenuActive = false;

int g_ButtonOrigWidth = 0;
int g_ButtonOrigHeight = 0;

std::atomic<float> g_TouchX(0.0f);
std::atomic<float> g_TouchY(0.0f);
std::atomic<bool> g_TouchDown(false);

static bool g_IsEatingTouch = false;

// USER NICKNAME
char g_Nickname[32] = "Player"; 

enum ActiveMenuType {
    MENU_NONE = 0,
    MENU_SETTINGS = 1,
    MENU_SAVELOAD = 2,
    MENU_INGAME = 3
};
ActiveMenuType g_CurrentMenu = MENU_NONE;

struct PeerDevice { std::string ip; std::string name; };
std::vector<PeerDevice> g_DiscoveredPeers;
std::mutex g_PeerMutex;
std::atomic<bool> g_IsSearching(false);

// POINTER MANAGEMENT
uintptr_t g_EngineInstance = 0; 
uintptr_t g_MenuInstance = 0;
uintptr_t g_StartMenuInstance = 0;
uintptr_t g_HUDInstance = 0;

std::atomic<bool> g_IsHost(false);
std::atomic<bool> g_IsClient(false);
std::atomic<bool> g_IsConnected(false);

// SOCKETS
int g_TcpServerFd = -1; 
int g_TcpSocket = -1;   
std::string g_ConnectedStatus = "Not Connected";

// CHAT SYSTEM
std::vector<std::string> g_ChatMessages;
std::mutex g_ChatMutex;

std::vector<std::string> g_OutgoingChats;
std::mutex g_OutgoingChatMutex;

// ========================================================================
// JNI_OnLoad
// ========================================================================
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_GlobalJavaVM = vm;
    return JNI_VERSION_1_6;
}

// ========================================================================
// NATIVE ANDROID TOAST NOTIFICATION (JNI)
// ========================================================================
void ShowNativeToast(std::string message) {
    std::thread([message]() {
        if (g_GlobalJavaVM == nullptr) return;
        JNIEnv* env = nullptr;
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) != 0) return;

        jclass looperClass = env->FindClass("android/os/Looper");
        jmethodID myLooper = env->GetStaticMethodID(looperClass, "myLooper", "()Landroid/os/Looper;");
        jobject looper = env->CallStaticObjectMethod(looperClass, myLooper);
        if (looper == nullptr) {
            jmethodID prepare = env->GetStaticMethodID(looperClass, "prepare", "()V");
            env->CallStaticVoidMethod(looperClass, prepare);
        }

        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        jmethodID currentActivityThread = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
        jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThread);
        
        if (activityThread != nullptr) {
            jmethodID getApplication = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
            jobject context = env->CallObjectMethod(activityThread, getApplication);

            if (context != nullptr) {
                jclass toastClass = env->FindClass("android/widget/Toast");
                jmethodID makeText = env->GetStaticMethodID(toastClass, "makeText", "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
                jstring jmsg = env->NewStringUTF(message.c_str());
                jobject toast = env->CallStaticObjectMethod(toastClass, makeText, context, jmsg, 0); 
                
                if (toast != nullptr) {
                    jmethodID show = env->GetMethodID(toastClass, "show", "()V");
                    env->CallVoidMethod(toast, show);
                }
                env->DeleteLocalRef(jmsg);
            }
        }
        
        jmethodID loop = env->GetStaticMethodID(looperClass, "loop", "()V");
        env->CallStaticVoidMethod(looperClass, loop);

        g_GlobalJavaVM->DetachCurrentThread();
    }).detach();
}

// ========================================================================
// HELPER FUNCTIONS
// ========================================================================
uintptr_t GetLibraryBase(const char* libName) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512];
    uintptr_t baseAddress = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libName)) {
            sscanf(line, "%x-%*x", &baseAddress);
            break;
        }
    }
    fclose(fp);
    return baseAddress;
}

void ClearChat() {
    {
        std::lock_guard<std::mutex> lock(g_ChatMutex);
        g_ChatMessages.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
        g_OutgoingChats.clear();
    }
}

void AutoStartGameForClient() {
    g_IsMultiplayerMenuActive = false;
    ShowNativeToast("Switching to game. Please start the game manually.");
}

// ========================================================================
// ORIGINAL FUNCTION POINTERS
// ========================================================================
typedef void* (*renderMenu_t)(void* thiz, void* p1, void* p2, void* p3);
renderMenu_t orig_renderMenu = nullptr;

typedef void* (*renderStartMenuMain_t)(void* thiz, void* p1, void* p2, void* p3);
renderStartMenuMain_t orig_renderStartMenuMain = nullptr;

typedef void* (*updateGUI_t)(void* thiz, void* p1, void* p2, void* p3, void* p4);
updateGUI_t orig_updateGUI = nullptr;

typedef int32_t (*AInputQueue_getEvent_t)(void* queue, AInputEvent** outEvent);
AInputQueue_getEvent_t orig_AInputQueue_getEvent = nullptr;

typedef void (*GameUpdate_t)(void* thiz, float param_1);
GameUpdate_t orig_GameUpdate = nullptr;

typedef void (*GameUpdateStateBase_t)(void* thiz, float param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
GameUpdateStateBase_t orig_GameUpdateStateBase = nullptr;

// ========================================================================
// VEHICLE / MULTIPLAYER SYNCHRONIZATION
// ========================================================================
struct TestB2Vec2 {
    float x;
    float y;
};

typedef void (*VehicleGetPosition_t)(void* vehicle, float* p1, float* p2);
typedef float (*VehicleGetOrientation_t)(void* vehicle);
typedef void (*b2BodySetTransform_t)(void* body, const TestB2Vec2* position, float angle);

VehicleGetPosition_t g_VehicleGetPosition = nullptr;
VehicleGetOrientation_t g_VehicleGetOrientation = nullptr;
b2BodySetTransform_t g_b2BodySetTransform = nullptr;

static const uint8_t PACKET_SESSION_WELCOME = 1;
static const uint8_t PACKET_CHAT = 2;
static const uint8_t PACKET_VEHICLE_POSITION = 3;

static const uint8_t MAX_PLAYERS = 4;
static const uint16_t VEHICLE_ID_INVALID = 0xFFFF;

static const uint32_t VEHICLE_SYNC_INTERVAL_MS = 200;
static const float POSITION_EPSILON = 0.02f;
static const float ANGLE_EPSILON = 0.005f;

#pragma pack(push, 1)

// Server -> client: assigns the client's player ID.
struct SessionWelcomePacket {
    uint8_t type;
    uint8_t ownerId;
    uint8_t maxPlayers;
    uint8_t reserved;
};

// Existing chat packet.
struct NetworkPacket {
    uint8_t type;
    char senderName[32];
    char chatData[223];
};

// Vehicle transform packet.
struct VehiclePositionPacket {
    uint8_t type;
    uint8_t ownerId;
    uint16_t vehicleId;
    float x;
    float y;
    float angle;
    uint32_t sequence;
};

#pragma pack(pop)

static_assert(sizeof(SessionWelcomePacket) == 4, "SessionWelcomePacket size mismatch");
static_assert(sizeof(NetworkPacket) == 256, "NetworkPacket size mismatch");
static_assert(sizeof(VehiclePositionPacket) == 20, "VehiclePositionPacket size mismatch");

static std::atomic<uint8_t> g_LocalPlayerId(0xFF);
static std::atomic<uint32_t> g_LocalVehicleSequence(0);

struct RemoteVehicleState {
    bool valid;
    uint8_t ownerId;
    uint16_t vehicleId;
    float x;
    float y;
    float angle;
    uint32_t sequence;
    uint32_t appliedSequence;
};

static RemoteVehicleState g_RemoteVehicles[MAX_PLAYERS];
static std::mutex g_RemoteVehicleMutex;

static VehiclePositionPacket g_PendingVehiclePacket;
static bool g_HasPendingVehiclePacket = false;
static std::mutex g_VehicleSendMutex;

static bool g_HasLastLocalVehicleState = false;
static uint16_t g_LastLocalVehicleId = VEHICLE_ID_INVALID;
static float g_LastLocalX = 0.0f;
static float g_LastLocalY = 0.0f;
static float g_LastLocalAngle = 0.0f;
static std::chrono::steady_clock::time_point g_LastVehicleSync =
    std::chrono::steady_clock::now();

static uintptr_t GetVehicleFromIndex(uintptr_t game, uint16_t vehicleId) {
    if (game == 0) return 0;

    const uint32_t vehicleCount = *(uint32_t*)(game + 0xA4);
    if (vehicleId >= vehicleCount || vehicleId > 512) return 0;

    const uintptr_t vehicleSlotAddress =
        game + ((uintptr_t)(vehicleId + 0x2A) * 4u) + 4u;

    return *(uintptr_t*)vehicleSlotAddress;
}

static uintptr_t GetActiveVehicleFromGame(uintptr_t game, uint16_t* outVehicleId) {
    if (game == 0) return 0;

    const uint32_t vehicleIndex = *(uint32_t*)(game + 0xA8);
    if (vehicleIndex > 512) return 0;

    if (outVehicleId) {
        *outVehicleId = (uint16_t)vehicleIndex;
    }

    return GetVehicleFromIndex(game, (uint16_t)vehicleIndex);
}

static bool SendAllBytes(int socketFd, const void* data, size_t size) {
    if (socketFd < 0 || data == nullptr || size == 0) return false;

    const uint8_t* bytes = (const uint8_t*)data;
    size_t totalSent = 0;

    while (totalSent < size) {
        ssize_t sent = send(socketFd, bytes + totalSent, size - totalSent, MSG_NOSIGNAL);
        if (sent > 0) {
            totalSent += (size_t)sent;
            continue;
        }

        if (sent < 0 && (errno == EINTR)) {
            continue;
        }

        return false;
    }

    return true;
}

static void QueueVehiclePosition(uint16_t vehicleId, float x, float y, float angle) {
    if (!g_IsConnected.load()) return;

    const uint8_t ownerId = g_LocalPlayerId.load();
    if (ownerId == 0xFF || ownerId >= MAX_PLAYERS) return;

    VehiclePositionPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_VEHICLE_POSITION;
    pkt.ownerId = ownerId;
    pkt.vehicleId = vehicleId;
    pkt.x = x;
    pkt.y = y;
    pkt.angle = angle;
    pkt.sequence = g_LocalVehicleSequence.fetch_add(1) + 1;

    {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
        g_PendingVehiclePacket = pkt;
        g_HasPendingVehiclePacket = true;
    }
}

static void CaptureAndQueueLocalVehicleState(uintptr_t game) {
    if (game == 0 || !g_IsConnected.load()) return;
    if (!g_VehicleGetPosition || !g_VehicleGetOrientation) return;

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const uint64_t elapsedMs =
        (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_LastVehicleSync).count();

    if (elapsedMs < VEHICLE_SYNC_INTERVAL_MS) return;
    g_LastVehicleSync = now;

    uint16_t vehicleId = VEHICLE_ID_INVALID;
    uintptr_t vehicle = GetActiveVehicleFromGame(game, &vehicleId);

    if (vehicle == 0) {
        LOGI("[VEHICLE SYNC] Local active vehicle not found.");
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    float angle = 0.0f;

    g_VehicleGetPosition((void*)vehicle, &x, &y);
    angle = g_VehicleGetOrientation((void*)vehicle);

    const bool vehicleChanged = (!g_HasLastLocalVehicleState ||
                                 vehicleId != g_LastLocalVehicleId);

    const bool positionChanged =
        fabsf(x - g_LastLocalX) > POSITION_EPSILON ||
        fabsf(y - g_LastLocalY) > POSITION_EPSILON;

    const bool angleChanged =
        fabsf(angle - g_LastLocalAngle) > ANGLE_EPSILON;

    if (!vehicleChanged && !positionChanged && !angleChanged) {
        return;
    }

    QueueVehiclePosition(vehicleId, x, y, angle);

    g_HasLastLocalVehicleState = true;
    g_LastLocalVehicleId = vehicleId;
    g_LastLocalX = x;
    g_LastLocalY = y;
    g_LastLocalAngle = angle;

    LOGI("[VEHICLE SEND QUEUED] owner=%u vehicle=%u pos=(%.3f, %.3f) angle=%.3f",
         (unsigned)g_LocalPlayerId.load(),
         (unsigned)vehicleId,
         x, y, angle);
}

static void ApplyRemoteVehicleStates(uintptr_t game) {
    if (game == 0 || !g_b2BodySetTransform) return;
    if (!g_IsConnected.load()) return;

    RemoteVehicleState snapshot[MAX_PLAYERS];
    memset(snapshot, 0, sizeof(snapshot));

    {
        std::lock_guard<std::mutex> lock(g_RemoteVehicleMutex);
        memcpy(snapshot, g_RemoteVehicles, sizeof(snapshot));
    }

    const uint32_t vehicleCount = *(uint32_t*)(game + 0xA4);

    for (uint8_t ownerId = 0; ownerId < MAX_PLAYERS; ++ownerId) {
        RemoteVehicleState& state = snapshot[ownerId];

        if (!state.valid) continue;
        if (state.ownerId == g_LocalPlayerId.load()) continue;
        if (state.vehicleId >= vehicleCount) continue;
        if (state.sequence == state.appliedSequence) continue;

        uintptr_t vehicle = GetVehicleFromIndex(game, state.vehicleId);
        if (vehicle == 0) continue;

        uintptr_t body = *(uintptr_t*)(vehicle + 0x528);
        if (body == 0) continue;

        TestB2Vec2 position;
        position.x = state.x;
        position.y = state.y;

        g_b2BodySetTransform((void*)body, &position, state.angle);

        LOGI("[VEHICLE RECEIVE APPLY] owner=%u vehicle=%u seq=%u pos=(%.3f, %.3f) angle=%.3f",
             (unsigned)state.ownerId,
             (unsigned)state.vehicleId,
             (unsigned)state.sequence,
             state.x,
             state.y,
             state.angle);

        std::lock_guard<std::mutex> lock(g_RemoteVehicleMutex);

        if (g_RemoteVehicles[ownerId].valid &&
            g_RemoteVehicles[ownerId].sequence == state.sequence) {
            g_RemoteVehicles[ownerId].appliedSequence = state.sequence;
        }
    }
}

static void HandleSessionWelcome(const SessionWelcomePacket& pkt) {
    if (pkt.ownerId >= MAX_PLAYERS) {
        LOGI("[NETWORK] Invalid assigned player ID: %u", (unsigned)pkt.ownerId);
        return;
    }

    g_LocalPlayerId.store(pkt.ownerId);

    LOGI("[NETWORK] Assigned local player ID=%u maxPlayers=%u",
         (unsigned)pkt.ownerId,
         (unsigned)pkt.maxPlayers);

    ShowNativeToast("Joined multiplayer as Player " +
                    std::to_string((int)pkt.ownerId + 1));
}

static void HandleVehiclePositionPacket(const VehiclePositionPacket& pkt) {
    if (pkt.ownerId >= MAX_PLAYERS) return;
    if (pkt.ownerId == g_LocalPlayerId.load()) return;

    std::lock_guard<std::mutex> lock(g_RemoteVehicleMutex);

    RemoteVehicleState& state = g_RemoteVehicles[pkt.ownerId];

    // Ignore stale or duplicate packets.
    if (state.valid && pkt.sequence <= state.sequence) return;

    state.valid = true;
    state.ownerId = pkt.ownerId;
    state.vehicleId = pkt.vehicleId;
    state.x = pkt.x;
    state.y = pkt.y;
    state.angle = pkt.angle;
    state.sequence = pkt.sequence;

    LOGI("[VEHICLE RECEIVED] owner=%u vehicle=%u seq=%u pos=(%.3f, %.3f) angle=%.3f",
         (unsigned)pkt.ownerId,
         (unsigned)pkt.vehicleId,
         (unsigned)pkt.sequence,
         pkt.x,
         pkt.y,
         pkt.angle);
}

static void ResetVehicleSyncState() {
    g_LocalPlayerId.store(0xFF);
    g_LocalVehicleSequence.store(0);

    g_HasLastLocalVehicleState = false;
    g_LastLocalVehicleId = VEHICLE_ID_INVALID;
    g_LastLocalX = 0.0f;
    g_LastLocalY = 0.0f;
    g_LastLocalAngle = 0.0f;
    g_LastVehicleSync = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_RemoteVehicleMutex);
        memset(g_RemoteVehicles, 0, sizeof(g_RemoteVehicles));
    }

    {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
        memset(&g_PendingVehiclePacket, 0, sizeof(g_PendingVehiclePacket));
        g_HasPendingVehiclePacket = false;
    }
}

// ========================================================================
// NETWORK FUNCTIONS
// ========================================================================
bool IsNetworkAvailable() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct ifconf ifc;
    char buf[4096];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
        close(sock);
        return false;
    }

    bool hasNetwork = false;
    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it) {
        if (it->ifr_addr.sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)&it->ifr_addr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
            if (strcmp(ip, "127.0.0.1") != 0 && strcmp(ip, "0.0.0.0") != 0) {
                hasNetwork = true;
                break;
            }
        }
    }
    close(sock);
    return hasNetwork;
}

bool IsLocalIP(const std::string& ip) {
    if (ip == "127.0.0.1") return true;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct ifconf ifc;
    char buf[4096];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
        close(sock);
        return false;
    }

    bool isLocal = false;
    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it) {
        if (it->ifr_addr.sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)&it->ifr_addr;
            char localIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, localIp, sizeof(localIp));
            if (ip == localIp) {
                isLocal = true;
                break;
            }
        }
    }
    close(sock);
    return isLocal;
}

void OpenAndroidKeyboard() {
    if (g_GlobalJavaVM == nullptr) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    jint res = g_GlobalJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        }
    }

    if (env != nullptr) {
        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (activityThreadClass != nullptr) {
            jmethodID currentActivityThreadMethod = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
            if (currentActivityThreadMethod != nullptr) {
                jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThreadMethod);
                if (activityThread != nullptr) {
                    jmethodID getApplicationMethod = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
                    jobject context = env->CallObjectMethod(activityThread, getApplicationMethod);

                    if (context != nullptr) {
                        jclass contextClass = env->GetObjectClass(context);
                        jmethodID getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
                        
                        jstring immStr = env->NewStringUTF("input_method");
                        jobject imm = env->CallObjectMethod(context, getSystemServiceMethod, immStr);
                        env->DeleteLocalRef(immStr);

                        if (imm != nullptr) {
                            jclass immClass = env->GetObjectClass(imm);
                            jmethodID toggleSoftInputMethod = env->GetMethodID(immClass, "toggleSoftInput", "(II)V");
                            if (toggleSoftInputMethod != nullptr) {
                                env->CallVoidMethod(imm, toggleSoftInputMethod, 2, 0); 
                            }
                        }
                    }
                }
            }
        }
    }

    if (attached) {
        g_GlobalJavaVM->DetachCurrentThread();
    }
}

void CloseAndroidKeyboard() {
    if (g_GlobalJavaVM == nullptr) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    jint res = g_GlobalJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        }
    }

    if (env != nullptr) {
        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (activityThreadClass != nullptr) {
            jmethodID currentActivityThreadMethod = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
            if (currentActivityThreadMethod != nullptr) {
                jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThreadMethod);
                if (activityThread != nullptr) {
                    jmethodID getApplicationMethod = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
                    jobject context = env->CallObjectMethod(activityThread, getApplicationMethod);

                    if (context != nullptr) {
                        jclass contextClass = env->GetObjectClass(context);
                        jmethodID getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
                        
                        jstring immStr = env->NewStringUTF("input_method");
                        jobject imm = env->CallObjectMethod(context, getSystemServiceMethod, immStr);
                        env->DeleteLocalRef(immStr);

                        if (imm != nullptr) {
                            jclass immClass = env->GetObjectClass(imm);
                            jmethodID toggleSoftInputMethod = env->GetMethodID(immClass, "toggleSoftInput", "(II)V");
                            if (toggleSoftInputMethod != nullptr) {
                                env->CallVoidMethod(imm, toggleSoftInputMethod, 0, 0); 
                            }
                        }
                    }
                }
            }
        }
    }

    if (attached) {
        g_GlobalJavaVM->DetachCurrentThread();
    }
}

GLuint LoadTextureFromPNGArray(const unsigned char* png_data, int data_len) {
    int channels;
    unsigned char* pixels = stbi_load_from_memory(png_data, data_len, &g_ButtonOrigWidth, &g_ButtonOrigHeight, &channels, 4);
    if (!pixels) return 0;
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_ButtonOrigWidth, g_ButtonOrigHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels); 
    return textureID;
}

// ========================================================================
// NETWORK THREADS
// ========================================================================
void NetworkLoop() {
    fcntl(g_TcpSocket, F_SETFL, O_NONBLOCK);

    uint8_t recvBuffer[4096];
    size_t bufferedBytes = 0;

    while (g_IsConnected.load()) {
        if (g_TcpSocket < 0) break;

        if (bufferedBytes < sizeof(recvBuffer)) {
            ssize_t bytesRead = recv(
                g_TcpSocket,
                recvBuffer + bufferedBytes,
                sizeof(recvBuffer) - bufferedBytes,
                0
            );

            if (bytesRead > 0) {
                bufferedBytes += (size_t)bytesRead;
            } else if (bytesRead == 0) {
                ShowNativeToast("Connection Lost (Other player left)!");
                g_IsConnected.store(false);
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
                ShowNativeToast("Error: Network Connection Lost!");
                g_IsConnected.store(false);
                break;
            }
        }

        // Parse the TCP byte stream into complete packets.
        while (bufferedBytes > 0) {
            const uint8_t packetType = recvBuffer[0];
            size_t packetSize = 0;

            if (packetType == PACKET_SESSION_WELCOME) {
                packetSize = sizeof(SessionWelcomePacket);
            } else if (packetType == PACKET_CHAT) {
                packetSize = sizeof(NetworkPacket);
            } else if (packetType == PACKET_VEHICLE_POSITION) {
                packetSize = sizeof(VehiclePositionPacket);
            } else {
                LOGI("[NETWORK] Unknown packet type=%u", (unsigned)packetType);
                g_IsConnected.store(false);
                break;
            }

            if (bufferedBytes < packetSize) break;

            if (packetType == PACKET_SESSION_WELCOME) {
                SessionWelcomePacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleSessionWelcome(pkt);
            } else if (packetType == PACKET_CHAT) {
                NetworkPacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));

                pkt.senderName[sizeof(pkt.senderName) - 1] = '\0';
                pkt.chatData[sizeof(pkt.chatData) - 1] = '\0';

                std::string senderStr(pkt.senderName);
                std::string msgStr(pkt.chatData);
                std::string formattedMsg = senderStr + ": " + msgStr;

                {
                    std::lock_guard<std::mutex> lock(g_ChatMutex);
                    g_ChatMessages.push_back(formattedMsg);
                }

                ShowNativeToast(formattedMsg);
            } else if (packetType == PACKET_VEHICLE_POSITION) {
                VehiclePositionPacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleVehiclePositionPacket(pkt);
            }

            bufferedBytes -= packetSize;

            if (bufferedBytes > 0) {
                memmove(recvBuffer, recvBuffer + packetSize, bufferedBytes);
            }
        }

        // Send queued chat packet.
        std::string outMsg;
        bool hasOutMsg = false;

        {
            std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
            if (!g_OutgoingChats.empty()) {
                outMsg = g_OutgoingChats.front();
                g_OutgoingChats.erase(g_OutgoingChats.begin());
                hasOutMsg = true;
            }
        }

        if (hasOutMsg && g_IsConnected.load()) {
            NetworkPacket outPkt;
            memset(&outPkt, 0, sizeof(outPkt));
            outPkt.type = PACKET_CHAT;

            strncpy(outPkt.senderName, g_Nickname, sizeof(outPkt.senderName) - 1);
            strncpy(outPkt.chatData, outMsg.c_str(), sizeof(outPkt.chatData) - 1);

            if (!SendAllBytes(g_TcpSocket, &outPkt, sizeof(outPkt))) {
                ShowNativeToast("Error: Failed to send chat.");
                g_IsConnected.store(false);
                break;
            }
        }

        // Send only the newest vehicle state. Older states are not useful.
        VehiclePositionPacket vehiclePkt;
        bool hasVehiclePkt = false;

        {
            std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
            if (g_HasPendingVehiclePacket) {
                vehiclePkt = g_PendingVehiclePacket;
                g_HasPendingVehiclePacket = false;
                hasVehiclePkt = true;
            }
        }

        if (hasVehiclePkt && g_IsConnected.load()) {
            if (!SendAllBytes(g_TcpSocket, &vehiclePkt, sizeof(vehiclePkt))) {
                ShowNativeToast("Error: Failed to send vehicle data.");
                g_IsConnected.store(false);
                break;
            }

            LOGI("[VEHICLE SENT] owner=%u vehicle=%u seq=%u pos=(%.3f, %.3f) angle=%.3f",
                 (unsigned)vehiclePkt.ownerId,
                 (unsigned)vehiclePkt.vehicleId,
                 (unsigned)vehiclePkt.sequence,
                 vehiclePkt.x,
                 vehiclePkt.y,
                 vehiclePkt.angle);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}


void StartPONGResponderThread() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in server_addr, client_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DISCOVERY_PORT);
    bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    char buffer[256];
    socklen_t client_len = sizeof(client_addr);
    while (true) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&client_addr, &client_len);
        if (n > 0) {
            buffer[n] = '\0';
            if (strcmp(buffer, "FS14_PING") == 0) {
                if (g_IsHost.load()) {
                    std::string roomName = std::string(g_Nickname) + "'s Room";
                    std::string reply = "FS14_PONG|" + roomName;
                    sendto(sockfd, reply.c_str(), reply.length(), 0, (struct sockaddr*)&client_addr, client_len);
                }
            }
        }
    }
}

void StartLANDiscoveryThread() {
    if (!IsNetworkAvailable()) {
        ShowNativeToast("Error: Check your network connection!");
        g_IsSearching.store(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_PeerMutex);
        g_DiscoveredPeers.clear();
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    
    struct sockaddr_in broadcast_addr;
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcast_addr.sin_port = htons(DISCOVERY_PORT);
    
    std::string pingMsg = "FS14_PING";
    sendto(sockfd, pingMsg.c_str(), pingMsg.length(), 0, (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    
    struct timeval tv;
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    char buffer[256];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < 3) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&sender_addr, &sender_len);
        if (n > 0) {
            buffer[n] = '\0';
            std::string msg(buffer);
            if (msg.find("FS14_PONG|") == 0) {
                std::string roomName = msg.substr(10); 
                std::string devIP = inet_ntoa(sender_addr.sin_addr);
                
                if (IsLocalIP(devIP)) {
                    continue;
                }

                std::lock_guard<std::mutex> lock(g_PeerMutex);
                bool exists = false;
                for (auto& peer : g_DiscoveredPeers) { if (peer.ip == devIP) { exists = true; break; } }
                if (!exists) g_DiscoveredPeers.push_back({devIP, roomName});
            }
        }
    }
    close(sockfd);
    g_IsSearching.store(false);
}

void TCPHostThread() {
    ResetVehicleSyncState();
    g_LocalPlayerId.store(0);

    if (!IsNetworkAvailable()) {
        ShowNativeToast("Error: Check your network connection!");
        g_ConnectedStatus = "Connection Error (No Network)";
        g_IsHost = false;
        return;
    }

    g_TcpServerFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_TcpServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_SYNC_PORT);
    bind(g_TcpServerFd, (struct sockaddr*)&address, sizeof(address));
    listen(g_TcpServerFd, 3);
    
    g_ConnectedStatus = "Host Started. Waiting for client...";
    ShowNativeToast("Room Created. Waiting for client...");
    int addrlen = sizeof(address);
    
    g_TcpSocket = accept(g_TcpServerFd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
    
    if (g_TcpSocket >= 0 && g_IsHost) {
        SessionWelcomePacket welcome;
        memset(&welcome, 0, sizeof(welcome));
        welcome.type = PACKET_SESSION_WELCOME;
        welcome.ownerId = 1;
        welcome.maxPlayers = MAX_PLAYERS;

        if (!SendAllBytes(g_TcpSocket, &welcome, sizeof(welcome))) {
            ShowNativeToast("Error: Failed to initialize multiplayer session.");
            g_ConnectedStatus = "Connection Error";
        } else {
            g_IsConnected = true;
            g_ConnectedStatus = "Client Connected!";
            ShowNativeToast("Client Joined the Room!");
            NetworkLoop();
        }
    }
    
    if (g_TcpSocket >= 0) { shutdown(g_TcpSocket, SHUT_RDWR); close(g_TcpSocket); g_TcpSocket = -1; }
    if (g_TcpServerFd >= 0) { shutdown(g_TcpServerFd, SHUT_RDWR); close(g_TcpServerFd); g_TcpServerFd = -1; }
    
    g_IsConnected = false;
    g_IsHost = false;
    ResetVehicleSyncState();
    if (g_ConnectedStatus != "Room Closed.") g_ConnectedStatus = "Connection Lost.";
}

void TCPClientThread(std::string hostIP) {
    ResetVehicleSyncState();
    g_LocalPlayerId.store(0xFF);

    g_TcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TCP_SYNC_PORT);
    
    if (inet_pton(AF_INET, hostIP.c_str(), &serv_addr.sin_addr) <= 0) {
        ShowNativeToast("Error: Invalid IP!");
        g_IsClient = false;
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(g_TcpSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    g_ConnectedStatus = "Connecting to room...";
    ShowNativeToast("Connecting to room: " + hostIP);

    if (connect(g_TcpSocket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) >= 0) {
        g_IsConnected = true;
        g_ConnectedStatus = "Connected to Host!";
        ShowNativeToast("Successfully Joined Room!"); 
        NetworkLoop(); 
    } else {
        ShowNativeToast("Error: Could not connect to room!");
        g_ConnectedStatus = "Connection Failed";
    }
    
    if (g_TcpSocket >= 0) { shutdown(g_TcpSocket, SHUT_RDWR); close(g_TcpSocket); g_TcpSocket = -1; }
    g_IsConnected = false;
    g_IsClient = false;
    ResetVehicleSyncState();
}

// ========================================================================
// HOOKS AND RENDERING
// ========================================================================
int32_t my_AInputQueue_getEvent(void* queue, AInputEvent** outEvent) {
    int32_t result;
    while (true) {
        result = orig_AInputQueue_getEvent(queue, outEvent);
        if (result >= 0 && outEvent != nullptr && *outEvent != nullptr) {
            int32_t eventType = AInputEvent_getType(*outEvent);

            if (eventType == AINPUT_EVENT_TYPE_MOTION) {
                int32_t action = AMotionEvent_getAction(*outEvent) & AMOTION_EVENT_ACTION_MASK;
                
                if (action == AMOTION_EVENT_ACTION_CANCEL) {
                    g_TouchDown.store(false);
                    g_IsEatingTouch = false;
                    void* finishEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_finishEvent");
                    if (finishEventAddr) {
                        typedef void (*finish_t)(void*, AInputEvent*, int);
                        ((finish_t)finishEventAddr)(queue, *outEvent, 1);
                    }
                    continue; 
                }
                
                size_t pointerCount = AMotionEvent_getPointerCount(*outEvent); 
                if (pointerCount > 0) {
                    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE) {
                        g_TouchX.store(AMotionEvent_getX(*outEvent, 0));
                        g_TouchY.store(AMotionEvent_getY(*outEvent, 0));
                        g_TouchDown.store(true);
                    } else if (action == AMOTION_EVENT_ACTION_UP) {
                        g_TouchDown.store(false);
                    }
                }

                bool shouldEatEvent = false;
                if (g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (action == AMOTION_EVENT_ACTION_DOWN) {
                        g_IsEatingTouch = io.WantCaptureMouse;
                    }
                    if (g_IsEatingTouch || io.WantCaptureMouse) {
                        shouldEatEvent = true;
                    }
                    if (action == AMOTION_EVENT_ACTION_UP) {
                        if (g_IsEatingTouch) shouldEatEvent = true;
                        g_IsEatingTouch = false; 
                    }
                }

                if (shouldEatEvent) {
                    void* finishEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_finishEvent");
                    if (finishEventAddr) {
                        typedef void (*finish_t)(void*, AInputEvent*, int);
                        ((finish_t)finishEventAddr)(queue, *outEvent, 1);
                    }
                    continue; 
                }
            }
            else if (eventType == AINPUT_EVENT_TYPE_KEY) {
                int32_t action = AKeyEvent_getAction(*outEvent);
                int32_t keyCode = AKeyEvent_getKeyCode(*outEvent);
                int32_t metaState = AKeyEvent_getMetaState(*outEvent);

                bool shouldEatEvent = false;

                if (g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    ImGuiIO& io = ImGui::GetIO();
                    bool isDown = (action == AKEY_EVENT_ACTION_DOWN);

                    if (keyCode == AKEYCODE_DEL) {
                        io.AddKeyEvent(ImGuiKey_Backspace, isDown);
                    } else if (keyCode == AKEYCODE_ENTER || keyCode == AKEYCODE_NUMPAD_ENTER) {
                        io.AddKeyEvent(ImGuiKey_Enter, isDown);
                    } else if (keyCode == AKEYCODE_DPAD_LEFT) {
                        io.AddKeyEvent(ImGuiKey_LeftArrow, isDown);
                    } else if (keyCode == AKEYCODE_DPAD_RIGHT) {
                        io.AddKeyEvent(ImGuiKey_RightArrow, isDown);
                    }

                    if (isDown) {
                        bool isShift = (metaState & AMETA_SHIFT_ON) != 0;
                        char c = 0;

                        if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z) {
                            c = (isShift ? 'A' : 'a') + (keyCode - AKEYCODE_A);
                        } else if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9) {
                            c = '0' + (keyCode - AKEYCODE_0);
                        } else if (keyCode == AKEYCODE_SPACE) { c = ' '; }
                          else if (keyCode == AKEYCODE_PERIOD) { c = '.'; }
                          else if (keyCode == AKEYCODE_COMMA) { c = ','; }
                          else if (keyCode == AKEYCODE_MINUS) { c = (isShift ? '_' : '-'); }
                          else if (keyCode == AKEYCODE_EQUALS) { c = (isShift ? '+' : '='); }
                          else if (keyCode == AKEYCODE_SLASH) { c = (isShift ? '?' : '/'); }
                        
                        if (c != 0) {
                            io.AddInputCharacter(c);
                        }
                    }

                    if (io.WantCaptureKeyboard) {
                        shouldEatEvent = true;
                    }
                }

                if (shouldEatEvent) {
                    void* finishEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_finishEvent");
                    if (finishEventAddr) {
                        typedef void (*finish_t)(void*, AInputEvent*, int);
                        ((finish_t)finishEventAddr)(queue, *outEvent, 1);
                    }
                    continue; 
                }
            }
        }
        break; 
    }
    return result; 
}

void DrawImGui() {
    if (!g_ImGuiInitialized) {
        ImGui::CreateContext();
        ImGui_ImplOpenGL3_Init("#version 100");
        ImGui::StyleColorsDark();
        g_ImGuiInitialized = true;
    }

    if (g_ImGuiInitialized) {
        ImGuiIO& io = ImGui::GetIO();
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        io.DisplaySize = ImVec2((float)viewport[2], (float)viewport[3]);
        io.DeltaTime = 1.0f / 60.0f;
        
        float uiScale = (float)viewport[3] / 400.0f; 
        if (uiScale < 1.4f) uiScale = 1.4f;
        if (uiScale > 2.5f) uiScale = 2.5f;
        io.FontGlobalScale = uiScale;

        io.AddMousePosEvent(g_TouchX.load(), g_TouchY.load());
        io.AddMouseButtonEvent(0, g_TouchDown.load());

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // MENU TOGGLE BUTTON
        if (g_CurrentMenu != MENU_INGAME && g_MultiplayerButtonTexture != 0) {
            float posX = 0.0f, posY = 0.0f;
            float targetWidth = 100.0f, targetHeight = 50.0f;

            if (g_CurrentMenu == MENU_SETTINGS) {
                posY = (float)viewport[3] * 0.010f; 
                targetWidth = (float)viewport[2] * 0.335f; 
                targetHeight = (float)viewport[3] * 0.2f; 
            } else if (g_CurrentMenu == MENU_SAVELOAD) {
                posX = (float)viewport[2] * 0.005f; 
                posY = (float)viewport[3] * 0.010f; 
                targetWidth = (float)viewport[2] * 0.220f; 
                targetHeight = (float)viewport[3] * 0.14f; 
            }

            ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always); 
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Mod Menu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings); 
            
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));       
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); 
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));  

            if (ImGui::ImageButton("MP_BTN", (ImTextureID)(intptr_t)g_MultiplayerButtonTexture, ImVec2(targetWidth, targetHeight))) {
                g_IsMultiplayerMenuActive = !g_IsMultiplayerMenuActive;
            }

            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            ImGui::End();
        }

        if (g_IsMultiplayerMenuActive) {
            float winWidth = (float)viewport[2] * 0.82f;
            float winHeight = (float)viewport[3] * 0.88f;
            
            ImGui::SetNextWindowPos(ImVec2(((float)viewport[2] - winWidth) * 0.5f, ((float)viewport[3] - winHeight) * 0.5f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(winWidth, winHeight), ImGuiCond_Always);
            
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 12.0f));

            auto RenderChatUI = [&]() {
                ImGui::Separator();
                ImGui::Text("Chat Panel:");
                
                float chatBoxHeight = winHeight * 0.33f;
                ImGui::BeginChild("ChatHistory", ImVec2(-1.0f, chatBoxHeight), true);
                {
                    std::lock_guard<std::mutex> lock(g_ChatMutex);
                    for (const auto& msg : g_ChatMessages) {
                        ImGui::TextWrapped("%s", msg.c_str());
                    }
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                        ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();

                static char inputBuffer[200] = "";
                float sendBtnWidth = 110.0f * (uiScale / 1.5f);
                float inputWidth = ImGui::GetContentRegionAvail().x - sendBtnWidth - 10.0f;

                ImGui::SetNextItemWidth(inputWidth);
                bool enterPressed = ImGui::InputText("##ChatInput", inputBuffer, IM_ARRAYSIZE(inputBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
                
                if (ImGui::IsItemClicked()) {
                    OpenAndroidKeyboard();
                }

                ImGui::SameLine();
                if (ImGui::Button("Send", ImVec2(sendBtnWidth, 42.0f * (uiScale / 1.5f))) || enterPressed) {
                    if (strlen(inputBuffer) > 0) {
                        std::string msgStr(inputBuffer);
                        {
                            std::lock_guard<std::mutex> lock(g_ChatMutex);
                            g_ChatMessages.push_back("You: " + msgStr);
                        }
                        {
                            std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
                            g_OutgoingChats.push_back(msgStr);
                        }
                        memset(inputBuffer, 0, sizeof(inputBuffer)); 
                    }
                    CloseAndroidKeyboard(); 
                }
            };
            
            // MENU_SETTINGS (HOST MENU)
            if (g_CurrentMenu == MENU_SETTINGS) {
                ImGui::Begin("Settings Menu", &g_IsMultiplayerMenuActive, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
                
                if (g_IsClient && g_IsConnected) {
                    ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "Network Status: %s", g_ConnectedStatus.c_str());
                    ImGui::Separator();
                    
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.1f, 0.1f, 1.0f)); 
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.05f, 0.05f, 1.0f));
                    
                    if (ImGui::Button("Leave Room", ImVec2(-1.0f, 60.0f * (uiScale / 1.5f)))) {
                        ClearChat(); 
                        g_IsClient = false; 
                        g_IsConnected = false;
                        if (g_TcpSocket >= 0) {
                            shutdown(g_TcpSocket, SHUT_RDWR);
                            close(g_TcpSocket);
                            g_TcpSocket = -1;
                        }
                        g_ConnectedStatus = "Left the room.";
                    }
                    ImGui::PopStyleColor(3);

                    RenderChatUI();
                } else {
                    ImGui::Text("Game Status: %s", (g_StartMenuInstance != 0) ? "Game Hooked" : "Waiting for pointer...");
                    ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "Network Status: %s", g_ConnectedStatus.c_str());
                    ImGui::Separator();

                    if (!g_IsHost) {
                        if (ImGui::Button("Host Room", ImVec2(-1.0f, 80.0f * (uiScale / 1.5f)))) {
                            ClearChat(); 
                            g_IsHost = true;
                            std::thread(TCPHostThread).detach();
                        }
                    } else {
                        if (ImGui::Button("Close Room", ImVec2(-1.0f, 80.0f * (uiScale / 1.5f)))) {
                            ClearChat(); 
                            g_IsHost = false; 
                            g_IsConnected = false;
                            
                            if (g_TcpServerFd >= 0) {
                                shutdown(g_TcpServerFd, SHUT_RDWR); 
                                close(g_TcpServerFd);
                                g_TcpServerFd = -1;
                            }
                            if (g_TcpSocket >= 0) {
                                shutdown(g_TcpSocket, SHUT_RDWR);
                                close(g_TcpSocket);
                                g_TcpSocket = -1;
                            }
                            
                            g_ConnectedStatus = "Room Closed.";
                        }
                    }

                    if (g_IsConnected) {
                        RenderChatUI();
                    }
                }
                ImGui::End();
            }
            // MENU_SAVELOAD (CLIENT JOIN MENU)
            else if (g_CurrentMenu == MENU_SAVELOAD) {
                ImGui::Begin("Server Browser (Client)", &g_IsMultiplayerMenuActive, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
                ImGui::Text("Game Status: %s", (g_MenuInstance != 0) ? "Game Hooked" : "Waiting for pointer...");
                ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "Network Status: %s", g_ConnectedStatus.c_str());
                ImGui::Separator();

                if (!g_IsConnected) {
                    ImGui::Text("Username:");
                    ImGui::SetNextItemWidth(-1.0f);
                    
                    bool nickEnterPressed = ImGui::InputText("##NicknameInput", g_Nickname, IM_ARRAYSIZE(g_Nickname), ImGuiInputTextFlags_EnterReturnsTrue);
                    if (ImGui::IsItemClicked()) {
                        OpenAndroidKeyboard();
                    }
                    if (nickEnterPressed) {
                        CloseAndroidKeyboard(); 
                    }
                    
                    ImGui::Spacing();

                    if (ImGui::Button(g_IsSearching.load() ? "Searching..." : "Scan Networks", ImVec2(-1.0f, 50.0f * (uiScale / 1.5f)))) {
                        if (!g_IsSearching.load()) {
                            g_IsSearching.store(true);
                            std::thread(StartLANDiscoveryThread).detach();
                        }
                    }
                    
                    ImGui::Separator();
                    ImGui::Text("Discovered Rooms (Tap to join):");
                    
                    {
                        std::lock_guard<std::mutex> lock(g_PeerMutex);
                        if (g_DiscoveredPeers.empty()) {
                            ImGui::TextDisabled("No active rooms found yet.");
                        } else {
                            for (size_t i = 0; i < g_DiscoveredPeers.size(); i++) {
                                std::string label = g_DiscoveredPeers[i].name + " [" + g_DiscoveredPeers[i].ip + "]"; 
                                if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 48.0f * (uiScale / 1.5f)))) {
                                    if (!g_IsHost.load() && !g_IsClient.load()) {
                                        ClearChat(); 
                                        g_IsClient.store(true);
                                        std::string targetIP = g_DiscoveredPeers[i].ip;
                                        std::thread(TCPClientThread, targetIP).detach();
                                    }
                                }
                            }
                        }
                    }
                } else {
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.6f, 0.1f, 1.0f)); 
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.05f, 0.5f, 0.05f, 1.0f));
                    if (ImGui::Button("JOIN GAME (START NOW)", ImVec2(-1.0f, 60.0f * (uiScale / 1.5f)))) {
                        AutoStartGameForClient();
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::Spacing();

                    if (ImGui::Button("Disconnect", ImVec2(-1.0f, 40.0f * (uiScale / 1.5f)))) {
                        ClearChat(); 
                        g_IsHost = false; g_IsClient = false; g_IsConnected = false;
                        if (g_TcpSocket >= 0) {
                            shutdown(g_TcpSocket, SHUT_RDWR);
                            close(g_TcpSocket);
                            g_TcpSocket = -1;
                        }
                    }
                    
                    RenderChatUI();
                }
                ImGui::End();
            }
            ImGui::PopStyleVar(2);
        }

        ImGui::Render();
        
        // SAVE AND RESTORE OPENGL STATE
        GLboolean depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
        GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        if (depthTestEnabled) glEnable(GL_DEPTH_TEST);
        if (cullFaceEnabled) glEnable(GL_CULL_FACE);
    }
}

// ========================================================================
// RENDER HOOKS
// ========================================================================
void my_GameUpdate(void* thiz, float param_1) {
    g_EngineInstance = (uintptr_t)thiz; 
    g_CurrentMenu = MENU_INGAME; 
    if (orig_GameUpdate) orig_GameUpdate(thiz, param_1);
}

void my_GameUpdateStateBase(void* thiz, float param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    g_EngineInstance = (uintptr_t)thiz;

    if (orig_GameUpdateStateBase) {
        orig_GameUpdateStateBase(thiz, param_1, param_2, param_3, param_4);
    }

    ApplyRemoteVehicleStates(g_EngineInstance);
    CaptureAndQueueLocalVehicleState(g_EngineInstance);
}

void* my_updateGUI(void* thiz, void* p1, void* p2, void* p3, void* p4) {
    g_HUDInstance = (uintptr_t)thiz;

    if (!g_TextureLoaded) {
        g_MultiplayerButtonTexture = LoadTextureFromPNGArray(buton_png_data, buton_png_len);
        g_TextureLoaded = true;
    }
    return orig_updateGUI ? orig_updateGUI(thiz, p1, p2, p3, p4) : nullptr;
}

void* my_renderMenu(void* thiz, void* p1, void* p2, void* p3) {
    g_MenuInstance = (uintptr_t)thiz;

    void* ret = nullptr;
    if (orig_renderMenu) ret = orig_renderMenu(thiz, p1, p2, p3);
    g_CurrentMenu = MENU_SAVELOAD;
    DrawImGui();
    return ret; 
}

void* my_renderStartMenuMain(void* thiz, void* p1, void* p2, void* p3) {
    g_StartMenuInstance = (uintptr_t)thiz;

    void* ret = nullptr;
    if (orig_renderStartMenuMain) ret = orig_renderStartMenuMain(thiz, p1, p2, p3);
    g_CurrentMenu = MENU_SETTINGS;
    DrawImGui();
    return ret; 
}

// ========================================================================
// MAIN MOD INITIALIZER
// ========================================================================
__attribute__((constructor))
void ModMain() {
    LOGI(">>> MULTIPLAYER MOD STARTING <<<");

    ResetVehicleSyncState();
    std::thread(StartPONGResponderThread).detach();

    uintptr_t libBase = GetLibraryBase("libapp.so");
    if (libBase == 0) return;
    
    uintptr_t renderMenuAddr = libBase + 0x00033974 + 1; 
    uintptr_t updateGUIAddr  = libBase + 0x0002f6a0 + 1; 
    uintptr_t gameUpdateAddr = libBase + 0x00047ee8 + 1;
    uintptr_t updateStateBaseAddr = libBase + 0x00046748 + 1;
    uintptr_t inGameMenuAddr = libBase + 0x00032090 + 1;

    g_VehicleGetPosition = (VehicleGetPosition_t)(libBase + 0x000397da + 1);
    g_VehicleGetOrientation = (VehicleGetOrientation_t)(libBase + 0x000397ea + 1);
    g_b2BodySetTransform = (b2BodySetTransform_t)(libBase + 0x00060a0c + 1);

    LOGI("Multiplayer vehicle funcs: getPos=%p getOri=%p setTransform=%p stateBase=%p",
         (void*)g_VehicleGetPosition,
         (void*)g_VehicleGetOrientation,
         (void*)g_b2BodySetTransform,
         (void*)updateStateBaseAddr);
    
    MSHookFunction((void*)renderMenuAddr, (void*)my_renderMenu, (void**)&orig_renderMenu);
    MSHookFunction((void*)updateGUIAddr, (void*)my_updateGUI, (void**)&orig_updateGUI);
    MSHookFunction((void*)gameUpdateAddr, (void*)my_GameUpdate, (void**)&orig_GameUpdate);
    MSHookFunction((void*)updateStateBaseAddr, (void*)my_GameUpdateStateBase, (void**)&orig_GameUpdateStateBase);
    MSHookFunction((void*)inGameMenuAddr, (void*)my_renderStartMenuMain, (void**)&orig_renderStartMenuMain);

    void* inputQueueGetEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_getEvent");
    if (inputQueueGetEventAddr != nullptr) {
        MSHookFunction(inputQueueGetEventAddr, (void*)my_AInputQueue_getEvent, (void**)&orig_AInputQueue_getEvent);
    }
}
