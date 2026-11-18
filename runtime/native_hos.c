/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"

enum { TIME, FS, THREAD, SYNC, GROUPS };
enum { FS_OPEN, FS_READ, FS_SIZE, FS_CLOSE, WALL_TIME };

typedef struct {
  const char* id;
  int group;
} NhaEntry;

static const NhaEntry svcs[0x46]={
  [0x08]={"svc.0x08",THREAD}, [0x09]={"svc.0x09",THREAD}, [0x0a]={"svc.0x0a",THREAD},
  [0x0b]={"svc.0x0b",TIME}, [0x1e]={"svc.0x1e",TIME},
  [0x11]={"svc.0x11",SYNC}, [0x12]={"svc.0x12",SYNC}, [0x17]={"svc.0x17",SYNC},
  [0x18]={"svc.0x18",SYNC}, [0x19]={"svc.0x19",SYNC}, [0x45]={"svc.0x45",SYNC},
  [0x1a]={"svc.0x1a",SYNC}, [0x1b]={"svc.0x1b",SYNC},
};

static const NhaEntry ipcs[]={
  [FS_OPEN]={"ipc.fsp-srv.IFileSystem.8",FS},
  [FS_READ]={"ipc.fsp-srv.IFile.0",FS},
  [FS_SIZE]={"ipc.fsp-srv.IFile.4",FS},
  [FS_CLOSE]={"ipc.fsp-srv.IFile.close",FS},
  [WALL_TIME]={"ipc.time.time.300",TIME},
};

static int enabled, trace_enabled;
static int group[GROUPS];
static signed char svc_override[sizeof svcs/sizeof *svcs];
static signed char ipc_override[sizeof ipcs/sizeof *ipcs];
static PMutex trace_lock;
static u64 next_call, completed_calls, dropped_records, truncated_ids;
static int trace_finished;

static int config_error(const char* key){
  WARN("Native Horizon Adapter: invalid %s configuration\n",key);
  return -1;
}

static int read_switch(const char* key, int* value){
  const char* s=getenv(key);
  if(!s || !strcmp(s,"0")){ *value=0; return 0; }
  if(!strcmp(s,"1")){ *value=1; return 0; }
  return config_error(key);
}

static signed char* find_override(const NhaEntry* entries, size_t n, signed char* values,
                                  const char* name, size_t len){
  for(size_t i=0;i<n;i++)
    if(entries[i].id && strlen(entries[i].id)==len && !memcmp(entries[i].id,name,len))
      return &values[i];
  return NULL;
}

static int selected(const NhaEntry* entry, int override){
  return enabled && (override? override>0 : group[entry->group]);
}

int nha_init(void){
  static const char* const keys[]={"SWR_NATIVE_TIME","SWR_NATIVE_FS","SWR_NATIVE_THREAD","SWR_NATIVE_SYNC"};
  if(read_switch("SWR_NATIVE_HOS",&enabled) || read_switch("SWR_NATIVE_TRACE",&trace_enabled)) return -1;
  for(int i=0;i<GROUPS;i++) if(read_switch(keys[i],&group[i])) return -1;
  memset(svc_override,0,sizeof svc_override);
  memset(ipc_override,0,sizeof ipc_override);
  const char* p=getenv("SWR_NATIVE_APIS");
  while(p && *p){
    const char* end=strchr(p,',');
    if(!end) end=p+strlen(p);
    const char* eq=memchr(p,'=',(size_t)(end-p));
    if(!eq) return config_error("SWR_NATIVE_APIS");
    signed char* slot=find_override(svcs,sizeof svcs/sizeof *svcs,svc_override,p,(size_t)(eq-p));
    if(!slot) slot=find_override(ipcs,sizeof ipcs/sizeof *ipcs,ipc_override,p,(size_t)(eq-p));
    if(!slot || *slot || end-eq!=7) return config_error("SWR_NATIVE_APIS");
    if(!memcmp(eq+1,"native",6)) *slot=1;
    else if(!memcmp(eq+1,"legacy",6)) *slot=-1;
    else return config_error("SWR_NATIVE_APIS");
    if(!*end) break;
    p=end+1;
    if(!*p) return config_error("SWR_NATIVE_APIS");
  }
  for(int i=FS_READ;i<=FS_CLOSE;i++)
    if(selected(&ipcs[i],ipc_override[i])!=selected(&ipcs[FS_OPEN],ipc_override[FS_OPEN]))
      return config_error("SWR_NATIVE_APIS (IFile lifetime)");
  if(trace_enabled){
    pm_init(&trace_lock);
    if(atexit(nha_trace_finish)) return config_error("SWR_NATIVE_TRACE");
  }
  return 0;
}

