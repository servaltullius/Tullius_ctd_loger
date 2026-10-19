# Overlay of vcpkg's x64-windows-static-md (static libraries, DLL CRT). The
# only difference: the ports keep the runtime-initialized std::mutex
# constructor, as the project itself does (_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
# in the top-level CMakeLists.txt). spdlog and fmt are linked statically into
# SkyrimDiag.dll and SkyrimDiagHelper.exe, so a constexpr-constructed mutex in
# them would crash on an msvcp140.dll older than 14.40 just the same.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

# vcpkg requires VCPKG_C_FLAGS whenever VCPKG_CXX_FLAGS is set.
set(VCPKG_CXX_FLAGS "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
set(VCPKG_C_FLAGS "")
