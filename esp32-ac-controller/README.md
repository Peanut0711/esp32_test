# ESP32-C3 SuperMini 에어컨 IR 제어

현재는 **1단계: 적외선 수신/분석**만 포함합니다.

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

Arduino IDE를 사용할 경우 보드로 ESP32-C3 Dev Module을 선택하고 Library Manager에서 **IRremoteESP8266**를 설치한 뒤, `src/main.cpp` 내용을 `.ino` 스케치에 복사하면 됩니다.
