// recommendation_classes[] (ADR-0018): the viewer section of each
// recommendation, decided by the analyzer instead of the viewer matching
// display text. Checks the grouping the viewer applied to the text before, and
// that every [tag] the analyzer and its data files write is either classified
// or known to stay in "verify", so a renamed tag fails here instead of
// silently moving its lines to another section.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

#include "RecommendationClass.h"
#include "SourceGuardTestUtils.h"

namespace {

using skydiag::dump_tool::ClassifyRecommendation;
using skydiag::tests::source_guard::ProjectRoot;
using skydiag::tests::source_guard::ReadAllText;

std::string WideToUtf8(std::wstring_view s)
{
  std::string out;
  for (const wchar_t wc : s) {
    // NOLINTNEXTLINE(bugprone-signed-char-misuse): wchar_t holds a UTF-16/32 code unit, not a char.
    const std::uint32_t cp = static_cast<std::make_unsigned_t<wchar_t>>(wc);
    if (cp < 0x80u) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }
  return out;
}

void Expect(std::wstring_view text, std::string_view group, std::string_view kind)
{
  const auto c = ClassifyRecommendation(text);
  if (c.group != group || c.kind != kind) {
    std::cerr << "FAIL: \"" << WideToUtf8(text) << "\" -> " << c.group << "/" << c.kind << ", expected " << group
              << "/" << kind << "\n";
    assert(false);
  }
}

// Tags whose lines the viewer has always shown under "Verify Next".
const std::set<std::wstring> kVerifyTags = {
  L"[Basics]", L"[기본]", L"[Clean exit]", L"[정상 종료]", L"[Config]", L"[설정]", L"[Context]", L"[상황]",
  L"[DLL guidance]", L"[DLL 점검]", L"[First-chance]", L"[Freeze]", L"[프리징]", L"[Hang]",
  L"[Interpretation]", L"[해석]", L"[Loading]", L"[로딩 중]", L"[Memory]", L"[메모리]", L"[Mesh/Anim]",
  L"[메쉬/애니]", L"[Performance]", L"[성능]", L"[SKSE]", L"[Stack scan]", L"[스택 스캔]", L"[Symbols]",
  L"[심볼]", L"[Troubleshooting]", L"[트러블슈팅]",
  // crash_signatures.json
  L"[Core mods]", L"[기반 모드]", L"[Disk]", L"[디스크]", L"[Files]", L"[파일]", L"[Load order]",
  L"[로드 순서]", L"[Mesh check]", L"[메시 검사]", L"[Mod check]", L"[모드 점검]", L"[Mod conflict]",
  L"[모드 충돌]", L"[Resources]", L"[리소스]", L"[Save]", L"[세이브]", L"[Scripts]", L"[스크립트]",
  L"[Security]", L"[보안]", L"[Setting]",
  // graphics_injection_rules.json
  L"[Graphics injector]", L"[그래픽 인젝터]", L"[Input]", L"[입력]",
  // plugin_rules.json
  L"[Alternative]", L"[대안]", L"[Optimization]", L"[최적화]", L"[Required]", L"[필수]", L"[Tool]",
  L"[도구]", L"[Upkeep]", L"[정리]",
};

// Enough UTF-8 for source and JSON text (no surrogate pairs needed: every tag
// is in the BMP).
std::wstring Utf8ToWide(std::string_view s)
{
  std::wstring out;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = 0;
    std::size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1Fu;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0Fu;
      len = 3;
    } else {
      cp = c & 0x07u;
      len = 4;
    }
    for (std::size_t k = 1; k < len && i + k < s.size(); ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    out.push_back(static_cast<wchar_t>(cp));
    i += len;
  }
  return out;
}

// Every "[tag]" that opens a string literal (opener is L"[ in C++, "[ in JSON).
std::set<std::wstring> LeadingTags(const std::wstring& text, std::wstring_view opener)
{
  std::set<std::wstring> tags;
  for (auto pos = text.find(opener); pos != std::wstring::npos; pos = text.find(opener, pos + 1)) {
    const auto start = pos + opener.size() - 1;
    const auto end = text.find_first_of(L"]\"\n", start);
    if (end != std::wstring::npos && text[end] == L']') {
      tags.insert(text.substr(start, end - start + 1));
    }
  }
  return tags;
}

void TestTagsAreClassifiedOrKnownVerify()
{
  const auto root = ProjectRoot();
  std::set<std::wstring> tags =
    LeadingTags(Utf8ToWide(ReadAllText(root / "dump_tool" / "src" / "EvidenceBuilderRecommendations.cpp")), L"L\"[");
  for (const char* data : { "crash_signatures.json", "graphics_injection_rules.json", "plugin_rules.json" }) {
    const auto found = LeadingTags(Utf8ToWide(ReadAllText(root / "dump_tool" / "data" / data)), L"\"[");
    tags.insert(found.begin(), found.end());
  }
  assert(tags.size() > 60u && "tag scan found too little; did the sources move?");
  for (const auto& tag : tags) {
    const auto c = ClassifyRecommendation(tag + L" x");
    if (c.kind.empty() && kVerifyTags.count(tag) == 0u) {
      std::cerr << "FAIL: tag " << WideToUtf8(tag)
                << " is neither in RecommendationClass.cpp nor a known verify tag of this test\n";
      assert(false);
    }
  }
  for (const auto& tag : kVerifyTags) {
    Expect(tag + L" x", "verify", "");
  }
}

}  // namespace

