#include "camera_daemon.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

namespace {
static long long monotonic_ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000LL; }
static std::string json_escape(const std::string& s) { std::string r; for (size_t i=0;i<s.size();++i) { if(s[i]=='"'||s[i]=='\\') r+='\\'; if(s[i]=='\n') r+="\\n"; else r+=s[i]; } return r; }
static bool field(const std::string& s, const char* name, std::string* value) {
  std::string key = std::string("\"") + name + "\""; size_t p=s.find(key); if(p==std::string::npos) return false;
  p=s.find(':',p+key.size()); if(p==std::string::npos) return false; p=s.find_first_not_of(" \t\r\n",p+1); if(p==std::string::npos) return false;
  if(s[p]=='\"') { size_t e=s.find('\"',p+1); if(e==std::string::npos) return false; *value=s.substr(p+1,e-p-1); return true; }
  size_t e=s.find_first_of(",}\r\n",p); *value=s.substr(p,e-p); return true;
}
static bool bool_field(const std::string&s,const char*n,bool*o) { std::string v; if(!field(s,n,&v))return false; *o=(v=="true"||v=="1");return true; }
static bool number_field(const std::string&s,const char*n,double*o) { std::string v; if(!field(s,n,&v))return false; char*e=0; *o=strtod(v.c_str(),&e);return e&&e!=v.c_str(); }
static void mkdir_parent(const std::string& path) { size_t p=0; while((p=path.find('/',p+1))!=std::string::npos) { std::string d=path.substr(0,p); if(!d.empty()) mkdir(d.c_str(),0755); } }
}

