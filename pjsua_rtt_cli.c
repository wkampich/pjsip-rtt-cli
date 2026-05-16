/*
 * Copyright (C) 2026 Wolfgang Kampichler.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <fcntl.h>
#include <ncurses.h>
#include <pjsua-lib/pjsua.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SIP_URI 128
#define BUF_SIZE 512

/* Global Variables */
WINDOW *win_hist;
WINDOW *win_log;
WINDOW *win_remote;
WINDOW *win_local;
pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile int ui_ready = 0;
pjsua_call_id current_call = PJSUA_INVALID_ID;
pjsua_player_id audio_player_id = PJSUA_INVALID_ID;

/* CLI Arguments */
int local_port = 5060;
char dest_uri[MAX_SIP_URI] = {0};
char public_ip[MAX_SIP_URI] = {0};
char local_user[MAX_SIP_URI] = {0};
char playback_file[MAX_SIP_URI] = {0};
int use_cr_only = 0;
int use_nl_only = 0;

/* Typing Buffers */
char remote_buf[BUF_SIZE] = {0};
int remote_idx = 0;
char local_buf[BUF_SIZE] = {0};
int local_idx = 0;

/* UI Helpers */
static void draw_borders() {
  box(win_remote, 0, 0);
  mvwprintw(win_remote, 0, 2, " Remote is typing... ");

  box(win_local, 0, 0);
  mvwprintw(win_local, 0, 2, " You (Press ESC to quit) ");
}

static void print_hist(const char *fmt, ...) {
  if (!ui_ready)
    return;
  va_list args;
  va_start(args, fmt);
  pthread_mutex_lock(&ui_mutex);
  vw_printw(win_hist, fmt, args);
  wrefresh(win_hist);
  pthread_mutex_unlock(&ui_mutex);
  va_end(args);
}

/* PJSIP Log Callback */
static void custom_log_cb(int level, const char *data, int len) {
  /* Fallback to standard output if the UI isn't loaded yet */
  if (!ui_ready) {
    printf("%.*s", len, data);
    fflush(stdout);
    return;
  }
  pthread_mutex_lock(&ui_mutex);
  wprintw(win_log, "%.*s", len, data);
  wrefresh(win_log);
  pthread_mutex_unlock(&ui_mutex);
}

/* RTT Transmitter */
static void send_rtt_chars(const char *str, int len) {
  if (current_call == PJSUA_INVALID_ID)
    return;
  pjsua_call_send_text_param param;
  pjsua_call_send_text_param_default(&param);

  pj_str_t pj_text;
  pj_text.ptr = (char *)str;
  pj_text.slen = len;

  param.text = pj_text;

  pj_status_t status = pjsua_call_send_text(current_call, &param);
  if (status != PJ_SUCCESS && ui_ready) {
    pthread_mutex_lock(&ui_mutex);
    wprintw(win_log, "[ERROR] Failed to send RTT text! Code: %d\n", status);
    wrefresh(win_log);
    pthread_mutex_unlock(&ui_mutex);
  }
}

/* SDP Sanitizer */
static void on_call_sdp_created(pjsua_call_id call_id, pjmedia_sdp_session *sdp,
                                pj_pool_t *pool,
                                const pjmedia_sdp_session *rem_sdp) {
  PJ_UNUSED_ARG(call_id);
  PJ_UNUSED_ARG(pool);
  PJ_UNUSED_ARG(rem_sdp);

  int text_stream_count = 0;

  for (unsigned i = 0; i < sdp->media_count; ++i) {
    pjmedia_sdp_media *m = sdp->media[i];

    if (pj_stricmp2(&m->desc.media, "text") == 0) {
      text_stream_count++;

      if (text_stream_count > 1) {
        if (ui_ready) {
          print_hist("\n[SDP FIX] Duplicate m=text detected. Forcing port to 0 "
                     "to prevent RED negotiation failure.\n");
        }
        m->desc.port = 0;
      }
    }
  }
}

