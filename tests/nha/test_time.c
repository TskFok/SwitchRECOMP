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
static int host_wait_calls, inject_timeout, legacy_wall_calls;
static KThread thread;
static KEvent* timed_event;
static PMutex readers_lock;
static PCond readers_cv;
static int readers_ready, readers_done;

#if defined(_WIN32)
typedef BOOL (WINAPI *WindowsWait)(PCONDITION_VARIABLE,PSRWLOCK,DWORD,ULONG);
extern WindowsWait __real___imp_SleepConditionVariableSRW;
static int check_milliseconds, conversion_checks;
static DWORD expected_ms;

static BOOL WINAPI record_windows_wait(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags){
  if(!check_milliseconds) return __real___imp_SleepConditionVariableSRW(cv,lock,ms,flags);
  assert(cv==&thread.cv.c && lock==&g_klock.l && ms==expected_ms && !flags);
  conversion_checks++;
  SetLastError(ERROR_TIMEOUT);
  return FALSE;
}

WindowsWait __wrap___imp_SleepConditionVariableSRW=record_windows_wait;
#endif

u64 __wrap_plat_ns(void){ return monotonic; }
i64 __wrap_plat_unix_time(void){ if(clock_error) errno=EOVERFLOW; return wall; }
void __wrap_plat_sleep_ns(i64 ns){ slept=ns; monotonic+=(u64)ns; }
void __wrap_plat_yield(void){ yielded++; }

int __real_pc_wait(PCond* cv, PMutex* lock, i64 ns);
int __wrap_pc_wait(PCond* cv, PMutex* lock, i64 ns){
  if(!inject_timeout) return __real_pc_wait(cv,lock,ns);
  assert(cv==&thread.cv && lock==&g_klock && ns==37);
  assert(timed_event->o.refs==2 && timed_event->o.nwait==1 && timed_event->o.waiters[0]==&thread);
  host_wait_calls++;
  monotonic+=(u64)ns;
  return 1;
}

static int legacy_wall(SvcObj* obj, IpcCtx* ctx){
  (void)obj; (void)ctx;
  __atomic_add_fetch(&legacy_wall_calls,1,__ATOMIC_RELAXED);
  return 0;
}

static void wall_request(KThread* caller, IpcCtx* ctx){
  SvcObj service={.name="time",.ipc_family="time",.nha_kind=NHA_TIME,.dispatch=legacy_wall};
  NhaCall call;
  *ctx=(IpcCtx){.cmd=300,.th=caller};
  nha_dispatch_ipc(&service,ctx,&call);
  nha_end_ipc(&call,ctx,0);
  assert(!__atomic_load_n(&legacy_wall_calls,__ATOMIC_RELAXED));
}

static void read_ticks(void* unused){
  (void)unused;
  KThread caller={0};
  tls_thread=&caller;
  caller.cpu.thread=&caller;
  caller.cpu.rt=&g_rtapi;
  pm_lock(&readers_lock);
  readers_ready++;
  pc_broadcast(&readers_cv);
  while(readers_ready!=2) pc_wait(&readers_cv,&readers_lock,-1);
  pm_unlock(&readers_lock);
  for(int i=0;i<16;i++){
    caller.cpu.x[1]=0xCAFE;
    swr_svc(&caller.cpu,0x1e);
    assert(caller.cpu.x[0]==19200480 && caller.cpu.x[1]==0xCAFE);
    IpcCtx ctx;
    errno=EIO;
    wall_request(&caller,&ctx);
    u64 seconds;
    memcpy(&seconds,ctx.out,8);
    assert(!ctx.rc && ctx.out_size==8 && seconds==1);
  }
  pm_lock(&readers_lock);
  readers_done++;
  pc_broadcast(&readers_cv);
  pm_unlock(&readers_lock);
}

