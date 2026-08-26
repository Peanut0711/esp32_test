# 오전 6시 이후 온도 자동 ON

펌웨어는 Wi-Fi와 NTP로 한국시간을 확인하고 다음 조건이 모두 만족되면 하루에 한 번 에어컨 ON 신호를 보낸다.

- 한국시간이 06:00 이상 24:00 미만이다.
- SHT40 측정 온도가 정확히 28.0°C보다 높다.
- 현재 날짜에 자동 ON 신호를 아직 보내지 않았다.
- IR 학습이 진행 중이지 않다.

ON에는 LittleFS의 `cool_27_f1_swing_on_turbo_off`, 수동 OFF에는 `power_off` 학습 레코드의 첫 번째 샘플을 사용한다. 해당 레코드를 찾지 못하면 소스에 포함된 기존 raw 배열을 대신 사용한다.

이번 단계에는 자동 OFF 조건을 넣지 않았다. 날짜가 바뀌면 다음 날 자동 ON을 다시 허용하며, 재부팅하면 당일 송신 여부가 초기화되므로 조건이 맞을 때 ON 신호를 한 번 더 보낼 수 있다. ON 신호는 전체 상태 패킷이어서 같은 설정을 다시 보내도 설정값 자체는 바뀌지 않는다.

## Wi-Fi 설정

`include/wifi_secrets.example.h`를 `include/wifi_secrets.h`로 복사하고 실제 접속 정보를 입력한다.

```powershell
Copy-Item include\wifi_secrets.example.h include\wifi_secrets.h
```

```cpp
#pragma once

constexpr char kWifiSsid[] = "SSID";
constexpr char kWifiPassword[] = "PASSWORD";
```

`wifi_secrets.h`는 `.gitignore`에 등록되어 커밋되지 않는다. 설정 파일이 없거나 SSID가 비어 있으면 자동제어는 비활성화되고 OLED에 `NO WIFI CFG`가 표시된다.

## OLED 상태

| 표시 | 의미 |
| --- | --- |
| `WIFI WAIT` | Wi-Fi 연결 대기 또는 재연결 중 |
| `TIME SYNC` | NTP 시간 동기화 대기 중 |
| `WAIT 06:00` | 시간이 오전 6시 이전임 |
| `TEMP WAIT` | 시간이 유효하지만 온도가 28°C 이하임 |
| `ON READY` | 자동 ON 조건이 성립해 송신을 요청함 |
| `ON SENT` | 오늘 자동 ON 송신을 완료함 |
| `SENSOR ERR` | 온도 측정값이 유효하지 않음 |

업로드 후 시리얼 로그에서 Wi-Fi IP와 `NTP synchronization requested`를 확인하고, OLED의 시간이 한국시간과 일치하는지 확인한다.
