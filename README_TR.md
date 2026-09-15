# Farming Simulator 12 Multiplayer Mod

Bu proje, **Farming Simulator 12 (FS12) Android sürümüne** sonradan multiplayer desteği kazandırmak amacıyla geliştirilmiş native bir Android modudur.

Proje, oyunun kendi APK dosyasını doğrudan değiştirmek yerine Android tarafında çalışan native bir **`.so` kütüphanesi** üzerinden oyunun çalışma sırasında belirli fonksiyonlarına müdahale eder. Modun temel amacı, tek oyunculu çalışan oyunun belirli menü ve oyun fonksiyonlarını hook ederek ağ üzerinden oyuncular arasında veri alışverişi yapılabilecek bir multiplayer altyapısı oluşturmaktır.

> **Önemli:** Bu proje özellikle eski Android cihazlar ve eski Android ABI yapısı için hazırlanmıştır. Modern Android/NDK ayarlarına göre rastgele güncellenmemelidir.

---

## Projenin mevcut durumu

Projenin GitHub Actions tabanlı build sistemi çalışmaktadır.

Kaynak kod GitHub üzerinde bulunduğunda workflow gerekli derleme ortamını oluşturur ve sonuç olarak:

```text
libmultiplayermod.so
```

dosyasını üretir.

Üretilen `.so` dosyası gerçek cihaz üzerinde test edilmiştir ve oyuna doğru şekilde yerleştirildiğinde **başarılı biçimde yüklenmekte ve çalışmaktadır.**

Bu nedenle gelecekte yapılacak değişikliklerde mevcut build sisteminin ve hedef mimarinin korunması çok önemlidir.

---

# Teknik hedef

Projenin mevcut derleme yapısı özellikle eski Android ABI desteğine göre hazırlanmıştır.

Temel hedef:

```text
Architecture : ARMv5 / armeabi
Android API  : 15
NDK          : Android NDK r16b
C++          : GNU++11
STL          : GNU libstdc++
```

Bu proje için hedef:

```text
armeabi
```

olmalıdır.

Şunlarla değiştirilmemelidir:

```text
armeabi-v7a
arm64-v8a
x86
x86_64
```

Özellikle **ARMv5** desteği bu projenin eski oyunla uyumluluğu açısından önemlidir.

NDK r16 döneminde ARMv5/armeabi artık eski kabul edilen bir hedef hâline gelmiş olsa da açıkça belirtilerek derlenebiliyordu. Daha sonraki NDK sürümlerinde bu destek kaldırıldığı için modern NDK'ya rastgele geçiş yapılmamalıdır.

---

# Kaynak kod

Ana native kaynak:

```text
yeni.cpp
```

Bu dosya projenin ana kodudur.

Kod C++ kullanılarak hazırlanmıştır ve Android NDK üzerinden native shared library olarak derlenir.

Kaynak kod içerisinde Android JNI, Android input sistemi, OpenGL ES, EGL, POSIX socket API'leri, thread işlemleri ve runtime hook mekanizması kullanılmaktadır.

Başlıca kullanılan Android/native bileşenler:

```cpp
jni.h
android/log.h
android/input.h
dlfcn.h
GLES2/gl2.h
EGL/egl.h
```

Ayrıca network tarafında standart Linux/POSIX socket yapıları kullanılmaktadır.

---

# Hook sistemi

Projenin en önemli bölümlerinden biri oyunun mevcut native fonksiyonlarına **hook uygulanmasıdır**.

Bunun için:

```cpp
#include "Substrate.h"
```

kullanılmaktadır.

Oyunun bellekte yüklenen native library adresi alınarak belirli fonksiyon offset'leri üzerinden hedef fonksiyonlara hook uygulanır.

Mevcut kaynak kodunda kullanılan ana adresler:

```cpp
renderMenuAddr = libBase + 0x00033974 + 1;
updateGUIAddr  = libBase + 0x0002f6a0 + 1;
gameUpdateAddr = libBase + 0x00057ee8 + 1;
inGameMenuAddr = libBase + 0x00032090 + 1;
```

Daha sonra bu adreslere `MSHookFunction` üzerinden müdahale edilir.

Ayrıca Android input sistemi için:

```cpp
AInputQueue_getEvent
```

üzerinden input hook mekanizması kullanılmaktadır.

