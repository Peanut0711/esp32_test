# TODO

## 다음 작업: 적외선 송신 검증

- [ ] HX-53 송신 모듈을 GPIO4와 공통 GND에 연결한다.
- [ ] `esp32-ac-controller/docs/ir-captures.md`의 자동 ON/OFF raw 배열을 펌웨어에 추가한다.
- [ ] GPIO4에서 38 kHz로 raw 배열 전체를 송신하는 함수를 구현한다.
- [ ] Serial Monitor의 `on`, `off` 명령으로 수동 송신을 시험한다.
- [ ] 자동 ON 신호가 냉방 / 27°C / 풍량 1단 / 스윙 ON / 터보 OFF 상태를 재현하는지 확인한다.
- [ ] 자동 OFF 신호가 전원을 끄는지 확인한다.
- [ ] 수신 거리와 송신 가능 거리를 기록한다.

## 이후 작업

- [ ] SHT40을 I2C SDA GPIO8, SCL GPIO9에 연결한다.
- [ ] 온도 기준 자동 ON/OFF 조건과 히스테리시스를 정한다.
- [ ] 반복 송신 방지용 동작 상태를 구현한다.
