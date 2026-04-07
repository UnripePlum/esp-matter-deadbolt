---
name: deploy
description: |
  빌드 → 커밋 → 푸시 → GitHub Release 업데이트까지 한 번에 처리.
  "배포", "deploy", "release", "ship", "커밋 푸시 릴리즈" 등 키워드에 반응.
allowed-tools:
  - Bash
  - Read
  - Edit
  - Grep
  - AskUserQuestion
---

# Deploy Skill — 빌드 · 커밋 · 푸시 · Release 업데이트

## 실행 순서

### 1. 현재 상태 파악

```bash
git status --short
git diff --stat
```

변경사항이 없으면 사용자에게 알리고 종료.

### 2. 현재 버전 확인

```bash
grep 'FIRMWARE_VERSION' main/app_main.cpp
```

현재 버전을 메모해둔다 (예: `v1.1.0`).

### 3. 버전 업데이트 (필수)

**버전 업데이트는 배포 시 항상 필수다.** 버전이 같으면 자동 OTA가 업데이트를 감지하지 못한다.

사용자에게 새 버전을 입력받는다:
- "현재 버전: {CURRENT_VERSION} — 새 버전을 입력하세요 (예: v1.2.0)"
- 입력 없이 건너뛰려 하면 반드시 경고: "버전을 올리지 않으면 기기가 자동 OTA로 업데이트를 감지할 수 없습니다."

버전 결정 후:
- `main/app_main.cpp`의 `FIRMWARE_VERSION` 업데이트
- `docs/API_SPECIFICATION.md`의 버전/날짜 업데이트

### 4. 빌드

```bash
source $HOME/.espressif/python_env/idf5.2_py3.9_env/bin/activate \
  && source ~/esp/esp-idf/export.sh 2>/dev/null \
  && source ~/esp/esp-matter/export.sh 2>/dev/null \
  && idf.py build 2>&1 | tail -5
```

빌드 실패 시 중단하고 에러 보고.

### 5. 커밋

변경된 파일을 스테이징하고 커밋:
```bash
git add -A
git status --short   # 커밋할 파일 확인
```

커밋 메시지는 변경 내용을 바탕으로 자동 생성.
형식: `feat/fix/chore/docs: <한 줄 요약>`

```bash
git commit -m "..."
```

### 6. 푸시

```bash
git push origin main
```

### 7. GitHub Release 업데이트

현재 버전 태그가 이미 존재하면 바이너리만 교체:
```bash
gh release upload {VERSION} \
  build/esp-matter-deadbolt.bin \
  --repo UnripePlum/esp-matter-deadbolt \
  --clobber
```

버전이 올라간 경우 새 릴리즈 생성:
```bash
gh release create {NEW_VERSION} \
  build/esp-matter-deadbolt.bin \
  --repo UnripePlum/esp-matter-deadbolt \
  --title "{NEW_VERSION}" \
  --notes "..." \
  --latest
```

### 8. 완료 보고

- 커밋 해시
- 릴리즈 URL
- 버전

## 주의사항

- 빌드 실패 시 커밋/푸시/릴리즈 진행하지 않음
- 버전 변경 시 `FIRMWARE_VERSION`과 릴리즈 태그를 항상 일치시킴
- `esp-matter-deadbolt.bin` 경로: `build/esp-matter-deadbolt.bin`
