/* Stop publishing misc devices before releasing their endpoint requests. */
static void s3_fast_stream_cleanup(void) {
  int i;

  fast_ready = false;
  if (fast_registered) {
    misc_deregister(&fast_misc);
    fast_registered = false;
  }

  mutex_lock(&fast_write_mutex);
  if (fast_ep) {
    for (i = 0; i < FAST_NBUFS; i++) {
      if (!fast_reqs[i])
        continue;
      usb_ep_dequeue(fast_ep, fast_reqs[i]);
      kfree(fast_reqs[i]->buf);
      fast_reqs[i]->buf = NULL;
      usb_ep_free_request(fast_ep, fast_reqs[i]);
      fast_reqs[i] = NULL;
    }
  }
  fast_ep = NULL;
  mutex_unlock(&fast_write_mutex);
}

static void s3_fast_input_cleanup(void) {
  int i;
  struct usb_ep *ep;

  /* The caller stopped the producer worker before this reclamation path. */
  mutex_lock(&fast_in_io_mutex);
  fast_in_stopping = true;
  ep = fast_in_ep;
  smp_store_release(&fast_in_ready, false);
  if (fast_in_wait_initialized)
    wake_up_interruptible(&fast_in_wait);
  if (fast_in_registered) {
    misc_deregister(&fast_in_misc);
    fast_in_registered = false;
  }
  /* Existing open descriptors can still call read after deregistration. */
  spin_lock_irq(&fast_in_lock);
  fast_in_bridge_mode = true;
  spin_unlock_irq(&fast_in_lock);

  if (ep) {
    for (i = 0; i < FAST_INPUT_NBUFS; i++) {
      if (!fast_in_reqs[i])
        continue;
      usb_ep_dequeue(ep, fast_in_reqs[i]);
      fast_in_reqs[i]->buf = NULL;
      usb_ep_free_request(ep, fast_in_reqs[i]);
      fast_in_reqs[i] = NULL;
    }
  }
  for (i = 0; i < FAST_INPUT_NBUFS; ++i) {
    kfree(fast_in_buffers[i]);
    fast_in_buffers[i] = NULL;
    fast_in_lengths[i] = 0;
  }
  spin_lock_irq(&fast_in_lock);
  fast_in_ep = NULL;
  fast_in_bridge_mode = false;
  fast_in_head = 0;
  fast_in_tail = 0;
  fast_in_wait_initialized = false;
  memset(fast_in_done, 0, sizeof(fast_in_done));
  spin_unlock_irq(&fast_in_lock);
  mutex_unlock(&fast_in_io_mutex);
}
