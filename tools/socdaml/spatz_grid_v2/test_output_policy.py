"""Focused controlled-experiment checks; no simulator."""
import unittest
from pathlib import Path
from common import ASSETS, EXPERIMENT, source_hashes, target_sources, config_fingerprint
from reproduce import parallel_output_checks


class OutputPolicyTests(unittest.TestCase):
    def test_only_output_policy_changes(self):
        original=(ASSETS/'grid.c').read_text()
        experiment=target_sources(EXPERIMENT)['grid.c'].read_text()
        # Everything after the three-line experimental attribution is accepted
        # source, apart from this single output-selection condition.
        self.assertEqual(experiment.splitlines(keepends=True)[3:],
                         original.replace('    t[5]=cycle32();\n    if (!GRID_BROADCAST) {',
                         '    t[5]=cycle32();\n    if (!GRID_BROADCAST && !GRID_PARALLEL_OUTPUT) {').splitlines(keepends=True))
        self.assertEqual(source_hashes('direct'),source_hashes('broadcast'))
        self.assertNotEqual(source_hashes(EXPERIMENT)['grid.c'],source_hashes('direct')['grid.c'])
        for name in ('gemm_fp32.S','word_export.h','grid.ld'):
            self.assertEqual(source_hashes(EXPERIMENT)[name],source_hashes('direct')[name])

    def test_parallel_check_rejects_serial_or_late_output(self):
        times={c:[0,1,2,2,2,10,50,60] for c in range(16)}
        computes={c:{'begin':3} for c in range(16)}
        details={'application_dma':[dict(cid=c,kind='OUT',start=11,end=20) for c in range(16)]}
        parallel_output_checks(details,times,computes)
        details['application_dma'][0]['end']=51
        with self.assertRaises(ValueError):parallel_output_checks(details,times,computes)
        details['application_dma']=[dict(cid=c,kind='OUT',start=11+c*2,end=12+c*2) for c in range(16)]
        with self.assertRaises(ValueError):parallel_output_checks(details,times,computes)

    def test_experimental_debug_name_normalization_only(self):
        def config(name,clock=1_000_000_000):
            return {'target':{'chip':{'cluster_0':{'loader':{'binary':['/native/'+name+'.elf']}}},
                'debug':'debug_binary_0_'+name+'.elf.debugInfo','clock':{'frequency':clock},
                'gvsoc':{'traces':{'include_regex':[]}}}}
        a=config_fingerprint(config('grid_direct'),Path('/native'))
        self.assertEqual(a,config_fingerprint(config('grid_direct_parallel'),Path('/native')))
        self.assertNotEqual(a,config_fingerprint(config('grid_direct_parallel',500_000_000),Path('/native')))


if __name__=='__main__':unittest.main()
