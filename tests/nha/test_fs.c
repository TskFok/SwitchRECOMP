/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "rt.h"
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#include <winioctl.h>
#endif

static PFile file;
static PMutex lock;
static PCond done;
static int remaining;
static unsigned rename_cycles, safe_reads;
static const u64 sparse_offset=0x100000003ULL;

static void reader(void* arg){
  u64 offset=*(const u64*)arg;
  for(int i=0;i<100;i++){
    PError error={0};
    char buf[2];
    u64 n=0;
    assert(!plat_file_read_at(&file,buf,sizeof buf,offset,&n,&error));
    if(offset) assert(n==1 && buf[0]=='z');
    else assert(n==2 && buf[0]=='a' && buf[1]==0);
  }
  pm_lock(&lock);
  remaining--;
  pc_signal(&done);
  pm_unlock(&lock);
}

static void put(const char* path, int ch){
  FILE* f=fopen(path,"wb");
  assert(f && fputc(ch,f)==ch && !fclose(f));
}

static void make_sparse(const char* path){
  FILE* f=fopen(path,"wb");
  assert(f);
#if defined(_WIN32)
  DWORD transferred;
  assert(DeviceIoControl((HANDLE)_get_osfhandle(_fileno(f)),FSCTL_SET_SPARSE,NULL,0,NULL,0,&transferred,NULL));
  assert(!_fseeki64(f,(i64)sparse_offset,SEEK_SET));
#else
  assert(!fseeko(f,(off_t)sparse_offset,SEEK_SET));
#endif
  assert(fputc('z',f)=='z' && !fclose(f));
}

static void reject(const char* root, const char* name){
  PFile bad;
  PError error={0};
  assert(plat_file_open_read(&bad,root,name,&error)!=PFILE_OK);
}

static void rename_directory(void* unused){
  (void)unused;
  for(int i=0;i<300;i++){
#if defined(_WIN32)
    if(!MoveFileA("root/live","root/parked")) continue;
    if(MoveFileA("root/jump","root/live")){
      assert(MoveFileA("root/live","root/jump"));
      rename_cycles++;
    }
    assert(MoveFileA("root/parked","root/live"));
#else
    assert(!rename("root/live","root/parked"));
    assert(!rename("root/jump","root/live"));
    assert(!rename("root/live","root/jump"));
    assert(!rename("root/parked","root/live"));
    rename_cycles++;
#endif
  }
  pm_lock(&lock);
  remaining--;
  pc_signal(&done);
  pm_unlock(&lock);
}

static void check_rename_race(void){
  assert(!plat_mkdir("root/live"));
  put("root/live/a",'a');
  remaining=1;
  assert(!plat_thread(rename_directory,NULL,1<<20));
  for(int i=0;i<600;i++){
    PError error={0};
    PFile opened;
    if(!plat_file_open_read(&opened,"root","live/a",&error)){
      char byte;
      u64 n;
      assert(!plat_file_read_at(&opened,&byte,1,0,&n,&error) && n==1 && byte=='a');
      assert(!plat_file_close(&opened,&error));
      safe_reads++;
    }
  }
  pm_lock(&lock);
  while(remaining) pc_wait(&done,&lock,-1);
  pm_unlock(&lock);
  assert(rename_cycles);
}

int main(int argc, char** argv){
  assert(argc==2);
  assert(!chdir(argv[1]));
  assert(!plat_mkdir("root"));
  assert(!plat_mkdir("root/sub"));
  assert(!plat_mkdir("outside"));
  put("root/a",'a');
  put("outside/a",'x');
  make_sparse("root/sub/sparse");

  PError error={0};
  int permission_checked=0;
  u64 n=0;
  char byte;
  assert(!plat_file_open_read(&file,"root","sub/sparse",&error));
  assert(!plat_file_size64(&file,&n,&error) && n==sparse_offset+1);
  assert(!plat_file_read_at(&file,&byte,1,sparse_offset,&n,&error) && n==1 && byte=='z');
  assert(!plat_file_read_at(&file,&byte,1,sparse_offset+1,&n,&error) && n==0);
  assert(!plat_file_close(&file,&error));
  make_sparse("root/sparse");
  /* Read both ends of the same synchronous handle concurrently. */
  FILE* f=fopen("root/sparse","rb+");
  assert(f && fputc('a',f)=='a' && !fclose(f));
  assert(!plat_file_open_read(&file,"root","sparse",&error));
  PFile shared;
  assert(!plat_file_open_read(&shared,"root","sparse",&error));
  assert(!plat_file_read_at(&shared,&byte,1,sparse_offset,&n,&error) && n==1 && byte=='z');
  assert(!plat_file_close(&shared,&error));
  pm_init(&lock); pc_init(&done);
  u64 offsets[]={sparse_offset,0};
  remaining=2;
  assert(!plat_thread(reader,&offsets[0],1<<20));
  assert(!plat_thread(reader,&offsets[1],1<<20));
  pm_lock(&lock);
  while(remaining) pc_wait(&done,&lock,-1);
  pm_unlock(&lock);
  assert(!plat_file_close(&file,&error));

  reject("root","../outside/a");
  reject("root","sub/../a");
  reject("root","sub");
  reject("root","missing");
  assert(plat_file_read_at(&file,&byte,1,(u64)-1,&n,&error)==PFILE_INVALID);
  assert(plat_file_read_at(&file,&byte,2,INT64_MAX,&n,&error)==PFILE_INVALID);
#if defined(_WIN32)
  /* Directory junctions need no symlink privilege on a local NTFS volume. */
  char outside[4096], junction[4200];
  DWORD len=GetFullPathNameA("outside",sizeof outside,outside,NULL);
  assert(len>0 && len<sizeof outside);
  snprintf(junction,sizeof junction,"cmd /d /c mklink /J root\\jump \"%s\" >nul",outside);
  assert(!system(junction));
  FILE* through=fopen("root/jump/a","rb");
  assert(through && fgetc(through)=='x' && !fclose(through));
  reject("root","jump/a");
  check_rename_race();
  assert(RemoveDirectoryA("root\\jump"));
  HANDLE denied=CreateFileA("root/a",GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
  assert(denied!=INVALID_HANDLE_VALUE);
  assert(plat_file_open_read(&file,"root","a",&error)==PFILE_DENIED);
  assert(error.domain==PERR_WIN32 && error.code==ERROR_SHARING_VIOLATION);
  assert(CloseHandle(denied));
  permission_checked=1;
#else
  assert(!symlink("../outside","root/jump"));
  reject("root","jump/a");
  assert(!symlink("../outside/a","root/link"));
  reject("root","link");
  check_rename_race();
  assert(!chmod("root/a",0000));
  if(geteuid()!=0){
    assert(plat_file_open_read(&file,"root","a",&error)==PFILE_DENIED);
    assert(error.domain==PERR_ERRNO && error.code==EACCES);
    permission_checked=1;
  }
  assert(!chmod("root/a",0600));
#endif
  fprintf(stderr,"NHA_TEST {\"permission_checked\":%d,\"rename_cycles\":%u,\"safe_race_reads\":%u}\n",
          permission_checked,rename_cycles,safe_reads);
  pc_destroy(&done);
  return 0;
}
