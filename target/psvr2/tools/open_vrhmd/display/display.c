#include "../open_vrhmd.h"
#include "../audio/audio.h"
#include "panel.h"
#include <signal.h>
#include <sys/select.h>

/* Global: ION buffer fd, exported for MCPE integration */
int g_hardware_powered_on = 0;
int fb_share_fd = -1;
int ion_handle_id = -1;

static int teardown_display(int stop_cooling);

/* Track successful acquisitions so partial initialization can unwind safely. */
static int fail_display_init(void) {
  int saved_errno = errno;
  cmd_stop();
  errno = saved_errno;
  return -1;
}

static int mutex_powered;
static int rdma_powered;
static int lhc_powered;
static volatile sig_atomic_t display_stop_requested;

static void request_display_stop(int signal_number) {
  (void)signal_number;
  display_stop_requested = 1;
  application_stop_requested = 1;
}

static int dprx_power_on(void) {
#if defined(PSVR2_SOURCE_FAMILY_0600)
  struct dprx_config config = {0}; /* HDCP disabled, 120 Hz EDID */
  return ioctl(fd_dprx, DPRX_POWER_ON, &config);
#elif defined(PSVR2_SOURCE_FAMILY_0110)
  return ioctl(fd_dprx, DPRX_POWER_ON, 0);
#else
  struct dprx_config config = {0};
  int ret = ioctl(fd_dprx, DPRX_POWER_ON_V0600, &config);
  if (ret < 0 && errno == EINVAL)
    ret = ioctl(fd_dprx, DPRX_POWER_ON_V0110, 0);
  return ret;
#endif
}

static int dsi_start_output(void) {
#if defined(PSVR2_SOURCE_FAMILY_0600)
  struct dsi_start_param args = {0, 1};
  return ioctl(fd_dsi, DSI_START_OUTPUT, &args);
#elif defined(PSVR2_SOURCE_FAMILY_0110)
  return ioctl(fd_dsi, DSI_START_OUTPUT, 0);
#else
  struct dsi_start_param args = {0, 1};
  int ret = ioctl(fd_dsi, DSI_START_OUTPUT_V0600, &args);
  if (ret < 0 && errno == EINVAL)
    ret = ioctl(fd_dsi, DSI_START_OUTPUT_V0110, 0);
  return ret;
#endif
}

static void release_framebuffer(void) {
  if (fb_share_fd >= 0) {
    close(fb_share_fd);
    fb_share_fd = -1;
  }
  if (ion_handle_id >= 0 && fd_ion >= 0) {
    struct ion_handle_data handle = {ion_handle_id};
    int ret = ioctl(fd_ion, ION_IOC_FREE_NR, &handle);
    LOG("    ION_IOC_FREE: ret=%d", ret);
    if (ret == 0)
      ion_handle_id = -1;
  }
}

static int allocate_framebuffer(uint32_t size) {
  struct ion_allocation_data alloc = {0};
  alloc.len = size;
  alloc.align = PAGE_SIZE_4K;
  alloc.heap_id_mask = 7;
  int ret = ioctl(fd_ion, ION_IOC_ALLOC_NR, &alloc);
  if (ret < 0) {
    alloc.heap_id_mask = 1;
    ret = ioctl(fd_ion, ION_IOC_ALLOC_NR, &alloc);
  }
  LOG("  ION alloc %u bytes: ret=%d handle=%d", size, ret, alloc.handle);
  if (ret < 0)
    return -1;

  ion_handle_id = alloc.handle;
  struct ion_fd_data share = {.handle = alloc.handle, .fd = -1};
  ret = ioctl(fd_ion, ION_IOC_SHARE_NR, &share);
  if (ret < 0 || share.fd < 0) {
    release_framebuffer();
    return -1;
  }
  fb_share_fd = share.fd;
  LOG("  ION share: fd=%d", fb_share_fd);
  return 0;
}

/* All rows are identical: calculate colors once, then copy the completed row. */
static int fill_framebuffer(uint32_t size, int pattern, uint8_t r,
                            uint8_t g, uint8_t b) {
  enum { WIDTH = 4000, HEIGHT = 2040, EYE_WIDTH = 2000, PITCH = WIDTH * 3,
         BAR_WIDTH = 1600, BAR_HEIGHT = 400, BAR_X = 200, BAR_Y = 820 };
  uint8_t *fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_share_fd, 0);
  if (fb == MAP_FAILED) {
    ERR("map framebuffer");
    return -1;
  }
  if (pattern == 1) {
    memset(fb, 0, size);
    uint8_t *row = fb + BAR_Y * PITCH;
    for (int x = 0; x < BAR_WIDTH; ++x) {
      uint8_t color[3];
      hsv2rgb(x * 360 / BAR_WIDTH, 255, 255, color, color + 1, color + 2);
      memcpy(row + (BAR_X + x) * 3, color, 3);
      memcpy(row + (EYE_WIDTH + BAR_X + x) * 3, color, 3);
    }
    for (int y = BAR_Y + 1; y < BAR_Y + BAR_HEIGHT; ++y)
      memcpy(fb + y * PITCH, row, PITCH);
  } else {
    LOG("  Filling framebuffer with RGB(%u,%u,%u)...", r, g, b);
    for (int x = 0; x < WIDTH; ++x) {
      fb[x * 3] = r;
      fb[x * 3 + 1] = g;
      fb[x * 3 + 2] = b;
    }
    for (int y = 1; y < HEIGHT; ++y)
      memcpy(fb + y * PITCH, fb, PITCH);
  }
  /* ION_IOC_SYNC makes CPU writes coherent for the scanout DMA engine.
   * This dma-buf mapping has no filesystem writeback contract for msync. */
  struct ion_fd_data sync = {.fd = fb_share_fd};
  int ret = ioctl(fd_ion, ION_IOC_SYNC_NR, &sync);
  int sync_errno = ret < 0 ? errno : 0;
  LOG("  ION cache sync: ret=%d errno=%d", ret, sync_errno);
  if (ret < 0) {
    errno = sync_errno;
    ERR("ION framebuffer cache sync");
  }
  int unmap_ret = munmap(fb, size);
  if (unmap_ret < 0)
    ERR("unmap framebuffer");
  if (ret < 0) {
    errno = sync_errno;
    return -1;
  }
  return unmap_ret;
}

static int set_scanout_routes(unsigned long request) {
  int failed = 0;
  static const struct mmsys_connect_args routes[] = {
      {COMP_DSC, COMP_DSI}, {COMP_MDP_RDMA, COMP_DP}, {COMP_MDP_RDMA, COMP_LHC}};
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    int ret = ioctl(fd_mmsys, request, &routes[i]);
    LOG("    route 0x%05X -> 0x%05X: ret=%d", routes[i].cur, routes[i].next, ret);
    failed |= ret < 0;
  }
  return failed ? -1 : 0;
}

static void remove_mutex_components(int include_stale) {
  static const uint32_t components[] = {
      COMP_DSI, COMP_DSC, COMP_MDP_RDMA, COMP_LHC, COMP_DP, COMP_PVRIC_RDMA,
      COMP_DISP_RDMA, COMP_SLICER_VID, COMP_SLICER_DSC, COMP_MUTEX};
  size_t count = include_stale ? sizeof(components) / sizeof(components[0]) : 4;
  for (size_t i = 0; i < count; ++i) {
    struct mutex_add_component_args args = {8, components[i]};
    int ret = ioctl(fd_mutex, MUTEX_REMOVE_COMPONENT, &args);
    LOG("    REMOVE(0x%05X): ret=%d", components[i], ret);
  }
}

/* ═══════════════════════════════════════════════════════════════
 *  ungate — Enable ALL MMSYS core clocks for the display pipeline
 *
 *  MMSYS core CG registers (from clk-mt3612.c):
 *    CG0 @ 0x14000000+0x00 — SLCR, RDMA, PVRIC, LHC, WDMA, DSC_MOUT
 *    CG1 @ 0x14000000+0x04 — DSI0-3_MM, DSI0-3_DIG, CROP, RSZ, DSC, SLICER, etc
 *    CG2 @ 0x14000000+0x08 — SMI LARBs, RBFC, SBRC, fake_eng, event_tx, mutex
 *
 *  These are "no_setclr" registers: bit=1 means GATED (off).
 *  To ungate: write 0 to clear all gate bits.
 *
 *  DSI-specific bits in CG1:
 *    bit 0: DSI0_MM    bit 4: DSI0_DIG
 *    bit 1: DSI1_MM    bit 5: DSI1_DIG
 *    bit 2: DSI2_MM    bit 6: DSI2_DIG
 *    bit 3: DSI3_MM    bit 7: DSI3_DIG
 *    bit 20: DSC0      bit 21: DSC1
 *    bit 22: DSI_LANE_SWAP
 *    bit 23: SLICER_MM
 * ═══════════════════════════════════════════════════════════════ */
