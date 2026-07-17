from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER = (ROOT / "src/server/sv_ml_frame_barrier.c").read_text(encoding="utf-8")


def test_drain_sigkill_is_a_distinct_sealed_v1_fault():
    assert "ML_FRAME_BARRIER_TEST_FAULTS_V1=" in SERVER
    assert "same-life-hold,epoch-drain," in SERVER
    assert "drain-sigkill,load-delay,old-telemetry" in SERVER
    assert SERVER.index("epoch-drain") < SERVER.index("drain-sigkill")
    known = SERVER.split("static qboolean\nML_TestFaultKnown", 1)[1].split(
        "static qboolean\nML_TestFaultIs", 1
    )[0]
    assert '"epoch-drain", "drain-sigkill", "load-delay"' in known
    assert "fault_vocabulary=v1 marker=%s" in SERVER
    for sealed in (
        "sv_ml_frame_barrier_test_mode->flags |= CVAR_NOSET",
        "sv_ml_frame_barrier_test_fault->flags |= CVAR_NOSET",
        "sv_ml_frame_barrier_test_tick->flags |= CVAR_NOSET",
        "sv_ml_frame_barrier_test_map->flags |= CVAR_NOSET",
    ):
        assert sealed in SERVER


def test_unknown_fault_preflight_still_fails_closed_in_test_mode():
    reset = SERVER.split("void\nSV_MLFrameBarrierResetMap", 1)[1]
    assert "sv_ml_frame_barrier_test_mode->value" in reset
    assert "!ML_TestFaultKnown()" in reset
    assert "event=test_controls_reject" in reset
    assert "ML_FrameBarrierProtocolFault(&ml_barrier)" in reset
