# Prerelease Release Notes Template Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** prerelease용 GitHub Release notes 형식을 repo 안의 고정 템플릿과 문서 흐름으로 정착시킨다.

**Architecture:** `docs/release/PRERELEASE_NOTES_TEMPLATE.md`를 새 canonical template로 추가하고, `docs/DEVELOPMENT.md` release section이 `gh release ... --notes-file` 흐름을 명시하도록 바꾼다. 필요 시 가벼운 source-guard 테스트로 템플릿 존재와 문서 참조를 고정한다.

**Tech Stack:** Markdown docs, Python source-guard test, GitHub CLI release workflow docs

---

## Chunk 1: Contract Guard

### Task 1: 템플릿/문서 참조를 failing test로 잠근다

**Files:**
- Modify: `tests/packaging_includes_cli_tests.py`
- Test: `tests/packaging_includes_cli_tests.py`

- [ ] **Step 1: Write the failing test**

Add checks for:
- `docs/release/PRERELEASE_NOTES_TEMPLATE.md` exists
- template contains required section headings
- `docs/DEVELOPMENT.md` mentions `--notes-file`
- `docs/DEVELOPMENT.md` mentions the template path

- [ ] **Step 2: Run focused test to confirm failure**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli
```

Expected: FAIL because template/doc flow is not yet present

## Chunk 2: Template File

### Task 2: prerelease note template를 추가한다

**Files:**
- Create: `docs/release/PRERELEASE_NOTES_TEMPLATE.md`
- Test: `tests/packaging_includes_cli_tests.py`

- [ ] **Step 1: Write the canonical template**

Must include:
- `## 핵심 변경`
- `## WinUI / 사용성`
- `## 엔진 변경`
- `## 빌드 / 운영`
- `## 주의사항`
- `## 검증`

Also include brief writing rules and placeholders.

- [ ] **Step 2: Run focused test**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli
```

Expected: still FAIL until development doc is updated

## Chunk 3: Development Guide

### Task 3: release workflow 문서에 `--notes-file` 흐름을 고정한다

**Files:**
- Modify: `docs/DEVELOPMENT.md`
- Test: `tests/packaging_includes_cli_tests.py`

- [ ] **Step 1: Update the Release section**

Add:
- template path
- copy/edit flow
- `gh release create/edit --notes-file ...`

- [ ] **Step 2: Run focused test to verify GREEN**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli
```

Expected: PASS

## Chunk 4: Full Verification And Commit

### Task 4: Run the lightest relevant verification and commit

**Files:**
- Verify only

- [ ] **Step 1: Run focused release-doc guard**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli
```

Expected: PASS

- [ ] **Step 2: Commit**

```bash
git add docs/release/PRERELEASE_NOTES_TEMPLATE.md docs/DEVELOPMENT.md tests/packaging_includes_cli_tests.py docs/superpowers/specs/2026-03-13-prerelease-release-notes-template-design.md docs/superpowers/plans/2026-03-13-prerelease-release-notes-template.md
git commit -m "docs: add prerelease release notes template"
```
