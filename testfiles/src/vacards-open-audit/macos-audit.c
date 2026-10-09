/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <spawn.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <mach-o/dyld.h>
#include "audit.h"

extern char **environ;
struct guarded_fd;
extern void *__mmap(void *,size_t,int,int,int,off_t);
extern int __open(const char *,int,...);
extern int __openat(int,const char *,int,...);
extern int __fcntl(int,int,...);
extern int __execve(const char *,char *const[],char *const[]);
extern pid_t __fork(void);
extern int __connect(int,const struct sockaddr *,socklen_t);
extern int __posix_spawn(pid_t *,const char *,const posix_spawn_file_actions_t *,const posix_spawnattr_t *,char *const[],char *const[]);
extern int audit_symbol_open_nocancel(const char *,int,...) __asm__("_open$NOCANCEL") __attribute__((weak_import));
extern int audit_symbol_openat_nocancel(int,const char *,int,...) __asm__("_openat$NOCANCEL") __attribute__((weak_import));
extern int audit_symbol_open_darwin_extsn(const char *,int,...) __asm__("_open$DARWIN_EXTSN") __attribute__((weak_import));
extern int audit_symbol_openat_darwin_extsn(int,const char *,int,...) __asm__("_openat$DARWIN_EXTSN") __attribute__((weak_import));
extern FILE *audit_symbol_fopen_darwin_extsn(const char *,const char *) __asm__("_fopen$DARWIN_EXTSN") __attribute__((weak_import));
extern FILE *audit_symbol_fopen_nocancel(const char *,const char *) __asm__("_fopen$NOCANCEL") __attribute__((weak_import));
extern DIR *audit_symbol_opendir_inode64(const char *) __asm__("_opendir$INODE64") __attribute__((weak_import));
extern int audit_symbol_close_nocancel(int) __asm__("_close$NOCANCEL") __attribute__((weak_import));
extern int audit_symbol_guarded_open(const char *,int,const struct guarded_fd *,uint32_t,...) __asm__("_guarded_open_np") __attribute__((weak_import));
typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef int (*guarded_open_fn)(const char *, int, const struct guarded_fd *, uint32_t, ...);
typedef FILE *(*fopen_fn)(const char *, const char *);
typedef FILE *(*freopen_fn)(const char *, const char *, FILE *);
typedef DIR *(*opendir_fn)(const char *);
typedef int (*fcntl_fn)(int, int, ...);
typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
typedef int (*close_fn)(int);
typedef void *(*dlopen_fn)(const char *, int);
typedef int (*posix_spawn_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]);
typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef pid_t (*fork_fn)(void);
typedef int (*connect_fn)(int, const struct sockaddr *, socklen_t);

static open_fn real_open; static openat_fn real_openat; static guarded_open_fn real_guarded_open;
static fopen_fn real_fopen; static freopen_fn real_freopen; static opendir_fn real_opendir;
static fcntl_fn real_fcntl; static mmap_fn real_mmap;
static dlopen_fn real_dlopen; static posix_spawn_fn real_posix_spawn; static execve_fn real_execve;
static fork_fn real_fork; static connect_fn real_connect;
static int log_fd = -1, active = 0, hook_ready = 0;
static char case_id[256];
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int in_hook;
static volatile int init_finished;
static volatile unsigned long event_count;
static void *audit_dlopen(const char *,int);
static dlopen_fn lookup_real_dlopen(void) {
    NSSymbol symbol=NSLookupAndBindSymbol("_dlopen");
    dlopen_fn fn=symbol?(dlopen_fn)NSAddressOfSymbol(symbol):NULL;
    return fn==(dlopen_fn)audit_dlopen?NULL:fn;
}

