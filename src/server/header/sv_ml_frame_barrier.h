#ifndef SV_ML_FRAME_BARRIER_H
#define SV_ML_FRAME_BARRIER_H

#include "server.h"
#include "sv_ml_frame_barrier_core.h"

void SV_MLFrameBarrierInit(void);
qboolean SV_MLFrameBarrierEnabled(void);
qboolean SV_MLFrameBarrierModeEnabled(void);
qboolean SV_MLFrameBarrierEpochDrain(void);
void SV_MLFrameBarrierResetMap(void);
qboolean SV_MLFrameBarrierAdmit(const char *userinfo, int slot,
	char *reason, size_t reason_size);
void SV_MLFrameBarrierConnected(client_t *client);
void SV_MLFrameBarrierDisconnected(client_t *client, const char *reason);
void SV_MLFrameBarrierStageMove(client_t *client,
	const usercmd_t *oldest, const usercmd_t *oldcmd,
	const usercmd_t *newcmd, int net_drop);
void SV_MLFrameBarrierDrainMove(client_t *client, const usercmd_t *newcmd);
void SV_MLFrameBarrierValidateClient(client_t *client);
void SV_MLFrameBarrierValidateLiveness(void);
qboolean SV_MLFrameBarrierStringCommand(client_t *client, const char *text);
ml_barrier_gate_result_t SV_MLFrameBarrierPoll(void);
void SV_MLFrameBarrierApplyCommands(void);
void SV_MLFrameBarrierCommitted(void);
const char *SV_MLFrameBarrierFault(void);

/* The ordinary command accounting/game call, exposed only to the barrier
 * commit path. */
void SV_ApplyClientCommand(client_t *client, usercmd_t *cmd);

#endif
