/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>
#include <errno.h>

static int native_calls, legacy_calls;
static char trace[4096];

int logf_(int level, const char* fmt, ...){
  (void)level;
  va_list args;
  va_start(args,fmt);
  size_t n=strlen(trace);
  vsnprintf(trace+n,sizeof trace-n,fmt,args);
  va_end(args);
  return 0;
}

void pm_init(PMutex* lock){ (void)lock; }
void pm_lock(PMutex* lock){ (void)lock; }
void pm_unlock(PMutex* lock){ (void)lock; }

u64 plat_ns(void){ static u64 now; return ++now; }

static int failed_open(SvcObj* obj, IpcCtx* ctx, NhaCall* call){
  (void)obj; (void)ctx;
  native_calls++;
  call->host_error=(PError){PERR_ERRNO,EIO};
  return RC(2,3005);
}

int nha_fs_open(SvcObj* o, IpcCtx* c, NhaCall* k){ return failed_open(o,c,k); }
int nha_fs_read(SvcObj* o, IpcCtx* c, NhaCall* k){ return failed_open(o,c,k); }
int nha_fs_size(SvcObj* o, IpcCtx* c, NhaCall* k){ return failed_open(o,c,k); }

int nha_time_get(SvcObj* obj, IpcCtx* ctx, NhaCall* call){
  (void)obj; (void)call;
  native_calls++;
  out64(ctx,123);
  return 0;
}

void nha_sleep_thread(CPU* c){ (void)c; native_calls++; }
void nha_kernel_svc(CPU* c, u32 imm, NhaCall* call){
  native_calls++;
  if(imm==0x09){
    c->x[0]=0xD001;
    call->host_error=(PError){PERR_ERRNO,EAGAIN};
  } else {
    assert(imm==0x18);
    c->x[0]=0xEA01;
  }
}

static int legacy(SvcObj* obj, IpcCtx* ctx){
  (void)obj; (void)ctx;
  legacy_calls++;
  return 0;
}

static u64 ticks(void){ native_calls++; return 19200000; }

int main(int argc, char** argv){
  assert(argc==2);
  const char* scenario=argv[1];
  int invalid=!strcmp(scenario,"invalid");
  int rc=nha_init();
  assert((rc!=0)==invalid);
  if(invalid) return 0;

  RtAPI api={.ticks=ticks};
  KThread thread={.id=42};
  thread.cpu.rt=&api;
  thread.cpu.pc=0x123;
  NhaCall call;
  int handled=nha_try_svc(&thread.cpu,0x1e,&call);
  assert(handled==(!strcmp(scenario,"time") || !strcmp(scenario,"override")));
  if(handled) assert(thread.cpu.x[0]==19200000);
  nha_end_svc(&call,&thread.cpu,0);

  SvcObj time={.name="time",.ipc_family="time",.nha_kind=NHA_TIME,.dispatch=legacy};
  IpcCtx ctx={.cmd=300,.th=&thread};
  nha_dispatch_ipc(&time,&ctx,&call);
  nha_end_ipc(&call,&ctx,0);
  assert(ctx.out_size==(!strcmp(scenario,"time")?8:0));

  SvcObj fs={.name="IFileSystem",.ipc_family="fsp-srv",.nha_kind=NHA_FS,.dispatch=legacy};
  memset(&ctx,0,sizeof ctx);
  u32 mode=1;
  ctx.raw=(const u8*)&mode; ctx.raw_size=sizeof mode; ctx.request_size=sizeof mode;
  ctx.cmd=8; ctx.th=&thread;
  int before=legacy_calls;
  nha_dispatch_ipc(&fs,&ctx,&call);
  nha_end_ipc(&call,&ctx,0);
  if(!strcmp(scenario,"fs")){
    assert(ctx.rc==RC(2,3005) && native_calls==1 && legacy_calls==before);
    assert(strstr(trace,"\"host_error\":{\"domain\":\"errno\",\"code\":"));
    assert(strstr(trace,"\"transport_rc\":0"));
    assert(strstr(trace,"\"service_rc\":1538562"));
    assert(strstr(trace,"\"guest_thread\":42"));
  } else {
    assert(ctx.rc==0 && legacy_calls==before+1);
  }
  assert(!nha_try_svc(&thread.cpu,0xffff,&call));
  thread.cpu.x[0]=0xF601;
  nha_end_svc(&call,&thread.cpu,0);

  if(!strcmp(scenario,"svc-errors")){
    const u32 calls[]={0x09,0x18};
    const u32 results[]={0xD001,0xEA01};
    before=legacy_calls;
    for(unsigned i=0;i<sizeof calls/sizeof *calls;i++){
      if(!nha_try_svc(&thread.cpu,calls[i],&call)) legacy_calls++;
      nha_end_svc(&call,&thread.cpu,0);
      assert(thread.cpu.x[0]==results[i]);
    }
    assert(legacy_calls==before && native_calls==2);
  }

  if(!strcmp(scenario,"fs")){
    mode=3;
    nha_dispatch_ipc(&fs,&ctx,&call);
    nha_end_ipc(&call,&ctx,0);
    assert(ctx.rc==0 && legacy_calls==before+1 && native_calls==1);
    assert(strstr(trace,"\"reason\":\"legacy_mount_or_mode\""));
  }
  return 0;
}