Bu offset'ler projenin mevcut oyun sürümüyle ilişkilidir.

### Çok önemli

Bu adresler rastgele değiştirilmemelidir.

Bunlar:

```text
0x00033974
0x0002f6a0
0x00057ee8
0x00032090
```

gibi sabit offset'lerdir ve hedef oyunun native binary yapısına bağlıdır.

Yeni özellik eklenirken mevcut hook adresleri korunmalıdır.

Oyunun farklı bir sürümüne geçilecekse offset'ler yeniden analiz edilmeden değiştirilmemelidir.

---

# Multiplayer altyapısı

Ana C++ kodunda temel multiplayer/network altyapısı bulunmaktadır.

Network sistemi iki ana bölüm kullanır.

## UDP discovery

LAN üzerindeki oyuncuları/host'ları bulmak amacıyla UDP discovery sistemi kullanılmaktadır.

Discovery için:

```text
UDP Port: 8888
```

kullanılmaktadır.

Kod içerisinde discovery paketleri için:

```text
FS14_PING
FS14_PONG
```

benzeri mesajlar bulunmaktadır.

Bunlar mevcut kaynak kodundaki protokol isimleridir ve gelecekte temizlenmek istenirse network protokolü bozulmayacak şekilde değiştirilmelidir.

---

## TCP synchronization

Oyuncular arasında temel veri senkronizasyonu için TCP bağlantısı da kullanılmaktadır.

Kullanılan port:

```text
TCP Port: 8889
```

Network sistemi içerisinde paket yapıları tanımlanmış ve TCP üzerinden veri gönderilip alınmaktadır.

Kod içerisinde network işlemlerinin ayrı thread üzerinde yürütülmesi için POSIX thread mekanizmasından faydalanılmaktadır.

Bu yapı sayesinde oyun render/update işlemleri ile network işlemleri birbirinden ayrılmaktadır.

---

# GUI sistemi

Mod içerisinde oyun üzerinde görüntülenen multiplayer arayüzü bulunmaktadır.

Arayüz için:

```text
Dear ImGui
```

kullanılmaktadır.

OpenGL ES 2 backend'i kullanılmaktadır:

```text
imgui_impl_opengl3.cpp
```

Proje Android üzerinde çalıştığı için GUI tarafı normal masaüstü OpenGL yerine oyunun kullandığı OpenGL ES ortamına uyarlanmıştır.

Ana ImGui dosyaları:

```text
imgui/
├── imgui.cpp
├── imgui.h
├── imgui_draw.cpp
├── imgui_tables.cpp
├── imgui_widgets.cpp
└── backends/
    ├── imgui_impl_opengl3.cpp
    ├── imgui_impl_opengl3.h
    └── imgui_impl_opengl3_loader.h
```

Bu dosyaların sürümü mümkün olduğunca mevcut proje ile uyumlu tutulmalıdır.

**ImGui dosyalarının güncel sürümle değiştirilmesi otomatik olarak yapılmamalıdır.**

API değişiklikleri ve ABI/uyumluluk problemleri oluşabilir.

---

# Android GUI / JNI

Kod içerisinde Android Java tarafıyla iletişim için JNI kullanılır.

JNI üzerinden Android tarafında çeşitli işlemler gerçekleştirilebilmektedir.

Bunlar arasında:

* Android UI işlemleri
* klavye girişleri
* toast benzeri bildirimler
* Java tarafına çağrı yapılması

gibi işlemler bulunmaktadır.

Bu nedenle native kod üzerinde değişiklik yapılırken JNI çağrılarının Android API 15 ile uyumlu olması gerekir.

---

# Input sistemi

Mod, Android'in native input sistemine de müdahale etmektedir.

Kullanılan temel API:

```cpp
AInputQueue_getEvent
```

Bu yapı üzerinden Android dokunmatik/klavye/input olayları yakalanarak ImGui arayüzüne aktarılabilmektedir.

Özellikle eski Android sürümilerinde input davranışı modern Android sürümilerinden farklı olabileceğinden input kodu değiştirilirken mevcut çalışma yapısı korunmalıdır.

---

# OpenGL

Mod, oyunun render ortamına hook üzerinden bağlanmaktadır.

Kullanılan grafik API'leri:

```text
OpenGL ES 2
EGL
```

Kod içerisinde:

