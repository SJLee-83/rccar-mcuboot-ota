# RA6E1 RC카 — MCUboot 부트로더 기반 OTA 구현

> SSAFY 임베디드 트랙 15기 · 1학기 관통 PJT (개인 심화 시도 · 미완성)
> 작성: 이승재(마루)

RC카 SDV 프로젝트의 OTA 기능을 **MCUboot 부트로더 방식**으로 독립 시도한 기록이다. **이 방식은 완성되지 못했다** — 부트로더 서명 검증과 슬롯 교체(C-7a)까지는 검증했으나, 앱이 실행 중 스스로 펌웨어를 기록하는 무선 전송 부분(C-7b)에서 코드플래시 self-programming 제약에 막혔다. 팀 최종 시연은 완성된 팀원의 Dual-Bank 방식으로 진행되었다. 이 문서는 도달한 지점까지의 기술적 성과와, 완성을 막은 한계, 그리고 발전 방향을 정리한다.

---

## 1. 접근 방식과 목표

일반적인 보안 OTA의 정석인 **MCUboot 부트로더**를 RA6E1에 포팅하는 것을 목표로 했다.

- Reset 직후 부트로더가 먼저 실행되어 이미지 유효성(서명)을 검증한 뒤 애플리케이션으로 점프
- Primary/Secondary 슬롯 구조로 새 펌웨어를 스테이징 후 교체
- ECDSA P-256 서명 검증으로 승인된 펌웨어만 실행

Dual-Bank 방식이 하드웨어 뱅크 전환으로 단순하다면, MCUboot 방식은 부트로더라는 독립 신뢰 지점을 두어 **서명 검증·롤백 등 보안 기능**을 표준적으로 확보할 수 있다는 장점이 있다.

---

## 2. 시스템 구성

```
[Qt GUI(PC)] ─MQTT─▶ [Rpi5 브로커 1883] ─MQTT─▶ [ESP32] ─SPI─▶ [Renesas RA6E1 + MCUboot] ─I2C─▶ [MotorHat] ─▶ 모터
```

- **MCU**: FPB-RA6E1 (R7FA6E10F2CFP), Cortex-M33, Code Flash 1MB / SRAM 256KB / Data Flash 8KB
- **구동**: 자동차식 — 뒷바퀴 DC 모터 구동 + 앞바퀴 서보 조향
- **조향(서보, ch0)**: Left=300, Mid=380, Right=440 (setPWM)
- **구동(DC)**: IN1=12, IN2=11, PWM=13
- **I2C**: MotorHat 주소 0x6f, P400=SCL / P401=SDA
- **SPI (Slave↔ESP32 Master)**: P100=MISO, P101=MOSI, P102=SCK, P402=CS. GND 공통 필수.

---

## 3. Flash 메모리 레이아웃

MCUboot 표준 슬롯 구조 (Overwrite 방식):

| 영역 | 주소 범위 | 크기 | 역할 |
|------|-----------|------|------|
| Bootloader | 0x00000 ~ 0x10000 | 64KB | MCUboot 부트로더 |
| Primary Slot | 0x10000 ~ 0x88000 | 480KB | 실행 중인 펌웨어 |
| Secondary Slot | 0x88000 ~ 0x100000 | 480KB | 새 펌웨어 스테이징 |

- 이미지 헤더 = 슬롯 시작 주소 (매직 넘버 `96F3B83D`)
- 코드 본체 = 슬롯 시작 + 0x200
- 부트로더가 Secondary에 유효한 새 이미지를 감지하면 Overwrite로 Primary에 복사 후 부팅

---

## 4. 구현 단계 및 완료 내역

### PHASE A — Renesas 포팅
FPB-RA6E1 보드에 기본 환경 포팅 완료.

### PHASE B — MQTT 무선 제어 체인
PC GUI → MQTT → ESP32 → SPI → Renesas → 모터의 전체 무선 제어 경로 구축. go/back/left/right/mid/stop 명령을 SPI 1바이트(w/x/a/d/s/f)로 매핑.

### PHASE C — MCUboot 부트로더 구축
- C-1~C-6: MCUboot 부트로더 빌드 완료
  - Overwrite 모드 / MbedTLS / **ECDSA P-256 서명** / Flat(Non-TrustZone) / GNU ARM 13.2.1
- 서명 키: `root-ec-p256.pem` (저장소에서 제외, 보안)

### 통합 테스트 — 모터 동작까지 (3가지 원인 순차 해결) ✅

부트로더에서 앱으로 점프가 안 되던 문제를 3단계로 규명:

