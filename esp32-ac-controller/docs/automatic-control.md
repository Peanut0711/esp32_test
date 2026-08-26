# 시간·온도 자동 ON

펌웨어는 Wi-Fi와 NTP로 한국시간을 확인하고 다음 조건이 모두 만족되면 하루에 한 번 에어컨 ON 신호를 보낸다. 기본값은 자동제어 ON, 시작 시각 06:00, 기준 온도 28.0°C다.

- 한국시간이 OLED에서 설정한 시작 시각 이상이다.
- SHT40 측정 온도가 OLED에서 설정한 기준 온도보다 높다.
- 현재 날짜에 자동 ON 신호를 아직 보내지 않았다.
- IR 학습이 진행 중이지 않다.

ON에는 LittleFS의 `cool_27_f1_swing_on_turbo_off`, 수동 OFF에는 `power_off` 학습 레코드의 첫 번째 샘플을 사용한다. 해당 레코드를 찾지 못하면 소스에 포함된 기존 raw 배열을 대신 사용한다.

이번 단계에는 자동 OFF 조건을 넣지 않았다. 날짜가 바뀌면 다음 날 자동 ON을 다시 허용하며, 재부팅하면 당일 송신 여부가 초기화되므로 조건이 맞을 때 ON 신호를 한 번 더 보낼 수 있다. ON 신호는 전체 상태 패킷이어서 같은 설정을 다시 보내도 설정값 자체는 바뀌지 않는다.

## OLED 메뉴

메인 화면에서 노브를 눌러 메뉴를 연다.

### 시계 조회

기본 화면 상단에는 한국시간과 Wi-Fi 연결 상태가 항상 표시된다. 연결된 경우 `W:OK`와 RSSI 신호 세기가 나오고, 연결 중이면 `W:WAIT`, 접속 정보가 없으면 `W:CFG?`가 나온다.

`CLOCK`을 선택하면 한국 날짜와 초 단위 현재 시각, Wi-Fi 연결 상태, RSSI와 IP 주소를 확인할 수 있다. BACK으로 메인 메뉴에 돌아간다. 시간이 아직 준비되지 않았으면 `TIME NOT SYNCED`와 연결 상태가 표시된다.

### 자동제어 설정

`AUTO SETTINGS`에서 다음 값을 변경한다.

| 항목 | 범위와 동작 |
| --- | --- |
| `ENABLE` | 자동 ON 기능을 ON/OFF 한다. |
| `START` | 모니터링 시작 시각을 00:00~23:59 범위에서 설정한다. |
| `ON TEMP` | ON 기준 온도를 16.0~35.0°C 범위에서 0.5°C 단위로 설정한다. |
| `SAVE & EXIT` | 설정을 비휘발성 저장소에 저장하고 메인 메뉴로 돌아간다. |

노브 회전으로 항목을 이동하고 PUSH 또는 CONFIRM으로 선택한다. START는 시, 분 순서로 편집한다. 값을 저장하지 않고 나가려면 편집 중이 아닐 때 BACK을 누른다. 저장한 설정은 재부팅 후에도 유지된다.

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
| `AUTO OFF` | OLED 설정에서 자동제어를 끈 상태 |
| `TIME WAIT` | 현재 시각이 설정한 시작 시각 이전임 |
| `TEMP WAIT` | 시간이 유효하지만 온도가 설정값 이하임 |
| `ON READY` | 자동 ON 조건이 성립해 송신을 요청함 |
| `ON SENT` | 오늘 자동 ON 송신을 완료함 |
| `SENSOR ERR` | 온도 측정값이 유효하지 않음 |

업로드 후 시리얼 로그에서 Wi-Fi IP와 `NTP synchronization requested`를 확인하고, OLED의 시간이 한국시간과 일치하는지 확인한다.