/* PJSIP Callbacks */
static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
                             pjsip_rx_data *rdata) {
  PJ_UNUSED_ARG(acc_id);
  PJ_UNUSED_ARG(rdata);
  print_hist("\n[UAS] Incoming call... Auto-answering...\n");
  pjsua_call_setting opt;
  pjsua_call_setting_default(&opt);
  opt.aud_cnt = 1;
  opt.txt_cnt = 1;
  pjsua_call_answer2(call_id, &opt, 200, NULL, NULL);
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
  pjsua_call_info ci;
  PJ_UNUSED_ARG(e);
  pjsua_call_get_info(call_id, &ci);

  if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
    current_call = call_id;
    print_hist("\n[SIP] Call %d connected via TCP. RTT is active.\n", call_id);
  } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
    current_call = PJSUA_INVALID_ID;
    print_hist("\n[SIP] Call %d disconnected.\n", call_id);
  }
}

/* Triggered when RTP (Audio) channels are established */
static void on_call_media_state(pjsua_call_id call_id) {
  pjsua_call_info ci;
  pjsua_call_get_info(call_id, &ci);

  if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
    /* If the user loaded an audio file, route it to the call */
    if (audio_player_id != PJSUA_INVALID_ID) {
      pjsua_conf_connect(pjsua_player_get_conf_port(audio_player_id),
                         ci.conf_slot);
      print_hist("\n[MEDIA] Looping WAV file routed to call audio!\n");
    }
  }
}

static void on_call_rx_text(pjsua_call_id call_id,
                            const pjsua_txt_stream_data *data) {
  PJ_UNUSED_ARG(call_id);
  if (!data || data->text.slen == 0 || !ui_ready)
    return;

  pthread_mutex_lock(&ui_mutex);
  for (int i = 0; i < data->text.slen; i++) {
    char c = data->text.ptr[i];

    if (c == '\r' || c == '\n') {
      if (remote_idx > 0) {
        wprintw(win_hist, "Remote: %s\n", remote_buf);
        memset(remote_buf, 0, sizeof(remote_buf));
        remote_idx = 0;
      }
    } else if (c == '\b' || c == 127 || c == 8) {
      if (remote_idx > 0) {
        do {
          remote_idx--;
          char deleted_byte = remote_buf[remote_idx];
          remote_buf[remote_idx] = '\0';
          if ((deleted_byte & 0xC0) != 0x80) {
            break;
          }
        } while (remote_idx > 0);
      }
    } else {
      if (remote_idx < BUF_SIZE - 2) {
        remote_buf[remote_idx++] = c;
        remote_buf[remote_idx] = '\0';
      }
    }
  }

  werase(win_remote);
  draw_borders();
  mvwprintw(win_remote, 1, 1, "%s", remote_buf);
  wrefresh(win_hist);
  wrefresh(win_remote);
  pthread_mutex_unlock(&ui_mutex);
}

