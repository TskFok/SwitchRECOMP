/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* HIPC/CMIF (+domains). requests get served on the calling thread, no server threads and no context switches */
#include "native_hos.h"

/* ============================================================ service objects */
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t ds){
  SvcObj* o=calloc(1,sizeof *o);
  if(!o) return NULL;
  o->name=name; o->dispatch=dispatch; o->refs=1;
  if(ds && !(o->data=calloc(1,ds))){ free(o); return NULL; }
  return o; }

void svcobj_unref(SvcObj* o){ if(!o) return; if(__atomic_sub_fetch(&o->refs,1,__ATOMIC_SEQ_CST)==0){ if(o->destroy) o->destroy(o); free(o->data); free(o); } }

static void domain_unref(Domain* d){ if(!d) return; pm_lock(&d->lk); int r=--d->refs; pm_unlock(&d->lk); if(r) return;
  for(int i=0;i<d->n;i++) if(d->obj[i]) svcobj_unref(d->obj[i]); free(d->obj); free(d); }

static void sess_destroy(KObj* k){ KSession* s=(KSession*)k; svcobj_unref(s->obj); domain_unref(s->domain); free(k->waiters); free(s); }

/* Publish an object with an owned reference, or leave ownership with the caller. */
static u32 domain_add(Domain* d, SvcObj* o){
  pm_lock(&d->lk);
  int id=-1;
  for(int i=1;i<d->n;i++) if(!d->obj[i]){ id=i; break; }
  if(id<0){
    if(d->n==d->cap){
      int cap=d->cap? d->cap*2 : 16;
      SvcObj** objs=realloc(d->obj,sizeof *objs*cap);
      if(!objs){ pm_unlock(&d->lk); return 0; }
      d->obj=objs;
      d->cap=cap;
    }
    id=d->n++;
  }
  d->obj[id]=o;
  pm_unlock(&d->lk);
  return (u32)id+1;
}

static SvcObj* domain_remove(Domain* d, u32 id, SvcObj* expected){
  SvcObj* obj=NULL;
  if(!d) return NULL;
  pm_lock(&d->lk);
  if(id && id<=(u32)d->n && (!expected || d->obj[id-1]==expected)){
    obj=d->obj[id-1]; d->obj[id-1]=NULL;
  }
  pm_unlock(&d->lk);
  return obj;
}

static u32 session_try_new_domain(SvcObj* obj, Domain* d){
  KSession* s=calloc(1,sizeof *s);
  if(!s) return 0;
  s->o.type=KO_SESSION; s->o.destroy=sess_destroy;
  s->o.name=obj && obj->name? obj->name : "session"; s->obj=obj;
  if(d){ pm_lock(&d->lk); d->refs++; pm_unlock(&d->lk); s->domain=d; }
  u32 handle=h_try_new(&s->o);
  if(!handle){ domain_unref(d); free(s); }
  return handle;
}

u32 session_new(SvcObj* obj){
  u32 handle=session_try_new_domain(obj,NULL);
  if(!handle) fatal("could not allocate a service session");
  return handle;
}

/* only warn the first few times per command, some games call them in a loop */
int ipc_warn_once(const char* name, u32 cmd){
  static struct { char name[64]; u32 cmd, count; } seen[512];
  static size_t count;
  pm_lock(&g_klock);
  for(size_t i=0;i<count;i++){
    if(seen[i].cmd==cmd && !strncmp(seen[i].name,name,sizeof seen[i].name-1)){
      int warn=seen[i].count<3;
      if(warn) seen[i].count++;
      pm_unlock(&g_klock);
      return warn;
    }
  }
  if(count<sizeof seen/sizeof *seen){
    snprintf(seen[count].name,sizeof seen[count].name,"%s",name);
    seen[count].cmd=cmd;
    seen[count++].count=1;
  }
  pm_unlock(&g_klock);
  return 1;
}

int stub_ok(SvcObj* o, IpcCtx* c){ c->legacy_stub=1; if(ipc_warn_once(o->name,c->cmd)) LOG("  [stub] %s cmd %u -> ok\n",o->name,c->cmd); return 0; }

/* ============================================================ registry */
typedef struct { char name[16]; SvcCtor ctor; } SvcReg;

static SvcReg regs[256]; static int nregs;

void svc_register(const char* n, SvcCtor c){ strncpy(regs[nregs].name,n,15); regs[nregs].ctor=c; nregs++; }