int cmd_ungate(void) {
  int fd_mem;
  uint32_t cg0_before, cg1_before, cg2_before;
  uint32_t cg0_after, cg1_after, cg2_after;
  uint32_t dsi_before, dsi_after;

  LOG("=== Ungate MMSYS Display Clocks ===");

  fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd_mem < 0) {
    ERR("open /dev/mem for write");
    return -1;
  }

  /* Read current state */
  cg0_before = devmem_read32(fd_mem, MMSYSCFG_BASE + 0x00);
  cg1_before = devmem_read32(fd_mem, MMSYSCFG_BASE + 0x04);
  cg2_before = devmem_read32(fd_mem, MMSYSCFG_BASE + 0x08);
  LOG("Before: CG0=0x%08X CG1=0x%08X CG2=0x%08X", cg0_before, cg1_before,
      cg2_before);

  /* Ungate ALL clocks in CG0, CG1, CG2 (write 0 = all ungated) */
  LOG("Ungating ALL MMSYS core clocks...");
  devmem_write32(fd_mem, MMSYSCFG_BASE + 0x00, 0x00000000);
  devmem_write32(fd_mem, MMSYSCFG_BASE + 0x04, 0x00000000);
  devmem_write32(fd_mem, MMSYSCFG_BASE + 0x08, 0x00000000);

  /* Verify */
  cg0_after = devmem_read32(fd_mem, MMSYSCFG_BASE + 0x00);
  cg1_after = devmem_read32(fd_mem, MMSYSCFG_BASE + 0x04);
  cg2_after = devmem_read32(fd_mem, MMSYSCFG_BASE + 0x08);
  LOG("After:  CG0=0x%08X CG1=0x%08X CG2=0x%08X", cg0_after, cg1_after,
      cg2_after);

  /* Check DSI registers to see if they're now readable */
  LOG("");
  LOG("Checking DSI0 registers after ungate...");
  dsi_before = devmem_read32(fd_mem, DSI0_BASE + 0x04); /* DSI_STATUS */
  LOG("  DSI_STATUS (0x04) = 0x%08X", dsi_before);
  dsi_after = devmem_read32(fd_mem, DSI0_BASE + 0x10); /* DSI_CON_CTRL */
  LOG("  DSI_CON    (0x10) = 0x%08X", dsi_after);

  close(fd_mem);

  if (cg0_after == 0 && cg1_after == 0 && cg2_after == 0) {
    LOG("=== ALL clocks UNGATED! DSI should now be accessible ===");
    LOG("Try: %s go", "open_vrhmd");
  } else {
    LOG("WARNING: Some gates didn't clear (HW may force-gate)");
  }

  return 0;
}