/* Main Application */
int main(int argc, char *argv[]) {
  pj_status_t status;

  /* Parse CLI Arguments */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      local_port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--call") == 0 && i + 1 < argc) {
      strncpy(dest_uri, argv[++i], MAX_SIP_URI - 1);
    } else if (strcmp(argv[i], "--public-ip") == 0 && i + 1 < argc) {
      strncpy(public_ip, argv[++i], MAX_SIP_URI - 1);
    } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
      strncpy(local_user, argv[++i], MAX_SIP_URI - 1);
    } else if (strcmp(argv[i], "--playback") == 0 && i + 1 < argc) {
      strncpy(playback_file, argv[++i], MAX_SIP_URI - 1);
    } else if (strcmp(argv[i], "--r") == 0) {
      use_cr_only = 1;
    } else if (strcmp(argv[i], "--n") == 0) {
      use_nl_only = 1;
    }
  }

  /* Initialize PJSIP */
  status = pjsua_create();
  if (status != PJ_SUCCESS)
    return 1;

  pjsua_config cfg;
  pjsua_logging_config log_cfg;
  pjsua_media_config media_cfg;
  pjsua_transport_config rtp_cfg;

  pjsua_config_default(&cfg);
  cfg.cb.on_incoming_call = &on_incoming_call;
  cfg.cb.on_call_state = &on_call_state;
  cfg.cb.on_call_media_state = &on_call_media_state;
  cfg.cb.on_call_rx_text = &on_call_rx_text;
  cfg.cb.on_call_sdp_created = &on_call_sdp_created;

  pjsua_logging_config_default(&log_cfg);
  log_cfg.console_level = 0;
  log_cfg.level = 4;
  log_cfg.cb = &custom_log_cb;

  pjsua_media_config_default(&media_cfg);

  status = pjsua_init(&cfg, &log_cfg, &media_cfg);
  if (status != PJ_SUCCESS)
    return 1;

  /* Setup TCP Transport */
  pjsua_transport_config trans_cfg;
  pjsua_transport_config_default(&trans_cfg);
  pjsua_transport_config_default(&rtp_cfg);
  trans_cfg.port = local_port;

  if (strlen(public_ip) > 0) {
    trans_cfg.public_addr = pj_str(public_ip);
    trans_cfg.bound_addr = pj_str(public_ip);
    rtp_cfg.bound_addr = pj_str(public_ip);
  }

  pjsua_transport_id tp_id;
  status = pjsua_transport_create(PJSIP_TRANSPORT_TCP, &trans_cfg, &tp_id);
  if (status != PJ_SUCCESS)
    return 1;

  /* Setup Local Account */
  pjsua_acc_config acc_cfg;
  pjsua_acc_config_default(&acc_cfg);
  acc_cfg.transport_id = tp_id;

  char id_uri[256];
  char *ip_to_use = strlen(public_ip) > 0 ? public_ip : "127.0.0.1";

  if (strlen(local_user) > 0) {
    snprintf(id_uri, sizeof(id_uri), "\"%s\" <sip:%s@%s:%d;transport=tcp>",
             local_user, local_user, ip_to_use, local_port);
  } else {
    snprintf(id_uri, sizeof(id_uri), "<sip:%s:%d;transport=tcp>", ip_to_use,
             local_port);
  }

  acc_cfg.id = pj_str(id_uri);
  acc_cfg.rtp_cfg = rtp_cfg;
  acc_cfg.txt_red_level = 2;

  pjsua_acc_id acc_id;
  status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &acc_id);
  if (status != PJ_SUCCESS)
    return 1;

  /* Initialize Ncurses */
  initscr();
  cbreak();
  noecho();
  int lines, cols;
  getmaxyx(stdscr, lines, cols);

  int dynamic_space = lines - 6;
  int hist_lines = dynamic_space / 2;
  int log_lines = dynamic_space - hist_lines;

  win_hist = newwin(hist_lines, cols, 0, 0);
  win_log = newwin(log_lines, cols, hist_lines, 0);
  win_remote = newwin(3, cols, dynamic_space, 0);
  win_local = newwin(3, cols, dynamic_space + 3, 0);

  keypad(win_local, TRUE);
  wtimeout(win_local, 100);

  scrollok(win_hist, TRUE);
  scrollok(win_log, TRUE);

  wattron(win_log, A_DIM);
  wprintw(win_log, "--- SYSTEM LOGS INITIALIZED ---\n");
  wprintw(win_hist, "--- CHAT HISTORY ---\n");
  draw_borders();

  wrefresh(win_hist);
  wrefresh(win_log);
  wrefresh(win_remote);
  wrefresh(win_local);

  ui_ready = 1;

  /* Audio Device Setup (null sound device) */
  status = pjsua_set_null_snd_dev();
  if (status != PJ_SUCCESS) {
    ui_ready = 0;
    endwin();
    return 1;
  }

  /* Start PJSUA */
  status = pjsua_start();
  if (status != PJ_SUCCESS) {
    ui_ready = 0;
    endwin();
    return 1;
  }

  /* Safely load the Audio Player into the newly activated media bridge */
  if (strlen(playback_file) > 0) {
    pj_str_t file_path = pj_str(playback_file);
    /* loop indefinitely */
    status = pjsua_player_create(&file_path, 0, &audio_player_id);
    if (status != PJ_SUCCESS) {
      ui_ready = 0;
      endwin();
      printf("[FATAL] Could not load audio file. Ensure it is a valid 16-bit "
             "PCM WAV.\n");
      return 1;
    }
    print_hist("[SYSTEM] Audio file loaded successfully: %s\n", playback_file);
  }

  /* Calling Logic */
  if (strlen(dest_uri) > 0) {
    char safe_uri[256];
    char temp_uri[256];
    strncpy(temp_uri, dest_uri, sizeof(temp_uri) - 1);

    if (temp_uri[0] == '<') {
      memmove(temp_uri, temp_uri + 1, strlen(temp_uri));
      if (temp_uri[strlen(temp_uri) - 1] == '>')
        temp_uri[strlen(temp_uri) - 1] = '\0';
    }

    if (strncmp(temp_uri, "sip:", 4) != 0)
      snprintf(safe_uri, sizeof(safe_uri), "sip:%s", temp_uri);
    else
      strncpy(safe_uri, temp_uri, sizeof(safe_uri));

    if (strstr(safe_uri, "transport=tcp") == NULL) {
      strncat(safe_uri, ";transport=tcp",
              sizeof(safe_uri) - strlen(safe_uri) - 1);
    }

    print_hist("[UAC] Calling %s over TCP...\n", safe_uri);

    pj_str_t uri = pj_str(safe_uri);
    pjsua_call_setting opt;
    pjsua_call_setting_default(&opt);
    opt.aud_cnt = 1;
    opt.txt_cnt = 1;

    pjsua_call_make_call(acc_id, &uri, &opt, NULL, NULL, NULL);
  } else {
    print_hist("[UAS] Listening on TCP port %d...\n", local_port);
  }

  /* Input Loop */
  int ch;
  while ((ch = wgetch(win_local)) != 27) {
    if (ch == ERR) {
      pthread_mutex_lock(&ui_mutex);
      wrefresh(win_hist);
      wrefresh(win_log);
      wrefresh(win_remote);
      wrefresh(win_local);
      pthread_mutex_unlock(&ui_mutex);
      continue;
    }

    /* Handle Resizing */
    if (ch == KEY_RESIZE) {
      pthread_mutex_lock(&ui_mutex);
      int new_lines, new_cols;
      getmaxyx(stdscr, new_lines, new_cols);

      if (new_lines >= 15 && new_cols >= 20) {
        int d_space = new_lines - 6;
        int h_lines = d_space / 2;
        int l_lines = d_space - h_lines;

        wresize(win_hist, h_lines, new_cols);
        wresize(win_log, l_lines, new_cols);
        mvwin(win_log, h_lines, 0);
        wresize(win_remote, 3, new_cols);
        mvwin(win_remote, d_space, 0);
        wresize(win_local, 3, new_cols);
        mvwin(win_local, d_space + 3, 0);

        werase(stdscr);
        refresh();
        werase(win_remote);
        werase(win_local);
        draw_borders();
        mvwprintw(win_remote, 1, 1, "%s", remote_buf);
        mvwprintw(win_local, 1, 1, "%s", local_buf);

        touchwin(win_hist);
        touchwin(win_log);
        wrefresh(win_hist);
        wrefresh(win_log);
        wrefresh(win_remote);
        wrefresh(win_local);
      }
      pthread_mutex_unlock(&ui_mutex);
      continue;
    }

    /* Handle Typing */
    pthread_mutex_lock(&ui_mutex);
    if (ch == '\n' || ch == '\r') {
      if (local_idx > 0) {
        char crlf[2] = {'\r', '\n'};
        if (use_cr_only) {
          char cr = '\r';
          send_rtt_chars(&cr, 1);
        } else if (use_nl_only) {
          char nl = '\n';
          send_rtt_chars(&nl, 1);
        } else if ((use_cr_only) && (use_nl_only)) {
          send_rtt_chars(crlf, 2);
        } else {
          send_rtt_chars(crlf, 2);
        }
        wprintw(win_hist, "Me: %s\n", local_buf);
        memset(local_buf, 0, sizeof(local_buf));
        local_idx = 0;
      }
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
      if (local_idx > 0) {
        local_buf[--local_idx] = '\0';
        char bs = '\b';
        send_rtt_chars(&bs, 1);
      }
    } else if (ch >= 32 && ch <= 126) {
      if (local_idx < BUF_SIZE - 2) {
        local_buf[local_idx++] = (char)ch;
        local_buf[local_idx] = '\0';
        char c = (char)ch;
        send_rtt_chars(&c, 1);
      }
    }

    werase(win_local);
    draw_borders();
    mvwprintw(win_local, 1, 1, "%s", local_buf);
    wrefresh(win_local);
    pthread_mutex_unlock(&ui_mutex);
  }

  /* Shutdown Sequence */
  ui_ready = 0;

  if (audio_player_id != PJSUA_INVALID_ID) {
    pjsua_player_destroy(audio_player_id);
  }

  pjsua_destroy();
  endwin();

  return 0;
}
