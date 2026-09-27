/* Workers clear their published pointer under worker_lock when exiting.
 * Pin a snapshot before unlocking: a naturally exiting worker can otherwise
 * release its task before kthread_stop obtains its own reference. */
static void s3_stop_worker(struct task_struct **slot) {
  struct task_struct *thread;

  mutex_lock(&worker_lock);
  thread = *slot;
  if (thread)
    get_task_struct(thread);
  *slot = NULL;
  mutex_unlock(&worker_lock);

  if (thread) {
    kthread_stop(thread);
    put_task_struct(thread);
  }
}
