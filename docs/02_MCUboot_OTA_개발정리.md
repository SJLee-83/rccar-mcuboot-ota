# RA6E1 RC카 - MCUboot 부트로더 기반 OTA 구현

> SSAFY 임베디드 트랙 15기 · 1학기 관통 PJT (개인 심화 시도 · 미완성)
> 작성: 이승재(마루)

RC카 SDV 프로젝트의 OTA 기능을 **MCUboot 부트로더 방식**으로 독립 시도한 기록. **미완성 방식**이며, 부트로더 서명 검증과 슬롯 교체(C-7a)까지 검증했으나 앱이 실행 중 스스로 펌웨어를 기록하는 무선 전송(C-7b)에서 코드플래시 self-programming 제약에 막힘. 팀 최종 시연은 완성된 팀원의 Dual-Bank 방식으로 진행.

---

## 1. 접근 방식과 목표

보안 OTA 표준 구성인 **MCUboot 부트로더**의 RA6E1 포팅이 목표.

- Reset 직후 부트로더가 먼저 실행되어 이미지 유효성(서명)을 검증한 뒤 애플리케이션으로 점프
- Primary/Secondary 슬롯 구조로 새 펌웨어를 스테이징 후 교체
- ECDSA P-256 서명 검증으로 승인된 펌웨어만 실행

Dual-Bank 가 하드웨어 뱅크 전환으로 단순한 반면, MCUboot 는 부트로더라는 독립 신뢰 지점을 두어 **서명 검증·롤백 등 보안 기능**을 표준 방식으로 확보.

---

## 2. 시스템 구성

```mermaid
flowchart LR
    G["Qt GUI (PC)"]
    B["Rpi5 브로커<br/>1883"]
    E["ESP32"]
    R["Renesas RA6E1<br/>+ MCUboot"]
    M["MotorHat"]
    W["모터"]

    G -->|MQTT| B
    B -->|MQTT| E
    E -->|SPI| R
    R -->|I2C| M
    M --> W
```

- **MCU**: FPB-RA6E1 (R7FA6E10F2CFP), Cortex-M33, Code Flash 1MB / SRAM 256KB / Data Flash 8KB
- **구동**: 자동차식. 뒷바퀴 DC 모터 구동 + 앞바퀴 서보 조향
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

### PHASE A - Renesas 포팅
FPB-RA6E1 보드에 기본 환경 포팅 완료.

### PHASE B - MQTT 무선 제어 체인
PC GUI → MQTT → ESP32 → SPI → Renesas → 모터 전체 무선 제어 경로 구축. go/back/left/right/mid/stop 명령을 SPI 1바이트(w/x/a/d/s/f)로 매핑.

### PHASE C - MCUboot 부트로더 구축
- C-1~C-6: MCUboot 부트로더 빌드 완료
  - Overwrite 모드 / MbedTLS / **ECDSA P-256 서명** / Flat(Non-TrustZone) / GNU ARM 13.2.1
- 서명 키: `root-ec-p256.pem` (저장소에서 제외, 보안)

### 통합 테스트 - 모터 동작까지 (3가지 원인 순차 해결)

부트로더에서 앱으로 점프가 되지 않던 문제를 3단계로 규명.

1. **TrustZone 경계 손상**: RDPM(Renesas Device Partition Manager)으로 초기화. J-Link 를 RDPM 전에 재연결해 Boot 모드 진입
2. **앱 로드 주소 오류**: 0x10200 이 아니라 **0x10000**(헤더 포함 .bin.signed)으로 로드
3. **부트로더에 `mcuboot_quick_setup()` 호출 누락**: TODO 주석만 있고 부팅 함수 호출이 빠져 startup.c 의 while(1) 로 진입. 호출 추가 후 부트로더 시작 주소가 0xec 에서 0xf48 로 바뀌고 BootApp 도달, 모터 동작 확인

> quick_setup 누락은 공식 튜토리얼(CircuitBread) 저자도 같은 지점에서 지연을 겪은 알려진 함정.

### C-7a - OTA 슬롯 교체 검증

한 디버그 세션에서 슬롯 교체 전체 흐름 검증.

- v1.0(직진) 펌웨어 → Primary(0x10000) 로드
- v1.5(지그재그) 펌웨어 → Secondary(0x88000) 로드
- Resume → 부트로더가 Secondary 의 새 이미지 감지 → Overwrite 로 Primary 에 복사 → v1.5 부팅
- **증거**: Secondary 영역 FF 소거(복사 완료), CPU 의 앱 영역(Non-secure) 실행, **모터 지그재그 동작 확인**

빌드 버전 관리는 v1.0=1.0.0, v1.5=1.5.0. 백업 바이너리 보관(v1_0.bin.signed, v1_5.bin.signed).

**MCUboot 의 서명 검증 → 슬롯 교체 → 새 펌웨어 부팅** 핵심 메커니즘의 실동작 확인.

---

## 5. 직면한 한계 - 코드플래시 self-programming (C-7b, 미완)

C-7a 는 디버거로 Secondary 에 이미지를 심어둔 상태의 슬롯 교체 검증. 무선 OTA 완성에는 **앱이 실행 중 스스로 Secondary(0x88000)에 펌웨어를 기록**하는 단계가 필요하며 여기서 중단.

### 증상
앱에 Flash 드라이버(r_flash_hp, g_flash0)를 추가하고 0x88000에 쓰기를 시도하면 앱이 hang. 모터·SPI 무반응, 디버그 통신 에러("Target disconnected"), 대상 주소는 여전히 FF.