int main(void){
  assert(!nha_init());
  kernel_init(); mem_init();
  tls_thread=&thread;
  thread.cpu.thread=&thread;
  thread.cpu.rt=&g_rtapi;
  pc_init(&thread.cv);
  assert(rt_ticks()==0);
  monotonic=52;
  assert(rt_ticks()==0);
  monotonic=53;
  assert(rt_ticks()==1);
  monotonic=1000000000;
  assert(rt_ticks()==19200000);
  IpcCtx ctx;
  wall_request(&thread,&ctx);
  assert(ctx.rc==0 && ctx.out_size==8);
  u64 value;
  memcpy(&value,ctx.out,8);
  assert(value==(u64)wall);
  wall=1;
  wall_request(&thread,&ctx);
  assert(ctx.rc==0 && ctx.out_size==8);
  memcpy(&value,ctx.out,8);
  assert(value==1 && rt_ticks()==19200000);
  thread.cpu.x[0]=25000; thread.cpu.x[30]=19;
  swr_svc(&thread.cpu,0x0b);
  assert(thread.cpu.x[0]==25000 && thread.sleep_lr==19 && thread.sleep_n==1 && slept==25000);
  assert(rt_ticks()==19200480);
  pm_init(&readers_lock);
  pc_init(&readers_cv);
  assert(!plat_thread(read_ticks,NULL,1<<20) && !plat_thread(read_ticks,NULL,1<<20));
  pm_lock(&readers_lock);
  while(readers_done!=2) pc_wait(&readers_cv,&readers_lock,-1);
  pm_unlock(&readers_lock);
  pc_destroy(&readers_cv);
  thread.cpu.x[0]=(u64)-2;
  swr_svc(&thread.cpu,0x0b);
  thread.cpu.x[0]=0;
  swr_svc(&thread.cpu,0x0b);
  assert(yielded==2 && thread.cpu.x[0]==0 && thread.sleep_n==3);
  wall=-1;
  wall_request(&thread,&ctx);
  assert(ctx.rc==0 && ctx.out_size==8);
  memcpy(&value,ctx.out,8);
  assert(value==UINT64_MAX);
  clock_error=1;
  wall_request(&thread,&ctx);
  assert(ctx.rc==RC(116,200) && ctx.out_size==0);

  timed_event=kevent_new("timeout test event",0);
  assert(timed_event && !mem_map(AS_BASE,0x1000,MS_NORMAL,PERM_RW));
  u32 handle=h_new(&timed_event->o);
  gw32(AS_BASE,handle);
  inject_timeout=1;
  u64 start=monotonic;
  thread.cpu.x[1]=AS_BASE;
  thread.cpu.x[2]=1;
  thread.cpu.x[3]=37;
  swr_svc(&thread.cpu,0x18);
  inject_timeout=0;
  assert(thread.cpu.x[0]==0xEA01 && !thread.cpu.x[1] && !thread.wait_kind && !thread.cancel);
  assert(host_wait_calls==1 && monotonic-start==37);
  assert(timed_event->o.refs==1 && !timed_event->o.nwait && !timed_event->signaled);
  assert(!h_close(handle));
#if defined(_WIN32)
  const struct { i64 ns; DWORD ms; } cases[]={
    {0,0}, {1,1}, {1000000,1}, {1000001,2}, {-1,INFINITE}, {INT64_MAX,INFINITE-1},
  };
  pm_lock(&g_klock);
  check_milliseconds=1;
  for(unsigned i=0;i<sizeof cases/sizeof *cases;i++){
    expected_ms=cases[i].ms;
    assert(__real_pc_wait(&thread.cv,&g_klock,cases[i].ns)==1);
  }
  assert(conversion_checks==sizeof cases/sizeof *cases);
  pm_unlock(&g_klock);
  fprintf(stderr,"NHA_TEST {\"wait_conversion_cases\":%d}\n",conversion_checks);
#endif
  pc_destroy(&thread.cv);
  fprintf(stderr,"NHA_TEST {\"host_wait_calls\":%d,\"host_wait_ns\":%llu,\"timeout_waiters_after\":0,"
          "\"legacy_wall_calls\":%d,\"wall_errno\":%d}\n",
          host_wait_calls,(unsigned long long)(monotonic-start),legacy_wall_calls,EOVERFLOW);
  return 0;
}