static void resolve_symbols(void) {
    real_open=__open; real_openat=__openat; real_fcntl=__fcntl; real_mmap=__mmap;
    real_guarded_open=(guarded_open_fn)audit_symbol_guarded_open;
    real_posix_spawn=__posix_spawn; real_execve=__execve;
    real_fork=__fork; real_connect=__connect;
    real_dlopen=lookup_real_dlopen();
}
static void escape_field(const char *src, char *dst, size_t cap) {
    static const char h[]="0123456789ABCDEF"; size_t n=0;
    if (!src) src="";
    while (*src && n+4<cap) { unsigned char c=(unsigned char)*src++;
        if (c=='%'||c=='\t'||c=='\r'||c=='\n'||c==' ') { dst[n++]='%'; dst[n++]=h[c>>4]; dst[n++]=h[c&15]; }
        else dst[n++]=(char)c;
    }
    dst[n]=0;
}
static void caller_image(void *ret, char *out, size_t cap) {
    Dl_info info; const char *name="<unknown>";
    if (dladdr(ret,&info) && info.dli_fname) name=info.dli_fname;
    snprintf(out,cap,"%s",name);
}
static void record_event(const char *kind,const char *api,const char *path,long access,long result,void *ret) {
    if (!active || log_fd<0 || in_hook) return;
    in_hook=1;
    char ec[512],ep[PATH_MAX*3],em[PATH_MAX*3],caller[PATH_MAX];
    escape_field(case_id,ec,sizeof(ec)); escape_field(path?path:"",ep,sizeof(ep));
    caller_image(ret,caller,sizeof(caller)); escape_field(caller,em,sizeof(em));
    char line[PATH_MAX*7]; int n=snprintf(line,sizeof(line),"OPEN\t%s\t%s\t%s\t%s\t0x%lx\t%ld\t%s\n",ec,kind,api,ep,(unsigned long)access,result,em);
    if(n>0) { pthread_mutex_lock(&log_mutex); (void)write(log_fd,line,(size_t)n<sizeof(line)?(size_t)n:sizeof(line)-1); pthread_mutex_unlock(&log_mutex); __sync_fetch_and_add(&event_count,1); }
    in_hook=0;
}
static void audit_init(void) __attribute__((constructor));
static void audit_init(void) {
    const char *file=getenv("VACARDS_AUDIT_EVENT_FILE"), *id=getenv("VACARDS_AUDIT_CASE");
    if (!file || !id) return;
    snprintf(case_id,sizeof(case_id),"%s",id); in_hook=1;
    log_fd=(int)syscall(SYS_open,file,O_WRONLY|O_CREAT|O_TRUNC,0600);
    if(log_fd<0)return;
    active=1;
    char line[128]; int n=snprintf(line,sizeof(line),"START\tp9-parser-opens/2\tmacos-dyld-interpose\n"); (void)write(log_fd,line,(size_t)n);
    in_hook=1; resolve_symbols(); in_hook=0;
    hook_ready=real_open&&real_openat&&real_fcntl&&real_mmap&&real_dlopen&&real_posix_spawn&&real_execve&&real_fork&&real_connect;
    init_finished=1;
    in_hook=0;
}
int VacardsOpenAuditStart(const char *file,const char *id) {
    (void)file; (void)id; return active && log_fd>=0;
}
int VacardsOpenAuditCheckCoverage(void) { return hook_ready; }
void VacardsOpenAuditStop(void) {
    if(!active||log_fd<0)return; in_hook=1; char line[128]; int n=snprintf(line,sizeof(line),"END\t%lu\t0\n",event_count); (void)write(log_fd,line,(size_t)n); (void)fsync(log_fd); (void)syscall(SYS_close,log_fd); log_fd=-1; active=0; in_hook=0;
}

