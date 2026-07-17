from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HARNESS = (ROOT / "src/client/cl_ml_harness.c").read_text(encoding="utf-8")
INPUT = (ROOT / "src/client/cl_input.c").read_text(encoding="utf-8")
PARSE = (ROOT / "src/client/cl_parse.c").read_text(encoding="utf-8")


def _function(source: str, signature: str, next_signature: str) -> str:
    return source.split(signature, 1)[1].split(next_signature, 1)[0]


def test_zero_duration_refresh_cannot_finalize_a_stale_policy_command():
    refresh = _function(INPUT, "CL_RefreshCmd(void)", "CL_RefreshMove(void)")
    early_return = refresh.index("if (frame_msec < 1)")
    apply = refresh.index("ML_HarnessApplyAction(cmd);")
    prepared = refresh.index("ML_HarnessActionAnglesPrepared(cmd);")
    assert early_return < apply < prepared

    finalize = HARNESS.split(
        "void ML_HarnessFinalizeAction(usercmd_t *cmd)", 1
    )[1]
    assert "prepared_action_cmd != cmd" in finalize
    assert "prepared_action_tick != latest_action_tick" in finalize
    assert "cmd->buttons &= ~ML_HARNESS_BUTTON_GENERATION_MASK" in finalize


def test_ready_requires_current_command_apply_prepare_and_finalize():
    apply = _function(
        HARNESS, "void ML_HarnessApplyAction(usercmd_t *cmd)",
        "void ML_HarnessActionAnglesPrepared(usercmd_t *cmd)",
    )
    prepare = _function(
        HARNESS, "void ML_HarnessActionAnglesPrepared(usercmd_t *cmd)",
        "void ML_HarnessResetEpoch(void)",
    )
    ready = _function(
        HARNESS, "static void ML_MaybeSendReady(void)",
        "static qboolean ML_MapEqual",
    )
    finalize = HARNESS.split(
        "void ML_HarnessFinalizeAction(usercmd_t *cmd)", 1
    )[1]
    assert "view_rebase.pending" in apply
    assert "preparing_action_cmd = cmd" in apply
    assert "preparing_action_tick = latest_action_tick" in apply
    assert "preparing_action_cmd != cmd" in prepare
    assert "prepared_action_cmd = cmd" in prepare
    assert "finalized_action_tick != latest_action_tick" in ready
    assert "finalized_action_tick = latest_action_tick" in finalize
    assert finalize.index("finalized_action_tick = latest_action_tick") < finalize.index(
        "ML_MaybeSendReady();"
    )


def test_life_arm_and_map_reset_clear_prepared_command_state():
    life_arm = HARNESS.split(
        "if (role_result > 0 || life_result > 0 || settle_result > 0)", 1
    )[1].split(
        "NET_SendPacket", 1
    )[0]
    reset = _function(
        HARNESS, "void ML_HarnessResetEpoch(void)",
        "void ML_HarnessServerFrame(void)",
    )
    assert "ML_ClearPreparedAction();" in life_arm
    assert "ML_ClearActionEpoch();" in reset
    assert "memset(&view_rebase, 0, sizeof(view_rebase));" in reset


def test_life_rebase_requires_causal_public_normal_role():
    process = _function(
        HARNESS, "static void ML_ProcessTelemetry(const byte *data)",
        "qboolean ML_HarnessPacket",
    )
    helper = (ROOT / "src/client/header/ml_harness_view_rebase.h").read_text(
        encoding="utf-8"
    )
    accept = helper.split("ML_HarnessViewAcceptLife", 1)[1].split(
        "ML_HarnessViewObserveSettleCausal", 1
    )[0]
    call = process.split("ML_HarnessViewAcceptLife", 1)[1].split(
        "if (life_result < 0)", 1
    )[0]
    assert "ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL" in call
    assert "if (!role_public_pm_normal)" in accept
    assert "BeginIntermission" in accept
    assert "state->accepted_life_epoch = life_epoch" in accept
    assert "state->pending = 0" in accept
    assert "state->settle_tracking = 0" in accept