```cpp
#include <GLES2/gl2.h>
#include <EGL/egl.h>
```

kullanılmaktadır.

ImGui'nin OpenGL3 backend dosyası burada kullanılsa da Android tarafında OpenGL ES ortamına göre yapılandırılmıştır.

---

# Texture sistemi

Projede GUI içerisinde özel buton/görsel kullanımı için:

```text
buton_texture.h
```

dosyası bulunmaktadır.

Ek olarak image loading işlemleri için:

```text
stb_image.h
```

kullanılmaktadır.

Bu yapı multiplayer menüsündeki görsel elemanların yüklenmesi ve render edilmesinde kullanılmaktadır.

---

# Multiplayer menüsü

Hook edilen menü fonksiyonları üzerinden oyunun mevcut arayüzüne multiplayer ile ilgili yeni kontroller eklenmektedir.

Ana amaçlardan biri oyunun menü sisteminin içine multiplayer seçenekleri yerleştirmektir.

GUI tarafında:

```text
Host
Client
LAN discovery
Bağlantı
Oyuncu bilgileri
Network durumu
```

gibi multiplayer işlemlerinin kontrol edilmesine yönelik altyapı bulunmaktadır.

Arayüz sistemi doğrudan oyunun native render/update döngüsüyle birlikte çalışmaktadır.

---

# Game update hook

Oyunun ana update döngülerinden biri hook edilmektedir.

Mevcut hedef:

```cpp
gameUpdateAddr = libBase + 0x00057ee8 + 1;
```

Bu hook multiplayer sisteminin oyun döngüsü ile senkron çalışması açısından önemlidir.

Network'ten gelen verilerin oyun tarafında işlenmesi veya multiplayer durumlarının güncellenmesi gibi işlemler için bu tip update hook'ları kullanılmaktadır.

---

# Menü hook'ları

Proje şu ana menü/in-game fonksiyonlarına hook uygulamaktadır:

```text
renderMenu
updateGUI
gameUpdate
inGameMenu
```

Amaç, mevcut oyunun GUI ve update sisteminin üzerine multiplayer özellikleri eklemektir.

Hook sistemi Substrate üzerinden gerçekleştirilmektedir.

---

# Network paketleri

Network tarafında paket yapısı bulunmaktadır.

Paketlerin amacı istemci/host arasında multiplayer durumunu aktarabilecek temel bir protokol oluşturmaktır.

Mevcut sistemin geliştirilmesi sırasında paket yapısının boyutu ve sıralaması dikkatli korunmalıdır.

Özellikle:

```text
struct
alignment
padding
integer size
byte order
```

gibi konular önemlidir.

Bir tarafta değiştirilen paket yapısı diğer istemcilerle uyumsuzluk oluşturabilir.

Bu nedenle network protokolü değiştirilecekse istemci ve host tarafı birlikte güncellenmelidir.

---

# Threading

Network işlemleri ayrı thread'lerde yürütülebilmektedir.

Bu yapı önemlidir çünkü socket işlemlerinin oyun render/update thread'ini bloklamaması gerekir.

Temel mantık:

```text
Game Thread
     |
     +---- GUI / Hook / Update
     
Network Thread
     |
     +---- UDP Discovery
     +---- TCP Communication
```

şeklindedir.

Thread güvenliği gerektiren ortak değişkenlerde yarış koşullarına dikkat edilmelidir.

---

# Mevcut otomatik oyun geçişi

Kod içerisinde istemci tarafındaki oyun geçişi için bir mekanizma bulunmaktadır.

Ancak mevcut durumda:

```text
"Oyuna gecis yapiliyor. Lutfen oyunu manuel baslatin."
```

şeklinde kullanıcıya bilgi verilmektedir.

Yani network bağlantısının kurulması ile oyunun gerçek oyun state'ine tamamen otomatik olarak geçirilmesi aynı şey değildir.

Gelecekte otomatik oyun başlatma veya game-state synchronization geliştirilmek istenirse önce mevcut oyun fonksiyonları analiz edilmelidir.

---

# Build sistemi

Proje GitHub Actions kullanmaktadır.

Workflow dosyası:

```text
.github/workflows/build.yml
```

şeklindedir.

Build sistemi eski Android NDK ortamını hazırlayıp CMake ile projeyi derler.

