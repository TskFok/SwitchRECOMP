/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#ifndef SWR_NATIVE_HOS_H
#define SWR_NATIVE_HOS_H
#include "rt.h"

/* Internal object identity, carried unchanged through session and domain clones. */
enum { NHA_UNTYPED, NHA_TIME, NHA_FS, NHA_LEGACY_FS, NHA_FILE, NHA_NATIVE_FILE, NHA_GENERIC };
enum { NHA_RETURNED, NHA_STUB, NHA_TERMINATED, NHA_UNIMPLEMENTED };

typedef struct {
  u64 id, start, thread, pc;
  const char* reason;
  PError host_error;
  int native;
  enum { NHA_RC, NHA_VALUE, NHA_VOID } result;
  u32 imm;
  char api_id[192];
} NhaCall;

int nha_init(void);
void nha_trace_finish(void);
int nha_try_svc(CPU* c, u32 imm, NhaCall* call);
void nha_end_svc(NhaCall* call, CPU* c, int status);
void nha_dispatch_ipc(SvcObj* obj, IpcCtx* ctx, NhaCall* call);
void nha_end_ipc(NhaCall* call, IpcCtx* ctx, u64 transport_rc);
void nha_begin_file_close(NhaCall* call, SvcObj* obj, KThread* thread);
void nha_end_file_close(NhaCall* call, u32 rc);

u64 rt_ticks(void);

/* Implementations provide the guest result and carry any raw host error in call. */
void nha_sleep_thread(CPU* c);
void nha_kernel_svc(CPU* c, u32 imm, NhaCall* call);
int nha_time_get(SvcObj* obj, IpcCtx* c, NhaCall* call);
int nha_fs_open(SvcObj* obj, IpcCtx* c, NhaCall* call);
int nha_fs_read(SvcObj* obj, IpcCtx* c, NhaCall* call);
int nha_fs_size(SvcObj* obj, IpcCtx* c, NhaCall* call);

#endif
