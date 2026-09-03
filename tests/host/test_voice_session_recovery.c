#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "voice_uplink_pump.h"

#define CAPACITY 4U
#define FRAME_SIZE 4U

typedef struct {
    voice_uplink_ring_t ring;
    voice_uplink_pump_t pump;
    uint8_t storage[CAPACITY * FRAME_SIZE];
    uint16_t lengths[CAPACITY];
    uint32_t generations[CAPACITY];
    uint8_t sent[CAPACITY];
    unsigned sent_count;
    int64_t now_us;
    bool overflow_requested;
} fixture_t;

static bool fake_send(void *ctx, const uint8_t *data, size_t len)
{
    fixture_t *fixture = ctx;
    assert(len == 1 && fixture->sent_count < CAPACITY);
    fixture->sent[fixture->sent_count++] = data[0];
    return true;
}

static int64_t fake_now(void *ctx)
{
    return ((fixture_t *)ctx)->now_us;
}

static void start_generation(fixture_t *fixture, uint32_t generation)
{
    assert(voice_uplink_ring_start_generation(&fixture->ring, generation));
    voice_uplink_pump_start_generation(&fixture->pump, generation);
}

static void init_fixture(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    assert(voice_uplink_ring_init(&fixture->ring, fixture->storage,
                                  fixture->lengths, fixture->generations,
                                  CAPACITY, FRAME_SIZE));
    const voice_uplink_pump_ops_t ops = {
        .ctx = fixture,
        .send = fake_send,
        .now_us = fake_now,
    };
    const voice_uplink_pump_config_t config = {
        .normal_batch = 1,
        .catchup_batch = 4,
        .catchup_threshold_frames = 2,
        .run_budget_us = 8000,
    };
    assert(voice_uplink_pump_init(&fixture->pump, &fixture->ring,
                                  &ops, &config));
    start_generation(fixture, 1);
}

static void producer_push(fixture_t *fixture, uint8_t value)
{
    voice_uplink_push_result_t result = voice_uplink_ring_push(
        &fixture->ring, &value, 1);
    if (result == VOICE_UPLINK_PUSH_FULL) {
        /* 与生产代码相同：producer 只关入口并请求 owner，不清ring。 */
        voice_uplink_ring_close_generation(&fixture->ring);
        fixture->overflow_requested = true;
    } else {
        assert(result == VOICE_UPLINK_PUSH_OK);
    }
}

static void owner_teardown(fixture_t *fixture)
{
    assert(fixture->overflow_requested);
    voice_uplink_pump_stop(&fixture->pump);
    voice_uplink_ring_stop_generation(&fixture->ring);
    fixture->overflow_requested = false;
}

int main(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    for (uint8_t value = 1; value <= CAPACITY; ++value) {
        producer_push(&fixture, value);
    }
    producer_push(&fixture, 99);
    assert(fixture.overflow_requested);
    assert(!voice_uplink_ring_is_accepting(&fixture.ring));
    assert(voice_uplink_ring_count(&fixture.ring) == CAPACITY);

    /* owner 尚未 teardown 前也不得继续发送这个已失效会话的积压。 */
    voice_uplink_pump_result_t closed = voice_uplink_pump_run(&fixture.pump);
    assert(closed.status == VOICE_UPLINK_PUMP_GENERATION_CLOSED);
    assert(closed.frames_sent == 0 && fixture.sent_count == 0);

    owner_teardown(&fixture);
    assert(voice_uplink_ring_count(&fixture.ring) == 0);
    start_generation(&fixture, 2);
    producer_push(&fixture, 100);
    voice_uplink_pump_result_t fresh = voice_uplink_pump_run(&fixture.pump);
    assert(fresh.status == VOICE_UPLINK_PUMP_OK && fresh.frames_sent == 1);
    assert(fixture.sent_count == 1 && fixture.sent[0] == 100);

    puts("PASS: overflow teardown is owner-only and old PCM is never replayed");
    return 0;
}
