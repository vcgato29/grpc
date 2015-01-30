/*
 *
 * Copyright 2014, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/grpc.h>

#include <stdio.h>
#include <string.h>

#include <grpc/support/cmdline.h>
#include <grpc/support/histogram.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "test/core/util/grpc_profiler.h"
#include "test/core/util/test_config.h"

static gpr_histogram *histogram;
static grpc_byte_buffer *the_buffer;
static grpc_channel *channel;
static grpc_completion_queue *cq;
static grpc_call *call;

static void init_ping_pong_request(void) {}

static void step_ping_pong_request(void) {
  call = grpc_channel_create_call_old(channel, "/Reflector/reflectUnary",
                                  "localhost", gpr_inf_future);
  GPR_ASSERT(grpc_call_invoke(call, cq, (void *)1, (void *)1,
                              GRPC_WRITE_BUFFER_HINT) == GRPC_CALL_OK);
  GPR_ASSERT(grpc_call_start_write(call, the_buffer, (void *)1,
                                   GRPC_WRITE_BUFFER_HINT) == GRPC_CALL_OK);
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
  GPR_ASSERT(grpc_call_start_read(call, (void *)1) == GRPC_CALL_OK);
  GPR_ASSERT(grpc_call_writes_done(call, (void *)1) == GRPC_CALL_OK);
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
  grpc_call_destroy(call);
  call = NULL;
}

static void init_ping_pong_stream(void) {
  call = grpc_channel_create_call_old(channel, "/Reflector/reflectStream",
                                  "localhost", gpr_inf_future);
  GPR_ASSERT(grpc_call_invoke(call, cq, (void *)1, (void *)1, 0) ==
             GRPC_CALL_OK);
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
}

static void step_ping_pong_stream(void) {
  GPR_ASSERT(grpc_call_start_write(call, the_buffer, (void *)1, 0) ==
             GRPC_CALL_OK);
  GPR_ASSERT(grpc_call_start_read(call, (void *)1) == GRPC_CALL_OK);
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
  grpc_event_finish(grpc_completion_queue_next(cq, gpr_inf_future));
}

static double now(void) {
  gpr_timespec tv = gpr_now();
  return 1e9 * tv.tv_sec + tv.tv_nsec;
}

typedef struct {
  const char *name;
  void (*init)();
  void (*do_one_step)();
} scenario;

static const scenario scenarios[] = {
    {"ping-pong-request", init_ping_pong_request, step_ping_pong_request},
    {"ping-pong-stream", init_ping_pong_stream, step_ping_pong_stream}, };

int main(int argc, char **argv) {
  gpr_slice slice = gpr_slice_from_copied_string("x");
  double start, stop;
  int i;

  char *fake_argv[1];

  int payload_size = 1;
  int done;
  int secure = 0;
  char *target = "localhost:443";
  gpr_cmdline *cl;
  char *scenario_name = "ping-pong-request";
  scenario sc = {NULL};

  GPR_ASSERT(argc >= 1);
  fake_argv[0] = argv[0];
  grpc_test_init(1, fake_argv);

  grpc_init();

  cl = gpr_cmdline_create("fling client");
  gpr_cmdline_add_int(cl, "payload_size", "Size of the payload to send",
                      &payload_size);
  gpr_cmdline_add_string(cl, "target", "Target host:port", &target);
  gpr_cmdline_add_flag(cl, "secure", "Run with security?", &secure);
  gpr_cmdline_add_string(cl, "scenario", "Scenario", &scenario_name);
  gpr_cmdline_parse(cl, argc, argv);
  gpr_cmdline_destroy(cl);

  for (i = 0; i < GPR_ARRAY_SIZE(scenarios); i++) {
    if (0 == strcmp(scenarios[i].name, scenario_name)) {
      sc = scenarios[i];
    }
  }
  if (!sc.name) {
    fprintf(stderr, "unsupported scenario '%s'. Valid are:", scenario_name);
    for (i = 0; i < GPR_ARRAY_SIZE(scenarios); i++) {
      fprintf(stderr, " %s", scenarios[i].name);
    }
    return 1;
  }

  channel = grpc_channel_create(target, NULL);
  cq = grpc_completion_queue_create();
  the_buffer = grpc_byte_buffer_create(&slice, payload_size);
  histogram = gpr_histogram_create(0.01, 60e9);
  sc.init();

  for (i = 0; i < 1000; i++) {
    sc.do_one_step();
  }

  gpr_log(GPR_INFO, "start profiling");
  grpc_profiler_start("client.prof");
  for (i = 0; i < 100000; i++) {
    start = now();
    sc.do_one_step();
    stop = now();
    gpr_histogram_add(histogram, stop - start);
  }
  grpc_profiler_stop();

  if (call) {
    grpc_call_destroy(call);
  }

  grpc_channel_destroy(channel);
  grpc_completion_queue_shutdown(cq);
  done = 0;
  while (!done) {
    grpc_event *ev = grpc_completion_queue_next(cq, gpr_inf_future);
    done = (ev->type == GRPC_QUEUE_SHUTDOWN);
    grpc_event_finish(ev);
  }
  grpc_completion_queue_destroy(cq);
  grpc_byte_buffer_destroy(the_buffer);
  gpr_slice_unref(slice);

  gpr_log(GPR_INFO, "latency (50/95/99/99.9): %f/%f/%f/%f",
          gpr_histogram_percentile(histogram, 50),
          gpr_histogram_percentile(histogram, 95),
          gpr_histogram_percentile(histogram, 99),
          gpr_histogram_percentile(histogram, 99.9));
  gpr_histogram_destroy(histogram);

  grpc_shutdown();

  return 0;
}
