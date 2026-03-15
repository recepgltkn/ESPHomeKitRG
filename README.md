# ESPHomeKitRG

Wemos D1 Mini R2 tabanli, Apple HomeKit uyumlu role, sensor ve PWM cikis projesi.

Bu proje ile:

- roleyi iPhone `Home` uygulamasina dogrudan ekleyebilirsiniz
- roleyi HomeKit uzerinden acip kapatabilirsiniz
- ayni cihazda sicaklik, nem, hareket ve isik sensorlerini HomeKit'e ekleyebilirsiniz
- iki ayri PWM cikis ile MOSFET uzerinden serit LED surebilirsiniz
- fiziksel buton ile roleyi acip kapatabilirsiniz
- cihazi USB baglamadan OTA ile guncelleyebilirsiniz
- cihaz durumunu HTTP uzerinden gorebilirsiniz
- canli bir durum panelini sayfa yenilemeden izleyebilirsiniz
- loglari Telnet uzerinden izleyebilirsiniz
- yeni bir eve goturdugunuzde Wi-Fi ayarlarini web arayuzunden degistirebilirsiniz
- sensor ve buton davranislarini `/config` altindan ayarlayabilirsiniz

## Donanim

- Wemos D1 Mini R2 veya uyumlu ESP8266 kart
- Wemos uyumlu relay modulu veya `D1/GPIO5` uzerinden surulen bir role
- `DHT22` sicaklik/nem sensoru
- dijital cikisli `PIR` hareket sensoru
- `A0` ile kullanilan LDR bolucu devresi
- `D2` uzerinden GND'ye baglanan anlik buton
- `D7` ve `D8` uzerinden surulen iki MOSFET/PWM cikisi
- 5V uygun besleme

Varsayilan pin plani:

- `D1 / GPIO5`: role
- `D2 / GPIO4`: buton
- `D5 / GPIO14`: PIR
- `D6 / GPIO12`: DHT22
- `D7 / GPIO13`: PWM1 / MOSFET gate
- `D8 / GPIO15`: PWM2 / MOSFET gate
- `A0`: LDR

Kodda role aktif seviyesi:

- `HIGH = role acik`
- `LOW = role kapali`

Eger kullandiginiz role karti ters lojik ile calisiyorsa `src/main.cpp` icindeki aktif/pasif seviye sabitlerini degistirin.

Baglanti notlari:

- buton bir ucu `D2`, diger ucu `GND`
- PIR cikisi `D5`, besleme ve GND ortak
- DHT22 data `D6`, besleme ve GND ortak
- LDR dogrudan baglanmaz; `A0` icin uygun gerilim bolucu ile kullanilmalidir
- `A0` pinine `3.3V` ustu gerilim verilmemelidir
- MOSFET gate hatlari icin `10k` pull-down tavsiye edilir
- harici LED serit beslemesi kullaniliyorsa Wemos GND ile ortak GND yapilmalidir
- `D8` boot pini oldugu icin bu hatta ters lojik veya agir yuk baglanmamali, sadece uygun gate surucu/pull-down ile kullanilmalidir

## Ozellikler

- Native HomeKit accessory olarak calisir
- Home app uzerinden tek cihaz altinda `Lightbulb`, `Temperature`, `Humidity`, `Motion`, `Light Sensor`, `PWM1 Lightbulb` ve `PWM2 Lightbulb` servisleri gorunur
- Wi-Fi koparsa yeniden baglanmayi dener
- Wi-Fi geri geldiginde HomeKit tarafini temiz toparlamak icin kontrollu restart uygular
- `ArduinoOTA` ile kablosuz firmware guncelleme destekler
- HTTP JSON status endpoint sunar
- `/status` altinda canli durum paneli sunar
- `/config` altinda kalici sensor ayar sayfasi sunar
- Telnet log portu sunar
- Wi-Fi kurulum sayfasi sunar
- mevcut ağa baglanamazsa kendi setup access point'ini acar
- HomeKit istemcisi uzun sure kaybolursa kontrollu toparlama restarti uygular

## Proje Yapisi

- `src/main.cpp`: ana firmware, Wi-Fi, HomeKit, OTA, HTTP, Telnet, sensorler, buton, PWM cikislari, config
- `src/my_accessory.c`: HomeKit accessory, sensor ve PWM service tanimlari
- `include/wifi_info.h`: Wi-Fi bilgileri
- `platformio.ini`: PlatformIO ortami