SvcObj* svc_open(const char* name){
  for(int i=0;i<nregs;i++) if(!strcmp(regs[i].name,name)){
    SvcObj* obj=regs[i].ctor();
    if(obj && !obj->ipc_family) obj->ipc_family=regs[i].name;
    return obj;
  }
  return NULL; }

/* ============================================================ buffers */
int buf_a(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->na) { *a=0; *s=0; return 0; } *a=c->a[i].addr; *s=c->a[i].size; return 1; }

int buf_b(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->nb) { *a=0; *s=0; return 0; } *a=c->b[i].addr; *s=c->b[i].size; return 1; }

int buf_xs(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->nx) { *a=0; *s=0; return 0; } *a=c->x[i].addr; *s=c->x[i].size; return 1; }

int buf_cs(IpcCtx* c, int i, u64* a, u64* s){ if(i>=c->nc) { *a=0; *s=0; return 0; } *a=c->c[i].addr; *s=c->c[i].size; return 1; }

int buf_in(IpcCtx* c, int i, u64* a, u64* s){
  if(i<c->na && c->a[i].addr && c->a[i].size) return buf_a(c,i,a,s);
  if(i<c->nx && c->x[i].addr) return buf_xs(c,i,a,s);
  if(i<c->na) return buf_a(c,i,a,s);
  *a=0; *s=0; return 0; }

int buf_out(IpcCtx* c, int i, u64* a, u64* s){
  if(i<c->nb && c->b[i].addr && c->b[i].size) return buf_b(c,i,a,s);
  if(i<c->nc && c->c[i].addr) return buf_cs(c,i,a,s);
  if(c->nc==1 && i>0 && c->c[0].addr){ *a=0; *s=0; return 0; }
  if(i<c->nb) return buf_b(c,i,a,s);
  *a=0; *s=0; return 0; }

void buf_x_out(IpcCtx* c, int i, u64 a, u64 s){ if(c->nx_out<MAX_BUFS){ c->x_out_addr[c->nx_out]=a; c->x_out_size[c->nx_out]=s; c->nx_out++; } (void)i; }

void out_copy_h(IpcCtx* c, u32 h){ c->out_copy[c->nout_copy++]=h; }

void out_move_h(IpcCtx* c, u32 h){ c->out_move[c->nout_move++]=h; }

void out_obj(IpcCtx* c, SvcObj* o){ c->out_objs[c->nout_objs++]=o; }

char g_ipc_ring[32][80]; u32 g_ipc_ring_n;