1. **TrustZone 경계 손상** → RDPM(Renesas Device Partition Manager)으로 초기화. J-Link를 RDPM 전에 뽑았다 꽂아 Boot 모드 진입.
2. **앱 로드 주소 오류** → 0x10200이 아니라 **0x10000** (헤더 포함 .bin.signed)으로 로드.
3. **결정타 — 부트로더에 `mcuboot_quick_setup()` 호출 누락** → TODO 주석만 있고 실제 부팅 함수 호출이 빠져 startup.c의 while(1)로 빠짐. 함수 정의는 hal_entry 밖에 있는데 호출을 hal_entry 안에서 안 함. 추가하니 부트로더 시작 주소 0xec→0xf48로 바뀌고 BootApp 도달, 모터 동작 확인.

> 이 quick_setup 누락은 공식 튜토리얼(CircuitBread) 저자도 1시간 헤맨 알려진 함정.

### C-7a — OTA 슬롯 교체 검증 ✅ (핵심 성과)

한 디버그 세션에서 슬롯 교체 전체 흐름을 검증:

- v1.0(직진) 펌웨어 → Primary(0x10000) 로드
- v1.5(지그재그) 펌웨어 → Secondary(0x88000) 로드
- Resume → 부트로더가 Secondary의 새 이미지 감지 → Overwrite로 Primary에 복사 → v1.5 부팅
- **증거**: Secondary 영역이 FF로 비워짐(복사 완료), CPU가 앱 영역(Non-secure) 실행, **모터가 지그재그로 동작 확인**

빌드 버전 관리: v1.0=1.0.0, v1.5=1.5.0. 백업 바이너리 보관(v1_0.bin.signed, v1_5.bin.signed).

이로써 **MCUboot의 서명 검증 → 슬롯 교체 → 새 펌웨어 부팅**의 핵심 메커니즘이 실제로 동작함을 확인했다.

---

## 5. 직면한 한계 — 코드플래시 self-programming (C-7b, 미완)

C-7a는 "디버거로 Secondary에 이미지를 심어두고" 슬롯 교체를 검증한 것이다. 진짜 무선 OTA를 완성하려면 **앱이 실행 중에 스스로 Secondary(0x88000)에 펌웨어를 기록**해야 하는데, 여기서 막혔다.

### 증상
앱에 Flash 드라이버(r_flash_hp, g_flash0)를 추가하고 0x88000에 쓰기를 시도하면 앱이 hang. 모터·SPI 무반응, 디버그 통신 에러("Target disconnected"), 대상 주소는 여전히 FF.

### 근본 원인 — 코드플래시 self-programming 제약
앱이 코드플래시(0x10000)에서 **실행 중**인 상태에서 같은 코드플래시(0x88000)에 쓰면, 쓰기 동작 중 자기 다음 명령을 fetch할 수 없어 hang된다. `__disable_irq()`로 인터럽트는 막았지만, `R_FLASH_HP_Write` 함수와 그 호출자 자체가 코드플래시에 있어서 문제가 발생한다.

> 공식 문서: "코드플래시를 수정하려면 관련 코드가 RAM에 상주해야 한다."
> 데이터플래시(8KB, BGO 가능)는 펌웨어(11KB+)를 담기엔 부족.

### 해결 방향 (다음 과제)
Flash 쓰기 코드를 RAM에서 실행되게 하는 것:
- 링커 스크립트에 RAM 실행 섹션 추가 + `__attribute__((section(".ramfunc")))`
- ROM→RAM 코드 복사, 필요 시 VTOR RAM 재배치

정밀 작업이라 시연 일정 내 완성이 어려워 여기서 중단했다.

> **주목할 점**: 팀원의 Dual-Bank 방식은 바로 이 self-programming 제약을 **하드웨어 뱅크 전환으로 우회**한다. Active Bank는 계속 실행하면서 Inactive Bank에만 기록하므로 "실행 중인 영역에 쓰는" 문제가 발생하지 않는다. 같은 문제에 대한 두 가지 다른 해법인 셈이다.

---

## 6. 최종 시연 방식 (반-무선)

C-7b(앱 자체 Flash 쓰기)가 막혀 진짜 무선 전송은 시간 내 불가능했다. 개인 방식의 시연은 다음과 같이 구성했다:

- **차량 조종**: 무선 (Qt → MQTT → ESP32 → SPI)
- **펌웨어 교체**: 디버거 Load Ancillary로 Secondary에 이미지 주입 → 부트로더가 슬롯 교체
- **핵심 메시지**: "같은 무선 Go 명령인데, 펌웨어 교체 후 동작이 직진→지그재그로 바뀐다"

2-클립 방식으로 촬영: (1) v1.0 로드→Go 직진, (2) v1.0+v1.5 로드→Resume(부트로더 교체)→Go 지그재그.

