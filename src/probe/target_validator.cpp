#include "probe/target_validator.h"

#include "common/contracts.h"
#include "common/win32_handle.h"

#include <Windows.h>
#include <Softpub.h>
#include <TlHelp32.h>
#include <WinCrypt.h>
#include <WinTrust.h>
#include <mscat.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace winexinfo {
namespace {

void RecordCaptureFailure(
    Status* const status,
    const ErrorCode code,
    const HRESULT hresult,
    const DWORD win32) {
    if (status->ok()) {
        *status = {code, hresult, win32};
    }
}

FileVersionEvidence ReadFixedFileVersion(
    const std::wstring& path,
    Status* const captureStatus) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        const DWORD error = GetLastError();
        RecordCaptureFailure(
            captureStatus,
            ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
        return {FileVersionSource::Missing, {}};
    }

    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) {
        const DWORD error = GetLastError();
        RecordCaptureFailure(
            captureStatus,
            ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
        return {FileVersionSource::Missing, {}};
    }

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedSize = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) ||
        fixed == nullptr || fixedSize != sizeof(VS_FIXEDFILEINFO) ||
        fixed->dwSignature != VS_FFI_SIGNATURE) {
        const DWORD error = ERROR_RESOURCE_DATA_NOT_FOUND;
        RecordCaptureFailure(
            captureStatus,
            ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
        return {FileVersionSource::Missing, {}};
    }

    return {
        FileVersionSource::FixedResource,
        std::to_wstring(HIWORD(fixed->dwFileVersionMS)) + L"." +
            std::to_wstring(LOWORD(fixed->dwFileVersionMS)) + L"." +
            std::to_wstring(HIWORD(fixed->dwFileVersionLS)) + L"." +
            std::to_wstring(LOWORD(fixed->dwFileVersionLS)),
    };
}

}  // namespace

TargetValidationResult ValidateTargetEvidence(const TargetValidationEvidence& evidence) {
    const bool osPassed = evidence.os.rtl_version_present && evidence.os.ubr_present &&
        evidence.os.major == kTargetOsMajor && evidence.os.minor == kTargetOsMinor &&
        evidence.os.build == kTargetOsBuild && evidence.os.ubr == kTargetOsUbr;
    const bool identityPassed = evidence.file_identity.target_opened &&
        evidence.file_identity.system_opened &&
        evidence.file_identity.target_volume_serial == evidence.file_identity.system_volume_serial &&
        evidence.file_identity.target_file_id == evidence.file_identity.system_file_id;
    const bool signaturePassed = evidence.signature.hash_calculated &&
        evidence.signature.catalog_found && evidence.signature.cache_only &&
        evidence.signature.revocation_none && !evidence.signature.embedded_signature_used &&
        evidence.signature.trusted &&
        evidence.signature.signer_subject ==
            L"CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";
    const bool principalPassed = evidence.principal.target_present &&
        evidence.principal.host_present &&
        evidence.principal.target_sid == evidence.principal.host_sid &&
        evidence.principal.target_session == evidence.principal.host_session &&
        evidence.principal.target_integrity == evidence.principal.host_integrity;
    const bool architecturePassed = evidence.architecture.query_succeeded &&
        evidence.architecture.process_machine == IMAGE_FILE_MACHINE_UNKNOWN &&
        evidence.architecture.native_machine == kTargetMachine;
    const bool explorerVersionPassed =
        evidence.explorer_version.source == FileVersionSource::FixedResource &&
        evidence.explorer_version.value == kTargetExplorerVersion;
    const bool explorerFrameVersionPassed =
        evidence.explorer_frame_version.source == FileVersionSource::FixedResource &&
        evidence.explorer_frame_version.value == kTargetExplorerFrameVersion;
    const bool shell32VersionPassed =
        evidence.shell32_version.source == FileVersionSource::FixedResource &&
        evidence.shell32_version.value == kTargetShell32Version;
    const bool mitigationsPassed = evidence.mitigations.extension_point_query_succeeded &&
        !evidence.mitigations.disable_extension_points &&
        evidence.mitigations.signature_query_succeeded &&
        !evidence.mitigations.microsoft_signed_only && evidence.mitigations.cfg_query_succeeded &&
        evidence.mitigations.cfg_enabled && !evidence.mitigations.strict_cfg;

    ErrorCode code = ErrorCode::OK;
    HRESULT hresult = S_OK;
    DWORD win32 = ERROR_SUCCESS;
    if (!evidence.capture_status.ok()) {
        code = evidence.capture_status.code;
        hresult = evidence.capture_status.hresult;
        win32 = evidence.capture_status.win32;
    } else if (!osPassed) {
        code = ErrorCode::UNSUPPORTED_OS_BUILD;
        hresult = S_FALSE;
    } else if (!identityPassed || !signaturePassed || !principalPassed || !architecturePassed) {
        code = ErrorCode::TARGET_VALIDATION_FAILED;
        hresult = signaturePassed ? S_FALSE : evidence.signature.trust_hresult;
        if (SUCCEEDED(hresult)) {
            hresult = S_FALSE;
        }
    } else if (!explorerVersionPassed || !explorerFrameVersionPassed || !shell32VersionPassed) {
        code = ErrorCode::UNSUPPORTED_EXPLORER_BUILD;
        hresult = S_FALSE;
    } else if (!mitigationsPassed) {
        code = ErrorCode::TARGET_MITIGATION_BLOCKED;
        hresult = S_FALSE;
    }

    return {
        Status{code, hresult, win32},
        {
            {"os_version", osPassed},
            {"file_identity", identityPassed},
            {"catalog_signature", signaturePassed},
            {"principal", principalPassed},
            {"architecture", architecturePassed},
            {"explorer_version", explorerVersionPassed},
            {"explorer_frame_version", explorerFrameVersionPassed},
            {"shell32_version", shell32VersionPassed},
            {"mitigations", mitigationsPassed},
        },
    };
}