/* ============================================================ parsing */
static int parse(IpcCtx* c){
  u8* m=c->msg; u32 w0, w1; memcpy(&w0,m,4); memcpy(&w1,m+4,4);
  c->type=w0&0xFFFF;
  if(c->type==2 || c->type==15) return 1;
  u32 nx=(w0>>16)&15, na=(w0>>20)&15, nb=(w0>>24)&15, nw=(w0>>28)&15;
  u32 ndata=w1&0x3FF, cflags=(w1>>10)&15; int special=(w1>>31)&1;
  u8* p=m+8;
  const u8* end=m+c->msg_size;
  /* the header allows 15 of each kind but the arrays hold 8, so read past the rest and keep 8 */
  if(special){
    if(end-p<4) return 0;
    u32 sh; memcpy(&sh,p,4); p+=4;
    u32 ncp=(sh>>1)&15, nmv=(sh>>5)&15; c->has_pid=sh&1;
    if((u32)(end-p)<(ncp+nmv+(c->has_pid?2:0))*4) return 0;
    if(c->has_pid){ memcpy(&c->pid,p,8); p+=8; }
    for(u32 i=0;i<ncp;i++){ if(i<8) memcpy(&c->copy_h[i],p,4); p+=4; }
    for(u32 i=0;i<nmv;i++){ if(i<8) memcpy(&c->move_h[i],p,4); p+=4; }
    c->ncopy= ncp<8? (int)ncp : 8; c->nmove= nmv<8? (int)nmv : 8; }
  u32 nc= cflags<2? 0 : cflags==2? 1 : cflags-2;
  if((u32)(end-p)<nx*8+(na+nb+nw)*12+ndata*4+nc*8) return 0;
  c->nx= nx<MAX_BUFS? (int)nx : MAX_BUFS;
  for(u32 i=0;i<nx;i++){ u32 a,b; memcpy(&a,p,4); memcpy(&b,p+4,4); p+=8; if(i>=MAX_BUFS) continue;
    c->x[i].addr=b|((u64)((a>>12)&15)<<32)|((u64)((a>>6)&7)<<36); c->x[i].size=a>>16; c->x_counters[i]=(a&0x3F)|((a>>9)&7)<<6; }
  IpcBuf* lists[3]={c->a,c->b,c->w}; u32 cnt[3]={na,nb,nw};
  for(int k=0;k<3;k++) for(u32 i=0;i<cnt[k];i++){ u32 s,a,f; memcpy(&s,p,4); memcpy(&a,p+4,4); memcpy(&f,p+8,4); p+=12; if(i>=MAX_BUFS) continue;
    lists[k][i].addr=a|((u64)((f>>28)&15)<<32)|((u64)((f>>2)&7)<<36); lists[k][i].size=s|((u64)((f>>24)&15)<<32); lists[k][i].flags=f&3; }
  c->na= na<MAX_BUFS? (int)na : MAX_BUFS; c->nb= nb<MAX_BUFS? (int)nb : MAX_BUFS; c->nw= nw<MAX_BUFS? (int)nw : MAX_BUFS;
  /* raw data is 16-aligned from the message start */
  u8* raw=m+(((p-m)+15)&~15);
  u8* raw_end=p+ndata*4;
  /* C descriptors follow the data. */
  u8* cp=raw_end;
  c->nc=nc<MAX_BUFS? (int)nc : MAX_BUFS;
  for(int i=0;i<c->nc;i++){
    u32 a,b; memcpy(&a,cp,4); memcpy(&b,cp+4,4); cp+=8;
    c->c[i].addr=a|((u64)(b&0xFFFF)<<32); c->c[i].size=b>>16;
  }
  if(c->type>=16){
    c->tipc=1; c->cmd=c->type-16; c->raw=p; c->raw_size=0x100;
    c->request_size=(u32)(raw_end-p);
    return 1;
  }
  u8* d=raw;
  if(c->sess && __atomic_load_n(&c->sess->domain,__ATOMIC_ACQUIRE) && (c->type==4 || c->type==6)){
    if(raw_end-d<16) return 0;
    u8 dcmd=d[0], nobj=d[1];
    u16 dlen; memcpy(&dlen,d+2,2); memcpy(&c->domain_obj,d+4,4);
    if(dcmd==2){ c->is_domain=3; return 1; }
    if(dcmd!=1 || dlen<16 || raw_end-d-16<dlen+nobj*4) return 0;
    c->is_domain=2;
    c->nin_objs=nobj>8? 8 : nobj;
    for(int i=0;i<c->nin_objs;i++) memcpy(&c->in_objs[i],d+16+dlen+i*4,4);
    d+=16;
    raw_end=d+dlen;
  }
  if(raw_end-d<16) return 0;
  memcpy(&c->cmd,d+8,4); memcpy(&c->token,d+12,4);
  c->raw=d+16; c->raw_size=0x100;
  c->request_size=(u32)(raw_end-c->raw);
  return 1;
}

/* ============================================================ reply */
static void respond(IpcCtx* c, const u32* object_ids){
  u8* m=c->msg; memset(m,0,0x100);
  int dom= c->is_domain!=0 && !c->tipc;
  u32 nhandles=c->nout_copy+c->nout_move;
  int object_count=c->nout_objs;
  if(!dom) nhandles+=object_count;           /* out objects become new session handles */
  u32 w0=0; w0|=(u32)(c->nx_out&15)<<16;
  u8* p=m+8;
  u32 special= (c->nout_copy||c->nout_move||(!dom && object_count))? 1 : 0;
  u32 hdr_words=2+(special? 1+nhandles : 0)+c->nx_out*2;
  u32 pad_bytes=((hdr_words*4+15)&~15)-hdr_words*4;
  /* SFCO(16) + payload; domains add a 16 byte header and object ids */
  u32 payload=(c->out_size+3)&~3u;
  u32 data_bytes=16+payload+(dom? 16+object_count*4 : 0);
  u32 ndata=(pad_bytes+data_bytes+3)/4+ (16/4);   /* same slack the real kernel leaves */
  memcpy(m,&w0,4);
  u32 w1=ndata|(special? 0x80000000u : 0); memcpy(m+4,&w1,4);
  if(special){
    u32 nmove=c->nout_move+(dom? 0 : object_count);
    u32 sh=(c->nout_copy<<1)|(nmove<<5); memcpy(p,&sh,4); p+=4;
    for(int i=0;i<c->nout_copy;i++){ memcpy(p,&c->out_copy[i],4); p+=4; }
    for(int i=0;i<c->nout_move;i++){ memcpy(p,&c->out_move[i],4); p+=4; }
    if(!dom) for(int i=0;i<object_count;i++){ memcpy(p,&object_ids[i],4); p+=4; }
  }
  for(int i=0;i<c->nx_out;i++){ u64 a=c->x_out_addr[i], s=c->x_out_size[i];
    u32 x0=(u32)(i&0x3F)|(u32)(((a>>36)&7)<<6)|(u32)(((i>>6)&7)<<9)|(u32)(((a>>32)&15)<<12)|(u32)(s<<16), x1=(u32)a;
    memcpy(p,&x0,4); memcpy(p+4,&x1,4); p+=8; }
  if(c->tipc){ /* TIPC reply: result + payload after the header */
    w1=(1+payload/4)|(special? 0x80000000u : 0); memcpy(m+4,&w1,4);
    memcpy(p,&c->rc,4); memcpy(p+4,c->out,payload);
    c->reply_size=(u32)(p-m)+4+payload; return; }
  u8* d=m+(((p-m)+15)&~15);
  if(dom){
    u32 n=(u32)object_count; memcpy(d,&n,4); d+=16;
  }
  u32 magic=0x4F434653; memcpy(d,&magic,4); u32 ver=0; memcpy(d+4,&ver,4); memcpy(d+8,&c->rc,4); memcpy(d+12,&c->token,4);
  memcpy(d+16,c->out,payload);
  if(dom) memcpy(d+16+payload,object_ids,(size_t)object_count*4);
  c->reply_size=(u32)(d-m)+16+payload+(dom? (u32)object_count*4 : 0);
}

