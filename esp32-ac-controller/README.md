# ESP32-C3 SuperMini 에어컨 IR 제어

적외선 수신/분석과 캡처한 자동 ON/OFF 신호의 터미널 송신 시험을 포함합니다.

## 연결

| HX-M121 계열 | ESP32-C3 SuperMini |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| OUT | GPIO3 |

수신 모듈을 5V로 구동하면 OUT도 5V가 될 수 있으므로 ESP32-C3 GPIO에 직접 연결하지 마세요. 3.3V 구동을 권장합니다.

## PlatformIO 빌드/실행

```sh
pio run -t upload
pio device monitor -b 115200
```

`src/main.cpp`은 프로토콜, bit 수, value, 지원되는 에어컨 상태와 `Source/raw` 데이터를 출력합니다. `UNKNOWN`이어도 마지막 항목에 raw timing 배열이 출력됩니다.

상태별 신호를 세 번씩 LittleFS에 저장할 수 있다. OLED에서 전원, 모드, 온도, 풍량, 스윙, 터보, 예약 종료 목표를 선택하는 방법과 터미널 명령은 [docs/ir-learning.md](docs/ir-learning.md)를 참고합니다.

한국시간 오전 6시 이후 SHT40 온도가 28°C를 초과하면 학습된 냉방 27°C·풍량 1단·스윙 ON·터보 OFF 신호를 하루 한 번 보내는 방법은 [docs/automatic-control.md](docs/automatic-control.md)를 참고합니다.

Arduino IDE를 사용할 경우 보드로 ESP32-C3 Dev Module을 선택하고 Library Manager에서 **IRremoteESP8266**를 설치한 뒤, `src/main.cpp` 내용을 `.ino` 스케치에 복사하면 됩니다.

## 터미널 ON/OFF 송신

HX-53 송신 모듈의 신호 입력을 GPIO4에, GND를 ESP32-C3 GND에 연결한다. 수신부와
송신부는 공통 GND를 사용한다.

펌웨어를 올린 뒤 다음 명령으로 COM46 로그를 연다.

```sh
pio device monitor -p COM46 -b 115200
```

줄 끝을 LF 또는 CRLF로 설정하고 `on` 또는 `off`를 입력하면 각각 냉방·27°C·풍량
1단·스윙 ON·터보 OFF 신호와 자동 OFF 신호를 38 kHz로 송신한다. `help`는 명령 목록을
출력한다. 송신 완료 로그는 ESP32가 IR 배열 전송을 끝냈다는 의미이며, 실제 에어컨 수신은
별도로 확인해야 한다.
