# Taskbar Key Indicator Development Rules

## 1. Project Context Isolation
- 본 프로젝트는 Windows 작업표시줄 상단 LED 바 기반 키보드 연결 인디케이터(`TaskbarKeyIndicator`) 전용 워크스페이스입니다.
- 외부 글로벌 규칙이나 다른 프로젝트(KeyBridge 안드로이드, ttyd 등)의 도메인 로직과 분리하여 관리합니다.

## 2. Windows Portability & Security Rules
- **Zero-Dependency**: C++ Win32 API 기반 단독 포터블 바이너리를 유지하며, 외부 런타임(MSVC DLL, .NET 등) 종속성을 추가하지 않습니다.
- **Zero-Network**: 네트워크 소켓 통신을 일체 생성하지 않으며 완전 오프라인으로 구동합니다.
- **100% Click-Through**: 작업표시줄 오버레이는 `WS_EX_TRANSPARENT | WS_EX_LAYERED`를 유지하여 마우스 클릭 및 윈도우 인터랙션을 절대 방해하지 않습니다.
- **Pure Event-Driven**: `WM_DEVICECHANGE` 및 윈도우 메시지 기반으로만 반응하며 백그라운드 폴링 루프를 두지 않습니다.

## 3. Build & Artifact Protocol
- 바이너리 빌드는 `build.sh` (`zig c++ -target x86_64-windows-gnu`)를 통해 단일 GUI 바이너리(`dist/TaskbarKeyIndicator.exe`)로 생성합니다.
- 사용자 요청에 따라 exe 파일은 저장소 내 `dist/` 경로에 함께 커밋하여 저장소 전체 ZIP 다운로드 시 즉시 포함되도록 관리합니다.