def test_rebase_runs_only_from_a_valid_normal_protocol_playerstate():
    valid_frame = PARSE.split("if (cl.frame.valid)", 1)[1].split(
        "/* getting a valid frame", 1
    )[0]
    server_frame = _function(
        HARNESS, "void ML_HarnessServerFrame(void)",
        "void ML_HarnessFinalizeAction(usercmd_t *cmd)",
    )
    view_frame = server_frame.split("if (!view_rebase.pending)", 1)[1]
    assert "ML_HarnessServerFrame();" in valid_frame
    assert "ML_HarnessViewSnapshotReady" in server_frame
    assert server_frame.index("ML_HarnessViewSnapshotReady") < server_frame.index(
        "event=life_view_rebase_nonplayable"
    )
    assert "pending_kind ==" in server_frame
    assert "ML_HARNESS_VIEW_REBASE_SETTLE_FRAME" in server_frame
    assert "pm_type == PM_DEAD" in server_frame
    assert "pm_type == PM_GIB" in server_frame
    assert "nonplayable view rebase kind=" in server_frame
    assert "pm_type == PM_FREEZE" not in view_frame
    assert "pm_type == PM_SPECTATOR" not in view_frame
    assert "event=life_view_rebase_nonplayable" in server_frame
    assert "applied_look_tick = view_rebase.pending_applied_action_tick" in server_frame
    assert "applied_look_tick = view_rebase.pending_applied_action_tick" in server_frame
    assert "playerstate.viewangles" in server_frame
    assert "playerstate.pmove.delta_angles" in server_frame


def test_only_public_normal_causal_packets_can_arm_teleport_settle():
    process = _function(
        HARNESS, "static void ML_ProcessTelemetry(const byte *data)",
        "qboolean ML_HarnessPacket",
    )
    apply = _function(
        HARNESS, "void ML_HarnessApplyAction(usercmd_t *cmd)",
        "void ML_HarnessActionAnglesPrepared(usercmd_t *cmd)",
    )
    settle_call = process.index("ML_HarnessViewObserveSettleCausal")
    public_normal_guard = process.index(
        "causal->flags & ML_CAUSAL_ROLE_PUBLIC_PM_NORMAL"
    )
    leave_normal = process.index("ML_HarnessViewLeavePublicNormal")
    assert public_normal_guard < settle_call < leave_normal
    assert "ML_HarnessViewObserveSettleCausal" in process
    assert "ML_HarnessViewLeavePublicNormal" in process
    assert "life_result > 0 || settle_result > 0" in process
    assert "ML_HARNESS_VIEW_REBASE_SETTLE_FRAME" in process
    assert "PMF_TIME_TELEPORT" in apply
    assert "ML_HarnessViewApplyPolicyPitch" in apply
    assert "pm_time > 0" not in apply


def test_public_normal_t1_clears_tracking_and_freeze_cannot_leave_it_armed():
    helper = (ROOT / "src/client/header/ml_harness_view_rebase.h").read_text(
        encoding="utf-8"
    )
    settle = helper.split("ML_HarnessViewObserveSettleCausal", 1)[1].split(
        "ML_HarnessViewLeavePublicNormal", 1
    )[0]
    leave = helper.split("ML_HarnessViewLeavePublicNormal", 1)[1].split(
        "ML_HarnessViewSnapshotReady", 1
    )[0]
    assert "if (!transition_trainable)" in settle
    assert "state->settle_tracking = 0" in settle
    assert "state->settling_seen = 0" in settle
    assert "ML_HARNESS_VIEW_REBASE_SETTLE_FRAME" in leave
    assert "return -1" in leave
    assert "state->settle_tracking = 0" in leave
    assert "state->settling_seen = 0" in leave