int cmd_go(uint8_t r, uint8_t g, uint8_t b, int pattern) {
  int ret;

  if (fb_share_fd >= 0 || ion_handle_id >= 0) {
    errno = EBUSY;
    ERR("display pipeline already owns a framebuffer");
    return -1;
  }
  LOG("=== go: Standalone Display Pipeline (%s) ===", OPEN_VRHMD_FIRMWARE);
  LOG("Replicating vrhmd PanelInitialize sequence...");

  /* Step 0: Enable SCPSYS power domains (DP specifically) */
  LOG("[0/13] Enable SCPSYS power domains...");
  {
    int fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem >= 0) {
      /* SPM base = 0x10B00000, need 0x400 range */
      volatile uint32_t *spm = (volatile uint32_t *)mmap(
          NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mem, 0x10B00000);
      if (spm != MAP_FAILED) {
        uint32_t pwr_sta = spm[0x190 / 4];
        LOG("  PWR_STATUS before: 0x%08X", pwr_sta);

        /* Check if DP (bit 19) is already on */
        if (pwr_sta & (1 << 19)) {
          LOG("  DP power domain already ON");
        } else {
          LOG("  DP power domain is OFF, enabling...");
          volatile uint32_t *dp_ctl = &spm[0x378 / 4];
          uint32_t val;

          /* 0. POWERON_CONFIG_EN = PROJECT_CODE | BCLK_CG_EN */
          spm[0x000 / 4] = 0x0b160001;

          /* 1. Set PWR_ON (bit 2) */
          val = *dp_ctl;
          LOG("  DP_PWR_CON initial: 0x%08X", val);
          val |= (1 << 2);
          *dp_ctl = val;

          /* 2. Set PWR_ON_2ND (bit 3) */
          val |= (1 << 3);
          *dp_ctl = val;

          /* 3. Clear ELS_ISO for DP (bit 7 of ELS_ISO_EN_CON @ 0x3BC) */
          volatile uint32_t *els = &spm[0x3BC / 4];
          uint32_t els_val = *els;
          els_val &= ~(1 << 7); /* DP_ELS_ISO */
          *els = els_val;

          /* 4. Wait for PWR_ACK (bit 19 in PWR_STATUS) */
          int timeout;
          for (timeout = 0; timeout < 1000; timeout++) {
            pwr_sta = spm[0x190 / 4];
            if (pwr_sta & (1 << 19))
              break;
            usleep(100);
          }
          LOG("  DP PWR_ACK after %d loops: 0x%08X (bit19=%d)", timeout,
              pwr_sta, !!(pwr_sta & (1 << 19)));

          if (pwr_sta & (1 << 19)) {
            /* 5. Clear PWR_CLK_DIS (bit 4) */
            val = *dp_ctl;
            val &= ~(1 << 4);
            *dp_ctl = val;

            /* 6. Clear PWR_ISO (bit 1) */
            val &= ~(1 << 1);
            *dp_ctl = val;

            /* 7. Set PWR_RST_B (bit 0) */
            val |= (1 << 0);
            *dp_ctl = val;

            /* 8. SRAM_SLEEP_B sequence (bits 12-15, one at a time) */
            val = *dp_ctl;
            if (!(val & (0xF << 12))) {
              for (int b = 12; b <= 15; b++) {
                val = *dp_ctl;
                val |= (1 << b);
                *dp_ctl = val;
              }
              usleep(1);
              /* SRAM_ISOINT = 1 (bit 6) */
              val = *dp_ctl;
              val |= (1 << 6);
              *dp_ctl = val;
              usleep(1);
              /* SRAM_CKISO = 0 (bit 5) */
              val &= ~(1 << 5);
              *dp_ctl = val;
            }

            /* 9. Clear SRAM_PDN (bits 8-11) */
            val = *dp_ctl;
            val &= ~(0xF << 8); /* DP_SRAM_PDN = GENMASK(11,8) */
            *dp_ctl = val;

            /* Wait for SRAM_PDN_ACK to clear (bits 24-27) */
            for (timeout = 0; timeout < 1000; timeout++) {
              if (!((*dp_ctl) & (0xF << 24)))
                break;
              usleep(100);
            }
            LOG("  SRAM_PDN_ACK cleared after %d loops", timeout);

            /* 10. DP_PHY_PWR_RST_B (bit 0 of DP_PHY_PWR_CON @ 0x384) */
            volatile uint32_t *dp_phy = &spm[0x384 / 4];
            uint32_t phy_val = *dp_phy;
            phy_val |= (1 << 0);
            *dp_phy = phy_val;
            LOG("  DP_PHY_PWR_CON: 0x%08X -> 0x%08X", phy_val & ~1, phy_val);
          } else {
            LOG("  *** DP PWR_ACK TIMEOUT! ***");
          }
        }

        pwr_sta = spm[0x190 / 4];
        LOG("  PWR_STATUS final: 0x%08X", pwr_sta);
        LOG("  DP_PWR_CON final: 0x%08X", spm[0x378 / 4]);
        munmap((void *)spm, 0x1000);
      }
      close(fd_mem);
    }
  }

  /* Step 1: Ungate clocks */
  LOG("[1/13] Ungate MMSYS clocks...");
  if (cmd_ungate() < 0)
    return -1;

  /* Step 1b: Panel Power-On via DPRX ioctl */
  LOG("[1b] Panel Power-On (DPRX POWER_ON ioctl)...");

  LOG("  DPRX Power On (%s)...", OPEN_VRHMD_FIRMWARE);
  ret = dprx_power_on();
  if (ret < 0) {
    LOG("  DPRX power-on failed; resetting and retrying the selected ABI");
    ioctl(fd_dprx, DPRX_POWER_OFF);
    usleep(100000);
    ret = dprx_power_on();
  }
  if (ret < 0) {
    ERR("DPRX power-on failed");
    return fail_display_init();
  }
  usleep(1000000); /* regulator startup */

  /* Wait for PD negotiation + regulator startup + check GPIO */
  {
    int poll;
    for (poll = 0; poll < 10; poll++) {
      usleep(500000); /* 500ms per check, up to 5 seconds total */
      int pgm = open("/dev/mem", O_RDWR | O_SYNC);
      if (pgm >= 0) {
        volatile uint8_t *gpio = (volatile uint8_t *)mmap(
            NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, pgm, 0x102D0000);
        if (gpio != MAP_FAILED) {
          uint32_t din4 =
              *(volatile uint32_t *)(gpio + 0x200 +
                                     4 * 0x10); /* DIN bank4 = 0x240 */
          LOG("  [%d/10] GPIO128=%d 129=%d 130=%d 131=%d 132=%d", poll + 1,
              (din4 >> 0) & 1, (din4 >> 1) & 1, (din4 >> 2) & 1,
              (din4 >> 3) & 1, (din4 >> 4) & 1);
          munmap((void *)gpio, 0x1000);
          if (din4 & 0x1F) {
            LOG("  *** POWER DETECTED! ***");
            close(pgm);
            break;
          }
        }
        close(pgm);
      }
    }
    if (poll == 10)
      LOG("  *** NO POWER after 5s — panels may not work ***");
  }

  /* Step 2: LCM panel reset */
  LOG("[2/13] LCM panel reset...");
  {
    int fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem >= 0) {
      volatile uint32_t *mmsys = (volatile uint32_t *)mmap(
          NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mem, 0x14000000);
      if (mmsys != MAP_FAILED) {
        volatile uint32_t *lcm_rst =
            (volatile uint32_t *)((uint8_t *)mmsys + 0x020C);
        uint32_t rst = (1 << 0) | (1 << 2);
        *lcm_rst |= rst;
        usleep(50);
        *lcm_rst &= ~rst;
        usleep(50);
        *lcm_rst |= rst;
        usleep(5000);
        LOG("  LCM_RST_B=0x%08X", *lcm_rst);
        munmap((void *)mmsys, 0x1000);
      }
      close(fd_mem);
    }
  }

  /* Step 3: DDDS config */
  LOG("[3/13] dsiSetDDDSConfig(26,27,1)...");
  {
    struct ddds_config d;
    memset(&d, 0, sizeof(d));
    d.target_freq = 26;
    d.max_freq = 27;
    d.xtal = 1;
    ret = ioctl(fd_dsi, DSI_SET_CONFIG_DDDS, &d);
    LOG("  ret=%d", ret);
   if (ret < 0)
     return fail_display_init();
  }

  /* Step 4: Panel param (DSC mode) */
  LOG("[4/13] dsiSetPanelParam (DSC, 1000x2040)...");
  {
    struct panel_param pp = {
        .hactive = 1000,
        .vactive = 2040,
        .vsync_len = 1,
        .vback_porch = 151,
        .vfront_porch = 8,
        .hsync_len = 22,
        .hback_porch = 38,
        .hfront_porch = 970,
        .format = 4 /* COMPRESSION_24_8 */
    };
    ret = ioctl(fd_dsi, DSI_SET_PANEL_PARAM, &pp);
    LOG("  ret=%d", ret);
   if (ret < 0)
     return fail_display_init();
  }

  /* Step 5: Data rate */
  LOG("[5/13] dsiSetDatarate(907252)...");
  {
    uint32_t dr = 907252; /* from vrhmd log: pll_ck=907252 KHz */
    ret = ioctl(fd_dsi, DSI_SET_DATARATE, dr);
    LOG("  ret=%d", ret);
   if (ret < 0)
     return fail_display_init();
  }

  /* Step 6: Lane swap */
  LOG("[6/13] mmsysDsiLaneSwap(0,1,3,2)...");
  {
    struct mmsys_dsi_lane_swap ls = {0, 1, 3, 2};
    ret = ioctl(fd_mmsys, MMSYS_DSI_LANE_SWAP, &ls);
    LOG("  ret=%d", ret);
   if (ret < 0)
     return fail_display_init();
   if (ret < 0)
     return fail_display_init();
  }

  /* Step 6b: DSI pre-conditioning — flush stale state from killed vrhmd.
   *
   * ROOT CAUSE: When vrhmd_main.elf is killed mid-stream, the DSI controller's
   * internal command FIFO retains pending transactions. A simple DSI_RESET
   * doesn't flush the commander queue — the hardware needs a full
   * STOP_OUTPUT → FINIT → RESET cycle to return to a clean idle state.
   * Without this, DCS commands at Step 10 hit "polling dsi wait not busy timeout". */
  LOG("[6b] DSI pre-conditioning (flush stale state)...");
  {
    int stop_ret, finit_ret;
    /* Stop any active output — this drains the DSI TX FIFO */
    stop_ret = ioctl(fd_dsi, DSI_STOP_OUTPUT);
    LOG("  DSI_STOP_OUTPUT: ret=%d (OK if -1/ENODEV, means already stopped)", stop_ret);

    /* De-initialize DSI — releases internal phy/pll locks */
    finit_ret = ioctl(fd_dsi, DSI_FINIT);
    LOG("  DSI_FINIT: ret=%d", finit_ret);

    /* Let the hardware settle — DSI commander needs time to fully drain */
    usleep(100000); /* 100ms */
  }

  /* Step 7: DSI reset + lane swap */
  LOG("[7/13] dsiReset + dsiLaneSwap(3)...");
  ret = ioctl(fd_dsi, DSI_RESET);
  LOG("  reset ret=%d", ret);
 if (ret < 0)
   return fail_display_init();
  { ret = ioctl(fd_dsi, DSI_LANE_SWAP, 3); }
  LOG("  laneswap ret=%d errno=%d", ret, ret < 0 ? errno : 0);

  /* Step 8: dsiInit */
  LOG("[8/13] dsiInit...");
  ret = ioctl(fd_dsi, DSI_INIT);
  LOG("  ret=%d errno=%d", ret, ret < 0 ? errno : 0);
  if (ret < 0) {
    ERR("dsiInit FAILED");
    return fail_display_init();
  }

  /* Debug: DSI register dump after dsiInit */
  {
    int dbg_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (dbg_mem >= 0) {
      volatile uint8_t *dsi_dbg =
          (volatile uint8_t *)mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, dbg_mem, 0x14020000);
      if (dsi_dbg != MAP_FAILED) {
        int di;
        LOG("  --- DSI state after dsiInit ---");
        for (di = 0; di < 4; di++) {
          volatile uint32_t *base =
              (volatile uint32_t *)(dsi_dbg + di * 0x1000);
          LOG("  DSI%d: START=%08X STA=%08X CON=%08X MODE=%08X TXRX=%08X "
              "PHY=%08X",
              di, base[0], base[1], base[4], base[5], base[6],
              *(volatile uint32_t *)(dsi_dbg + di * 0x1000 + 0x104));
        }
        munmap((void *)dsi_dbg, 0x4000);
      }
      /* Also check GPIO 128-132 power-good */
      volatile uint8_t *gpio =
          (volatile uint8_t *)mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, dbg_mem, 0x102D0000);
      if (gpio != MAP_FAILED) {
        uint32_t dir4 =
            *(volatile uint32_t *)(gpio + 0x000 +
                                   4 * 0x10); /* DIR bank4 = 0x040 */
        uint32_t dout4 =
            *(volatile uint32_t *)(gpio + 0x100 +
                                   4 * 0x10); /* DOUT bank4 = 0x140 */
        uint32_t din4 =
            *(volatile uint32_t *)(gpio + 0x200 +
                                   4 * 0x10); /* DIN bank4 = 0x240 */
        LOG("  --- GPIO 128-132 (display power) ---");
        LOG("  DIR[4]=%08X  DOUT[4]=%08X  DIN[4]=%08X", dir4, dout4, din4);
        LOG("  GPIO128=%d 129=%d 130=%d 131=%d 132=%d", (din4 >> 0) & 1,
            (din4 >> 1) & 1, (din4 >> 2) & 1, (din4 >> 3) & 1, (din4 >> 4) & 1);
        munmap((void *)gpio, 0x1000);
      }
      close(dbg_mem);
    }
  }

  /* Step 9: Update Hblank */
  LOG("[9/13] dsiUpdateHblank(4,210,308)...");
  {
    /* vrhmd log: hsync:4, hbpwc:210, hfpwc:308 */
    struct dsi_hblank_args hb = {4, 210, 308};
    ret = ioctl(fd_dsi, DSI_UPDATE_HBLANKING, &hb);
    LOG("  ret=%d", ret);
   if (ret < 0)
     return fail_display_init();
  }

  /* Step 10: DCS command tables — EXACT VrhmdMain sequence from ioctl hook */
  LOG("[10/13] DCS commands (VrhmdMain-exact)...");
  if (panel_send_commands(PANEL_COMMAND_INIT) < 0)
    return fail_display_init();

  /* Step 11: Panel ID */
  LOG("[11/13] Panel ID...");
  {
    struct {
      uint8_t l[11];
      uint8_t r[11];
    } pid;
    memset(&pid, 0, sizeof(pid));
    ret = ioctl(fd_dsi, 0x80164412, &pid);
    if (ret >= 0) {
      LOG("  L: %02X %02X %02X %02X %02X %02X %02X", pid.l[0], pid.l[1],
          pid.l[2], pid.l[3], pid.l[4], pid.l[5], pid.l[6]);
      LOG("  R: %02X %02X %02X %02X %02X %02X %02X", pid.r[0], pid.r[1],
          pid.r[2], pid.r[3], pid.r[4], pid.r[5], pid.r[6]);
    } else {
      LOG("  failed ret=%d", ret);
    }
  }

  /* CMD Batch 7 (1 entry): NOP after panel read */
  if (panel_send_commands(PANEL_COMMAND_AFTER_READ) < 0)
    return fail_display_init();

  /* Step 12: DSI_SET_SW_MUTE(false) + DSI_START_OUTPUT */
  LOG("[12/13] dsiStartOutput...");
  {
    uint8_t m = 0;
    ret = ioctl(fd_dsi, DSI_SET_SW_MUTE, &m);
    if (ret < 0)
      return fail_display_init();
  }
  ret = dsi_start_output();
  LOG("  DSI_START_OUTPUT(%s): ret=%d", OPEN_VRHMD_FIRMWARE, ret);
  if (ret < 0)
    return fail_display_init();

  /* Step 12b: VM-mode DCS batches after DSI_START_OUTPUT (CRITICAL!)
   * These unmute the video path and send Sleep Out in video mode */
  LOG("[12b] Post-start VM DCS (video unmute + sleep out)...");
  if (panel_send_commands(PANEL_COMMAND_AFTER_START) < 0)
    return fail_display_init();

  /* Step 12c: Camera sync BEFORE pipeline (matches VrhmdMain order) */
  LOG("[12c] mmsysCameraSyncClockSel + SyncConfig...");
  {
    uint8_t sync_sel = 1;
    ret = ioctl(fd_mmsys, MMSYS_CAMERA_SYNC_SEL, sync_sel);
    LOG("  CameraSyncClockSel(1): ret=%d", ret);

    struct {
      uint32_t camera_id, vsync_cycle, delay_cycle, vsync_low_active;
    } sc;
    sc.camera_id = 0;
    sc.vsync_cycle = 16;
    sc.delay_cycle = 100;
    sc.vsync_low_active = 0;
    ioctl(fd_mmsys, 0x40104D05, &sc);
    sc.camera_id = 1;
    sc.delay_cycle = 74947;
    ioctl(fd_mmsys, 0x40104D05, &sc);
    sc.camera_id = 2;
    ioctl(fd_mmsys, 0x40104D05, &sc);
  }

  /* Step 12c: DSI state after start */
  {
    int memfd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (memfd >= 0) {
      volatile uint32_t *d = (volatile uint32_t *)mmap(
          NULL, 0x4000, PROT_READ, MAP_SHARED, memfd, 0x14020000);
      if (d != MAP_FAILED) {
        LOG("  --- DSI after StartOutput ---");
        LOG("  DSI0: START=%08X INTSTA=%08X CON=%08X MODE=%08X", d[0],
            d[0x0C / 4], d[0x10 / 4], d[0x14 / 4]);
        LOG("  PSCON=%08X TXRX=%08X", d[0x1C / 4], d[0x18 / 4]);
        munmap((void *)d, 0x4000);
      }
      close(memfd);
    }
  }

  /* ═══════════════════════════════════════════════════════════════
   * Step 13: RGB888 Data Pipeline (replaces BIST)
   * From IDA VrhmdMain v01.10: exact ioctl sequence for non-PVRIC scanout
   * Pipeline: ION(RGB888) -> MDP_RDMA -> LHC -> DSC -> DSI
   * ═══════════════════════════════════════════════════════════════ */
  LOG("[13] Setting up RGB888 data pipeline (VrhmdMain-exact)...");

  /* --- 13a: Allocate ION framebuffer (RGB888: 4000x2040x3 = ~24MB) --- */
  uint32_t fb_size = 4000 * 2040 * 3; /* RGB888 */
  if (allocate_framebuffer(fb_size) < 0) {
    ERR("ION allocation/share failed");
    return fail_display_init();
  }
  {
    /* --- 13b: Fill framebuffer --- */
#ifdef GPU_RENDER
    if (pattern == 2) {
      if (gpu_render_gradient(fb_share_fd) < 0) {
        ERR("GPU gradient failed; using CPU gradient");
        if (fill_framebuffer(fb_size, 1, r, g, b) < 0)
          return fail_display_init();
      }
    } else
#endif
    {
      /* Native render clients return with this buffer already scanning out.
       * Initialize it before enabling the panel, including the handoff mode. */
      if ((pattern < 3 || pattern == 100) &&
          fill_framebuffer(fb_size, pattern, r, g, b) < 0)
        return fail_display_init();
    }

    /* ═══════════════════════════════════════════════════════════
     * VrhmdMain "Create Scanout Path w/o PVRIC" exact sequence:
     *   1. mutexGetResource(8)       - value direct
     *   2. mutexPowerOn(8)           - value direct
     *   3. dscencPowerOn()
     *   4. lhcPowerOn()
     *   5. rdmaPowerOn(MDP_RDMA)
     *   6. rdmaLarbGet(MDP_RDMA, 0)  - value direct
     *   7. mmsysConnectComponent(DSI, DSC)
     *   8. mmsysConnectComponent(MDP_RDMA, DP)
     *   9. mmsysConnectComponent(MDP_RDMA, LHC)
     *  10. rdmaTypeSel(MDP_RDMA, 0)  - value direct
     *  11. rdmaConfigFrame(...)
     *  12. rdmaStart()
     *  13. mmsysLhcSwapConfig({1,0,3,2})
     *  14. lhcConfigSlice(w=1000,h=2040,start=0,end=999)
     *  15. lhcStart()
     *  16.
     * dscencInitHw(w=2000,h=2040,slice_h=102,2in2out,v1.1,AT_LINE,24_8,pps)
     *  17. dscencStart() or dscencRelay()
     *  18. mutexAddComponent(8, DSI=0x60000)
     *  19. mutexAddComponent(8, DSC=0x70000)
     *  20. mutexAddComponent(8, MDP_RDMA=0x40000)
     *  21. mutexAddComponent(8, LHC=0xB0000)
     *  22. mutexSetConfig(8, SOF_DSI, DSI, 0, 0)
     *  23. mutexEnable(8, 1)
     * ═══════════════════════════════════════════════════════════ */

    /* Pre-condition: Full scanout pipeline teardown from killed vrhmd.
     *
     * When vrhmd_main.elf is killed mid-stream, RDMA/LHC/DSC/MUTEX remain in
     * active state. The kernel drivers refuse to re-initialize components that
     * are "already running". We must stop everything first. Errors from these
     * calls are expected and harmless (means component wasn't running). */
    LOG("  [pre] Pipeline pre-teardown (clear stale state)...");
    {
      int pr;

      /* Stop RDMA */
      pr = ioctl(fd_rdma, RDMA_STOP);
      LOG("    RDMA_STOP: ret=%d", pr);

      /* Stop LHC */
      pr = ioctl(fd_lhc, LHC_STOP);
      LOG("    LHC_STOP: ret=%d", pr);

      /* Reset DSC */
      pr = ioctl(fd_dscenc, DSC_RESET);
      LOG("    DSC_RESET: ret=%d", pr);

      set_scanout_routes(MMSYS_DISCONNECT_COMP);
    }

    /* Pre-condition: Release any stale MUTEX8 from a killed vrhmd_main.elf.
     *
     * ROOT CAUSE: When vrhmd_main.elf is killed mid-stream, MUTEX resource 8
     * remains acquired/powered/enabled in the kernel driver. The driver refuses
     * to re-acquire it (EINVAL). We must fully tear down the old MUTEX8 state
     * before re-initializing: Disable → Remove all components → Power Off →
     * Put Resource. Errors here are expected and harmless (means it wasn't held). */
    LOG("  [0] MUTEX8 pre-release (clear stale state)...");
    {
      struct { uint32_t idx; uint32_t enable; } men = { 8, 0 };
      int pr;

      /* Disable MUTEX8 */
      pr = ioctl(fd_mutex, MUTEX_ENABLE, &men);
      LOG("    Disable: ret=%d", pr);

      remove_mutex_components(1);

      /* Power off MUTEX8 */
      pr = ioctl(fd_mutex, MUTEX_POWER_OFF, 8);
      LOG("    PowerOff: ret=%d", pr);

      /* Release resource */
      pr = ioctl(fd_mutex, MUTEX_PUT_RESOURCE, 8);
      LOG("    PutResource: ret=%d", pr);

      usleep(50000); /* 50ms settle */
    }

    /* Step 1: mutexGetResource(8) — NOTE: VrhmdMain passes value directly */
    LOG("  [1] mutexGetResource(8)...");
    ret = ioctl(fd_mutex, 0x40044D01 /* MUTEX_GET_RESOURCE */, 8);
    LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
   if (ret < 0)
     return fail_display_init();

    /* Step 2: mutexPowerOn(8) — value directly */
    LOG("  [2] mutexPowerOn(8)...");
    ret = ioctl(fd_mutex, MUTEX_POWER_ON, 8);
    mutex_powered = (ret == 0);
    g_hardware_powered_on = mutex_powered;
    LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
   if (ret < 0)
     return fail_display_init();

    /* Step 3: dscencPowerOn */
    LOG("  [3] dscencPowerOn...");
    ret = ioctl(fd_dscenc, 0x4501 /* DSCENC_POWER_ON */);
    LOG("    ret=%d", ret);
   if (ret < 0)
     return fail_display_init();

    /* Step 4: lhcPowerOn */
    LOG("  [4] lhcPowerOn...");
    ret = ioctl(fd_lhc, LHC_POWER_ON);
    lhc_powered = (ret == 0);
    LOG("    ret=%d", ret);
   if (ret < 0)
     return fail_display_init();

    /* Step 5: rdmaPowerOn — single RDMA for MDP (component=0x40000) */
    LOG("  [5] rdmaPowerOn...");
    ret = ioctl(fd_rdma, RDMA_POWER_ON);
    rdma_powered = (ret == 0);
    LOG("    ret=%d", ret);
   if (ret < 0)
     return fail_display_init();

    /* Step 6: rdmaLarbGet(0) — VrhmdMain passes value directly */
    LOG("  [6] rdmaLarbGet(0)...");
    ret = ioctl(fd_rdma, 0x40015208 /* RDMA_LARB_GET */, 0);
    LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
   if (ret < 0)
     return fail_display_init();

    /* Steps 7-9: mmsysConnectComponent via MMSYSCFG_CONNECT_COMPONENT ioctl
     * ioctl(mmsys_fd, 0x40084D01, {cur, next}) on /dev/mmsys
     * Note: 0x40084D01 is MMSYSCFG_CONNECT_COMPONENT, NOT MUTEX_GET_RESOURCE.
     * Both use IOC type 'M' but go to different devices! */
    LOG("  [7-9] mmsysConnectComponent...");
    if (set_scanout_routes(MMSYS_CONNECT_COMP) < 0)
      return fail_display_init();

    /* Step 10: rdmaTypeSel — kernel passes arg directly as value, NOT pointer
     */
    LOG("  [10] rdmaTypeSel(MDP=0)...");
    ret = ioctl(fd_rdma, 0x4001520B /* RDMA_TYPE_SEL */,
                0 /* MTK_WRAPPER_RDMA_TYPE_MDP */);
    LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
   if (ret < 0)
     return fail_display_init();

    /* Step 11: rdmaConfigFrame (non-PVRIC: format=RGB, pitch=3*width) */
    LOG("  [11] rdmaConfigFrame...");
    {
      struct rdma_set_config rdma_cfg;
      memset(&rdma_cfg, 0, sizeof(rdma_cfg));
      rdma_cfg.format = 3; /* MTK_WRAPPER_FRAME_FORMAT_RGB */
      rdma_cfg.ionfd[0] = fb_share_fd;
      rdma_cfg.ionfd[1] = fb_share_fd; /* VrhmdMain uses buffer_num=3 */
      rdma_cfg.ionfd[2] = fb_share_fd;
      rdma_cfg.buffer_num = 3; /* dword_1663DC = 3 in VrhmdMain */
      /* VrhmdMain uses FULL width: dword_1663E0 = panel[12]*panel[4] = 2*2000 =
       * 4000 pitch = 3 * 4000 = 12000. RDMA internally splits into 4 ×
       * 1000-pixel strips for the 4 DSI engines. */
      rdma_cfg.pitch = 12000; /* 4000 * 3 bytes/pixel */
      rdma_cfg.width = 4000;  /* full dual-panel width */
      rdma_cfg.height = 2040;
      rdma_cfg.pvric_header_offset = 0;
      rdma_cfg.rbfc_setting = 1; /* VrhmdMain: rbfc_setting=1, enables RDMA
                                    frame-complete interrupt */
      rdma_cfg.use_sram = 0;
      ret = ioctl(fd_rdma, 0x40285206 /* RDMA_CONFIG_FRAME */, &rdma_cfg);
      LOG("    CONFIG_FRAME(RGB,w=%u,h=%u,pitch=%u): ret=%d errno=%d",
          rdma_cfg.width, rdma_cfg.height, rdma_cfg.pitch, ret,
          ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 12: rdmaStart */
    LOG("  [12] rdmaStart...");
    ret = ioctl(fd_rdma, 0x5204 /* RDMA_START */);
    LOG("    ret=%d", ret);
   if (ret < 0)
     return fail_display_init();

    /* Step 13: mmsysLhcSwapConfig — VrhmdMain uses {0,1,2,3} (identity, no
     * swap) */
    LOG("  [13] mmsysLhcSwapConfig({0,1,2,3})...");
    {
      uint32_t lhc_swap[4] = {0, 1, 2, 3}; /* identity — from intercept */
      ret =
          ioctl(fd_mmsys, 0x40104D0B /* MMSYSCFG_LHC_SWAP_CONFIG */, lhc_swap);
      LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 14: lhcConfigSlice — from IDA: w=a1>>2, h=a2, start=0, end=(a1>>2)-1
     * a1 = panel[12]*panel[4] = 2*2000 = 4000
     * so w = 4000>>2 = 1000, end = 999 */
    LOG("  [14] lhcConfigSlice...");
    {
      struct {
        struct {
          uint32_t w, h, start, end;
        } slice[4];
      } lhc_cfg;
      for (int i = 0; i < 4; i++) {
        lhc_cfg.slice[i].w = 1000;
        lhc_cfg.slice[i].h = 2040;
        lhc_cfg.slice[i].start = 0;
        lhc_cfg.slice[i].end = 999; /* (a1>>2) - 1 = 999 */
      }
      ret = ioctl(fd_lhc, 0x40404C03 /* LHC_CONFIG_SLICE */, &lhc_cfg);
      LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 15: lhcStart */
    LOG("  [15] lhcStart...");
    ret = ioctl(fd_lhc, 0x4C04 /* LHC_START */);
    LOG("    ret=%d", ret);
   if (ret < 0)
     return fail_display_init();

    /* Step 16: dscencInitHw — from IDA panel entry 3 (DSC_8BPP):
     * pic_w=2000, pic_h=2040, slice_h=102, 2in2out, v1.1, AT_LINE, COMP_24_8
     * PPS from VrhmdMain binary at 0xC17A0 */
    LOG("  [16] dscencInitHw...");
    {
      /* PPS from VrhmdMain binary at 0xC17A0 — stored as RAW BYTES
       * (not u32 values!) to preserve exact byte order from binary.
       * Verified byte-for-byte against LD_PRELOAD intercept. */
      static const uint8_t pps_bytes[76] = {
          0x00, 0x02, 0x73, 0x03, 0x20, 0x00, 0x2F, 0x0B, 0x0D, 0x00, 0x0F,
          0x00, 0x31, 0x01, 0x8A, 0x00, 0x00, 0x18, 0xF0, 0x10, 0x03, 0x0C,
          0x00, 0x20, 0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38, 0x46,
          0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x00, 0x00,
          0x80, 0x08, 0x80, 0x00, 0xA1, 0x00, 0xC1, 0xF8, 0xE3, 0xF0, 0xE3,
          0xE8, 0xE3, 0xE0, 0x03, 0xE1, 0x23, 0xE1, 0x43, 0xD9, 0x45, 0xD9,
          0x65, 0xD1, 0x65, 0xD1, 0x89, 0xD1, 0xAC, 0xD1, 0x00, 0x00};

      struct dsc_init_config dsc_cfg;
      memset(&dsc_cfg, 0, sizeof(dsc_cfg));
      dsc_cfg.disp_pic_w = 2000;
      dsc_cfg.disp_pic_h = 2040;
      dsc_cfg.slice_h = 102; /* from IDA: 0x66 = 102 (2040/102=20 slices) */
      dsc_cfg.inout_sel = 2; /* 2_IN_2_OUT */
      dsc_cfg.version = 0;   /* DSC 1.1 */
      dsc_cfg.ich_line_clear = 1; /* AT_LINE (from IDA: value=1) */
      dsc_cfg.format = 4; /* COMPRESSION_24_8 -> chunk_size=1000 (3:1 DSC) */
      dsc_cfg.pps = (uint64_t)(uintptr_t)pps_bytes;
      ret = ioctl(fd_dscenc, 0x40284502 /* DSCENC_INIT_HW */, &dsc_cfg);
      LOG("    INIT_HW(w=%u,h=%u,slice_h=%u,2in2out,AT_LINE,24_8): ret=%d "
          "errno=%d",
          dsc_cfg.disp_pic_w, dsc_cfg.disp_pic_h, dsc_cfg.slice_h, ret,
          ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 17: dscencStart (or dscencRelay depending on panel config) */
    LOG("  [17] dscencStart...");
    ret = ioctl(fd_dscenc, 0x4503 /* DSCENC_START */);
    LOG("    ret=%d", ret);
   if (ret < 0)
     return fail_display_init();

    /* Steps 18-21: mutexAddComponent — add DSI, DSC, MDP_RDMA, LHC */
    LOG("  [18-21] mutexAddComponent...");
    {
      struct mutex_add_component_args add_comp;
      uint32_t components[] = {
          COMP_DSI,      /* 0x60000 — VrhmdMain adds DSI first */
          COMP_DSC,      /* 0x70000 */
          COMP_MDP_RDMA, /* 0x40000 */
          COMP_LHC,      /* 0xB0000 */
      };
      for (int i = 0; i < 4; i++) {
        add_comp.idx = 8;
        add_comp.component = components[i];
        ret = ioctl(fd_mutex, 0x40084D04 /* MUTEX_ADD_COMPONENT */, &add_comp);
        LOG("    ADD(0x%05X): ret=%d", components[i], ret);
       if (ret < 0)
         return fail_display_init();
      }
    }

    /* Step 22: mutexSetConfig(8, SOF_DSI, DSI, 0, 0) */
    LOG("  [22] mutexSetConfig...");
    {
      struct mutex_set_config_args mutex_cfg;
      memset(&mutex_cfg, 0, sizeof(mutex_cfg));
      mutex_cfg.idx = 8;
      mutex_cfg.sof_component = 1;              /* SOF_DSI */
      mutex_cfg.sof_timer_component = COMP_DSI; /* 0x60000 */
      mutex_cfg.src_time = 0;
      mutex_cfg.ref_time = 0;
      ret = ioctl(fd_mutex, 0x40144D03 /* MUTEX_SET_CONFIG */, &mutex_cfg);
      LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 23: mutexEnable(8, 0xB0001) — VrhmdMain passes 0xB0001, NOT 1 */
    LOG("  [23] mutexEnable(8, 0xB0001)...");
    {
      struct mutex_enable_args mutex_en;
      memset(&mutex_en, 0, sizeof(mutex_en));
      mutex_en.idx = 8;
      mutex_en.enable = 0xB0001;
      ret = ioctl(fd_mutex, 0x40084D06 /* MUTEX_ENABLE */, &mutex_en);
      LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 24: Post-MUTEX VM DCS (4 batches from VrhmdMain)
     * These set brightness, configure panel registers, and complete unmute */
    LOG("  [24] Post-MUTEX VM DCS...");
    if (panel_send_commands(PANEL_COMMAND_AFTER_MUTEX) < 0)
      return fail_display_init();

    /* Step 25: RDMA_UPDATE_WP(0) — tell RDMA which buffer to display */
    LOG("  [25] RDMA_UPDATE_WP(0)...");
    ret = ioctl(fd_rdma, 0x4004520C /* RDMA_UPDATE_WP */, 0);
    LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
   if (ret < 0)
     return fail_display_init();

    /* Step 26: DSI_SEND_VM_CMD(0x29) = Display ON */
    LOG("  [26] DSI_SEND_VM_CMD(0x29=Display ON)...");
    {
      uint8_t vm_cmd[18];
      memset(vm_cmd, 0, sizeof(vm_cmd));
      vm_cmd[0] = 0x29; /* DCS Display ON */
      ret = ioctl(fd_dsi, 0x40124411 /* DSI_SEND_VM_CMD */, vm_cmd);
      LOG("    ret=%d errno=%d", ret, ret < 0 ? errno : 0);
     if (ret < 0)
       return fail_display_init();
    }

    /* Step 27: Camera Sync Disable (VrhmdMain: 3x MMSYS_SYNC_DISABLE after
     * Display ON) */
    LOG("  [27] CameraSyncDisable...");
    {
      uint32_t sync_id;
      sync_id = 0;
      ioctl(fd_mmsys, 0x40044D08 /* MMSYS_SYNC_DISABLE */, &sync_id);
      sync_id = 1;
      ioctl(fd_mmsys, 0x40044D08, &sync_id);
      sync_id = 2;
      ioctl(fd_mmsys, 0x40044D08, &sync_id);
      LOG("    done");
    }

    /* Also clear DSC MUTE_CON bit 31 via /dev/mem as safety */
    {
      int mfd = open("/dev/mem", O_RDWR | O_SYNC);
      if (mfd >= 0) {
        static const uint32_t dsc_offsets[] = {0, 0x400, 0x1000, 0x1400};
        volatile uint8_t *map = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, mfd, 0x1401E000);
        if (map != MAP_FAILED) {
          for (size_t i = 0; i < sizeof(dsc_offsets) / sizeof(dsc_offsets[0]); ++i) {
            volatile uint32_t *mute = (volatile uint32_t *)(map + dsc_offsets[i] + 0x06C);
            uint32_t old = *mute;
            *mute = old & ~(1u << 31);
            LOG("    DSC%zu MUTE_CON: 0x%08X -> 0x%08X", i, old, *mute);
          }
          munmap((void *)map, 0x2000);
        }
        close(mfd);
      }
    }

    /* --- Dump state after pipeline start --- */
    usleep(100000); /* 100ms — let frames through */
    {
      int memfd = open("/dev/mem", O_RDONLY | O_SYNC);
      if (memfd >= 0) {
        volatile uint32_t *d = (volatile uint32_t *)mmap(
            NULL, 0x4000, PROT_READ, MAP_SHARED, memfd, 0x14020000);
        if (d != MAP_FAILED) {
          LOG("  --- DSI state after pipeline start ---");
          LOG("  DSI0: START=%08X INTSTA=%08X MODE=%08X PSCON=%08X", d[0],
              d[0x0C / 4], d[0x14 / 4], d[0x1C / 4]);
          munmap((void *)d, 0x4000);
        }
        volatile uint32_t *mu = (volatile uint32_t *)mmap(
            NULL, 0x200, PROT_READ, MAP_SHARED, memfd, 0x14001000);
        if (mu != MAP_FAILED) {
          LOG("  MUTEX8: EN=%08X CTL=%08X MOD0=%08X MOD1=%08X", mu[0x120 / 4],
              mu[0x12C / 4], mu[0x130 / 4], mu[0x138 / 4]);
          munmap((void *)mu, 0x200);
        }
        volatile uint32_t *rdma_r = (volatile uint32_t *)mmap(
            NULL, 0x2000, PROT_READ, MAP_SHARED, memfd, 0x14004000);
        if (rdma_r != MAP_FAILED) {
          LOG("  RDMA0: EN=%08X SRC_CON=%08X SRC_SIZE=%08X BKGD=%08X",
              rdma_r[0x000 / 4], rdma_r[0x030 / 4], rdma_r[0x070 / 4],
              rdma_r[0x060 / 4]);
          LOG("  RDMA0: BASE=%08X OFFSET=%08X END=%08X MON_STA0=%08X",
              rdma_r[0xF00 / 4], rdma_r[0x118 / 4], rdma_r[0x100 / 4],
              rdma_r[0x400 / 4]);
          munmap((void *)rdma_r, 0x2000);
        }
        volatile uint32_t *dsc_r = (volatile uint32_t *)mmap(
            NULL, 0x1000, PROT_READ, MAP_SHARED, memfd, 0x1401E000);
        if (dsc_r != MAP_FAILED) {
          LOG("  DSC0: CON=%08X PIC_W=%08X PIC_H=%08X SLICE=%08X MUTE=%08X",
              dsc_r[0x000 / 4], dsc_r[0x018 / 4], dsc_r[0x01C / 4],
              dsc_r[0x020 / 4], dsc_r[0x06C / 4]);
          munmap((void *)dsc_r, 0x1000);
        }
        /* MMSYS routing readback */
        volatile uint32_t *mm = (volatile uint32_t *)mmap(
            NULL, 0x200, PROT_READ, MAP_SHARED, memfd, 0x14000000);
        if (mm != MAP_FAILED) {
          LOG("  MMSYS: OUT_SEL=%u,%u,%u,%u  LHC_SEL=%u,%u,%u,%u",
              mm[0x0FC / 4], mm[0x100 / 4], mm[0x104 / 4], mm[0x108 / 4],
              mm[0x124 / 4], mm[0x128 / 4], mm[0x12C / 4], mm[0x130 / 4]);
          munmap((void *)mm, 0x200);
        }
        close(memfd);
      }
    }

    LOG("");
    LOG("=== DISPLAY PIPELINE ACTIVE! Check headset ===");

    if (application_stop_requested) return teardown_display(0);
    if (pattern == 100) {
      LOG("  [MCPE] Pipeline ready; returning to host engine");
      return 0;
    }

#ifdef GPU_RENDER
    if (pattern >= 3) {
      /* 3D benchmark render loop / 2D Image Viewer */
      ret = gpu_app_loop(fb_share_fd);
    } else
#endif
    {
      LOG("Press Ctrl+C or type 'stop' then Enter on stdin to end.");
      struct sigaction stop_action = {0}, old_interrupt, old_terminate;
      stop_action.sa_handler = request_display_stop;
      sigemptyset(&stop_action.sa_mask);
      display_stop_requested = application_stop_requested;
      int restore_interrupt = sigaction(SIGINT, &stop_action, &old_interrupt) == 0;
      int restore_terminate = sigaction(SIGTERM, &stop_action, &old_terminate) == 0;

#ifdef GPU_RENDER
      uint8_t *mirror_buf = NULL;
      size_t mirror_size = 1000 * 510 * 3;
      if (g_stream) {
        g_stream_fd = open("/dev/fast_stream", O_WRONLY | O_NONBLOCK);
        if (g_stream_fd >= 0) {
          mirror_buf = malloc(sizeof(struct vrh2_header) + mirror_size);
          if (mirror_buf) {
            struct vrh2_header *hdr = (struct vrh2_header *)mirror_buf;
            hdr->magic = 0x32485256;
            hdr->frame_no = 0;
            hdr->width = 1000;
            hdr->height = 510;
            hdr->pitch = 1000 * 3;
            hdr->format = 0; /* RGB888 */
            hdr->size = mirror_size;
            hdr->reserved = 0;

            uint8_t *payload = mirror_buf + sizeof(struct vrh2_header);

            /* Map full buffer (RGB888 4000x2040) statically */
            uint8_t *fb = (uint8_t *)mmap(NULL, 12000 * 2040, PROT_READ,
                                          MAP_SHARED, fb_share_fd, 0);
            if (fb != MAP_FAILED) {
              /* Nearest neighbor scale: both eyes (x=0 to 4000) mapped to 1000,
               * y=0 to 2040 mapped to 510. */
              /* Ratio: w = 4, h = 4 */
              for (int y = 0; y < 510; y++) {
                int src_y = y * 4;
                for (int x = 0; x < 1000; x++) {
                  int src_x = x * 4;
                  int dst_off = (y * 1000 + x) * 3;
                  int src_off = (src_y * 4000 + src_x) * 3;
                  payload[dst_off + 0] = fb[src_off + 0]; /* R */
                  payload[dst_off + 1] = fb[src_off + 1]; /* G */
                  payload[dst_off + 2] = fb[src_off + 2]; /* B */
                }
              }
              munmap(fb, 12000 * 2040);
              LOG("  Mirrorscope successfully cached 1000x510 static "
                  "downsample.");
            } else {
              free(mirror_buf);
              mirror_buf = NULL;
            }
          }
        }
      }

#endif
      ret = 0;
      int old_fl = fcntl(0, F_GETFL, 0);
      int stdin_ready = old_fl >= 0 && fcntl(0, F_SETFL, old_fl | O_NONBLOCK) == 0;
      if (!stdin_ready)
        ret = -1;

      char stopbuf[64];
      int stoplen = 0;
      int running = stdin_ready;

      while (running && !display_stop_requested && !application_stop_requested) {
        fd_set fds;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 33000; /* ~30 FPS */
        FD_ZERO(&fds);
        FD_SET(0, &fds);

        int s_ret = select(1, &fds, NULL, NULL, &tv);
        if (s_ret < 0 && errno != EINTR) {
          ERR("select stdin");
          ret = -1;
          break;
        }
        if (s_ret > 0) {
          char c;
          ssize_t bytes;
          while ((bytes = read(0, &c, 1)) == 1) {
            if (c == '\n') {
              stopbuf[stoplen] = '\0';
              if (strcmp(stopbuf, "stop") == 0)
                running = 0;
              stoplen = 0;
            } else if (stoplen < 63) {
              stopbuf[stoplen++] = c;
            }
          }
          if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EINTR))
            running = 0;
        }

#ifdef GPU_RENDER
        if (g_stream && g_stream_fd >= 0 && mirror_buf) {
          int sent = mirrorscope_write_packet(mirror_buf,
                         sizeof(struct vrh2_header) + mirror_size, NULL);
          if (sent < 0) {
            ERR("Mirrorscope static frame write");
            close(g_stream_fd);
            g_stream_fd = -1;
          } else if (sent > 0) {
            ((struct vrh2_header *)mirror_buf)->frame_no++;
          }
        }
#endif
      }

      if (old_fl >= 0)
        fcntl(0, F_SETFL, old_fl);

#ifdef GPU_RENDER
      if (mirror_buf)
        free(mirror_buf);
      if (g_stream_fd >= 0) {
        close(g_stream_fd);
        g_stream_fd = -1;
      }
#endif
      if (restore_interrupt)
        sigaction(SIGINT, &old_interrupt, NULL);
      if (restore_terminate)
        sigaction(SIGTERM, &old_terminate, NULL);
    }

    /* ═══════════════════════════════════════════════════════════
     * SAFE TEARDOWN — reverse of init order
     * 1. DCS Display OFF + Sleep IN (panel stops)
     * 2. DSI SW Mute (blank pixel stream)
     * 3. MUTEX Disable (stop frame sync — must be before component stops!)
     * 4. DSC Stop + Reset
     * 5. LHC Stop
     * 6. RDMA Stop (safe now that mutex isn't driving it)
     * 7. DSI Stop Output
     * 8. MMSYS Disconnect
     * 9. Component power off (reverse of power on)
     * 10. DSI Finit + DPRX Power Off
     * 11. ION cleanup
     * ═══════════════════════════════════════════════════════════ */


    int stop_ret = teardown_display(0);
    return ret < 0 ? ret : stop_ret;
  }
  return -1;
}

static int teardown_display(int stop_cooling) {
  int ret;

  LOG("");
  LOG("=== TEARDOWN: Shutting down display pipeline ===");

  audio_stop_hardware();

  /* 1. Display OFF (DCS 0x28) */
  LOG("  [1] Display OFF (0x28)...");
  {
    struct vm_cmd_ioctl vm = {0};
    vm.cmd = 0x28;
    vm.count = 0;
    ret = ioctl(fd_dsi, DSI_SEND_VM_COMMAND, &vm);
    LOG("    ret=%d", ret);
  }
  usleep(50000); /* 50ms for panel to process */

  /* 2. Sleep IN (DCS 0x10) */
  LOG("  [2] Sleep IN (0x10)...");
  {
    struct vm_cmd_ioctl vm = {0};
    vm.cmd = 0x10;
    vm.count = 0;
    ret = ioctl(fd_dsi, DSI_SEND_VM_COMMAND, &vm);
    LOG("    ret=%d", ret);
  }
  usleep(120000); /* 120ms for sleep entry */

  /* 3. DSI SW Mute (blank pixel stream) */
  LOG("  [3] DSI SW Mute...");
  {
    uint8_t mute_val = 1;
    ret = ioctl(fd_dsi, DSI_SET_SW_MUTE, &mute_val);
    LOG("    ret=%d", ret);
  }
  usleep(50000); /* 50ms for mute to take effect */

  /* 4. MUTEX Disable — MUST be before stopping components! */
  LOG("  [4] MUTEX Disable...");
  {
    struct {
      uint32_t idx;
      uint32_t enable;
    } men;
    men.idx = 8;
    men.enable = 0;
    ret = ioctl(fd_mutex, MUTEX_ENABLE, &men);
    LOG("    ret=%d", ret);
  }
  usleep(50000); /* 50ms for mutex to fully stop */

  /* 5. DSC Stop + Reset */
  LOG("  [5] DSC Stop + Reset...");
  if (fd_dscenc >= 0) {
    ret = ioctl(fd_dscenc, DSC_RESET);
    LOG("    DSC_RESET: ret=%d", ret);
  }

  /* 6. LHC Stop */
  LOG("  [6] LHC Stop...");
  if (fd_lhc >= 0) {
    ret = ioctl(fd_lhc, LHC_STOP);
    LOG("    ret=%d", ret);
  }

  /* 7. RDMA Stop (safe now — mutex stopped, no more frame requests) */
  LOG("  [7] RDMA Stop...");
  if (fd_rdma >= 0) {
    ret = ioctl(fd_rdma, RDMA_STOP);
    LOG("    ret=%d", ret);
  }

  /* 8. DSI Stop Output */
  LOG("  [8] DSI Stop Output...");
  ret = ioctl(fd_dsi, DSI_STOP_OUTPUT);
  LOG("    ret=%d", ret);

  /* 9. MMSYS Disconnect (reverse of connect) */
  LOG("  [9] MMSYS Disconnect...");
  if (fd_mmsys >= 0)
    set_scanout_routes(MMSYS_DISCONNECT_COMP);

  /* 10. Component cleanup — remove from mutex, power off (reverse of init) */
  LOG("  [10] Component cleanup...");

  if (fd_mutex >= 0) {
    /* We always attempt to power on the mutex to allow REMOVE_COMPONENT to succeed.
     * If it was already on, this just increment the refcount temporarily. */
    ret = ioctl(fd_mutex, MUTEX_POWER_ON, 8);
    int temporary_power = ret == 0;
    LOG("    MUTEX_POWER_ON (pre-cleanup): ret=%d", ret);

    remove_mutex_components(0);

    /* Shut it down once for ourselves (matching the pre-cleanup PowerOn) */
    if (temporary_power) {
      ret = ioctl(fd_mutex, MUTEX_POWER_OFF, 8);
      LOG("    MUTEX_POWER_OFF: ret=%d", ret);
    }

    /* If we were the one who originally powered it on in cmd_go, shut it down again */
    if (mutex_powered) {
        ret = ioctl(fd_mutex, MUTEX_POWER_OFF, 8);
        LOG("    MUTEX_POWER_OFF (balanced): ret=%d", ret);
    }

    ret = ioctl(fd_mutex, MUTEX_PUT_RESOURCE, 8);
    LOG("    MUTEX_PUT_RESOURCE: ret=%d", ret);
  }

  /* RDMA Power Down */
  if (fd_rdma >= 0) {
    /* Temporary PowerOn to satisfy driver context requirements for reset */
    int temporary_power = ioctl(fd_rdma, RDMA_POWER_ON, 0) == 0;

    ret = ioctl(fd_rdma, RDMA_LARB_PUT, 0);
    LOG("    RDMA_LARB_PUT: ret=%d", ret);

    if (temporary_power) {
      ret = ioctl(fd_rdma, RDMA_POWER_OFF);
      LOG("    RDMA_POWER_OFF: ret=%d", ret);
    }

    if (rdma_powered) {
        ret = ioctl(fd_rdma, RDMA_POWER_OFF);
        LOG("    RDMA_POWER_OFF (balanced): ret=%d", ret);
    }
  }

  /* LHC Power Down */
  if (fd_lhc >= 0) {
    int temporary_power = ioctl(fd_lhc, LHC_POWER_ON) == 0;
    if (temporary_power) {
      ret = ioctl(fd_lhc, LHC_POWER_OFF);
      LOG("    LHC_POWER_OFF: ret=%d", ret);
    }
    if (lhc_powered) {
        ret = ioctl(fd_lhc, LHC_POWER_OFF);
        LOG("    LHC_POWER_OFF (balanced): ret=%d", ret);
    }
  }

  if (fd_dscenc >= 0) {
     ret = ioctl(fd_dscenc, DSC_RESET);
  }


  /* 11. DSI Finit */
  LOG("  [11] DSI Finit...");
  ret = ioctl(fd_dsi, DSI_FINIT);
  LOG("    ret=%d", ret);

  /* 12. DPRX Power Off */
  LOG("  [12] DPRX Power Off...");
  {
    if (fd_dprx >= 0) {
      ret = ioctl(fd_dprx, DPRX_POWER_OFF);
      LOG("    ret=%d", ret);
    } else {
      LOG("    *** Cannot access fd_dprx to power off");
    }
  }

  /* 13. Close ION buffer */
  LOG("  [13] Close ION buffer...");
  release_framebuffer();
  g_hardware_powered_on = 0;
  mutex_powered = rdma_powered = lhc_powered = 0;

  if (stop_cooling && fan_ctrl_teardown() < 0) return -1;
  LOG("=== TEARDOWN COMPLETE ===");
  return 0;
}

/* An explicit stop also releases cooling; display return keeps the daemon. */
int cmd_stop(void) {
  return teardown_display(1);
}
