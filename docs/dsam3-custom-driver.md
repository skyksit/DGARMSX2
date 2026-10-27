# 커스텀 Vulkan 드라이버 — dsam3 연동 계약

emucore는 libadrenotools를 static으로 내장하고 있어서 Vulkan 드라이버 `.so`(ICD)를 벤더와 상관없이 로드합니다. 다운로드, 압축 해제, 선택 UI는 dsam3가 맡고, 코어는 아래 계약만 지킵니다.

## 1. 배포물 (`cores/<ver>/arm64-v8a/`)

| 파일 | 용도 |
|---|---|
| `libemucore.so` | 코어 |
| `libhook_impl.so`, `libmain_hook.so` | adrenotools hook. **필수**이며, 없으면 시스템 드라이버로 대체됩니다 |
| `libfile_redirect_hook.so` | redirectDir을 넘길 때 필요합니다 |
| `libgsl_alloc_hook.so` | upstream과 맞추려고 함께 배포합니다 (현재 미사용) |
| `BUILD_INFO.txt` | `HOOK_LIBS=`, `HOOK_LIB_TARGET=files/ps2core/hooks` |

hook `.so`는 코어에 static으로 들어간 adrenotools와 **같은 빌드여야** 합니다. 이 4개로 서버의 `drivers/adrenotools_hooks_v1.zip`(zip 루트에 4개 `.so`)을 만들어 올립니다.

## 2. dsam3 설치 구조 (현행)

- 드라이버 zip과 hook zip은 `{STORE_DOMAIN}/drivers/`에서 받습니다.
- 둘 다 **내부 저장소** `filesDir/ps2data/gpu_driver/`에 풉니다 (`Ps2GpuDriverInstaller`). 외부 저장소에서는 dlopen이 실패합니다.
- zip은 adrenotools 표준 형식입니다. `meta.json`의 `libraryName`이 실제 `.so` 파일명입니다.
- 드라이버 목록은 서버 카탈로그 `drivers/ps2_gpu_drivers.json`이 정합니다 (`Ps2GpuDrivers.parseCatalog`). 서버에 없으면 앱 내장 목록을 씁니다.

## 3. 호출 순서

```java
// 첫 Vulkan 로드(= 첫 VM 부팅) 전에
NativeApp.setCustomVulkanDriver(
    gpuDriverDir + "/",         // filesDir/ps2data/gpu_driver/ (trailing slash 필수)
    libraryName,                // meta.json libraryName
    "",                         // 경로 재지정 미사용
    gpuDriverDir);              // hook .so 가 같은 디렉터리에 있음
// 시스템 드라이버로 돌아가려면 네 인자 모두 "" 를 넘깁니다

// 부팅 후 (GS 디바이스 생성 이후)
String json = NativeApp.getVulkanDriverStatusJSON();
```

`getVulkanDriverStatusJSON()` 예시:

```json
{"requested":true,"loaded":true,"libraryName":"libvulkan_freedreno.so","fallbackReason":"",
 "hasDevice":true,"driverId":18,"driverName":"turnip Mesa driver","driverInfo":"Mesa 25.2.0",
 "apiVersion":"1.4.318","gpu":"Turnip Adreno (TM) 740"}
```

- `requested=true`인데 `loaded=false`이면 시스템 드라이버로 대체된 것입니다. 이유는 `fallbackReason`에 있습니다.
- `driverId`는 VkDriverId 값입니다.

| driverId | 드라이버 |
|---|---|
| 8 | Qualcomm 독점 |
| 18 | Mesa Turnip |
| 21 | Samsung Xclipse |
| 9 | ARM Mali |

- 드라이버는 Vulkan 라이브러리를 처음 로드할 때만 바뀝니다. 다른 드라이버로 바꾸려면 프로세스를 재시작해야 합니다.

## 4. GPU별 드라이버 소스 (GitHub releases)

| GPU | 추천 소스 | 카탈로그 variant |
|---|---|---|
| Adreno 8xx | `crueter/GameHub-8Elite-Drivers`, `StevenMXZ/Adreno-Tools-Drivers` (Gen8) | `a8xx` |
| Adreno 7xx | `MrPurple666/purple-turnip`, `StevenMXZ/Adreno-Tools-Drivers` | `a7xx` |
| Adreno 6xx | `K11MCH1/AdrenoToolsDrivers` | `a7xx` + `"minAdreno":600,"maxAdreno":699` |
| Mali / PowerVR / Xclipse | 시스템 드라이버 | — |

Xclipse용 ExynosTools는 독립 ICD가 아닙니다. Samsung 순정 드라이버(`vulkan.samsung.so`)를 동봉한 Vulkan **레이어**라서 adrenotools 경로로는 로드되지 않습니다. 순정 드라이버를 재배포하는 문제도 있어 지원하지 않습니다.

세대가 맞지 않는 Turnip은 dlopen에 성공해도 물리 디바이스를 0개 돌려줍니다. 그러면 GS 스레드가 abort하므로 variant와 모델 범위를 정확히 적어야 합니다.
