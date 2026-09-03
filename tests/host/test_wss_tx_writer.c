#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wss_tx_writer.h"

#define FAKE_WANT_READ  (-0x6900)
#define FAKE_WANT_WRITE (-0x6880)
#define FAKE_EAGAIN     11
#define MAX_STEPS       256

typedef struct {
    int result;
    int system_error;
    int64_t elapsed_us;
} fake_step_t;

typedef struct {
    const uint8_t *base;
    fake_step_t steps[MAX_STEPS];
    size_t step_count;
    size_t step_index;
    bool repeat_last;
    int64_t now_us;
    int64_t wait_us;
    size_t call_offsets[MAX_STEPS];
    size_t call_lengths[MAX_STEPS];
    size_t call_count;
} fake_io_t;

static int fake_write(void *ctx, const uint8_t *data, size_t len,
                      int *system_error)
{
    fake_io_t *io = ctx;
    assert(io->call_count < MAX_STEPS);
    io->call_offsets[io->call_count] = (size_t)(data - io->base);
    io->call_lengths[io->call_count] = len;
    io->call_count++;

    size_t index = io->step_index;
    assert(index < io->step_count);
    if (io->step_index + 1U < io->step_count || !io->repeat_last) {
        io->step_index++;
    }
    fake_step_t step = io->steps[index];
    io->now_us += step.elapsed_us;
    *system_error = step.system_error;
    return step.result;
}

static int64_t fake_now_us(void *ctx)
{
    return ((fake_io_t *)ctx)->now_us;
}

static void fake_wait_once(void *ctx)
{
    fake_io_t *io = ctx;
    io->now_us += io->wait_us;
}

static bool fake_is_transient(void *ctx, int result, int system_error)
{
    (void)ctx;
    return result == FAKE_WANT_READ || result == FAKE_WANT_WRITE ||
           (result == -1 && system_error == FAKE_EAGAIN);
}

static wss_tx_writer_ops_t fake_ops(fake_io_t *io)
{
    const wss_tx_writer_ops_t ops = {
        .ctx = io,
        .write = fake_write,
        .now_us = fake_now_us,
        .wait_once = fake_wait_once,
        .is_transient = fake_is_transient,
    };
    return ops;
}

static void init_io(fake_io_t *io, const uint8_t *data)
{
    memset(io, 0, sizeof(*io));
    io->base = data;
    io->wait_us = 10000;
}

static void test_single_and_partial_write(void)
{
    uint8_t data[100] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.steps[0].result = 100;
    io.step_count = 1;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;
    assert(wss_tx_write_all(&ops, data, sizeof(data), 2000000, &stats) ==
           WSS_TX_WRITE_OK);
    assert(stats.bytes_sent == sizeof(data));
    assert(stats.transient_retries == 0);
    assert(io.call_count == 1 && io.call_offsets[0] == 0 &&
           io.call_lengths[0] == sizeof(data));

    init_io(&io, data);
    io.steps[0].result = 40;
    io.steps[1].result = 60;
    io.step_count = 2;
    ops = fake_ops(&io);
    assert(wss_tx_write_all(&ops, data, sizeof(data), 2000000, &stats) ==
           WSS_TX_WRITE_OK);
    assert(io.call_count == 2);
    assert(io.call_offsets[0] == 0 && io.call_lengths[0] == 100);
    assert(io.call_offsets[1] == 40 && io.call_lengths[1] == 60);
}

static void test_want_states_retry_identical_range(void)
{
    uint8_t data[100] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.steps[0].result = FAKE_WANT_WRITE;
    io.steps[1].result = 40;
    io.steps[2].result = FAKE_WANT_READ;
    io.steps[3].result = 60;
    io.step_count = 4;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;

    assert(wss_tx_write_all(&ops, data, sizeof(data), 2000000, &stats) ==
           WSS_TX_WRITE_OK);
    assert(stats.transient_retries == 2);
    assert(stats.last_transient_result == FAKE_WANT_READ);
    assert(io.call_count == 4);
    assert(io.call_offsets[0] == 0 && io.call_lengths[0] == 100);
    assert(io.call_offsets[1] == 0 && io.call_lengths[1] == 100);
    assert(io.call_offsets[2] == 40 && io.call_lengths[2] == 60);
    assert(io.call_offsets[3] == 40 && io.call_lengths[3] == 60);
}

