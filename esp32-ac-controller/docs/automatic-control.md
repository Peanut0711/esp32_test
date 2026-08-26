# 조건별 자동 ON과 송신 프로필

기본값은 자동제어 ON, 조건 `BOTH`, 시작 시각 06:00, 기준 온도 28.0°C다. OLED에서 다음 세 조건 중 하나를 선택할 수 있다.

| 조건 | 동작 |
| --- | --- |
| `BOTH` | 설정 시각 이후이면서 측정 온도가 기준보다 높을 때 하루 한 번 ON |
| `TIME` | 온도와 무관하게 설정 시각이 되면 하루 한 번 ON |
| `TEMP` | 시각 및 Wi-Fi 연결과 무관하게 측정 온도가 기준보다 높아지면 ON |

`TEMP` 조건은 ON을 한 번 보낸 뒤 측정 온도가 기준보다 0.5°C 이상 낮아져야 다시 대기 상태로 돌아간다. 이 히스테리시스로 기준값 주변에서 같은 신호가 반복되는 것을 막는다. 이 상태는 Deep Sleep 동안 유지되지만 전원이 완전히 끊기거나 펌웨어를 다시 올리면 초기화될 수 있다.

자동 ON에는 OLED에서 저장한 사용자 지정 IR 프로필을 사용한다. 초기 프로필은 `cool_27_f1_swing_on_turbo_off`이며, 이 초기 항목만 학습 데이터가 없을 때 소스의 raw 배열을 대신 사용할 수 있다. 사용자가 저장한 다른 프로필이 LittleFS에 없으면 잘못된 상태를 보내지 않는다. 수동 OFF는 `power_off`를 사용한다.

이번 단계에는 자동 OFF 조건을 넣지 않았다. `BOTH`와 `TIME`은 날짜가 바뀌면 다음 송신을 허용한다. 당일 송신 여부는 Deep Sleep 동안 유지되지만 전원이 완전히 끊기거나 펌웨어를 다시 올리면 초기화될 수 있으므로, 이미 조건이 맞으면 한 번 더 송신할 수 있다. 모든 ON 신호는 전체 상태 패킷이다.

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
| `COND` | `BOTH`, `TIME`, `TEMP` 중 자동 ON 조건을 선택한다. |
| `START` | 모니터링 시작 시각을 00:00~23:59 범위에서 설정한다. |
| `TEMP` | ON 기준 온도를 16.0~35.0°C 범위에서 0.5°C 단위로 설정한다. |
| `SLEEP` | 무조작 시 Deep Sleep에 들어가는 배터리 절전 기능을 ON/OFF 한다. |
| `WAKE` | 온도와 조건을 검사할 기상 간격을 1~5분으로 설정한다. |
| `SAVE & EXIT` | 설정을 비휘발성 저장소에 저장하고 메인 메뉴로 돌아간다. |

노브 회전으로 항목을 이동하고 PUSH 또는 CONFIRM으로 선택한다. START는 시, 분 순서로 편집한다. `*` 표시는 현재 값을 편집 중이라는 의미다. 편집 중 BACK을 누르면 해당 항목 편집을 끝내고, 다시 BACK을 누르면 이번 화면에서 바꾼 값을 모두 버리고 이전 화면으로 돌아간다. `SAVE & EXIT`를 선택한 경우에만 저장되며 저장한 설정은 재부팅 후에도 유지된다.

### 자동 ON 프로필 저장

1. `IR TRANSMIT`에서 자동으로 켤 전원·모드·온도·풍량·스윙·터보를 선택한다.
2. 필요하면 `SEND`로 실제 동작을 먼저 확인한다.
3. `SET AUTO PROFILE`을 선택한다.
4. `AUTO PROFILE: SAVED`가 표시되면 저장이 완료된 것이다.

현재 전체 상태와 정확히 일치하는 학습 레코드가 있어야 저장된다. 없으면 `AUTO PROFILE: NO IR`가 표시되며, `LEARN CURRENT`로 먼저 학습해야 한다. 선택한 프로필 이름은 자동제어 조건과 함께 재부팅 후에도 유지된다.

시리얼 모니터에서 `auto info`를 입력하면 현재 활성화 여부, 조건, 시각, 온도와 저장된 프로필 이름을 확인할 수 있다.

배터리 절전 동작과 버튼 기상 방법은 [power-saving.md](power-saving.md)를 참고한다.

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

`wifi_secrets.h`는 `.gitignore`에 등록되어 커밋되지 않는다. 설정 파일이 없거나 SSID가 비어 있으면 `BOTH`와 `TIME` 조건은 동작하지 않고 OLED에 `NO WIFI CFG`가 표시된다. `TEMP` 조건은 시계가 필요하지 않으므로 Wi-Fi 없이도 동작한다.

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
| `TEMP SENT` | 온도 전용 조건으로 ON을 보낸 뒤 재무장 온도를 기다리는 중 |
| `ON READY` | 자동 ON 조건이 성립해 송신을 요청함 |
| `ON SENT` | 오늘 자동 ON 송신을 완료함 |
| `SENSOR ERR` | 온도 측정값이 유효하지 않음 |

업로드 후 시리얼 로그에서 Wi-Fi IP와 `NTP synchronization requested`를 확인하고, OLED의 시간이 한국시간과 일치하는지 확인한다.

연결 문제를 확인할 때 시리얼 모니터에서 `wifi info`를 실행하면 펌웨어에 포함된 자격 증명의 진단 정보를 출력하고, `wifi scan`을 실행하면 ESP32-C3가 감지한 주변 SSID, 채널, RSSI와 보안 방식 번호를 출력한다. `wifi detail`은 자동 재연결을 잠시 중단한 뒤 설정 SSID만 스캔하고, AP의 BSSID·암호화 방식·PHY·채널 폭을 출력한 다음 가장 강한 BSSID에 고정한 단일 연결 시험을 수행한다. 비밀번호 원문은 어떤 진단에서도 출력하지 않는다. 스캔이나 상세 시험이 끝난 뒤 연결되지 않은 상태라면 기존 30초 재시도 흐름으로 돌아간다. 연결이 끊기거나 인증에 실패하면 사유 이름, BSSID와 RSSI도 로그에 출력한다.

일부 ESP32-C3 SuperMini 생산분은 RF 레이아웃 문제로 기본 송신 출력에서 인증 응답을 받지 못할 수 있다. 이 프로젝트는 Wi-Fi 초기화 직후 송신 출력을 `WIFI_POWER_8_5dBm`으로 제한한다. 실제 대상 보드에서는 제한 전 `AUTH_EXPIRE`가 반복됐지만, 제한 후 WPA2 연결과 DHCP 주소 할당까지 약 1.4초 안에 완료됐다. 외장 안테나 보드나 RF 문제가 없는 보드에서 출력 범위를 늘리려면 먼저 동일 장소에서 장시간 연결 안정성을 확인한다.
