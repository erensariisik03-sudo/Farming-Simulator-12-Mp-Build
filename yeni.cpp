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
#include <cmath>

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

// NETWORK PACKET STRUCTURE
#pragma pack(push, 1)
struct NetworkPacket {
    uint8_t type;          
    char senderName[32];   
    char chatData[223];    
};
#pragma pack(pop)

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
// VEHICLE NETWORK STATE TEST
// ========================================================================
// The game stores the vehicle array at Game+0xAC and the vehicle count at
// Game+0xA4. updateStateBase() iterates this exact array and calls Vehicle::update().
// We assign a session-local network ID to each discovered vehicle pointer.

struct VehicleNetState {
    uintptr_t vehiclePtr;
    uint16_t networkId;
    float lastX;
    float lastY;
    float lastAngle;
    bool initialized;
};

static std::vector<VehicleNetState> g_VehicleStates;
static uint16_t g_NextVehicleNetworkId = 0;
static std::mutex g_VehicleStateMutex;

static const uint32_t VEHICLE_SCAN_INTERVAL_MS = 200; // 5 Hz
static const float POSITION_EPSILON = 0.02f;
static const float ANGLE_EPSILON = 0.01f;
static const uint32_t MAX_GAME_VEHICLES = 128;

static uintptr_t GetVehicleAtIndex(uintptr_t game, uint32_t index) {
    if (game == 0) return 0;
    if (index >= MAX_GAME_VEHICLES) return 0;

    // Game vehicle array starts at Game+0xAC.
    return *(uintptr_t*)(game + 0xAC + (index * 4u));
}

static VehicleNetState* FindVehicleStateLocked(uintptr_t vehicle) {
    for (size_t i = 0; i < g_VehicleStates.size(); ++i) {
        if (g_VehicleStates[i].vehiclePtr == vehicle) {
            return &g_VehicleStates[i];
        }
    }
    return nullptr;
}

static VehicleNetState* GetOrCreateVehicleStateLocked(uintptr_t vehicle, uint32_t gameIndex) {
    VehicleNetState* state = FindVehicleStateLocked(vehicle);
    if (state != nullptr) return state;

    VehicleNetState newState;
    newState.vehiclePtr = vehicle;
    newState.networkId = g_NextVehicleNetworkId++;
    newState.lastX = 0.0f;
    newState.lastY = 0.0f;
    newState.lastAngle = 0.0f;
    newState.initialized = false;

    g_VehicleStates.push_back(newState);

    LOGI("[VEHICLE ID] New vehicle: gameIndex=%u networkId=%u vehicle=%p",
         gameIndex,
         (unsigned)newState.networkId,
         (void*)vehicle);

    return &g_VehicleStates.back();
}