Temel işlem:

```text
GitHub Repository
        ↓
GitHub Actions
        ↓
Android NDK r16b
        ↓
CMake
        ↓
ARMv5 / armeabi
        ↓
libmultiplayermod.so
```

şeklindedir.

Üretilen `.so` dosyası Actions artifact olarak alınabilir ve Android oyun ortamında test edilebilir.

---

# CMake

Projenin CMake yapılandırması:

```text
CMakeLists.txt
```

dosyasındadır.

Kaynaklar arasında ana C++ dosyasının yanında ImGui kaynakları da bulunmaktadır.

Temel hedef:

```text
multiplayermod
```

isimli shared library üretmektir.

Çıktı:

```text
libmultiplayermod.so
```

şeklindedir.

---

# Repository yapısı

Mevcut proje genel olarak aşağıdaki yapıyı kullanır:

```text
fs12multiplayerolacak-/
│
├── yeni.cpp
├── buton_texture.h
├── stb_image.h
├── Substrate.h
├── CMakeLists.txt
│
├── imgui/
│   ├── imgui.cpp
│   ├── imgui.h
│   ├── imgui_draw.cpp
│   ├── imgui_tables.cpp
│   ├── imgui_widgets.cpp
│   ├── imconfig.h
│   ├── imgui_internal.h
│   ├── imstb_rectpack.h
│   ├── imstb_textedit.h
│   ├── imstb_truetype.h
│   │
│   └── backends/
│       ├── imgui_impl_opengl3.cpp
│       ├── imgui_impl_opengl3.h
│       └── imgui_impl_opengl3_loader.h
│
├── libs/
│   └── armeabi/
│       └── libsubstrate.so
│
└── .github/
    └── workflows/
        └── build.yml
```

---

# Yapay zekâ / geliştirici için önemli kurallar

Bu repository üzerinde başka bir yapay zekâ veya geliştirici çalışma yaparken aşağıdaki kurallara uymalıdır.

## 1. Çalışan build sistemini bozma

Şu anda repository `.so` üretebilmektedir.

Bu nedenle çalışan:

```text
build.yml
CMakeLists.txt
NDK ayarları
ABI ayarları
```

gereksiz yere değiştirilmemelidir.

Önce mevcut sistemi anlamak gerekir.

---

## 2. ARMv5 desteğini koru

Proje:

```text
ARMv5 / armeabi
```

hedeflemektedir.

`armeabi-v7a` veya `arm64-v8a` kullanmak projenin amacıyla uyumlu değildir.

---

## 3. NDK sürümünü rastgele değiştirme

Bu proje eski Android oyunuyla uyumluluk amacıyla:

```text
NDK r16b
```

kullanmaktadır.

Modern NDK'ya geçiş yapılmadan önce eski ARMv5 toolchain gereksinimleri mutlaka değerlendirilmelidir.

---

## 4. Hook offsetlerini rastgele değiştirme

Aşağıdaki offsetler mevcut projede kritik öneme sahiptir:

```text
0x00033974
0x0002f6a0
0x00057ee8
0x00032090
```

Bunlar oyunun belirli native fonksiyonlarına karşılık gelmektedir.

Bir yapay zekâ bu değerleri görür görmez "eski oldukları için güncel offset ile değiştireyim" yaklaşımı uygulamamalıdır.

Yeni offset ancak hedef oyunun binary'si analiz edilerek belirlenmelidir.

---

## 5. ImGui sürümünü rastgele güncelleme

Projede mevcut ImGui kaynaklarının birbiriyle uyumlu olması gerekmektedir.

Örneğin:

```text
imgui.cpp
imgui.h
imgui_internal.h
imgui_draw.cpp
imgui_tables.cpp
imgui_widgets.cpp
```

gibi dosyalar birbirleriyle aynı sürüm ailesinden olmalıdır.

Sadece bir veya iki dosyanın güncellenmesi derleme veya runtime sorunlarına neden olabilir.

---

## 6. Substrate ABI uyumluluğunu koru

Projede:

```text
Substrate.h
libsubstrate.so
```

kullanılmaktadır.

Özellikle:

```text
libs/armeabi/libsubstrate.so
```

dosyası hedef ABI açısından önemlidir.

Rastgele başka bir Substrate binary'siyle değiştirilmemelidir.

---