## Wi-Fi Ayari

`include/wifi_info.h` dosyasini duzenleyin:

```cpp
#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"
```

Bu repodaki mevcut varsayilan STA bilgileri:

```text
SSID: GULTEKIN
Sifre: admin10.
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

Home uygulamasinda cihaz adi varsayilan olarak `Wemos Role <CHIPID>` gorunur.

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

## Firmware Update Yapisi

Bu projede iki ayri update yolu vardir:

- klasik `ArduinoOTA` aktif
- Git tabanli self-update arayuzu mevcut ama otomatik kurulum kapali

Su anki durum:

- `pio run -t upload --upload-port <ip>` ile OTA yukleme kullanilabilir
- `/update` sayfasi manuel kontrol/yukleme arayuzu sunar
- GitHub tabanli otomatik self-update varsayilan olarak kapalidir

Manifest adresi:

```text
https://raw.githubusercontent.com/recepgltkn/ESPHomeKitRG/main/docs/latest/version.json
```

## Wi-Fi Kurulum Modu

Bu proje tasinabilir kullanim icin Wi-Fi kurulum modu icerir.

Calisma mantigi:

- cihaz acilista hem mevcut Wi-Fi'ye baglanmayi dener
- hem de gecici kurulum agini acar

Kurulum access point adi:

```text
Wemos-Setup-<CHIPID>
```

Kurulum access point sifresi:

```text
12345678
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

Bu durumda `Wemos-Setup-<CHIPID>` acik kalir.

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
- PWM cikis durumlari
- sensor olcumleri
- config degerleri
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
- role, Wi-Fi, RSSI, heap, sicaklik, nem, PIR, isik, PWM1 ve PWM2 bilgisini gosterir
- update durumunu ve bir sonraki kontrol zamanini gosterir
- ag, sistem ve servis detaylarini canli olarak listeler

Ham JSON:

```text
http://192.168.68.101/api/status
```

## Config Ekrani

Tarayicidan:

```text
http://192.168.68.101/config
```

Bu sayfadan su ayarlar degistirilebilir:

- `LDR threshold`
- `LDR hysteresis`
- `PIR hold ms`
- `PIR cooldown ms`
- `temperature offset`
- `humidity offset`
- `button debounce ms`
- `PWM1 default brightness`
- `PWM2 default brightness`
- `PWM1 invert 0/1`
- `PWM2 invert 0/1`

Ayarlar `LittleFS` icinde saklanir ve yeniden baslatma sonrasi korunur.

## HomeKit Sensor Destegi

Bu firmware tek bir Wemos D1 Mini uzerinde ayni anda su HomeKit servislerini sunar:

- `Lightbulb`
- `Temperature Sensor`
- `Humidity Sensor`
- `Motion Sensor`
- `Light Sensor`
- `PWM1 Lightbulb`
- `PWM2 Lightbulb`

Yani ayni aksesuar altinda hem ana isik/röle hem de DHT/PIR/LDR verileri ve iki ek PWM LED cikisi Home uygulamasinda kullanilabilir.

## Buton Davranisi

`D2` pinine baglanan fiziksel buton `INPUT_PULLUP` ile okunur.

Butona basildiginda:

- role toggle olur
- HomeKit switch durumu aninda guncellenir

Buton debounce suresi `/config` altindan ayarlanabilir.

## PWM / MOSFET Cikislari

Iki ek cikis HomeKit'te ayri ayri dim edilebilir:

- `PWM1`: `D7`
- `PWM2`: `D8`

Tipik MOSFET baglantisi:

- `D7` veya `D8` -> MOSFET gate
- gate ile `GND` arasina `10k` pull-down
- MOSFET source -> `GND`
- MOSFET drain -> LED serit eksi
- LED serit arti -> harici besleme arti
- Wemos `GND` ve harici besleme `GND` ortak

Bu cikislar HomeKit'te `On/Off` ve `Brightness` ile kontrol edilir.

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

- RGB/RGBW cikis destegi
- MQTT veya syslog entegrasyonu
- guc tuketimi olcumu icin `INA219` destegi
- ESP32 tabanli daha kararlı HomeKit varyanti