struct CameraDaemon::Impl {
  DaemonConfig c; int server_fd; pid_t sender_pid, npu_pid, rkipc_pid; bool running, npu_started_once, manual_restart_pending;
  /* V4L2 control readback is the sensor driver's cached state.  Once RKAIQ
   * owns AE it is not a reliable source for the last manual request, so keep
   * the accepted setpoints here and use them for subsequent transactions. */
  int manual_exposure, manual_gain;
  int failures, dark_frames, bright_frames; long long restart_at, npu_start_at; std::string state, last_error, mode;
  Impl(const DaemonConfig& x):c(x),server_fd(-1),sender_pid(-1),npu_pid(-1),rkipc_pid(-1),running(true),npu_started_once(false),manual_restart_pending(false),manual_exposure(x.default_exposure),manual_gain(x.default_analogue_gain),failures(0),dark_frames(0),bright_frames(0),restart_at(0),npu_start_at(0),state("NORMAL"),mode("DISPLAY") {}
  void event(const char* type, const std::string& detail) {
    mkdir_parent(c.log_path); int fd=open(c.log_path.c_str(),O_WRONLY|O_CREAT|O_APPEND,0644); if(fd<0)return;
    std::ostringstream o; o << "{\"monotonic_ms\":" << monotonic_ms() << ",\"type\":\"" << type << "\",\"state\":\"" << state << "\",\"detail\":\"" << json_escape(detail) << "\"}\n";
    std::string line=o.str(); ssize_t ignored = write(fd,line.data(),line.size()); (void)ignored; close(fd);
  }
  bool spawn(const std::string& path, bool sender) {
    if(access(path.c_str(),X_OK)!=0) { last_error="executable unavailable: "+path; event("spawn_failed",last_error); return false; }
    pid_t p=fork(); if(p<0){last_error="fork failed";return false;} if(p==0) {
      setsid(); setenv("LD_LIBRARY_PATH", "/oem/usr/lib", 1); if(sender && path==c.bridge_path) execl(path.c_str(),path.c_str(),"--url",c.rtsp_url.c_str(),(char*)0);
      else if(sender) execl(path.c_str(),path.c_str(),"--no-vo","--preview-shm","/ai_cam_preview","--preview-width","384","--preview-height","216","--preview-fps","15","--isp-control-socket",c.isp_control_socket.c_str(),"-a",c.iq_dir.c_str(),"-o","/dev/null",(char*)0);
      else
        execl(path.c_str(),path.c_str(),"--no-backlight-control",(char*)0);
      _exit(127);
    }
    if(sender)sender_pid=p; else npu_pid=p; event(sender?"pipeline_started":"npu_started",path); return true;
  }
  const std::string& capture_path() const { return c.sender_path; }
  bool rkipc_ready() const {
    return access(c.rkipc_socket.c_str(), F_OK) == 0;
  }
  pid_t find_rkipc() const {
    DIR* dir=opendir("/proc"); if(!dir)return -1; struct dirent* entry; pid_t found=-1;
    while((entry=readdir(dir))!=0) { char* end=0; long value=strtol(entry->d_name,&end,10); if(!end||*end||value<1)continue;
      std::string base=std::string("/proc/")+entry->d_name;
      std::string comm_path=base+"/comm"; int comm_fd=open(comm_path.c_str(),O_RDONLY); char comm[32]={0}; ssize_t comm_size=comm_fd<0?-1:read(comm_fd,comm,sizeof(comm)-1); if(comm_fd>=0)close(comm_fd);
      std::string cmd_path=base+"/cmdline"; int fd=open(cmd_path.c_str(),O_RDONLY); char cmd[256]={0}; ssize_t n=fd<0?-1:read(fd,cmd,sizeof(cmd)-1); if(fd>=0)close(fd);
      if((comm_size>0&&strncmp(comm,"rkipc",5)==0)||(n>0&&strstr(cmd,c.rkipc_path.c_str()))) {found=(pid_t)value;break;}
    }
    closedir(dir); return found;
  }
  bool start_rkipc() {
    if(rkipc_ready())return true;
    unlink(c.rkipc_socket.c_str()); pid_t p=fork(); if(p<0){last_error="fork rkipc failed";return false;}
    if(p==0){setsid();setenv("LD_LIBRARY_PATH","/oem/usr/lib",1);execl(c.rkipc_path.c_str(),c.rkipc_path.c_str(),(char*)0);_exit(127);}
    rkipc_pid=p;
    for(int i=0;i<50;++i){if(rkipc_ready()){event("rkipc_started",c.rkipc_path);return true;}usleep(100000);}
    last_error="rkipc did not expose its control socket"; return false;
  }
  bool stop_rkipc() {
    for(int i=0;i<40;++i) { pid_t p=find_rkipc(); if(p<0){unlink(c.rkipc_socket.c_str());rkipc_pid=-1;event("rkipc_stopped","");return true;} kill(p,SIGTERM); usleep(100000); }
    event("rkipc_killed","SIGTERM timeout; sent SIGKILL to rkipc only");
    for(int i=0;i<30;++i) { pid_t p=find_rkipc(); if(p<0){unlink(c.rkipc_socket.c_str());rkipc_pid=-1;event("rkipc_stopped","");return true;} kill(p,SIGKILL); usleep(100000); }
    /* Some firmware leaves a short-lived zombie entry in /proc after SIGKILL.
     * The following RKAIQ-ready check is the authoritative ownership check. */
    unlink(c.rkipc_socket.c_str());
    event("rkipc_force_kill_pending","continuing to RKAIQ readiness check");
    return true;
  }
  void start_npu_later() { npu_started_once=false; npu_start_at=c.start_npu ? monotonic_ms()+1500 : 0; }
  bool start_capture() { return spawn(capture_path(),true); }
  void stop_capture() { kill_child(&sender_pid); kill_child(&npu_pid); npu_started_once=false; npu_start_at=0; restart_at=0; }
  bool enter_debug() {
    mode="DEBUG";
    event("debug_entered","native RKAIQ/SC3336 diagnostic mode");
    return true;
  }
  bool exit_debug() {
    if(mode=="DISPLAY")return true;
    /* Leaving this page must not recycle media-sender.  Some RKAIQ builds
     * reject AUTO after MANUAL AE; restarting then replaces the DMA-BUF
     * preview while Qt still holds old FDs and makes the display go black.
     * Automatic AE is therefore an explicit in-page operation. */
    mode="DISPLAY";
    event("debug_exited",c.auto_ae ? "returned to preview with auto AE" : "returned to preview with retained manual AE");
    return true;
  }
  void kill_child(pid_t* p) {
    if(*p<=0)return;
    pid_t child=*p; kill(-child,SIGTERM); kill(child,SIGTERM);
    for(int i=0;i<30;++i) { int status=0; pid_t result=waitpid(child,&status,WNOHANG); if(result==child || (result<0&&errno==ECHILD)){*p=-1;return;} usleep(100000); }
    event("child_killed","SIGTERM timeout; sent SIGKILL");
    kill(-child,SIGKILL); kill(child,SIGKILL);
    while(waitpid(child,0,0)<0&&errno==EINTR) {}
    *p=-1;
  }
  void check_children() {
    int status=0; pid_t p;
    while((p=waitpid(-1,&status,WNOHANG))>0) {
      bool sender=(p==sender_pid); if(sender) sender_pid=-1; if(p==npu_pid)npu_pid=-1;
      if(sender&&running) {
        last_error="pipeline exited";
        event("pipeline_exit",last_error);
        if(manual_restart_pending) {
          manual_restart_pending=false;
          failures=0;
          state="RECOVERING";
          restart_at=monotonic_ms()+500;
          event("restart_scheduled","manual request");
        } else {
          ++failures;
          if(failures>=c.restart_after_failures) {state="ERROR";restart_at=monotonic_ms()+c.restart_backoff_ms;event("restart_scheduled",last_error);}
        }
      }
    }
    if(c.start_npu && !npu_started_once && npu_start_at && monotonic_ms() >= npu_start_at) {
      npu_started_once=true;
      (void)spawn(c.npu_path,false);
    }
    if(sender_pid<0 && restart_at && monotonic_ms()>=restart_at) {restart_at=0; failures=0; state="RECOVERING"; if(start_capture()) state="NORMAL";}
  }
  bool rkipc_write_all(int fd,const void* data,size_t bytes) {
    const char* p=(const char*)data;
    while(bytes) { ssize_t n=write(fd,p,bytes); if(n<=0)return false; p+=n; bytes-=(size_t)n; }
    return true;
  }
  bool rkipc_read_all(int fd,void* data,size_t bytes) {
    char* p=(char*)data;
    while(bytes) { ssize_t n=read(fd,p,bytes); if(n<=0)return false; p+=n; bytes-=(size_t)n; }
    return true;
  }
  bool rkipc_open(int* fd) {
    *fd=socket(AF_UNIX,SOCK_STREAM,0); if(*fd<0)return false;
    struct timeval timeout; timeout.tv_sec=2; timeout.tv_usec=0;
    (void)setsockopt(*fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    (void)setsockopt(*fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    struct sockaddr_un a;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strncpy(a.sun_path,c.rkipc_socket.c_str(),sizeof(a.sun_path)-1);
    int hello=0;
    if(connect(*fd,(struct sockaddr*)&a,sizeof(a))<0||!rkipc_read_all(*fd,&hello,sizeof(hello))){close(*fd);*fd=-1;last_error="rkipc IPC unavailable";return false;}
    return true;
  }
  bool rkipc_set_string(const char* method,const std::string& value) {
    int fd=-1,id=0,result=-1,name_len=(int)strlen(method)+1,value_len=(int)value.size()+1;
    if(!rkipc_open(&fd)||!rkipc_write_all(fd,&name_len,sizeof(name_len))||!rkipc_write_all(fd,method,(size_t)name_len)||!rkipc_write_all(fd,&id,sizeof(id))||!rkipc_write_all(fd,&value_len,sizeof(value_len))||!rkipc_write_all(fd,value.c_str(),(size_t)value_len)||!rkipc_read_all(fd,&result,sizeof(result))){if(fd>=0)close(fd);last_error="rkipc IPC request failed";return false;}
    close(fd); if(result){last_error="rkipc ISP rejected request";return false;} return true;
  }
  bool rkipc_set_gain(int raw_gain) {
    const char* method="rk_isp_set_exposure_gain"; int fd=-1,id=0,result=-1,name_len=(int)strlen(method)+1,gain=raw_gain/128; if(gain<1)gain=1;
    if(!rkipc_open(&fd)||!rkipc_write_all(fd,&name_len,sizeof(name_len))||!rkipc_write_all(fd,method,(size_t)name_len)||!rkipc_write_all(fd,&id,sizeof(id))||!rkipc_write_all(fd,&gain,sizeof(gain))||!rkipc_read_all(fd,&result,sizeof(result))){if(fd>=0)close(fd);last_error="rkipc gain request failed";return false;}
    close(fd); if(result){last_error="rkipc ISP rejected gain";return false;} return true;
  }
  bool rkipc_set_manual(int exposure,int gain) {
    return rkipc_set_string("rk_isp_set_exposure_mode","manual") &&
           rkipc_set_string("rk_isp_set_gain_mode","manual") &&
           rkipc_set_string("rk_isp_set_exposure_time",std::to_string((double)exposure/40800.0)) &&
           rkipc_set_gain(gain);
  }
  bool control(const std::string& id, int value) {
    if(mode!="DEBUG") { last_error="参数调节仅在驱动调试模式可用"; return false; }
    if(c.auto_ae && (id=="exposure"||id=="analogue_gain")) { last_error="manual exposure/gain rejected while auto_ae=true"; return false; }
    if(id=="exposure") {
      const int gain=manual_gain>=128 ? manual_gain : c.default_analogue_gain;
      if(!isp("manual "+std::to_string(value)+" "+std::to_string(gain)+"\n"))return false;
      manual_exposure=value; manual_gain=gain; event("control_set","exposure"); return true;
    }
    if(id=="analogue_gain") {
      const int exposure=manual_exposure>=1 ? manual_exposure : c.default_exposure;
      if(!isp("manual "+std::to_string(exposure)+" "+std::to_string(value)+"\n"))return false;
      manual_exposure=exposure; manual_gain=value; event("control_set","analogue_gain"); return true;
    }
    __u32 cid=0; if(id=="vblank")cid=V4L2_CID_VBLANK; else if(id=="hflip")cid=V4L2_CID_HFLIP; else if(id=="vflip")cid=V4L2_CID_VFLIP; else if(id=="test_pattern")cid=V4L2_CID_TEST_PATTERN; else {last_error="unsupported control";return false;}
    int fd=open(c.sensor_subdev.c_str(),O_RDWR); if(fd<0){last_error="open sensor failed: "+std::string(strerror(errno));return false;}
    struct v4l2_control ctl; memset(&ctl,0,sizeof(ctl)); ctl.id=cid;ctl.value=value; int r=ioctl(fd,VIDIOC_S_CTRL,&ctl); close(fd); if(r<0){last_error="VIDIOC_S_CTRL failed: "+std::string(strerror(errno));return false;} event("control_set",id); return true;
  }
  bool isp(const std::string&s){int f=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un a;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strncpy(a.sun_path,c.isp_control_socket.c_str(),sizeof(a.sun_path)-1);if(f<0||connect(f,(struct sockaddr*)&a,sizeof(a))<0){last_error="RKAIQ control unavailable";if(f>=0)close(f);return false;}write(f,s.data(),s.size());char b[8]={0};int n=read(f,b,7);close(f);return n>1&&b[0]=='o'&&b[1]=='k';}
  bool restore_defaults() {
    if(mode!="DEBUG") { last_error="恢复默认参数仅在驱动调试模式可用"; return false; }
    const __u32 ids[] = {V4L2_CID_EXPOSURE, V4L2_CID_ANALOGUE_GAIN, V4L2_CID_VBLANK,
                         V4L2_CID_HFLIP, V4L2_CID_VFLIP, V4L2_CID_TEST_PATTERN};
    const int values[] = {c.default_exposure, c.default_analogue_gain, c.default_vblank,
                          c.default_hflip, c.default_vflip, c.default_test_pattern};
    if(!isp("manual "+std::to_string(c.default_exposure)+" "+std::to_string(c.default_analogue_gain)+"\n")) return false;
    int fd=open(c.sensor_subdev.c_str(),O_RDWR);
    if(fd<0){last_error="open sensor failed: "+std::string(strerror(errno));return false;}
    for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);++i) {
      if(ids[i]==V4L2_CID_EXPOSURE || ids[i]==V4L2_CID_ANALOGUE_GAIN)continue;
      struct v4l2_control ctl; memset(&ctl,0,sizeof(ctl)); ctl.id=ids[i]; ctl.value=values[i];
      if(ioctl(fd,VIDIOC_S_CTRL,&ctl)<0) { close(fd); last_error="restore default failed: "+std::string(strerror(errno)); return false; }
    }
    close(fd); manual_exposure=c.default_exposure; manual_gain=c.default_analogue_gain; c.auto_ae=false; last_error.clear(); event("controls_restored","fixed SC3336 defaults, auto_ae=false"); return true;
  }
  int read_control(__u32 cid) const {
    int fd=open(c.sensor_subdev.c_str(),O_RDONLY);
    if(fd<0) return -1;
    struct v4l2_control ctl; memset(&ctl,0,sizeof(ctl)); ctl.id=cid;
    int result=ioctl(fd,VIDIOC_G_CTRL,&ctl); close(fd);
    return result==0 ? ctl.value : -1;
  }
  std::string status() const {
    std::ostringstream o;
    o<<"{\"ok\":true,\"state\":\""<<state<<"\",\"mode\":\""<<mode<<"\",\"auto_ae\":"<<(c.auto_ae?"true":"false")
     <<",\"pipeline_pid\":"<<sender_pid<<",\"npu_pid\":"<<npu_pid<<",\"failures\":"<<failures
     <<",\"last_error\":\""<<json_escape(last_error)<<"\",\"controls\":{\"exposure\":"<<(c.auto_ae ? read_control(V4L2_CID_EXPOSURE) : manual_exposure)
     <<",\"analogue_gain\":"<<(c.auto_ae ? read_control(V4L2_CID_ANALOGUE_GAIN) : manual_gain)<<",\"vblank\":"<<read_control(V4L2_CID_VBLANK)
     <<",\"hflip\":"<<read_control(V4L2_CID_HFLIP)<<",\"vflip\":"<<read_control(V4L2_CID_VFLIP)
     <<",\"test_pattern\":"<<read_control(V4L2_CID_TEST_PATTERN)<<"}}";
    return o.str();
  }
};

