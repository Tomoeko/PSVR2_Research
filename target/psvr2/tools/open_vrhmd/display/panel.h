#ifndef OPEN_VRHMD_PANEL_H
#define OPEN_VRHMD_PANEL_H

enum panel_command_stage {
  PANEL_COMMAND_INIT,
  PANEL_COMMAND_AFTER_READ,
  PANEL_COMMAND_AFTER_START,
  PANEL_COMMAND_AFTER_MUTEX
};

int panel_send_commands(enum panel_command_stage stage);

#endif
