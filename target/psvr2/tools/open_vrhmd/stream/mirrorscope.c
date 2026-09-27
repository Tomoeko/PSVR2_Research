#include "../open_vrhmd.h"

#ifdef GPU_RENDER

/* Return 1 for a complete packet, 0 for a frame skipped before writing, and -1
 * for a failed packet. A caller must stop using the stream after -1: appending
 * another header to a partial payload would corrupt the byte-stream framing. */
int mirrorscope_write_packet(const void *data, size_t size,
                             const _Atomic bool *running) {
  if (g_stream_fd < 0) {
    errno = EBADF;
    return -1;
  }
  const uint8_t *bytes = data;
  size_t written = 0;
  struct timespec last_progress, now;
  clock_gettime(CLOCK_MONOTONIC, &last_progress);
  while (written < size) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    double idle = (now.tv_sec - last_progress.tv_sec) +
                  (now.tv_nsec - last_progress.tv_nsec) * 1e-9;
    if (idle >= 0.1) {
      if (!written)
        return 0;
      errno = ETIMEDOUT;
      return -1;
    }
    if (running && !atomic_load(running)) {
      errno = ECANCELED;
      return -1;
    }
    struct pollfd pfd = {.fd = g_stream_fd, .events = POLLOUT};
    int ready = poll(&pfd, 1, 100);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (!ready) {
      if (!written)
        return 0;
      errno = ETIMEDOUT;
      return -1;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      errno = EIO;
      return -1;
    }
    if (!(pfd.revents & POLLOUT))
      continue;
    ssize_t count = write(g_stream_fd, bytes + written, size - written);
    if (count > 0) {
      written += (size_t)count;
      clock_gettime(CLOCK_MONOTONIC, &last_progress);
    } else if (count < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK)) {
      continue;
    } else {
      if (!count)
        errno = EIO;
      return -1;
    }
  }
  return 1;
}

void *mirrorscope_stream_thread(void *arg) {
  (void)arg;
  LOG("Mirrorscope streaming thread started.");
  while (g_mirror_running) {
    int ready;
    do {
      ready = sem_wait(&g_sem_full);
    } while (ready < 0 && errno == EINTR && g_mirror_running);
    if (ready < 0 || !g_mirror_running)
      break;

    int idx = g_pbo_read_idx;
    struct vrh2_header *header = (struct vrh2_header *)g_vhdr_packets[idx];
    if (header && g_stream_fd >= 0 &&
        mirrorscope_write_packet(header, sizeof(*header) + header->size,
                                 &g_mirror_running) < 0) {
      if (g_mirror_running)
        ERR("Mirrorscope stream failed; capture stopped");
      g_mirror_running = false;
    }
    g_pbo_read_idx = (idx + 1) % PBO_COUNT;
    sem_post(&g_sem_free);
  }
  g_mirror_running = false;
  LOG("Mirrorscope streaming thread exiting.");
  return NULL;
}

#endif /* GPU_RENDER */
