from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HARNESS = (ROOT / "src/client/cl_ml_harness.c").read_text(encoding="utf-8")
SERVER_BARRIER = (ROOT / "src/server/sv_ml_frame_barrier.c").read_text(
    encoding="utf-8"
)


def _function(signature: str, next_signature: str) -> str:
    return HARNESS.split(signature, 1)[1].split(next_signature, 1)[0]


def test_role_schema_is_one_way_and_has_no_legacy_fallback():
    assert "#define ML_CLIENT_WIRE_VERSION 8u" in HARNESS
    assert "#define ML_CAUSAL_VERSION        2u" in HARNESS
    assert "#define ML_CAUSAL_ROLE_PLAYING (1u << 20)" in HARNESS
    assert "#define ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL (1u << 21)" in HARNESS
    assert "((1u << 22) - 1u)" in HARNESS
    envelope = _function(
        "static qboolean ML_CausalEnvelopeValid",
        "static size_t ML_BoundedLength",
    )
    assert "ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL" in envelope
    assert "ML_CAUSAL_ROLE_PLAYING" in envelope
    assert "ML_CLIENT_WIRE_VERSION 7u" not in HARNESS
    assert "ML_CAUSAL_VERSION        1u" not in HARNESS
    assert "wire=8 barrier=1 capability=1" in SERVER_BARRIER
    assert "wire=7 barrier=1 capability=1" not in SERVER_BARRIER


def test_bootstrap_and_every_action_stage_are_role_fenced():
    pump = _function("void ML_HarnessPump(void)", "static void ML_ProcessTelemetry")
    process = _function("static void ML_ProcessTelemetry", "qboolean ML_HarnessPacket")
    apply = _function("void ML_HarnessApplyAction", "void ML_HarnessActionAnglesPrepared")
    prepare = _function("void ML_HarnessActionAnglesPrepared", "void ML_HarnessResetEpoch")
    finalize = HARNESS.split("void ML_HarnessFinalizeAction", 1)[1]
    ready = _function("static void ML_MaybeSendReady", "static qboolean ML_MapEqual")

    assert "ML_HarnessBootstrapRoleReady" in pump
    assert "pm_type == PM_NORMAL" in pump
    assert "ML_HarnessRoleArm" in process
    assert process.index("ML_HarnessRoleArm") < process.index("NET_SendPacket")
    for block in (apply, prepare, finalize, ready):
        assert "role_admission.pending" in block


def test_exact_public_role_fence_rejects_spectator_and_checks_normal():
    frame = _function("void ML_HarnessServerFrame", "void ML_HarnessFinalizeAction")
    assert "ML_HarnessRoleSnapshotReady" in frame
    assert "pm_type == PM_NORMAL" in frame
    assert "pm_type == PM_SPECTATOR" in frame
    assert "pm_type == PM_DEAD" in frame
    assert "pm_type == PM_GIB" in frame
    assert "pm_type == PM_FREEZE" in frame
    assert "routed role mismatch" in frame
