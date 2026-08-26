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

기본 화면에서 CONFIRM을 누르면 `AUTO SETTINGS`가 바로 열린다. 노브를 눌러 `MAIN MENU`로 들어간 뒤 `AUTO SETTINGS`를 선택해도 같은 화면을 열 수 있다.

`AUTO SETTINGS`에서 다음 값을 변경한다.

| 항목 | 범위와 동작 |
| --- | --- |
| `ENABLE` | 자동 ON 기능을 ON/OFF 한다. |
| `START` | 모니터링 시작 시각을 00:00~23:59 범위에서 설정한다. |
| `ON TEMP` | ON 기준 온도를 16.0~35.0°C 범위에서 0.5°C 단위로 설정한다. |
| `SAVE & EXIT` | 설정을 비휘발성 저장소에 저장하고 메인 메뉴로 돌아간다. |

노브 회전으로 항목을 이동하고 PUSH 또는 CONFIRM으로 선택한다. START는 시, 분 순서로 편집한다. `*` 표시는 현재 값을 편집 중이라는 의미다. 편집 중 BACK을 누르면 해당 항목 편집을 끝내고, 다시 BACK을 누르면 이번 화면에서 바꾼 값을 모두 버리고 이전 화면으로 돌아간다. `SAVE & EXIT`를 선택한 경우에만 저장되며 저장한 설정은 재부팅 후에도 유지된다.

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

### 펌웨어 자격 증명 확인

`wifi_secrets.h`를 수정한 뒤에는 PlatformIO가 이전 오브젝트 파일을 재사용하지 않도록 클린 빌드한다.

```powershell
pio run -t clean
pio run -t upload
pio device monitor -b 115200
```

부팅 로그 또는 시리얼 명령 `wifi info`에서 펌웨어 빌드 시각, SSID, 비밀번호 바이트 수와 8자리 진단 지문을 확인할 수 있다. 비밀번호 원문은 출력하지 않는다. PC에서 다음 스크립트를 실행하면 현재 `wifi_secrets.h`로 동일한 지문을 계산한다.

```powershell
.\scripts\wifi-credential-diagnostic.ps1
```

PC와 ESP32의 `password bytes`와 `fingerprint`가 모두 같으면 현재 펌웨어에 동일한 접속 정보가 포함된 것이다. 지문은 설정 불일치 확인용이며 비밀번호를 대신하는 인증 정보로 사용하지 않는다.

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

연결 문제를 확인할 때 시리얼 모니터에서 `wifi info`를 실행하면 펌웨어에 포함된 자격 증명의 진단 정보를 출력하고, `wifi scan`을 실행하면 ESP32-C3가 감지한 주변 SSID, 채널, RSSI와 보안 방식 번호를 출력한다. `wifi detail`은 자동 재연결을 잠시 중단한 뒤 설정 SSID만 스캔하고, AP의 BSSID·암호화 방식·PHY·채널 폭을 출력한 다음 가장 강한 BSSID에 고정한 단일 연결 시험을 수행한다. 비밀번호 원문은 어떤 진단에서도 출력하지 않는다. 스캔이나 상세 시험이 끝난 뒤 연결되지 않은 상태라면 기존 30초 재시도 흐름으로 돌아간다. 연결이 끊기거나 인증에 실패하면 사유 이름, BSSID와 RSSI도 로그에 출력한다.

일부 ESP32-C3 SuperMini 생산분은 RF 레이아웃 문제로 기본 송신 출력에서 인증 응답을 받지 못할 수 있다. 이 프로젝트는 Wi-Fi 초기화 직후 송신 출력을 `WIFI_POWER_8_5dBm`으로 제한한다. 실제 대상 보드에서는 제한 전 `AUTH_EXPIRE`가 반복됐지만, 제한 후 WPA2 연결과 DHCP 주소 할당까지 약 1.4초 안에 완료됐다. 외장 안테나 보드나 RF 문제가 없는 보드에서 출력 범위를 늘리려면 먼저 동일 장소에서 장시간 연결 안정성을 확인한다.
