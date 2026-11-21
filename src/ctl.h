/* Control channel: the camera's settings and the picture controls, for the OBS script or any client.
   127.0.0.1 only, one text command per connection, one reply:
     list                 -> {"make":"canon","connected":true,"controls":[{...}, ...]}
     set NAME VALUE       -> ok | error: why
   Control types: range (min/max/step), bool, menu (options [[value,"label"],...]), button (set NAME 1),
   step (set focus -3..3, 0 excluded), info (read-only text). */
#pragma once
#include "image.h"
#include "ptp.h"

#define CTL_PORT_BASE 47810 /* + slot: 0 any make, 1 Canon, 2 Sony */

int ctl_start(int port);                   /* serves on 127.0.0.1:port in a background thread; 0 = ok */
int ctl_client(int port, const char *cmd); /* --control: sends one command, prints the reply; exit status */
void ctl_set_view(const view_t *v);        /* picture controls changed elsewhere (startup, an app's settings) */
int ctl_take_view(view_t *v);              /* 1 when a client changed them; copies only the adjustable fields */
void ctl_camera(ptp_t *cam);               /* capture thread, between frames: publish props, run queued commands */
char *ctl_reply(const char *line);         /* the reply to one command line (malloc'd); what the server sends */