#define RETADDR() __builtin_return_address(0)
static int call_open(const char *p,int f,mode_t m,int has_mode,const char *api,open_fn fn,void *caller) {
    if(in_hook||!fn)return (int)syscall(SYS_open,p,f,m); int v=has_mode?fn(p,f,m):fn(p,f); int e=errno; record_event("open",api,p,f,v<0?-e:0,caller); errno=e; return v;
}
static int audit_open(const char *p,int f,...) { mode_t m=0; int has=(f&O_CREAT)!=0; if(has){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);} return call_open(p,f,m,has,"open",real_open,RETADDR()); }
static int audit_open_nocancel(const char *p,int f,...) { mode_t m=0; int has=(f&O_CREAT)!=0; if(has){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);} return call_open(p,f,m,has,"open$NOCANCEL",real_open,RETADDR()); }
static int audit_open_darwin_extsn(const char *p,int f,...) { mode_t m=0; int has=(f&O_CREAT)!=0; if(has){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);} return call_open(p,f,m,has,"open$DARWIN_EXTSN",real_open,RETADDR()); }
static int audit_openat_common(int d,const char *p,int f,mode_t m,int has,const char *api,void *caller) {
    if(in_hook||!real_openat)return (int)syscall(SYS_openat,d,p,f,m);
    int v=has?real_openat(d,p,f,m):real_openat(d,p,f); int e=errno;
    record_event("open",api,p,f,v<0?-e:0,caller); errno=e; return v;
}
static int audit_openat(int d,const char *p,int f,...) { mode_t m=0;int has=(f&O_CREAT)!=0;if(has){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);}return audit_openat_common(d,p,f,m,has,"openat",RETADDR()); }
static int audit_openat_nocancel(int d,const char *p,int f,...) { mode_t m=0;int has=(f&O_CREAT)!=0;if(has){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);}return audit_openat_common(d,p,f,m,has,"openat$NOCANCEL",RETADDR()); }
static int audit_openat_darwin_extsn(int d,const char *p,int f,...) { mode_t m=0;int has=(f&O_CREAT)!=0;if(has){va_list a;va_start(a,f);m=va_arg(a,int);va_end(a);}return audit_openat_common(d,p,f,m,has,"openat$DARWIN_EXTSN",RETADDR()); }
static int audit_guarded_open(const char *p,int f,const struct guarded_fd *g,uint32_t v,...) { mode_t m=0;int has=(f&O_CREAT)!=0;if(has){va_list a;va_start(a,v);m=va_arg(a,int);va_end(a);}if(in_hook)return has?real_guarded_open(p,f,g,v,m):real_guarded_open(p,f,g,v);int fd=has?real_guarded_open(p,f,g,v,m):real_guarded_open(p,f,g,v);int e=errno;record_event("open","guarded_open_np",p,f,fd<0?-e:0,RETADDR());errno=e;return fd; }
static FILE *audit_fopen_common(const char *p,const char *m,const char *api,void *caller) {
    if(in_hook||!p||!m)return NULL; int flags=O_RDONLY; if(m[0]=='w')flags=O_WRONLY|O_CREAT|O_TRUNC;else if(m[0]=='a')flags=O_WRONLY|O_CREAT|O_APPEND;else if(m[0]!='r'){errno=EINVAL;return NULL;}
    if(strchr(m,'+'))flags=(flags&~(O_RDONLY|O_WRONLY))|O_RDWR; int fd=(flags&O_CREAT)?real_open(p,flags,0666):real_open(p,flags);
    FILE *f=fd<0?NULL:fdopen(fd,m);if(!f&&fd>=0)(void)syscall(SYS_close,fd);int e=errno;record_event("open",api,p,flags,f?0:-e,caller);errno=e;return f;
}
static FILE *audit_fopen(const char *p,const char *m) { return audit_fopen_common(p,m,"fopen",RETADDR()); }
static FILE *audit_fopen_darwin_extsn(const char *p,const char *m) { return audit_fopen_common(p,m,"fopen$DARWIN_EXTSN",RETADDR()); }
static FILE *audit_fopen_nocancel(const char *p,const char *m) { return audit_fopen_common(p,m,"fopen$NOCANCEL",RETADDR()); }
static FILE *audit_freopen(const char *p,const char *m,FILE *s) {if(s)fclose(s);return audit_fopen_common(p,m,"freopen",RETADDR());}
static DIR *audit_opendir_common(const char *p,const char *api,void *caller) {if(in_hook||!p)return NULL;int fd=real_open(p,O_RDONLY|O_DIRECTORY);DIR *d=fd<0?NULL:fdopendir(fd);if(!d&&fd>=0)(void)syscall(SYS_close,fd);int e=errno;record_event("open",api,p,O_RDONLY|O_DIRECTORY,d?0:-e,caller);errno=e;return d;}
static DIR *audit_opendir(const char *p) { return audit_opendir_common(p,"opendir",RETADDR()); }
static DIR *audit_opendir_inode64(const char *p) { return audit_opendir_common(p,"opendir$INODE64",RETADDR()); }
static int audit_fcntl(int fd,int cmd,...) {
    if(cmd==F_GETFD||cmd==F_GETFL||cmd==F_GETOWN)return real_fcntl?real_fcntl(fd,cmd):(int)syscall(SYS_fcntl,fd,cmd,0);
    va_list a; va_start(a,cmd); void *arg=va_arg(a,void *); va_end(a); if(!real_fcntl)return (int)syscall(SYS_fcntl,fd,cmd,arg); return real_fcntl(fd,cmd,arg);
}
static void *audit_mmap(void *a,size_t n,int p,int f,int fd,off_t o) {void *v=__mmap(a,n,p,f,fd,o);int e=errno;errno=e;return v;}
static int audit_close(int fd) {return (int)syscall(SYS_close,fd);}
static int audit_close_nocancel(int fd) {return audit_close(fd);}
static void *audit_dlopen(const char *p,int f) {if(in_hook||!real_dlopen)return NULL;void *v=real_dlopen(p,f);int e=errno;record_event("load","dlopen",p,f,v?0:-e,RETADDR());errno=e;return v;}
static int audit_posix_spawn(pid_t *pid,const char *p,const posix_spawn_file_actions_t *fa,const posix_spawnattr_t *at,char *const av[],char *const ev[]) {if(in_hook||!real_posix_spawn)return ENOSYS;int v=real_posix_spawn(pid,p,fa,at,av,ev);record_event("spawn","posix_spawn",p,0,v?-(long)v:0,RETADDR());return v;}
static int audit_execve(const char *p,char *const av[],char *const ev[]) {if(in_hook||!real_execve){errno=ENOSYS;return -1;}record_event("exec","execve",p,0,0,RETADDR());return real_execve(p,av,ev);}
static pid_t audit_fork(void) {if(in_hook||!real_fork){errno=ENOSYS;return -1;}record_event("fork","fork","",0,0,RETADDR());return real_fork();}
static int audit_connect(int fd,const struct sockaddr *a,socklen_t n) {
    if(in_hook||!real_connect){errno=ENOSYS;return -1;}
    int result=real_connect(fd,a,n), saved_errno=errno;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)+1]="";
    int family=a?a->sa_family:0;
    if(a && family==AF_UNIX && n>offsetof(struct sockaddr_un,sun_path)) {
        size_t available=(size_t)n - offsetof(struct sockaddr_un,sun_path);
        if(available>sizeof(path)-1) available=sizeof(path)-1;
        size_t length=strnlen(((const struct sockaddr_un *)a)->sun_path,available);
        memcpy(path,((const struct sockaddr_un *)a)->sun_path,length); path[length]=0;
    }
    record_event("network","connect",path,family,result<0?-saved_errno:0,RETADDR());
    errno=saved_errno; return result;
}