static const char* route_reason(const NhaEntry* entry, int override, int native){
  if(!entry || !entry->id) return "unclassified";
  if(!enabled) return "master_disabled";
  if(override) return "override";
  return native? "group_enabled" : "group_disabled";
}

static void emit(const NhaCall* call, const char* phase, const char* outcome, const char* result, u64 duration){
  char error[80]="null";
  if(call->host_error.domain)
    snprintf(error,sizeof error,"{\"domain\":\"%s\",\"code\":%u}",
             call->host_error.domain==PERR_WIN32? "win32" : "errno",call->host_error.code);
  if(LOG("{\"schema_version\":1,\"call_id\":%llu,\"phase\":\"%s\",\"api_id\":\"%s\","
      "\"route\":\"%s\",\"reason\":\"%s\",\"guest_thread\":%llu,\"guest_pc\":%llu,"
      "\"outcome\":\"%s\",\"guest_result\":%s,\"host_error\":%s,\"duration_ns\":%llu}\n",
      (unsigned long long)call->id,phase,call->api_id,
      call->native? "native" : "legacy",call->reason,(unsigned long long)call->thread,
      (unsigned long long)call->pc,outcome,result,error,(unsigned long long)duration))
    dropped_records++;
}

void nha_trace_finish(void){
  if(!trace_enabled) return;
  pm_lock(&trace_lock);
  if(!trace_finished){
    trace_finished=1;
    LOG("{\"schema_version\":1,\"phase\":\"trace_summary\",\"calls\":%llu,"
        "\"incomplete_calls\":%llu,\"dropped_records\":%llu,\"truncated_ids\":%llu}\n",
        (unsigned long long)next_call,(unsigned long long)(next_call-completed_calls),
        (unsigned long long)dropped_records,(unsigned long long)truncated_ids);
  }
  pm_unlock(&trace_lock);
}

static void trace_begin(NhaCall* call, const CPU* cpu, const KThread* thread, const char* reason){
  pm_lock(&trace_lock);
  if(!trace_finished){
    call->thread=thread? thread->id : 0;
    call->pc=cpu? cpu->pc : 0;
    call->reason=reason;
    call->id=++next_call;
    call->start=plat_ns();
    if(!strcmp(reason,"api_id_truncated")) truncated_ids++;
    emit(call,"begin","pending","null",0);
  }
  pm_unlock(&trace_lock);
}

static void trace_end(NhaCall* call, const char* outcome, const char* result){
  pm_lock(&trace_lock);
  if(!trace_finished){
    emit(call,"end",outcome,result,plat_ns()-call->start);
    completed_calls++;
  }
  pm_unlock(&trace_lock);
  call->id=0;
}

int nha_try_svc(CPU* c, u32 imm, NhaCall* call){
  call->id=0;
  call->host_error=(PError){0};
  const NhaEntry* entry=imm<sizeof svcs/sizeof *svcs? &svcs[imm] : NULL;
  int has_native=entry && entry->id;
  int override=has_native? svc_override[imm] : 0;
  call->native=has_native && selected(entry,override);
  if(trace_enabled){
    call->imm=imm;
    call->result=imm==0x1e || imm==0x10? NHA_VALUE : imm==0x0b? NHA_VOID : NHA_RC;
    snprintf(call->api_id,sizeof call->api_id,"svc.0x%02x",imm);
    trace_begin(call,c,c->thread,route_reason(entry,override,call->native));
  }
  if(!call->native) return 0;
  if(imm==0x1e) c->x[0]=c->rt->ticks();
  else if(imm==0x0b) nha_sleep_thread(c);
  else nha_kernel_svc(c,imm,call);
  return 1;
}

void nha_end_svc(NhaCall* call, CPU* c, int status){
  if(!call->id) return;
  char result[160]="null";
  const char* outcome="ok";
  if(status==NHA_TERMINATED) outcome="terminated";
  else if(call->result==NHA_VOID) strcpy(result,"{\"kind\":\"void\"}");
  else {
    const char* kind=call->result==NHA_VALUE? "value" : "rc";
    int n=snprintf(result,sizeof result,"{\"kind\":\"%s\",\"x0\":%llu",kind,(unsigned long long)c->x[0]);
    if((!c->x[0] && (call->imm==0x08 || call->imm==0x45)) || call->imm==0x18)
      n+=snprintf(result+n,sizeof result-(size_t)n,",\"x1\":%llu",(unsigned long long)c->x[1]);
    if(!c->x[0] && call->imm==0x45)
      n+=snprintf(result+n,sizeof result-(size_t)n,",\"x2\":%llu",(unsigned long long)c->x[2]);
    strcpy(result+n,"}");
    if(status==NHA_STUB) outcome="legacy_stub";
    else if(status==NHA_UNIMPLEMENTED) outcome="unimplemented";
    else if(call->result==NHA_RC && c->x[0]) outcome="error";
  }
  trace_end(call,outcome,result);
}