static SvcObj* domain_get(KSession* s, u32 id){ Domain* d=__atomic_load_n(&s->domain,__ATOMIC_ACQUIRE); if(!d) return NULL; pm_lock(&d->lk);
  SvcObj* o= (id && id<=(u32)d->n)? d->obj[id-1] : NULL; if(o) __atomic_add_fetch(&o->refs,1,__ATOMIC_SEQ_CST); pm_unlock(&d->lk); return o; }

/* object passed IN to a command (IStorage pushed to an applet...): domain id or a moved session handle. it comes
   back referenced, the caller unrefs it */
SvcObj* ipc_in_obj(IpcCtx* c, int i){
  if(c->is_domain) return (i<c->nin_objs && c->sess)? domain_get(c->sess,c->in_objs[i]) : NULL;
  if(i>=c->nmove) return NULL;
  KSession* s=(KSession*)h_ref_type(c->move_h[i],KO_SESSION);
  if(!s) return NULL;
  SvcObj* obj=s->obj;
  if(obj) __atomic_add_fetch(&obj->refs,1,__ATOMIC_SEQ_CST);
  kobj_unref(&s->o);
  return obj; }

static void discard_reply(IpcCtx* c, Domain* domain, const u32* ids){
  for(int i=0;i<c->nout_objs;i++){
    SvcObj* obj=c->out_objs[i];
    if(!ids[i]) svcobj_unref(obj);
    else if(domain){
      svcobj_unref(domain_remove(domain,ids[i],obj));
      svcobj_unref(obj);   /* publication reference */
    } else h_close(ids[i]);
  }
  for(int i=0;i<c->nout_copy;i++) h_close(c->out_copy[i]);
  for(int i=0;i<c->nout_move;i++) h_close(c->out_move[i]);
  c->nout_objs=c->nout_copy=c->nout_move=c->nx_out=0;
  c->out_size=0;
}

static void ipc_reply(IpcCtx* c){
  Domain* domain=c->is_domain && !c->tipc? __atomic_load_n(&c->sess->domain,__ATOMIC_ACQUIRE) : NULL;
  u32 ids[8]={0};
  for(int i=0;i<c->nout_objs;i++){
    SvcObj* obj=c->out_objs[i];
    if(domain){
      __atomic_add_fetch(&obj->refs,1,__ATOMIC_SEQ_CST);
      ids[i]=domain_add(domain,obj);
      if(!ids[i]) svcobj_unref(obj);
    } else ids[i]=session_try_new_domain(obj,NULL);
    if(!ids[i]){
      c->rc=obj->nha_kind==NHA_NATIVE_FILE? RC(2,6709) : RC(1,105);
      discard_reply(c,domain,ids);
      break;
    }
  }
  respond(c,ids);
  u32 n=c->reply_size>0x100? c->reply_size : c->msg_size<0x100? c->msg_size : 0x100;
  if(c->reply_size<=c->msg_size && mem_access_begin(c->msg_addr,n,PERM_W)){
    memcpy(GP(c->msg_addr),c->msg,n);
    mem_access_end();
    if(domain) for(int i=0;i<c->nout_objs;i++) svcobj_unref(c->out_objs[i]);
    return;
  }
  discard_reply(c,domain,ids);
  c->th->cpu.x[0]=0xCC01;
}

