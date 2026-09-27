#ifndef PSVR2_STAGE1_H
#define PSVR2_STAGE1_H

#include "psvr2/runtime.h"

bool psvr2_upload_file(psvr2_runtime *runtime, const char *local_path,
                       const char *remote_name);
bool psvr2_parse_stage1_mailbox(const char *status,
                                psvr2_stage1_mailbox *box);
bool psvr2_stage1_discover(psvr2_runtime *runtime);
bool psvr2_stage1_read_status(psvr2_runtime *runtime,
                              char *output, size_t output_size);
bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command,
                       double timeout, int64_t *retval, psvr2_buffer *output);
bool psvr2_stage1_exec_blind(psvr2_runtime *runtime, const char *command);
bool psvr2_load_stage1(psvr2_runtime *runtime, const char *ko_path,
                       bool force_tmp, bool force);

#endif
