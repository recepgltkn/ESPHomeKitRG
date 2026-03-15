# ESPHomeKitRG

Wemos D1 Mini R2 tabanli, Apple HomeKit uyumlu tek kanalli role projesi.

Bu proje ile:

- roleyi iPhone `Home` uygulamasina dogrudan ekleyebilirsiniz
- roleyi HomeKit uzerinden acip kapatabilirsiniz
- cihazi USB baglamadan OTA ile guncelleyebilirsiniz
- GitHub'daki yeni firmware'i her dakika kontrol edip otomatik kurabilirsiniz
- cihaz durumunu HTTP uzerinden gorebilirsiniz
- canli bir durum panelini sayfa yenilemeden izleyebilirsiniz
- loglari Telnet uzerinden izleyebilirsiniz
- yeni bir eve goturdugunuzde Wi-Fi ayarlarini web arayuzunden degistirebilirsiniz

## Donanim

- Wemos D1 Mini R2 veya uyumlu ESP8266 kart
- Wemos uyumlu relay modulu veya `D1/GPIO5` uzerinden surulen bir role
- 5V uygun besleme

Varsayilan role pini:

- `D1 / GPIO5`

Kodda role aktif seviyesi:

- `LOW = role acik`
- `HIGH = role kapali`

Eger kullandiginiz role karti ters lojik ile calisiyorsa `src/main.cpp` icindeki aktif/pasif seviye sabitlerini degistirin.

## Ozellikler

- Native HomeKit accessory olarak calisir
- Home app uzerinden `Switch` aksesuar tipinde gorunur
- Wi-Fi koparsa yeniden baglanmayi dener
- Wi-Fi geri geldiginde HomeKit tarafini temiz toparlamak icin kontrollu restart uygular
- `ArduinoOTA` ile kablosuz firmware guncelleme destekler
- HTTP JSON status endpoint sunar
- `/status` altinda canli durum paneli sunar
- Telnet log portu sunar
- Wi-Fi kurulum sayfasi sunar
- mevcut ağa baglanamazsa kendi setup access point'ini acar
- GitHub `gh-pages` uzerindeki manifest dosyasini her 60 saniyede bir kontrol eder
- yeni surum bulursa firmware'i otomatik indirip kurar

## Proje Yapisi

- `src/main.cpp`: ana firmware, Wi-Fi, HomeKit, OTA, HTTP, Telnet
- `src/my_accessory.c`: HomeKit accessory tanimi
- `include/wifi_info.h`: Wi-Fi bilgileri
- `platformio.ini`: PlatformIO ortami

## Wi-Fi Ayari

`include/wifi_info.h` dosyasini duzenleyin:

```cpp
#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"
```

## Derleme ve USB ile Yukleme

```bash
~/.platformio/penv/bin/pio run
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbserial-110
```

Seri monitor:

```bash
~/.platformio/penv/bin/pio device monitor -b 115200 -p /dev/cu.usbserial-110
```

## HomeKit Kurulumu

Cihaz Home uygulamasina su kod ile eklenir:

```text
111-11-111
```

Home uygulamasinda cihaz adi varsayilan olarak `Wemos Role` gorunur.

Eger cihaz daha once eslestirilmis ve kaldirilmis ise bazen flash temizligi gerekebilir.

ESP flash temizleme:

```bash
~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbserial-110 erase_flash
```

Ardindan firmware tekrar yuklenmelidir.

## OTA Guncelleme

Cihaz agda calisiyorsa USB olmadan guncelleme yapabilirsiniz:

```bash
~/.platformio/penv/bin/pio run -t upload --upload-port 192.168.68.101
```

PlatformIO IP adresi gordugunde otomatik olarak `espota` kullanir.

## Otomatik GitHub Guncelleme

Bu proje GitHub'a push edilen yeni firmware'i otomatik alacak sekilde hazirlanmistir.

Akis:

- `main` branch'e push yaparsiniz
- GitHub Actions firmware'i derler
- `gh-pages/latest/firmware.bin` ve `gh-pages/latest/version.json` dosyalarini yayinlar
- cihaz bu manifest'i her `60 saniyede` bir kontrol eder
- yeni surum bulursa otomatik OTA guncelleme yapar

Manifest adresi:

```text
https://recepgltkn.github.io/ESPHomeKitRG/latest/version.json
```

Not:

- ilk manifest ancak workflow bir kez calistiktan sonra olusur
- cihaz JSON icinde son update kontrol sonucunu da gosterir
- otomatik guncelleme sirasinda cihaz kendini yeniden baslatir

