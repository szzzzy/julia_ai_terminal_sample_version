#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "julia_charge_detector.h"

static bool sample(julia_charge_detector_t *detector, uint16_t mv)
{
    return julia_charge_detector_update(detector, true, mv, 20U, 20U, 2U, 6U);
}

int main(void)
{
    julia_charge_detector_t detector = {0};

    assert(!sample(&detector, 3600));
    assert(!sample(&detector, 3621));
    assert(sample(&detector, 3623));

    /* Raw-voltage drop clears CHG after two samples, without slow UI filtering. */
    assert(sample(&detector, 3600));
    assert(!sample(&detector, 3599));

    julia_charge_detector_reset(&detector);
    assert(!sample(&detector, 4000));
    assert(!sample(&detector, 4021));
    assert(sample(&detector, 4022));

    /* A flat voltage cannot prove cable presence, so the inference expires. */
    for (unsigned i = 0; i < 5; ++i) assert(sample(&detector, 4022));
    assert(!sample(&detector, 4022));

    assert(!julia_charge_detector_update(&detector, false, 0, 20, 20, 2, 6));
    assert(!detector.initialized);

    puts("charge detector tests passed");
    return 0;
}
