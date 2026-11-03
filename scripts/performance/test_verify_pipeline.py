"""Negative acceptance checks must fail even when a process exited successfully."""
import json
import csv
from pathlib import Path
import tempfile
import unittest

from verify_pipeline import (verify, check_gui_contract, check_receiver_progress,
                             check_refresh_cadence, check_source_window, verify_audio,
                             verify_memory, verify_micro)


class PipelineVerificationTests(unittest.TestCase):
    def directory(self, kind='gui', duration=120, smoke=False):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name)
        (path / 'command-result.json').write_text(json.dumps(dict(
            kind=kind, success=True, smoke=smoke, warmup=30, duration=duration,
            commands=[], exits={})), encoding='utf-8')
        return path

    def test_exit_zero_without_required_artifacts_is_rejected_for_every_kind(self):
        for kind in ('pressure', 'overload', 'stall', 'memory', 'gui', 'manual', 'micro', 'audio'):
            with self.subTest(kind=kind):
                result = verify(self.directory(kind))
                self.assertFalse(result['success'])

    def test_formal_duration_cannot_be_redefined_by_the_command_receipt(self):
        result = verify(self.directory(duration=12))
        self.assertFalse(result['success'])
        self.assertTrue(any('parameters' in error for error in result['errors']))

    def contract(self):
        kinds = ('switch', 'configure', 'filter', 'scroll', 'details', 'clear', 'export-start')
        summary = dict(run_complete=True, run_count=1, pending_operations_at_end=0, groups=[dict(
            stage='pipeline', scene='Pipeline', frame_budget_met=True,
            operations_by_target_ms={kind: dict(n=20, p95=90) for kind in kinds},
            work_per_frame={'fixture-controls:1': dict(minimum=5000), 'fixture-controls:2': dict(minimum=5000),
                            'fixture-window:1': dict(maximum=1024), 'fixture-window:2': dict(maximum=1024)})])
        events = [dict(event=name, qpc=tick, value=value, id='0') for name, tick, value in (
            ('warmup-start', 0, 0), ('formal-start', 0, 30000),
            ('formal-planned-end', 0, 150000), ('formal-end', 150200, 150000))]
        for identity in range(1, 141):
            events.extend(dict(event=name, id=identity, qpc=30000 + identity * 600) for name in ('command-issued', 'effect-visible'))
        return summary, events

    def test_complete_formal_operation_contract_passes(self):
        summary, events = self.contract()
        self.assertTrue(check_gui_contract(summary, events, 30, 120)['success'])

    def test_absent_class_nineteen_samples_pending_occlusion_and_short_history_fail(self):
        for defect in ('absent', 'nineteen', 'pending', 'occluded', 'history'):
            summary, events = self.contract()
            group = summary['groups'][0]
            if defect == 'absent':
                del group['operations_by_target_ms']['clear']
            elif defect == 'nineteen':
                group['operations_by_target_ms']['clear']['n'] = 19
            elif defect == 'pending':
                summary['pending_operations_at_end'] = 1
            elif defect == 'occluded':
                events.append(dict(event='occluded', qpc=40000, id='0'))
            else:
                group['work_per_frame']['fixture-controls:1']['minimum'] = 4999
            with self.subTest(defect=defect):
                self.assertFalse(check_gui_contract(summary, events, 30, 120)['success'])

    def test_filled_warmup_window_cannot_replace_formal_worker_progress(self):
        self.assertFalse(check_receiver_progress([(tick, 1024) for tick in range(0, 123000, 100)], 3000, 1000, 120))
        self.assertTrue(check_receiver_progress([(tick, tick) for tick in range(0, 123000, 100)], 3000, 1000, 120))

    def test_twenty_hertz_then_silence_fails_even_when_average_is_ten(self):
        self.assertFalse(check_refresh_cadence(list(range(0, 60000, 50))))
        self.assertTrue(check_refresh_cadence(list(range(0, 120000, 100))))
        self.assertFalse(check_refresh_cadence([]))

    def test_window_contents_must_reach_the_independent_source_and_match_identity(self):
        target = dict(pid=42, created_filetime=1234, originals=2200)
        rows = [dict(pid=42, processCreated=1234, observationGeneration=1,
            iface='IAudioClock', method='GetFrequency', streamId=4096,
            args='hz=' + str(100000 + row), hresult=1 if row % 7 == 0 else 0) for row in range(1177, 2201)]
        self.assertTrue(check_source_window(rows, target, 0, 64, False)['success'])
        target['originals'] = 4000
        self.assertFalse(check_source_window(rows, target, 0, 64, True)['success'])
        target['originals'] = 2200
        rows[-1]['processCreated'] = 999
        self.assertFalse(check_source_window(rows, target, 0, 64, False)['success'])

    def test_audio_full_group_labels_cannot_hide_missing_raw_tail_or_preformal_sample(self):
        path = self.directory('audio')
        states = ('unattached', 'installed-mask0', 'default', 'high-frequency')
        summary = []
        boundaries = []
        for repeat in (1, 2, 3):
            for state in states:
                boundaries.append(dict(repeat=repeat, state=state, warmup_start=0,
                    formal_start=30000, formal_end=150000, qpc_frequency=1000))
                for target in (0, 1):
                    summary.append(dict(repeat=repeat, state=state, target=target, pid=42 + target,
                        created_filetime=1234 + target, callbacks=20000, original_calls=40000,
                        failures=0, discontinuities=0, captured_frames=20000, samples=20000,
                        overflow=0, mean_us=1, p50_us=1, p95_us=1, p99_us=1, max_us=1, min_us=1, exit=0))
        for name, rows in (('audio-summary.csv', summary), ('audio-boundaries.csv', boundaries)):
            with (path / name).open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
        (path / 'probe.stdout.txt').write_text('', encoding='utf-8')
        raw = path / 'audio-samples.csv'
        raw.write_text('repeat,state,target,pid,sample,start_qpc,ticks\n1,unattached,0,42,0,1,1\n')
        result = verify_audio(path, dict(qpc_frequency=1000, warmup=30, duration=120))
        self.assertFalse(result['success'])
        self.assertTrue(any('boundary mismatch' in error for error in result['errors']))
        self.assertTrue(any('samples, original' in error for error in result['errors']))

    def audio_contract(self, discontinuities):
        path = self.directory('audio')
        states = ('unattached', 'installed-mask0', 'default', 'high-frequency')
        summaries, boundaries, samples, metadata = [], [], [], []
        for repeat in (1, 2, 3):
            for target in (0, 1):
                metadata.append('audio_target repeat={} target={} pid={} created={} endpoint=test rate=48000'.format(
                    repeat, target, 42 + target, 1234 + target))
            for state in states:
                boundaries.append(dict(repeat=repeat, state=state, warmup_start=0,
                    formal_start=30000, formal_end=150000, qpc_frequency=1000))
                for target in (0, 1):
                    summaries.append(dict(repeat=repeat, state=state, target=target, pid=42 + target,
                        created_filetime=1234 + target, callbacks=120, original_calls=240,
                        failures=0, discontinuities=discontinuities[repeat - 1]
                            if target == 0 and state == 'high-frequency' else 0,
                        captured_frames=120, samples=120, overflow=0, mean_us=1000, p50_us=1000,
                        p95_us=1000, p99_us=1000, max_us=1000, min_us=1000, exit=0))
                    samples.extend(dict(repeat=repeat, state=state, target=target, pid=42 + target,
                        sample=sample, start_qpc=30000 + sample * 1000, ticks=1) for sample in range(120))
        for name, rows in (('audio-summary.csv', summaries), ('audio-boundaries.csv', boundaries),
                           ('audio-samples.csv', samples)):
            with (path / name).open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
        (path / 'probe.stdout.txt').write_text('\n'.join(metadata), encoding='utf-8')
        return path, dict(qpc_frequency=1000, warmup=30, duration=120)

    def test_audio_two_of_three_new_discontinuities_require_investigation(self):
        for observed in ((1, 1, 0), (1, 0, 1), (0, 1, 1)):
            with self.subTest(observed=observed):
                path, command = self.audio_contract(observed)
                result = verify_audio(path, command)
                self.assertFalse(result['success'])
                self.assertEqual(result['errors'], ['repeated new formal audio discontinuities require investigation and rerun'])
                self.assertEqual(result['repeated_new_discontinuities'], [dict(
                    target=0, state='high-frequency', baseline=[0, 0, 0], observed=list(observed))])

    def test_audio_one_of_three_new_discontinuities_is_not_repeated(self):
        path, command = self.audio_contract((0, 1, 0))
        result = verify_audio(path, command)
        self.assertTrue(result['success'])
        self.assertEqual(result['errors'], [])
        self.assertEqual(result['repeated_new_discontinuities'], [])
        self.assertEqual(result['new_formal_discontinuities'], [dict(
            target=0, state='high-frequency', baseline=[0, 0, 0], observed=[0, 1, 0], increased_repeats=[2])])

    def test_audio_three_of_three_new_discontinuities_require_investigation(self):
        path, command = self.audio_contract((1, 1, 1))
        result = verify_audio(path, command)
        self.assertFalse(result['success'])
        self.assertEqual(result['errors'], ['repeated new formal audio discontinuities require investigation and rerun'])
        self.assertEqual(result['repeated_new_discontinuities'], [dict(
            target=0, state='high-frequency', baseline=[0, 0, 0], observed=[1, 1, 1])])

    def test_micro_full_summary_cannot_hide_truncated_raw_samples(self):
        path = self.directory('micro')
        lines = ['# QPC frequency=10000000; batch=300000; samples=20000',
            'repeat,entry,state,batch_ns,p50_ns,p95_ns,p99_ns,calls,mean_ns,max_ns,minimum_ns']
        for repeat in (1, 2, 3):
            for entry in range(4):
                for state in ('direct', 'installed-mask0', 'default', 'high-frequency'):
                    lines.append('{},{},{},1,1,1,1,330000,1,1,1'.format(repeat, entry, state))
        lines.append('result=PASS warmup=10000 repeats=3 observer=compiled-out')
        (path / 'probe.stdout.txt').write_text('\n'.join(lines))
        (path / 'micro-samples.csv').write_text('repeat,entry,state,sample,ticks\n1,0,direct,0,0\n')
        self.assertFalse(verify_micro(path, {})['success'])

    def test_memory_same_executable_cannot_substitute_another_process_instance(self):
        path = self.directory('memory')
        command = dict(warmup=0, duration=2, commands=[dict(pid=1, created_filetime=10), dict(pid=2, created_filetime=20)])
        source = dict(start_qpc=0, qpc_frequency=1000,
            targets=[dict(pid=3, created_filetime=30), dict(pid=4, created_filetime=40)])
        rows = [dict(pid=pid, created_filetime=created, executable='WinAudioPerformanceGui.exe' if pid == 2 else 'SegmentWindowProbe.exe',
            qpc=tick, error=0, private_bytes=1000, working_set=2000)
            for pid, created in ((1, 10), (2, 20), (3, 30), (4, 99)) for tick in (100, 900, 1100, 1900)]
        (path / 'process.jsonl').write_text('\n'.join(json.dumps(row) for row in rows))
        self.assertFalse(verify_memory(path, command, source)['success'])


if __name__ == '__main__':
    unittest.main()
