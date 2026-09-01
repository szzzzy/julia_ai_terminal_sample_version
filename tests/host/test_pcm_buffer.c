#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pcm_buffer.h"

int main(void)
{
    uint8_t storage[18], out[32];
    uint8_t first[] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t second[] = {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    pcm_buffer_t fifo;
    pcm_buffer_init(&fifo, storage, sizeof(storage));
    assert(pcm_buffer_read(&fifo, out, sizeof(out)) == 0);
    assert(pcm_buffer_write(&fifo, first, sizeof(first)));
    assert(pcm_buffer_read(&fifo, out, 6) == 6);
    assert(memcmp(out, first, 6) == 0);
    assert(pcm_buffer_write(&fifo, second, sizeof(second))); /* wrapped write */
    assert(pcm_buffer_read(&fifo, out, sizeof(out)) == 14);
    for (unsigned i = 0; i < 14; ++i) assert(out[i] == i + 6);

    assert(pcm_buffer_write(&fifo, first, sizeof(first)));
    pcm_buffer_end(&fifo); /* END preserves the queued tail */
    assert(!pcm_buffer_finished(&fifo));
    assert(!pcm_buffer_write(&fifo, second, 2));
    assert(pcm_buffer_read(&fifo, out, 3) == 2); /* never split a PCM16 sample */
    assert(pcm_buffer_read(&fifo, out + 2, sizeof(out) - 2) == 6);
    assert(memcmp(out, first, sizeof(first)) == 0);
    assert(pcm_buffer_finished(&fifo));

    pcm_buffer_reset(&fifo); /* cancel discards old audio and END */
    assert(pcm_buffer_write(&fifo, first, sizeof(first)));
    assert(!pcm_buffer_write(&fifo, second, sizeof(second))); /* atomic overflow */
    assert(fifo.size == sizeof(first));
    assert(!pcm_buffer_write(&fifo, second, 3));
    assert(!pcm_buffer_write(&fifo, NULL, 2));
    pcm_buffer_reset(&fifo);
    assert(pcm_buffer_write(&fifo, second, sizeof(second)));
    assert(pcm_buffer_read(&fifo, out, sizeof(out)) == sizeof(second));
    assert(memcmp(out, second, sizeof(second)) == 0);

    /* Variable packet boundaries and wraps must reproduce the original stream. */
    pcm_buffer_reset(&fifo);
    unsigned produced = 0, consumed = 0;
    while (consumed < 20000) {
        uint8_t packet[12];
        size_t bytes = 2 * (1 + produced % 6);
        if (bytes > 20000 - produced) bytes = 20000 - produced;
        for (size_t i = 0; i < bytes; ++i) packet[i] = (uint8_t)(produced + i);
        if (bytes && pcm_buffer_write(&fifo, packet, bytes)) produced += (unsigned)bytes;
        size_t got = pcm_buffer_read(&fifo, out, 2 * (1 + consumed % 5));
        for (size_t i = 0; i < got; ++i) assert(out[i] == (uint8_t)(consumed + i));
        consumed += (unsigned)got;
    }
    assert(produced == consumed && fifo.size == 0);
    puts("PASS: FIFO wrap, ordered END, cancellation, overflow, PCM16 alignment, 20000-byte stream");
    return 0;
}
