/* Host-only hardware mocks: no device access is performed by this harness. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define open mock_open
#define ioctl mock_ioctl
#define mmap mock_mmap
#define munmap mock_munmap
#define msync mock_msync
#define close mock_close
#define usleep mock_usleep
#include "../../target/psvr2/tools/open_vrhmd/display/display.c"
#include "../../target/psvr2/tools/open_vrhmd/display/panel.c"
#include "../../target/psvr2/tools/open_vrhmd/render/color.c"
int fd_dsi=1,fd_mmsys=2,fd_dscenc=3,fd_mutex=4,fd_lhc=5,fd_rdma=6,fd_slicer=7,fd_ion=8,fd_dprx=9;
volatile sig_atomic_t application_stop_requested;
static uint8_t *pixels;
static int freed,closed,share_fail,synced;
static int fan_stops, fan_failure, quiet_batches;
static int fail_temporary_power, mutex_off, rdma_off, lhc_off;
static int require_black_scanout;
static int framebuffer_mapped, framebuffer_synced;
static int sync_failure, msync_calls, scanout_starts;
int mock_open(const char *path, int flags, ...){(void)flags;assert(!strcmp(path,"/dev/mem"));return 42;}
int mock_ioctl(int fd,unsigned long req,...) {
  if (fail_temporary_power &&
      ((fd == fd_mutex && req == MUTEX_POWER_ON) ||
       (fd == fd_rdma && req == RDMA_POWER_ON) ||
       (fd == fd_lhc && req == LHC_POWER_ON))) {
    errno = EIO;
    return -1;
  }
  if (fd == fd_mutex && req == MUTEX_POWER_OFF) ++mutex_off;
  if (fd == fd_rdma && req == RDMA_POWER_OFF) ++rdma_off;
  if (fd == fd_lhc && req == LHC_POWER_OFF) ++lhc_off;
  va_list va;va_start(va,req);
  if(req==ION_IOC_ALLOC_NR){struct ion_allocation_data *a=va_arg(va,void*);a->handle=17;}
  else if(req==ION_IOC_SHARE_NR){struct ion_fd_data *a=va_arg(va,void*);if(share_fail){errno=EIO;va_end(va);return -1;}a->fd=23;}
  else if(req==ION_IOC_FREE_NR){struct ion_handle_data *a=va_arg(va,void*);assert(a->handle==17);freed++;}
  else if (req == ION_IOC_SYNC_NR) {
    struct ion_fd_data *args = va_arg(va, void *);
    assert(fd == fd_ion && args->fd == 23);
    assert(framebuffer_mapped);
    ++synced;
    if (sync_failure) {
      errno = EIO;
      va_end(va);
      return -1;
    }
    framebuffer_synced = 1;
  }
  else if(req==DSI_SET_LCM_PARAM && !quiet_batches){struct lcm_param_ioctl *a=va_arg(va,void*);printf("%d %d ",a->is_vm_mode,a->table_num);for(int i=0;i<a->table_num;i++){const uint8_t *t=(const uint8_t*)&a->table[i];for(size_t j=0;j<sizeof(a->table[i]);j++)printf("%02x",t[j]);}puts("");}
  else if(req==DPRX_POWER_ON){struct dprx_config *a=va_arg(va,void*);assert(a->hdcp2==0 && a->edid_type==0);}
  else if(req==DSI_START_OUTPUT){struct dsi_start_param *a=va_arg(va,void*);assert(a->wait==0 && a->mask==1);}
  else if (fd == fd_rdma && req == 0x5204) {
    assert(framebuffer_synced && !framebuffer_mapped);
    ++scanout_starts;
    assert(pixels);
    if (require_black_scanout) {
      for (size_t index = 0; index < 24480000; ++index)
        assert(pixels[index] == 0);
    }
  }
  va_end(va);return 0;
}
void *mock_mmap(void *address, size_t size, int protection, int flags,
                int descriptor, off_t offset) {
  (void)address;
  (void)protection;
  (void)flags;
  (void)offset;
  if (descriptor == 42)
    return MAP_FAILED;
  assert(descriptor == 23);
  pixels = malloc(size);
  assert(pixels);
  memset(pixels, 0xa7, size);
  framebuffer_mapped = 1;
  framebuffer_synced = 0;
  return pixels;
}
int mock_munmap(void *address, size_t size) {
  (void)size;
  assert(address == pixels && framebuffer_mapped);
  framebuffer_mapped = 0;
  return 0;
}
int mock_msync(void *address, size_t size, int flags) {
  (void)address;
  (void)size;
  (void)flags;
  ++msync_calls;
  errno = EINVAL;
  return -1;
}
int mock_usleep(useconds_t duration){(void)duration;return 0;}
int mock_close(int fd){if(fd==42)return 0;assert(fd==23);closed++;return 0;}
int audio_stop_hardware(void){return 0;}int fan_ctrl_teardown(void){fan_stops++;return fan_failure ? -1 : 0;}
uint32_t devmem_read32(int fd,uint64_t p){(void)fd;(void)p;return 0;}void devmem_write32(int fd,uint64_t p,uint32_t v){(void)fd;(void)p;(void)v;}
int main(void){
  assert(dprx_power_on()==0);assert(dsi_start_output()==0);
  share_fail=1;assert(allocate_framebuffer(24480000)<0);assert(fb_share_fd==-1 && ion_handle_id==-1 && freed==1 && closed==0);
  share_fail=0;assert(allocate_framebuffer(24480000)==0);
  assert(fill_framebuffer(24480000,0,12,34,56)==0);
  for(size_t i=0;i<24480000;i+=3)assert(pixels[i]==12 && pixels[i+1]==34 && pixels[i+2]==56);free(pixels);
  assert(fill_framebuffer(24480000,1,0,0,0)==0);
  for(int y=0;y<2040;y++)for(int x=0;x<4000;x++){uint8_t expected[3]={0};int lx=x%2000-200;if(y>=820 && y<1220 && lx>=0 && lx<1600)hsv2rgb(lx*360/1600,255,255,expected,expected+1,expected+2);assert(memcmp(pixels+(y*4000+x)*3,expected,3)==0);}free(pixels);
  assert(synced==2);release_framebuffer();assert(freed==2 && closed==1 && fb_share_fd==-1 && ion_handle_id==-1);
  /* Failed temporary acquisitions must not decrement another owner's refs. */
  fail_temporary_power=1;
  assert(cmd_stop()==0);
  assert(mutex_off==0 && rdma_off==0 && lhc_off==0);
  mutex_powered=rdma_powered=lhc_powered=1;
  assert(cmd_stop()==0);
  assert(mutex_off==1 && rdma_off==1 && lhc_off==1);
  assert(g_hardware_powered_on==0);
  for(int s=0;s<4;s++)assert(panel_send_commands(s)==0);
  /* Actual display return releases the pipeline while cooling continues. */
  quiet_batches=1;
  fail_temporary_power=0;
  int prior_fan_stops=fan_stops;
  assert(cmd_go(12,34,56,0)==0);
  free(pixels);
  assert(fan_stops==prior_fan_stops);
  /* Render handoff clears uninitialized ION memory before panel scanout. */
  require_black_scanout = 1;
  assert(cmd_go(0, 0, 0, 100) == 0);
  require_black_scanout = 0;
  assert(cmd_stop() == 0);
  free(pixels);
  /* Failed DMA cache synchronization prevents scanout and unwinds ION. */
  int prior_scanout_starts = scanout_starts;
  int prior_freed = freed;
  int prior_closed = closed;
  sync_failure = 1;
  assert(cmd_go(0, 0, 0, 100) < 0);
  assert(errno == EIO);
  assert(!framebuffer_mapped && !framebuffer_synced);
  assert(scanout_starts == prior_scanout_starts);
  assert(freed == prior_freed + 1 && closed == prior_closed + 1);
  assert(fb_share_fd == -1 && ion_handle_id == -1);
  assert(g_hardware_powered_on == 0);
  assert(msync_calls == 0);
  free(pixels);
  sync_failure = 0;
  prior_fan_stops = fan_stops;
  /* Explicit stop propagates failed fan shutdown rather than claiming success. */
  fan_failure=1;
  assert(cmd_stop()<0);
  assert(fan_stops==prior_fan_stops+1);
  return 0;
}