int main()
{
  // Do This Now, both languages; English ignores case, Korean is exact.
  Expect(L"[Known cause] Move ContentCatalog.txt to the Recycle Bin.", "immediate", "known_cause");
  Expect(L"[알려진 원인] ContentCatalog.txt를 휴지통으로 옮기세요.", "immediate", "known_cause");
  Expect(L"[Known crash site] x", "immediate", "known_crash_site");
  Expect(L"[알려진 크래시 지점] x", "immediate", "known_crash_site");
  Expect(L"[MODAL DIALOG] Read the dialog.", "immediate", "modal_dialog");
  Expect(L"[Modal 대화상자] 대화상자를 확인하세요.", "immediate", "modal_dialog");
  Expect(L"[Main thread] x", "immediate", "main_thread");
  Expect(L"[메인 스레드] x", "immediate", "main_thread");
  Expect(L"[Synchronization stall] x", "immediate", "synchronization_stall");
  Expect(L"[동기화 정지] x", "immediate", "synchronization_stall");
  Expect(L"[actionable candidate] Check SmoothCam.dll first.", "immediate", "actionable_candidate");
  Expect(L"[행동 우선 후보] SmoothCam.dll부터 확인하세요.", "immediate", "actionable_candidate");
  Expect(L"[Crash Logger frame] x", "immediate", "crash_logger_frame");
  Expect(L"[Crash Logger 프레임] x", "immediate", "crash_logger_frame");
  Expect(L"[Top suspect] x", "immediate", "top_suspect");
  Expect(L"[유력 후보] x", "immediate", "top_suspect");
  Expect(L"[ESP/ESM] x", "immediate", "esp_esm");
  Expect(L"[Object ref] x", "immediate", "object_ref");
  Expect(L"[오브젝트 참조] x", "immediate", "object_ref");
  Expect(L"[Conflict] x", "immediate", "conflict");
  Expect(L"[충돌] x", "immediate", "conflict");
  Expect(L"[Problem module] x", "immediate", "problem_module");
  Expect(L"[문제 모듈] x", "immediate", "problem_module");
  Expect(L"[Masters] x", "immediate", "masters");
  Expect(L"[마스터] x", "immediate", "masters");
  Expect(L"[BEES] x", "immediate", "bees");
  Expect(L"[Check] The crash is in the game executable.", "immediate", "check");
  Expect(L"[점검] x", "immediate", "check");
  Expect(L"[Hook framework] x", "immediate", "hook_framework");
  Expect(L"[훅 프레임워크] x", "immediate", "hook_framework");
  Expect(L"[External software] x", "immediate", "external_software");
  Expect(L"[외부 프로그램] x", "immediate", "external_software");

  // Recapture or Compare: the snapshot/manual tags, and capture-again wording
  // anywhere, which outranks an immediate tag (the kind still names the tag).
  Expect(L"[Snapshot] No exception/crash info is present.", "recapture", "snapshot");
  Expect(L"[정상/스냅샷] x", "recapture", "snapshot");
  Expect(L"[Manual] x", "recapture", "manual");
  Expect(L"[수동] x", "recapture", "manual");
  Expect(L"[Recapture] Set DumpMode=2 and capture again.", "recapture", "recapture");
  Expect(L"[재수집] x", "recapture", "recapture");
  Expect(L"[Actionable candidate] If unsure, capture another dump.", "recapture", "actionable_candidate");
  Expect(L"[Basics] Use dumpmode=2 next time.", "recapture", "");
  Expect(L"[Conflict] Compare a second signal to break the tie.", "recapture", "conflict");
  Expect(L"[Freeze] Use a capture taken during the freeze.", "recapture", "");
  Expect(L"[Basics] RECAPTURE with full memory.", "recapture", "");
  Expect(L"[충돌] 두 번째 신호를 비교하세요.", "recapture", "conflict");
  Expect(L"[기본] 한 번 더 모아 보세요.", "recapture", "");
  Expect(L"[프리징] 다시 캡처하세요.", "recapture", "");
  // The English form of the same freeze line goes to the same group.
  Expect(L"[Freeze] Compare related candidates and repeat capture during the issue.", "recapture", "");
  Expect(L"[행동 우선 후보] 재수집 전에 확인하세요.", "recapture", "actionable_candidate");

  // Verify Next: everything else, including no tag and a tag not at the start.
  Expect(L"[Basics] ExceptionCode=0xC0000005 (Access Violation).", "verify", "");
  Expect(L"[DLL guidance] x", "verify", "");
  Expect(L"Update HDT-SMP to the latest version", "verify", "");
  Expect(L"See [Known cause] above.", "verify", "");
  Expect(L"", "verify", "");
  // Korean tags are matched exactly, as the viewer did.
  Expect(L"[MODAL 대화상자] x", "verify", "");

  // The viewer grouped the trimmed text.
  Expect(L"  [Known cause] x  ", "immediate", "known_cause");

  TestTagsAreClassifiedOrKnownVerify();
  return 0;
}
