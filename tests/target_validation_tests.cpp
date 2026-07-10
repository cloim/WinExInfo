#include "test_framework.h"

#include "common/contracts.h"
#include "probe/target_validator.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

winexinfo::TargetValidationEvidence ExactTarget() {
    winexinfo::TargetValidationEvidence evidence{};
    evidence.os = {
        true,
        winexinfo::kTargetOsMajor,
        winexinfo::kTargetOsMinor,
        winexinfo::kTargetOsBuild,
        true,
        winexinfo::kTargetOsUbr,
    };
    evidence.file_identity = {
        true,
        true,
        0x1020304050607080ULL,
        0x1020304050607080ULL,
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    };
    evidence.signature = {
        true,
        true,
        true,
        true,
        false,
        true,
        L"CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US",
        S_OK,
    };
    evidence.principal = {
        true,
        true,
        {1, 2, 3, 4},
        {1, 2, 3, 4},
        1,
        1,
        SECURITY_MANDATORY_MEDIUM_RID,
        SECURITY_MANDATORY_MEDIUM_RID,
    };
    evidence.architecture = {true, IMAGE_FILE_MACHINE_UNKNOWN, winexinfo::kTargetMachine};
    evidence.explorer_version = {
        winexinfo::FileVersionSource::FixedResource,
        std::wstring{winexinfo::kTargetExplorerVersion},
    };
    evidence.explorer_frame_version = {
        winexinfo::FileVersionSource::FixedResource,
        std::wstring{winexinfo::kTargetExplorerFrameVersion},
    };
    evidence.shell32_version = {
        winexinfo::FileVersionSource::FixedResource,
        std::wstring{winexinfo::kTargetShell32Version},
    };
    evidence.mitigations = {true, false, true, false, true, true, false};
    return evidence;
}

void RequireCode(
    const winexinfo::TargetValidationEvidence& evidence,
    const winexinfo::ErrorCode expected) {
    const winexinfo::TargetValidationResult result = winexinfo::ValidateTargetEvidence(evidence);
    WXI_REQUIRE_EQ(result.status.code, expected);
}

WXI_TEST(target_validation_exact_target_passes, "target_validation.exact_target_passes") {
    const winexinfo::TargetValidationResult result =
        winexinfo::ValidateTargetEvidence(ExactTarget());

    WXI_REQUIRE(result.status.ok());
    WXI_REQUIRE(!result.rows.empty());
    for (const winexinfo::TargetValidationRow& row : result.rows) {
        WXI_REQUIRE(row.passed);
    }
}

WXI_TEST(target_validation_requires_exact_ubr, "target_validation.requires_exact_ubr") {
    winexinfo::TargetValidationEvidence missing = ExactTarget();
    missing.os.ubr_present = false;
    RequireCode(missing, winexinfo::ErrorCode::UNSUPPORTED_OS_BUILD);

    winexinfo::TargetValidationEvidence alternate = ExactTarget();
    alternate.os.ubr = winexinfo::kTargetOsUbr + 1;
    RequireCode(alternate, winexinfo::ErrorCode::UNSUPPORTED_OS_BUILD);
}

WXI_TEST(target_validation_uses_file_identity_not_path, "target_validation.uses_file_identity_not_path") {
    winexinfo::TargetValidationEvidence evidence = ExactTarget();
    evidence.file_identity.target_file_id[15] = 99;
    RequireCode(evidence, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);
}

WXI_TEST(target_validation_requires_cache_only_catalog, "target_validation.requires_cache_only_catalog") {
    winexinfo::TargetValidationEvidence cacheMiss = ExactTarget();
    cacheMiss.signature.catalog_found = false;
    cacheMiss.signature.trusted = false;
    cacheMiss.signature.trust_hresult = TRUST_E_NOSIGNATURE;
    RequireCode(cacheMiss, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);

    winexinfo::TargetValidationEvidence networkCapable = ExactTarget();
    networkCapable.signature.cache_only = false;
    RequireCode(networkCapable, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);

    winexinfo::TargetValidationEvidence revocationEnabled = ExactTarget();
    revocationEnabled.signature.revocation_none = false;
    RequireCode(revocationEnabled, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);

    winexinfo::TargetValidationEvidence embedded = ExactTarget();
    embedded.signature.embedded_signature_used = true;
    RequireCode(embedded, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);
}

