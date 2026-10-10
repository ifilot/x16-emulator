// X16 AgentBridge native process API.
#ifndef AGENT_H
#define AGENT_H
#include <stdbool.h>
#include <stdint.h>
extern bool agent_mode;
bool agent_open(void);
void agent_close(void);
bool agent_before_step(uint32_t *old_clock);
void agent_after_step(bool frame, bool instruction);
void agent_stop(const char *reason);
bool agent_render_video(void);
#endif
