"""Small packaging/guard checks; no simulator or native functional reruns."""
from pathlib import Path
import ast
import unittest
from common import PACKAGE, REPO, config_fingerprint, verify_package


class PackagingTests(unittest.TestCase):
    def test_validated_bytes_and_recovered_originals(self):
        verify_package()

    def test_accepted_helper_bodies_were_not_refactored(self):
        pairs=[('direct_checks.py','verify_grid_direct_v2.py',('require','inspect_trace','extra_checks')),
               ('broadcast_checks.py','verify_grid_broadcast_v2_pty.py',('require','input_fingerprint','canonical_config','extra_checks')),
               ('capture.py','verify_grid_broadcast_v2_pty.py',('capture_pty',))]
        def functions(path):
            return {n.name:ast.dump(n,include_attributes=False) for n in ast.parse(path.read_text()).body
                    if isinstance(n,ast.FunctionDef)}
        for module,original,names in pairs:
            actual=functions(PACKAGE/module)
            expected=functions(PACKAGE/'provenance'/original)
            for name in names:
                self.assertEqual(actual[name],expected[name],(module,name))

    def test_config_guard_keeps_clock_and_hardware_differences(self):
        def config(binary,frequency,latency,trace):
            return {'target':{'chip':{'cluster_0':{'loader':{'binary':[binary]},'latency':latency}},
                              'clock':{'frequency':frequency},'gvsoc':{'traces':{'include_regex':[trace]}}}}
        a=config('/native/direct.elf',1_000_000_000,3,'direct trace')
        b=config('/native/broadcast.elf',1_000_000_000,3,'broadcast trace')
        baseline=config_fingerprint(a,Path('/native'))
        self.assertEqual(baseline,config_fingerprint(b,Path('/native')))
        self.assertNotEqual(baseline,config_fingerprint(config('/native/broadcast.elf',500_000_000,3,'x'),Path('/native')))
        self.assertNotEqual(baseline,config_fingerprint(config('/native/broadcast.elf',1_000_000_000,4,'x'),Path('/native')))


if __name__=='__main__':
    unittest.main()
