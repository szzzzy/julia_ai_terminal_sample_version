#include "julia_charge_detector.h"

#include <limits.h>
#include <string.h>

#define CHARGE_TREND_NOISE_MV 3U

void julia_charge_detector_reset(julia_charge_detector_t *detector)
{
    if (detector != NULL) memset(detector, 0, sizeof(*detector));
}

static void start_baseline(julia_charge_detector_t *detector, uint16_t raw_mv)
{
    detector->initialized = true;
    detector->charging = false;
    detector->previous_mv = raw_mv;
    detector->reference_mv = raw_mv;
    detector->peak_mv = raw_mv;
    detector->rise_samples = 0U;
    detector->drop_samples = 0U;
    detector->no_rise_samples = 0U;
}

bool julia_charge_detector_update(julia_charge_detector_t *detector,
                                  bool battery_present,
                                  uint16_t raw_mv,
                                  uint16_t rise_mv,
                                  uint16_t exit_drop_mv,
                                  uint8_t confirm_samples,
                                  uint16_t evidence_timeout_samples)
{
    if (detector == NULL) return false;
    if (!battery_present) {
        julia_charge_detector_reset(detector);
        return false;
    }
    if (!detector->initialized) {
        start_baseline(detector, raw_mv);
        return false;
    }
    if (confirm_samples == 0U) confirm_samples = 1U;
    if (evidence_timeout_samples == 0U) evidence_timeout_samples = 1U;

    if (!detector->charging) {
        if (raw_mv < detector->reference_mv ||
            (uint32_t)raw_mv + CHARGE_TREND_NOISE_MV < detector->previous_mv) {
            detector->reference_mv = raw_mv;
            detector->rise_samples = 0U;
        } else if ((uint32_t)raw_mv >=
                       (uint32_t)detector->reference_mv + rise_mv &&
                   (uint32_t)raw_mv + CHARGE_TREND_NOISE_MV >=
                       detector->previous_mv) {
            if (detector->rise_samples < UINT8_MAX) ++detector->rise_samples;
        } else if ((uint32_t)raw_mv <
                   (uint32_t)detector->reference_mv + rise_mv) {
            detector->rise_samples = 0U;
        }

        if (detector->rise_samples >= confirm_samples) {
            detector->charging = true;
            detector->peak_mv = raw_mv;
            detector->drop_samples = 0U;
            detector->no_rise_samples = 0U;
        }
    } else {
        if (raw_mv > detector->peak_mv) detector->peak_mv = raw_mv;

        if ((uint32_t)raw_mv >=
            (uint32_t)detector->previous_mv + CHARGE_TREND_NOISE_MV) {
            detector->no_rise_samples = 0U;
        } else if (detector->no_rise_samples < UINT16_MAX) {
            ++detector->no_rise_samples;
        }

        if ((uint32_t)raw_mv + exit_drop_mv <= detector->peak_mv) {
            if (detector->drop_samples < UINT8_MAX) ++detector->drop_samples;
        } else {
            detector->drop_samples = 0U;
        }

        if (detector->drop_samples >= confirm_samples ||
            detector->no_rise_samples >= evidence_timeout_samples) {
            start_baseline(detector, raw_mv);
        }
    }

    detector->previous_mv = raw_mv;
    return detector->charging;
}
