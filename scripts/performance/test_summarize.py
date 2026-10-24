import tempfile
import unittest
from pathlib import Path

from summarize import nearest_rank, summarize


class FrameSummaryTest(unittest.TestCase):
    def test_work_distribution_sums_hits_and_keeps_idle_frames(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.txt'
            lines = []
            for frame in range(1, 4):
                lines.append('slice=main-thread N=1 ticks=5 qpc_freq=1000 frame={}\n'.format(frame))
                lines.append('kind=frame schema=2 frame={} start={} interval_ticks=10 qpc_freq=1000 '
                             'scene=Render stage=a valid_mask=16 lost_slices=0 lost_operations=0 '
                             'lost_work=0 lost_frames=0 pending_operations=0\n'.format(frame, 100 + frame * 10))
            lines.extend(['kind=work frame=1 metric=upload value=4 source=2 direction=1 channel=0\n',
                          'kind=work frame=1 metric=upload value=8 source=2 direction=1 channel=0\n',
                          'kind=work frame=2 metric=upload value=3 source=2 direction=1 channel=0\n'])
            source.write_text(''.join(lines), encoding='utf-8')
            group = summarize(source)['groups'][0]
            self.assertEqual(group['work']['upload:2:1:0'], 15)
            self.assertEqual(group['work_per_frame']['upload:2:1:0'],
                             dict(n=3, p50=3, p95=12, p99=12, maximum=12))

    def test_nearest_rank_and_separate_stages(self):
        self.assertEqual(nearest_rank([10, 40, 20, 30], 0.5), 20)
        self.assertEqual(nearest_rank([10, 40, 20, 30], 0.95), 40)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.txt'
            source.write_text(
                'slice=main-thread N=1 ticks=5 qpc_freq=1000 schema=2 frame=1 stage=a\n'
                'kind=frame schema=2 frame=1 start=100 interval_ticks=0 qpc_freq=1000 '
                'scene=Pipeline stage=a valid_mask=16 lost_slices=0 lost_operations=0 '
                'lost_work=0 lost_frames=0 pending_operations=0\n'
                'slice=main-thread N=1 ticks=20 qpc_freq=1000 schema=2 frame=2 stage=b\n'
                'kind=frame schema=2 frame=2 start=140 interval_ticks=40 qpc_freq=1000 '
                'scene=Pipeline stage=b valid_mask=16 lost_slices=0 lost_operations=0 '
                'lost_work=0 lost_frames=0 pending_operations=0\n', encoding='utf-8')
            result = summarize(source)
            self.assertEqual(len(result['groups']), 2)
            self.assertEqual(result['groups'][0]['main_thread_ms']['p95'], 5)
            self.assertEqual(result['groups'][1]['main_thread_ms']['p95'], 20)

    def test_missing_frame_and_lost_samples_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.txt'
            source.write_text('slice=main-thread N=1 ticks=5 frame=1 schema=2\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                summarize(source)

    def test_loss_mask_and_frame_gaps_are_rejected(self):
        base = ('slice=main-thread N=1 ticks=5 qpc_freq=1000 frame=1\n'
                'kind=frame schema=2 frame=1 start=100 interval_ticks=0 qpc_freq=1000 '
                'scene=Pipeline stage=a valid_mask=16 lost_slices=0 lost_operations=0 '
                'lost_work=0 lost_frames=0 pending_operations=0\n')
        bad = [base.replace(field + '=0', field + '=1')
               for field in ('lost_slices', 'lost_operations', 'lost_work', 'lost_frames')]
        bad.append(base.replace('valid_mask=16', 'valid_mask=32'))
        bad.append(base + base.replace('frame=1', 'frame=3'))
        bad.append(base.replace('qpc_freq=1000 frame=1', 'qpc_freq=2000 frame=1'))
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.txt'
            for text in bad:
                source.write_text(text, encoding='utf-8')
                with self.subTest(text=text), self.assertRaises(ValueError):
                    summarize(source)

    def test_response_contains_waiting_and_must_end_in_submitted_frame(self):
        base = ('slice=main-thread N=1 ticks=5 qpc_freq=1000 frame=1\n'
                'kind=frame schema=2 frame=1 start=100 interval_ticks=0 qpc_freq=1000 '
                'scene=Pipeline stage=a valid_mask=16 lost_slices=0 lost_operations=0 '
                'lost_work=0 lost_frames=0 pending_operations=0\n'
                'kind=operation frame=1 id=1 target=expand start=40 end=105 ticks=65 '
                'qpc_freq=1000 scene=Pipeline stage=a\n')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.txt'
            source.write_text(base, encoding='utf-8')
            self.assertEqual(summarize(source)['groups'][0]['operation_response_ms']['p95'], 65)
            for text in (base.replace('end=105', 'end=106'), base.replace('ticks=65', 'ticks=66')):
                source.write_text(text, encoding='utf-8')
                with self.subTest(text=text), self.assertRaises(ValueError):
                    summarize(source)

    def test_truncated_tail_is_rejected_and_clean_end_is_reported(self):
        base = ('slice=main-thread N=1 ticks=5 qpc_freq=1000 frame=1\n'
                'kind=frame schema=2 frame=1 start=100 interval_ticks=0 qpc_freq=1000 '
                'scene=Pipeline stage=a valid_mask=16 lost_slices=0 lost_operations=0 '
                'lost_work=0 lost_frames=0 pending_operations=0\n')
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.txt'
            source.write_text(base + 'slice=main-thread N=1 ticks=', encoding='utf-8')
            with self.assertRaises(ValueError):
                summarize(source)
            source.write_text(base + '# End frame probe frame=1 complete=1\n', encoding='utf-8')
            self.assertTrue(summarize(source)['run_complete'])


if __name__ == '__main__':
    unittest.main()
