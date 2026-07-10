#pragma once

#include "common/status.h"
#include "common/win32_handle.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace winexinfo {

enum class FileVersionSource {
    FixedResource,
    Alternate,
    Missing,
};

enum class TargetModuleFile {
    ExplorerFrame,
    Shell32,
};

struct OsVersionEvidence final {
    bool rtl_version_present;
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t build;
    bool ubr_present;
    std::uint32_t ubr;
};

struct FileIdentityEvidence final {
    bool target_opened;
    bool system_opened;
    std::uint64_t target_volume_serial;
    std::uint64_t system_volume_serial;
    std::array<std::uint8_t, 16> target_file_id;
    std::array<std::uint8_t, 16> system_file_id;
};

struct OpenedIdentityFiles final {
    FileIdentityEvidence evidence;
    UniqueHandle target_file;
    UniqueHandle system_file;
};

struct CatalogSignatureEvidence final {
    bool hash_calculated;
    bool catalog_found;
    bool cache_only;
    bool revocation_none;
    bool embedded_signature_used;
    bool trusted;
    std::wstring signer_subject;
    HRESULT trust_hresult;
};

struct PrincipalEvidence final {
    bool target_present;
    bool host_present;
    std::vector<std::uint8_t> target_sid;
    std::vector<std::uint8_t> host_sid;
    std::uint32_t target_session;
    std::uint32_t host_session;
    std::uint32_t target_integrity;
    std::uint32_t host_integrity;
};

struct ArchitectureEvidence final {
    bool query_succeeded;
    USHORT process_machine;
    USHORT native_machine;
};

struct FileVersionEvidence final {
    FileVersionSource source;
    std::wstring value;
};

struct MitigationEvidence final {
    bool extension_point_query_succeeded;
    bool disable_extension_points;
    bool signature_query_succeeded;
    bool microsoft_signed_only;
    bool cfg_query_succeeded;
    bool cfg_enabled;
    bool strict_cfg;
};

struct TargetValidationEvidence final {
    OsVersionEvidence os;
    FileIdentityEvidence file_identity;
    CatalogSignatureEvidence signature;
    PrincipalEvidence principal;
    ArchitectureEvidence architecture;
    FileVersionEvidence explorer_version;
    FileVersionEvidence explorer_frame_version;
    FileVersionEvidence shell32_version;
    MitigationEvidence mitigations;
    Status capture_status;
};

struct TargetValidationRow final {
    std::string name;
    bool passed;
};

struct TargetValidationResult final {
    Status status;
    std::vector<TargetValidationRow> rows;
};

[[nodiscard]] TargetValidationResult ValidateTargetEvidence(
    const TargetValidationEvidence& evidence);
[[nodiscard]] Status CaptureTargetValidationEvidence(
    DWORD processId,
    TargetValidationEvidence* output);
[[nodiscard]] bool IsExactTargetModuleBasename(
    std::wstring_view basename,
    TargetModuleFile target);
[[nodiscard]] bool ShouldRetryModuleSnapshot(DWORD error) noexcept;
[[nodiscard]] Status CaptureFileIdentityEvidence(
    const std::wstring& targetPath,
    const std::wstring& systemPath,
    OpenedIdentityFiles* output);

}  // namespace winexinfo
