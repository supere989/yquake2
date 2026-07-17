from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parents[1] / "src/client/cl_ml_harness.c"


def _source() -> str:
    return SOURCE.read_text(encoding="utf-8")


def test_telemetry_credential_is_environment_only_and_never_a_cvar():
    source = _source()
    assert '#define ML_CLIENT_TOKEN_ENV "Q2_ML_CLIENT_TELEMETRY_TOKEN"' in source
    assert "getenv(ML_CLIENT_TOKEN_ENV)" in source
    assert 'Cvar_Get("ml_telemetry_token"' not in source
    assert "static cvar_t *ml_telemetry_token" not in source
    assert "CVAR_ARCHIVE" not in "\n".join(
        line for line in source.splitlines() if "token" in line.lower()
    )


def test_missing_or_malformed_token_fails_closed_before_address_admission():
    source = _source()
    resolve = source[source.index("static void ML_ResolveAddresses"):
                     source.index("void ML_HarnessInit")]
    assert "addresses_valid = false" in resolve
    assert "if (!ml_telemetry_token_valid)" in resolve
    assert "Com_Error(ERR_FATAL" in resolve
    loader = source[source.index("static qboolean ML_LoadTelemetryToken"):
                    source.index("static const char *ML_WeaponName")]
    assert "ML_CLIENT_TOKEN_MIN_LENGTH" in loader
    assert "length >= ML_CLIENT_TOKEN_SIZE" in loader
    for allowed in ("'.'", "'_'", "'~'", "'+'", "'/'", "'='", "'-'"):
        assert allowed in loader


def test_registration_uses_private_buffer_and_logs_never_reference_its_value():
    source = _source()
    assert "memcpy(registration.token, ml_telemetry_token" in source
    assert "ml_telemetry_token->string" not in source
    assert "memset(ml_telemetry_token, 0, sizeof(ml_telemetry_token))" in source
    print_calls = re.findall(r"(?:Com_Printf|Com_DPrintf)\([^;]+;", source, re.DOTALL)
    assert print_calls
    assert all("ml_telemetry_token" not in call for call in print_calls)