Status CaptureTargetValidationEvidence(
    const DWORD processId,
    TargetValidationEvidence* const output) {
    if (output == nullptr || processId == 0) {
        return {ErrorCode::TARGET_VALIDATION_FAILED, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }

    TargetValidationEvidence evidence{};
    evidence.capture_status = {ErrorCode::OK, S_OK, ERROR_SUCCESS};

    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtlGetVersion = ntdll == nullptr
        ? nullptr
        : reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion != nullptr && rtlGetVersion(&version) == 0) {
        evidence.os.rtl_version_present = true;
        evidence.os.major = version.dwMajorVersion;
        evidence.os.minor = version.dwMinorVersion;
        evidence.os.build = version.dwBuildNumber;
    } else {
        const DWORD error = ERROR_PROC_NOT_FOUND;
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::UNSUPPORTED_OS_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
    }

    HKEY versionKey = nullptr;
    LSTATUS registryStatus = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        0,
        KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        &versionKey);
    if (registryStatus == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD value = 0;
        DWORD valueSize = sizeof(value);
        registryStatus = RegQueryValueExW(
            versionKey,
            L"UBR",
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&value),
            &valueSize);
        RegCloseKey(versionKey);
        if (registryStatus == ERROR_SUCCESS && type == REG_DWORD && valueSize == sizeof(value)) {
            evidence.os.ubr_present = true;
            evidence.os.ubr = value;
        } else {
            const DWORD error = registryStatus == ERROR_SUCCESS ? ERROR_DATATYPE_MISMATCH
                                                                : static_cast<DWORD>(registryStatus);
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::UNSUPPORTED_OS_BUILD,
                HRESULT_FROM_WIN32(error),
                error);
        }
    } else {
        const DWORD error = static_cast<DWORD>(registryStatus);
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::UNSUPPORTED_OS_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
    }

    UniqueHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)};
    std::wstring targetPath;
    if (process) {
        std::vector<wchar_t> pathBuffer(32768);
        DWORD pathSize = static_cast<DWORD>(pathBuffer.size());
        if (QueryFullProcessImageNameW(process.get(), 0, pathBuffer.data(), &pathSize)) {
            targetPath.assign(pathBuffer.data(), pathSize);
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_VALIDATION_FAILED,
                HRESULT_FROM_WIN32(error),
                error);
        }
    } else {
        const DWORD error = GetLastError();
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::TARGET_VALIDATION_FAILED,
            HRESULT_FROM_WIN32(error),
            error);
    }

    std::vector<wchar_t> windowsDirectory(32768);
    const UINT windowsLength =
        GetWindowsDirectoryW(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
    std::wstring systemExplorerPath;
    if (windowsLength > 0 && windowsLength < windowsDirectory.size()) {
        systemExplorerPath.assign(windowsDirectory.data(), windowsLength);
        systemExplorerPath.append(L"\\explorer.exe");
    } else {
        const DWORD error = windowsLength == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::TARGET_VALIDATION_FAILED,
            HRESULT_FROM_WIN32(error),
            error);
    }

    UniqueHandle targetFile;
    if (!targetPath.empty()) {
        targetFile.reset(CreateFileW(
            targetPath.c_str(),
            FILE_READ_ATTRIBUTES | GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
    }
    UniqueHandle systemFile;
    if (!systemExplorerPath.empty()) {
        systemFile.reset(CreateFileW(
            systemExplorerPath.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
    }

    FILE_ID_INFO targetIdentity{};
    if (targetFile && GetFileInformationByHandleEx(
                          targetFile.get(), FileIdInfo, &targetIdentity, sizeof(targetIdentity))) {
        evidence.file_identity.target_opened = true;
        evidence.file_identity.target_volume_serial = targetIdentity.VolumeSerialNumber;
        std::memcpy(
            evidence.file_identity.target_file_id.data(),
            targetIdentity.FileId.Identifier,
            evidence.file_identity.target_file_id.size());
    } else {
        const DWORD error = targetFile ? GetLastError() : ERROR_FILE_NOT_FOUND;
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::TARGET_VALIDATION_FAILED,
            HRESULT_FROM_WIN32(error),
            error);
    }

    FILE_ID_INFO systemIdentity{};
    if (systemFile && GetFileInformationByHandleEx(
                          systemFile.get(), FileIdInfo, &systemIdentity, sizeof(systemIdentity))) {
        evidence.file_identity.system_opened = true;
        evidence.file_identity.system_volume_serial = systemIdentity.VolumeSerialNumber;
        std::memcpy(
            evidence.file_identity.system_file_id.data(),
            systemIdentity.FileId.Identifier,
            evidence.file_identity.system_file_id.size());
    } else {
        const DWORD error = systemFile ? GetLastError() : ERROR_FILE_NOT_FOUND;
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::TARGET_VALIDATION_FAILED,
            HRESULT_FROM_WIN32(error),
            error);
    }

    evidence.signature.cache_only = true;
    evidence.signature.revocation_none = true;
    evidence.signature.embedded_signature_used = false;
    HCATADMIN catalogAdmin = nullptr;
    if (targetFile && CryptCATAdminAcquireContext2(
                          &catalogAdmin, nullptr, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
        DWORD hashSize = 0;
        if (CryptCATAdminCalcHashFromFileHandle2(
                catalogAdmin, targetFile.get(), &hashSize, nullptr, 0) &&
            hashSize > 0) {
            std::vector<BYTE> hash(hashSize);
            if (CryptCATAdminCalcHashFromFileHandle2(
                    catalogAdmin, targetFile.get(), &hashSize, hash.data(), 0)) {
                evidence.signature.hash_calculated = true;
                HCATINFO previousCatalog = nullptr;
                HCATINFO catalog = CryptCATAdminEnumCatalogFromHash(
                    catalogAdmin, hash.data(), hashSize, 0, &previousCatalog);
                if (catalog != nullptr) {
                    evidence.signature.catalog_found = true;
                    CATALOG_INFO catalogInfo{};
                    catalogInfo.cbStruct = sizeof(catalogInfo);
                    if (CryptCATCatalogInfoFromContext(catalog, &catalogInfo, 0)) {
                        std::wostringstream memberTag;
                        memberTag << std::uppercase << std::hex << std::setfill(L'0');
                        for (const BYTE byte : hash) {
                            memberTag << std::setw(2) << static_cast<unsigned int>(byte);
                        }
                        const std::wstring memberTagText = memberTag.str();

                        WINTRUST_CATALOG_INFO trustCatalog{};
                        trustCatalog.cbStruct = sizeof(trustCatalog);
                        trustCatalog.pcwszCatalogFilePath = catalogInfo.wszCatalogFile;
                        trustCatalog.pcwszMemberTag = memberTagText.c_str();
                        trustCatalog.pcwszMemberFilePath = targetPath.c_str();
                        trustCatalog.hMemberFile = targetFile.get();

                        WINTRUST_DATA trustData{};
                        trustData.cbStruct = sizeof(trustData);
                        trustData.dwUIChoice = WTD_UI_NONE;
                        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
                        trustData.dwUnionChoice = WTD_CHOICE_CATALOG;
                        trustData.pCatalog = &trustCatalog;
                        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
                        trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

                        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
                        const LONG trustResult = WinVerifyTrust(nullptr, &action, &trustData);
                        evidence.signature.trust_hresult = static_cast<HRESULT>(trustResult);
                        evidence.signature.trusted = trustResult == ERROR_SUCCESS;
                        if (evidence.signature.trusted) {
                            CRYPT_PROVIDER_DATA* provider =
                                WTHelperProvDataFromStateData(trustData.hWVTStateData);
                            CRYPT_PROVIDER_SGNR* signer = provider == nullptr
                                ? nullptr
                                : WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
                            if (signer != nullptr && signer->csCertChain > 0 &&
                                signer->pasCertChain[0].pCert != nullptr) {
                                CERT_NAME_BLOB subject =
                                    signer->pasCertChain[0].pCert->pCertInfo->Subject;
                                const DWORD subjectLength = CertNameToStrW(
                                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                    &subject,
                                    CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                                    nullptr,
                                    0);
                                if (subjectLength > 1) {
                                    std::vector<wchar_t> subjectText(subjectLength);
                                    if (CertNameToStrW(
                                            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                            &subject,
                                            CERT_X500_NAME_STR | CERT_NAME_STR_REVERSE_FLAG,
                                            subjectText.data(),
                                            subjectLength) == subjectLength) {
                                        evidence.signature.signer_subject.assign(
                                            subjectText.data(), subjectLength - 1);
                                    }
                                }
                            }
                        }

                        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
                        static_cast<void>(WinVerifyTrust(nullptr, &action, &trustData));
                    } else {
                        const DWORD error = GetLastError();
                        RecordCaptureFailure(
                            &evidence.capture_status,
                            ErrorCode::TARGET_VALIDATION_FAILED,
                            HRESULT_FROM_WIN32(error),
                            error);
                    }
                    CryptCATAdminReleaseCatalogContext(catalogAdmin, catalog, 0);
                } else {
                    evidence.signature.trust_hresult = TRUST_E_NOSIGNATURE;
                }
            } else {
                const DWORD error = GetLastError();
                RecordCaptureFailure(
                    &evidence.capture_status,
                    ErrorCode::TARGET_VALIDATION_FAILED,
                    HRESULT_FROM_WIN32(error),
                    error);
            }
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_VALIDATION_FAILED,
                HRESULT_FROM_WIN32(error),
                error);
        }
        CryptCATAdminReleaseContext(catalogAdmin, 0);
    } else {
        const DWORD error = targetFile ? GetLastError() : ERROR_INVALID_HANDLE;
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::TARGET_VALIDATION_FAILED,
            HRESULT_FROM_WIN32(error),
            error);
    }

    std::array<HANDLE, 2> tokenProcesses{process.get(), GetCurrentProcess()};
    for (std::size_t index = 0; index < tokenProcesses.size(); ++index) {
        UniqueHandle token;
        HANDLE rawToken = nullptr;
        if (tokenProcesses[index] == nullptr ||
            !OpenProcessToken(tokenProcesses[index], TOKEN_QUERY, &rawToken)) {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_VALIDATION_FAILED,
                HRESULT_FROM_WIN32(error),
                error);
            continue;
        }
        token.reset(rawToken);

        DWORD userSize = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &userSize);
        std::vector<std::byte> userBuffer(userSize);
        DWORD session = 0;
        DWORD sessionSize = sizeof(session);
        DWORD integritySize = 0;
        GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &integritySize);
        std::vector<std::byte> integrityBuffer(integritySize);
        if (userSize == 0 || integritySize == 0 ||
            !GetTokenInformation(
                token.get(), TokenUser, userBuffer.data(), userSize, &userSize) ||
            !GetTokenInformation(
                token.get(), TokenSessionId, &session, sessionSize, &sessionSize) ||
            !GetTokenInformation(
                token.get(),
                TokenIntegrityLevel,
                integrityBuffer.data(),
                integritySize,
                &integritySize)) {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_VALIDATION_FAILED,
                HRESULT_FROM_WIN32(error),
                error);
            continue;
        }

        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(userBuffer.data());
        const auto* mandatoryLabel =
            reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrityBuffer.data());
        const DWORD sidLength = GetLengthSid(tokenUser->User.Sid);
        const UCHAR subAuthorityCount = *GetSidSubAuthorityCount(mandatoryLabel->Label.Sid);
        if (sidLength == 0 || subAuthorityCount == 0) {
            const DWORD error = ERROR_INVALID_SID;
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_VALIDATION_FAILED,
                HRESULT_FROM_WIN32(error),
                error);
            continue;
        }
        const DWORD integrity =
            *GetSidSubAuthority(mandatoryLabel->Label.Sid, subAuthorityCount - 1);
        std::vector<std::uint8_t> sid(sidLength);
        std::memcpy(sid.data(), tokenUser->User.Sid, sidLength);
        if (index == 0) {
            evidence.principal.target_present = true;
            evidence.principal.target_sid = std::move(sid);
            evidence.principal.target_session = session;
            evidence.principal.target_integrity = integrity;
        } else {
            evidence.principal.host_present = true;
            evidence.principal.host_sid = std::move(sid);
            evidence.principal.host_session = session;
            evidence.principal.host_integrity = integrity;
        }
    }

    if (process) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (IsWow64Process2(process.get(), &processMachine, &nativeMachine)) {
            evidence.architecture = {true, processMachine, nativeMachine};
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_VALIDATION_FAILED,
                HRESULT_FROM_WIN32(error),
                error);
        }
    }

    std::wstring explorerFramePath;
    std::wstring shell32Path;
    UniqueHandle moduleSnapshot{
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId)};
    if (moduleSnapshot) {
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (Module32FirstW(moduleSnapshot.get(), &module)) {
            do {
                if (std::wstring_view{module.szModule} == L"ExplorerFrame.dll") {
                    explorerFramePath = module.szExePath;
                } else if (std::wstring_view{module.szModule} == L"shell32.dll") {
                    shell32Path = module.szExePath;
                }
            } while (Module32NextW(moduleSnapshot.get(), &module));
            const DWORD enumerationError = GetLastError();
            if (enumerationError != ERROR_NO_MORE_FILES) {
                RecordCaptureFailure(
                    &evidence.capture_status,
                    ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
                    HRESULT_FROM_WIN32(enumerationError),
                    enumerationError);
            }
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
                HRESULT_FROM_WIN32(error),
                error);
        }
    } else {
        const DWORD error = GetLastError();
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
    }

    evidence.explorer_version = targetPath.empty()
        ? FileVersionEvidence{FileVersionSource::Missing, {}}
        : ReadFixedFileVersion(targetPath, &evidence.capture_status);
    evidence.explorer_frame_version = explorerFramePath.empty()
        ? FileVersionEvidence{FileVersionSource::Missing, {}}
        : ReadFixedFileVersion(explorerFramePath, &evidence.capture_status);
    evidence.shell32_version = shell32Path.empty()
        ? FileVersionEvidence{FileVersionSource::Missing, {}}
        : ReadFixedFileVersion(shell32Path, &evidence.capture_status);
    if (explorerFramePath.empty() || shell32Path.empty()) {
        const DWORD error = ERROR_MOD_NOT_FOUND;
        RecordCaptureFailure(
            &evidence.capture_status,
            ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
            HRESULT_FROM_WIN32(error),
            error);
    }

    if (process) {
        PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY extensionPoints{};
        evidence.mitigations.extension_point_query_succeeded = GetProcessMitigationPolicy(
            process.get(),
            ProcessExtensionPointDisablePolicy,
            &extensionPoints,
            sizeof(extensionPoints));
        if (evidence.mitigations.extension_point_query_succeeded) {
            evidence.mitigations.disable_extension_points =
                extensionPoints.DisableExtensionPoints != 0;
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_MITIGATION_BLOCKED,
                HRESULT_FROM_WIN32(error),
                error);
        }

        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signaturePolicy{};
        evidence.mitigations.signature_query_succeeded = GetProcessMitigationPolicy(
            process.get(),
            ProcessSignaturePolicy,
            &signaturePolicy,
            sizeof(signaturePolicy));
        if (evidence.mitigations.signature_query_succeeded) {
            evidence.mitigations.microsoft_signed_only = signaturePolicy.MicrosoftSignedOnly != 0;
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_MITIGATION_BLOCKED,
                HRESULT_FROM_WIN32(error),
                error);
        }

        PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfgPolicy{};
        evidence.mitigations.cfg_query_succeeded = GetProcessMitigationPolicy(
            process.get(),
            ProcessControlFlowGuardPolicy,
            &cfgPolicy,
            sizeof(cfgPolicy));
        if (evidence.mitigations.cfg_query_succeeded) {
            evidence.mitigations.cfg_enabled = cfgPolicy.EnableControlFlowGuard != 0;
            evidence.mitigations.strict_cfg = cfgPolicy.StrictMode != 0;
        } else {
            const DWORD error = GetLastError();
            RecordCaptureFailure(
                &evidence.capture_status,
                ErrorCode::TARGET_MITIGATION_BLOCKED,
                HRESULT_FROM_WIN32(error),
                error);
        }
    }

    *output = std::move(evidence);
    return output->capture_status;
}

}  // namespace winexinfo