CameraDaemon::CameraDaemon(const DaemonConfig& c):impl_(new Impl(c)) {}
CameraDaemon::~CameraDaemon(){stop();delete impl_;}
bool CameraDaemon::start() {
  mkdir_parent(impl_->c.socket_path); unlink(impl_->c.socket_path.c_str()); impl_->server_fd=socket(AF_UNIX,SOCK_STREAM,0); if(impl_->server_fd<0)return false;
  struct sockaddr_un a; memset(&a,0,sizeof(a));a.sun_family=AF_UNIX; strncpy(a.sun_path,impl_->c.socket_path.c_str(),sizeof(a.sun_path)-1);
  if(bind(impl_->server_fd,(struct sockaddr*)&a,sizeof(a))<0||listen(impl_->server_fd,4)<0){close(impl_->server_fd);impl_->server_fd=-1;return false;} chmod(impl_->c.socket_path.c_str(),0660); impl_->event("daemon_started","socket ready");
  if(!impl_->stop_rkipc()) return false;
  if(!impl_->start_capture()) return false;
  impl_->start_npu_later();
  return true;
}
void CameraDaemon::stop(){if(!impl_||!impl_->running)return;impl_->running=false;impl_->stop_capture();impl_->mode="DISPLAY";(void)impl_->start_rkipc();if(impl_->server_fd>=0){close(impl_->server_fd);impl_->server_fd=-1;}unlink(impl_->c.socket_path.c_str());impl_->event("daemon_stopped","");}
void CameraDaemon::run() {
  while(impl_->running) {
    struct pollfd p={impl_->server_fd,POLLIN,0}; int r=poll(&p,1,1000); impl_->check_children();
    if(r>0&&(p.revents&POLLIN)) {
      int fd=accept(impl_->server_fd,0,0);
      if(fd>=0) {
        char b[1025]; ssize_t n=read(fd,b,1024);
        if(n>0) { b[n]=0; std::string x=handle(b); x+='\n'; ssize_t ignored=write(fd,x.data(),x.size()); (void)ignored; }
        close(fd);
      }
    }
  }
}
std::string CameraDaemon::handle(const std::string& r) {
  std::string cmd; if(!field(r,"cmd",&cmd))return "{\"ok\":false,\"error\":\"missing cmd\"}";
  if(cmd=="get_status")return impl_->status();
  if(cmd=="enter_debug")return impl_->enter_debug()?impl_->status():"{\"ok\":false,\"error\":\""+json_escape(impl_->last_error)+"\"}";
  if(cmd=="exit_debug")return impl_->exit_debug()?impl_->status():"{\"ok\":false,\"error\":\""+json_escape(impl_->last_error)+"\"}";
  if(cmd=="restart_pipeline"){
    if(impl_->sender_pid>0) { kill(-impl_->sender_pid,SIGTERM); kill(impl_->sender_pid,SIGTERM); impl_->manual_restart_pending=true; }
    else { impl_->failures=0; impl_->restart_at=monotonic_ms(); }
    impl_->event("restart_requested","");
    return "{\"ok\":true}";
  }
  if(cmd=="set_auto_ae"){if(impl_->mode!="DEBUG")return "{\"ok\":false,\"error\":\"请先进入驱动调试模式\"}";bool x;if(!bool_field(r,"auto_ae",&x))return "{\"ok\":false,\"error\":\"missing auto_ae\"}";int exposure=impl_->manual_exposure>=1?impl_->manual_exposure:impl_->c.default_exposure;int gain=impl_->manual_gain>=128?impl_->manual_gain:impl_->c.default_analogue_gain;bool ok=x?impl_->isp("auto\n"):impl_->isp("manual "+std::to_string(exposure)+" "+std::to_string(gain)+"\n");if(!ok)return "{\"ok\":false,\"error\":\""+json_escape(impl_->last_error)+"\"}";impl_->c.auto_ae=x;impl_->event("ae_mode",x?"auto":"manual");return impl_->status();}
  if(cmd=="set_control"){std::string id;double v;if(!field(r,"id",&id)||!number_field(r,"value",&v))return "{\"ok\":false,\"error\":\"id/value required\"}";return impl_->control(id,(int)v)?"{\"ok\":true}":"{\"ok\":false,\"error\":\""+json_escape(impl_->last_error)+"\"}";}
  if(cmd=="restore_defaults")return impl_->restore_defaults()?impl_->status():"{\"ok\":false,\"error\":\""+json_escape(impl_->last_error)+"\"}";
  if(cmd=="report_metrics"){double luma=0,lat=0;bool stream=true;number_field(r,"luma",&luma);number_field(r,"npu_latency_ms",&lat);bool_field(r,"stream_ok",&stream);if(!stream){++impl_->failures;} if(luma<impl_->c.low_light_luma){++impl_->dark_frames;impl_->bright_frames=0;if(impl_->dark_frames>=impl_->c.low_light_frames&&impl_->state=="NORMAL"){impl_->state="LOW_LIGHT";impl_->event("low_light","threshold reached");}}else{++impl_->bright_frames;impl_->dark_frames=0;if(impl_->state=="LOW_LIGHT"&&impl_->bright_frames>=impl_->c.recover_frames){impl_->state="NORMAL";impl_->event("light_recovered","");}}if(lat>impl_->c.npu_latency_max_ms)impl_->event("npu_latency_high","threshold exceeded");return impl_->status();}
  return "{\"ok\":false,\"error\":\"unknown cmd\"}";
}
bool CameraDaemon::load_config(const std::string& path, DaemonConfig* c, std::string* e) {
  std::ifstream f(path.c_str());if(!f){*e="cannot open";return false;}std::stringstream ss;ss<<f.rdbuf();std::string s=ss.str(),v;
  c->socket_path="/userdata/rv1106-smart-camera/run/camera-daemon.sock";c->log_path="/userdata/rv1106-smart-camera/logs/events.jsonl";c->sensor_subdev="/dev/v4l-subdev2";c->sender_path="/userdata/rv1106-smart-camera/bin/media-sender";c->bridge_path="/userdata/rv1106-smart-camera/bin/rtsp-preview-bridge";c->npu_path="/userdata/npu_detect/npu_detect";c->iq_dir="/oem/usr/share/iqfiles";c->isp_control_socket="/tmp/rv1106_isp_control.sock";c->rkipc_path="/oem/usr/bin/rkipc";c->rkipc_socket="/var/tmp/rkipc";c->rtsp_url="rtsp://127.0.0.1/live/0";c->restart_after_failures=3;c->restart_backoff_ms=3000;c->low_light_frames=15;c->recover_frames=30;c->low_light_luma=45;c->npu_latency_max_ms=150;c->default_exposure=128;c->default_analogue_gain=128;c->default_vblank=64;c->default_hflip=0;c->default_vflip=0;c->default_test_pattern=0;c->start_pipeline=true;c->start_npu=true;c->auto_ae=true;
#define STR(k,m) if(field(s,k,&v))c->m=v
#define NUM(k,m) {double x;if(number_field(s,k,&x))c->m=(int)x;}
#define DBL(k,m) {double x;if(number_field(s,k,&x))c->m=x;}
#define BOL(k,m) {bool x;if(bool_field(s,k,&x))c->m=x;}
  STR("socket_path",socket_path);STR("log_path",log_path);STR("sensor_subdev",sensor_subdev);STR("sender_path",sender_path);STR("bridge_path",bridge_path);STR("npu_path",npu_path);STR("iq_dir",iq_dir);STR("isp_control_socket",isp_control_socket);STR("rkipc_path",rkipc_path);STR("rkipc_socket",rkipc_socket);STR("rtsp_url",rtsp_url);NUM("restart_after_failures",restart_after_failures);NUM("restart_backoff_ms",restart_backoff_ms);NUM("low_light_frames",low_light_frames);NUM("recover_frames",recover_frames);NUM("default_exposure",default_exposure);NUM("default_analogue_gain",default_analogue_gain);NUM("default_vblank",default_vblank);NUM("default_hflip",default_hflip);NUM("default_vflip",default_vflip);NUM("default_test_pattern",default_test_pattern);DBL("low_light_luma",low_light_luma);DBL("npu_latency_max_ms",npu_latency_max_ms);BOL("start_pipeline",start_pipeline);BOL("start_npu",start_npu);BOL("auto_ae",auto_ae);
#undef STR
#undef NUM
#undef DBL
#undef BOL
  if(c->restart_after_failures<1||c->low_light_frames<1||c->recover_frames<1){*e="thresholds must be positive";return false;}return true;
}
