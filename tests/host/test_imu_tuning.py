"""Check polling replay, baseline, OR confirmation, gaps and cooldown semantics."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/imu_logger'))
from tune_motion import Params, load_params, sampled_features, replay, parse_values, interactive
from types import SimpleNamespace
from unittest.mock import patch
import io


def row(ms, counter, ax=0, gx=0):
    return dict(t_us=ms*1000, sensor_counter=counter, ax_g=ax, ay_g=0,
                az_g=1, gx_dps=gx, gy_dps=0, gz_dps=0)


class TuningTests(unittest.TestCase):
    def test_direct_values_keep_defaults_and_validate(self):
        p = Params()
        self.assertEqual(parse_values('', p), p)
        self.assertEqual(parse_values('10 10 10000 750 500', p), Params(gyro_dps=500))
        self.assertEqual(parse_values('- - - off 800', p), Params(accel_mg=None, gyro_dps=800))
        for text in ('10 10', '0 10 10000 750 500', '10 0 10000 750 500', '10 10 -1 750 500'):
            with self.assertRaises(ValueError):
                parse_values(text, p)

    def test_interactive_multiple_runs_and_invalid_input(self):
        with patch('builtins.input', side_effect=['bad', '10 10 10000 750 500', '', '- 1 - off 800', 'q']), \
             patch('tune_motion.run_once') as run, patch('sys.stdout', new_callable=io.StringIO):
            interactive([], SimpleNamespace(), Params())
        self.assertEqual([call.args[2] for call in run.call_args_list],
                         [Params(gyro_dps=500), Params(gyro_dps=500),
                          Params(confirm_frames=1, accel_mg=None, gyro_dps=800)])

    def test_or_can_confirm_with_alternating_axes(self):
        p = Params(confirm_frames=2)
        result = replay([(0,None,None), (10,800,0), (20,0,100)], p, combined_or=True)
        self.assertEqual(result['first_trigger_s'], .02)
        self.assertEqual(result['triggers'][0]['accel_hits'], 1)
        self.assertEqual(result['triggers'][0]['gyro_hits'], 1)

    def test_separate_axes_reject_alternation_even_if_total_hits_exceeds_ten(self):
        f = [(0,None,None)] + [(i*10,800 if i%3 else 0,100 if i%3!=1 else 0) for i in range(1,11)]
        self.assertTrue(replay(f,Params(),combined_or=True)['triggered'])
        self.assertFalse(replay(f,Params())['triggered'])

    def test_either_axis_can_confirm_independently(self):
        for a,g,expected in [(800,0,(10,0)),(0,100,(0,10)),(800,100,(10,10))]:
            f=[(0,None,None)]+[(i*10,a,g) for i in range(1,11)]
            event=replay(f,Params())['triggers'][0]
            self.assertEqual((event['accel_hits'],event['gyro_hits']),expected)
            self.assertEqual(event['time_s'],.1)

    def test_failed_sample_resets_streak(self):
        p = Params(confirm_frames=2)
        result = replay([(0,None,None), (10,0,100), (20,0,0), (30,0,100)], p)
        self.assertFalse(result['triggered'])

    def test_resample_recomputes_delta_and_uses_no_future_sample(self):
        rows = [row(0,0), row(8,1,ax=.4), row(16,2,ax=.8), row(24,3,ax=1.2)]
        f = sampled_features(rows,20,0)
        self.assertEqual(f, [(0,None,None),(20,800,0)])
        self.assertTrue(replay(f,Params(confirm_frames=1))['triggered'])

    def test_repeated_register_value_still_counts_for_gyro(self):
        rows = [row(0,0,gx=100), row(30,1,gx=100)]
        f = sampled_features(rows,10,0)
        self.assertEqual(replay(f,Params(confirm_frames=3))['first_trigger_s'], .03)

    def test_missing_sample_resets_baseline_and_wrap_is_valid(self):
        f = sampled_features([row(0,0xffffff,gx=100), row(10,0,gx=100),
                              row(20,2,gx=100), row(30,3,gx=100)],10,0)
        self.assertIsNone(f[2][1])
        self.assertFalse(replay(f,Params(confirm_frames=2))['triggered'])

    def test_cooldown_only_affects_repeats_and_rebuilds_baseline(self):
        f = [(0,None,None)]+[(ms,0,100) for ms in range(10,101,10)]
        p=Params(confirm_frames=1,cooldown_ms=30)
        self.assertEqual([x['time_s'] for x in replay(f,p)['triggers']], [.01])
        self.assertEqual([x['time_s'] for x in replay(f,p,True)['triggers']], [.01,.05,.09])

    def test_off_disables_only_selected_branch(self):
        self.assertFalse(replay([(0,None,None),(10,2000,0)],Params(confirm_frames=1,accel_mg=None))['triggered'])

    def test_config_read(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'sdkconfig'
            path.write_text('CONFIG_JULIA_IMU_MOTION_SAMPLE_MS=10\nCONFIG_JULIA_IMU_MOTION_CONFIRM_FRAMES=10\n'
                'CONFIG_JULIA_IMU_MOTION_COOLDOWN_MS=10000\nCONFIG_JULIA_IMU_ACCEL_DELTA_MG=750\n'
                'CONFIG_JULIA_IMU_GYRO_THRESHOLD_DPS=90\n')
            self.assertEqual(load_params(path),Params())


if __name__ == '__main__':
    unittest.main()
