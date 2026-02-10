#include "AnalyzerInternals.h"

#include "Bucket.h"

#include <algorithm>

namespace skydiag::dump_tool::internal {

void ComputeCrashBucket(AnalysisResult& out)
{
  std::vector<std::wstring> bucketFrames;
  if (!out.stackwalk_primary_frames.empty()) {
    const std::size_t n = std::min<std::size_t>(out.stackwalk_primary_frames.size(), 6);
    bucketFrames.assign(out.stackwalk_primary_frames.begin(), out.stackwalk_primary_frames.begin() + n);
  } else if (!out.suspects.empty()) {
    const std::size_t n = std::min<std::size_t>(out.suspects.size(), 4);
    bucketFrames.reserve(n);
    for (std::size_t i = 0; i < n; i++) {
      bucketFrames.push_back(out.suspects[i].module_filename);
    }
  } else if (!out.fault_module_plus_offset.empty()) {
    bucketFrames.push_back(out.fault_module_plus_offset);
  }

  std::wstring faultModule = out.fault_module_filename;
  if (faultModule.empty()) {
    faultModule = out.fault_module_plus_offset;
  }
  out.crash_bucket_key = ComputeCrashBucketKey(out.exc_code, faultModule, bucketFrames);
}

}  // namespace skydiag::dump_tool::internal

