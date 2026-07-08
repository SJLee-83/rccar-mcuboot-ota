# RA6E1 RC카 — MCUboot 부트로더 기반 OTA

Renesas RA6E1(FPB-RA6E1) RC카에 **MCUboot 부트로더 기반 보안 OTA**를 구현한 개인 심화 저장소입니다. SSAFY 임베디드 트랙 15기 1학기 관통 PJT에서, 팀의 무선 OTA 기능을 MCUboot 방식으로 독립 시도한 기록입니다.

> ⚠️ **이 방식은 미완성입니다.** 부트로더 서명 검증과 슬롯 교체(C-7a)까지는 검증했으나, 앱이 실행 중 스스로 펌웨어를 기록하는 무선 전송 부분(C-7b)에서 코드플래시 self-programming 제약에 막혔습니다. 팀 최종 시연은 완성된 팀원의 Dual-Bank 방식으로 진행되었습니다. 자세한 내용은 [개발 정리 문서](docs/02_MCUboot_OTA_개발정리.md)를 참고하세요.

---

## 무엇을 했나

- **MCUboot 부트로더 포팅**: Reset 직후 부트로더가 먼저 실행되어 이미지 서명을 검증한 뒤 애플리케이션으로 점프하는 표준 보안 부트 구조를 RA6E1에 포팅
- **ECDSA P-256 서명 검증**: MbedTLS 기반으로 승인된 펌웨어만 부팅
- **슬롯 교체 검증 (C-7a)**: Primary/Secondary 슬롯 구조에서 새 이미지를 스테이징하고, 부트로더가 감지·복사·부팅하는 전 과정을 실제로 검증 (모터 동작 v1.0 직진 → v1.5 지그재그 전환 확인)

## 어디서 막혔나

앱이 실행 중인 코드플래시(0x10000)에서 같은 코드플래시(0x88000)에 직접 쓰면, 쓰기 동작 중 다음 명령을 fetch할 수 없어 hang됩니다. 이를 풀려면 Flash 쓰기 코드를 RAM에서 실행해야 하는데(링커 스크립트 + `.ramfunc`), 시연 일정 내 완성이 어려워 중단했습니다.

> 팀원의 Dual-Bank 방식은 이 self-programming 제약을 하드웨어 뱅크 전환으로 우회합니다. 같은 문제에 대한 두 가지 다른 해법입니다.

---

## 저장소 구성

```
rccar-mcuboot-ota/
├── ra_mcuboot_rccar/     # MCUboot 부트로더 (Flash 0x0~0x10000)
├── 1st_pjt_rccar_ota/    # RC카 애플리케이션 (모터/SPI/I2C 제어, Flash 0x10000~)
└── docs/
    └── 02_MCUboot_OTA_개발정리.md   # 상세 개발 기록
```

| 폴더 | 역할 |
|------|------|
| `ra_mcuboot_rccar` | 부트로더. 서명 검증 후 앱으로 점프. mbedtls, MCUboot, ECDSA 포함 |
| `1st_pjt_rccar_ota` | 애플리케이션. 부트로더가 부팅시키는 대상. SPI 명령 수신 → 모터 제어 |

## Flash 메모리 레이아웃

| 영역 | 주소 | 크기 | 역할 |
|------|------|------|------|
| Bootloader | 0x00000 ~ 0x10000 | 64KB | MCUboot 부트로더 |
| Primary Slot | 0x10000 ~ 0x88000 | 480KB | 실행 중인 펌웨어 |
| Secondary Slot | 0x88000 ~ 0x100000 | 480KB | 새 펌웨어 스테이징 |

- 이미지 헤더 = 슬롯 시작 (매직 `96F3B83D`), 코드 본체 = 슬롯 시작 + 0x200
- Overwrite 모드: 부트로더가 Secondary의 유효 이미지를 Primary로 복사 후 부팅

---

## 하드웨어

- **MCU**: FPB-RA6E1 (R7FA6E10F2CFP), Cortex-M33, Code Flash 1MB / SRAM 256KB
- **구동**: 자동차식 — 뒷바퀴 DC 모터 + 앞바퀴 서보 조향
- **모터 제어**: PCA9685 Motor HAT, I2C 0x6f (P400=SCL, P401=SDA)
  - 조향(서보 ch0): Left=300, Mid=380, Right=440
  - 구동(DC): IN1=12, IN2=11, PWM=13
- **명령 수신**: SPI Slave (ESP32 Master). w=전진, x=후진, a=좌, d=우, s=중립, f=정지

## 빌드 환경

- e2 studio + FSP
- GNU ARM 13.2.1, Flat (Non-TrustZone) 구성
- 부트로더는 `mcuboot_quick_setup()` → `RM_MCUBOOT_PORT_BootApp()` 순으로 앱 부팅

> **보안 주의**: 서명 키 `root-ec-p256.pem`은 `.gitignore`로 제외되어 저장소에 포함되지 않습니다. 빌드하려면 별도로 키를 생성/배치해야 합니다.

---

## 관련 저장소

- **팀 전체 프로젝트**: [rccar_ota_project](https://github.com/SJLee-83/rccar_ota_project) — Dual-Bank OTA(최종 채택), AI 음성 제어, 충돌 회피 등 전체 시스템

## 개발자

이승재 — RA6E1 펌웨어 · 주행/센서 제어 담당
