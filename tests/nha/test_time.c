/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>
#include <errno.h>

static u64 monotonic;
static i64 wall=0x100000003LL;
static i64 slept;
static int yielded, clock_error;
static KThread thread;

u64 __wrap_plat_ns(void){ return monotonic; }
i64 __wrap_plat_unix_time(void){ if(clock_error) errno=EOVERFLOW; return wall; }
void __wrap_plat_sleep_ns(i64 ns){ slept=ns; monotonic+=(u64)ns; }
void __wrap_plat_yield(void){ yielded++; }

int main(void){
  tls_thread=&thread;
  monotonic=1000000000;
  assert(rt_ticks()==19200000);
  IpcCtx ctx={0};
  NhaCall call={0};
  assert(nha_time_get(NULL,&ctx,&call)==0 && ctx.out_size==8);
  u64 value;
  memcpy(&value,ctx.out,8);
  assert(value==(u64)wall);
  wall=1;
  ctx.out_size=0;
  assert(nha_time_get(NULL,&ctx,&call)==0 && ctx.out_size==8);
  memcpy(&value,ctx.out,8);
  assert(value==1 && rt_ticks()==19200000);
  thread.cpu.x[0]=25000; thread.cpu.x[30]=19;
  nha_sleep_thread(&thread.cpu);
  assert(thread.cpu.x[0]==25000 && thread.sleep_lr==19 && thread.sleep_n==1 && slept==25000);
  assert(rt_ticks()==19200480);
  thread.cpu.x[0]=(u64)-2;
  nha_sleep_thread(&thread.cpu);
  thread.cpu.x[0]=0;
  nha_sleep_thread(&thread.cpu);
  assert(yielded==2 && thread.cpu.x[0]==0 && thread.sleep_n==3);
  wall=-1; clock_error=1; ctx.out_size=0;
  assert(nha_time_get(NULL,&ctx,&call)==RC(116,200) && ctx.out_size==0);
  assert(call.host_error.domain==PERR_ERRNO && call.host_error.code==EOVERFLOW);
  return 0;
}