static void test_errno_eagain_is_retryable(void)
{
    uint8_t data[16] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.steps[0] = (fake_step_t){.result = -1, .system_error = FAKE_EAGAIN};
    io.steps[1].result = 16;
    io.step_count = 2;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;

    assert(wss_tx_write_all(&ops, data, sizeof(data), 2000000, &stats) ==
           WSS_TX_WRITE_OK);
    assert(stats.transient_retries == 1);
    assert(stats.last_transient_result == -1);
    assert(stats.last_transient_system_error == FAKE_EAGAIN);
    assert(io.call_offsets[0] == io.call_offsets[1]);
    assert(io.call_lengths[0] == io.call_lengths[1]);
}

static void test_absolute_deadline_is_not_extended_by_progress(void)
{
    uint8_t data[100] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.steps[0].result = FAKE_WANT_WRITE;
    io.steps[1] = (fake_step_t){.result = 40, .elapsed_us = 15000};
    io.steps[2].result = 60;
    io.step_count = 3;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;

    assert(wss_tx_write_all(&ops, data, sizeof(data), 20000, &stats) ==
           WSS_TX_WRITE_TIMEOUT);
    assert(stats.bytes_sent == 40);
    assert(io.call_count == 2);
}

static void test_repeated_transient_times_out(void)
{
    uint8_t data[8] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.steps[0].result = FAKE_WANT_WRITE;
    io.step_count = 1;
    io.repeat_last = true;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;

    assert(wss_tx_write_all(&ops, data, sizeof(data), 20000, &stats) ==
           WSS_TX_WRITE_TIMEOUT);
    assert(stats.bytes_sent == 0);
    assert(stats.transient_retries == 2);
    assert(io.call_count == 2);
    assert(stats.finished_us >= 20000);
}

static void test_expired_deadline_does_not_start_another_phase(void)
{
    uint8_t data[8] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.now_us = 20000;
    io.steps[0].result = 8;
    io.step_count = 1;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;

    assert(wss_tx_write_all(&ops, data, sizeof(data), 20000, &stats) ==
           WSS_TX_WRITE_TIMEOUT);
    assert(io.call_count == 0 && stats.bytes_sent == 0);
}

static void test_fatal_and_invalid_progress_stop_immediately(void)
{
    uint8_t data[8] = {0};
    fake_io_t io;
    init_io(&io, data);
    io.steps[0].result = -1234;
    io.step_count = 1;
    wss_tx_writer_ops_t ops = fake_ops(&io);
    wss_tx_write_stats_t stats;
    assert(wss_tx_write_all(&ops, data, sizeof(data), 2000000, &stats) ==
           WSS_TX_WRITE_FATAL);
    assert(io.call_count == 1 && stats.bytes_sent == 0);

    init_io(&io, data);
    io.steps[0].result = 9;
    io.step_count = 1;
    ops = fake_ops(&io);
    assert(wss_tx_write_all(&ops, data, sizeof(data), 2000000, &stats) ==
           WSS_TX_WRITE_FATAL);
    assert(io.call_count == 1 && stats.bytes_sent == 0);
}

int main(void)
{
    test_single_and_partial_write();
    test_want_states_retry_identical_range();
    test_errno_eagain_is_retryable();
    test_absolute_deadline_is_not_extended_by_progress();
    test_repeated_transient_times_out();
    test_expired_deadline_does_not_start_another_phase();
    test_fatal_and_invalid_progress_stop_immediately();
    puts("PASS: bounded TLS writes preserve offsets across transient states");
    return 0;
}