WXI_TEST(target_validation_requires_exact_signer, "target_validation.requires_exact_signer") {
    winexinfo::TargetValidationEvidence evidence = ExactTarget();
    evidence.signature.signer_subject = L"CN=Microsoft Corporation";
    RequireCode(evidence, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);
}

WXI_TEST(target_validation_requires_same_principal, "target_validation.requires_same_principal") {
    winexinfo::TargetValidationEvidence sid = ExactTarget();
    sid.principal.target_sid.push_back(5);
    RequireCode(sid, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);

    winexinfo::TargetValidationEvidence session = ExactTarget();
    ++session.principal.target_session;
    RequireCode(session, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);

    winexinfo::TargetValidationEvidence integrity = ExactTarget();
    integrity.principal.target_integrity = SECURITY_MANDATORY_HIGH_RID;
    RequireCode(integrity, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);
}

WXI_TEST(target_validation_requires_native_amd64, "target_validation.requires_native_amd64") {
    winexinfo::TargetValidationEvidence wow64 = ExactTarget();
    wow64.architecture.process_machine = IMAGE_FILE_MACHINE_I386;
    RequireCode(wow64, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);

    winexinfo::TargetValidationEvidence arm64 = ExactTarget();
    arm64.architecture.native_machine = IMAGE_FILE_MACHINE_ARM64;
    RequireCode(arm64, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);
}

WXI_TEST(target_validation_requires_fixed_versions, "target_validation.requires_fixed_versions") {
    winexinfo::TargetValidationEvidence explorer = ExactTarget();
    explorer.explorer_version.value = L"10.0.26100.1";
    RequireCode(explorer, winexinfo::ErrorCode::UNSUPPORTED_EXPLORER_BUILD);

    winexinfo::TargetValidationEvidence frame = ExactTarget();
    frame.explorer_frame_version.value = L"10.0.26100.1";
    RequireCode(frame, winexinfo::ErrorCode::UNSUPPORTED_EXPLORER_BUILD);

    winexinfo::TargetValidationEvidence shell = ExactTarget();
    shell.shell32_version.value = L"10.0.26100.1";
    RequireCode(shell, winexinfo::ErrorCode::UNSUPPORTED_EXPLORER_BUILD);

    winexinfo::TargetValidationEvidence alternate = ExactTarget();
    alternate.explorer_version.source = winexinfo::FileVersionSource::Alternate;
    RequireCode(alternate, winexinfo::ErrorCode::UNSUPPORTED_EXPLORER_BUILD);
}

WXI_TEST(target_validation_requires_exact_mitigations, "target_validation.requires_exact_mitigations") {
    winexinfo::TargetValidationEvidence extensionPoints = ExactTarget();
    extensionPoints.mitigations.disable_extension_points = true;
    RequireCode(extensionPoints, winexinfo::ErrorCode::TARGET_MITIGATION_BLOCKED);

    winexinfo::TargetValidationEvidence signedOnly = ExactTarget();
    signedOnly.mitigations.microsoft_signed_only = true;
    RequireCode(signedOnly, winexinfo::ErrorCode::TARGET_MITIGATION_BLOCKED);

    winexinfo::TargetValidationEvidence cfg = ExactTarget();
    cfg.mitigations.cfg_enabled = false;
    RequireCode(cfg, winexinfo::ErrorCode::TARGET_MITIGATION_BLOCKED);

    winexinfo::TargetValidationEvidence strictCfg = ExactTarget();
    strictCfg.mitigations.strict_cfg = true;
    RequireCode(strictCfg, winexinfo::ErrorCode::TARGET_MITIGATION_BLOCKED);
}

WXI_TEST(target_validation_retains_capture_error, "target_validation.retains_capture_error") {
    winexinfo::TargetValidationEvidence evidence = ExactTarget();
    evidence.capture_status = {
        winexinfo::ErrorCode::TARGET_VALIDATION_FAILED,
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
        ERROR_ACCESS_DENIED,
    };

    const winexinfo::TargetValidationResult result =
        winexinfo::ValidateTargetEvidence(evidence);
    WXI_REQUIRE_EQ(result.status.code, winexinfo::ErrorCode::TARGET_VALIDATION_FAILED);
    WXI_REQUIRE_EQ(result.status.hresult, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(result.status.win32, DWORD{ERROR_ACCESS_DENIED});
}

}  // namespace