## Wi-Fi Kurulum Modu

Bu proje tasinabilir kullanim icin Wi-Fi kurulum modu icerir.

Calisma mantigi:

- cihaz acilista hem mevcut Wi-Fi'ye baglanmayi dener
- hem de gecici kurulum agini acar

Kurulum access point adi:

```text
Wemos-Setup
```

Iki farkli kullanim vardir:

### 1. Cihaz mevcut Wi-Fi'ye baglanirsa

Bu durumda setup access point kapanir ama ayar sayfasi cihazin normal IP adresinde acik kalir:

```text
http://cihaz-ip/setup
```

Ornek:

```text
http://192.168.68.101/setup
```

### 2. Cihaz mevcut Wi-Fi'ye baglanamazsa

Bu durumda `Wemos-Setup` acik kalir.

Telefondan veya bilgisayardan bu aga baglanip su adrese gidersiniz:

```text
http://192.168.4.1/setup
```

Bu sayfa:

- yakindaki Wi-Fi aglarini tarar
- SSID secmenize veya elle yazmaniza izin verir
- sifre girmenizi saglar
- bilgileri cihaza kaydeder
- cihazi yeniden baslatir

Wi-Fi bilgileri cihaz uzerinde `LittleFS` icinde saklanir.

## HTTP Durum Endpoint'i

Varsayilan endpoint:

```text
http://192.168.68.101/
```

JSON olarak su bilgileri doner:

- cihaz bilgileri
- firmware bilgileri
- Wi-Fi durumu
- IP, gateway, subnet, DNS
- MAC, BSSID, kanal, RSSI
- Wi-Fi reconnect sayaçlari
- setup endpoint bilgisi
- role durumu
- HomeKit istemci sayisi
- uptime
- heap ve bellek parcaciklanmasi
- sketch boyutu
- reset nedeni
- servis URL bilgileri
- otomatik update durumu
- son bulunan uzak surum bilgisi
- bir sonraki update kontrolune kalan sure

Ornek:

```json
{
  "device": {
    "name": "Wemos Role"
  },
  "network": {
    "connected": true,
    "ip": "192.168.68.101",
    "rssi": -64
  },
  "relay": {
    "on": false
  }
}
```

## Telnet Log

Loglar ag uzerinden izlenebilir:

```bash
telnet 192.168.68.101 23
```

Not:

- tek istemci kabul edilir
- ikinci telnet baglantisi reddedilir

## Canli Durum Ekrani

Tarayicidan:

```text
http://192.168.68.101/status
```

Bu sayfa:

- 3 saniyede bir otomatik yenilenir
- role, Wi-Fi, RSSI, heap ve surum bilgisini gosterir
- update durumunu ve bir sonraki kontrol zamanini gosterir
- ag, sistem ve servis detaylarini canli olarak listeler

Ham JSON:

```text
http://192.168.68.101/api/status
```

## Varsayilan Ag Servisleri

- HTTP: `80`
- Telnet: `23`
- OTA: `8266`
- Setup AP fallback IP: `192.168.4.1`

## Git

`.pio` ve bazi VS Code ciktilari `.gitignore` icinde dislanmistir.

Repo:

- GitHub: `https://github.com/recepgltkn/ESPHomeKitRG`

## Bilinen Sinirlar

- Proje `ESP8266` uzerindedir; HomeKit kutuphanesi modern ortamlarda zaman zaman kararsizlik gosterebilir
- Mesh Wi-Fi aglarda bazen cihaz agda bagli olsa bile Home uygulamasinda gecici olarak `No Response` gorulebilir
- Bu davranis ozellikle `ESP8266 + HomeKit + mesh AP` kombinasyonunda daha sik gorulebilir
- En iyi sonuc icin:
  - DHCP rezervasyonu verin
  - cihazi sabit 2.4 GHz kapsama alaninda tutun
  - gerekirse IoT icin ayri SSID kullanin

## Guvenlik Notu

Bu proje test ve kisisel kullanim icin hazirlanmistir.

- HomeKit setup kodunu degistirmek isteyebilirsiniz
- OTA sifresi su anda tanimli degil
- Ag icinde calistirilmasi tavsiye edilir

## Gelecekte Eklenebilecekler

- fiziksel buton destegi
- HTML dashboard
- MQTT veya syslog entegrasyonu
- guc tuketimi olcumu icin `INA219` destegi
- ESP32 tabanli daha kararlı HomeKit varyanti