### 근본 원인 - 코드플래시 self-programming 제약
앱이 코드플래시(0x10000)에서 **실행 중**인 상태로 같은 코드플래시(0x88000)에 쓰면 쓰기 동작 중 다음 명령을 fetch 하지 못해 hang. `__disable_irq()` 로 인터럽트를 막아도 `R_FLASH_HP_Write` 와 그 호출자 자체가 코드플래시에 있어 동일 문제 발생.

> 공식 문서: "코드플래시를 수정하려면 관련 코드가 RAM에 상주해야 한다."
> 데이터플래시(8KB, BGO 가능)는 펌웨어(11KB+)를 담기엔 부족.

### 해결 방향
Flash 쓰기 코드의 RAM 실행화.
- 링커 스크립트에 RAM 실행 섹션 추가 + `__attribute__((section(".ramfunc")))`
- ROM→RAM 코드 복사, 필요 시 VTOR RAM 재배치

정밀 작업이라 시연 일정 내 완성이 어려워 중단.

> 팀원의 Dual-Bank 방식은 같은 제약을 **하드웨어 뱅크 전환으로 우회**. Active Bank 실행 중 Inactive Bank 에만 기록하므로 실행 영역 쓰기 문제가 발생하지 않는 다른 해법.

---

## 6. 최종 시연 방식 (반-무선)

C-7b(앱 자체 Flash 쓰기) 중단으로 무선 전송은 일정 내 불가. 개인 방식 시연 구성은 다음과 같음.

- **차량 조종**: 무선 (Qt → MQTT → ESP32 → SPI)
- **펌웨어 교체**: 디버거 Load Ancillary로 Secondary에 이미지 주입 → 부트로더가 슬롯 교체
- **핵심 메시지**: "같은 무선 Go 명령인데, 펌웨어 교체 후 동작이 직진→지그재그로 바뀐다"

촬영은 2클립 구성. (1) v1.0 로드 후 Go 직진, (2) v1.0 + v1.5 로드 후 Resume(부트로더 교체) → Go 지그재그.

개인 방식은 미완성이라 팀 최종 발표 시연은 팀원의 완성된 Dual-Bank 방식으로 진행.

---

## 7. 확보한 기술 범위

- **부트로더 신뢰 체인**: Reset → 부트로더 → 서명 검증 → 앱 점프의 표준 보안 OTA 구조 포팅
- **RA6E1 보안 기능**: TrustZone 경계, RDPM 기반 파티션 초기화, Flat 과 TrustZone 모드 차이
- **ECDSA P-256 서명 검증**: MbedTLS 기반 이미지 서명·검증 파이프라인
- **디버깅 방법론**: 부트로더 시작 주소 추적, 메모리 덤프로 슬롯 매직·헤더 확인, J-Link 통신 이슈 대응
- **self-programming 제약**: 실행 중 코드플래시 쓰기의 하드웨어 한계와 RAM 실행 해법

---

## 8. 남은 로드맵

- **C-7b**: Secondary 쓰기 코드 RAM 실행화 (링커·ramfunc)
- **C-7b-2**: SPI 프로토콜에 'U'=update 모드 추가 (1바이트 RC 명령과 구분)
- **C-7b-3**: 청크 수신(11KB+) + CRC + 0x88000 순차 기록
- **C-7c/d/e**: PC→ESP32→Renesas 무선 펌웨어 전송 통합
- **부속**: 앱 영구 플래시(현재 Load Ancillary는 휘발성), 3D 보드 마운트

---

## 9. 한계와 개선 방향

MCUboot 방식에서 미완으로 남은 항목과 후속 작업 방향.

### 9.1 self-programming 미완

앱이 실행 중 코드플래시(0x88000)에 스스로 쓰는 구간(C-7b)이 RAM 실행 이슈로 미해결. 이 항목 해결이 무선 OTA 완성의 전제.

- **필요 작업**: Flash 쓰기 코드를 `.ramfunc` 로 RAM 배치, 링커 스크립트 수정, ROM → RAM 복사, VTOR 재배치

### 9.2 서명 검증과 무선 전송의 분리

이 방식은 ECDSA P-256 서명 검증을 갖췄으나 무선 전송 미완이고, 팀 Dual-Bank 방식은 무선을 완성했으나 서명 검증 부재. 두 방식이 서로의 공백을 채우는 관계.

- **개선 방향**: MCUboot 서명 검증과 Dual-Bank 무선 전송을 결합한 보안 OTA 구성

### 9.3 롤백 미구현

C-7a 에서 슬롯 교체는 검증했으나 Overwrite 모드라 롤백 부재. 새 펌웨어 부팅 실패 시 복구 불가.

- **개선 방향**: MCUboot swap 모드 전환으로 이미지 confirm/revert 기반 자동 롤백 확보

### 9.4 후속 작업 축 정리

1. self-programming 해결(RAM 실행)로 무선 완성
2. 서명 검증과 무선 전송 결합으로 보안 OTA 구성
3. swap 모드 롤백으로 fail-safe 확보

---

## 10. 저장소 구성

```
rccar-mcuboot-ota/
├── ra_mcuboot_rccar/     # MCUboot 부트로더 프로젝트
├── 1st_pjt_rccar_ota/    # RC카 애플리케이션 (모터/SPI/I2C 제어)
├── gateway/              # 무선 제어 체인 (PySide6 GUI + ESP32)
└── docs/                 # 개발 기록
```

> **보안 주의**: 서명 키 `root-ec-p256.pem`은 `.gitignore`로 제외됨. 저장소에 포함되지 않음.

**→ 팀 전체 프로젝트: [rccar_ota_project](https://github.com/SJLee-83/rccar_ota_project)**
