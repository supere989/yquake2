#ifndef YQ2_ML_HARNESS_H
#define YQ2_ML_HARNESS_H

#include "client.h"

void ML_HarnessInit(void);
void ML_HarnessShutdown(void);
qboolean ML_HarnessHeadless(void);
void ML_HarnessPump(void);
qboolean ML_HarnessPacket(netadr_t from, const byte *data, int length);
void ML_HarnessApplyAction(usercmd_t *cmd);
void ML_HarnessFinalizeAction(usercmd_t *cmd);

#endif
