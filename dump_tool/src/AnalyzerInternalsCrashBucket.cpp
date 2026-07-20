#include "AnalyzerInternals.h"

#include "Bucket.h"

#include <algorithm>

namespace skydiag::dump_tool::internal {

void ComputeCrashBucket(AnalysisResult& out)
{
  // Avoid generating a misleading "always the same" bucket key for snapshot/manual dumps that
  // contain no exception/module/callstack information.
  if (out.exc_code == 0u &&
      out.stackwalk_primary_frames.empty() &&
      out.suspects.empty() &&
      out.fault_module_plus_offset.empty()) {
    out.crash_bucket_key.clear();
    return;
  }

  std::vector<CrashBucketFrame> bucketFrames;
  if (!out.stackwalk_primary_bucket_frames.empty()) {
    const std::size_t n = std::min<std::size_t>(out.stackwalk_primary_bucket_frames.size(), 6);
    bucketFrames.assign(out.stackwalk_primary_bucket_frames.begin(), out.stackwalk_primary_bucket_frames.begin() + n);
  } else if (!out.suspects.empty()) {
    const std::size_t n = std::min<std::size_t>(out.suspects.size(), 4);
    bucketFrames.reserve(n);
    for (std::size_t i = 0; i < n; i++) {
      bucketFrames.push_back({ out.suspects[i].module_filename, 0 });
    }
  }

  std::wstring faultModule = out.fault_module_filename;
  if (faultModule.empty()) {
    faultModule = out.fault_module_plus_offset;
  }
  out.crash_bucket_version = 2;
  out.crash_bucket_key = ComputeCrashBucketKey(
    out.exc_code,
    faultModule,
    out.fault_module_offset,
    bucketFrames);
}

}  // namespace skydiag::dump_tool::internal