#define INTERPOSE(replacement, replacee) { (const void *)(replacement), (const void *)(replacee) }
__attribute__((used,section("__DATA,__interpose"))) static const struct {const void *replacement,*replacee;} interposers[] = {
    INTERPOSE(audit_open,open), INTERPOSE(audit_open_nocancel,audit_symbol_open_nocancel), INTERPOSE(audit_open_darwin_extsn,audit_symbol_open_darwin_extsn),
    INTERPOSE(audit_openat,openat), INTERPOSE(audit_openat_nocancel,audit_symbol_openat_nocancel), INTERPOSE(audit_openat_darwin_extsn,audit_symbol_openat_darwin_extsn),
    INTERPOSE(audit_guarded_open,audit_symbol_guarded_open),
    INTERPOSE(audit_fopen,fopen), INTERPOSE(audit_fopen_darwin_extsn,audit_symbol_fopen_darwin_extsn), INTERPOSE(audit_fopen_nocancel,audit_symbol_fopen_nocancel),
    INTERPOSE(audit_freopen,freopen), INTERPOSE(audit_opendir,opendir), INTERPOSE(audit_opendir_inode64,audit_symbol_opendir_inode64),
    INTERPOSE(audit_fcntl,fcntl), INTERPOSE(audit_mmap,mmap), INTERPOSE(audit_close,close), INTERPOSE(audit_close_nocancel,audit_symbol_close_nocancel), INTERPOSE(audit_dlopen,dlopen),
    INTERPOSE(audit_posix_spawn,posix_spawn), INTERPOSE(audit_execve,execve), INTERPOSE(audit_fork,fork), INTERPOSE(audit_connect,connect)
};