# Geliştirme yaklaşımı

Bu repository üzerinde yeni özellik geliştirirken önce mevcut mimari incelenmelidir.

Önerilen akış:

```text
1. yeni.cpp incelenir
2. Hook noktaları belirlenir
3. Mevcut network protokolü incelenir
4. GUI yapısı incelenir
5. Game update akışı incelenir
6. Gerekli değişiklik yapılır
7. GitHub Actions ile build alınır
8. Oluşan libmultiplayermod.so test edilir
```

Bir değişiklik yalnızca derlenebiliyor diye başarılı kabul edilmemelidir.

Çünkü bu proje native hook kullandığından:

```text
Compile başarı
≠
Runtime başarı
```

olabilir.

Asıl test gerçek Android cihaz ve gerçek oyun üzerinde yapılmalıdır.

---

# Gelecekte geliştirilebilecek sistemler

Projenin gelecekte aşağıdaki konularda geliştirilmesi mümkündür:

```text
- Gerçek oyuncu pozisyon senkronizasyonu
- Oyuncu isimleri
- Oyuncu araçlarının senkronizasyonu
- Oyun state synchronization
- Host/client sistemi
- LAN lobby
- İnternet üzerinden bağlantı
- Daha gelişmiş server sistemi
- Bağlantı kopması yönetimi
- Paket doğrulama
- Reconnect sistemi
- Multiplayer oyun başlatma
- Daha gelişmiş GUI
```

Ancak yeni özellikler eklenirken mevcut ARMv5/NDK r16b/Android API 15 uyumluluğu korunmalıdır.

---

# Projenin amacı

Bu repository'nin temel amacı, eski bir Android oyununun native çalışma yapısını kullanarak modern anlamda multiplayer benzeri bir deneyim oluşturabilecek bir altyapı geliştirmektir.

Proje özellikle eski Android teknolojileriyle çalıştığı için günümüz Android projelerinden farklıdır.

Buradaki temel yaklaşım:

```text
Oyunun native binary'si
        ↓
Runtime hook
        ↓
Native mod library
        ↓
GUI + Network
        ↓
Multiplayer altyapısı
```

şeklindedir.

Bu nedenle proje yalnızca standart bir Android uygulaması değildir.

Bu, oyunun runtime'ına native seviyede bağlanan bir mod projesidir.

---

# Test edilmiş çıktı

Build sisteminin başarılı çalışması sonucunda:

```text
libmultiplayermod.so
```

üretilmektedir.

Bu kütüphane hedef Android oyun ortamına yerleştirildiğinde yüklenebilmekte ve mevcut hook/GUI/network kodu çalışmaktadır.

Bu repository'de yapılan değişiklikler sonrasında ilk kontrol edilmesi gereken şey:

```text
GitHub Actions → Build → libmultiplayermod.so
```

çıktısının başarıyla oluşmasıdır.

Ardından gerçek Android cihaz üzerinde runtime testi yapılmalıdır.

---

# Katkıda bulunma

Projeye katkı sağlayan geliştiricilerden mevcut mimariyi koruması beklenmektedir.

Özellikle şu dört konu kritik kabul edilir:

```text
ARMv5 / armeabi
Android API 15
NDK r16b
Native hook offsetleri
```

Bunlardan biri değiştirilmeden önce projenin mevcut çalışma durumu dikkate alınmalıdır.

---

# Repository

GitHub:

https://github.com/erensariisik03-sudo/fs12multiplayerolacak-

Git clone:

```bash
git clone https://github.com/erensariisik03-sudo/fs12multiplayerolacak-.git
```

---

## Son not

Bu proje halen geliştirme aşamasındadır.

Mevcut `.so` üretim sistemi ve runtime yükleme mekanizması çalışır durumdadır. Multiplayer sisteminin daha ileri seviyeye taşınması için öncelikle oyunun native fonksiyonlarının, state yapısının ve network senkronizasyon noktalarının daha ayrıntılı analiz edilmesi gerekmektedir.

Yeni bir yapay zekâ veya geliştirici bu repository ile çalışmaya başladığında öncelikle:

```text
README.md
CMakeLists.txt
.github/workflows/build.yml
yeni.cpp
```

dosyalarını incelemeli ve mevcut mimariyi anlamadan kritik altyapıyı değiştirmemelidir.
