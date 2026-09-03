#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "voice_uplink_ring.h"

#define TEST_CAPACITY 4U
#define TEST_FRAME_SIZE 8U

typedef struct {
    voice_uplink_ring_t ring;
    uint8_t storage[TEST_CAPACITY * TEST_FRAME_SIZE];
    uint16_t lengths[TEST_CAPACITY];
    uint32_t generations[TEST_CAPACITY];
} fixture_t;

static void init_fixture(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    assert(voice_uplink_ring_init(&fixture->ring, fixture->storage,
                                  fixture->lengths, fixture->generations,
                                  TEST_CAPACITY, TEST_FRAME_SIZE));
}

static void expect_front(fixture_t *fixture, uint32_t generation,
                         const uint8_t *expected, size_t expected_len)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    assert(voice_uplink_ring_peek(&fixture->ring, generation, &data, &len));
    assert(len == expected_len && memcmp(data, expected, len) == 0);
}

static void test_inactive_and_validation(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const uint8_t frame[] = {1, 2, 3};
    assert(!voice_uplink_ring_is_accepting(&fixture.ring));
    assert(voice_uplink_ring_push(&fixture.ring, frame, sizeof(frame)) ==
           VOICE_UPLINK_PUSH_INACTIVE);
    assert(!voice_uplink_ring_start_generation(&fixture.ring, 0));
    voice_uplink_ring_t invalid_ring;
    uint8_t invalid_storage[3 * TEST_FRAME_SIZE];
    uint16_t invalid_lengths[3];
    uint32_t invalid_generations[3];
    assert(!voice_uplink_ring_init(&invalid_ring, invalid_storage,
                                   invalid_lengths, invalid_generations,
                                   3, TEST_FRAME_SIZE));
    assert(voice_uplink_ring_push(&fixture.ring, NULL, 1) ==
           VOICE_UPLINK_PUSH_INVALID);
    assert(voice_uplink_ring_push(&fixture.ring, frame, 0) ==
           VOICE_UPLINK_PUSH_INVALID);
    assert(voice_uplink_ring_push(&fixture.ring, frame,
                                  TEST_FRAME_SIZE + 1U) ==
           VOICE_UPLINK_PUSH_INVALID);
}

static void test_fifo_peek_and_consume(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const uint8_t first[] = {1, 2};
    const uint8_t second[] = {3, 4, 5};
    assert(voice_uplink_ring_start_generation(&fixture.ring, 1));
    assert(voice_uplink_ring_push(&fixture.ring, first, sizeof(first)) ==
           VOICE_UPLINK_PUSH_OK);
    assert(voice_uplink_ring_push(&fixture.ring, second, sizeof(second)) ==
           VOICE_UPLINK_PUSH_OK);
    assert(voice_uplink_ring_count(&fixture.ring) == 2);

    expect_front(&fixture, 1, first, sizeof(first));
    expect_front(&fixture, 1, first, sizeof(first));
    assert(voice_uplink_ring_count(&fixture.ring) == 2);
    assert(voice_uplink_ring_consume(&fixture.ring));
    expect_front(&fixture, 1, second, sizeof(second));
    assert(voice_uplink_ring_consume(&fixture.ring));
    assert(!voice_uplink_ring_consume(&fixture.ring));
    assert(voice_uplink_ring_count(&fixture.ring) == 0);
}

static void test_full_and_wrap_preserve_order(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const uint8_t frames[][2] = {
        {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5},
    };
    assert(voice_uplink_ring_start_generation(&fixture.ring, 7));
    for (size_t i = 0; i < TEST_CAPACITY; ++i) {
        assert(voice_uplink_ring_push(&fixture.ring, frames[i], 2) ==
               VOICE_UPLINK_PUSH_OK);
    }
    assert(voice_uplink_ring_push(&fixture.ring, frames[4], 2) ==
           VOICE_UPLINK_PUSH_FULL);
    expect_front(&fixture, 7, frames[0], 2);
    assert(voice_uplink_ring_consume(&fixture.ring));
    assert(voice_uplink_ring_push(&fixture.ring, frames[4], 2) ==
           VOICE_UPLINK_PUSH_OK);
    for (size_t i = 1; i < 5; ++i) {
        expect_front(&fixture, 7, frames[i], 2);
        assert(voice_uplink_ring_consume(&fixture.ring));
    }
    assert(voice_uplink_ring_count(&fixture.ring) == 0);
}

