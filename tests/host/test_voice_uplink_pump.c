#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "voice_uplink_pump.h"

#define CAPACITY 16U
#define FRAME_SIZE 8U

typedef struct {
    voice_uplink_ring_t ring;
    voice_uplink_pump_t pump;
    uint8_t storage[CAPACITY * FRAME_SIZE];
    uint16_t lengths[CAPACITY];
    uint32_t generations[CAPACITY];
    int64_t now_us;
    int64_t send_cost_us;
    unsigned send_calls;
    unsigned fail_on_call;
    uint8_t sent_values[CAPACITY];
} fixture_t;

static bool fake_send(void *ctx, const uint8_t *data, size_t len)
{
    fixture_t *fixture = ctx;
    fixture->send_calls++;
    fixture->now_us += fixture->send_cost_us;
    if (fixture->fail_on_call == fixture->send_calls) return false;
    assert(len == 1);
    fixture->sent_values[fixture->send_calls - 1U] = data[0];
    return true;
}

static int64_t fake_now(void *ctx)
{
    return ((fixture_t *)ctx)->now_us;
}

static void init_fixture(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    assert(voice_uplink_ring_init(&fixture->ring, fixture->storage,
                                  fixture->lengths, fixture->generations,
                                  CAPACITY, FRAME_SIZE));
    assert(voice_uplink_ring_start_generation(&fixture->ring, 1));
    const voice_uplink_pump_ops_t ops = {
        .ctx = fixture,
        .send = fake_send,
        .now_us = fake_now,
    };
    const voice_uplink_pump_config_t config = {
        .normal_batch = 1,
        .catchup_batch = 8,
        .catchup_threshold_frames = 2,
        .run_budget_us = 8000,
    };
    assert(voice_uplink_pump_init(&fixture->pump, &fixture->ring,
                                  &ops, &config));
    voice_uplink_pump_start_generation(&fixture->pump, 1);
}

static void push_values(fixture_t *fixture, unsigned first, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        uint8_t value = (uint8_t)(first + i);
        assert(voice_uplink_ring_push(&fixture->ring, &value, 1) ==
               VOICE_UPLINK_PUSH_OK);
    }
}

static void test_normal_path_sends_one(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    push_values(&fixture, 1, 1);
    voice_uplink_pump_result_t result = voice_uplink_pump_run(&fixture.pump);
    assert(result.status == VOICE_UPLINK_PUMP_OK);
    assert(result.frames_sent == 1 && result.remaining_frames == 0);
    assert(!result.catchup_started && !result.catchup_completed);
}

static void test_backlog_uses_catchup_batch_and_reports_completion(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    push_values(&fixture, 10, 6);
    voice_uplink_pump_result_t result = voice_uplink_pump_run(&fixture.pump);
    assert(result.status == VOICE_UPLINK_PUMP_OK);
    assert(result.frames_sent == 6 && result.remaining_frames == 0);
    assert(result.catchup_started && result.catchup_completed);
    assert(result.catchup_peak_frames == 6);
    for (unsigned i = 0; i < 6; ++i) {
        assert(fixture.sent_values[i] == (uint8_t)(10 + i));
    }
}

static void test_time_budget_bounds_each_run(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    fixture.send_cost_us = 3000;
    push_values(&fixture, 20, 8);
    voice_uplink_pump_result_t first = voice_uplink_pump_run(&fixture.pump);
    assert(first.frames_sent == 3 && first.remaining_frames == 5);
    assert(first.catchup_started && !first.catchup_completed);
    voice_uplink_pump_result_t second = voice_uplink_pump_run(&fixture.pump);
    assert(second.frames_sent == 3 && second.remaining_frames == 2);
    voice_uplink_pump_result_t third = voice_uplink_pump_run(&fixture.pump);
    assert(third.frames_sent == 2 && third.remaining_frames == 0);
    assert(third.catchup_completed && third.catchup_peak_frames == 8);
    assert(third.catchup_drain_us == 24000);
}

static void test_send_failure_keeps_front_frame(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    push_values(&fixture, 30, 3);
    fixture.fail_on_call = 1;
    voice_uplink_pump_result_t failed = voice_uplink_pump_run(&fixture.pump);
    assert(failed.status == VOICE_UPLINK_PUMP_SEND_FAILED);
    assert(failed.frames_sent == 0 && failed.remaining_frames == 3);

    fixture.fail_on_call = 0;
    fixture.send_calls = 0;
    voice_uplink_pump_result_t recovered = voice_uplink_pump_run(&fixture.pump);
    assert(recovered.status == VOICE_UPLINK_PUMP_OK);
    assert(fixture.sent_values[0] == 30);
}

static void test_stop_rejects_pumping_old_generation(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    push_values(&fixture, 40, 2);
    voice_uplink_pump_stop(&fixture.pump);
    voice_uplink_pump_result_t result = voice_uplink_pump_run(&fixture.pump);
    assert(result.status == VOICE_UPLINK_PUMP_RING_ERROR);
    assert(result.frames_sent == 0);
}

static void test_closed_generation_is_not_drained(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    push_values(&fixture, 50, 3);
    voice_uplink_ring_close_generation(&fixture.ring);
    voice_uplink_pump_result_t result = voice_uplink_pump_run(&fixture.pump);
    assert(result.status == VOICE_UPLINK_PUMP_GENERATION_CLOSED);
    assert(result.frames_sent == 0 && result.remaining_frames == 3);
    assert(fixture.send_calls == 0);
}

int main(void)
{
    test_normal_path_sends_one();
    test_backlog_uses_catchup_batch_and_reports_completion();
    test_time_budget_bounds_each_run();
    test_send_failure_keeps_front_frame();
    test_stop_rejects_pumping_old_generation();
    test_closed_generation_is_not_drained();
    puts("PASS: uplink pump catches up within batch/time budgets without early consume");
    return 0;
}