static void ScanVehicleStates(uintptr_t game) {
    static std::chrono::steady_clock::time_point lastScan =
        std::chrono::steady_clock::now();

    if (game == 0 || !g_VehicleGetPosition || !g_VehicleGetOrientation) return;

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const uint64_t elapsedMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastScan).count();

    if (elapsedMs < VEHICLE_SCAN_INTERVAL_MS) return;
    lastScan = now;

    uint32_t vehicleCount = *(uint32_t*)(game + 0xA4);
    uint32_t activeVehicleIndex = *(uint32_t*)(game + 0xA8);

    if (vehicleCount > MAX_GAME_VEHICLES) {
        LOGI("[VEHICLE SCAN] Ignoring invalid vehicleCount=%u", vehicleCount);
        vehicleCount = MAX_GAME_VEHICLES;
    }

    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);

    for (uint32_t i = 0; i < vehicleCount; ++i) {
        uintptr_t vehicle = GetVehicleAtIndex(game, i);
        if (vehicle == 0) continue;

        VehicleNetState* state = GetOrCreateVehicleStateLocked(vehicle, i);
        if (state == nullptr) continue;

        float x = 0.0f;
        float y = 0.0f;
        float angle = 0.0f;

        g_VehicleGetPosition((void*)vehicle, &x, &y);
        angle = g_VehicleGetOrientation((void*)vehicle);

        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(angle)) {
            LOGI("[VEHICLE SCAN] Invalid transform: gameIndex=%u networkId=%u vehicle=%p",
                 i, (unsigned)state->networkId, (void*)vehicle);
            continue;
        }

        bool changed = !state->initialized ||
                       std::fabs(x - state->lastX) > POSITION_EPSILON ||
                       std::fabs(y - state->lastY) > POSITION_EPSILON ||
                       std::fabs(angle - state->lastAngle) > ANGLE_EPSILON;

        if (changed) {
            if (!state->initialized) {
                LOGI("[VEHICLE STATE] INIT gameIndex=%u networkId=%u pos=(%.3f, %.3f) angle=%.3f",
                     i, (unsigned)state->networkId, x, y, angle);
            } else {
                LOGI("[VEHICLE STATE] CHANGED gameIndex=%u networkId=%u pos=(%.3f, %.3f) angle=%.3f delta=(%.3f, %.3f, %.3f)%s",
                     i,
                     (unsigned)state->networkId,
                     x,
                     y,
                     angle,
                     x - state->lastX,
                     y - state->lastY,
                     angle - state->lastAngle,
                     (i == activeVehicleIndex) ? " ACTIVE" : "");
            }

            state->lastX = x;
            state->lastY = y;
            state->lastAngle = angle;
            state->initialized = true;
        }
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
    NetworkPacket pkt;
    while (g_IsConnected) {
        int bytesRead = recv(g_TcpSocket, &pkt, sizeof(pkt), 0);
        
        if (bytesRead > 0) {
            if (pkt.type == 2) { 
                std::string senderStr(pkt.senderName);
                std::string msgStr(pkt.chatData);
                std::string formattedMsg = senderStr + ": " + msgStr;
                
                {
                    std::lock_guard<std::mutex> lock(g_ChatMutex);
                    g_ChatMessages.push_back(formattedMsg);
                }
                
                ShowNativeToast(formattedMsg);
            } 
        } 
        else if (bytesRead == 0) {
            ShowNativeToast("Connection Lost (Other player left)!");
            g_IsConnected = false;
            break; 
        }
        else if (bytesRead < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                ShowNativeToast("Error: Network Connection Lost!");
                g_IsConnected = false;
                break;
            }
        }

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

        if (hasOutMsg) {
            NetworkPacket outPkt;
            outPkt.type = 2; 
            strncpy(outPkt.senderName, g_Nickname, 31);
            outPkt.senderName[31] = '\0';
            
            strncpy(outPkt.chatData, outMsg.c_str(), 222);
            outPkt.chatData[222] = '\0';
            
            send(g_TcpSocket, &outPkt, sizeof(outPkt), 0);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); 
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
        g_IsConnected = true;
        g_ConnectedStatus = "Client Connected!";
        ShowNativeToast("Client Joined the Room!"); 
        NetworkLoop(); 
    }
    
    if (g_TcpSocket >= 0) { shutdown(g_TcpSocket, SHUT_RDWR); close(g_TcpSocket); g_TcpSocket = -1; }
    if (g_TcpServerFd >= 0) { shutdown(g_TcpServerFd, SHUT_RDWR); close(g_TcpServerFd); g_TcpServerFd = -1; }
    
    g_IsConnected = false;
    g_IsHost = false;
    if (g_ConnectedStatus != "Room Closed.") g_ConnectedStatus = "Connection Lost.";
}

void TCPClientThread(std::string hostIP) {
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
    if (orig_GameUpdateStateBase) {
        orig_GameUpdateStateBase(thiz, param_1, param_2, param_3, param_4);
    }

    g_EngineInstance = (uintptr_t)thiz;
    ScanVehicleStates(g_EngineInstance);
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

    LOGI("Vehicle state funcs: getPos=%p getOri=%p stateBase=%p",
         (void*)g_VehicleGetPosition,
         (void*)g_VehicleGetOrientation,
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