static void test_session_boundary_discards_old_audio(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const uint8_t old_frame[] = {1, 2, 3};
    const uint8_t new_frame[] = {8, 9};
    assert(voice_uplink_ring_start_generation(&fixture.ring, 10));
    assert(voice_uplink_ring_push(&fixture.ring, old_frame,
                                  sizeof(old_frame)) == VOICE_UPLINK_PUSH_OK);
    voice_uplink_ring_stop_generation(&fixture.ring);
    assert(!voice_uplink_ring_is_accepting(&fixture.ring));
    assert(voice_uplink_ring_count(&fixture.ring) == 0);
    assert(voice_uplink_ring_push(&fixture.ring, old_frame,
                                  sizeof(old_frame)) ==
           VOICE_UPLINK_PUSH_INACTIVE);

    assert(voice_uplink_ring_start_generation(&fixture.ring, 11));
    assert(voice_uplink_ring_generation(&fixture.ring) == 11);
    assert(voice_uplink_ring_push(&fixture.ring, new_frame,
                                  sizeof(new_frame)) == VOICE_UPLINK_PUSH_OK);
    expect_front(&fixture, 11, new_frame, sizeof(new_frame));
}

static void test_late_old_generation_slot_is_never_replayed(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const uint8_t current[] = {6, 7};
    assert(voice_uplink_ring_start_generation(&fixture.ring, 21));

    /* 模拟 producer 在旧会话结束竞态中迟到发布的完整槽。consumer 必须跳过。 */
    fixture.storage[0] = 0xAA;
    fixture.lengths[0] = 1;
    fixture.generations[0] = 20;
    fixture.ring.write_sequence = 1;

    const uint8_t *data = NULL;
    size_t len = 0;
    assert(!voice_uplink_ring_peek(&fixture.ring, 21, &data, &len));
    assert(voice_uplink_ring_count(&fixture.ring) == 0);
    assert(voice_uplink_ring_push(&fixture.ring, current, sizeof(current)) ==
           VOICE_UPLINK_PUSH_OK);
    expect_front(&fixture, 21, current, sizeof(current));
}

static void test_sequence_wrap_preserves_fifo(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const uint8_t before_wrap[] = {0xFE};
    const uint8_t after_wrap[] = {0x01};
    fixture.ring.write_sequence = UINT32_MAX;
    fixture.ring.read_sequence = UINT32_MAX;
    assert(voice_uplink_ring_start_generation(&fixture.ring, 30));
    assert(voice_uplink_ring_push(&fixture.ring, before_wrap,
                                  sizeof(before_wrap)) == VOICE_UPLINK_PUSH_OK);
    assert(voice_uplink_ring_push(&fixture.ring, after_wrap,
                                  sizeof(after_wrap)) == VOICE_UPLINK_PUSH_OK);
    expect_front(&fixture, 30, before_wrap, sizeof(before_wrap));
    assert(voice_uplink_ring_consume(&fixture.ring));
    expect_front(&fixture, 30, after_wrap, sizeof(after_wrap));
    assert(voice_uplink_ring_consume(&fixture.ring));
    assert(voice_uplink_ring_count(&fixture.ring) == 0);
}

int main(void)
{
    test_inactive_and_validation();
    test_fifo_peek_and_consume();
    test_full_and_wrap_preserve_order();
    test_session_boundary_discards_old_audio();
    test_late_old_generation_slot_is_never_replayed();
    test_sequence_wrap_preserves_fifo();
    puts("PASS: uplink ring preserves frames and rejects old connection generations");
    return 0;
}