static void ipc_request(KThread* t, KSession* s, u64 msg_addr, u64 msg_size){
  CPU* cpu=&t->cpu;
  /* The HIPC header cannot describe more than this many request bytes.
     Allow an extra zeroed window for legacy handlers that read optional fields. */
  u8 message[0x1400+0x100];
  u32 length=msg_size>0x1400? 0x1400 : (u32)msg_size;
  if(length<8 || !mem_access_begin(msg_addr,length,PERM_RW)){ cpu->x[0]=0xCC01; return; }
  memcpy(message,GP(msg_addr),length);
  mem_access_end();
  memset(message+length,0,0x100);
  IpcCtx* c=calloc(1,sizeof *c);
  if(!c){ cpu->x[0]=0xD001; return; }
  c->th=t; c->msg=message; c->msg_addr=msg_addr; c->msg_size=length; c->sess=s;
  if(!parse(c)){ cpu->x[0]=0xEE01; free(c); return; }
  cpu->x[0]=0;
  if(c->type==2 || c->type==15){ /* client closes the handle itself afterwards */ free(c); return; }
  if(c->type==5 || c->type==7){ /* control */
    switch(c->cmd){
      case 0: {
        pm_lock(&g_klock);
        if(!__atomic_load_n(&s->domain,__ATOMIC_RELAXED)){
          Domain* d=calloc(1,sizeof *d);
          if(!d) c->rc=0xD001;
          else {
            pm_init(&d->lk); d->refs=1;
            if(domain_add(d,s->obj)){
              __atomic_add_fetch(&s->obj->refs,1,__ATOMIC_SEQ_CST);
              __atomic_store_n(&s->domain,d,__ATOMIC_RELEASE);
            } else { domain_unref(d); c->rc=0xD001; }
          }
        }
        pm_unlock(&g_klock);
        if(!c->rc) out32(c,1); break;
      }
      case 2: case 4: {
        Domain* d=__atomic_load_n(&s->domain,__ATOMIC_ACQUIRE);
        __atomic_add_fetch(&s->obj->refs,1,__ATOMIC_SEQ_CST);
        u32 handle=session_try_new_domain(s->obj,d);
        if(handle) out_move_h(c,handle);
        else { svcobj_unref(s->obj); c->rc=RC(1,105); }
        break;
      }
      case 3: out32(c,0x8000); break;
      default: WARN("control %u\n",c->cmd); break;
    }
    c->is_domain=0; ipc_reply(c); free(c); return;
  }
  SvcObj* obj=s->obj;
  if(c->is_domain){
    if(c->is_domain==3){ /* close a domain object */
      Domain* d=__atomic_load_n(&s->domain,__ATOMIC_ACQUIRE);
      svcobj_unref(domain_remove(d,c->domain_obj,NULL));
      c->is_domain=1; ipc_reply(c); free(c); return; }
    obj=domain_get(s,c->domain_obj);
    if(!obj){ WARN("invalid domain object %u\n",c->domain_obj); c->rc=0x1D60A; ipc_reply(c); free(c); return; }
  }
  LOGV("IPC %s cmd %u\n",obj->name,c->cmd);
  NhaCall call;
  nha_dispatch_ipc(obj,c,&call);
  { u32 i=__atomic_fetch_add(&g_ipc_ring_n,1,__ATOMIC_RELAXED)&31; u32 in0=0,in1=0; memcpy(&in0,c->raw,4); memcpy(&in1,c->raw+4,4); snprintf(g_ipc_ring[i],80,"%s cmd %u (in %x %x) -> %x",obj->name,c->cmd,in0,in1,c->rc); }
  ipc_reply(c);
  nha_end_ipc(&call,c,cpu->x[0]);
  if(c->is_domain) svcobj_unref(obj);           /* drop the ref from domain_get */
  free(c);
}

void ipc_handle(KThread* t, u32 handle, u64 msg_addr, u64 msg_size){
  KSession* s=(KSession*)h_ref_type(handle,KO_SESSION);
  if(!s){
    WARN("SendSyncRequest to invalid handle %x (pc=%llx)\n",handle,(unsigned long long)t->cpu.pc);
    t->cpu.x[0]=0xE401;
    return;
  }
  ipc_request(t,s,msg_addr,msg_size);
  kobj_unref(&s->o);
}