이 개인 방식은 미완성이라, 팀 최종 발표 시연은 팀원의 완성된 Dual-Bank 방식으로 진행되었다.

---

## 7. 배운 점

- **부트로더 신뢰 체인**: Reset → 부트로더 → 서명 검증 → 앱 점프의 표준 보안 OTA 구조를 직접 포팅하며 이해.
- **RA6E1 보안 기능**: TrustZone 경계, RDPM을 통한 파티션 초기화, Flat vs TrustZone 모드 차이.
- **ECDSA P-256 서명 검증**: MbedTLS 기반 이미지 서명·검증 파이프라인.
- **디버깅 방법론**: 부트로더 시작 주소 추적, 메모리 덤프(슬롯 매직/헤더 확인), J-Link 통신 이슈 대응.
- **self-programming 제약의 본질**: 실행 중 코드플래시 쓰기의 하드웨어적 한계와 RAM 실행 해법. 이 한계를 이해했기에 팀원의 Dual-Bank 우회 방식의 가치도 명확히 평가할 수 있었다.

---

## 8. 남은 로드맵

- **C-7b**: Secondary 쓰기 코드 RAM 실행화 (링커·ramfunc)
- **C-7b-2**: SPI 프로토콜에 'U'=update 모드 추가 (1바이트 RC 명령과 구분)
- **C-7b-3**: 청크 수신(11KB+) + CRC + 0x88000 순차 기록
- **C-7c/d/e**: PC→ESP32→Renesas 무선 펌웨어 전송 통합
- **부속**: 앱 영구 플래시(현재 Load Ancillary는 휘발성), 3D 보드 마운트

---

## 9. 한계 및 개선 여지 (회고)

다음 프로젝트 주제 선정을 위한 회고 재료로, MCUboot 방식에서 미완으로 남은 부분과 발전 방향을 정리한다.

### 9.1 self-programming 미완 (핵심 기술 부채)

앱이 실행 중 코드플래시(0x88000)에 스스로 쓰는 부분(C-7b)이 RAM 실행 이슈로 막혀 있다. 이것이 해결되어야 "진짜 무선 OTA"가 완성된다.

- **필요 작업**: Flash 쓰기 코드를 RAM에 배치(`.ramfunc`), 링커 스크립트 수정, ROM→RAM 복사, VTOR 재배치
- **의미**: Cortex-M 펌웨어의 실행 위치 제어와 부트 과정에 대한 깊은 이해가 필요한 심화 주제. 개인 임베디드 역량 강화의 명확한 다음 목표.

### 9.2 서명 검증은 됐으나 무선이 미완 — 팀 방식과 상호보완

이 방식은 ECDSA P-256 서명 검증을 갖췄지만 무선 전송을 완성하지 못했고, 팀의 Dual-Bank 방식은 무선은 완성했지만 서명 검증이 없다. 두 방식은 서로의 빈 곳을 채우는 관계다.

→ **발전 방향**: MCUboot의 서명 검증 + Dual-Bank의 무선 전송 경험을 결합하면 "무선으로 완성되면서 서명 검증까지 갖춘 보안 OTA"가 된다. 이는 실무 SDV OTA에 가장 근접한 형태다.

### 9.3 롤백 미구현

C-7a에서 슬롯 교체는 검증했으나 Overwrite 모드라 롤백이 없다. 새 펌웨어 부팅 실패 시 복구 불가.

→ **발전 방향**: MCUboot의 **swap 모드**로 전환하면 이미지 confirm/revert 기반 자동 롤백을 표준적으로 구현할 수 있다. Overwrite→swap 전환 자체가 하나의 학습 주제.

### 9.4 정리 — 이 방식이 남긴 발전 축

1. self-programming 해결 (RAM 실행) → 무선 완성
2. 서명 검증 + 무선의 결합 (보안 OTA)
3. swap 모드 롤백 (fail-safe)

세 축 모두 "이번에 여기까지 했으니 다음엔 이걸 넘어선다"는 연속성이 있어, 차기 주제의 재료가 된다.

---

## 10. 저장소 구성

| 폴더 | 내용 |
|------|------|
| `ra_mcuboot_rccar/` | MCUboot 부트로더 프로젝트 |
| `1st_pjt_rccar_ota/` | RC카 애플리케이션 (모터/SPI/I2C 제어) |

> **보안 주의**: 서명 키 `root-ec-p256.pem`은 `.gitignore`로 제외됨. 저장소에 포함되지 않음.

**→ 팀 전체 프로젝트: [rccar_ota_project](https://github.com/SJLee-83/rccar_ota_project)**