static int ipc_index(const SvcObj* obj, u32 cmd){
  switch(obj->nha_kind){
    case NHA_TIME: return cmd==300? WALL_TIME : -1;
    case NHA_FS: case NHA_LEGACY_FS: return cmd==8? FS_OPEN : -1;
    case NHA_FILE: case NHA_NATIVE_FILE: return cmd==0? FS_READ : cmd==4? FS_SIZE : -1;
    default: return -1;
  }
}

/* Names originate in the service registry and constructors. Encode separators and
   punctuation in unknown API IDs so they stay unambiguous and JSON-safe. */
static int encode_id(char* dst, size_t cap, const char* name){
  size_t n=0;
  const char hex[]="0123456789ABCDEF";
  for(;*name;name++){
    u8 c=(u8)*name;
    int plain=(c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') ||
              c=='-' || c=='_' || c==':';
    if(n+(plain?1:3)>=cap) break;
    if(plain) dst[n++]=(char)c;
    else { dst[n++]='%'; dst[n++]=hex[c>>4]; dst[n++]=hex[c&15]; }
  }
  dst[n]=0;
  return *name!=0;
}

void nha_dispatch_ipc(SvcObj* obj, IpcCtx* ctx, NhaCall* call){
  static int (*const handlers[])(SvcObj*,IpcCtx*,NhaCall*)={
    [FS_OPEN]=nha_fs_open, [FS_READ]=nha_fs_read, [FS_SIZE]=nha_fs_size, [WALL_TIME]=nha_time_get
  };
  *call=(NhaCall){0};
  int i=ipc_index(obj,ctx->cmd);
  const char* reason="unclassified";
  if(i>=0){
    call->native=selected(&ipcs[i],ipc_override[i]);
    reason=route_reason(&ipcs[i],ipc_override[i],call->native);
    if(i==FS_OPEN && (obj->nha_kind!=NHA_FS || (ctx->request_size>=4 && in32(ctx,0)!=1))){
      call->native=0;
      reason="legacy_mount_or_mode";
    }
    if(i==FS_READ || i==FS_SIZE){
      if(call->native && obj->nha_kind!=NHA_NATIVE_FILE) reason="legacy_file_owner";
      call->native=obj->nha_kind==NHA_NATIVE_FILE;
    }
  }
  if(i<0 && obj->nha_kind==NHA_NATIVE_FILE && ctx->cmd!=5){
    call->native=1;
    reason="native_file_owner";
  }
  if(trace_enabled){
    if(i>=0) snprintf(call->api_id,sizeof call->api_id,"%s",ipcs[i].id);
    else {
      char family[64], interface[96];
      if(encode_id(family,sizeof family,obj->ipc_family? obj->ipc_family : "unregistered") |
         encode_id(interface,sizeof interface,obj->nha_kind==NHA_GENERIC? "unknown" : obj->name? obj->name : "unknown")){
        reason="api_id_truncated";
      }
      snprintf(call->api_id,sizeof call->api_id,"ipc.%s.%s.%u",family,interface,ctx->cmd);
    }
    trace_begin(call,ctx->th? &ctx->th->cpu : NULL,ctx->th,reason);
  }
  ctx->rc=call->native && i>=0? (u32)handlers[i](obj,ctx,call) : (u32)obj->dispatch(obj,ctx);
  for(int j=0;j<ctx->nout_objs;j++)
    if(!ctx->out_objs[j]->ipc_family) ctx->out_objs[j]->ipc_family=obj->ipc_family;
}

void nha_end_ipc(NhaCall* call, IpcCtx* ctx, u64 transport_rc){
  if(!call->id) return;
  char result[112];
  snprintf(result,sizeof result,"{\"kind\":\"ipc\",\"transport_rc\":%llu,\"service_rc\":%u}",
           (unsigned long long)transport_rc,ctx->rc);
  trace_end(call,transport_rc || ctx->rc? "error" : ctx->legacy_stub? "legacy_stub" : "ok",result);
}

void nha_begin_file_close(NhaCall* call, SvcObj* obj, KThread* thread){
  call->id=0;
  call->host_error=(PError){0};
  if(!trace_enabled) return;
  call->native=obj->nha_kind==NHA_NATIVE_FILE;
  snprintf(call->api_id,sizeof call->api_id,"%s",ipcs[FS_CLOSE].id);
  trace_begin(call,thread? &thread->cpu : NULL,thread,call->native? "native_file_owner" : "legacy_file_owner");
}

void nha_end_file_close(NhaCall* call, u32 rc){
  if(!call->id) return;
  char result[96];
  snprintf(result,sizeof result,"{\"kind\":\"resource_close\",\"service_rc\":%u}",rc);
  trace_end(call,rc? "error" : "ok",result);
}
