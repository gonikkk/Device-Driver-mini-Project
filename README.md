# Device-Driver-mini-Project

Linux 커널 모듈 기반 디바이스 드라이버(OLED, RTC, Rotary)를 구현하고  
Raspberry Pi에서 사용자 애플리케이션과 연동한 임베디드 UI 프로젝트입니다.

---

## Development Environment

- Kernel Driver Build: Ubuntu Linux  
- Target Device: Raspberry Pi  
- Language: C  
- Interface: Character Device (/dev)

커널 모듈(.ko)은 Ubuntu 환경에서 빌드한 뒤 Raspberry Pi로 전송하여 실행하며,  
사용자 영역 애플리케이션은 Raspberry Pi에서 직접 컴파일 및 실행합니다.

---

## Build Kernel Drivers (Ubuntu)

각 디바이스 드라이버 디렉토리에서 make를 실행하여 커널 모듈(.ko)을 생성합니다.
cd drivers/ssd1306
make
cd ../ds1302
make
cd ../rotary
make

빌드가 완료되면 각 디렉토리에 ko 파일이 생성됩니다.

---

## Transfer Kernel Modules to Raspberry Pi

Ubuntu에서 빌드한 커널 모듈 파일을 Raspberry Pi로 전송합니다.

scp *.ko pi@<raspberry_pi_ip>:/home/pi/

---

## Load Kernel Modules (Raspberry Pi)

Raspberry Pi에서 커널 모듈을 로드합니다.

sudo insmod ssd1306_driver.ko
sudo insmod ds1302_driver.ko
sudo insmod rotary.ko

모듈이 정상적으로 로드되었는지 확인합니다.

lsmod | grep driver

---

## Create Device Files

디바이스 노드가 자동으로 생성되지 않는 경우 수동으로 생성합니다.

sudo mknod /dev/ssd1306_driver c <major> 0
sudo mknod /dev/ds1302_driver c <major> 0
sudo mknod /dev/rotary_driver c <major> 0

major 번호는 dmesg 또는 /proc/devices에서 확인할 수 있습니다.

sudo chmod 666 /dev/ssd1306_driver
sudo chmod 666 /dev/ds1302_driver
sudo chmod 666 /dev/rotary_driver

---

## Build Application (Raspberry Pi)

gcc main1.c -o main1

---

## Run Application

로터리 입력을 통해 메뉴를 조작하며 OLED UI 동작을 확인할 수 있습니다.

---

## Notes

- 커널 드라이버는 Ubuntu 환경에서 빌드 후 Raspberry Pi로 배포하는 구조를 사용합니다.
- /dev 인터페이스를 통해 사용자 애플리케이션과 커널 드라이버가 연동됩니다.
