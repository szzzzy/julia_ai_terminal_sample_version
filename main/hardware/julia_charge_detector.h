#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    bool charging;
    uint16_t previous_mv;
    uint16_t reference_mv;
    uint16_t peak_mv;
    uint8_t rise_samples;
    uint8_t drop_samples;
    uint16_t no_rise_samples;
} julia_charge_detector_t;

void julia_charge_detector_reset(julia_charge_detector_t *detector);

/**
 * Infer active charging from raw battery-voltage samples.
 *
 * This is deliberately a time-limited inference: without a charger STAT or
 * VBUS input, a flat battery voltage cannot prove that a cable is still
 * attached.
 */
bool julia_charge_detector_update(julia_charge_detector_t *detector,
                                  bool battery_present,
                                  uint16_t raw_mv,
                                  uint16_t rise_mv,
                                  uint16_t exit_drop_mv,
                                  uint8_t confirm_samples,
                                  uint16_t evidence_timeout_samples);
